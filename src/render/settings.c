/*
 * settings.c: render quality presets, the clamp to the device, the caps-only auto selection and
 * the r.* cvars (SPEC §12.8, §3.3, §5.8, R7; DENG-174).
 *
 * This file owns the preset API and the preset tables for the whole project. S20 (DENG-276)
 * routes de_render_settings_auto to a first-run GPU benchmark (src/render/preset/autodetect.c)
 * and may tune the table values below in place; de__render_auto_select stays its fallback.
 *
 * Tiers come from de_gpu_evaluate_tier (src/gpu/tier.c, DENG-290): it decides whether the device
 * meets Base and RT, and which tier to use by default (its default-to-Base table lists the Steam
 * Deck and Apple M1/M2). This file does not keep a second device list.
 */
#include "render/renderer_internal.h"

#include "platform/de_atomic.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---------- Preset tables ----------
 *
 * One table per tier, one row per quality Low..Ultra. A preset sets every field of
 * de_render_settings; settings that differ from all of them in any field are "Custom".
 *
 * Per-effect quality (shadows, ao, reflections, gi, volumetrics, sss) picks the cost of each
 * effect's Base technique (CSM resolution and filtering, GTAO samples, SSR + probes, probe GI)
 * and, at the RT tier, of the RT effect's denoiser and its fallback beyond the ray range.
 * Volumetrics and SSS are a step below the rest, because they are the most expensive per pixel.
 *
 * RT effects per RT row: Low ray-traces shadows; Medium adds AO; High adds reflections and GI;
 * Ultra adds refraction (SPEC §12.8 table).
 *
 * render_scale is the internal resolution before the upscaler (upscaler Auto in every preset):
 *   - Base: Low 0.67, Medium 0.77 (Steam Deck: 30 fps with upscaling, DENG-2 Q4), High and
 *     Ultra 1.0 (desktop Base: 1080p60 native, DENG-2 Q4).
 *   - RT: 0.5, 0.59, 0.67, 0.77 (the FSR performance, balanced, quality and ultra-quality ratios;
 *     RT High: 1440p60 with upscaling on an RTX 3070, DENG-2 Q4).
 * Every preset has dynamic resolution off, the path tracer off and 8 path bounces (the value the
 * path tracer starts from when it is turned on).
 */
typedef struct preset_row {
    de_quality shadows, ao, reflections, gi, volumetrics, sss;
    bool rt_shadows, rt_ao, rt_reflections, rt_gi, rt_refraction;
    f32 render_scale;
} preset_row;

#define Q_OFF DE_QUALITY_OFF
#define Q_LOW DE_QUALITY_LOW
#define Q_MED DE_QUALITY_MEDIUM
#define Q_HI  DE_QUALITY_HIGH
#define Q_ULT DE_QUALITY_ULTRA

/* Index: quality - DE_QUALITY_LOW. */
static const preset_row k_base_presets[4] = {
    /*          shadows ao     refl   gi     vol    sss    rt: sh  ao     refl   gi     refr */
    /* Low */ {Q_LOW, Q_LOW, Q_LOW, Q_LOW, Q_OFF, Q_OFF, false, false, false, false, false, 0.67f},
    /* Med */ {Q_MED, Q_MED, Q_MED, Q_MED, Q_LOW, Q_LOW, false, false, false, false, false, 0.77f},
    /* High */ {Q_HI, Q_HI, Q_HI, Q_HI, Q_MED, Q_MED, false, false, false, false, false, 1.0f},
    /* Ultra */ {Q_ULT, Q_ULT, Q_ULT, Q_ULT, Q_ULT, Q_ULT, false, false, false, false, false, 1.0f},
};

static const preset_row k_rt_presets[4] = {
    /* Low */ {Q_LOW, Q_LOW, Q_LOW, Q_LOW, Q_OFF, Q_OFF, true, false, false, false, false, 0.5f},
    /* Med */ {Q_MED, Q_MED, Q_MED, Q_MED, Q_LOW, Q_LOW, true, true, false, false, false, 0.59f},
    /* High */ {Q_HI, Q_HI, Q_HI, Q_HI, Q_MED, Q_MED, true, true, true, true, false, 0.67f},
    /* Ultra */ {Q_ULT, Q_ULT, Q_ULT, Q_ULT, Q_ULT, Q_ULT, true, true, true, true, true, 0.77f},
};

#define PRESET_PATH_BOUNCES 8u

de_render_settings de_render_settings_preset(de_tier tier, de_quality quality) {
    if (tier != DE_TIER_RT)
        tier = DE_TIER_BASE;
    if ((int)quality < (int)DE_QUALITY_LOW)
        quality = DE_QUALITY_LOW;
    if ((int)quality > (int)DE_QUALITY_ULTRA)
        quality = DE_QUALITY_ULTRA;
    const preset_row *p =
        &(tier == DE_TIER_RT ? k_rt_presets : k_base_presets)[quality - DE_QUALITY_LOW];
    de_render_settings s;
    memset(&s, 0, sizeof s); /* padding too: the same bytes on every call */
    s.tier = tier;
    s.shadows = p->shadows;
    s.ao = p->ao;
    s.reflections = p->reflections;
    s.gi = p->gi;
    s.volumetrics = p->volumetrics;
    s.sss = p->sss;
    s.rt_shadows = p->rt_shadows;
    s.rt_ao = p->rt_ao;
    s.rt_reflections = p->rt_reflections;
    s.rt_gi = p->rt_gi;
    s.rt_refraction = p->rt_refraction;
    s.upscaler = DE_UPSCALE_AUTO;
    s.render_scale = p->render_scale;
    s.dynamic_res_target_ms = 0.0f;
    s.path_tracer = false;
    s.path_max_bounces = PRESET_PATH_BOUNCES;
    return s;
}

/* Field by field (not memcmp: callers' padding is unspecified). */
bool de__render_settings_equal(const de_render_settings *a, const de_render_settings *b) {
    return a->tier == b->tier && a->shadows == b->shadows && a->ao == b->ao &&
           a->reflections == b->reflections && a->gi == b->gi && a->volumetrics == b->volumetrics &&
           a->sss == b->sss && a->rt_shadows == b->rt_shadows && a->rt_ao == b->rt_ao &&
           a->rt_reflections == b->rt_reflections && a->rt_gi == b->rt_gi &&
           a->rt_refraction == b->rt_refraction && a->upscaler == b->upscaler &&
           a->render_scale == b->render_scale &&
           a->dynamic_res_target_ms == b->dynamic_res_target_ms &&
           a->path_tracer == b->path_tracer && a->path_max_bounces == b->path_max_bounces;
}

bool de__render_settings_match(const de_render_settings *s, de_quality *quality) {
    if (s == NULL || (s->tier != DE_TIER_BASE && s->tier != DE_TIER_RT))
        return false;
    for (int q = DE_QUALITY_LOW; q <= DE_QUALITY_ULTRA; ++q) {
        const de_render_settings p = de_render_settings_preset(s->tier, (de_quality)q);
        if (de__render_settings_equal(s, &p)) {
            if (quality != NULL)
                *quality = (de_quality)q;
            return true;
        }
    }
    return false;
}

bool de_render_settings_is_preset(const de_render_settings *s) {
    return de__render_settings_match(s, NULL);
}

/* ---------- Clamp to the device ---------- */

bool de__render_rt_allowed(const de_gpu_caps *caps, const de_gpu_tier_report *report) {
    /* RT tier by choice also on API-only ray tracing (Apple M1/M2: "M1/M2 run the API at lower
     * tiers", SPEC §3.3); de_gpu_evaluate_tier's rt_ok (hardware only) decides the default. */
    return caps != NULL && report != NULL && report->base_ok && caps->raytracing != DE_RT_NONE &&
           caps->ray_query;
}

static de_quality clamp_quality(de_quality q) {
    return (int)q < (int)DE_QUALITY_OFF || (int)q > (int)DE_QUALITY_ULTRA ? DE_QUALITY_OFF : q;
}

void de__render_settings_clamp(const de_gpu_caps *caps, const de_gpu_tier_report *report,
                               de_render_settings *s) {
    if (s == NULL)
        return;
    if (s->tier != DE_TIER_RT || !de__render_rt_allowed(caps, report))
        s->tier = DE_TIER_BASE;
    s->shadows = clamp_quality(s->shadows);
    s->ao = clamp_quality(s->ao);
    s->reflections = clamp_quality(s->reflections);
    s->gi = clamp_quality(s->gi);
    s->volumetrics = clamp_quality(s->volumetrics);
    s->sss = clamp_quality(s->sss);
    if (s->tier == DE_TIER_BASE) {
        s->rt_shadows = s->rt_ao = s->rt_reflections = s->rt_gi = s->rt_refraction = false;
        s->path_tracer = false;
    }
    if ((int)s->upscaler < (int)DE_UPSCALE_NONE || (int)s->upscaler > (int)DE_UPSCALE_AUTO)
        s->upscaler = DE_UPSCALE_AUTO;
    if (isnan(s->render_scale))
        s->render_scale = 1.0f;
    if (s->render_scale < 0.33f)
        s->render_scale = 0.33f;
    if (s->render_scale > 1.0f)
        s->render_scale = 1.0f;
    if (isnan(s->dynamic_res_target_ms) || s->dynamic_res_target_ms < 0.0f)
        s->dynamic_res_target_ms = 0.0f;
    if (s->path_max_bounces < 1u)
        s->path_max_bounces = 1u;
    if (s->path_max_bounces > 64u)
        s->path_max_bounces = 64u;
}

/* ---------- Auto selection (caps only; DENG-276 adds the benchmark) ---------- */

void de__render_auto_select(const de_gpu_caps *caps, const de_gpu_tier_report *report,
                            de_tier *tier, de_quality *quality) {
    de_tier t = DE_TIER_BASE;
    de_quality q = DE_QUALITY_MEDIUM;
    if (caps != NULL && report != NULL) {
        /* The tier de_gpu_evaluate_tier recommends: RT only with hardware RT and not on its
         * default-to-Base list (Steam Deck, Apple M1/M2: SPEC R7). */
        if (report->recommended == DE_GPU_TIER_RT)
            t = DE_TIER_RT;
        /* High on discrete GPUs; Medium on the default-to-Base devices, API-only RT (M1/M2)
         * and integrated GPUs (unified memory: Apple silicon, APUs). */
        const bool listed = report->default_base_device[0] != '\0';
        if (!listed && !caps->unified_memory && caps->raytracing != DE_RT_API_ONLY)
            q = DE_QUALITY_HIGH;
    }
    if (tier != NULL)
        *tier = t;
    if (quality != NULL)
        *quality = q;
}

de_render_settings de_render_settings_auto(de_renderer *r) {
    if (r == NULL)
        return de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_MEDIUM);
    de_tier t;
    de_quality q;
    de__render_auto_select(&r->caps, &r->report, &t, &q);
    de_render_settings s = de_render_settings_preset(t, q);
    de__render_settings_clamp(&r->caps, &r->report, &s);
    return s;
}

/* ---------- Cvars ---------- */

static const char *const k_tier_names[] = {"auto", "base", "rt"};
static const char *const k_quality_sel_names[] = {"auto", "low",   "medium",
                                                  "high", "ultra", "custom"};
static const char *const k_quality_names[] = {"off", "low", "medium", "high", "ultra"};
static const char *const k_upscaler_names[] = {"none", "taa", "fsr", "metalfx", "auto"};

_Static_assert(sizeof k_tier_names / sizeof k_tier_names[0] == DE__RENDER_TIER_SEL_COUNT,
               "r.tier names");
_Static_assert(sizeof k_quality_sel_names / sizeof k_quality_sel_names[0] ==
                   DE__RENDER_QUALITY_SEL_COUNT,
               "r.quality names");
_Static_assert(sizeof k_quality_names / sizeof k_quality_names[0] == DE_QUALITY_ULTRA + 1,
               "quality names follow de_quality");
_Static_assert(sizeof k_upscaler_names / sizeof k_upscaler_names[0] == DE_UPSCALE_AUTO + 1,
               "upscaler names follow de_upscaler");

/* The renderer that reads and writes the cvars. */
static de_atomic_ptr g_cvar_owner;

static de_cvar reg_enum(const char *name, const char *help, const char *const *names, u32 count,
                        const char *def) {
    return de_cvar_register(&(de_cvar_desc){.name = name,
                                            .help = help,
                                            .type = DE_CVAR_ENUM,
                                            .flags = DE_CVAR_SAVED,
                                            .enum_values = names,
                                            .enum_count = count,
                                            .default_string = def});
}

static de_cvar reg_bool(const char *name, const char *help) {
    return de_cvar_register(
        &(de_cvar_desc){.name = name, .help = help, .type = DE_CVAR_BOOL, .flags = DE_CVAR_SAVED});
}

static de_cvar reg_number(const char *name, const char *help, de_cvar_type type, f64 def, f64 min,
                          f64 max) {
    return de_cvar_register(&(de_cvar_desc){.name = name,
                                            .help = help,
                                            .type = type,
                                            .flags = DE_CVAR_SAVED,
                                            .default_number = def,
                                            .min = min,
                                            .max = max});
}

#define QUALITY_HELP(effect) "Quality of " effect ": off, low, medium, high, ultra"

void de__render_cvars_init(de_renderer *r) {
    de_cvar *h = r->cvars.handle;
    h[DE__RCV_TIER] = reg_enum("r.tier", "Renderer tier: auto (from the GPU), base, rt",
                               k_tier_names, DE__RENDER_TIER_SEL_COUNT, "auto");
    h[DE__RCV_QUALITY] =
        reg_enum("r.quality",
                 "Quality preset: auto (from the GPU), low, medium, high, ultra, custom (the "
                 "per-effect cvars)",
                 k_quality_sel_names, DE__RENDER_QUALITY_SEL_COUNT, "auto");
    h[DE__RCV_SHADOWS] = reg_enum("r.shadow_quality", QUALITY_HELP("shadows"), k_quality_names,
                                  DE_QUALITY_ULTRA + 1, "medium");
    h[DE__RCV_AO] = reg_enum("r.ao_quality", QUALITY_HELP("ambient occlusion"), k_quality_names,
                             DE_QUALITY_ULTRA + 1, "medium");
    h[DE__RCV_REFLECTIONS] = reg_enum("r.reflection_quality", QUALITY_HELP("reflections"),
                                      k_quality_names, DE_QUALITY_ULTRA + 1, "medium");
    h[DE__RCV_GI] = reg_enum("r.gi_quality", QUALITY_HELP("global illumination"), k_quality_names,
                             DE_QUALITY_ULTRA + 1, "medium");
    h[DE__RCV_VOLUMETRICS] = reg_enum("r.volumetric_quality", QUALITY_HELP("volumetrics"),
                                      k_quality_names, DE_QUALITY_ULTRA + 1, "low");
    h[DE__RCV_SSS] = reg_enum("r.sss_quality", QUALITY_HELP("subsurface scattering"),
                              k_quality_names, DE_QUALITY_ULTRA + 1, "low");
    h[DE__RCV_RT_SHADOWS] = reg_bool("r.rt_shadows", "Ray-traced shadows (RT tier)");
    h[DE__RCV_RT_AO] = reg_bool("r.rt_ao", "Ray-traced ambient occlusion (RT tier)");
    h[DE__RCV_RT_REFLECTIONS] = reg_bool("r.rt_reflections", "Ray-traced reflections (RT tier)");
    h[DE__RCV_RT_GI] = reg_bool("r.rt_gi", "Ray-traced global illumination (RT tier)");
    h[DE__RCV_RT_REFRACTION] = reg_bool("r.rt_refraction", "Ray-traced refraction (RT tier)");
    h[DE__RCV_UPSCALER] = reg_enum("r.upscaler", "Upscaler: none, taa, fsr, metalfx, auto",
                                   k_upscaler_names, DE_UPSCALE_AUTO + 1, "auto");
    h[DE__RCV_RENDER_SCALE] =
        reg_number("r.render_scale", "Internal resolution as a fraction of the output, 0.33 to 1",
                   DE_CVAR_FLOAT, 1.0, 0.33, 1.0);
    h[DE__RCV_DYN_RES_MS] = reg_number("r.dynamic_res_target_ms",
                                       "Dynamic resolution GPU frame-time target in ms (0 = off)",
                                       DE_CVAR_FLOAT, 0.0, 0.0, 1000.0);
    h[DE__RCV_PATH_TRACER] = reg_bool("r.path_tracer", "Path-traced reference mode (RT tier)");
    h[DE__RCV_PATH_BOUNCES] = reg_number("r.path_max_bounces", "Path tracer bounces, 1 to 64",
                                         DE_CVAR_INT, PRESET_PATH_BOUNCES, 1.0, 64.0);
    void *expected = NULL;
    r->cvars.owner =
        de_atomic_cas_ptr(&g_cvar_owner, &expected, r, DE_ATOMIC_ACQ_REL, DE_ATOMIC_ACQUIRE);
}

void de__render_cvars_release(de_renderer *r) {
    if (!r->cvars.owner)
        return;
    void *expected = r;
    (void)de_atomic_cas_ptr(&g_cvar_owner, &expected, NULL, DE_ATOMIC_ACQ_REL, DE_ATOMIC_ACQUIRE);
    r->cvars.owner = false;
}

static f64 cvar_number(de_cvar cv, de__render_cvar_id id) {
    switch (id) {
    case DE__RCV_RT_SHADOWS:
    case DE__RCV_RT_AO:
    case DE__RCV_RT_REFLECTIONS:
    case DE__RCV_RT_GI:
    case DE__RCV_RT_REFRACTION:
    case DE__RCV_PATH_TRACER:
        return de_cvar_bool(cv) ? 1.0 : 0.0;
    case DE__RCV_RENDER_SCALE:
    case DE__RCV_DYN_RES_MS:
        return de_cvar_float(cv);
    default:
        return (f64)de_cvar_int(cv); /* enums: the index; ints */
    }
}

static void read_snapshot(const de_renderer *r, de__render_cvar_snapshot *out) {
    for (u32 i = 0; i < DE__RCV_COUNT; ++i)
        out->v[i] = cvar_number(r->cvars.handle[i], (de__render_cvar_id)i);
}

static de_quality snap_quality(const de__render_cvar_snapshot *s, de__render_cvar_id id) {
    return clamp_quality((de_quality)(int)s->v[id]);
}

/* The tier the selection resolves to on this device. */
static de_tier resolve_tier(const de_renderer *r, u32 sel) {
    if (sel == DE__RENDER_TIER_SEL_RT)
        return de__render_rt_allowed(&r->caps, &r->report) ? DE_TIER_RT : DE_TIER_BASE;
    if (sel == DE__RENDER_TIER_SEL_BASE)
        return DE_TIER_BASE;
    de_tier t;
    de__render_auto_select(&r->caps, &r->report, &t, NULL);
    return t;
}

static de_quality resolve_quality(const de_renderer *r, u32 sel) {
    if (sel >= (u32)DE_QUALITY_LOW && sel <= (u32)DE_QUALITY_ULTRA)
        return (de_quality)sel;
    de_quality q;
    de__render_auto_select(&r->caps, &r->report, NULL, &q);
    return q;
}

/* Settings from the per-effect cvars ("custom"), at tier `t`. */
static de_render_settings from_cvars(const de__render_cvar_snapshot *s, de_tier t) {
    de_render_settings o;
    memset(&o, 0, sizeof o);
    o.tier = t;
    o.shadows = snap_quality(s, DE__RCV_SHADOWS);
    o.ao = snap_quality(s, DE__RCV_AO);
    o.reflections = snap_quality(s, DE__RCV_REFLECTIONS);
    o.gi = snap_quality(s, DE__RCV_GI);
    o.volumetrics = snap_quality(s, DE__RCV_VOLUMETRICS);
    o.sss = snap_quality(s, DE__RCV_SSS);
    o.rt_shadows = s->v[DE__RCV_RT_SHADOWS] != 0.0;
    o.rt_ao = s->v[DE__RCV_RT_AO] != 0.0;
    o.rt_reflections = s->v[DE__RCV_RT_REFLECTIONS] != 0.0;
    o.rt_gi = s->v[DE__RCV_RT_GI] != 0.0;
    o.rt_refraction = s->v[DE__RCV_RT_REFRACTION] != 0.0;
    o.upscaler = (de_upscaler)(int)s->v[DE__RCV_UPSCALER];
    o.render_scale = (f32)s->v[DE__RCV_RENDER_SCALE];
    o.dynamic_res_target_ms = (f32)s->v[DE__RCV_DYN_RES_MS];
    o.path_tracer = s->v[DE__RCV_PATH_TRACER] != 0.0;
    o.path_max_bounces = (u32)s->v[DE__RCV_PATH_BOUNCES];
    return o;
}

de_render_settings de__render_cvars_initial(de_renderer *r) {
    if (!r->cvars.owner)
        return de_render_settings_auto(r);
    de__render_cvar_snapshot snap;
    read_snapshot(r, &snap);
    const u32 qsel = (u32)snap.v[DE__RCV_QUALITY];
    const de_tier t = resolve_tier(r, (u32)snap.v[DE__RCV_TIER]);
    de_render_settings s = qsel == DE__RENDER_QUALITY_CUSTOM
                               ? from_cvars(&snap, t)
                               : de_render_settings_preset(t, resolve_quality(r, qsel));
    de__render_settings_clamp(&r->caps, &r->report, &s);
    return s;
}

bool de__render_cvars_sync(de_renderer *r) {
    if (!r->cvars.owner)
        return false;
    de__render_cvar_snapshot now;
    read_snapshot(r, &now);
    if (memcmp(&now, &r->cvars.seen, sizeof now) == 0)
        return false;
    const de__render_cvar_snapshot *old = &r->cvars.seen;
    const u32 tsel = (u32)now.v[DE__RCV_TIER], qsel = (u32)now.v[DE__RCV_QUALITY];
    const bool selection_changed =
        tsel != (u32)old->v[DE__RCV_TIER] || qsel != (u32)old->v[DE__RCV_QUALITY];
    const de_tier t = resolve_tier(r, tsel);
    de_render_settings s;
    u32 publish = 0;
    if (qsel != DE__RENDER_QUALITY_CUSTOM && selection_changed) {
        s = de_render_settings_preset(t, resolve_quality(r, qsel));
    } else {
        /* Custom, or a per-effect cvar changed under a preset: the per-effect cvars decide, and
         * r.quality names the preset they now equal, or "custom". */
        s = from_cvars(&now, t);
        publish |= DE__RCV_PUBLISH_QUALITY;
    }
    de__render_settings_clamp(&r->caps, &r->report, &s);
    const bool changed = !de__render_settings_equal(&s, &r->settings);
    r->settings = s;
    de__render_cvars_publish(r, publish);
    return changed;
}

/* Sets cvar `id` to `text` when it holds another value (no work, no log for equal values). */
static void put(de_renderer *r, de__render_cvar_id id, f64 value, const char *text) {
    const de_cvar cv = r->cvars.handle[id];
    if (!DE_HANDLE_VALID(cv) || cvar_number(cv, id) == value)
        return;
    if (de_cvar_set(cv, text) != DE_OK)
        DE_WARN(r->log, "renderer: cannot set cvar %u to \"%s\"", (unsigned)id, text);
}

static void put_quality(de_renderer *r, de__render_cvar_id id, de_quality q) {
    put(r, id, (f64)q, k_quality_names[clamp_quality(q)]);
}

static void put_bool(de_renderer *r, de__render_cvar_id id, bool v) {
    put(r, id, v ? 1.0 : 0.0, v ? "1" : "0");
}

static void put_float(de_renderer *r, de__render_cvar_id id, f32 v) {
    const de_cvar cv = r->cvars.handle[id];
    /* Compared as f32: the cvar keeps the f64 of the decimal text, which is not (f64)v. */
    if (!DE_HANDLE_VALID(cv) || (f32)de_cvar_float(cv) == v)
        return;
    char text[32];
    snprintf(text, sizeof text, "%.9g", (double)v); /* round-trips an f32 */
    if (de_cvar_set(cv, text) != DE_OK)
        DE_WARN(r->log, "renderer: cannot set cvar %u to \"%s\"", (unsigned)id, text);
}

void de__render_cvars_publish(de_renderer *r, u32 what) {
    if (!r->cvars.owner)
        return;
    const de_render_settings *s = &r->settings;
    if (what & DE__RCV_PUBLISH_TIER) {
        const u32 sel = s->tier == DE_TIER_RT ? DE__RENDER_TIER_SEL_RT : DE__RENDER_TIER_SEL_BASE;
        put(r, DE__RCV_TIER, (f64)sel, k_tier_names[sel]);
    }
    if (what & DE__RCV_PUBLISH_QUALITY) {
        de_quality q;
        const u32 sel = de__render_settings_match(s, &q) ? (u32)q : DE__RENDER_QUALITY_CUSTOM;
        put(r, DE__RCV_QUALITY, (f64)sel, k_quality_sel_names[sel]);
    }
    put_quality(r, DE__RCV_SHADOWS, s->shadows);
    put_quality(r, DE__RCV_AO, s->ao);
    put_quality(r, DE__RCV_REFLECTIONS, s->reflections);
    put_quality(r, DE__RCV_GI, s->gi);
    put_quality(r, DE__RCV_VOLUMETRICS, s->volumetrics);
    put_quality(r, DE__RCV_SSS, s->sss);
    put_bool(r, DE__RCV_RT_SHADOWS, s->rt_shadows);
    put_bool(r, DE__RCV_RT_AO, s->rt_ao);
    put_bool(r, DE__RCV_RT_REFLECTIONS, s->rt_reflections);
    put_bool(r, DE__RCV_RT_GI, s->rt_gi);
    put_bool(r, DE__RCV_RT_REFRACTION, s->rt_refraction);
    const u32 up = (u32)s->upscaler <= (u32)DE_UPSCALE_AUTO ? (u32)s->upscaler : DE_UPSCALE_AUTO;
    put(r, DE__RCV_UPSCALER, (f64)up, k_upscaler_names[up]);
    put_float(r, DE__RCV_RENDER_SCALE, s->render_scale);
    put_float(r, DE__RCV_DYN_RES_MS, s->dynamic_res_target_ms);
    put_bool(r, DE__RCV_PATH_TRACER, s->path_tracer);
    char bounces[16];
    snprintf(bounces, sizeof bounces, "%u", s->path_max_bounces);
    put(r, DE__RCV_PATH_BOUNCES, (f64)s->path_max_bounces, bounces);
    read_snapshot(r, &r->cvars.seen);
}
