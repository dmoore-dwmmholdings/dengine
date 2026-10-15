/*
 * test_meshlet_cull.c: GPU meshlet culling (DENG-231): frustum, backface-cone and small-meshlet
 * tests, the visible-meshlet list and the raster arguments of both phases.
 * Executable test_render_meshlet_cull; CTest names render_meshlet_cull.<suite>.<test>.
 *
 * Meshes are built here from real triangles: each meshlet is a small, possibly bent grid patch
 * (or a soup of random triangles, which has no cone), and its bounds come from
 * meshopt_computeMeshletBounds with the sphere widened like the cooker's (tools/cook/importers/
 * model/mesh_meshlets.c), written as DMSH v2 meshlet records into a buffer the GPU scene names.
 * A fake render world drives a GPU scene (DENG-177); the vis frame (src/render/passes/vis/) runs
 * instance culling (DENG-229) and meshlet culling, and a test pass reads back the work items, the
 * visible list, the counters and the raster arguments.
 *
 * Each GPU test compares the visible list, meshlet by meshlet, with the CPU reference of
 * vis_stages.h (de__vis_meshlet_test) on the entries the GPU read (de__gpu_scene_debug_pack) and
 * the cull constants the frame uploaded. A meshlet whose CPU decision lies within a float
 * tolerance of a threshold ("marginal") may go either way; every other must match exactly. The
 * cone and small-meshlet tests are also checked against the triangles themselves: a meshlet the
 * cone culled has no front-facing triangle, and a meshlet the small test culled covers no pixel
 * centre of a CPU rasterization.
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
#include "render/loaders/render_assets.h"
#include "render/passes/vis/vis_stages.h"
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
#include <meshoptimizer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS (10ull * 1000000000ull)

static void *zalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    ASSERT_TRUE(p != NULL);
    return p;
}

/* ================================================================================================
 * Random numbers
 * ============================================================================================== */

typedef struct rng {
    u64 s;
} rng;

static u32 rnd(rng *g) {
    g->s ^= g->s << 13;
    g->s ^= g->s >> 7;
    g->s ^= g->s << 17;
    return (u32)(g->s >> 11);
}
/* Uniform in [lo, hi). */
static f32 rndf(rng *g, f32 lo, f32 hi) {
    return lo + (hi - lo) * (f32)(rnd(g) % 1000000u) / 1000000.0f;
}
static de_vec3 rnd_unit(rng *g) {
    for (;;) {
        const de_vec3 v = {rndf(g, -1, 1), rndf(g, -1, 1), rndf(g, -1, 1)};
        const f32 l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        if (l > 0.05f && l <= 1.0f)
            return (de_vec3){v.x / l, v.y / l, v.z / l};
    }
}

/* ================================================================================================
 * Meshes with real meshlets
 * ============================================================================================== */

#define GRID       5u                               /* grid patch: 5 x 5 vertices */
#define PATCH_TRIS (2u * (GRID - 1u) * (GRID - 1u)) /* 32 */
#define SOUP_TRIS  16u
#define MAX_VERTS  (3u * SOUP_TRIS) /* per meshlet, at most */
#define MAX_TRIS   PATCH_TRIS
#define MAX_MESHES 8u
#define MAT_SINGLE 1u /* material handle ids of the fake materials */
#define MAT_DOUBLE 2u

typedef struct mesh_style {
    u32 meshlets;
    f32 half;                 /* meshlet patches lie in [-half, half]^3 */
    f32 patch_min, patch_max; /* patch edge length */
    f32 bend;                 /* curvature: up to bend / size */
    u32 soup_percent;         /* meshlets of random triangles (no cone) */
    bool facing_y;            /* every patch flat and facing +y */
    u32 submeshes;
} mesh_style;

typedef struct fmesh {
    de_render_mesh rm;
    u32 meshlets;
    u8 *records;    /* meshlets x DE_COOKED_MESHLET_SIZE */
    f32 *pos;       /* object-space vertices */
    u32 *tri;       /* per meshlet MAX_TRIS x 3 vertex indices */
    u32 *tri_count; /* per meshlet */
    de_aabb bounds;
    de_gpu_buffer buf;
} fmesh;

static fmesh g_mesh[MAX_MESHES];
static u32 g_mesh_count;
static de_render_material g_mat[2];

static const de_render_mesh *fake_mesh(void *user, de_mesh m) {
    (void)user;
    return m.id >= 1u && m.id <= g_mesh_count ? &g_mesh[m.id - 1u].rm : NULL;
}
static const de_render_material *fake_material(void *user, de_material m) {
    (void)user;
    return m.id == MAT_SINGLE || m.id == MAT_DOUBLE ? &g_mat[m.id - 1u] : NULL;
}
static u32 fake_texture(void *user, de_texture t) {
    (void)user;
    (void)t;
    return 0;
}

static void put_u32(u8 *p, u32 v) {
    memcpy(p, &v, 4); /* the hosts are little-endian (de_cooked_le.h) */
}
static void put_f32(u8 *p, f32 v) {
    memcpy(p, &v, 4);
}

/* tools/cook/importers/model/mesh_meshlets.c contain_radius: every vertex inside, with a margin. */
static f32 contain_radius(const f32 c[3], f32 radius, const u32 *verts, u32 count, const f32 *pos) {
    f64 r2 = (f64)radius * radius, mag = 0.0;
    for (u32 k = 0; k < 3; ++k)
        mag = fabs((f64)c[k]) > mag ? fabs((f64)c[k]) : mag;
    for (u32 i = 0; i < count; ++i) {
        const f32 *p = pos + 3u * (u64)verts[i];
        f64 d2 = 0.0;
        for (u32 k = 0; k < 3; ++k) {
            const f64 d = (f64)p[k] - c[k];
            d2 += d * d;
            mag = fabs((f64)p[k]) > mag ? fabs((f64)p[k]) : mag;
        }
        r2 = d2 > r2 ? d2 : r2;
    }
    return (f32)(sqrt(r2) * (1.0 + 1e-6) + mag * 1e-6);
}

/* Builds mesh `fm` (CPU data only; mesh_upload makes its GPU buffer). */
static void mesh_build(fmesh *fm, const mesh_style *st, rng *g) {
    memset(fm, 0, sizeof *fm);
    fm->meshlets = st->meshlets;
    fm->records = (u8 *)zalloc((size_t)st->meshlets * DE_COOKED_MESHLET_SIZE);
    fm->pos = (f32 *)zalloc((size_t)st->meshlets * MAX_VERTS * 3u * sizeof(f32));
    fm->tri = (u32 *)zalloc((size_t)st->meshlets * MAX_TRIS * 3u * sizeof(u32));
    fm->tri_count = (u32 *)zalloc((size_t)st->meshlets * sizeof(u32));
    fm->bounds = de_aabb_empty();
    u32 nv = 0;
    for (u32 ml = 0; ml < st->meshlets; ++ml) {
        const f32 s = rndf(g, st->patch_min, st->patch_max);
        const f32 lim = st->half - s;
        const de_vec3 p = {rndf(g, -lim, lim), rndf(g, -lim, lim), rndf(g, -lim, lim)};
        const bool soup = !st->facing_y && rnd(g) % 100u < st->soup_percent;
        const u32 base = nv;
        u32 verts[MAX_VERTS];
        u8 local[MAX_TRIS * 3u];
        u32 nt = 0;
        if (soup) {
            for (u32 i = 0; i < 3u * SOUP_TRIS; ++i) {
                f32 *q = fm->pos + 3u * (size_t)nv++;
                q[0] = p.x + rndf(g, -0.5f, 0.5f) * s;
                q[1] = p.y + rndf(g, -0.5f, 0.5f) * s;
                q[2] = p.z + rndf(g, -0.5f, 0.5f) * s;
            }
            for (u32 t = 0; t < SOUP_TRIS; ++t, ++nt)
                for (u32 k = 0; k < 3; ++k)
                    local[3u * nt + k] = (u8)(3u * t + k);
        } else {
            const de_vec3 n = st->facing_y ? (de_vec3){0, 1, 0} : rnd_unit(g);
            const de_vec3 a = fabsf(n.z) < 0.9f ? (de_vec3){0, 0, 1} : (de_vec3){1, 0, 0};
            de_vec3 t = de_v3_cross(a, n);
            t = de_v3_scale(t, 1.0f / de_v3_length(t));
            const de_vec3 b = de_v3_cross(n, t); /* t x b = n */
            const f32 k = st->facing_y ? 0.0f : rndf(g, -st->bend, st->bend) / s;
            for (u32 j = 0; j < GRID; ++j)
                for (u32 i = 0; i < GRID; ++i) {
                    const f32 x = ((f32)i / (f32)(GRID - 1u) - 0.5f) * s;
                    const f32 y = ((f32)j / (f32)(GRID - 1u) - 0.5f) * s;
                    const f32 z = k * (x * x + y * y);
                    f32 *q = fm->pos + 3u * (size_t)nv++;
                    q[0] = p.x + x * t.x + y * b.x + z * n.x;
                    q[1] = p.y + x * t.y + y * b.y + z * n.y;
                    q[2] = p.z + x * t.z + y * b.z + z * n.z;
                }
            for (u32 j = 0; j + 1u < GRID; ++j)
                for (u32 i = 0; i + 1u < GRID; ++i) {
                    const u8 v00 = (u8)(j * GRID + i), v10 = (u8)(v00 + 1u);
                    const u8 v11 = (u8)(v10 + GRID), v01 = (u8)(v00 + GRID);
                    const u8 tris[6] = {v00, v10, v11,
                                        v00, v11, v01}; /* counter-clockwise from +n */
                    memcpy(local + 3u * nt, tris, 6);
                    nt += 2u;
                }
        }
        const u32 count = nv - base;
        for (u32 i = 0; i < count; ++i) {
            verts[i] = base + i;
            const f32 *q = fm->pos + 3u * (size_t)(base + i);
            fm->bounds.min = de_v3_min(fm->bounds.min, (de_vec3){q[0], q[1], q[2]});
            fm->bounds.max = de_v3_max(fm->bounds.max, (de_vec3){q[0], q[1], q[2]});
        }
        fm->tri_count[ml] = nt;
        for (u32 i = 0; i < 3u * nt; ++i)
            fm->tri[(size_t)ml * MAX_TRIS * 3u + i] = base + local[i];
        const struct meshopt_Bounds bo =
            meshopt_computeMeshletBounds(verts, local, nt, fm->pos, nv, 12u);
        const f32 radius = contain_radius(bo.center, bo.radius, verts, count, fm->pos);
        u8 *r = fm->records + (size_t)ml * DE_COOKED_MESHLET_SIZE;
        put_u32(r + 0, base);
        put_u32(r + 4, 0);
        const u32 sub = st->submeshes > 1u ? ml % st->submeshes : 0u;
        put_u32(r + 8, count | (nt << 8) | (sub << 16));
        r[12] = (u8)bo.cone_axis_s8[0];
        r[13] = (u8)bo.cone_axis_s8[1];
        r[14] = (u8)bo.cone_axis_s8[2];
        r[15] = (u8)bo.cone_cutoff_s8;
        for (u32 c = 0; c < 3; ++c) {
            put_f32(r + 16 + 4 * c, bo.center[c]);
            put_f32(r + 32 + 4 * c, bo.cone_apex[c]);
            put_f32(r + 48 + 4 * c, bo.cone_axis[c]);
        }
        put_f32(r + 28, radius);
        put_f32(r + 44, bo.cone_cutoff);
        put_f32(r + 60, 0.0f);
    }
    de_render_mesh *m = &fm->rm;
    for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s)
        m->stream_offset[s] = UINT32_MAX;
    m->vertex_count = nv;
    m->meshlet_count = st->meshlets;
    m->submesh_count = st->submeshes > 1u ? st->submeshes : 1u;
    m->lod_count = 1;
    m->lods[0] = (de_cooked_mesh_lod){.meshlet_count = st->meshlets,
                                      .triangle_count = PATCH_TRIS * st->meshlets,
                                      .vertex_count = nv};
    const f32 mn[3] = {fm->bounds.min.x, fm->bounds.min.y, fm->bounds.min.z};
    const f32 mx[3] = {fm->bounds.max.x, fm->bounds.max.y, fm->bounds.max.z};
    for (u32 c = 0; c < 3; ++c) {
        m->bounds_min[c] = mn[c];
        m->bounds_max[c] = mx[c];
        m->sphere[c] = 0.5f * (mn[c] + mx[c]);
        m->position_scale[c] = 1.0f;
    }
    m->sphere[3] =
        0.5f * sqrtf((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                     (mx[2] - mn[2]) * (mx[2] - mn[2]));
}

/* The meshlet records into an UPLOAD buffer (read through de_bytes) the GPU scene names. */
static void mesh_upload(fmesh *fm, de_gpu_device *dev) {
    const u64 size = (u64)fm->meshlets * DE_COOKED_MESHLET_SIZE;
    fm->buf = de_gpu_buffer_create(dev, &(de_gpu_buffer_desc){.size = size,
                                                              .usage = DE_USAGE_STORAGE,
                                                              .memory = DE_MEM_UPLOAD,
                                                              .name = "test meshlets"});
    ASSERT_TRUE(DE_HANDLE_VALID(fm->buf));
    u8 *map = (u8 *)de_gpu_buffer_map(dev, fm->buf);
    ASSERT_TRUE(map != NULL);
    memcpy(map, fm->records, (size_t)size);
    fm->rm.buffers[DE_RENDER_MESH_MESHLETS] = fm->buf;
    fm->rm.bindless[DE_RENDER_MESH_MESHLETS] = de_gpu_buffer_bindless(dev, fm->buf);
    fm->rm.buffer_bytes[DE_RENDER_MESH_MESHLETS] = size;
}

static void mesh_free(fmesh *fm, de_gpu_device *dev) {
    if (dev != NULL && DE_HANDLE_VALID(fm->buf))
        de_gpu_buffer_destroy(dev, fm->buf);
    free(fm->records);
    free(fm->pos);
    free(fm->tri);
    free(fm->tri_count);
    memset(fm, 0, sizeof *fm);
}

/* ================================================================================================
 * Fake render world
 * ============================================================================================== */

typedef struct world {
    de__render_world_frame f;
    u32 cap;
    bool touch_all;
} world;

static void world_init(world *w, u32 cap) {
    memset(w, 0, sizeof *w);
    w->cap = cap;
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

/* Slot `s`: mesh `mesh` (1-based) with transform `xf`, material `mat` on every submesh (0: none,
 * the mesh's default), casting shadows or not. */
static void world_set(world *w, u32 s, u32 mesh, de_affine xf, u32 mat, bool caster) {
    de__rw_instances *in = &w->f.instances;
    in->entity[s] = 1000u + s;
    in->mesh[s].id = mesh;
    in->transform[s] = xf;
    in->prev_transform[s] = xf;
    in->layer_mask[s] = 1u;
    in->bounds[s] = de_aabb_transform(g_mesh[mesh - 1u].bounds, &xf);
    in->flags[s] = (u8)(DE__RW_INST_HAS_BOUNDS | (caster ? DE__RW_INST_CAST_SHADOWS : 0u));
    for (u32 k = 0; k < DE__RW_MATERIALS; ++k)
        in->materials[(size_t)s * DE__RW_MATERIALS + k].id = mat;
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

typedef struct mc_result {
    bool valid;
    u32 inst_counters[4];
    u32 counters[4];
    de__vis_raster_args args[DE__VIS_PHASES];
    bool args_valid[DE__VIS_PHASES];
    de__vis_work_item *items;
    u32 item_count, item_cap;
    de__vis_meshlet *visible;
    u32 visible_stored, visible_cap_alloc;
    u32 capacity; /* the visible list's */
    de__vis_cull_view cv;
    de__gs_instance *inst;
    u32 inst_count, inst_cap;
    de__gs_material *mat;
    u32 mat_count, mat_cap;
} mc_result;

typedef struct mcx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_gpu_device *dev;
    bool real_gpu;
    de_rg *g;
    de__gpu_scene *gs;
    de__vis_shared *vis;
    de_gpu_fence fence;
    u64 frames;
    de_gpu_buffer rb;
    u64 rb_size;
    world w;
    rng r;
    /* The camera and the frame's options. */
    de_vec3 eye, target;
    f32 fov;
    bool ortho;
    f32 ortho_half; /* half height of the orthographic view, metres */
    u32 width, height;
    bool phase2_repeat; /* run meshlet culling's phase 2 on phase 1's work items */
    mc_result res;
} mcx;

static void mcx_setup(mcx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "meshlet_cull_test"}, &f->arena), DE_OK);
    de_error_clear();
    memset(g_mesh, 0, sizeof g_mesh);
    g_mesh_count = 0;
    memset(g_mat, 0, sizeof g_mat);
    g_mat[0].standard.double_sided = false;
    g_mat[1].standard.double_sided = true;
    f->eye = (de_vec3){0.0f, 0.0f, 0.0f};
    f->target = (de_vec3){0.0f, 1.0f, 0.0f};
    f->fov = 1.0f;
    f->width = 1280;
    f->height = 720;
    f->ortho_half = 20.0f;
    f->r.s = 0x9E3779B97F4A7C15ull;
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

static void mcx_teardown(mcx *f) {
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
    free(f->res.visible);
    free(f->res.inst);
    free(f->res.mat);
    world_free(&f->w);
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
    /* The cvars are process-wide: back to their defaults. */
    const char *const names[] = {"r.cull_meshlet_cone", "r.cull_small_tri", "r.cull_disable",
                                 "r.lod_bias", "r.occlusion"};
    const char *const values[] = {"1", "1", "0", "0", "1"};
    for (u32 i = 0; i < 5; ++i) {
        const de_cvar cv = de_cvar_find(names[i]);
        if (DE_HANDLE_VALID(cv))
            (void)de_cvar_set(cv, values[i]);
    }
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

/* Opens a device (SKIPs when it is not there), a graph, a GPU scene and the vis frame. */
static void mcx_open(mcx *f, de_gpu_backend backend, u32 instances) {
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
    printf("device: \"%s\" (%s), waves %u..%u\n", caps->device_name, de_gpu_backend_name(backend),
           caps->wave_size_min, caps->wave_size_max);
    render_d3d12_report(f->dev);
    if (!caps->base_tier_ok) {
        snprintf(why, sizeof why, "below the Base tier: %.400s", caps->base_tier_reason);
        SKIP(why);
    }
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "mc"},
                            &f->g),
              DE_OK);
    f->fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(f->fence));
    de__gpu_scene_desc gd = {.device = f->dev,
                             .allocator = &f->alloc,
                             .resolver = {.mesh = fake_mesh,
                                          .material = fake_material,
                                          .texture_bindless = fake_texture},
                             .staging_bytes = (u64)64 << 20};
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
    /* LOD 0 everywhere (the meshes have one LOD anyway). */
    const de_cvar bias = de_cvar_find("r.lod_bias");
    if (DE_HANDLE_VALID(bias))
        (void)de_cvar_set(bias, "-7");
    /* DENG-233: without occlusion, phase 1 culls every frustum-visible instance (a new view's
     * first frame would otherwise test everything in phase 2). test_two_phase.c tests occlusion. */
    const de_cvar occ = de_cvar_find("r.occlusion");
    if (DE_HANDLE_VALID(occ))
        (void)de_cvar_set(occ, "0");
}

/* Adds mesh `style` (built and uploaded); returns its 1-based id. */
static u32 mcx_mesh(mcx *f, const mesh_style *style) {
    ASSERT_TRUE(g_mesh_count < MAX_MESHES);
    fmesh *fm = &g_mesh[g_mesh_count];
    mesh_build(fm, style, &f->r);
    mesh_upload(fm, f->dev);
    return ++g_mesh_count;
}

/* ---------- One frame ---------- */

#define RB_ICOUNTERS 0u
#define RB_COUNTERS  64u
#define RB_ARGS      128u /* DE__VIS_PHASES x 64 */
#define RB_ITEMS     256u

typedef struct rb_pass {
    mcx *f;
    de_rg_resource items, icounters, counters, visible, args[DE__VIS_PHASES];
    u64 items_bytes, visible_off, visible_bytes;
} rb_pass;

static void readback_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    const de_gpu_buffer rb = p->f->rb;
    de_gpu_cmd_copy_buffer(cmd, rb, RB_ICOUNTERS, de_rg_buffer(pass, p->icounters), 0,
                           DE__VIS_COUNTER_BYTES);
    de_gpu_cmd_copy_buffer(cmd, rb, RB_COUNTERS, de_rg_buffer(pass, p->counters), 0,
                           DE__VIS_MESHLET_COUNTER_BYTES);
    for (u32 i = 0; i < DE__VIS_PHASES; ++i)
        if (DE_HANDLE_VALID(p->args[i]))
            de_gpu_cmd_copy_buffer(cmd, rb, RB_ARGS + 64u * i, de_rg_buffer(pass, p->args[i]), 0,
                                   sizeof(de__vis_raster_args));
    de_gpu_cmd_copy_buffer(cmd, rb, RB_ITEMS, de_rg_buffer(pass, p->items), 0, p->items_bytes);
    de_gpu_cmd_copy_buffer(cmd, rb, p->visible_off, de_rg_buffer(pass, p->visible), 0,
                           p->visible_bytes);
}

static void ensure_rb(mcx *f, u64 bytes) {
    if (bytes <= f->rb_size)
        return;
    if (DE_HANDLE_VALID(f->rb)) {
        de_gpu_wait_idle(f->dev);
        de_gpu_buffer_destroy(f->dev, f->rb);
    }
    f->rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = bytes,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK,
                                                               .name = "meshlet test readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->rb));
    f->rb_size = bytes;
}

static void grow(void **p, u32 *cap, u32 n, size_t elem) {
    if (n <= *cap)
        return;
    free(*p);
    *p = zalloc((size_t)n * elem);
    *cap = n;
}

/* The camera's view and projection matrices. */
static void camera(const mcx *f, de_mat4 *view, de_mat4 *proj) {
    *view = de_m4_look_at(f->eye, f->target, (de_vec3){0.0f, 0.0f, 1.0f});
    const f32 aspect = (f32)f->width / (f32)f->height;
    *proj = f->ortho ? de_m4_ortho_rz(-f->ortho_half * aspect, f->ortho_half * aspect,
                                      -f->ortho_half, f->ortho_half, 0.1f, 1000.0f)
                     : de_m4_perspective_rz(f->fov, aspect, 0.1f);
}

/* Builds, runs and waits for one frame; fills f->res. */
static bool mcx_frame(mcx *f) {
    const u64 n = f->frames++;
    world_begin(&f->w);
    if (de__gpu_scene_update(f->gs, &f->w.f) != DE_OK)
        return false;
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    if (de__rg_begin(f->g, f->arena) != DE_OK || de__gpu_scene_add_passes(f->gs, f->g) != DE_OK)
        return false;
    de_mat4 view, proj;
    camera(f, &view, &proj);
    const de__vis_frame_desc d = {.gpu_scene = f->gs,
                                  .view = (de_view){.id = 1},
                                  .view_slot = 0,
                                  .frame_index = n,
                                  .frames_completed = n, /* every earlier frame was waited for */
                                  .width = f->width,
                                  .height = f->height,
                                  .view_matrix = view,
                                  .proj = proj,
                                  .camera_position = f->eye,
                                  .origin = f->w.f.origin};
    de__vis_context *vc = NULL;
    if (de__vis_frame_build(f->vis, f->g, &d, &vc) != DE_OK || vc == NULL)
        return false;
    if (f->phase2_repeat && DE_HANDLE_VALID(vc->work_items[0])) {
        /* Stand in for DENG-233's phase 2: the same work items again. */
        vc->work_items[1] = vc->work_items[0];
        vc->cull_args[1] = vc->cull_args[0];
        de__vis_cull_meshlets(f->g, vc, DE__VIS_PHASE_2);
    }
    rb_pass *rp = NULL;
    const bool want_rb = DE_HANDLE_VALID(vc->visible_meshlets);
    if (want_rb) {
        rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
        ASSERT_TRUE(rp != NULL);
        rp->f = f;
        rp->items = vc->work_items[0];
        rp->icounters = vc->instance_counters[0];
        rp->counters = vc->meshlet_counters;
        rp->visible = vc->visible_meshlets;
        for (u32 i = 0; i < DE__VIS_PHASES; ++i)
            rp->args[i] = vc->raster_args[i];
        rp->items_bytes = (u64)vc->work_capacity * sizeof(de__vis_work_item);
        rp->visible_off = (RB_ITEMS + rp->items_bytes + 255u) & ~(u64)255u;
        rp->visible_bytes = (u64)vc->visible_capacity * sizeof(de__vis_meshlet);
        ensure_rb(f, rp->visible_off + rp->visible_bytes);
        de_rg_pass *p =
            de_rg_add_pass(f->g, "test.readback", DE_QUEUE_GRAPHICS, readback_execute, rp);
        de_rg_read(p, rp->items, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->icounters, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->counters, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->visible, DE_ACCESS_COPY_SRC);
        for (u32 i = 0; i < DE__VIS_PHASES; ++i)
            if (DE_HANDLE_VALID(rp->args[i]))
                de_rg_read(p, rp->args[i], DE_ACCESS_COPY_SRC);
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
    mc_result *res = &f->res;
    res->inst_count = de__gpu_scene_table(f->gs, DE__GS_INSTANCES).count;
    res->mat_count = de__gpu_scene_table(f->gs, DE__GS_MATERIALS).count;
    grow((void **)&res->inst, &res->inst_cap, res->inst_count, sizeof(de__gs_instance));
    grow((void **)&res->mat, &res->mat_cap, res->mat_count, sizeof(de__gs_material));
    for (u32 i = 0; i < res->inst_count; ++i)
        de__gpu_scene_debug_pack(f->gs, DE__GS_INSTANCES, i, &res->inst[i]);
    for (u32 i = 0; i < res->mat_count; ++i)
        de__gpu_scene_debug_pack(f->gs, DE__GS_MATERIALS, i, &res->mat[i]);
    de__gpu_scene_frame_end(f->gs, f->g, true);
    if (de_gpu_fence_wait(f->dev, f->fence, n + 1, WAIT_NS) != DE_OK)
        return false;
    res->valid = false;
    res->cv = vc->cull;
    res->capacity = vc->visible_capacity;
    if (!want_rb)
        return true;
    const u8 *m = (const u8 *)de_gpu_buffer_map(f->dev, f->rb);
    memcpy(res->inst_counters, m + RB_ICOUNTERS, sizeof res->inst_counters);
    memcpy(res->counters, m + RB_COUNTERS, sizeof res->counters);
    for (u32 i = 0; i < DE__VIS_PHASES; ++i) {
        res->args_valid[i] = DE_HANDLE_VALID(rp->args[i]);
        memset(&res->args[i], 0, sizeof res->args[i]);
        if (res->args_valid[i])
            memcpy(&res->args[i], m + RB_ARGS + 64u * i, sizeof res->args[i]);
    }
    const u32 asked = res->inst_counters[DE__VIS_COUNTER_WORK_ITEMS / 4u];
    res->item_count = asked < vc->work_capacity ? asked : vc->work_capacity;
    grow((void **)&res->items, &res->item_cap, res->item_count, sizeof(de__vis_work_item));
    if (res->item_count > 0)
        memcpy(res->items, m + RB_ITEMS, res->item_count * sizeof(de__vis_work_item));
    const u32 vis = res->counters[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u];
    res->visible_stored = vis < vc->visible_capacity ? vis : vc->visible_capacity;
    grow((void **)&res->visible, &res->visible_cap_alloc, res->visible_stored,
         sizeof(de__vis_meshlet));
    if (res->visible_stored > 0)
        memcpy(res->visible, m + rp->visible_off, res->visible_stored * sizeof(de__vis_meshlet));
    res->valid = true;
    return true;
}

/* ================================================================================================
 * Comparison with the CPU reference
 * ============================================================================================== */

/* The fake mesh of instance slot `s`. */
static const fmesh *slot_mesh(const mcx *f, u32 s) {
    const u32 id = f->w.f.instances.mesh[s].id;
    return id >= 1u && id <= g_mesh_count ? &g_mesh[id - 1u] : NULL;
}

/* The flags the frame used (cvars), as the stage computes them. */
static u32 frame_flags(void) {
    const de_cvar cone = de_cvar_find("r.cull_meshlet_cone");
    const de_cvar small_cv = de_cvar_find("r.cull_small_tri");
    const de_cvar dis = de_cvar_find("r.cull_disable");
    if (DE_HANDLE_VALID(dis) && de_cvar_bool(dis))
        return 0;
    u32 flags = 0;
    if (!DE_HANDLE_VALID(cone) || de_cvar_bool(cone))
        flags |= DE__VIS_MESHLET_CONE;
    if (!DE_HANDLE_VALID(small_cv) || de_cvar_bool(small_cv))
        flags |= DE__VIS_MESHLET_SMALL;
    return flags;
}

typedef struct cmp_out {
    u32 tested, cpu_visible, gpu_visible, marginal, mismatches, bad_entries;
    u32 culled[DE__VIS_MESHLET_CULLED_SMALL + 1];
} cmp_out;

/* Called for every tested meshlet. */
typedef void (*visit_fn)(mcx *f, u32 slot, const fmesh *fm, u32 meshlet, bool gpu_visible,
                         de__vis_meshlet_result cpu, bool marginal, void *user);

/* Compares the entries [first, first + count) of the visible list with the CPU reference of every
 * meshlet of every work item. */
static cmp_out compare(mcx *f, u32 first, u32 count, u32 flags, visit_fn visit, void *user) {
    cmp_out o;
    memset(&o, 0, sizeof o);
    const mc_result *res = &f->res;
    EXPECT_TRUE(res->valid);
    if (!res->valid)
        return o;
    u32 maxm = 1;
    for (u32 i = 0; i < g_mesh_count; ++i)
        maxm = g_mesh[i].meshlets > maxm ? g_mesh[i].meshlets : maxm;
    const size_t bits = (size_t)res->inst_count * maxm;
    u8 *seen = (u8 *)zalloc(bits);
    for (u32 i = first; i < first + count && i < res->visible_stored; ++i) {
        const de__vis_meshlet e = res->visible[i];
        const fmesh *fm = e.instance < res->inst_count ? slot_mesh(f, e.instance) : NULL;
        if (fm == NULL || e.meshlet >= fm->meshlets) {
            if (o.bad_entries++ < 8)
                printf("  entry %u: instance %u meshlet %u\n", i, e.instance, e.meshlet);
            continue;
        }
        u8 *b = &seen[(size_t)e.instance * maxm + e.meshlet];
        if (*b) {
            if (o.bad_entries++ < 8)
                printf("  entry %u: instance %u meshlet %u twice\n", i, e.instance, e.meshlet);
            continue;
        }
        *b = 1;
        o.gpu_visible++;
    }
    const de__vis_meshlet_params p = {.flags = flags,
                                      .width = (f32)f->width,
                                      .height = (f32)f->height,
                                      .materials = res->mat,
                                      .material_count = res->mat_count};
    for (u32 i = 0; i < res->item_count; ++i) {
        const de__vis_work_item *it = &res->items[i];
        const fmesh *fm = it->instance < res->inst_count ? slot_mesh(f, it->instance) : NULL;
        if (fm == NULL) {
            o.bad_entries++;
            continue;
        }
        const de__gs_instance *inst = &res->inst[it->instance];
        for (u32 k = 0; k < it->meshlet_count; ++k) {
            const u32 ml = it->meshlet_first + k;
            if (ml >= fm->meshlets)
                continue;
            o.tested++;
            const de__vis_meshlet_bounds mb = de__vis_meshlet_bounds_load(fm->records, ml);
            bool marg = false;
            const de__vis_meshlet_result cpu = de__vis_meshlet_test(&res->cv, inst, &mb, &p, &marg);
            const bool gpu = seen[(size_t)it->instance * maxm + ml] != 0;
            o.culled[cpu]++;
            o.cpu_visible += cpu == DE__VIS_MESHLET_VISIBLE;
            if (marg)
                o.marginal++;
            else if (gpu != (cpu == DE__VIS_MESHLET_VISIBLE)) {
                if (o.mismatches++ < 8)
                    printf("  instance %u meshlet %u: cpu %d, gpu %s\n", it->instance, ml, (int)cpu,
                           gpu ? "visible" : "culled");
            }
            if (visit != NULL)
                visit(f, it->instance, fm, ml, gpu, cpu, marg, user);
        }
    }
    free(seen);
    return o;
}

/* Checks the full list (phase 1 only, every entry stored) against the CPU and the counters. */
static cmp_out check_all(mcx *f, u32 flags, visit_fn visit, void *user) {
    const mc_result *res = &f->res;
    const u32 vis = res->counters[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u];
    EXPECT_LE(vis, res->capacity);
    const cmp_out o = compare(f, 0, res->visible_stored, flags, visit, user);
    EXPECT_EQ(o.bad_entries, 0u);
    EXPECT_EQ(o.mismatches, 0u);
    EXPECT_EQ(res->counters[DE__VIS_MESHLET_COUNTER_TESTED / 4u], o.tested);
    EXPECT_EQ(o.gpu_visible, vis);
    /* A few marginal meshlets at most (more where many meshlets are about a pixel across, and the
     * small test's rounding is near a pixel centre). */
    EXPECT_LE(o.marginal, o.tested / 33u + 4u);
    printf("  %u tested, %u visible (cpu %u), %u marginal; culled: frustum %u, cone %u, small "
           "%u\n",
           o.tested, o.gpu_visible, o.cpu_visible, o.marginal,
           o.culled[DE__VIS_MESHLET_CULLED_FRUSTUM], o.culled[DE__VIS_MESHLET_CULLED_CONE],
           o.culled[DE__VIS_MESHLET_CULLED_SMALL]);
    return o;
}

/* The raster arguments of `count` meshlets from entry `first`. */
static void expect_args(const de__vis_raster_args *a, u32 first, u32 count) {
    const de__vis_dispatch_args d = de__vis_dispatch_args_for(count);
    EXPECT_EQ(a->first, first);
    EXPECT_EQ(a->count, count);
    EXPECT_EQ(a->mesh_x, d.x);
    EXPECT_EQ(a->mesh_y, d.y);
    EXPECT_EQ(a->mesh_z, 1u);
    EXPECT_EQ(a->expand_x, d.x);
    EXPECT_EQ(a->expand_y, d.y);
    EXPECT_EQ(a->expand_z, 1u);
}

static bool cvar_set(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (!DE_HANDLE_VALID(cv))
        return false;
    EXPECT_EQ(de_cvar_set(cv, value), DE_OK);
    return true;
}

/* ================================================================================================
 * Scenes
 * ============================================================================================== */

static de_affine rnd_affine(rng *g, de_vec3 lo, de_vec3 hi, f32 smin, f32 smax, bool mirror) {
    const de_vec3 t = {rndf(g, lo.x, hi.x), rndf(g, lo.y, hi.y), rndf(g, lo.z, hi.z)};
    const de_quat q = de_quat_axis_angle(rnd_unit(g), rndf(g, 0.0f, 6.2831853f));
    de_vec3 s = {rndf(g, smin, smax), rndf(g, smin, smax), rndf(g, smin, smax)};
    if (mirror)
        s.x = -s.x;
    return de_affine_from_trs(t, q, s);
}

static const mesh_style k_general = {.meshlets = 256,
                                     .half = 2.0f,
                                     .patch_min = 0.2f,
                                     .patch_max = 0.6f,
                                     .bend = 1.2f,
                                     .soup_percent = 10,
                                     .submeshes = 3};

/* `n` instances of `meshes` general meshes around the origin; a fifth double-sided, a tenth
 * without a material, a twentieth mirrored, half casting shadows. */
static void scene_general(mcx *f, u32 n, u32 meshes, f32 spread) {
    u32 ids[MAX_MESHES];
    for (u32 i = 0; i < meshes; ++i)
        ids[i] = mcx_mesh(f, &k_general);
    for (u32 s = 0; s < n; ++s) {
        const u32 kind = rnd(&f->r) % 20u;
        const u32 mat = kind < 4u ? MAT_DOUBLE : kind < 6u ? 0u : MAT_SINGLE;
        const de_affine xf =
            rnd_affine(&f->r, (de_vec3){-spread, -spread * 0.3f, -spread * 0.2f},
                       (de_vec3){spread, spread, spread * 0.2f}, 0.3f, 3.0f, kind == 7u);
        world_set(&f->w, s, ids[s % meshes], xf, mat, (s & 1u) != 0);
    }
}

/* ================================================================================================
 * GPU tests
 * ============================================================================================== */

/* meshlet_cull_frustum_cpu_ref: about 100k meshlets per camera; the GPU visible set equals the CPU
 * reference under random cameras, with every test on and with the small test off. */
static void frustum_cpu_ref(mcx *f, de_gpu_backend b) {
    mcx_open(f, b, 1600u);
    scene_general(f, 1600u, 4u, 120.0f);
    de__vis_meshlet_set_debug(f->vis, &(de__vis_meshlet_debug){.capacity_fixed = 1u << 19});
    u32 total = 0, cone = 0, small_n = 0;
    for (u32 cam = 0; cam < 4; ++cam) {
        f->eye = (de_vec3){rndf(&f->r, -40, 40), rndf(&f->r, -60, 0), rndf(&f->r, -5, 10)};
        f->target = (de_vec3){rndf(&f->r, -60, 60), rndf(&f->r, 40, 120), rndf(&f->r, -10, 10)};
        for (u32 run = 0; run < 2; ++run) {
            const bool small_on = run == 0;
            (void)cvar_set("r.cull_small_tri", small_on ? "1" : "0");
            ASSERT_TRUE(mcx_frame(f));
            const cmp_out o = check_all(f, frame_flags(), NULL, NULL);
            total += o.tested;
            cone += o.culled[DE__VIS_MESHLET_CULLED_CONE];
            small_n += o.culled[DE__VIS_MESHLET_CULLED_SMALL];
            EXPECT_GT(o.tested, 30000u);
            expect_args(&f->res.args[0], 0, f->res.visible_stored);
            /* DENG-233: phase 2 runs too; with occlusion off it draws nothing, an empty range
             * after phase 1's entries. */
            EXPECT_TRUE(f->res.args_valid[1]);
            expect_args(&f->res.args[1], f->res.visible_stored, 0);
        }
    }
    printf("%u meshlets tested in 8 frames; %u cone-culled, %u small-culled\n", total, cone,
           small_n);
    EXPECT_GT(total, 400000u);
    EXPECT_GT(cone, 1000u);
    /* An orthographic camera too. */
    f->ortho = true;
    f->ortho_half = 60.0f;
    f->eye = (de_vec3){0, -200, 30};
    f->target = (de_vec3){0, 0, 0};
    ASSERT_TRUE(mcx_frame(f));
    check_all(f, frame_flags(), NULL, NULL);
}

/* The world-space triangles of meshlet `ml` of slot `s`, one by one. */
static de_vec3 world_vertex(const mcx *f, u32 s, const fmesh *fm, u32 v) {
    const f32 *p = fm->pos + 3u * (size_t)v;
    return de_affine_transform_point(&f->w.f.instances.transform[s], (de_vec3){p[0], p[1], p[2]});
}

typedef struct cone_check {
    u32 culled, front_facing;
} cone_check;

/* A meshlet the cone test culled has no triangle facing the camera. */
static void visit_cone(mcx *f, u32 s, const fmesh *fm, u32 ml, bool gpu, de__vis_meshlet_result cpu,
                       bool marg, void *user) {
    cone_check *cc = (cone_check *)user;
    (void)marg;
    /* Culled on the GPU, visible to a frustum-only test: the cone culled it. */
    if (gpu || !(cpu == DE__VIS_MESHLET_CULLED_CONE || cpu == DE__VIS_MESHLET_VISIBLE))
        return;
    cc->culled++;
    const de__vis_cull_view *v = &f->res.cv;
    const de_vec3 cam = {v->camera[0], v->camera[1], v->camera[2]};
    const u32 *tri = fm->tri + (size_t)ml * MAX_TRIS * 3u;
    for (u32 t = 0; t < fm->tri_count[ml]; ++t) {
        const de_vec3 a = world_vertex(f, s, fm, tri[3u * t]);
        const de_vec3 b = world_vertex(f, s, fm, tri[3u * t + 1u]);
        const de_vec3 c = world_vertex(f, s, fm, tri[3u * t + 2u]);
        const de_vec3 n = de_v3_cross(de_v3_sub(b, a), de_v3_sub(c, a));
        const de_vec3 d = v->lod[1] != 0.0f
                              ? (de_vec3){-v->planes[4][0], -v->planes[4][1], -v->planes[4][2]}
                              : de_v3_sub(cam, a);
        const f32 dn = de_v3_dot(d, n);
        if (dn > 1e-3f * de_v3_length(d) * de_v3_length(n)) {
            if (cc->front_facing++ < 8)
                printf("  instance %u meshlet %u triangle %u faces the camera (%g)\n", s, ml, t,
                       (double)(dn / (de_v3_length(d) * de_v3_length(n))));
        }
    }
}

/* meshlet_cull_cone_conservative: cameras inside a dense cluster of patches; the cone culls many
 * meshlets and none of them has a front-facing triangle. */
static void cone_conservative(mcx *f, de_gpu_backend b) {
    mcx_open(f, b, 600u);
    (void)cvar_set("r.cull_small_tri", "0");
    u32 ids[3];
    for (u32 i = 0; i < 3; ++i)
        ids[i] = mcx_mesh(f, &k_general);
    for (u32 s = 0; s < 600u; ++s)
        world_set(
            &f->w, s, ids[s % 3u],
            rnd_affine(&f->r, (de_vec3){-25, -25, -10}, (de_vec3){25, 25, 10}, 0.3f, 3.0f, false),
            MAT_SINGLE, true);
    de__vis_meshlet_set_debug(f->vis, &(de__vis_meshlet_debug){.capacity_fixed = 1u << 18});
    cone_check cc = {0};
    for (u32 cam = 0; cam < 6; ++cam) {
        f->eye = (de_vec3){rndf(&f->r, -10, 10), rndf(&f->r, -10, 10), rndf(&f->r, -3, 3)};
        f->target = de_v3_add(f->eye, rnd_unit(&f->r));
        ASSERT_TRUE(mcx_frame(f));
        /* Small test off: the CPU reference sees frustum and cone only. */
        check_all(f, frame_flags(), visit_cone, &cc);
    }
    printf("%u cone-culled meshlets checked\n", cc.culled);
    EXPECT_GT(cc.culled, 2000u);
    EXPECT_EQ(cc.front_facing, 0u);
}

typedef struct small_check {
    u32 culled, covering;
} small_check;

/* True when a triangle of meshlet `ml` covers a pixel centre (CPU rasterization; both facings). */
static bool covers_pixel(const mcx *f, u32 s, const fmesh *fm, u32 ml) {
    const de__vis_cull_view *v = &f->res.cv;
    const u32 *tri = fm->tri + (size_t)ml * MAX_TRIS * 3u;
    const f32 W = (f32)f->width, H = (f32)f->height;
    for (u32 t = 0; t < fm->tri_count[ml]; ++t) {
        f32 px[3], py[3];
        bool behind = false;
        for (u32 k = 0; k < 3; ++k) {
            const de_vec3 p = world_vertex(f, s, fm, tri[3u * t + k]);
            f32 clip[4];
            for (u32 r = 0; r < 4; ++r)
                clip[r] = v->view_proj[r][0] * p.x + v->view_proj[r][1] * p.y +
                          v->view_proj[r][2] * p.z + v->view_proj[r][3];
            if (clip[3] <= v->camera[3])
                behind = true;
            px[k] = (clip[0] / clip[3] * 0.5f + 0.5f) * W;
            py[k] = (clip[1] / clip[3] * 0.5f + 0.5f) * H;
        }
        if (behind)
            return true; /* the GPU skips the test there: treat as covering */
        const f32 x0 = fminf(px[0], fminf(px[1], px[2])), x1 = fmaxf(px[0], fmaxf(px[1], px[2]));
        const f32 y0 = fminf(py[0], fminf(py[1], py[2])), y1 = fmaxf(py[0], fmaxf(py[1], py[2]));
        const i32 i0 = (i32)ceilf(x0 - 0.5f), i1 = (i32)floorf(x1 - 0.5f);
        const i32 j0 = (i32)ceilf(y0 - 0.5f), j1 = (i32)floorf(y1 - 0.5f);
        for (i32 j = j0 > 0 ? j0 : 0; j <= j1 && j < (i32)H; ++j)
            for (i32 i = i0 > 0 ? i0 : 0; i <= i1 && i < (i32)W; ++i) {
                const f32 cx = (f32)i + 0.5f, cy = (f32)j + 0.5f;
                f32 e[3];
                for (u32 k = 0; k < 3; ++k) {
                    const u32 k1 = (k + 1u) % 3u;
                    e[k] = (px[k1] - px[k]) * (cy - py[k]) - (py[k1] - py[k]) * (cx - px[k]);
                }
                if ((e[0] >= 0 && e[1] >= 0 && e[2] >= 0) || (e[0] <= 0 && e[1] <= 0 && e[2] <= 0))
                    return true;
            }
    }
    return false;
}

/* A meshlet the small test culled covers no pixel centre. */
static void visit_small(mcx *f, u32 s, const fmesh *fm, u32 ml, bool gpu,
                        de__vis_meshlet_result cpu, bool marg, void *user) {
    small_check *sc = (small_check *)user;
    (void)marg;
    if (gpu || cpu == DE__VIS_MESHLET_CULLED_FRUSTUM || cpu == DE__VIS_MESHLET_CULLED_CONE)
        return; /* visible, outside the frustum, or culled by the cone (on in Shipping) */
    sc->culled++;
    if (covers_pixel(f, s, fm, ml) && sc->covering++ < 8)
        printf("  instance %u meshlet %u was culled but covers a pixel centre\n", s, ml);
}

/* meshlet_cull_small_tri_no_false_negative: dense distant geometry (patches of a few centimetres
 * 150 to 400 m away); the small test culls many meshlets, none of which covers a pixel centre. */
static void small_tri(mcx *f, de_gpu_backend b) {
    mcx_open(f, b, 400u);
    (void)cvar_set("r.cull_meshlet_cone", "0");
    static const mesh_style tiny = {.meshlets = 128,
                                    .half = 1.0f,
                                    .patch_min = 0.02f,
                                    .patch_max = 0.15f,
                                    .bend = 1.0f,
                                    .soup_percent = 20,
                                    .submeshes = 1};
    u32 ids[4];
    for (u32 i = 0; i < 4; ++i)
        ids[i] = mcx_mesh(f, &tiny);
    for (u32 s = 0; s < 400u; ++s) {
        const f32 y = rndf(&f->r, 150.0f, 400.0f);
        world_set(&f->w, s, ids[s % 4u],
                  rnd_affine(&f->r, (de_vec3){-0.5f * y, y, -0.25f * y},
                             (de_vec3){0.5f * y, y, 0.25f * y}, 0.5f, 2.0f, s % 9u == 0),
                  MAT_SINGLE, true);
    }
    de__vis_meshlet_set_debug(f->vis, &(de__vis_meshlet_debug){.capacity_fixed = 1u << 17});
    small_check sc = {0};
    ASSERT_TRUE(mcx_frame(f));
    check_all(f, frame_flags(), visit_small, &sc);
    /* A lower resolution culls more. */
    f->width = 640;
    f->height = 360;
    ASSERT_TRUE(mcx_frame(f));
    check_all(f, frame_flags(), visit_small, &sc);
    /* Orthographic. */
    f->ortho = true;
    f->ortho_half = 150.0f;
    f->eye = (de_vec3){0, -10, 0};
    f->target = (de_vec3){0, 1, 0};
    ASSERT_TRUE(mcx_frame(f));
    check_all(f, frame_flags(), visit_small, &sc);
    printf("%u small-culled meshlets rasterized\n", sc.culled);
    EXPECT_GT(sc.culled, 5000u);
    EXPECT_EQ(sc.covering, 0u);
}

/* meshlet_cull_double_sided: flat patches all facing away from the camera. The cone culls every
 * meshlet of a single-sided material and none of a double-sided one, of an instance without a
 * material (the mesh default) or of a mirrored instance. */
static void double_sided(mcx *f, de_gpu_backend b) {
    mcx_open(f, b, 64u);
    (void)cvar_set("r.cull_small_tri", "0");
    static const mesh_style away = {.meshlets = 100,
                                    .half = 1.0f,
                                    .patch_min = 0.2f,
                                    .patch_max = 0.4f,
                                    .facing_y = true,
                                    .submeshes = 2};
    const u32 mesh = mcx_mesh(f, &away);
    /* 16 each: single-sided, double-sided, no material, mirrored single-sided. */
    for (u32 s = 0; s < 64u; ++s) {
        const u32 kind = s / 16u;
        const de_vec3 t = {(f32)(s % 8u) * 3.0f - 10.5f, 30.0f + (f32)((s / 8u) % 2u) * 4.0f,
                           (f32)(kind) * 3.0f - 4.5f};
        const de_vec3 sc = {kind == 3u ? -1.0f : 1.0f, 1.0f, 1.0f};
        world_set(&f->w, s, mesh, de_affine_from_trs(t, (de_quat){0, 0, 0, 1}, sc),
                  kind == 0u || kind == 3u ? MAT_SINGLE
                  : kind == 1u             ? MAT_DOUBLE
                                           : 0u,
                  true);
    }
    ASSERT_TRUE(mcx_frame(f));
    check_all(f, frame_flags(), NULL, NULL);
    u32 per_kind[4] = {0};
    for (u32 i = 0; i < f->res.visible_stored; ++i)
        per_kind[f->res.visible[i].instance / 16u]++;
    EXPECT_EQ(per_kind[0], 0u);         /* single-sided: every meshlet faces away */
    EXPECT_EQ(per_kind[1], 16u * 100u); /* double-sided: none culled */
    EXPECT_EQ(per_kind[2], 16u * 100u); /* the mesh default: unknown, not culled */
    EXPECT_EQ(per_kind[3], 16u * 100u); /* mirrored: not culled */
    /* With the cone test off, everything. */
    if (cvar_set("r.cull_meshlet_cone", "0")) {
        ASSERT_TRUE(mcx_frame(f));
        check_all(f, frame_flags(), NULL, NULL);
        EXPECT_EQ(f->res.visible_stored, 64u * 100u);
    }
}

/* meshlet_cull_args_match_count: the raster arguments cover exactly the stored entries; a full
 * list drops entries but counts them; in the automatic mode the list grows from what was read
 * back; more than 65535 visible meshlets continue in y; r.cull_disable keeps every meshlet. */
static void args_match_count(mcx *f, de_gpu_backend b) {
    mcx_open(f, b, 800u);
    (void)cvar_set("r.cull_small_tri", "0");
    scene_general(f, 800u, 2u, 60.0f);
    f->eye = (de_vec3){0, -80, 0};
    f->target = (de_vec3){0, 0, 0};
    de__vis_meshlet_set_debug(f->vis, &(de__vis_meshlet_debug){.capacity_fixed = 1u << 18});
    ASSERT_TRUE(mcx_frame(f));
    const cmp_out all = check_all(f, frame_flags(), NULL, NULL);
    const u32 visible = f->res.counters[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u];
    printf("%u visible meshlets\n", visible);
    EXPECT_GT(visible, 70000u);
    expect_args(&f->res.args[0], 0, visible);
    EXPECT_EQ(f->res.args[0].mesh_x, DE__VIS_MAX_GROUPS_X); /* more than 65535: on in y */
    EXPECT_GT(f->res.args[0].mesh_y, 1u);
    EXPECT_EQ(f->res.counters[DE__VIS_MESHLET_COUNTER_PHASE1 / 4u], visible);
    /* A fixed list of a third: the rest dropped but counted; the stored ones are visible ones. */
    const u32 cap = visible / 3u;
    de__vis_meshlet_set_debug(f->vis, &(de__vis_meshlet_debug){.capacity_fixed = cap});
    ASSERT_TRUE(mcx_frame(f));
    EXPECT_EQ(f->res.capacity, cap);
    EXPECT_EQ(f->res.counters[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u], visible);
    EXPECT_EQ(f->res.visible_stored, cap);
    expect_args(&f->res.args[0], 0, cap);
    cmp_out o = compare(f, 0, cap, frame_flags(), NULL, NULL);
    EXPECT_EQ(o.bad_entries, 0u);
    EXPECT_EQ(o.gpu_visible, cap);
    EXPECT_LE(o.mismatches, all.cpu_visible - cap + o.marginal); /* only the dropped ones */
    /* Automatic: the list grows from what was read back (the overflow of the frame before, and
     * the meshlet count of the visible instances), so every entry fits again. */
    de__vis_meshlet_set_debug(f->vis, NULL);
    de__vis_meshlet_stats st;
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_TRUE(mcx_frame(f));
        st = de__vis_view_meshlet_stats(f->vis, 0);
        printf("  frame %u: list of %u, %u stored\n", i, f->res.capacity, f->res.visible_stored);
    }
    EXPECT_GT(f->res.capacity, visible);
    check_all(f, frame_flags(), NULL, NULL);
    expect_args(&f->res.args[0], 0, visible);
    EXPECT_TRUE(st.valid);
    EXPECT_EQ(st.visible, visible);
    EXPECT_EQ(st.tested, all.tested);
    EXPECT_EQ(st.phase1, visible);
    /* Both phases (phase 2 on the same work items) ask for twice as many: that frame drops
     * entries of phase 2, the next grows the list (with a warning) and holds both. */
    f->phase2_repeat = true;
    const u32 cap0 = f->res.capacity;
    ASSERT_TRUE(mcx_frame(f));
    if (2u * visible > cap0) {
        EXPECT_EQ(f->res.counters[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u], 2u * visible);
        EXPECT_EQ(f->res.visible_stored, cap0);
        expect_args(&f->res.args[1], visible, cap0 - visible);
        ASSERT_TRUE(mcx_frame(f));
        EXPECT_GT(f->res.capacity, cap0);
    }
    EXPECT_EQ(f->res.visible_stored, 2u * visible);
    expect_args(&f->res.args[0], 0, visible);
    expect_args(&f->res.args[1], visible, visible);
    f->phase2_repeat = false;
    /* r.cull_disable: no meshlet test (the debug cvars are not in Shipping). */
    if (cvar_set("r.cull_disable", "1")) {
        ASSERT_TRUE(mcx_frame(f));
        ASSERT_TRUE(mcx_frame(f)); /* the list may grow */
        EXPECT_EQ(f->res.counters[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u],
                  f->res.counters[DE__VIS_MESHLET_COUNTER_TESTED / 4u]);
        check_all(f, 0u, NULL, NULL);
    }
}

/* meshlet_cull_phase_ranges: phase 2 (here: the same work items again) appends after phase 1;
 * each phase's arguments cover its own range, and a full list cuts phase 2 first. */
static void phase_ranges(mcx *f, de_gpu_backend b) {
    mcx_open(f, b, 300u);
    scene_general(f, 300u, 2u, 40.0f);
    f->eye = (de_vec3){0, -50, 0};
    f->target = (de_vec3){0, 0, 0};
    f->phase2_repeat = true;
    de__vis_meshlet_set_debug(f->vis, &(de__vis_meshlet_debug){.capacity_fixed = 1u << 18});
    ASSERT_TRUE(mcx_frame(f));
    const u32 n1 = f->res.counters[DE__VIS_MESHLET_COUNTER_PHASE1 / 4u];
    printf("phase 1: %u visible meshlets\n", n1);
    EXPECT_GT(n1, 1000u);
    ASSERT_TRUE(f->res.args_valid[0] && f->res.args_valid[1]);
    expect_args(&f->res.args[0], 0, n1);
    expect_args(&f->res.args[1], n1, n1);
    EXPECT_EQ(f->res.counters[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u], 2u * n1);
    /* Each phase's range holds the CPU set once (the packed IDs of both phases are distinct). */
    for (u32 ph = 0; ph < 2; ++ph) {
        const cmp_out o = compare(f, ph * n1, n1, frame_flags(), NULL, NULL);
        EXPECT_EQ(o.bad_entries, 0u);
        EXPECT_EQ(o.mismatches, 0u);
        EXPECT_EQ(o.gpu_visible, n1);
        EXPECT_EQ(f->res.counters[DE__VIS_MESHLET_COUNTER_TESTED / 4u], 2u * o.tested);
    }
    /* A list that holds phase 1 and half of phase 2. */
    de__vis_meshlet_set_debug(f->vis, &(de__vis_meshlet_debug){.capacity_fixed = n1 + n1 / 2u});
    ASSERT_TRUE(mcx_frame(f));
    expect_args(&f->res.args[0], 0, n1);
    expect_args(&f->res.args[1], n1, n1 / 2u);
    EXPECT_EQ(f->res.counters[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u], 2u * n1);
    EXPECT_EQ(f->res.visible_stored, n1 + n1 / 2u);
    /* One that does not hold phase 1: phase 2 is empty (one group of nothing). */
    de__vis_meshlet_set_debug(f->vis, &(de__vis_meshlet_debug){.capacity_fixed = n1 / 2u});
    ASSERT_TRUE(mcx_frame(f));
    expect_args(&f->res.args[0], 0, n1 / 2u);
    expect_args(&f->res.args[1], n1 / 2u, 0u);
    EXPECT_EQ(f->res.args[1].mesh_x, 1u);
    EXPECT_EQ(f->res.args[1].expand_x, 1u);
}

/* Shadow-caster views: only instances that cast shadows. */
static void casters_only(mcx *f, de_gpu_backend b) {
    mcx_open(f, b, 200u);
    scene_general(f, 200u, 1u, 30.0f);
    f->eye = (de_vec3){0, -40, 0};
    f->target = (de_vec3){0, 0, 0};
    de__vis_meshlet_set_debug(
        f->vis, &(de__vis_meshlet_debug){.capacity_fixed = 1u << 16, .casters_only = true});
    ASSERT_TRUE(mcx_frame(f));
    const cmp_out o = check_all(f, frame_flags() | DE__VIS_MESHLET_CASTERS_ONLY, NULL, NULL);
    EXPECT_GT(o.gpu_visible, 0u);
    EXPECT_GT(o.culled[DE__VIS_MESHLET_CULLED_INSTANCE], 0u);
    for (u32 i = 0; i < f->res.visible_stored; ++i)
        EXPECT_TRUE(
            (f->w.f.instances.flags[f->res.visible[i].instance] & DE__RW_INST_CAST_SHADOWS) != 0);
}

#define GPU_TESTS(sfx, backend)                                                                    \
    TEST_F(meshlet_cull, frustum_cpu_ref_##sfx, mcx) {                                             \
        frustum_cpu_ref(f, backend);                                                               \
    }                                                                                              \
    TEST_F(meshlet_cull, cone_conservative_##sfx, mcx) {                                           \
        cone_conservative(f, backend);                                                             \
    }                                                                                              \
    TEST_F(meshlet_cull, small_tri_no_false_negative_##sfx, mcx) {                                 \
        small_tri(f, backend);                                                                     \
    }                                                                                              \
    TEST_F(meshlet_cull, double_sided_##sfx, mcx) {                                                \
        double_sided(f, backend);                                                                  \
    }                                                                                              \
    TEST_F(meshlet_cull, args_match_count_##sfx, mcx) {                                            \
        args_match_count(f, backend);                                                              \
    }                                                                                              \
    TEST_F(meshlet_cull, phase_ranges_##sfx, mcx) {                                                \
        phase_ranges(f, backend);                                                                  \
    }                                                                                              \
    TEST_F(meshlet_cull, casters_only_##sfx, mcx) {                                                \
        casters_only(f, backend);                                                                  \
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

/* meshlet_cull.slang's constants against the C table, DeVisRasterArgs's size, the record decode
 * against de_cooked_mesh_meshlet, and the push constants' size. */
TEST(meshlet_cull, layout_match) {
    char path[1024];
    snprintf(path, sizeof path, "%s/culling/meshlet_cull.slang", DE_VIS_TEST_SHADER_DIR);
    char *text = read_text(path);
    ASSERT_TRUE(text != NULL);
    u32 count = 0, found = 0;
    const de__vis_layout_const *k = de__vis_meshlet_layout_consts(&count);
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
            printf("  %s is not in de__vis_meshlet_layout_consts\n", name);
        EXPECT_TRUE(ok);
        ++found;
    }
    EXPECT_EQ(found, count);
    /* MeshletCullParams: 16 four-byte fields. */
    const char *ps = strstr(text, "struct MeshletCullParams");
    const char *pe = ps != NULL ? strstr(ps, "};") : NULL;
    u32 fields = 0;
    for (const char *ln = ps; ln != NULL && pe != NULL && ln < pe; ln = strchr(ln + 1, '\n'))
        fields += strncmp(ln, "\n    uint ", 10) == 0 || strncmp(ln, "\n    float ", 11) == 0;
    EXPECT_EQ(fields * 4u, 64u);
    free(text);
    /* DeVisRasterArgs: 8 uint fields. */
    snprintf(path, sizeof path, "%s/visbuffer/vis_types.slang", DE_VIS_TEST_SHADER_DIR);
    text = read_text(path);
    ASSERT_TRUE(text != NULL);
    const char *rs = strstr(text, "public struct DeVisRasterArgs\n");
    const char *re = rs != NULL ? strstr(rs, "};") : NULL;
    u32 uints = 0;
    for (const char *ln = rs; ln != NULL && re != NULL && ln < re;
         ln = strstr(ln + 1, "public uint"))
        uints += strncmp(ln, "public uint", 11) == 0;
    EXPECT_EQ(uints * 4u, (u32)sizeof(de__vis_raster_args));
    free(text);
    /* The record decode. */
    rng g = {77};
    fmesh fm;
    mesh_build(&fm, &k_general, &g);
    de_cooked_mesh_view mv;
    memset(&mv, 0, sizeof mv);
    mv.meshlets = fm.records;
    mv.meshlet_count = fm.meshlets;
    for (u32 i = 0; i < fm.meshlets; ++i) {
        const de_cooked_meshlet c = de_cooked_mesh_meshlet(&mv, i);
        const de__vis_meshlet_bounds b = de__vis_meshlet_bounds_load(fm.records, i);
        EXPECT_EQ(b.submesh, c.submesh);
        for (u32 j = 0; j < 3; ++j) {
            EXPECT_FLOAT_EQ(b.sphere[j], c.center[j], 0.0f);
            EXPECT_FLOAT_EQ(b.apex[j], c.cone_apex[j], 0.0f);
            EXPECT_FLOAT_EQ(b.axis[j], c.cone_axis[j], 0.0f);
        }
        EXPECT_FLOAT_EQ(b.sphere[3], c.radius, 0.0f);
        EXPECT_FLOAT_EQ(b.apex[3], c.cone_cutoff, 0.0f);
    }
    mesh_free(&fm, NULL);
}

/* An instance entry as the GPU scene packs it (rows of the 3x4). */
static de__gs_instance pack_instance(const de_affine *xf, u32 mat) {
    de__gs_instance e;
    memset(&e, 0, sizeof e);
    for (u32 r = 0; r < 3; ++r) {
        e.transform[r].v[0] = xf->m[r];
        e.transform[r].v[1] = xf->m[3 + r];
        e.transform[r].v[2] = xf->m[6 + r];
        e.transform[r].v[3] = xf->m[9 + r];
    }
    e.flags = DE__GS_INSTANCE_LIVE | DE__GS_INSTANCE_HAS_BOUNDS;
    e.layer_mask = 1u;
    for (u32 k = 0; k < DE__GS_INSTANCE_MATERIALS; ++k)
        e.materials[k / 4u].v[k % 4u] = mat;
    return e;
}

static de__vis_cull_view cpu_view(de_vec3 eye, de_vec3 target, bool ortho, u32 w, u32 h) {
    const de_mat4 view = de_m4_look_at(eye, target, (de_vec3){0, 0, 1});
    const f32 aspect = (f32)w / (f32)h;
    const de_mat4 proj = ortho ? de_m4_ortho_rz(-10 * aspect, 10 * aspect, -10, 10, 0.1f, 1000.0f)
                               : de_m4_perspective_rz(1.0f, aspect, 0.1f);
    de__vis_cull_view v;
    de__vis_cull_view_init(&v, &(de__vis_cull_camera){.view_proj = de_m4_mul(&proj, &view),
                                                      .proj = proj,
                                                      .position = eye,
                                                      .height = h,
                                                      .lod_error_px = 1.0f});
    v.layer_mask = ~0u;
    return v;
}

/* The cone test on the CPU against the triangles, for many cameras: near the patches, inside the
 * cones' apexes, far away, orthographic; non-uniform scales; mirrored transforms never cull. */
TEST(meshlet_cull, cone_conservative_cpu) {
    rng g = {12345};
    fmesh fm;
    mesh_build(&fm, &k_general, &g);
    de__gs_material mats[1];
    memset(mats, 0, sizeof mats);
    mats[0].flags = DE__GS_MATERIAL_READY;
    u32 culled = 0, front = 0, mirrored_culled = 0;
    for (u32 iter = 0; iter < 4000u; ++iter) {
        const bool mirror = iter % 10u == 0;
        const de_affine xf =
            rnd_affine(&g, (de_vec3){-3, -3, -3}, (de_vec3){3, 3, 3}, 0.2f, 4.0f, mirror);
        const de__gs_instance inst = pack_instance(&xf, 0u);
        const bool ortho = iter % 7u == 0;
        const f32 range = iter % 3u == 0 ? 2.0f : iter % 3u == 1 ? 10.0f : 200.0f;
        const de_vec3 eye = {rndf(&g, -range, range), rndf(&g, -range, range),
                             rndf(&g, -range, range)};
        const de__vis_cull_view v = cpu_view(eye, de_v3_add(eye, rnd_unit(&g)), ortho, 64, 64);
        for (u32 n = 0; n < 16u; ++n) {
            const u32 ml = rnd(&g) % fm.meshlets;
            const de__vis_meshlet_bounds mb = de__vis_meshlet_bounds_load(fm.records, ml);
            if (!de__vis_meshlet_cone_culled(&inst, &mb, &v))
                continue;
            ++culled;
            mirrored_culled += mirror;
            const u32 *tri = fm.tri + (size_t)ml * MAX_TRIS * 3u;
            for (u32 t = 0; t < fm.tri_count[ml]; ++t) {
                de_vec3 p[3];
                for (u32 k = 0; k < 3; ++k) {
                    const f32 *q = fm.pos + 3u * (size_t)tri[3u * t + k];
                    p[k] = de_affine_transform_point(&xf, (de_vec3){q[0], q[1], q[2]});
                }
                const de_vec3 nn = de_v3_cross(de_v3_sub(p[1], p[0]), de_v3_sub(p[2], p[0]));
                const de_vec3 d = ortho
                                      ? (de_vec3){-v.planes[4][0], -v.planes[4][1], -v.planes[4][2]}
                                      : de_v3_sub(eye, p[0]);
                if (de_v3_dot(d, nn) > 1e-3f * de_v3_length(d) * de_v3_length(nn))
                    ++front;
            }
        }
    }
    printf("%u cone-culled meshlets checked\n", culled);
    EXPECT_GT(culled, 3000u);
    EXPECT_EQ(front, 0u);
    EXPECT_EQ(mirrored_culled, 0u);
    /* The material gate: only a READY single-sided material lets the cone cull. */
    de__gs_instance inst = pack_instance(&(de_affine){{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}}, 0u);
    de__gs_material m2[2];
    memset(m2, 0, sizeof m2);
    m2[0].flags = DE__GS_MATERIAL_READY;
    m2[1].flags = DE__GS_MATERIAL_READY | DE__GS_MATERIAL_DOUBLE_SIDED;
    const de__vis_meshlet_params p = {.materials = m2, .material_count = 2};
    EXPECT_TRUE(de__vis_meshlet_cone_allowed(&inst, 0, &p));
    EXPECT_FALSE(de__vis_meshlet_cone_allowed(&inst, DE__GS_INSTANCE_MATERIALS, &p));
    inst.materials[0].v[1] = 1u;
    EXPECT_FALSE(de__vis_meshlet_cone_allowed(&inst, 1, &p));
    inst.materials[0].v[1] = DE__GS_NONE;
    EXPECT_FALSE(de__vis_meshlet_cone_allowed(&inst, 1, &p));
    inst.materials[0].v[1] = 2u; /* past the table */
    EXPECT_FALSE(de__vis_meshlet_cone_allowed(&inst, 1, &p));
    m2[0].flags = 0; /* not READY */
    EXPECT_FALSE(de__vis_meshlet_cone_allowed(&inst, 0, &p));
    mesh_free(&fm, NULL);
}

/* The screen rectangle of a sphere contains the projection of points on the sphere and is tight;
 * perspective, jittered perspective and orthographic. Spheres reaching the near plane skip. */
TEST(meshlet_cull, small_rect_cpu) {
    rng g = {999};
    u32 rects = 0;
    for (u32 iter = 0; iter < 600u; ++iter) {
        const u32 kind = iter % 3u;
        const de_vec3 eye = {rndf(&g, -5, 5), rndf(&g, -5, 5), rndf(&g, -5, 5)};
        de__vis_cull_view v = cpu_view(eye, de_v3_add(eye, rnd_unit(&g)), kind == 2u, 800, 600);
        if (kind == 1u) { /* a TAA-style jitter: x and y offsets in clip space */
            de_mat4 view = de_m4_look_at(eye, de_v3_add(eye, rnd_unit(&g)), (de_vec3){0, 0, 1});
            de_mat4 proj = de_m4_perspective_rz(1.2f, 800.0f / 600.0f, 0.1f);
            proj.m[8] += 0.7f / 800.0f; /* column 2, row 0 */
            proj.m[9] -= 0.3f / 600.0f; /* column 2, row 1 */
            de__vis_cull_view_init(&v, &(de__vis_cull_camera){.view_proj = de_m4_mul(&proj, &view),
                                                              .proj = proj,
                                                              .position = eye,
                                                              .height = 600,
                                                              .lod_error_px = 1.0f});
        }
        /* A sphere in front of the camera (in the view direction, some to the side). */
        const de_vec3 fwd = {v.planes[4][0], v.planes[4][1], v.planes[4][2]};
        const f32 dist = rndf(&g, 0.05f, 60.0f);
        const de_vec3 side = rnd_unit(&g);
        const de_vec3 c3 = de_v3_add(de_v3_add(eye, de_v3_scale(fwd, dist)),
                                     de_v3_scale(side, rndf(&g, 0.0f, 0.5f) * dist));
        const f32 r = rndf(&g, 0.001f, 0.8f) * dist;
        const f32 c[3] = {c3.x, c3.y, c3.z};
        f32 rect[4];
        if (!de__vis_meshlet_screen_rect(&v, c, r, 800, 600, rect)) {
            /* Reaches the near plane (or behind the camera for a perspective view). */
            const f32 w = v.view_proj[3][0] * c[0] + v.view_proj[3][1] * c[1] +
                          v.view_proj[3][2] * c[2] + v.view_proj[3][3];
            EXPECT_TRUE(kind != 2u);
            EXPECT_TRUE(w - r <= v.camera[3] * 1.001f + 1e-5f);
            EXPECT_FALSE(de__vis_meshlet_small_culled(&v, c, r, 800, 600));
            continue;
        }
        ++rects;
        f32 lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (u32 s = 0; s < 4000u; ++s) {
            const de_vec3 d = rnd_unit(&g);
            const de_vec3 p = de_v3_add(c3, de_v3_scale(d, r));
            f32 clip[4];
            for (u32 row = 0; row < 4; ++row)
                clip[row] = v.view_proj[row][0] * p.x + v.view_proj[row][1] * p.y +
                            v.view_proj[row][2] * p.z + v.view_proj[row][3];
            const f32 px = (clip[0] / clip[3] * 0.5f + 0.5f) * 800.0f;
            const f32 py = (clip[1] / clip[3] * 0.5f + 0.5f) * 600.0f;
            lo[0] = fminf(lo[0], px);
            hi[0] = fmaxf(hi[0], px);
            lo[1] = fminf(lo[1], py);
            hi[1] = fmaxf(hi[1], py);
        }
        for (u32 a = 0; a < 2; ++a) {
            const f32 span = rect[2 * a + 1] - rect[2 * a];
            const f32 tol = 1e-3f * (1.0f + fabsf(rect[2 * a]) + fabsf(rect[2 * a + 1]));
            EXPECT_TRUE(rect[2 * a] <= lo[a] + tol); /* contains */
            EXPECT_TRUE(rect[2 * a + 1] >= hi[a] - tol);
            /* Tight: random points come within a few percent of each edge. */
            EXPECT_TRUE(lo[a] - rect[2 * a] <= 0.05f * span + tol);
            EXPECT_TRUE(rect[2 * a + 1] - hi[a] <= 0.05f * span + tol);
        }
    }
    printf("%u rectangles checked\n", rects);
    EXPECT_GT(rects, 300u);
    /* The pixel-centre rule. */
    const de__vis_cull_view o = cpu_view((de_vec3){0, 0, 0}, (de_vec3){0, 1, 0}, true, 100, 100);
    /* Orthographic, 20 m over 100 px: 5 px per metre; the camera looks along +y, x right. A sphere
     * of 0.08 m (0.4 px) centred on a pixel centre covers it; centred between two, it does not. */
    const f32 px_m = 100.0f / 20.0f;
    const f32 on[3] = {(50.5f - 50.0f) / px_m, 10.0f, (50.5f - 50.0f) / px_m};
    const f32 off[3] = {(51.0f - 50.0f) / px_m, 10.0f, (51.0f - 50.0f) / px_m};
    EXPECT_FALSE(de__vis_meshlet_small_culled(&o, on, 0.08f, 100, 100));
    EXPECT_TRUE(de__vis_meshlet_small_culled(&o, off, 0.08f, 100, 100));
    EXPECT_FALSE(de__vis_meshlet_small_culled(&o, off, 0.12f, 100, 100)); /* 0.6 px: reaches */
}
