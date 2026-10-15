/*
 * test_resolve.c: the material resolve of the visibility buffer (DENG-238): reconstructed positions
 * and UVs, texture LOD from analytic derivatives, motion vectors, the G-buffer encoding, the
 * texture-streaming feedback, the Unlit model's dispatch and the tile counts.
 * Executable test_render_resolve; CTest names render_resolve.<suite>.<test>.
 *
 * A fake render world drives a GPU scene of planes: each mesh is a grid of meshlet patches (4 x 4
 * quads, 25 vertices) with positions, normals, tangents and UV0 (F32X2, or UNORM16X2), and its
 * materials have test textures (RGBA8, every mip). The whole vis frame runs (culling, raster,
 * classification, resolve, with the resolve's debug position / UV targets on), and a test pass
 * reads back the G-buffer, the debug targets, the vis IDs, the visible list and classification's
 * arguments. The CPU reference casts the camera ray of each pixel (the inverse view-projection the
 * GPU scene uploaded) against the triangle the GPU's vis ID names, in double precision.
 *
 * The LOD test draws the same triangles with a forward pass (tests/unit/render/data/
 * resolve_lod_ref.slang, cooked at test time when the build has the shader importer) that samples
 * a mip-coded texture with the hardware's derivatives, and compares with what the resolve sampled
 * with its analytic ones.
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
#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/graph/rg_internal.h"
#include "render/loaders/render_assets.h"
#include "render/passes/vis/gbuffer.h"
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

#if defined(DE_RS_TEST_COOK) && DE_RS_TEST_COOK
#include <dengine/de_asset.h>

#include "asset/manifest_internal.h"
#include "cook/importers/shader/shader_importer.h"
#include "cook/importers/shader/slang_compile.h"
#include "cook/lib/de_cooker.h"
#include "gpu/shader/shader_internal.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS (10ull * 1000000000ull)
/* The first frame of a test also pays for the driver compiling the vis frame's shaders: llvmpipe
 * (lavapipe) JIT-compiles each kernel at its first dispatch, about 12 s for the whole frame on a
 * CI runner with a cold shader cache and GPU-assisted validation (DENG-428). Waits before the
 * first frame completes get this bound; every later wait must finish within WAIT_NS. */
#define FIRST_FRAME_WAIT_NS (120ull * 1000000000ull)

static void *zalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    ASSERT_TRUE(p != NULL);
    return p;
}

static void put_u32(u8 *p, u32 v) {
    memcpy(p, &v, 4);
}
static void put_f32(u8 *p, f32 v) {
    memcpy(p, &v, 4);
}

/* IEEE half to float. */
static f32 half_to_f32(u16 h) {
    const u32 s = (u32)(h >> 15) << 31, e = (h >> 10) & 31u, m = h & 1023u;
    u32 bits;
    if (e == 0) {
        if (m == 0) {
            bits = s;
        } else {
            f32 v = (f32)m / 1024.0f / 16384.0f;
            return s ? -v : v;
        }
    } else if (e == 31) {
        bits = s | 0x7F800000u | (m << 13);
    } else {
        bits = s | ((e + 112u) << 23) | (m << 13);
    }
    f32 f;
    memcpy(&f, &bits, 4);
    return f;
}

/* ================================================================================================
 * Meshes: planes of meshlet patches
 * ============================================================================================== */

#define PATCH      5u /* vertices per patch edge */
#define PATCH_VERT (PATCH * PATCH)
#define PATCH_TRIS 32u
#define MAX_MESHES 4u
#define MAX_TEX    8u
#define MAX_MATS   8u
#define STREAM_AL  256u

typedef struct fmesh {
    de_render_mesh rm;
    u32 meshlets, verts;
    f32 *pos; /* object space, per vertex */
    f32 *uv;  /* decoded UV0 per vertex */
    u8 tris[PATCH_TRIS * 3];
    de_gpu_buffer buf[4]; /* vertices, records, meshlet vertices, meshlet triangles */
} fmesh;

static fmesh g_mesh[MAX_MESHES];
static u32 g_mesh_count;
static de_render_material g_mat[MAX_MATS];
static u32 g_mat_count;
static u32 g_tex_bindless[MAX_TEX + 1]; /* by texture handle id (1-based) */

static const de_render_mesh *fake_mesh(void *user, de_mesh m) {
    (void)user;
    return m.id >= 1u && m.id <= g_mesh_count ? &g_mesh[m.id - 1u].rm : NULL;
}
static const de_render_material *fake_material(void *user, de_material m) {
    (void)user;
    return m.id >= 1u && m.id <= g_mat_count ? &g_mat[m.id - 1u] : NULL;
}
static u32 fake_texture(void *user, de_texture t) {
    (void)user;
    return t.id >= 1u && t.id <= MAX_TEX ? g_tex_bindless[t.id] : 0u;
}

static de_gpu_buffer upload_buf(de_gpu_device *dev, const void *data, u64 bytes, u32 stride,
                                const char *name) {
    const de_gpu_buffer b =
        de_gpu_buffer_create(dev, &(de_gpu_buffer_desc){.size = bytes,
                                                        .usage = DE_USAGE_STORAGE,
                                                        .memory = DE_MEM_UPLOAD,
                                                        .stride = stride,
                                                        .name = name});
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    void *m = de_gpu_buffer_map(dev, b);
    ASSERT_TRUE(m != NULL);
    memcpy(m, data, (size_t)bytes);
    return b;
}

/* A plane of `cols` x `rows` patches in z = 0 (x right, y up, normal +z), `size` metres per patch
 * edge, centred on the origin; UV0 = (x, y) / (cols * size) * uv_repeat (v down), stored as F32X2
 * or UNORM16X2. Uploaded to `dev`. */
static void mesh_plane(fmesh *fm, de_gpu_device *dev, u32 cols, u32 rows, f32 size, f32 uv_repeat,
                       bool quantized_uv) {
    memset(fm, 0, sizeof *fm);
    fm->meshlets = cols * rows;
    fm->verts = fm->meshlets * PATCH_VERT;
    fm->pos = (f32 *)zalloc((size_t)fm->verts * 3u * sizeof(f32));
    fm->uv = (f32 *)zalloc((size_t)fm->verts * 2u * sizeof(f32));
    u32 nt = 0;
    for (u32 j = 0; j + 1u < PATCH; ++j)
        for (u32 i = 0; i + 1u < PATCH; ++i) {
            const u8 v00 = (u8)(j * PATCH + i), v10 = (u8)(v00 + 1u);
            const u8 v11 = (u8)(v10 + PATCH), v01 = (u8)(v00 + PATCH);
            const u8 t[6] = {v00, v10, v11, v00, v11, v01}; /* counter-clockwise from +z */
            memcpy(fm->tris + 3u * nt, t, 6);
            nt += 2u;
        }
    const f32 w = (f32)cols * size, h = (f32)rows * size;
    const f32 uv_off[2] = {0.0f, 0.0f}, uv_scale[2] = {uv_repeat / 65535.0f, uv_repeat / 65535.0f};
    for (u32 ml = 0; ml < fm->meshlets; ++ml) {
        const u32 cx = ml % cols, cy = ml / cols;
        for (u32 j = 0; j < PATCH; ++j)
            for (u32 i = 0; i < PATCH; ++i) {
                const u32 v = ml * PATCH_VERT + j * PATCH + i;
                const f32 x = -0.5f * w + ((f32)cx + (f32)i / (f32)(PATCH - 1u)) * size;
                const f32 y = -0.5f * h + ((f32)cy + (f32)j / (f32)(PATCH - 1u)) * size;
                fm->pos[3u * v] = x;
                fm->pos[3u * v + 1u] = y;
                fm->pos[3u * v + 2u] = 0.0f;
                f32 u = (x / w + 0.5f) * uv_repeat, t = (0.5f - y / h) * uv_repeat;
                if (quantized_uv) { /* de_cooked_mesh_uv: offset + q * scale */
                    const u16 qu = (u16)floorf(u / uv_scale[0] + 0.5f);
                    const u16 qv = (u16)floorf(t / uv_scale[1] + 0.5f);
                    u = uv_off[0] + (f32)qu * uv_scale[0];
                    t = uv_off[1] + (f32)qv * uv_scale[1];
                }
                fm->uv[2u * v] = u;
                fm->uv[2u * v + 1u] = t;
            }
    }
    /* Streams: POSITION F32X3, NORMAL OCT16 (+z), TANGENT OCT16_SIGN (+x), UV0. */
    const u32 off_pos = 0, off_n = (fm->verts * 12u + STREAM_AL - 1u) & ~(STREAM_AL - 1u);
    const u32 off_t = off_n + ((fm->verts * 4u + STREAM_AL - 1u) & ~(STREAM_AL - 1u));
    const u32 off_uv = off_t + ((fm->verts * 4u + STREAM_AL - 1u) & ~(STREAM_AL - 1u));
    const u32 uv_bytes = fm->verts * (quantized_uv ? 4u : 8u);
    const u32 vb = off_uv + uv_bytes;
    u8 *data = (u8 *)zalloc(vb);
    for (u32 v = 0; v < fm->verts; ++v) {
        memcpy(data + off_pos + 12u * v, fm->pos + 3u * v, 12);
        put_u32(data + off_n + 4u * v, 0u);     /* oct (0, 0) = +z */
        put_u32(data + off_t + 4u * v, 32767u); /* oct (1, 0) = +x, sign +1 */
        if (quantized_uv) {
            const u32 qu = (u32)floorf(fm->uv[2u * v] / uv_scale[0] + 0.5f);
            const u32 qv = (u32)floorf(fm->uv[2u * v + 1u] / uv_scale[1] + 0.5f);
            put_u32(data + off_uv + 4u * v, qu | (qv << 16));
        } else {
            memcpy(data + off_uv + 8u * v, fm->uv + 2u * v, 8);
        }
    }
    u8 *records = (u8 *)zalloc((size_t)fm->meshlets * DE_COOKED_MESHLET_SIZE);
    u32 *mverts = (u32 *)zalloc((size_t)fm->verts * 4u);
    u8 *mtris = (u8 *)zalloc((size_t)fm->meshlets * PATCH_TRIS * 3u);
    for (u32 ml = 0; ml < fm->meshlets; ++ml) {
        u8 *r = records + (size_t)ml * DE_COOKED_MESHLET_SIZE;
        put_u32(r + 0, ml * PATCH_VERT);
        put_u32(r + 4, ml * PATCH_TRIS * 3u);
        put_u32(r + 8, PATCH_VERT | (PATCH_TRIS << 8));
        const f32 *c0 = fm->pos + 3u * (ml * PATCH_VERT),
                  *c1 = fm->pos + 3u * (ml * PATCH_VERT + PATCH_VERT - 1u);
        put_f32(r + 16, 0.5f * (c0[0] + c1[0]));
        put_f32(r + 20, 0.5f * (c0[1] + c1[1]));
        put_f32(r + 24, 0.0f);
        put_f32(r + 28, 0.75f * size); /* > half the diagonal */
        put_f32(r + 44, 1.0f);         /* cone cutoff 1: no cone */
        for (u32 i = 0; i < PATCH_VERT; ++i)
            mverts[ml * PATCH_VERT + i] = ml * PATCH_VERT + i;
        memcpy(mtris + (size_t)ml * PATCH_TRIS * 3u, fm->tris, sizeof fm->tris);
    }
    de_render_mesh *m = &fm->rm;
    for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s)
        m->stream_offset[s] = UINT32_MAX;
    m->stream_offset[DE_COOKED_STREAM_POSITION] = off_pos;
    m->stream_format[DE_COOKED_STREAM_POSITION] = DE_COOKED_VF_F32X3;
    m->stream_offset[DE_COOKED_STREAM_NORMAL] = off_n;
    m->stream_format[DE_COOKED_STREAM_NORMAL] = DE_COOKED_VF_OCT16;
    m->stream_offset[DE_COOKED_STREAM_TANGENT] = off_t;
    m->stream_format[DE_COOKED_STREAM_TANGENT] = DE_COOKED_VF_OCT16_SIGN;
    m->stream_offset[DE_COOKED_STREAM_UV0] = off_uv;
    m->stream_format[DE_COOKED_STREAM_UV0] =
        quantized_uv ? DE_COOKED_VF_UNORM16X2 : DE_COOKED_VF_F32X2;
    for (u32 k = 0; k < 2; ++k) {
        m->uv_offset[0][k] = uv_off[k];
        m->uv_scale[0][k] = quantized_uv ? uv_scale[k] : 1.0f;
    }
    m->vertex_count = fm->verts;
    m->meshlet_count = fm->meshlets;
    m->meshlet_vertex_count = fm->verts;
    m->meshlet_triangle_bytes = fm->meshlets * PATCH_TRIS * 3u;
    m->submesh_count = 1;
    m->lod_count = 1;
    m->lods[0] = (de_cooked_mesh_lod){.meshlet_count = fm->meshlets,
                                      .triangle_count = fm->meshlets * PATCH_TRIS,
                                      .vertex_count = fm->verts};
    m->bounds_min[0] = -0.5f * w;
    m->bounds_min[1] = -0.5f * h;
    m->bounds_max[0] = 0.5f * w;
    m->bounds_max[1] = 0.5f * h;
    m->bounds_min[2] = -0.01f;
    m->bounds_max[2] = 0.01f;
    m->sphere[3] = 0.5f * sqrtf(w * w + h * h) + 0.01f;
    for (u32 c = 0; c < 3; ++c)
        m->position_scale[c] = 1.0f;
    struct {
        const void *p;
        u64 n;
        u32 which;
    } sec[4] = {{data, vb, DE_RENDER_MESH_VERTICES},
                {records, (u64)fm->meshlets * DE_COOKED_MESHLET_SIZE, DE_RENDER_MESH_MESHLETS},
                {mverts, (u64)fm->verts * 4u, DE_RENDER_MESH_MESHLET_VERTICES},
                {mtris, (u64)fm->meshlets * PATCH_TRIS * 3u, DE_RENDER_MESH_MESHLET_TRIANGLES}};
    for (u32 i = 0; i < 4; ++i) {
        fm->buf[i] = upload_buf(dev, sec[i].p, sec[i].n, 0, "test mesh section");
        m->buffers[sec[i].which] = fm->buf[i];
        m->bindless[sec[i].which] = de_gpu_buffer_bindless(dev, fm->buf[i]);
        m->buffer_bytes[sec[i].which] = sec[i].n;
    }
    free(data);
    free(records);
    free(mverts);
    free(mtris);
}

static void mesh_free(fmesh *fm, de_gpu_device *dev) {
    for (u32 i = 0; i < 4; ++i)
        if (dev != NULL && DE_HANDLE_VALID(fm->buf[i]))
            de_gpu_buffer_destroy(dev, fm->buf[i]);
    free(fm->pos);
    free(fm->uv);
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
    w->f.origin = (de_dvec3){0.0, 0.0, 0.0};
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

/* Slot `s`: mesh `mesh` (1-based), transform `xf` (previous `prev`), material `mat`. */
static void world_set(world *w, u32 s, u32 mesh, de_affine xf, de_affine prev, u32 mat) {
    de__rw_instances *in = &w->f.instances;
    in->entity[s] = 1000u + s;
    in->mesh[s].id = mesh;
    in->transform[s] = xf;
    in->prev_transform[s] = prev;
    in->layer_mask[s] = 1u;
    de_aabb b = {{g_mesh[mesh - 1u].rm.bounds_min[0], g_mesh[mesh - 1u].rm.bounds_min[1],
                  g_mesh[mesh - 1u].rm.bounds_min[2]},
                 {g_mesh[mesh - 1u].rm.bounds_max[0], g_mesh[mesh - 1u].rm.bounds_max[1],
                  g_mesh[mesh - 1u].rm.bounds_max[2]}};
    in->bounds[s] = de_aabb_transform(b, &xf);
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

/* Material `i` (1-based id): a standard material with `base` colour. */
static u32 material_add(de_vec4 base) {
    ASSERT_TRUE(g_mat_count < MAX_MATS);
    de_render_material *m = &g_mat[g_mat_count++];
    memset(m, 0, sizeof *m);
    m->standard.base_color = base;
    m->standard.roughness = 0.5f;
    m->standard.metallic = 0.25f;
    m->standard.specular = 0.5f;
    m->standard.normal_scale = 1.0f;
    m->standard.emissive_strength = 1.0f;
    return g_mat_count;
}

/* ================================================================================================
 * Fixture
 * ============================================================================================== */

typedef struct rs_result {
    bool valid;
    u32 width, height;
    u32 *ids;      /* vis IDs */
    f32 *pos;      /* xyzw per pixel (debug) */
    f32 *uv;       /* uv0 uv1 per pixel (debug) */
    u32 *gb[3];    /* raw words of GB0..GB2 */
    f32 *motion;   /* xy per pixel */
    f32 *lighting; /* rgba per pixel */
    de__vis_meshlet *visible;
    u32 visible_count;
    de__vis_dispatch_args args[DE__VIS_CLASSIFY_LISTS];
    de__gs_instance *inst;
    u32 inst_count;
    de__gs_view view;
    u64 frame;
} rs_result;

typedef struct rsx {
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
    de_vec3 eye, target;
    f32 fov;
    u32 width, height;
    de_gpu_texture tex[MAX_TEX + 1];
    u32 tex_count;
    rs_result res;
    bool warm; /* a frame completed, so the kernels are compiled (FIRST_FRAME_WAIT_NS) */
    /* Classification's per-model counts of each frame (by frame % 8), for the tile check. */
    u32 list_counts[8][DE__GS_MATERIAL_MODELS];
    u64 list_frame[8];
} rsx;

static void rsx_setup(rsx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "resolve_test"}, &f->arena), DE_OK);
    de_error_clear();
    memset(g_mesh, 0, sizeof g_mesh);
    g_mesh_count = 0;
    memset(g_mat, 0, sizeof g_mat);
    g_mat_count = 0;
    memset(g_tex_bindless, 0, sizeof g_tex_bindless);
    f->eye = (de_vec3){0.0f, 0.0f, 10.0f};
    f->target = (de_vec3){0.0f, 0.0f, 0.0f};
    f->fov = 0.9f;
    f->width = 320;
    f->height = 192;
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

static bool cvar_set(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (!DE_HANDLE_VALID(cv))
        return false;
    EXPECT_EQ(de_cvar_set(cv, value), DE_OK);
    return true;
}

static void free_result(rs_result *r) {
    free(r->ids);
    free(r->pos);
    free(r->uv);
    for (u32 i = 0; i < 3; ++i)
        free(r->gb[i]);
    free(r->motion);
    free(r->lighting);
    free(r->visible);
    free(r->inst);
    memset(r, 0, sizeof *r);
}

static void rsx_teardown(rsx *f) {
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
        for (u32 i = 0; i <= MAX_TEX; ++i)
            if (DE_HANDLE_VALID(f->tex[i]))
                de_gpu_texture_destroy(f->dev, f->tex[i]);
        if (DE_HANDLE_VALID(f->rb))
            de_gpu_buffer_destroy(f->dev, f->rb);
        if (DE_HANDLE_VALID(f->fence))
            de_gpu_fence_destroy(f->dev, f->fence);
        de_gpu_wait_idle(f->dev);
        de_gpu_device_destroy(f->dev);
    }
    free_result(&f->res);
    world_free(&f->w);
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
    const char *const names[] = {"r.lod_bias", "r.occlusion", "r.rg_record_cost"};
    const char *const values[] = {"0", "1", "32"};
    for (u32 i = 0; i < 3; ++i) {
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

static void rsx_open(rsx *f, de_gpu_backend backend) {
    static char why[700];
    if (!backend_built(backend)) {
        snprintf(why, sizeof why, "the %s backend is not in this build",
                 de_gpu_backend_name(backend));
        SKIP(why);
    }
    f->backend = backend;
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
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "rs"},
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
    gd.initial_capacity[DE__GS_INSTANCES] = 32;
    ASSERT_EQ(de__gpu_scene_create(&gd, &f->gs), DE_OK);
    ASSERT_EQ(de__vis_shared_create(
                  &(de__vis_shared_desc){.device = f->dev, .allocator = &f->alloc}, &f->vis),
              DE_OK);
    if (!f->vis->enabled) {
        snprintf(why, sizeof why, "the vis frame is off: %.600s", f->vis->why);
        SKIP(why);
    }
    world_init(&f->w, 32);
    (void)cvar_set("r.lod_bias", "-7");
    (void)cvar_set("r.occlusion", "0");
    de__vis_resolve_set_debug(f->vis, &(de__vis_resolve_debug){.targets = true, .readback = true});
    /* One command list per submission on Vulkan: VVL 1.3.275 (GPU-assisted) checks bindless
     * accesses against the layout at the end of each list (see test_raster_compute.c). */
    if (backend == DE_GPU_VULKAN && !cvar_set("r.rg_record_cost", "1048576")) {
        char *args[] = {"test", "+r.rg_record_cost", "1048576"};
        EXPECT_EQ(de_cvar_apply_args(3, args), DE_OK);
    }
}

static u32 rsx_mesh_plane(rsx *f, u32 cols, u32 rows, f32 size, f32 uv_repeat, bool quantized) {
    ASSERT_TRUE(g_mesh_count < MAX_MESHES);
    mesh_plane(&g_mesh[g_mesh_count], f->dev, cols, rows, size, uv_repeat, quantized);
    return ++g_mesh_count;
}

/* ---------- Textures ---------- */

static bool rsx_wait(rsx *f, u64 v, const char *what);

typedef struct tex_pass {
    rsx *f;
    de_rg_resource t;
    de_gpu_buffer src;
    u32 mips, size;
    u64 off[16];
    u32 pitch[16];
} tex_pass;

static void tex_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const tex_pass *p = (const tex_pass *)user;
    for (u32 m = 0; m < p->mips; ++m)
        de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, p->t), m, 0, p->src, p->off[m],
                                          p->pitch[m]);
}

/* A `size` x `size` RGBA8_UNORM texture with every mip; mip k filled with texels (r(k), g, b,
 * 255), r(k) = `mip_r[k]`. Uploaded in a graph of its own; returns its handle id (1-based). */
static u32 rsx_texture(rsx *f, u32 size, const u8 *mip_r, u8 g, u8 b) {
    ASSERT_TRUE(f->tex_count < MAX_TEX);
    u32 mips = 1;
    while ((size >> mips) > 0)
        ++mips;
    const u32 id = ++f->tex_count;
    f->tex[id] = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                       .width = size,
                                       .height = size,
                                       .mip_count = mips,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                       .name = "test texture"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->tex[id]));
    g_tex_bindless[id] = de_gpu_texture_bindless(f->dev, f->tex[id], 0, false);
    tex_pass *tp = (tex_pass *)zalloc(sizeof *tp);
    tp->f = f;
    tp->mips = mips;
    tp->size = size;
    u64 total = 0;
    for (u32 m = 0; m < mips; ++m) {
        const u32 s = size >> m;
        tp->pitch[m] = (s * 4u + 255u) & ~255u;
        tp->off[m] = total;
        total = (total + (u64)tp->pitch[m] * s + 511u) & ~(u64)511u;
    }
    tp->src = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = total,
                                                                 .usage = DE_USAGE_COPY_SRC,
                                                                 .memory = DE_MEM_UPLOAD,
                                                                 .name = "test texture upload"});
    ASSERT_TRUE(DE_HANDLE_VALID(tp->src));
    u8 *map = (u8 *)de_gpu_buffer_map(f->dev, tp->src);
    for (u32 m = 0; m < mips; ++m) {
        const u32 s = size >> m;
        for (u32 y = 0; y < s; ++y)
            for (u32 x = 0; x < s; ++x) {
                u8 *t = map + tp->off[m] + (u64)y * tp->pitch[m] + 4u * x;
                t[0] = mip_r[m];
                t[1] = g;
                t[2] = b;
                t[3] = 255;
            }
    }
    const u64 n = f->frames++;
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    tp->t = de_rg_import_texture(f->g, f->tex[id], DE_ACCESS_NONE);
    de_rg_pass *p = de_rg_add_pass(f->g, "test.texture_upload", DE_QUEUE_GRAPHICS, tex_execute, tp);
    de_rg_write(p, tp->t, DE_ACCESS_COPY_DST);
    de_rg_export(f->g, tp->t, DE_ACCESS_SHADER_READ);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
    ASSERT_EQ(de__rg_compile(f->g), DE_OK);
    ASSERT_EQ(de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = n,
                                                          .frame_fence = f->fence,
                                                          .frame_value = n + 1}),
              DE_OK);
    ASSERT_TRUE(rsx_wait(f, n + 1, "texture upload"));
    de_gpu_buffer_destroy(f->dev, tp->src);
    free(tp);
    return id;
}

/* ---------- One frame ---------- */

typedef struct rb_item {
    de_rg_resource r;
    bool texture;
    u64 off, bytes;
    u32 pitch;
} rb_item;

enum { RB_IDS, RB_POS, RB_UV, RB_GB0, RB_GB1, RB_GB2, RB_MOTION, RB_LIGHT, RB_VIS, RB_ARGS, RB_N };

typedef struct rb_pass {
    rsx *f;
    rb_item it[RB_N];
} rb_pass;

static void rb_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    for (u32 i = 0; i < RB_N; ++i) {
        const rb_item *it = &p->it[i];
        if (it->texture)
            de_gpu_cmd_copy_texture_to_buffer(cmd, p->f->rb, it->off, it->pitch,
                                              de_rg_texture(pass, it->r), 0, 0);
        else
            de_gpu_cmd_copy_buffer(cmd, p->f->rb, it->off, de_rg_buffer(pass, it->r), 0, it->bytes);
    }
}

static void camera(const rsx *f, de_mat4 *view, de_mat4 *proj) {
    *view = de_m4_look_at(f->eye, f->target, (de_vec3){0.0f, 1.0f, 0.0f});
    *proj = de_m4_perspective_rz(f->fov, (f32)f->width / (f32)f->height, 0.1f);
}

static u64 align_up(u64 v, u64 a) {
    return (v + a - 1u) / a * a;
}

/* Copies the rows of a readback image into a tight array. */
static void *rows_of(const u8 *m, const rb_item *it, u32 w, u32 h, u32 bpp) {
    u8 *out = (u8 *)zalloc((size_t)w * h * bpp);
    for (u32 y = 0; y < h; ++y)
        memcpy(out + (size_t)y * w * bpp, m + it->off + (u64)y * it->pitch, (size_t)w * bpp);
    return out;
}

/* Waits for value `v` of the fixture's fence (FIRST_FRAME_WAIT_NS until a frame completed, then
 * WAIT_NS). On a timeout it says which wait failed, after how long and where the fence is. */
static bool rsx_wait(rsx *f, u64 v, const char *what) {
    const u64 t0 = de_time_now_ns();
    const de_result r =
        de_gpu_fence_wait(f->dev, f->fence, v, f->warm ? WAIT_NS : FIRST_FRAME_WAIT_NS);
    const f64 ms = (f64)(de_time_now_ns() - t0) / 1e6;
    if (r == DE_OK) {
        if (!f->warm && strcmp(what, "frame") == 0)
            printf("first frame (includes the shader compile at first use): %.0f ms\n", ms);
        return true;
    }
    fprintf(stderr, "%s: the fence wait for value %llu failed after %.0f ms (%s); fence at %llu\n",
            what, (unsigned long long)v, ms, de_result_str(r),
            (unsigned long long)de_gpu_fence_value(f->dev, f->fence));
    return false;
}

/* Builds, runs and waits for one frame; fills f->res. */
static bool rsx_frame(rsx *f) {
    const u64 n = f->frames++;
    world_begin(&f->w);
    de_mat4 view, proj;
    camera(f, &view, &proj);
    if (de__gpu_scene_update(f->gs, &f->w.f) != DE_OK)
        return false;
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
    rb_pass *rp = NULL;
    const bool want = DE_HANDLE_VALID(vc->gbuffer[0]) && DE_HANDLE_VALID(vc->resolve_debug_pos);
    if (want) {
        rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
        ASSERT_TRUE(rp != NULL);
        rp->f = f;
        const de_rg_resource tex[] = {vc->vis_ids,
                                      vc->resolve_debug_pos,
                                      vc->resolve_debug_uv,
                                      vc->gbuffer[DE__GB0],
                                      vc->gbuffer[DE__GB1],
                                      vc->gbuffer[DE__GB2],
                                      vc->gbuffer[DE__GB_MOTION],
                                      vc->gbuffer[DE__GB_LIGHTING]};
        static const u32 bpp[] = {4, 16, 16, 4, 4, 4, 4, 8};
        u64 at = 0;
        for (u32 i = 0; i < 8; ++i) {
            rb_item *it = &rp->it[i];
            it->r = tex[i];
            it->texture = true;
            it->pitch = (u32)align_up((u64)f->width * bpp[i], 256u);
            it->off = at;
            it->bytes = (u64)it->pitch * f->height;
            at = align_up(at + it->bytes, 512u);
        }
        rp->it[RB_VIS] =
            (rb_item){.r = vc->visible_meshlets,
                      .off = at,
                      .bytes = (u64)(vc->visible_capacity + vc->visible_alpha_capacity) *
                               sizeof(de__vis_meshlet)};
        at = align_up(at + rp->it[RB_VIS].bytes, 512u);
        rp->it[RB_ARGS] =
            (rb_item){.r = vc->classify_args, .off = at, .bytes = DE__VIS_CLASSIFY_LISTS * 16u};
        at += rp->it[RB_ARGS].bytes;
        if (at > f->rb_size) {
            if (DE_HANDLE_VALID(f->rb)) {
                de_gpu_wait_idle(f->dev);
                de_gpu_buffer_destroy(f->dev, f->rb);
            }
            f->rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = at,
                                                                       .usage = DE_USAGE_COPY_DST,
                                                                       .memory = DE_MEM_READBACK,
                                                                       .name = "resolve readback"});
            ASSERT_TRUE(DE_HANDLE_VALID(f->rb));
            f->rb_size = at;
        }
        de_rg_pass *p = de_rg_add_pass(f->g, "test.readback", DE_QUEUE_GRAPHICS, rb_execute, rp);
        for (u32 i = 0; i < RB_N; ++i)
            de_rg_read(p, rp->it[i].r, DE_ACCESS_COPY_SRC);
        de_rg_write(p, de_rg_import_buffer(f->g, f->rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
        /* Back to the layouts the bindless reads expect before the list ends (VVL checks them
         * against the end of the list, not the access): the depth sampled, the targets storage. */
        p = de_rg_add_pass(f->g, "test.restore", DE_QUEUE_GRAPHICS, NULL, NULL);
        de_rg_pass_side_effect(p);
        de_rg_read(p, vc->depth, DE_ACCESS_SHADER_READ);
        for (u32 i = 1; i < 8; ++i)
            de_rg_write(p, tex[i], DE_ACCESS_SHADER_WRITE);
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
    rs_result *res = &f->res;
    free_result(res);
    res->frame = n;
    res->inst_count = de__gpu_scene_table(f->gs, DE__GS_INSTANCES).count;
    res->inst = (de__gs_instance *)zalloc((size_t)res->inst_count * sizeof(de__gs_instance));
    for (u32 i = 0; i < res->inst_count; ++i)
        de__gpu_scene_debug_pack(f->gs, DE__GS_INSTANCES, i, &res->inst[i]);
    de__gpu_scene_debug_pack(f->gs, DE__GS_VIEWS, 0, &res->view);
    de__gpu_scene_frame_end(f->gs, f->g, true);
    if (!rsx_wait(f, n + 1, "frame"))
        return false;
    f->warm = true;
    if (!want)
        return true;
    const u8 *m = (const u8 *)de_gpu_buffer_map(f->dev, f->rb);
    const u32 w = f->width, h = f->height;
    res->width = w;
    res->height = h;
    res->ids = (u32 *)rows_of(m, &rp->it[RB_IDS], w, h, 4);
    res->pos = (f32 *)rows_of(m, &rp->it[RB_POS], w, h, 16);
    res->uv = (f32 *)rows_of(m, &rp->it[RB_UV], w, h, 16);
    for (u32 i = 0; i < 3; ++i)
        res->gb[i] = (u32 *)rows_of(m, &rp->it[RB_GB0 + i], w, h, 4);
    u16 *mo = (u16 *)rows_of(m, &rp->it[RB_MOTION], w, h, 4);
    u16 *li = (u16 *)rows_of(m, &rp->it[RB_LIGHT], w, h, 8);
    res->motion = (f32 *)zalloc((size_t)w * h * 2u * sizeof(f32));
    res->lighting = (f32 *)zalloc((size_t)w * h * 4u * sizeof(f32));
    for (size_t i = 0; i < (size_t)w * h * 2u; ++i)
        res->motion[i] = half_to_f32(mo[i]);
    for (size_t i = 0; i < (size_t)w * h * 4u; ++i)
        res->lighting[i] = half_to_f32(li[i]);
    free(mo);
    free(li);
    res->visible_count = (u32)(rp->it[RB_VIS].bytes / sizeof(de__vis_meshlet));
    res->visible = (de__vis_meshlet *)zalloc((size_t)rp->it[RB_VIS].bytes);
    memcpy(res->visible, m + rp->it[RB_VIS].off, (size_t)rp->it[RB_VIS].bytes);
    memcpy(res->args, m + rp->it[RB_ARGS].off, sizeof res->args);
    for (u32 k = 0; k < DE__GS_MATERIAL_MODELS; ++k)
        f->list_counts[n % 8u][k] = res->args[k].count;
    f->list_frame[n % 8u] = n;
    res->valid = true;
    return true;
}

/* ================================================================================================
 * CPU reference
 * ============================================================================================== */

/* The triangle a pixel's vis ID names: slot, mesh, the three mesh vertices. */
typedef struct px_tri {
    bool ok;
    u32 slot;
    const fmesh *fm;
    u32 v[3];
} px_tri;

static px_tri pixel_triangle(const rsx *f, u32 id) {
    px_tri t = {0};
    const rs_result *r = &f->res;
    const u32 e = de__vis_id_meshlet(id), tri = de__vis_id_triangle(id);
    if (id == DE__VIS_ID_EMPTY || e >= r->visible_count)
        return t;
    const de__vis_meshlet vm = r->visible[e];
    if (vm.instance >= f->w.f.instances.count)
        return t;
    const u32 mesh = f->w.f.instances.mesh[vm.instance].id;
    if (mesh < 1u || mesh > g_mesh_count)
        return t;
    t.fm = &g_mesh[mesh - 1u];
    if (vm.meshlet >= t.fm->meshlets || tri >= PATCH_TRIS)
        return t;
    t.slot = vm.instance;
    for (u32 k = 0; k < 3; ++k)
        t.v[k] = vm.meshlet * PATCH_VERT + t.fm->tris[3u * tri + k];
    t.ok = true;
    return t;
}

static void xform(const de__gs_instance *in, const f32 *p, f64 out[3]) {
    for (u32 r = 0; r < 3; ++r) {
        const f32 *row = in->transform[r].v;
        out[r] = (f64)row[0] * p[0] + (f64)row[1] * p[1] + (f64)row[2] * p[2] + (f64)row[3];
    }
}

/* NDC (x, y, z) through the inverse view-projection rows (double). */
static void unproject(const de__gs_view *v, f64 x, f64 y, f64 z, f64 out[3]) {
    f64 h[4];
    for (u32 r = 0; r < 4; ++r) {
        const f32 *row = v->inv_view_proj[r].v;
        h[r] = row[0] * x + row[1] * y + row[2] * z + row[3];
    }
    for (u32 k = 0; k < 3; ++k)
        out[k] = h[k] / h[3];
}

static void sub3(const f64 *a, const f64 *b, f64 *o) {
    o[0] = a[0] - b[0];
    o[1] = a[1] - b[1];
    o[2] = a[2] - b[2];
}
static void cross3(const f64 *a, const f64 *b, f64 *o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static f64 dot3(const f64 *a, const f64 *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Barycentrics where the camera ray of pixel (px, py) meets the triangle's plane. */
static bool ref_bary(const rsx *f, const px_tri *t, u32 px, u32 py, f64 bary[3], f64 world[3]) {
    const rs_result *r = &f->res;
    f64 w[3][3] = {{0}};
    for (u32 k = 0; k < 3; ++k)
        xform(&r->inst[t->slot], t->fm->pos + 3u * t->v[k], w[k]);
    const f64 x = ((f64)px + 0.5) / r->width * 2.0 - 1.0,
              y = 1.0 - ((f64)py + 0.5) / r->height * 2.0;
    f64 o[3] = {0}, b[3] = {0}, d[3] = {0};
    unproject(&r->view, x, y, 1.0, o);
    unproject(&r->view, x, y, 0.5, b);
    sub3(b, o, d);
    f64 e1[3], e2[3], p[3], s[3], q[3];
    sub3(w[1], w[0], e1);
    sub3(w[2], w[0], e2);
    cross3(d, e2, p);
    const f64 det = dot3(e1, p);
    if (fabs(det) < 1e-30)
        return false;
    sub3(o, w[0], s);
    const f64 u = dot3(s, p) / det;
    cross3(s, e1, q);
    const f64 v = dot3(d, q) / det;
    bary[0] = 1.0 - u - v;
    bary[1] = u;
    bary[2] = v;
    for (u32 k = 0; k < 3; ++k)
        world[k] = bary[0] * w[0][k] + bary[1] * w[1][k] + bary[2] * w[2][k];
    return true;
}

/* ================================================================================================
 * GPU tests
 * ============================================================================================== */

static de_affine trs(de_vec3 t, de_vec3 axis, f32 angle, f32 s) {
    return de_affine_from_trs(t, de_quat_axis_angle(axis, angle), (de_vec3){s, s, s});
}

/* resolve_positions_uvs_cpu_ref: reconstructed world positions and UVs (F32X2 and UNORM16X2
 * meshes, tilted planes) against the CPU ray cast within 1e-4 relative; the GB1 normal against
 * the plane's within 0.1 degrees. */
static void positions_uvs(rsx *f, de_gpu_backend b) {
    rsx_open(f, b);
    const u32 a = rsx_mesh_plane(f, 3, 2, 1.0f, 2.0f, false);
    const u32 q = rsx_mesh_plane(f, 2, 2, 1.5f, 3.0f, true);
    const u32 mat = material_add((de_vec4){0.8f, 0.6f, 0.4f, 1.0f});
    const de_affine x0 = trs((de_vec3){-1.8f, 0.6f, 0.0f}, (de_vec3){1, 0, 0}, -0.6f, 1.2f);
    const de_affine x1 = trs((de_vec3){1.9f, -0.4f, -1.0f}, (de_vec3){0, 1, 0}, 0.5f, 1.0f);
    world_set(&f->w, 0, a, x0, x0, mat);
    world_set(&f->w, 1, q, x1, x1, mat);
    ASSERT_TRUE(rsx_frame(f));
    ASSERT_TRUE(f->res.valid);
    const rs_result *r = &f->res;
    u32 covered = 0, pos_bad = 0, uv_bad = 0, n_bad = 0;
    f64 worst_pos = 0.0, worst_uv = 0.0, worst_n = 0.0;
    for (u32 y = 0; y < r->height; ++y)
        for (u32 x = 0; x < r->width; ++x) {
            const size_t i = (size_t)y * r->width + x;
            const px_tri t = pixel_triangle(f, r->ids[i]);
            if (!t.ok)
                continue;
            f64 bary[3] = {0, 0, 0}, world[3] = {0, 0, 0};
            ASSERT_TRUE(ref_bary(f, &t, x, y, bary, world));
            ++covered;
            const f64 cam[3] = {f->eye.x, f->eye.y, f->eye.z};
            f64 dpos[3] = {r->pos[4 * i] - world[0], r->pos[4 * i + 1] - world[1],
                           r->pos[4 * i + 2] - world[2]};
            f64 to_cam[3];
            sub3(world, cam, to_cam);
            const f64 epos = sqrt(dot3(dpos, dpos)) / sqrt(dot3(to_cam, to_cam));
            worst_pos = epos > worst_pos ? epos : worst_pos;
            pos_bad += epos > 1e-4;
            for (u32 c = 0; c < 2; ++c) {
                f64 uv = 0.0;
                for (u32 k = 0; k < 3; ++k)
                    uv += bary[k] * t.fm->uv[2u * t.v[k] + c];
                const f64 euv = fabs(r->uv[4 * i + c] - uv) / (fabs(uv) > 1.0 ? fabs(uv) : 1.0);
                worst_uv = euv > worst_uv ? euv : worst_uv;
                uv_bad += euv > 1e-4;
            }
            /* The plane's normal: +z through the instance's rotation. */
            f64 nz[3];
            const de__gs_instance *in = &r->inst[t.slot];
            for (u32 k = 0; k < 3; ++k)
                nz[k] = in->transform[k].v[2];
            const f64 l = sqrt(dot3(nz, nz));
            f32 n[3], rough;
            de__gb1_unpack(r->gb[1][i], n, &rough);
            const f64 nd[3] = {n[0], n[1], n[2]};
            f64 c = dot3(nd, nz) / l;
            c = c > 1.0 ? 1.0 : c;
            const f64 ang = acos(c) * 180.0 / 3.14159265358979;
            worst_n = ang > worst_n ? ang : worst_n;
            n_bad += ang > 0.1;
            EXPECT_TRUE(fabsf(rough - 0.5f) <= 1.0f / 1023.0f);
        }
    printf("positions/UVs: %u px; worst position error %.2e (relative to the distance), UV %.2e, "
           "normal %.4f deg\n",
           covered, worst_pos, worst_uv, worst_n);
    EXPECT_GT(covered, 2000u);
    EXPECT_EQ(pos_bad, 0u);
    EXPECT_EQ(uv_bad, 0u);
    EXPECT_EQ(n_bad, 0u);
    /* GB0 (sRGB-encoded) and GB2. */
    u32 checked = 0;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        if (r->ids[i] == DE__VIS_ID_EMPTY)
            continue;
        const u32 g0 = r->gb[0][i], g2 = r->gb[2][i];
        const f32 base[3] = {0.8f, 0.6f, 0.4f};
        for (u32 c = 0; c < 3; ++c) {
            const f32 lin = de__gb_srgb_decode((f32)((g0 >> (8u * c)) & 255u) / 255.0f);
            EXPECT_TRUE(fabsf(lin - base[c]) <= 0.01f);
        }
        EXPECT_EQ(g0 >> 24, 255u);   /* AO 1 */
        EXPECT_LE((g2 & 255u), 65u); /* metallic 0.25 */
        EXPECT_GE((g2 & 255u), 62u);
        EXPECT_EQ((g2 >> 8) & 255u, 0u);  /* Standard */
        EXPECT_EQ((g2 >> 16) & 255u, 0u); /* no flags */
        ++checked;
        if (checked > 500u)
            break;
    }
}

/* resolve_motion_vectors_static_zero: a static camera and scene give zero motion. */
static void motion_static(rsx *f, de_gpu_backend b) {
    rsx_open(f, b);
    const u32 a = rsx_mesh_plane(f, 3, 3, 1.0f, 1.0f, false);
    const u32 mat = material_add((de_vec4){1, 1, 1, 1});
    const de_affine x0 = trs((de_vec3){0, 0, 0}, (de_vec3){1, 0, 0}, -0.4f, 1.5f);
    world_set(&f->w, 0, a, x0, x0, mat);
    ASSERT_TRUE(rsx_frame(f));
    ASSERT_TRUE(rsx_frame(f));
    ASSERT_TRUE(f->res.valid);
    u32 covered = 0, bad = 0;
    for (size_t i = 0; i < (size_t)f->width * f->height; ++i) {
        if (f->res.ids[i] == DE__VIS_ID_EMPTY) {
            EXPECT_TRUE(f->res.motion[2 * i] == 0.0f);
            continue;
        }
        ++covered;
        bad += fabsf(f->res.motion[2 * i]) > 1e-5f || fabsf(f->res.motion[2 * i + 1]) > 1e-5f;
    }
    printf("static: %u px, %u with motion\n", covered, bad);
    EXPECT_GT(covered, 1000u);
    EXPECT_EQ(bad, 0u);
}

/* The expected motion of pixel i (current instance rows from res, previous transform `prev`). */
static void expect_motion(rsx *f, const de_affine *now_xf, const de_affine *prev_xf, u32 slot,
                          const char *what) {
    const rs_result *r = &f->res;
    const de_affine inv = de_affine_inverse(now_xf);
    u32 n = 0, bad = 0;
    f64 worst = 0.0;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        const px_tri t = pixel_triangle(f, r->ids[i]);
        if (!t.ok || t.slot != slot)
            continue;
        const de_vec3 wpos = {r->pos[4 * i], r->pos[4 * i + 1], r->pos[4 * i + 2]};
        const de_vec3 obj = de_affine_transform_point(&inv, wpos);
        const de_vec3 prev_w = de_affine_transform_point(prev_xf, obj);
        f64 now[4], then[4];
        for (u32 k = 0; k < 4; ++k) {
            const f32 *a = r->view.view_proj[k].v, *b = r->view.prev_view_proj[k].v;
            now[k] = (f64)a[0] * wpos.x + (f64)a[1] * wpos.y + (f64)a[2] * wpos.z + a[3];
            then[k] = (f64)b[0] * prev_w.x + (f64)b[1] * prev_w.y + (f64)b[2] * prev_w.z + b[3];
        }
        const f64 ex = now[0] / now[3] - then[0] / then[3];
        const f64 ey = now[1] / now[3] - then[1] / then[3];
        const f64 dx = r->motion[2 * i] - ex, dy = r->motion[2 * i + 1] - ey;
        const f64 err = sqrt(dx * dx + dy * dy);
        const f64 tol = 2e-3 * sqrt(ex * ex + ey * ey) + 5e-4;
        worst = err > worst ? err : worst;
        bad += err > tol;
        ++n;
    }
    printf("%s: %u px, worst motion error %.2e\n", what, n, worst);
    EXPECT_GT(n, 500u);
    EXPECT_EQ(bad, 0u);
}

/* resolve_motion_vectors_moving: a translating instance (object motion) and a moving camera
 * (camera motion) against the CPU reference. */
static void motion_moving(rsx *f, de_gpu_backend b) {
    rsx_open(f, b);
    const u32 a = rsx_mesh_plane(f, 3, 3, 1.0f, 1.0f, false);
    const u32 mat = material_add((de_vec4){1, 1, 1, 1});
    const de_affine x0 = trs((de_vec3){0, 0, 0}, (de_vec3){1, 0, 0}, -0.4f, 1.5f);
    world_set(&f->w, 0, a, x0, x0, mat);
    ASSERT_TRUE(rsx_frame(f));
    /* The instance moves (its previous transform is x0). */
    const de_affine x1 = trs((de_vec3){0.35f, -0.2f, 0.1f}, (de_vec3){1, 0, 0}, -0.4f, 1.5f);
    world_set(&f->w, 0, a, x1, x0, mat);
    ASSERT_TRUE(rsx_frame(f));
    ASSERT_TRUE(f->res.valid);
    expect_motion(f, &x1, &x0, 0, "object motion");
    /* Still instance, the camera moves. */
    world_set(&f->w, 0, a, x1, x1, mat);
    ASSERT_TRUE(rsx_frame(f));
    f->eye = (de_vec3){0.6f, 0.3f, 9.5f};
    ASSERT_TRUE(rsx_frame(f));
    ASSERT_TRUE(f->res.valid);
    expect_motion(f, &x1, &x1, 0, "camera motion");
}

/* resolve_feedback_mips: a magnified texture reports mip 0, a minified one a higher mip near
 * the CPU estimate; the readback arrives the next frame. */
static void feedback_mips(rsx *f, de_gpu_backend b) {
    rsx_open(f, b);
    static const u8 r0[16] = {0};
    const u32 near_tex = rsx_texture(f, 32, r0, 128, 128);
    const u32 far_tex = rsx_texture(f, 256, r0, 128, 128);
    const u32 big = rsx_mesh_plane(f, 4, 3, 1.5f, 1.0f, false);
    const u32 small_mesh = rsx_mesh_plane(f, 1, 1, 0.4f, 1.0f, false);
    const u32 m_near = material_add((de_vec4){1, 1, 1, 1});
    g_mat[m_near - 1u].textures[DE_COOKED_TEX_BASE_COLOR].id = near_tex;
    const u32 m_far = material_add((de_vec4){1, 1, 1, 1});
    g_mat[m_far - 1u].textures[DE_COOKED_TEX_BASE_COLOR].id = far_tex;
    const de_affine id = trs((de_vec3){0, 0, 0}, (de_vec3){0, 0, 1}, 0.0f, 1.0f);
    const de_affine fx = trs((de_vec3){2.5f, 1.5f, 1.0f}, (de_vec3){0, 0, 1}, 0.0f, 1.0f);
    world_set(&f->w, 0, big, id, id, m_near);
    world_set(&f->w, 1, small_mesh, fx, fx, m_far);
    const u32 tn = g_tex_bindless[near_tex], tf = g_tex_bindless[far_tex];
    de__vis_resolve_stats st = {0};
    u32 far_mip = DE__VIS_FEEDBACK_NONE, near_mip = DE__VIS_FEEDBACK_NONE;
    for (u32 i = 0; i < 40u && (far_mip == DE__VIS_FEEDBACK_NONE || near_mip != 0u); ++i) {
        ASSERT_TRUE(rsx_frame(f));
        st = de__vis_view_resolve_stats(f->vis, 0);
        if (!st.valid)
            continue;
        EXPECT_GE(st.frame + 2u, f->res.frame); /* at most 2 frames late */
        ASSERT_TRUE(tn < st.feedback_count && tf < st.feedback_count);
        if (st.feedback[tn] != DE__VIS_FEEDBACK_NONE)
            near_mip = st.feedback[tn] < near_mip ? st.feedback[tn] : near_mip;
        if (st.feedback[tf] != DE__VIS_FEEDBACK_NONE)
            far_mip = st.feedback[tf] < far_mip ? st.feedback[tf] : far_mip;
    }
    /* The far plane: 256 texels over its width on screen. */
    u32 minx = f->width, maxx = 0;
    for (size_t i = 0; i < (size_t)f->width * f->height; ++i) {
        const px_tri t = pixel_triangle(f, f->res.ids[i]);
        if (!t.ok || t.slot != 1u)
            continue;
        const u32 x = (u32)(i % f->width);
        minx = x < minx ? x : minx;
        maxx = x > maxx ? x : maxx;
    }
    const f64 px = maxx >= minx ? (f64)(maxx - minx + 1u) : 1.0;
    const f64 expect = log2(256.0 / px);
    printf("feedback: near %u, far %u (CPU estimate %.2f for %.0f px)\n", near_mip, far_mip, expect,
           px);
    EXPECT_EQ(near_mip, 0u);
    EXPECT_NE(far_mip, DE__VIS_FEEDBACK_NONE);
    EXPECT_TRUE(fabs((f64)far_mip - expect) <= 1.0);
    EXPECT_GE(far_mip, 2u);
}

/* resolve_unlit_model_dispatch: an Unlit material's pixels get its base colour in GB0 and as
 * radiance, GB2 the Unlit model and flag; Standard pixels beside them the Standard model; both
 * models' dispatches processed tiles. */
static void unlit_dispatch(rsx *f, de_gpu_backend b) {
    rsx_open(f, b);
    const u32 a = rsx_mesh_plane(f, 2, 2, 1.0f, 1.0f, false);
    const u32 m_std = material_add((de_vec4){0.9f, 0.9f, 0.9f, 1.0f});
    const u32 m_unlit = material_add((de_vec4){0.2f, 0.5f, 0.8f, 1.0f});
    g_mat[m_unlit - 1u].standard.unlit = true;
    const de_affine l = trs((de_vec3){-1.2f, 0, 0}, (de_vec3){0, 0, 1}, 0.0f, 1.0f);
    const de_affine rr = trs((de_vec3){1.2f, 0, 0}, (de_vec3){0, 0, 1}, 0.0f, 1.0f);
    world_set(&f->w, 0, a, l, l, m_std);
    world_set(&f->w, 1, a, rr, rr, m_unlit);
    de__vis_resolve_stats st = {0};
    for (u32 i = 0; i < 4u && !st.valid; ++i) {
        ASSERT_TRUE(rsx_frame(f));
        st = de__vis_view_resolve_stats(f->vis, 0);
    }
    ASSERT_TRUE(st.valid);
    const rs_result *r = &f->res;
    u32 unlit = 0, standard = 0;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        const px_tri t = pixel_triangle(f, r->ids[i]);
        if (!t.ok)
            continue;
        const u32 g2 = r->gb[2][i];
        if (t.slot == 1u) {
            ++unlit;
            EXPECT_EQ((g2 >> 8) & 255u, (u32)DE__GS_MATERIAL_MODEL_UNLIT);
            EXPECT_EQ((g2 >> 16) & 255u, (u32)DE__GB_FLAG_UNLIT);
            EXPECT_TRUE(fabsf(r->lighting[4 * i] - 0.2f) <= 2e-3f);
            EXPECT_TRUE(fabsf(r->lighting[4 * i + 1] - 0.5f) <= 2e-3f);
            EXPECT_TRUE(fabsf(r->lighting[4 * i + 2] - 0.8f) <= 2e-3f);
            EXPECT_EQ(r->gb[1][i], 0u); /* Unlit writes no normal */
        } else {
            ++standard;
            EXPECT_EQ((g2 >> 8) & 255u, (u32)DE__GS_MATERIAL_MODEL_STANDARD);
            EXPECT_TRUE(r->lighting[4 * i] == 0.0f); /* no emissive */
            EXPECT_TRUE(r->lighting[4 * i + 3] == 1.0f);
        }
        if (unlit > 400u && standard > 400u)
            break;
    }
    printf("unlit: %u unlit px, %u standard px; tiles: standard %u, unlit %u\n", unlit, standard,
           st.tiles[0], st.tiles[1]);
    EXPECT_GT(unlit, 100u);
    EXPECT_GT(standard, 100u);
    EXPECT_GT(st.tiles[DE__GS_MATERIAL_MODEL_UNLIT], 0u);
    EXPECT_GT(st.tiles[DE__GS_MATERIAL_MODEL_STANDARD], 0u);
}

/* resolve_only_listed_tiles: each model's dispatch processed exactly the tiles of its list in
 * classification's arguments of the same frame. */
static void only_listed(rsx *f, de_gpu_backend b) {
    rsx_open(f, b);
    const u32 a = rsx_mesh_plane(f, 3, 2, 1.0f, 1.0f, false);
    const u32 m0 = material_add((de_vec4){1, 1, 1, 1});
    const u32 m1 = material_add((de_vec4){1, 0, 0, 1});
    g_mat[m1 - 1u].standard.unlit = true;
    for (u32 s = 0; s < 6u; ++s) {
        const de_affine x =
            trs((de_vec3){-2.4f + 1.6f * (f32)(s % 3u), -0.9f + 1.8f * (f32)(s / 3u), 0.0f},
                (de_vec3){0, 1, 0}, 0.3f * (f32)s, 0.5f);
        world_set(&f->w, s, a, x, x, s & 1u ? m1 : m0);
    }
    de__vis_resolve_stats st = {0};
    bool compared = false;
    for (u32 i = 0; i < 6u && !compared; ++i) {
        ASSERT_TRUE(rsx_frame(f));
        st = de__vis_view_resolve_stats(f->vis, 0);
        if (!st.valid || f->list_frame[st.frame % 8u] != st.frame)
            continue;
        for (u32 m = 0; m < DE__GS_MATERIAL_MODELS; ++m)
            EXPECT_EQ(st.tiles[m], f->list_counts[st.frame % 8u][m]);
        printf("tiles of frame %llu: standard %u (list %u), unlit %u (list %u)\n",
               (unsigned long long)st.frame, st.tiles[0], f->list_counts[st.frame % 8u][0],
               st.tiles[1], f->list_counts[st.frame % 8u][1]);
        compared = true;
    }
    EXPECT_TRUE(compared);
    EXPECT_GT(st.tiles[0] + st.tiles[1], 0u);
}

/* ---------- Texture LOD against a forward-rendered reference ---------- */

#if defined(DE_RS_TEST_COOK) && DE_RS_TEST_COOK
static const char *target_name(de__shader_target t) {
    switch (t) {
    case DE__SHADER_TARGET_SPIRV:
        return "spirv";
    case DE__SHADER_TARGET_DXIL:
        return "dxil";
    case DE__SHADER_TARGET_METALLIB:
        return "metallib";
    default:
        return "none";
    }
}

static bool find_entry(const de__shader_view *v, u32 target, const char *name,
                       de_gpu_shader_blob *out, de__shader_record *rec_out) {
    for (u32 e = 0; e < v->header.entry_count; ++e) {
        const char *n = NULL;
        (void)de__shader_view_entry(v, e, &n);
        if (strcmp(n, name) != 0)
            continue;
        const i32 ri = de__shader_view_find(v, target, 0, e);
        if (ri < 0)
            return false;
        const de__shader_record rec = de__shader_view_record(v, (u32)ri);
        out->code = v->base + rec.code_offset;
        out->size = rec.code_size;
        out->entry = n;
        *rec_out = rec;
        return true;
    }
    return false;
}

/* The reference pipeline (data/resolve_lod_ref.slang cooked for `backend`); invalid with `why`. */
static de_gpu_pipeline ref_pipeline(rsx *f, char *why, size_t cap) {
    const de__shader_target target = de__shader_target_for_backend(f->backend);
    char msg[200];
    if (!de__slang_available(msg, sizeof msg)) {
        snprintf(why, cap, "no Slang compiler: %.150s", msg);
        return (de_gpu_pipeline){0};
    }
    static bool mounted;
    if (!mounted && de_fs_mount("rsdata", DE_RS_TEST_DATA_DIR, 0, 0) == DE_OK)
        mounted = true;
    de_cooker *c = NULL;
    de_cooker_desc cd = {.verify_determinism = DE_COOK_VERIFY_OFF};
    static const char *const exts[] = {".slang"};
    de_asset_type_desc t;
    memset(&t, 0, sizeof t);
    t.name = "shader";
    t.extensions = exts;
    t.extension_count = 1;
    t.importer_version = de_shader_importer_version();
    t.import = de_shader_import;
    const char *targets[1] = {target_name(target)};
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = "targets";
    kv.value.type = DE__MANIFEST_STRING_ARRAY;
    kv.value.u.arr.items = targets;
    kv.value.u.arr.count = 1;
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = "resolve_lod_ref";
    a.type = "shader";
    a.source = "resolve_lod_ref.slang";
    a.file = "rsdata:/assets.toml";
    a.settings = &kv;
    a.setting_count = 1;
    de_cook_result res;
    memset(&res, 0, sizeof res);
    de_result r = mounted ? DE_OK : DE_ERR_NOT_FOUND;
    if (r == DE_OK &&
        (de_cooker_create(&cd, &c) != DE_OK || de_cooker_register_type(c, &t) != DE_OK))
        r = DE_ERR_INTERNAL;
    if (r == DE_OK) {
        de_cook_input in = {.asset = &a, .asset_root = "rsdata:/", .source = NULL};
        r = de_cooker_cook(c, &in, &res);
    }
    de__shader_view view;
    de_gpu_shader_blob vs = {0}, ps = {0};
    de__shader_record rv, rpx;
    de_gpu_pipeline p = {0};
    if (r == DE_OK && de__shader_view_init(res.data, res.size, &view) == DE_OK &&
        find_entry(&view, (u32)target, "vs_main", &vs, &rv) &&
        find_entry(&view, (u32)target, "ps_main", &ps, &rpx)) {
        p = de_gpu_pipeline_create(f->dev,
                                   &(de_gpu_pipeline_desc){.kind = DE_PIPE_GRAPHICS,
                                                           .vs = vs,
                                                           .ps = ps,
                                                           .color_formats = {DE_FMT_RGBA16_FLOAT},
                                                           .color_count = 1,
                                                           .cull = DE_CULL_NONE,
                                                           .push_constant_bytes = 16,
                                                           .name = "resolve lod reference"});
    }
    if (!DE_HANDLE_VALID(p))
        snprintf(why, cap, "the reference shader did not cook or link: %.200s",
                 de_error_last() ? de_error_last()->message : "?");
    de_cook_result_free(&res);
    de_cooker_destroy(c);
    return p;
}

typedef struct ref_pass {
    rsx *f;
    de_gpu_pipeline pipe;
    de_gpu_buffer verts;
    u32 tex, sampler, vertex_count;
    de_rg_resource target;
    u32 pitch;
} ref_pass;

static void ref_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ref_pass *p = (const ref_pass *)user;
    const de_gpu_attachment a = {
        .texture = de_rg_texture(pass, p->target), .clear = true, .store = true};
    de_gpu_cmd_begin_render(cmd, &a, 1, NULL);
    de_gpu_cmd_set_pipeline(cmd, p->pipe);
    const u32 pc[4] = {de_gpu_buffer_bindless(p->f->dev, p->verts), p->tex, p->sampler, 0};
    de_gpu_cmd_push_constants(cmd, pc, sizeof pc);
    de_gpu_cmd_draw(cmd, p->vertex_count, 1, 0, 0);
    de_gpu_cmd_end_render(cmd);
}
static void ref_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ref_pass *p = (const ref_pass *)user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, p->f->rb, 0, p->pitch, de_rg_texture(pass, p->target), 0,
                                      0);
}
#endif

/* resolve_texture_lod_vs_hw: the LOD the resolve's analytic derivatives select (SampleGrad of a
 * mip-coded emissive texture) against the hardware's on the same triangles drawn forward, within
 * 0.25 mip on interior pixels. */
static void texture_lod(rsx *f, de_gpu_backend b) {
#if defined(DE_RS_TEST_COOK) && DE_RS_TEST_COOK
    rsx_open(f, b);
    char why[400];
    const de_gpu_pipeline pipe = ref_pipeline(f, why, sizeof why);
    if (!DE_HANDLE_VALID(pipe))
        SKIP(why);
    /* Mip k holds 28 k in red: trilinear filtering gives LOD = value * 255 / 28. */
    u8 coded[16];
    for (u32 k = 0; k < 16; ++k)
        coded[k] = (u8)(28u * (k < 9u ? k : 8u));
    const u32 tex = rsx_texture(f, 256, coded, 0, 0);
    const u32 plane = rsx_mesh_plane(f, 4, 4, 1.0f, 1.0f, false);
    const u32 mat = material_add((de_vec4){0, 0, 0, 1});
    de_render_material *m = &g_mat[mat - 1u];
    m->standard.emissive = (de_vec3){1, 1, 1};
    m->standard.emissive_strength = 1.0f;
    m->textures[DE_COOKED_TEX_EMISSIVE].id = tex;
    const de_affine x = trs((de_vec3){0, 0.5f, 0}, (de_vec3){1, 0, 0}, -1.1f, 2.2f);
    world_set(&f->w, 0, plane, x, x, mat);
    ASSERT_TRUE(rsx_frame(f));
    ASSERT_TRUE(f->res.valid);
    /* The reference: every triangle of the plane, clip positions from the uploaded rows. */
    const fmesh *fm = &g_mesh[plane - 1u];
    const de__gs_instance *in = &f->res.inst[0];
    const u32 nverts = fm->meshlets * PATCH_TRIS * 3u;
    f32 *vb = (f32 *)zalloc((size_t)nverts * 8u * sizeof(f32));
    for (u32 ml = 0, o = 0; ml < fm->meshlets; ++ml)
        for (u32 t = 0; t < PATCH_TRIS * 3u; ++t, ++o) {
            const u32 v = ml * PATCH_VERT + fm->tris[t];
            f64 w[3] = {0, 0, 0};
            xform(in, fm->pos + 3u * v, w);
            for (u32 r = 0; r < 4; ++r) {
                const f32 *row = f->res.view.view_proj[r].v;
                vb[8u * o + r] = (f32)(row[0] * w[0] + row[1] * w[1] + row[2] * w[2] + row[3]);
            }
            vb[8u * o + 4u] = fm->uv[2u * v];
            vb[8u * o + 5u] = fm->uv[2u * v + 1u];
        }
    const de_gpu_buffer vbuf =
        upload_buf(f->dev, vb, (u64)nverts * 32u, 16u, "lod reference vertices");
    free(vb);
    const de_gpu_sampler smp =
        de_gpu_sampler_create(f->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                             .mag = DE_FILTER_LINEAR,
                                                             .mip = DE_FILTER_LINEAR,
                                                             .max_aniso = 1.0f});
    ASSERT_TRUE(DE_HANDLE_VALID(smp));
    ref_pass *rp = (ref_pass *)zalloc(sizeof *rp);
    rp->f = f;
    rp->pipe = pipe;
    rp->verts = vbuf;
    rp->tex = g_tex_bindless[tex];
    rp->sampler = de_gpu_sampler_bindless(f->dev, smp);
    rp->vertex_count = nverts;
    rp->pitch = (u32)align_up((u64)f->width * 8u, 256u);
    const u64 n = f->frames++;
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    rp->target = de_rg_create_texture(
        f->g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                     .width = f->width,
                                     .height = f->height,
                                     .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC,
                                     .name = "lod reference"});
    de_rg_pass *p = de_rg_add_pass(f->g, "test.lod_ref", DE_QUEUE_GRAPHICS, ref_execute, rp);
    de_rg_write(p, rp->target, DE_ACCESS_RENDER_TARGET);
    p = de_rg_add_pass(f->g, "test.lod_ref_copy", DE_QUEUE_GRAPHICS, ref_copy, rp);
    de_rg_read(p, rp->target, DE_ACCESS_COPY_SRC);
    de_rg_write(p, de_rg_import_buffer(f->g, f->rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
    ASSERT_EQ(de__rg_compile(f->g), DE_OK);
    ASSERT_EQ(de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = n,
                                                          .frame_fence = f->fence,
                                                          .frame_value = n + 1}),
              DE_OK);
    ASSERT_TRUE(rsx_wait(f, n + 1, "LOD reference"));
    const u8 *map = (const u8 *)de_gpu_buffer_map(f->dev, f->rb);
    u32 n_px = 0, bad = 0;
    f64 worst = 0.0, lo = 99.0, hi = 0.0;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 xx = 0; xx < f->width; ++xx) {
            const size_t i = (size_t)y * f->width + xx;
            const px_tri t = pixel_triangle(f, f->res.ids[i]);
            if (!t.ok)
                continue;
            /* Interior pixels: two pixels from the plane's edge (UV inside [0.02, 0.98]). */
            const f32 u = f->res.uv[4 * i], v = f->res.uv[4 * i + 1];
            if (u < 0.02f || u > 0.98f || v < 0.02f || v > 0.98f)
                continue;
            u16 hv;
            memcpy(&hv, map + (u64)y * rp->pitch + 8u * xx, 2);
            const f64 ref = half_to_f32(hv) * 255.0 / 28.0;
            const f64 got = f->res.lighting[4 * i] * 255.0 / 28.0;
            const f64 d = fabs(got - ref);
            worst = d > worst ? d : worst;
            lo = ref < lo ? ref : lo;
            hi = ref > hi ? ref : hi;
            bad += d > 0.25;
            ++n_px;
        }
    printf("texture LOD: %u px, LOD %.2f .. %.2f, worst difference %.3f mip\n", n_px, lo, hi,
           worst);
    EXPECT_GT(n_px, 1000u);
    EXPECT_TRUE(hi - lo > 1.0); /* the plane spans several mips */
    EXPECT_EQ(bad, 0u);
    de_gpu_wait_idle(f->dev);
    de_gpu_buffer_destroy(f->dev, vbuf);
    de_gpu_sampler_destroy(f->dev, smp);
    de_gpu_pipeline_destroy(f->dev, pipe);
    free(rp);
#else
    (void)f;
    (void)b;
    SKIP("this build has no shader importer (DE_BUILD_TOOLS off or no Slang)");
#endif
}

#define GPU_TESTS(sfx, backend)                                                                    \
    TEST_F(resolve, positions_uvs_cpu_ref_##sfx, rsx) {                                            \
        positions_uvs(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(resolve, texture_lod_vs_hw_##sfx, rsx) {                                                \
        texture_lod(f, backend);                                                                   \
    }                                                                                              \
    TEST_F(resolve, motion_vectors_static_zero_##sfx, rsx) {                                       \
        motion_static(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(resolve, motion_vectors_moving_##sfx, rsx) {                                            \
        motion_moving(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(resolve, feedback_mips_##sfx, rsx) {                                                    \
        feedback_mips(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(resolve, unlit_model_dispatch_##sfx, rsx) {                                             \
        unlit_dispatch(f, backend);                                                                \
    }                                                                                              \
    TEST_F(resolve, only_listed_tiles_##sfx, rsx) {                                                \
        only_listed(f, backend);                                                                   \
    }

GPU_TESTS(metal, DE_GPU_METAL)
GPU_TESTS(vulkan, DE_GPU_VULKAN)
GPU_TESTS(d3d12, DE_GPU_D3D12)

/* ================================================================================================
 * CPU tests
 * ============================================================================================== */

typedef struct lcg {
    u64 s;
} lcg;
static f32 lcg_f(lcg *g) {
    g->s = g->s * 6364136223846793005ull + 1442695040888963407ull;
    return (f32)((g->s >> 40) & 0xFFFFFFu) / 16777216.0f;
}

/* gbuffer_encode_decode_roundtrip: GB1 normals within 0.1 degrees (11 + 11 bits, precise
 * encoding) over random directions and the octahedron's edges; roughness within half a step;
 * sRGB within an 8-bit step. */
TEST(resolve, gbuffer_encode_decode_roundtrip) {
    lcg g = {12345};
    f64 worst = 0.0;
    for (u32 i = 0; i < 400000u; ++i) {
        f32 n[3];
        f32 l2;
        do {
            n[0] = lcg_f(&g) * 2.0f - 1.0f;
            n[1] = lcg_f(&g) * 2.0f - 1.0f;
            n[2] = lcg_f(&g) * 2.0f - 1.0f;
            if (i % 7u == 0u)
                n[i % 3u] = 0.0f; /* on an edge of the octahedron */
            l2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        } while (l2 < 1e-4f || l2 > 1.0f);
        const f32 l = sqrtf(l2);
        for (u32 k = 0; k < 3; ++k)
            n[k] /= l;
        const f32 rough = lcg_f(&g);
        const u32 w = de__gb1_pack(n, rough);
        f32 d[3], r;
        de__gb1_unpack(w, d, &r);
        f64 c = (f64)n[0] * d[0] + (f64)n[1] * d[1] + (f64)n[2] * d[2];
        c = c > 1.0 ? 1.0 : c;
        const f64 ang = acos(c) * 180.0 / 3.14159265358979;
        worst = ang > worst ? ang : worst;
        EXPECT_TRUE(fabsf(r - rough) <= 0.5f / 1023.0f + 1e-6f);
    }
    printf("GB1 normal: worst %.4f degrees (11 + 11 bits); roughness step 1/1023\n", worst);
    EXPECT_TRUE(worst < 0.1);
    for (u32 i = 0; i <= 255u; ++i) {
        const f32 c = (f32)i / 255.0f;
        const f32 e = de__gb_srgb_encode(de__gb_srgb_decode(c));
        EXPECT_TRUE(fabsf(e - c) <= 1e-5f);
    }
    EXPECT_EQ(de__gb_format(DE__GB0), DE_FMT_RGBA8_UNORM);
    EXPECT_EQ(de__gb_format(DE__GB1), DE_FMT_RGB10A2_UNORM);
    EXPECT_EQ(de__gb_format(DE__GB_MOTION), DE_FMT_RG16_FLOAT);
    EXPECT_EQ(de__gb_format(DE__GB_LIGHTING), DE_FMT_RGBA16_FLOAT);
}

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

/* The `public static const uint` values of resolve_standard.slang, gbuffer.slang and
 * feedback.slang against the C table, and ResolveParams's size. */
TEST(resolve, layout_match) {
    static const char *const files[3] = {"visbuffer/resolve_standard.slang",
                                         "visbuffer/gbuffer.slang", "visbuffer/feedback.slang"};
    u32 count = 0, found = 0;
    const de__vis_layout_const *k = de__vis_resolve_layout_consts(&count);
    for (u32 fi = 0; fi < 3; ++fi) {
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
                printf("  %s is not in de__vis_resolve_layout_consts\n", name);
            EXPECT_TRUE(ok);
            ++found;
        }
        if (fi == 0) {
            const char *ps = strstr(text, "struct ResolveParams");
            const char *pe = ps != NULL ? strstr(ps, "};") : NULL;
            u32 fields = 0;
            for (const char *ln = ps; ln != NULL && pe != NULL && ln < pe;
                 ln = strchr(ln + 1, '\n'))
                fields += strncmp(ln, "\n    uint ", 10) == 0;
            EXPECT_EQ(fields * 4u, 112u);
        }
        free(text);
    }
    EXPECT_EQ(found, count);
}
