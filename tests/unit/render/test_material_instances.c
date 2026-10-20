/*
 * test_material_instances.c: material instances (DENG-302, SPEC §12.6, §12.2;
 * src/render/materials/material_instances.h, material_params.h): the 256-byte parameter block,
 * de_material_set_params and runtime instances, dirty uploads, resolve permutations and texture
 * reloads. Executable test_render_material_instances; CTest names
 * render_material_instances.materials.<test>.
 *
 *   material_params_layout_match             material_params.slang against the C block: fields,
 *                                            offsets, sizes, constants
 *   material_set_params_no_pipeline_rebuild  roughness and base colour of 1000 materials change
 *                                            every frame: no pipeline is created (the device's
 *                                            live pipeline count), 1000 blocks upload, each with
 *                                            its new values
 *   material_incremental_upload              10 changed of 1000: 10 blocks packed and uploaded; a
 *                                            static frame packs and uploads none
 *   material_permutation_switch_counted      clearcoat 0 -> 0.5 switches the material to the
 *                                            pre-warmed CLEARCOAT permutation (one switch, no
 *                                            pipeline), sheen to the all-features one, back to 0
 *                                            to the base one; permutations_per_model
 *   material_texture_reload_rebind           a texture hot reload: the block keeps the old
 *                                            texture while the new one uploads, and names the new
 *                                            one in the frame it publishes
 *   material_runtime_instance_copy           de_material_create_instance copies a material; its
 *                                            changes stay its own; destroy; errors
 *   material_mesh_default_slot               an empty DeMeshRenderer slot takes the mesh's
 *                                            submesh material (DENG-231 note)
 *
 * Null backend, the real asset system (DMSH, DMAT, DTEX blobs from loader_test_util.h), the real
 * ECS loop with r.pipelined = 0: a frame is the asset update, PostUpdate, Extract and the kick,
 * which renders at once.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#define _CRT_SECURE_NO_WARNINGS /* layout_match reads material_params.slang with fopen */

#include "de_alloc_guard.h"
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>

#include "asset/hot_reload_internal.h"
#include "asset/load_internal.h"
#include "gpu/gpu_backend.h"
#include "loader_test_util.h"
#include "render/components_internal.h"
#include "render/loaders/render_assets.h"
#include "render/loaders/upload_ring.h"
#include "render/materials/material_instances.h"
#include "render/materials/material_models.h"
#include "render/materials/material_params.h"
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MI_WAIT_NS (20ull * 1000000000ull)
/* Loading 1000 separate files under parallel Windows Debug CI is fixture setup, not the
 * incremental-upload performance/behavior check. Keep a finite bound below CTest's 300 s. */
#define MI_STARTUP_WAIT_NS (120ull * 1000000000ull)
#define MI_MAX             1024u

typedef struct mfx {
    de_tracking_allocator track, dev_track;
    de_allocator alloc, dev_alloc;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *w;
    de_asset_system *s;
    de_gpu_device *d;
    de_renderer *r;
    DeRenderComponents ids;
    de_builtin_component_ids bi;
    char dir[96];
    u32 count;
    de_load_handle loads[MI_MAX + 4];
    u32 load_count;
    de_material mats[MI_MAX];
} mfx;

static void set_cvar(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (DE_HANDLE_VALID(cv))
        (void)de_cvar_set(cv, value);
}

static void mfx_setup(mfx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    f->dev_alloc = de_tracking_allocator_init(&f->dev_track, NULL, DE_MEM_RENDER);
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("rmi", DE_RMI_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
}

static void mfx_teardown(mfx *f) {
    if (f->r != NULL)
        de__render_world_wait_idle(f->r);
    for (u32 i = 0; f->s != NULL && i < f->load_count; ++i)
        de_asset_release(f->s, f->loads[i]);
    if (f->s != NULL)
        de_asset_system_destroy(f->s);
    if (f->r != NULL)
        de_renderer_destroy(f->r);
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->d != NULL)
        de_gpu_device_destroy(f->d);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    set_cvar("r.pipelined", "1");
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->dev_track).blocks_live, 0u);
}

static void put(mfx *f, const char *file, const void *data, u64 size) {
    char path[160];
    snprintf(path, sizeof path, "%s/%s", f->dir, file);
    ASSERT_EQ(de_file_write_all_atomic(path, data, size), DE_OK);
}

/* Copies texture fixture `name` (tests/unit/import/data/texture/) to `<dir>/<as>`. */
static void copy_fixture(mfx *f, const char *name, const char *as) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", DE_RMI_TEST_TEXTURE_DIR, name);
    FILE *fp = fopen(path, "rb");
    ASSERT_TRUE(fp != NULL);
    u8 buf[4096];
    const size_t n = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    ASSERT_TRUE(n > 0 && n < sizeof buf);
    put(f, as, buf, n);
}

static de_asset_id mat_id(u32 i) {
    char name[32];
    snprintf(name, sizeof name, "mi/mat%u", i);
    return de_asset_id_from(name);
}

typedef struct start_opts {
    u32 materials;    /**< DMAT materials mi/mat<i>, each on its own entity */
    bool texture;     /**< mi/tex, the base colour texture of every material */
    bool hot_reload;  /**< an asset system with hot reload (mi/tex reloads) */
    u64 mesh_default; /**< the mesh's submesh material ID (0: none) */
    bool no_entities; /**< spawn nothing */
} start_opts;

/* One host frame: the asset update, PostUpdate, Extract, the kick (r.pipelined = 0: it renders). */
static void frame(mfx *f) {
    de_asset_system_update(f->s, NULL);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->r, f->w), DE_OK);
}

static de_entity spawn(mfx *f, de_material m, f64 x) {
    const de_component comps[2] = {f->bi.transform, f->ids.mesh_renderer};
    de_entity e = DE_ENTITY_NULL;
    if (de_entity_spawn_with(f->w, comps, 2, &e) != DE_OK)
        return DE_ENTITY_NULL;
    DeTransform *t = (DeTransform *)de_entity_get(f->w, e, f->bi.transform);
    t->position = (de_dvec3){x, 0, 0};
    t->rotation = (de_quat){0, 0, 0, 1};
    t->scale = (de_vec3){1, 1, 1};
    DeMeshRenderer *mr = (DeMeshRenderer *)de_entity_get(f->w, e, f->ids.mesh_renderer);
    mr->mesh = de_asset_mesh(f->s, de_asset_id_from("mi/mesh"));
    mr->materials[0] = m;
    mr->layer_mask = 1u;
    return e;
}

typedef struct readiness {
    u32 states[DE_LOAD_CANCELLED + 1];
    u32 incomplete, terminal;
    de_asset_id first;
    de_load_state first_state;
    char first_name[32];
} readiness;

static void check_asset(mfx *f, readiness *r, de_asset_id id, const char *name) {
    const de_load_state state = de_asset_status_of(f->s, id);
    ++r->states[state];
    if (state == DE_LOAD_READY)
        return;
    ++r->incomplete;
    const bool terminal = state == DE_LOAD_INVALID || state == DE_LOAD_PARTIAL ||
                          state == DE_LOAD_FAILED || state == DE_LOAD_CANCELLED;
    /* Prefer a failed asset to a pending one in diagnostics, even if it sorts later. */
    if (r->first.hash == 0 || (terminal && r->terminal == 0)) {
        r->first = id;
        r->first_state = state;
        (void)snprintf(r->first_name, sizeof r->first_name, "%s", name);
    }
    if (terminal)
        ++r->terminal;
}

static readiness asset_readiness(mfx *f) {
    readiness r = {0};
    check_asset(f, &r, de_asset_id_from("mi/mesh"), "mi/mesh");
    for (u32 i = 0; i < f->count; ++i) {
        char name[32];
        (void)snprintf(name, sizeof name, "mi/mat%u", i);
        check_asset(f, &r, mat_id(i), name);
    }
    return r;
}

static void readiness_diagnostic(mfx *f, const readiness *r, u64 elapsed, u32 rounds) {
    char error[256] = "";
    (void)de_asset_last_error(f->s, r->first, error, sizeof error);
    const de_asset_stats assets = de_asset_get_stats(f->s);
    de__asset_wait_stats waits;
    de__asset_wait_get_stats(f->s, &waits);
    printf("material fixture startup %.3f s, %u rounds: %u/%u ready, "
           "%u pending, %u loading, %u terminal; first incomplete %s state %d error '%s'; "
           "loads %u uploads %u; wake blocks %llu signalled %llu timeouts %llu\n",
           (double)elapsed / 1e9, rounds, r->states[DE_LOAD_READY], f->count + 1,
           r->states[DE_LOAD_PENDING], r->states[DE_LOAD_LOADING], r->terminal,
           r->first.hash ? r->first_name : "none", (int)r->first_state, error,
           assets.loads_in_flight, assets.uploads_in_flight, (unsigned long long)waits.blocks,
           (unsigned long long)waits.signalled, (unsigned long long)waits.timeouts);
}

static void wait_assets(mfx *f) {
    const u64 t0 = de_time_now_ns();
    u32 rounds = 0;
    for (;;) {
        /* As in the loader fixture: flush copies, clear the previous wake BEFORE checking
         * publication, then block for new progress. A stale pending signal otherwise spins.
         * ECS extraction/rendering is unnecessary until assets are ready and entities exist. */
        de_render_uploader_flush(de__renderer_uploader(f->r));
        de__asset_wake_reset(f->s);
        de_asset_system_update(f->s, NULL);
        ++rounds;
        const readiness r = asset_readiness(f);
        const u64 elapsed = de_time_now_ns() - t0;
        if (r.incomplete == 0) {
            readiness_diagnostic(f, &r, elapsed, rounds);
            return;
        }
        if (r.terminal != 0 || elapsed >= MI_STARTUP_WAIT_NS) {
            readiness_diagnostic(f, &r, elapsed, rounds);
            FAIL("material fixture assets failed or exceeded the startup deadline");
        }
        const u64 remaining = MI_STARTUP_WAIT_NS - elapsed;
        (void)de__asset_wake_block(f->s, remaining < 2000000u ? remaining : 2000000u);
    }
}

static void start(mfx *f, const char *name, start_opts o) {
    de_fp_env_init_thread();
    snprintf(f->dir, sizeof f->dir, "rmi:/%s", name);
    if (!de_path_exists(f->dir))
        ASSERT_EQ(de_dir_create(f->dir), DE_OK);
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks), DE_OK);
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    ASSERT_EQ(
        de_world_create(
            &(de_world_desc){.allocator = &f->alloc, .types = f->types, .tasks = f->tasks}, &f->w),
        DE_OK);
    char cache[128];
    snprintf(cache, sizeof cache, "%s/decache", f->dir);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc,
                                                             .tasks = f->tasks,
                                                             .io_threads = 1,
                                                             .cache_dir = cache,
                                                             .staging_ring_bytes = 8u << 20,
                                                             .hot_reload = o.hot_reload,
                                                             .reload_retire_frames = 2,
                                                             .types = f->types},
                                     &f->s),
              DE_OK);
    ASSERT_EQ(de_gpu_device_create(
                  &(de_gpu_device_desc){.backend = DE_GPU_NULL, .allocator = &f->dev_alloc}, &f->d),
              DE_OK);
    ASSERT_EQ(de_renderer_create(
                  &(de_renderer_desc){
                      .device = f->d, .assets = f->s, .types = f->types, .allocator = &f->alloc},
                  &f->r),
              DE_OK);
    de_renderer_attach(f->r, f->w);
    f->ids = *de__render_components(f->w);
    f->bi = de_builtin_components(f->w);
    set_cvar("r.pipelined", "0");

    /* The files and the manifest. */
    size_t cap = 256u + (size_t)o.materials * 96u;
    char *text = (char *)malloc(cap);
    ASSERT_TRUE(text != NULL);
    size_t at = (size_t)snprintf(text, cap, "[assets]\n");
    lt_blob mesh = lt_dmsh(64u, o.mesh_default, 7u);
    put(f, "mesh.dmsh", mesh.data, mesh.size);
    lt_free(&mesh);
    at += (size_t)snprintf(text + at, cap - at,
                           "\"mi/mesh\" = { type = \"mesh\", source = \"mesh.dmsh\" }\n");
    if (o.texture) { /* a PNG: the texture importer cooks it (DE_HAS_COOKER builds) */
        copy_fixture(f, "checker.png", "tex.png");
        at += (size_t)snprintf(text + at, cap - at,
                               "\"mi/tex\" = { type = \"texture\", source = \"tex.png\" }\n");
    }
    const u32 slot = DE_COOKED_TEX_BASE_COLOR;
    const u64 tex = de_asset_id_from("mi/tex").hash;
    for (u32 i = 0; i < o.materials; ++i) {
        lt_blob b = lt_dmat(&slot, &tex, o.texture ? 1u : 0u, 0.001f * (f32)(i + 1u));
        char file[32];
        snprintf(file, sizeof file, "mat%u.dmat", i);
        put(f, file, b.data, b.size);
        lt_free(&b);
        at += (size_t)snprintf(text + at, cap - at,
                               "\"mi/mat%u\" = { type = \"material\", source = \"mat%u.dmat\" }\n",
                               i, i);
    }
    put(f, "assets.toml", text, strlen(text));
    free(text);
    char path[160];
    snprintf(path, sizeof path, "%s/assets.toml", f->dir);
    ASSERT_EQ(de_asset_manifest_load(f->s, path), DE_OK);

    /* Load everything, spawn one entity per material, run until every material is READY. */
    f->count = o.materials;
    f->loads[f->load_count++] = de_asset_load(f->s, de_asset_id_from("mi/mesh"), 0);
    for (u32 i = 0; i < o.materials; ++i) {
        f->loads[f->load_count++] = de_asset_load(f->s, mat_id(i), 0);
        f->mats[i] = de_asset_material(f->s, mat_id(i));
    }
    wait_assets(f);
    for (u32 i = 0; !o.no_entities && i < o.materials; ++i)
        ASSERT_NE(spawn(f, f->mats[i], (f64)i).bits, 0u);
    frame(f); /* the blocks of the last materials to publish */
    frame(f);
}

/* The block of material `m` as the GPU scene uploaded it (false: no instance uses it). */
static bool block_of(mfx *f, de_material m, de__material_params *out) {
    const u32 slot = de__gpu_scene_material_slot(f->r->gpu_scene, m);
    if (slot == DE__GS_NONE)
        return false;
    de__gpu_scene_debug_pack(f->r->gpu_scene, DE__GS_MATERIALS, slot, out);
    return true;
}

static const de__gpu_scene_stats *scene_stats(mfx *f) {
    return de__gpu_scene_stats_get(f->r->gpu_scene);
}

static u32 live_pipelines(mfx *f) {
    return de__gpu_live_count(f->d, DE__GPU_PIPELINE);
}

/* ================================================================================================
 * material_params_layout_match
 * ============================================================================================== */

static char *read_text(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return NULL;
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1u);
    if (buf != NULL && fread(buf, 1, (size_t)n, fp) == (size_t)n)
        buf[n] = '\0';
    fclose(fp);
    return buf;
}

static void strip_comments(char *s) {
    for (char *p = s; *p; ++p)
        if (p[0] == '/' && p[1] == '/')
            while (*p && *p != '\n')
                *p++ = ' ';
}

TEST(materials, material_params_layout_match) {
    char path[1024];
    snprintf(path, sizeof path, "%s/materials/material_params.slang", DE_RMI_TEST_SHADER_DIR);
    char *text = read_text(path);
    ASSERT_TRUE(text != NULL);
    strip_comments(text);
    /* The struct: "public struct DeGpuMaterial { public type name[N]; ... };" */
    char *p = strstr(text, "struct DeGpuMaterial");
    ASSERT_TRUE(p != NULL);
    char *body = strchr(p, '{');
    char *end = body != NULL ? strchr(body, '}') : NULL;
    ASSERT_TRUE(body != NULL && end != NULL);
    *end = '\0';
    u32 nfields = 0;
    const de__material_layout_field *fields = de__material_params_layout(&nfields);
    u32 k = 0, off = 0;
    for (char *q = body + 1; *q;) {
        char *semi = strchr(q, ';');
        if (semi == NULL)
            break;
        *semi = '\0';
        char a[48] = "", b[48] = "", c[64] = "";
        const int n = sscanf(q, "%47s %47s %63s", a, b, c);
        ASSERT_TRUE(n >= 2);
        const char *type = strcmp(a, "public") == 0 ? b : a;
        const char *decl = strcmp(a, "public") == 0 ? c : b;
        const char *br = strchr(decl, '[');
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(br ? br - decl : (long)strlen(decl)), decl);
        const u32 count = br ? (u32)strtoul(br + 1, NULL, 10) : 1u;
        const bool vec = strcmp(type, "uint4") == 0 || strcmp(type, "float4") == 0;
        ASSERT_TRUE(vec || strcmp(type, "uint") == 0 || strcmp(type, "float") == 0);
        const u32 align = vec ? 16u : 4u;
        off = (off + align - 1u) & ~(align - 1u);
        const u32 size = (vec ? 16u : 4u) * count;
        ASSERT_LT(k, nfields);
        if (strcmp(name, fields[k].name) != 0 || off != fields[k].offset || size != fields[k].size)
            fprintf(stderr, "field %u: slang %s %s @%u (%u), C %s %s @%u (%u)\n", k, type, name,
                    off, size, fields[k].slang_type, fields[k].name, fields[k].offset,
                    fields[k].size);
        EXPECT_STREQ(name, fields[k].name);
        EXPECT_STREQ(type, fields[k].slang_type);
        EXPECT_EQ(off, fields[k].offset);
        EXPECT_EQ(size, fields[k].size);
        off += size;
        ++k;
        q = semi + 1;
    }
    EXPECT_EQ(k, nfields);
    EXPECT_EQ(off, (u32)DE__MATERIAL_PARAMS_SIZE);
    EXPECT_EQ(fields[0].offset, 0u); /* model first: classification reads it there */
    EXPECT_STREQ(fields[0].name, "model");
    free(text);

    /* Constants: "public static const uint NAME = value;" */
    text = read_text(path);
    ASSERT_TRUE(text != NULL);
    strip_comments(text);
    u32 nconst = 0;
    const de__material_layout_const *consts = de__material_params_consts(&nconst);
    u32 found = 0;
    for (char *q = strstr(text, "static const uint "); q != NULL;
         q = strstr(q + 1, "static const uint ")) {
        char name[64] = "", value[32] = "";
        ASSERT_EQ(sscanf(q + 18, "%63s = %31[0-9xXa-fA-F]", name, value), 2);
        bool known = false;
        for (u32 i = 0; i < nconst; ++i)
            if (strcmp(consts[i].name, name) == 0) {
                known = true;
                EXPECT_EQ((u32)strtoul(value, NULL, 0), consts[i].value);
            }
        if (!known)
            fprintf(stderr, "constant %s has no C mirror\n", name);
        EXPECT_TRUE(known);
        ++found;
    }
    EXPECT_EQ(found, nconst);
    free(text);

    /* The diffuse transmission colour word: 1 - colour, so zero is white. */
    EXPECT_EQ(de__material_dt_color_pack(1.0f, 1.0f, 1.0f), 0u);
    EXPECT_EQ(de__material_dt_color_pack(0.0f, 0.0f, 0.0f), 0x3FFFFFFFu);
    EXPECT_EQ(de__material_dt_color_pack(1.0f, 0.0f, 2.0f), 0x000FFC00u);
}

/* ================================================================================================
 * Features (pure)
 * ============================================================================================== */

TEST(materials, material_features_rules) {
    de_standard_material s;
    memset(&s, 0, sizeof s);
    EXPECT_EQ(de__material_features(&s, 0u, 0u), 0u);
    s.clearcoat = 0.5f;
    EXPECT_EQ(de__material_features(&s, 0u, 0u), (u32)DE__MATERIAL_FEATURE_CLEARCOAT);
    EXPECT_EQ(de__material_permutation_of(0u, DE__MATERIAL_FEATURE_CLEARCOAT), 1u);
    s.sheen_color.y = 0.1f;
    EXPECT_EQ(de__material_permutation_of(0u, de__material_features(&s, 0u, 0u)), 2u);
    memset(&s, 0, sizeof s);
    s.thickness = 1.0f; /* a volume only with transmission (DENG-301's rule) */
    EXPECT_EQ(de__material_features(&s, 0u, 0u), 0u);
    s.transmission = 0.5f;
    EXPECT_EQ(de__material_features(&s, 0u, 0u),
              (u32)(DE__MATERIAL_FEATURE_TRANSMISSION | DE__MATERIAL_FEATURE_VOLUME |
                    DE__MATERIAL_FEATURE_FORWARD));
    s.transmission = 0.0f;
    s.diffuse_transmission = 0.5f;
    EXPECT_EQ(de__material_features(&s, 0u, 0u),
              (u32)(DE__MATERIAL_FEATURE_DIFFUSE_TRANSMISSION | DE__MATERIAL_FEATURE_VOLUME));
    memset(&s, 0, sizeof s);
    s.double_sided = true;
    EXPECT_EQ(de__material_features(&s, DE_COOKED_ALPHA_BLEND, DE__GS_MATERIAL_MODEL_UNLIT),
              (u32)(DE__MATERIAL_FEATURE_DOUBLE_SIDED | DE__MATERIAL_FEATURE_FORWARD |
                    DE__MATERIAL_FEATURE_UNLIT));
    EXPECT_EQ(de__material_features(&s, DE_COOKED_ALPHA_MASK, 0u),
              (u32)(DE__MATERIAL_FEATURE_DOUBLE_SIDED | DE__MATERIAL_FEATURE_ALPHA_MASK));
    s.clearcoat = nanf(""); /* NaN sets nothing */
    s.double_sided = false;
    EXPECT_EQ(de__material_features(&s, 0u, 0u), 0u);
    /* Permutation counts: Standard 3, Unlit 1, Sprite/Terrain/Water share Standard's. */
    EXPECT_EQ(de__material_permutations_compiled(0u, true), 3u);
    EXPECT_EQ(de__material_permutations_compiled(1u, true), 1u);
    EXPECT_EQ(de__material_permutations_compiled(2u, true), 0u);
    EXPECT_EQ(de__material_permutations_compiled(7u, true), 1u);
    EXPECT_EQ(de__material_permutations_compiled(7u, false), 0u);
}

/* ================================================================================================
 * material_set_params_no_pipeline_rebuild
 * ============================================================================================== */

TEST_F(materials, material_set_params_no_pipeline_rebuild, mfx) {
    start(f, "no_rebuild", (start_opts){.materials = 1000});
    frame(f);
    const u32 pipes = live_pipelines(f);
    const de_render_stats st0 = *de_renderer_stats(f->r);
    EXPECT_EQ(st0.permutations_per_model[0], 3u);
    EXPECT_EQ(st0.permutations_per_model[1], 1u);
    EXPECT_EQ(st0.material_pipelines, 5u); /* + the forward transparent pass's (DENG-305) */
    for (u32 n = 0; n < 8; ++n) {
        for (u32 i = 0; i < 1000; ++i) {
            de_standard_material p;
            ASSERT_EQ(de_material_get_params(f->r, f->mats[i], &p), DE_OK);
            p.roughness = 0.1f + 0.0005f * (f32)i + 0.01f * (f32)n;
            p.base_color.y = 0.25f + 0.05f * (f32)n;
            ASSERT_EQ(de_material_set_params(f->r, f->mats[i], &p), DE_OK);
        }
        frame(f);
        const de_render_stats *st = de_renderer_stats(f->r);
        EXPECT_EQ(live_pipelines(f), pipes); /* nothing was created */
        EXPECT_EQ(st->material_pipelines, st0.material_pipelines);
        EXPECT_EQ(st->material_permutation_switches, 0u);
        EXPECT_EQ(st->material_uploads, 1000u);
        EXPECT_EQ(scene_stats(f)->material_packs, 1000u);
        for (u32 i = 0; i < 1000; i += 97) {
            de__material_params b;
            ASSERT_TRUE(block_of(f, f->mats[i], &b));
            EXPECT_FLOAT_EQ(b.pbr.v[1], 0.1f + 0.0005f * (f32)i + 0.01f * (f32)n, 1e-6);
            EXPECT_FLOAT_EQ(b.base_color.v[1], 0.25f + 0.05f * (f32)n, 1e-6);
            EXPECT_EQ(b.flags & DE__MATERIAL_FLAG_READY, (u32)DE__MATERIAL_FLAG_READY);
        }
    }
}

/* ================================================================================================
 * material_incremental_upload
 * ============================================================================================== */

TEST_F(materials, material_incremental_upload, mfx) {
    start(f, "incremental", (start_opts){.materials = 1000});
    frame(f);
    EXPECT_EQ(de_renderer_stats(f->r)->material_uploads, 0u); /* steady */
    EXPECT_EQ(scene_stats(f)->material_packs, 0u);
    for (u32 i = 0; i < 10; ++i) {
        de_standard_material p;
        ASSERT_EQ(de_material_get_params(f->r, f->mats[i * 100u], &p), DE_OK);
        p.metallic = 0.75f;
        ASSERT_EQ(de_material_set_params(f->r, f->mats[i * 100u], &p), DE_OK);
    }
    frame(f);
    EXPECT_EQ(de_renderer_stats(f->r)->material_uploads, 10u);
    EXPECT_EQ(scene_stats(f)->material_packs, 10u);
    de__material_params b;
    ASSERT_TRUE(block_of(f, f->mats[500], &b));
    EXPECT_FLOAT_EQ(b.pbr.v[0], 0.75f, 1e-6);
    ASSERT_TRUE(block_of(f, f->mats[501], &b));
    EXPECT_FLOAT_EQ(b.pbr.v[0], 1.0f, 1e-6); /* untouched (the glTF default) */
    /* Setting the same values packs the block but uploads nothing. */
    de_standard_material p;
    ASSERT_EQ(de_material_get_params(f->r, f->mats[3], &p), DE_OK);
    ASSERT_EQ(de_material_set_params(f->r, f->mats[3], &p), DE_OK);
    frame(f);
    EXPECT_EQ(de_renderer_stats(f->r)->material_uploads, 0u);
    EXPECT_EQ(scene_stats(f)->material_packs, 1u);
    for (u32 i = 0; i < 3; ++i) {
        frame(f);
        EXPECT_EQ(de_renderer_stats(f->r)->material_uploads, 0u);
        EXPECT_EQ(scene_stats(f)->material_packs, 0u);
    }
    /* Errors. */
    EXPECT_EQ(de_material_set_params(f->r, f->mats[0], NULL), DE_ERR_INVALID_ARGUMENT);
    p.alpha_mode = 3u;
    EXPECT_EQ(de_material_set_params(f->r, f->mats[0], &p), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_material_set_params(f->r, (de_material){0}, &p), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_material_get_params(NULL, f->mats[0], &p), DE_ERR_INVALID_ARGUMENT);
}

/* ================================================================================================
 * material_permutation_switch_counted
 * ============================================================================================== */

static void set_layers(mfx *f, u32 i, f32 clearcoat, f32 sheen) {
    de_standard_material p;
    ASSERT_EQ(de_material_get_params(f->r, f->mats[i], &p), DE_OK);
    p.clearcoat = clearcoat;
    p.sheen_color = (de_vec3){sheen, sheen, sheen};
    ASSERT_EQ(de_material_set_params(f->r, f->mats[i], &p), DE_OK);
}

TEST_F(materials, material_permutation_switch_counted, mfx) {
    start(f, "permutations", (start_opts){.materials = 4});
    const u32 pipes = live_pipelines(f);
    EXPECT_EQ(de__gpu_scene_material_permutation(f->r->gpu_scene, 0), 0u); /* base */
    EXPECT_EQ(de_renderer_stats(f->r)->permutations_per_model[0], 3u);

    set_layers(f, 0, 0.5f, 0.0f); /* clearcoat 0 -> 0.5 */
    frame(f);
    const de_render_stats *st = de_renderer_stats(f->r);
    EXPECT_EQ(st->material_permutation_switches, 1u);
    EXPECT_EQ(st->permutations_per_model[0], 3u);
    EXPECT_EQ(st->material_pipelines, 5u); /* + the forward transparent pass's (DENG-305) */
    EXPECT_EQ(live_pipelines(f), pipes);
    EXPECT_EQ(de__gpu_scene_material_permutation(f->r->gpu_scene, 0), 1u); /* CLEARCOAT */
    de__material_params b;
    ASSERT_TRUE(block_of(f, f->mats[0], &b));
    EXPECT_EQ(b.features & DE__MATERIAL_FEATURE_CLEARCOAT, (u32)DE__MATERIAL_FEATURE_CLEARCOAT);

    set_layers(f, 0, 0.75f, 0.0f); /* within the same feature set */
    frame(f);
    EXPECT_EQ(de_renderer_stats(f->r)->material_permutation_switches, 0u);
    EXPECT_EQ(de_renderer_stats(f->r)->material_uploads, 1u);

    set_layers(f, 1, 0.0f, 0.5f); /* sheen: the all-features permutation */
    frame(f);
    EXPECT_EQ(de_renderer_stats(f->r)->material_permutation_switches, 1u);
    EXPECT_EQ(de__gpu_scene_material_permutation(f->r->gpu_scene, 0), 2u);

    set_layers(f, 0, 0.0f, 0.0f);
    set_layers(f, 1, 0.0f, 0.0f);
    frame(f);
    EXPECT_EQ(de_renderer_stats(f->r)->material_permutation_switches, 2u);
    EXPECT_EQ(de__gpu_scene_material_permutation(f->r->gpu_scene, 0), 0u);
    EXPECT_EQ(live_pipelines(f), pipes);

    /* Unlit is its own model with one kernel. */
    de_standard_material p;
    ASSERT_EQ(de_material_get_params(f->r, f->mats[2], &p), DE_OK);
    p.unlit = true;
    ASSERT_EQ(de_material_set_params(f->r, f->mats[2], &p), DE_OK);
    frame(f);
    ASSERT_TRUE(block_of(f, f->mats[2], &b));
    EXPECT_EQ(b.model, (u32)DE__GS_MATERIAL_MODEL_UNLIT);
    EXPECT_EQ(b.flags & DE__MATERIAL_FLAG_UNLIT, (u32)DE__MATERIAL_FLAG_UNLIT);
    EXPECT_EQ(b.features & DE__MATERIAL_FEATURE_UNLIT, (u32)DE__MATERIAL_FEATURE_UNLIT);
    EXPECT_EQ(de_renderer_stats(f->r)->material_permutation_switches, 1u); /* a model change */
    EXPECT_EQ(live_pipelines(f), pipes);
}

/* ================================================================================================
 * material_texture_reload_rebind
 * ============================================================================================== */

TEST_F(materials, material_texture_reload_rebind, mfx) {
#if !DE__ASSET_RELOAD_ENABLED || !DE_HAS_COOKER
    SKIP("hot reload and the texture importer are compiled out of this build (Release, Shipping)");
#else
    start(f, "reload", (start_opts){.materials = 2, .texture = true, .hot_reload = true});
    de__asset_reload_set_watching(f->s, false);
    const de_texture tex = de_asset_texture(f->s, de_asset_id_from("mi/tex"));
    const de_render_texture *t1 = de_render_texture_get(f->s, tex);
    ASSERT_TRUE(t1 != NULL);
    const u32 old_index = t1->bindless;
    ASSERT_NE(old_index, 0u);
    de__material_params b;
    ASSERT_TRUE(block_of(f, f->mats[0], &b));
    EXPECT_EQ(de__material_params_texture(&b, DE_COOKED_TEX_BASE_COLOR), old_index);

    /* The file changes: a new texture uploads behind the same handle. */
    copy_fixture(f, "npot.png", "tex.png");
    char path[160];
    snprintf(path, sizeof path, "%s/tex.png", f->dir);
    de_render_uploader *u = de__renderer_uploader(f->r);
    ASSERT_TRUE(u != NULL);
    de__uploader_test_hold_fence(u, 0); /* the new copies wait: no stall, the old one serves */
    de__asset_reload_on_change(f->s, path, DE_WATCH_MODIFIED, false);
    const u64 t0 = de_time_now_ns();
    while (de_asset_get_stats(f->s).uploads_in_flight == 0) {
        frame(f);
        ASSERT_TRUE(de_time_now_ns() - t0 < MI_WAIT_NS);
        (void)de__asset_wake_block(f->s, 2000000u);
    }
    for (u32 i = 0; i < 3; ++i) {
        frame(f);
        ASSERT_TRUE(block_of(f, f->mats[0], &b));
        EXPECT_EQ(de__material_params_texture(&b, DE_COOKED_TEX_BASE_COLOR), old_index);
        EXPECT_EQ(de_renderer_stats(f->r)->material_uploads, 0u);
    }
    /* The copies complete: the frame that swaps the texture in rebinds both blocks. */
    de__uploader_test_hold_fence(u, UINT64_MAX);
    de__asset_reload_stats rs;
    de__asset_reload_get_stats(f->s, &rs);
    const u64 swapped0 = rs.swapped;
    frame(f);
    de__asset_reload_get_stats(f->s, &rs);
    ASSERT_EQ(rs.swapped, swapped0 + 1u);
    const de_render_texture *t2r = de_render_texture_get(f->s, tex);
    ASSERT_TRUE(t2r != NULL);
    EXPECT_NE(t2r->bindless, old_index);
    for (u32 i = 0; i < 2; ++i) {
        ASSERT_TRUE(block_of(f, f->mats[i], &b));
        EXPECT_EQ(de__material_params_texture(&b, DE_COOKED_TEX_BASE_COLOR), t2r->bindless);
    }
    EXPECT_EQ(de_renderer_stats(f->r)->material_uploads, 2u); /* the same frame */
    frame(f);
    EXPECT_EQ(de_renderer_stats(f->r)->material_uploads, 0u);
#endif
}

/* ================================================================================================
 * material_runtime_instance_copy
 * ============================================================================================== */

TEST_F(materials, material_runtime_instance_copy, mfx) {
    start(f, "instances", (start_opts){.materials = 2});
    const de_material base = f->mats[0];
    const de_material inst = de_material_create_instance(f->r, base);
    ASSERT_TRUE(DE_HANDLE_VALID(inst));
    EXPECT_TRUE(de__material_is_instance(inst));
    EXPECT_FALSE(de__material_is_instance(base));
    de_standard_material pb, pi;
    ASSERT_EQ(de_material_get_params(f->r, base, &pb), DE_OK);
    ASSERT_EQ(de_material_get_params(f->r, inst, &pi), DE_OK);
    EXPECT_EQ(memcmp(&pb, &pi, sizeof pb), 0);

    pi.roughness = 0.9f;
    pi.base_color = (de_vec4){0.0f, 1.0f, 0.0f, 1.0f};
    ASSERT_EQ(de_material_set_params(f->r, inst, &pi), DE_OK);
    ASSERT_EQ(de_material_get_params(f->r, base, &pb), DE_OK);
    EXPECT_FLOAT_EQ(pb.roughness, 0.25f, 1e-6); /* the base keeps its own */
    const de_entity e = spawn(f, inst, 100.0);
    ASSERT_NE(e.bits, 0u);
    frame(f);
    EXPECT_EQ(de_renderer_stats(f->r)->material_instances, 1u);
    de__material_params b;
    ASSERT_TRUE(block_of(f, inst, &b));
    EXPECT_EQ(b.flags & DE__MATERIAL_FLAG_READY, (u32)DE__MATERIAL_FLAG_READY);
    EXPECT_FLOAT_EQ(b.pbr.v[1], 0.9f, 1e-6);
    EXPECT_FLOAT_EQ(b.base_color.v[1], 1.0f, 1e-6);
    ASSERT_TRUE(block_of(f, base, &b));
    EXPECT_FLOAT_EQ(b.pbr.v[1], 0.25f, 1e-6);

    /* An instance of the instance, then destroying the first: its block loses READY. */
    const de_material inst2 = de_material_create_instance(f->r, inst);
    ASSERT_TRUE(DE_HANDLE_VALID(inst2));
    ASSERT_EQ(de_material_get_params(f->r, inst2, &pi), DE_OK);
    EXPECT_FLOAT_EQ(pi.roughness, 0.9f, 1e-6);
    de_material_destroy_instance(f->r, inst);
    frame(f);
    EXPECT_EQ(de_renderer_stats(f->r)->material_instances, 1u);
    ASSERT_TRUE(block_of(f, inst, &b));
    EXPECT_EQ(b.flags & DE__MATERIAL_FLAG_READY, 0u);
    EXPECT_EQ(de_material_set_params(f->r, inst, &pi), DE_ERR_INVALID_ARGUMENT); /* stale */
    EXPECT_FALSE(DE_HANDLE_VALID(de_material_create_instance(f->r, inst)));
    /* The slot is reused with another generation: the old handle stays stale. */
    const de_material inst3 = de_material_create_instance(f->r, base);
    ASSERT_TRUE(DE_HANDLE_VALID(inst3));
    EXPECT_NE(inst3.id, inst.id);
    EXPECT_EQ(de_material_get_params(f->r, inst, &pi), DE_ERR_INVALID_ARGUMENT);
    /* A material that is not loaded cannot be copied. */
    EXPECT_FALSE(DE_HANDLE_VALID(
        de_material_create_instance(f->r, de_asset_material(f->s, de_asset_id_from("mi/none")))));
    de_error_clear();
    de_material_destroy_instance(f->r, inst2);
    de_material_destroy_instance(f->r, inst3);

    /* A cooked material that names its model (DMAT v2 model hash) resolves by the registry's
     * names; a name nobody registered (yet) resolves as Standard. */
    de_render_material *rec = de__material_instances_get(f->r->material_instances, f->mats[1]);
    ASSERT_TRUE(rec != NULL);
    rec->model_hash = de_cooked_material_model_hash("terrain");
    de__gpu_scene_material_changed(f->r->gpu_scene, f->mats[1]);
    frame(f);
    ASSERT_TRUE(block_of(f, f->mats[1], &b));
    EXPECT_EQ(b.model, (u32)DE__GS_MATERIAL_MODEL_TERRAIN);
    rec = de__material_instances_get(f->r->material_instances, f->mats[1]);
    rec->model_hash = de_cooked_material_model_hash("toon");
    de__gpu_scene_material_changed(f->r->gpu_scene, f->mats[1]);
    frame(f);
    ASSERT_TRUE(block_of(f, f->mats[1], &b));
    EXPECT_EQ(b.model, (u32)DE__GS_MATERIAL_MODEL_STANDARD);
}

/* ================================================================================================
 * material_mesh_default_slot
 * ============================================================================================== */

TEST_F(materials, material_mesh_default_slot, mfx) {
    /* The mesh's submesh names mi/mat0; the entity's material slots are all empty. */
    start(f, "mesh_default",
          (start_opts){.materials = 1, .mesh_default = mat_id(0).hash, .no_entities = true});
    const de_entity e = spawn(f, (de_material){0}, 0.0);
    ASSERT_NE(e.bits, 0u);
    frame(f);
    const u32 slot = de__gpu_scene_material_slot(f->r->gpu_scene, f->mats[0]);
    ASSERT_NE(slot, DE__GS_NONE);
    /* Only the instance's empty slot can reference it: the mesh's default resolved into it. */
    EXPECT_EQ(scene_stats(f)->live_materials, 1u);
    de__material_params b;
    ASSERT_TRUE(block_of(f, f->mats[0], &b));
    EXPECT_EQ(b.flags & DE__MATERIAL_FLAG_READY, (u32)DE__MATERIAL_FLAG_READY);
}

/* DENG-533: borrowed GPU generations remain valid through instance use, cloning,
 * transactional replacement, caller retirement, and renderer teardown. */
TEST_F(materials, gpu_texture_override_ownership_clone_resize_and_stale_handles, mfx) {
    start(f, "gpu_bindings", (start_opts){.materials = 1, .no_entities = true});
    de_material instance = de_material_create_instance(f->r, f->mats[0]);
    ASSERT_TRUE(instance.id);
    de_gpu_texture old = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.width = 32,
                                     .height = 16,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_RENDER_TARGET});
    ASSERT_TRUE(old.id);
    ASSERT_EQ(de_material_set_gpu_texture(f->r, instance, DE_MATERIAL_TEXTURE_BASE_COLOR, old),
              DE_OK);
    ASSERT_EQ(de_material_set_gpu_texture(f->r, instance, DE_MATERIAL_TEXTURE_EMISSIVE, old),
              DE_OK);
    EXPECT_EQ(de__material_instances_gpu_bindings(f->r->material_instances), 2u);
    ASSERT_TRUE(spawn(f, instance, 0).bits);
    frame(f);
    de__material_params block;
    ASSERT_TRUE(block_of(f, instance, &block));
    u32 sampled = de_gpu_texture_bindless(f->d, old, 0, false);
    EXPECT_EQ(de__material_params_texture(&block, DE_COOKED_TEX_BASE_COLOR), sampled);
    EXPECT_EQ(de__material_params_texture(&block, DE_COOKED_TEX_EMISSIVE), sampled);
    u32 count;
    const de__material_sample *samples = de__gpu_scene_material_samples(f->r->gpu_scene, &count);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(samples[0].texture.id, old.id);
    EXPECT_EQ(samples[0].layer_mask, 1u);
    /* Every immutable world buffer needs its first populated allocation. */
    for (u32 i = 0; i < DE__RW_MAX_FRAMES; ++i)
        frame(f);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 i = 0; i < 16; ++i)
        frame(f);
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    de_material clone = de_material_create_instance(f->r, instance);
    ASSERT_TRUE(clone.id);
    EXPECT_EQ(de__material_instances_gpu_bindings(f->r->material_instances), 4u);
    de_gpu_texture_destroy(f->d, old);
    EXPECT_TRUE(de__gpu_get_texture(f->d, old) != NULL);
    /* Pending caller destruction prevents acquiring a new lease, not use of an
     * existing binding. A failed clone must not consume a slot or release the base. */
    u32 live = de__material_instances_live(f->r->material_instances);
    EXPECT_EQ(de_material_create_instance(f->r, instance).id, 0u);
    EXPECT_EQ(de__material_instances_live(f->r->material_instances), live);
    de_gpu_texture resized = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.width = 64,
                                     .height = 32,
                                     .format = DE_FMT_RGBA16_FLOAT,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_RENDER_TARGET});
    ASSERT_TRUE(resized.id);
    ASSERT_EQ(de_material_set_gpu_texture(f->r, instance, DE_MATERIAL_TEXTURE_BASE_COLOR, resized),
              DE_OK);
    ASSERT_EQ(de_material_set_gpu_texture(f->r, instance, DE_MATERIAL_TEXTURE_EMISSIVE, resized),
              DE_OK);
    de_material_destroy_instance(f->r, clone);
    EXPECT_TRUE(de__gpu_get_texture(f->d, old) == NULL);
    EXPECT_EQ(de_material_set_gpu_texture(f->r, instance, DE_MATERIAL_TEXTURE_NORMAL, old),
              DE_ERR_INVALID_ARGUMENT);
    de_standard_material params;
    ASSERT_EQ(de_material_get_params(f->r, instance, &params), DE_OK);
    params.roughness = .75f;
    ASSERT_EQ(de_material_set_params(f->r, instance, &params), DE_OK);
    frame(f);
    ASSERT_TRUE(block_of(f, instance, &block));
    EXPECT_EQ(de__material_params_texture(&block, DE_COOKED_TEX_BASE_COLOR),
              de_gpu_texture_bindless(f->d, resized, 0, false));
    ASSERT_EQ(de_material_set_gpu_texture(f->r, instance, DE_MATERIAL_TEXTURE_EMISSIVE,
                                          (de_gpu_texture){0}),
              DE_OK);
    EXPECT_EQ(de__material_instances_gpu_bindings(f->r->material_instances), 1u);
    de_gpu_texture_destroy(f->d, resized);
    de_material_destroy_instance(f->r, instance);
    EXPECT_TRUE(de__gpu_get_texture(f->d, resized) == NULL);
    EXPECT_EQ(de_material_set_gpu_texture(f->r, instance, DE_MATERIAL_TEXTURE_BASE_COLOR,
                                          (de_gpu_texture){0}),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__material_instances_gpu_bindings(f->r->material_instances), 0u);
}

TEST_F(materials, gpu_texture_override_rejects_invalid_input_without_replacing_binding, mfx) {
    start(f, "gpu_binding_errors", (start_opts){.materials = 1, .no_entities = true});
    de_material instance = de_material_create_instance(f->r, f->mats[0]);
    de_gpu_texture valid = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.width = 16,
                                     .height = 8,
                                     .format = DE_FMT_RGBA8_UNORM,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_RENDER_TARGET});
    ASSERT_TRUE(valid.id);
    ASSERT_EQ(de_material_set_gpu_texture(f->r, instance, DE_MATERIAL_TEXTURE_BASE_COLOR, valid),
              DE_OK);
    de_gpu_texture not_sampled =
        de_gpu_texture_create(f->d, &(de_gpu_texture_desc){.width = 16,
                                                           .height = 8,
                                                           .format = DE_FMT_RGBA8_UNORM,
                                                           .usage = DE_USAGE_RENDER_TARGET});
    ASSERT_TRUE(not_sampled.id);
    EXPECT_EQ(de_material_set_gpu_texture(NULL, instance, DE_MATERIAL_TEXTURE_BASE_COLOR, valid),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_material_set_gpu_texture(f->r, f->mats[0], DE_MATERIAL_TEXTURE_BASE_COLOR, valid),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_material_set_gpu_texture(f->r, instance, (de_material_texture_slot)99, valid),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(
        de_material_set_gpu_texture(f->r, instance, DE_MATERIAL_TEXTURE_BASE_COLOR, not_sampled),
        DE_ERR_INVALID_ARGUMENT);
    de_render_material *rec = de__material_instances_get(f->r->material_instances, instance);
    EXPECT_EQ(rec->gpu_textures[DE_COOKED_TEX_BASE_COLOR].id, valid.id);
    EXPECT_EQ(de__gpu_get_texture(f->d, not_sampled)->leases, 0u);
    de_gpu_texture integer = de_gpu_texture_create(
        f->d, &(de_gpu_texture_desc){.width = 16,
                                     .height = 8,
                                     .format = DE_FMT_R32_UINT,
                                     .usage = DE_USAGE_SAMPLED | DE_USAGE_RENDER_TARGET});
    ASSERT_TRUE(integer.id);
    EXPECT_EQ(de_material_set_gpu_texture(f->r, instance, DE_MATERIAL_TEXTURE_BASE_COLOR, integer),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__gpu_get_texture(f->d, integer)->leases, 0u);
    de_gpu_texture_destroy(f->d, integer);
    de_gpu_texture_destroy(f->d, not_sampled);
    de_gpu_texture_destroy(f->d, valid);
    /* Match uploader ownership: assets unload before their renderer is freed. */
    de__render_world_wait_idle(f->r);
    de_asset_system_destroy(f->s);
    f->s = NULL;
    /* Renderer teardown releases the binding after its scene and frames drain. */
    de_renderer_destroy(f->r);
    f->r = NULL;
    EXPECT_TRUE(de__gpu_get_texture(f->d, valid) == NULL);
}
