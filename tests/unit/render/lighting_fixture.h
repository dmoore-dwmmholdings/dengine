/*
 * lighting_fixture.h: the GPU fixture of the lighting tests (DENG-303; shared since DENG-306): a
 * fake render world drives a GPU scene of planes (meshlet patches facing +z, the camera looking
 * down) and lights; lx_frame runs the whole vis frame (with the resolve's debug position target),
 * then de__lighting_build, then (when lx.sss is set, DENG-306) de__sss_build, and a test pass reads
 * back the lighting target, the G-buffer, the split targets and the cluster lists. Header-only
 * (static inline) for test_clustered_lights.c and test_sss.c; include it after their includes.
 */
#ifndef DE_TEST_RENDER_LIGHTING_FIXTURE_H
#define DE_TEST_RENDER_LIGHTING_FIXTURE_H

#include "render/passes/sss/sss.h"
#include "render/shadows/shadows_internal.h"
#include "render/passes/forward/forward.h"
#if DE_HAS_GPU_VULKAN
#include "gpu/vulkan/vk_internal.h"
#endif
#define WAIT_NS             (10ull * 1000000000ull)
#define FIRST_FRAME_WAIT_NS (120ull * 1000000000ull) /* lavapipe JIT (DENG-428) */
#define PI_D                3.14159265358979323846

static inline void *zalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    ASSERT_TRUE(p != NULL);
    return p;
}

static inline void put_u32(u8 *p, u32 v) {
    memcpy(p, &v, 4);
}
static inline void put_f32(u8 *p, f32 v) {
    memcpy(p, &v, 4);
}

static inline f32 half_to_f32(u16 h) {
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

typedef struct lcg {
    u64 s;
} lcg;
static inline f32 lcg_f(lcg *g) {
    g->s = g->s * 6364136223846793005ull + 1442695040888963407ull;
    return (f32)((g->s >> 40) & 0xFFFFFFu) / 16777216.0f;
}
static inline f32 lcg_range(lcg *g, f32 lo, f32 hi) {
    return lo + (hi - lo) * lcg_f(g);
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

static inline const de_render_mesh *fake_mesh(void *user, de_mesh m) {
    (void)user;
    return m.id >= 1u && m.id <= g_mesh_count ? &g_mesh[m.id - 1u].rm : NULL;
}
static inline const de_render_material *fake_material(void *user, de_material m) {
    (void)user;
    return m.id >= 1u && m.id <= g_mat_count ? &g_mat[m.id - 1u] : NULL;
}
static inline u32 fake_texture(void *user, de_texture t) {
    (void)user;
    (void)t;
    return 0u;
}

static inline de_gpu_buffer upload_buf(de_gpu_device *dev, const void *data, u64 bytes) {
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
static inline void mesh_plane(fmesh *fm, de_gpu_device *dev, u32 cols, u32 rows, f32 size) {
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

static inline void mesh_free(fmesh *fm, de_gpu_device *dev) {
    for (u32 i = 0; i < 4; ++i)
        if (dev != NULL && DE_HANDLE_VALID(fm->buf[i]))
            de_gpu_buffer_destroy(dev, fm->buf[i]);
    memset(fm, 0, sizeof *fm);
}

/* Material (1-based id): grey `base`, roughness, metallic, specular. */
static inline u32 material_add(f32 base, f32 roughness, f32 metallic, f32 specular) {
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

static inline void world_init(world *w, u32 cap) {
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

static inline void world_free(world *w) {
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

static inline de_affine trs(de_vec3 t, de_vec3 axis, f32 angle, f32 s) {
    return de_affine_from_trs(t, de_quat_axis_angle(axis, angle), (de_vec3){s, s, s});
}
static inline de_affine at(f32 x, f32 y, f32 z) {
    return trs((de_vec3){x, y, z}, (de_vec3){0, 0, 1}, 0.0f, 1.0f);
}

static inline void world_set(world *w, u32 s, u32 mesh, de_affine xf, u32 mat) {
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

static inline void world_light(world *w, u32 s, const DeLight *l, de_affine xf) {
    ASSERT_TRUE(s < MAX_LIGHTS);
    de__rw_lights *li = &w->f.lights;
    li->entity[s] = 5000u + s;
    li->transform[s] = xf;
    li->light[s] = *l;
    if (s >= li->count)
        li->count = s + 1u;
    w->touch_lights = true;
}

static inline void world_begin(world *w) {
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
} lx_result;

typedef struct lx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_arena *arena;
    de_gpu_device *dev;
    de_gpu_backend backend;
    bool real_gpu, warm, sync_diagnostics;
    de_rg *g;
    de__gpu_scene *gs;
    de__vis_shared *vis;
    de__material_models *models;
    de__lighting *lt;
    de__brdf_luts *luts; /* the Standard model's energy LUT (DENG-299) */
    /* DENG-306: subsurface scattering after lighting (NULL: none). The test makes it with
     * de__sss_create_ex; lx_frame then asks lighting for the subsurface split and builds the SSS
     * passes at sss_quality, and keeps the frame's context in sss_frame. */
    de__csm *csm;
    de__forward *forward;
    de_quality shadow_quality;
    de__sss *sss;
    de_quality sss_quality;
    de__sss_context *sss_frame;
    de_gpu_fence fence;
    u64 frames;
    de_gpu_buffer rb;
    u64 rb_size;
    world w;
    de_vec3 eye, target;
    f32 fov;
    f32 ortho_height;
    u32 width, height;
    lx_result res;
} lx;

static inline void lx_setup(lx *f) {
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

static inline u32 validation_errors(de_gpu_device *d) {
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

static inline bool cvar_set(const char *name, const char *value) {
    const de_cvar cv = de_cvar_find(name);
    if (!DE_HANDLE_VALID(cv))
        return false;
    EXPECT_EQ(de_cvar_set(cv, value), DE_OK);
    return true;
}

static inline void free_result(lx_result *r) {
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
    memset(r, 0, sizeof *r);
}

static inline void lx_sync_dump(lx *f, const char *stage, bool query_completed) {
#if DE_HAS_GPU_VULKAN
    if (f->sync_diagnostics)
        de__gpu_vk_sync_diagnostics_dump(f->dev, f->fence, stage, query_completed);
#else
    (void)f;
    (void)stage;
    (void)query_completed;
#endif
}

static inline void lx_diag_stage(lx *f, const char *stage, u64 *previous) {
    if (f->sync_diagnostics) {
        const u64 now = de_time_now_ns();
        fprintf(stderr, "lx-diag t_ns=%llu stage=%s frame=%llu elapsed_ns=%llu\n",
                (unsigned long long)now, stage, (unsigned long long)f->frames,
                (unsigned long long)(now - *previous));
        *previous = now;
    }
}

static inline void lx_teardown(lx *f) {
    lx_sync_dump(f, "teardown-begin", true);
    de__forward_destroy(f->forward);
    f->forward = NULL;
    de__csm_destroy(f->csm);
    f->csm = NULL;
    if (f->g) {
        de__rg_reset(f->g);
        de__rg_destroy(f->g);
    }
    if (f->dev)
        de_gpu_wait_idle(f->dev);
    lx_sync_dump(f, "teardown-after-existing-idle", true);
    de__sss_destroy(f->sss); /* DENG-306 */
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
    const char *const names[] = {"r.lod_bias",         "r.occlusion",
                                 "r.rg_record_cost",   "r.cluster_max_lights",
                                 "r.split_lighting",   "r.emissive_light_threshold",
                                 "r.light_cutoff_lux", "r.cluster_tile",
                                 "r.cluster_slices",   "r.cluster_far"};
    const char *const values[] = {"0", "1", "32", "256", "0", "1000", "0.05", "64", "32", "500"};
    for (u32 i = 0; i < sizeof names / sizeof names[0]; ++i) {
        const de_cvar cv = de_cvar_find(names[i]);
        if (DE_HANDLE_VALID(cv))
            (void)de_cvar_set(cv, values[i]);
    }
}

static inline bool backend_built(de_gpu_backend b) {
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

static inline void lx_open(lx *f, de_gpu_backend backend) {
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
#if DE_HAS_GPU_VULKAN
    const char *diagnostics = getenv("DE_VK_SYNC_DIAGNOSTICS");
    if (backend == DE_GPU_VULKAN && diagnostics && strcmp(diagnostics, "1") == 0) {
        f->sync_diagnostics = true;
        de__gpu_vk_sync_diagnostics_enable(f->dev);
        fprintf(stderr, "lx-diag enabled: validation unchanged, engine pipeline_cache_dir empty\n");
    }
#endif
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

static inline u32 lx_mesh_plane(lx *f, u32 cols, u32 rows, f32 size) {
    ASSERT_TRUE(g_mesh_count < MAX_MESHES);
    mesh_plane(&g_mesh[g_mesh_count], f->dev, cols, rows, size);
    return ++g_mesh_count;
}

/* The receiver every GPU test lights: a 12 m square plane at z = 0 in slot 0. */
static inline void lx_floor(lx *f, u32 mat) {
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

static inline void rb_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
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

static inline void camera(const lx *f, de_mat4 *view, de_mat4 *proj) {
    *view = de_m4_look_at(f->eye, f->target, (de_vec3){0.0f, 1.0f, 0.0f});
    if (f->ortho_height > 0) {
        f32 h = .5f * f->ortho_height, w = h * (f32)f->width / (f32)f->height;
        *proj = de_m4_ortho_rz(-w, w, -h, h, .1f, 100000);
    } else
        *proj = de_m4_perspective_rz(f->fov, (f32)f->width / (f32)f->height, 0.1f);
}

static inline u64 align_up(u64 v, u64 a) {
    return (v + a - 1u) / a * a;
}

static inline void *rows_of(const u8 *m, const rb_item *it, u32 w, u32 h, u32 bpp) {
    u8 *out = (u8 *)zalloc((size_t)w * h * bpp);
    for (u32 y = 0; y < h; ++y)
        memcpy(out + (size_t)y * w * bpp, m + it->off + (u64)y * it->pitch, (size_t)w * bpp);
    return out;
}

static inline f32 *halves_of(const u8 *m, const rb_item *it, u32 w, u32 h) {
    u16 *raw = (u16 *)rows_of(m, it, w, h, 8);
    f32 *out = (f32 *)zalloc((size_t)w * h * 4u * sizeof(f32));
    for (size_t i = 0; i < (size_t)w * h * 4u; ++i)
        out[i] = half_to_f32(raw[i]);
    free(raw);
    return out;
}

static inline bool lx_wait(lx *f, u64 v, const char *what) {
    const u64 begin = f->sync_diagnostics ? de_time_now_ns() : 0;
    lx_sync_dump(f, "wait-begin", false);
    const u64 t0 = de_time_now_ns();
    const de_result r =
        de_gpu_fence_wait(f->dev, f->fence, v, f->warm ? WAIT_NS : FIRST_FRAME_WAIT_NS);
    const u64 wait_end = de_time_now_ns();
    const f64 ms = (f64)(wait_end - t0) / 1e6;
    lx_sync_dump(f, r == DE_OK ? "wait-complete" : "wait-failed", true);
    if (f->sync_diagnostics)
        fprintf(stderr,
                "lx-diag t_ns=%llu wait-value=%llu pre_snapshot_ns=%llu api_wait_ns=%llu "
                "post_snapshot_ns=%llu wall_ns=%llu\n",
                (unsigned long long)de_time_now_ns(), (unsigned long long)v,
                (unsigned long long)(t0 - begin), (unsigned long long)(wait_end - t0),
                (unsigned long long)(de_time_now_ns() - wait_end),
                (unsigned long long)(de_time_now_ns() - begin));
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
static inline bool lx_frame(lx *f) {
    u64 diag_mark = f->sync_diagnostics ? de_time_now_ns() : 0;
    const u64 frame_begin = diag_mark;
    lx_diag_stage(f, "frame-begin", &diag_mark);
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
    EXPECT_TRUE(de__vis_frame_context(f->vis, d.view, f->g, n) == vc);
    if (f->csm) {
        de__rw_view cam = {.offset = f->eye,
                           .camera = {.near_z = .1f,
                                      .projection = f->ortho_height > 0 ? DE_PROJ_ORTHOGRAPHIC
                                                                        : DE_PROJ_PERSPECTIVE}};
        de_render_settings settings = {.shadows = f->shadow_quality};
        de__view_frame vf = {.view = d.view,
                             .graph = f->g,
                             .world = &f->w.f,
                             .camera = &cam,
                             .settings = &settings,
                             .frame_index = n,
                             .width = f->width,
                             .height = f->height,
                             .view_matrix = view,
                             .proj = proj};
        if (de__csm_build(f->csm, &vf, vc) != DE_OK) {
            de__csm_complete(f->csm, d.view, f->g, false, false);
            return false;
        }
        de__csm_publish(f->csm, d.view, f->g, n, de__lighting_view_inputs(f->lt, 0));
    }
    de__lighting_context *lc = NULL;
    if (f->sss != NULL) /* DENG-306: SUBSURFACE pixels split their diffuse off for the SSS pass */
        de__lighting_view_inputs(f->lt, 0)->subsurface = f->sss_quality != DE_QUALITY_OFF;
    if (de__lighting_build(f->lt, f->g, vc, &lc) != DE_OK)
        return false;
    f->sss_frame = NULL;
    if (f->sss != NULL && lc != NULL &&
        de__sss_build(f->sss, f->g, vc, lc, f->sss_quality, &f->sss_frame) != DE_OK)
        return false;
    if (f->forward && lc) {
        de__forward_context *forward_context = NULL;
        if (de__forward_build(f->forward, f->g, vc, lc, &f->w.f, &forward_context) != DE_OK)
            return false;
    }
    rb_pass *rp = NULL;
    if (lc != NULL) {
        rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
        ASSERT_TRUE(rp != NULL);
        rp->f = f;
        const de_rg_resource tex[7] = {vc->resolve_debug_pos, vc->gbuffer[DE__GB_LIGHTING],
                                       vc->gbuffer[DE__GB0],  vc->gbuffer[DE__GB1],
                                       vc->gbuffer[DE__GB2],  lc->diffuse,
                                       lc->specular};
        static const u32 bpp[7] = {16, 8, 4, 4, 4, 8, 8};
        u64 off = 0;
        for (u32 i = 0; i < 7; ++i) {
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
        const de_rg_resource storage[8] = {
            vc->gbuffer[DE__GB_MOTION],
            vc->gbuffer[DE__GB_LIGHTING],
            vc->resolve_debug_pos,
            vc->resolve_debug_uv,
            lc->diffuse,
            lc->specular,
            f->sss_frame != NULL ? f->sss_frame->temp : (de_rg_resource){0},
            f->sss_frame != NULL ? f->sss_frame->params : (de_rg_resource){0}};
        for (u32 i = 0; i < 8; ++i)
            if (DE_HANDLE_VALID(storage[i]))
                de_rg_write(p, storage[i], DE_ACCESS_SHADER_WRITE);
    }
    lx_diag_stage(f, "build-end", &diag_mark);
    de_result r = de__rg_finalize(f->g);
    lx_diag_stage(f, "finalize-end", &diag_mark);
    if (r == DE_OK)
        r = de__rg_compile(f->g);
    lx_diag_stage(f, "compile-end", &diag_mark);
    bool csm_attempted = r == DE_OK;
    if (r == DE_OK)
        r = de__rg_execute(f->g, &(de__rg_execute_desc){.frame_index = n,
                                                        .frame_fence = f->fence,
                                                        .frame_value = n + 1});
    lx_diag_stage(f, "record-submit-end", &diag_mark);
    lx_sync_dump(f, "record-submit-end", false);
    de__csm_complete(f->csm, d.view, f->g, csm_attempted, r == DE_OK);
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
    lx_diag_stage(f, "pre-wait-end", &diag_mark);
    const bool waited = lx_wait(f, n + 1, "frame");
    lx_diag_stage(f, "wait-return", &diag_mark);
    if (f->sync_diagnostics)
        fprintf(stderr, "lx-diag frame=%llu frame_to_wait_return_ns=%llu\n", (unsigned long long)n,
                (unsigned long long)(de_time_now_ns() - frame_begin));
    if (!waited)
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
static inline void lx_run(lx *f) {
    ASSERT_TRUE(lx_frame(f));
    ASSERT_TRUE(lx_frame(f));
    ASSERT_TRUE(f->res.valid);
}

#endif /* DE_TEST_RENDER_LIGHTING_FIXTURE_H */
