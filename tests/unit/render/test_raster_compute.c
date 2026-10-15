/*
 * test_raster_compute.c: the compute-expansion raster path of the visibility buffer (DENG-235):
 * the vis-ID image and the reverse-Z depth against a CPU rasterizer, both culling phases, the
 * overflow clamp of the triangle buffers, and (once DENG-234's mesh path exists) equality with the
 * mesh path. Executable test_render_raster_compute; CTest names
 * render_raster_compute.raster_compute.<test>.
 *
 * Meshes are built here from real triangles: each meshlet is a small, possibly bent 5 x 5 grid
 * patch (32 triangles, 25 vertices), with positions as F32X3 or quantized UNORM16X4, its bounds
 * from meshopt_computeMeshletBounds (sphere widened as the cooker does), and every DMSH v2 section
 * the raster reads (vertices, meshlet records, meshlet vertices, meshlet triangles) in its own
 * buffer. A fake render world drives a GPU scene; the vis frame (src/render/passes/vis/) runs
 * instance and meshlet culling, the compute raster in both phases and the HZB; a test pass reads
 * back the vis ID, the depth, the visible-meshlet list and the meshlet counters.
 *
 * The CPU reference rasterizes every triangle of every instance (the positions the GPU decodes,
 * the instance rows and the view-projection the GPU scene uploaded) at pixel centres, with the
 * raster's facing rule (back faces of single-sided materials and zero-area triangles are not
 * drawn; mirrored instances flip the winding) and reverse-Z depth (nearest wins). A GPU pixel
 * decodes through the visible list to (instance, meshlet, triangle). Pixels may differ only where
 * the reference is ambiguous (a pixel centre within 1e-3 px of an edge, or a depth tie), and at
 * most 0.1% of all pixels may differ.
 *
 * GPU tests run per backend (_metal, _vulkan, _d3d12; SKIP without the device or without the
 * engine shaders). CI: Vulkan on lavapipe and D3D12 on the vendored WARP in the golden jobs.
 * moltenvk_smoke is a small scene on the Vulkan backend: MoltenVK on macOS, lavapipe on Linux.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "de_alloc_guard.h"
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
#define PATCH_VERT (GRID * GRID)                    /* 25 */
#define PATCH_TRIS (2u * (GRID - 1u) * (GRID - 1u)) /* 32 */
#define MAX_MESHES 8u
#define MAT_SINGLE 1u /* material handle ids of the fake materials */
#define MAT_DOUBLE 2u

typedef struct mesh_style {
    u32 meshlets;
    f32 half;                 /* patch centres lie in [-half, half]^3 */
    f32 patch_min, patch_max; /* patch edge length */
    f32 bend;                 /* curvature: up to bend / size */
    bool quantized;           /* UNORM16X4 positions (else F32X3) */
    u32 wall;                 /* > 0: a flat wall of wall x wall tiles facing -y, no randomness */
    u32 submeshes;
} mesh_style;

typedef struct fmesh {
    de_render_mesh rm;
    u32 meshlets, verts;
    f32 *pos;    /* decoded object-space positions (what the GPU reads back from the stream) */
    u8 *records; /* meshlets x DE_COOKED_MESHLET_SIZE */
    u32 *mverts; /* meshlet vertices */
    u8 *mtris;   /* meshlet triangles: PATCH_TRIS x 3 bytes per meshlet */
    u8 *stream;  /* the POSITION stream */
    u32 stream_bytes;
    de_aabb bounds;
    de_gpu_buffer buf[4]; /* vertices, records, meshlet vertices, meshlet triangles */
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
static f32 contain_radius(const f32 c[3], f32 radius, const f32 *pos, u32 first, u32 count) {
    f64 r2 = (f64)radius * radius, mag = 0.0;
    for (u32 k = 0; k < 3; ++k)
        mag = fabs((f64)c[k]) > mag ? fabs((f64)c[k]) : mag;
    for (u32 i = 0; i < count; ++i) {
        const f32 *p = pos + 3u * (u64)(first + i);
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

/* Builds mesh `fm` (CPU data only; mesh_upload makes its GPU buffers). */
static void mesh_build(fmesh *fm, const mesh_style *st, rng *g) {
    memset(fm, 0, sizeof *fm);
    const u32 n = st->wall > 0 ? st->wall * st->wall : st->meshlets;
    fm->meshlets = n;
    fm->verts = n * PATCH_VERT;
    fm->pos = (f32 *)zalloc((size_t)fm->verts * 3u * sizeof(f32));
    fm->records = (u8 *)zalloc((size_t)n * DE_COOKED_MESHLET_SIZE);
    fm->mverts = (u32 *)zalloc((size_t)fm->verts * sizeof(u32));
    fm->mtris = (u8 *)zalloc((size_t)n * PATCH_TRIS * 3u);
    fm->bounds = de_aabb_empty();
    u8 local[PATCH_TRIS * 3u];
    u32 nt = 0;
    for (u32 j = 0; j + 1u < GRID; ++j)
        for (u32 i = 0; i + 1u < GRID; ++i) {
            const u8 v00 = (u8)(j * GRID + i), v10 = (u8)(v00 + 1u);
            const u8 v11 = (u8)(v10 + GRID), v01 = (u8)(v00 + GRID);
            const u8 tris[6] = {v00, v10, v11, v00, v11, v01}; /* counter-clockwise from +n */
            memcpy(local + 3u * nt, tris, 6);
            nt += 2u;
        }
    /* The patches' positions. */
    for (u32 ml = 0; ml < n; ++ml) {
        de_vec3 p, n3, t, b;
        f32 s, k;
        if (st->wall > 0) {
            s = 2.0f * st->half / (f32)st->wall;
            p = (de_vec3){-st->half + s * ((f32)(ml % st->wall) + 0.5f), 0.0f,
                          -st->half + s * ((f32)(ml / st->wall) + 0.5f)};
            n3 = (de_vec3){0.0f, -1.0f, 0.0f};
            t = (de_vec3){1.0f, 0.0f, 0.0f};
            b = (de_vec3){0.0f, 0.0f, 1.0f}; /* t x b = n */
            k = 0.0f;
        } else {
            s = rndf(g, st->patch_min, st->patch_max);
            const f32 lim = st->half - s;
            p = (de_vec3){rndf(g, -lim, lim), rndf(g, -lim, lim), rndf(g, -lim, lim)};
            n3 = rnd_unit(g);
            const de_vec3 a = fabsf(n3.z) < 0.9f ? (de_vec3){0, 0, 1} : (de_vec3){1, 0, 0};
            t = de_v3_cross(a, n3);
            t = de_v3_scale(t, 1.0f / de_v3_length(t));
            b = de_v3_cross(n3, t);
            k = rndf(g, -st->bend, st->bend) / s;
        }
        for (u32 j = 0; j < GRID; ++j)
            for (u32 i = 0; i < GRID; ++i) {
                const f32 x = ((f32)i / (f32)(GRID - 1u) - 0.5f) * s;
                const f32 y = ((f32)j / (f32)(GRID - 1u) - 0.5f) * s;
                const f32 z = k * (x * x + y * y);
                f32 *q = fm->pos + 3u * ((size_t)ml * PATCH_VERT + j * GRID + i);
                q[0] = p.x + x * t.x + y * b.x + z * n3.x;
                q[1] = p.y + x * t.y + y * b.y + z * n3.y;
                q[2] = p.z + x * t.z + y * b.z + z * n3.z;
                fm->bounds.min = de_v3_min(fm->bounds.min, (de_vec3){q[0], q[1], q[2]});
                fm->bounds.max = de_v3_max(fm->bounds.max, (de_vec3){q[0], q[1], q[2]});
            }
    }
    /* The position stream; `pos` becomes what the GPU decodes. */
    de_render_mesh *m = &fm->rm;
    f32 off[3] = {0, 0, 0}, scale[3] = {1, 1, 1};
    if (st->quantized) {
        const f32 mn[3] = {fm->bounds.min.x, fm->bounds.min.y, fm->bounds.min.z};
        const f32 mx[3] = {fm->bounds.max.x, fm->bounds.max.y, fm->bounds.max.z};
        for (u32 c = 0; c < 3; ++c) {
            off[c] = mn[c];
            scale[c] = (mx[c] - mn[c]) / 65535.0f;
        }
        fm->stream_bytes = fm->verts * 8u;
        fm->stream = (u8 *)zalloc(fm->stream_bytes);
        fm->bounds = de_aabb_empty();
        for (u32 v = 0; v < fm->verts; ++v) {
            f32 *q = fm->pos + 3u * (size_t)v;
            u16 qq[4] = {0, 0, 0, 0};
            for (u32 c = 0; c < 3; ++c) {
                const f32 u = scale[c] > 0.0f ? (q[c] - off[c]) / scale[c] : 0.0f;
                const f32 r = floorf(u + 0.5f);
                qq[c] = (u16)(r < 0.0f ? 0.0f : r > 65535.0f ? 65535.0f : r);
                q[c] = off[c] + (f32)qq[c] * scale[c]; /* de_cooked_mesh_position */
            }
            memcpy(fm->stream + 8u * (size_t)v, qq, 8);
            fm->bounds.min = de_v3_min(fm->bounds.min, (de_vec3){q[0], q[1], q[2]});
            fm->bounds.max = de_v3_max(fm->bounds.max, (de_vec3){q[0], q[1], q[2]});
        }
    } else {
        fm->stream_bytes = fm->verts * 12u;
        fm->stream = (u8 *)zalloc(fm->stream_bytes);
        memcpy(fm->stream, fm->pos, fm->stream_bytes);
    }
    /* The meshlet sections. */
    for (u32 ml = 0; ml < n; ++ml) {
        const u32 base = ml * PATCH_VERT;
        u32 verts[PATCH_VERT];
        for (u32 i = 0; i < PATCH_VERT; ++i) {
            verts[i] = base + i;
            fm->mverts[base + i] = base + i;
        }
        memcpy(fm->mtris + (size_t)ml * PATCH_TRIS * 3u, local, sizeof local);
        const struct meshopt_Bounds bo =
            meshopt_computeMeshletBounds(verts, local, PATCH_TRIS, fm->pos, fm->verts, 12u);
        const f32 radius = contain_radius(bo.center, bo.radius, fm->pos, base, PATCH_VERT);
        u8 *r = fm->records + (size_t)ml * DE_COOKED_MESHLET_SIZE;
        put_u32(r + 0, base);
        put_u32(r + 4, ml * PATCH_TRIS * 3u);
        const u32 sub = st->submeshes > 1u ? ml % st->submeshes : 0u;
        put_u32(r + 8, PATCH_VERT | (PATCH_TRIS << 8) | (sub << 16));
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
    for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s)
        m->stream_offset[s] = UINT32_MAX;
    m->stream_offset[DE_COOKED_STREAM_POSITION] = 0;
    m->stream_format[DE_COOKED_STREAM_POSITION] =
        st->quantized ? DE_COOKED_VF_UNORM16X4 : DE_COOKED_VF_F32X3;
    m->vertex_count = fm->verts;
    m->meshlet_count = n;
    m->meshlet_vertex_count = fm->verts;
    m->meshlet_triangle_bytes = n * PATCH_TRIS * 3u;
    m->submesh_count = st->submeshes > 1u ? st->submeshes : 1u;
    m->lod_count = 1;
    m->lods[0] = (de_cooked_mesh_lod){
        .meshlet_count = n, .triangle_count = PATCH_TRIS * n, .vertex_count = fm->verts};
    const f32 mn[3] = {fm->bounds.min.x, fm->bounds.min.y, fm->bounds.min.z};
    const f32 mx[3] = {fm->bounds.max.x, fm->bounds.max.y, fm->bounds.max.z};
    for (u32 c = 0; c < 3; ++c) {
        m->bounds_min[c] = mn[c];
        m->bounds_max[c] = mx[c];
        m->sphere[c] = 0.5f * (mn[c] + mx[c]);
        m->position_offset[c] = off[c];
        m->position_scale[c] = scale[c];
    }
    m->sphere[3] =
        0.5f * sqrtf((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
                     (mx[2] - mn[2]) * (mx[2] - mn[2]));
}

/* One UPLOAD buffer of `bytes` (read through de_bytes) for section `which` of the mesh. */
static void mesh_section(fmesh *fm, de_gpu_device *dev, u32 i, u32 which, const void *data,
                         u64 bytes) {
    fm->buf[i] = de_gpu_buffer_create(dev, &(de_gpu_buffer_desc){.size = bytes,
                                                                 .usage = DE_USAGE_STORAGE,
                                                                 .memory = DE_MEM_UPLOAD,
                                                                 .name = "test mesh section"});
    ASSERT_TRUE(DE_HANDLE_VALID(fm->buf[i]));
    u8 *map = (u8 *)de_gpu_buffer_map(dev, fm->buf[i]);
    ASSERT_TRUE(map != NULL);
    memcpy(map, data, (size_t)bytes);
    fm->rm.buffers[which] = fm->buf[i];
    fm->rm.bindless[which] = de_gpu_buffer_bindless(dev, fm->buf[i]);
    fm->rm.buffer_bytes[which] = bytes;
}

static void mesh_upload(fmesh *fm, de_gpu_device *dev) {
    mesh_section(fm, dev, 0, DE_RENDER_MESH_VERTICES, fm->stream, fm->stream_bytes);
    mesh_section(fm, dev, 1, DE_RENDER_MESH_MESHLETS, fm->records,
                 (u64)fm->meshlets * DE_COOKED_MESHLET_SIZE);
    mesh_section(fm, dev, 2, DE_RENDER_MESH_MESHLET_VERTICES, fm->mverts, (u64)fm->verts * 4u);
    mesh_section(fm, dev, 3, DE_RENDER_MESH_MESHLET_TRIANGLES, fm->mtris,
                 (u64)fm->meshlets * PATCH_TRIS * 3u);
}

static void mesh_free(fmesh *fm, de_gpu_device *dev) {
    for (u32 i = 0; i < 4; ++i)
        if (dev != NULL && DE_HANDLE_VALID(fm->buf[i]))
            de_gpu_buffer_destroy(dev, fm->buf[i]);
    free(fm->pos);
    free(fm->records);
    free(fm->mverts);
    free(fm->mtris);
    free(fm->stream);
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

/* Slot `s`: mesh `mesh` (1-based) with transform `xf` and material `mat` on every submesh (0:
 * none, the mesh's default). */
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
    w->touch_all = true;
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

/* What one frame read back. */
typedef struct rc_result {
    bool valid;
    u32 width, height;
    u32 *ids; /* vis ID per pixel */
    f32 *depth;
    de__vis_meshlet *visible;
    u32 visible_stored;
    u32 meshlet_counters[4];
    de__gs_instance *inst;
    u32 inst_count;
    de__gs_view view;
    u64 frame;
} rc_result;

typedef struct rcx {
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
    de_vec3 eye, target;
    f32 fov;
    u32 width, height;
    bool no_readback; /* frames add no test readback (the null-backend allocation test) */
    rc_result res;
} rcx;

static void rcx_setup(rcx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "raster_compute_test"}, &f->arena), DE_OK);
    de_error_clear();
    memset(g_mesh, 0, sizeof g_mesh);
    g_mesh_count = 0;
    memset(g_mat, 0, sizeof g_mat);
    g_mat[0].standard.double_sided = false;
    g_mat[1].standard.double_sided = true;
    f->eye = (de_vec3){0.0f, -30.0f, 2.0f};
    f->target = (de_vec3){0.0f, 0.0f, 0.0f};
    f->fov = 0.9f;
    f->width = 640;
    f->height = 360;
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

/* Sets a cvar by name (false when it does not exist, e.g. a Dev-only cvar in Shipping). */
static bool cvar_set(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (!DE_HANDLE_VALID(cv))
        return false;
    EXPECT_EQ(de_cvar_set(cv, value), DE_OK);
    return true;
}

static void rcx_teardown(rcx *f) {
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
    free(f->res.ids);
    free(f->res.depth);
    free(f->res.visible);
    free(f->res.inst);
    world_free(&f->w);
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
    /* The cvars are process-wide: back to their defaults. */
    const char *const names[] = {
        "r.raster_tri_cull", "r.raster_compute_max_tris", "r.lod_bias",      "r.occlusion",
        "r.raster_path",     "r.cull_meshlet_cone",       "r.rg_record_cost"};
    const char *const values[] = {"1", "16777216", "0", "1", "auto", "1", "32"};
    for (u32 i = 0; i < 7; ++i) {
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
static void rcx_open(rcx *f, de_gpu_backend backend, u32 instances) {
    static char why[700];
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
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "rc"},
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
    world_init(&f->w, instances);
    /* LOD 0 everywhere (the meshes have one LOD anyway); no occlusion unless a test turns it on
     * (a new view's first frame would otherwise draw everything in phase 2). Once DENG-234's
     * path switch exists, this test takes the compute path on every device. */
    (void)cvar_set("r.lod_bias", "-7");
    (void)cvar_set("r.occlusion", "0");
    (void)cvar_set("r.raster_path", "compute");
    /* One command list per submission: Vulkan's GPU-assisted validation (VVL 1.3.275) checks the
     * HZB build's sampled read of the depth against the depth's layout at the end of each command
     * list, so a list that ends after a raster pass (depth attachment) reports it. The renderer's
     * lists are the same commands cut differently. */
    if (backend == DE_GPU_VULKAN) {
        /* The graph registers it at its first execute: until then the value waits (pending). */
        if (!cvar_set("r.rg_record_cost", "1048576")) {
            char *args[] = {"test", "+r.rg_record_cost", "1048576"};
            EXPECT_EQ(de_cvar_apply_args(3, args), DE_OK);
        }
    }
}

/* Adds mesh `style` (built and uploaded); returns its 1-based id. */
static u32 rcx_mesh(rcx *f, const mesh_style *style) {
    ASSERT_TRUE(g_mesh_count < MAX_MESHES);
    fmesh *fm = &g_mesh[g_mesh_count];
    mesh_build(fm, style, &f->r);
    mesh_upload(fm, f->dev);
    return ++g_mesh_count;
}

/* ---------- One frame ---------- */

#define RB_COUNTERS 0u
#define RB_IMAGES   512u

typedef struct rb_pass {
    rcx *f;
    de_rg_resource ids, depth, visible, counters;
    u32 pitch;
    u64 ids_off, depth_off, visible_off, visible_bytes;
} rb_pass;

static void readback_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    const de_gpu_buffer rb = p->f->rb;
    de_gpu_cmd_copy_buffer(cmd, rb, RB_COUNTERS, de_rg_buffer(pass, p->counters), 0,
                           DE__VIS_MESHLET_COUNTER_BYTES);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, p->ids_off, p->pitch, de_rg_texture(pass, p->ids), 0,
                                      0);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, p->depth_off, p->pitch,
                                      de_rg_texture(pass, p->depth), 0, 0);
    de_gpu_cmd_copy_buffer(cmd, rb, p->visible_off, de_rg_buffer(pass, p->visible), 0,
                           p->visible_bytes);
}

static void ensure_rb(rcx *f, u64 bytes) {
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

static u64 align_up(u64 v, u64 a) {
    return (v + a - 1u) / a * a;
}

/* The camera's view and projection matrices. */
static void camera(const rcx *f, de_mat4 *view, de_mat4 *proj) {
    *view = de_m4_look_at(f->eye, f->target, (de_vec3){0.0f, 0.0f, 1.0f});
    *proj = de_m4_perspective_rz(f->fov, (f32)f->width / (f32)f->height, 0.1f);
}

/* Builds, runs and waits for one frame; fills f->res (valid when the raster ran). */
static bool rcx_frame(rcx *f) {
    const u64 n = f->frames++;
    world_begin(&f->w);
    de_mat4 view, proj;
    camera(f, &view, &proj);
    if (de__gpu_scene_update(f->gs, &f->w.f) != DE_OK)
        return false;
    /* The view's scene constants (after the update: they belong to its frame). */
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
    rb_pass *rp = NULL;
    const bool want_rb =
        !f->no_readback && DE_HANDLE_VALID(vc->vis_ids) && DE_HANDLE_VALID(vc->visible_meshlets);
    if (want_rb) {
        rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
        ASSERT_TRUE(rp != NULL);
        rp->f = f;
        rp->ids = vc->vis_ids;
        rp->depth = vc->depth;
        rp->visible = vc->visible_meshlets;
        rp->counters = vc->meshlet_counters;
        rp->pitch = (u32)align_up((u64)vc->depth_width * 4u, 256u);
        const u64 image = (u64)rp->pitch * vc->depth_height;
        rp->ids_off = RB_IMAGES;
        rp->depth_off = align_up(rp->ids_off + image, 512u);
        rp->visible_off = align_up(rp->depth_off + image, 512u);
        rp->visible_bytes = (u64)vc->visible_capacity * sizeof(de__vis_meshlet);
        ensure_rb(f, rp->visible_off + rp->visible_bytes);
        de_rg_pass *p =
            de_rg_add_pass(f->g, "test.readback", DE_QUEUE_GRAPHICS, readback_execute, rp);
        de_rg_read(p, rp->ids, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->depth, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->visible, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->counters, DE_ACCESS_COPY_SRC);
        const de_rg_resource rb = de_rg_import_buffer(f->g, f->rb, DE_ACCESS_NONE);
        de_rg_write(p, rb, DE_ACCESS_COPY_DST);
        /* The depth back to SHADER_READ before the command list ends (as test_raster_mesh.c):
         * Vulkan validation (VVL 1.3.275, GPU-assisted) checks the HZB build's sampled read of it
         * against its layout at the end of the command buffer, not at the access. The renderer
         * never copies it. */
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
    rc_result *res = &f->res;
    res->valid = false;
    res->frame = n;
    const u32 ic = de__gpu_scene_table(f->gs, DE__GS_INSTANCES).count;
    if (ic > res->inst_count) {
        free(res->inst);
        res->inst = (de__gs_instance *)zalloc((size_t)ic * sizeof(de__gs_instance));
    }
    res->inst_count = ic;
    for (u32 i = 0; i < ic; ++i)
        de__gpu_scene_debug_pack(f->gs, DE__GS_INSTANCES, i, &res->inst[i]);
    de__gpu_scene_debug_pack(f->gs, DE__GS_VIEWS, 0, &res->view);
    de__gpu_scene_frame_end(f->gs, f->g, true);
    if (de_gpu_fence_wait(f->dev, f->fence, n + 1, WAIT_NS) != DE_OK)
        return false;
    if (!want_rb)
        return true;
    const u8 *m = (const u8 *)de_gpu_buffer_map(f->dev, f->rb);
    if (res->width != vc->depth_width || res->height != vc->depth_height) {
        free(res->ids);
        free(res->depth);
        res->width = vc->depth_width;
        res->height = vc->depth_height;
        res->ids = (u32 *)zalloc((size_t)res->width * res->height * 4u);
        res->depth = (f32 *)zalloc((size_t)res->width * res->height * 4u);
    }
    for (u32 y = 0; y < res->height; ++y) {
        memcpy(res->ids + (size_t)y * res->width, m + rp->ids_off + (u64)y * rp->pitch,
               (size_t)res->width * 4u);
        memcpy(res->depth + (size_t)y * res->width, m + rp->depth_off + (u64)y * rp->pitch,
               (size_t)res->width * 4u);
    }
    memcpy(res->meshlet_counters, m + RB_COUNTERS, sizeof res->meshlet_counters);
    const u32 vis = res->meshlet_counters[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u];
    res->visible_stored = vis < vc->visible_capacity ? vis : vc->visible_capacity;
    free(res->visible);
    res->visible = (de__vis_meshlet *)zalloc((size_t)res->visible_stored * sizeof(de__vis_meshlet));
    if (res->visible_stored > 0)
        memcpy(res->visible, m + rp->visible_off, res->visible_stored * sizeof(de__vis_meshlet));
    res->valid = true;
    return true;
}

/* Runs frames until the raster read back a frame at or after `from` (the stats are one or more
 * frames late); false when that does not happen within a few frames. */
static bool rcx_frames_until_stats(rcx *f, u64 from, de__vis_raster_compute_stats *out) {
    for (u32 i = 0; i < 8; ++i) {
        if (!rcx_frame(f))
            return false;
        const de__vis_raster_compute_stats st = de__vis_view_raster_compute_stats(f->vis, 0);
        if (st.valid && st.frame >= from) {
            *out = st;
            return true;
        }
    }
    return false;
}

/* ================================================================================================
 * The CPU reference
 * ============================================================================================== */

#define REF_NONE UINT32_MAX

typedef struct ref_image {
    u32 width, height;
    u32 *inst, *meshlet, *tri; /* REF_NONE: nothing */
    f64 *depth;                /* nearest z/w (reverse-Z: the largest) */
    f64 *edge_z, *tie_z;       /* nearest edge sample, nearest depth tie (-1: none) */
    u8 *ambiguous;             /* an edge or depth tie decided the pixel */
} ref_image;

static void ref_free(ref_image *r) {
    free(r->inst);
    free(r->meshlet);
    free(r->tri);
    free(r->depth);
    free(r->edge_z);
    free(r->tie_z);
    free(r->ambiguous);
    memset(r, 0, sizeof *r);
}

/* clip = view_proj * (rows * p): the GPU's arithmetic, in double. */
static void ref_clip(const de__gs_instance *in, const de__gs_view *v, const f32 p[3], f64 out[4]) {
    f64 w[3];
    for (u32 r = 0; r < 3; ++r) {
        const f32 *row = in->transform[r].v;
        w[r] = (f64)row[0] * p[0] + (f64)row[1] * p[1] + (f64)row[2] * p[2] + (f64)row[3];
    }
    for (u32 r = 0; r < 4; ++r) {
        const f32 *row = v->view_proj[r].v;
        out[r] = (f64)row[0] * w[0] + (f64)row[1] * w[1] + (f64)row[2] * w[2] + (f64)row[3];
    }
}

static bool ref_mirrored(const de__gs_instance *in) {
    const f32 *a = in->transform[0].v, *b = in->transform[1].v, *c = in->transform[2].v;
    const f64 det = (f64)a[0] * ((f64)b[1] * c[2] - (f64)b[2] * c[1]) -
                    (f64)a[1] * ((f64)b[0] * c[2] - (f64)b[2] * c[0]) +
                    (f64)a[2] * ((f64)b[0] * c[1] - (f64)b[1] * c[0]);
    return det < 0.0;
}

/* The raster's facing rule for the fake materials: only a READY double-sided material draws back
 * faces; the default (no material) and single-sided ones do not. */
static bool ref_single_sided(const world *w, u32 s) {
    return w->f.instances.materials[(size_t)s * DE__RW_MATERIALS].id != MAT_DOUBLE;
}

/* Sub-pixel snapping: 8 bits, the D3D12 and Vulkan rule and what Metal GPUs do. */
static f64 snap(f64 v) {
    return floor(v * 256.0 + 0.5) / 256.0;
}

/* Rasterizes every triangle of every live instance (`cull`: r.raster_tri_cull). */
static void ref_render(const rcx *f, bool cull, ref_image *r) {
    const rc_result *res = &f->res;
    const u32 w = res->width, h = res->height;
    memset(r, 0, sizeof *r);
    r->width = w;
    r->height = h;
    const size_t np = (size_t)w * h;
    r->inst = (u32 *)zalloc(np * 4u);
    r->meshlet = (u32 *)zalloc(np * 4u);
    r->tri = (u32 *)zalloc(np * 4u);
    r->depth = (f64 *)zalloc(np * sizeof(f64));
    r->edge_z = (f64 *)zalloc(np * sizeof(f64));
    r->tie_z = (f64 *)zalloc(np * sizeof(f64));
    r->ambiguous = (u8 *)zalloc(np);
    for (size_t i = 0; i < np; ++i) {
        r->inst[i] = REF_NONE;
        r->depth[i] = 0.0; /* the cleared depth: far */
        r->edge_z[i] = -1.0;
        r->tie_z[i] = -1.0;
    }
    const de__rw_instances *in = &f->w.f.instances;
    for (u32 s = 0; s < in->count && s < res->inst_count; ++s) {
        if (in->entity[s] == 0 || in->mesh[s].id == 0)
            continue;
        const fmesh *fm = &g_mesh[in->mesh[s].id - 1u];
        const de__gs_instance *gi = &res->inst[s];
        const bool mirrored = ref_mirrored(gi), single = ref_single_sided(&f->w, s);
        for (u32 ml = 0; ml < fm->meshlets; ++ml) {
            f64 c[PATCH_VERT][4];
            for (u32 v = 0; v < PATCH_VERT; ++v)
                ref_clip(gi, &res->view, fm->pos + 3u * ((size_t)ml * PATCH_VERT + v), c[v]);
            for (u32 t = 0; t < PATCH_TRIS; ++t) {
                const u8 *lt = fm->mtris + ((size_t)ml * PATCH_TRIS + t) * 3u;
                const f64 *a = c[lt[0]], *b = c[lt[1]], *cc = c[lt[2]];
                if (!(a[3] > 0.0 && b[3] > 0.0 && cc[3] > 0.0))
                    continue; /* the scenes keep every triangle in front of the camera */
                /* Pixel space (y down), snapped to the rasterizer's 1/256 px grid, and NDC
                 * depth. */
                const f64 x0 = snap((a[0] / a[3] * 0.5 + 0.5) * w);
                const f64 y0 = snap((0.5 - a[1] / a[3] * 0.5) * h);
                const f64 x1 = snap((b[0] / b[3] * 0.5 + 0.5) * w);
                const f64 y1 = snap((0.5 - b[1] / b[3] * 0.5) * h);
                const f64 x2 = snap((cc[0] / cc[3] * 0.5 + 0.5) * w);
                const f64 y2 = snap((0.5 - cc[1] / cc[3] * 0.5) * h);
                const f64 z0 = a[2] / a[3], z1 = b[2] / b[3], z2 = cc[2] / cc[3];
                /* Twice the signed area in pixel space (y down); NDC (y up) has the opposite
                 * sign, and counter-clockwise in NDC is front; mirrored instances flip it. */
                const f64 dbl = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                f64 facing = -dbl;
                if (mirrored)
                    facing = -facing;
                if (dbl == 0.0 || (cull && single && facing < 0.0))
                    continue;
                /* Clockwise on screen (positive dbl) from here: the top-left rule below. */
                f64 vx[3] = {x0, x1, x2}, vy[3] = {y0, y1, y2}, vz[3] = {z0, z1, z2};
                if (dbl < 0.0) {
                    f64 tmp = vx[1];
                    vx[1] = vx[2];
                    vx[2] = tmp;
                    tmp = vy[1];
                    vy[1] = vy[2];
                    vy[2] = tmp;
                    tmp = vz[1];
                    vz[1] = vz[2];
                    vz[2] = tmp;
                }
                const f64 area2 = fabs(dbl);
                const f64 minx = fmin(x0, fmin(x1, x2)), maxx = fmax(x0, fmax(x1, x2));
                const f64 miny = fmin(y0, fmin(y1, y2)), maxy = fmax(y0, fmax(y1, y2));
                i64 px0 = (i64)floor(minx - 0.5), px1 = (i64)ceil(maxx - 0.5);
                i64 py0 = (i64)floor(miny - 0.5), py1 = (i64)ceil(maxy - 0.5);
                px0 = px0 < 0 ? 0 : px0;
                py0 = py0 < 0 ? 0 : py0;
                px1 = px1 >= (i64)w ? (i64)w - 1 : px1;
                py1 = py1 >= (i64)h ? (i64)h - 1 : py1;
                f64 len[3];
                bool top_left[3];
                for (u32 k = 0; k < 3; ++k) {
                    const u32 k1 = (k + 1u) % 3u;
                    len[k] = hypot(vx[k1] - vx[k], vy[k1] - vy[k]);
                    /* Top: horizontal, going right; left: going up (y down, clockwise). */
                    top_left[k] = (vy[k] == vy[k1] && vx[k1] > vx[k]) || vy[k1] < vy[k];
                }
                const f64 tol = 1.0 / 256.0; /* pixels: a vertex one grid step off */
                for (i64 py = py0; py <= py1; ++py)
                    for (i64 px = px0; px <= px1; ++px) {
                        const f64 qx = (f64)px + 0.5, qy = (f64)py + 0.5;
                        bool inside = true, near_edge = false, outside_far = false;
                        f64 e[3];
                        for (u32 k = 0; k < 3; ++k) {
                            const u32 k1 = (k + 1u) % 3u;
                            e[k] =
                                (vx[k1] - vx[k]) * (qy - vy[k]) - (vy[k1] - vy[k]) * (qx - vx[k]);
                            if (e[k] < -tol * len[k])
                                outside_far = true;
                            if (fabs(e[k]) <= tol * len[k])
                                near_edge = true;
                            if (!(e[k] > 0.0 || (e[k] == 0.0 && top_left[k])))
                                inside = false;
                        }
                        if (outside_far)
                            continue;
                        /* Barycentric depth (z/w is linear in screen space). */
                        const f64 z = (e[1] * vz[0] + e[2] * vz[1] + e[0] * vz[2]) / area2;
                        if (!(z >= 0.0 && z <= 1.0))
                            continue;
                        const size_t i = (size_t)py * w + (size_t)px;
                        if (near_edge) /* one grid step decides: the GPU may differ */
                            r->edge_z[i] = fmax(r->edge_z[i], z);
                        if (!inside)
                            continue;
                        const f64 old = r->depth[i];
                        if (r->inst[i] != REF_NONE &&
                            fabs(z - old) <= 1e-6 * fmax(fabs(z), fabs(old)))
                            r->tie_z[i] = fmax(r->tie_z[i], fmax(z, old));
                        if (r->inst[i] == REF_NONE || z > old) {
                            r->depth[i] = z;
                            r->inst[i] = s;
                            r->meshlet[i] = ml;
                            r->tri[i] = t;
                        }
                    }
            }
        }
    }
    /* Ambiguous: an edge sample or a depth tie at or in front of the winner. */
    for (size_t i = 0; i < np; ++i) {
        const f64 d = r->depth[i] * (1.0 - 1e-6);
        r->ambiguous[i] = (u8)(r->edge_z[i] >= d || (r->tie_z[i] >= 0.0 && r->tie_z[i] >= d));
    }
}

typedef struct cmp_stats {
    u32 pixels, covered, mismatch, mismatch_clear, depth_bad, bad_id;
} cmp_stats;

/* The GPU image against the reference: ids decoded through the visible list. */
static cmp_stats compare_ref(const rcx *f, const ref_image *r) {
    const rc_result *res = &f->res;
    cmp_stats c = {0};
    c.pixels = r->width * r->height;
    for (u32 i = 0; i < c.pixels; ++i) {
        const u32 id = res->ids[i];
        u32 gi = REF_NONE, gm = 0, gt = 0;
        if (id != DE__VIS_ID_EMPTY) {
            const u32 e = de__vis_id_meshlet(id);
            if (e >= res->visible_stored) {
                ++c.bad_id;
                ++c.mismatch;
                continue;
            }
            gi = res->visible[e].instance;
            gm = res->visible[e].meshlet;
            gt = de__vis_id_triangle(id);
        }
        c.covered += r->inst[i] != REF_NONE;
        const bool same =
            gi == r->inst[i] && (gi == REF_NONE || (gm == r->meshlet[i] && gt == r->tri[i]));
        if (!same) {
            ++c.mismatch;
            if (!r->ambiguous[i])
                ++c.mismatch_clear;
            continue;
        }
        if (gi != REF_NONE) {
            const f64 dz = fabs((f64)res->depth[i] - r->depth[i]);
            if (dz > 1e-4 * fabs(r->depth[i]) + 1e-7)
                ++c.depth_bad;
        } else if (res->depth[i] != 0.0f) {
            ++c.depth_bad; /* the background keeps the cleared far depth */
        }
    }
    return c;
}

static void expect_matches_cov(const rcx *f, bool cull, const char *what, u32 min_cover_pct) {
    ref_image r;
    ref_render(f, cull, &r);
    const cmp_stats c = compare_ref(f, &r);
    printf("%s: %u px, %u covered, %u differ (%u where the reference is not ambiguous), %u depth, "
           "%u bad ids\n",
           what, c.pixels, c.covered, c.mismatch, c.mismatch_clear, c.depth_bad, c.bad_id);
    EXPECT_GT(c.covered * 100u, c.pixels * min_cover_pct); /* the scene is in view */
    EXPECT_EQ(c.bad_id, 0u);
    EXPECT_LE(c.mismatch * 1000u, c.pixels); /* >= 99.9% identical */
    EXPECT_LE(c.mismatch_clear * 10000u, c.pixels);
    EXPECT_LE(c.depth_bad * 1000u, c.pixels);
    ref_free(&r);
}

static void expect_matches(const rcx *f, bool cull, const char *what) {
    expect_matches_cov(f, cull, what, 10u);
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

static const mesh_style k_float = {
    .meshlets = 48, .half = 1.5f, .patch_min = 0.4f, .patch_max = 1.0f, .bend = 1.0f};
static const mesh_style k_quant = {.meshlets = 48,
                                   .half = 1.5f,
                                   .patch_min = 0.4f,
                                   .patch_max = 1.0f,
                                   .bend = 1.0f,
                                   .quantized = true,
                                   .submeshes = 2};

/* `n` instances of two meshes (float and quantized positions) in a box in front of the camera:
 * a quarter double-sided, a sixth without a material, a tenth mirrored. */
static void scene_cloud(rcx *f, u32 n, u32 first_slot) {
    const u32 a = rcx_mesh(f, &k_float), b = rcx_mesh(f, &k_quant);
    for (u32 i = 0; i < n; ++i) {
        const u32 kind = rnd(&f->r) % 12u;
        const u32 mat = kind < 3u ? MAT_DOUBLE : kind < 5u ? 0u : MAT_SINGLE;
        const de_affine xf = rnd_affine(&f->r, (de_vec3){-12.0f, -8.0f, -6.0f},
                                        (de_vec3){12.0f, 10.0f, 8.0f}, 0.5f, 1.6f, kind == 7u);
        world_set(&f->w, first_slot + i, (i & 1u) ? b : a, xf, mat);
    }
}

/* ================================================================================================
 * GPU tests
 * ============================================================================================== */

/* raster_compute_ids_match_cpu_ref: the vis ID and the depth of a random scene against the CPU
 * rasterizer, with per-triangle culling on and off. */
static void ids_match_cpu_ref(rcx *f, de_gpu_backend b) {
    rcx_open(f, b, 256u);
    scene_cloud(f, 160u, 0u);
    ASSERT_TRUE(rcx_frame(f));
    ASSERT_TRUE(f->res.valid);
    expect_matches(f, true, "tri cull on");
    /* Two more frames: the stats of a frame are read back later; nothing overflowed. */
    de__vis_raster_compute_stats st = {0};
    ASSERT_TRUE(rcx_frames_until_stats(f, 0, &st));
    EXPECT_GT(st.triangles, 0u);
    EXPECT_LE(st.triangles, st.capacity);
    EXPECT_LE(st.triangles, st.tested);
    EXPECT_EQ(st.phase1, st.triangles); /* occlusion off: everything is drawn in phase 1 */
    EXPECT_GT(st.meshlets, 0u);
    printf("triangles %u of %u tested, %u meshlets, capacity %u\n", st.triangles, st.tested,
           st.meshlets, st.capacity);
    expect_matches(f, true, "tri cull on, later frame");
    /* r.raster_tri_cull 0: every triangle of the visible meshlets, back faces too (with the
     * meshlet cone test off, which would drop wholly back-facing meshlets; a Dev cvar). */
    if (!cvar_set("r.cull_meshlet_cone", "0"))
        return;
    ASSERT_TRUE(cvar_set("r.raster_tri_cull", "0"));
    ASSERT_TRUE(rcx_frame(f));
    ASSERT_TRUE(f->res.valid);
    expect_matches(f, false, "tri cull off");
}

/* raster_compute_two_phase: occlusion on, a wall in front of a cloud. The image equals the
 * reference in every frame: the first frame of a new view draws everything in phase 2, later
 * frames draw last frame's visible set in phase 1, and an instance moved out from behind the wall
 * is drawn in phase 2 of the same frame (vis IDs after phase 1's entries). */
static void two_phase(rcx *f, de_gpu_backend b) {
    rcx_open(f, b, 256u);
    ASSERT_TRUE(cvar_set("r.occlusion", "1"));
    const mesh_style wall = {.half = 8.0f, .wall = 8u};
    const u32 wm = rcx_mesh(f, &wall);
    world_set(&f->w, 0, wm,
              de_affine_from_trs((de_vec3){0, -6.0f, 1.0f}, de_quat_identity(), (de_vec3){1, 1, 1}),
              MAT_SINGLE);
    /* The cloud behind the wall (y > -6), and a few instances beside it. */
    const u32 a = rcx_mesh(f, &k_float);
    for (u32 i = 1; i < 60u; ++i) {
        const bool beside = i % 6u == 0u;
        const de_vec3 lo = beside ? (de_vec3){9.5f, -4.0f, -2.0f} : (de_vec3){-5.0f, 0.0f, -4.0f};
        const de_vec3 hi = beside ? (de_vec3){13.0f, 4.0f, 4.0f} : (de_vec3){5.0f, 8.0f, 5.0f};
        world_set(&f->w, i, a, rnd_affine(&f->r, lo, hi, 0.6f, 1.2f, false),
                  i % 3u == 0u ? MAT_DOUBLE : MAT_SINGLE);
    }
    f->eye = (de_vec3){0.0f, -40.0f, 1.0f};
    f->target = (de_vec3){0.0f, 0.0f, 1.0f};
    /* A first frame without occlusion: the view's HZB is made and built once before phase 2
     * samples it. (Vulkan's GPU-assisted validation checks every sampled index a frame read
     * against the image's layout as the submission's first command buffer saw it: a just-made
     * HZB is UNDEFINED there.) */
    ASSERT_TRUE(cvar_set("r.occlusion", "0"));
    ASSERT_TRUE(rcx_frame(f));
    ASSERT_TRUE(cvar_set("r.occlusion", "1"));
    /* Frame 1: no history (a cut: r.occlusion_reset): phase 1 is empty. */
    ASSERT_TRUE(cvar_set("r.occlusion_reset", "1"));
    ASSERT_TRUE(rcx_frame(f));
    ASSERT_TRUE(f->res.valid);
    EXPECT_EQ(f->res.meshlet_counters[DE__VIS_MESHLET_COUNTER_PHASE1 / 4u], 0u);
    expect_matches(f, true, "frame 1 (all phase 2)");
    /* Frame 2: last frame's visible set in phase 1. */
    ASSERT_TRUE(rcx_frame(f));
    ASSERT_TRUE(f->res.valid);
    const u32 p1 = f->res.meshlet_counters[DE__VIS_MESHLET_COUNTER_PHASE1 / 4u];
    EXPECT_GT(p1, 0u);
    expect_matches(f, true, "frame 2");
    /* Frame 3: an instance from behind the wall moves beside it. */
    u32 moved = 0;
    for (u32 i = 1; i < 60u && moved == 0; ++i)
        if (i % 6u != 0u)
            moved = i;
    world_set(
        &f->w, moved, a,
        de_affine_from_trs((de_vec3){-11.0f, -2.0f, 1.0f}, de_quat_identity(), (de_vec3){1, 1, 1}),
        MAT_DOUBLE);
    ASSERT_TRUE(rcx_frame(f));
    ASSERT_TRUE(f->res.valid);
    const u32 n1 = f->res.meshlet_counters[DE__VIS_MESHLET_COUNTER_PHASE1 / 4u];
    EXPECT_GT(n1, 0u);
    EXPECT_GT(f->res.visible_stored, n1);
    expect_matches(f, true, "frame 3 (reveal)");
    /* The moved instance is drawn, and only by phase 2. */
    u32 px_moved = 0, px_phase2 = 0, px_phase1 = 0;
    for (u32 i = 0; i < f->res.width * f->res.height; ++i) {
        const u32 id = f->res.ids[i];
        if (id == DE__VIS_ID_EMPTY)
            continue;
        const u32 e = de__vis_id_meshlet(id);
        ASSERT_TRUE(e < f->res.visible_stored);
        if (e >= n1)
            ++px_phase2;
        else
            ++px_phase1;
        if (f->res.visible[e].instance == moved) {
            ++px_moved;
            EXPECT_GE(e, n1);
        }
    }
    printf("frame 3: %u px from phase 1, %u from phase 2, %u of the moved instance\n", px_phase1,
           px_phase2, px_moved);
    EXPECT_GT(px_moved, 0u);
    EXPECT_GT(px_phase1, 0u);
}

/* raster_compute_overflow_clamp: r.raster_compute_max_tris 64 draws 64 triangles at most and
 * counts every one (no GPU hang); back at the default, the image is whole again. */
static void overflow_clamp(rcx *f, de_gpu_backend b) {
    rcx_open(f, b, 256u);
    scene_cloud(f, 120u, 0u);
    ASSERT_TRUE(cvar_set("r.raster_compute_max_tris", "64"));
    const u64 from = f->frames;
    de__vis_raster_compute_stats st = {0};
    ASSERT_TRUE(rcx_frames_until_stats(f, from, &st));
    ASSERT_TRUE(f->res.valid);
    EXPECT_EQ(st.capacity, 64u);
    EXPECT_GT(st.triangles, 64u);
    EXPECT_EQ(st.phase1, 64u);
    /* At most 64 distinct triangles in the image, each a real one. */
    u32 seen[64], nseen = 0;
    for (u32 i = 0; i < f->res.width * f->res.height; ++i) {
        const u32 id = f->res.ids[i];
        if (id == DE__VIS_ID_EMPTY)
            continue;
        ASSERT_TRUE(de__vis_id_meshlet(id) < f->res.visible_stored);
        EXPECT_LT(de__vis_id_triangle(id), PATCH_TRIS);
        bool found = false;
        for (u32 k = 0; k < nseen && !found; ++k)
            found = seen[k] == id;
        if (!found) {
            ASSERT_TRUE(nseen < 64u);
            seen[nseen++] = id;
        }
    }
    printf("clamped: %u triangles asked for, %u distinct in the image\n", st.triangles, nseen);
    EXPECT_GT(nseen, 0u);
    /* The ceiling holds in later frames. */
    ASSERT_TRUE(rcx_frames_until_stats(f, f->frames, &st));
    EXPECT_EQ(st.capacity, 64u);
    /* Back to the default: the whole image. */
    ASSERT_TRUE(cvar_set("r.raster_compute_max_tris", "16777216"));
    ASSERT_TRUE(rcx_frames_until_stats(f, f->frames, &st));
    EXPECT_LE(st.triangles, st.capacity);
    ASSERT_TRUE(rcx_frame(f));
    ASSERT_TRUE(f->res.valid);
    expect_matches(f, true, "after the clamp");
}

/* raster_compute_ids_match_mesh_path: the same frame through the mesh path (DENG-234) and this
 * path decodes to the same (instance, meshlet, triangle) and depth, bit for bit, on >= 99.9% of
 * the pixels (where both run: Metal on the host). */
static void ids_match_mesh_path(rcx *f, de_gpu_backend b) {
    rcx_open(f, b, 256u);
    if (!de__vis_raster_mesh_ready(f->vis))
        SKIP("the device has no mesh shaders (or no mesh pipeline)");
    scene_cloud(f, 160u, 0u);
    ASSERT_TRUE(cvar_set("r.raster_path", "compute"));
    ASSERT_TRUE(rcx_frame(f));
    ASSERT_TRUE(f->res.valid);
    const size_t np = (size_t)f->res.width * f->res.height;
    u32 *key = (u32 *)zalloc(np * 3u * sizeof(u32));
    f32 *depth = (f32 *)zalloc(np * sizeof(f32));
    for (size_t i = 0; i < np; ++i) {
        const u32 id = f->res.ids[i];
        const u32 e = id != DE__VIS_ID_EMPTY ? de__vis_id_meshlet(id) : UINT32_MAX;
        key[3 * i] = e < f->res.visible_stored ? f->res.visible[e].instance : UINT32_MAX;
        key[3 * i + 1] = e < f->res.visible_stored ? f->res.visible[e].meshlet : 0u;
        key[3 * i + 2] = id != DE__VIS_ID_EMPTY ? de__vis_id_triangle(id) : 0u;
        depth[i] = f->res.depth[i];
    }
    ASSERT_TRUE(cvar_set("r.raster_path", "mesh"));
    ASSERT_TRUE(rcx_frame(f));
    ASSERT_TRUE(f->res.valid);
    u32 differ = 0, depth_bits = 0, depth_far = 0;
    for (size_t i = 0; i < np; ++i) {
        const u32 id = f->res.ids[i];
        const u32 e = id != DE__VIS_ID_EMPTY ? de__vis_id_meshlet(id) : UINT32_MAX;
        const u32 inst = e < f->res.visible_stored ? f->res.visible[e].instance : UINT32_MAX;
        const u32 ml = e < f->res.visible_stored ? f->res.visible[e].meshlet : 0u;
        const u32 tri = id != DE__VIS_ID_EMPTY ? de__vis_id_triangle(id) : 0u;
        const bool same_id = inst == key[3 * i] && ml == key[3 * i + 1] && tri == key[3 * i + 2];
        differ += !same_id;
        if (same_id && memcmp(&depth[i], &f->res.depth[i], 4) != 0) {
            ++depth_bits;
            const f32 d = fabsf(depth[i] - f->res.depth[i]);
            depth_far += d > 1e-4f * fabsf(depth[i]) + 1e-7f; /* compare_ref's bound */
        }
    }
    printf("mesh vs compute: %u of %zu px differ in the ID; %u in the depth bits (%u beyond "
           "1e-4)\n",
           differ, np, depth_bits, depth_far);
    EXPECT_LE((u64)differ * 1000u, (u64)np);
    EXPECT_EQ(depth_far, 0u);
    /* Bit for bit where both paths compile through the same toolchain (Metal on the host); a
     * software rasterizer may evaluate the mesh and vertex stages' arithmetic differently. */
    if (b == DE_GPU_METAL)
        EXPECT_EQ(depth_bits, 0u);
    free(key);
    free(depth);
}

/* raster_compute_moltenvk_smoke: a small scene on the Vulkan backend (MoltenVK on macOS, lavapipe
 * on Linux CI) matches the reference. */
static void vulkan_smoke(rcx *f) {
    rcx_open(f, DE_GPU_VULKAN, 64u);
    scene_cloud(f, 24u, 0u);
    ASSERT_TRUE(rcx_frame(f));
    ASSERT_TRUE(f->res.valid);
    expect_matches_cov(f, true, "vulkan smoke", 3u);
}

#define GPU_TESTS(sfx, backend)                                                                    \
    TEST_F(raster_compute, ids_match_cpu_ref_##sfx, rcx) {                                         \
        ids_match_cpu_ref(f, backend);                                                             \
    }                                                                                              \
    TEST_F(raster_compute, two_phase_##sfx, rcx) {                                                 \
        two_phase(f, backend);                                                                     \
    }                                                                                              \
    TEST_F(raster_compute, overflow_clamp_##sfx, rcx) {                                            \
        overflow_clamp(f, backend);                                                                \
    }                                                                                              \
    TEST_F(raster_compute, ids_match_mesh_path_##sfx, rcx) {                                       \
        ids_match_mesh_path(f, backend);                                                           \
    }

GPU_TESTS(metal, DE_GPU_METAL)
GPU_TESTS(vulkan, DE_GPU_VULKAN)
GPU_TESTS(d3d12, DE_GPU_D3D12)

TEST_F(raster_compute, moltenvk_smoke, rcx) {
    vulkan_smoke(f);
}

/* raster_compute.steady_frames_no_alloc: on the null backend, with r.raster_path compute, the
 * compute path's passes run every frame (its stats read back) and steady frames hit the compiled
 * graph cache and allocate nothing (heap or tracked). */
TEST_F(raster_compute, steady_frames_no_alloc, rcx) {
    rcx_open(f, DE_GPU_NULL, 64u);
    scene_cloud(f, 24u, 0u);
    f->no_readback = true;
    for (u32 i = 0; i < 6; ++i)
        ASSERT_TRUE(rcx_frame(f));
    const de__vis_raster_compute_stats st = de__vis_view_raster_compute_stats(f->vis, 0);
    EXPECT_TRUE(st.valid); /* the compute path ran (its stats pass read back) */
    EXPECT_GE(st.capacity, 1u << 18);
    const de__rg_stats before_stats = de__rg_compile_stats(f->g);
    const de_tracking_stats before = de_tracking_allocator_stats(&f->track);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    bool ok = true;
    for (u32 i = 0; i < 20 && ok; ++i)
        ok = rcx_frame(f);
    de_alloc_guard_end(&guard);
    ASSERT_TRUE(ok);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).alloc_count, before.alloc_count);
    const de__rg_stats after_stats = de__rg_compile_stats(f->g);
    EXPECT_EQ(after_stats.compiles, before_stats.compiles);
    EXPECT_EQ(after_stats.hits, before_stats.hits + 20u);
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

/* Counts the four-byte fields of `struct <name>` in `text`. */
static u32 struct_bytes(const char *text, const char *name) {
    const char *ps = strstr(text, name);
    const char *pe = ps != NULL ? strstr(ps, "};") : NULL;
    u32 fields = 0;
    for (const char *ln = ps; ln != NULL && pe != NULL && ln < pe; ln = strchr(ln + 1, '\n'))
        fields += strncmp(ln, "\n    uint ", 10) == 0 || strncmp(ln, "\n    float ", 11) == 0;
    return fields * 4u;
}

/* vis_expand.slang's constants against the C table, and the push-constant blocks' sizes. */
TEST(raster_compute, layout_match) {
    char path[1024];
    snprintf(path, sizeof path, "%s/visbuffer/vis_expand.slang", DE_VIS_TEST_SHADER_DIR);
    char *text = read_text(path);
    ASSERT_TRUE(text != NULL);
    u32 count = 0, found = 0;
    const de__vis_layout_const *k = de__vis_raster_compute_layout_consts(&count);
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
            printf("  %s is not in de__vis_raster_compute_layout_consts\n", name);
        EXPECT_TRUE(ok);
        ++found;
    }
    EXPECT_EQ(found, count);
    EXPECT_EQ(struct_bytes(text, "struct ExpandParams"), 80u);
    free(text);
    snprintf(path, sizeof path, "%s/visbuffer/vis_vs_ps.slang", DE_VIS_TEST_SHADER_DIR);
    text = read_text(path);
    ASSERT_TRUE(text != NULL);
    EXPECT_EQ(struct_bytes(text, "struct DrawParams"), 32u);
    free(text);
    /* The packed ID leaves 7 bits for 124 triangles; the group covers them. */
    EXPECT_GE(DE__VIS_EXPAND_GROUP, DE_COOKED_MESHLET_MAX_TRIANGLES);
    EXPECT_LE(DE_COOKED_MESHLET_MAX_TRIANGLES, DE__VIS_ID_TRIANGLE_MASK + 1u);
    EXPECT_EQ(sizeof(de__vis_draw_indexed_args), 20u);
}
