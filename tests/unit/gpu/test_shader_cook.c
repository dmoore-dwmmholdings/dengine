/*
 * test_shader_cook.c: the shader pipeline (DENG-289, SPEC §11.6, §12.2): the `shader` importer
 * (tools/cook/importers/shader/) through the cooker, the cooked format and the runtime type
 * (src/gpu/shader/), in SOURCE mode with the in-process cooker and hot reload.
 *
 * The sources in tests/unit/gpu/data/shaders/ are copied into a scratch directory of the build
 * tree (shd:/<test>/shaders/), so a test can edit them. Every test needs the Slang compiler of
 * this host (third_party/slang/); without it they skip.
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "asset/hot_reload_internal.h"
#include "asset/load_internal.h"
#include "asset/manifest_internal.h"
#include "cook/importers/shader/shader_importer.h"
#include "cook/importers/shader/slang_compile.h"
#include "cook/lib/de_cooker.h"
#include "cook/lib/decache.h"
#include "gpu/shader/shader_format.h"
#include "gpu/shader/shader_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if DE_SHADER_TEST_METAL
int de_test_metallib_load(const void *data, size_t size, const char *entry, char *err, size_t cap);
#endif

#define WAIT_NS ((u64)60 * 1000000000u)

/* The cook cache does not store a cook whose files changed less than its racy window (2 s)
 * before the cook began; tests that check stores let the copied sources age first. */
#define AGE_NS ((u64)DE__DECACHE_RACY_NS + 200000000u)

/* ---- Fixture --------------------------------------------------------------------------------- */

typedef struct shd_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    char root[64]; /* shd:/<test>: the asset root */
} shd_fx;

static void mount_once(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("shd", DE_SHADER_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("shdata", DE_SHADER_TEST_DATA_DIR, 0, 0), DE_OK);
    mounted = true;
}

static bool slang_ok(void) {
    char why[256];
    if (de__slang_available(why, sizeof why))
        return true;
    printf("  (Slang not available: %s)\n", why);
    return false;
}

static void shd_fx_setup(shd_fx *f) {
    mount_once();
    memset(f, 0, sizeof *f);
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    de_fp_env_init_thread();
}

static void shd_fx_teardown(shd_fx *f) {
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
}

static void put_text(const char *vpath, const char *text) {
    ASSERT_EQ(de_file_write_all_atomic(vpath, text, strlen(text)), DE_OK);
}

/* Copies data/shaders/<name> to <root>/shaders/<name>. */
static void copy_in(shd_fx *f, const char *name) {
    char src[128], dst[128];
    snprintf(src, sizeof src, "shdata:/%s", name);
    snprintf(dst, sizeof dst, "%s/shaders/%s", f->root, name);
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all(src, NULL, &data, &size), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic(dst, data, size), DE_OK);
    DE_FREE(de_heap_allocator(), data, (size_t)size + 1u);
}

/* Deletes every file below `vpath` (an earlier run's cook cache); directories stay. */
static bool delete_fn(const char *name, bool is_dir, void *user) {
    char path[256];
    snprintf(path, sizeof path, "%s/%s", (const char *)user, name);
    if (is_dir)
        (void)de_dir_list(path, delete_fn, path);
    (void)de_file_delete(path); /* a directory once it is empty */
    return true;
}

static void delete_files_below(const char *vpath) {
    char path[256];
    snprintf(path, sizeof path, "%s", vpath);
    (void)de_dir_list(path, delete_fn, path);
}

/* shd:/<name>/shaders/ with every test source in it, and no cook cache left from a last run. */
static void fx_dir(shd_fx *f, const char *name) {
    snprintf(f->root, sizeof f->root, "shd:/%s", name);
    (void)de_dir_create(f->root);
    char dir[96];
    snprintf(dir, sizeof dir, "%s/decache", f->root);
    delete_files_below(dir);
    snprintf(dir, sizeof dir, "%s/shaders", f->root);
    (void)de_dir_create(dir);
    static const char *const files[] = {"compute.slang", "tint.slang", "triangle.slang",
                                        "mesh.slang", "too_many_push_constants.slang"};
    for (u32 i = 0; i < sizeof files / sizeof files[0]; ++i)
        copy_in(f, files[i]);
}

static de_asset_type_desc shader_type(void) {
    static const char *const exts[] = {".slang"};
    de_asset_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "shader";
    d.extensions = exts;
    d.extension_count = 1;
    d.importer_version = de_shader_importer_version();
    d.import = de_shader_import;
    return d;
}

static de_cooker *make_cooker(shd_fx *f, de_cook_platform platform, const char *cache) {
    de_cooker *c = NULL;
    de_cooker_desc d = {.allocator = &f->alloc,
                        .platform = platform,
                        .cache_dir = cache,
                        .verify_determinism = DE_COOK_VERIFY_OFF};
    EXPECT_EQ(de_cooker_create(&d, &c), DE_OK);
    de_asset_type_desc t = shader_type();
    EXPECT_EQ(de_cooker_register_type(c, &t), DE_OK);
    return c;
}

/* Settings: string arrays, keys sorted. NULL arrays are left out. */
typedef struct shd_settings {
    de__manifest_kv kv[3];
    u32 count;
} shd_settings;

static shd_settings settings(const char *const *entries, u32 ne, const char *const *perms, u32 np,
                             const char *const *targets, u32 nt) {
    shd_settings s;
    memset(&s, 0, sizeof s);
    const char *keys[3] = {"entry_points", "permutations", "targets"};
    const char *const *arrs[3] = {entries, perms, targets};
    u32 counts[3] = {ne, np, nt};
    for (u32 i = 0; i < 3; ++i) {
        if (arrs[i] == NULL)
            continue;
        de__manifest_kv *kv = &s.kv[s.count++];
        kv->key = keys[i];
        kv->value.type = DE__MANIFEST_STRING_ARRAY;
        kv->value.u.arr.items = arrs[i];
        kv->value.u.arr.count = counts[i];
    }
    return s;
}

static de_result cook(shd_fx *f, de_cooker *c, const char *source, const shd_settings *s,
                      de_cook_result *out) {
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "shaders/test";
    a.type = "shader";
    a.source = source;
    a.file = "shd:/assets.toml";
    a.settings = s->kv;
    a.setting_count = s->count;
    de_cook_input in = {.asset = &a, .asset_root = f->root, .source = NULL};
    return de_cooker_cook(c, &in, out);
}

static const de__shader_record *find_rec(const de__shader_view *v, u32 target, u32 perm,
                                         const char *entry, de__shader_record *buf) {
    for (u32 e = 0; e < v->header.entry_count; ++e) {
        const char *name = NULL;
        (void)de__shader_view_entry(v, e, &name);
        if (strcmp(name, entry) != 0)
            continue;
        i32 i = de__shader_view_find(v, target, perm, e);
        if (i < 0)
            return NULL;
        *buf = de__shader_view_record(v, (u32)i);
        return buf;
    }
    return NULL;
}

/* spirv-val on `code` when the Vulkan SDK's spirv-val is on PATH: 1 valid, 0 invalid,
 * -1 no validator. */
static int spirv_validate(const void *code, u64 size, const char *name) {
    const char *exe = "spirv-val"; /* the Vulkan SDK's setup puts it on PATH */
    char cmd[1200];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "%s --version >NUL 2>&1", exe);
#else
    snprintf(cmd, sizeof cmd, "%s --version >/dev/null 2>&1", exe);
#endif
    if (system(cmd) != 0)
        return -1;
    char vpath[128], real[512];
    snprintf(vpath, sizeof vpath, "shd:/%s.spv", name);
    if (de_file_write_all_atomic(vpath, code, size) != DE_OK)
        return 0;
    snprintf(real, sizeof real, "%s/%s.spv", DE_SHADER_TEST_TMP_DIR, name);
    snprintf(cmd, sizeof cmd, "%s --target-env vulkan1.3 \"%s\"", exe, real);
    return system(cmd) == 0 ? 1 : 0;
}

static const char *const k_compute_entries[] = {"cs_main"};
static const char *const k_pair_entries[] = {"vs_main:vertex", "ps_main:pixel"};
static const char *const k_t_spirv[] = {"spirv"};
static const char *const k_t_dxil[] = {"dxil"};
static const char *const k_t_all[] = {"spirv", "dxil", "metallib"};
static const char *const k_perms_tint[] = {"", "USE_TINT=1"};

/* ---- shader.cook_spirv ------------------------------------------------------------------------
 *
 * The compute shader and the vertex/pixel pair (bindless + push constants) cook to SPIR-V: one
 * record per entry, SPIR-V magic, valid under spirv-val (Vulkan 1.3) when the SDK is installed. */

TEST_F(shader, cook_spirv, shd_fx) {
    if (!slang_ok())
        SKIP("no Slang compiler for this host");
    fx_dir(f, "cook_spirv");
    de_cooker *c = make_cooker(f, DE_COOK_PLATFORM_LINUX, NULL);
    const char *const files[] = {"shaders/compute.slang", "shaders/triangle.slang"};
    shd_settings ss[2] = {settings(k_compute_entries, 1, NULL, 0, NULL, 0),
                          settings(k_pair_entries, 2, NULL, 0, k_t_spirv, 1)};
    const char *const entries[2][2] = {{"cs_main", NULL}, {"vs_main", "ps_main"}};
    for (u32 k = 0; k < 2; ++k) {
        de_cook_result r;
        ASSERT_EQ(cook(f, c, files[k], &ss[k], &r), DE_OK);
        de__shader_view v;
        ASSERT_EQ(de__shader_view_init(r.data, r.size, &v), DE_OK);
        for (u32 e = 0; e < 2 && entries[k][e] != NULL; ++e) {
            de__shader_record rec;
            ASSERT_TRUE(find_rec(&v, DE__SHADER_TARGET_SPIRV, 0, entries[k][e], &rec) != NULL);
            const u8 *code = v.base + rec.code_offset;
            ASSERT_GE(rec.code_size, 20u);
            EXPECT_EQ(code[0] | (code[1] << 8) | (code[2] << 16) | ((u32)code[3] << 24),
                      0x07230203u);
            int ok = spirv_validate(code, rec.code_size, entries[k][e]);
            if (ok < 0)
                printf("  (spirv-val not found: validation skipped)\n");
            else
                EXPECT_EQ(ok, 1);
        }
        /* Linux cooks SPIR-V only, by default. */
        de__shader_record rec;
        EXPECT_TRUE(find_rec(&v, DE__SHADER_TARGET_METALLIB, 0, entries[k][0], &rec) == NULL);
        de_cook_result_free(&r);
    }
    de_cooker_destroy(c);
}

/* ---- shader.cook_metallib ---------------------------------------------------------------------
 *
 * On a macOS host the same shaders cook to metallib, which the Metal device loads with
 * newLibraryWithData: and which has the entry point. The mesh shader too (R2). */

TEST_F(shader, cook_metallib, shd_fx) {
#if !DE_SHADER_TEST_METAL
    SKIP("metallib cooks on macOS hosts only");
#else
    if (!slang_ok())
        SKIP("no Slang compiler for this host");
    if (!de__slang_target_supported(DE__SLANG_METALLIB))
        SKIP("no Metal toolchain (xcrun metal) on this host");
    fx_dir(f, "cook_metallib");
    de_cooker *c = make_cooker(f, DE_COOK_PLATFORM_MACOS, NULL);
    static const char *const k_t_metallib[] = {"metallib"};
    static const char *const mesh_entries[] = {"ms_main:mesh", "mesh_ps:pixel"};
    const char *const files[] = {"shaders/compute.slang", "shaders/triangle.slang",
                                 "shaders/mesh.slang"};
    shd_settings ss[3] = {settings(k_compute_entries, 1, NULL, 0, k_t_metallib, 1),
                          settings(k_pair_entries, 2, NULL, 0, k_t_metallib, 1),
                          settings(mesh_entries, 2, NULL, 0, k_t_metallib, 1)};
    const char *const entries[3][2] = {
        {"cs_main", NULL}, {"vs_main", "ps_main"}, {"ms_main", "mesh_ps"}};
    for (u32 k = 0; k < 3; ++k) {
        de_cook_result r;
        ASSERT_EQ(cook(f, c, files[k], &ss[k], &r), DE_OK);
        de__shader_view v;
        ASSERT_EQ(de__shader_view_init(r.data, r.size, &v), DE_OK);
        for (u32 e = 0; e < 2 && entries[k][e] != NULL; ++e) {
            de__shader_record rec;
            ASSERT_TRUE(find_rec(&v, DE__SHADER_TARGET_METALLIB, 0, entries[k][e], &rec) != NULL);
            const u8 *code = v.base + rec.code_offset;
            ASSERT_GE(rec.code_size, 4u);
            EXPECT_EQ(memcmp(code, "MTLB", 4), 0);
            char err[256] = "";
            int ok = de_test_metallib_load(code, rec.code_size, entries[k][e], err, sizeof err);
            if (ok < 0) {
                printf("  (no Metal device: load skipped)\n");
            } else {
                if (ok != 1)
                    printf("  %s: %s\n", entries[k][e], err);
                EXPECT_EQ(ok, 1);
            }
        }
        /* Only the targets asked for. */
        de__shader_record rec;
        EXPECT_TRUE(find_rec(&v, DE__SHADER_TARGET_SPIRV, 0, entries[k][0], &rec) == NULL);
        de_cook_result_free(&r);
    }
    de_cooker_destroy(c);
#endif
}

/* ---- shader.reflection ------------------------------------------------------------------------
 *
 * Stage, threadgroup size, push-constant bytes, bindless flags and mesh limits per entry, on
 * every target this host cooks. Entry points come from [shader] attributes when the settings do
 * not list them. */

TEST_F(shader, reflection, shd_fx) {
    if (!slang_ok())
        SKIP("no Slang compiler for this host");
    fx_dir(f, "reflection");
    de_cooker *c = make_cooker(f, DE_COOK_PLATFORM_MACOS, NULL);
    shd_settings none = settings(NULL, 0, NULL, 0, k_t_all, 3);

    de_cook_result r;
    ASSERT_EQ(cook(f, c, "shaders/compute.slang", &none, &r), DE_OK);
    de__shader_view v;
    ASSERT_EQ(de__shader_view_init(r.data, r.size, &v), DE_OK);
    EXPECT_EQ(v.header.entry_count, 1u);
    EXPECT_EQ(v.header.permutation_count, 1u);
    for (u32 t = DE__SHADER_TARGET_SPIRV; t < DE__SHADER_TARGET_COUNT_; ++t) {
        de__shader_record rec;
        if (find_rec(&v, t, 0, "cs_main", &rec) == NULL)
            continue; /* a target this host cannot compile */
        EXPECT_EQ(rec.stage, (u32)DE__SHADER_STAGE_COMPUTE);
        EXPECT_EQ(rec.threadgroup[0], 8u);
        EXPECT_EQ(rec.threadgroup[1], 4u);
        EXPECT_EQ(rec.threadgroup[2], 1u);
        EXPECT_EQ(rec.push_constant_bytes, 16u);
        EXPECT_EQ(rec.bindless_flags, (u32)(DE__SHADER_USES_TEXTURES | DE__SHADER_USES_SAMPLERS |
                                            DE__SHADER_USES_BUFFERS));
        EXPECT_EQ(rec.mesh_limits, 0u);
    }
    de__shader_record rec;
    EXPECT_TRUE(find_rec(&v, DE__SHADER_TARGET_SPIRV, 0, "cs_main", &rec) != NULL);
    de_cook_result_free(&r);

    ASSERT_EQ(cook(f, c, "shaders/triangle.slang", &none, &r), DE_OK);
    ASSERT_EQ(de__shader_view_init(r.data, r.size, &v), DE_OK);
    EXPECT_EQ(v.header.entry_count, 2u);
    ASSERT_TRUE(find_rec(&v, DE__SHADER_TARGET_SPIRV, 0, "vs_main", &rec) != NULL);
    EXPECT_EQ(rec.stage, (u32)DE__SHADER_STAGE_VERTEX);
    EXPECT_EQ(rec.threadgroup[0], 0u);
    EXPECT_EQ(rec.push_constant_bytes, 32u);
    EXPECT_EQ(rec.bindless_flags, (u32)DE__SHADER_USES_BUFFERS);
    ASSERT_TRUE(find_rec(&v, DE__SHADER_TARGET_SPIRV, 0, "ps_main", &rec) != NULL);
    EXPECT_EQ(rec.stage, (u32)DE__SHADER_STAGE_PIXEL);
    EXPECT_EQ(rec.push_constant_bytes, 32u);
    EXPECT_EQ(rec.bindless_flags, (u32)(DE__SHADER_USES_TEXTURES | DE__SHADER_USES_SAMPLERS));
    de_cook_result_free(&r);

    ASSERT_EQ(cook(f, c, "shaders/mesh.slang", &none, &r), DE_OK);
    ASSERT_EQ(de__shader_view_init(r.data, r.size, &v), DE_OK);
    ASSERT_TRUE(find_rec(&v, DE__SHADER_TARGET_SPIRV, 0, "ms_main", &rec) != NULL);
    EXPECT_EQ(rec.stage, (u32)DE__SHADER_STAGE_MESH);
    EXPECT_EQ(rec.threadgroup[0], 32u);
    EXPECT_EQ(rec.push_constant_bytes, 4u);
    EXPECT_EQ(rec.mesh_limits & 0xffffu, 64u);
    EXPECT_EQ(rec.mesh_limits >> 16, 124u);
    if (find_rec(&v, DE__SHADER_TARGET_METALLIB, 0, "ms_main", &rec) != NULL)
        EXPECT_EQ(rec.mesh_limits >> 16, 124u); /* the same on every target */
    de_cook_result_free(&r);
    de_cooker_destroy(c);
}

/* ---- shader.push_constant_limit ---------------------------------------------------------------
 *
 * 132 bytes of push constants fail the cook with a message that names the entry point, the size
 * and the limit. */

TEST_F(shader, push_constant_limit, shd_fx) {
    if (!slang_ok())
        SKIP("no Slang compiler for this host");
    fx_dir(f, "push_limit");
    de_cooker *c = make_cooker(f, DE_COOK_PLATFORM_LINUX, NULL);
    shd_settings s = settings(NULL, 0, NULL, 0, NULL, 0);
    de_cook_result r;
    de_error_clear();
    EXPECT_EQ(cook(f, c, "shaders/too_many_push_constants.slang", &s, &r), DE_ERR_INVALID_ARGUMENT);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    printf("  %s\n", e->message);
    EXPECT_TRUE(strstr(e->message, "\"cs_main\"") != NULL);
    EXPECT_TRUE(strstr(e->message, "132 bytes of push constants") != NULL);
    EXPECT_TRUE(strstr(e->message, "limit is 128") != NULL);
    de_cooker_destroy(c);
}

/* ---- shader.cook_deterministic ----------------------------------------------------------------
 *
 * Two cooks (two cookers, and the cooker's own double import) give byte-identical output. */

TEST_F(shader, cook_deterministic, shd_fx) {
    if (!slang_ok())
        SKIP("no Slang compiler for this host");
    fx_dir(f, "deterministic");
    shd_settings s = settings(NULL, 0, k_perms_tint, 2, k_t_all, 3);
    de_cooker *c1 = make_cooker(f, DE_COOK_PLATFORM_MACOS, NULL);
    de_cooker *c2 = NULL;
    de_cooker_desc d = {.allocator = &f->alloc,
                        .platform = DE_COOK_PLATFORM_MACOS,
                        .verify_determinism = DE_COOK_VERIFY_ON};
    ASSERT_EQ(de_cooker_create(&d, &c2), DE_OK);
    de_asset_type_desc t = shader_type();
    ASSERT_EQ(de_cooker_register_type(c2, &t), DE_OK);
    de_cook_result a, b;
    ASSERT_EQ(cook(f, c1, "shaders/compute.slang", &s, &a), DE_OK);
    ASSERT_EQ(cook(f, c2, "shaders/compute.slang", &s, &b), DE_OK); /* imports twice, compares */
    ASSERT_EQ(a.size, b.size);
    EXPECT_EQ(memcmp(a.data, b.data, (size_t)a.size), 0);
    EXPECT_EQ(a.output_hash, b.output_hash);
    /* The import is the one extra source: the engine modules are not files, and Slang's probes
     * for them and for precompiled modules never reach the project. */
    ASSERT_EQ(a.extra_source_count, 1u);
    EXPECT_STREQ(a.extra_sources[0], "shaders/tint.slang");
    /* The permutations differ. */
    de__shader_view v;
    ASSERT_EQ(de__shader_view_init(a.data, a.size, &v), DE_OK);
    EXPECT_EQ(v.header.permutation_count, 2u);
    EXPECT_STREQ(de__shader_view_permutation(&v, 1), "USE_TINT=1");
    de__shader_record r0, r1;
    ASSERT_TRUE(find_rec(&v, DE__SHADER_TARGET_SPIRV, 0, "cs_main", &r0) != NULL);
    ASSERT_TRUE(find_rec(&v, DE__SHADER_TARGET_SPIRV, 1, "cs_main", &r1) != NULL);
    EXPECT_TRUE(r0.code_size != r1.code_size ||
                memcmp(v.base + r0.code_offset, v.base + r1.code_offset, r0.code_size) != 0);
    de_cook_result_free(&a);
    de_cook_result_free(&b);
    de_cooker_destroy(c1);
    de_cooker_destroy(c2);
}

/* ---- shader.import_dependency_invalidates -----------------------------------------------------
 *
 * With a cook cache: a second cook is served from the cache with the same bytes; editing the
 * imported tint.slang invalidates the entry (a miss), and the next cook has the new code. */

TEST_F(shader, import_dependency_invalidates, shd_fx) {
    if (!slang_ok())
        SKIP("no Slang compiler for this host");
    fx_dir(f, "dependency");
    de_sleep_ns(AGE_NS);
    char cache[96];
    snprintf(cache, sizeof cache, "%s/decache", f->root);
    shd_settings s = settings(NULL, 0, k_perms_tint, 2, k_t_spirv, 1);
    de_cooker *c = make_cooker(f, DE_COOK_PLATFORM_LINUX, cache);
    de_cook_result a, b, x;
    de_cooker_cache_stats cs;
    ASSERT_EQ(cook(f, c, "shaders/compute.slang", &s, &a), DE_OK);
    de_cooker_get_cache_stats(c, &cs);
    EXPECT_GE(cs.stores, 1u);
    EXPECT_EQ(cs.hits, 0u);
    ASSERT_EQ(cook(f, c, "shaders/compute.slang", &s, &b), DE_OK);
    de_cooker_get_cache_stats(c, &cs);
    EXPECT_EQ(cs.hits, 1u);
    ASSERT_EQ(a.size, b.size);
    EXPECT_EQ(memcmp(a.data, b.data, (size_t)a.size), 0);

    char path[128];
    snprintf(path, sizeof path, "%s/shaders/tint.slang", f->root);
    put_text(path, "module tint;\n"
                   "public float4 de_tint(float4 c) { return c.bgra * 3.0; }\n");
    ASSERT_EQ(cook(f, c, "shaders/compute.slang", &s, &x), DE_OK);
    de_cooker_get_cache_stats(c, &cs);
    EXPECT_EQ(cs.hits, 1u); /* the edit made it a miss */
    EXPECT_EQ(cs.misses, 2u);
    de__shader_view va, vx;
    ASSERT_EQ(de__shader_view_init(a.data, a.size, &va), DE_OK);
    ASSERT_EQ(de__shader_view_init(x.data, x.size, &vx), DE_OK);
    de__shader_record ra0, ra1, rx0, rx1;
    ASSERT_TRUE(find_rec(&va, DE__SHADER_TARGET_SPIRV, 0, "cs_main", &ra0) != NULL);
    ASSERT_TRUE(find_rec(&va, DE__SHADER_TARGET_SPIRV, 1, "cs_main", &ra1) != NULL);
    ASSERT_TRUE(find_rec(&vx, DE__SHADER_TARGET_SPIRV, 0, "cs_main", &rx0) != NULL);
    ASSERT_TRUE(find_rec(&vx, DE__SHADER_TARGET_SPIRV, 1, "cs_main", &rx1) != NULL);
    /* Without USE_TINT the import is not called: the same code. With it: new code. */
    EXPECT_EQ(ra0.code_size, rx0.code_size);
    EXPECT_EQ(memcmp(va.base + ra0.code_offset, vx.base + rx0.code_offset, ra0.code_size), 0);
    EXPECT_TRUE(ra1.code_size != rx1.code_size ||
                memcmp(va.base + ra1.code_offset, vx.base + rx1.code_offset, ra1.code_size) != 0);
    de_cook_result_free(&a);
    de_cook_result_free(&b);
    de_cook_result_free(&x);
    de_cooker_destroy(c);
}

/* ---- shader.cook_dxil -------------------------------------------------------------------------
 *
 * With DXC on the host (Windows with the Windows SDK on the library path): the compute shader and
 * the vertex/pixel pair cook to DXIL: a DXBC container that DXC's validator signed (a non-zero
 * hash), with the reflection of the other targets. Without DXC it skips: DXIL is then covered by
 * targets_and_errors (the skip path). */

TEST_F(shader, cook_dxil, shd_fx) {
    if (!slang_ok())
        SKIP("no Slang compiler for this host");
    if (!de__slang_target_supported(DE__SLANG_DXIL))
        SKIP("no DXC (dxcompiler) on this host: DXIL is not cooked here");
    fx_dir(f, "cook_dxil");
    de_cooker *c = make_cooker(f, DE_COOK_PLATFORM_WINDOWS, NULL);
    const char *const files[] = {"shaders/compute.slang", "shaders/triangle.slang"};
    shd_settings ss[2] = {settings(k_compute_entries, 1, NULL, 0, k_t_dxil, 1),
                          settings(k_pair_entries, 2, NULL, 0, k_t_dxil, 1)};
    const char *const entries[2][2] = {{"cs_main", NULL}, {"vs_main", "ps_main"}};
    for (u32 k = 0; k < 2; ++k) {
        de_cook_result r;
        ASSERT_EQ(cook(f, c, files[k], &ss[k], &r), DE_OK);
        de__shader_view v;
        ASSERT_EQ(de__shader_view_init(r.data, r.size, &v), DE_OK);
        for (u32 e = 0; e < 2 && entries[k][e] != NULL; ++e) {
            de__shader_record rec;
            ASSERT_TRUE(find_rec(&v, DE__SHADER_TARGET_DXIL, 0, entries[k][e], &rec) != NULL);
            const u8 *code = v.base + rec.code_offset;
            ASSERT_GE(rec.code_size, 32u);
            EXPECT_EQ(memcmp(code, "DXBC", 4), 0);
            u8 any = 0;
            for (u32 i = 4; i < 20; ++i)
                any |= code[i];
            EXPECT_NE(any, 0); /* signed by the validator */
            EXPECT_EQ(rec.push_constant_bytes, k == 0 ? 16u : 32u);
        }
        de_cook_result_free(&r);
    }
    de_cooker_destroy(c);
}

/* ---- shader.targets_and_errors ----------------------------------------------------------------
 *
 * DXIL without DXC on the host is skipped with a warning (a DXIL-only cook then fails: no target
 * left); bad settings and a source that does not compile fail with clear errors. */

TEST_F(shader, targets_and_errors, shd_fx) {
    if (!slang_ok())
        SKIP("no Slang compiler for this host");
    fx_dir(f, "targets");
    de_cooker *c = make_cooker(f, DE_COOK_PLATFORM_WINDOWS, NULL);
    de_cook_result r;
    if (!de__slang_target_supported(DE__SLANG_DXIL)) {
        printf("  (no DXC on this host: DXIL is skipped)\n");
        shd_settings dxil = settings(k_compute_entries, 1, NULL, 0, k_t_dxil, 1);
        EXPECT_EQ(cook(f, c, "shaders/compute.slang", &dxil, &r), DE_ERR_UNSUPPORTED);
        /* The Windows default (dxil + spirv) still cooks, with SPIR-V only. */
        shd_settings def = settings(k_compute_entries, 1, NULL, 0, NULL, 0);
        ASSERT_EQ(cook(f, c, "shaders/compute.slang", &def, &r), DE_OK);
        de__shader_view v;
        ASSERT_EQ(de__shader_view_init(r.data, r.size, &v), DE_OK);
        de__shader_record rec;
        EXPECT_TRUE(find_rec(&v, DE__SHADER_TARGET_SPIRV, 0, "cs_main", &rec) != NULL);
        EXPECT_TRUE(find_rec(&v, DE__SHADER_TARGET_DXIL, 0, "cs_main", &rec) == NULL);
        de_cook_result_free(&r);
    }

    static const char *const bad_target[] = {"glsl"};
    static const char *const bad_stage[] = {"cs_main:geometry"};
    static const char *const missing[] = {"nope:compute"};
    static const char *const bad_perm[] = {"9X=1"};
    const shd_settings bad[] = {
        settings(NULL, 0, NULL, 0, bad_target, 1), settings(bad_stage, 1, NULL, 0, NULL, 0),
        settings(missing, 1, NULL, 0, NULL, 0), settings(NULL, 0, bad_perm, 1, NULL, 0)};
    const char *const expect[] = {"unknown target \"glsl\"", "unknown stage \"geometry\"", "nope",
                                  "\"9X=1\""};
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        de_error_clear();
        EXPECT_NE(cook(f, c, "shaders/compute.slang", &bad[i], &r), DE_OK);
        const de_error *e = de_error_last();
        ASSERT_TRUE(e != NULL);
        if (strstr(e->message, expect[i]) == NULL)
            printf("  unexpected message: %s\n", e->message);
        EXPECT_TRUE(strstr(e->message, expect[i]) != NULL);
    }

    char path[128];
    snprintf(path, sizeof path, "%s/shaders/broken.slang", f->root);
    put_text(path, "[shader(\"compute\")] [numthreads(1,1,1)] void main() { undefined_fn(); }\n");
    shd_settings none = settings(NULL, 0, NULL, 0, NULL, 0);
    de_error_clear();
    EXPECT_EQ(cook(f, c, "shaders/broken.slang", &none, &r), DE_ERR_PARSE);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_TRUE(strstr(e->message, "undefined_fn") != NULL);
    de_cooker_destroy(c);
}

/* ---- shader.format_view -----------------------------------------------------------------------
 *
 * The loader's checks: a cooked blob passes; truncation, a bad magic, another version and an
 * out-of-range record fail. */

TEST_F(shader, format_view, shd_fx) {
    if (!slang_ok())
        SKIP("no Slang compiler for this host");
    fx_dir(f, "format");
    de_cooker *c = make_cooker(f, DE_COOK_PLATFORM_LINUX, NULL);
    shd_settings s = settings(NULL, 0, k_perms_tint, 2, NULL, 0);
    de_cook_result r;
    ASSERT_EQ(cook(f, c, "shaders/compute.slang", &s, &r), DE_OK);
    de__shader_view v;
    ASSERT_EQ(de__shader_view_init(r.data, r.size, &v), DE_OK);
    u8 *copy = (u8 *)DE_ALLOC(&f->alloc, (size_t)r.size, 16, DE_MEM_ASSET);
    ASSERT_TRUE(copy != NULL);

    memcpy(copy, r.data, (size_t)r.size);
    EXPECT_EQ(de__shader_view_init(copy, 31, &v), DE_ERR_PARSE);
    EXPECT_EQ(de__shader_view_init(copy, v.header.strings_offset, &v), DE_ERR_PARSE);
    copy[0] ^= 0xffu;
    EXPECT_EQ(de__shader_view_init(copy, r.size, &v), DE_ERR_PARSE);
    memcpy(copy, r.data, (size_t)r.size);
    copy[4] = 99;
    EXPECT_EQ(de__shader_view_init(copy, r.size, &v), DE_ERR_VERSION_MISMATCH);
    memcpy(copy, r.data, (size_t)r.size);
    /* The first record's code size (record word 5) past the end. */
    size_t rec0 = sizeof(de__shader_header) + sizeof(de__shader_entry) * 1u +
                  sizeof(de__shader_permutation) * 2u;
    copy[rec0 + 20] = 0xff;
    copy[rec0 + 21] = 0xff;
    copy[rec0 + 22] = 0xff;
    EXPECT_EQ(de__shader_view_init(copy, r.size, &v), DE_ERR_PARSE);
    DE_FREE(&f->alloc, copy, (size_t)r.size);
    de_cook_result_free(&r);
    de_cooker_destroy(c);
}

/* ---- shader.dev_on_demand_and_reload ----------------------------------------------------------
 *
 * Dev runtime (SOURCE mode, the in-process cooker): loading a `shader` asset compiles it on
 * demand and stores the cook in `.decache`; de__shader_blob hands out the host backend's code
 * with its reflection. Editing the imported module hot-reloads the asset: the reload callback
 * fires once, and the handle resolves to the new code. */

#if DE_HAS_COOKER && DE__ASSET_RELOAD_ENABLED
static u32 g_reloads;
static de_asset_id g_reload_id;

static void on_reload(de_asset_id id, void *user) {
    (void)user;
    g_reloads++;
    g_reload_id = id;
}

static bool dir_has_files(const char *name, bool is_dir, void *user) {
    (void)name;
    (void)is_dir;
    (*(u32 *)user)++;
    return true;
}
#endif

TEST_F(shader, dev_on_demand_and_reload, shd_fx) {
#if !DE_HAS_COOKER || !DE__ASSET_RELOAD_ENABLED
    SKIP("the in-process cooker and hot reload exist in Debug and Dev builds only");
#else
    if (!slang_ok())
        SKIP("no Slang compiler for this host");
    fx_dir(f, "dev");
    char cache[96], manifest[96], path[128];
    snprintf(cache, sizeof cache, "%s/decache", f->root);
    snprintf(manifest, sizeof manifest, "%s/assets.toml", f->root);
    put_text(manifest, "[assets]\n"
                       "\"shaders/compute\" = { type = \"shader\", source = "
                       "\"shaders/compute.slang\", permutations = [\"\", \"USE_TINT=1\"] }\n");
    de_sleep_ns(AGE_NS); /* old enough for the cook cache to store the cook */

    de_task_system *tasks = NULL;
    de_task_system_desc td = {.worker_count = 2, .background_worker_count = 1};
    ASSERT_EQ(de_task_system_create(&td, &tasks), DE_OK);
    de_asset_system *s = NULL;
    de_asset_system_desc d = {0};
    d.allocator = &f->alloc;
    d.tasks = tasks;
    d.io_threads = 1;
    d.cache_dir = cache;
    d.hot_reload = true;
    d.mode = DE_ASSET_MODE_SOURCE;
    ASSERT_EQ(de_asset_system_create(&d, &s), DE_OK);
    de__asset_reload_set_watching(s, false);
    de__shader_types *st = NULL;
    de__shader_type_desc sd = {.allocator = &f->alloc,
                               .import = de_shader_import,
                               .importer_version = de_shader_importer_version()};
    ASSERT_EQ(de__shader_type_register(s, &sd, &st), DE_OK);
    ASSERT_EQ(de__shader_on_reload(st, on_reload, NULL), DE_OK);
    g_reloads = 0;
    ASSERT_EQ(de_asset_manifest_load(s, manifest), DE_OK);

    const de_asset_id id = de_asset_id_from("shaders/compute");
    de_shader sh = de_asset_shader(s, id);
    ASSERT_TRUE(DE_HANDLE_VALID(sh));
    de_gpu_shader_blob blob;
    EXPECT_EQ(de__shader_blob(s, sh, DE_GPU_DEFAULT, 0, "cs_main", &blob, NULL), DE_ERR_BUSY);
    de_load_handle h = de_asset_load(s, id, 0);
    ASSERT_EQ((int)de_asset_wait(s, h, WAIT_NS), (int)DE_LOAD_READY);

    /* The cook went into the cache. */
    u32 cached = 0;
    EXPECT_EQ(de_dir_list(cache, dir_has_files, &cached), DE_OK);
    EXPECT_GT(cached, 0u);

    de__shader_reflection refl;
    ASSERT_EQ(de__shader_blob(s, sh, DE_GPU_DEFAULT, 1, "cs_main", &blob, &refl), DE_OK);
    EXPECT_GT(blob.size, 0u);
    EXPECT_STREQ(blob.entry, "cs_main");
    EXPECT_EQ(refl.stage, (u32)DE__SHADER_STAGE_COMPUTE);
    EXPECT_EQ(refl.threadgroup[0], 8u);
    EXPECT_EQ(refl.push_constant_bytes, 16u);
    EXPECT_EQ(de__shader_permutation_count(s, sh), 2u);
    EXPECT_STREQ(de__shader_permutation_defines(s, sh, 1), "USE_TINT=1");
    EXPECT_EQ(de__shader_blob(s, sh, DE_GPU_VULKAN, 0, "nope", &blob, NULL), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de__shader_blob(s, sh, DE_GPU_VULKAN, 2, "cs_main", &blob, NULL), DE_ERR_NOT_FOUND);
    ASSERT_EQ(de__shader_blob(s, sh, DE_GPU_VULKAN, 1, "cs_main", &blob, NULL), DE_OK);
    u64 old_size = blob.size;
    u8 old_code[4096];
    ASSERT_LE(old_size, (u64)sizeof old_code);
    memcpy(old_code, blob.code, (size_t)old_size);

    /* Edit the import: the shader reloads, the callback fires once, the code is new. */
    snprintf(path, sizeof path, "%s/shaders/tint.slang", f->root);
    put_text(path, "module tint;\n"
                   "public float4 de_tint(float4 c) { return c.bgra * 3.0; }\n");
    de__asset_reload_on_change(s, path, DE_WATCH_MODIFIED, false);
    const u64 t0 = de_time_now_ns();
    while (g_reloads == 0 && de_time_now_ns() - t0 < WAIT_NS) {
        de_asset_system_update(s, NULL);
        de_sleep_ns(2000000u);
    }
    EXPECT_EQ(g_reloads, 1u);
    EXPECT_EQ(g_reload_id.hash, id.hash);
    ASSERT_EQ(de__shader_blob(s, sh, DE_GPU_VULKAN, 1, "cs_main", &blob, NULL), DE_OK);
    EXPECT_TRUE(blob.size != old_size || memcmp(blob.code, old_code, (size_t)old_size) != 0);
    for (u32 i = 0; i < 4; ++i)
        de_asset_system_update(s, NULL);
    EXPECT_EQ(g_reloads, 1u);

    de_asset_release(s, h);
    de__shader_on_reload_remove(st, on_reload, NULL);
    de_asset_system_destroy(s);
    de__shader_types_destroy(st);
    de_task_system_destroy(tasks);
#endif
}
