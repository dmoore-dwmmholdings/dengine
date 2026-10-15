/*
 * occlusion_fixture.c: see occlusion_fixture.h (DENG-233).
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "occlusion_fixture.h"

#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_platform.h>

#include "render/graph/rg_internal.h"
#include "render/loaders/render_assets.h"
#include "render/scene/gpu_scene.h"

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
#define NEAR_Z  0.1f

/* ---------- The fake mesh: a unit cube, one LOD, one meshlet ---------- */

static de_render_mesh g_cube;

static void cube_init(void) {
    memset(&g_cube, 0, sizeof g_cube);
    for (u32 c = 0; c < 3; ++c) {
        g_cube.bounds_min[c] = -1.0f;
        g_cube.bounds_max[c] = 1.0f;
        g_cube.position_scale[c] = 1.0f;
    }
    g_cube.sphere[3] = 1.7320508f;
    for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s)
        g_cube.stream_offset[s] = UINT32_MAX;
    g_cube.submesh_count = 1;
    g_cube.lod_count = 1;
    g_cube.lods[0] = (de_cooked_mesh_lod){
        .index_count = 36, .meshlet_count = 1, .triangle_count = 12, .vertex_count = 8};
    g_cube.meshlet_count = 1;
    g_cube.vertex_count = 8;
    g_cube.index_count = 36;
}

static const de_render_mesh *fake_mesh(void *user, de_mesh m) {
    (void)user;
    return m.id == 1u ? &g_cube : NULL;
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

static void *zalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    ASSERT_TRUE(p != NULL);
    return p;
}

/* ---------- Setup ---------- */

void ocx_setup(ocx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "occlusion_test"}, &f->arena), DE_OK);
    de_error_clear();
    cube_init();
    f->eye = (de_vec3){0.0f, 0.0f, 0.0f};
    f->target = (de_vec3){0.0f, 1.0f, 0.0f};
    f->fov = 1.0f;
    f->width = 320;
    f->height = 180;
    f->view_id = 1;
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

void ocx_teardown(ocx *f) {
    if (f->g) {
        de__rg_reset(f->g);
        de__rg_destroy(f->g);
    }
    for (u32 i = 0; i < f->keep_graph_count; ++i) {
        de__rg_reset(f->keep_graph[i]);
        de__rg_destroy(f->keep_graph[i]);
    }
    if (f->dev)
        de_gpu_wait_idle(f->dev);
    de__vis_shared_destroy(f->vis);
    de__gpu_scene_destroy(f->gs);
    if (f->dev) {
        de_gpu_wait_idle(f->dev);
        if (f->real_gpu)
            EXPECT_EQ(validation_errors(f->dev), 0u);
        for (u32 i = 0; i < f->keep_tex_count; ++i)
            de_gpu_texture_destroy(f->dev, f->keep_tex[i]);
        for (u32 i = 0; i < f->keep_buf_count; ++i)
            de_gpu_buffer_destroy(f->dev, f->keep_buf[i]);
        if (DE_HANDLE_VALID(f->rb))
            de_gpu_buffer_destroy(f->dev, f->rb);
        if (DE_HANDLE_VALID(f->depth_upload))
            de_gpu_buffer_destroy(f->dev, f->depth_upload);
        if (DE_HANDLE_VALID(f->depth_tex))
            de_gpu_texture_destroy(f->dev, f->depth_tex);
        if (DE_HANDLE_VALID(f->fence))
            de_gpu_fence_destroy(f->dev, f->fence);
        de_gpu_wait_idle(f->dev);
        de_gpu_device_destroy(f->dev);
    }
    de__rw_instances *in = &f->w.instances;
    free(in->entity);
    free(in->mesh);
    free(in->materials);
    free(in->transform);
    free(in->prev_transform);
    free(in->bounds);
    free(in->layer_mask);
    free(in->flags);
    free(f->w.dirty_instances);
    free(f->w.removed_instances);
    free(f->w.dirty_lights);
    free(f->w.removed_lights);
    free(f->dirty_mark);
    free(f->depth_cpu);
    free(f->id_cpu);
    free(f->items[0]);
    free(f->items[1]);
    free(f->visibility);
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
    /* The cvars are process-wide: back to their defaults. */
    (void)ocx_cvar("r.occlusion", "1");
    (void)ocx_cvar("r.occlusion_reset", "0");
    (void)ocx_cvar("r.hzb_single_pass", "1");
}

void ocx_keep_texture(ocx *f, de_gpu_texture t) {
    ASSERT_TRUE(f->keep_tex_count < 64u);
    f->keep_tex[f->keep_tex_count++] = t;
}

void ocx_keep_buffer(ocx *f, de_gpu_buffer b) {
    ASSERT_TRUE(f->keep_buf_count < 64u);
    f->keep_buf[f->keep_buf_count++] = b;
}

de_rg *ocx_graph(ocx *f, const char *name) {
    de_rg *g = NULL;
    ASSERT_TRUE(f->keep_graph_count < 16u);
    ASSERT_EQ(
        de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = name}, &g),
        DE_OK);
    f->keep_graph[f->keep_graph_count++] = g;
    return g;
}

bool ocx_cvar(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (!DE_HANDLE_VALID(cv))
        return false;
    return de_cvar_set(cv, value) == DE_OK;
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

void ocx_open(ocx *f, de_gpu_backend backend, u32 instances) {
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
            snprintf(why, sizeof why, "below the Base tier: %.400s", caps->base_tier_reason);
            SKIP(why);
        }
    }
    ASSERT_EQ(
        de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "occlusion"},
                      &f->g),
        DE_OK);
    f->fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(f->fence));
    de__gpu_scene_desc gd = {.device = f->dev,
                             .allocator = &f->alloc,
                             .resolver = {.mesh = fake_mesh,
                                          .material = fake_material,
                                          .texture_bindless = fake_texture},
                             .staging_bytes = (u64)32 << 20};
    gd.initial_capacity[DE__GS_INSTANCES] = instances;
    ASSERT_EQ(de__gpu_scene_create(&gd, &f->gs), DE_OK);
    ASSERT_EQ(de__vis_shared_create(
                  &(de__vis_shared_desc){.device = f->dev, .allocator = &f->alloc}, &f->vis),
              DE_OK);
    if (!f->vis->enabled) {
        snprintf(why, sizeof why, "the vis frame is off: %.600s", f->vis->why);
        SKIP(why);
    }
    /* The fake render world (OCX_MAX_INSTANCES slots at most). */
    const u32 cap = OCX_MAX_INSTANCES;
    f->cap = cap;
    de__rw_instances *in = &f->w.instances;
    in->entity = (u64 *)zalloc(cap * sizeof(u64));
    in->mesh = (de_mesh *)zalloc(cap * sizeof(de_mesh));
    in->materials = (de_material *)zalloc((size_t)cap * DE__RW_MATERIALS * sizeof(de_material));
    in->transform = (de_affine *)zalloc(cap * sizeof(de_affine));
    in->prev_transform = (de_affine *)zalloc(cap * sizeof(de_affine));
    in->bounds = (de_aabb *)zalloc(cap * sizeof(de_aabb));
    in->layer_mask = (u32 *)zalloc(cap * sizeof(u32));
    in->flags = (u8 *)zalloc(cap);
    f->w.dirty_instances = (u32 *)zalloc(cap * sizeof(u32));
    f->w.removed_instances = (u32 *)zalloc(sizeof(u32));
    f->w.dirty_lights = (u32 *)zalloc(sizeof(u32));
    f->w.removed_lights = (u32 *)zalloc(sizeof(u32));
    f->w.origin = (de_dvec3){4096.0, -1024.0, 0.0};
    f->dirty_mark = (u32 *)zalloc(cap * sizeof(u32));
    /* The phase-1 depth texture and its upload buffer. */
    f->depth_cpu = (f32 *)zalloc((size_t)f->width * f->height * sizeof(f32));
    f->id_cpu = (u32 *)zalloc((size_t)f->width * f->height * sizeof(u32));
    f->depth_pitch = (f->width * 4u + 255u) & ~255u;
    f->depth_tex = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                                       .width = f->width,
                                       .height = f->height,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                       .name = "test phase-1 depth"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->depth_tex));
    f->depth_upload =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = (u64)f->depth_pitch * f->height,
                                                           .usage = DE_USAGE_COPY_SRC,
                                                           .memory = DE_MEM_UPLOAD,
                                                           .name = "test phase-1 depth upload"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->depth_upload));
}

/* ---------- The world ---------- */

void ocx_set_box(ocx *f, u32 s, de_vec3 center, de_vec3 half) {
    ASSERT_TRUE(s < f->cap);
    de__rw_instances *in = &f->w.instances;
    const bool live = half.x > 0.0f;
    const de_affine xf = de_affine_from_trs(center, (de_quat){0, 0, 0, 1}, half);
    in->entity[s] = live ? 100u + s : 0u;
    in->mesh[s].id = live ? 1u : 0u;
    in->transform[s] = xf;
    in->prev_transform[s] = xf;
    in->layer_mask[s] = 1u;
    in->bounds[s] = live ? (de_aabb){{center.x - half.x, center.y - half.y, center.z - half.z},
                                     {center.x + half.x, center.y + half.y, center.z + half.z}}
                         : de_aabb_empty();
    in->flags[s] = live ? DE__RW_INST_HAS_BOUNDS : 0u;
    if (s >= in->count)
        in->count = s + 1u;
    if (!f->dirty_mark[s]) {
        f->dirty_mark[s] = 1u;
        f->w.dirty_instances[f->w.dirty_instance_count++] = s;
    }
}

de_aabb ocx_box(const ocx *f, u32 s) {
    return f->w.instances.bounds[s];
}

static int cmp_u32(const void *a, const void *b) {
    const u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* ---------- CPU views of the frame ---------- */

static de_mat4 view_matrix(const ocx *f) {
    return de_m4_look_at(f->eye, f->target, (de_vec3){0.0f, 0.0f, 1.0f});
}
static de_mat4 proj_matrix(const ocx *f) {
    return de_m4_perspective_rz(f->fov, (f32)f->width / (f32)f->height, NEAR_Z);
}

de__vis_cull_view ocx_cpu_view(const ocx *f) {
    const de_mat4 view = view_matrix(f), proj = proj_matrix(f);
    de__vis_cull_view v;
    de__vis_cull_view_init(&v, &(de__vis_cull_camera){.view_proj = de_m4_mul(&proj, &view),
                                                      .proj = proj,
                                                      .position = f->eye,
                                                      .height = f->height,
                                                      .lod_error_px = 1.0f});
    return v;
}

bool ocx_in_frustum(const ocx *f, const de__vis_cull_view *v, u32 s) {
    const de_aabb b = f->w.instances.bounds[s];
    if (f->w.instances.entity[s] == 0)
        return false;
    const f32 hx = 0.5f * (b.max.x - b.min.x), hy = 0.5f * (b.max.y - b.min.y),
              hz = 0.5f * (b.max.z - b.min.z);
    const f32 c[3] = {b.min.x + hx, b.min.y + hy, b.min.z + hz};
    const f32 mn[3] = {b.min.x, b.min.y, b.min.z}, mx[3] = {b.max.x, b.max.y, b.max.z};
    return de__vis_cull_sphere(v, c, sqrtf(hx * hx + hy * hy + hz * hz)) &&
           de__vis_cull_aabb(v, mn, mx);
}

static de_vec3 v3_norm(de_vec3 a) {
    const f32 l = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
    return (de_vec3){a.x / l, a.y / l, a.z / l};
}
static de_vec3 v3_cross(de_vec3 a, de_vec3 b) {
    return (de_vec3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/* Slab test: the entry distance of the ray (o, d) into box b, or a negative value. */
static f32 ray_box(de_vec3 o, de_vec3 d, de_aabb b) {
    const f32 oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    const f32 lo[3] = {b.min.x, b.min.y, b.min.z}, hi[3] = {b.max.x, b.max.y, b.max.z};
    f32 t0 = 0.0f, t1 = 1e30f;
    for (u32 k = 0; k < 3; ++k) {
        if (fabsf(dd[k]) < 1e-12f) {
            if (oo[k] < lo[k] || oo[k] > hi[k])
                return -1.0f;
            continue;
        }
        f32 a = (lo[k] - oo[k]) / dd[k], c = (hi[k] - oo[k]) / dd[k];
        if (a > c) {
            const f32 t = a;
            a = c;
            c = t;
        }
        t0 = a > t0 ? a : t0;
        t1 = c < t1 ? c : t1;
        if (t0 > t1)
            return -1.0f;
    }
    return t0;
}

void ocx_raycast(ocx *f, const bool *draw) {
    const de_vec3 fwd =
        v3_norm((de_vec3){f->target.x - f->eye.x, f->target.y - f->eye.y, f->target.z - f->eye.z});
    const de_vec3 right = v3_norm(v3_cross(fwd, (de_vec3){0.0f, 0.0f, 1.0f}));
    const de_vec3 up = v3_cross(right, fwd);
    const f32 th = tanf(0.5f * f->fov), aspect = (f32)f->width / (f32)f->height;
    const u32 n = f->w.instances.count;
    for (u32 py = 0; py < f->height; ++py) {
        for (u32 px = 0; px < f->width; ++px) {
            const f32 nx = ((f32)px + 0.5f) / (f32)f->width * 2.0f - 1.0f;
            const f32 ny = 1.0f - ((f32)py + 0.5f) / (f32)f->height * 2.0f;
            const de_vec3 d = {fwd.x + right.x * nx * th * aspect + up.x * ny * th,
                               fwd.y + right.y * nx * th * aspect + up.y * ny * th,
                               fwd.z + right.z * nx * th * aspect + up.z * ny * th};
            f32 best = 1e30f;
            u32 id = UINT32_MAX;
            for (u32 s = 0; s < n; ++s) {
                if (f->w.instances.entity[s] == 0 || (draw != NULL && !draw[s]))
                    continue;
                const f32 t = ray_box(f->eye, d, f->w.instances.bounds[s]);
                if (t >= 0.0f && t < best) {
                    best = t;
                    id = s;
                }
            }
            f32 depth = 0.0f;
            if (id != UINT32_MAX) {
                /* Distance along the view axis: d has a unit forward component. */
                const f32 z = best;
                depth = z > NEAR_Z ? NEAR_Z / z : 1.0f;
            }
            f->depth_cpu[py * f->width + px] = depth;
            f->id_cpu[py * f->width + px] = id;
        }
    }
}

/* ---------- One frame ---------- */

typedef struct rb_pass {
    ocx *f;
    de_rg_resource items[2], counters[2], visibility;
    u64 items_bytes, vis_bytes;
} rb_pass;

#define RB_COUNTERS 0u
#define RB_VIS      512u

static void readback_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    for (u32 k = 0; k < 2; ++k)
        de_gpu_cmd_copy_buffer(cmd, p->f->rb, RB_COUNTERS + 16u * k,
                               de_rg_buffer(pass, p->counters[k]), 0, 16);
    de_gpu_cmd_copy_buffer(cmd, p->f->rb, RB_VIS, de_rg_buffer(pass, p->visibility), 0,
                           p->vis_bytes);
    const u64 items0 = (RB_VIS + p->vis_bytes + 255u) & ~(u64)255u;
    for (u32 k = 0; k < 2; ++k)
        de_gpu_cmd_copy_buffer(cmd, p->f->rb, items0 + k * p->items_bytes,
                               de_rg_buffer(pass, p->items[k]), 0, p->items_bytes);
}

typedef struct upload_pass {
    ocx *f;
    de_rg_resource depth;
} upload_pass;

static void upload_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const upload_pass *p = (const upload_pass *)user;
    de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, p->depth), 0, 0, p->f->depth_upload,
                                      0, p->f->depth_pitch);
}

static void ensure_rb(ocx *f, u64 bytes) {
    if (bytes <= f->rb_size)
        return;
    if (DE_HANDLE_VALID(f->rb)) {
        de_gpu_wait_idle(f->dev);
        de_gpu_buffer_destroy(f->dev, f->rb);
    }
    f->rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = bytes,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK,
                                                               .name = "occlusion test readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->rb));
    f->rb_size = bytes;
}

bool ocx_frame(ocx *f) {
    const u64 n = f->frames++;
    /* The world: this frame's buffer (the first frame is an origin change: everything). */
    de__render_world_frame *w = &f->w;
    w->frame++;
    w->origin_changed = w->frame == 1;
    qsort(w->dirty_instances, w->dirty_instance_count, sizeof(u32), cmp_u32);
    w->instances.live = 0;
    for (u32 s = 0; s < w->instances.count; ++s)
        w->instances.live += w->instances.entity[s] != 0;
    if (de__gpu_scene_update(f->gs, w) != DE_OK)
        return false;
    for (u32 i = 0; i < w->dirty_instance_count; ++i)
        f->dirty_mark[w->dirty_instances[i]] = 0;
    w->dirty_instance_count = 0;

    /* The phase-1 depth into its upload buffer. */
    u8 *up = (u8 *)de_gpu_buffer_map(f->dev, f->depth_upload);
    for (u32 y = 0; y < f->height; ++y)
        memcpy(up + (size_t)y * f->depth_pitch, f->depth_cpu + (size_t)y * f->width,
               (size_t)f->width * sizeof(f32));

    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    if (de__rg_begin(f->g, f->arena) != DE_OK || de__gpu_scene_add_passes(f->gs, f->g) != DE_OK)
        return false;
    upload_pass *u = (upload_pass *)de_arena_push_zero(f->arena, sizeof *u, 16);
    u->f = f;
    u->depth = de_rg_import_texture(f->g, f->depth_tex, f->depth_access);
    de_rg_pass *pu =
        de_rg_add_pass(f->g, "test.depth_upload", DE_QUEUE_GRAPHICS, upload_execute, u);
    de_rg_write(pu, u->depth, DE_ACCESS_COPY_DST);
    de_rg_export(f->g, u->depth, DE_ACCESS_SHADER_READ);

    const de__vis_frame_desc d = {.gpu_scene = f->gs,
                                  .view = (de_view){.id = f->view_id},
                                  .view_slot = f->view_slot,
                                  .frame_index = n,
                                  .frames_completed = n,
                                  .width = f->width,
                                  .height = f->height,
                                  .view_matrix = view_matrix(f),
                                  .proj = proj_matrix(f),
                                  .camera_position = f->eye,
                                  .origin = w->origin,
                                  .depth_override = u->depth,
                                  .depth_override_width = f->width,
                                  .depth_override_height = f->height};
    de__vis_context *vc = NULL;
    if (de__vis_frame_build(f->vis, f->g, &d, &vc) != DE_OK || vc == NULL)
        return false;
    f->cv = vc->cull;
    f->hzb_width = vc->hzb_width;
    f->hzb_height = vc->hzb_height;
    f->hzb_mips = vc->hzb_mips;
    const u32 count = vc->instance_count;
    rb_pass *rp = NULL;
    if (DE_HANDLE_VALID(vc->work_items[0]) && DE_HANDLE_VALID(vc->work_items[1]) &&
        DE_HANDLE_VALID(vc->visibility)) {
        rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
        rp->f = f;
        rp->items_bytes = (u64)vc->work_capacity * sizeof(de__vis_work_item);
        rp->vis_bytes = (u64)count * 4u;
        ensure_rb(f, ((RB_VIS + rp->vis_bytes + 255u) & ~(u64)255u) + 2u * rp->items_bytes);
        de_rg_pass *p =
            de_rg_add_pass(f->g, "test.readback", DE_QUEUE_GRAPHICS, readback_execute, rp);
        for (u32 k = 0; k < 2; ++k) {
            rp->items[k] = vc->work_items[k];
            rp->counters[k] = vc->instance_counters[k];
            de_rg_read(p, rp->items[k], DE_ACCESS_COPY_SRC);
            de_rg_read(p, rp->counters[k], DE_ACCESS_COPY_SRC);
        }
        rp->visibility = vc->visibility;
        de_rg_read(p, rp->visibility, DE_ACCESS_COPY_SRC);
        de_rg_write(p, de_rg_import_buffer(f->g, f->rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
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
    f->depth_access = DE_ACCESS_SHADER_READ;
    if (de_gpu_fence_wait(f->dev, f->fence, n + 1, WAIT_NS) != DE_OK)
        return false;
    if (rp == NULL)
        return true;
    const u8 *m = (const u8 *)de_gpu_buffer_map(f->dev, f->rb);
    memcpy(f->counters, m + RB_COUNTERS, sizeof f->counters);
    free(f->visibility);
    f->visibility = (u32 *)zalloc((size_t)count * 4u);
    memcpy(f->visibility, m + RB_VIS, (size_t)count * 4u);
    f->visibility_count = count;
    const u64 items0 = (RB_VIS + rp->vis_bytes + 255u) & ~(u64)255u;
    for (u32 k = 0; k < 2; ++k) {
        const u32 asked = f->counters[k][DE__VIS_COUNTER_WORK_ITEMS / 4u];
        const u32 stored = asked < vc->work_capacity ? asked : vc->work_capacity;
        if (stored > f->item_cap[k]) {
            free(f->items[k]);
            f->items[k] = (de__vis_work_item *)zalloc(stored * sizeof(de__vis_work_item));
            f->item_cap[k] = stored;
        }
        f->item_count[k] = stored;
        if (stored > 0)
            memcpy(f->items[k], m + items0 + k * rp->items_bytes,
                   stored * sizeof(de__vis_work_item));
    }
    return true;
}

bool ocx_drawn(const ocx *f, u32 phase_index, u32 s) {
    for (u32 i = 0; i < f->item_count[phase_index]; ++i)
        if (f->items[phase_index][i].instance == s)
            return true;
    return false;
}
