/*
 * test_raster_alpha.c: the alpha-tested vis-buffer raster (DENG-236): meshlets of mask materials
 * routed to their own bucket of the visible list, drawn with an alpha test on both raster paths,
 * after each phase's opaque raster (so phase 1's HZB holds the cutout occluders).
 * Executable test_render_raster_alpha; CTest names render_raster_alpha.<suite>.<test>.
 *
 * Meshes are flat 5 x 5 grid patches (one meshlet, 32 triangles) with F32X3 positions and an F32X2
 * UV0 stream spanning [0, uv_scale]. Materials come from the instances' material overrides (the
 * fake render world): S14 resolves no mesh-default materials yet, so a mask material only reaches
 * the GPU scene that way. The mask material samples an 8 x 8 checker whose alpha alternates 255 /
 * 0 (cutoff 0.5) through its base-colour slot.
 *
 * The reference is a CPU rasterizer (vertices snapped to 1/256 pixel, pixel centres, top-left
 * rule, reverse-Z GREATER_EQUAL) that, for a mask material, interpolates UV0 perspective-correctly
 * at the pixel centre and samples the checker bilinearly at mip 0 with wrap (the pass's sampler):
 * the quads are magnified, so the hardware samples mip 0 too (minified cutouts may differ: the
 * hardware's mip choice is not modelled). A pixel may differ only where it is a tie: a centre on
 * an edge, two surfaces at nearly the same depth, or an alpha within 1/32 of the cutoff.
 *
 * GPU tests run per backend (_metal, _vulkan, _d3d12) and SKIP without the device, below the Base
 * tier or (mesh path) without mesh shaders. CI: Vulkan on lavapipe and D3D12 on the vendored WARP.
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

#define WAIT_NS (10ull * 1000000000ull)

#define GRID        5u
#define PATCH_VERTS (GRID * GRID)
#define PATCH_TRIS  (2u * (GRID - 1u) * (GRID - 1u))
#define MAX_MESHES  4u
#define MAT_OPAQUE  1u /* fake material handle ids */
#define MAT_MASK    2u
#define TEX_CHECKER 1u
#define CHECK_SIZE  8u
#define CUTOFF      0.5f
#define NO_KEY      UINT64_MAX

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
    f32 uv[PATCH_VERTS * 2];
    u8 tri[PATCH_TRIS * 3];
    de_aabb bounds;
    de_gpu_buffer buf[4];
} fmesh;

static fmesh g_mesh[MAX_MESHES];
static u32 g_mesh_count;
static de_render_material g_mat[2];
static u32 g_checker_bindless;
static u8 g_checker_alpha[CHECK_SIZE * CHECK_SIZE];

static const de_render_mesh *fake_mesh(void *user, de_mesh m) {
    (void)user;
    return m.id >= 1u && m.id <= g_mesh_count ? &g_mesh[m.id - 1u].rm : NULL;
}
static const de_render_material *fake_material(void *user, de_material m) {
    (void)user;
    return m.id == MAT_OPAQUE || m.id == MAT_MASK ? &g_mat[m.id - 1u] : NULL;
}
static u32 fake_texture(void *user, de_texture t) {
    (void)user;
    return t.id == TEX_CHECKER ? g_checker_bindless : 0u;
}

static void put_u32(u8 *p, u32 v) {
    memcpy(p, &v, 4);
}
static void put_f32(u8 *p, f32 v) {
    memcpy(p, &v, 4);
}

/* A flat square patch of edge `size` centred at the origin, facing `normal` (its triangles wind
 * counter-clockwise seen from there), UV0 over [0, uv_scale]^2. */
static void mesh_quad(fmesh *fm, de_vec3 normal, f32 size, f32 uv_scale, de_gpu_device *dev) {
    memset(fm, 0, sizeof *fm);
    const de_vec3 n = normal;
    const de_vec3 a = fabsf(n.z) < 0.9f ? (de_vec3){0, 0, 1} : (de_vec3){1, 0, 0};
    de_vec3 t = de_v3_cross(a, n);
    t = de_v3_scale(t, 1.0f / de_v3_length(t));
    const de_vec3 b = de_v3_cross(n, t);
    fm->bounds = de_aabb_empty();
    for (u32 j = 0; j < GRID; ++j)
        for (u32 i = 0; i < GRID; ++i) {
            const u32 v = j * GRID + i;
            const f32 x = ((f32)i / (f32)(GRID - 1u) - 0.5f) * size;
            const f32 y = ((f32)j / (f32)(GRID - 1u) - 0.5f) * size;
            const de_vec3 p = {x * t.x + y * b.x, x * t.y + y * b.y, x * t.z + y * b.z};
            fm->pos[3 * v] = p.x;
            fm->pos[3 * v + 1] = p.y;
            fm->pos[3 * v + 2] = p.z;
            fm->uv[2 * v] = (f32)i / (f32)(GRID - 1u) * uv_scale;
            fm->uv[2 * v + 1] = (f32)j / (f32)(GRID - 1u) * uv_scale;
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
    /* The vertex buffer: positions at 0, UV0 at the next 256-byte boundary. */
    const u32 uv_off = 256u * ((PATCH_VERTS * 12u + 255u) / 256u);
    u8 vbytes[1024];
    memset(vbytes, 0, sizeof vbytes);
    for (u32 v = 0; v < PATCH_VERTS; ++v) {
        for (u32 c = 0; c < 3; ++c)
            put_f32(vbytes + 12u * v + 4u * c, fm->pos[3 * v + c]);
        put_f32(vbytes + uv_off + 8u * v, fm->uv[2 * v]);
        put_f32(vbytes + uv_off + 8u * v + 4u, fm->uv[2 * v + 1]);
    }
    /* The meshlet record: a bounding sphere, no cone. */
    u8 record[DE_COOKED_MESHLET_SIZE];
    memset(record, 0, sizeof record);
    put_u32(record + 8, PATCH_VERTS | (PATCH_TRIS << 8));
    put_f32(record + 28, size); /* centre 0, radius covers the patch */
    put_f32(record + 44, 1.0f); /* cutoff 1: no cone */
    u32 mlv[PATCH_VERTS];
    for (u32 v = 0; v < PATCH_VERTS; ++v)
        mlv[v] = v;
    const void *data[4] = {vbytes, record, mlv, fm->tri};
    const u64 bytes[4] = {uv_off + PATCH_VERTS * 8u, sizeof record, sizeof mlv, sizeof fm->tri};
    const u32 which[4] = {DE_RENDER_MESH_VERTICES, DE_RENDER_MESH_MESHLETS,
                          DE_RENDER_MESH_MESHLET_VERTICES, DE_RENDER_MESH_MESHLET_TRIANGLES};
    de_render_mesh *m = &fm->rm;
    for (u32 i = 0; i < 4; ++i) {
        fm->buf[i] = de_gpu_buffer_create(dev, &(de_gpu_buffer_desc){.size = bytes[i],
                                                                     .usage = DE_USAGE_STORAGE,
                                                                     .memory = DE_MEM_UPLOAD,
                                                                     .name = "alpha test mesh"});
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
    m->stream_offset[DE_COOKED_STREAM_UV0] = uv_off;
    m->stream_format[DE_COOKED_STREAM_UV0] = DE_COOKED_VF_F32X2;
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
        m->uv_scale[0][c < 2 ? c : 1] = 1.0f;
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

/* Slot `s`: mesh `mesh` (1-based) at `pos` (scaled by `scale`), material override `mat`. */
static void world_set(world *w, u32 s, u32 mesh, de_vec3 pos, f32 scale, u32 mat) {
    de__rw_instances *in = &w->f.instances;
    const de_affine xf =
        de_affine_from_trs(pos, (de_quat){0, 0, 0, 1}, (de_vec3){scale, scale, scale});
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
    if (w->f.frame > 0) /* after the first frame: an update of this slot */
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

typedef struct ra_result {
    u32 *ids;
    f32 *depth;
    de__vis_meshlet *visible; /* the whole list: opaque region, then the alpha region */
    u32 list_entries, visible_capacity, alpha_capacity;
    u32 counters[DE__VIS_MESHLET_COUNTER_BYTES / 4u];
    de__vis_raster_args args[DE__VIS_PHASES], alpha_args[DE__VIS_PHASES];
    de__gs_instance inst[16];
    u32 inst_count;
    de__gs_material mat[4];
    u32 mat_count;
    u32 raster_path;
    de_mat4 view_proj;
} ra_result;

typedef struct rax {
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
    de_gpu_texture checker;
    de_gpu_buffer checker_up;
    world w;
    bool dev_null;
    de_vec3 eye, target;
    f32 fov;
    u32 width, height;
    ra_result res;
} rax;

static void rax_setup(rax *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "raster_alpha_test"}, &f->arena), DE_OK);
    de_error_clear();
    memset(g_mesh, 0, sizeof g_mesh);
    g_mesh_count = 0;
    memset(g_mat, 0, sizeof g_mat);
    g_mat[0].standard.base_color = (de_vec4){1, 1, 1, 1};
    g_mat[1].standard.base_color = (de_vec4){1, 1, 1, 1};
    g_mat[1].standard.alpha_cutoff = CUTOFF;
    g_mat[1].standard.alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[1].standard.double_sided = true; /* the usual for foliage cards */
    g_mat[1].alpha_mode = DE_COOKED_ALPHA_MASK;
    g_mat[1].textures[DE_COOKED_TEX_BASE_COLOR] = (de_texture){.id = TEX_CHECKER};
    g_checker_bindless = 0;
    for (u32 y = 0; y < CHECK_SIZE; ++y)
        for (u32 x = 0; x < CHECK_SIZE; ++x)
            g_checker_alpha[y * CHECK_SIZE + x] = ((x + y) & 1u) ? 0u : 255u;
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

static void rax_teardown(rax *f) {
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
        if (DE_HANDLE_VALID(f->checker))
            de_gpu_texture_destroy(f->dev, f->checker);
        if (DE_HANDLE_VALID(f->checker_up))
            de_gpu_buffer_destroy(f->dev, f->checker_up);
        if (DE_HANDLE_VALID(f->fence))
            de_gpu_fence_destroy(f->dev, f->fence);
        de_gpu_wait_idle(f->dev);
        de_gpu_device_destroy(f->dev);
    }
    free(f->res.ids);
    free(f->res.depth);
    free(f->res.visible);
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

/* The 8 x 8 checker (RGBA8, alpha 255 / 0) uploaded and left in SHADER_READ. */
static void make_checker(rax *f) {
    f->checker = de_gpu_texture_create(
        f->dev, &(de_gpu_texture_desc){.format = DE_FMT_RGBA8_UNORM,
                                       .width = CHECK_SIZE,
                                       .height = CHECK_SIZE,
                                       .usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST,
                                       .name = "alpha checker"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->checker));
    f->checker_up = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = 256u * CHECK_SIZE,
                                                                       .usage = DE_USAGE_COPY_SRC,
                                                                       .memory = DE_MEM_UPLOAD,
                                                                       .name = "checker upload"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->checker_up));
    u8 *up = (u8 *)de_gpu_buffer_map(f->dev, f->checker_up);
    ASSERT_TRUE(up != NULL);
    for (u32 y = 0; y < CHECK_SIZE; ++y)
        for (u32 x = 0; x < CHECK_SIZE; ++x) {
            u8 *p = up + 256u * y + 4u * x;
            p[0] = 200;
            p[1] = 180;
            p[2] = 60;
            p[3] = g_checker_alpha[y * CHECK_SIZE + x];
        }
    de_gpu_cmd *c = de_gpu_cmd_begin(f->dev, DE_QUEUE_GRAPHICS, "checker upload");
    ASSERT_TRUE(c != NULL);
    de_gpu_barrier b = {
        .texture = f->checker, .before = DE_ACCESS_NONE, .after = DE_ACCESS_COPY_DST};
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_cmd_copy_buffer_to_texture(c, f->checker, 0, 0, f->checker_up, 0, 256u);
    b.before = DE_ACCESS_COPY_DST;
    b.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1, .signal = f->fence, .signal_value = 1};
    ASSERT_EQ(de_gpu_queue_submit(f->dev, DE_QUEUE_GRAPHICS, &s), DE_OK);
    ASSERT_EQ(de_gpu_fence_wait(f->dev, f->fence, 1, WAIT_NS), DE_OK);
    f->frames = 1; /* the fence is at 1 */
    g_checker_bindless = de_gpu_texture_bindless(f->dev, f->checker, 0, false);
    ASSERT_NE(g_checker_bindless, DE_GPU_BINDLESS_INVALID);
}

/* Opens a device (SKIPs without; `need_mesh`: without mesh shaders too), a graph, a GPU scene and
 * the vis frame, and picks the raster path. */
static void rax_open(rax *f, de_gpu_backend backend, bool mesh_path, u32 instances) {
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
    /* Vulkan: the validation layer without GPU-assisted (and synchronization) validation. VVL
     * 1.3.275's GPU-assisted validation (CI's lavapipe) re-checks, at queue retire, every bindless
     * image a shader of the set ever read against the image's layout at the end of the command
     * buffer of a later draw: once a draw samples a bindless texture (the alpha test), the depth
     * and the HZB that compute passes read earlier are reported in whatever layout the command
     * buffer left them. A newer layer does not do this (follow-up: the CI layer version). */
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
    if (backend == DE_GPU_NULL)
        f->dev_null = true;
    if (mesh_path && !caps->mesh_shaders)
        SKIP("the device has no mesh shaders (the compute-path test covers it)");
    ASSERT_EQ(de__rg_create(&(de__rg_desc){.allocator = &f->alloc, .device = f->dev, .name = "ra"},
                            &f->g),
              DE_OK);
    f->fence = de_gpu_fence_create(f->dev, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(f->fence));
    make_checker(f);
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

static u32 rax_mesh(rax *f, de_vec3 normal, f32 size, f32 uv_scale) {
    ASSERT_TRUE(g_mesh_count < MAX_MESHES);
    mesh_quad(&g_mesh[g_mesh_count], normal, size, uv_scale, f->dev);
    return ++g_mesh_count;
}

/* ---------- One frame ---------- */

typedef struct rb_pass {
    rax *f;
    de_rg_resource ids, depth, visible, counters, args[DE__VIS_PHASES], alpha[DE__VIS_PHASES];
    u32 pitch;
    u64 ids_off, depth_off, misc_off, visible_off, visible_bytes;
} rb_pass;

#define RB_COUNTERS 0u
#define RB_ARGS     64u  /* + 32 * phase */
#define RB_ALPHA    128u /* + 32 * phase */

static void readback_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_pass *p = (const rb_pass *)user;
    const de_gpu_buffer rb = p->f->rb;
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, p->ids_off, p->pitch, de_rg_texture(pass, p->ids), 0,
                                      0);
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, p->depth_off, p->pitch,
                                      de_rg_texture(pass, p->depth), 0, 0);
    de_gpu_cmd_copy_buffer(cmd, rb, p->misc_off + RB_COUNTERS, de_rg_buffer(pass, p->counters), 0,
                           DE__VIS_MESHLET_COUNTER_BYTES);
    for (u32 i = 0; i < DE__VIS_PHASES; ++i) {
        if (DE_HANDLE_VALID(p->args[i]))
            de_gpu_cmd_copy_buffer(cmd, rb, p->misc_off + RB_ARGS + 32u * i,
                                   de_rg_buffer(pass, p->args[i]), 0, 32u);
        if (DE_HANDLE_VALID(p->alpha[i]))
            de_gpu_cmd_copy_buffer(cmd, rb, p->misc_off + RB_ALPHA + 32u * i,
                                   de_rg_buffer(pass, p->alpha[i]), 0, 32u);
    }
    de_gpu_cmd_copy_buffer(cmd, rb, p->visible_off, de_rg_buffer(pass, p->visible), 0,
                           p->visible_bytes);
}

static u64 align_up(u64 v, u64 a) {
    return (v + a - 1u) / a * a;
}

static void ensure_rb(rax *f, u64 bytes) {
    if (bytes <= f->rb_size)
        return;
    if (DE_HANDLE_VALID(f->rb)) {
        de_gpu_wait_idle(f->dev);
        de_gpu_buffer_destroy(f->dev, f->rb);
    }
    f->rb = de_gpu_buffer_create(f->dev, &(de_gpu_buffer_desc){.size = bytes,
                                                               .usage = DE_USAGE_COPY_DST,
                                                               .memory = DE_MEM_READBACK,
                                                               .name = "alpha test readback"});
    ASSERT_TRUE(DE_HANDLE_VALID(f->rb));
    f->rb_size = bytes;
}

static bool rax_frame(rax *f) {
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
    ra_result *res = &f->res;
    res->raster_path = vc->raster_path;
    res->view_proj = de_m4_mul(&proj, &view);
    res->visible_capacity = vc->visible_capacity;
    res->alpha_capacity = vc->visible_alpha_capacity;
    rb_pass *rp = NULL;
    if (DE_HANDLE_VALID(vc->vis_ids)) {
        rp = (rb_pass *)de_arena_push_zero(f->arena, sizeof *rp, 16);
        ASSERT_TRUE(rp != NULL);
        rp->f = f;
        rp->ids = vc->vis_ids;
        rp->depth = vc->depth;
        rp->visible = vc->visible_meshlets;
        rp->counters = vc->meshlet_counters;
        for (u32 i = 0; i < DE__VIS_PHASES; ++i) {
            rp->args[i] = vc->raster_args[i];
            rp->alpha[i] = vc->raster_args_alpha[i];
        }
        rp->pitch = (u32)align_up((u64)f->width * 4u, 256u);
        const u64 img = (u64)rp->pitch * f->height;
        rp->ids_off = 0;
        rp->depth_off = align_up(img, 512u);
        rp->misc_off = align_up(rp->depth_off + img, 512u);
        rp->visible_off = rp->misc_off + 256u;
        res->list_entries = vc->visible_capacity + vc->visible_alpha_capacity;
        rp->visible_bytes = (u64)res->list_entries * sizeof(de__vis_meshlet);
        ensure_rb(f, rp->visible_off + rp->visible_bytes);
        de_rg_pass *p =
            de_rg_add_pass(f->g, "test.readback", DE_QUEUE_GRAPHICS, readback_execute, rp);
        de_rg_read(p, rp->ids, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->depth, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->visible, DE_ACCESS_COPY_SRC);
        de_rg_read(p, rp->counters, DE_ACCESS_COPY_SRC);
        for (u32 i = 0; i < DE__VIS_PHASES; ++i) {
            if (DE_HANDLE_VALID(rp->args[i]))
                de_rg_read(p, rp->args[i], DE_ACCESS_COPY_SRC);
            if (DE_HANDLE_VALID(rp->alpha[i]))
                de_rg_read(p, rp->alpha[i], DE_ACCESS_COPY_SRC);
        }
        de_rg_write(p, de_rg_import_buffer(f->g, f->rb, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
        /* The depth back to SHADER_READ before the list ends (Vulkan validation checks the HZB's
         * sampled read against the layout at the end of the command buffer). */
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
    ASSERT_TRUE(res->inst_count <= 16u && res->mat_count <= 4u);
    for (u32 i = 0; i < res->inst_count; ++i)
        de__gpu_scene_debug_pack(f->gs, DE__GS_INSTANCES, i, &res->inst[i]);
    for (u32 i = 0; i < res->mat_count; ++i)
        de__gpu_scene_debug_pack(f->gs, DE__GS_MATERIALS, i, &res->mat[i]);
    de__gpu_scene_frame_end(f->gs, f->g, true);
    f->w.f.dirty_instance_count = 0;
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
    memcpy(res->counters, m + rp->misc_off + RB_COUNTERS, sizeof res->counters);
    for (u32 i = 0; i < DE__VIS_PHASES; ++i) {
        memcpy(&res->args[i], m + rp->misc_off + RB_ARGS + 32u * i, 32u);
        memcpy(&res->alpha_args[i], m + rp->misc_off + RB_ALPHA + 32u * i, 32u);
    }
    free(res->visible);
    res->visible = (de__vis_meshlet *)zalloc(rp->visible_bytes);
    memcpy(res->visible, m + rp->visible_off, (size_t)rp->visible_bytes);
    return true;
}

/* ================================================================================================
 * CPU reference
 * ============================================================================================== */

static u64 key_of(u32 inst, u32 tri) {
    return ((u64)inst << 32) | tri;
}

/* The key of GPU vis ID `id` (NO_KEY: empty; NO_KEY - 1: an entry outside the stored ranges). */
static u64 gpu_key(const rax *f, u32 id) {
    if (id == DE__VIS_ID_EMPTY)
        return NO_KEY;
    const u32 e = de__vis_id_meshlet(id);
    if (e >= f->res.list_entries)
        return NO_KEY - 1u;
    return key_of(f->res.visible[e].instance, de__vis_id_triangle(id));
}

static f32 dot4(const f32 r[4], const f32 p[4]) {
    return r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3] * p[3];
}

static f32 edge(f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static bool top_left(f32 ax, f32 ay, f32 bx, f32 by) {
    return (ay == by && bx < ax) || by < ay;
}

static i32 wrap_i(i32 v) {
    const i32 n = (i32)CHECK_SIZE;
    return ((v % n) + n) % n;
}

/* Bilinear alpha (0..1) of the checker at `uv`, wrap, texel centres at (i + 0.5) / size. */
static f32 checker_alpha(f32 u, f32 v) {
    const f32 x = u * (f32)CHECK_SIZE - 0.5f, y = v * (f32)CHECK_SIZE - 0.5f;
    const f32 fx = floorf(x), fy = floorf(y);
    const f32 tx = x - fx, ty = y - fy;
    const i32 x0 = wrap_i((i32)fx), y0 = wrap_i((i32)fy);
    const i32 x1 = wrap_i((i32)fx + 1), y1 = wrap_i((i32)fy + 1);
    const f32 a00 = g_checker_alpha[y0 * (i32)CHECK_SIZE + x0] / 255.0f;
    const f32 a10 = g_checker_alpha[y0 * (i32)CHECK_SIZE + x1] / 255.0f;
    const f32 a01 = g_checker_alpha[y1 * (i32)CHECK_SIZE + x0] / 255.0f;
    const f32 a11 = g_checker_alpha[y1 * (i32)CHECK_SIZE + x1] / 255.0f;
    return (a00 * (1 - tx) + a10 * tx) * (1 - ty) + (a01 * (1 - tx) + a11 * tx) * ty;
}

typedef struct ref_image {
    u64 *key;
    f32 *depth;
    u8 *alpha_tie; /* the pixel's alpha test of its front-most mask triangle was within 1/32 */
} ref_image;

/* Every live instance's triangles; mask instances (GPU material alpha_mode mask) alpha-tested. */
static void cpu_raster(const rax *f, ref_image *out) {
    const u32 W = f->width, H = f->height;
    const size_t px = (size_t)W * H;
    out->key = (u64 *)zalloc(px * sizeof(u64));
    out->depth = (f32 *)zalloc(px * sizeof(f32));
    out->alpha_tie = (u8 *)zalloc(px);
    for (size_t i = 0; i < px; ++i)
        out->key[i] = NO_KEY;
    f32 vp[4][4];
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            vp[r][c] = f->res.view_proj.m[c * 4u + r];
    for (u32 s = 0; s < f->res.inst_count; ++s) {
        const de__gs_instance *in = &f->res.inst[s];
        if ((in->flags & DE__GS_INSTANCE_LIVE) == 0)
            continue;
        const fmesh *fm = &g_mesh[f->w.f.instances.mesh[s].id - 1u];
        const u32 mat = in->materials[0].v[0];
        const bool mask = mat < f->res.mat_count &&
                          f->res.mat[mat].alpha_mode == DE__VIS_ALPHA_MODE_MASK &&
                          (f->res.mat[mat].flags & DE__GS_MATERIAL_READY) != 0;
        const bool two_sided =
            mat < f->res.mat_count && (f->res.mat[mat].flags & DE__GS_MATERIAL_DOUBLE_SIDED) != 0;
        f32 clip[PATCH_VERTS][4];
        for (u32 v = 0; v < PATCH_VERTS; ++v) {
            const f32 h[4] = {fm->pos[3 * v], fm->pos[3 * v + 1], fm->pos[3 * v + 2], 1.0f};
            const f32 w4[4] = {dot4(in->transform[0].v, h), dot4(in->transform[1].v, h),
                               dot4(in->transform[2].v, h), 1.0f};
            for (u32 k = 0; k < 4; ++k)
                clip[v][k] = dot4(vp[k], w4);
        }
        for (u32 t = 0; t < PATCH_TRIS; ++t) {
            const u8 *tri = fm->tri + 3u * t;
            f32 nx[3], ny[3], nz[3], iw[3], sx[3], sy[3], uu[3], vv[3];
            for (u32 k = 0; k < 3; ++k) {
                const f32 *c = clip[tri[k]];
                iw[k] = 1.0f / c[3];
                nx[k] = c[0] * iw[k];
                ny[k] = c[1] * iw[k];
                nz[k] = c[2] * iw[k];
                sx[k] = roundf((nx[k] * 0.5f + 0.5f) * (f32)W * 256.0f) / 256.0f;
                sy[k] = roundf((0.5f - ny[k] * 0.5f) * (f32)H * 256.0f) / 256.0f;
                uu[k] = fm->uv[2u * tri[k]];
                vv[k] = fm->uv[2u * tri[k] + 1u];
            }
            const f32 area = (nx[1] - nx[0]) * (ny[2] - ny[0]) - (nx[2] - nx[0]) * (ny[1] - ny[0]);
            if (area == 0.0f || (!two_sided && area < 0.0f))
                continue;
            u32 o[3] = {0, 1, 2};
            f32 a2 = edge(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
            if (a2 == 0.0f)
                continue;
            if (a2 < 0.0f) {
                o[1] = 2;
                o[2] = 1;
                a2 = -a2;
            }
            const f32 X[3] = {sx[o[0]], sx[o[1]], sx[o[2]]}, Y[3] = {sy[o[0]], sy[o[1]], sy[o[2]]};
            i32 x0 = (i32)floorf(fminf(X[0], fminf(X[1], X[2])));
            i32 x1 = (i32)ceilf(fmaxf(X[0], fmaxf(X[1], X[2])));
            i32 y0 = (i32)floorf(fminf(Y[0], fminf(Y[1], Y[2])));
            i32 y1 = (i32)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
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
                    const f32 b[3] = {w0 / a2, w1 / a2, w2 / a2};
                    const f32 z = b[0] * nz[o[0]] + b[1] * nz[o[1]] + b[2] * nz[o[2]];
                    if (z < 0.0f || z > 1.0f)
                        continue;
                    const size_t at = (size_t)y * W + (size_t)x;
                    if (out->key[at] != NO_KEY && z < out->depth[at])
                        continue;
                    if (mask) {
                        /* Perspective-correct UV: interpolate attribute / w, then divide. */
                        const f32 q[3] = {b[0] * iw[o[0]], b[1] * iw[o[1]], b[2] * iw[o[2]]};
                        const f32 qs = q[0] + q[1] + q[2];
                        const f32 u = (q[0] * uu[o[0]] + q[1] * uu[o[1]] + q[2] * uu[o[2]]) / qs;
                        const f32 v = (q[0] * vv[o[0]] + q[1] * vv[o[1]] + q[2] * vv[o[2]]) / qs;
                        const f32 a = checker_alpha(u, v);
                        out->alpha_tie[at] |= fabsf(a - CUTOFF) < 1.0f / 32.0f;
                        if (a < CUTOFF)
                            continue; /* discard */
                    }
                    out->depth[at] = z;
                    out->key[at] = key_of(s, t);
                }
        }
    }
}

typedef struct cmp_out {
    u32 pixels, covered_gpu, covered_cpu, same, ties, other, bad_ids;
} cmp_out;

/* Per pixel; a differing pixel is a tie when the CPU flagged its alpha near the cutoff, or a
 * neighbour (3 x 3) matches (an edge or an intersection one pixel off). */
static cmp_out compare(const rax *f) {
    ref_image ref;
    cpu_raster(f, &ref);
    cmp_out o = {0};
    const u32 W = f->width, H = f->height;
    o.pixels = W * H;
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x) {
            const size_t i = (size_t)y * W + x;
            const u64 g = gpu_key(f, f->res.ids[i]);
            o.bad_ids += g == NO_KEY - 1u;
            o.covered_gpu += g != NO_KEY;
            o.covered_cpu += ref.key[i] != NO_KEY;
            if (g == ref.key[i]) {
                ++o.same;
                continue;
            }
            bool tie = ref.alpha_tie[i] != 0;
            for (i32 dy = -1; dy <= 1 && !tie; ++dy)
                for (i32 dx = -1; dx <= 1 && !tie; ++dx) {
                    const i32 nx = (i32)x + dx, ny = (i32)y + dy;
                    if (nx < 0 || ny < 0 || nx >= (i32)W || ny >= (i32)H)
                        continue;
                    const size_t j = (size_t)ny * W + (size_t)nx;
                    tie = ref.key[j] == g || gpu_key(f, f->res.ids[j]) == ref.key[i];
                }
            if (tie)
                ++o.ties;
            else
                ++o.other;
        }
    printf("  %u pixels: %u covered (CPU %u), %u differ (%u ties, %u other)\n", o.pixels,
           o.covered_gpu, o.covered_cpu, o.pixels - o.same, o.ties, o.other);
    free(ref.key);
    free(ref.depth);
    free(ref.alpha_tie);
    return o;
}

static void expect_match(const cmp_out *o) {
    EXPECT_EQ(o->bad_ids, 0u);
    EXPECT_EQ(o->other, 0u);
    EXPECT_GE((u64)o->same * 100u, (u64)o->pixels * 99u);
}

/* Pixels whose ID names instance `s`. */
static u32 pixels_of(const rax *f, u32 s) {
    u32 n = 0;
    for (u32 i = 0; i < f->width * f->height; ++i) {
        const u64 k = gpu_key(f, f->res.ids[i]);
        n += k != NO_KEY && (k >> 32) == s;
    }
    return n;
}

/* True when instance `s` has an entry in a stored range of the list (both buckets, both phases). */
static bool listed(const rax *f, u32 s) {
    const ra_result *r = &f->res;
    for (u32 ph = 0; ph < DE__VIS_PHASES; ++ph) {
        for (u32 k = 0; k < r->args[ph].count; ++k)
            if (r->visible[r->args[ph].first + k].instance == s)
                return true;
        for (u32 k = 0; k < r->alpha_args[ph].count; ++k)
            if (r->visible[r->alpha_args[ph].first + k].instance == s)
                return true;
    }
    return false;
}

/* ================================================================================================
 * GPU tests
 * ============================================================================================== */

/* raster_alpha_cutout_{mesh,compute}_path: a magnified cutout quad (checker alpha) in front of an
 * opaque one: holes exactly where alpha < cutoff, the opaque quad showing through them. */
static void cutout(rax *f, de_gpu_backend b, bool mesh_path) {
    rax_open(f, b, mesh_path, 8u);
    const u32 card = rax_mesh(f, (de_vec3){0, -1, 0}, 2.0f, 1.0f);
    const u32 back = rax_mesh(f, (de_vec3){0, -1, 0}, 6.0f, 1.0f);
    world_set(&f->w, 0, card, (de_vec3){0.0f, 3.0f, 0.0f}, 1.0f, MAT_MASK);
    world_set(&f->w, 1, back, (de_vec3){0.0f, 8.0f, 0.0f}, 1.0f, MAT_OPAQUE);
    ASSERT_TRUE(rax_frame(f));
    EXPECT_EQ(f->res.raster_path,
              mesh_path ? (u32)DE__VIS_RASTER_MESH : (u32)DE__VIS_RASTER_COMPUTE);
    const cmp_out o = compare(f);
    expect_match(&o);
    const u32 card_px = pixels_of(f, 0), back_px = pixels_of(f, 1);
    printf("  cutout card %u pixels, opaque quad %u\n", card_px, back_px);
    EXPECT_GT(card_px, 1000u);
    EXPECT_GT(back_px, 1000u);
    /* The card's IDs come from the alpha region of the list. */
    EXPECT_EQ(f->res.alpha_args[0].count, 1u);
    EXPECT_EQ(f->res.alpha_args[0].first, f->res.visible_capacity);
    EXPECT_EQ(f->res.visible[f->res.visible_capacity].instance, 0u);
    /* Seen from behind (double-sided): the same holes. */
    f->eye = (de_vec3){0.0f, 5.0f, 0.0f};
    f->target = (de_vec3){0.0f, 0.0f, 0.0f};
    ASSERT_TRUE(rax_frame(f));
    const cmp_out o2 = compare(f);
    expect_match(&o2);
    EXPECT_GT(pixels_of(f, 0), 1000u);
}

/* raster_alpha_bucket_routing: mask instances go to the alpha region, the rest to the opaque one;
 * IDs of both regions decode through the one list. */
static void bucket_routing(rax *f, de_gpu_backend b) {
    rax_open(f, b, false, 8u);
    const u32 quad = rax_mesh(f, (de_vec3){0, -1, 0}, 1.0f, 1.0f);
    for (u32 s = 0; s < 6; ++s)
        world_set(&f->w, s, quad, (de_vec3){-2.5f + (f32)s, 6.0f, 0.0f}, 0.9f,
                  s % 2u == 0 ? MAT_MASK : MAT_OPAQUE);
    ASSERT_TRUE(rax_frame(f));
    const ra_result *r = &f->res;
    EXPECT_EQ(r->counters[DE__VIS_MESHLET_COUNTER_ALPHA / 4u], 3u);
    EXPECT_EQ(r->counters[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u], 3u);
    EXPECT_EQ(r->alpha_args[0].first, r->visible_capacity);
    EXPECT_EQ(r->alpha_args[0].count, 3u);
    EXPECT_EQ(r->args[0].first, 0u);
    EXPECT_EQ(r->args[0].count, 3u);
    for (u32 k = 0; k < 3; ++k) {
        EXPECT_EQ(r->visible[k].instance % 2u, 1u);                       /* opaque */
        EXPECT_EQ(r->visible[r->visible_capacity + k].instance % 2u, 0u); /* mask */
    }
    for (u32 s = 0; s < 6; ++s)
        EXPECT_GT(pixels_of(f, s), 50u);
    const cmp_out o = compare(f);
    expect_match(&o);
}

/* raster_alpha_zero_work_when_none: no mask material: the frame has no alpha pass (the null-device
 * test checks the pass list) and the alpha bucket stays empty in both phases. */
static void zero_work(rax *f, de_gpu_backend b) {
    rax_open(f, b, false, 8u);
    const u32 quad = rax_mesh(f, (de_vec3){0, -1, 0}, 1.0f, 1.0f);
    for (u32 s = 0; s < 4; ++s)
        world_set(&f->w, s, quad, (de_vec3){-1.5f + (f32)s, 6.0f, 0.0f}, 0.9f, MAT_OPAQUE);
    ASSERT_TRUE(rax_frame(f));
    EXPECT_EQ(de__gpu_scene_stats_get(f->gs)->mask_materials, 0u); /* so no alpha pass ran */
    EXPECT_EQ(f->res.counters[DE__VIS_MESHLET_COUNTER_ALPHA / 4u], 0u);
    for (u32 ph = 0; ph < DE__VIS_PHASES; ++ph)
        EXPECT_EQ(f->res.alpha_args[ph].count, 0u);
    EXPECT_EQ(f->res.args[0].count, 4u);
    const de__vis_raster_alpha_info info = de__vis_raster_alpha_get_info(f->vis);
    EXPECT_TRUE(info.compute);
    EXPECT_NE(info.sampler, 0u);
    const cmp_out o = compare(f);
    expect_match(&o);
}

/* raster_alpha_hzb_occluder: occlusion on. A cutout wall of four big cells (a 2 x 2 checker) in
 * front of two small quads: the one behind a solid cell is culled once the HZB (built after
 * phase 1's alpha raster) holds the wall; the one behind a hole is not. */
static void hzb_occluder(rax *f, de_gpu_backend b) {
    rax_open(f, b, false, 8u);
    cvar_set("r.occlusion", "1");
    const u32 wall = rax_mesh(f, (de_vec3){0, -1, 0}, 4.0f, 0.25f); /* 2 x 2 checker cells */
    const u32 small_q = rax_mesh(f, (de_vec3){0, -1, 0}, 0.4f, 1.0f);
    world_set(&f->w, 0, wall, (de_vec3){0.0f, 5.0f, 0.0f}, 1.0f, MAT_MASK);
    /* UV (0, 0) is the patch's (-x, -z) corner: texel (0, 0) has alpha 255 (solid), texel (1, 0)
     * alpha 0 (a hole). With UV over [0, 0.25] the wall shows texels 0 and 1 in each direction:
     * solid at (-x, -z) and (+x, +z), holes at (+x, -z) and (-x, +z). */
    world_set(&f->w, 1, small_q, (de_vec3){-1.0f, 9.0f, -1.0f}, 1.0f, MAT_OPAQUE); /* solid */
    world_set(&f->w, 2, small_q, (de_vec3){1.0f, 9.0f, -1.0f}, 1.0f, MAT_OPAQUE);  /* hole */
    for (u32 i = 0; i < 4; ++i)
        ASSERT_TRUE(rax_frame(f));
    printf("  behind solid: %u pixels, listed %d; behind the hole: %u pixels, listed %d\n",
           pixels_of(f, 1), listed(f, 1), pixels_of(f, 2), listed(f, 2));
    EXPECT_FALSE(listed(f, 1)); /* culled: neither phase drew it */
    EXPECT_TRUE(listed(f, 2));
    EXPECT_EQ(pixels_of(f, 1), 0u);
    EXPECT_GT(pixels_of(f, 2), 20u);
    EXPECT_GT(pixels_of(f, 0), 1000u);
    const cmp_out o = compare(f);
    expect_match(&o);
}

/* raster_alpha_zero_work_null: on the null device (its command log): without a mask material the
 * frame has no alpha pass; with one, phase 1's alpha raster follows the opaque one and precedes
 * the HZB. */
TEST_F(raster_alpha, zero_work_null, rax) {
    rax_open(f, DE_GPU_NULL, true, 8u);
    const u32 quad = rax_mesh(f, (de_vec3){0, -1, 0}, 1.0f, 1.0f);
    for (u32 s = 0; s < 3; ++s)
        world_set(&f->w, s, quad, (de_vec3){-1.0f + (f32)s, 6.0f, 0.0f}, 0.9f, MAT_OPAQUE);
    de__gpu_null_log_enable(f->dev, true);
    ASSERT_TRUE(rax_frame(f));
    de__gpu_null_log_enable(f->dev, false);
    u32 alpha = 0;
    for (u32 i = 0; i < de__gpu_null_log_count(f->dev); ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(f->dev, i);
        alpha +=
            e->op == DE__GPU_NULL_OP_MARKER_PUSH && strncmp(e->text, "vis.raster_alpha", 16) == 0;
    }
    EXPECT_EQ(alpha, 0u);
    EXPECT_EQ(de__gpu_scene_stats_get(f->gs)->mask_materials, 0u);
    /* One instance turns alpha-tested. */
    world_set(&f->w, 1, quad, (de_vec3){0.0f, 6.0f, 0.0f}, 0.9f, MAT_MASK);
    de__gpu_null_log_enable(f->dev, true);
    ASSERT_TRUE(rax_frame(f));
    de__gpu_null_log_enable(f->dev, false);
    EXPECT_EQ(de__gpu_scene_stats_get(f->gs)->mask_materials, 1u);
    char order[256] = "";
    size_t at = 0;
    for (u32 i = 0; i < de__gpu_null_log_count(f->dev); ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(f->dev, i);
        if (e->op != DE__GPU_NULL_OP_MARKER_PUSH)
            continue;
        if (strcmp(e->text, "vis.raster.mesh") == 0 ||
            strcmp(e->text, "vis.raster_alpha.mesh") == 0 || strcmp(e->text, "vis.hzb.level0") == 0)
            at += (size_t)snprintf(order + at, sizeof order - at, "%s%s", at ? "," : "", e->text);
    }
    EXPECT_STREQ(order, "vis.raster.mesh,vis.raster_alpha.mesh,vis.hzb.level0");
}

#define GPU_TESTS(sfx, backend)                                                                    \
    TEST_F(raster_alpha, cutout_mesh_path_##sfx, rax) {                                            \
        cutout(f, backend, true);                                                                  \
    }                                                                                              \
    TEST_F(raster_alpha, cutout_compute_path_##sfx, rax) {                                         \
        cutout(f, backend, false);                                                                 \
    }                                                                                              \
    TEST_F(raster_alpha, bucket_routing_##sfx, rax) {                                              \
        bucket_routing(f, backend);                                                                \
    }                                                                                              \
    TEST_F(raster_alpha, zero_work_when_none_##sfx, rax) {                                         \
        zero_work(f, backend);                                                                     \
    }                                                                                              \
    TEST_F(raster_alpha, hzb_occluder_##sfx, rax) {                                                \
        hzb_occluder(f, backend);                                                                  \
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
    char *s = (char *)zalloc((size_t)n + 1u);
    const size_t got = fread(s, 1, (size_t)n, fp);
    s[got] = '\0';
    fclose(fp);
    return s;
}

/* vis_alpha.slang's constants and AlphaParams against the C side; the opaque raster shaders carry
 * no alpha test (no discard, no texture sample): alpha state lives in vis_alpha.slang only. */
TEST(raster_alpha, layout_and_opaque_shaders) {
    char path[1024];
    snprintf(path, sizeof path, "%s/visbuffer/vis_alpha.slang", DE_VIS_TEST_SHADER_DIR);
    char *text = read_text(path);
    ASSERT_TRUE(text != NULL);
    u32 count = 0, found = 0;
    const de__vis_layout_const *k = de__vis_raster_alpha_layout_consts(&count);
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
    const char *ps = strstr(text, "struct AlphaParams");
    const char *pe = ps != NULL ? strstr(ps, "};") : NULL;
    u32 fields = 0;
    for (const char *ln = ps; ln != NULL && pe != NULL && ln < pe; ln = strchr(ln + 1, '\n'))
        fields += strncmp(ln, "\n    uint ", 10) == 0;
    EXPECT_EQ(fields, 8u);
    free(text);
    static const char *const opaque[3] = {"visbuffer/vis_mesh.slang", "visbuffer/vis_vs_ps.slang",
                                          "visbuffer/vis_expand.slang"};
    for (u32 i = 0; i < 3; ++i) {
        snprintf(path, sizeof path, "%s/%s", DE_VIS_TEST_SHADER_DIR, opaque[i]);
        text = read_text(path);
        ASSERT_TRUE(text != NULL);
        EXPECT_TRUE(strstr(text, "discard") == NULL);
        EXPECT_TRUE(strstr(text, ".Sample(") == NULL);
        EXPECT_TRUE(strstr(text, "de_sampler(") == NULL);
        free(text);
    }
}

/* The checker reference: texel centres exact, the cutoff halfway between neighbours. */
TEST(raster_alpha, checker_reference) {
    for (u32 y = 0; y < CHECK_SIZE; ++y)
        for (u32 x = 0; x < CHECK_SIZE; ++x)
            g_checker_alpha[y * CHECK_SIZE + x] = ((x + y) & 1u) ? 0u : 255u;
    EXPECT_FLOAT_EQ(checker_alpha(0.5f / CHECK_SIZE, 0.5f / CHECK_SIZE), 1.0f, 1e-6f);
    EXPECT_FLOAT_EQ(checker_alpha(1.5f / CHECK_SIZE, 0.5f / CHECK_SIZE), 0.0f, 1e-6f);
    EXPECT_FLOAT_EQ(checker_alpha(1.0f / CHECK_SIZE, 0.5f / CHECK_SIZE), 0.5f, 1e-6f);
    /* Wrap: u = 0 lies between texel 7 (alpha 0 on row 0) and texel 0 (255). */
    EXPECT_FLOAT_EQ(checker_alpha(0.0f, 0.5f / CHECK_SIZE), 0.5f, 1e-6f);
}
