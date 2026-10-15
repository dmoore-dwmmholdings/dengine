/*
 * sss.c: screen-space separable subsurface scattering with the Burley profile (sss.h; SPEC §12.6;
 * DENG-306): the profile in double precision (the reference of burley_profile.slang), the kernel
 * tables, and the view stage that adds the passes of shaders/sss/sss_separable.slang.
 */
#include "render/passes/sss/sss.h"

#include "render/lighting/lighting.h"
#include "render/passes/vis/gbuffer.h"
#include "render/passes/vis/vis_stages.h"
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"

#include <dengine/de_debug.h>

#include <math.h>
#include <string.h>

/* =================================================================================================
 * The profile
 * ============================================================================================== */

f64 de__burley_scale(f64 albedo) {
    const f64 a = albedo < 0.0 ? 0.0 : albedo > 1.0 ? 1.0 : albedo;
    const f64 t = fabs(a - 0.8);
    return 1.85 - a + 7.0 * t * t * t;
}

f64 de__burley_d(f64 mfp, f64 albedo) {
    return (mfp > 0.0 ? mfp : 0.0) / de__burley_scale(albedo);
}

f64 de__burley_profile(f64 r, f64 d) {
    if (!(d > 0.0) || !(r > 0.0))
        return 0.0;
    return (exp(-r / d) + exp(-r / (3.0 * d))) / (8.0 * 3.14159265358979323846 * d * r);
}

f64 de__burley_cdf(f64 r, f64 d) {
    if (!(d > 0.0))
        return 1.0;
    const f64 x = (r > 0.0 ? r : 0.0) / d;
    return 1.0 - 0.25 * exp(-x) - 0.75 * exp(-x / 3.0);
}

f64 de__burley_inverse_unit(f64 u) {
    if (!(u > 0.0))
        return 0.0;
    if (u >= 1.0)
        return 1e30;
    /* Bisection: the distribution is monotonic; 200 steps reach double precision. */
    f64 lo = 0.0, hi = 1.0;
    while (de__burley_cdf(hi, 1.0) < u)
        hi *= 2.0;
    for (u32 i = 0; i < 200; ++i) {
        const f64 mid = 0.5 * (lo + hi);
        if (de__burley_cdf(mid, 1.0) < u)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

/* =================================================================================================
 * Kernels
 * ============================================================================================== */

static u32 clamp_taps(u32 taps) {
    if (taps < DE__SSS_TAPS_MIN)
        taps = DE__SSS_TAPS_MIN;
    if (taps > DE__SSS_TAPS_MAX)
        taps = DE__SSS_TAPS_MAX;
    return taps | 1u;
}

void de__sss_kernel_build(u32 taps, de__sss_bin *out) {
    taps = clamp_taps(taps);
    const u32 bins = DE__SSS_BINS(taps);
    /* Mass boundaries of the folded distribution: the centre bin holds one tap's share, every
     * other bin two (one tap on each side). */
    const f64 share = 1.0 / (f64)taps;
    for (u32 k = 0; k < bins; ++k) {
        const f64 u_lo = k == 0 ? 0.0 : share + (f64)(k - 1u) * 2.0 * share;
        const f64 u_hi = k + 1u == bins ? 1.0 : share + (f64)k * 2.0 * share;
        de__sss_bin *b = &out[k];
        b->lo = (f32)de__burley_inverse_unit(u_lo);
        b->hi = k + 1u == bins ? 1e30f : (f32)de__burley_inverse_unit(u_hi);
        b->pos = k == 0 ? 0.0f : (f32)de__burley_inverse_unit(0.5 * (u_lo + u_hi));
        b->reserved = 0.0f;
    }
}

u32 de__sss_kernel_first(u32 taps) {
    const u32 bins = DE__SSS_BINS(clamp_taps(taps));
    return (bins - 1u) * bins / 2u - 1u; /* 2 + 3 + ... + (bins - 1) */
}

static de__sss_bin g_tables[DE__SSS_TABLE_BINS];
static bool g_tables_built;

const de__sss_bin *de__sss_kernel_all(void) {
    if (!g_tables_built) { /* deterministic, so a race only writes the same values twice */
        for (u32 taps = DE__SSS_TAPS_MIN; taps <= DE__SSS_TAPS_MAX; taps += 2u)
            de__sss_kernel_build(taps, &g_tables[de__sss_kernel_first(taps)]);
        g_tables_built = true;
    }
    return g_tables;
}

void de__sss_kernel_weights(const de__sss_bin *bins, u32 count, const f64 d[3], f64 (*w)[3]) {
    f64 d_ref = d[0];
    if (d[1] > d_ref)
        d_ref = d[1];
    if (d[2] > d_ref)
        d_ref = d[2];
    for (u32 k = 0; k < count; ++k)
        for (u32 c = 0; c < 3; ++c)
            w[k][c] = de__burley_cdf((f64)bins[k].hi * d_ref, d[c]) -
                      de__burley_cdf((f64)bins[k].lo * d_ref, d[c]);
}

u32 de__sss_taps(de_quality q, i64 samples) {
    if (q == DE_QUALITY_OFF)
        return 0u;
    if (samples > 0)
        return clamp_taps(samples > (i64)DE__SSS_TAPS_MAX ? DE__SSS_TAPS_MAX : (u32)samples);
    switch (q) {
    case DE_QUALITY_LOW:
        return 11u;
    case DE_QUALITY_MEDIUM:
        return 17u;
    default:
        return 25u;
    }
}

DE_STATIC_ASSERT(sizeof(de__sss_bin) == 16, "a bin is one float4");

/* =================================================================================================
 * The stage
 * ============================================================================================== */

/** sss_separable.slang SssParams (112 bytes). */
typedef struct ss_params {
    u32 scene, view_slot, depth, gb0, gb1, gb2, diffuse, temp, params, lighting, ids, pitch;
    u32 visible, visible_count, tiles, counter, args, kernel, kernel_first, bins;
    u32 width, height, tiles_x, flags;
    f32 max_px;
    u32 reserved0, reserved1, reserved2;
} ss_params;
DE_STATIC_ASSERT(sizeof(ss_params) == 112, "SssParams is 112 bytes");

static const de__sss_const k_consts[] = {
    {"DE_SSS_TILE", DE__SSS_TILE},
    {"DE_SSS_GROUP", 64u}, /* DE__SSS_TILE squared */
    {"DE_SSS_FOLLOW_SURFACE", DE__SSS_FOLLOW_SURFACE},
    {"DE_SSS_MAX_BINS", DE__SSS_BINS(DE__SSS_TAPS_MAX)},
    {"DE_SSS_COUNTER_TILES", 0u},
};

const de__sss_const *de__sss_layout_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

u32 de__sss_params_bytes(void) {
    return (u32)sizeof(ss_params);
}

enum { SS_CLEAR, SS_TILES, SS_ARGS, SS_BLUR_H, SS_BLUR_V, SS_KERNELS };

struct de__sss {
    de__vis_shared *vis;
    de__lighting *lighting;
    de__vis_kernel k[SS_KERNELS];
    de_gpu_buffer table; /**< every kernel table (DE__SSS_TABLE_BINS float4), SHADER_READ */
    u32 table_bindless;
    bool enabled;
    de_cvar cv_samples, cv_follow, cv_max_px;
};

/* The tables into `ss->table`: an UPLOAD storage buffer written once by the host and read through
 * bindless, as lighting's constants are (8 KiB). No copy and no barrier: a fresh GPU-only buffer
 * filled by a copy can reuse memory an earlier upload's copy read, which Vulkan's synchronization
 * validation reports as a write-after-read hazard (a buffer barrier from DE_ACCESS_NONE waits for
 * no earlier command). */
static de_result table_upload(de__sss *ss) {
    de_gpu_device *dev = ss->vis->dev;
    const u64 bytes = (u64)DE__SSS_TABLE_BINS * sizeof(de__sss_bin);
    ss->table = de_gpu_buffer_create(dev, &(de_gpu_buffer_desc){.size = bytes,
                                                                .usage = DE_USAGE_STORAGE,
                                                                .memory = DE_MEM_UPLOAD,
                                                                .stride = (u32)sizeof(de__sss_bin),
                                                                .name = "sss kernels"});
    u8 *map = NULL;
    if (!DE_HANDLE_VALID(ss->table) || (map = (u8 *)de_gpu_buffer_map(dev, ss->table)) == NULL)
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "sss: the kernel tables");
    memcpy(map, de__sss_kernel_all(), (size_t)bytes);
    ss->table_bindless = de_gpu_buffer_bindless(dev, ss->table);
    return DE_OK;
}

de_result de__sss_create_ex(de__vis_shared *vis, de__sss **out) {
    *out = NULL;
    if (vis == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__sss_create_ex: no vis frame");
    de__sss *ss = DE_NEW(vis->alloc, de__sss, DE_MEM_RENDER);
    if (ss == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__sss_create_ex: no memory");
    memset(ss, 0, sizeof *ss);
    ss->vis = vis;
    ss->cv_samples = de_cvar_register(&(de_cvar_desc){
        .name = "r.sss_samples",
        .help =
            "Subsurface scattering: kernel taps per direction (odd, 3 to 63); 0 = by the view's "
            "sss quality (11 low, 17 medium, 25 high and ultra)",
        .type = DE_CVAR_INT,
        .default_number = 0,
        .min = 0,
        .max = DE__SSS_TAPS_MAX});
    ss->cv_follow = de_cvar_register(&(de_cvar_desc){
        .name = "r.sss_follow_surface",
        .help =
            "Subsurface scattering: taps fade to the centre with the depth difference (1), or are "
            "rejected beyond the kernel's reach (0)",
        .type = DE_CVAR_BOOL,
        .default_number = 1});
    ss->cv_max_px = de_cvar_register(
        &(de_cvar_desc){.name = "r.sss_max_radius_px",
                        .help = "Subsurface scattering: the farthest tap in pixels (close-ups)",
                        .type = DE_CVAR_FLOAT,
                        .default_number = 64,
                        .min = 1,
                        .max = 1024});
    de_error_clear();
    de_result r = DE_OK;
    if (vis->enabled) {
        static const char *const k_entries[SS_KERNELS] = {
            "cs_sss_clear", "cs_sss_tiles", "cs_sss_args", "cs_sss_blur_h", "cs_sss_blur_v"};
        static const char *const k_names[SS_KERNELS] = {"sss clear", "sss tiles", "sss args",
                                                        "sss blur h", "sss blur v"};
        static const u32 g1[3] = {1u, 1u, 1u}, g8[3] = {8u, 8u, 1u}, g64[3] = {64u, 1u, 1u};
        const u32 *groups[SS_KERNELS] = {g1, g8, g1, g64, g64};
        de__engine_shader_code code;
        r = de__vis_shader_load(vis, "sss_separable", k_entries, SS_KERNELS, &code);
        for (u32 i = 0; i < SS_KERNELS && r == DE_OK; ++i)
            r = de__vis_kernel_create(vis, &code, i, k_names[i], groups[i], (u32)sizeof(ss_params),
                                      &ss->k[i]);
        if (r == DE_OK || r != DE_ERR_UNSUPPORTED)
            de__engine_shader_code_free(&code);
        if (r == DE_OK)
            r = table_upload(ss);
        if (r == DE_OK) {
            ss->enabled = true;
        } else if (r == DE_ERR_UNSUPPORTED) {
            const de_error *e = de_error_last();
            DE_WARN(vis->log, "sss: subsurface scattering is off: %s",
                    e != NULL ? e->message : "no engine shaders");
            de_error_clear();
            r = DE_OK;
        }
    }
    if (r != DE_OK) {
        de__sss_destroy(ss);
        return r;
    }
    *out = ss;
    return DE_OK;
}

void de__sss_destroy(de__sss *ss) {
    if (ss == NULL)
        return;
    for (u32 i = 0; i < SS_KERNELS; ++i)
        de__vis_kernel_destroy(ss->vis, &ss->k[i]);
    if (DE_HANDLE_VALID(ss->table))
        de_gpu_buffer_destroy(ss->vis->dev, ss->table);
    DE_FREE(ss->vis->alloc, ss, sizeof *ss);
}

bool de__sss_enabled(const de__sss *ss) {
    return ss != NULL && ss->enabled;
}

/* ---- Passes ---- */

typedef struct ss_pass {
    de_gpu_device *dev;
    const de__vis_kernel *kernel;
    ss_params pc;
    u32 groups_x, groups_y; /**< a direct dispatch; 0: indirect over the listed tiles */
    bool targets_storage;   /**< temp and params are written (blur_h), else sampled */
    de_rg_resource depth, gb[3], diffuse, temp, params, lighting, ids, visible, tiles, counter,
        args, scene;
} ss_pass;

static u32 tex(const ss_pass *p, const de_rg_pass *pass, de_rg_resource r, bool storage) {
    return DE_HANDLE_VALID(r) ? de_gpu_texture_bindless(p->dev, de_rg_texture(pass, r), 0, storage)
                              : 0u;
}
static u32 buf(const ss_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return DE_HANDLE_VALID(r) ? de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, r)) : 0u;
}

static void ss_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ss_pass *p = (const ss_pass *)user;
    ss_params pc = p->pc;
    pc.scene = buf(p, pass, p->scene);
    pc.depth = tex(p, pass, p->depth, false);
    pc.gb0 = tex(p, pass, p->gb[0], false);
    pc.gb1 = tex(p, pass, p->gb[1], false);
    pc.gb2 = tex(p, pass, p->gb[2], false);
    pc.diffuse = tex(p, pass, p->diffuse, false);
    /* blur_h writes temp and params (storage), blur_v reads them (sampled). */
    pc.temp = tex(p, pass, p->temp, p->targets_storage);
    pc.params = tex(p, pass, p->params, p->targets_storage);
    pc.lighting = tex(p, pass, p->lighting, true);
    pc.ids = buf(p, pass, p->ids);
    pc.visible = buf(p, pass, p->visible);
    pc.tiles = buf(p, pass, p->tiles);
    pc.counter = buf(p, pass, p->counter);
    pc.args = buf(p, pass, p->args);
    de_gpu_cmd_set_pipeline(cmd, p->kernel->pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    if (p->groups_x != 0)
        de_gpu_cmd_dispatch(cmd, p->groups_x, p->groups_y, 1);
    else
        de_gpu_cmd_dispatch_indirect(cmd, de_rg_buffer(pass, p->args), 0);
}

static ss_pass *pass_data(de__vis_context *vc, const ss_pass *base) {
    ss_pass *p = (ss_pass *)de_arena_push(vc->arena, sizeof *p, 16);
    if (p != NULL)
        *p = *base;
    else
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sss: the frame arena is full");
    return p;
}

de_result de__sss_build(de__sss *ss, de_rg *g, de__vis_context *vc, const de__lighting_context *lc,
                        de_quality q, de__sss_context **out) {
    if (out != NULL)
        *out = NULL;
    if (ss == NULL || g == NULL || vc == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__sss_build: NULL argument");
    const u32 taps =
        de__sss_taps(q, DE_HANDLE_VALID(ss->cv_samples) ? de_cvar_int(ss->cv_samples) : 0);
    if (taps == 0 || !ss->enabled || lc == NULL || !lc->lit || !DE_HANDLE_VALID(lc->diffuse) ||
        !DE_HANDLE_VALID(lc->target) || !DE_HANDLE_VALID(vc->depth) ||
        !DE_HANDLE_VALID(vc->vis_ids_buffer) || !DE_HANDLE_VALID(vc->scene_views) ||
        vc->view_slot >= DE__VIEW_MAX || vc->width == 0 || vc->height == 0)
        return DE_OK; /* off, or nothing lit this frame */
    for (u32 t = DE__GB0; t <= DE__GB2; ++t)
        if (!DE_HANDLE_VALID(vc->gbuffer[t]))
            return DE_OK;
    de__sss_context *sc = (de__sss_context *)de_arena_push_zero(vc->arena, sizeof *sc, 16);
    if (sc == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__sss_build: the frame arena is full");
    sc->taps = taps;
    const u32 tiles_x = (vc->width + DE__SSS_TILE - 1u) / DE__SSS_TILE;
    const u32 tiles_y = (vc->height + DE__SSS_TILE - 1u) / DE__SSS_TILE;
    sc->tiles = de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = (u64)tiles_x * tiles_y * 4u,
                                                             .usage = DE_USAGE_STORAGE,
                                                             .memory = DE_MEM_GPU_ONLY,
                                                             .name = "sss.tiles"});
    sc->counter = de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = 16u,
                                                               .usage = DE_USAGE_STORAGE,
                                                               .memory = DE_MEM_GPU_ONLY,
                                                               .name = "sss.counter"});
    sc->args = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = 16u,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT | DE_USAGE_COPY_SRC,
                                 .memory = DE_MEM_GPU_ONLY,
                                 .name = "sss.args"});
    const de_gpu_texture_desc td = {.format = DE_FMT_RGBA16_FLOAT,
                                    .width = vc->width,
                                    .height = vc->height,
                                    .usage =
                                        DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC};
    de_gpu_texture_desc tt = td, tp = td;
    tt.name = "sss.temp";
    tp.name = "sss.params";
    sc->temp = de_rg_create_texture(g, &tt);
    sc->params = de_rg_create_texture(g, &tp);
    if (!DE_HANDLE_VALID(sc->tiles) || !DE_HANDLE_VALID(sc->counter) ||
        !DE_HANDLE_VALID(sc->args) || !DE_HANDLE_VALID(sc->temp) || !DE_HANDLE_VALID(sc->params))
        return DE_OK; /* error set; finalize reports it */

    const bool follow = !DE_HANDLE_VALID(ss->cv_follow) || de_cvar_bool(ss->cv_follow);
    const f64 max_px = DE_HANDLE_VALID(ss->cv_max_px) ? de_cvar_float(ss->cv_max_px) : 64.0;
    ss_pass base = {.dev = ss->vis->dev,
                    .pc = {.view_slot = vc->view_slot,
                           .pitch = vc->vis_ids_pitch,
                           .visible_count = vc->visible_capacity + vc->visible_alpha_capacity,
                           .kernel = ss->table_bindless, /* host-written, not in the graph */
                           .kernel_first = de__sss_kernel_first(taps),
                           .bins = DE__SSS_BINS(taps),
                           .width = vc->width,
                           .height = vc->height,
                           .tiles_x = tiles_x,
                           .flags = follow ? DE__SSS_FOLLOW_SURFACE : 0u,
                           .max_px = max_px > 0.0 ? (f32)max_px : 64.0f}};
    /* Each pass names only the resources it declares (de_rg_* need a declared access). */

    /* sss.clear */
    ss_pass b = base;
    b.kernel = &ss->k[SS_CLEAR];
    b.groups_x = b.groups_y = 1u;
    b.counter = sc->counter;
    ss_pass *pd = pass_data(vc, &b);
    if (pd == NULL)
        return DE_OK;
    de_rg_pass *p = de_rg_add_pass(g, "sss.clear", DE_QUEUE_GRAPHICS, ss_execute, pd);
    de_rg_write(p, sc->counter, DE_ACCESS_SHADER_WRITE);

    /* sss.tiles */
    b = base;
    b.kernel = &ss->k[SS_TILES];
    b.groups_x = tiles_x;
    b.groups_y = tiles_y;
    b.gb[2] = vc->gbuffer[DE__GB2];
    b.counter = sc->counter;
    b.tiles = sc->tiles;
    if ((pd = pass_data(vc, &b)) == NULL)
        return DE_OK;
    p = de_rg_add_pass(g, "sss.tiles", DE_QUEUE_GRAPHICS, ss_execute, pd);
    de_rg_read(p, vc->gbuffer[DE__GB2], DE_ACCESS_SHADER_READ);
    de_rg_read(p, sc->counter, DE_ACCESS_SHADER_READ); /* atomics */
    de_rg_write(p, sc->counter, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, sc->tiles, DE_ACCESS_SHADER_WRITE);

    /* sss.args */
    b = base;
    b.kernel = &ss->k[SS_ARGS];
    b.groups_x = b.groups_y = 1u;
    b.counter = sc->counter;
    b.args = sc->args;
    if ((pd = pass_data(vc, &b)) == NULL)
        return DE_OK;
    p = de_rg_add_pass(g, "sss.args", DE_QUEUE_GRAPHICS, ss_execute, pd);
    de_rg_read(p, sc->counter, DE_ACCESS_SHADER_READ);
    de_rg_write(p, sc->args, DE_ACCESS_SHADER_WRITE);

    /* sss.blur_h: indirect over the listed tiles. */
    b = base;
    b.kernel = &ss->k[SS_BLUR_H];
    b.tiles = sc->tiles;
    b.args = sc->args;
    b.depth = vc->depth;
    b.gb[0] = vc->gbuffer[DE__GB0];
    b.gb[1] = vc->gbuffer[DE__GB1];
    b.gb[2] = vc->gbuffer[DE__GB2];
    b.diffuse = lc->diffuse;
    b.temp = sc->temp;
    b.params = sc->params;
    b.ids = vc->vis_ids_buffer;
    b.visible = vc->visible_meshlets;
    b.scene = vc->scene_views;
    b.targets_storage = true;
    if ((pd = pass_data(vc, &b)) == NULL)
        return DE_OK;
    p = de_rg_add_pass(g, "sss.blur_h", DE_QUEUE_GRAPHICS, ss_execute, pd);
    de_rg_read(p, sc->args, DE_ACCESS_INDIRECT | DE_ACCESS_SHADER_READ);
    de_rg_read(p, sc->tiles, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->depth, DE_ACCESS_SHADER_READ);
    for (u32 t = DE__GB0; t <= DE__GB2; ++t)
        de_rg_read(p, vc->gbuffer[t], DE_ACCESS_SHADER_READ);
    de_rg_read(p, lc->diffuse, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->vis_ids_buffer, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(vc->visible_meshlets))
        de_rg_read(p, vc->visible_meshlets, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(vc->scene_instances))
        de_rg_read(p, vc->scene_instances, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(vc->scene_meshes))
        de_rg_read(p, vc->scene_meshes, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(vc->scene_materials))
        de_rg_read(p, vc->scene_materials, DE_ACCESS_SHADER_READ);
    de_rg_write(p, sc->temp, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, sc->params, DE_ACCESS_SHADER_WRITE);

    /* sss.blur_v: the same tiles; recombines into the lighting target. */
    b.kernel = &ss->k[SS_BLUR_V];
    b.gb[0] = (de_rg_resource){0};
    b.ids = b.visible = (de_rg_resource){0};
    b.lighting = lc->target;
    b.targets_storage = false;
    if ((pd = pass_data(vc, &b)) == NULL)
        return DE_OK;
    p = de_rg_add_pass(g, "sss.blur_v", DE_QUEUE_GRAPHICS, ss_execute, pd);
    de_rg_read(p, sc->args, DE_ACCESS_INDIRECT | DE_ACCESS_SHADER_READ);
    de_rg_read(p, sc->tiles, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->depth, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->gbuffer[DE__GB1], DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->gbuffer[DE__GB2], DE_ACCESS_SHADER_READ);
    de_rg_read(p, lc->diffuse, DE_ACCESS_SHADER_READ);
    de_rg_read(p, sc->temp, DE_ACCESS_SHADER_READ);
    de_rg_read(p, sc->params, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_read(p, lc->target, DE_ACCESS_SHADER_READ);
    de_rg_write(p, lc->target, DE_ACCESS_SHADER_WRITE);
    if (out != NULL)
        *out = sc;
    return DE_OK;
}

/* The view stage: after lighting, when the view's sss quality is on. */
static void sss_stage(de__view_frame *vf, void *user) {
    de__sss *ss = (de__sss *)user;
    if (!de__sss_enabled(ss) || vf->settings == NULL || vf->settings->sss == DE_QUALITY_OFF)
        return; /* no dispatch at all */
    de__vis_context *vc = de__vis_frame_context(ss->vis, vf->view, vf->graph, vf->frame_index);
    const de__lighting_context *lc =
        de__lighting_frame_context(ss->lighting, vf->view, vf->graph, vf->frame_index);
    if (vc == NULL || lc == NULL)
        return;
    const de_result r = de__sss_build(ss, vf->graph, vc, lc, vf->settings->sss, NULL);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        DE_ERROR(ss->vis->log, "sss: view \"%s\": %s", vf->name,
                 e != NULL ? e->message : de_result_str(r));
    }
}

de_result de__sss_create(de_renderer *r, de__vis_shared *vis, de__lighting *lt, de__sss **out) {
    *out = NULL;
    if (vis == NULL || lt == NULL)
        return DE_OK; /* no vis frame or lighting: nothing to scatter */
    de__sss *ss = NULL;
    de_result res = de__sss_create_ex(vis, &ss);
    if (res != DE_OK)
        return res;
    ss->lighting = lt;
    res = de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, DE__SSS_STAGE_ORDER, "sss", sss_stage,
                                  ss);
    if (res != DE_OK) {
        de__sss_destroy(ss);
        return res;
    }
    *out = ss;
    return DE_OK;
}
