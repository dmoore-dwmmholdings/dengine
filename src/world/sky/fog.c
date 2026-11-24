/*
 * fog.c: froxel volumetric fog on the GPU (DENG-207, S34): the per-view volumes (two history
 * atlases that alternate, the integrated atlas), the constant rings, and the four render-graph
 * passes. The sky's view stage (sky_luts.c) calls de__fog_add_passes after sky.render. See
 * fog_internal.h.
 */
#include "world/sky/fog_internal.h"

#if DE_HAS_RENDER

#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/passes/vis/vis_stages.h"
#include "render/shaders/engine_shaders.h"
#include "render/views_internal.h"
#include "render/world/render_world.h"

#include <dengine/de_debug.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { FOG_PIPE_MEDIA, FOG_PIPE_LIGHT, FOG_PIPE_INTEGRATE, FOG_PIPE_APPLY, FOG_PIPES };

/* froxel_inject_media.slang MediaParams */
typedef struct media_params {
    u32 header, entry, media0, media1;
} media_params;

/* froxel_inject_light.slang LightParams */
typedef struct light_params {
    u32 header, entry, media0, media1;
    u32 history_cur, history_prev, lview, lentry;
    u32 lights, dir_lights, grid, list;
    u32 counters, width, height, flags; /* flags: 1 direct lights, 2 ambient */
} light_params;

/* froxel_integrate.slang IntegrateParams */
typedef struct integrate_params {
    u32 header, entry, src, dst;
} integrate_params;

/* fog_apply.slang ApplyParams */
typedef struct apply_params {
    u32 header, entry, integrated, depth;
    u32 target, aerial, aerial_sampler, aerial_range_bits;
    u32 width, height, ortho, pad;
    f32 ray[4];  /* view-space ray at z = -1: top-left (x, y), change across (dx, dy) */
    f32 proj[4]; /* projection m[10], m[14], m[11], m[15] */
} apply_params;
DE_STATIC_ASSERT(sizeof(light_params) == 64, "LightParams is 64 bytes");
DE_STATIC_ASSERT(sizeof(apply_params) == 80, "ApplyParams is 80 bytes");

typedef struct fog_view {
    de_gpu_texture history[2], integrated;
    u32 history_access[2], integrated_access;
    de_gpu_buffer header_buf, volume_buf;
    u8 *header_map, *volume_map;
    u32 header_bindless, volume_bindless;
    de__fog_grid grid;
    u64 frames; /* fog frames rendered with this grid */
    bool failed;
} fog_view;

struct de__fog {
    de_gpu_device *dev;
    const de_allocator *alloc;
    de_gpu_pipeline pipe[FOG_PIPES];
    de_cvar cv_max_distance;
    fog_view views[DE__VIEW_MAX];
};

static const struct {
    const char *shader, *entry;
    u32 bytes, gx, gy, gz;
} k_pipes[FOG_PIPES] = {
    {"fog_inject_media", "cs_fog_media", (u32)sizeof(media_params), 4u, 4u, 4u},
    {"fog_inject_light", "cs_fog_light", (u32)sizeof(light_params), 4u, 4u, 4u},
    {"fog_integrate", "cs_fog_integrate", (u32)sizeof(integrate_params), 8u, 8u, 1u},
    {"fog_apply", "cs_fog_apply", (u32)sizeof(apply_params), 8u, 8u, 1u},
};

static de_result load_pipe(de__fog *f, u32 i) {
    f->pipe[i] = (de_gpu_pipeline){0};
    const char *const entries[1] = {k_pipes[i].entry};
    de__engine_shader_code code;
    char why[512] = "";
    de_result r = de__engine_shader_load(f->dev, f->alloc, k_pipes[i].shader, 0, entries, 1, &code,
                                         why, sizeof why);
    if (r == DE_ERR_OUT_OF_MEMORY || r == DE_ERR_INVALID_ARGUMENT)
        return DE_FAIL(r, "fog: engine shader %s: %.400s", k_pipes[i].shader, why);
    if (r != DE_OK) {
        de_error_clear();
        return DE_OK; /* this backend has no code for it */
    }
    f->pipe[i] = de_gpu_pipeline_create(
        f->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                        .cs = code.entries[0].blob,
                                        .push_constant_bytes = k_pipes[i].bytes,
                                        .threadgroup_size = {k_pipes[i].gx, k_pipes[i].gy,
                                                             k_pipes[i].gz},
                                        .name = k_pipes[i].shader});
    de__engine_shader_code_free(&code);
    if (!DE_HANDLE_VALID(f->pipe[i]))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "fog: the %s pipeline", k_pipes[i].shader);
    return DE_OK;
}

static void view_release(de__fog *f, fog_view *v) {
    for (u32 i = 0; i < 2u; ++i)
        if (DE_HANDLE_VALID(v->history[i]))
            de_gpu_texture_destroy(f->dev, v->history[i]);
    if (DE_HANDLE_VALID(v->integrated))
        de_gpu_texture_destroy(f->dev, v->integrated);
    if (DE_HANDLE_VALID(v->header_buf))
        de_gpu_buffer_destroy(f->dev, v->header_buf);
    if (DE_HANDLE_VALID(v->volume_buf))
        de_gpu_buffer_destroy(f->dev, v->volume_buf);
    memset(v, 0, sizeof *v);
}

/* Creates the view's volumes for `grid` (destroying those of another grid; the destroys wait for
 * the GPU). The only allocation of a view's fog, done in its first fog frame and on a change. */
static bool view_ensure(de__fog *f, fog_view *v, u32 slot, de__fog_grid grid) {
    if (v->failed)
        return false;
    if (DE_HANDLE_VALID(v->integrated) && v->grid.w == grid.w && v->grid.h == grid.h &&
        v->grid.d == grid.d)
        return true;
    de_gpu_buffer header = v->header_buf, volumes = v->volume_buf;
    u8 *hmap = v->header_map, *vmap = v->volume_map;
    u32 hb = v->header_bindless, vb = v->volume_bindless;
    for (u32 i = 0; i < 2u; ++i)
        if (DE_HANDLE_VALID(v->history[i]))
            de_gpu_texture_destroy(f->dev, v->history[i]);
    if (DE_HANDLE_VALID(v->integrated))
        de_gpu_texture_destroy(f->dev, v->integrated);
    memset(v, 0, sizeof *v);
    v->header_buf = header;
    v->volume_buf = volumes;
    v->header_map = hmap;
    v->volume_map = vmap;
    v->header_bindless = hb;
    v->volume_bindless = vb;
    char name[48];
    if (!DE_HANDLE_VALID(v->header_buf)) {
        snprintf(name, sizeof name, "fog header %u", slot);
        v->header_buf = de_gpu_buffer_create(
            f->dev, &(de_gpu_buffer_desc){.size = DE__VIS_FRAME_RING * sizeof(de__fog_header),
                                          .usage = DE_USAGE_STORAGE,
                                          .memory = DE_MEM_UPLOAD,
                                          .stride = (u32)sizeof(de__fog_header),
                                          .name = name});
        snprintf(name, sizeof name, "fog volumes %u", slot);
        v->volume_buf = de_gpu_buffer_create(
            f->dev, &(de_gpu_buffer_desc){.size = (u64)DE__VIS_FRAME_RING * DE__FOG_MAX_VOLUMES *
                                                  sizeof(de__fog_volume_gpu),
                                          .usage = DE_USAGE_STORAGE,
                                          .memory = DE_MEM_UPLOAD,
                                          .stride = (u32)sizeof(de__fog_volume_gpu),
                                          .name = name});
        v->header_map = DE_HANDLE_VALID(v->header_buf)
                            ? (u8 *)de_gpu_buffer_map(f->dev, v->header_buf)
                            : NULL;
        v->volume_map = DE_HANDLE_VALID(v->volume_buf)
                            ? (u8 *)de_gpu_buffer_map(f->dev, v->volume_buf)
                            : NULL;
        v->header_bindless =
            v->header_map != NULL ? de_gpu_buffer_bindless(f->dev, v->header_buf) : 0u;
        v->volume_bindless =
            v->volume_map != NULL ? de_gpu_buffer_bindless(f->dev, v->volume_buf) : 0u;
    }
    u32 aw = 0, ah = 0;
    de__fog_atlas_size(grid, &aw, &ah);
    static const char *const k_names[3] = {"fog.history0", "fog.history1", "fog.integrated"};
    de_gpu_texture *t[3] = {&v->history[0], &v->history[1], &v->integrated};
    for (u32 i = 0; i < 3u; ++i)
        *t[i] = de_gpu_texture_create(
            f->dev, &(de_gpu_texture_desc){.width = aw,
                                           .height = ah,
                                           .format = DE_FMT_RGBA16_FLOAT,
                                           .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                           .name = k_names[i]});
    if (v->header_map == NULL || v->volume_map == NULL || !DE_HANDLE_VALID(v->history[0]) ||
        !DE_HANDLE_VALID(v->history[1]) || !DE_HANDLE_VALID(v->integrated)) {
        const de_error *e = de_error_last();
        DE_ERROR(DE_LOG_CAT_GENERAL, "fog: view slot %u: volumes: %s; fog is off for it", slot,
                     e != NULL ? e->message : "?");
        view_release(f, v);
        v->failed = true;
        return false;
    }
    v->grid = grid;
    return true;
}

de_result de__fog_create(de_gpu_device *dev, const de_allocator *alloc, de__fog **out) {
    if (out != NULL)
        *out = NULL;
    if (dev == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__fog_create: NULL argument");
    if (alloc == NULL)
        alloc = de_heap_allocator();
    de__fog *f = DE_NEW(alloc, de__fog, DE_MEM_RENDER);
    if (f == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "fog: no memory");
    memset(f, 0, sizeof *f);
    f->dev = dev;
    f->alloc = alloc;
    de_result r = DE_OK;
    for (u32 i = 0; i < FOG_PIPES && r == DE_OK; ++i)
        r = load_pipe(f, i);
    if (r == DE_OK)
        f->cv_max_distance = de_cvar_register(&(de_cvar_desc){
            .name = "r.fog.max_distance",
            .help = "Volumetric fog: the depth (m) its froxel slices cover; the aerial "
                    "perspective continues beyond",
            .type = DE_CVAR_FLOAT,
            .flags = DE_CVAR_SAVED,
            .default_number = DE__FOG_MAX_DISTANCE,
            .min = 1.0,
            .max = 4096.0});
    if (r != DE_OK) {
        de__fog_destroy(f);
        return r;
    }
    *out = f;
    return DE_OK;
}

void de__fog_destroy(de__fog *f) {
    if (f == NULL)
        return;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        view_release(f, &f->views[i]);
    for (u32 i = 0; i < FOG_PIPES; ++i)
        if (DE_HANDLE_VALID(f->pipe[i]))
            de_gpu_pipeline_destroy(f->dev, f->pipe[i]);
    DE_FREE(f->alloc, f, sizeof *f);
}

bool de__fog_ready(const de__fog *f) {
    if (f == NULL)
        return false;
    for (u32 i = 0; i < FOG_PIPES; ++i)
        if (!DE_HANDLE_VALID(f->pipe[i]))
            return false;
    return true;
}

f32 de__fog_max_distance(const de__fog *f) {
    if (f == NULL || !f->cv_max_distance.id)
        return DE__FOG_MAX_DISTANCE;
    return (f32)de_cvar_float(f->cv_max_distance);
}

/* ---------- The passes ---------- */

typedef struct fog_pass {
    de_gpu_device *dev;
    de_gpu_pipeline pipe;
    u32 gx, gy, gz;
    de_rg_resource r[4]; /* per pass: see each exec */
    union {
        media_params media;
        light_params light;
        integrate_params integrate;
        apply_params apply;
    } pc;
    u32 pc_bytes;
    const de__lighting_context *lc;
} fog_pass;

static u32 tex_index(const fog_pass *p, const de_rg_pass *pass, de_rg_resource r, bool storage) {
    return DE_HANDLE_VALID(r) ? de_gpu_texture_bindless(p->dev, de_rg_texture(pass, r), 0, storage)
                              : DE_GPU_BINDLESS_INVALID;
}

static u32 buf_index(const fog_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return DE_HANDLE_VALID(r) ? de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, r))
                              : DE_GPU_BINDLESS_INVALID;
}

static void dispatch(de_gpu_cmd *cmd, fog_pass *p) {
    if (!DE_HANDLE_VALID(p->pipe))
        return;
    de_gpu_cmd_set_pipeline(cmd, p->pipe);
    de_gpu_cmd_push_constants(cmd, &p->pc, p->pc_bytes);
    de_gpu_cmd_dispatch(cmd, p->gx, p->gy, p->gz);
}

static void media_exec(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    fog_pass *p = (fog_pass *)user;
    p->pc.media.media0 = tex_index(p, pass, p->r[0], true);
    p->pc.media.media1 = tex_index(p, pass, p->r[1], true);
    dispatch(cmd, p);
}

static void light_exec(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    fog_pass *p = (fog_pass *)user;
    p->pc.light.media0 = tex_index(p, pass, p->r[0], false);
    p->pc.light.media1 = tex_index(p, pass, p->r[1], false);
    p->pc.light.history_cur = tex_index(p, pass, p->r[2], true);
    p->pc.light.history_prev = tex_index(p, pass, p->r[3], false);
    if (p->lc != NULL) {
        p->pc.light.lights = buf_index(p, pass, p->lc->lights);
        p->pc.light.dir_lights = buf_index(p, pass, p->lc->dir_lights);
        p->pc.light.grid = buf_index(p, pass, p->lc->grid_buf);
        p->pc.light.list = buf_index(p, pass, p->lc->list);
        p->pc.light.counters = buf_index(p, pass, p->lc->counters);
    }
    dispatch(cmd, p);
}

static void integrate_exec(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    fog_pass *p = (fog_pass *)user;
    p->pc.integrate.src = tex_index(p, pass, p->r[0], false);
    p->pc.integrate.dst = tex_index(p, pass, p->r[1], true);
    dispatch(cmd, p);
}

static void apply_exec(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    fog_pass *p = (fog_pass *)user;
    p->pc.apply.integrated = tex_index(p, pass, p->r[0], false);
    p->pc.apply.depth = tex_index(p, pass, p->r[1], false);
    p->pc.apply.target = tex_index(p, pass, p->r[2], true);
    p->pc.apply.aerial = tex_index(p, pass, p->r[3], false);
    dispatch(cmd, p);
}

static void put4(f32 o[4], de_vec3 v, f32 w) {
    o[0] = v.x;
    o[1] = v.y;
    o[2] = v.z;
    o[3] = w;
}

static de_vec3 unproject(const de_mat4 *inv, f32 x, f32 y, f32 z) {
    const de_vec4 h = de_m4_mul_v4(inv, (de_vec4){x, y, z, 1.0f});
    const f32 w = h.w != 0.0f ? h.w : 1e-20f;
    return (de_vec3){h.x / w, h.y / w, h.z / w};
}

/* The 3x3 part of `m` applied to `v`. */
static de_vec3 rotate(const de_mat4 *m, de_vec3 v) {
    return (de_vec3){m->m[0] * v.x + m->m[4] * v.y + m->m[8] * v.z,
                     m->m[1] * v.x + m->m[5] * v.y + m->m[9] * v.z,
                     m->m[2] * v.x + m->m[6] * v.y + m->m[10] * v.z};
}

/* Radical inverse base 2 of `i` (the slice jitter sequence). */
static f32 halton2(u32 i) {
    f32 f = 1.0f, r = 0.0f;
    for (u32 n = i + 1u; n > 0u; n >>= 1u) {
        f *= 0.5f;
        r += f * (f32)(n & 1u);
    }
    return r;
}

static fog_pass *new_pass(de__fog *f, de_arena *arena, u32 pipe, u32 bytes) {
    fog_pass *p = (fog_pass *)de_arena_push(arena, sizeof *p, 16);
    if (p == NULL)
        return NULL;
    memset(p, 0, sizeof *p);
    p->dev = f->dev;
    p->pipe = f->pipe[pipe];
    p->pc_bytes = bytes;
    return p;
}

static void read_lighting(de_rg_pass *pass, const de__lighting_context *lc) {
    const de_rg_resource bufs[] = {lc->lights,         lc->dir_lights,    lc->grid_buf,
                                   lc->list,           lc->counters,      lc->shadow_records,
                                   lc->atlas_records,  lc->atlas_texture};
    for (u32 i = 0; i < (u32)(sizeof bufs / sizeof bufs[0]); ++i)
        if (DE_HANDLE_VALID(bufs[i]))
            de_rg_read(pass, bufs[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->shadow_texture_count && i < 8u; ++i)
        if (DE_HANDLE_VALID(lc->shadow_textures[i]))
            de_rg_read(pass, lc->shadow_textures[i], DE_ACCESS_SHADER_READ);
}

u32 de__fog_add_passes(de__fog *f, de_rg *g, de_arena *arena, const de__fog_inputs *in,
                       de__fog_output *out) {
    if (out != NULL) {
        out->integrated = (de_rg_resource){0};
        out->header = 0u;
    }
    if (f == NULL || g == NULL || arena == NULL || in == NULL || in->slot >= DE__VIEW_MAX ||
        in->width == 0u || in->height == 0u || !DE_HANDLE_VALID(in->depth) ||
        !DE_HANDLE_VALID(in->target))
        return 0u;
    const f32 far_m = de__fog_max_distance(f);
    if (!de__fog_enabled(in->settings, in->volumes, in->volume_count, far_m))
        return 0u;
    const de__fog_grid grid = de__fog_grid_for(in->settings->volumetrics);
    fog_view *v = &f->views[in->slot];
    const bool regrid = v->grid.w != grid.w || v->grid.h != grid.h || v->grid.d != grid.d;
    if (!view_ensure(f, v, in->slot, grid))
        return 0u;
    if (regrid)
        v->frames = 0;

    /* ---- The constants (this frame's ring entry: no allocation) ---- */
    const u32 entry = (u32)(in->frame_index % DE__VIS_FRAME_RING);
    de__fog_header *h = (de__fog_header *)(v->header_map + (size_t)entry * sizeof *h);
    de__fog_volume_gpu *vols =
        (de__fog_volume_gpu *)(v->volume_map +
                               (size_t)entry * DE__FOG_MAX_VOLUMES * sizeof(de__fog_volume_gpu));
    const de_mat4 view_inv = de_m4_inverse_affine(&in->view_matrix);
    const de_vec3 cam = {view_inv.m[12], view_inv.m[13], view_inv.m[14]};
    const u32 count =
        de__fog_pack_volumes(in->volumes, in->volume_count, cam, vols, DE__FOG_MAX_VOLUMES);
    const de_mat4 *pr = &in->proj;
    const bool ortho = pr->m[11] == 0.0f && pr->m[15] != 0.0f;
    const bool history = in->history_ok && v->frames > 0u && !ortho;
    const u32 cur = (u32)(v->frames & 1u), prev = cur ^ 1u;
    const de_mat4 ip = de_m4_inverse(pr);
    de_vec3 c00 = unproject(&ip, -1.0f, 1.0f, 0.5f), c11 = unproject(&ip, 1.0f, -1.0f, 0.5f);
    if (!ortho && c00.z != 0.0f && c11.z != 0.0f) {
        const f32 s0 = -1.0f / c00.z, s1 = -1.0f / c11.z;
        c00 = (de_vec3){c00.x * s0, c00.y * s0, -1.0f};
        c11 = (de_vec3){c11.x * s1, c11.y * s1, -1.0f};
    }
    memset(h, 0, sizeof *h);
    h->grid[0] = grid.w;
    h->grid[1] = grid.h;
    h->grid[2] = grid.d;
    h->grid[3] = DE__FOG_ATLAS_COLUMNS;
    h->tex[0] = de_gpu_texture_bindless(f->dev, v->integrated, 0, false);
    h->tex[1] = in->sampler;
    h->tex[2] = v->volume_bindless;
    h->tex[3] = count;
    h->depth[0] = DE__FOG_NEAR;
    h->depth[1] = far_m;
    h->depth[2] = logf(far_m / DE__FOG_NEAR);
    h->depth[3] = history ? halton2((u32)(in->frame_index & 15u)) : 0.5f;
    if (ortho) {
        put4(h->ray00, rotate(&view_inv, (de_vec3){0.0f, 0.0f, -1.0f}), 0.0f);
        put4(h->ray_du, (de_vec3){0.0f, 0.0f, 0.0f}, in->pre_exposure);
        put4(h->ray_dv, (de_vec3){0.0f, 0.0f, 0.0f}, 1.0f);
        put4(h->org00, rotate(&view_inv, (de_vec3){c00.x, c00.y, 0.0f}),
             (f32)(entry * DE__FOG_MAX_VOLUMES));
        put4(h->org_du, rotate(&view_inv, (de_vec3){c11.x - c00.x, 0.0f, 0.0f}), 0.0f);
        put4(h->org_dv, rotate(&view_inv, (de_vec3){0.0f, c11.y - c00.y, 0.0f}), 0.0f);
    } else {
        put4(h->ray00, rotate(&view_inv, c00), history ? DE__FOG_HISTORY_WEIGHT : 0.0f);
        put4(h->ray_du, rotate(&view_inv, (de_vec3){c11.x - c00.x, 0.0f, 0.0f}),
             in->pre_exposure);
        put4(h->ray_dv, rotate(&view_inv, (de_vec3){0.0f, c11.y - c00.y, 0.0f}), 0.0f);
        put4(h->org00, (de_vec3){0.0f, 0.0f, 0.0f}, (f32)(entry * DE__FOG_MAX_VOLUMES));
    }
    put4(h->camera, cam, 0.0f);
    for (u32 r = 0; r < 4u; ++r)
        for (u32 c = 0; c < 4u; ++c)
            h->prev_vp[r][c] = in->prev_view_proj.m[c * 4u + r];

    /* ---- The graph ---- */
    u32 aw = 0, ah = 0;
    de__fog_atlas_size(grid, &aw, &ah);
    const de_gpu_texture_desc media_desc = {.width = aw,
                                            .height = ah,
                                            .format = DE_FMT_RGBA16_FLOAT,
                                            .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                            .name = "fog.media"};
    de_gpu_texture_desc phase_desc = media_desc;
    phase_desc.name = "fog.phase";
    const de_rg_resource media0 = de_rg_create_texture(g, &media_desc);
    const de_rg_resource media1 = de_rg_create_texture(g, &phase_desc);
    const de_rg_resource hist_cur = de_rg_import_texture(g, v->history[cur], v->history_access[cur]);
    const de_rg_resource hist_prev =
        de_rg_import_texture(g, v->history[prev], v->history_access[prev]);
    const de_rg_resource integrated =
        de_rg_import_texture(g, v->integrated, v->integrated_access);
    fog_pass *pm = new_pass(f, arena, FOG_PIPE_MEDIA, (u32)sizeof(media_params));
    fog_pass *pl = new_pass(f, arena, FOG_PIPE_LIGHT, (u32)sizeof(light_params));
    fog_pass *pi = new_pass(f, arena, FOG_PIPE_INTEGRATE, (u32)sizeof(integrate_params));
    fog_pass *pa = new_pass(f, arena, FOG_PIPE_APPLY, (u32)sizeof(apply_params));
    if (!DE_HANDLE_VALID(media0) || !DE_HANDLE_VALID(media1) || !DE_HANDLE_VALID(hist_cur) ||
        !DE_HANDLE_VALID(hist_prev) || !DE_HANDLE_VALID(integrated) || pm == NULL || pl == NULL ||
        pi == NULL || pa == NULL) {
        ++g->error_count;
        return 0u;
    }
    de_rg_set_name(g, hist_cur, "fog.history");
    de_rg_set_name(g, hist_prev, "fog.history_prev");
    de_rg_set_name(g, integrated, "fog.integrated");
    u32 added = 0;
    const u32 hdr = v->header_bindless;

    pm->gx = (grid.w + 3u) / 4u;
    pm->gy = (grid.h + 3u) / 4u;
    pm->gz = (grid.d + 3u) / 4u;
    pm->r[0] = media0;
    pm->r[1] = media1;
    pm->pc.media = (media_params){.header = hdr, .entry = entry};
    de_rg_pass *pass = de_rg_add_pass(g, "fog.media", DE_QUEUE_GRAPHICS, media_exec, pm);
    if (pass == NULL)
        return added;
    ++added;
    de_rg_write(pass, media0, DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass, media1, DE_ACCESS_SHADER_WRITE);

    *pl = *pm;
    pl->pipe = f->pipe[FOG_PIPE_LIGHT];
    pl->pc_bytes = (u32)sizeof(light_params);
    pl->r[2] = hist_cur;
    pl->r[3] = hist_prev;
    const de__lighting_context *lc = in->lc;
    pl->lc = lc;
    pl->pc.light = (light_params){.header = hdr,
                                  .entry = entry,
                                  .lview = lc != NULL ? lc->constants_bindless : 0u,
                                  .lentry = lc != NULL ? lc->constants_entry : 0u,
                                  .width = in->width,
                                  .height = in->height,
                                  .flags = (lc != NULL ? 1u : 0u) |
                                           (lc != NULL && in->ambient ? 2u : 0u)};
    pass = de_rg_add_pass(g, "fog.light", DE_QUEUE_GRAPHICS, light_exec, pl);
    if (pass == NULL)
        return added;
    ++added;
    de_rg_read(pass, media0, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, media1, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, hist_prev, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, hist_cur, DE_ACCESS_SHADER_WRITE);
    if (lc != NULL)
        read_lighting(pass, lc);

    pi->gx = (grid.w + 7u) / 8u;
    pi->gy = (grid.h + 7u) / 8u;
    pi->gz = 1u;
    pi->r[0] = hist_cur;
    pi->r[1] = integrated;
    pi->pc.integrate = (integrate_params){.header = hdr, .entry = entry};
    pass = de_rg_add_pass(g, "fog.integrate", DE_QUEUE_GRAPHICS, integrate_exec, pi);
    if (pass == NULL)
        return added;
    ++added;
    de_rg_read(pass, hist_cur, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, integrated, DE_ACCESS_SHADER_WRITE);

    pa->gx = (in->width + 7u) / 8u;
    pa->gy = (in->height + 7u) / 8u;
    pa->gz = 1u;
    pa->r[0] = integrated;
    pa->r[1] = in->depth;
    pa->r[2] = in->target;
    pa->r[3] = in->aerial;
    f32 range = in->aerial_range_km;
    u32 range_bits = 0;
    memcpy(&range_bits, &range, sizeof range_bits);
    pa->pc.apply = (apply_params){.header = hdr,
                                  .entry = entry,
                                  .aerial_sampler = in->aerial_sampler,
                                  .aerial_range_bits = range_bits,
                                  .width = in->width,
                                  .height = in->height,
                                  .ortho = ortho ? 1u : 0u,
                                  .ray = {c00.x, c00.y, c11.x - c00.x, c11.y - c00.y},
                                  .proj = {pr->m[10], pr->m[14], pr->m[11], pr->m[15]}};
    pass = de_rg_add_pass(g, "fog.apply", DE_QUEUE_GRAPHICS, apply_exec, pa);
    if (pass == NULL)
        return added;
    ++added;
    de_rg_read(pass, integrated, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, in->depth, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(in->aerial))
        de_rg_read(pass, in->aerial, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, in->target, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, in->target, DE_ACCESS_SHADER_WRITE);

    /* Both histories and the volume end the frame sampled; next frame imports them so. */
    de_rg_export(g, hist_cur, DE_ACCESS_SHADER_READ);
    de_rg_export(g, hist_prev, DE_ACCESS_SHADER_READ);
    de_rg_export(g, integrated, DE_ACCESS_SHADER_READ);
    v->history_access[cur] = v->history_access[prev] = DE_ACCESS_SHADER_READ;
    v->integrated_access = DE_ACCESS_SHADER_READ;
    ++v->frames;
    if (out != NULL) {
        out->integrated = integrated;
        out->header = hdr;
    }
    return added;
}

#endif /* DE_HAS_RENDER */
