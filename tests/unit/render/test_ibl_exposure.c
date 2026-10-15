/*
 * test_ibl_exposure.c: image-based lighting, physical exposure and the basic output pass
 * (DENG-304). Executable test_render_ibl_exposure; CTest names render_ibl_exposure.<suite>.<test>.
 *
 * GPU tests use the harness of test_clustered_lights.c (a fake render world of planes facing the
 * camera, the whole vis frame, de__lighting_build, a readback pass) with a synthetic environment:
 * an RGBA16F cube uploaded by the test (constant faces) and SH9 coefficients set to match, given
 * to lighting with de__lighting_set_environment. The references:
 *
 *   ibl_white_furnace_diffuse   a uniform environment of radiance L on a white Lambertian surface
 *                               (Standard, metallic 0, specular 0) gives L (2 %)
 *   ibl_white_furnace_metal     ... and on white metals of several roughnesses L (3 %): the
 *                               split-sum albedo with Kulla-Conty compensation is 1
 *   ibl_rotation                DeEnvironment.rotation 90 degrees: the SH irradiance of a tilted
 *                               plane and a mirror's reflected face follow Rz(-rotation)
 *   ibl_no_environment_fallback no environment: black, or r.ambient_fallback's constant radiance
 *   exposure_ev100_scale        EV100 0 and 10 differ by 2^10 in the lighting target and in the
 *                               resolve's emission (the scene constants' pre-exposure)
 *   ibl_deferred_forward_equal  a test kernel (tests/unit/render/data/ibl_forward.slang, cooked at
 *                               test time with DE_IBL_TEST_COOK) calls evaluate_indirect on the
 * same G-buffer texels as the deferred pass: equal (the shared function) output_basic_srgb_encode
 * the output kernel's clamp and sRGB OETF against the C formula
 *
 * CPU tests: the EV100 formula, the NaN fallback and the previous-frame exposure, the SH9 packing
 * and its irradiance, the strip mip counts.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv (loader_test_util.h) */
#endif
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#if defined(DE_IBL_TEST_COOK) && DE_IBL_TEST_COOK
#include <dengine/de_asset.h>

#include "asset/manifest_internal.h"
#include "cook/importers/shader/shader_importer.h"
#include "cook/importers/shader/slang_compile.h"
#include "cook/lib/de_cooker.h"
#include "gpu/shader/shader_internal.h"
#endif

#include "gpu/gpu_backend.h"
#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/materials/brdf_luts.h"
#include "render/loaders/render_assets.h"
#include "render/materials/material_models.h"
#include "render/passes/vis/gbuffer.h"
#include "render/passes/vis/vis_stages.h"
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"
#include "render/world/render_world.h"
#include "render/components_internal.h"
#include "render/formats/de_cooked_env.h"

#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#if defined(_WIN32) && DE_HAS_GPU_D3D12
#include "gpu/d3d12/d3d12_internal.h"
#endif
#include "loader_test_util.h" /* lt_env: a cooked BC6H environment blob */
#include "render_d3d12_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS             (10ull * 1000000000ull)
#define FIRST_FRAME_WAIT_NS (120ull * 1000000000ull) /* lavapipe JIT (DENG-428) */
#define PI_D                3.14159265358979323846

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

/* ================================================================================================
 * Meshes: planes of meshlet patches (as test_resolve.c)
 * ============================================================================================== */

#define PATCH      5u
#define PATCH_VERT (PATCH * PATCH)
#define PATCH_TRIS 32u
#define MAX_MESHES 4u
#define MAX_MATS   8u
#define MAX_LIGHTS 64u
#define STREAM_AL  256u

typedef struct fmesh {
    de_render_mesh rm;
    u32 meshlets, verts;
    de_gpu_buffer buf[4];
} fmesh;

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

/* Material (1-based id): grey `base`, roughness, metallic, specular. */
static u32 material_add(f32 base, f32 roughness, f32 metallic, f32 specular) {
    ASSERT_TRUE(g_mat_count < MAX_MATS);
    de_render_material *m = &g_mat[g_mat_count++];
    memset(m, 0, sizeof *m);
    m->standard.base_color = (de_vec4){base, base, base, 1.0f};
    m->standard.roughness = roughness;
    m->standard.metallic = metallic;
    m->standard.specular = specular;
    m->standard.normal_scale = 1.0f;
    m->standard.emissive_strength = 1.0f;
    return g_mat_count;
}

/* ================================================================================================
 * Fake render world (instances and lights)
 * ============================================================================================== */

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

typedef struct lx_result {
    bool valid;
    u32 width, height;
    f32 *pos;      /* xyzw per pixel (resolve debug) */
    f32 *lighting; /* rgba */
    f32 *diffuse, *specular;
    u32 *gb[3];
    de__cluster_light *lights; /* local_capacity */
    de__cluster_light dir[DE__LIGHTING_MAX_DIRECTIONAL];
    u32 *grid; /* offset, count per cluster */
    u32 *list;
    f32 *bounds; /* min xyz_, max xyz_ per cluster */
    u32 counters[DE__LIGHTING_COUNTER_BYTES / 4u];
    u32 tiles[DE__GS_MATERIAL_MODELS];
    de__vis_dispatch_args args[DE__VIS_CLASSIFY_LISTS];
    de__lighting_context lc;
    u64 frame;
    f32 *extra; /* rgba of the test's extra texture (lx.extra_tex) */
} lx_result;

typedef struct lx lx;
/* A test's hook after the lighting build: adds passes; may set lx.extra_tex to read back. */
typedef void (*lx_extra_fn)(lx *f, de__vis_context *vc, de__lighting_context *lc);

struct lx {
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
    de__brdf_luts *luts; /* the Standard model's energy LUT (DENG-299) */
    de_gpu_fence fence;
    u64 frames;
    de_gpu_buffer rb;
    u64 rb_size;
    world w;
    de_vec3 eye, target;
    f32 fov;
    u32 width, height;
    lx_result res;
    /* DENG-304 */
    f32 pre_exposure; /* > 0: the frame's pre-exposure (scene constants and lighting) */
    bool use_ev100;   /* the exposure from ev100 through de__lighting_view_exposure */
    f32 ev100;        /* NaN: the camera's automatic exposure (r.exposure_ev100_default) */
    de_gpu_texture cube;
    lx_extra_fn extra;
    de_rg_resource extra_tex; /* set by `extra` this frame */
    u32 extra_bpp;            /* 8 (RGBA16F) or 16 (RGBA32F) */
    void *user;
};

static void lx_setup(lx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "lights_test"}, &f->arena), DE_OK);
    de_error_clear();
    memset(g_mesh, 0, sizeof g_mesh);
    g_mesh_count = 0;
    memset(g_mat, 0, sizeof g_mat);
    g_mat_count = 0;
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

static void free_result(lx_result *r) {
    free(r->pos);
    free(r->lighting);
    free(r->diffuse);
    free(r->specular);
    for (u32 i = 0; i < 3; ++i)
        free(r->gb[i]);
    free(r->lights);
    free(r->grid);
    free(r->list);
    free(r->bounds);
    free(r->extra);
    memset(r, 0, sizeof *r);
}

static void lx_teardown(lx *f) {
    if (f->g) {
        de__rg_reset(f->g);
        de__rg_destroy(f->g);
    }
    if (f->dev)
        de_gpu_wait_idle(f->dev);
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
        if (DE_HANDLE_VALID(f->cube))
            de_gpu_texture_destroy(f->dev, f->cube);
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
    const char *const names[] = {
        "r.lod_bias",           "r.occlusion",        "r.rg_record_cost",
        "r.cluster_max_lights", "r.split_lighting",   "r.emissive_light_threshold",
        "r.light_cutoff_lux",   "r.cluster_tile",     "r.cluster_slices",
        "r.cluster_far",        "r.ambient_fallback", "r.exposure_ev100_default"};
    const char *const values[] = {"0",    "1",  "32", "256", "0", "1000",
                                  "0.05", "64", "32", "500", "0", "15"};
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
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "lx"},
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
    de__lighting_set_debug(f->lt, &(de__lighting_debug){.readback = true});
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

/* The receiver every GPU test lights: a 12 m square plane at z = 0 in slot 0. */
static void lx_floor(lx *f, u32 mat) {
    const u32 m = lx_mesh_plane(f, 8, 8, 1.5f);
    world_set(&f->w, 0, m, at(0, 0, 0), mat);
}

/* ---------- One frame ---------- */

typedef struct rb_item {
    de_rg_resource r;
    bool texture;
    u64 off, bytes;
    u32 pitch;
} rb_item;

enum {
    RB_EXTRA,
    RB_POS,
    RB_LIGHT,
    RB_GB0,
    RB_GB1,
    RB_GB2,
    RB_DIFF,
    RB_SPEC,
    RB_LIGHTS,
    RB_DIR,
    RB_GRID,
    RB_LIST,
    RB_BOUNDS,
    RB_COUNTERS,
    RB_TILES,
    RB_ARGS,
    RB_N
};

typedef struct rb_pass {
    lx *f;
    rb_item it[RB_N];
} rb_pass;

static void rb_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    for (u32 i = 0; i < RB_N; ++i) {
        const rb_item *it = &p->it[i];
        if (!DE_HANDLE_VALID(it->r))
            continue;
        if (it->texture)
            de_gpu_cmd_copy_texture_to_buffer(cmd, p->f->rb, it->off, it->pitch,
                                              de_rg_texture(pass, it->r), 0, 0);
        else
            de_gpu_cmd_copy_buffer(cmd, p->f->rb, it->off, de_rg_buffer(pass, it->r), 0, it->bytes);
    }
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

/* Builds, runs and waits for one frame; fills f->res. */
static bool lx_frame(lx *f) {
    const u64 n = f->frames++;
    world_begin(&f->w);
    de_mat4 view, proj;
    camera(f, &view, &proj);
    /* DENG-304: the frame's exposure, as the renderer's GPU-scene view stage computes it. */
    f32 pre = f->pre_exposure > 0.0f ? f->pre_exposure : 0.0f, prev = pre;
    if (f->use_ev100)
        pre = de__lighting_view_exposure(f->lt, 0, (de_view){.id = 1}, n, f->ev100, &prev);
    else if (pre > 0.0f)
        de__lighting_view_inputs(f->lt, 0)->pre_exposure = pre;
    f->extra_tex = (de_rg_resource){0};
    if (de__gpu_scene_update(f->gs, &f->w.f) != DE_OK)
        return false;
    if (de__gpu_scene_set_view(f->gs, 0,
                               &(de__gpu_view_params){.view = view,
                                                      .proj = proj,
                                                      .camera_position = f->eye,
                                                      .width = f->width,
                                                      .height = f->height,
                                                      .pre_exposure = pre,
                                                      .prev_pre_exposure = prev}) != DE_OK)
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
    EXPECT_TRUE(de__vis_frame_context(f->vis, d.view, f->g, n) == vc);
    de__lighting_context *lc = NULL;
    if (de__lighting_build(f->lt, f->g, vc, &lc) != DE_OK)
        return false;
    if (f->extra != NULL && lc != NULL)
        f->extra(f, vc, lc);
    rb_pass *rp = NULL;
    if (lc != NULL) {
        rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
        ASSERT_TRUE(rp != NULL);
        rp->f = f;
        const de_rg_resource tex[8] = {
            f->extra_tex,         vc->resolve_debug_pos, vc->gbuffer[DE__GB_LIGHTING],
            vc->gbuffer[DE__GB0], vc->gbuffer[DE__GB1],  vc->gbuffer[DE__GB2],
            lc->diffuse,          lc->specular};
        const u32 bpp[8] = {f->extra_bpp != 0 ? f->extra_bpp : 8u, 16, 8, 4, 4, 4, 8, 8};
        u64 off = 0;
        for (u32 i = 0; i < 8; ++i) {
            rb_item *it = &rp->it[i];
            it->r = tex[i];
            it->texture = true;
            it->pitch = (u32)align_up((u64)f->width * bpp[i], 256u);
            it->off = off;
            it->bytes = (u64)it->pitch * f->height;
            if (DE_HANDLE_VALID(tex[i]))
                off = align_up(off + it->bytes, 512u);
        }
        const de_rg_resource bufs[RB_N - RB_LIGHTS] = {
            lc->lights, lc->dir_lights, lc->grid_buf,      lc->list,
            lc->bounds, lc->counters,   lc->tile_counters, vc->classify_args};
        const u64 sizes[RB_N - RB_LIGHTS] = {(u64)lc->local_capacity * sizeof(de__cluster_light),
                                             DE__LIGHTING_MAX_DIRECTIONAL *
                                                 sizeof(de__cluster_light),
                                             (u64)lc->grid.count * 8u,
                                             (u64)lc->list_capacity * 4u,
                                             (u64)lc->grid.count * 32u,
                                             DE__LIGHTING_COUNTER_BYTES,
                                             DE__LIGHTING_TILE_COUNTER_BYTES,
                                             DE__VIS_CLASSIFY_LISTS * 16u};
        for (u32 i = RB_LIGHTS; i < RB_N; ++i) {
            rp->it[i] =
                (rb_item){.r = bufs[i - RB_LIGHTS], .off = off, .bytes = sizes[i - RB_LIGHTS]};
            off = align_up(off + rp->it[i].bytes, 512u);
        }
        if (off > f->rb_size) {
            if (DE_HANDLE_VALID(f->rb)) {
                de_gpu_wait_idle(f->dev);
                de_gpu_buffer_destroy(f->dev, f->rb);
            }
            f->rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = off,
                                                                       .usage = DE_USAGE_COPY_DST,
                                                                       .memory = DE_MEM_READBACK,
                                                                       .name = "lights readback"});
            ASSERT_TRUE(DE_HANDLE_VALID(f->rb));
            f->rb_size = off;
        }
        de_rg_pass *p = de_rg_add_pass(f->g, "test.readback", DE_QUEUE_GRAPHICS, rb_execute, rp);
        for (u32 i = 0; i < RB_N; ++i)
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
        /* The output pass samples the lighting target (the deferred pass's storage access is
         * not what VVL checks). */
        de_rg_read(p, vc->gbuffer[DE__GB_LIGHTING], DE_ACCESS_SHADER_READ);
        const de_rg_resource storage[7] = {vc->gbuffer[DE__GB_MOTION],
                                           (de_rg_resource){0},
                                           vc->resolve_debug_pos,
                                           vc->resolve_debug_uv,
                                           lc->diffuse,
                                           lc->specular,
                                           f->extra_tex};
        for (u32 i = 0; i < 7; ++i)
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
    lx_result *res = &f->res;
    free_result(res);
    res->frame = n;
    de__gpu_scene_frame_end(f->gs, f->g, true);
    if (!lx_wait(f, n + 1, "frame"))
        return false;
    f->warm = true;
    if (lc == NULL)
        return true;
    res->lc = *lc;
    const u8 *m = (const u8 *)de_gpu_buffer_map(f->dev, f->rb);
    const u32 w = f->width, h = f->height;
    res->width = w;
    res->height = h;
    res->pos = (f32 *)rows_of(m, &rp->it[RB_POS], w, h, 16);
    if (DE_HANDLE_VALID(f->extra_tex))
        res->extra = f->extra_bpp == 16u ? (f32 *)rows_of(m, &rp->it[RB_EXTRA], w, h, 16)
                                         : halves_of(m, &rp->it[RB_EXTRA], w, h);
    res->lighting = halves_of(m, &rp->it[RB_LIGHT], w, h);
    for (u32 i = 0; i < 3; ++i)
        res->gb[i] = (u32 *)rows_of(m, &rp->it[RB_GB0 + i], w, h, 4);
    if (DE_HANDLE_VALID(lc->diffuse)) {
        res->diffuse = halves_of(m, &rp->it[RB_DIFF], w, h);
        res->specular = halves_of(m, &rp->it[RB_SPEC], w, h);
    }
    res->lights = (de__cluster_light *)zalloc((size_t)rp->it[RB_LIGHTS].bytes);
    memcpy(res->lights, m + rp->it[RB_LIGHTS].off, (size_t)rp->it[RB_LIGHTS].bytes);
    memcpy(res->dir, m + rp->it[RB_DIR].off, sizeof res->dir);
    res->grid = (u32 *)zalloc((size_t)rp->it[RB_GRID].bytes);
    memcpy(res->grid, m + rp->it[RB_GRID].off, (size_t)rp->it[RB_GRID].bytes);
    res->list = (u32 *)zalloc((size_t)rp->it[RB_LIST].bytes);
    memcpy(res->list, m + rp->it[RB_LIST].off, (size_t)rp->it[RB_LIST].bytes);
    res->bounds = (f32 *)zalloc((size_t)rp->it[RB_BOUNDS].bytes);
    memcpy(res->bounds, m + rp->it[RB_BOUNDS].off, (size_t)rp->it[RB_BOUNDS].bytes);
    memcpy(res->counters, m + rp->it[RB_COUNTERS].off, sizeof res->counters);
    memcpy(res->tiles, m + rp->it[RB_TILES].off, sizeof res->tiles);
    memcpy(res->args, m + rp->it[RB_ARGS].off, sizeof res->args);
    res->valid = true;
    return true;
}

/* Two frames (the second with every table uploaded and the kernels warm); the last's results. */
static void lx_run(lx *f) {
    ASSERT_TRUE(lx_frame(f));
    ASSERT_TRUE(lx_frame(f));
    ASSERT_TRUE(f->res.valid);
}

/* ================================================================================================
 * CPU reference shading
 * ============================================================================================== */

typedef struct surf {
    f32 pos[3], n[3];
    f32 base, roughness, metallic, specular;
    u32 model, flags;
} surf;

/* The surface of pixel i from the decoded G-buffer (false: no surface). */
static bool surf_at(const lx_result *r, size_t i, surf *s) {
    if (!(r->pos[4 * i + 3] > 0.5f) || !(r->lighting[4 * i + 3] > 0.5f))
        return false;
    memcpy(s->pos, &r->pos[4 * i], sizeof s->pos);
    const u32 g0 = r->gb[0][i], g2 = r->gb[2][i];
    s->base = de__gb_srgb_decode((f32)(g0 & 255u) / 255.0f);
    de__gb1_unpack(r->gb[1][i], s->n, &s->roughness);
    s->metallic = (f32)(g2 & 255u) / 255.0f;
    s->model = (g2 >> 8) & 255u;
    s->flags = (g2 >> 16) & 255u;
    s->specular = (f32)(g2 >> 24) / 255.0f;
    return true;
}

/* Relative error with an absolute floor. */
static bool close_to(f32 got, f32 want, f32 rel, f32 floor_abs) {
    return fabsf(got - want) <= rel * fabsf(want) + floor_abs;
}

/* ================================================================================================
 * The environment
 * ============================================================================================== */

#define ENV_FACE 32u
#define ENV_MIPS 6u
#define SH_Y0    0.282094791773878
#define SH_Y1    0.488602511902920

static u16 f32_to_half(f32 v) {
    u32 x;
    memcpy(&x, &v, 4);
    const u32 sign = (x >> 16) & 0x8000u;
    const i32 e = (i32)((x >> 23) & 0xFFu) - 127 + 15;
    u32 m = x & 0x7FFFFFu;
    if (e <= 0)
        return (u16)sign; /* the tests' values are not that small */
    if (e >= 31)
        return (u16)(sign | 0x7C00u);
    m += 0x1000u; /* round to nearest */
    if (m & 0x800000u)
        return (u16)(sign | ((u32)(e + 1) << 10));
    return (u16)(sign | ((u32)e << 10) | (m >> 13));
}

typedef struct cube_pass {
    de_rg_resource t;
    de_gpu_buffer src;
    u64 off[ENV_MIPS][6];
    u32 pitch[ENV_MIPS];
} cube_pass;

static void cube_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const cube_pass *p = (const cube_pass *)user;
    for (u32 m = 0; m < ENV_MIPS; ++m)
        for (u32 f = 0; f < 6; ++f)
            de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, p->t), m, f, p->src,
                                              p->off[m][f], p->pitch[m]);
}

/* An RGBA16F cube (ENV_FACE, ENV_MIPS mips), face k of every mip the constant `rgb[k]`, uploaded
 * in a graph of its own; lx.cube. */
static void env_cube(lx *f, const f32 *rgb) { /* rgb[3 face + channel] */
    if (DE_HANDLE_VALID(f->cube)) {
        de_gpu_wait_idle(f->dev);
        de_gpu_texture_destroy(f->dev, f->cube);
    }
    f->cube = de_gpu_texture_create(
        f->dev,
        &(de_gpu_texture_desc){.dim = DE_TEX_CUBE,
                               .format = DE_FMT_RGBA16_FLOAT,
                               .width = ENV_FACE,
                               .height = ENV_FACE,
                               .depth_or_layers = 6,
                               .mip_count = ENV_MIPS,
                               .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                               .name = "test environment"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->cube));
    cube_pass *cp = (cube_pass *)zalloc(sizeof *cp);
    u64 total = 0;
    for (u32 m = 0; m < ENV_MIPS; ++m) {
        const u32 s = ENV_FACE >> m;
        cp->pitch[m] = (u32)align_up((u64)s * 8u, 256u);
        for (u32 k = 0; k < 6; ++k) {
            cp->off[m][k] = total;
            total = align_up(total + (u64)cp->pitch[m] * s, 512u);
        }
    }
    cp->src =
        de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = total,
                                                           .usage = DE_USAGE_COPY_SRC,
                                                           .memory = DE_MEM_UPLOAD,
                                                           .name = "test environment upload"});
    ASSERT_TRUE(DE_HANDLE_VALID(cp->src));
    u8 *map = (u8 *)de_gpu_buffer_map(f->dev, cp->src);
    for (u32 m = 0; m < ENV_MIPS; ++m) {
        const u32 s = ENV_FACE >> m;
        for (u32 k = 0; k < 6; ++k)
            for (u32 y = 0; y < s; ++y)
                for (u32 x = 0; x < s; ++x) {
                    u16 *t = (u16 *)(void *)(map + cp->off[m][k] + (u64)y * cp->pitch[m] + 8u * x);
                    for (u32 c = 0; c < 3; ++c)
                        t[c] = f32_to_half(rgb[3u * k + c]);
                    t[3] = f32_to_half(1.0f);
                }
    }
    const u64 n = f->frames++;
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    cp->t = de_rg_import_texture(f->g, f->cube, DE_ACCESS_NONE);
    de_rg_pass *p = de_rg_add_pass(f->g, "test.cube_upload", DE_QUEUE_GRAPHICS, cube_execute, cp);
    de_rg_write(p, cp->t, DE_ACCESS_COPY_DST);
    de_rg_export(f->g, cp->t, DE_ACCESS_SHADER_READ);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
    ASSERT_EQ(de__rg_compile(f->g), DE_OK);
    ASSERT_EQ(de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = n,
                                                          .frame_fence = f->fence,
                                                          .frame_value = n + 1}),
              DE_OK);
    ASSERT_TRUE(lx_wait(f, n + 1, "cube upload"));
    de_gpu_buffer_destroy(f->dev, cp->src);
    free(cp);
}

/* The environment of lx.cube with SH9 `sh` (NULL: zero), intensity and rotation. */
static void env_set(lx *f, const f32 *sh, f32 intensity, f32 rotation) { /* sh[3 k + c] */
    de__ibl_environment e;
    memset(&e, 0, sizeof e);
    e.valid = true;
    e.cube = f->cube;
    e.format = DE_FMT_RGBA16_FLOAT;
    e.face_size = ENV_FACE;
    e.mip_count = ENV_MIPS;
    e.mip_per_roughness = (f32)(ENV_MIPS - 1u);
    if (sh != NULL)
        memcpy(e.sh9, sh, sizeof e.sh9);
    e.intensity = intensity;
    e.rotation = rotation;
    de__lighting_set_environment(f->lt, &e);
}

/* A uniform environment of radiance `l` (cube and SH9: E = pi l). */
static void env_uniform(lx *f, f32 l) {
    f32 faces[6][3], sh[9][3];
    memset(sh, 0, sizeof sh);
    for (u32 k = 0; k < 6; ++k)
        faces[k][0] = faces[k][1] = faces[k][2] = l;
    for (u32 c = 0; c < 3; ++c)
        sh[0][c] = (f32)(PI_D * l / SH_Y0);
    env_cube(f, &faces[0][0]);
    env_set(f, &sh[0][0], 1.0f, 0.0f);
}

/* Planes of 2 x 2 patches of 1 m (mesh 1-based id). */
static u32 lx_square(lx *f) {
    return lx_mesh_plane(f, 2, 2, 1.0f);
}

/* ================================================================================================
 * GPU tests
 * ============================================================================================== */

/* ibl_white_furnace_diffuse: a uniform environment of radiance 2 on a white Lambertian plane (the
 * Standard model with metallic 0 and specular 0) gives 2 (2 %). */
static void furnace_diffuse(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, material_add(1.0f, 1.0f, 0.0f, 0.0f));
    env_uniform(f, 2.0f);
    lx_run(f);
    const lx_result *r = &f->res;
    u32 checked = 0, bad = 0;
    f32 worst = 0.0f;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        surf s;
        if (!surf_at(r, i, &s))
            continue;
        ++checked;
        for (u32 c = 0; c < 3; ++c) {
            const f32 e = fabsf(r->lighting[4 * i + c] - 2.0f) / 2.0f;
            worst = e > worst ? e : worst;
            bad += e > 0.02f;
        }
    }
    printf("diffuse furnace: %u px, worst %.3f%%\n", checked, 100.0 * worst);
    EXPECT_GT(checked, 10000u);
    EXPECT_EQ(bad, 0u);
}

/* ibl_white_furnace_metal: the same environment on white metals of roughness 0.2, 0.5 and 1 gives
 * 2 (3 %): the split-sum albedo of F0 = F90 = 1 with Kulla-Conty compensation is 1. */
static void furnace_metal(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    const u32 sq = lx_square(f);
    const f32 rough[3] = {0.2f, 0.5f, 1.0f};
    for (u32 k = 0; k < 3; ++k)
        world_set(&f->w, k, sq, at(-2.5f + 2.5f * (f32)k, 0, 0),
                  material_add(1.0f, rough[k], 1.0f, 0.5f));
    env_uniform(f, 2.0f);
    lx_run(f);
    const lx_result *r = &f->res;
    u32 checked[3] = {0, 0, 0}, bad = 0;
    f32 worst[3] = {0, 0, 0};
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        surf s;
        if (!surf_at(r, i, &s))
            continue;
        const u32 k = s.roughness < 0.35f ? 0u : s.roughness < 0.75f ? 1u : 2u;
        ++checked[k];
        const f32 e = fabsf(r->lighting[4 * i] - 2.0f) / 2.0f;
        worst[k] = e > worst[k] ? e : worst[k];
        bad += e > 0.03f;
    }
    printf("metal furnace: roughness 0.2 worst %.3f%% (%u px), 0.5 %.3f%% (%u px), 1 %.3f%% (%u "
           "px)\n",
           100.0 * worst[0], checked[0], 100.0 * worst[1], checked[1], 100.0 * worst[2],
           checked[2]);
    for (u32 k = 0; k < 3; ++k)
        EXPECT_GT(checked[k], 500u);
    EXPECT_EQ(bad, 0u);
}

/* The world reflection of the camera ray of surface s. */
static void reflect_dir(const lx *f, const surf *s, f32 out[3]) {
    f32 v[3] = {f->eye.x - s->pos[0], f->eye.y - s->pos[1], f->eye.z - s->pos[2]};
    const f32 l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    f32 d = 0.0f;
    for (u32 k = 0; k < 3; ++k) {
        v[k] /= l;
        d += v[k] * s->n[k];
    }
    for (u32 k = 0; k < 3; ++k)
        out[k] = 2.0f * d * s->n[k] - v[k];
}

/* ibl_rotation: DeEnvironment.rotation turns what a surface sees about +Z. A Lambertian plane
 * tilted towards +X under an SH9 with E(n) = pi (1 + 0.8 x) gets 1.8 sin-weighted at rotation 0
 * and 1 at 90 degrees (the environment's +X now lies along +Y); a mirror tilted towards +X
 * reflects the +X face at rotation 0 and the -Y face at 90 degrees. */
static void rotation(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    const u32 sq = lx_square(f);
    const f32 tilt = 40.0f * (f32)PI_D / 180.0f;
    world_set(&f->w, 0, sq, trs((de_vec3){-2.5f, 0, 0}, (de_vec3){0, 1, 0}, tilt, 1.0f),
              material_add(1.0f, 1.0f, 0.0f, 0.0f));
    world_set(&f->w, 1, sq, trs((de_vec3){2.5f, 0, 0}, (de_vec3){0, 1, 0}, 0.5236f, 1.0f),
              material_add(1.0f, 0.0f, 1.0f, 0.5f));
    const f32 faces[6][3] = {{4.0f, 0.5f, 0.5f}, {0.5f, 4.0f, 0.5f}, {0.5f, 0.5f, 4.0f},
                             {3.0f, 3.0f, 0.5f}, {0.3f, 0.3f, 0.3f}, {0.1f, 0.1f, 0.1f}};
    env_cube(f, &faces[0][0]);
    f32 sh[9][3];
    memset(sh, 0, sizeof sh);
    for (u32 c = 0; c < 3; ++c) {
        sh[0][c] = (f32)(PI_D / SH_Y0);
        sh[3][c] = (f32)(0.8 * PI_D / SH_Y1); /* + 0.8 pi x */
    }
    f32 diffuse_mean[2] = {0, 0};
    for (u32 pass = 0; pass < 2; ++pass) {
        const f32 rot = pass == 0 ? 0.0f : (f32)(PI_D * 0.5);
        env_set(f, &sh[0][0], 1.0f, rot);
        lx_run(f);
        const lx_result *r = &f->res;
        u32 nd = 0, ns = 0, bad = 0;
        f64 sum = 0.0;
        for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
            surf s;
            if (!surf_at(r, i, &s))
                continue;
            const f32 got = r->lighting[4 * i];
            if (s.metallic < 0.5f) {
                /* E(n) of the rotated environment: Rz(-rot) n */
                const f32 x = cosf(rot) * s.n[0] + sinf(rot) * s.n[1];
                const f32 want = 1.0f + 0.8f * x;
                bad += !close_to(got, want, 0.02f, 0.0f);
                sum += got;
                ++nd;
            } else {
                f32 rd[3], d[3], sc, tc;
                reflect_dir(f, &s, rd);
                d[0] = cosf(rot) * rd[0] + sinf(rot) * rd[1];
                d[1] = -sinf(rot) * rd[0] + cosf(rot) * rd[1];
                d[2] = rd[2];
                const u32 face = de_cooked_env_direction_face(d, &sc, &tc);
                if (fabsf(sc) > 0.85f || fabsf(tc) > 0.85f)
                    continue; /* near a face edge */
                ++ns;
                for (u32 c = 0; c < 3; ++c)
                    bad += !close_to(r->lighting[4 * i + c], faces[face][c], 0.03f, 0.0f);
                if (pass == 0 && ns == 1)
                    EXPECT_EQ(face, (u32)DE_COOKED_ENV_FACE_POS_X);
                if (pass == 1 && ns == 1)
                    EXPECT_EQ(face, (u32)DE_COOKED_ENV_FACE_NEG_Y);
            }
        }
        diffuse_mean[pass] = nd > 0 ? (f32)(sum / nd) : 0.0f;
        printf("rotation %.0f deg: %u diffuse px (mean %.3f), %u mirror px; %u bad\n",
               (double)(rot * 180.0f / (f32)PI_D), nd, diffuse_mean[pass], ns, bad);
        EXPECT_GT(nd, 500u);
        EXPECT_GT(ns, 200u);
        EXPECT_EQ(bad, 0u);
    }
    EXPECT_TRUE(diffuse_mean[0] > diffuse_mean[1] * 1.2f);
}

/* ibl_no_environment_fallback: without an environment the indirect light is black, or with
 * r.ambient_fallback a constant radiance from every direction (a white Lambertian and a white
 * metal give it back). */
static void no_environment(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    const u32 sq = lx_square(f);
    world_set(&f->w, 0, sq, at(-1.5f, 0, 0), material_add(1.0f, 1.0f, 0.0f, 0.0f));
    world_set(&f->w, 1, sq, at(1.5f, 0, 0), material_add(1.0f, 0.5f, 1.0f, 0.5f));
    lx_run(f);
    u32 lit = 0, checked = 0;
    for (size_t i = 0; i < (size_t)f->res.width * f->res.height; ++i) {
        surf s;
        if (!surf_at(&f->res, i, &s))
            continue;
        ++checked;
        lit += f->res.lighting[4 * i] != 0.0f;
    }
    EXPECT_GT(checked, 1000u);
    EXPECT_EQ(lit, 0u);
    ASSERT_TRUE(cvar_set("r.ambient_fallback", "0.5"));
    lx_run(f);
    u32 bad = 0;
    checked = 0;
    for (size_t i = 0; i < (size_t)f->res.width * f->res.height; ++i) {
        surf s;
        if (!surf_at(&f->res, i, &s))
            continue;
        ++checked;
        bad += !close_to(f->res.lighting[4 * i], 0.5f, s.metallic > 0.5f ? 0.03f : 0.02f, 0.0f);
    }
    printf("ambient fallback: %u px, %u bad\n", checked, bad);
    EXPECT_GT(checked, 1000u);
    EXPECT_EQ(bad, 0u);
}

/* exposure_ev100_scale: EV100 0 and EV100 10 (through de__lighting_view_exposure, as the renderer)
 * give lighting and resolve emission that differ by 2^10. */
static void exposure_scale(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, material_add(1.0f, 1.0f, 0.0f, 0.0f));
    const u32 patch = lx_mesh_plane(f, 1, 1, 1.0f);
    const u32 em = material_add(0.0f, 1.0f, 0.0f, 0.0f);
    g_mat[em - 1u].standard.emissive = (de_vec3){1.0f, 1.0f, 1.0f};
    g_mat[em - 1u].standard.emissive_strength = 2000.0f;
    world_set(&f->w, 1, patch, at(0, 0, 0.5f), em);
    ASSERT_TRUE(cvar_set("r.emissive_light_threshold", "0")); /* no proxy light on the floor */
    env_uniform(f, 1000.0f);
    f->use_ev100 = true;
    f64 floor_mean[2] = {0, 0}, emit_mean[2] = {0, 0};
    for (u32 pass = 0; pass < 2; ++pass) {
        f->ev100 = pass == 0 ? 0.0f : 10.0f;
        lx_run(f);
        const lx_result *r = &f->res;
        u32 nf = 0, ne = 0;
        for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
            surf s;
            if (!surf_at(r, i, &s))
                continue;
            if (s.pos[2] > 0.25f) {
                emit_mean[pass] += r->lighting[4 * i];
                ++ne;
            } else {
                floor_mean[pass] += r->lighting[4 * i];
                ++nf;
            }
        }
        ASSERT_TRUE(nf > 1000u && ne > 100u);
        floor_mean[pass] /= nf;
        emit_mean[pass] /= ne;
    }
    const f64 rf = floor_mean[0] / floor_mean[1], re = emit_mean[0] / emit_mean[1];
    printf("EV100 0 / EV100 10: lighting %.4f (%.3f / %.6f), emission %.4f (%.3f / %.6f)\n", rf,
           floor_mean[0], floor_mean[1], re, emit_mean[0], emit_mean[1]);
    EXPECT_TRUE(fabs(rf / 1024.0 - 1.0) < 0.005);
    EXPECT_TRUE(fabs(re / 1024.0 - 1.0) < 0.005);
    /* EV100 0: exposure 1 / 1.2. */
    EXPECT_TRUE(fabs(floor_mean[0] - 1000.0 / 1.2) < 0.02 * 1000.0 / 1.2);
    EXPECT_TRUE(fabs(emit_mean[0] - 2000.0 / 1.2) < 0.005 * 2000.0 / 1.2);
}

#if defined(DE_IBL_TEST_COOK) && DE_IBL_TEST_COOK

/* "iblsrc:/": data/ over the engine's shaders/. */
static bool mount_sources(void) {
    static int mounted = -1;
    if (mounted < 0)
        mounted = de_fs_mount("iblsrc", DE_IBL_TEST_SHADER_DIR, 0, 0) == DE_OK &&
                  de_fs_mount("iblsrc", DE_IBL_TEST_DATA_DIR, 0, 1) == DE_OK;
    return mounted == 1;
}

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

/* data/ibl_forward.slang's cs_ibl_forward for the fixture's backend; invalid with `why`. */
static de_gpu_pipeline forward_pipeline(lx *f, char *why, size_t cap) {
    const de__shader_target target = de__shader_target_for_backend(f->backend);
    char msg[200];
    if (!de__slang_available(msg, sizeof msg)) {
        snprintf(why, cap, "no Slang compiler: %.150s", msg);
        return (de_gpu_pipeline){0};
    }
    if (!mount_sources()) {
        snprintf(why, cap, "cannot mount the shader sources");
        return (de_gpu_pipeline){0};
    }
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
    a.id = "ibl_forward";
    a.type = "shader";
    a.source = "ibl_forward.slang";
    a.file = "iblsrc:/assets.toml";
    a.settings = &kv;
    a.setting_count = 1;
    de_cook_result res;
    memset(&res, 0, sizeof res);
    de_result r = DE_OK;
    de_gpu_pipeline pipe = {0};
    if (de_cooker_create(&cd, &c) != DE_OK || de_cooker_register_type(c, &t) != DE_OK)
        r = DE_ERR_INTERNAL;
    if (r == DE_OK) {
        de_cook_input in = {.asset = &a, .asset_root = "iblsrc:/", .source = NULL};
        r = de_cooker_cook(c, &in, &res);
    }
    de__shader_view view;
    if (r == DE_OK && de__shader_view_init(res.data, res.size, &view) == DE_OK) {
        for (u32 e = 0; e < view.header.entry_count; ++e) {
            const char *n = NULL;
            (void)de__shader_view_entry(&view, e, &n);
            const i32 ri = strcmp(n, "cs_ibl_forward") == 0
                               ? de__shader_view_find(&view, (u32)target, 0, e)
                               : -1;
            if (ri < 0)
                continue;
            const de__shader_record rec = de__shader_view_record(&view, (u32)ri);
            pipe = de_gpu_pipeline_create(
                f->dev,
                &(de_gpu_pipeline_desc){
                    .kind = DE_PIPE_COMPUTE,
                    .cs = {.code = view.base + rec.code_offset, .size = rec.code_size, .entry = n},
                    .push_constant_bytes = 48,
                    .threadgroup_size = {rec.threadgroup[0], rec.threadgroup[1],
                                         rec.threadgroup[2]},
                    .name = "test ibl forward"});
        }
    }
    if (!DE_HANDLE_VALID(pipe))
        snprintf(why, cap, "ibl_forward.slang did not cook or link: %.600s",
                 de_error_last() ? de_error_last()->message : "?");
    de_cook_result_free(&res);
    de_cooker_destroy(c);
    return pipe;
}

typedef struct fwd_pass {
    lx *f;
    de_gpu_pipeline pipe;
    de_rg_resource depth, gb[3], out, scene, strip;
    u32 view, entry, scene_index;
} fwd_pass;

static void fwd_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const fwd_pass *p = (const fwd_pass *)user;
    de_gpu_device *dev = p->f->dev;
    const u32 params[12] = {p->view,
                            p->entry,
                            p->scene_index,
                            0u,
                            de_gpu_texture_bindless(dev, de_rg_texture(pass, p->depth), 0, false),
                            de_gpu_texture_bindless(dev, de_rg_texture(pass, p->gb[0]), 0, false),
                            de_gpu_texture_bindless(dev, de_rg_texture(pass, p->gb[1]), 0, false),
                            de_gpu_texture_bindless(dev, de_rg_texture(pass, p->gb[2]), 0, false),
                            de_gpu_texture_bindless(dev, de_rg_texture(pass, p->out), 0, true),
                            p->f->width,
                            p->f->height,
                            0u};
    de_gpu_cmd_set_pipeline(cmd, p->pipe);
    de_gpu_cmd_push_constants(cmd, params, sizeof params);
    de_gpu_cmd_dispatch(cmd, (p->f->width + 7u) / 8u, (p->f->height + 7u) / 8u, 1);
}

/* The forward kernel over this frame's G-buffer into an RGBA32F texture (lx.extra_tex). */
static void forward_extra(lx *f, de__vis_context *vc, de__lighting_context *lc) {
    fwd_pass *p = (fwd_pass *)de_arena_push_zero(f->arena, sizeof *p, 16);
    ASSERT_TRUE(p != NULL);
    p->f = f;
    p->pipe = *(const de_gpu_pipeline *)f->user;
    p->depth = vc->depth;
    for (u32 t = 0; t < 3; ++t)
        p->gb[t] = vc->gbuffer[DE__GB0 + t];
    p->scene = vc->scene_views;
    p->strip = lc->strip;
    p->view = f->lt->views[0].constants_bindless;
    p->entry = f->lt->views[0].entry;
    p->scene_index = vc->scene_views_bindless;
    p->out = de_rg_create_texture(
        f->g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA32_FLOAT,
                                     .width = f->width,
                                     .height = f->height,
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                     .name = "test.ibl_forward"});
    de_rg_pass *pass = de_rg_add_pass(f->g, "test.ibl_forward", DE_QUEUE_GRAPHICS, fwd_execute, p);
    de_rg_read(pass, p->depth, DE_ACCESS_SHADER_READ);
    for (u32 t = 0; t < 3; ++t)
        de_rg_read(pass, p->gb[t], DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->scene, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(p->strip))
        de_rg_read(pass, p->strip, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, p->out, DE_ACCESS_SHADER_WRITE);
    f->extra_tex = p->out;
    f->extra_bpp = 16u;
}

#endif

/* ibl_deferred_forward_equal: the deferred pass's indirect light (a scene without lights) equals
 * evaluate_indirect called by a forward kernel on the same G-buffer texels. */
static void deferred_forward(lx *f, de_gpu_backend b) {
    lx_open(f, b);
#if defined(DE_IBL_TEST_COOK) && DE_IBL_TEST_COOK
    static char why[800];
    de_gpu_pipeline pipe = forward_pipeline(f, why, sizeof why);
    if (!DE_HANDLE_VALID(pipe))
        SKIP(why);
    f->user = &pipe;
    f->extra = forward_extra;
    const u32 sq = lx_square(f);
    world_set(&f->w, 0, sq, trs((de_vec3){-2.5f, 0, 0}, (de_vec3){0, 1, 0}, 0.4f, 1.0f),
              material_add(0.7f, 0.6f, 0.0f, 0.5f));
    world_set(&f->w, 1, sq, trs((de_vec3){0, 0, 0}, (de_vec3){1, 0, 0}, -0.3f, 1.0f),
              material_add(0.9f, 0.25f, 1.0f, 0.5f));
    world_set(&f->w, 2, sq, at(2.5f, 0, 0), material_add(0.3f, 0.05f, 0.4f, 1.0f));
    const f32 faces[6][3] = {{4.0f, 0.5f, 0.5f}, {0.5f, 4.0f, 0.5f}, {0.5f, 0.5f, 4.0f},
                             {3.0f, 3.0f, 0.5f}, {0.3f, 0.3f, 0.3f}, {0.1f, 0.1f, 0.1f}};
    env_cube(f, &faces[0][0]);
    f32 sh[9][3];
    memset(sh, 0, sizeof sh);
    for (u32 c = 0; c < 3; ++c) {
        sh[0][c] = (f32)(PI_D / SH_Y0) * (1.0f + 0.2f * (f32)c);
        sh[3][c] = 1.5f;
        sh[2][c] = 0.7f;
        sh[6][c] = 0.2f;
    }
    env_set(f, &sh[0][0], 1.5f, 0.7f);
    f->pre_exposure = 0.25f;
    lx_run(f);
    f->pre_exposure = 0.0f;
    const lx_result *r = &f->res;
    ASSERT_TRUE(r->extra != NULL);
    u32 checked = 0, bad = 0;
    f32 worst = 0.0f;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        surf s;
        if (!surf_at(r, i, &s))
            continue;
        ++checked;
        for (u32 c = 0; c < 3; ++c) {
            const f32 d = r->lighting[4 * i + c], fw = r->extra[4 * i + c];
            const f32 e = fabsf(d - fw) / (fabsf(fw) + 1e-3f);
            worst = e > worst ? e : worst;
            bad += !close_to(d, fw, 0.002f, 1e-3f); /* the deferred target is half float */
        }
    }
    printf("deferred vs forward: %u px, worst %.4f%%\n", checked, 100.0 * worst);
    EXPECT_GT(checked, 2000u);
    EXPECT_EQ(bad, 0u);
    f->extra = NULL;
    de_gpu_wait_idle(f->dev);
    de_gpu_pipeline_destroy(f->dev, pipe);
#else
    (void)f;
    (void)b;
    SKIP("this build has no shader importer (DE_BUILD_TOOLS off or no Slang)");
#endif
}

/* The output pass of the output test. */
typedef struct out_user {
    bool encode;
    de__output_basic *ob;
} out_user;

static void output_extra(lx *f, de__vis_context *vc, de__lighting_context *lc) {
    (void)vc;
    const out_user *u = (const out_user *)f->user;
    f->extra_tex =
        de__output_basic_add(u->ob, f->g, f->arena, lc->target, lc->width, lc->height, u->encode);
    f->extra_bpp = 8u;
    EXPECT_TRUE(DE_HANDLE_VALID(f->extra_tex));
}

static f32 srgb_oetf(f32 c) {
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* output_basic_srgb_encode: the output kernel clamps the pre-exposed lighting to [0, 1] and, when
 * asked, applies the sRGB OETF (checked against the C formula on surfaces of radiance 0.05 to 3);
 * without encoding it writes the clamped linear value (the sRGB swapchains encode). */
static void output_encode(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    de__output_basic *ob = NULL;
    ASSERT_EQ(de__output_basic_create_ex(f->vis, &ob), DE_OK);
    if (!de__output_basic_enabled(ob)) {
        de__output_basic_destroy(ob);
        SKIP("no post_output_basic code for this backend in this build");
    }
    out_user u = {.ob = ob};
    f->user = &u;
    f->extra = output_extra;
    const u32 sq = lx_square(f);
    const f32 base[4] = {0.05f, 0.3f, 0.9f, 1.0f};
    for (u32 k = 0; k < 4; ++k)
        world_set(&f->w, k, sq, at(-3.75f + 2.5f * (f32)k, 0, 0),
                  material_add(base[k], 1.0f, 0.0f, 0.0f));
    const u32 patch = lx_mesh_plane(f, 1, 1, 1.0f); /* an emitter of 3 nits: clamped */
    const u32 em = material_add(0.0f, 1.0f, 0.0f, 0.0f);
    g_mat[em - 1u].standard.emissive = (de_vec3){1.0f, 1.0f, 1.0f};
    g_mat[em - 1u].standard.emissive_strength = 3.0f;
    world_set(&f->w, 4, patch, at(0, 2.5f, 0.5f), em);
    ASSERT_TRUE(cvar_set("r.emissive_light_threshold", "0"));
    env_uniform(f, 1.0f);
    for (u32 pass = 0; pass < 2; ++pass) {
        u.encode = pass == 0;
        f->pre_exposure = 1.0f;
        lx_run(f);
        const lx_result *r = &f->res;
        ASSERT_TRUE(r->extra != NULL);
        u32 checked = 0, bad = 0, over = 0;
        for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
            const f32 a = r->lighting[4 * i + 3];
            for (u32 c = 0; c < 3; ++c) {
                f32 x = r->lighting[4 * i + c];
                over += a > 0.5f && x > 1.0f;
                x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
                const f32 want = u.encode ? srgb_oetf(x) : x;
                bad += fabsf(r->extra[4 * i + c] - want) > 1e-3f + 1e-3f * want;
            }
            ++checked;
        }
        printf("output (%s): %u px, %u channels over 1, %u bad\n", u.encode ? "sRGB" : "linear",
               checked, over, bad);
        EXPECT_GT(over, 100u);
        EXPECT_EQ(bad, 0u);
    }
    f->extra = NULL;
    de_gpu_wait_idle(f->dev);
    de__output_basic_destroy(ob);
}

/* ---------- The strip copy of a BC6H cube ---------- */

typedef struct blob_upload {
    de_rg_resource t;
    de_gpu_buffer src;
    de_cooked_texture_subresource sub[6][8];
    u32 mips;
} blob_upload;

static void blob_upload_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const blob_upload *u = (const blob_upload *)user;
    for (u32 l = 0; l < 6; ++l)
        for (u32 m = 0; m < u->mips; ++m)
            de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, u->t), m, l, u->src,
                                              u->sub[l][m].offset, u->sub[l][m].row_pitch);
}

typedef struct strip_read {
    de_rg_resource t;
    de_gpu_buffer dst;
    u64 off[8];
    u32 pitch[8], mips;
} strip_read;

static void strip_read_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const strip_read *r = (const strip_read *)user;
    for (u32 m = 0; m < r->mips; ++m)
        de_gpu_cmd_copy_texture_to_buffer(cmd, r->dst, r->off[m], r->pitch[m],
                                          de_rg_texture(pass, r->t), m, 0);
}

/* Runs one graph with pass `fn` (user `u`) writing `res` with `access`, exported `final`. */
static void one_pass(lx *f, const char *name, de_rg_execute_fn fn, void *u, de_gpu_texture t,
                     u32 before, u32 access, u32 final, de_rg_resource *res) {
    const u64 n = f->frames++;
    de__rg_reset(f->g);
    de_arena_reset(f->arena);
    ASSERT_EQ(de__rg_begin(f->g, f->arena), DE_OK);
    *res = de_rg_import_texture(f->g, t, before);
    de_rg_pass *p = de_rg_add_pass(f->g, name, DE_QUEUE_GRAPHICS, fn, u);
    if (access == DE_ACCESS_COPY_SRC) {
        de_rg_read(p, *res, access);
        de_rg_pass_side_effect(p);
    } else {
        de_rg_write(p, *res, access);
    }
    de_rg_export(f->g, *res, final);
    ASSERT_EQ(de__rg_finalize(f->g), DE_OK);
    ASSERT_EQ(de__rg_compile(f->g), DE_OK);
    ASSERT_EQ(de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = n,
                                                          .frame_fence = f->fence,
                                                          .frame_value = n + 1}),
              DE_OK);
    ASSERT_TRUE(lx_wait(f, n + 1, name));
}

/* ibl_strip_copy: a cooked BC6H environment (lt_env, 16-texel faces, 5 mips) is copied into the
 * strip block for block: the faces of every mip with at least one 4 x 4 block stacked in layer
 * order (3 mips), the rest left out (the shader clamps the LOD). */
static void strip_copy(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, material_add(1.0f, 1.0f, 0.0f, 0.0f));
    lt_blob env = lt_env(DE_FMT_BC6H_UFLOAT, 16, 9);
    ASSERT_TRUE(env.data != NULL);
    blob_upload *u = (blob_upload *)zalloc(sizeof *u);
    u->mips = 5;
    for (u32 l = 0; l < 6; ++l)
        for (u32 m = 0; m < 5; ++m)
            u->sub[l][m] = lt_dtex_sub(&env, l * 5u + m);
    f->cube = de_gpu_texture_create(
        f->dev,
        &(de_gpu_texture_desc){.dim = DE_TEX_CUBE,
                               .format = DE_FMT_BC6H_UFLOAT,
                               .width = 16,
                               .height = 16,
                               .depth_or_layers = 6,
                               .mip_count = 5,
                               .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                               .name = "test bc6h environment"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->cube));
    u->src = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = env.size,
                                                                .usage = DE_USAGE_COPY_SRC,
                                                                .memory = DE_MEM_UPLOAD,
                                                                .name = "test bc6h upload"});
    ASSERT_TRUE(DE_HANDLE_VALID(u->src));
    memcpy(de_gpu_buffer_map(f->dev, u->src), env.data, (size_t)env.size);
    one_pass(f, "test.bc6h_upload", blob_upload_execute, u, f->cube, DE_ACCESS_NONE,
             DE_ACCESS_COPY_DST, DE_ACCESS_SHADER_READ, &u->t);
    de__ibl_environment e;
    memset(&e, 0, sizeof e);
    e.valid = true;
    e.cube = f->cube;
    e.format = DE_FMT_BC6H_UFLOAT;
    e.face_size = 16;
    e.mip_count = 5;
    e.mip_per_roughness = 4.0f;
    e.intensity = 1.0f;
    de__lighting_set_environment(f->lt, &e);
    lx_run(f); /* the first frame copies the strip */
    ASSERT_TRUE(DE_HANDLE_VALID(f->lt->strip));
    EXPECT_EQ(f->lt->strip_mips, 3u);
    strip_read *rd = (strip_read *)zalloc(sizeof *rd);
    rd->mips = 3;
    u64 at = 0;
    for (u32 m = 0; m < 3; ++m) {
        rd->pitch[m] = 256u;
        rd->off[m] = at;
        at = align_up(at + (u64)rd->pitch[m] * 6u * ((16u >> m) / 4u), 512u);
    }
    rd->dst = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = at,
                                                                 .usage = DE_USAGE_COPY_DST,
                                                                 .memory = DE_MEM_READBACK,
                                                                 .name = "test strip readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(rd->dst));
    one_pass(f, "test.strip_read", strip_read_execute, rd, f->lt->strip, DE_ACCESS_SHADER_READ,
             DE_ACCESS_COPY_SRC, DE_ACCESS_SHADER_READ, &rd->t);
    const u8 *m8 = (const u8 *)de_gpu_buffer_map(f->dev, rd->dst);
    u32 bad = 0, blocks = 0;
    for (u32 m = 0; m < 3; ++m) {
        const u32 rows = (16u >> m) / 4u,
                  row_bytes = rows * 16u; /* square: blocks per row = rows */
        for (u32 l = 0; l < 6; ++l)
            for (u32 r = 0; r < rows; ++r) {
                const u8 *got = m8 + rd->off[m] + (u64)(l * rows + r) * rd->pitch[m];
                const u8 *want = env.data + u->sub[l][m].offset + (u64)r * u->sub[l][m].row_pitch;
                bad += memcmp(got, want, row_bytes) != 0;
                blocks += rows;
            }
    }
    printf("strip copy: %u blocks of 3 mips x 6 faces, %u rows differ\n", blocks, bad);
    EXPECT_EQ(bad, 0u);
    de_gpu_wait_idle(f->dev);
    de_gpu_buffer_destroy(f->dev, rd->dst);
    de_gpu_buffer_destroy(f->dev, u->src);
    free(rd);
    free(u);
    lt_free(&env);
}

#define GPU_TESTS(sfx, backend)                                                                    \
    TEST_F(ibl, ibl_white_furnace_diffuse_##sfx, lx) {                                             \
        furnace_diffuse(f, backend);                                                               \
    }                                                                                              \
    TEST_F(ibl, ibl_white_furnace_metal_##sfx, lx) {                                               \
        furnace_metal(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(ibl, ibl_rotation_##sfx, lx) {                                                          \
        rotation(f, backend);                                                                      \
    }                                                                                              \
    TEST_F(ibl, ibl_no_environment_fallback_##sfx, lx) {                                           \
        no_environment(f, backend);                                                                \
    }                                                                                              \
    TEST_F(ibl, exposure_ev100_scale_##sfx, lx) {                                                  \
        exposure_scale(f, backend);                                                                \
    }                                                                                              \
    TEST_F(ibl, ibl_deferred_forward_equal_##sfx, lx) {                                            \
        deferred_forward(f, backend);                                                              \
    }                                                                                              \
    TEST_F(ibl, output_basic_srgb_encode_##sfx, lx) {                                              \
        output_encode(f, backend);                                                                 \
    }                                                                                              \
    TEST_F(ibl, ibl_strip_copy_bc6h_##sfx, lx) {                                                   \
        strip_copy(f, backend);                                                                    \
    }

GPU_TESTS(metal, DE_GPU_METAL)
GPU_TESTS(vulkan, DE_GPU_VULKAN)
GPU_TESTS(d3d12, DE_GPU_D3D12)

/* ================================================================================================
 * CPU tests
 * ============================================================================================== */

/* exposure_ev100: 1 / (1.2 x 2^EV100); EV100 0 and 10 differ by 2^10. */
TEST(exposure, ev100_formula) {
    EXPECT_TRUE(fabsf(de__exposure_from_ev100(0.0f) - 1.0f / 1.2f) < 1e-7f);
    EXPECT_TRUE(fabsf(de__exposure_from_ev100(0.0f) / de__exposure_from_ev100(10.0f) - 1024.0f) <
                1e-2f);
    EXPECT_TRUE(fabsf(de__exposure_from_ev100(15.0f) * 1.2f * 32768.0f - 1.0f) < 1e-5f);
}

/* exposure_nan_fallback: a NaN EV100 (automatic) takes the fallback, r.exposure_ev100_default
 * through the lighting state; the previous frame's exposure follows; a frame asks once. */
TEST(exposure, nan_fallback) {
    de__exposure_state s;
    memset(&s, 0, sizeof s);
    const de_view v = {.id = 1};
    f32 prev = 0.0f;
    f32 e = de__exposure_update(&s, v, 5, (f32)NAN, 7.0f, &prev);
    EXPECT_TRUE(fabsf(e - de__exposure_from_ev100(7.0f)) < 1e-9f);
    EXPECT_TRUE(prev == e); /* the view's first frame */
    e = de__exposure_update(&s, v, 6, 3.0f, 7.0f, &prev);
    EXPECT_TRUE(fabsf(e - de__exposure_from_ev100(3.0f)) < 1e-9f);
    EXPECT_TRUE(fabsf(prev - de__exposure_from_ev100(7.0f)) < 1e-9f);
    /* The same frame again: the same values, whatever the camera says now. */
    EXPECT_TRUE(de__exposure_update(&s, v, 6, 9.0f, 7.0f, &prev) == e);
    /* A new view in the slot starts over. */
    e = de__exposure_update(&s, (de_view){.id = 2}, 7, (f32)NAN, 2.0f, &prev);
    EXPECT_TRUE(fabsf(e - de__exposure_from_ev100(2.0f)) < 1e-9f && prev == e);

    /* Through the lighting state and its cvar (the null backend). */
    de_gpu_device *d = NULL;
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &d), DE_OK);
    de__lighting *lt = NULL;
    ASSERT_EQ(de__lighting_create_ex(&(de__lighting_desc){.device = d}, &lt), DE_OK);
    ASSERT_TRUE(cvar_set("r.exposure_ev100_default", "7"));
    e = de__lighting_view_exposure(lt, 0, v, 1, (f32)NAN, &prev);
    EXPECT_TRUE(fabsf(e - de__exposure_from_ev100(7.0f)) < 1e-9f);
    e = de__lighting_view_exposure(lt, 0, v, 2, 12.0f, &prev);
    EXPECT_TRUE(fabsf(e - de__exposure_from_ev100(12.0f)) < 1e-12f);
    EXPECT_TRUE(fabsf(prev - de__exposure_from_ev100(7.0f)) < 1e-9f);
    (void)cvar_set("r.exposure_ev100_default", "15");
    de__lighting_destroy(lt);
    de_gpu_device_destroy(d);
}

/* ibl_pack_irradiance: the packed SH9 times intensity, its C irradiance (a uniform environment
 * gives pi L; the rotation turns a linear term about +Z), the ambient without an environment, and
 * the strip mips (all for RGBA16F, faces of at least 4 texels for BC6H). */
TEST(ibl, pack_irradiance) {
    de__ibl_environment e;
    memset(&e, 0, sizeof e);
    e.valid = true;
    e.face_size = 256;
    e.mip_count = 9;
    e.mip_per_roughness = 8.0f;
    e.intensity = 2.0f;
    for (u32 c = 0; c < 3; ++c) {
        e.sh9[0][c] = (f32)(PI_D / SH_Y0);
        e.sh9[3][c] = (f32)(0.5 * PI_D / SH_Y1);
    }
    de__ibl_gpu g;
    de__ibl_pack(&e, 7, 9, 3, 0.25f, &g);
    EXPECT_EQ(g.flags, (u32)DE__IBL_ENVIRONMENT);
    EXPECT_EQ(g.strip, 7u);
    EXPECT_TRUE(g.sh[0][3] == 2.0f && g.rotation[3] == 8.0f && g.rotation[2] == 8.0f);
    f32 out[3];
    de__ibl_irradiance(&g, (const f32[3]){0, 0, 1}, out);
    EXPECT_TRUE(fabsf(out[0] - 2.0f * (f32)PI_D) < 1e-4f);
    de__ibl_irradiance(&g, (const f32[3]){1, 0, 0}, out);
    EXPECT_TRUE(fabsf(out[0] - 2.0f * (f32)PI_D * 1.5f) < 1e-4f);
    e.rotation = (f32)(PI_D * 0.5); /* the environment's +X now lies along +Y */
    de__ibl_pack(&e, 7, 9, 3, 0.25f, &g);
    de__ibl_irradiance(&g, (const f32[3]){0, 1, 0}, out);
    EXPECT_TRUE(fabsf(out[0] - 2.0f * (f32)PI_D * 1.5f) < 1e-3f);
    de__ibl_irradiance(&g, (const f32[3]){1, 0, 0}, out);
    EXPECT_TRUE(fabsf(out[0] - 2.0f * (f32)PI_D) < 1e-3f);
    de__ibl_pack(NULL, 7, 9, 3, 0.25f, &g);
    EXPECT_EQ(g.flags, 0u);
    de__ibl_irradiance(&g, (const f32[3]){0, 0, 1}, out);
    EXPECT_TRUE(fabsf(out[1] - 0.25f * (f32)PI_D) < 1e-5f);
    EXPECT_EQ(de__ibl_strip_mips(DE_FMT_RGBA16_FLOAT, 256, 9), 9u);
    EXPECT_EQ(de__ibl_strip_mips(DE_FMT_BC6H_UFLOAT, 256, 9), 7u);
    EXPECT_EQ(de__ibl_strip_mips(DE_FMT_BC6H_UFLOAT, 2, 2), 0u);
}

/* The `public static const uint` values of ibl.slang against the C table. */
TEST(ibl, layout_match) {
    char path[1024];
    snprintf(path, sizeof path, "%s/lighting/ibl.slang", DE_IBL_TEST_SHADER_DIR);
    FILE *fp = fopen(path, "rb");
    ASSERT_TRUE(fp != NULL);
    char text[16384];
    const size_t n = fread(text, 1, sizeof text - 1u, fp);
    fclose(fp);
    text[n] = '\0';
    u32 count = 0, found = 0;
    const de__lighting_const *k = de__lighting_layout_consts(&count);
    for (const char *p = text; (p = strstr(p, "public static const uint ")) != NULL;) {
        p += strlen("public static const uint ");
        char name[96];
        long long value = 0;
        ASSERT_EQ(sscanf(p, "%95[A-Za-z0-9_] = %lli", name, &value), 2);
        bool ok = false;
        for (u32 i = 0; i < count; ++i)
            if (strcmp(k[i].name, name) == 0) {
                ok = true;
                EXPECT_EQ((u32)value, k[i].value);
            }
        EXPECT_TRUE(ok);
        ++found;
    }
    EXPECT_EQ(found, 2u);
}
