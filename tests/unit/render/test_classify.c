/*
 * test_classify.c: material classification of the visibility buffer (DENG-237): per-model tile
 * lists, the background list, the per-tile model masks and the indirect arguments.
 * Executable test_render_classify; CTest names render_classify.<suite>.<test>.
 *
 * Each GPU test drives the classify stage (src/render/passes/vis/classify.c) on its own: it makes
 * synthetic GPU-scene tables (instances, meshes with meshlet records, materials with models) and a
 * visible-meshlet list in UPLOAD buffers, writes a synthetic vis-ID image into an R32_UINT texture,
 * fills a de__vis_context with them and calls de__vis_classify; a test pass reads back the lists,
 * the arguments, the masks and the stage's copy of the vis IDs. The CPU reference maps every pixel
 * to its model with the shader's rule (de__vis_classify_model) and bins the tiles; lists compare
 * as sets (their order is not deterministic), and a tile may appear at most once per list.
 *
 * GPU tests run per backend (_metal, _vulkan, _d3d12; SKIP without the device or without the
 * engine shaders). CI: Vulkan on lavapipe and D3D12 on the vendored WARP in the golden jobs.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include "gpu/gpu_backend.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/graph/rg_internal.h"
#include "render/passes/vis/vis_stages.h"
#include "render/scene/gpu_scene.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif
#include "render_d3d12_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS (10ull * 1000000000ull)

static void *zalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    ASSERT_TRUE(p != NULL);
    return p;
}

typedef struct rng {
    u64 s;
} rng;

static u32 rnd(rng *g) {
    g->s ^= g->s << 13;
    g->s ^= g->s >> 7;
    g->s ^= g->s << 17;
    return (u32)(g->s >> 11);
}

/* ================================================================================================
 * The synthetic scene
 * ============================================================================================== */

#define MESHES        4u
#define MESH_MESHLETS 16u
#define MAX_MATERIALS 64u
#define MAX_INSTANCES 64u
#define MAX_VISIBLE   1024u

typedef struct scene {
    de__gs_instance inst[MAX_INSTANCES];
    u32 inst_count;
    de__gs_mesh mesh[MESHES];
    u8 records[MESHES][MESH_MESHLETS * DE_COOKED_MESHLET_SIZE];
    de__gs_material mat[MAX_MATERIALS];
    u32 mat_count;
    de__vis_meshlet visible[MAX_VISIBLE];
    u32 visible_count;
} scene;

/* Material `i`: model `model`, READY unless `ready` is false. */
static void scene_material(scene *sc, u32 i, u32 model, bool ready) {
    memset(&sc->mat[i], 0, sizeof sc->mat[i]);
    sc->mat[i].model = model;
    sc->mat[i].flags = ready ? DE__GS_MATERIAL_READY : 0u;
    if (i >= sc->mat_count)
        sc->mat_count = i + 1u;
}

/* Mesh `m` with MESH_MESHLETS meshlets whose submeshes come from `sub` (the last mesh has no
 * meshlet buffer when `no_records`). */
static void scene_mesh(scene *sc, u32 m, const u32 *sub, bool no_records) {
    memset(&sc->mesh[m], 0, sizeof sc->mesh[m]);
    sc->mesh[m].meshlet_count = MESH_MESHLETS;
    sc->mesh[m].flags = DE__GS_MESH_READY | DE__GS_MESH_MESHLETS;
    sc->mesh[m].meshlets = no_records ? 0u : 1u; /* replaced by the bindless index at upload */
    for (u32 k = 0; k < MESH_MESHLETS; ++k) {
        const u32 w = 3u | (1u << 8) | (sub[k] << 16);
        memcpy(&sc->records[m][(size_t)k * DE_COOKED_MESHLET_SIZE + 8u], &w, 4);
    }
}

/* Instance `i`: mesh `m`, material slot per submesh. */
static void scene_instance(scene *sc, u32 i, u32 m, const u32 *slots) {
    memset(&sc->inst[i], 0, sizeof sc->inst[i]);
    sc->inst[i].mesh = m;
    sc->inst[i].flags = DE__GS_INSTANCE_LIVE;
    for (u32 k = 0; k < DE__GS_INSTANCE_MATERIALS; ++k)
        sc->inst[i].materials[k / 4u].v[k % 4u] = slots[k];
    if (i >= sc->inst_count)
        sc->inst_count = i + 1u;
}

/* The model classification gives visible-list entry `e` (the shader's rule). */
static u32 entry_model(const scene *sc, u32 e) {
    if (e >= sc->visible_count)
        return DE__GS_MATERIAL_MODEL_STANDARD;
    const de__vis_meshlet *vm = &sc->visible[e];
    const de__gs_instance *in = &sc->inst[vm->instance];
    const de__gs_mesh *m = &sc->mesh[in->mesh];
    if (m->meshlets == 0 || vm->meshlet >= m->meshlet_count)
        return DE__GS_MATERIAL_MODEL_STANDARD;
    u32 w;
    memcpy(&w, &sc->records[in->mesh][(size_t)vm->meshlet * DE_COOKED_MESHLET_SIZE + 8u], 4);
    const u32 submesh = w >> 16;
    const u32 slot = submesh < DE__GS_INSTANCE_MATERIALS
                         ? in->materials[submesh / 4u].v[submesh % 4u]
                         : DE__GS_NONE;
    return de__vis_classify_model(sc->mat, sc->mat_count, slot);
}

/* A random scene: materials of every model (some not READY), meshes with random submeshes (one
 * without meshlet records), instances with random slots (some past the table, some NONE), and a
 * visible list (some meshlets past the mesh's count). */
static void scene_random(scene *sc, rng *g) {
    memset(sc, 0, sizeof *sc);
    for (u32 i = 0; i < 48u; ++i)
        scene_material(sc, i, rnd(g) % DE__GS_MATERIAL_MODELS, rnd(g) % 8u != 0u);
    for (u32 m = 0; m < MESHES; ++m) {
        u32 sub[MESH_MESHLETS];
        for (u32 k = 0; k < MESH_MESHLETS; ++k)
            sub[k] = rnd(g) % 10u; /* 8, 9: past the instance's slots */
        scene_mesh(sc, m, sub, m == MESHES - 1u);
    }
    for (u32 i = 0; i < 40u; ++i) {
        u32 slots[DE__GS_INSTANCE_MATERIALS];
        for (u32 k = 0; k < DE__GS_INSTANCE_MATERIALS; ++k) {
            const u32 r = rnd(g) % 20u;
            slots[k] = r == 0u ? DE__GS_NONE : r == 1u ? 60u : rnd(g) % 48u;
        }
        scene_instance(sc, i, rnd(g) % MESHES, slots);
    }
    sc->visible_count = 700u;
    for (u32 e = 0; e < sc->visible_count; ++e) {
        sc->visible[e].instance = rnd(g) % sc->inst_count;
        sc->visible[e].meshlet = rnd(g) % (MESH_MESHLETS + 2u);
    }
}

/* ================================================================================================
 * Fixture
 * ============================================================================================== */

typedef struct ccx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_gpu_device *dev;
    bool real_gpu;
    de_rg *g;
    de__vis_shared *vis;
    de_gpu_fence fence;
    u64 frames;
    rng r;
    /* Synthetic inputs. */
    scene sc;
    u32 width, height;
    u32 *ids; /* width x height */
    /* GPU copies (made per run). */
    de_gpu_buffer buf_inst, buf_mesh, buf_mat, buf_visible, buf_records[MESHES], buf_upload, rb;
    de_gpu_texture tex_ids;
    u32 ids_access;
    u64 rb_size;
    /* Results. */
    u32 tile, tiles_x, tiles_y, tile_count;
    u32 *lists, *masks, *ids_copy;
    de__vis_dispatch_args args[DE__VIS_CLASSIFY_LISTS];
    f32 classify_ms; /* GPU time of the classify passes (0 when not timed) */
    bool print_passes;
    u32 alpha_first; /* > 0: visible entries from here are the alpha bucket's region */
} ccx;

static void ccx_setup(ccx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "classify_test"}, &f->arena), DE_OK);
    de_error_clear();
    f->r.s = 0x2545F4914F6CDD1Dull;
}

static u32 validation_errors(de_gpu_device *d) {
    switch (de_gpu_get_caps(d)->backend) {
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN: {
        u32 warnings = 0;
        return de__gpu_vk_validation_messages(d, &warnings);
    }
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return de__d3d12_debug_error_count(d);
#endif
    default:
        return 0;
    }
}

static void destroy_buf(ccx *f, de_gpu_buffer *b) {
    if (DE_HANDLE_VALID(*b))
        de_gpu_buffer_destroy(f->dev, *b);
    *b = (de_gpu_buffer){0};
}

static void free_inputs(ccx *f) {
    if (f->dev == NULL)
        return;
    de_gpu_wait_idle(f->dev);
    destroy_buf(f, &f->buf_inst);
    destroy_buf(f, &f->buf_mesh);
    destroy_buf(f, &f->buf_mat);
    destroy_buf(f, &f->buf_visible);
    destroy_buf(f, &f->buf_upload);
    for (u32 m = 0; m < MESHES; ++m)
        destroy_buf(f, &f->buf_records[m]);
    if (DE_HANDLE_VALID(f->tex_ids))
        de_gpu_texture_destroy(f->dev, f->tex_ids);
    f->tex_ids = (de_gpu_texture){0};
}

static void ccx_teardown(ccx *f) {
    if (f->g) {
        de__rg_reset(f->g);
        de__rg_destroy(f->g);
    }
    free_inputs(f);
    de__vis_shared_destroy(f->vis);
    if (f->dev) {
        de_gpu_wait_idle(f->dev);
        if (f->real_gpu)
            EXPECT_EQ(validation_errors(f->dev), 0u);
        destroy_buf(f, &f->rb);
        if (DE_HANDLE_VALID(f->fence))
            de_gpu_fence_destroy(f->dev, f->fence);
        de_gpu_wait_idle(f->dev);
        de_gpu_device_destroy(f->dev);
    }
    free(f->ids);
    free(f->lists);
    free(f->masks);
    free(f->ids_copy);
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
    const de_cvar cv = de_cvar_find("r.classify_tile");
    if (DE_HANDLE_VALID(cv))
        (void)de_cvar_set(cv, "8");
}

static bool backend_built(de_gpu_backend b) {
    switch (b) {
#if DE_HAS_GPU_METAL
    case DE_GPU_METAL:
        return true;
#endif
#if DE_HAS_GPU_VULKAN
    case DE_GPU_VULKAN:
        return true;
#endif
#if DE_HAS_GPU_D3D12
    case DE_GPU_D3D12:
        return true;
#endif
    default:
        return false;
    }
}

/* Opens a device (SKIPs when it is not there), a graph and the vis frame's stages. */
static void ccx_open(ccx *f, de_gpu_backend backend) {
    static char why[700];
    if (!backend_built(backend)) {
        snprintf(why, sizeof why, "the %s backend is not in this build",
                 de_gpu_backend_name(backend));
        SKIP(why);
    }
    f->real_gpu = true;
#if defined(__APPLE__)
    if (backend == DE_GPU_METAL)
        setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
    de_gpu_device_desc desc = {.backend = backend,
                               .debug_layer = true,
                               .gpu_validation = backend == DE_GPU_VULKAN,
                               .pipeline_cache_dir = ""};
    de_result r = de_gpu_device_create(&desc, &f->dev);
    if (r == DE_ERR_UNSUPPORTED && backend == DE_GPU_VULKAN) {
        desc.adapter = 1;
        r = de_gpu_device_create(&desc, &f->dev);
    }
    if (r != DE_OK) {
        f->dev = NULL;
        snprintf(why, sizeof why, "no usable %s device: %.400s", de_gpu_backend_name(backend),
                 de_error_last() ? de_error_last()->message : "?");
        SKIP(why);
    }
    const de_gpu_caps *caps = de_gpu_get_caps(f->dev);
    printf("device: \"%s\" (%s)\n", caps->device_name, de_gpu_backend_name(backend));
    render_d3d12_report(f->dev);
    if (!caps->base_tier_ok) {
        snprintf(why, sizeof why, "below the Base tier: %.400s", caps->base_tier_reason);
        SKIP(why);
    }
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "cl"},
                            &f->g),
              DE_OK);
    f->fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(f->fence));
    ASSERT_EQ(de__vis_shared_create(
                  &(de__vis_shared_desc){.device = f->dev, .allocator = &f->alloc}, &f->vis),
              DE_OK);
    if (!f->vis->enabled) {
        snprintf(why, sizeof why, "the vis frame is off: %.600s", f->vis->why);
        SKIP(why);
    }
}

/* A new vis-ID image of `w` x `h`, all background. */
static void ccx_image(ccx *f, u32 w, u32 h) {
    free(f->ids);
    f->width = w;
    f->height = h;
    f->ids = (u32 *)zalloc((size_t)w * h * 4u);
    for (size_t i = 0; i < (size_t)w * h; ++i)
        f->ids[i] = DE__VIS_ID_EMPTY;
}

/* Fills [x0, x1) x [y0, y1) with triangles of visible entry `e`. */
static void ccx_rect(ccx *f, u32 x0, u32 y0, u32 x1, u32 y1, u32 e) {
    for (u32 y = y0; y < y1 && y < f->height; ++y)
        for (u32 x = x0; x < x1 && x < f->width; ++x)
            f->ids[(size_t)y * f->width + x] = de__vis_id_pack(e, (x * 7u + y * 3u) % 124u);
}

/* Random rectangles of random entries over the background. */
static void ccx_random_image(ccx *f, u32 w, u32 h, u32 rects) {
    ccx_image(f, w, h);
    for (u32 i = 0; i < rects; ++i) {
        const u32 x0 = rnd(&f->r) % w, y0 = rnd(&f->r) % h;
        const u32 rw = 1u + rnd(&f->r) % 120u, rh = 1u + rnd(&f->r) % 90u;
        ccx_rect(f, x0, y0, x0 + rw, y0 + rh, rnd(&f->r) % f->sc.visible_count);
    }
}

static de_gpu_buffer upload(ccx *f, const void *data, u64 bytes, u32 stride, const char *name) {
    const de_gpu_buffer b = de_gpu_buffer_create(
        f->dev, &(de_gpu_buffer_desc){.size = bytes,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                      .memory = DE_MEM_UPLOAD,
                                      .stride = stride,
                                      .name = name});
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    void *m = de_gpu_buffer_map(f->dev, b);
    ASSERT_TRUE(m != NULL);
    memcpy(m, data, (size_t)bytes);
    return b;
}

/* The scene's tables and the vis-ID image to the GPU (new objects each call). */
static void ccx_upload(ccx *f) {
    free_inputs(f);
    scene *sc = &f->sc;
    de__gs_mesh meshes[MESHES];
    memcpy(meshes, sc->mesh, sizeof meshes);
    for (u32 m = 0; m < MESHES; ++m) {
        f->buf_records[m] = upload(f, sc->records[m], sizeof sc->records[m], 0, "test records");
        if (meshes[m].meshlets != 0)
            meshes[m].meshlets = de_gpu_buffer_bindless(f->dev, f->buf_records[m]);
    }
    f->buf_inst = upload(f, sc->inst, sizeof(de__gs_instance) * sc->inst_count,
                         (u32)sizeof(de__gs_instance), "test instances");
    f->buf_mesh = upload(f, meshes, sizeof meshes, (u32)sizeof(de__gs_mesh), "test meshes");
    f->buf_mat = upload(f, sc->mat, sizeof(de__gs_material) * sc->mat_count,
                        (u32)sizeof(de__gs_material), "test materials");
    f->buf_visible = upload(f, sc->visible, sizeof(de__vis_meshlet) * sc->visible_count,
                            (u32)sizeof(de__vis_meshlet), "test visible");
    const u32 pitch = (f->width * 4u + 255u) & ~255u;
    u8 *rows = (u8 *)zalloc((size_t)pitch * f->height);
    for (u32 y = 0; y < f->height; ++y)
        memcpy(rows + (size_t)y * pitch, f->ids + (size_t)y * f->width, (size_t)f->width * 4u);
    f->buf_upload = upload(f, rows, (u64)pitch * f->height, 0, "test ids upload");
    free(rows);
    f->tex_ids = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_R32_UINT,
                                       .width = f->width,
                                       .height = f->height,
                                       .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_DST |
                                                DE_USAGE_COPY_SRC,
                                       .name = "test vis ids"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->tex_ids));
    f->ids_access = DE_ACCESS_NONE;
}

/* ---------- One run ---------- */

typedef struct up_pass {
    ccx *f;
    de_rg_resource ids;
    u32 pitch;
} up_pass;

static void up_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const up_pass *p = (const up_pass *)user;
    de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, p->ids), 0, 0, p->f->buf_upload, 0,
                                      p->pitch);
}

typedef struct rb_pass {
    ccx *f;
    de_rg_resource lists, args, masks, ids_buffer;
    u64 lists_off, masks_off, ids_off, lists_bytes, masks_bytes, ids_bytes;
} rb_pass;

static void rb_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    const de_gpu_buffer rb = p->f->rb;
    de_gpu_cmd_copy_buffer(cmd, rb, 0, de_rg_buffer(pass, p->args), 0,
                           DE__VIS_CLASSIFY_LISTS * 16u);
    de_gpu_cmd_copy_buffer(cmd, rb, p->lists_off, de_rg_buffer(pass, p->lists), 0, p->lists_bytes);
    de_gpu_cmd_copy_buffer(cmd, rb, p->masks_off, de_rg_buffer(pass, p->masks), 0, p->masks_bytes);
    de_gpu_cmd_copy_buffer(cmd, rb, p->ids_off, de_rg_buffer(pass, p->ids_buffer), 0, p->ids_bytes);
}

static u64 align_up(u64 v, u64 a) {
    return (v + a - 1u) / a * a;
}

/* Runs the classify stage on the uploaded inputs and reads its outputs back. */
static bool ccx_run(ccx *f) {
    const u64 n = f->frames++;
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    if (de__rg_begin(f->g, f->arena) != DE_OK)
        return false;
    de_rg *g = f->g;
    up_pass *up = (up_pass *)de_arena_push_zero(f->arena, sizeof *up, 16);
    ASSERT_TRUE(up != NULL);
    up->f = f;
    up->pitch = (f->width * 4u + 255u) & ~255u;
    up->ids = de_rg_import_texture(g, f->tex_ids, f->ids_access);
    de_rg_pass *p = de_rg_add_pass(g, "test.ids_upload", DE_QUEUE_GRAPHICS, up_execute, up);
    de_rg_write(p, up->ids, DE_ACCESS_COPY_DST);

    de__vis_context *vc = (de__vis_context *)de_arena_push_zero(f->arena, sizeof *vc, 16);
    ASSERT_TRUE(vc != NULL);
    vc->shared = f->vis;
    vc->graph = g;
    vc->arena = f->arena;
    vc->view = (de_view){.id = 1};
    vc->frame_index = n;
    vc->frames_completed = n;
    vc->width = f->width;
    vc->height = f->height;
    vc->vis_ids = up->ids;
    vc->visible_meshlets = de_rg_import_buffer(g, f->buf_visible, DE_ACCESS_SHADER_READ);
    /* The list's opaque region, then the alpha-tested bucket's (DENG-236): one index space. */
    vc->visible_capacity = f->alpha_first != 0 ? f->alpha_first : f->sc.visible_count;
    vc->visible_alpha_capacity = f->sc.visible_count - vc->visible_capacity;
    vc->scene_instances = de_rg_import_buffer(g, f->buf_inst, DE_ACCESS_SHADER_READ);
    vc->scene_meshes = de_rg_import_buffer(g, f->buf_mesh, DE_ACCESS_SHADER_READ);
    vc->scene_materials = de_rg_import_buffer(g, f->buf_mat, DE_ACCESS_SHADER_READ);
    vc->scene_material_count = f->sc.mat_count;
    de__vis_classify(g, vc);
    ASSERT_TRUE(DE_HANDLE_VALID(vc->classify_tiles));
    f->tile = vc->classify_tile;
    f->tiles_x = vc->classify_tiles_x;
    f->tiles_y = vc->classify_tiles_y;
    f->tile_count = vc->classify_tile_count;
    EXPECT_EQ(vc->vis_ids_pitch, up->pitch);

    rb_pass *rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
    ASSERT_TRUE(rp != NULL);
    rp->f = f;
    rp->lists = vc->classify_tiles;
    rp->args = vc->classify_args;
    rp->masks = vc->classify_masks;
    rp->ids_buffer = vc->vis_ids_buffer;
    rp->lists_bytes = (u64)DE__VIS_CLASSIFY_LISTS * f->tile_count * 4u;
    rp->masks_bytes = (u64)f->tile_count * 4u;
    rp->ids_bytes = (u64)up->pitch * f->height;
    rp->lists_off = 1024u;
    rp->masks_off = align_up(rp->lists_off + rp->lists_bytes, 256u);
    rp->ids_off = align_up(rp->masks_off + rp->masks_bytes, 256u);
    const u64 rb_bytes = rp->ids_off + rp->ids_bytes;
    if (rb_bytes > f->rb_size) {
        destroy_buf(f, &f->rb);
        f->rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = rb_bytes,
                                                                   .usage = DE_USAGE_COPY_DST,
                                                                   .memory = DE_MEM_READBACK,
                                                                   .name = "classify readback"});
        ASSERT_TRUE(DE_HANDLE_VALID(f->rb));
        f->rb_size = rb_bytes;
    }
    p = de_rg_add_pass(g, "test.readback", DE_QUEUE_GRAPHICS, rb_execute, rp);
    de_rg_read(p, rp->lists, DE_ACCESS_COPY_SRC);
    de_rg_read(p, rp->args, DE_ACCESS_COPY_SRC);
    de_rg_read(p, rp->masks, DE_ACCESS_COPY_SRC);
    de_rg_read(p, rp->ids_buffer, DE_ACCESS_COPY_SRC);
    de_rg_write(p, de_rg_import_buffer(g, f->rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);

    de_result r = de__rg_finalize(g);
    if (r == DE_OK)
        r = de__rg_compile(g);
    if (r == DE_OK)
        r = de__rg_execute(g, &(de__rg_execute_desc){
                                  .frame_index = n, .frame_fence = f->fence, .frame_value = n + 1});
    if (r != DE_OK) {
        fprintf(stderr, "run %llu: %s\n", (unsigned long long)n,
                de_error_last() ? de_error_last()->message : "?");
        return false;
    }
    f->ids_access = DE_ACCESS_COPY_SRC; /* the stage's copy was the last access */
    if (de_gpu_fence_wait(f->dev, f->fence, n + 1, WAIT_NS) != DE_OK)
        return false;
    /* The classify passes' GPU time, from the newest frame the graph timed. */
    const de__rg_timings *tm = de__rg_timings_get(g);
    f->classify_ms = 0.0f;
    if (tm != NULL && tm->valid)
        for (u32 i = 0; i < tm->pass_count; ++i)
            if (strncmp(tm->pass_names[i], "vis.classify", 12) == 0) {
                f->classify_ms += tm->pass_gpu_ms[i];
                if (f->print_passes)
                    printf("  %s: %.3f ms\n", tm->pass_names[i], (double)tm->pass_gpu_ms[i]);
            }
    const u8 *m = (const u8 *)de_gpu_buffer_map(f->dev, f->rb);
    memcpy(f->args, m, sizeof f->args);
    free(f->lists);
    free(f->masks);
    free(f->ids_copy);
    f->lists = (u32 *)zalloc((size_t)rp->lists_bytes);
    f->masks = (u32 *)zalloc((size_t)rp->masks_bytes);
    f->ids_copy = (u32 *)zalloc((size_t)f->width * f->height * 4u);
    memcpy(f->lists, m + rp->lists_off, (size_t)rp->lists_bytes);
    memcpy(f->masks, m + rp->masks_off, (size_t)rp->masks_bytes);
    for (u32 y = 0; y < f->height; ++y)
        memcpy(f->ids_copy + (size_t)y * f->width, m + rp->ids_off + (u64)y * up->pitch,
               (size_t)f->width * 4u);
    return true;
}

/* ================================================================================================
 * The CPU reference and the comparison
 * ============================================================================================== */

typedef struct ref {
    u32 *mask;      /* per tile: models */
    u8 *background; /* per tile: a pixel holds no ID */
} ref;

static ref ref_build(const ccx *f) {
    ref r;
    r.mask = (u32 *)zalloc((size_t)f->tile_count * 4u);
    r.background = (u8 *)zalloc(f->tile_count);
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x) {
            const u32 t = (y / f->tile) * f->tiles_x + x / f->tile;
            const u32 id = f->ids[(size_t)y * f->width + x];
            if (id == DE__VIS_ID_EMPTY)
                r.background[t] = 1;
            else
                r.mask[t] |= 1u << entry_model(&f->sc, de__vis_id_meshlet(id));
        }
    return r;
}

static void ref_free(ref *r) {
    free(r->mask);
    free(r->background);
}

/* How many times tile `t` is in list `k` (the GPU's). */
static u32 list_hits(const ccx *f, u32 k, u32 tile_x, u32 tile_y) {
    const u32 want = tile_x | (tile_y << DE__VIS_CLASSIFY_TILE_BITS);
    u32 n = 0;
    for (u32 i = 0; i < f->args[k].count && i < f->tile_count; ++i)
        n += f->lists[(size_t)k * f->tile_count + i] == want;
    return n;
}

/* Every list equals the reference as a set (each tile at most once), every mask equals it, the
 * arguments equal the lengths, and the stage's copy of the IDs equals the image. */
static void expect_matches(const ccx *f, const char *what) {
    ref r = ref_build(f);
    u32 entries = 0, bad = 0;
    u32 *seen = (u32 *)zalloc((size_t)f->tile_count * 4u);
    for (u32 k = 0; k < DE__VIS_CLASSIFY_LISTS; ++k) {
        u32 want = 0;
        for (u32 t = 0; t < f->tile_count; ++t)
            want += k == DE__VIS_CLASSIFY_BACKGROUND ? r.background[t] : (r.mask[t] >> k) & 1u;
        const de__vis_dispatch_args a = f->args[k];
        const de__vis_dispatch_args e = de__vis_dispatch_args_for(want);
        EXPECT_EQ(a.count, want);
        EXPECT_EQ(a.x, e.x);
        EXPECT_EQ(a.y, e.y);
        EXPECT_EQ(a.z, 1u);
        memset(seen, 0, (size_t)f->tile_count * 4u);
        for (u32 i = 0; i < a.count && i < f->tile_count; ++i) {
            const u32 v = f->lists[(size_t)k * f->tile_count + i];
            const u32 tx = v & 0xFFFFu, ty = v >> DE__VIS_CLASSIFY_TILE_BITS;
            if (tx >= f->tiles_x || ty >= f->tiles_y) {
                ++bad;
                continue;
            }
            const u32 t = ty * f->tiles_x + tx;
            const bool in = k == DE__VIS_CLASSIFY_BACKGROUND ? r.background[t] != 0
                                                             : ((r.mask[t] >> k) & 1u) != 0;
            bad += !in || seen[t] != 0;
            seen[t] = 1;
        }
        entries += a.count;
    }
    u32 mask_bad = 0;
    for (u32 t = 0; t < f->tile_count; ++t)
        mask_bad += f->masks[t] != r.mask[t];
    u32 copy_bad = 0;
    for (size_t i = 0; i < (size_t)f->width * f->height; ++i)
        copy_bad += f->ids_copy[i] != f->ids[i];
    printf("%s: %u x %u, %u tiles of %u px, %u list entries, %u bad, %u masks differ, %u IDs "
           "differ in the copy\n",
           what, f->width, f->height, f->tile_count, f->tile, entries, bad, mask_bad, copy_bad);
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(mask_bad, 0u);
    EXPECT_EQ(copy_bad, 0u);
    free(seen);
    ref_free(&r);
}

static bool cvar_set(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (!DE_HANDLE_VALID(cv))
        return false;
    EXPECT_EQ(de_cvar_set(cv, value), DE_OK);
    return true;
}

/* ================================================================================================
 * GPU tests
 * ============================================================================================== */

/* classify_tiles_match_cpu: random scenes and images (sizes that are and are not multiples of the
 * tile; a third of the entries in the alpha bucket's region of the list) bin like the CPU
 * reference. */
static void tiles_match_cpu(ccx *f, de_gpu_backend b) {
    ccx_open(f, b);
    static const u32 sizes[3][2] = {{640, 360}, {1000, 563}, {97, 61}};
    for (u32 i = 0; i < 3; ++i) {
        scene_random(&f->sc, &f->r);
        ccx_random_image(f, sizes[i][0], sizes[i][1], 60u);
        /* The last third of the entries is the alpha bucket's region (DENG-236): its IDs
         * classify like the opaque ones. */
        f->alpha_first = f->sc.visible_count * 2u / 3u;
        ccx_upload(f);
        ASSERT_TRUE(ccx_run(f));
        EXPECT_EQ(f->tile, 8u);
        expect_matches(f, "random");
    }
}

/* classify_mixed_tile_both_lists: a tile of two models is in both lists exactly once; a tile of one
 * model and background in that model's list and the background list. */
static void mixed_tile(ccx *f, de_gpu_backend b) {
    ccx_open(f, b);
    scene *sc = &f->sc;
    memset(sc, 0, sizeof *sc);
    scene_material(sc, 0, 5u, true);
    scene_material(sc, 1, 17u, true);
    const u32 sub0[MESH_MESHLETS] = {0};
    for (u32 m = 0; m < MESHES; ++m)
        scene_mesh(sc, m, sub0, false);
    u32 slots_a[DE__GS_INSTANCE_MATERIALS], slots_b[DE__GS_INSTANCE_MATERIALS];
    for (u32 k = 0; k < DE__GS_INSTANCE_MATERIALS; ++k) {
        slots_a[k] = 0;
        slots_b[k] = 1;
    }
    scene_instance(sc, 0, 0, slots_a);
    scene_instance(sc, 1, 1, slots_b);
    sc->visible_count = 2;
    sc->visible[0] = (de__vis_meshlet){.instance = 0, .meshlet = 3};
    sc->visible[1] = (de__vis_meshlet){.instance = 1, .meshlet = 7};
    ccx_image(f, 64, 16);
    ccx_rect(f, 0, 0, 4, 8, 0);   /* tile (0, 0): model 5 ... */
    ccx_rect(f, 4, 0, 8, 8, 1);   /* ... and model 17 */
    ccx_rect(f, 8, 0, 12, 8, 0);  /* tile (1, 0): model 5 and background */
    ccx_rect(f, 16, 0, 24, 8, 1); /* tile (2, 0): model 17 only */
    ccx_upload(f);
    ASSERT_TRUE(ccx_run(f));
    expect_matches(f, "mixed");
    EXPECT_EQ(list_hits(f, 5, 0, 0), 1u);
    EXPECT_EQ(list_hits(f, 17, 0, 0), 1u);
    EXPECT_EQ(list_hits(f, DE__VIS_CLASSIFY_BACKGROUND, 0, 0), 0u);
    EXPECT_EQ(list_hits(f, 5, 1, 0), 1u);
    EXPECT_EQ(list_hits(f, DE__VIS_CLASSIFY_BACKGROUND, 1, 0), 1u);
    EXPECT_EQ(list_hits(f, 17, 1, 0), 0u);
    EXPECT_EQ(list_hits(f, 17, 2, 0), 1u);
    EXPECT_EQ(list_hits(f, DE__VIS_CLASSIFY_BACKGROUND, 2, 0), 0u);
    EXPECT_EQ(f->masks[0], (1u << 5) | (1u << 17));
    EXPECT_EQ(f->masks[1], 1u << 5);
    EXPECT_EQ(f->masks[2], 1u << 17);
    EXPECT_EQ(f->args[5].count, 2u);
    EXPECT_EQ(f->args[17].count, 2u);
}

/* classify_background_list: background-only tiles are in the background list and no other; an
 * all-background image has every tile there and no model list. */
static void background_list(ccx *f, de_gpu_backend b) {
    ccx_open(f, b);
    scene_random(&f->sc, &f->r);
    ccx_image(f, 200, 120);
    ccx_upload(f);
    ASSERT_TRUE(ccx_run(f));
    expect_matches(f, "all background");
    EXPECT_EQ(f->args[DE__VIS_CLASSIFY_BACKGROUND].count, f->tile_count);
    for (u32 k = 0; k < DE__VIS_CLASSIFY_BACKGROUND; ++k)
        EXPECT_EQ(f->args[k].count, 0u);
    for (u32 t = 0; t < f->tile_count; ++t)
        EXPECT_EQ(f->masks[t], 0u);
    /* Half covered: the uncovered tiles only in the background list. */
    ccx_random_image(f, 200, 120, 0u);
    ccx_rect(f, 0, 0, 96, 120, 3);
    ccx_upload(f);
    ASSERT_TRUE(ccx_run(f));
    expect_matches(f, "half");
    for (u32 ty = 0; ty < f->tiles_y; ++ty)
        for (u32 tx = 13; tx < f->tiles_x; ++tx) { /* x >= 104: background only */
            EXPECT_EQ(list_hits(f, DE__VIS_CLASSIFY_BACKGROUND, tx, ty), 1u);
            EXPECT_EQ(f->masks[ty * f->tiles_x + tx], 0u);
        }
}

/* classify_tile_size_16: r.classify_tile 16 bins 16 x 16 tiles. */
static void tile_size_16(ccx *f, de_gpu_backend b) {
    ccx_open(f, b);
    ASSERT_TRUE(cvar_set("r.classify_tile", "16"));
    scene_random(&f->sc, &f->r);
    ccx_random_image(f, 1000, 563, 80u);
    ccx_upload(f);
    ASSERT_TRUE(ccx_run(f));
    EXPECT_EQ(f->tile, 16u);
    EXPECT_EQ(f->tiles_x, 63u);
    EXPECT_EQ(f->tiles_y, 36u);
    expect_matches(f, "tile 16");
}

/* classify_args_equal_counts: a 1080p image; the arguments equal the list lengths (checked in
 * expect_matches), and the classify passes' GPU time is printed (budget: 0.2 ms on M1-class
 * hardware; a note, not a gate). */
static void args_equal_counts(ccx *f, de_gpu_backend b) {
    ccx_open(f, b);
    scene_random(&f->sc, &f->r);
    ccx_random_image(f, 1920, 1080, 600u);
    ccx_upload(f);
    for (u32 i = 0; i < 4; ++i) { /* timings arrive a frame or more later */
        f->print_passes = i == 3u;
        ASSERT_TRUE(ccx_run(f));
    }
    expect_matches(f, "1080p");
    printf("perf note: classification of 1920 x 1080 (copy, clear, classify, args): %.3f ms GPU "
           "(budget 0.2 ms on M1-class hardware; 0 = not timed)\n",
           (double)f->classify_ms);
}

#define GPU_TESTS(sfx, backend)                                                                    \
    TEST_F(classify, tiles_match_cpu_##sfx, ccx) {                                                 \
        tiles_match_cpu(f, backend);                                                               \
    }                                                                                              \
    TEST_F(classify, mixed_tile_both_lists_##sfx, ccx) {                                           \
        mixed_tile(f, backend);                                                                    \
    }                                                                                              \
    TEST_F(classify, background_list_##sfx, ccx) {                                                 \
        background_list(f, backend);                                                               \
    }                                                                                              \
    TEST_F(classify, tile_size_16_##sfx, ccx) {                                                    \
        tile_size_16(f, backend);                                                                  \
    }                                                                                              \
    TEST_F(classify, args_equal_counts_##sfx, ccx) {                                               \
        args_equal_counts(f, backend);                                                             \
    }

GPU_TESTS(metal, DE_GPU_METAL)
GPU_TESTS(vulkan, DE_GPU_VULKAN)
GPU_TESTS(d3d12, DE_GPU_D3D12)

/* ================================================================================================
 * CPU tests
 * ============================================================================================== */

static char *read_text(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return NULL;
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1u);
    if (s != NULL && fread(s, 1, (size_t)n, fp) != (size_t)n) {
        free(s);
        s = NULL;
    }
    if (s != NULL)
        s[n] = '\0';
    fclose(fp);
    return s;
}

/* classify.slang's constants against the C table, ClassifyParams's size, the model rule. */
TEST(classify, layout_match) {
    char path[1024];
    snprintf(path, sizeof path, "%s/visbuffer/classify.slang", DE_VIS_TEST_SHADER_DIR);
    char *text = read_text(path);
    ASSERT_TRUE(text != NULL);
    u32 count = 0, found = 0;
    const de__vis_layout_const *k = de__vis_classify_layout_consts(&count);
    for (const char *at = text; (at = strstr(at, "public static const uint ")) != NULL;) {
        at += strlen("public static const uint ");
        char name[96];
        long long value = 0;
        if (sscanf(at, "%95[A-Za-z0-9_] = %lli", name, &value) != 2) {
            EXPECT_TRUE(false);
            continue;
        }
        bool ok = false;
        for (u32 i = 0; i < count; ++i)
            if (strcmp(k[i].name, name) == 0) {
                ok = true;
                EXPECT_EQ((u32)value, k[i].value);
            }
        if (!ok)
            printf("  %s is not in de__vis_classify_layout_consts\n", name);
        EXPECT_TRUE(ok);
        ++found;
    }
    EXPECT_EQ(found, count);
    const char *ps = strstr(text, "struct ClassifyParams");
    const char *pe = ps != NULL ? strstr(ps, "};") : NULL;
    u32 fields = 0;
    for (const char *ln = ps; ln != NULL && pe != NULL && ln < pe; ln = strchr(ln + 1, '\n'))
        fields += strncmp(ln, "\n    uint ", 10) == 0;
    EXPECT_EQ(fields * 4u, 80u);
    free(text);
    EXPECT_EQ(DE__VIS_CLASSIFY_LISTS, DE__GS_MATERIAL_MODELS + 1u);
    /* The model rule: READY entries give their model, everything else Standard. */
    de__gs_material m[3];
    memset(m, 0, sizeof m);
    m[0].flags = DE__GS_MATERIAL_READY;
    m[0].model = 9;
    m[1].model = 4; /* not READY */
    m[2].flags = DE__GS_MATERIAL_READY;
    m[2].model = DE__GS_MATERIAL_MODEL_UNLIT;
    EXPECT_EQ(de__vis_classify_model(m, 3, 0), 9u);
    EXPECT_EQ(de__vis_classify_model(m, 3, 1), (u32)DE__GS_MATERIAL_MODEL_STANDARD);
    EXPECT_EQ(de__vis_classify_model(m, 3, 2), (u32)DE__GS_MATERIAL_MODEL_UNLIT);
    EXPECT_EQ(de__vis_classify_model(m, 3, 3), (u32)DE__GS_MATERIAL_MODEL_STANDARD);
    EXPECT_EQ(de__vis_classify_model(m, 3, DE__GS_NONE), (u32)DE__GS_MATERIAL_MODEL_STANDARD);
    EXPECT_EQ(de__vis_classify_model(NULL, 0, 0), (u32)DE__GS_MATERIAL_MODEL_STANDARD);
}
