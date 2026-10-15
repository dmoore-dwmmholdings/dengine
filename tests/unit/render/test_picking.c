/*
 * test_picking.c: vis-buffer picking (DENG-239): de_view_request_pick / de_view_pick_result and
 * the pick stage of the vis frame (src/render/passes/vis/picking.c, shaders/visbuffer/pick.slang).
 * Executable test_render_picking; CTest names render_picking.<suite>.<test>.
 *
 * The GPU tests drive the vis frame directly (a GPU scene from a fake render world, as
 * test_raster_alpha.c): flat 5 x 5 grid patches (one meshlet, 32 triangles, the meshlet's
 * triangles in the order the test wrote them), each instance with its own entity bits. A pick is
 * checked two ways: against a known triangle (a pixel whose centre lies well inside one
 * triangle's screen projection: entity and (meshlet << 7) | triangle), and against the vis buffer
 * the same frame drew (read back by the test and decoded on the CPU through the visible-meshlet
 * list read back with it), which checks the decode at any pixel, edges included.
 *
 * GPU tests run per backend (_metal, _vulkan, _d3d12) and SKIP without the device, below the Base
 * tier or (mesh path) without mesh shaders. CI: Vulkan on lavapipe and D3D12 on the vendored WARP.
 * The null-backend tests (request cap, pass list, the public API through a renderer) run
 * everywhere.
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
#include "render/formats/de_cooked_mesh.h"
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

#define WAIT_NS (10ull * 1000000000ull)

#define GRID        5u
#define PATCH_VERTS (GRID * GRID)
#define PATCH_TRIS  (2u * (GRID - 1u) * (GRID - 1u))
#define MAX_MESHES  2u
#define MAT_OPAQUE  1u
#define ENTITY_BASE 0x0000000700001000ull /* entity bits of slot s: ENTITY_BASE + s */

static void *zalloc(size_t n) {
    void *p = calloc(1, n > 0 ? n : 1);
    if (p == NULL) {
        fprintf(stderr, "out of memory\n");
        abort();
    }
    return p;
}

/* ================================================================================================
 * Fake assets
 * ============================================================================================== */

typedef struct fmesh {
    de_render_mesh rm;
    f32 pos[PATCH_VERTS * 3];
    u8 tri[PATCH_TRIS * 3];
    de_aabb bounds;
    de_gpu_buffer buf[4];
} fmesh;

static fmesh g_mesh[MAX_MESHES];
static u32 g_mesh_count;
static de_render_material g_mat;

static const de_render_mesh *fake_mesh(void *user, de_mesh m) {
    (void)user;
    return m.id >= 1u && m.id <= g_mesh_count ? &g_mesh[m.id - 1u].rm : NULL;
}
static const de_render_material *fake_material(void *user, de_material m) {
    (void)user;
    return m.id == MAT_OPAQUE ? &g_mat : NULL;
}
static u32 fake_texture(void *user, de_texture t) {
    (void)user;
    (void)t;
    return 0u;
}

static void put_u32(u8 *p, u32 v) {
    memcpy(p, &v, 4);
}
static void put_f32(u8 *p, f32 v) {
    memcpy(p, &v, 4);
}

/* A flat square patch of edge `size` centred at the origin, facing -y (towards a camera at the
 * origin looking along +y): its triangles wind counter-clockwise seen from there. */
static void mesh_quad(fmesh *fm, f32 size, de_gpu_device *dev) {
    memset(fm, 0, sizeof *fm);
    fm->bounds = de_aabb_empty();
    for (u32 j = 0; j < GRID; ++j)
        for (u32 i = 0; i < GRID; ++i) {
            const u32 v = j * GRID + i;
            /* Tangent (1, 0, 0) and bitangent (0, 0, 1) of the normal (0, -1, 0). */
            const f32 x = ((f32)i / (f32)(GRID - 1u) - 0.5f) * size;
            const f32 y = ((f32)j / (f32)(GRID - 1u) - 0.5f) * size;
            const de_vec3 p = {x, 0.0f, y};
            fm->pos[3 * v] = p.x;
            fm->pos[3 * v + 1] = p.y;
            fm->pos[3 * v + 2] = p.z;
            fm->bounds.min = de_v3_min(fm->bounds.min, p);
            fm->bounds.max = de_v3_max(fm->bounds.max, p);
        }
    u32 nt = 0;
    for (u32 j = 0; j + 1u < GRID; ++j)
        for (u32 i = 0; i + 1u < GRID; ++i) {
            const u8 v00 = (u8)(j * GRID + i), v10 = (u8)(v00 + 1u);
            const u8 v11 = (u8)(v10 + GRID), v01 = (u8)(v00 + GRID);
            const u8 tris[6] = {v00, v10, v11, v00, v11, v01};
            memcpy(fm->tri + 3u * nt, tris, 6);
            nt += 2u;
        }
    u8 vbytes[PATCH_VERTS * 12u];
    for (u32 v = 0; v < PATCH_VERTS; ++v)
        for (u32 c = 0; c < 3; ++c)
            put_f32(vbytes + 12u * v + 4u * c, fm->pos[3 * v + c]);
    u8 record[DE_COOKED_MESHLET_SIZE];
    memset(record, 0, sizeof record);
    put_u32(record + 8, PATCH_VERTS | (PATCH_TRIS << 8));
    put_f32(record + 28, size); /* centre 0, radius covers the patch */
    put_f32(record + 44, 1.0f); /* cutoff 1: no cone */
    u32 mlv[PATCH_VERTS];
    for (u32 v = 0; v < PATCH_VERTS; ++v)
        mlv[v] = v;
    const void *data[4] = {vbytes, record, mlv, fm->tri};
    const u64 bytes[4] = {sizeof vbytes, sizeof record, sizeof mlv, sizeof fm->tri};
    const u32 which[4] = {DE_RENDER_MESH_VERTICES, DE_RENDER_MESH_MESHLETS,
                          DE_RENDER_MESH_MESHLET_VERTICES, DE_RENDER_MESH_MESHLET_TRIANGLES};
    de_render_mesh *m = &fm->rm;
    for (u32 i = 0; i < 4; ++i) {
        fm->buf[i] = de_gpu_buffer_create(dev, &(de_gpu_buffer_desc){.size = bytes[i],
                                                                     .usage = DE_USAGE_STORAGE,
                                                                     .memory = DE_MEM_UPLOAD,
                                                                     .name = "pick test mesh"});
        ASSERT_TRUE(DE_HANDLE_VALID(fm->buf[i]));
        u8 *map = (u8 *)de_gpu_buffer_map(dev, fm->buf[i]);
        ASSERT_TRUE(map != NULL);
        memcpy(map, data[i], (size_t)bytes[i]);
        m->buffers[which[i]] = fm->buf[i];
        m->bindless[which[i]] = de_gpu_buffer_bindless(dev, fm->buf[i]);
        m->buffer_bytes[which[i]] = bytes[i];
    }
    for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s)
        m->stream_offset[s] = UINT32_MAX;
    m->stream_offset[DE_COOKED_STREAM_POSITION] = 0;
    m->stream_format[DE_COOKED_STREAM_POSITION] = DE_COOKED_VF_F32X3;
    m->vertex_count = PATCH_VERTS;
    m->meshlet_count = 1;
    m->meshlet_vertex_count = PATCH_VERTS;
    m->meshlet_triangle_bytes = PATCH_TRIS * 3u;
    m->submesh_count = 1;
    m->lod_count = 1;
    m->lods[0] = (de_cooked_mesh_lod){
        .meshlet_count = 1, .triangle_count = PATCH_TRIS, .vertex_count = PATCH_VERTS};
    const f32 mn[3] = {fm->bounds.min.x, fm->bounds.min.y, fm->bounds.min.z};
    const f32 mx[3] = {fm->bounds.max.x, fm->bounds.max.y, fm->bounds.max.z};
    for (u32 c = 0; c < 3; ++c) {
        m->bounds_min[c] = mn[c];
        m->bounds_max[c] = mx[c];
        m->sphere[c] = 0.5f * (mn[c] + mx[c]);
        m->position_scale[c] = 1.0f;
    }
    m->sphere[3] = size;
}

static void mesh_free(fmesh *fm, de_gpu_device *dev) {
    for (u32 i = 0; i < 4; ++i)
        if (dev != NULL && DE_HANDLE_VALID(fm->buf[i]))
            de_gpu_buffer_destroy(dev, fm->buf[i]);
    memset(fm, 0, sizeof *fm);
}

/* ================================================================================================
 * Fake render world
 * ============================================================================================== */

typedef struct world {
    de__render_world_frame f;
} world;

static void world_init(world *w, u32 cap) {
    memset(w, 0, sizeof *w);
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

/* Slot `s`: mesh `mesh` (1-based) at `pos`, scaled by `scale`, on layers `layers`. */
static void world_set(world *w, u32 s, u32 mesh, de_vec3 pos, f32 scale, u32 layers) {
    de__rw_instances *in = &w->f.instances;
    const de_affine xf =
        de_affine_from_trs(pos, (de_quat){0, 0, 0, 1}, (de_vec3){scale, scale, scale});
    in->entity[s] = ENTITY_BASE + s;
    in->mesh[s].id = mesh;
    in->transform[s] = xf;
    in->prev_transform[s] = xf;
    in->layer_mask[s] = layers;
    in->bounds[s] = de_aabb_transform(g_mesh[mesh - 1u].bounds, &xf);
    in->flags[s] = (u8)DE__RW_INST_HAS_BOUNDS;
    for (u32 k = 0; k < DE__RW_MATERIALS; ++k)
        in->materials[(size_t)s * DE__RW_MATERIALS + k].id = MAT_OPAQUE;
    if (s >= in->count)
        in->count = s + 1u;
    if (w->f.frame > 0)
        w->f.dirty_instances[w->f.dirty_instance_count++] = s;
}

static void world_begin(world *w) {
    w->f.frame++;
    w->f.origin_changed = w->f.frame == 1;
    w->f.instances.live = 0;
    for (u32 s = 0; s < w->f.instances.count; ++s)
        w->f.instances.live += w->f.instances.entity[s] != 0;
}

/* ================================================================================================
 * Fixture
 * ============================================================================================== */

typedef struct pkx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_gpu_device *dev;
    de_rg *g;
    de__gpu_scene *gs;
    de__vis_shared *vis;
    de_gpu_fence fence;
    u64 frames;
    de_gpu_buffer rb;
    u64 rb_size;
    world w;
    bool dev_null;
    de_vec3 eye, target;
    f32 fov;
    u32 width, height;         /* internal */
    u32 out_width, out_height; /* output (0: the internal size) */
    u32 layer_mask;            /* the view's (0: all) */
    u32 lag;                   /* frames_completed = frame - lag */
    bool readback;             /* read the vis buffer and the visible list back */
    /* The last read-back frame. */
    u32 *ids;
    de__vis_meshlet *visible;
    u32 list_entries;
    de_mat4 view_proj;
} pkx;

static void pkx_setup(pkx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "picking_test"}, &f->arena), DE_OK);
    de_error_clear();
    memset(g_mesh, 0, sizeof g_mesh);
    g_mesh_count = 0;
    memset(&g_mat, 0, sizeof g_mat);
    g_mat.standard.base_color = (de_vec4){1, 1, 1, 1};
    f->eye = (de_vec3){0.0f, 0.0f, 0.0f};
    f->target = (de_vec3){0.0f, 1.0f, 0.0f};
    f->fov = 1.0f;
    f->width = 256;
    f->height = 160;
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

static void pkx_teardown(pkx *f) {
    if (f->g) {
        de__rg_reset(f->g);
        de__rg_destroy(f->g);
    }
    if (f->dev)
        de_gpu_wait_idle(f->dev);
    de__vis_shared_destroy(f->vis);
    de__gpu_scene_destroy(f->gs);
    for (u32 i = 0; i < MAX_MESHES; ++i)
        mesh_free(&g_mesh[i], f->dev);
    g_mesh_count = 0;
    if (f->dev) {
        de_gpu_wait_idle(f->dev);
        if (!f->dev_null)
            EXPECT_EQ(validation_errors(f->dev), 0u);
        if (DE_HANDLE_VALID(f->rb))
            de_gpu_buffer_destroy(f->dev, f->rb);
        if (DE_HANDLE_VALID(f->fence))
            de_gpu_fence_destroy(f->dev, f->fence);
        de_gpu_wait_idle(f->dev);
        de_gpu_device_destroy(f->dev);
    }
    free(f->ids);
    free(f->visible);
    world_free(&f->w);
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
    const char *const names[] = {"r.occlusion", "r.raster_path", "r.lod_bias"};
    const char *const values[] = {"1", "auto", "0"};
    for (u32 i = 0; i < 3; ++i) {
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

static void cvar_set(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    ASSERT_TRUE(DE_HANDLE_VALID(cv));
    ASSERT_EQ(de_cvar_set(cv, value), DE_OK);
}

/* Opens a device (SKIPs without; `mesh_path`: without mesh shaders too), a graph, a GPU scene and
 * the vis frame, and picks the raster path. */
static void pkx_open(pkx *f, de_gpu_backend backend, bool mesh_path, u32 instances) {
    static char why[700];
    if (!backend_built(backend)) {
        snprintf(why, sizeof why, "the %s backend is not in this build",
                 de_gpu_backend_name(backend));
        SKIP(why);
    }
#if defined(__APPLE__)
    if (backend == DE_GPU_METAL)
        setenv("MTL_DEBUG_LAYER", "1", 0);
#endif
    /* Vulkan: the validation layer without GPU-assisted validation (as test_raster_alpha.c). VVL
     * 1.3.275's GPU-assisted validation (CI's lavapipe) checks every bindless image a shader of the
     * command buffer read against the layout a later draw of the same command buffer sees: the
     * pick keeps phase 2's raster (the vis IDs have a reader), so its draw follows the HZB build's
     * sampled read of the depth, and the layer reports the depth "in DEPTH_ATTACHMENT, expected
     * SHADER_READ_ONLY" at that draw, which never reads it through a descriptor. A newer layer
     * does not (follow-up: the CI layer version). */
    de_gpu_device_desc desc = {
        .backend = backend, .debug_layer = backend != DE_GPU_NULL, .pipeline_cache_dir = ""};
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
    printf("device: \"%s\" (%s), mesh shaders %s\n", caps->device_name,
           de_gpu_backend_name(backend), caps->mesh_shaders ? "yes" : "no");
    render_d3d12_report(f->dev);
    if (!caps->base_tier_ok) {
        snprintf(why, sizeof why, "below the Base tier: %.400s", caps->base_tier_reason);
        SKIP(why);
    }
    f->dev_null = backend == DE_GPU_NULL;
    if (mesh_path && !caps->mesh_shaders)
        SKIP("the device has no mesh shaders (the compute-path test covers it)");
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "pk"},
                            &f->g),
              DE_OK);
    f->fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(f->fence));
    de__gpu_scene_desc gd = {.device = f->dev,
                             .allocator = &f->alloc,
                             .resolver = {.mesh = fake_mesh,
                                          .material = fake_material,
                                          .texture_bindless = fake_texture},
                             .staging_bytes = (u64)8 << 20};
    gd.initial_capacity[DE__GS_INSTANCES] = instances;
    ASSERT_EQ(de__gpu_scene_create(&gd, &f->gs), DE_OK);
    ASSERT_EQ(de__vis_shared_create(
                  &(de__vis_shared_desc){.device = f->dev, .allocator = &f->alloc}, &f->vis),
              DE_OK);
    if (!f->vis->enabled) {
        snprintf(why, sizeof why, "the vis frame is off: %.600s", f->vis->why);
        SKIP(why);
    }
    world_init(&f->w, instances);
    cvar_set("r.lod_bias", "-7");
    cvar_set("r.occlusion", "0");
    cvar_set("r.raster_path", mesh_path ? "mesh" : "compute");
}

static u32 pkx_mesh(pkx *f, f32 size) {
    ASSERT_TRUE(g_mesh_count < MAX_MESHES);
    mesh_quad(&g_mesh[g_mesh_count], size, f->dev);
    return ++g_mesh_count;
}

/* ---------- One frame ---------- */

typedef struct rb_pass {
    pkx *f;
    de_rg_resource ids, visible;
    u32 pitch;
    u64 visible_off, visible_bytes;
} rb_pass;

static void readback_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, p->f->rb, 0, p->pitch, de_rg_texture(pass, p->ids), 0,
                                      0);
    de_gpu_cmd_copy_buffer(cmd, p->f->rb, p->visible_off, de_rg_buffer(pass, p->visible), 0,
                           p->visible_bytes);
}

static u64 align_up(u64 v, u64 a) {
    return (v + a - 1u) / a * a;
}

static void ensure_rb(pkx *f, u64 bytes) {
    if (bytes <= f->rb_size)
        return;
    if (DE_HANDLE_VALID(f->rb)) {
        de_gpu_wait_idle(f->dev);
        de_gpu_buffer_destroy(f->dev, f->rb);
    }
    f->rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = bytes,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK,
                                                               .name = "pick test readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->rb));
    f->rb_size = bytes;
}

/* One frame of view slot 0; with `f->readback`, the vis buffer and the visible list read back. */
static bool pkx_frame(pkx *f) {
    const u64 n = f->frames++;
    world_begin(&f->w);
    if (de__gpu_scene_update(f->gs, &f->w.f) != DE_OK)
        return false;
    const de_mat4 view = de_m4_look_at(f->eye, f->target, (de_vec3){0.0f, 0.0f, 1.0f});
    const de_mat4 proj = de_m4_perspective_rz(f->fov, (f32)f->width / (f32)f->height, 0.1f);
    if (de__gpu_scene_set_view(f->gs, 0,
                               &(de__gpu_view_params){.view = view,
                                                      .proj = proj,
                                                      .camera_position = f->eye,
                                                      .width = f->width,
                                                      .height = f->height}) != DE_OK)
        return false;
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    if (de__rg_begin(f->g, f->arena) != DE_OK || de__gpu_scene_add_passes(f->gs, f->g) != DE_OK)
        return false;
    const de__vis_frame_desc d = {.gpu_scene = f->gs,
                                  .view = (de_view){.id = 1},
                                  .view_slot = 0,
                                  .frame_index = n,
                                  .frames_completed = n >= f->lag ? n - f->lag : 0u,
                                  .width = f->width,
                                  .height = f->height,
                                  .view_matrix = view,
                                  .proj = proj,
                                  .camera_position = f->eye,
                                  .origin = f->w.f.origin,
                                  .layer_mask = f->layer_mask,
                                  .output_width = f->out_width,
                                  .output_height = f->out_height};
    de__vis_context *vc = NULL;
    if (de__vis_frame_build(f->vis, f->g, &d, &vc) != DE_OK || vc == NULL)
        return false;
    f->view_proj = de_m4_mul(&proj, &view);
    rb_pass *rp = NULL;
    if (DE_HANDLE_VALID(vc->vis_ids)) {
        if (f->readback && !f->dev_null) {
            rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
            ASSERT_TRUE(rp != NULL);
            rp->f = f;
            rp->ids = vc->vis_ids;
            rp->visible = vc->visible_meshlets;
            rp->pitch = (u32)align_up((u64)f->width * 4u, 256u);
            rp->visible_off = align_up((u64)rp->pitch * f->height, 512u);
            f->list_entries = vc->visible_capacity + vc->visible_alpha_capacity;
            rp->visible_bytes = (u64)f->list_entries * sizeof(de__vis_meshlet);
            ensure_rb(f, rp->visible_off + rp->visible_bytes);
            de_rg_pass *p =
                de_rg_add_pass(f->g, "test.readback", DE_QUEUE_GRAPHICS, readback_execute, rp);
            de_rg_read(p, rp->ids, DE_ACCESS_COPY_SRC);
            de_rg_read(p, rp->visible, DE_ACCESS_COPY_SRC);
            de_rg_write(p, de_rg_import_buffer(f->g, f->rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
        }
        /* The depth back to SHADER_READ before the list ends (Vulkan validation checks the HZB's
         * sampled read against the layout at the end of the command buffer). */
        de_rg_pass *p = de_rg_add_pass(f->g, "test.restore", DE_QUEUE_GRAPHICS, NULL, NULL);
        de_rg_pass_side_effect(p);
        de_rg_read(p, vc->depth, DE_ACCESS_SHADER_READ);
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
    de__gpu_scene_frame_end(f->gs, f->g, true);
    f->w.f.dirty_instance_count = 0;
    if (de_gpu_fence_wait(f->dev, f->fence, n + 1, WAIT_NS) != DE_OK)
        return false;
    if (rp == NULL)
        return true;
    const u8 *m = (const u8 *)de_gpu_buffer_map(f->dev, f->rb);
    if (f->ids == NULL)
        f->ids = (u32 *)zalloc((size_t)f->width * f->height * 4u);
    for (u32 y = 0; y < f->height; ++y)
        memcpy(f->ids + (size_t)y * f->width, m + (u64)y * rp->pitch, (size_t)f->width * 4u);
    free(f->visible);
    f->visible = (de__vis_meshlet *)zalloc(rp->visible_bytes);
    memcpy(f->visible, m + rp->visible_off, (size_t)rp->visible_bytes);
    return true;
}

/* ================================================================================================
 * CPU references
 * ============================================================================================== */

/* The pick the vis buffer read back holds at internal pixel (x, y). */
static void decode_readback(const pkx *f, u32 x, u32 y, u64 *entity, u32 *triangle) {
    *entity = 0;
    *triangle = DE_PICK_TRIANGLE_NONE;
    const u32 id = f->ids[(size_t)y * f->width + x];
    if (id == DE__VIS_ID_EMPTY)
        return;
    const u32 e = de__vis_id_meshlet(id);
    ASSERT_TRUE(e < f->list_entries);
    const de__vis_meshlet vm = f->visible[e];
    ASSERT_TRUE(vm.instance < f->w.f.instances.count);
    *entity = f->w.f.instances.entity[vm.instance];
    *triangle = (vm.meshlet << DE__VIS_ID_TRIANGLE_BITS) | de__vis_id_triangle(id);
}

/* Screen position (pixels, y down) of object point `p` of slot `s`. */
static void project(const pkx *f, u32 s, const f32 p[3], f32 *sx, f32 *sy) {
    const de_affine *xf = &f->w.f.instances.transform[s];
    const de_vec3 w = de_affine_transform_point(xf, (de_vec3){p[0], p[1], p[2]});
    const de_vec4 c = de_m4_mul_v4(&f->view_proj, (de_vec4){w.x, w.y, w.z, 1.0f});
    *sx = (c.x / c.w * 0.5f + 0.5f) * (f32)f->width;
    *sy = (0.5f - c.y / c.w * 0.5f) * (f32)f->height;
}

/* Distance (pixels) of (px, py) inside the edge a -> b (negative: outside), for a triangle with
 * signed area `sign`. */
static f32 inside(f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py, f32 sign) {
    const f32 ex = bx - ax, ey = by - ay;
    const f32 len = sqrtf(ex * ex + ey * ey);
    return sign * (ex * (py - ay) - ey * (px - ax)) / (len > 0.0f ? len : 1.0f);
}

/* A pixel whose centre is at least 0.75 px inside triangle `t` of slot `s` (its centroid's pixel,
 * or a neighbour). False when the triangle is too small. */
static bool triangle_pixel(const pkx *f, u32 s, u32 t, u32 *x, u32 *y) {
    const fmesh *fm = &g_mesh[f->w.f.instances.mesh[s].id - 1u];
    f32 X[3], Y[3];
    for (u32 k = 0; k < 3; ++k)
        project(f, s, &fm->pos[3u * fm->tri[3u * t + k]], &X[k], &Y[k]);
    const f32 area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    const f32 sign = area > 0.0f ? 1.0f : -1.0f;
    const f32 cx = (X[0] + X[1] + X[2]) / 3.0f, cy = (Y[0] + Y[1] + Y[2]) / 3.0f;
    for (i32 dy = -1; dy <= 1; ++dy)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 px = (i32)floorf(cx) + dx, py = (i32)floorf(cy) + dy;
            if (px < 0 || py < 0 || px >= (i32)f->width || py >= (i32)f->height)
                continue;
            const f32 qx = (f32)px + 0.5f, qy = (f32)py + 0.5f;
            const f32 m0 = inside(X[0], Y[0], X[1], Y[1], qx, qy, sign);
            const f32 m1 = inside(X[1], Y[1], X[2], Y[2], qx, qy, sign);
            const f32 m2 = inside(X[2], Y[2], X[0], Y[0], qx, qy, sign);
            if (m0 >= 0.75f && m1 >= 0.75f && m2 >= 0.75f) {
                *x = (u32)px;
                *y = (u32)py;
                return true;
            }
        }
    return false;
}

/* ================================================================================================
 * GPU tests
 * ============================================================================================== */

/* picking_known_entity_{mesh,compute}_path: three quads; a pick inside a known triangle of each
 * returns that instance's entity bits and (0 << 7) | triangle; 13 more picks at random pixels
 * equal the vis buffer of the same frame decoded on the CPU. Not available after the pick frame
 * (it completes with the next one), available after the next. */
static void known_entity(pkx *f, de_gpu_backend b, bool mesh_path) {
    pkx_open(f, b, mesh_path, 8u);
    const u32 quad = pkx_mesh(f, 1.6f);
    for (u32 s = 0; s < 3; ++s)
        world_set(&f->w, s, quad, (de_vec3){-1.8f + 1.8f * (f32)s, 6.0f, 0.2f * (f32)s}, 1.0f, 1u);
    ASSERT_TRUE(pkx_frame(f)); /* frame 0: the scene uploads */
    u32 tx[3], ty[3], tt[3];
    for (u32 s = 0; s < 3; ++s) {
        bool ok = false;
        for (u32 k = 0; k < PATCH_TRIS && !ok; ++k) {
            tt[s] = (7u * k + 3u * s) % PATCH_TRIS; /* a different triangle per quad */
            ok = triangle_pixel(f, s, tt[s], &tx[s], &ty[s]);
        }
        ASSERT_TRUE(ok);
        de__vis_pick_request(f->vis, (de_view){.id = 1}, tx[s], ty[s], 100u + s);
    }
    u32 rx[13], ry[13];
    u64 seed = 0x9E3779B97F4A7C15ull;
    for (u32 k = 0; k < 13; ++k) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        rx[k] = (u32)(seed >> 33) % f->width;
        ry[k] = (u32)(seed >> 17) % f->height;
        if (k < 4) { /* some at the quads' centres */
            rx[k] = f->width / 2u + (k % 3u) * 37u - 37u;
            ry[k] = f->height / 2u;
        }
        ASSERT_TRUE(de__vis_pick_request(f->vis, (de_view){.id = 1}, rx[k], ry[k], 200u + k));
    }
    EXPECT_FALSE(de__vis_pick_request(f->vis, (de_view){.id = 1}, 0, 0, 999u)); /* the 17th */
    f->readback = true;
    ASSERT_TRUE(pkx_frame(f)); /* frame 1: the picks */
    f->readback = false;
    u64 e = 0;
    u32 t = 0;
    EXPECT_FALSE(de__vis_pick_result(f->vis, 100u, &e, &t)); /* frame 1 is not complete yet */
    ASSERT_TRUE(pkx_frame(f));                               /* frame 2: frame 1 completed */
    for (u32 s = 0; s < 3; ++s) {
        ASSERT_TRUE(de__vis_pick_result(f->vis, 100u + s, &e, &t));
        printf("  quad %u: pixel (%u, %u) -> entity %llx triangle %u (expected %u)\n", s, tx[s],
               ty[s], (unsigned long long)e, t, tt[s]);
        EXPECT_EQ(e, ENTITY_BASE + s);
        EXPECT_EQ(t, tt[s]); /* meshlet 0 */
    }
    u32 hits = 0;
    for (u32 k = 0; k < 13; ++k) {
        u64 want_e = 0;
        u32 want_t = 0;
        decode_readback(f, rx[k], ry[k], &want_e, &want_t);
        ASSERT_TRUE(de__vis_pick_result(f->vis, 200u + k, &e, &t));
        EXPECT_EQ(e, want_e);
        EXPECT_EQ(t, want_t);
        hits += e != 0;
    }
    EXPECT_GE(hits, 4u);                                       /* the centre picks hit */
    EXPECT_FALSE(de__vis_pick_result(f->vis, 999u, &e, &t));   /* dropped */
    EXPECT_FALSE(de__vis_pick_result(f->vis, 12345u, &e, &t)); /* never asked */
    const de__vis_pick_stats st = de__vis_pick_get_stats(f->vis);
    EXPECT_EQ(st.requested, 16u);
    EXPECT_EQ(st.dropped, 1u);
    EXPECT_EQ(st.dispatched, 16u);
    EXPECT_EQ(st.completed, 16u);
    EXPECT_EQ(st.lost, 0u);
}

/* picking_background_zero: empty pixels and pixels outside the view are background (entity 0,
 * DE_PICK_TRIANGLE_NONE); an instance on a layer the view does not draw is not pickable. */
static void background(pkx *f, de_gpu_backend b) {
    pkx_open(f, b, false, 8u);
    const u32 quad = pkx_mesh(f, 1.2f);
    world_set(&f->w, 0, quad, (de_vec3){0.0f, 6.0f, 0.0f}, 1.0f, 1u);
    world_set(&f->w, 1, quad, (de_vec3){-2.0f, 6.0f, 0.0f}, 1.0f, 2u); /* layer 2 */
    ASSERT_TRUE(pkx_frame(f));
    const de_view v = {.id = 1};
    u32 lx = 0, ly = 0, cx = 0, cy = 0;
    ASSERT_TRUE(triangle_pixel(f, 1, 9u, &lx, &ly)); /* inside the layer-2 quad */
    ASSERT_TRUE(triangle_pixel(f, 0, 9u, &cx, &cy));
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, 2, 2, 1u));                          /* corner */
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, f->width - 1u, f->height - 1u, 2u)); /* corner */
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, f->width, 10, 3u));                  /* outside */
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, 10, f->height + 50u, 4u));           /* outside */
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, cx, cy, 5u));                        /* the quad */
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, lx, ly, 6u)); /* layer 2: drawn (mask: all) */
    ASSERT_TRUE(pkx_frame(f));
    f->layer_mask = 1u; /* the view draws layer 1 only */
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, lx, ly, 7u));
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, cx, cy, 8u));
    ASSERT_TRUE(pkx_frame(f));
    ASSERT_TRUE(pkx_frame(f));
    for (u64 id = 1; id <= 4; ++id) {
        u64 e = 77;
        u32 t = 77;
        ASSERT_TRUE(de__vis_pick_result(f->vis, id, &e, &t));
        EXPECT_EQ(e, 0u);
        EXPECT_EQ(t, DE_PICK_TRIANGLE_NONE);
    }
    u64 e = 0;
    u32 t = 0;
    ASSERT_TRUE(de__vis_pick_result(f->vis, 5u, &e, &t));
    EXPECT_EQ(e, ENTITY_BASE);
    EXPECT_EQ(t, 9u);
    ASSERT_TRUE(de__vis_pick_result(f->vis, 6u, &e, &t));
    EXPECT_EQ(e, ENTITY_BASE + 1u);
    ASSERT_TRUE(de__vis_pick_result(f->vis, 7u, &e, &t));
    EXPECT_EQ(e, 0u); /* not drawn: not pickable */
    EXPECT_EQ(t, DE_PICK_TRIANGLE_NONE);
    ASSERT_TRUE(de__vis_pick_result(f->vis, 8u, &e, &t));
    EXPECT_EQ(e, ENTITY_BASE);
    EXPECT_TRUE(de__vis_pick_result(f->vis, 1u, NULL, NULL)); /* NULL outputs */
}

/* picking_latency_window: with frames in flight (frames_completed two behind), a pick of frame N
 * is not available after frames N, N + 1 and N + 2, and is after N + 3 (within three frames). A
 * result is kept 8 frames after it became available, then forgotten. */
static void latency_window(pkx *f, de_gpu_backend b) {
    pkx_open(f, b, false, 8u);
    const u32 quad = pkx_mesh(f, 1.2f);
    world_set(&f->w, 0, quad, (de_vec3){0.0f, 6.0f, 0.0f}, 1.0f, 1u);
    f->lag = 2u;
    ASSERT_TRUE(pkx_frame(f));
    u32 cx = 0, cy = 0;
    ASSERT_TRUE(triangle_pixel(f, 0, 20u, &cx, &cy));
    ASSERT_TRUE(de__vis_pick_request(f->vis, (de_view){.id = 1}, cx, cy, 42u));
    u64 e = 0;
    u32 t = 0;
    u32 frames = 0;
    for (; frames < 6; ++frames) {
        ASSERT_TRUE(pkx_frame(f));
        if (de__vis_pick_result(f->vis, 42u, &e, &t))
            break;
    }
    printf("  available after %u frames (frames_completed 2 behind)\n", frames + 1u);
    EXPECT_EQ(frames + 1u, 4u); /* the pick frame and three more */
    EXPECT_EQ(e, ENTITY_BASE);
    EXPECT_EQ(t, 20u);
    for (u32 k = 0; k < DE__VIS_PICK_KEEP_FRAMES; ++k) {
        ASSERT_TRUE(pkx_frame(f));
        EXPECT_TRUE(de__vis_pick_result(f->vis, 42u, NULL, NULL));
    }
    ASSERT_TRUE(pkx_frame(f));
    EXPECT_FALSE(de__vis_pick_result(f->vis, 42u, NULL, NULL)); /* expired */
    /* No lag: available after the next frame. */
    f->lag = 0u;
    ASSERT_TRUE(de__vis_pick_request(f->vis, (de_view){.id = 1}, cx, cy, 43u));
    ASSERT_TRUE(pkx_frame(f));
    EXPECT_FALSE(de__vis_pick_result(f->vis, 43u, NULL, NULL));
    ASSERT_TRUE(pkx_frame(f));
    EXPECT_TRUE(de__vis_pick_result(f->vis, 43u, &e, NULL));
    EXPECT_EQ(e, ENTITY_BASE);
    EXPECT_EQ(de__vis_pick_get_stats(f->vis).lost, 0u);
}

/* picking_render_scale_mapping: the output is twice the internal size (render_scale 0.5): output
 * pixel (x, y) reads internal pixel (floor((x + 0.5) / 2), floor((y + 0.5) / 2)). At the left edge
 * of a quad: output 2e and 2e + 1 hit (internal e, the first covered pixel), 2e - 1 does not. */
static void render_scale_mapping(pkx *f, de_gpu_backend b) {
    pkx_open(f, b, false, 8u);
    f->width = 128;
    f->height = 80;
    f->out_width = 256;
    f->out_height = 160;
    const u32 quad = pkx_mesh(f, 1.2f);
    world_set(&f->w, 0, quad, (de_vec3){0.0f, 6.0f, 0.0f}, 1.0f, 1u);
    f->readback = true;
    ASSERT_TRUE(pkx_frame(f));
    const u32 y = f->height / 2u;
    u32 edge = 0;
    for (u32 x = 1; x < f->width && edge == 0; ++x)
        if (f->ids[(size_t)y * f->width + x] != DE__VIS_ID_EMPTY &&
            f->ids[(size_t)y * f->width + x - 1u] == DE__VIS_ID_EMPTY)
            edge = x;
    ASSERT_GT(edge, 0u);
    u64 want_e = 0;
    u32 want_t = 0;
    decode_readback(f, edge, y, &want_e, &want_t);
    ASSERT_EQ(want_e, ENTITY_BASE);
    const de_view v = {.id = 1};
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, 2u * edge, 2u * y + 1u, 1u));
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, 2u * edge + 1u, 2u * y, 2u));
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, 2u * edge - 1u, 2u * y + 1u, 3u));
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, 255u, 159u, 4u)); /* last output pixel: inside */
    ASSERT_TRUE(de__vis_pick_request(f->vis, v, 256u, 0u, 5u));   /* outside the output */
    ASSERT_TRUE(pkx_frame(f));
    ASSERT_TRUE(pkx_frame(f));
    u64 e = 0;
    u32 t = 0;
    ASSERT_TRUE(de__vis_pick_result(f->vis, 1u, &e, &t));
    EXPECT_EQ(e, want_e);
    EXPECT_EQ(t, want_t);
    ASSERT_TRUE(de__vis_pick_result(f->vis, 2u, &e, &t));
    EXPECT_EQ(e, want_e);
    EXPECT_EQ(t, want_t);
    ASSERT_TRUE(de__vis_pick_result(f->vis, 3u, &e, &t));
    EXPECT_EQ(e, 0u);
    ASSERT_TRUE(de__vis_pick_result(f->vis, 4u, &e, &t));
    u64 corner_e = 0;
    u32 corner_t = 0;
    decode_readback(f, f->width - 1u, f->height - 1u, &corner_e, &corner_t);
    EXPECT_EQ(e, corner_e);
    ASSERT_TRUE(de__vis_pick_result(f->vis, 5u, &e, &t));
    EXPECT_EQ(e, 0u);
    EXPECT_EQ(t, DE_PICK_TRIANGLE_NONE);
}

#define GPU_TESTS(sfx, backend)                                                                    \
    TEST_F(picking, known_entity_mesh_path_##sfx, pkx) {                                           \
        known_entity(f, backend, true);                                                            \
    }                                                                                              \
    TEST_F(picking, known_entity_compute_path_##sfx, pkx) {                                        \
        known_entity(f, backend, false);                                                           \
    }                                                                                              \
    TEST_F(picking, background_zero_##sfx, pkx) {                                                  \
        background(f, backend);                                                                    \
    }                                                                                              \
    TEST_F(picking, latency_window_##sfx, pkx) {                                                   \
        latency_window(f, backend);                                                                \
    }                                                                                              \
    TEST_F(picking, render_scale_mapping_##sfx, pkx) {                                             \
        render_scale_mapping(f, backend);                                                          \
    }

GPU_TESTS(metal, DE_GPU_METAL)
GPU_TESTS(vulkan, DE_GPU_VULKAN)
GPU_TESTS(d3d12, DE_GPU_D3D12)

/* ================================================================================================
 * Null backend
 * ============================================================================================== */

/* The vis.pick* markers of the last logged frame, comma separated. */
static void pick_markers(de_gpu_device *d, char *out, size_t cap) {
    out[0] = '\0';
    size_t at = 0;
    for (u32 i = 0; i < de__gpu_null_log_count(d); ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(d, i);
        if (e->op == DE__GPU_NULL_OP_MARKER_PUSH && strncmp(e->text, "vis.pick", 8) == 0)
            at += (size_t)snprintf(out + at, cap - at, "%s%s", at ? "," : "", e->text);
    }
}

/* Counts the drop warnings (log sink). */
static de_atomic_u32 g_drop_warnings;
static void drop_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level == DE_LOG_WARN && rec->message.len > 0 &&
        strstr(rec->message.ptr, "de_view_request_pick") != NULL)
        de_atomic_fetch_add_u32(&g_drop_warnings, 1u, DE_ATOMIC_RELAXED);
}

/* picking_request_cap: 16 requests per view and frame are taken; the 17th and later are dropped
 * (one warning per view and frame in Debug and Dev); requesting allocates nothing. The pick
 * passes run only in a frame with requests, after the last raster pass and before classify. */
TEST_F(picking, request_cap, pkx) {
    pkx_open(f, DE_GPU_NULL, true, 8u);
    const u32 quad = pkx_mesh(f, 1.0f);
    world_set(&f->w, 0, quad, (de_vec3){0.0f, 6.0f, 0.0f}, 1.0f, 1u);
    ASSERT_TRUE(pkx_frame(f));
    ASSERT_TRUE(pkx_frame(f)); /* warm: the pick-less graph */

    const bool log_started = de_log_init(&(de_log_desc){.no_console_sink = false}) == DE_OK;
    de_error_clear();
    de_log_add_sink(drop_sink, NULL);
    de_log_flush();
    const u32 warnings_before = de_atomic_load_u32(&g_drop_warnings, DE_ATOMIC_RELAXED);
    /* This thread's log ring is made at its first message (the logger's, once per thread): make
     * it before the guarded region, as any engine thread has. */
    DE_INFO(de_log_register_category("test", DE_LOG_INFO), "picking: request cap");
    const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    u32 taken = 0;
    for (u32 k = 0; k < 20; ++k)
        taken += de__vis_pick_request(f->vis, (de_view){.id = 1}, 10u + k, 10u, 1000u + k);
    u64 e = 0;
    const bool early = de__vis_pick_result(f->vis, 1000u, &e, NULL);
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).alloc_count, before.alloc_count);
    EXPECT_EQ(taken, DE__VIS_PICK_MAX);
    EXPECT_FALSE(early);
    de_log_flush();
    const u32 warnings = de_atomic_load_u32(&g_drop_warnings, DE_ATOMIC_RELAXED) - warnings_before;
    de_log_remove_sink(drop_sink, NULL);
    if (log_started)
        de_log_shutdown();
    de__vis_pick_stats st = de__vis_pick_get_stats(f->vis);
    EXPECT_EQ(st.requested, 16u);
    EXPECT_EQ(st.dropped, 4u);
#if defined(DE_DEBUG) || defined(DE_DEV)
    EXPECT_EQ(st.warnings, 1u); /* once per view and frame */
    EXPECT_EQ(warnings, 1u);
#else
    EXPECT_EQ(st.warnings, 0u);
    EXPECT_EQ(warnings, 0u);
#endif

    /* The frame with requests has the pick passes, after the last raster and before classify. */
    char got[256];
    de__gpu_null_log_enable(f->dev, true);
    ASSERT_TRUE(pkx_frame(f));
    de__gpu_null_log_enable(f->dev, false);
    pick_markers(f->dev, got, sizeof got);
    EXPECT_STREQ(got, "vis.pick.copy,vis.pick,vis.pick.readback");
    bool raster_before = false, pick_seen = false;
    for (u32 i = 0; i < de__gpu_null_log_count(f->dev); ++i) {
        const de__gpu_null_entry *en = de__gpu_null_log_entry(f->dev, i);
        if (en->op != DE__GPU_NULL_OP_MARKER_PUSH)
            continue;
        if (strcmp(en->text, "vis.pick.copy") == 0)
            pick_seen = true;
        if (strncmp(en->text, "vis.raster", 10) == 0 && !pick_seen)
            raster_before = true;
        EXPECT_FALSE(strncmp(en->text, "vis.raster", 10) == 0 && pick_seen); /* none after */
    }
    EXPECT_TRUE(raster_before);
    /* The next frame has none; it collects the 16 (the null device reads back zeros:
     * background). */
    de__gpu_null_log_enable(f->dev, true);
    ASSERT_TRUE(pkx_frame(f));
    de__gpu_null_log_enable(f->dev, false);
    pick_markers(f->dev, got, sizeof got);
    EXPECT_STREQ(got, "");
    for (u32 k = 0; k < 16; ++k) {
        u32 t = 0;
        ASSERT_TRUE(de__vis_pick_result(f->vis, 1000u + k, &e, &t));
        EXPECT_EQ(e, 0u);
        EXPECT_EQ(t, DE_PICK_TRIANGLE_NONE);
    }
    st = de__vis_pick_get_stats(f->vis);
    EXPECT_EQ(st.completed, 16u);
    /* The cap is per frame: 16 more are taken now. */
    taken = 0;
    for (u32 k = 0; k < 16; ++k)
        taken += de__vis_pick_request(f->vis, (de_view){.id = 1}, 1u, 1u, 2000u + k);
    EXPECT_EQ(taken, 16u);
    /* A request for another view in the same slot drops the old view's queue. */
    EXPECT_TRUE(de__vis_pick_request(f->vis, (de_view){.id = (1u << 8) | 1u}, 1u, 1u, 3000u));
    EXPECT_EQ(de__vis_pick_get_stats(f->vis).dropped, 4u + 16u);
}

/* The public API through a renderer (null backend): picks of the main view complete (nothing is
 * loaded, so background); stale and destroyed views; de_view_set_layer_mask reaches the view. */
TEST(picking, public_api_renderer) {
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
    for (u32 i = 0; i < 4; ++i) {
        de_entity e = DE_ENTITY_NULL;
        ASSERT_EQ(de_entity_spawn_with(w, mc, 2, &e), DE_OK);
        DeTransform *t = (DeTransform *)de_entity_get(w, e, bi.transform);
        t->position = (de_dvec3){(f64)i, 0, 0};
        t->rotation = (de_quat){0, 0, 0, 1};
        t->scale = (de_vec3){1, 1, 1};
        ((DeMeshRenderer *)de_entity_get(w, e, ids.mesh_renderer))->mesh.id = 1u;
    }
#define RUN_FRAME()                                                                                \
    do {                                                                                           \
        ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f), DE_OK);   \
        ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f), DE_OK);       \
        ASSERT_EQ(de__render_extract_and_kick(r, w), DE_OK);                                       \
    } while (0)
    RUN_FRAME();
    const de_view mv = de_renderer_main_view(r);
    de_view_set_layer_mask(r, mv, 0x5u);
    de_view_request_pick(r, mv, 3, 4, 7u);
    de_view_request_pick(r, (de_view){0}, 3, 4, 8u);                       /* invalid: ignored */
    const de_view other = de_view_create(r, &(de_view_desc){.name = "b"}); /* no camera */
    ASSERT_TRUE(DE_HANDLE_VALID(other));
    de_view_request_pick(r, other, 1, 1, 9u);
    de_view_destroy(r, other);                 /* its queued request never completes */
    de_view_request_pick(r, other, 1, 1, 10u); /* stale: ignored */
    u64 e = 1;
    u32 t = 0;
    EXPECT_FALSE(de_view_pick_result(r, 7u, &e, &t));
    u32 frames = 0;
    for (; frames < 4 && !de_view_pick_result(r, 7u, &e, &t); ++frames)
        RUN_FRAME();
    printf("  main view pick available after %u frames\n", frames);
    EXPECT_GE(frames, 1u);
    EXPECT_LE(frames, 3u);
    EXPECT_EQ(e, 0u); /* nothing is loaded */
    EXPECT_EQ(t, DE_PICK_TRIANGLE_NONE);
    EXPECT_FALSE(de_view_pick_result(r, 8u, NULL, NULL));
    EXPECT_FALSE(de_view_pick_result(r, 9u, NULL, NULL));
    EXPECT_FALSE(de_view_pick_result(r, 10u, NULL, NULL));
    EXPECT_FALSE(de_view_pick_result(NULL, 7u, NULL, NULL));
    de__view_info info;
    ASSERT_TRUE(de__view_info_get(r, mv, &info));
    EXPECT_EQ(info.layer_mask, 0x5u);
    const de__vis_pick_stats st = de__vis_pick_get_stats(de__views_vis(r));
    EXPECT_EQ(st.requested, 2u); /* 7 and 9 */
    de_view_set_layer_mask(r, mv, 0u);
    RUN_FRAME();
    ASSERT_TRUE(de__view_info_get(r, mv, &info));
    EXPECT_EQ(info.layer_mask, 0u);
#undef RUN_FRAME
    (void)de_cvar_set(pipelined, "1");
    de_renderer_destroy(r);
    de_world_destroy(w);
    de_gpu_device_destroy(d);
    de_type_registry_destroy(types);
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}

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
    char *s = (char *)zalloc((size_t)n + 1u);
    const size_t got = fread(s, 1, (size_t)n, fp);
    s[got] = '\0';
    fclose(fp);
    return s;
}

/* pick.slang's constants and PickParams (12 uints, then uint4 coords[4]) against picking.c. */
TEST(picking, layout_match) {
    char path[1024];
    snprintf(path, sizeof path, "%s/visbuffer/pick.slang", DE_VIS_TEST_SHADER_DIR);
    char *text = read_text(path);
    ASSERT_TRUE(text != NULL);
    u32 count = 0, found = 0;
    const de__vis_layout_const *k = de__vis_pick_layout_consts(&count);
    for (const char *at = text; (at = strstr(at, "public static const uint ")) != NULL;) {
        at += strlen("public static const uint ");
        char name[96];
        long long value = 0;
        ASSERT_EQ(sscanf(at, "%95[A-Za-z0-9_] = %lli", name, &value), 2);
        bool ok = false;
        for (u32 i = 0; i < count; ++i)
            if (strcmp(k[i].name, name) == 0) {
                ok = true;
                EXPECT_EQ((u32)value, k[i].value);
            }
        EXPECT_TRUE(ok);
        ++found;
    }
    EXPECT_EQ(found, count);
    const char *ps = strstr(text, "struct PickParams");
    const char *pe = ps != NULL ? strstr(ps, "};") : NULL;
    ASSERT_TRUE(ps != NULL && pe != NULL);
    u32 fields = 0;
    for (const char *ln = ps; ln != NULL && ln < pe; ln = strchr(ln + 1, '\n'))
        fields += strncmp(ln, "\n    uint ", 10) == 0;
    EXPECT_EQ(fields, 12u);
    const char *coords = strstr(ps, "uint4 coords[4];");
    EXPECT_TRUE(coords != NULL && coords < pe);
    EXPECT_EQ(DE__VIS_PICK_MAX, 16u);
    free(text);
}
