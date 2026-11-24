/*
 * clouds.c: raymarched volumetric clouds on the GPU (DENG-208, S34): the noises (made once), the
 * per-view shadow map, histories and constant rings, and the four render-graph passes. The sky's
 * stages (sky_luts.c) call de__clouds_add_shadow_pass before the lighting build and
 * de__clouds_add_passes after sky.render. See clouds_internal.h.
 */
#include "world/sky/clouds_internal.h"

#if DE_HAS_RENDER

#include "world/sky/sky_internal.h"

#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/passes/vis/vis_stages.h"
#include "render/shaders/engine_shaders.h"
#include "render/views_internal.h"

#include <dengine/de_debug.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    CLOUDS_PIPE_NOISE,
    CLOUDS_PIPE_SHADOW,
    CLOUDS_PIPE_MARCH,
    CLOUDS_PIPE_RECONSTRUCT,
    CLOUDS_PIPE_COMPOSITE,
    CLOUDS_PIPES
};

/* clouds_noise_gen.slang NoiseParams */
typedef struct noise_params {
    u32 target, mode, width, height;
} noise_params;

/* clouds_shadow.slang ShadowParams */
typedef struct shadow_params {
    u32 header, entry, target, n;
    f32 area[4]; /* centre x, y, extent, layer bottom z (render-origin) */
} shadow_params;

/* clouds_march.slang MarchParams */
typedef struct march_params {
    u32 header, entry, depth, color;
    u32 dist, lview, lentry, width;
    u32 height, mw, mh, flags; /* flags: 1 the IBL ambient */
} march_params;

/* clouds_reconstruct.slang ReconstructParams */
typedef struct reconstruct_params {
    u32 header, entry, march, dist;
    u32 prev, cur, width, height;
    u32 mw, mh, sampler, pad;
} reconstruct_params;

/* clouds_composite.slang CompositeParams */
typedef struct composite_params {
    u32 header, entry, clouds, depth;
    u32 target, aerial, aerial_sampler, aerial_range_bits;
    u32 width, height, mw, mh;
    u32 dist, flags, pad0, pad1; /* flags: 1 the aerial perspective on opaque pixels */
} composite_params;
DE_STATIC_ASSERT(sizeof(shadow_params) == 32, "ShadowParams is 32 bytes");
DE_STATIC_ASSERT(sizeof(march_params) == 48, "MarchParams is 48 bytes");
DE_STATIC_ASSERT(sizeof(reconstruct_params) == 48, "ReconstructParams is 48 bytes");
DE_STATIC_ASSERT(sizeof(composite_params) == 64, "CompositeParams is 64 bytes");

typedef struct clouds_view {
    de_gpu_texture shadow, history[2];
    u32 shadow_access, history_access[2];
    u32 shadow_bindless;
    de_gpu_buffer header_buf, shadow_buf;
    u8 *header_map, *shadow_map;
    u32 header_bindless, shadow_header_bindless;
    u32 width, height; /* the histories' size */
    u64 frames;        /* cloud frames with these histories */
    bool failed;
} clouds_view;

struct de__clouds {
    de_gpu_device *dev;
    const de_allocator *alloc;
    de_gpu_pipeline pipe[CLOUDS_PIPES];
    de_gpu_texture base, detail, weather;
    u32 base_bindless, detail_bindless, weather_bindless;
    de_gpu_sampler wrap;
    u32 wrap_bindless;
    bool noise_ready, noise_failed;
    clouds_view views[DE__VIEW_MAX];
};

static const struct {
    const char *shader, *entry;
    u32 bytes;
} k_pipes[CLOUDS_PIPES] = {
    {"clouds_noise", "cs_clouds_noise", (u32)sizeof(noise_params)},
    {"clouds_shadow", "cs_clouds_shadow", (u32)sizeof(shadow_params)},
    {"clouds_march", "cs_clouds_march", (u32)sizeof(march_params)},
    {"clouds_reconstruct", "cs_clouds_reconstruct", (u32)sizeof(reconstruct_params)},
    {"clouds_composite", "cs_clouds_composite", (u32)sizeof(composite_params)},
};

static de_result load_pipe(de__clouds *c, u32 i) {
    c->pipe[i] = (de_gpu_pipeline){0};
    const char *const entries[1] = {k_pipes[i].entry};
    de__engine_shader_code code;
    char why[512] = "";
    de_result r = de__engine_shader_load(c->dev, c->alloc, k_pipes[i].shader, 0, entries, 1, &code,
                                         why, sizeof why);
    if (r == DE_ERR_OUT_OF_MEMORY || r == DE_ERR_INVALID_ARGUMENT)
        return DE_FAIL(r, "clouds: engine shader %s: %.400s", k_pipes[i].shader, why);
    if (r != DE_OK) {
        de_error_clear();
        return DE_OK; /* this backend has no code for it */
    }
    c->pipe[i] = de_gpu_pipeline_create(
        c->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                        .cs = code.entries[0].blob,
                                        .push_constant_bytes = k_pipes[i].bytes,
                                        .threadgroup_size = {8u, 8u, 1u},
                                        .name = k_pipes[i].shader});
    de__engine_shader_code_free(&code);
    if (!DE_HANDLE_VALID(c->pipe[i]))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "clouds: the %s pipeline", k_pipes[i].shader);
    return DE_OK;
}

static void destroy_texture(de__clouds *c, de_gpu_texture *t) {
    if (DE_HANDLE_VALID(*t))
        de_gpu_texture_destroy(c->dev, *t);
    *t = (de_gpu_texture){0};
}

static void view_release(de__clouds *c, clouds_view *v) {
    destroy_texture(c, &v->shadow);
    destroy_texture(c, &v->history[0]);
    destroy_texture(c, &v->history[1]);
    if (DE_HANDLE_VALID(v->header_buf))
        de_gpu_buffer_destroy(c->dev, v->header_buf);
    if (DE_HANDLE_VALID(v->shadow_buf))
        de_gpu_buffer_destroy(c->dev, v->shadow_buf);
    memset(v, 0, sizeof *v);
}

static void view_fail(de__clouds *c, clouds_view *v, u32 slot, const char *what) {
    const de_error *e = de_error_last();
    DE_ERROR(DE_LOG_CAT_GENERAL, "clouds: view slot %u: %s: %s; clouds are off for it", slot, what,
             e != NULL ? e->message : "?");
    view_release(c, v);
    v->failed = true;
}

static de_gpu_buffer ring_create(de__clouds *c, u32 entry_bytes, u32 stride, const char *name,
                                 u8 **map, u32 *bindless) {
    const de_gpu_buffer b = de_gpu_buffer_create(
        c->dev, &(de_gpu_buffer_desc){.size = (u64)DE__VIS_FRAME_RING * entry_bytes,
                                      .usage = DE_USAGE_STORAGE,
                                      .memory = DE_MEM_UPLOAD,
                                      .stride = stride,
                                      .name = name});
    *map = DE_HANDLE_VALID(b) ? (u8 *)de_gpu_buffer_map(c->dev, b) : NULL;
    *bindless = *map != NULL ? de_gpu_buffer_bindless(c->dev, b) : 0u;
    return b;
}

/* The view's constant rings and shadow map: made in its first cloud frame. */
static bool view_ensure_common(de__clouds *c, clouds_view *v, u32 slot) {
    if (v->failed)
        return false;
    if (v->header_map != NULL && v->shadow_map != NULL && DE_HANDLE_VALID(v->shadow))
        return true;
    char name[48];
    snprintf(name, sizeof name, "clouds header %u", slot);
    v->header_buf = ring_create(c, (u32)sizeof(de__clouds_header), (u32)sizeof(de__clouds_header),
                                name, &v->header_map, &v->header_bindless);
    /* de_input_cloud_shadow reads it as bytes: stride 0. */
    snprintf(name, sizeof name, "clouds shadow header %u", slot);
    v->shadow_buf = ring_create(c, (u32)sizeof(de__cloud_shadow_gpu), 0u, name, &v->shadow_map,
                                &v->shadow_header_bindless);
    v->shadow = de_gpu_texture_create(
        c->dev, &(de_gpu_texture_desc){.width = DE__CLOUDS_SHADOW_N,
                                       .height = DE__CLOUDS_SHADOW_N,
                                       .format = DE_FMT_R16_FLOAT,
                                       .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                       .name = "clouds.shadow"});
    v->shadow_bindless =
        DE_HANDLE_VALID(v->shadow) ? de_gpu_texture_bindless(c->dev, v->shadow, 0, false) : 0u;
    v->shadow_access = DE_ACCESS_NONE;
    if (v->header_map == NULL || v->shadow_map == NULL || !DE_HANDLE_VALID(v->shadow)) {
        view_fail(c, v, slot, "the constants or the shadow map");
        return false;
    }
    return true;
}

/* The histories for `w` x `h` (destroying those of another size; the destroys wait for the GPU). */
static bool view_ensure_history(de__clouds *c, clouds_view *v, u32 slot, u32 w, u32 h) {
    if (!view_ensure_common(c, v, slot))
        return false;
    if (DE_HANDLE_VALID(v->history[0]) && v->width == w && v->height == h)
        return true;
    destroy_texture(c, &v->history[0]);
    destroy_texture(c, &v->history[1]);
    static const char *const k_names[2] = {"clouds.history0", "clouds.history1"};
    for (u32 i = 0; i < 2u; ++i) {
        v->history[i] = de_gpu_texture_create(
            c->dev, &(de_gpu_texture_desc){.width = w,
                                           .height = h,
                                           .format = DE_FMT_RGBA16_FLOAT,
                                           .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                           .name = k_names[i]});
        v->history_access[i] = DE_ACCESS_NONE;
    }
    if (!DE_HANDLE_VALID(v->history[0]) || !DE_HANDLE_VALID(v->history[1])) {
        view_fail(c, v, slot, "the histories");
        return false;
    }
    v->width = w;
    v->height = h;
    v->frames = 0;
    return true;
}

static de_gpu_barrier tex_barrier(de_gpu_texture t, u32 before, u32 after) {
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.texture = t;
    b.before = before;
    b.after = after;
    return b;
}

/* The base and detail noise atlases and the weather map: made and filled once, in one submission
 * outside the graph (later work on the graphics queue sees them). */
static bool ensure_noise(de__clouds *c) {
    if (c->noise_ready)
        return true;
    if (c->noise_failed)
        return false;
    static const struct {
        u32 w, h;
        const char *name;
    } k_tex[3] = {
        {DE__CLOUDS_BASE_TILE * DE__CLOUDS_BASE_COLUMNS,
         DE__CLOUDS_BASE_TILE * (DE__CLOUDS_BASE_N / DE__CLOUDS_BASE_COLUMNS), "clouds.base"},
        {DE__CLOUDS_DETAIL_TILE * DE__CLOUDS_DETAIL_COLUMNS,
         DE__CLOUDS_DETAIL_TILE * (DE__CLOUDS_DETAIL_N / DE__CLOUDS_DETAIL_COLUMNS),
         "clouds.detail"},
        {DE__CLOUDS_WEATHER_N, DE__CLOUDS_WEATHER_N, "clouds.weather"},
    };
    de_gpu_texture *t[3] = {&c->base, &c->detail, &c->weather};
    for (u32 i = 0; i < 3u; ++i)
        *t[i] = de_gpu_texture_create(
            c->dev, &(de_gpu_texture_desc){.width = k_tex[i].w,
                                           .height = k_tex[i].h,
                                           .format = DE_FMT_RGBA8_UNORM,
                                           .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                           .name = k_tex[i].name});
    de_gpu_cmd *cmd = NULL;
    if (DE_HANDLE_VALID(c->base) && DE_HANDLE_VALID(c->detail) && DE_HANDLE_VALID(c->weather))
        cmd = de_gpu_cmd_begin(c->dev, DE_QUEUE_GRAPHICS, "clouds noise");
    de_result r = DE_ERR_INTERNAL;
    if (cmd != NULL) {
        de_gpu_barrier b[3];
        for (u32 i = 0; i < 3u; ++i)
            b[i] = tex_barrier(*t[i], DE_ACCESS_NONE, DE_ACCESS_SHADER_WRITE);
        de_gpu_cmd_barriers(cmd, b, 3);
        de_gpu_cmd_set_pipeline(cmd, c->pipe[CLOUDS_PIPE_NOISE]);
        for (u32 i = 0; i < 3u; ++i) {
            const noise_params pc = {.target = de_gpu_texture_bindless(c->dev, *t[i], 0, true),
                                     .mode = i,
                                     .width = k_tex[i].w,
                                     .height = k_tex[i].h};
            de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
            de_gpu_cmd_dispatch(cmd, (k_tex[i].w + 7u) / 8u, (k_tex[i].h + 7u) / 8u, 1u);
        }
        for (u32 i = 0; i < 3u; ++i)
            b[i] = tex_barrier(*t[i], DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        de_gpu_cmd_barriers(cmd, b, 3);
        const de_gpu_submit s = {.cmds = &cmd, .cmd_count = 1};
        r = de_gpu_queue_submit(c->dev, DE_QUEUE_GRAPHICS, &s);
    }
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        DE_ERROR(DE_LOG_CAT_GENERAL, "clouds: the noise textures: %s; clouds are off",
                 e != NULL ? e->message : "?");
        for (u32 i = 0; i < 3u; ++i)
            destroy_texture(c, t[i]);
        c->noise_failed = true;
        return false;
    }
    c->base_bindless = de_gpu_texture_bindless(c->dev, c->base, 0, false);
    c->detail_bindless = de_gpu_texture_bindless(c->dev, c->detail, 0, false);
    c->weather_bindless = de_gpu_texture_bindless(c->dev, c->weather, 0, false);
    c->noise_ready = true;
    return true;
}

de_result de__clouds_create(de_gpu_device *dev, const de_allocator *alloc, de__clouds **out) {
    if (out != NULL)
        *out = NULL;
    if (dev == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__clouds_create: NULL argument");
    if (alloc == NULL)
        alloc = de_heap_allocator();
    de__clouds *c = DE_NEW(alloc, de__clouds, DE_MEM_RENDER);
    if (c == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "clouds: no memory");
    memset(c, 0, sizeof *c);
    c->dev = dev;
    c->alloc = alloc;
    de_result r = DE_OK;
    for (u32 i = 0; i < CLOUDS_PIPES && r == DE_OK; ++i)
        r = load_pipe(c, i);
    if (r == DE_OK) {
        c->wrap = de_gpu_sampler_create(dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                    .mag = DE_FILTER_LINEAR,
                                                                    .mip = DE_FILTER_NEAREST,
                                                                    .u = DE_ADDR_WRAP,
                                                                    .v = DE_ADDR_WRAP,
                                                                    .w = DE_ADDR_WRAP});
        if (!DE_HANDLE_VALID(c->wrap))
            r = DE_FAIL_WRAP(DE_ERR_INTERNAL, "clouds: the sampler");
        else
            c->wrap_bindless = de_gpu_sampler_bindless(dev, c->wrap);
    }
    if (r != DE_OK) {
        de__clouds_destroy(c);
        return r;
    }
    *out = c;
    return DE_OK;
}

void de__clouds_destroy(de__clouds *c) {
    if (c == NULL)
        return;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        view_release(c, &c->views[i]);
    destroy_texture(c, &c->base);
    destroy_texture(c, &c->detail);
    destroy_texture(c, &c->weather);
    if (DE_HANDLE_VALID(c->wrap))
        de_gpu_sampler_destroy(c->dev, c->wrap);
    for (u32 i = 0; i < CLOUDS_PIPES; ++i)
        if (DE_HANDLE_VALID(c->pipe[i]))
            de_gpu_pipeline_destroy(c->dev, c->pipe[i]);
    DE_FREE(c->alloc, c, sizeof *c);
}

bool de__clouds_ready(const de__clouds *c) {
    if (c == NULL)
        return false;
    for (u32 i = 0; i < CLOUDS_PIPES; ++i)
        if (!DE_HANDLE_VALID(c->pipe[i]))
            return false;
    return true;
}

/* ---------- The constants ---------- */

static void put4(f32 o[4], de_vec3 v, f32 w) {
    o[0] = v.x;
    o[1] = v.y;
    o[2] = v.z;
    o[3] = w;
}

/* The frame's ring entry with the layer, noise and sun constants (both stages write these; the
 * view part is clouds.march's). */
static de__clouds_header *fill_common(const de__clouds *c, clouds_view *v,
                                      const de__clouds_frame *f, u32 entry) {
    de__clouds_header *h = (de__clouds_header *)(v->header_map + (size_t)entry * sizeof *h);
    const de__clouds_quality q = de__clouds_quality_for(f->settings->volumetrics);
    const de__clouds_settings *s = &f->clouds;
    const f32 planet = (f32)(DE__SKY_BOTTOM * 1000.0);
    const f64 dx = -f->drift[0], dy = -f->drift[1]; /* the clouds move with the wind */
    h->tex[0] = c->base_bindless;
    h->tex[1] = c->detail_bindless;
    h->tex[2] = c->weather_bindless;
    h->tex[3] = c->wrap_bindless;
    h->tex2[0] = f->transmittance;
    h->tex2[1] = f->sampler;
    h->tex2[2] = (u32)f->frame_index;
    h->tex2[3] = 0u;
    h->steps[0] = q.primary_steps;
    h->steps[1] = q.light_steps;
    h->steps[2] = q.octaves;
    h->steps[3] = q.shadow_steps;
    h->layer[0] = planet;
    h->layer[1] = planet + s->bottom_m;
    h->layer[2] = planet + s->top_m;
    h->layer[3] = (f32)fmax(f->camera_world[2], 1.0);
    h->shape[0] = s->coverage;
    h->shape[1] = s->density;
    h->shape[2] = s->erosion;
    h->shape[3] = 0.0f;
    h->off_wb[0] = de__clouds_tile_offset(f->camera_world[0], dx, DE__CLOUDS_WEATHER_TILE_M);
    h->off_wb[1] = de__clouds_tile_offset(f->camera_world[1], dy, DE__CLOUDS_WEATHER_TILE_M);
    h->off_wb[2] = de__clouds_tile_offset(f->camera_world[0], dx, DE__CLOUDS_BASE_TILE_M);
    h->off_wb[3] = de__clouds_tile_offset(f->camera_world[1], dy, DE__CLOUDS_BASE_TILE_M);
    h->off_d[0] = de__clouds_tile_offset(f->camera_world[0], dx, DE__CLOUDS_DETAIL_TILE_M);
    h->off_d[1] = de__clouds_tile_offset(f->camera_world[1], dy, DE__CLOUDS_DETAIL_TILE_M);
    h->off_d[2] = (f32)(1.0 / DE__CLOUDS_WEATHER_TILE_M);
    h->off_d[3] = (f32)(1.0 / DE__CLOUDS_BASE_TILE_M);
    h->misc[0] = (f32)(1.0 / DE__CLOUDS_DETAIL_TILE_M);
    h->misc[1] = 1.0f;
    h->misc[2] = f->camera.z;
    h->misc[3] = 0.0f;
    put4(h->sun, f->sun_dir, f->sun_illuminance);
    put4(h->camera, f->camera, 0.0f);
    return h;
}

static bool frame_ok(const de__clouds *c, const de__clouds_frame *f) {
    return c != NULL && f != NULL && f->slot < DE__VIEW_MAX && de__clouds_ready(c) &&
           de__clouds_enabled(&f->clouds, f->settings);
}

/* ---------- The passes ---------- */

typedef struct clouds_pass {
    de_gpu_device *dev;
    de_gpu_pipeline pipe;
    u32 gx, gy;
    de_rg_resource r[6]; /* per pass: see each exec */
    union {
        shadow_params shadow;
        march_params march;
        reconstruct_params reconstruct;
        composite_params composite;
    } pc;
    u32 pc_bytes;
} clouds_pass;

static u32 tex_index(const clouds_pass *p, const de_rg_pass *pass, de_rg_resource r, bool storage) {
    return DE_HANDLE_VALID(r) ? de_gpu_texture_bindless(p->dev, de_rg_texture(pass, r), 0, storage)
                              : DE_GPU_BINDLESS_INVALID;
}

static void dispatch(de_gpu_cmd *cmd, clouds_pass *p) {
    if (!DE_HANDLE_VALID(p->pipe))
        return;
    de_gpu_cmd_set_pipeline(cmd, p->pipe);
    de_gpu_cmd_push_constants(cmd, &p->pc, p->pc_bytes);
    de_gpu_cmd_dispatch(cmd, p->gx, p->gy, 1u);
}

static void shadow_exec(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    clouds_pass *p = (clouds_pass *)user;
    p->pc.shadow.target = tex_index(p, pass, p->r[0], true);
    dispatch(cmd, p);
}

static void march_exec(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    clouds_pass *p = (clouds_pass *)user;
    p->pc.march.depth = tex_index(p, pass, p->r[0], false);
    p->pc.march.color = tex_index(p, pass, p->r[1], true);
    p->pc.march.dist = tex_index(p, pass, p->r[2], true);
    dispatch(cmd, p);
}

static void reconstruct_exec(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    clouds_pass *p = (clouds_pass *)user;
    p->pc.reconstruct.march = tex_index(p, pass, p->r[0], false);
    p->pc.reconstruct.dist = tex_index(p, pass, p->r[1], false);
    p->pc.reconstruct.prev = tex_index(p, pass, p->r[2], false);
    p->pc.reconstruct.cur = tex_index(p, pass, p->r[3], true);
    dispatch(cmd, p);
}

static void composite_exec(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    clouds_pass *p = (clouds_pass *)user;
    p->pc.composite.clouds = tex_index(p, pass, p->r[0], false);
    p->pc.composite.depth = tex_index(p, pass, p->r[1], false);
    p->pc.composite.target = tex_index(p, pass, p->r[2], true);
    p->pc.composite.aerial = tex_index(p, pass, p->r[3], false);
    p->pc.composite.dist = tex_index(p, pass, p->r[4], false);
    dispatch(cmd, p);
}

static clouds_pass *new_pass(const de__clouds *c, de_arena *arena, u32 pipe, u32 bytes) {
    clouds_pass *p = (clouds_pass *)de_arena_push(arena, sizeof *p, 16);
    if (p == NULL)
        return NULL;
    memset(p, 0, sizeof *p);
    p->dev = c->dev;
    p->pipe = c->pipe[pipe];
    p->pc_bytes = bytes;
    return p;
}

u32 de__clouds_add_shadow_pass(de__clouds *c, de_rg *g, de_arena *arena, const de__clouds_frame *f,
                               de__clouds_shadow_output *out) {
    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (g == NULL || arena == NULL || !frame_ok(c, f))
        return 0u;
    clouds_view *v = &c->views[f->slot];
    if (!ensure_noise(c) || !view_ensure_common(c, v, f->slot))
        return 0u;
    const u32 entry = (u32)(f->frame_index % DE__VIS_FRAME_RING);
    (void)fill_common(c, v, f, entry);

    /* The map: centred on the camera, snapped to its texels in world space (no shimmer). */
    const f64 texel = (f64)DE__CLOUDS_SHADOW_EXTENT_M / (f64)DE__CLOUDS_SHADOW_N;
    const f64 sx = floor(f->camera_world[0] / texel) * texel - f->camera_world[0];
    const f64 sy = floor(f->camera_world[1] / texel) * texel - f->camera_world[1];
    const f32 area[4] = {f->camera.x + (f32)sx, f->camera.y + (f32)sy, DE__CLOUDS_SHADOW_EXTENT_M,
                         f->camera.z + (f32)((f64)f->clouds.bottom_m - f->camera_world[2])};
    de__cloud_shadow_gpu *sg =
        (de__cloud_shadow_gpu *)(v->shadow_map + (size_t)entry * sizeof(de__cloud_shadow_gpu));
    memset(sg, 0, sizeof *sg);
    sg->tex[0] = v->shadow_bindless;
    sg->tex[1] = f->sampler;
    sg->tex[2] = 1u;
    sg->area[0] = area[0];
    sg->area[1] = area[1];
    sg->area[2] = 1.0f / DE__CLOUDS_SHADOW_EXTENT_M;
    sg->area[3] = area[3];
    put4(sg->sun, f->sun_dir, f->sun_dir.z > 0.0f ? 1.0f : 0.0f);

    clouds_pass *p = new_pass(c, arena, CLOUDS_PIPE_SHADOW, (u32)sizeof(shadow_params));
    const de_rg_resource map = de_rg_import_texture(g, v->shadow, v->shadow_access);
    if (p == NULL || !DE_HANDLE_VALID(map)) {
        ++g->error_count;
        return 0u;
    }
    de_rg_set_name(g, map, "clouds.shadow");
    p->gx = (DE__CLOUDS_SHADOW_N + 7u) / 8u;
    p->gy = (DE__CLOUDS_SHADOW_N + 7u) / 8u;
    p->r[0] = map;
    p->pc.shadow = (shadow_params){.header = v->header_bindless,
                                   .entry = entry,
                                   .n = DE__CLOUDS_SHADOW_N,
                                   .area = {area[0], area[1], area[2], area[3]}};
    de_rg_pass *pass = de_rg_add_pass(g, "clouds.shadow", DE_QUEUE_GRAPHICS, shadow_exec, p);
    if (pass == NULL)
        return 0u;
    de_rg_write(pass, map, DE_ACCESS_SHADER_WRITE);
    de_rg_export(g, map, DE_ACCESS_SHADER_READ);
    v->shadow_access = DE_ACCESS_SHADER_READ;
    if (out != NULL) {
        out->texture = map;
        out->header = v->shadow_header_bindless;
        out->entry = entry;
    }
    return 1u;
}

u32 de__clouds_add_passes(de__clouds *c, de_rg *g, de_arena *arena, const de__clouds_inputs *in) {
    if (g == NULL || arena == NULL || in == NULL || !frame_ok(c, &in->frame) || in->width == 0u ||
        in->height == 0u || !DE_HANDLE_VALID(in->depth) || !DE_HANDLE_VALID(in->target))
        return 0u;
    const de_mat4 *pr = &in->proj;
    if (pr->m[11] == 0.0f && pr->m[15] != 0.0f)
        return 0u; /* orthographic: no clouds */
    const de__clouds_frame *f = &in->frame;
    clouds_view *v = &c->views[f->slot];
    if (!ensure_noise(c) || !view_ensure_history(c, v, f->slot, in->width, in->height))
        return 0u;
    const u32 entry = (u32)(f->frame_index % DE__VIS_FRAME_RING);
    de__clouds_header *h = fill_common(c, v, f, entry);
    const bool history = in->history_ok && v->frames > 0u;
    const u32 cur = (u32)(v->frames & 1u), prev = cur ^ 1u;
    const u32 mw = (in->width + 1u) / 2u, mh = (in->height + 1u) / 2u;

    /* The view part of the constants. */
    const de_mat4 ip = de_m4_inverse(pr);
    const de_vec4 a = de_m4_mul_v4(&ip, (de_vec4){-1.0f, 1.0f, 0.5f, 1.0f});
    const de_vec4 b = de_m4_mul_v4(&ip, (de_vec4){1.0f, -1.0f, 0.5f, 1.0f});
    f32 c00x = 0.0f, c00y = 0.0f, c11x = 0.0f, c11y = 0.0f;
    if (a.z != 0.0f && b.z != 0.0f) {
        c00x = -a.x / a.z;
        c00y = -a.y / a.z;
        c11x = -b.x / b.z;
        c11y = -b.y / b.z;
    }
    h->shape[3] = history ? DE__CLOUDS_HISTORY_WEIGHT : 0.0f;
    h->misc[1] = in->pre_exposure;
    h->sun[3] = f->sun_illuminance * in->pre_exposure;
    put4(h->ray00, in->ray00, 1.0f);
    put4(h->ray_du, in->ray_du, 0.0f);
    put4(h->ray_dv, in->ray_dv, 0.0f);
    h->vray[0] = c00x;
    h->vray[1] = c00y;
    h->vray[2] = c11x - c00x;
    h->vray[3] = c11y - c00y;
    h->proj[0] = pr->m[10];
    h->proj[1] = pr->m[14];
    h->proj[2] = pr->m[11];
    h->proj[3] = pr->m[15];
    for (u32 r = 0; r < 4u; ++r)
        for (u32 k = 0; k < 4u; ++k)
            h->prev_vp[r][k] = in->prev_view_proj.m[k * 4u + r];

    /* ---- The graph ---- */
    const de_rg_resource color = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.width = mw,
                                  .height = mh,
                                  .format = DE_FMT_RGBA16_FLOAT,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                  .name = "clouds.march"});
    const de_rg_resource dist = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.width = mw,
                                  .height = mh,
                                  .format = DE_FMT_R32_FLOAT,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                  .name = "clouds.distance"});
    const de_rg_resource hist_cur = de_rg_import_texture(g, v->history[cur], v->history_access[cur]);
    const de_rg_resource hist_prev =
        de_rg_import_texture(g, v->history[prev], v->history_access[prev]);
    clouds_pass *pm = new_pass(c, arena, CLOUDS_PIPE_MARCH, (u32)sizeof(march_params));
    clouds_pass *pr2 = new_pass(c, arena, CLOUDS_PIPE_RECONSTRUCT, (u32)sizeof(reconstruct_params));
    clouds_pass *pc = new_pass(c, arena, CLOUDS_PIPE_COMPOSITE, (u32)sizeof(composite_params));
    if (!DE_HANDLE_VALID(color) || !DE_HANDLE_VALID(dist) || !DE_HANDLE_VALID(hist_cur) ||
        !DE_HANDLE_VALID(hist_prev) || pm == NULL || pr2 == NULL || pc == NULL) {
        ++g->error_count;
        return 0u;
    }
    de_rg_set_name(g, hist_cur, "clouds.history");
    de_rg_set_name(g, hist_prev, "clouds.history_prev");
    const u32 hdr = v->header_bindless;
    const de__lighting_context *lc = in->lc;
    u32 added = 0;

    pm->gx = (mw + 7u) / 8u;
    pm->gy = (mh + 7u) / 8u;
    pm->r[0] = in->depth;
    pm->r[1] = color;
    pm->r[2] = dist;
    pm->pc.march = (march_params){.header = hdr,
                                  .entry = entry,
                                  .lview = lc != NULL ? lc->constants_bindless : 0u,
                                  .lentry = lc != NULL ? lc->constants_entry : 0u,
                                  .width = in->width,
                                  .height = in->height,
                                  .mw = mw,
                                  .mh = mh,
                                  .flags = lc != NULL ? 1u : 0u};
    de_rg_pass *pass = de_rg_add_pass(g, "clouds.march", DE_QUEUE_GRAPHICS, march_exec, pm);
    if (pass == NULL)
        return added;
    ++added;
    de_rg_read(pass, in->depth, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, color, DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass, dist, DE_ACCESS_SHADER_WRITE);

    pr2->gx = (in->width + 7u) / 8u;
    pr2->gy = (in->height + 7u) / 8u;
    pr2->r[0] = color;
    pr2->r[1] = dist;
    pr2->r[2] = hist_prev;
    pr2->r[3] = hist_cur;
    pr2->pc.reconstruct = (reconstruct_params){.header = hdr,
                                               .entry = entry,
                                               .width = in->width,
                                               .height = in->height,
                                               .mw = mw,
                                               .mh = mh,
                                               .sampler = f->sampler};
    pass = de_rg_add_pass(g, "clouds.reconstruct", DE_QUEUE_GRAPHICS, reconstruct_exec, pr2);
    if (pass == NULL)
        return added;
    ++added;
    de_rg_read(pass, color, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, dist, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, hist_prev, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, hist_cur, DE_ACCESS_SHADER_WRITE);

    pc->gx = (in->width + 7u) / 8u;
    pc->gy = (in->height + 7u) / 8u;
    pc->r[0] = hist_cur;
    pc->r[1] = in->depth;
    pc->r[2] = in->target;
    pc->r[3] = in->aerial;
    pc->r[4] = dist;
    f32 range = in->aerial_range_km;
    u32 range_bits = 0;
    memcpy(&range_bits, &range, sizeof range_bits);
    pc->pc.composite = (composite_params){.header = hdr,
                                          .entry = entry,
                                          .aerial_sampler = in->aerial_sampler,
                                          .aerial_range_bits = range_bits,
                                          .width = in->width,
                                          .height = in->height,
                                          .mw = mw,
                                          .mh = mh,
                                          .flags = in->aerial_on_opaque ? 1u : 0u};
    pass = de_rg_add_pass(g, "clouds.composite", DE_QUEUE_GRAPHICS, composite_exec, pc);
    if (pass == NULL)
        return added;
    ++added;
    de_rg_read(pass, hist_cur, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, in->depth, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, dist, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(in->aerial))
        de_rg_read(pass, in->aerial, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, in->target, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, in->target, DE_ACCESS_SHADER_WRITE);

    /* Both histories end the frame sampled; next frame imports them so. */
    de_rg_export(g, hist_cur, DE_ACCESS_SHADER_READ);
    de_rg_export(g, hist_prev, DE_ACCESS_SHADER_READ);
    v->history_access[cur] = v->history_access[prev] = DE_ACCESS_SHADER_READ;
    ++v->frames;
    return added;
}

#endif /* DE_HAS_RENDER */
