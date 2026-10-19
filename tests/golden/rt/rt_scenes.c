/* Procedural production-renderer RT goldens. Assets are shared with the S14 loader. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "rt_golden.h"
#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"
#include "render/views_internal.h"
#include "render/lighting/lighting.h"
#include "render/rt/rt_shadows.h"
#include "render/rt/rtao.h"
#include "render/rt/rt_reflections.h"
#include "render/rt/restir_di.h"
#include "render/rt/rt_gi_probes.h"
#include "render/rt/rt_refraction.h"
#include <dengine/de_debug.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rt_proof {
    de_golden_ctx *ctx;
    de_renderer *r;
    de_view view;
    u32 effect, frames, passes;
    bool base, negative, fallback, shadow_pass;
    de__lighting_inputs_gpu inputs;
    const de__lighting_view *uploaded;
} rt_proof;
static void set(const char *name, const char *value) {
    de_cvar c = de_cvar_find(name);
    if (c.id)
        (void)de_cvar_set(c, value);
    de_error_clear();
}
static void proof_stage(de__view_frame *vf, void *user) {
    rt_proof *p = user;
    if (vf->view.id != p->view.id)
        return;
    ++p->frames;
    de__lighting *l = de__views_lighting(p->r);
    const de__lighting_view_state *s = &l->views[(p->view.id & 255u) - 1u];
    if (s->constants_map)
        p->uploaded = (const de__lighting_view *)(s->constants_map +
                                                  (size_t)s->entry * sizeof(de__lighting_view));
    p->passes = 0;
    p->fallback = false;
    p->shadow_pass = false;
    for (u32 i = 0; i < vf->graph->pass_count; ++i) {
        const char *n = vf->graph->passes[i]->name;
        p->passes += strncmp(n, "RT.", 3) == 0;
        p->shadow_pass |= strcmp(n, "RT.Shadows") == 0;
        switch (p->effect) {
        case DE__GOLDEN_RT_SHADOWS:
        case DE__GOLDEN_RT_DI:
        case DE__GOLDEN_RT_FOLIAGE:
            p->fallback |= strstr(n, "shadow.") != NULL;
            break;
        case DE__GOLDEN_RT_AO:
            p->fallback |= strstr(n, "gtao.") != NULL;
            break;
        case DE__GOLDEN_RT_REFLECTIONS:
            p->fallback |= strstr(n, "ssr.") != NULL;
            break;
        case DE__GOLDEN_RT_GI:
            p->fallback |= strstr(n, "ssgi.") != NULL;
            break;
        case DE__GOLDEN_RT_REFRACTION:
            p->fallback |= strstr(n, "forward.") != NULL;
            break;
        }
    }
}
void *de__golden_rt_prepare(de_golden_ctx *ctx, de_renderer *r, de_view view, u32 effect,
                            bool base) {
    rt_proof *p = calloc(1, sizeof *p);
    if (!p) {
        de_golden_fail(ctx, "RT proof allocation");
        return NULL;
    }
    *p = (rt_proof){.ctx = ctx, .r = r, .view = view, .effect = effect, .base = base};
    de_cvar c = de_cvar_register(&(de_cvar_desc){.name = "r.rt.golden_sample",
                                                 .type = DE_CVAR_INT,
                                                 .default_number = -1,
                                                 .min = -1,
                                                 .max = 2147483647});
    (void)c;
    set("r.pipelined", "0");
    set("r.refraction", "1");
    set("r.gpu_profile", "0");
    set("r.rg_gpu_timers", "0");
    set("r.ambient_fallback", "0.15");
    set("r.rt.restir_di", effect == DE__GOLDEN_RT_DI ? "1" : "0");
    de_render_settings s =
        de_render_settings_preset(base ? DE_TIER_BASE : DE_TIER_RT, DE_QUALITY_HIGH);
    s.shadows = s.ao = s.reflections = s.gi = s.volumetrics = s.sss = DE_QUALITY_OFF;
    s.render_scale = 1.f;
    s.dynamic_res_target_ms = 0.f;
    s.rt_shadows = s.rt_ao = s.rt_reflections = s.rt_gi = s.rt_refraction = false;
    s.gi_mode = DE_GI_SCREEN_SPACE;
    switch (effect) {
    case DE__GOLDEN_RT_SHADOWS:
    case DE__GOLDEN_RT_FOLIAGE:
        s.shadows = DE_QUALITY_HIGH;
        s.rt_shadows = !base;
        break;
    case DE__GOLDEN_RT_DI:
        s.shadows = DE_QUALITY_HIGH;
        s.rt_shadows = !base;
        break;
    case DE__GOLDEN_RT_AO:
        s.ao = DE_QUALITY_HIGH;
        s.rt_ao = !base;
        break;
    case DE__GOLDEN_RT_REFLECTIONS:
        s.reflections = DE_QUALITY_HIGH;
        s.rt_reflections = !base;
        break;
    case DE__GOLDEN_RT_GI:
        s.gi = DE_QUALITY_HIGH;
        s.rt_gi = !base;
        s.gi_mode = base ? DE_GI_SCREEN_SPACE : DE_GI_RT;
        break;
    case DE__GOLDEN_RT_REFRACTION:
        s.reflections = DE_QUALITY_ULTRA;
        s.rt_refraction = !base;
        break;
    }
    const char *negative = getenv("DE_RT_GOLDEN_DISABLE_EFFECT");
    if (negative && strcmp(negative, "shadows") == 0 && effect == DE__GOLDEN_RT_SHADOWS) {
        s.shadows = DE_QUALITY_OFF;
        s.rt_shadows = false;
        p->negative = true;
    }
    de_renderer_set_settings(r, &s);
    if (de__view_register_stage(r, DE__VIEW_SLOT_TRANSPARENT, 99, "RT golden producer consumption",
                                proof_stage, p) != DE_OK)
        de_golden_fail(ctx, "RT proof stage");
    return p;
}
void de__golden_rt_sample(u32 sample) {
    char text[16];
    snprintf(text, sizeof text, "%u", sample);
    set("r.rt.golden_sample", text);
}
static f32 half_value(u16 h) {
    u32 e = (h >> 10) & 31u, m = h & 1023u;
    f32 v = e == 0    ? ldexpf((f32)m, -24)
            : e == 31 ? INFINITY
                      : ldexpf(1.f + (f32)m / 1024.f, (i32)e - 15);
    return h & 32768u ? -v : v;
}
static f32 packed_uf(u32 v, u32 mantissa) {
    u32 e = v >> mantissa, m = v & ((1u << mantissa) - 1u);
    return e == 0    ? ldexpf((f32)m, -14 - (i32)mantissa)
           : e == 31 ? INFINITY
                     : ldexpf(1.f + (f32)m / (f32)(1u << mantissa), (i32)e - 15);
}
void de__golden_rt_finish(void *state) {
    rt_proof *p = state;
    if (!p)
        return;
    de_renderer *r = p->r;
    de_gpu_device *d = r->dev;
    de_gpu_wait_idle(d);
    if (p->uploaded)
        p->inputs = p->uploaded->inputs;
    bool active = false, identity = false;
    de_gpu_texture texture = {0};
    de_gpu_buffer buffer = {0};
    u32 width = 0, height = 0, bpp = 0;
    u64 rays = 0;
    if (p->base || p->negative) {
        active = p->negative ? !p->shadow_pass : p->passes == 0;
        identity = p->negative || p->fallback;
    } else
        switch (p->effect) {
        case DE__GOLDEN_RT_SHADOWS:
        case DE__GOLDEN_RT_FOLIAGE: {
            de__rt_shadows_info i = {0};
            active = de__rt_shadows_get(r->rt_shadows, p->view, &i);
            texture = i.visibility;
            width = i.width;
            height = i.height * i.slots;
            bpp = 2;
            rays = i.rays;
            identity =
                p->inputs.rt_shadow_visibility == de_gpu_texture_bindless(d, texture, 0, false) &&
                p->inputs.rt_shadow_keys == de_gpu_buffer_bindless(d, i.keys);
            break;
        }
        case DE__GOLDEN_RT_AO: {
            de__rtao_info i = {0};
            active = de__rtao_get(r->rtao, p->view, &i);
            texture = i.ao;
            width = i.width;
            height = i.height;
            bpp = 1;
            rays = i.rays;
            identity = p->inputs.ao == de_gpu_texture_bindless(d, texture, 0, false);
            break;
        }
        case DE__GOLDEN_RT_REFLECTIONS: {
            de__rt_reflections_info i = {0};
            active = de__rt_reflections_get(r->rt_reflections, p->view, &i);
            texture = i.radiance;
            width = i.width;
            height = i.height;
            bpp = 8;
            rays = i.rays_traced;
            identity = p->inputs.indirect_specular == de_gpu_texture_bindless(d, texture, 0, false);
            break;
        }
        case DE__GOLDEN_RT_DI: {
            de__di_info i = {0};
            active = de__restir_di_get(r->restir_di, p->view, &i) && i.source_count >= 512;
            texture = i.diffuse;
            width = i.width;
            height = i.height;
            bpp = 16;
            identity =
                p->inputs.restir_diffuse == de_gpu_texture_bindless(d, texture, 0, false) &&
                p->inputs.restir_specular == de_gpu_texture_bindless(d, i.specular, 0, false);
            printf("  RT DI source_count=%u history=%u\n", i.source_count, i.history);
            break;
        }
        case DE__GOLDEN_RT_GI: {
            de__rt_gi_info i = {0};
            active = de__rt_gi_get(r->rt_gi, p->view, 0, &i) && i.probes_updated > 0;
            texture = i.irradiance;
            width = i.layout.iw;
            height = i.layout.ih;
            bpp = 4;
            rays = i.rays_traced;
            identity = p->inputs.reserved1 != 0;
            break;
        }
        case DE__GOLDEN_RT_REFRACTION: {
            de__rt_refraction_info i = {0};
            active = de__rt_refraction_get(r->rt_refraction, p->view, &i) && i.draws > 0;
            buffer = i.radiance;
            width = i.width;
            height = i.height;
            bpp = 16;
            rays = i.rays_traced;
            identity = i.events == 2;
            break;
        }
        }
    printf("  RT producer effect=%u base=%u negative=%u frames=%u passes=%u active=%u consumed=%u "
           "rays=%llu\n",
           p->effect, p->base, p->negative, p->frames, p->passes, active, identity,
           (unsigned long long)rays);
    if (p->frames != 32 || !active || !identity)
        de_golden_fail(p->ctx, "RT producer/consumer or fixed32 coverage");
    if ((texture.id || buffer.id) && width && height) {
        u32 pitch = (width * bpp + 255u) & ~255u;
        u64 size = buffer.id ? (u64)width * height * bpp : (u64)pitch * height;
        de_gpu_buffer rb = de_gpu_buffer_create(
            d, &(de_gpu_buffer_desc){.size = size,
                                     .usage = DE_USAGE_COPY_DST,
                                     .memory = DE_MEM_READBACK,
                                     .name = "RT golden actual producer readback"});
        de_gpu_cmd *cmd = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "RT golden signal proof");
        de_gpu_access before =
            p->effect == DE__GOLDEN_RT_GI ? DE__GI_ACCESS : DE_ACCESS_SHADER_READ;
        de_gpu_barrier b = {
            .texture = texture, .buffer = buffer, .before = before, .after = DE_ACCESS_COPY_SRC};
        de_gpu_cmd_barriers(cmd, &b, 1);
        if (buffer.id)
            de_gpu_cmd_copy_buffer(cmd, rb, 0, buffer, 0, size);
        else
            de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 0, pitch, texture, 0, 0);
        b.before = DE_ACCESS_COPY_SRC;
        b.after = before;
        de_gpu_cmd_barriers(cmd, &b, 1);
        (void)de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1});
        de_gpu_wait_idle(d);
        const u8 *bytes = de_gpu_buffer_map(d, rb);
        f64 energy = 0;
        f32 lo = INFINITY, hi = -INFINITY;
        u32 finite = 0, signal = 0;
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                const u8 *q =
                    bytes + (size_t)y * (buffer.id ? width * bpp : pitch) + (size_t)x * bpp;
                f32 v = 0;
                if (bpp == 1)
                    v = (f32)*q / 255.f;
                else if (bpp == 2)
                    v = half_value(*(const u16 *)q);
                else if (bpp == 4) {
                    u32 n = *(const u32 *)q;
                    v = packed_uf(n & 2047u, 6) + packed_uf((n >> 11) & 2047u, 6) +
                        packed_uf(n >> 22, 5);
                } else if (bpp == 8) {
                    const u16 *h = (const u16 *)q;
                    v = half_value(h[0]) + half_value(h[1]) + half_value(h[2]);
                } else {
                    const f32 *f = (const f32 *)q;
                    v = f[0] + f[1] + f[2];
                }
                if (isfinite(v)) {
                    ++finite;
                    energy += v;
                    if (v < lo)
                        lo = v;
                    if (v > hi)
                        hi = v;
                    signal += bpp <= 2 ? v < .99f : v > 1e-5f;
                }
            }
        printf("  typed actual producer %ux%u bpp%u finite=%u signal=%u range=[%g,%g] "
               "RGB_or_visibility_sum=%g\n",
               width, height, bpp, finite, signal, lo, hi, energy);
        if (finite != width * height || signal < 4 || (bpp <= 2 && hi - lo < .01f) ||
            (bpp > 2 && energy <= .001))
            de_golden_fail(p->ctx, "actual effect typed nonvacuity (RGB excludes alpha; occlusion "
                                   "range .01 / 4 affected pixels)");
        de_gpu_buffer_destroy(d, rb);
    }
    de__view_unregister_stage(r, proof_stage, p);
    set("r.rt.golden_sample", "-1");
    free(p);
}
#define RT_SCENE(name, kind)                                                                       \
    DE_GOLDEN_SCENE(name) {                                                                        \
        de__golden_rt_assets(ctx, "rt/" #name, kind, false);                                       \
    }                                                                                              \
    DE_GOLDEN_SCENE(name##_base) {                                                                 \
        de__golden_rt_assets(ctx, "rt/" #name, kind, true);                                        \
    }
RT_SCENE(rt_shadows_pole, DE__GOLDEN_RT_SHADOWS)
RT_SCENE(rt_restir_many_lights, DE__GOLDEN_RT_DI)
RT_SCENE(rt_ao_corner, DE__GOLDEN_RT_AO)
RT_SCENE(rt_reflections_mirror_rough, DE__GOLDEN_RT_REFLECTIONS)
RT_SCENE(rt_gi_cornell, DE__GOLDEN_RT_GI)
RT_SCENE(rt_refraction_glass, DE__GOLDEN_RT_REFRACTION)
RT_SCENE(rt_foliage_shadow, DE__GOLDEN_RT_FOLIAGE)
