/*
 * test_forward_transparent.c: the forward transparent pass (DENG-305). Executable
 * test_render_forward_transparent; CTest names render_forward_transparent.<suite>.<test>.
 *
 * CPU tests: the back-to-front sort (order, ties, stability over 100 frames of shuffled input),
 * the C mirror of the routing predicate, and the shader's constants and push-constant layout.
 *
 * GPU tests (per backend: _metal, _vulkan, _d3d12; SKIP without the device or the engine shaders;
 * CI: lavapipe and WARP): a fake render world drives a GPU scene of planes (meshlet patches facing
 * +z; the camera 10 m above looking down), the whole vis frame, lighting (de__lighting_build) and
 * the forward pass (de__forward_build); a test pass reads back the HDR lighting target and the
 * resolve's debug position target.
 *
 *   forward_routing_excludes_visbuffer  a blended quad over the floor: the vis buffer (the
 *                                       resolve's positions) holds the floor under it, meshlet
 *                                       culling counted its meshlets as forward
 *   forward_zero_work_when_none         no forward material, or one out of view: no pass
 *   forward_sort_back_to_front          three overlapping Unlit quads (alpha 0.5) listed out of
 *                                       depth order: the target matches a CPU reference blend
 *                                       back to front (the golden), at every covered pixel
 *   forward_lighting_matches_deferred   a blended quad of alpha 1 against the same quad opaque
 *                                       (deferred), point-lit: within 1 %
 *   deferred_layers_*_match_forward     (DENG-429) the same with a clearcoat, sheen, iridescence
 *                                       or anisotropy layer: the deferred pass restores the layer
 *                                       from the pixel's material, so it matches the forward pass
 *                                       within 1 % (and differs from the plain base)
 *   forward_double_sided_two_draws      a double-sided quad seen from behind: back then front
 *                                       draw, visible; single-sided: one draw, culled
 *   forward_scene_color_copy            a transmissive quad over an emissive floor: the copy is
 *                                       made, the floor shows through x (1 - F) x Beer-Lambert;
 *                                       none without a transmissive material
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
#include "render/lighting/lighting.h"
#include "render/loaders/render_assets.h"
#include "render/materials/brdf_luts.h"
#include "render/materials/material_instances.h"
#include "render/materials/material_models.h"
#include "render/passes/forward/forward.h"
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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS             (10ull * 1000000000ull)
#define FIRST_FRAME_WAIT_NS (180ull * 1000000000ull) /* lavapipe JIT (DENG-428) */

#define PATCH      5u
#define PATCH_VERT (PATCH * PATCH)
#define PATCH_TRIS 32u
#define MAX_MESHES 4u
#define MAX_MATS   8u
#define MAX_LIGHTS 8u
#define STREAM_AL  256u

typedef struct fmesh {
    de_render_mesh rm;
    u32 meshlets, verts;
    de_gpu_buffer buf[4];
} fmesh;

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

static fmesh g_mesh[MAX_MESHES];
static u32 g_mesh_count;
static de_render_material g_mat[MAX_MATS];
static u32 g_mat_count;

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
    (void)t;
    return 0u;
}

static de_gpu_buffer upload_buf(de_gpu_device *dev, const void *data, u64 bytes) {
    const de_gpu_buffer b =
        de_gpu_buffer_create(dev, &(de_gpu_buffer_desc){.size = bytes,
                                                        .usage = DE_USAGE_STORAGE,
                                                        .memory = DE_MEM_UPLOAD,
                                                        .name = "test mesh section"});
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    void *m = de_gpu_buffer_map(dev, b);
    ASSERT_TRUE(m != NULL);
    memcpy(m, data, (size_t)bytes);
    return b;
}

/* A plane of `cols` x `rows` patches in z = 0 (normal +z), `size` metres per patch edge, centred
 * on the origin. */
static void mesh_plane(fmesh *fm, de_gpu_device *dev, u32 cols, u32 rows, f32 size) {
    memset(fm, 0, sizeof *fm);
    fm->meshlets = cols * rows;
    fm->verts = fm->meshlets * PATCH_VERT;
    u8 tris[PATCH_TRIS * 3];
    u32 nt = 0;
    for (u32 j = 0; j + 1u < PATCH; ++j)
        for (u32 i = 0; i + 1u < PATCH; ++i) {
            const u8 v00 = (u8)(j * PATCH + i), v10 = (u8)(v00 + 1u);
            const u8 v11 = (u8)(v10 + PATCH), v01 = (u8)(v00 + PATCH);
            const u8 t[6] = {v00, v10, v11, v00, v11, v01};
            memcpy(tris + 3u * nt, t, 6);
            nt += 2u;
        }
    const f32 w = (f32)cols * size, h = (f32)rows * size;
    f32 *pos = (f32 *)zalloc((size_t)fm->verts * 12u);
    for (u32 ml = 0; ml < fm->meshlets; ++ml) {
        const u32 cx = ml % cols, cy = ml / cols;
        for (u32 j = 0; j < PATCH; ++j)
            for (u32 i = 0; i < PATCH; ++i) {
                const u32 v = ml * PATCH_VERT + j * PATCH + i;
                pos[3u * v] = -0.5f * w + ((f32)cx + (f32)i / (f32)(PATCH - 1u)) * size;
                pos[3u * v + 1u] = -0.5f * h + ((f32)cy + (f32)j / (f32)(PATCH - 1u)) * size;
                pos[3u * v + 2u] = 0.0f;
            }
    }
    const u32 off_n = (fm->verts * 12u + STREAM_AL - 1u) & ~(STREAM_AL - 1u);
    const u32 off_t = off_n + ((fm->verts * 4u + STREAM_AL - 1u) & ~(STREAM_AL - 1u));
    const u32 off_uv = off_t + ((fm->verts * 4u + STREAM_AL - 1u) & ~(STREAM_AL - 1u));
    const u32 vb = off_uv + fm->verts * 8u;
    u8 *data = (u8 *)zalloc(vb);
    for (u32 v = 0; v < fm->verts; ++v) {
        memcpy(data + 12u * v, pos + 3u * v, 12);
        put_u32(data + off_n + 4u * v, 0u);     /* oct (0, 0) = +z */
        put_u32(data + off_t + 4u * v, 32767u); /* +x, sign +1 */
        put_f32(data + off_uv + 8u * v, pos[3u * v] / w + 0.5f);
        put_f32(data + off_uv + 8u * v + 4u, 0.5f - pos[3u * v + 1u] / h);
    }
    u8 *records = (u8 *)zalloc((size_t)fm->meshlets * DE_COOKED_MESHLET_SIZE);
    u32 *mverts = (u32 *)zalloc((size_t)fm->verts * 4u);
    u8 *mtris = (u8 *)zalloc((size_t)fm->meshlets * PATCH_TRIS * 3u);
    for (u32 ml = 0; ml < fm->meshlets; ++ml) {
        u8 *r = records + (size_t)ml * DE_COOKED_MESHLET_SIZE;
        put_u32(r + 0, ml * PATCH_VERT);
        put_u32(r + 4, ml * PATCH_TRIS * 3u);
        put_u32(r + 8, PATCH_VERT | (PATCH_TRIS << 8));
        const f32 *c0 = pos + 3u * (ml * PATCH_VERT), *c1 = pos + 3u * (ml * PATCH_VERT + 24u);
        put_f32(r + 16, 0.5f * (c0[0] + c1[0]));
        put_f32(r + 20, 0.5f * (c0[1] + c1[1]));
        put_f32(r + 24, 0.0f);
        put_f32(r + 28, 0.75f * size);
        put_f32(r + 44, 1.0f); /* no cone */
        for (u32 i = 0; i < PATCH_VERT; ++i)
            mverts[ml * PATCH_VERT + i] = ml * PATCH_VERT + i;
        memcpy(mtris + (size_t)ml * PATCH_TRIS * 3u, tris, sizeof tris);
    }
    de_render_mesh *m = &fm->rm;
    for (u32 s = 0; s < DE_COOKED_STREAM_COUNT; ++s)
        m->stream_offset[s] = UINT32_MAX;
    m->stream_offset[DE_COOKED_STREAM_POSITION] = 0;
    m->stream_format[DE_COOKED_STREAM_POSITION] = DE_COOKED_VF_F32X3;
    m->stream_offset[DE_COOKED_STREAM_NORMAL] = off_n;
    m->stream_format[DE_COOKED_STREAM_NORMAL] = DE_COOKED_VF_OCT16;
    m->stream_offset[DE_COOKED_STREAM_TANGENT] = off_t;
    m->stream_format[DE_COOKED_STREAM_TANGENT] = DE_COOKED_VF_OCT16_SIGN;
    m->stream_offset[DE_COOKED_STREAM_UV0] = off_uv;
    m->stream_format[DE_COOKED_STREAM_UV0] = DE_COOKED_VF_F32X2;
    m->uv_scale[0][0] = m->uv_scale[0][1] = 1.0f;
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
    m->bounds_min[2] = -0.01f;
    m->bounds_max[0] = 0.5f * w;
    m->bounds_max[1] = 0.5f * h;
    m->bounds_max[2] = 0.01f;
    m->sphere[3] = 0.5f * sqrtf(w * w + h * h) + 0.01f;
    for (u32 c = 0; c < 3; ++c)
        m->position_scale[c] = 1.0f;
    const struct {
        const void *p;
        u64 n;
        u32 which;
    } sec[4] = {{data, vb, DE_RENDER_MESH_VERTICES},
                {records, (u64)fm->meshlets * DE_COOKED_MESHLET_SIZE, DE_RENDER_MESH_MESHLETS},
                {mverts, (u64)fm->verts * 4u, DE_RENDER_MESH_MESHLET_VERTICES},
                {mtris, (u64)fm->meshlets * PATCH_TRIS * 3u, DE_RENDER_MESH_MESHLET_TRIANGLES}};
    for (u32 i = 0; i < 4; ++i) {
        fm->buf[i] = upload_buf(dev, sec[i].p, sec[i].n);
        m->buffers[sec[i].which] = fm->buf[i];
        m->bindless[sec[i].which] = de_gpu_buffer_bindless(dev, fm->buf[i]);
        m->buffer_bytes[sec[i].which] = sec[i].n;
    }
    free(pos);
    free(data);
    free(records);
    free(mverts);
    free(mtris);
}

static void mesh_free(fmesh *fm, de_gpu_device *dev) {
    for (u32 i = 0; i < 4; ++i)
        if (dev != NULL && DE_HANDLE_VALID(fm->buf[i]))
            de_gpu_buffer_destroy(dev, fm->buf[i]);
    memset(fm, 0, sizeof *fm);
}

/* ---------- Materials (1-based ids; the GPU scene re-packs them every update) ---------- */

static de_render_material *mat_new(void) {
    ASSERT_TRUE(g_mat_count < MAX_MATS);
    de_render_material *m = &g_mat[g_mat_count++];
    memset(m, 0, sizeof *m);
    m->standard.normal_scale = 1.0f;
    m->standard.emissive_strength = 1.0f;
    m->standard.specular = 1.0f;
    m->standard.specular_color = (de_vec3){1.0f, 1.0f, 1.0f};
    m->standard.ior = 1.5f;
    m->standard.roughness = 0.5f;
    return m;
}

/* A Standard grey `base` of `alpha` and alpha mode `mode` (0 opaque, 2 blend). */
static u32 mat_standard(f32 base, f32 alpha, u32 mode, bool double_sided) {
    de_render_material *m = mat_new();
    m->standard.base_color = (de_vec4){base, base, base, alpha};
    m->standard.double_sided = double_sided;
    m->alpha_mode = mode;
    return g_mat_count;
}

/* An Unlit colour (its emission) of `alpha` and alpha mode `mode`. */
static u32 mat_unlit(f32 r, f32 g, f32 b, f32 alpha, u32 mode) {
    de_render_material *m = mat_new();
    m->standard.base_color = (de_vec4){r, g, b, alpha};
    m->standard.unlit = true;
    m->alpha_mode = mode;
    return g_mat_count;
}

static de_render_material *mat_of(u32 id) {
    return &g_mat[id - 1u];
}

typedef struct world {
    de__render_world_frame f;
    u32 cap;
    bool touch_all, touch_lights;
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
    de__rw_lights *li = &w->f.lights;
    li->entity = (u64 *)zalloc(MAX_LIGHTS * sizeof(u64));
    li->transform = (de_affine *)zalloc(MAX_LIGHTS * sizeof(de_affine));
    li->light = (DeLight *)zalloc(MAX_LIGHTS * sizeof(DeLight));
    w->f.dirty_instances = (u32 *)zalloc(cap * sizeof(u32));
    w->f.removed_instances = (u32 *)zalloc(sizeof(u32));
    w->f.dirty_lights = (u32 *)zalloc(MAX_LIGHTS * sizeof(u32));
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
    free(w->f.lights.entity);
    free(w->f.lights.transform);
    free(w->f.lights.light);
    free(w->f.dirty_instances);
    free(w->f.removed_instances);
    free(w->f.dirty_lights);
    free(w->f.removed_lights);
    memset(w, 0, sizeof *w);
}

static de_affine trs(de_vec3 t, de_vec3 axis, f32 angle, f32 s) {
    return de_affine_from_trs(t, de_quat_axis_angle(axis, angle), (de_vec3){s, s, s});
}
static de_affine at(f32 x, f32 y, f32 z) {
    return trs((de_vec3){x, y, z}, (de_vec3){0, 0, 1}, 0.0f, 1.0f);
}

static void world_set(world *w, u32 s, u32 mesh, de_affine xf, u32 mat) {
    de__rw_instances *in = &w->f.instances;
    in->entity[s] = 1000u + s;
    in->mesh[s].id = mesh;
    in->transform[s] = xf;
    in->prev_transform[s] = xf;
    in->layer_mask[s] = 1u;
    const de_render_mesh *m = &g_mesh[mesh - 1u].rm;
    const de_aabb b = {{m->bounds_min[0], m->bounds_min[1], m->bounds_min[2]},
                       {m->bounds_max[0], m->bounds_max[1], m->bounds_max[2]}};
    in->bounds[s] = de_aabb_transform(b, &xf);
    in->flags[s] = (u8)DE__RW_INST_HAS_BOUNDS;
    for (u32 k = 0; k < DE__RW_MATERIALS; ++k)
        in->materials[(size_t)s * DE__RW_MATERIALS + k].id = mat;
    if (s >= in->count)
        in->count = s + 1u;
    w->touch_all = true;
}

static void world_light(world *w, u32 s, const DeLight *l, de_affine xf) {
    ASSERT_TRUE(s < MAX_LIGHTS);
    de__rw_lights *li = &w->f.lights;
    li->entity[s] = 5000u + s;
    li->transform[s] = xf;
    li->light[s] = *l;
    if (s >= li->count)
        li->count = s + 1u;
    w->touch_lights = true;
}

static void world_begin(world *w) {
    w->f.frame++;
    w->f.origin_changed = w->f.frame == 1;
    w->f.dirty_instance_count = 0;
    w->f.dirty_light_count = 0;
    if (w->touch_all && w->f.frame > 1)
        for (u32 s = 0; s < w->f.instances.count; ++s)
            w->f.dirty_instances[w->f.dirty_instance_count++] = s;
    if (w->touch_lights && w->f.frame > 1)
        for (u32 s = 0; s < w->f.lights.count; ++s)
            w->f.dirty_lights[w->f.dirty_light_count++] = s;
    w->touch_all = false;
    w->touch_lights = false;
    w->f.instances.live = 0;
    for (u32 s = 0; s < w->f.instances.count; ++s)
        w->f.instances.live += w->f.instances.entity[s] != 0;
    w->f.lights.live = 0;
    for (u32 s = 0; s < w->f.lights.count; ++s)
        w->f.lights.live += w->f.lights.entity[s] != 0;
}

/* ================================================================================================
 * Fixture
 * ============================================================================================== */

#define MAX_DRAWS 16u

typedef struct fx_result {
    bool valid;
    u32 width, height;
    f32 *pos;           /* xyzw per pixel (resolve debug: the vis buffer's surface) */
    f32 *lighting;      /* rgba per pixel */
    bool forward;       /* de__forward_build added its passes */
    u32 forward_passes; /* passes named "forward.*" in the graph */
    u32 count, draw_count;
    u32 order[MAX_DRAWS]; /* the listed instances, in draw order */
    de__forward_draw draws[MAX_DRAWS];
    de__transparent_counts counts;
    bool scene_color;
    f32 pre_exposure;
    u64 frame;
} fx_result;

typedef struct lx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_gpu_device *dev;
    de_gpu_backend backend;
    bool real_gpu, warm;
    de_rg *g;
    de__gpu_scene *gs;
    de__vis_shared *vis;
    de__material_models *models;
    de__lighting *lt;
    de__brdf_luts *luts;
    de__forward *fw;
    de_gpu_fence fence;
    u64 frames;
    de_gpu_buffer rb;
    u64 rb_size;
    world w;
    de_vec3 eye, target;
    f32 fov;
    u32 width, height;
    fx_result res;
} lx;

static void lx_setup(lx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "forward_test"}, &f->arena), DE_OK);
    de_error_clear();
    memset(g_mesh, 0, sizeof g_mesh);
    g_mesh_count = 0;
    memset(g_mat, 0, sizeof g_mat);
    g_mat_count = 0;
    f->eye = (de_vec3){0.0f, 0.0f, 10.0f};
    f->target = (de_vec3){0.0f, 0.0f, 0.0f};
    f->fov = 0.9f;
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

static bool cvar_set(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (!DE_HANDLE_VALID(cv))
        return false;
    EXPECT_EQ(de_cvar_set(cv, value), DE_OK);
    return true;
}

static void free_result(fx_result *r) {
    free(r->pos);
    free(r->lighting);
    memset(r, 0, sizeof *r);
}

static void lx_teardown(lx *f) {
    if (f->g) {
        de__rg_reset(f->g);
        de__rg_destroy(f->g);
    }
    if (f->dev)
        de_gpu_wait_idle(f->dev);
    de__forward_destroy(f->fw);
    de__lighting_destroy(f->lt);
    de__brdf_luts_destroy(f->luts);
    de__vis_shared_destroy(f->vis);
    de__material_models_destroy(f->models);
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
    free_result(&f->res);
    world_free(&f->w);
    if (f->arena)
        de_arena_destroy(f->arena);
    if (f->alloc.alloc)
        EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0);
    /* The cvars are process-wide: back to their defaults. */
    const char *const names[] = {"r.lod_bias", "r.occlusion", "r.rg_record_cost"};
    const char *const values[] = {"0", "1", "32"};
    for (u32 i = 0; i < sizeof names / sizeof names[0]; ++i) {
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

static void lx_open(lx *f, de_gpu_backend backend) {
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
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "fx"},
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
    gd.initial_capacity[DE__GS_LIGHTS] = MAX_LIGHTS;
    ASSERT_EQ(de__gpu_scene_create(&gd, &f->gs), DE_OK);
    ASSERT_EQ(de__material_models_create(f->dev, &f->alloc, &f->models), DE_OK);
    ASSERT_EQ(de__vis_shared_create(&(de__vis_shared_desc){.device = f->dev,
                                                           .allocator = &f->alloc,
                                                           .material_models = f->models},
                                    &f->vis),
              DE_OK);
    if (!f->vis->enabled) {
        snprintf(why, sizeof why, "the vis frame is off: %.600s", f->vis->why);
        SKIP(why);
    }
    ASSERT_EQ(de__brdf_luts_create(f->dev, &f->alloc, &f->luts), DE_OK);
    u64 lut_value = 0;
    const de_gpu_fence lut_fence = de__brdf_luts_fence(f->luts, &lut_value);
    if (DE_HANDLE_VALID(lut_fence))
        ASSERT_EQ(de_gpu_fence_wait(f->dev, lut_fence, lut_value, FIRST_FRAME_WAIT_NS), DE_OK);
    ASSERT_EQ(
        de__lighting_create_ex(&(de__lighting_desc){.device = f->dev,
                                                    .allocator = &f->alloc,
                                                    .vis = f->vis,
                                                    .brdf_lut = de__brdf_luts_bindless(f->luts)},
                               &f->lt),
        DE_OK);
    if (!de__lighting_enabled(f->lt)) {
        snprintf(why, sizeof why, "lighting is off: %.600s", f->lt->why);
        SKIP(why);
    }
    ASSERT_EQ(de__forward_create_ex(f->vis, f->lt, &f->fw), DE_OK);
    if (!de__forward_enabled(f->fw)) {
        snprintf(why, sizeof why, "the forward pass is off (no forward_transparent code)");
        SKIP(why);
    }
    world_init(&f->w, 32);
    (void)cvar_set("r.lod_bias", "-7");
    (void)cvar_set("r.occlusion", "0");
    de__vis_resolve_set_debug(f->vis, &(de__vis_resolve_debug){.targets = true});
    /* One command list per submission on Vulkan (VVL 1.3.275 checks bindless accesses against
     * the layouts at the end of each list; DENG-235, DENG-427). */
    if (backend == DE_GPU_VULKAN && !cvar_set("r.rg_record_cost", "1048576")) {
        char *args[] = {"test", "+r.rg_record_cost", "1048576"};
        EXPECT_EQ(de_cvar_apply_args(3, args), DE_OK);
    }
}

static u32 lx_mesh_plane(lx *f, u32 cols, u32 rows, f32 size) {
    ASSERT_TRUE(g_mesh_count < MAX_MESHES);
    mesh_plane(&g_mesh[g_mesh_count], f->dev, cols, rows, size);
    return ++g_mesh_count;
}

/* The floor: a 12 m square plane at z = 0 in slot 0. */
static void lx_floor(lx *f, u32 mat) {
    const u32 m = lx_mesh_plane(f, 8, 8, 1.5f);
    world_set(&f->w, 0, m, at(0, 0, 0), mat);
}

/* ---------- One frame ---------- */

typedef struct rb_item {
    de_rg_resource r;
    u64 off, bytes;
    u32 pitch;
} rb_item;

typedef struct rb_pass {
    lx *f;
    rb_item it[2]; /* resolve debug position, lighting target */
} rb_pass;

static void rb_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    for (u32 i = 0; i < 2; ++i)
        if (DE_HANDLE_VALID(p->it[i].r))
            de_gpu_cmd_copy_texture_to_buffer(cmd, p->f->rb, p->it[i].off, p->it[i].pitch,
                                              de_rg_texture(pass, p->it[i].r), 0, 0);
}

static void camera(const lx *f, de_mat4 *view, de_mat4 *proj) {
    *view = de_m4_look_at(f->eye, f->target, (de_vec3){0.0f, 1.0f, 0.0f});
    *proj = de_m4_perspective_rz(f->fov, (f32)f->width / (f32)f->height, 0.1f);
}

static u64 align_up(u64 v, u64 a) {
    return (v + a - 1u) / a * a;
}

static void *rows_of(const u8 *m, const rb_item *it, u32 w, u32 h, u32 bpp) {
    u8 *out = (u8 *)zalloc((size_t)w * h * bpp);
    for (u32 y = 0; y < h; ++y)
        memcpy(out + (size_t)y * w * bpp, m + it->off + (u64)y * it->pitch, (size_t)w * bpp);
    return out;
}

static f32 *halves_of(const u8 *m, const rb_item *it, u32 w, u32 h) {
    u16 *raw = (u16 *)rows_of(m, it, w, h, 8);
    f32 *out = (f32 *)zalloc((size_t)w * h * 4u * sizeof(f32));
    for (size_t i = 0; i < (size_t)w * h * 4u; ++i)
        out[i] = half_to_f32(raw[i]);
    free(raw);
    return out;
}

static bool lx_wait(lx *f, u64 v, const char *what) {
    const u64 t0 = de_time_now_ns();
    const de_result r =
        de_gpu_fence_wait(f->dev, f->fence, v, f->warm ? WAIT_NS : FIRST_FRAME_WAIT_NS);
    const f64 ms = (f64)(de_time_now_ns() - t0) / 1e6;
    if (r == DE_OK) {
        if (!f->warm)
            printf("first frame (includes the shader compile at first use): %.0f ms\n", ms);
        return true;
    }
    fprintf(stderr, "%s: the fence wait for value %llu failed after %.0f ms (%s); fence at %llu\n",
            what, (unsigned long long)v, ms, de_result_str(r),
            (unsigned long long)de_gpu_fence_value(f->dev, f->fence));
    return false;
}

/* Builds, runs and waits for one frame (vis frame, lighting, forward); fills f->res. */
static bool lx_frame(lx *f) {
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
    de__lighting_context *lc = NULL;
    if (de__lighting_build(f->lt, f->g, vc, &lc) != DE_OK)
        return false;
    const u32 passes_before = f->g->pass_count;
    de__forward_context *fc = NULL;
    if (de__forward_build(f->fw, f->g, vc, lc, &f->w.f, &fc) != DE_OK)
        return false;
    u32 forward_passes = 0;
    for (u32 i = passes_before; i < f->g->pass_count; ++i)
        forward_passes += strncmp(f->g->passes[i]->name, "forward.", 8) == 0;
    rb_pass *rp = NULL;
    if (lc != NULL) {
        rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
        ASSERT_TRUE(rp != NULL);
        rp->f = f;
        const de_rg_resource tex[2] = {vc->resolve_debug_pos, lc->target};
        static const u32 bpp[2] = {16, 8};
        u64 off = 0;
        for (u32 i = 0; i < 2; ++i) {
            rb_item *it = &rp->it[i];
            it->r = tex[i];
            it->pitch = (u32)align_up((u64)f->width * bpp[i], 256u);
            it->off = off;
            it->bytes = (u64)it->pitch * f->height;
            off = align_up(off + it->bytes, 512u);
        }
        if (off > f->rb_size) {
            if (DE_HANDLE_VALID(f->rb)) {
                de_gpu_wait_idle(f->dev);
                de_gpu_buffer_destroy(f->dev, f->rb);
            }
            f->rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = off,
                                                                       .usage = DE_USAGE_COPY_DST,
                                                                       .memory = DE_MEM_READBACK,
                                                                       .name = "forward readback"});
            ASSERT_TRUE(DE_HANDLE_VALID(f->rb));
            f->rb_size = off;
        }
        de_rg_pass *p = de_rg_add_pass(f->g, "test.readback", DE_QUEUE_GRAPHICS, rb_execute, rp);
        for (u32 i = 0; i < 2; ++i)
            if (DE_HANDLE_VALID(rp->it[i].r))
                de_rg_read(p, rp->it[i].r, DE_ACCESS_COPY_SRC);
        de_rg_write(p, de_rg_import_buffer(f->g, f->rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
        /* Back to the layouts the bindless accesses expect before the list ends (VVL 1.3.275
         * checks them against the end of the list): what lighting sampled (depth, G-buffer)
         * sampled, the storage targets storage. */
        p = de_rg_add_pass(f->g, "test.restore", DE_QUEUE_GRAPHICS, NULL, NULL);
        de_rg_pass_side_effect(p);
        de_rg_read(p, vc->depth, DE_ACCESS_SHADER_READ);
        for (u32 t = DE__GB0; t <= DE__GB2; ++t)
            de_rg_read(p, vc->gbuffer[t], DE_ACCESS_SHADER_READ);
        const de_rg_resource storage[4] = {vc->gbuffer[DE__GB_MOTION], vc->gbuffer[DE__GB_LIGHTING],
                                           vc->resolve_debug_pos, vc->resolve_debug_uv};
        for (u32 i = 0; i < 4; ++i)
            if (DE_HANDLE_VALID(storage[i]))
                de_rg_write(p, storage[i], DE_ACCESS_SHADER_WRITE);
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
    fx_result *res = &f->res;
    free_result(res);
    res->frame = n;
    res->forward_passes = forward_passes;
    if (fc != NULL) {
        res->forward = true;
        res->count = fc->count;
        res->draw_count = fc->draw_count;
        for (u32 i = 0; i < fc->count && i < MAX_DRAWS; ++i)
            res->order[i] = fc->items[i].instance;
        for (u32 i = 0; i < fc->draw_count && i < MAX_DRAWS; ++i)
            res->draws[i] = fc->draws[i];
        res->counts = fc->counts;
        res->scene_color = DE_HANDLE_VALID(fc->scene_color);
    }
    de__gpu_scene_frame_end(f->gs, f->g, true);
    if (!lx_wait(f, n + 1, "frame"))
        return false;
    f->warm = true;
    if (lc == NULL)
        return true;
    res->pre_exposure = lc->view.pre_exposure;
    const u8 *m = (const u8 *)de_gpu_buffer_map(f->dev, f->rb);
    const u32 w = f->width, h = f->height;
    res->width = w;
    res->height = h;
    res->pos = (f32 *)rows_of(m, &rp->it[0], w, h, 16);
    res->lighting = halves_of(m, &rp->it[1], w, h);
    res->valid = true;
    return true;
}

/* Two frames (the second with every table uploaded and the kernels warm); the last's results. */
static void lx_run(lx *f) {
    ASSERT_TRUE(lx_frame(f));
    ASSERT_TRUE(lx_frame(f));
    ASSERT_TRUE(f->res.valid);
}

/* Where pixel (x, y)'s ray meets the plane z = h (the camera looks straight down from f->eye). */
static void pixel_on_plane(const lx *f, u32 x, u32 y, f32 h, f32 out[2]) {
    const f32 t = tanf(0.5f * f->fov), aspect = (f32)f->width / (f32)f->height;
    const f32 nx = ((f32)x + 0.5f) / (f32)f->width * 2.0f - 1.0f;
    const f32 ny = 1.0f - ((f32)y + 0.5f) / (f32)f->height * 2.0f;
    const f32 d = f->eye.z - h;
    out[0] = f->eye.x + nx * t * aspect * d;
    out[1] = f->eye.y + ny * t * d;
}

/* A square quad (centre cx, cy at height h, half-size r): 1 when pixel (x, y) is inside by
 * `margin`, 0 outside by `margin`, -1 near its edge. */
static i32 covers(const lx *f, u32 x, u32 y, f32 cx, f32 cy, f32 h, f32 r, f32 margin) {
    f32 p[2];
    pixel_on_plane(f, x, y, h, p);
    const f32 dx = fabsf(p[0] - cx), dy = fabsf(p[1] - cy);
    if (dx < r - margin && dy < r - margin)
        return 1;
    if (dx > r + margin || dy > r + margin)
        return 0;
    return -1;
}

static const f32 *px_of(const f32 *img, const lx *f, u32 x, u32 y) {
    return img + ((size_t)y * f->width + x) * 4u;
}

static bool close_to(f32 got, f32 want, f32 rel, f32 floor_abs) {
    return fabsf(got - want) <= rel * fabsf(want) + floor_abs;
}

static void point_light(lx *f, u32 slot, f32 x, f32 y, f32 z, f32 lumens) {
    const DeLight l = {.kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = lumens};
    world_light(&f->w, slot, &l, at(x, y, z));
}

/* ================================================================================================
 * GPU tests
 * ============================================================================================== */

/* A blended quad over the floor: the vis buffer under it holds the floor, its meshlets are
 * counted as forward, and the pass draws it. Made opaque, the vis buffer holds it (the check
 * would see a leak). */
static void routing(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, mat_unlit(0.2f, 0.2f, 0.2f, 1.0f, 0));
    const u32 quad = mat_standard(0.6f, 0.5f, DE_COOKED_ALPHA_BLEND, false);
    world_set(&f->w, 1, lx_mesh_plane(f, 1, 1, 2.0f), at(0, 0, 3), quad);
    point_light(f, 0, 0, 0, 6, 2000.0f);
    lx_run(f);
    ASSERT_TRUE(lx_frame(f)); /* the meshlet stats of an earlier frame are read back by now */
    const fx_result *r = &f->res;
    EXPECT_TRUE(r->forward);
    EXPECT_EQ(r->count, 1u);
    EXPECT_EQ(r->counts.forward_materials, 1u);
    u32 inside = 0, floor_under = 0;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x) {
            if (covers(f, x, y, 0, 0, 3, 1.0f, 0.05f) != 1)
                continue;
            inside++;
            const f32 *p = px_of(r->pos, f, x, y);
            floor_under += p[3] > 0.5f && fabsf(p[2]) < 1e-3f;
        }
    printf("quad pixels %u, floor in the vis buffer under %u\n", inside, floor_under);
    EXPECT_TRUE(inside > 500u);
    EXPECT_EQ(floor_under, inside);
    const de__vis_meshlet_stats ms = de__vis_view_meshlet_stats(f->vis, 0);
    printf("meshlet stats (frame %llu): visible %u forward %u\n", (unsigned long long)ms.frame,
           ms.visible, ms.forward);
    EXPECT_TRUE(ms.valid);
    EXPECT_TRUE(ms.forward >= 1u);
    /* Control: the same quad opaque is in the vis buffer, and no forward pass runs. */
    mat_of(quad)->alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    lx_run(f);
    EXPECT_FALSE(f->res.forward);
    u32 quad_in_vis = 0;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x)
            if (covers(f, x, y, 0, 0, 3, 1.0f, 0.05f) == 1)
                quad_in_vis += fabsf(px_of(f->res.pos, f, x, y)[2] - 3.0f) < 1e-3f;
    EXPECT_EQ(quad_in_vis, inside);
}

/* No forward material: no pass, nothing gathered past the materials. A forward material out of
 * view: still no pass. The pipeline count never changes (pre-warmed at create). */
static void zero_work(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    EXPECT_EQ(de__forward_pipeline_count(f->fw), 1u);
    lx_floor(f, mat_unlit(0.2f, 0.2f, 0.2f, 1.0f, 0));
    const u32 quad = mat_standard(0.6f, 1.0f, DE_COOKED_ALPHA_OPAQUE, false);
    world_set(&f->w, 1, lx_mesh_plane(f, 1, 1, 2.0f), at(0, 0, 3), quad);
    lx_run(f);
    EXPECT_FALSE(f->res.forward);
    EXPECT_EQ(f->res.forward_passes, 0u);
    /* Blended but out of view. */
    mat_of(quad)->alpha_mode = DE_COOKED_ALPHA_BLEND;
    world_set(&f->w, 1, g_mesh_count, at(100, 0, 3), quad);
    lx_run(f);
    EXPECT_FALSE(f->res.forward);
    EXPECT_EQ(f->res.forward_passes, 0u);
    /* In view: the pass runs (two passes at most: no transmission, so only the draw). */
    world_set(&f->w, 1, g_mesh_count, at(0, 0, 3), quad);
    lx_run(f);
    EXPECT_TRUE(f->res.forward);
    EXPECT_EQ(f->res.forward_passes, 1u);
    EXPECT_FALSE(f->res.scene_color);
    EXPECT_EQ(de__forward_pipeline_count(f->fw), 1u);
}

/* Three overlapping Unlit quads of alpha 0.5, listed out of depth order (slot 1 the nearest, slot
 * 2 the farthest): every covered pixel matches a CPU blend of the covering quads back to front
 * over the floor; a front-to-back blend would differ. */
static void back_to_front(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    const f32 bg[3] = {0.1f, 0.1f, 0.1f};
    lx_floor(f, mat_unlit(bg[0], bg[1], bg[2], 1.0f, 0));
    const u32 mesh = lx_mesh_plane(f, 1, 1, 2.0f);
    static const f32 col[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    static const f32 cx[3] = {-0.5f, 0.5f, 0.0f}, cz[3] = {4.0f, 2.0f, 3.0f};
    for (u32 i = 0; i < 3; ++i)
        world_set(&f->w, 1u + i, mesh, at(cx[i], 0, cz[i]),
                  mat_unlit(col[i][0], col[i][1], col[i][2], 0.5f, DE_COOKED_ALPHA_BLEND));
    lx_run(f);
    const fx_result *r = &f->res;
    ASSERT_TRUE(r->forward);
    ASSERT_EQ(r->count, 3u);
    EXPECT_EQ(r->order[0], 2u); /* z = 2: the farthest */
    EXPECT_EQ(r->order[1], 3u);
    EXPECT_EQ(r->order[2], 1u);
    const f32 e = r->pre_exposure;
    static const u32 far_to_near[3] = {1, 2, 0}; /* quad indices by depth */
    u32 checked = 0, triple = 0, bad = 0;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x) {
            const i32 fl = covers(f, x, y, 0, 0, 0, 6.0f, 0.03f); /* the floor (12 m) */
            const f32 under = fl == 1 ? e : 0.0f;
            f32 want[3] = {bg[0] * under, bg[1] * under, bg[2] * under};
            bool edge = fl < 0;
            u32 layers = 0;
            for (u32 k = 0; k < 3; ++k) {
                const u32 q = far_to_near[k];
                const i32 c = covers(f, x, y, cx[q], 0, cz[q], 1.0f, 0.03f);
                edge |= c < 0;
                if (c != 1)
                    continue;
                layers++;
                for (u32 ch = 0; ch < 3; ++ch)
                    want[ch] = col[q][ch] * e * 0.5f + want[ch] * 0.5f;
            }
            if (edge)
                continue;
            checked++;
            triple += layers == 3u;
            const f32 *got = px_of(r->lighting, f, x, y);
            bool ok = true;
            for (u32 ch = 0; ch < 3; ++ch)
                ok &= close_to(got[ch], want[ch], 0.01f, 2e-3f * e);
            if (!ok && bad++ < 4)
                printf("pixel %u,%u: got %.4f %.4f %.4f want %.4f %.4f %.4f (%u layers)\n", x, y,
                       got[0], got[1], got[2], want[0], want[1], want[2], layers);
        }
    printf("checked %u pixels (%u under all three quads), %u differ\n", checked, triple, bad);
    EXPECT_TRUE(checked > 10000u);
    EXPECT_TRUE(triple > 200u);
    EXPECT_EQ(bad, 0u);
}

/* An opaque quad rejects a farther transparent quad while a nearer one still blends.
 * Exercise both forward material routes; early depth must retain the same HDR result. */
static void opaque_occlusion(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, mat_unlit(0.1f, 0.1f, 0.1f, 1.0f, 0));
    const u32 mesh = lx_mesh_plane(f, 1, 1, 2.0f);
    world_set(&f->w, 1, mesh, at(0, 0, 3), mat_unlit(0.3f, 0.3f, 0.3f, 1.0f, 0));
    world_set(&f->w, 3, mesh, at(0, 0, 4),
              mat_unlit(0.0f, 1.0f, 0.0f, 0.5f, DE_COOKED_ALPHA_BLEND));
    const u32 hidden[] = {mat_unlit(8.0f, 0.0f, 0.0f, 0.75f, DE_COOKED_ALPHA_BLEND),
                          mat_standard(0.8f, 0.75f, DE_COOKED_ALPHA_BLEND, false)};
    for (u32 model = 0; model < 2; ++model) {
        world_set(&f->w, 2, mesh, at(0, 0, 2), hidden[model]);
        lx_run(f);
        const fx_result *r = &f->res;
        ASSERT_TRUE(r->forward);
        ASSERT_EQ(r->count, 2u); /* Both forward draws exist; depth rejects only hidden pixels. */
        u32 checked = 0, occluded = 0, foreground = 0, bad = 0;
        for (u32 y = 0; y < f->height; ++y)
            for (u32 x = 0; x < f->width; ++x) {
                const i32 floor = covers(f, x, y, 0, 0, 0, 6.0f, 0.03f);
                const i32 opaque = covers(f, x, y, 0, 0, 3, 1.0f, 0.03f);
                const i32 behind = covers(f, x, y, 0, 0, 2, 1.0f, 0.03f);
                const i32 front = covers(f, x, y, 0, 0, 4, 1.0f, 0.03f);
                if (floor < 0 || opaque < 0 || behind < 0 || front < 0)
                    continue;
                const f32 under = (opaque == 1 ? 0.3f : floor == 1 ? 0.1f : 0.0f) * r->pre_exposure;
                f32 want[3] = {under, under, under};
                if (front == 1) {
                    for (u32 ch = 0; ch < 3; ++ch)
                        want[ch] = want[ch] * 0.5f + (ch == 1 ? 0.5f * r->pre_exposure : 0);
                    foreground++;
                }
                occluded += behind == 1 && opaque == 1;
                checked++;
                const f32 *got = px_of(r->lighting, f, x, y);
                for (u32 ch = 0; ch < 3; ++ch)
                    if (!close_to(got[ch], want[ch], 0.01f, 2e-3f * r->pre_exposure)) {
                        bad++;
                        break;
                    }
            }
        printf("occlusion model %u: checked %u hidden %u foreground %u bad %u\n", model, checked,
               occluded, foreground, bad);
        EXPECT_GT(checked, 10000u);
        EXPECT_GT(occluded, 200u);
        EXPECT_GT(foreground, 500u);
        EXPECT_EQ(bad, 0u);
    }
}

/* The Standard layers the deferred comparisons cover (DENG-429). */
typedef enum fx_layer {
    FX_LAYER_NONE,
    FX_LAYER_CLEARCOAT,
    FX_LAYER_SHEEN,
    FX_LAYER_IRIDESCENCE,
    FX_LAYER_ANISOTROPY
} fx_layer;

/* A Standard quad blended with alpha 1 against the same quad opaque (deferred), lit by a point
 * light and the sun: within 1 % at every interior pixel. With a layer (DENG-429), the deferred
 * pass must restore it from the pixel's material (the G-buffer does not keep it); the forward pass
 * resolves it per fragment. */
static void matches_deferred_layer(lx *f, de_gpu_backend b, fx_layer layer) {
    lx_open(f, b);
    lx_floor(f, mat_unlit(0.0f, 0.0f, 0.0f, 1.0f, 0));
    /* Values the thin G-buffer stores exactly (8-bit sRGB base colour 128, roughness 153 / 255),
     * so the comparison measures the shading, not the deferred path's quantization. */
    const u32 quad = mat_standard(0.2158605f, 1.0f, DE_COOKED_ALPHA_BLEND, false);
    de_standard_material *sm = &mat_of(quad)->standard;
    sm->roughness = 0.6f;
    switch (layer) {
    case FX_LAYER_CLEARCOAT:
        sm->clearcoat = 1.0f;
        /* Rough enough that the G-buffer's 11-bit normal does not move the coat's peak (a
         * roughness of 0.15 differs by up to 6 % at the peak, from the normal alone). */
        sm->clearcoat_roughness = 0.4f;
        break;
    case FX_LAYER_SHEEN:
        sm->sheen_color = (de_vec3){1.0f, 0.6f, 0.3f};
        sm->sheen_roughness = 0.5f;
        break;
    case FX_LAYER_IRIDESCENCE:
        sm->iridescence = 1.0f;
        sm->iridescence_ior = 1.3f;
        sm->iridescence_thickness_min = 100.0f;
        sm->iridescence_thickness_max = 400.0f;
        sm->roughness = 0.4f; /* 102 / 255 */
        break;
    case FX_LAYER_ANISOTROPY:
        sm->anisotropy = 0.8f;
        sm->anisotropy_rotation = 0.5f;
        sm->roughness = 0.4f; /* 102 / 255 */
        break;
    case FX_LAYER_NONE:
        break;
    }
    /* Anisotropy: the quad turned a quarter about +z, so its tangent (world +y) is not the frame
     * the normal alone would give (+x); the deferred pass must take the triangle's tangent. */
    const f32 turn = layer == FX_LAYER_ANISOTROPY ? 1.5707963f : 0.0f;
    world_set(&f->w, 1, lx_mesh_plane(f, 2, 2, 2.0f),
              trs((de_vec3){0, 0, 3}, (de_vec3){0, 0, 1}, turn, 1.0f), quad);
    point_light(f, 0, 0.6f, 0.4f, 5.0f, 3000.0f);
    const DeLight sun = {.kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = 4.0f};
    world_light(&f->w, 1, &sun,
                trs((de_vec3){0, 0, 20}, (de_vec3){1, 0, 0}, 0.4f, 1.0f)); /* -Z: down, tilted */
    lx_run(f);
    ASSERT_TRUE(f->res.forward);
    const size_t npx = (size_t)f->width * f->height * 4u;
    f32 *fwd = (f32 *)zalloc(npx * sizeof(f32));
    memcpy(fwd, f->res.lighting, npx * sizeof(f32));
    mat_of(quad)->alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    lx_run(f);
    EXPECT_FALSE(f->res.forward);
    u32 checked = 0, bad = 0;
    f32 worst = 0.0f, peak = 0.0f;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x) {
            if (covers(f, x, y, 0, 0, 3, 2.0f, 0.05f) != 1)
                continue;
            const f32 *a = px_of(fwd, f, x, y), *d = px_of(f->res.lighting, f, x, y);
            checked++;
            for (u32 ch = 0; ch < 3; ++ch) {
                peak = fmaxf(peak, d[ch]);
                const f32 rel = fabsf(a[ch] - d[ch]) / fmaxf(d[ch], 1e-6f);
                worst = fmaxf(worst, rel);
                if (!close_to(a[ch], d[ch], 0.01f, 1e-4f) && bad++ < 4)
                    printf("pixel %u,%u ch %u: forward %.5f deferred %.5f\n", x, y, ch, a[ch],
                           d[ch]);
            }
        }
    printf("checked %u pixels, worst relative difference %.4f, peak %.3f\n", checked, worst, peak);
    EXPECT_TRUE(checked > 2000u);
    EXPECT_TRUE(peak > 0.01f);
    EXPECT_EQ(bad, 0u);
    if (layer != FX_LAYER_NONE) {
        /* The layer shows: the same quad deferred without it differs (before DENG-429 the
         * deferred pass lit every layered surface as the plain base). */
        memcpy(fwd, f->res.lighting, npx * sizeof(f32));
        sm = &mat_of(quad)->standard;
        sm->clearcoat = 0.0f;
        sm->sheen_color = (de_vec3){0.0f, 0.0f, 0.0f};
        sm->iridescence = 0.0f;
        sm->anisotropy = 0.0f;
        lx_run(f);
        f32 change = 0.0f;
        for (u32 y = 0; y < f->height; ++y)
            for (u32 x = 0; x < f->width; ++x) {
                if (covers(f, x, y, 0, 0, 3, 2.0f, 0.05f) != 1)
                    continue;
                const f32 *a = px_of(fwd, f, x, y), *d = px_of(f->res.lighting, f, x, y);
                for (u32 ch = 0; ch < 3; ++ch)
                    change = fmaxf(change, fabsf(a[ch] - d[ch]) / fmaxf(d[ch], 1e-6f));
            }
        printf("largest change against the plain base: %.4f\n", change);
        EXPECT_TRUE(change > 0.05f);
    }
    free(fwd);
}

static void matches_deferred(lx *f, de_gpu_backend b) {
    matches_deferred_layer(f, b, FX_LAYER_NONE);
}

/* A quad turned away from the camera: double-sided, it draws twice (back faces, then front
 * faces) and is seen; single-sided, it draws once (front faces) and is culled. */
static void double_sided(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, mat_unlit(0.0f, 0.0f, 0.0f, 1.0f, 0));
    const u32 quad = mat_standard(0.5f, 1.0f, DE_COOKED_ALPHA_BLEND, true);
    world_set(&f->w, 1, lx_mesh_plane(f, 1, 1, 2.0f),
              trs((de_vec3){0, 0, 3}, (de_vec3){1, 0, 0}, 3.14159265f, 1.0f), quad);
    point_light(f, 0, 0, 0, 6, 3000.0f);
    lx_run(f);
    ASSERT_TRUE(f->res.forward);
    ASSERT_EQ(f->res.draw_count, 2u);
    EXPECT_EQ(f->res.draws[0].side, (u32)DE__FWD_SIDE_BACK);
    EXPECT_EQ(f->res.draws[1].side, (u32)DE__FWD_SIDE_FRONT);
    EXPECT_EQ(f->res.draws[0].instance, 1u);
    u32 inside = 0, lit = 0;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x)
            if (covers(f, x, y, 0, 0, 3, 1.0f, 0.05f) == 1) {
                inside++;
                lit += px_of(f->res.lighting, f, x, y)[0] > 1e-3f;
            }
    printf("double-sided: %u of %u quad pixels lit\n", lit, inside);
    EXPECT_TRUE(inside > 500u);
    EXPECT_EQ(lit, inside);
    mat_of(quad)->standard.double_sided = false;
    lx_run(f);
    ASSERT_TRUE(f->res.forward);
    ASSERT_EQ(f->res.draw_count, 1u);
    EXPECT_EQ(f->res.draws[0].side, (u32)DE__FWD_SIDE_FRONT);
    lit = 0;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x)
            if (covers(f, x, y, 0, 0, 3, 1.0f, 0.05f) == 1)
                lit += px_of(f->res.lighting, f, x, y)[0] > 1e-3f;
    printf("single-sided: %u quad pixels lit\n", lit);
    EXPECT_EQ(lit, 0u);
}

/* A transmissive quad (transmission 1, a volume absorbing red) over an emissive floor, no lights:
 * the scene-colour copy is made and the floor shows through x (1 - F) x Beer-Lambert. A blended
 * quad without transmission makes no copy. */
static void scene_color(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    const f32 floor_c[3] = {2.0f, 4.0f, 6.0f};
    lx_floor(f, mat_unlit(floor_c[0], floor_c[1], floor_c[2], 1.0f, 0));
    /* A uniform radiance field over the whole viewport remains uniform at every Gaussian mip. */
    world_set(&f->w, 0, 1, trs((de_vec3){0, 0, 0}, (de_vec3){0, 0, 1}, 0, 2), 1);
    const u32 quad = mat_standard(1.0f, 1.0f, DE_COOKED_ALPHA_OPAQUE, false);
    de_standard_material *s = &mat_of(quad)->standard;
    s->transmission = 1.0f;
    s->roughness = 0.3f;
    s->thickness = 0.5f;
    s->attenuation_color = (de_vec3){0.25f, 1.0f, 1.0f};
    s->attenuation_distance = 1.0f;
    world_set(&f->w, 1, lx_mesh_plane(f, 1, 1, 2.0f), at(0, 0, 3), quad);
    lx_run(f);
    ASSERT_TRUE(f->res.forward);
    EXPECT_TRUE(f->res.scene_color);
    u32 levels = 1;
    for (u32 w = f->width, h = f->height; w > 1 || h > 1;
         w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
        ++levels;
    EXPECT_EQ(f->res.forward_passes, levels + 1u);
    const f32 e = f->res.pre_exposure;
    u32 checked = 0, bad = 0;
    for (u32 y = 0; y < f->height; ++y)
        for (u32 x = 0; x < f->width; ++x) {
            if (covers(f, x, y, 0, 0, 3, 1.0f, 0.05f) != 1)
                continue;
            f32 p[2];
            pixel_on_plane(f, x, y, 3.0f, p);
            const f32 v[3] = {f->eye.x - p[0], f->eye.y - p[1], f->eye.z - 3.0f};
            const f32 nov = v[2] / sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            const f32 internal_cos = sqrtf(1.0f - (1.0f - nov * nov) / (1.5f * 1.5f));
            const f32 absorb[3] = {powf(.25f, .5f / internal_cos), 1, 1};
            const f32 f0 = 0.04f, fr = f0 + (1.0f - f0) * powf(1.0f - nov, 5.0f);
            const f32 *got = px_of(f->res.lighting, f, x, y);
            checked++;
            for (u32 ch = 0; ch < 3; ++ch) {
                const f32 want = floor_c[ch] * e * (1.0f - fr) * absorb[ch];
                if (!close_to(got[ch], want, 0.01f, 1e-3f) && bad++ < 4)
                    printf("pixel %u,%u ch %u: got %.4f want %.4f\n", x, y, ch, got[ch], want);
            }
        }
    printf("checked %u transmissive pixels, %u differ\n", checked, bad);
    EXPECT_TRUE(checked > 500u);
    EXPECT_EQ(bad, 0u);
    /* Blended, not transmissive: no copy. */
    s->transmission = 0.0f;
    mat_of(quad)->alpha_mode = DE_COOKED_ALPHA_BLEND;
    s->base_color.w = 0.5f;
    lx_run(f);
    ASSERT_TRUE(f->res.forward);
    EXPECT_FALSE(f->res.scene_color);
    EXPECT_EQ(f->res.forward_passes, 1u);
}

#define GPU_TESTS(sfx, backend)                                                                    \
    TEST_F(forward, forward_routing_excludes_visbuffer_##sfx, lx) {                                \
        routing(f, backend);                                                                       \
    }                                                                                              \
    TEST_F(forward, forward_zero_work_when_none_##sfx, lx) {                                       \
        zero_work(f, backend);                                                                     \
    }                                                                                              \
    TEST_F(forward, forward_sort_back_to_front_golden_##sfx, lx) {                                 \
        back_to_front(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(forward, forward_opaque_occlusion_preserves_near_blend_##sfx, lx) {                     \
        opaque_occlusion(f, backend);                                                              \
    }                                                                                              \
    TEST_F(forward, forward_lighting_matches_deferred_##sfx, lx) {                                 \
        matches_deferred(f, backend);                                                              \
    }                                                                                              \
    TEST_F(forward, deferred_layers_clearcoat_match_forward_##sfx, lx) {                           \
        matches_deferred_layer(f, backend, FX_LAYER_CLEARCOAT);                                    \
    }                                                                                              \
    TEST_F(forward, deferred_layers_sheen_match_forward_##sfx, lx) {                               \
        matches_deferred_layer(f, backend, FX_LAYER_SHEEN);                                        \
    }                                                                                              \
    TEST_F(forward, deferred_layers_iridescence_match_forward_##sfx, lx) {                         \
        matches_deferred_layer(f, backend, FX_LAYER_IRIDESCENCE);                                  \
    }                                                                                              \
    TEST_F(forward, deferred_layers_anisotropy_match_forward_##sfx, lx) {                          \
        matches_deferred_layer(f, backend, FX_LAYER_ANISOTROPY);                                   \
    }                                                                                              \
    TEST_F(forward, forward_double_sided_two_draws_##sfx, lx) {                                    \
        double_sided(f, backend);                                                                  \
    }                                                                                              \
    TEST_F(forward, forward_scene_color_copy_##sfx, lx) {                                          \
        scene_color(f, backend);                                                                   \
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
static u32 lcg_u(lcg *g) {
    g->s = g->s * 6364136223846793005ull + 1442695040888963407ull;
    return (u32)(g->s >> 33);
}

static bool sorted_ok(const de__transparent_item *it, u32 n) {
    for (u32 i = 1; i < n; ++i)
        if (de__transparent_before(&it[i], &it[i - 1u]))
            return false;
    return true;
}

/* forward_sort_back_to_front: farther first; equal depths by entity; NaN first; the sort keeps
 * every item. */
TEST(forward, forward_sort_back_to_front) {
    enum { N = 1000 };
    de__transparent_item *items = (de__transparent_item *)zalloc(N * sizeof *items);
    de__transparent_item *scratch = (de__transparent_item *)zalloc(N * sizeof *scratch);
    lcg g = {7};
    u64 sum = 0;
    for (u32 i = 0; i < N; ++i) {
        items[i] = (de__transparent_item){.depth = (f32)(lcg_u(&g) % 5000u) * 0.01f,
                                          .instance = i,
                                          .entity = 1000u + lcg_u(&g) % 300u,
                                          .index = i};
        if (i % 97u == 0)
            items[i].depth = NAN;
        sum += i;
    }
    de__transparent_sort(items, N, scratch);
    EXPECT_TRUE(sorted_ok(items, N));
    u64 got = 0;
    for (u32 i = 0; i < N; ++i)
        got += items[i].instance;
    EXPECT_EQ(got, sum);
    EXPECT_TRUE(isnan(items[0].depth));
    /* A small case by hand: depths 1, 3, 2, 3 (entities 5, 9, 1, 4) -> 3/4, 3/9, 2, 1. */
    de__transparent_item s[4] = {{.depth = 1, .entity = 5, .index = 0},
                                 {.depth = 3, .entity = 9, .index = 1},
                                 {.depth = 2, .entity = 1, .index = 2},
                                 {.depth = 3, .entity = 4, .index = 3}};
    de__transparent_sort(s, 4, scratch);
    EXPECT_EQ(s[0].index, 3u);
    EXPECT_EQ(s[1].index, 1u);
    EXPECT_EQ(s[2].index, 2u);
    EXPECT_EQ(s[3].index, 0u);
    free(items);
    free(scratch);
}

/* forward_sort_stable_ties: many equal depths. Equal depth and entity keep the gather order (the
 * index); with distinct entities the order is the same for 100 frames whatever order the items
 * were gathered in. */
TEST(forward, forward_sort_stable_ties) {
    enum { N = 200, FRAMES = 100 };
    de__transparent_item *items = (de__transparent_item *)zalloc(N * sizeof *items);
    de__transparent_item *scratch = (de__transparent_item *)zalloc(N * sizeof *scratch);
    u64 first[N];
    lcg g = {11};
    /* Ties of depth and entity: the index decides, ascending. */
    for (u32 i = 0; i < N; ++i)
        items[i] = (de__transparent_item){
            .depth = (f32)(i % 3u), .entity = 7u + (i % 2u), .index = i, .instance = i};
    de__transparent_sort(items, N, scratch);
    EXPECT_TRUE(sorted_ok(items, N));
    for (u32 i = 1; i < N; ++i)
        if (items[i].depth == items[i - 1u].depth && items[i].entity == items[i - 1u].entity)
            EXPECT_TRUE(items[i].index > items[i - 1u].index);
    /* Distinct entities, few depths, shuffled each frame. */
    for (u32 frame = 0; frame < FRAMES; ++frame) {
        for (u32 i = 0; i < N; ++i)
            items[i] = (de__transparent_item){.depth = (f32)((i * 7u) % 4u) * 0.5f,
                                              .entity = 0x100000000ull + (u64)((i * 37u) % N),
                                              .instance = i};
        for (u32 i = N - 1u; i > 0; --i) { /* Fisher-Yates */
            const u32 j = lcg_u(&g) % (i + 1u);
            const de__transparent_item t = items[i];
            items[i] = items[j];
            items[j] = t;
        }
        for (u32 i = 0; i < N; ++i)
            items[i].index = i; /* the gather order of this frame */
        de__transparent_sort(items, N, scratch);
        ASSERT_TRUE(sorted_ok(items, N));
        for (u32 i = 0; i < N; ++i) {
            if (frame == 0)
                first[i] = items[i].entity;
            else if (items[i].entity != first[i]) {
                printf("frame %u: position %u holds entity %llx, frame 0 had %llx\n", frame, i,
                       (unsigned long long)items[i].entity, (unsigned long long)first[i]);
                EXPECT_TRUE(items[i].entity == first[i]);
                frame = FRAMES;
                break;
            }
        }
    }
    free(items);
    free(scratch);
}

/* forward_routing_predicate: the C mirror of forward_routing.slang, on the features DENG-302
 * derives: blend and transmission are forward, opaque and mask are not, nor a block not READY. */
TEST(forward, forward_routing_predicate) {
    de_standard_material sm;
    memset(&sm, 0, sizeof sm);
    sm.base_color = (de_vec4){1, 1, 1, 1};
    de__gs_material m;
    memset(&m, 0, sizeof m);
    m.flags = DE__GS_MATERIAL_READY;
    m.features = de__material_features(&sm, DE_COOKED_ALPHA_OPAQUE, 0);
    EXPECT_FALSE(de__forward_material(&m)); /* opaque */
    m.features = de__material_features(&sm, DE_COOKED_ALPHA_MASK, 0);
    EXPECT_FALSE(de__forward_material(&m)); /* mask: the vis buffer's alpha bucket */
    m.features = de__material_features(&sm, DE_COOKED_ALPHA_BLEND, 0);
    EXPECT_TRUE(de__forward_material(&m));
    sm.transmission = 0.25f;
    m.features = de__material_features(&sm, DE_COOKED_ALPHA_OPAQUE, 0);
    EXPECT_TRUE(de__forward_material(&m));
    EXPECT_TRUE((m.features & DE__MATERIAL_FEATURE_TRANSMISSION) != 0u);
    m.flags = 0u;
    EXPECT_FALSE(de__forward_material(&m)); /* not READY */
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

/* The value of `public static const uint <name> = <v>;` in `text`, or -1. */
static i64 const_of(const char *text, const char *name) {
    char head[128];
    snprintf(head, sizeof head, "public static const uint %s = ", name);
    const char *p = strstr(text, head);
    return p != NULL ? (i64)strtoul(p + strlen(head), NULL, 0) : -1;
}

/* forward.layout_match: the shader constants and ForwardParams against forward.h. */
TEST(forward, layout_match) {
    char *t = read_text(DE_FWD_TEST_SHADER_DIR "/forward/forward_transparent.slang");
    char *r = read_text(DE_FWD_TEST_SHADER_DIR "/forward/forward_routing.slang");
    char *c = read_text(DE_FWD_TEST_SHADER_DIR "/culling/meshlet_cull.slang");
    ASSERT_TRUE(t != NULL && r != NULL && c != NULL);
    EXPECT_EQ(const_of(t, "DE_FWD_SIDE_FRONT"), (i64)DE__FWD_SIDE_FRONT);
    EXPECT_EQ(const_of(t, "DE_FWD_SIDE_BACK"), (i64)DE__FWD_SIDE_BACK);
    EXPECT_EQ(const_of(t, "DE_FWD_VERTICES"), (i64)DE__FWD_VERTICES);
    EXPECT_EQ(const_of(t, "DE_FWD_SCENE_COLOR_BPP"), (i64)DE__FWD_SCENE_COLOR_BPP);
    EXPECT_TRUE(strstr(r, "DE_MATERIAL_FEATURE_FORWARD") != NULL); /* the routing bit */
    EXPECT_EQ(const_of(c, "DE_VIS_BUCKET_FORWARD"), (i64)DE__VIS_BUCKET_FORWARD);
    EXPECT_EQ(const_of(c, "DE_VIS_MESHLET_COUNTER_FORWARD"), (i64)DE__VIS_MESHLET_COUNTER_FORWARD);
    /* ForwardParams: 16 uints, then DeClusterLists (8). */
    const char *ps = strstr(t, "struct ForwardParams\n");
    const char *pe = ps != NULL ? strstr(ps, "};") : NULL;
    ASSERT_TRUE(ps != NULL && pe != NULL);
    u32 words = 0;
    for (const char *ln = strchr(ps, '\n'); ln != NULL && ln < pe; ln = strchr(ln + 1, '\n')) {
        const char *s = ln + 1;
        while (*s == ' ')
            ++s;
        if (strncmp(s, "public uint ", 12) == 0)
            words += 1u;
        else if (strncmp(s, "public DeClusterLists ", 22) == 0)
            words += 8u;
    }
    EXPECT_EQ(words * 4u, (u32)sizeof(de__fwd_params));
    EXPECT_EQ(DE__FWD_VERTICES, 3u * 124u);
    free(t);
    free(r);
    free(c);
}
