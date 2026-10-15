/*
 * test_instance_cull.c: GPU instance culling and the visibility-buffer frame skeleton (DENG-229).
 * Executable test_render_vis; CTest names render_vis.<suite>.<test>.
 *
 * A fake render world and fake mesh records drive a GPU scene (DENG-177); the vis frame
 * (src/render/passes/vis/) is built into the same graph, and a test pass reads back the work list,
 * the counters and the dispatch arguments. Each GPU test compares them, instance by instance and
 * field by field, with the CPU reference of vis_stages.h run on the very entries the GPU read
 * (de__gpu_scene_debug_pack) and the cull constants the frame uploaded (de__vis_context.cull).
 * Scenes are generated so that no instance lies within a small margin of a frustum plane or of an
 * LOD threshold: float differences between the CPU and the GPU cannot flip a decision, so the sets
 * must match exactly.
 *
 * GPU tests run per backend (_metal, _vulkan, _d3d12; SKIP without the device, or without the
 * engine shader vis_instance_cull for it, DENG-421). CI: Vulkan on lavapipe and D3D12 on the
 * vendored WARP in the golden jobs.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "de_alloc_guard.h"
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include "gpu/gpu_backend.h"
#include "render/components_internal.h"
#include "render/graph/rg_internal.h"
#include "render/loaders/render_assets.h"
#include "render/passes/vis/vis_stages.h"
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif
#include "render_d3d12_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS  (10ull * 1000000000ull)
#define NONE_U32 0xFFFFFFFFu

/* ================================================================================================
 * Fake meshes: 1..12 with meshlets and LODs, 13 without meshlets, 14 never ready
 * ============================================================================================== */

#define MESHES         14u
#define MESH_NO_MLETS  13u
#define MESH_NOT_READY 14u
static const u32 k_meshlets0[12] = {1, 5, 64, 65, 130, 200, 300, 17, 128, 129, 2, 40};
static de_render_mesh g_mesh[MESHES];

static f32 mesh_half(u32 id) {
    return 0.5f + 0.1f * (f32)id;
}

static void meshes_init(void) {
    memset(g_mesh, 0, sizeof g_mesh);
    for (u32 i = 0; i < MESHES; ++i) {
        const u32 id = i + 1u;
        de_render_mesh *m = &g_mesh[i];
        const f32 h = mesh_half(id);
        for (u32 c = 0; c < 3; ++c) {
            m->bounds_min[c] = -h;
            m->bounds_max[c] = h;
            m->position_scale[c] = 1.0f;
        }
        m->sphere[3] = h * 1.7320508f;
        for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s)
            m->stream_offset[s] = UINT32_MAX;
        m->submesh_count = 1;
        const u32 m0 = i < 12u ? k_meshlets0[i] : 4u;
        m->lod_count = i < 12u ? 1u + i % DE_COOKED_MESH_MAX_LODS : 2u;
        u32 first = 0;
        for (u32 l = 0; l < m->lod_count; ++l) {
            const u32 n = (m0 >> l) > 0 ? (m0 >> l) : 1u;
            m->lods[l] = (de_cooked_mesh_lod){.index_first = l * 300u,
                                              .index_count = 300u,
                                              .meshlet_first = first,
                                              .meshlet_count = n,
                                              .triangle_count = 100u * n,
                                              .vertex_count = 64u * n,
                                              .error = l == 0 ? 0.0f : 0.004f * (f32)(1u << l) * h};
            first += n;
        }
        m->meshlet_count = id == MESH_NO_MLETS ? 0u : first;
        m->vertex_count = 64u * first;
        m->index_count = 300u * m->lod_count;
    }
}

static const de_render_mesh *fake_mesh(void *user, de_mesh m) {
    (void)user;
    return m.id >= 1u && m.id < MESH_NOT_READY ? &g_mesh[m.id - 1u] : NULL;
}
static const de_render_material *fake_material(void *user, de_material m) {
    (void)user;
    (void)m;
    return NULL;
}
static u32 fake_texture(void *user, de_texture t) {
    (void)user;
    (void)t;
    return 0;
}

/* ================================================================================================
 * Fake render world: static instances, uploaded in the first frame
 * ============================================================================================== */

typedef struct world {
    de__render_world_frame f;
    u32 cap;
    u64 rng;
    bool touch_all; /* the next frame lists every slot as changed (mesh changes) */
} world;

static void *zalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    ASSERT_TRUE(p != NULL);
    return p;
}

static void world_init(world *w, u32 cap, u64 seed) {
    memset(w, 0, sizeof *w);
    w->cap = cap;
    w->rng = seed ? seed : 0x9E3779B97F4A7C15ull;
    de__rw_instances *in = &w->f.instances;
    in->entity = (u64 *)zalloc(cap * sizeof(u64));
    in->mesh = (de_mesh *)zalloc(cap * sizeof(de_mesh));
    in->materials = (de_material *)zalloc((size_t)cap * DE__RW_MATERIALS * sizeof(de_material));
    in->transform = (de_affine *)zalloc(cap * sizeof(de_affine));
    in->prev_transform = (de_affine *)zalloc(cap * sizeof(de_affine));
    in->bounds = (de_aabb *)zalloc(cap * sizeof(de_aabb));
    in->layer_mask = (u32 *)zalloc(cap * sizeof(u32));
    in->flags = (u8 *)zalloc(cap);
    w->f.dirty_instances = (u32 *)zalloc(cap * sizeof(u32));
    w->f.removed_instances = (u32 *)zalloc(sizeof(u32));
    w->f.dirty_lights = (u32 *)zalloc(sizeof(u32));
    w->f.removed_lights = (u32 *)zalloc(sizeof(u32));
    w->f.origin = (de_dvec3){1024.0, -2048.0, 0.0};
}

static void world_free(world *w) {
    de__rw_instances *in = &w->f.instances;
    free(in->entity);
    free(in->mesh);
    free(in->materials);
    free(in->transform);
    free(in->prev_transform);
    free(in->bounds);
    free(in->layer_mask);
    free(in->flags);
    free(w->f.dirty_instances);
    free(w->f.removed_instances);
    free(w->f.dirty_lights);
    free(w->f.removed_lights);
    memset(w, 0, sizeof *w);
}

static u32 rnd(world *w) {
    w->rng ^= w->rng << 13;
    w->rng ^= w->rng >> 7;
    w->rng ^= w->rng << 17;
    return (u32)(w->rng >> 11);
}
/* Uniform in [lo, hi). */
static f32 rndf(world *w, f32 lo, f32 hi) {
    return lo + (hi - lo) * (f32)(rnd(w) % 1000000u) / 1000000.0f;
}

/* Slot `s`: an instance of mesh `mesh` (0: a free slot) with transform `xf`. */
static void world_set(world *w, u32 s, u32 mesh, de_affine xf, u32 layer, bool bounds) {
    de__rw_instances *in = &w->f.instances;
    in->entity[s] = mesh != 0 ? 1000u + s : 0u;
    in->mesh[s].id = mesh;
    in->transform[s] = xf;
    in->prev_transform[s] = xf;
    in->layer_mask[s] = layer;
    in->flags[s] = 0;
    in->bounds[s] = de_aabb_empty();
    if (mesh != 0 && bounds && mesh <= MESHES) {
        const f32 h = mesh_half(mesh);
        in->bounds[s] = de_aabb_transform((de_aabb){{-h, -h, -h}, {h, h, h}}, &xf);
        in->flags[s] = DE__RW_INST_HAS_BOUNDS;
    }
    if (s >= in->count)
        in->count = s + 1u;
}

static void world_begin(world *w) {
    w->f.frame++;
    w->f.origin_changed = w->f.frame == 1;
    w->f.dirty_instance_count = 0;
    if (w->touch_all && w->f.frame > 1)
        for (u32 s = 0; s < w->f.instances.count; ++s)
            w->f.dirty_instances[w->f.dirty_instance_count++] = s;
    w->touch_all = false;
    w->f.instances.live = 0;
    for (u32 s = 0; s < w->f.instances.count; ++s)
        w->f.instances.live += w->f.instances.entity[s] != 0;
}

/* ================================================================================================
 * Fixture
 * ============================================================================================== */

typedef struct cull_result {
    bool valid;
    u32 counters[4];
    de__vis_dispatch_args args;
    de__vis_work_item *items;
    u32 item_count, item_cap; /* stored items (min(asked, capacity)), array size */
    u32 capacity;
    de__vis_cull_view cv; /* the constants the frame uploaded */
    /* The table entries the GPU read, packed while the frame was current (debug_pack). */
    de__gs_instance *inst;
    u32 inst_count, inst_cap;
    de__gs_mesh *mesh;
    u32 mesh_count, mesh_cap;
} cull_result;

typedef struct icx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_gpu_device *dev;
    de_gpu_backend backend;
    bool real_gpu;
    de_rg *g;
    de__gpu_scene *gs;
    de__vis_shared *vis;
    de_gpu_fence fence;
    u64 frames;
    de_gpu_buffer rb;
    u64 rb_size;
    world w;
    /* The camera and the frame's options. */
    de_vec3 eye, target;
    f32 fov;
    u32 width, height;
    u32 layer_mask;
    u32 fixed_capacity;
    bool readback; /* add the test's readback pass */
    cull_result res;
} icx;

static void icx_setup(icx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "instance_cull_test"}, &f->arena), DE_OK);
    de_error_clear();
    meshes_init();
    f->eye = (de_vec3){0.0f, 0.0f, 1.7f};
    f->target = (de_vec3){0.0f, 10.0f, 1.7f};
    f->fov = 1.0f;
    f->width = 1280;
    f->height = 720;
    f->readback = true;
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

static void icx_teardown(icx *f) {
    if (f->g) {
        de__rg_reset(f->g);
        de__rg_destroy(f->g);
    }
    if (f->dev)
        de_gpu_wait_idle(f->dev);
    de__vis_shared_destroy(f->vis);
    de__gpu_scene_destroy(f->gs);
    if (f->dev) {
        de_gpu_wait_idle(f->dev);
        if (f->real_gpu)
            EXPECT_EQ(validation_errors(f->dev), 0u);
        if (DE_HANDLE_VALID(f->rb))
            de_gpu_buffer_destroy(f->dev, f->rb);
        if (DE_HANDLE_VALID(f->fence))
            de_gpu_fence_destroy(f->dev, f->fence);
        de_gpu_wait_idle(f->dev);
        de_gpu_device_destroy(f->dev);
    }
    free(f->res.items);
    free(f->res.inst);
    free(f->res.mesh);
    world_free(&f->w);
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
    /* The cvars are process-wide: back to their defaults. */
    const char *const names[] = {"r.cull_freeze", "r.cull_disable", "r.lod_error_px", "r.lod_bias",
                                 "r.occlusion"};
    const char *const values[] = {"0", "0", "1", "0", "1"};
    for (u32 i = 0; i < 5; ++i) {
        const de_cvar cv = de_cvar_find(names[i]);
        if (DE_HANDLE_VALID(cv))
            (void)de_cvar_set(cv, values[i]);
    }
}

static bool backend_built(de_gpu_backend b) {
    switch (b) {
    case DE_GPU_NULL:
        return true;
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

/* Opens a device (SKIPs when it is not there), a graph, a GPU scene and the vis frame. */
static void icx_open(icx *f, de_gpu_backend backend, u32 instances, u64 seed) {
    static char why[700];
    f->backend = backend;
    if (!backend_built(backend)) {
        snprintf(why, sizeof why, "the %s backend is not in this build",
                 de_gpu_backend_name(backend));
        SKIP(why);
    }
    f->real_gpu = backend != DE_GPU_NULL;
#if defined(__APPLE__)
    if (backend == DE_GPU_METAL)
        setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
    de_gpu_device_desc desc = {.backend = backend,
                               .debug_layer = f->real_gpu,
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
    if (f->real_gpu) {
        printf("device: \"%s\" (%s)\n", caps->device_name, de_gpu_backend_name(backend));
        render_d3d12_report(f->dev);
        if (!caps->base_tier_ok) {
            /* No compute pipelines below the Base tier (CI's paravirtual Metal GPU). */
            snprintf(why, sizeof why, "below the Base tier: %.400s", caps->base_tier_reason);
            SKIP(why);
        }
    }
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "vis"},
                            &f->g),
              DE_OK);
    f->fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(f->fence));
    de__gpu_scene_desc gd = {.device = f->dev,
                             .allocator = &f->alloc,
                             .resolver = {.mesh = fake_mesh,
                                          .material = fake_material,
                                          .texture_bindless = fake_texture},
                             .staging_bytes = (u64)128 << 20};
    gd.initial_capacity[DE__GS_INSTANCES] = instances;
    ASSERT_EQ(de__gpu_scene_create(&gd, &f->gs), DE_OK);
    ASSERT_EQ(de__vis_shared_create(
                  &(de__vis_shared_desc){.device = f->dev, .allocator = &f->alloc}, &f->vis),
              DE_OK);
    if (!f->vis->enabled) {
        snprintf(why, sizeof why, "the vis frame is off: %.600s", f->vis->why);
        SKIP(why);
    }
    /* These tests check frustum culling: with two-phase occlusion off (DENG-233) phase 1 draws
     * every frustum-visible instance and phase 2 none. test_hzb.c and test_two_phase.c test the
     * occlusion. */
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.occlusion"), "0"), DE_OK);
    world_init(&f->w, instances, seed);
}

/* ---------- One frame ---------- */

typedef struct rb_pass {
    icx *f;
    de_rg_resource items, counters, args;
    u64 items_bytes;
} rb_pass;

#define RB_COUNTERS 0u
#define RB_ARGS     256u
#define RB_ITEMS    512u

static void readback_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    de_gpu_cmd_copy_buffer(cmd, p->f->rb, RB_COUNTERS, de_rg_buffer(pass, p->counters), 0,
                           DE__VIS_COUNTER_BYTES);
    de_gpu_cmd_copy_buffer(cmd, p->f->rb, RB_ARGS, de_rg_buffer(pass, p->args), 0,
                           sizeof(de__vis_dispatch_args));
    de_gpu_cmd_copy_buffer(cmd, p->f->rb, RB_ITEMS, de_rg_buffer(pass, p->items), 0,
                           p->items_bytes);
}

/* Makes the readback buffer hold `bytes`. */
static void ensure_rb(icx *f, u64 bytes) {
    if (bytes <= f->rb_size)
        return;
    if (DE_HANDLE_VALID(f->rb)) {
        de_gpu_wait_idle(f->dev);
        de_gpu_buffer_destroy(f->dev, f->rb);
    }
    f->rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = bytes,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK,
                                                               .name = "vis test readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->rb));
    f->rb_size = bytes;
}

/* Builds, runs and waits for one frame; fills f->res (on a GPU). `vc_out` gets the context (valid
 * until the next frame). */
static bool icx_frame(icx *f, de__vis_context **vc_out) {
    const u64 n = f->frames++;
    world_begin(&f->w);
    if (de__gpu_scene_update(f->gs, &f->w.f) != DE_OK)
        return false;
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    if (de__rg_begin(f->g, f->arena) != DE_OK || de__gpu_scene_add_passes(f->gs, f->g) != DE_OK)
        return false;
    const de_vec3 up = {0.0f, 0.0f, 1.0f};
    de__vis_frame_desc d = {.gpu_scene = f->gs,
                            .view = (de_view){.id = 1},
                            .view_slot = 0,
                            .frame_index = n,
                            .frames_completed = n, /* every earlier frame was waited for */
                            .width = f->width,
                            .height = f->height,
                            .view_matrix = de_m4_look_at(f->eye, f->target, up),
                            .proj =
                                de_m4_perspective_rz(f->fov, (f32)f->width / (f32)f->height, 0.1f),
                            .camera_position = f->eye,
                            .origin = f->w.f.origin,
                            .layer_mask = f->layer_mask,
                            .work_capacity_fixed = f->fixed_capacity};
    de__vis_context *vc = NULL;
    if (de__vis_frame_build(f->vis, f->g, &d, &vc) != DE_OK || vc == NULL)
        return false;
    if (vc_out != NULL)
        *vc_out = vc;
    rb_pass *rp = NULL;
    const bool want_rb = f->readback && f->real_gpu && DE_HANDLE_VALID(vc->work_items[0]);
    if (want_rb) {
        rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
        rp->f = f;
        rp->items = vc->work_items[0];
        rp->counters = vc->instance_counters[0];
        rp->args = vc->cull_args[0];
        rp->items_bytes = (u64)vc->work_capacity * sizeof(de__vis_work_item);
        ensure_rb(f, RB_ITEMS + rp->items_bytes);
        de_rg_pass *p =
            de_rg_add_pass(f->g, "test.readback", DE_QUEUE_GRAPHICS, readback_execute, rp);
        de_rg_read(p, rp->items, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->counters, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->args, DE_ACCESS_COPY_SRC);
        const de_rg_resource rb = de_rg_import_buffer(f->g, f->rb, DE_ACCESS_NONE);
        de_rg_write(p, rb, DE_ACCESS_COPY_DST);
    }
    de_result r = de__rg_finalize(f->g);
    if (r == DE_OK)
        r = de__rg_compile(f->g);
    if (r == DE_OK)
        r = de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = n,
                                                        .frame_fence = f->fence,
                                                        .frame_value = n + 1});
    if (r != DE_OK) {
        fprintf(stderr, "frame %llu: %s\n", (unsigned long long)n,
                de_error_last() ? de_error_last()->message : "?");
        de__gpu_scene_frame_end(f->gs, f->g, false);
        return false;
    }
    cull_result *res = &f->res;
    /* The entries as this frame uploaded them (instances from this frame's render world). */
    res->inst_count = de__gpu_scene_table(f->gs, DE__GS_INSTANCES).count;
    res->mesh_count = de__gpu_scene_table(f->gs, DE__GS_MESHES).count;
    if (res->inst_count > res->inst_cap) {
        free(res->inst);
        res->inst = (de__gs_instance *)zalloc(res->inst_count * sizeof(de__gs_instance));
        res->inst_cap = res->inst_count;
    }
    if (res->mesh_count > res->mesh_cap) {
        free(res->mesh);
        res->mesh = (de__gs_mesh *)zalloc(res->mesh_count * sizeof(de__gs_mesh));
        res->mesh_cap = res->mesh_count;
    }
    for (u32 i = 0; i < res->inst_count; ++i)
        de__gpu_scene_debug_pack(f->gs, DE__GS_INSTANCES, i, &res->inst[i]);
    for (u32 i = 0; i < res->mesh_count; ++i)
        de__gpu_scene_debug_pack(f->gs, DE__GS_MESHES, i, &res->mesh[i]);
    de__gpu_scene_frame_end(f->gs, f->g, true);
    if (de_gpu_fence_wait(f->dev, f->fence, n + 1, WAIT_NS) != DE_OK)
        return false;
    res->valid = false;
    res->cv = vc->cull;
    res->capacity = vc->work_capacity;
    if (!want_rb)
        return true;
    const u8 *m = (const u8 *)de_gpu_buffer_map(f->dev, f->rb);
    memcpy(res->counters, m + RB_COUNTERS, sizeof res->counters);
    memcpy(&res->args, m + RB_ARGS, sizeof res->args);
    const u32 asked = res->counters[DE__VIS_COUNTER_WORK_ITEMS / 4u];
    res->item_count = asked < vc->work_capacity ? asked : vc->work_capacity;
    if (res->item_count > res->item_cap) {
        free(res->items);
        res->items = (de__vis_work_item *)zalloc(res->item_count * sizeof(de__vis_work_item));
        res->item_cap = res->item_count;
    }
    if (res->item_count > 0)
        memcpy(res->items, m + RB_ITEMS, res->item_count * sizeof(de__vis_work_item));
    res->valid = true;
    return true;
}

/* ================================================================================================
 * Scenes and the CPU comparison
 * ============================================================================================== */

/* The cull constants the frame will upload for the fixture's camera (de__vis_frame_build makes
 * the same with the same inputs). */
static de__vis_cull_view cpu_view(const icx *f) {
    const de_mat4 view = de_m4_look_at(f->eye, f->target, (de_vec3){0.0f, 0.0f, 1.0f});
    const de_mat4 proj = de_m4_perspective_rz(f->fov, (f32)f->width / (f32)f->height, 0.1f);
    de__vis_cull_view v;
    de__vis_cull_view_init(
        &v,
        &(de__vis_cull_camera){.view_proj = de_m4_mul(&proj, &view),
                               .proj = proj,
                               .position = f->eye,
                               .height = f->height,
                               .lod_error_px = (f32)de_cvar_float(de_cvar_find("r.lod_error_px")),
                               .lod_bias = (i32)de_cvar_int(de_cvar_find("r.lod_bias"))});
    v.layer_mask = f->layer_mask != 0 ? f->layer_mask : ~0u;
    return v;
}

static f32 absf(f32 x) {
    return x < 0.0f ? -x : x;
}

/* True when an instance with world AABB `b`, transform `xf`, mesh `mesh` sits within a margin of
 * a frustum plane or an LOD threshold (a float difference could flip the decision). */
static bool marginal(const de__vis_cull_view *v, de_aabb b, const de_affine *xf, u32 mesh) {
    const f32 hx = 0.5f * (b.max.x - b.min.x), hy = 0.5f * (b.max.y - b.min.y),
              hz = 0.5f * (b.max.z - b.min.z);
    const f32 c[3] = {b.min.x + hx, b.min.y + hy, b.min.z + hz};
    const f32 r = sqrtf(hx * hx + hy * hy + hz * hz);
    const f32 mn[3] = {b.min.x, b.min.y, b.min.z}, mx[3] = {b.max.x, b.max.y, b.max.z};
    for (u32 i = 0; i < DE__VIS_CULL_PLANES; ++i) {
        const f32 *p = v->planes[i];
        const f32 dc = p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3];
        const f32 eps = 1e-3f * (1.0f + absf(p[3]) + absf(dc) + r);
        if (absf(dc + r) < eps)
            return true;
        const f32 x = p[0] >= 0.0f ? mx[0] : mn[0], y = p[1] >= 0.0f ? mx[1] : mn[1],
                  z = p[2] >= 0.0f ? mx[2] : mn[2];
        if (absf(p[0] * x + p[1] * y + p[2] * z + p[3]) < eps)
            return true;
        /* A component of the normal near zero picks the corner by its sign. */
        for (u32 k = 0; k < 3; ++k)
            if (p[k] != 0.0f && absf(p[k]) < 1e-6f)
                return true;
    }
    if (mesh == 0 || mesh > MESHES)
        return false;
    const de_render_mesh *m = &g_mesh[mesh - 1u];
    f32 sq = 0.0f;
    for (u32 j = 0; j < 3; ++j) {
        const f32 s = xf->m[j * 3u] * xf->m[j * 3u] + xf->m[j * 3u + 1u] * xf->m[j * 3u + 1u] +
                      xf->m[j * 3u + 2u] * xf->m[j * 3u + 2u];
        sq = s > sq ? s : sq;
    }
    const f32 scale = sqrtf(sq);
    const f32 dx = c[0] - v->camera[0], dy = c[1] - v->camera[1], dz = c[2] - v->camera[2];
    f32 dist = sqrtf(dx * dx + dy * dy + dz * dz) - r;
    if (absf(dist - v->camera[3]) < 1e-3f * (1.0f + dist))
        return true;
    dist = dist > v->camera[3] ? dist : v->camera[3];
    for (u32 l = 1; l < m->lod_count; ++l) {
        const f32 e = m->lods[l].error * scale * v->lod[0];
        if (absf(e - dist) <= 1e-3f * (e + dist))
            return true;
    }
    return false;
}

static de_affine rnd_affine(world *w, f32 x0, f32 x1, f32 y0, f32 y1, f32 z0, f32 z1) {
    const de_vec3 t = {rndf(w, x0, x1), rndf(w, y0, y1), rndf(w, z0, z1)};
    de_vec3 axis = {rndf(w, -1, 1), rndf(w, -1, 1), rndf(w, -1, 1)};
    const f32 len = sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    axis = len > 1e-3f ? (de_vec3){axis.x / len, axis.y / len, axis.z / len} : (de_vec3){0, 0, 1};
    const de_quat q = de_quat_axis_angle(axis, rndf(w, 0.0f, 6.2831853f));
    const de_vec3 s = {rndf(w, 0.3f, 4.0f), rndf(w, 0.3f, 4.0f), rndf(w, 0.3f, 4.0f)};
    return de_affine_from_trs(t, q, s);
}

/* `n` random instances around the camera: about a third visible; some free slots, some without
 * bounds, some of the mesh without meshlets or the mesh that never loads. `layers`: pick a
 * random layer mask from them (NULL: 1). */
static void scene_random(icx *f, u32 n, const u32 *layers, u32 layer_count,
                         const de__vis_cull_view *other) {
    const de__vis_cull_view v = cpu_view(f);
    world *w = &f->w;
    for (u32 s = 0; s < n; ++s) {
        const u32 kind = rnd(w) % 100u;
        u32 mesh = 1u + rnd(w) % 12u;
        bool bounds = true;
        if (kind < 2)
            mesh = 0;
        else if (kind < 4)
            bounds = false;
        else if (kind < 6)
            mesh = MESH_NO_MLETS;
        else if (kind < 8)
            mesh = MESH_NOT_READY;
        const u32 layer = layers != NULL ? layers[rnd(w) % layer_count] : 1u;
        de_affine xf = de_affine_identity();
        for (u32 tries = 0;; ++tries) {
            xf = rnd_affine(w, -150.0f, 150.0f, -60.0f, 300.0f, -20.0f, 40.0f);
            const f32 h = mesh_half(mesh != 0 ? mesh : 1u);
            const de_aabb b = de_aabb_transform((de_aabb){{-h, -h, -h}, {h, h, h}}, &xf);
            const bool m2 = other != NULL && marginal(other, b, &xf, mesh);
            if ((!marginal(&v, b, &xf, mesh) && !m2) || tries > 100)
                break;
        }
        world_set(w, s, mesh, xf, layer, bounds);
    }
}

/* Compares f->res with the CPU reference for every instance slot. With `all_fit` (the work list
 * held every item) each visible instance must have all its chunks; otherwise only the stored
 * items are checked (each a chunk of a visible instance, no chunk twice). */
static void check_against_cpu(icx *f, bool all_fit, u32 *visible_out) {
    const cull_result *res = &f->res;
    ASSERT_TRUE(res->valid);
    const de__vis_cull_view *v = &res->cv;
    const u32 n = res->inst_count;
    u32 *gpu_lod = (u32 *)zalloc(n * sizeof(u32));
    u32 *gpu_chunks = (u32 *)zalloc(n * sizeof(u32));
    u64 *gpu_mask = (u64 *)zalloc(n * sizeof(u64) * 8u); /* 512 chunks per instance at most */
    for (u32 s = 0; s < n; ++s)
        gpu_lod[s] = NONE_U32;
    u32 bad_items = 0;
    for (u32 i = 0; i < res->item_count; ++i) {
        const de__vis_work_item *it = &res->items[i];
        if (it->instance >= n) {
            ++bad_items;
            continue;
        }
        const de__gs_instance e = res->inst[it->instance];
        if (e.mesh == DE__GS_NONE || e.mesh >= res->mesh_count) {
            if (bad_items++ < 8)
                printf("  item %u: instance %u has no mesh\n", i, it->instance);
            continue;
        }
        const de__gs_mesh m = res->mesh[e.mesh];
        if (gpu_lod[it->instance] == NONE_U32)
            gpu_lod[it->instance] = it->lod;
        if (gpu_lod[it->instance] != it->lod || it->lod >= m.lod_count) {
            if (bad_items++ < 8)
                printf("  item %u: instance %u lod %u (first seen %u, %u LODs)\n", i, it->instance,
                       it->lod, gpu_lod[it->instance], m.lod_count);
            continue;
        }
        const de__gs_mesh_lod *l = &m.lods[it->lod];
        const u32 off = it->meshlet_first - l->meshlet_first;
        const u32 chunk = off / DE__VIS_CHUNK_MESHLETS;
        const u32 rest = l->meshlet_count - off;
        const u32 want = rest < DE__VIS_CHUNK_MESHLETS ? rest : DE__VIS_CHUNK_MESHLETS;
        if (it->meshlet_first < l->meshlet_first || off % DE__VIS_CHUNK_MESHLETS != 0 ||
            off >= l->meshlet_count || it->meshlet_count != want || chunk >= 512u) {
            ++bad_items;
            continue;
        }
        u64 *mask = &gpu_mask[(size_t)it->instance * 8u + chunk / 64u];
        if ((*mask >> (chunk % 64u)) & 1u) {
            ++bad_items; /* the same chunk twice */
            continue;
        }
        *mask |= 1ull << (chunk % 64u);
        gpu_chunks[it->instance]++;
    }
    EXPECT_EQ(bad_items, 0u);

    u32 tested = 0, visible = 0, meshlets = 0, chunks = 0, mismatches = 0;
    for (u32 s = 0; s < n; ++s) {
        const de__gs_instance e = res->inst[s];
        de__gs_mesh m;
        memset(&m, 0, sizeof m);
        if (e.mesh != DE__GS_NONE && e.mesh < res->mesh_count)
            m = res->mesh[e.mesh];
        u32 lod = 0;
        const i32 t = de__vis_instance_test(v, &e, e.mesh != DE__GS_NONE ? &m : NULL, &lod);
        tested += t >= 0;
        u32 want_chunks = 0;
        if (t == 1) {
            ++visible;
            meshlets += m.lods[lod].meshlet_count;
            want_chunks =
                (m.lods[lod].meshlet_count + DE__VIS_CHUNK_MESHLETS - 1u) / DE__VIS_CHUNK_MESHLETS;
            chunks += want_chunks;
        }
        bool ok;
        if (all_fit)
            ok = (t == 1 ? gpu_lod[s] == lod : gpu_lod[s] == NONE_U32) &&
                 gpu_chunks[s] == want_chunks;
        else
            ok = gpu_lod[s] == NONE_U32 || (t == 1 && gpu_lod[s] == lod);
        if (!ok) {
            if (mismatches < 8)
                printf("  slot %u: cpu %d lod %u chunks %u; gpu lod %d chunks %u\n", s, t, lod,
                       want_chunks, gpu_lod[s] == NONE_U32 ? -1 : (i32)gpu_lod[s], gpu_chunks[s]);
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u);
    EXPECT_EQ(res->counters[DE__VIS_COUNTER_INSTANCES_TESTED / 4u], tested);
    EXPECT_EQ(res->counters[DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u], visible);
    EXPECT_EQ(res->counters[DE__VIS_COUNTER_MESHLETS / 4u], meshlets);
    EXPECT_EQ(res->counters[DE__VIS_COUNTER_WORK_ITEMS / 4u], chunks);
    const u32 stored = chunks < res->capacity ? chunks : res->capacity;
    EXPECT_EQ(res->item_count, stored);
    const de__vis_dispatch_args a = de__vis_dispatch_args_for(stored);
    EXPECT_EQ(res->args.x, a.x);
    EXPECT_EQ(res->args.y, a.y);
    EXPECT_EQ(res->args.z, a.z);
    EXPECT_EQ(res->args.count, a.count);
    if (visible_out != NULL)
        *visible_out = visible;
    free(gpu_lod);
    free(gpu_chunks);
    free(gpu_mask);
}

/* ================================================================================================
 * CPU-only tests
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

/* The `public static const uint NAME = VALUE;` lines of a Slang file, checked against the C
 * table: every one must be there with the same value. Returns how many it found. */
static u32 check_consts(const char *path) {
    char *text = read_text(path);
    EXPECT_TRUE(text != NULL);
    if (text == NULL)
        return 0;
    u32 n = 0, count = 0;
    const de__vis_layout_const *k = de__vis_layout_consts(&count);
    for (const char *at = text; (at = strstr(at, "public static const uint ")) != NULL;) {
        at += strlen("public static const uint ");
        char name[96];
        long long value = 0;
        if (sscanf(at, "%95[A-Za-z0-9_] = %lli", name, &value) != 2) {
            EXPECT_TRUE(false);
            continue;
        }
        bool found = false;
        for (u32 i = 0; i < count; ++i) {
            if (strcmp(k[i].name, name) == 0) {
                found = true;
                if ((u32)value != k[i].value)
                    printf("  %s: slang %lld, C %u\n", name, value, k[i].value);
                EXPECT_EQ((u32)value, k[i].value);
            }
        }
        if (!found)
            printf("  %s is not in de__vis_layout_consts\n", name);
        EXPECT_TRUE(found);
        ++n;
    }
    free(text);
    return n;
}

/* The size of `struct` `name` in a Slang file: uint/int/float 4 bytes, the 4-vectors 16, arrays
 * by their length (the rules of vis_types.slang). 0 when it is not there. */
static u32 slang_struct_size(const char *path, const char *name) {
    char *text = read_text(path);
    if (text == NULL)
        return 0;
    char head[128];
    snprintf(head, sizeof head, "public struct %s\n", name);
    const char *at = strstr(text, head);
    u32 size = 0;
    const char *body = at != NULL ? strchr(at, '{') : NULL;
    const char *end = body != NULL ? strstr(body, "};") : NULL;
    if (end != NULL) {
        for (const char *ln = strstr(body, "public "); ln != NULL && ln < end;
             ln = strstr(ln + 7, "public ")) {
            char type[32], field[64];
            if (sscanf(ln, "public %31s %63[]A-Za-z0-9_[]", type, field) != 2)
                continue;
            u32 elem = strcmp(type, "float4") == 0 || strcmp(type, "uint4") == 0 ? 16u : 4u;
            const char *br = strchr(field, '[');
            const u32 len = br != NULL ? (u32)strtoul(br + 1, NULL, 10) : 1u;
            size += elem * len;
        }
    }
    free(text);
    return size;
}

TEST(vis, layout_match) {
    char path[1024];
    snprintf(path, sizeof path, "%s/visbuffer/vis_types.slang", DE_VIS_TEST_SHADER_DIR);
    const u32 a = check_consts(path);
    EXPECT_EQ(slang_struct_size(path, "DeVisWorkItem"), (u32)sizeof(de__vis_work_item));
    EXPECT_EQ(slang_struct_size(path, "DeVisMeshlet"), (u32)sizeof(de__vis_meshlet));
    EXPECT_EQ(slang_struct_size(path, "DeVisDispatchArgs"), (u32)sizeof(de__vis_dispatch_args));
    EXPECT_EQ(slang_struct_size(path, "DeVisMeshArgs"), (u32)sizeof(de__vis_mesh_args));
    EXPECT_EQ(slang_struct_size(path, "DeVisDrawIndexedArgs"),
              (u32)sizeof(de__vis_draw_indexed_args));
    snprintf(path, sizeof path, "%s/culling/cull_common.slang", DE_VIS_TEST_SHADER_DIR);
    const u32 b = check_consts(path);
    EXPECT_EQ(slang_struct_size(path, "DeVisCullView"), (u32)sizeof(de__vis_cull_view));
    u32 count = 0;
    (void)de__vis_layout_consts(&count);
    EXPECT_EQ(a + b, count); /* and every C constant is in one of the files */
    /* The packed ID: 25 + 7 bits. */
    EXPECT_EQ(de__vis_id_pack(DE__VIS_ID_MAX_VISIBLE_MESHLETS - 1u, 123u), 0xFFFFFFFBu);
    EXPECT_EQ(de__vis_id_meshlet(de__vis_id_pack(1234567u, 99u)), 1234567u);
    EXPECT_EQ(de__vis_id_triangle(de__vis_id_pack(1234567u, 99u)), 99u);
}

TEST(instance_cull, indirect_args_cpu) {
    static const u32 counts[] = {0, 1, 2, 65534, 65535, 65536, 131070, 131071, 1000000};
    for (u32 i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
        const de__vis_dispatch_args a = de__vis_dispatch_args_for(counts[i]);
        EXPECT_GE(a.x, 1u);
        EXPECT_GE(a.y, 1u);
        EXPECT_EQ(a.z, 1u);
        EXPECT_LE(a.x, DE__VIS_MAX_GROUPS_X);
        EXPECT_EQ(a.count, counts[i]);
        /* Every item has a group, and the last row is the only partial one. */
        EXPECT_GE((u64)a.x * a.y, (u64)counts[i]);
        EXPECT_LT((u64)a.x * (a.y - 1u), counts[i] > 0 ? (u64)counts[i] : 1u);
    }
}

TEST(instance_cull, cpu_reference) {
    /* The planes are de_frustum_from_m4's; the tests agree with de_frustum_test_* (which also
     * test the far plane, at infinity here). */
    const de_mat4 view = de_m4_look_at((de_vec3){1, 2, 3}, (de_vec3){4, 9, 2}, (de_vec3){0, 0, 1});
    const de_mat4 proj = de_m4_perspective_rz(1.2f, 1.5f, 0.25f);
    const de_mat4 vp = de_m4_mul(&proj, &view);
    de__vis_cull_view v;
    de__vis_cull_view_init(&v, &(de__vis_cull_camera){.view_proj = vp,
                                                      .proj = proj,
                                                      .position = {1, 2, 3},
                                                      .height = 900,
                                                      .lod_error_px = 2.0f,
                                                      .lod_bias = 1});
    const de_frustum fr = de_frustum_from_m4(&vp);
    for (u32 i = 0; i < DE__VIS_CULL_PLANES; ++i) {
        EXPECT_FLOAT_EQ(v.planes[i][0], fr.planes[i].n.x, 0.0f);
        EXPECT_FLOAT_EQ(v.planes[i][3], fr.planes[i].d, 0.0f);
    }
    EXPECT_FLOAT_EQ(v.camera[3], 0.25f, 0.0f);
    EXPECT_FLOAT_EQ(v.lod[0], 0.5f * 900.0f * (1.0f / tanf(0.6f)) / 2.0f, 1e-2f);
    EXPECT_FLOAT_EQ(v.lod[1], 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(v.lod[2], 1.0f, 0.0f);
    EXPECT_EQ(v.flags, (u32)DE__VIS_CULL_FRUSTUM);
    world w = {.rng = 77};
    u32 agree = 0;
    for (u32 i = 0; i < 2000; ++i) {
        const f32 c[3] = {rndf(&w, -50, 50), rndf(&w, -50, 50), rndf(&w, -50, 50)};
        const f32 r = rndf(&w, 0.1f, 5.0f);
        const f32 mn[3] = {c[0] - r, c[1] - r * 0.5f, c[2] - r * 0.25f};
        const f32 mx[3] = {c[0] + r, c[1] + r * 0.5f, c[2] + r * 0.25f};
        const bool s1 = de__vis_cull_sphere(&v, c, r);
        const bool s2 = de_frustum_test_sphere(&fr, (de_vec3){c[0], c[1], c[2]}, r);
        const bool b1 = de__vis_cull_aabb(&v, mn, mx);
        const bool b2 =
            de_frustum_test_aabb(&fr, (de_aabb){{mn[0], mn[1], mn[2]}, {mx[0], mx[1], mx[2]}});
        agree += s1 == s2 && b1 == b2;
    }
    EXPECT_EQ(agree, 2000u);
    /* An orthographic view: distance 1, no near clamp. */
    const de_mat4 ortho = de_m4_ortho_rz(-10, 10, -5, 5, 0.1f, 1000.0f);
    de__vis_cull_view o;
    de__vis_cull_view_init(&o, &(de__vis_cull_camera){.view_proj = de_m4_mul(&ortho, &view),
                                                      .proj = ortho,
                                                      .height = 100,
                                                      .lod_error_px = 1.0f});
    EXPECT_FLOAT_EQ(o.lod[1], 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(o.lod[0], 0.5f * 100.0f * 0.2f, 1e-5f); /* pixels per metre */
}

/* r.cull_freeze across a rebase: the frozen view moved to the new origin decides like a view
 * built at the new origin from the same world-space camera. */
TEST(instance_cull, freeze_rebase_cpu) {
    const de_dvec3 o1 = {1024.0, -2048.0, 0.0}, o2 = {2048.0, -1024.0, 1024.0};
    const de_vec3 eye_w = {1030.0f, -2040.0f, 2.0f}; /* world, near o1 */
    const de_vec3 tgt_w = {1040.0f, -2000.0f, 2.0f};
    const de_mat4 proj = de_m4_perspective_rz(1.0f, 1.6f, 0.1f);
    de__vis_cull_view v1, v2;
    for (u32 k = 0; k < 2; ++k) {
        const de_dvec3 o = k == 0 ? o1 : o2;
        const de_vec3 e = {(f32)(eye_w.x - o.x), (f32)(eye_w.y - o.y), (f32)(eye_w.z - o.z)};
        const de_vec3 t = {(f32)(tgt_w.x - o.x), (f32)(tgt_w.y - o.y), (f32)(tgt_w.z - o.z)};
        const de_mat4 view = de_m4_look_at(e, t, (de_vec3){0, 0, 1});
        de__vis_cull_view_init(k == 0 ? &v1 : &v2,
                               &(de__vis_cull_camera){.view_proj = de_m4_mul(&proj, &view),
                                                      .proj = proj,
                                                      .position = e,
                                                      .height = 720,
                                                      .lod_error_px = 1.0f});
    }
    de__vis_cull_view_rebase(&v1, o1, o2);
    for (u32 i = 0; i < DE__VIS_CULL_PLANES; ++i)
        for (u32 c = 0; c < 4; ++c)
            EXPECT_FLOAT_EQ(v1.planes[i][c], v2.planes[i][c], 2e-3f);
    for (u32 c = 0; c < 3; ++c)
        EXPECT_FLOAT_EQ(v1.camera[c], v2.camera[c], 1e-3f);
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            EXPECT_FLOAT_EQ(v1.view_proj[r][c], v2.view_proj[r][c], 2e-2f);
}

/* LOD selection of the CPU reference at chosen distances (the same projected-error formula the
 * GPU test then checks): the coarsest LOD within the pixel error, then the bias. */
TEST(instance_cull, lod_selection_cpu) {
    meshes_init();
    const de_mat4 proj = de_m4_perspective_rz(1.0f, 16.0f / 9.0f, 0.1f);
    const de_mat4 view = de_m4_look_at((de_vec3){0, 0, 0}, (de_vec3){0, 1, 0}, (de_vec3){0, 0, 1});
    de__vis_cull_view v;
    de__vis_cull_view_init(&v, &(de__vis_cull_camera){.view_proj = de_m4_mul(&proj, &view),
                                                      .proj = proj,
                                                      .height = 720,
                                                      .lod_error_px = 1.0f});
    const f32 px_per_m = 0.5f * 720.0f / tanf(0.5f); /* at distance 1 */
    de__gs_mesh m;
    memset(&m, 0, sizeof m);
    m.flags = DE__GS_MESH_READY | DE__GS_MESH_MESHLETS;
    m.lod_count = 8;
    for (u32 l = 0; l < 8; ++l)
        m.lods[l].error = l == 0 ? 0.0f : 0.001f * (f32)(1u << l);
    de__gs_instance e;
    memset(&e, 0, sizeof e);
    for (u32 r = 0; r < 3; ++r)
        e.transform[r].v[r] = 2.0f; /* scale 2 */
    e.sphere.v[3] = 0.5f;
    for (f32 d = 0.05f; d < 5000.0f; d *= 1.37f) {
        e.sphere.v[1] = d + 0.5f; /* closest point at d */
        u32 want = 0;
        const f32 dist = d > 0.1f ? d : 0.1f;
        for (u32 l = 1; l < 8; ++l)
            if (m.lods[l].error * 2.0f * px_per_m / dist <= 1.0f * (1.0f + 1e-5f))
                want = l;
        const u32 got = de__vis_select_lod(&v, &e, &m);
        /* Away from a threshold the two forms agree exactly. */
        bool near_threshold = false;
        for (u32 l = 1; l < 8; ++l) {
            const f32 x = m.lods[l].error * 2.0f * px_per_m;
            near_threshold |= fabsf(x - dist) < 1e-3f * (x + dist);
        }
        if (!near_threshold)
            EXPECT_EQ(got, want);
    }
    /* Bias: clamped to the mesh's LODs. */
    e.sphere.v[1] = 1000.0f;
    v.lod[2] = 3.0f;
    EXPECT_EQ(de__vis_select_lod(&v, &e, &m), 7u);
    v.lod[2] = -20.0f;
    EXPECT_EQ(de__vis_select_lod(&v, &e, &m), 0u);
}

/* ================================================================================================
 * GPU tests
 * ============================================================================================== */

/* Sets a cvar; false when it does not exist (the DE_CVAR_DEV_ONLY debug cvars in Shipping). */
static bool cvar_set(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (!DE_HANDLE_VALID(cv))
        return false;
    EXPECT_EQ(de_cvar_set(cv, value), DE_OK);
    return true;
}

/* instance_cull_frustum_matches_cpu: 10k instances, the GPU set and LODs equal the CPU's. */
static void frustum_matches_cpu(icx *f, de_gpu_backend b) {
    icx_open(f, b, 10000u, 1234u);
    /* The second camera's view, so the scene has no marginal instance for either. */
    const de_vec3 eye = f->eye, target = f->target;
    f->eye = (de_vec3){-40.0f, 120.0f, 10.0f};
    f->target = (de_vec3){60.0f, 40.0f, -5.0f};
    const de__vis_cull_view second = cpu_view(f);
    f->eye = eye;
    f->target = target;
    scene_random(f, 10000u, NULL, 0, &second);
    ASSERT_TRUE(icx_frame(f, NULL));
    u32 visible = 0;
    check_against_cpu(f, true, &visible);
    printf("10000 instances: %u visible, %u work items\n", visible,
           f->res.counters[DE__VIS_COUNTER_WORK_ITEMS / 4u]);
    EXPECT_GT(visible, 1000u);
    EXPECT_LT(visible, 9000u);
    /* Another camera on the same scene. */
    f->eye = (de_vec3){-40.0f, 120.0f, 10.0f};
    f->target = (de_vec3){60.0f, 40.0f, -5.0f};
    ASSERT_TRUE(icx_frame(f, NULL));
    check_against_cpu(f, true, NULL);
}

/* instance_cull_layer_mask: instances whose layer mask misses the view's are never tested. */
static void layer_mask(icx *f, de_gpu_backend b) {
    icx_open(f, b, 4000u, 99u);
    static const u32 layers[] = {1u, 2u, 4u, 8u, 3u, 6u, 0u, 0xF0u};
    f->layer_mask = 0x5u;
    scene_random(f, 4000u, layers, 8, NULL);
    ASSERT_TRUE(icx_frame(f, NULL));
    check_against_cpu(f, true, NULL);
    for (u32 i = 0; i < f->res.item_count; ++i) {
        const u32 s = f->res.items[i].instance;
        EXPECT_NE(f->w.f.instances.layer_mask[s] & 0x5u, 0u);
    }
    /* Every layer: more tested. */
    const u32 tested5 = f->res.counters[DE__VIS_COUNTER_INSTANCES_TESTED / 4u];
    f->layer_mask = 0;
    ASSERT_TRUE(icx_frame(f, NULL));
    check_against_cpu(f, true, NULL);
    EXPECT_GT(f->res.counters[DE__VIS_COUNTER_INSTANCES_TESTED / 4u], tested5);
}

/* instance_cull_lod_selection: instances along the view axis at many distances and scales, with
 * r.lod_error_px and r.lod_bias changed between frames. */
static void lod_selection(icx *f, de_gpu_backend b) {
    icx_open(f, b, 256u, 5u);
    f->eye = (de_vec3){0.0f, 0.0f, 0.0f};
    f->target = (de_vec3){0.0f, 1.0f, 0.0f};
    const de__vis_cull_view v = cpu_view(f);
    u32 s = 0;
    for (u32 k = 0; k < 3; ++k) {
        const f32 scale = k == 0 ? 1.0f : k == 1 ? 2.5f : 0.4f;
        for (f32 d = 1.0f; d < 3000.0f && s < 256u; d *= 1.21f) {
            f32 dd = d;
            de_affine xf = de_affine_identity();
            for (u32 tries = 0; tries < 50; ++tries) {
                xf = de_affine_from_trs((de_vec3){0.0f, dd, 0.0f}, (de_quat){0, 0, 0, 1},
                                        (de_vec3){scale, scale, scale});
                const f32 h = mesh_half(8);
                const de_aabb bb = de_aabb_transform((de_aabb){{-h, -h, -h}, {h, h, h}}, &xf);
                if (!marginal(&v, bb, &xf, 8))
                    break;
                dd *= 1.01f;
            }
            world_set(&f->w, s++, 8u /* 8 LODs */, xf, 1u, true);
        }
    }
    static const char *const err[] = {"1", "4", "0.5", "1"};
    static const char *const bias[] = {"0", "0", "-1", "2"};
    for (u32 run = 0; run < 4; ++run) {
        ASSERT_TRUE(cvar_set("r.lod_error_px", err[run]));
        ASSERT_TRUE(cvar_set("r.lod_bias", bias[run]));
        ASSERT_TRUE(icx_frame(f, NULL));
        check_against_cpu(f, true, NULL);
        /* Several LODs in use. */
        u32 seen = 0;
        for (u32 i = 0; i < f->res.item_count; ++i)
            seen |= 1u << f->res.items[i].lod;
        u32 distinct = 0;
        for (u32 l = 0; l < 8; ++l)
            distinct += (seen >> l) & 1u;
        EXPECT_GE(distinct, 4u);
    }
}

/* instance_cull_chunking: chunks of at most 64 meshlets (65 meshlets are 64 + 1); a full work
 * list drops items but counts them, the arguments cover only what was stored, and in the
 * automatic mode the next frame grows the list from the count read back. */
static void chunking(icx *f, de_gpu_backend b) {
    icx_open(f, b, 2400u, 7u);
    f->eye = (de_vec3){0.0f, -2.0f, 0.0f};
    f->target = (de_vec3){0.0f, 1.0f, 0.0f};
    /* r.lod_bias -7: LOD 0 everywhere, 1 .. 300 meshlets (1 .. 5 chunks). */
    ASSERT_TRUE(cvar_set("r.lod_bias", "-7"));
    u32 s = 0;
    for (u32 y = 0; y < 40; ++y)
        for (u32 x = 0; x < 60; ++x, ++s)
            world_set(&f->w, s, 1u + s % 12u,
                      de_affine_from_trs((de_vec3){(f32)x * 0.7f - 21.0f, 30.0f + (f32)y * 0.9f,
                                                   (f32)(s % 7u) - 3.0f},
                                         (de_quat){0, 0, 0, 1}, (de_vec3){1, 1, 1}),
                      1u, true);
    ASSERT_TRUE(icx_frame(f, NULL));
    check_against_cpu(f, true, NULL);
    const u32 all = f->res.counters[DE__VIS_COUNTER_WORK_ITEMS / 4u];
    printf("%u instances, %u work items, list of %u\n", s, all, f->res.capacity);
    EXPECT_GT(all, 4000u);
    /* A fixed list of 1000 entries. */
    f->fixed_capacity = 1000u;
    ASSERT_TRUE(icx_frame(f, NULL));
    EXPECT_EQ(f->res.counters[DE__VIS_COUNTER_WORK_ITEMS / 4u], all);
    EXPECT_EQ(f->res.item_count, 1000u);
    EXPECT_EQ(f->res.args.count, 1000u);
    check_against_cpu(f, false, NULL);
    /* Automatic again, with every instance of the 300-meshlet mesh (5 chunks): more than the
     * list holds. That frame drops items; the next reads the count back and grows the list. */
    f->fixed_capacity = 0;
    for (u32 i = 0; i < s; ++i)
        world_set(&f->w, i, 7u, f->w.f.instances.transform[i], 1u, true);
    f->w.touch_all = true;
    ASSERT_TRUE(icx_frame(f, NULL));
    const u32 cap1 = f->res.capacity;
    const u32 asked = f->res.counters[DE__VIS_COUNTER_WORK_ITEMS / 4u];
    printf("%u work items for a list of %u\n", asked, cap1);
    EXPECT_GT(asked, cap1);
    check_against_cpu(f, false, NULL);
    ASSERT_TRUE(icx_frame(f, NULL));
    EXPECT_GT(f->res.capacity, cap1);
    check_against_cpu(f, true, NULL);
    const de__vis_stats st = de__vis_view_stats(f->vis, 0);
    EXPECT_TRUE(st.valid);
    EXPECT_EQ(st.work_items, asked);
    EXPECT_EQ(st.work_capacity, cap1);
    EXPECT_EQ(st.instances_visible, s);
}

/* A grid of `count` small instances of mesh 1 (one meshlet) in front of the camera. */
static void grid(icx *f, u32 count) {
    const u32 side = (u32)ceilf(sqrtf((f32)count));
    for (u32 i = 0; i < count; ++i)
        world_set(&f->w, i, 1u,
                  de_affine_from_trs((de_vec3){((f32)(i % side) - (f32)side * 0.5f) * 0.3f,
                                               60.0f + (f32)(i / side) * 0.3f, 0.0f},
                                     (de_quat){0, 0, 0, 1}, (de_vec3){0.1f, 0.1f, 0.1f}),
                  1u, true);
}

/* instance_cull_indirect_args_valid: nothing visible gives one empty group; more than 65535 work
 * items continue in y. */
static void indirect_args_valid(icx *f, de_gpu_backend b) {
    icx_open(f, b, 70000u, 3u);
    f->eye = (de_vec3){0.0f, 0.0f, 0.0f};
    f->target = (de_vec3){0.0f, 1.0f, 0.0f};
    grid(f, 70000u);
    ASSERT_TRUE(icx_frame(f, NULL));
    check_against_cpu(f, true, NULL);
    EXPECT_EQ(f->res.counters[DE__VIS_COUNTER_WORK_ITEMS / 4u], 70000u);
    EXPECT_EQ(f->res.args.x, DE__VIS_MAX_GROUPS_X);
    EXPECT_EQ(f->res.args.y, 2u);
    EXPECT_EQ(f->res.args.z, 1u);
    EXPECT_EQ(f->res.args.count, 70000u);
    /* Looking away: nothing visible, one group of nothing. */
    f->target = (de_vec3){0.0f, -1.0f, 0.0f};
    ASSERT_TRUE(icx_frame(f, NULL));
    check_against_cpu(f, true, NULL);
    EXPECT_EQ(f->res.counters[DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u], 0u);
    EXPECT_EQ(f->res.args.x, 1u);
    EXPECT_EQ(f->res.args.y, 1u);
    EXPECT_EQ(f->res.args.z, 1u);
    EXPECT_EQ(f->res.args.count, 0u);
    /* r.cull_disable: everything with a loaded mesh (the debug cvars are not in Shipping). */
    if (!cvar_set("r.cull_disable", "1"))
        return;
    ASSERT_TRUE(icx_frame(f, NULL));
    EXPECT_EQ(f->res.counters[DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u], 70000u);
    EXPECT_EQ(f->res.args.count, 70000u);
    (void)cvar_set("r.cull_disable", "0");
    /* r.cull_freeze: frozen while looking away, the camera turning back changes nothing. */
    (void)cvar_set("r.cull_freeze", "1");
    ASSERT_TRUE(icx_frame(f, NULL));
    f->target = (de_vec3){0.0f, 1.0f, 0.0f};
    ASSERT_TRUE(icx_frame(f, NULL));
    EXPECT_EQ(f->res.counters[DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u], 0u);
    EXPECT_TRUE((f->res.cv.flags & DE__VIS_CULL_FROZEN) != 0);
    (void)cvar_set("r.cull_freeze", "0");
    ASSERT_TRUE(icx_frame(f, NULL));
    EXPECT_EQ(f->res.counters[DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u], 70000u);
}

#define GPU_TESTS(backend_suffix, backend)                                                         \
    TEST_F(instance_cull, frustum_matches_cpu_##backend_suffix, icx) {                             \
        frustum_matches_cpu(f, backend);                                                           \
    }                                                                                              \
    TEST_F(instance_cull, layer_mask_##backend_suffix, icx) {                                      \
        layer_mask(f, backend);                                                                    \
    }                                                                                              \
    TEST_F(instance_cull, lod_selection_##backend_suffix, icx) {                                   \
        lod_selection(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(instance_cull, chunking_##backend_suffix, icx) {                                        \
        chunking(f, backend);                                                                      \
    }                                                                                              \
    TEST_F(instance_cull, indirect_args_valid_##backend_suffix, icx) {                             \
        indirect_args_valid(f, backend);                                                           \
    }

GPU_TESTS(metal, DE_GPU_METAL)
GPU_TESTS(vulkan, DE_GPU_VULKAN)
GPU_TESTS(d3d12, DE_GPU_D3D12)

/* ================================================================================================
 * The frame skeleton (null backend)
 * ============================================================================================== */

/* The marker names of the null log that start with `prefix`, in order, joined by ','. */
static void markers(de_gpu_device *d, const char *prefix, char *out, size_t cap) {
    out[0] = '\0';
    size_t at = 0;
    const u32 n = de__gpu_null_log_count(d);
    for (u32 i = 0; i < n; ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(d, i);
        if (e->op != DE__GPU_NULL_OP_MARKER_PUSH || strncmp(e->text, prefix, strlen(prefix)) != 0)
            continue;
        const int w = snprintf(out + at, cap - at, "%s%s", at ? "," : "", e->text);
        if (w < 0 || (size_t)w >= cap - at)
            break;
        at += (size_t)w;
    }
}

/* vis_frame_stub_graph_builds: with the later stages stubbed the frame builds; the instance- and
 * meshlet-culling passes exist (meshlet culling reads the arguments, and the stats readbacks keep
 * both alive; DENG-231); steady frames hit the compiled-graph cache and allocate nothing. */
TEST_F(vis_frame, stub_graph_builds, icx) {
    icx_open(f, DE_GPU_NULL, 512u, 11u);
    scene_random(f, 300u, NULL, 0, NULL);
    f->readback = false;
    de__gpu_null_log_enable(f->dev, true);
    de__vis_context *vc = NULL;
    ASSERT_TRUE(icx_frame(f, &vc));
    de__gpu_null_log_enable(f->dev, false);
    char got[1024];
    markers(f->dev, "vis.", got, sizeof got);
    /* DENG-231: meshlet culling reads the arguments and its stats pass keeps it alive. DENG-233:
     * the depth clear, the HZB (1280 x 720 depth: a 1024 x 512 HZB of 11 mips, mip 0 then mips
     * 1-6 and 7-10 in two reduce passes) and phase 2 of both culling stages run too. DENG-234:
     * the null device has mesh shaders, so the vis-ID clear and phase 1's mesh raster run (its
     * depth feeds the HZB); phase 2's raster has no reader yet (classify, DENG-237), so the graph
     * culls it. DENG-236: no mask material, so no alpha-tested raster pass. */
    EXPECT_STREQ(got, "vis.depth_clear,vis.cull_instances.clear,vis.cull_instances,"
                      "vis.cull_instances.args,vis.cull_meshlets.clear,vis.cull_meshlets,"
                      "vis.cull_meshlets.args,vis.vis_ids_clear,vis.raster.mesh,vis.hzb.level0,"
                      "vis.hzb.reduce0,vis.hzb.reduce6,"
                      "vis.cull_instances2.clear,vis.cull_instances2,vis.cull_instances2.args,"
                      "vis.cull_instances.stats,vis.cull_meshlets.phase2,"
                      "vis.cull_meshlets.args.phase2,vis.cull_meshlets.stats");
    /* The context: phase 1's outputs, nothing for phase 2 or the stubbed stages. */
    ASSERT_TRUE(vc != NULL);
    EXPECT_TRUE(DE_HANDLE_VALID(vc->work_items[0]));
    EXPECT_TRUE(DE_HANDLE_VALID(vc->cull_args[0]));
    EXPECT_TRUE(DE_HANDLE_VALID(vc->instance_counters[0]));
    EXPECT_TRUE(DE_HANDLE_VALID(vc->work_items[1])); /* phase 2 (DENG-233) */
    EXPECT_TRUE(DE_HANDLE_VALID(vc->depth));
    EXPECT_TRUE(DE_HANDLE_VALID(vc->hzb));
    EXPECT_TRUE(DE_HANDLE_VALID(vc->visibility));
    EXPECT_TRUE(DE_HANDLE_VALID(vc->vis_ids)); /* DENG-234 */
    EXPECT_TRUE(vc->depth_target);
    EXPECT_EQ(vc->raster_path, (u32)DE__VIS_RASTER_MESH);
    EXPECT_EQ(vc->instance_count, 300u);
    EXPECT_TRUE(vc->view_new);
    EXPECT_GE(vc->work_capacity, 4096u);
    EXPECT_EQ(vc->cull.instance_count, 300u);
    EXPECT_EQ(vc->cull.work_capacity, vc->work_capacity);
    EXPECT_EQ(vc->cull_view_index, 0u);
    /* Meshlet culling (DENG-231): one list for both phases, phase 1's raster arguments. */
    EXPECT_TRUE(DE_HANDLE_VALID(vc->visible_meshlets));
    EXPECT_TRUE(DE_HANDLE_VALID(vc->meshlet_counters));
    EXPECT_TRUE(DE_HANDLE_VALID(vc->raster_args[0]));
    EXPECT_TRUE(DE_HANDLE_VALID(vc->raster_args[1])); /* phase 2 (DENG-233) */
    EXPECT_GE(vc->visible_capacity, 65536u);

    /* A reader of the arguments keeps the args pass. */
    f->readback = true;
    f->real_gpu = true; /* readback pass on the null device (it copies nothing real) */
    de__gpu_null_log_enable(f->dev, true);
    ASSERT_TRUE(icx_frame(f, &vc));
    de__gpu_null_log_enable(f->dev, false);
    f->real_gpu = false;
    f->readback = false;
    markers(f->dev, "vis.", got, sizeof got);
    EXPECT_STREQ(got, "vis.depth_clear,vis.cull_instances.clear,vis.cull_instances,"
                      "vis.cull_instances.args,vis.cull_meshlets.clear,vis.cull_meshlets,"
                      "vis.cull_meshlets.args,vis.vis_ids_clear,vis.raster.mesh,vis.hzb.level0,"
                      "vis.hzb.reduce0,vis.hzb.reduce6,"
                      "vis.cull_instances2.clear,vis.cull_instances2,vis.cull_instances2.args,"
                      "vis.cull_instances.stats,vis.cull_meshlets.phase2,"
                      "vis.cull_meshlets.args.phase2,vis.cull_meshlets.stats");
    EXPECT_FALSE(vc->view_new);
    EXPECT_EQ(vc->cull_view_index, 1u);

    /* Steady frames: cache hits, no heap allocation. */
    for (u32 i = 0; i < 6; ++i)
        ASSERT_TRUE(icx_frame(f, NULL));
    const de__rg_stats before_stats = de__rg_compile_stats(f->g);
    const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    bool ok = true;
    for (u32 i = 0; i < 20 && ok; ++i)
        ok = icx_frame(f, NULL);
    de_alloc_guard_end(&guard);
    ASSERT_TRUE(ok);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).alloc_count, before.alloc_count);
    const de__rg_stats after_stats = de__rg_compile_stats(f->g);
    EXPECT_EQ(after_stats.compiles, before_stats.compiles);
    EXPECT_EQ(after_stats.hits, before_stats.hits + 20u);
}

/* A scene with no instance slots adds nothing to the view graph. */
TEST_F(vis_frame, no_instances_no_passes, icx) {
    icx_open(f, DE_GPU_NULL, 16u, 1u);
    f->readback = false;
    de__gpu_null_log_enable(f->dev, true);
    de__vis_context *vc = NULL;
    ASSERT_TRUE(icx_frame(f, &vc));
    de__gpu_null_log_enable(f->dev, false);
    char got[256];
    markers(f->dev, "vis.", got, sizeof got);
    EXPECT_STREQ(got, "");
    ASSERT_TRUE(vc != NULL);
    EXPECT_EQ(vc->instance_count, 0u);
    EXPECT_FALSE(DE_HANDLE_VALID(vc->work_items[0]));
    EXPECT_FALSE(DE_HANDLE_VALID(vc->scene_instances));
}

/* The renderer registers "vis.frame" after "gpu_scene.view": its passes appear in the main view's
 * graph; de_render_stats.instances_visible is written; steady frames hit the graph cache. */
TEST(vis_frame, renderer_stage) {
    de_tracking_allocator track;
    de_allocator a = de_tracking_allocator_init(&track, NULL, DE_MEM_RENDER);
    de_fp_env_init_thread();
    de_type_registry *types = NULL;
    de_world *w = NULL;
    de_gpu_device *d = NULL;
    de_renderer *r = NULL;
    ASSERT_EQ(de_type_registry_create(&a, &types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &a, .types = types}, &w), DE_OK);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &d), DE_OK);
    ASSERT_EQ(
        de_renderer_create(&(de_renderer_desc){.device = d, .types = types, .allocator = &a}, &r),
        DE_OK);
    de_renderer_attach(r, w);
    const de_cvar pipelined = de_cvar_find("r.pipelined");
    ASSERT_TRUE(DE_HANDLE_VALID(pipelined));
    (void)de_cvar_set(pipelined, "0");
    const DeRenderComponents ids = *de__render_components(w);
    const de_builtin_component_ids bi = de_builtin_components(w);
    de_entity cam = DE_ENTITY_NULL;
    const de_component cc[2] = {bi.transform, ids.camera};
    ASSERT_EQ(de_entity_spawn_with(w, cc, 2, &cam), DE_OK);
    DeTransform *ct = (DeTransform *)de_entity_get(w, cam, bi.transform);
    ct->position = (de_dvec3){0, -10, 2};
    ct->rotation = (de_quat){0, 0, 0, 1};
    ct->scale = (de_vec3){1, 1, 1};
    const de_component mc[2] = {bi.transform, ids.mesh_renderer};
    for (u32 i = 0; i < 50; ++i) {
        de_entity e = DE_ENTITY_NULL;
        ASSERT_EQ(de_entity_spawn_with(w, mc, 2, &e), DE_OK);
        DeTransform *t = (DeTransform *)de_entity_get(w, e, bi.transform);
        t->position = (de_dvec3){(f64)(i % 10), (f64)(i / 10), 0};
        t->rotation = (de_quat){0, 0, 0, 1};
        t->scale = (de_vec3){1, 1, 1};
        ((DeMeshRenderer *)de_entity_get(w, e, ids.mesh_renderer))->mesh.id = 1u + i % 3u;
    }
#define RUN_FRAME()                                                                                \
    do {                                                                                           \
        ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f), DE_OK);   \
        ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f), DE_OK);       \
        ASSERT_EQ(de__render_extract_and_kick(r, w), DE_OK);                                       \
    } while (0)
    de__gpu_null_log_enable(d, true);
    RUN_FRAME();
    de__gpu_null_log_enable(d, false);
    /* The main view's graph: the debug clear, the scene constants, then the vis frame. */
    char got[1024];
    got[0] = '\0';
    size_t at = 0;
    const u32 n = de__gpu_null_log_count(d);
    for (u32 i = 0; i < n; ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(d, i);
        if (e->op != DE__GPU_NULL_OP_MARKER_PUSH || strncmp(e->list, "view:main", 9) != 0)
            continue;
        at += (size_t)snprintf(got + at, sizeof got - at, "%s%s", at ? "," : "", e->text);
    }
    /* DENG-304: the output pass reads the lighting target, so lighting, the resolve and
     * classification run, and the view's colour is the output pass's (the debug clear's write is
     * overwritten, so it is culled). */
    EXPECT_STREQ(got,
                 "gpu_scene.view,vis.depth_clear,vis.cull_instances.clear,"
                 "vis.cull_instances,vis.cull_instances.args,vis.cull_meshlets.clear,"
                 "vis.cull_meshlets,vis.cull_meshlets.args,vis.vis_ids_clear,vis.raster.mesh,"
                 "vis.hzb.level0,vis.hzb.reduce0,vis.hzb.reduce6,vis.cull_instances2.clear,"
                 "vis.cull_instances2,"
                 "vis.cull_instances2.args,vis.cull_instances.stats,vis.cull_meshlets.phase2,"
                 "vis.cull_meshlets.args.phase2,vis.cull_meshlets.stats,vis.raster.mesh.phase2,"
                 "vis.classify.copy,vis.classify.clear,vis.classify,vis.classify.args,"
                 "vis.resolve.clear,vis.resolve.feedback,vis.resolve,lighting.cluster_bounds,"
                 "lighting.lights_clear,lighting.lights_prep,lighting.lights_count,"
                 "lighting.lights_scan,lighting.lights_fill,lighting.deferred,output.basic,"
                 "view.resolve");
    const de_render_stats *rs = de_renderer_stats(r);
    for (u32 i = 0; i < 4; ++i)
        RUN_FRAME();
    EXPECT_EQ(rs->instances_visible, 0u); /* nothing is loaded: no mesh is READY */
    EXPECT_EQ(rs->meshlets_visible, 0u);
    u64 compiles = 0, hits = 0;
    de__view_graph_stats(r, de_renderer_main_view(r), &compiles, &hits);
    for (u32 i = 0; i < 5; ++i)
        RUN_FRAME();
    u64 compiles2 = 0, hits2 = 0;
    de__view_graph_stats(r, de_renderer_main_view(r), &compiles2, &hits2);
    EXPECT_EQ(compiles2, compiles);
    EXPECT_EQ(hits2, hits + 5u);
    /* The debug cvars (not in Shipping) change nothing structural. */
    (void)cvar_set("r.cull_freeze", "1");
    RUN_FRAME();
    ct = (DeTransform *)de_entity_get(w, cam, bi.transform);
    ct->position.x = 3.0;
    RUN_FRAME();
    (void)cvar_set("r.cull_freeze", "0");
    (void)cvar_set("r.cull_disable", "1");
    RUN_FRAME();
    (void)cvar_set("r.cull_disable", "0");
    /* r.hzb_debug (DENG-233, Dev only): the view shows an HZB mip, composited into its output. */
    if (cvar_set("r.hzb_debug", "2")) {
        de__gpu_null_log_enable(d, true);
        RUN_FRAME();
        de__gpu_null_log_enable(d, false);
        bool debug = false, resolve = false;
        for (u32 i = 0; i < de__gpu_null_log_count(d); ++i) {
            const de__gpu_null_entry *e = de__gpu_null_log_entry(d, i);
            if (e->op != DE__GPU_NULL_OP_MARKER_PUSH || strncmp(e->list, "view:main", 9) != 0)
                continue;
            debug |= strcmp(e->text, "vis.hzb_debug") == 0;
            resolve |= strcmp(e->text, "view.resolve") == 0;
        }
        EXPECT_TRUE(debug);
        EXPECT_TRUE(resolve);
        (void)cvar_set("r.hzb_debug", "-1");
    }
#undef RUN_FRAME
    (void)de_cvar_set(pipelined, "1");
    de_renderer_destroy(r);
    de_world_destroy(w);
    de_gpu_device_destroy(d);
    de_type_registry_destroy(types);
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}
