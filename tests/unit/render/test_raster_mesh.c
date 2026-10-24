/*
 * test_raster_mesh.c: the mesh-shader vis-buffer raster (DENG-234): the packed vis IDs and the
 * reverse-Z depth it writes, the two-phase ID ranges, double-sided and mirrored triangles,
 * quantized positions, the split of large phases and the cook of every target.
 * Executable test_render_raster_mesh; CTest names render_raster_mesh.<suite>.<test>.
 *
 * Meshes are built here: each meshlet is a 5 x 5 grid patch (25 vertices, 32 triangles, wound
 * counter-clockwise seen from the side its normal points to), with real meshlet bounds
 * (meshopt_computeMeshletBounds, the sphere widened like the cooker's) and every buffer the
 * raster reads: positions (F32X3, or UNORM16X4 in the mesh's AABB), DMSH v2 meshlet records, the
 * meshlet vertex buffer (indices into the positions, in reverse order so the indirection
 * matters) and the meshlet triangle bytes. A fake render world drives a GPU scene (DENG-177);
 * the vis frame (src/render/passes/vis/) culls and draws; a test pass reads back the vis IDs,
 * the depth and the visible list.
 *
 * The reference is a CPU rasterizer over every meshlet of every instance (no culling: culling
 * only drops what draws nothing): vertices snapped to 1/256 pixel (8 bits of sub-pixel precision,
 * as the GPUs), pixel centres, a top-left fill rule, reverse-Z depth with GREATER_EQUAL, the
 * shader's triangle-culling rule. Each GPU ID is decoded through the visible
 * list to (instance, meshlet, triangle) and compared per pixel; at least 99.9% must match (the
 * rest are edge ties between triangles of nearly equal depth, or pixel centres on an edge).
 *
 * GPU tests run per backend (_metal, _vulkan, _d3d12) and SKIP without the device, below the
 * Base tier or without mesh shaders. CI: Vulkan on lavapipe and D3D12 on the vendored WARP.
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
#include "gpu/shader/shader_format.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/graph/rg_internal.h"
#include "render/loaders/render_assets.h"
#include "render/passes/vis/vis_stages.h"
#include "render/scene/gpu_scene.h"
#include "render/shaders/engine_shaders.h"
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

#define GRID        5u
#define PATCH_VERTS (GRID * GRID)                    /* 25 */
#define PATCH_TRIS  (2u * (GRID - 1u) * (GRID - 1u)) /* 32 */
#define MAX_MESHES  8u
#define MAT_SINGLE  1u /* material handle ids of the fake materials */
#define MAT_DOUBLE  2u
#define NO_KEY      UINT64_MAX

static void *zalloc(size_t n) {
    void *p = calloc(1, n > 0 ? n : 1);
    if (p == NULL) {
        fprintf(stderr, "out of memory\n");
        abort();
    }
    return p;
}

typedef struct rng {
    u64 s;
} rng;

static u32 rnd(rng *g) {
    g->s = g->s * 6364136223846793005ull + 1442695040888963407ull;
    return (u32)(g->s >> 33);
}
static f32 rndf(rng *g, f32 lo, f32 hi) {
    return lo + (hi - lo) * (f32)(rnd(g) % 1000000u) / 1000000.0f;
}
static de_vec3 rnd_unit(rng *g) {
    for (;;) {
        const de_vec3 v = {rndf(g, -1, 1), rndf(g, -1, 1), rndf(g, -1, 1)};
        const f32 l = de_v3_length(v);
        if (l > 0.1f && l <= 1.0f)
            return de_v3_scale(v, 1.0f / l);
    }
}

/* ================================================================================================
 * Fake meshes
 * ============================================================================================== */

/* How a mesh's meshlets are laid out. */
typedef struct mesh_style {
    u32 meshlets;
    f32 half;                 /* patch centres lie in [-half, half]^3 */
    f32 patch_min, patch_max; /* patch edge length */
    f32 bend;                 /* curvature: up to bend / size */
    bool facing;              /* every patch flat with normal `normal` */
    de_vec3 normal;
    bool quantized; /* positions as UNORM16X4 (else F32X3) */
} mesh_style;

typedef struct fmesh {
    de_render_mesh rm;
    u32 meshlets, verts;
    f32 *pos; /* object-space positions as the GPU decodes them, by mesh vertex index */
    u32 *mlv; /* meshlet vertex buffer: per meshlet PATCH_VERTS mesh vertex indices */
    u8 *tri;  /* per meshlet PATCH_TRIS x 3 local indices */
    de_aabb bounds;
    de_gpu_buffer buf[4];
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

/* Builds mesh `fm` and its GPU buffers (UPLOAD, read through bindless). */
static void mesh_build(fmesh *fm, const mesh_style *st, rng *g, de_gpu_device *dev) {
    memset(fm, 0, sizeof *fm);
    const u32 nm = st->meshlets, nv = nm * PATCH_VERTS;
    fm->meshlets = nm;
    fm->verts = nv;
    fm->pos = (f32 *)zalloc((size_t)nv * 3u * sizeof(f32));
    fm->mlv = (u32 *)zalloc((size_t)nv * sizeof(u32));
    fm->tri = (u8 *)zalloc((size_t)nm * PATCH_TRIS * 3u);
    /* Patch vertices, stored in reverse order: local vertex i of meshlet m is mesh vertex
     * nv - 1 - (m * 25 + i). */
    f32 *raw = (f32 *)zalloc((size_t)nv * 3u * sizeof(f32));
    for (u32 ml = 0; ml < nm; ++ml) {
        const f32 s = rndf(g, st->patch_min, st->patch_max);
        const f32 lim = st->half;
        const de_vec3 p = {rndf(g, -lim, lim), rndf(g, -lim, lim), rndf(g, -lim, lim)};
        const de_vec3 n = st->facing ? st->normal : rnd_unit(g);
        const de_vec3 a = fabsf(n.z) < 0.9f ? (de_vec3){0, 0, 1} : (de_vec3){1, 0, 0};
        de_vec3 t = de_v3_cross(a, n);
        t = de_v3_scale(t, 1.0f / de_v3_length(t));
        const de_vec3 b = de_v3_cross(n, t); /* t x b = n */
        const f32 k = st->facing ? 0.0f : rndf(g, -st->bend, st->bend) / s;
        for (u32 j = 0; j < GRID; ++j)
            for (u32 i = 0; i < GRID; ++i) {
                const f32 x = ((f32)i / (f32)(GRID - 1u) - 0.5f) * s;
                const f32 y = ((f32)j / (f32)(GRID - 1u) - 0.5f) * s;
                const f32 z = k * (x * x + y * y);
                const u32 local = j * GRID + i, v = nv - 1u - (ml * PATCH_VERTS + local);
                fm->mlv[ml * PATCH_VERTS + local] = v;
                f32 *q = raw + 3u * (size_t)v;
                q[0] = p.x + x * t.x + y * b.x + z * n.x;
                q[1] = p.y + x * t.y + y * b.y + z * n.y;
                q[2] = p.z + x * t.z + y * b.z + z * n.z;
            }
        u8 *tr = fm->tri + (size_t)ml * PATCH_TRIS * 3u;
        u32 nt = 0;
        for (u32 j = 0; j + 1u < GRID; ++j)
            for (u32 i = 0; i + 1u < GRID; ++i) {
                const u8 v00 = (u8)(j * GRID + i), v10 = (u8)(v00 + 1u);
                const u8 v11 = (u8)(v10 + GRID), v01 = (u8)(v00 + GRID);
                const u8 tris[6] = {v00, v10, v11, v00, v11, v01}; /* counter-clockwise from +n */
                memcpy(tr + 3u * nt, tris, 6);
                nt += 2u;
            }
    }
    /* Positions as stored, and as the GPU decodes them. */
    de_aabb rb = de_aabb_empty();
    for (u32 v = 0; v < nv; ++v)
        rb = (de_aabb){de_v3_min(rb.min, (de_vec3){raw[3 * v], raw[3 * v + 1], raw[3 * v + 2]}),
                       de_v3_max(rb.max, (de_vec3){raw[3 * v], raw[3 * v + 1], raw[3 * v + 2]})};
    const f32 off[3] = {rb.min.x, rb.min.y, rb.min.z};
    f32 scale[3] = {rb.max.x - rb.min.x, rb.max.y - rb.min.y, rb.max.z - rb.min.z};
    const u32 vstride = st->quantized ? 8u : 12u;
    u8 *vbytes = (u8 *)zalloc((size_t)nv * vstride);
    for (u32 v = 0; v < nv; ++v) {
        for (u32 c = 0; c < 3; ++c) {
            if (!st->quantized) {
                fm->pos[3 * v + c] = raw[3 * v + c];
                put_f32(vbytes + (size_t)v * 12u + 4u * c, raw[3 * v + c]);
                continue;
            }
            if (!(scale[c] > 0.0f))
                scale[c] = 1.0f;
            f32 u = (raw[3 * v + c] - off[c]) / scale[c];
            u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
            const u16 q = (u16)lrintf(u * 65535.0f);
            memcpy(vbytes + (size_t)v * 8u + 2u * c, &q, 2);
            /* de_cooked_mesh_position and vis_decode: offset + q * step (DENG-235 fixed the
             * decode, which divided by 65535 again) */
            fm->pos[3 * v + c] = off[c] + (f32)q * (scale[c] / 65535.0f);
        }
    }
    free(raw);
    /* Meshlet records, from the decoded positions. */
    u8 *records = (u8 *)zalloc((size_t)nm * DE_COOKED_MESHLET_SIZE);
    fm->bounds = de_aabb_empty();
    for (u32 v = 0; v < nv; ++v) {
        const de_vec3 q = {fm->pos[3 * v], fm->pos[3 * v + 1], fm->pos[3 * v + 2]};
        fm->bounds.min = de_v3_min(fm->bounds.min, q);
        fm->bounds.max = de_v3_max(fm->bounds.max, q);
    }
    for (u32 ml = 0; ml < nm; ++ml) {
        const u32 *verts = fm->mlv + ml * PATCH_VERTS;
        const u8 *tr = fm->tri + (size_t)ml * PATCH_TRIS * 3u;
        const struct meshopt_Bounds bo =
            meshopt_computeMeshletBounds(verts, tr, PATCH_TRIS, fm->pos, nv, 12u);
        const f32 radius = contain_radius(bo.center, bo.radius, verts, PATCH_VERTS, fm->pos);
        u8 *r = records + (size_t)ml * DE_COOKED_MESHLET_SIZE;
        put_u32(r + 0, ml * PATCH_VERTS);
        put_u32(r + 4, ml * PATCH_TRIS * 3u);
        put_u32(r + 8, PATCH_VERTS | (PATCH_TRIS << 8));
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
    }
    /* GPU buffers. */
    const void *data[4] = {vbytes, records, fm->mlv, fm->tri};
    const u64 size[4] = {(u64)nv * vstride, (u64)nm * DE_COOKED_MESHLET_SIZE, (u64)nv * 4u,
                         (u64)nm * PATCH_TRIS * 3u};
    const u32 which[4] = {DE_RENDER_MESH_VERTICES, DE_RENDER_MESH_MESHLETS,
                          DE_RENDER_MESH_MESHLET_VERTICES, DE_RENDER_MESH_MESHLET_TRIANGLES};
    de_render_mesh *m = &fm->rm;
    for (u32 i = 0; i < 4; ++i) {
        fm->buf[i] = de_gpu_buffer_create(dev, &(de_gpu_buffer_desc){.size = size[i],
                                                                     .usage = DE_USAGE_STORAGE,
                                                                     .memory = DE_MEM_UPLOAD,
                                                                     .name = "raster test mesh"});
        ASSERT_TRUE(DE_HANDLE_VALID(fm->buf[i]));
        u8 *map = (u8 *)de_gpu_buffer_map(dev, fm->buf[i]);
        ASSERT_TRUE(map != NULL);
        memcpy(map, data[i], (size_t)size[i]);
        m->buffers[which[i]] = fm->buf[i];
        m->bindless[which[i]] = de_gpu_buffer_bindless(dev, fm->buf[i]);
        m->buffer_bytes[which[i]] = size[i];
    }
    free(vbytes);
    free(records);
    for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s)
        m->stream_offset[s] = UINT32_MAX;
    m->stream_offset[DE_COOKED_STREAM_POSITION] = 0;
    m->stream_format[DE_COOKED_STREAM_POSITION] =
        st->quantized ? DE_COOKED_VF_UNORM16X4 : DE_COOKED_VF_F32X3;
    m->vertex_count = nv;
    m->meshlet_count = nm;
    m->meshlet_vertex_count = nv;
    m->meshlet_triangle_bytes = nm * PATCH_TRIS * 3u;
    m->submesh_count = 1;
    m->lod_count = 1;
    m->lods[0] = (de_cooked_mesh_lod){
        .meshlet_count = nm, .triangle_count = PATCH_TRIS * nm, .vertex_count = nv};
    const f32 mn[3] = {fm->bounds.min.x, fm->bounds.min.y, fm->bounds.min.z};
    const f32 mx[3] = {fm->bounds.max.x, fm->bounds.max.y, fm->bounds.max.z};
    f32 d2 = 0.0f;
    for (u32 c = 0; c < 3; ++c) {
        m->bounds_min[c] = mn[c];
        m->bounds_max[c] = mx[c];
        m->sphere[c] = 0.5f * (mn[c] + mx[c]);
        m->position_offset[c] = st->quantized ? off[c] : 0.0f;
        m->position_scale[c] = st->quantized ? scale[c] / 65535.0f : 1.0f; /* one step of q */
        d2 += (mx[c] - mn[c]) * (mx[c] - mn[c]);
    }
    m->sphere[3] = 0.5f * sqrtf(d2);
}

static void mesh_free(fmesh *fm, de_gpu_device *dev) {
    for (u32 i = 0; i < 4; ++i)
        if (dev != NULL && DE_HANDLE_VALID(fm->buf[i]))
            de_gpu_buffer_destroy(dev, fm->buf[i]);
    free(fm->pos);
    free(fm->mlv);
    free(fm->tri);
    memset(fm, 0, sizeof *fm);
}

/* ================================================================================================
 * Fake render world
 * ============================================================================================== */

typedef struct world {
    de__render_world_frame f;
    u32 cap;
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

/* Slot `s`: mesh `mesh` (1-based) with transform `xf` and material `mat` (0: none). Marked
 * changed for the next frame. */
static void world_set(world *w, u32 s, u32 mesh, de_affine xf, u32 mat) {
    de__rw_instances *in = &w->f.instances;
    in->entity[s] = 1000u + s;
    in->mesh[s].id = mesh;
    in->transform[s] = xf;
    in->prev_transform[s] = xf;
    in->layer_mask[s] = 1u;
    in->bounds[s] = de_aabb_transform(g_mesh[mesh - 1u].bounds, &xf);
    in->flags[s] = (u8)DE__RW_INST_HAS_BOUNDS;
    for (u32 k = 0; k < DE__RW_MATERIALS; ++k)
        in->materials[(size_t)s * DE__RW_MATERIALS + k].id = mat;
    if (s >= in->count)
        in->count = s + 1u;
    bool listed = false;
    for (u32 i = 0; i < w->f.dirty_instance_count; ++i)
        listed |= w->f.dirty_instances[i] == s;
    if (!listed && w->f.frame > 0)
        w->f.dirty_instances[w->f.dirty_instance_count++] = s;
}

static void world_begin(world *w) {
    w->f.frame++;
    w->f.origin_changed = w->f.frame == 1;
    w->f.instances.live = 0;
    for (u32 s = 0; s < w->f.instances.count; ++s)
        w->f.instances.live += w->f.instances.entity[s] != 0;
}

static void world_end(world *w) {
    w->f.dirty_instance_count = 0;
}

/* ================================================================================================
 * Fixture
 * ============================================================================================== */

typedef struct rm_result {
    u32 *ids;   /* width x height vis IDs */
    f32 *depth; /* width x height */
    de__vis_meshlet *visible;
    u32 visible_stored, visible_alloc;
    de__vis_raster_args args[DE__VIS_PHASES];
    de__gs_instance *inst;
    u32 inst_count, inst_alloc;
    de__gs_material *mat;
    u32 mat_count, mat_alloc;
    u32 raster_path;
    de_mat4 view_proj;
} rm_result;

typedef struct rmx {
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
    rng r;
    de_vec3 eye, target;
    f32 fov;
    u32 width, height;
    rm_result res;
} rmx;

static void rmx_setup(rmx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "raster_mesh_test"}, &f->arena), DE_OK);
    de_error_clear();
    memset(g_mesh, 0, sizeof g_mesh);
    g_mesh_count = 0;
    memset(g_mat, 0, sizeof g_mat);
    g_mat[0].standard.double_sided = false;
    g_mat[1].standard.double_sided = true;
    f->eye = (de_vec3){0.0f, 0.0f, 0.0f};
    f->target = (de_vec3){0.0f, 1.0f, 0.0f};
    f->fov = 1.0f;
    f->width = 320;
    f->height = 192;
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

static void rmx_teardown(rmx *f) {
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
        EXPECT_EQ(validation_errors(f->dev), 0u);
        if (DE_HANDLE_VALID(f->rb))
            de_gpu_buffer_destroy(f->dev, f->rb);
        if (DE_HANDLE_VALID(f->fence))
            de_gpu_fence_destroy(f->dev, f->fence);
        de_gpu_wait_idle(f->dev);
        de_gpu_device_destroy(f->dev);
    }
    free(f->res.ids);
    free(f->res.depth);
    free(f->res.visible);
    free(f->res.inst);
    free(f->res.mat);
    world_free(&f->w);
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
    /* The cvars are process-wide: back to their defaults. */
    const char *const names[] = {"r.occlusion", "r.raster_tri_cull", "r.raster_path", "r.lod_bias"};
    const char *const values[] = {"1", "1", "auto", "0"};
    for (u32 i = 0; i < 4; ++i) {
        const de_cvar cv = de_cvar_find(names[i]);
        if (DE_HANDLE_VALID(cv))
            (void)de_cvar_set(cv, values[i]);
    }
    de__vis_meshlet_set_debug(NULL, NULL);
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

static bool cvar_set(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (!DE_HANDLE_VALID(cv))
        return false; /* a Dev-only cvar in Shipping */
    EXPECT_EQ(de_cvar_set(cv, value), DE_OK);
    return true;
}

/* Opens a device with mesh shaders (SKIPs without), a graph, a GPU scene and the vis frame. */
static void rmx_open(rmx *f, de_gpu_backend backend, u32 instances) {
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
    printf("device: \"%s\" (%s), mesh shaders %s\n", caps->device_name,
           de_gpu_backend_name(backend), caps->mesh_shaders ? "yes" : "no");
    render_d3d12_report(f->dev);
    if (!caps->base_tier_ok) {
        snprintf(why, sizeof why, "below the Base tier: %.400s", caps->base_tier_reason);
        SKIP(why);
    }
    if (!caps->mesh_shaders)
        SKIP("the device has no mesh shaders (the compute path draws there, DENG-235)");
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "rm"},
                            &f->g),
              DE_OK);
    f->fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(f->fence));
    de__gpu_scene_desc gd = {.device = f->dev,
                             .allocator = &f->alloc,
                             .resolver = {.mesh = fake_mesh,
                                          .material = fake_material,
                                          .texture_bindless = fake_texture},
                             .staging_bytes = (u64)16 << 20};
    gd.initial_capacity[DE__GS_INSTANCES] = instances;
    ASSERT_EQ(de__gpu_scene_create(&gd, &f->gs), DE_OK);
    ASSERT_EQ(de__vis_shared_create(
                  &(de__vis_shared_desc){.device = f->dev, .allocator = &f->alloc}, &f->vis),
              DE_OK);
    if (!f->vis->enabled) {
        snprintf(why, sizeof why, "the vis frame is off: %.600s", f->vis->why);
        SKIP(why);
    }
    ASSERT_TRUE(de__vis_raster_mesh_ready(f->vis));
    world_init(&f->w, instances);
    /* One LOD per mesh; occlusion off unless a test turns it on (phase 1 then draws all). */
    (void)cvar_set("r.lod_bias", "-7");
    (void)cvar_set("r.occlusion", "0");
}

static u32 rmx_mesh(rmx *f, const mesh_style *st) {
    ASSERT_TRUE(g_mesh_count < MAX_MESHES);
    mesh_build(&g_mesh[g_mesh_count], st, &f->r, f->dev);
    return ++g_mesh_count;
}

/* ---------- One frame ---------- */

typedef struct rb_pass {
    rmx *f;
    de_rg_resource ids, depth, visible, counters, args[DE__VIS_PHASES];
    u32 pitch;
    u64 ids_off, depth_off, visible_off, visible_bytes, counters_off, args_off;
} rb_pass;

static void readback_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    const de_gpu_buffer rb = p->f->rb;
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, p->ids_off, p->pitch, de_rg_texture(pass, p->ids), 0,
                                      0);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, p->depth_off, p->pitch,
                                      de_rg_texture(pass, p->depth), 0, 0);
    de_gpu_cmd_copy_buffer(cmd, rb, p->visible_off, de_rg_buffer(pass, p->visible), 0,
                           p->visible_bytes);
    de_gpu_cmd_copy_buffer(cmd, rb, p->counters_off, de_rg_buffer(pass, p->counters), 0,
                           DE__VIS_MESHLET_COUNTER_BYTES);
    for (u32 i = 0; i < DE__VIS_PHASES; ++i)
        if (DE_HANDLE_VALID(p->args[i]))
            de_gpu_cmd_copy_buffer(cmd, rb, p->args_off + 64u * i, de_rg_buffer(pass, p->args[i]),
                                   0, sizeof(de__vis_raster_args));
}

static void ensure_rb(rmx *f, u64 bytes) {
    if (bytes <= f->rb_size)
        return;
    if (DE_HANDLE_VALID(f->rb)) {
        de_gpu_wait_idle(f->dev);
        de_gpu_buffer_destroy(f->dev, f->rb);
    }
    f->rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = bytes,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK,
                                                               .name = "raster test readback"});
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

static u64 align_up(u64 v, u64 a) {
    return (v + a - 1u) / a * a;
}

/* Builds, runs and waits for one frame; fills f->res. */
static bool rmx_frame(rmx *f) {
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
                                  .frames_completed = n,
                                  .width = f->width,
                                  .height = f->height,
                                  .view_matrix = view,
                                  .proj = proj,
                                  .camera_position = f->eye,
                                  .origin = f->w.f.origin};
    de__vis_context *vc = NULL;
    if (de__vis_frame_build(f->vis, f->g, &d, &vc) != DE_OK || vc == NULL)
        return false;
    rm_result *res = &f->res;
    res->raster_path = vc->raster_path;
    res->view_proj = de_m4_mul(&proj, &view);
    rb_pass *rp = NULL;
    if (DE_HANDLE_VALID(vc->vis_ids)) {
        rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
        ASSERT_TRUE(rp != NULL);
        rp->f = f;
        rp->ids = vc->vis_ids;
        rp->depth = vc->depth;
        rp->visible = vc->visible_meshlets;
        rp->counters = vc->meshlet_counters;
        for (u32 i = 0; i < DE__VIS_PHASES; ++i)
            rp->args[i] = vc->raster_args[i];
        rp->pitch = (u32)align_up((u64)f->width * 4u, 256u);
        const u64 img = (u64)rp->pitch * f->height;
        rp->ids_off = 0;
        rp->depth_off = align_up(img, 512u);
        rp->counters_off = align_up(rp->depth_off + img, 512u);
        rp->args_off = rp->counters_off + 256u;
        rp->visible_off = rp->args_off + 256u;
        rp->visible_bytes = (u64)vc->visible_capacity * sizeof(de__vis_meshlet);
        ensure_rb(f, rp->visible_off + rp->visible_bytes);
        de_rg_pass *p =
            de_rg_add_pass(f->g, "test.readback", DE_QUEUE_GRAPHICS, readback_execute, rp);
        de_rg_read(p, rp->ids, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->depth, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->visible, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->counters, DE_ACCESS_COPY_SRC);
        for (u32 i = 0; i < DE__VIS_PHASES; ++i)
            if (DE_HANDLE_VALID(rp->args[i]))
                de_rg_read(p, rp->args[i], DE_ACCESS_COPY_SRC);
        de_rg_write(p, de_rg_import_buffer(f->g, f->rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
        /* The depth back to SHADER_READ before the command list ends: Vulkan validation checks
         * the HZB build's sampled read of it against its layout at the end of the command
         * buffer, not at the access (the renderer never copies it). */
        p = de_rg_add_pass(f->g, "test.restore", DE_QUEUE_GRAPHICS, NULL, NULL);
        de_rg_pass_side_effect(p);
        de_rg_read(p, rp->depth, DE_ACCESS_SHADER_READ);
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
    res->inst_count = de__gpu_scene_table(f->gs, DE__GS_INSTANCES).count;
    res->mat_count = de__gpu_scene_table(f->gs, DE__GS_MATERIALS).count;
    grow((void **)&res->inst, &res->inst_alloc, res->inst_count, sizeof(de__gs_instance));
    grow((void **)&res->mat, &res->mat_alloc, res->mat_count, sizeof(de__gs_material));
    for (u32 i = 0; i < res->inst_count; ++i)
        de__gpu_scene_debug_pack(f->gs, DE__GS_INSTANCES, i, &res->inst[i]);
    for (u32 i = 0; i < res->mat_count; ++i)
        de__gpu_scene_debug_pack(f->gs, DE__GS_MATERIALS, i, &res->mat[i]);
    de__gpu_scene_frame_end(f->gs, f->g, true);
    world_end(&f->w);
    if (de_gpu_fence_wait(f->dev, f->fence, n + 1, WAIT_NS) != DE_OK)
        return false;
    if (rp == NULL)
        return true;
    const u8 *m = (const u8 *)de_gpu_buffer_map(f->dev, f->rb);
    const size_t px = (size_t)f->width * f->height;
    if (res->ids == NULL) {
        res->ids = (u32 *)zalloc(px * 4u);
        res->depth = (f32 *)zalloc(px * 4u);
    }
    for (u32 y = 0; y < f->height; ++y) {
        memcpy(res->ids + (size_t)y * f->width, m + rp->ids_off + (u64)y * rp->pitch,
               (size_t)f->width * 4u);
        memcpy(res->depth + (size_t)y * f->width, m + rp->depth_off + (u64)y * rp->pitch,
               (size_t)f->width * 4u);
    }
    u32 counters[4];
    memcpy(counters, m + rp->counters_off, sizeof counters);
    for (u32 i = 0; i < DE__VIS_PHASES; ++i)
        memcpy(&res->args[i], m + rp->args_off + 64u * i, sizeof res->args[i]);
    const u32 asked = counters[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u];
    const u32 cap = (u32)(rp->visible_bytes / sizeof(de__vis_meshlet));
    res->visible_stored = asked < cap ? asked : cap;
    grow((void **)&res->visible, &res->visible_alloc, res->visible_stored, sizeof(de__vis_meshlet));
    if (res->visible_stored > 0)
        memcpy(res->visible, m + rp->visible_off, res->visible_stored * sizeof(de__vis_meshlet));
    return true;
}

/* ================================================================================================
 * CPU reference rasterizer
 * ============================================================================================== */

/* A pixel's (instance, meshlet, triangle). */
static u64 key_of(u32 inst, u32 meshlet, u32 tri) {
    return ((u64)inst << 32) | ((u64)meshlet << 8) | tri;
}

/* The key of GPU vis ID `id` through the visible list (NO_KEY: empty; bad IDs: UINT64_MAX - 1). */
static u64 gpu_key(const rmx *f, u32 id) {
    if (id == DE__VIS_ID_EMPTY)
        return NO_KEY;
    const u32 e = de__vis_id_meshlet(id);
    if (e >= f->res.visible_stored)
        return NO_KEY - 1u;
    return key_of(f->res.visible[e].instance, f->res.visible[e].meshlet, de__vis_id_triangle(id));
}

static f32 dot4(const f32 r[4], const f32 p[4]) {
    return r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3] * p[3];
}

typedef struct ref_image {
    u64 *key;
    f32 *depth;
} ref_image;

/* Edge function of (a, b) at p; positive on the counter-clockwise side in pixel space (y down:
 * the triangle is oriented so that its interior is positive). */
static f32 edge(f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Top-left rule for an interior-positive edge in y-down pixel space. */
static bool top_left(f32 ax, f32 ay, f32 bx, f32 by) {
    return (ay == by && bx < ax) || by < ay;
}

/* Every meshlet of every live instance, rasterized with the shader's culling rule. */
static void cpu_raster(const rmx *f, bool tri_cull, ref_image *out) {
    const u32 W = f->width, H = f->height;
    const size_t px = (size_t)W * H;
    out->key = (u64 *)zalloc(px * sizeof(u64));
    out->depth = (f32 *)zalloc(px * sizeof(f32));
    for (size_t i = 0; i < px; ++i)
        out->key[i] = NO_KEY;
    f32 vp[4][4];
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            vp[r][c] = f->res.view_proj.m[c * 4u + r];
    for (u32 s = 0; s < f->res.inst_count; ++s) {
        const de__gs_instance *in = &f->res.inst[s];
        if ((in->flags & DE__GS_INSTANCE_LIVE) == 0 || in->mesh == DE__GS_NONE)
            continue;
        const u32 mid = f->w.f.instances.mesh[s].id;
        if (mid == 0 || mid > g_mesh_count)
            continue;
        const fmesh *fm = &g_mesh[mid - 1u];
        const f32 *r0 = in->transform[0].v, *r1 = in->transform[1].v, *r2 = in->transform[2].v;
        const f32 det = r0[0] * (r1[1] * r2[2] - r1[2] * r2[1]) -
                        r0[1] * (r1[0] * r2[2] - r1[2] * r2[0]) +
                        r0[2] * (r1[0] * r2[1] - r1[1] * r2[0]);
        const f32 winding = det < 0.0f ? -1.0f : 1.0f;
        const u32 mat = in->materials[0].v[0];
        bool two_sided = false;
        if (mat != DE__GS_NONE && mat < f->res.mat_count) {
            const u32 fl = f->res.mat[mat].flags;
            two_sided = (fl & (DE__GS_MATERIAL_READY | DE__GS_MATERIAL_DOUBLE_SIDED)) ==
                        (DE__GS_MATERIAL_READY | DE__GS_MATERIAL_DOUBLE_SIDED);
        }
        for (u32 ml = 0; ml < fm->meshlets; ++ml) {
            f32 clip[PATCH_VERTS][4];
            for (u32 v = 0; v < PATCH_VERTS; ++v) {
                const f32 *p = fm->pos + 3u * (size_t)fm->mlv[ml * PATCH_VERTS + v];
                const f32 h[4] = {p[0], p[1], p[2], 1.0f};
                const f32 w4[4] = {dot4(r0, h), dot4(r1, h), dot4(r2, h), 1.0f};
                for (u32 k = 0; k < 4; ++k)
                    clip[v][k] = dot4(vp[k], w4);
            }
            for (u32 t = 0; t < PATCH_TRIS; ++t) {
                const u8 *tri = fm->tri + ((size_t)ml * PATCH_TRIS + t) * 3u;
                const f32 *c[3] = {clip[tri[0]], clip[tri[1]], clip[tri[2]]};
                if (!(c[0][3] > 0.0f && c[1][3] > 0.0f && c[2][3] > 0.0f))
                    continue; /* the scenes keep triangles in front of the camera */
                f32 nx[3], ny[3], nz[3], sx[3], sy[3];
                for (u32 k = 0; k < 3; ++k) {
                    nx[k] = c[k][0] / c[k][3];
                    ny[k] = c[k][1] / c[k][3];
                    nz[k] = c[k][2] / c[k][3];
                    /* Rasterizers snap vertices to 1/256 pixel (8 bits of sub-pixel precision). */
                    sx[k] = roundf((nx[k] * 0.5f + 0.5f) * (f32)W * 256.0f) / 256.0f;
                    sy[k] = roundf((0.5f - ny[k] * 0.5f) * (f32)H * 256.0f) / 256.0f;
                }
                const f32 area =
                    ((nx[1] - nx[0]) * (ny[2] - ny[0]) - (nx[2] - nx[0]) * (ny[1] - ny[0])) *
                    winding;
                if (tri_cull && (area == 0.0f || (!two_sided && area < 0.0f)))
                    continue;
                /* Orient so the interior is positive in y-down pixel space (CCW in NDC is
                 * clockwise there). */
                u32 i0 = 0, i1 = 1, i2 = 2;
                f32 a2 = edge(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
                if (a2 == 0.0f)
                    continue;
                if (a2 < 0.0f) {
                    i1 = 2;
                    i2 = 1;
                    a2 = -a2;
                }
                const f32 X[3] = {sx[i0], sx[i1], sx[i2]}, Y[3] = {sy[i0], sy[i1], sy[i2]};
                const f32 Z[3] = {nz[i0], nz[i1], nz[i2]};
                f32 lo_x = fminf(X[0], fminf(X[1], X[2])), hi_x = fmaxf(X[0], fmaxf(X[1], X[2]));
                f32 lo_y = fminf(Y[0], fminf(Y[1], Y[2])), hi_y = fmaxf(Y[0], fmaxf(Y[1], Y[2]));
                i32 x0 = (i32)floorf(lo_x), x1 = (i32)ceilf(hi_x);
                i32 y0 = (i32)floorf(lo_y), y1 = (i32)ceilf(hi_y);
                x0 = x0 < 0 ? 0 : x0;
                y0 = y0 < 0 ? 0 : y0;
                x1 = x1 > (i32)W - 1 ? (i32)W - 1 : x1;
                y1 = y1 > (i32)H - 1 ? (i32)H - 1 : y1;
                const bool tl0 = top_left(X[1], Y[1], X[2], Y[2]);
                const bool tl1 = top_left(X[2], Y[2], X[0], Y[0]);
                const bool tl2 = top_left(X[0], Y[0], X[1], Y[1]);
                for (i32 y = y0; y <= y1; ++y)
                    for (i32 x = x0; x <= x1; ++x) {
                        const f32 cx = (f32)x + 0.5f, cy = (f32)y + 0.5f;
                        const f32 w0 = edge(X[1], Y[1], X[2], Y[2], cx, cy);
                        const f32 w1 = edge(X[2], Y[2], X[0], Y[0], cx, cy);
                        const f32 w2 = edge(X[0], Y[0], X[1], Y[1], cx, cy);
                        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                            continue;
                        if ((w0 == 0.0f && !tl0) || (w1 == 0.0f && !tl1) || (w2 == 0.0f && !tl2))
                            continue;
                        const f32 z = (w0 * Z[0] + w1 * Z[1] + w2 * Z[2]) / a2;
                        if (z < 0.0f || z > 1.0f)
                            continue;
                        const size_t o = (size_t)y * W + (size_t)x;
                        if (out->key[o] != NO_KEY && z < out->depth[o])
                            continue;
                        out->depth[o] = z;
                        out->key[o] = key_of(s, ml, t);
                    }
            }
        }
    }
}

/* Triangle `key` at the centre of pixel (x, y): its depth, and the distance in pixels of the
 * centre from its nearest edge (negative outside). False when it is behind the camera. */
static bool tri_at(const rmx *f, u64 key, u32 x, u32 y, f32 *z, f32 *edge_px) {
    const u32 s = (u32)(key >> 32), ml = (u32)((key >> 8) & 0xFFFFFFu), t = (u32)(key & 0xFFu);
    const de__gs_instance *in = &f->res.inst[s];
    const fmesh *fm = &g_mesh[f->w.f.instances.mesh[s].id - 1u];
    f32 vp[4][4];
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            vp[r][c] = f->res.view_proj.m[c * 4u + r];
    const u8 *tri = fm->tri + ((size_t)ml * PATCH_TRIS + t) * 3u;
    f32 X[3], Y[3], Z[3];
    for (u32 k = 0; k < 3; ++k) {
        const f32 *p = fm->pos + 3u * (size_t)fm->mlv[ml * PATCH_VERTS + tri[k]];
        const f32 h[4] = {p[0], p[1], p[2], 1.0f};
        const f32 w4[4] = {dot4(in->transform[0].v, h), dot4(in->transform[1].v, h),
                           dot4(in->transform[2].v, h), 1.0f};
        f32 c[4];
        for (u32 j = 0; j < 4; ++j)
            c[j] = dot4(vp[j], w4);
        if (!(c[3] > 0.0f))
            return false;
        X[k] = (c[0] / c[3] * 0.5f + 0.5f) * (f32)f->width;
        Y[k] = (0.5f - c[1] / c[3] * 0.5f) * (f32)f->height;
        Z[k] = c[2] / c[3];
    }
    f32 a2 = edge(X[0], Y[0], X[1], Y[1], X[2], Y[2]);
    if (a2 == 0.0f)
        return false;
    const f32 sg = a2 < 0.0f ? -1.0f : 1.0f;
    const f32 cx = (f32)x + 0.5f, cy = (f32)y + 0.5f;
    f32 w[3], m = 1e30f;
    for (u32 k = 0; k < 3; ++k) {
        const u32 a = (k + 1u) % 3u, b = (k + 2u) % 3u;
        w[k] = edge(X[a], Y[a], X[b], Y[b], cx, cy) * sg;
        const f32 len = sqrtf((X[b] - X[a]) * (X[b] - X[a]) + (Y[b] - Y[a]) * (Y[b] - Y[a]));
        const f32 d = len > 0.0f ? w[k] / len : 0.0f;
        m = d < m ? d : m;
    }
    *edge_px = m;
    *z = (w[0] * Z[0] + w[1] * Z[1] + w[2] * Z[2]) / (a2 * sg);
    return true;
}

typedef struct cmp_out {
    u32 pixels, covered_gpu, covered_cpu, same, bad_ids, ties, unexplained;
} cmp_out;

/* True when a differing pixel is a tie: the two triangles there are at nearly the same depth (an
 * intersection of two surfaces), or the pixel centre lies on (within float rounding of) an edge
 * of one of them, so either answer is right. */
static bool is_tie(const rmx *f, u64 gk, u64 ck, u32 x, u32 y) {
    const f32 edge_eps = 2e-3f;
    f32 zg = 0.0f, zc = 0.0f, eg = 1.0f, ec = 1.0f;
    const bool hg = gk != NO_KEY && tri_at(f, gk, x, y, &zg, &eg);
    const bool hc = ck != NO_KEY && tri_at(f, ck, x, y, &zc, &ec);
    if ((hg && fabsf(eg) < edge_eps) || (hc && fabsf(ec) < edge_eps))
        return true;
    if (hg && hc && fabsf(zg - zc) <= 1e-6f + 1e-4f * fabsf(zc))
        return true;
    return false;
}

/* Compares the GPU IDs with the reference, pixel by pixel. */
static cmp_out compare(const rmx *f, bool tri_cull) {
    ref_image ref;
    cpu_raster(f, tri_cull, &ref);
    cmp_out o = {0};
    const size_t px = (size_t)f->width * f->height;
    o.pixels = (u32)px;
    for (size_t i = 0; i < px; ++i) {
        const u64 g = gpu_key(f, f->res.ids[i]);
        o.bad_ids += g == NO_KEY - 1u;
        o.covered_gpu += g != NO_KEY;
        o.covered_cpu += ref.key[i] != NO_KEY;
        if (g == ref.key[i]) {
            ++o.same;
            continue;
        }
        if (g != NO_KEY - 1u && is_tie(f, g, ref.key[i], (u32)(i % f->width), (u32)(i / f->width)))
            ++o.ties;
        else
            ++o.unexplained;
    }
    printf("  %u pixels: %u covered (CPU %u), %u differ (%u ties, %u other)\n", o.pixels,
           o.covered_gpu, o.covered_cpu, o.pixels - o.same, o.ties, o.unexplained);
    free(ref.key);
    free(ref.depth);
    return o;
}

/* Every pixel matches, except ties (edges and surface intersections), which are at most 0.1%. */
static void expect_match(const cmp_out *o) {
    EXPECT_EQ(o->bad_ids, 0u);
    EXPECT_EQ(o->unexplained, 0u);
    EXPECT_GE((u64)o->same * 1000u, (u64)o->pixels * 999u);
}

static de_affine rnd_affine(rng *g, de_vec3 lo, de_vec3 hi, f32 smin, f32 smax, bool mirror) {
    const de_vec3 t = {rndf(g, lo.x, hi.x), rndf(g, lo.y, hi.y), rndf(g, lo.z, hi.z)};
    const de_vec3 axis = rnd_unit(g);
    const f32 ang = rndf(g, -3.0f, 3.0f), sn = sinf(ang * 0.5f);
    const de_quat q = {axis.x * sn, axis.y * sn, axis.z * sn, cosf(ang * 0.5f)};
    de_vec3 s = {rndf(g, smin, smax), rndf(g, smin, smax), rndf(g, smin, smax)};
    if (mirror)
        s.x = -s.x;
    return de_affine_from_trs(t, q, s);
}

/* ================================================================================================
 * GPU tests
 * ============================================================================================== */

/* raster_mesh_ids_match_cpu_ref: 300 instances of three meshes (bent patches in random
 * directions; single- and double-sided materials; some mirrored) spread so that few surfaces
 * intersect (an intersection is a depth tie), two cameras. */
static void ids_match_cpu_ref(rmx *f, de_gpu_backend b) {
    rmx_open(f, b, 300u);
    const mesh_style st = {
        .meshlets = 16, .half = 1.5f, .patch_min = 0.4f, .patch_max = 1.0f, .bend = 0.3f};
    u32 ids[3];
    for (u32 i = 0; i < 3; ++i)
        ids[i] = rmx_mesh(f, &st);
    for (u32 s = 0; s < 300u; ++s) {
        const de_affine xf = rnd_affine(&f->r, (de_vec3){-20, 10, -8}, (de_vec3){20, 60, 8}, 0.6f,
                                        1.6f, s % 7u == 3u);
        world_set(&f->w, s, ids[s % 3u], xf, s % 4u == 0 ? MAT_DOUBLE : MAT_SINGLE);
    }
    ASSERT_TRUE(rmx_frame(f));
    EXPECT_EQ(f->res.raster_path, (u32)DE__VIS_RASTER_MESH);
    cmp_out o = compare(f, true);
    expect_match(&o);
    EXPECT_GT(o.covered_gpu, o.pixels / 4u);
    /* Another camera on the same scene. */
    f->eye = (de_vec3){-12.0f, 2.0f, 4.0f};
    f->target = (de_vec3){4.0f, 24.0f, -1.0f};
    ASSERT_TRUE(rmx_frame(f));
    o = compare(f, true);
    expect_match(&o);
    EXPECT_GT(o.covered_gpu, o.pixels / 8u);
}

/* raster_mesh_reverse_z_clear: a near and a far quad; the cleared background is depth 0 and
 * DE__VIS_ID_EMPTY; the near quad has the larger depth and wins where they overlap. */
static void reverse_z_clear(rmx *f, de_gpu_backend b) {
    rmx_open(f, b, 4u);
    /* One flat patch facing -y (towards the camera at the origin looking down +y). */
    const mesh_style st = {.meshlets = 1,
                           .half = 0.0f,
                           .patch_min = 2.0f,
                           .patch_max = 2.0f,
                           .facing = true,
                           .normal = {0, -1, 0}};
    const u32 m = rmx_mesh(f, &st);
    const de_quat q0 = {0, 0, 0, 1};
    world_set(&f->w, 0, m, de_affine_from_trs((de_vec3){-0.6f, 5.0f, 0.0f}, q0, (de_vec3){1, 1, 1}),
              MAT_SINGLE);
    world_set(&f->w, 1, m, de_affine_from_trs((de_vec3){0.6f, 12.0f, 0.0f}, q0, (de_vec3){3, 3, 3}),
              MAT_SINGLE);
    ASSERT_TRUE(rmx_frame(f));
    const u32 W = f->width, H = f->height;
    u32 empty = 0, near_px = 0, far_px = 0;
    f32 near_depth = 0.0f, far_depth = 1.0f;
    for (u32 i = 0; i < W * H; ++i) {
        const u64 k = gpu_key(f, f->res.ids[i]);
        if (k == NO_KEY) {
            ++empty;
            EXPECT_TRUE(f->res.depth[i] == 0.0f); /* cleared reverse-Z far */
            continue;
        }
        const u32 inst = (u32)(k >> 32);
        if (inst == 0) {
            ++near_px;
            near_depth = f->res.depth[i] > near_depth ? f->res.depth[i] : near_depth;
        } else {
            ++far_px;
            far_depth = f->res.depth[i] < far_depth ? f->res.depth[i] : far_depth;
        }
        EXPECT_TRUE(f->res.depth[i] > 0.0f);
    }
    /* The corners are background. */
    EXPECT_EQ(f->res.ids[0], DE__VIS_ID_EMPTY);
    EXPECT_EQ(f->res.ids[W * H - 1u], DE__VIS_ID_EMPTY);
    EXPECT_GT(empty, 0u);
    EXPECT_GT(near_px, 100u);
    EXPECT_GT(far_px, 100u);
    EXPECT_TRUE(near_depth > far_depth); /* nearer is larger */
    /* The centre column between them: the near quad covers the far one. */
    const cmp_out o = compare(f, true);
    expect_match(&o);
}

/* raster_mesh_two_phase_unique_ids: occlusion on; the camera turns a little between frames, so
 * phase 1 draws what was visible and phase 2 what came into view. Phase 2's IDs follow phase 1's
 * entries, every entry names a different meshlet, and the image equals the reference. */
static void two_phase_unique_ids(rmx *f, de_gpu_backend b) {
    rmx_open(f, b, 400u);
    (void)cvar_set("r.occlusion", "1");
    const mesh_style st = {
        .meshlets = 12, .half = 1.2f, .patch_min = 0.4f, .patch_max = 0.9f, .bend = 0.2f};
    const u32 m = rmx_mesh(f, &st);
    for (u32 s = 0; s < 400u; ++s)
        world_set(&f->w, s, m,
                  rnd_affine(&f->r, (de_vec3){-40, 6, -8}, (de_vec3){40, 40, 8}, 0.7f, 1.4f, false),
                  MAT_DOUBLE);
    f->target = (de_vec3){-0.25f, 1.0f, 0.0f};
    ASSERT_TRUE(rmx_frame(f));                /* a new view: phase 2 draws everything */
    ASSERT_TRUE(rmx_frame(f));                /* history */
    f->target = (de_vec3){0.15f, 1.0f, 0.0f}; /* a turn of ~22 degrees: not a cut */
    ASSERT_TRUE(rmx_frame(f));
    const u32 n1 = f->res.args[0].count, first2 = f->res.args[1].first, n2 = f->res.args[1].count;
    printf("  phase 1: %u meshlets, phase 2: %u from entry %u\n", n1, n2, first2);
    EXPECT_GT(n1, 0u);
    EXPECT_GT(n2, 0u);
    EXPECT_EQ(f->res.args[0].first, 0u);
    EXPECT_EQ(first2, n1);
    EXPECT_EQ(f->res.visible_stored, n1 + n2);
    /* Every entry names a different (instance, meshlet). */
    u64 *keys = (u64 *)zalloc((size_t)f->res.visible_stored * sizeof(u64));
    for (u32 e = 0; e < f->res.visible_stored; ++e)
        keys[e] = key_of(f->res.visible[e].instance, f->res.visible[e].meshlet, 0);
    u32 dup = 0;
    for (u32 i = 0; i < f->res.visible_stored; ++i)
        for (u32 j = i + 1u; j < f->res.visible_stored; ++j)
            dup += keys[i] == keys[j];
    free(keys);
    EXPECT_EQ(dup, 0u);
    /* Pixels of both phases, and IDs inside the two ranges. */
    u32 p1 = 0, p2 = 0, outside = 0;
    for (u32 i = 0; i < f->width * f->height; ++i) {
        const u32 id = f->res.ids[i];
        if (id == DE__VIS_ID_EMPTY)
            continue;
        const u32 e = de__vis_id_meshlet(id);
        p1 += e < n1;
        p2 += e >= first2 && e < first2 + n2;
        outside += e >= first2 + n2;
    }
    printf("  pixels: %u from phase 1, %u from phase 2\n", p1, p2);
    EXPECT_GT(p1, 0u);
    EXPECT_GT(p2, 0u);
    EXPECT_EQ(outside, 0u);
    const cmp_out o = compare(f, true);
    expect_match(&o);
}

/* raster_mesh_double_sided: patches facing away from the camera: single-sided ones draw nothing,
 * double-sided ones draw; a mirrored instance keeps its front faces (the front patch draws, the
 * back one does not); r.raster_tri_cull 0 draws back faces too. */
static void double_sided(rmx *f, de_gpu_backend b) {
    rmx_open(f, b, 8u);
    const mesh_style away = {.meshlets = 1,
                             .half = 0.0f,
                             .patch_min = 1.6f,
                             .patch_max = 1.6f,
                             .facing = true,
                             .normal = {0, 1, 0}}; /* the camera looks at -n */
    const mesh_style front = {.meshlets = 1,
                              .half = 0.0f,
                              .patch_min = 1.6f,
                              .patch_max = 1.6f,
                              .facing = true,
                              .normal = {0, -1, 0}};
    const u32 ma = rmx_mesh(f, &away), mf = rmx_mesh(f, &front);
    const de_quat q0 = {0, 0, 0, 1};
    const de_vec3 one = {1, 1, 1}, mirror = {-1, 1, 1};
    world_set(&f->w, 0, ma, de_affine_from_trs((de_vec3){-2.4f, 8, 1.0f}, q0, one), MAT_SINGLE);
    world_set(&f->w, 1, ma, de_affine_from_trs((de_vec3){-0.8f, 8, 1.0f}, q0, one), MAT_DOUBLE);
    world_set(&f->w, 2, mf, de_affine_from_trs((de_vec3){0.8f, 8, 1.0f}, q0, mirror), MAT_SINGLE);
    world_set(&f->w, 3, ma, de_affine_from_trs((de_vec3){2.4f, 8, 1.0f}, q0, mirror), MAT_SINGLE);
    world_set(&f->w, 4, mf, de_affine_from_trs((de_vec3){0.0f, 8, -1.0f}, q0, one), MAT_SINGLE);
    world_set(&f->w, 5, mf, de_affine_from_trs((de_vec3){1.6f, 8, -1.0f}, q0, one), 0u);
    ASSERT_TRUE(rmx_frame(f));
    u32 per[6] = {0};
    for (u32 i = 0; i < f->width * f->height; ++i) {
        const u64 k = gpu_key(f, f->res.ids[i]);
        if (k != NO_KEY && (k >> 32) < 6u)
            ++per[k >> 32];
    }
    printf("  pixels per instance: %u %u %u %u %u %u\n", per[0], per[1], per[2], per[3], per[4],
           per[5]);
    EXPECT_EQ(per[0], 0u);  /* back face, single-sided */
    EXPECT_GT(per[1], 50u); /* back face, double-sided */
    EXPECT_GT(per[2], 50u); /* front patch, mirrored: still its front */
    EXPECT_EQ(per[3], 0u);  /* back patch, mirrored: still its back */
    EXPECT_GT(per[4], 50u); /* front face */
    EXPECT_GT(per[5], 50u); /* the default material (DE_GPU_NONE) draws its front */
    cmp_out o = compare(f, true);
    expect_match(&o);
    /* Without triangle culling every face draws (the debug cvar is not in Shipping). */
    if (cvar_set("r.raster_tri_cull", "0")) {
        ASSERT_TRUE(rmx_frame(f));
        u32 back = 0;
        for (u32 i = 0; i < f->width * f->height; ++i) {
            const u64 k = gpu_key(f, f->res.ids[i]);
            back += k != NO_KEY && ((k >> 32) == 0u || (k >> 32) == 3u);
        }
        /* Meshlet culling's cone test may still drop whole back-facing meshlets of resolved
         * single-sided materials: only the mirrored one (never cone-culled) must show. */
        EXPECT_GT(back, 50u);
        o = compare(f, false);
        EXPECT_EQ(o.bad_ids, 0u);
        (void)cvar_set("r.raster_tri_cull", "1");
    }
}

/* raster_mesh_quantized_positions: the same scene with UNORM16X4 positions; the IDs match the
 * reference built from the dequantized positions. */
static void quantized_positions(rmx *f, de_gpu_backend b) {
    rmx_open(f, b, 120u);
    const mesh_style st = {.meshlets = 24,
                           .half = 2.0f,
                           .patch_min = 0.5f,
                           .patch_max = 1.2f,
                           .bend = 0.4f,
                           .quantized = true};
    const u32 m = rmx_mesh(f, &st);
    for (u32 s = 0; s < 120u; ++s)
        world_set(&f->w, s, m,
                  rnd_affine(&f->r, (de_vec3){-10, 8, -5}, (de_vec3){10, 30, 5}, 0.8f, 1.5f, false),
                  MAT_SINGLE);
    ASSERT_TRUE(rmx_frame(f));
    EXPECT_EQ(g_mesh[m - 1u].rm.stream_format[DE_COOKED_STREAM_POSITION],
              (u32)DE_COOKED_VF_UNORM16X4);
    const cmp_out o = compare(f, true);
    expect_match(&o);
    EXPECT_GT(o.covered_gpu, o.pixels / 8u);
}

/* raster_mesh_split_dispatch: a visible list larger than one mesh dispatch takes (2^22 + 1
 * entries): the phase is drawn by two dispatches through the split kernel, with the same image. */
static void split_dispatch(rmx *f, de_gpu_backend b) {
    rmx_open(f, b, 40u);
    const mesh_style st = {
        .meshlets = 8, .half = 1.0f, .patch_min = 0.5f, .patch_max = 1.0f, .bend = 0.2f};
    const u32 m = rmx_mesh(f, &st);
    for (u32 s = 0; s < 40u; ++s)
        world_set(&f->w, s, m,
                  rnd_affine(&f->r, (de_vec3){-6, 6, -3}, (de_vec3){6, 20, 3}, 0.8f, 1.5f, false),
                  MAT_SINGLE);
    de__vis_meshlet_set_debug(
        f->vis, &(de__vis_meshlet_debug){.capacity_fixed = DE__VIS_MESH_GROUPS_MAX + 1u});
    ASSERT_TRUE(rmx_frame(f));
    EXPECT_EQ(de__vis_mesh_dispatch_count(DE__VIS_MESH_GROUPS_MAX + 1u), 2u);
    const cmp_out o = compare(f, true);
    expect_match(&o);
    EXPECT_GT(o.covered_gpu, 1000u);
    de__vis_meshlet_set_debug(f->vis, NULL);
}

#define GPU_TESTS(backend_suffix, backend)                                                         \
    TEST_F(raster_mesh, ids_match_cpu_ref_##backend_suffix, rmx) {                                 \
        ids_match_cpu_ref(f, backend);                                                             \
    }                                                                                              \
    TEST_F(raster_mesh, reverse_z_clear_##backend_suffix, rmx) {                                   \
        reverse_z_clear(f, backend);                                                               \
    }                                                                                              \
    TEST_F(raster_mesh, two_phase_unique_ids_##backend_suffix, rmx) {                              \
        two_phase_unique_ids(f, backend);                                                          \
    }                                                                                              \
    TEST_F(raster_mesh, double_sided_##backend_suffix, rmx) {                                      \
        double_sided(f, backend);                                                                  \
    }                                                                                              \
    TEST_F(raster_mesh, quantized_positions_##backend_suffix, rmx) {                               \
        quantized_positions(f, backend);                                                           \
    }                                                                                              \
    TEST_F(raster_mesh, split_dispatch_##backend_suffix, rmx) {                                    \
        split_dispatch(f, backend);                                                                \
    }

GPU_TESTS(metal, DE_GPU_METAL)
GPU_TESTS(vulkan, DE_GPU_VULKAN)
GPU_TESTS(d3d12, DE_GPU_D3D12)

/* DENG-615: full type reflection can omit the production mesh's bindings only when
 * Metal shader instrumentation is active. Keep the ordinary noninstrumented case too. */
TEST_F(raster_mesh, ids_match_cpu_ref_metal_shader_validation, rmx) {
#if defined(__APPLE__)
    if (getenv("CI"))
        SKIP("native Metal shader-validation repro; hosted CI GPU instrumentation is unsafe");
    ASSERT_EQ(setenv("MTL_SHADER_VALIDATION", "1", 1), 0);
#endif
    ids_match_cpu_ref(f, DE_GPU_METAL);
}

/* ================================================================================================
 * CPU tests
 * ============================================================================================== */

/* The split of a phase into mesh dispatches of at most 2^22 groups. */
TEST(raster_mesh, split_cpu) {
    EXPECT_EQ(de__vis_mesh_dispatch_count(0), 1u);
    EXPECT_EQ(de__vis_mesh_dispatch_count(65536u), 1u);
    EXPECT_EQ(de__vis_mesh_dispatch_count(DE__VIS_MESH_GROUPS_MAX), 1u);
    EXPECT_EQ(de__vis_mesh_dispatch_count(DE__VIS_MESH_GROUPS_MAX + 1u), 2u);
    EXPECT_EQ(de__vis_mesh_dispatch_count(DE__VIS_ID_MAX_VISIBLE_MESHLETS), 8u);
    static const u32 counts[] = {0,
                                 1,
                                 65535,
                                 65536,
                                 DE__VIS_MESH_GROUPS_MAX,
                                 DE__VIS_MESH_GROUPS_MAX + 1u,
                                 3u * DE__VIS_MESH_GROUPS_MAX + 7u,
                                 DE__VIS_ID_MAX_VISIBLE_MESHLETS};
    for (u32 i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
        const u32 c = counts[i], n = de__vis_mesh_dispatch_count(c);
        u64 covered = 0;
        for (u32 k = 0; k < n + 1u; ++k) {
            u32 x = 0, y = 0;
            de__vis_mesh_dispatch_groups(c, k, &x, &y);
            EXPECT_GE(x, 1u);
            EXPECT_GE(y, 1u);
            EXPECT_LE(x, DE__VIS_MAX_GROUPS_X);
            EXPECT_LE((u64)x * y, (u64)DE__VIS_MESH_GROUPS_MAX + DE__VIS_MAX_GROUPS_X);
            const u64 start = (u64)k * DE__VIS_MESH_GROUPS_MAX;
            const u64 want =
                c > start ? (c - start < DE__VIS_MESH_GROUPS_MAX ? c - start
                                                                 : (u64)DE__VIS_MESH_GROUPS_MAX)
                          : 0u;
            EXPECT_GE((u64)x * y, want); /* every meshlet of the dispatch has a group */
            covered += want;
        }
        EXPECT_EQ(covered, (u64)c);
    }
}

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

/* Every `public static const uint` of vis_decode.slang and vis_mesh.slang equals its C mirror;
 * RasterMeshParams is 8 uints. */
TEST(raster_mesh, layout_match) {
    u32 count = 0, found = 0;
    const de__vis_layout_const *k = de__vis_raster_layout_consts(&count);
    static const char *const files[2] = {"visbuffer/vis_decode.slang", "visbuffer/vis_mesh.slang"};
    for (u32 fi = 0; fi < 2; ++fi) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", DE_VIS_TEST_SHADER_DIR, files[fi]);
        char *text = read_text(path);
        ASSERT_TRUE(text != NULL);
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
                printf("  %s is not in de__vis_raster_layout_consts\n", name);
            EXPECT_TRUE(ok);
            ++found;
        }
        if (fi == 1) {
            const char *ps = strstr(text, "struct RasterMeshParams");
            const char *pe = ps != NULL ? strstr(ps, "};") : NULL;
            u32 fields = 0;
            for (const char *ln = ps; ln != NULL && pe != NULL && ln < pe;
                 ln = strchr(ln + 1, '\n'))
                fields += strncmp(ln, "\n    uint ", 10) == 0;
            EXPECT_EQ(fields, 8u);
        }
        free(text);
    }
    EXPECT_EQ(found, count);
}

/* raster_mesh_shader_cook_all_targets: the build-time cook of "vis_mesh" holds every entry for
 * each target this platform cooks (macOS: metallib + SPIR-V; Linux: SPIR-V; Windows: SPIR-V, and
 * DXIL whenever the other engine shaders have it), with the mesh shader's reflection. */
TEST(raster_mesh, shader_cook_all_targets) {
    const de__engine_shader_info *info = de__engine_shader_find("vis_mesh");
    ASSERT_TRUE(info != NULL);
    if (info->blob == NULL)
        SKIP("this build embeds no engine shaders (no build-time cook)");
    const u8 *blob = info->blob;
    de__shader_header h;
    memcpy(&h, blob, sizeof h);
    ASSERT_EQ(h.magic, DE__SHADER_MAGIC);
    ASSERT_EQ(h.version, DE__SHADER_VERSION);
    const u8 *entries = blob + sizeof h;
    const u8 *records = entries + (size_t)h.entry_count * sizeof(de__shader_entry) +
                        (size_t)h.permutation_count * sizeof(de__shader_permutation);
    const char *strings = (const char *)blob + h.strings_offset;
    static const char *const names[5] = {"ms_vis", "ps_vis", "vs_vis_clear", "ps_vis_clear",
                                         "cs_raster_mesh_split"};
    bool want[DE__SHADER_TARGET_COUNT_] = {false};
    want[DE__SHADER_TARGET_SPIRV] = true;
#if defined(__APPLE__)
    want[DE__SHADER_TARGET_METALLIB] = true;
#endif
    /* DXIL: whenever the build cooked it for the other engine shaders (DXC found). */
    const de__engine_shader_info *other = de__engine_shader_find("vis_meshlet_cull");
    if (other != NULL && other->blob != NULL) {
        de__shader_header oh;
        memcpy(&oh, other->blob, sizeof oh);
        const u8 *orec = other->blob + sizeof oh +
                         (size_t)oh.entry_count * sizeof(de__shader_entry) +
                         (size_t)oh.permutation_count * sizeof(de__shader_permutation);
        for (u32 i = 0; i < oh.record_count; ++i) {
            de__shader_record r;
            memcpy(&r, orec + (size_t)i * sizeof r, sizeof r);
            if (r.target == DE__SHADER_TARGET_DXIL)
                want[DE__SHADER_TARGET_DXIL] = true;
        }
    }
    for (u32 t = 1; t < DE__SHADER_TARGET_COUNT_; ++t) {
        if (!want[t])
            continue;
        for (u32 n = 0; n < 5; ++n) {
            bool found = false;
            for (u32 i = 0; i < h.record_count; ++i) {
                de__shader_record r;
                memcpy(&r, records + (size_t)i * sizeof r, sizeof r);
                de__shader_entry e;
                memcpy(&e, entries + (size_t)r.entry * sizeof e, sizeof e);
                if (r.target != t || strcmp(strings + e.name, names[n]) != 0)
                    continue;
                found = r.code_size > 0;
                if (n == 0) {
                    EXPECT_EQ(r.stage, (u32)DE__SHADER_STAGE_MESH);
                    EXPECT_EQ(r.threadgroup[0], 64u);
                    EXPECT_EQ(r.push_constant_bytes, 32u);
                }
            }
            if (!found)
                printf("  target %u has no %s\n", t, names[n]);
            EXPECT_TRUE(found);
        }
    }
}
