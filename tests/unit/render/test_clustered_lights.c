/*
 * test_clustered_lights.c: clustered lighting (DENG-303): the froxel cluster lists, the light
 * units, the area lights, the emissive proxies and the deferred pass over the thin G-buffer.
 * Executable test_render_clustered_lights; CTest names render_clustered_lights.<suite>.<test>.
 *
 * GPU tests: a fake render world drives a GPU scene of planes (meshlet patches facing +z, the
 * camera 10 m above looking down) and lights; the whole vis frame runs (with the resolve's debug
 * position target), then de__lighting_build, and a test pass reads back the lighting target, the
 * G-buffer, the split targets and the cluster lists. The CPU reference shades each pixel from its
 * decoded G-buffer (materials with specular 0 and metallic 0, for which the Standard model is
 * exactly Lambert), the light units of light_data.c, and the cluster bounds and light tests of
 * light_data.c. Area lights are compared with a numerical integration of the form factor.
 * The GPU tests run per backend (_metal, _vulkan, _d3d12; SKIP without the device or the engine
 * shaders; CI: lavapipe and WARP).
 *
 * CPU tests: units, the LTC polygon integral, the tables, the cluster grid, a conservative cluster
 * test over random lights and points, and the shader constants and struct sizes.
 */
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

#include "lighting_fixture.h"

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

/* BSDF x cos of the test materials for light from world direction `wi`. The GPU tests' materials
 * have metallic 0 and specular 0, so the Standard model's dielectric F0 and F90 are 0: no specular
 * lobe, and the diffuse coupling (1 - Ed) is 1. The model is then exactly Lambert, base / pi, and
 * the references do not depend on the model's specular details (DENG-299 and later layers). */
static f32 ref_eval(const surf *s, f32 base, const f32 wi[3]) {
    const f32 c = wi[0] * s->n[0] + wi[1] * s->n[1] + wi[2] * s->n[2];
    return c > 0.0f ? base / (f32)PI_D * c : 0.0f;
}

/* A punctual light of the test (shading units: candela, or lux for directional). */
typedef struct ref_light {
    u32 kind;
    f32 pos[3], dir[3]; /* dir: where it points */
    f32 intensity;      /* cd (point, spot) or lux */
    f32 range;          /* 0: derived */
    f32 inner, outer;   /* spot half-angles */
    f32 radius;         /* source radius */
} ref_light;

/* Illuminance (lux at normal incidence) and direction towards the light at point p. */
static f32 ref_illuminance(const ref_light *l, const f32 p[3], f32 cutoff, f32 wi[3]) {
    if (l->kind == DE__LIGHT_DIRECTIONAL) {
        for (u32 k = 0; k < 3; ++k)
            wi[k] = -l->dir[k];
        return l->intensity;
    }
    f32 v[3] = {l->pos[0] - p[0], l->pos[1] - p[1], l->pos[2] - p[2]};
    const f32 d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (u32 k = 0; k < 3; ++k)
        wi[k] = v[k] / d;
    const f32 r = l->range > 0.0f ? l->range : de__light_derived_range(l->intensity, cutoff);
    if (!(d < r))
        return 0.0f;
    f32 d2 = d * d;
    if (d2 < l->radius * l->radius)
        d2 = l->radius * l->radius;
    f32 e = l->intensity * de__light_window(d, r) / d2;
    if (l->kind == DE__LIGHT_SPOT)
        e *= de__light_spot_factor(-(wi[0] * l->dir[0] + wi[1] * l->dir[1] + wi[2] * l->dir[2]),
                                   l->inner, l->outer);
    return e;
}

/* The reference radiance of surface s from the lights (white). */
static f32 ref_radiance(const surf *s, const ref_light *ls, u32 n, f32 base) {
    f32 sum = 0.0f;
    for (u32 i = 0; i < n; ++i) {
        f32 wi[3];
        const f32 e = ref_illuminance(&ls[i], s->pos, 0.05f, wi);
        if (e > 0.0f)
            sum += e * ref_eval(s, base, wi);
    }
    return sum;
}

/* Relative error with an absolute floor. */
static bool close_to(f32 got, f32 want, f32 rel, f32 floor_abs) {
    return fabsf(got - want) <= rel * fabsf(want) + floor_abs;
}

static de_affine pointing(f32 x, f32 y, f32 z, de_vec3 axis, f32 angle) {
    return trs((de_vec3){x, y, z}, axis, angle, 1.0f);
}

/* ================================================================================================
 * GPU tests
 * ============================================================================================== */

/* lights_point_lumens_to_lux: a 1000 lm point light 1 m above a white Lambert-like plane: the
 * radiance is the Standard model's with 1000 / 4 pi cd (79.6 lux at the foot) within 1%. */
static void point_lumens(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, material_add(1.0f, 1.0f, 0.0f, 0.0f));
    world_light(&f->w, 0,
                &(DeLight){.kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = 1000.0f},
                at(0, 0, 1));
    lx_run(f);
    const lx_result *r = &f->res;
    const ref_light L = {
        .kind = DE__LIGHT_POINT, .pos = {0, 0, 1}, .intensity = 1000.0f / (4.0f * (f32)PI_D)};
    u32 checked = 0, bad = 0;
    f32 worst = 0.0f, foot_e = 0.0f;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        surf s;
        if (!surf_at(r, i, &s))
            continue;
        const f32 want = ref_radiance(&s, &L, 1, s.base);
        const f32 got = r->lighting[4 * i];
        if (want < 0.05f)
            continue;
        ++checked;
        const f32 e = fabsf(got - want) / want;
        worst = e > worst ? e : worst;
        bad += e > 0.01f;
        if (fabsf(s.pos[0]) < 0.03f && fabsf(s.pos[1]) < 0.03f) {
            /* The illuminance the GPU's radiance implies, moved to exactly 1 m. */
            f32 wi[3];
            const f32 e_here = ref_illuminance(&L, s.pos, 0.05f, wi);
            const f32 d2 = s.pos[0] * s.pos[0] + s.pos[1] * s.pos[1] + 1.0f;
            foot_e = got / ref_eval(&s, s.base, wi) * d2 *
                     (e_here > 0.0f ? (L.intensity / d2) / e_here : 1.0f);
        }
    }
    printf("point: %u px, worst %.3f%%; illuminance at the foot %.2f lux (79.58 expected)\n",
           checked, 100.0 * worst, foot_e);
    EXPECT_GT(checked, 1000u);
    EXPECT_EQ(bad, 0u);
    EXPECT_TRUE(fabsf(foot_e - 79.577f) <= 0.01f * 79.577f);
    EXPECT_EQ(r->counters[DE__LIGHTING_COUNTER_LOCAL / 4u], 1u);
}

/* lights_sun_lux: a 100000 lux directional light 30 degrees off the normal. */
static void sun_lux(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, material_add(0.5f, 0.7f, 0.0f, 0.0f));
    const de_affine xf = pointing(0, 0, 5, (de_vec3){1, 0, 0}, 30.0f * (f32)PI_D / 180.0f);
    world_light(
        &f->w, 0,
        &(DeLight){.kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = 100000.0f}, xf);
    lx_run(f);
    const lx_result *r = &f->res;
    /* The light points along -Z of its transform. */
    ref_light L = {.kind = DE__LIGHT_DIRECTIONAL, .intensity = 100000.0f};
    L.dir[0] = -xf.m[6];
    L.dir[1] = -xf.m[7];
    L.dir[2] = -xf.m[8];
    u32 checked = 0, bad = 0;
    f32 worst = 0.0f;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        surf s;
        if (!surf_at(r, i, &s))
            continue;
        const f32 want = ref_radiance(&s, &L, 1, s.base);
        const f32 got = r->lighting[4 * i];
        ++checked;
        const f32 e = fabsf(got - want) / want;
        worst = e > worst ? e : worst;
        bad += e > 0.01f;
    }
    printf("sun: %u px, worst %.3f%%\n", checked, 100.0 * worst);
    EXPECT_GT(checked, 1000u);
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(r->counters[DE__LIGHTING_COUNTER_DIRECTIONAL / 4u], 1u);
    EXPECT_EQ(r->counters[DE__LIGHTING_COUNTER_LOCAL / 4u], 0u);
}

/* lights_spot_cone: a spot (2000 lm, half-angles 15 and 25 degrees) 3 m above the plane: full
 * inside the inner cone, nothing outside the outer one, the t^2 falloff between. */
static void spot_cone(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, material_add(0.8f, 0.6f, 0.0f, 0.0f));
    const f32 inner = 15.0f * (f32)PI_D / 180.0f, outer = 25.0f * (f32)PI_D / 180.0f;
    world_light(&f->w, 0,
                &(DeLight){.kind = DE_LIGHT_SPOT,
                           .color = {1, 1, 1},
                           .intensity = 2000.0f,
                           .spot_inner = inner,
                           .spot_outer = outer},
                at(0, 0, 3));
    lx_run(f);
    const lx_result *r = &f->res;
    const ref_light L = {.kind = DE__LIGHT_SPOT,
                         .pos = {0, 0, 3},
                         .dir = {0, 0, -1},
                         .intensity = de__light_spot_candela(2000.0f, outer),
                         .inner = inner,
                         .outer = outer};
    u32 inside = 0, outside = 0, between = 0, bad = 0;
    f32 peak = 0.0f;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        surf s;
        if (!surf_at(r, i, &s))
            continue;
        const f32 ang = atan2f(sqrtf(s.pos[0] * s.pos[0] + s.pos[1] * s.pos[1]), 3.0f);
        const f32 want = ref_radiance(&s, &L, 1, s.base);
        const f32 got = r->lighting[4 * i];
        peak = want > peak ? want : peak;
        if (ang > outer + 0.01f) {
            ++outside;
            bad += got != 0.0f;
        } else if (ang < inner - 0.01f) {
            ++inside;
            bad += !close_to(got, want, 0.01f, 0.0f);
        } else {
            ++between;
            bad += !close_to(got, want, 0.01f, 0.002f * peak);
        }
    }
    printf("spot: %u px inside, %u between, %u outside; peak %.2f nits; %u bad\n", inside, between,
           outside, peak, bad);
    EXPECT_GT(inside, 200u);
    EXPECT_GT(between, 100u);
    EXPECT_GT(outside, 1000u);
    EXPECT_EQ(bad, 0u);
}

/* lights_range_cutoff: a point light with range 2 m lights nothing beyond it (the window goes to
 * 0 there), and a 1 lm light without range ends at its derived range sqrt(I / cutoff). */
static void range_cutoff(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, material_add(0.8f, 0.6f, 0.0f, 0.0f));
    world_light(
        &f->w, 0,
        &(DeLight){.kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = 1000.0f, .range = 2.0f},
        at(-3, 0, 1));
    world_light(&f->w, 1, &(DeLight){.kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = 1.0f},
                at(3, 0, 0.5f));
    lx_run(f);
    const lx_result *r = &f->res;
    const ref_light L[2] = {
        {.kind = DE__LIGHT_POINT,
         .pos = {-3, 0, 1},
         .intensity = 1000.0f / (4.0f * (f32)PI_D),
         .range = 2.0f},
        {.kind = DE__LIGHT_POINT, .pos = {3, 0, 0.5f}, .intensity = 1.0f / (4.0f * (f32)PI_D)}};
    const f32 derived = de__light_derived_range(L[1].intensity, 0.05f);
    printf("derived range of 1 lm at 0.05 lux: %.3f m\n", derived);
    EXPECT_TRUE(fabsf(derived - sqrtf(1.0f / (4.0f * (f32)PI_D) / 0.05f)) < 1e-4f);
    u32 beyond = 0, within = 0, bad = 0;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        surf s;
        if (!surf_at(r, i, &s))
            continue;
        f32 d[2];
        for (u32 k = 0; k < 2; ++k) {
            const f32 v[3] = {L[k].pos[0] - s.pos[0], L[k].pos[1] - s.pos[1],
                              L[k].pos[2] - s.pos[2]};
            d[k] = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        }
        const f32 got = r->lighting[4 * i];
        if (d[0] > 2.0f + 0.01f && d[1] > derived + 0.01f) {
            ++beyond;
            bad += got != 0.0f;
        } else {
            ++within;
            const f32 want = ref_radiance(&s, L, 2, s.base);
            bad += !close_to(got, want, 0.01f, 1e-3f);
        }
    }
    printf("range: %u px within, %u beyond; %u bad\n", within, beyond, bad);
    EXPECT_GT(within, 500u);
    EXPECT_GT(beyond, 5000u);
    EXPECT_EQ(bad, 0u);
}

/* The form factor (cosine-weighted solid angle / pi) of an emitter of corners
 * c + x u + y v (u, v in [-1, 1] x half extents) emitting towards `front`, at point p with normal
 * n: midpoint rule in double (or a disk of radius hx when `disk`). */
static f64 ff_numeric(const f32 c[3], const f32 ax[3], const f32 ay[3], f32 hx, f32 hy, bool disk,
                      const f32 front[3], const f32 p[3], const f32 n[3]) {
    const u32 N = 240;
    f64 sum = 0.0;
    const f64 da = (2.0 * hx / N) * (2.0 * hy / N);
    for (u32 j = 0; j < N; ++j)
        for (u32 i = 0; i < N; ++i) {
            const f64 u = -hx + (i + 0.5) * 2.0 * hx / N, v = -hy + (j + 0.5) * 2.0 * hy / N;
            if (disk && u * u + v * v > (f64)hx * hx)
                continue;
            f64 q[3], w[3];
            for (u32 k = 0; k < 3; ++k) {
                q[k] = c[k] + u * ax[k] + v * ay[k];
                w[k] = q[k] - p[k];
            }
            const f64 r2 = w[0] * w[0] + w[1] * w[1] + w[2] * w[2], rl = sqrt(r2);
            const f64 cp = (w[0] * n[0] + w[1] * n[1] + w[2] * n[2]) / rl;
            const f64 cl = -(w[0] * front[0] + w[1] * front[1] + w[2] * front[2]) / rl;
            if (cp > 0.0 && cl > 0.0)
                sum += cp * cl / r2 * da;
        }
    return sum / PI_D;
}

/* lights_ltc_rect_vs_reference: a 1 x 0.5 m rect and a 0.8 m disk light (1000 nits) above a
 * white diffuse plane (specular 0: no LTC specular): the radiance is L x the form factor of a
 * numerical integration within 2%. */
static void ltc_rect(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, material_add(1.0f, 1.0f, 0.0f, 0.0f));
    world_light(&f->w, 0,
                &(DeLight){.kind = DE_LIGHT_RECT,
                           .color = {1, 1, 1},
                           .intensity = 1000.0f,
                           .area_size = {1.0f, 0.5f}},
                at(-2.0f, 0, 1));
    world_light(&f->w, 1,
                &(DeLight){.kind = DE_LIGHT_DISK,
                           .color = {1, 1, 1},
                           .intensity = 1000.0f,
                           .area_size = {0.8f, 0.8f}},
                at(2.0f, 0.5f, 0.8f));
    lx_run(f);
    const lx_result *r = &f->res;
    const f32 front[3] = {0, 0, -1}, ax[3] = {1, 0, 0}, ay[3] = {0, 1, 0};
    const f32 c0[3] = {-2.0f, 0, 1}, c1[3] = {2.0f, 0.5f, 0.8f};
    const f32 range0 = de__light_derived_range(1000.0f * 0.5f, 0.05f);
    const f32 range1 = de__light_derived_range(1000.0f * (f32)PI_D * 0.16f, 0.05f);
    u32 checked = 0, bad = 0, n = 0;
    f32 worst = 0.0f;
    for (size_t i = 0; i < (size_t)r->width * r->height; i += 7) {
        surf s;
        if (!surf_at(r, i, &s))
            continue;
        if (++n % 3u != 0u)
            continue; /* a subset: the integration is slow */
        f32 d0[3], d1[3];
        for (u32 k = 0; k < 3; ++k) {
            d0[k] = s.pos[k] - c0[k];
            d1[k] = s.pos[k] - c1[k];
        }
        const f32 w0 =
            de__light_window(sqrtf(d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2]), range0);
        const f32 w1 =
            de__light_window(sqrtf(d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2]), range1);
        const f64 ff = w0 * ff_numeric(c0, ax, ay, 0.5f, 0.25f, false, front, s.pos, s.n) +
                       w1 * ff_numeric(c1, ax, ay, 0.4f, 0.4f, true, front, s.pos, s.n);
        const f32 want = (f32)(1000.0 * ff) * s.base;
        if (want < 5.0f)
            continue;
        const f32 got = r->lighting[4 * i];
        ++checked;
        const f32 e = fabsf(got - want) / want;
        worst = e > worst ? e : worst;
        bad += e > 0.02f;
    }
    printf("area lights: %u px, worst %.3f%% against the numerical form factor\n", checked,
           100.0 * worst);
    EXPECT_GT(checked, 100u);
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(r->counters[DE__LIGHTING_COUNTER_LOCAL / 4u], 2u);
}

/* lights_emissive_proxy: a bright emissive square (5000 nits, 0.5 m) 1.5 m above the plane gets a
 * proxy point light (an isotropic sphere emitter of the bounds' radius R: L pi R^2 cd) that lights
 * the plane; above r.emissive_light_threshold only. */
static void emissive_proxy(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, material_add(1.0f, 1.0f, 0.0f, 0.0f));
    const u32 tile_mesh = lx_mesh_plane(f, 1, 1, 0.5f);
    const u32 em = material_add(0.0f, 1.0f, 0.0f, 0.0f);
    g_mat[em - 1u].standard.emissive = (de_vec3){1.0f, 1.0f, 1.0f};
    g_mat[em - 1u].standard.emissive_strength = 5000.0f;
    world_set(&f->w, 1, tile_mesh, at(0.0f, 0.0f, 1.5f), em);
    lx_run(f);
    const lx_result *r = &f->res;
    EXPECT_EQ(r->counters[DE__LIGHTING_COUNTER_PROXIES / 4u], 1u);
    EXPECT_EQ(r->counters[DE__LIGHTING_COUNTER_LOCAL / 4u], 1u);
    const f32 radius = sqrtf(0.25f * 0.25f * 2.0f + 0.01f * 0.01f);
    const ref_light L = {.kind = DE__LIGHT_POINT,
                         .pos = {0, 0, 1.5f},
                         .intensity = 5000.0f * (f32)PI_D * radius * radius,
                         .radius = radius};
    u32 checked = 0, bad = 0;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        surf s;
        if (!surf_at(r, i, &s) || s.pos[2] > 0.5f)
            continue; /* the emitter itself */
        const f32 want = ref_radiance(&s, &L, 1, s.base);
        const f32 got = r->lighting[4 * i];
        ++checked;
        bad += !close_to(got, want, 0.015f, 1e-3f);
    }
    printf("emissive proxy: %u receiver px, %u bad (R %.3f m, %.0f cd)\n", checked, bad, radius,
           L.intensity);
    EXPECT_GT(checked, 1000u);
    EXPECT_EQ(bad, 0u);

    /* Below the threshold: no proxy, the plane stays dark. */
    ASSERT_TRUE(cvar_set("r.emissive_light_threshold", "5000"));
    lx_run(f);
    r = &f->res;
    EXPECT_EQ(r->counters[DE__LIGHTING_COUNTER_PROXIES / 4u], 0u);
    u32 lit = 0;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        surf s;
        if (surf_at(r, i, &s) && s.pos[2] < 0.5f)
            lit += r->lighting[4 * i] != 0.0f;
    }
    EXPECT_EQ(lit, 0u);
}

/* lighting_only_classified_tiles: the deferred pass lit exactly classification's tiles of each
 * lit model and none of Unlit's; Unlit pixels keep their emission, background pixels stay 0. */
static void only_classified(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    const u32 m_std = material_add(0.8f, 0.5f, 0.0f, 0.5f);
    const u32 m_unlit = material_add(0.4f, 0.5f, 0.0f, 0.5f);
    g_mat[m_unlit - 1u].standard.unlit = true;
    const u32 sq = lx_mesh_plane(f, 2, 2, 1.0f);
    world_set(&f->w, 0, sq, at(-1.5f, 0, 0), m_std);
    world_set(&f->w, 1, sq, at(1.5f, 0, 0), m_unlit);
    world_light(&f->w, 0,
                &(DeLight){.kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = 1000.0f},
                at(0, 0, 5));
    world_light(&f->w, 1,
                &(DeLight){.kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = 500.0f},
                at(1.5f, 0, 1));
    lx_run(f);
    const lx_result *r = &f->res;
    for (u32 m = 0; m < DE__GS_MATERIAL_MODELS; ++m)
        EXPECT_EQ(r->tiles[m], m == DE__GS_MATERIAL_MODEL_UNLIT ? 0u : r->args[m].count);
    EXPECT_GT(r->tiles[DE__GS_MATERIAL_MODEL_STANDARD], 0u);
    EXPECT_GT(r->args[DE__GS_MATERIAL_MODEL_UNLIT].count, 0u);
    const f32 unlit_lin = de__gb_srgb_decode(de__gb_srgb_encode(0.4f));
    u32 unlit = 0, lit = 0, background = 0, bad = 0;
    for (size_t i = 0; i < (size_t)r->width * r->height; ++i) {
        const f32 *L = &r->lighting[4 * i];
        if (!(L[3] > 0.5f)) {
            ++background;
            bad += L[0] != 0.0f || L[1] != 0.0f || L[2] != 0.0f;
            continue;
        }
        const u32 model = (r->gb[2][i] >> 8) & 255u;
        if (model == DE__GS_MATERIAL_MODEL_UNLIT) {
            ++unlit;
            bad += fabsf(L[0] - 0.4f) > 2e-3f && fabsf(L[0] - unlit_lin) > 2e-3f;
        } else {
            ++lit;
            bad += !(L[0] > 1.0f);
        }
    }
    printf("classified: %u lit px, %u unlit px, %u background px; tiles standard %u (list %u), "
           "unlit list %u; %u bad\n",
           lit, unlit, background, r->tiles[0], r->args[0].count, r->args[1].count, bad);
    EXPECT_GT(unlit, 300u);
    EXPECT_GT(lit, 300u);
    EXPECT_GT(background, 1000u);
    EXPECT_EQ(bad, 0u);
}

/* lighting_split_diffuse_specular: with r.split_lighting the diffuse and specular targets add up
 * to the lighting target, which is the same as without the split; for a dielectric the specular
 * part does not change with the base colour and the diffuse part scales with it (the split's
 * definition: specular = the model with base colour 0). */
static void split(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    const u32 dark = material_add(0.2f, 0.4f, 0.0f, 0.5f);
    const u32 light = material_add(0.8f, 0.4f, 0.0f, 0.5f);
    lx_floor(f, light);
    world_light(&f->w, 0,
                &(DeLight){.kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = 3000.0f},
                at(1.0f, -0.5f, 1.5f));
    world_light(&f->w, 1,
                &(DeLight){.kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = 2000.0f},
                pointing(0, 0, 5, (de_vec3){0, 1, 0}, 0.4f));
    lx_run(f);
    const size_t n = (size_t)f->width * f->height * 4u;
    f32 *plain = (f32 *)zalloc(n * sizeof(f32));
    memcpy(plain, f->res.lighting, n * sizeof(f32));
    EXPECT_TRUE(f->res.diffuse == NULL);
    ASSERT_TRUE(cvar_set("r.split_lighting", "1"));
    lx_run(f);
    ASSERT_TRUE(f->res.diffuse != NULL && f->res.specular != NULL);
    f32 *dif_l = (f32 *)zalloc(n * sizeof(f32)), *spe_l = (f32 *)zalloc(n * sizeof(f32));
    memcpy(dif_l, f->res.diffuse, n * sizeof(f32));
    memcpy(spe_l, f->res.specular, n * sizeof(f32));
    const f32 base_l = de__gb_srgb_decode(de__gb_srgb_encode(0.8f));
    u32 checked = 0, bad = 0;
    f64 sum_spec = 0.0, sum_total = 0.0;
    {
        const lx_result *r = &f->res;
        for (size_t i = 0; i < n / 4u; ++i) {
            surf s;
            if (!surf_at(r, i, &s))
                continue;
            ++checked;
            const f32 total = r->lighting[4 * i];
            bad += !close_to(dif_l[4 * i] + spe_l[4 * i], total, 0.004f, 1e-3f);
            bad += !close_to(total, plain[4 * i], 0.002f, 1e-3f);
            bad += dif_l[4 * i] < 0.0f || spe_l[4 * i] < 0.0f;
            sum_spec += spe_l[4 * i];
            sum_total += total;
        }
    }
    /* The same floor with base colour 0.2. */
    world_set(&f->w, 0, f->w.f.instances.mesh[0].id, at(0, 0, 0), dark);
    lx_run(f);
    const lx_result *r = &f->res;
    ASSERT_TRUE(r->diffuse != NULL);
    u32 compared = 0;
    for (size_t i = 0; i < n / 4u; ++i) {
        surf s;
        if (!surf_at(r, i, &s) || !(plain[4 * i + 3] > 0.5f))
            continue;
        ++compared;
        bad += !close_to(r->specular[4 * i], spe_l[4 * i], 0.004f, 1e-3f);
        bad += !close_to(r->diffuse[4 * i] * (base_l / s.base), dif_l[4 * i], 0.006f, 2e-3f);
    }
    free(plain);
    free(dif_l);
    free(spe_l);
    printf("split: %u px (%u compared across base colours), specular %.1f%% of the light, %u bad\n",
           checked, compared, 100.0 * sum_spec / (sum_total > 0.0 ? sum_total : 1.0), bad);
    EXPECT_GT(checked, 1000u);
    EXPECT_GT(compared, 1000u);
    EXPECT_TRUE(sum_spec > 0.01 * sum_total);
    EXPECT_EQ(bad, 0u);
}

/* The CPU decision for (light, cluster) with a small slack both ways: 1 / 0 decisive, -1 marginal.
 */
static i32 cpu_touch(const de__cluster_light *l, const f32 lo[3], const f32 hi[3]) {
    de__cluster_light grown = *l, shrunk = *l;
    f32 blo[3], bhi[3], slo[3], shi[3];
    for (u32 k = 0; k < 3; ++k) {
        const f32 e = 1e-3f * (1.0f + fabsf(lo[k]) + fabsf(hi[k]));
        blo[k] = lo[k] - e;
        bhi[k] = hi[k] + e;
        slo[k] = lo[k] + e;
        shi[k] = hi[k] - e;
        if (slo[k] > shi[k])
            slo[k] = shi[k] = 0.5f * (lo[k] + hi[k]);
    }
    grown.bound[3] = l->bound[3] * 1.001f + 1e-3f;
    shrunk.bound[3] = l->bound[3] * 0.999f - 1e-3f;
    const bool a = de__cluster_light_touches(&grown, blo, bhi);
    const bool b = de__cluster_light_touches(&shrunk, slo, shi);
    return a == b ? (a ? 1 : 0) : -1;
}

/* Lights of every local kind scattered through the view: points, spots, rects. */
static u32 scatter_lights(lx *f, u32 seed, u32 count, f32 rmin, f32 rmax, f32 spread) {
    lcg g = {seed};
    for (u32 i = 0; i < count; ++i) {
        DeLight l = {.color = {1, 1, 1}, .intensity = 100.0f};
        l.kind = i % 5u == 3u ? DE_LIGHT_SPOT : i % 11u == 7u ? DE_LIGHT_RECT : DE_LIGHT_POINT;
        l.range = lcg_range(&g, rmin, rmax);
        l.spot_inner = 0.2f;
        l.spot_outer = lcg_range(&g, 0.3f, 0.9f);
        l.area_size = (de_vec2){0.6f, 0.4f};
        const de_vec3 axis = {lcg_range(&g, -1, 1), lcg_range(&g, -1, 1), lcg_range(&g, -1, 1)};
        const de_affine xf =
            trs((de_vec3){lcg_range(&g, -spread, spread),
                          lcg_range(&g, -0.6f * spread, 0.6f * spread), lcg_range(&g, 0.2f, 8.0f)},
                de_v3_length(axis) > 1e-3f ? de_v3_normalize(axis) : (de_vec3){0, 0, 1},
                lcg_range(&g, 0.0f, 3.0f), 1.0f);
        world_light(&f->w, i, &l, xf);
    }
    return count;
}

/* Compares the GPU lists with the CPU decisions over the GPU's own light list and the CPU bounds;
 * returns the CPU-total of decisive touches (into *total) and the marginal pairs (*marginal). */
static u32 compare_lists(const lx_result *r, u32 cap, u64 *total, u32 *marginal) {
    const de__cluster_grid *g = &r->lc.grid;
    const u32 n = r->counters[DE__LIGHTING_COUNTER_LOCAL / 4u];
    u32 mismatches = 0, bounds_bad = 0;
    *total = 0;
    *marginal = 0;
    for (u32 c = 0; c < g->count; ++c) {
        f32 lo[3], hi[3];
        de__cluster_bounds(g, &r->lc.view.inv_proj[0][0], c, lo, hi);
        for (u32 k = 0; k < 3; ++k) {
            const f32 glo = r->bounds[8u * c + k], ghi = r->bounds[8u * c + 4u + k];
            bounds_bad +=
                !close_to(glo, lo[k], 1e-3f, 1e-3f) || !close_to(ghi, hi[k], 1e-3f, 1e-3f);
        }
        const u32 off = r->grid[2u * c], cnt = r->grid[2u * c + 1u];
        EXPECT_TRUE(cnt <= cap);
        u32 decisive_in = 0;
        for (u32 i = 0; i < n; ++i) {
            bool in_gpu = false;
            for (u32 k = 0; k < cnt; ++k)
                in_gpu |= r->list[off + k] == i;
            const i32 cpu = cpu_touch(&r->lights[i], lo, hi);
            if (cpu < 0) {
                ++*marginal;
                continue;
            }
            *total += (u64)cpu;
            decisive_in += in_gpu && cpu == 1;
            if (cnt < cap) /* a full cluster may have cut any of them */
                mismatches += (u32)in_gpu != (u32)cpu;
            else
                mismatches += in_gpu && cpu == 0;
        }
        (void)decisive_in;
        for (u32 a = 0; a < cnt; ++a) /* no duplicates */
            for (u32 b2 = a + 1u; b2 < cnt; ++b2)
                mismatches += r->list[off + a] == r->list[off + b2];
    }
    EXPECT_EQ(bounds_bad, 0u);
    return mismatches;
}

/* clusters_assignment_cpu_ref: the GPU's cluster bounds and light lists equal the CPU's over
 * random point, spot and rect lights (sets; pairs within a float tolerance of the test skipped),
 * and the GPU's converted lights carry the expected units and view-space spheres. */
static void assignment(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, material_add(0.8f, 0.5f, 0.0f, 0.5f));
    const u32 n = scatter_lights(f, 77u, 48u, 0.5f, 4.0f, 6.0f);
    lx_run(f);
    const lx_result *r = &f->res;
    EXPECT_EQ(r->counters[DE__LIGHTING_COUNTER_LOCAL / 4u], n);
    EXPECT_EQ(r->counters[DE__LIGHTING_COUNTER_OVERFLOW / 4u], 0u);
    EXPECT_EQ(r->counters[DE__LIGHTING_COUNTER_DROPPED / 4u], 0u);
    u64 total = 0;
    u32 marginal = 0;
    const u32 mism = compare_lists(r, r->lc.view.max_per_cluster, &total, &marginal);
    printf("assignment: %u clusters, %u lights, %llu touches, %u marginal pairs, %u mismatches\n",
           r->lc.grid.count, n, (unsigned long long)total, marginal, mism);
    EXPECT_EQ(mism, 0u);
    EXPECT_GT(total, 100u);
    /* The converted lights: each GPU light matches a world light by position. */
    de_mat4 view, proj;
    camera(f, &view, &proj);
    u32 matched = 0;
    for (u32 i = 0; i < n; ++i) {
        const de__cluster_light *gl = &r->lights[i];
        for (u32 s = 0; s < n; ++s) {
            const de_affine *xf = &f->w.f.lights.transform[s];
            const de_vec3 p = {xf->m[9], xf->m[10], xf->m[11]};
            if (fabsf(p.x - gl->position[0]) > 1e-4f || fabsf(p.y - gl->position[1]) > 1e-4f ||
                fabsf(p.z - gl->position[2]) > 1e-4f)
                continue;
            const DeLight *l = &f->w.f.lights.light[s];
            ++matched;
            const f32 want_i = l->kind == DE_LIGHT_POINT ? de__light_point_candela(l->intensity)
                               : l->kind == DE_LIGHT_SPOT
                                   ? de__light_spot_candela(l->intensity, l->spot_outer)
                                   : l->intensity;
            EXPECT_TRUE(close_to(gl->color[0], want_i, 1e-4f, 1e-6f));
            EXPECT_TRUE(close_to(gl->position[3], l->range, 1e-6f, 0.0f));
            const de_vec3 vp = de_m4_mul_point(&view, p);
            EXPECT_TRUE(close_to(gl->bound[0], vp.x, 1e-4f, 1e-4f));
            EXPECT_TRUE(close_to(gl->bound[2], vp.z, 1e-4f, 1e-4f));
            break;
        }
    }
    EXPECT_EQ(matched, n);
}

/* clusters_overflow_counted: with r.cluster_max_lights 4 and 24 overlapping lights, no cluster
 * holds more than 4, the excess is counted (entries + overflow = every touch), and the stats
 * read back say so a frame or two later. */
static void overflow(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    lx_floor(f, material_add(0.8f, 0.5f, 0.0f, 0.5f));
    ASSERT_TRUE(cvar_set("r.cluster_max_lights", "4"));
    lcg g = {9u};
    for (u32 i = 0; i < 24u; ++i)
        world_light(
            &f->w, i,
            &(DeLight){
                .kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = 50.0f, .range = 5.0f},
            at(lcg_range(&g, -1, 1), lcg_range(&g, -1, 1), lcg_range(&g, 1, 3)));
    lx_run(f);
    const lx_result *r = &f->res;
    u64 entries = 0, over_lo = 0, over_hi = 0;
    u32 full = 0;
    for (u32 c = 0; c < r->lc.grid.count; ++c) {
        entries += r->grid[2u * c + 1u];
        EXPECT_TRUE(r->grid[2u * c + 1u] <= 4u);
        f32 lo[3], hi[3];
        de__cluster_bounds(&r->lc.grid, &r->lc.view.inv_proj[0][0], c, lo, hi);
        u32 lo_cnt = 0, hi_cnt = 0; /* decisive touches; with the marginal ones */
        for (u32 i = 0; i < 24u; ++i) {
            const i32 t = cpu_touch(&r->lights[i], lo, hi);
            lo_cnt += t == 1;
            hi_cnt += t != 0;
        }
        over_lo += lo_cnt > 4u ? lo_cnt - 4u : 0u;
        over_hi += hi_cnt > 4u ? hi_cnt - 4u : 0u;
        full += lo_cnt > 4u;
    }
    const u32 overflow = r->counters[DE__LIGHTING_COUNTER_OVERFLOW / 4u];
    printf("overflow: %u full clusters, overflow %u (CPU %llu .. %llu), entries %llu (counter "
           "%u)\n",
           full, overflow, (unsigned long long)over_lo, (unsigned long long)over_hi,
           (unsigned long long)entries, r->counters[DE__LIGHTING_COUNTER_ENTRIES / 4u]);
    EXPECT_GT(full, 10u);
    EXPECT_GT(overflow, 0u);
    EXPECT_TRUE(overflow >= over_lo && overflow <= over_hi);
    EXPECT_EQ((u64)r->counters[DE__LIGHTING_COUNTER_ENTRIES / 4u], entries);
    EXPECT_EQ(r->counters[DE__LIGHTING_COUNTER_DROPPED / 4u], 0u);
    /* The stats readback. */
    de__lighting_stats st = {0};
    for (u32 i = 0; i < 4u && !st.valid; ++i) {
        ASSERT_TRUE(lx_frame(f));
        st = de__lighting_view_stats(f->lt, 0);
    }
    ASSERT_TRUE(st.valid);
    EXPECT_EQ(st.local, 24u);
    EXPECT_GT(st.overflow, 0u);
    EXPECT_GT(st.tiles[DE__GS_MATERIAL_MODEL_STANDARD], 0u);
}

#define GPU_TESTS(sfx, backend)                                                                    \
    TEST_F(clustered_lights, clusters_assignment_cpu_ref_##sfx, lx) {                              \
        assignment(f, backend);                                                                    \
    }                                                                                              \
    TEST_F(clustered_lights, clusters_overflow_counted_##sfx, lx) {                                \
        overflow(f, backend);                                                                      \
    }                                                                                              \
    TEST_F(clustered_lights, lights_point_lumens_to_lux_##sfx, lx) {                               \
        point_lumens(f, backend);                                                                  \
    }                                                                                              \
    TEST_F(clustered_lights, lights_sun_lux_##sfx, lx) {                                           \
        sun_lux(f, backend);                                                                       \
    }                                                                                              \
    TEST_F(clustered_lights, lights_spot_cone_##sfx, lx) {                                         \
        spot_cone(f, backend);                                                                     \
    }                                                                                              \
    TEST_F(clustered_lights, lights_range_cutoff_##sfx, lx) {                                      \
        range_cutoff(f, backend);                                                                  \
    }                                                                                              \
    TEST_F(clustered_lights, lights_ltc_rect_vs_reference_##sfx, lx) {                             \
        ltc_rect(f, backend);                                                                      \
    }                                                                                              \
    TEST_F(clustered_lights, lights_emissive_proxy_##sfx, lx) {                                    \
        emissive_proxy(f, backend);                                                                \
    }                                                                                              \
    TEST_F(clustered_lights, lighting_only_classified_tiles_##sfx, lx) {                           \
        only_classified(f, backend);                                                               \
    }                                                                                              \
    TEST_F(clustered_lights, lighting_split_diffuse_specular_##sfx, lx) {                          \
        split(f, backend);                                                                         \
    }

GPU_TESTS(metal, DE_GPU_METAL)
GPU_TESTS(vulkan, DE_GPU_VULKAN)
GPU_TESTS(d3d12, DE_GPU_D3D12)

/* ================================================================================================
 * CPU tests
 * ============================================================================================== */

/* lights_units: 1000 lm at 1 m is 79.6 lux (1%), a spot's lumens go into its cone, the window is 1
 * at the light and 0 at the range, the derived range is where the illuminance meets the cutoff. */
TEST(clustered_lights, lights_units) {
    const f32 e = de__light_point_illuminance(1000.0f, 1.0f, 0.0f, 0.0f, 0.05f);
    printf("1000 lm at 1 m: %.3f lux\n", e);
    EXPECT_TRUE(fabsf(e - 79.577f) <= 0.01f * 79.577f);
    EXPECT_TRUE(fabsf(de__light_point_candela(4.0f * (f32)PI_D) - 1.0f) < 1e-6f);
    /* A spot's lumens over its cone's solid angle 2 pi (1 - cos): a 90 degree half-angle is a
     * hemisphere (twice a point light's candela). */
    EXPECT_TRUE(fabsf(de__light_spot_candela(1000.0f, (f32)PI_D * 0.5f) -
                      2.0f * de__light_point_candela(1000.0f)) < 1e-3f);
    EXPECT_TRUE(de__light_window(0.0f, 5.0f) == 1.0f);
    EXPECT_TRUE(de__light_window(5.0f, 5.0f) == 0.0f);
    EXPECT_TRUE(de__light_window(6.0f, 5.0f) == 0.0f);
    const f32 r = de__light_derived_range(100.0f, 0.05f);
    EXPECT_TRUE(fabsf(100.0f / (r * r) - 0.05f) < 1e-5f);
    EXPECT_TRUE(de__light_point_illuminance(1000.0f, 3.0f, 2.0f, 0.0f, 0.05f) == 0.0f);
    /* The source radius bounds 1 / d^2. */
    EXPECT_TRUE(fabsf(de__light_point_illuminance(1000.0f, 0.01f, 10.0f, 0.5f, 0.05f) -
                      de__light_point_candela(1000.0f) / 0.25f) < 1e-2f);
    /* Spot falloff: 1 inside, 0 outside, (t)^2 between. */
    EXPECT_TRUE(de__light_spot_factor(cosf(0.1f), 0.2f, 0.4f) == 1.0f);
    EXPECT_TRUE(de__light_spot_factor(cosf(0.5f), 0.2f, 0.4f) == 0.0f);
    const f32 t = (cosf(0.3f) - cosf(0.4f)) / (cosf(0.2f) - cosf(0.4f));
    EXPECT_TRUE(fabsf(de__light_spot_factor(cosf(0.3f), 0.2f, 0.4f) - t * t) < 1e-5f);
}

/* lights_ltc_polygon_vs_numeric: the clipped polygon integral of light_data.c (ltc.slang's
 * arithmetic) against the numerical form factor of rects and disks (12-gons of equal area) in
 * random poses, some partly below the horizon: within 2% (diffuse). */
TEST(clustered_lights, lights_ltc_polygon_vs_numeric) {
    lcg g = {4242u};
    const f32 p[3] = {0, 0, 0}, n[3] = {0, 0, 1};
    f64 worst = 0.0;
    u32 tested = 0;
    for (u32 it = 0; it < 60u; ++it) {
        const bool disk = it % 3u == 2u;
        const f32 c[3] = {lcg_range(&g, -2, 2), lcg_range(&g, -2, 2), lcg_range(&g, 0.3f, 2.5f)};
        /* the emitter faces the point (roughly), tilted at random */
        de_vec3 front = de_v3_normalize(
            (de_vec3){-c[0] + lcg_range(&g, -1, 1), -c[1] + lcg_range(&g, -1, 1), -c[2]});
        de_vec3 ax = de_v3_normalize(de_v3_cross(front, (de_vec3){0.3f, 0.7f, 0.1f}));
        de_vec3 ay = de_v3_cross(front, ax);
        ay = (de_vec3){-ay.x, -ay.y, -ay.z};
        const f32 hx = lcg_range(&g, 0.2f, 1.0f), hy = disk ? hx : lcg_range(&g, 0.1f, 0.8f);
        const f32 axf[3] = {ax.x, ax.y, ax.z}, ayf[3] = {ay.x, ay.y, ay.z},
                  ff[3] = {front.x, front.y, front.z};
        f32 q[16][3];
        u32 nq = 0;
        if (!disk) {
            const f32 sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
            for (u32 k = 0; k < 4; ++k, ++nq)
                for (u32 j = 0; j < 3; ++j)
                    q[nq][j] = c[j] + axf[j] * hx * sx[k] + ayf[j] * hy * sy[k];
        } else {
            const f32 rr = hx * 1.0233267f;
            for (u32 k = 0; k < 12u; ++k, ++nq) {
                const f32 a = 2.0f * (f32)PI_D * (f32)k / 12.0f;
                for (u32 j = 0; j < 3; ++j)
                    q[nq][j] = c[j] + (axf[j] * cosf(a) + ayf[j] * sinf(a)) * rr;
            }
        }
        const f64 want = ff_numeric(c, axf, ayf, hx, hy, disk, ff, p, n);
        if (want < 0.01)
            continue;
        const f64 got = de__ltc_polygon(&q[0][0], nq);
        const f64 e = fabs(got - want) / want;
        worst = e > worst ? e : worst;
        EXPECT_TRUE(e <= 0.02);
        ++tested;
    }
    printf("LTC polygon: %u emitters, worst %.3f%%\n", tested, 100.0 * worst);
    EXPECT_GT(tested, 30u);
}

/* lights_ltc_tables_sane: finite entries; the fitted M^-1 is the identity at roughness 1 is not
 * required, but at every entry the Schlick split stays in [0, 1] (x + y <= 1 + eps), and at normal
 * incidence m02 = m20 = 0 (an isotropic lobe). */
TEST(clustered_lights, lights_ltc_tables_sane) {
    u32 bad = 0;
    for (u32 r = 0; r < DE__LTC_SIZE; ++r)
        for (u32 v = 0; v < DE__LTC_SIZE; ++v) {
            const float *t1 = de__ltc_tab1[r][v], *t2 = de__ltc_tab2[r][v];
            for (u32 k = 0; k < 4; ++k)
                bad += !isfinite(t1[k]);
            bad += !(t2[0] >= -1e-4f && t2[1] >= -1e-4f && t2[0] + t2[1] <= 1.0f + 1e-3f);
            bad += !(t1[3] > 0.0f);
            if (v == 0u)
                bad += fabsf(t1[1]) > 1e-3f || fabsf(t1[2]) > 1e-3f;
        }
    EXPECT_EQ(bad, 0u);
    /* At normal incidence the full reflectance F0 + F90 = 1 is near the GGX albedo's upper bound.
     */
    EXPECT_TRUE(de__ltc_tab2[0][0][0] > 0.9f);
}

/* clusters_grid_slices: slice k's near depth maps back to k, slices grow exponentially, depths
 * before the near plane go to slice 0 and past `far` to the last slice. */
TEST(clustered_lights, clusters_grid_slices) {
    const de__cluster_grid g = de__cluster_grid_make(1920, 1080, 64, 32, 0.1f, 500.0f);
    EXPECT_EQ(g.tiles_x, 30u);
    EXPECT_EQ(g.tiles_y, 17u);
    EXPECT_EQ(g.count, 30u * 17u * 32u);
    for (u32 k = 0; k < g.slices; ++k) {
        const f32 d = de__cluster_slice_depth(&g, k);
        EXPECT_EQ(de__cluster_slice(&g, d * 1.0001f), k);
        if (k > 0)
            EXPECT_TRUE(d > de__cluster_slice_depth(&g, k - 1u));
    }
    EXPECT_TRUE(fabsf(de__cluster_slice_depth(&g, 31) - 500.0f) < 0.05f);
    EXPECT_EQ(de__cluster_slice(&g, 0.01f), 0u);
    EXPECT_EQ(de__cluster_slice(&g, 1e6f), 31u);
}

/* clusters_conservative_random: every light that reaches a point (inside its range, and its cone
 * for a spot) is in the list of that point's cluster, over random lights and points of the
 * view frustum (the CPU arithmetic of the GPU passes). */
TEST(clustered_lights, clusters_conservative_random) {
    const de_mat4 view = de_m4_look_at((de_vec3){0, 0, 10}, (de_vec3){0, 0, 0}, (de_vec3){0, 1, 0});
    const de_mat4 proj = de_m4_perspective_rz(0.9f, 320.0f / 192.0f, 0.1f);
    f32 ip[4][4];
    de__lighting_inv_proj(&proj, ip);
    const de__cluster_grid g = de__cluster_grid_make(320, 192, 32, 24, 0.1f, 60.0f);
    lcg rng = {31337u};
    enum { NL = 200 };
    de__cluster_light ls[NL];
    for (u32 i = 0; i < NL; ++i) {
        de__cluster_light *l = &ls[i];
        memset(l, 0, sizeof *l);
        const de_vec3 wp = {lcg_range(&rng, -12, 12), lcg_range(&rng, -8, 8),
                            lcg_range(&rng, -40, 9.5f)};
        const de_vec3 vp = de_m4_mul_point(&view, wp);
        const f32 range = lcg_range(&rng, 0.2f, 6.0f);
        const bool spot = i % 2u == 0u;
        const u32 kind = spot ? DE__LIGHT_SPOT : DE__LIGHT_POINT;
        memcpy(&l->shape[3], &kind, 4);
        de_vec3 dir = de_v3_normalize(
            (de_vec3){lcg_range(&rng, -1, 1), lcg_range(&rng, -1, 1), lcg_range(&rng, -1, 1)});
        const f32 outer = lcg_range(&rng, 0.1f, 1.4f);
        l->color[3] = cosf(outer);
        l->axis[0] = dir.x; /* view space = world rotation is identity here (look down -z) */
        l->axis[1] = dir.y;
        l->axis[2] = dir.z;
        l->bound[0] = vp.x;
        l->bound[1] = vp.y;
        l->bound[2] = vp.z;
        l->bound[3] = range;
    }
    u32 checked = 0, missed = 0;
    for (u32 it = 0; it < 20000u; ++it) {
        const u32 px = (u32)(lcg_f(&rng) * 320.0f) % 320u, py = (u32)(lcg_f(&rng) * 192.0f) % 192u;
        const f32 depth = 0.1f * powf(1000.0f, lcg_f(&rng));
        /* The view-space point of pixel (px, py) at depth `depth`. */
        const f32 nx = ((f32)px + 0.5f) / 320.0f * 2.0f - 1.0f;
        const f32 ny = 1.0f - ((f32)py + 0.5f) / 192.0f * 2.0f;
        f32 a[4], q[3];
        for (u32 r = 0; r < 4; ++r)
            a[r] = ip[r][0] * nx + ip[r][1] * ny + ip[r][2] * 0.5f + ip[r][3];
        for (u32 k = 0; k < 3; ++k)
            q[k] = a[k] / a[3] * (depth / (-a[2] / a[3]));
        const u32 c = (de__cluster_slice(&g, depth) * g.tiles_y + py / g.tile_px) * g.tiles_x +
                      px / g.tile_px;
        f32 lo[3], hi[3];
        de__cluster_bounds(&g, &ip[0][0], c, lo, hi);
        for (u32 i = 0; i < NL; ++i) {
            const de__cluster_light *l = &ls[i];
            const f32 v[3] = {q[0] - l->bound[0], q[1] - l->bound[1], q[2] - l->bound[2]};
            const f32 d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (!(d < l->bound[3] * 0.999f))
                continue;
            u32 kind;
            memcpy(&kind, &l->shape[3], 4);
            if (kind == DE__LIGHT_SPOT && d > 1e-4f &&
                (v[0] * l->axis[0] + v[1] * l->axis[1] + v[2] * l->axis[2]) / d <
                    l->color[3] + 1e-3f)
                continue; /* outside the cone */
            ++checked;
            missed += !de__cluster_light_touches(l, lo, hi);
        }
    }
    printf("conservative: %u (point, light) pairs that receive light, %u missed\n", checked,
           missed);
    EXPECT_GT(checked, 1000u);
    EXPECT_EQ(missed, 0u);
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

/* The words of `struct <name>` in `text` (uint/float fields 1, float4 4, LightingInputs 8,
 * DeClusterLists 8, float4 x[4] 16). */
static u32 struct_words(const char *text, const char *name) {
    char head[96];
    snprintf(head, sizeof head, "struct %s\n", name);
    const char *ps = strstr(text, head);
    const char *pe = ps != NULL ? strstr(ps, "};") : NULL;
    if (ps == NULL || pe == NULL)
        return 0;
    u32 words = 0;
    for (const char *ln = strchr(ps, '\n'); ln != NULL && ln < pe; ln = strchr(ln + 1, '\n')) {
        const char *s = ln + 1;
        while (*s == ' ')
            ++s;
        if (strncmp(s, "public ", 7) == 0)
            s += 7;
        const char *semi = strchr(s, ';');
        const char *br = strchr(s, '[');
        const u32 n = br != NULL && semi != NULL && br < semi ? (u32)strtoul(br + 1, NULL, 10) : 1u;
        if (strncmp(s, "float4 ", 7) == 0)
            words += 4u * n;
        else if (strncmp(s, "uint3 ", 6) == 0)
            words += 3u * n;
        else if (strncmp(s, "uint ", 5) == 0 || strncmp(s, "float ", 6) == 0)
            words += n;
        else if (strncmp(s, "LightingInputs ", 15) == 0 || strncmp(s, "DeClusterLists ", 15) == 0)
            words += strncmp(s, "LightingInputs ", 15) == 0
                         ? (u32)(sizeof(de__lighting_inputs_gpu) / 4u)
                         : 8u;
        else if (strncmp(s, "DeIbl ", 6) == 0) /* DENG-304 */
            words += 48u;
    }
    return words;
}

/* layout_match: the `public static const uint` values of the lighting shaders against the C
 * table, and the struct sizes against the C mirrors. */
TEST(clustered_lights, layout_match) {
    static const char *const files[] = {
        "lighting/lighting_common.slang",   "lighting/lights.slang",
        "lighting/light_cull.slang",        "lighting/ltc.slang",
        "lighting/deferred_lighting.slang", "lighting/lighting_inputs.slang",
        "lighting/cluster_build.slang",     "lighting/ibl.slang"};
    u32 count = 0, found = 0;
    const de__lighting_const *k = de__lighting_layout_consts(&count);
    char *texts[8] = {0};
    for (u32 fi = 0; fi < 8u; ++fi) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", DE_LIGHTS_TEST_SHADER_DIR, files[fi]);
        texts[fi] = read_text(path);
        ASSERT_TRUE(texts[fi] != NULL);
        for (const char *p = texts[fi]; (p = strstr(p, "public static const uint ")) != NULL;) {
            p += strlen("public static const uint ");
            char name[96];
            long long value = 0;
            if (sscanf(p, "%95[A-Za-z0-9_] = %lli", name, &value) != 2) {
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
                printf("  %s is not in de__lighting_layout_consts\n", name);
            EXPECT_TRUE(ok);
            ++found;
        }
    }
    EXPECT_EQ(found, count);
    EXPECT_EQ(struct_words(texts[0], "DeLightingView") * 4u, (u32)sizeof(de__lighting_view));
    EXPECT_EQ(struct_words(texts[0], "DeClusterLists") * 4u, 32u);
    EXPECT_EQ(struct_words(texts[1], "DeClusterLight") * 4u, (u32)sizeof(de__cluster_light));
    EXPECT_EQ(struct_words(texts[2], "CullParams") * 4u, 64u);      /* clusters.c cl_cull_params */
    EXPECT_EQ(struct_words(texts[4], "LightingParams") * 4u, 128u); /* DENG-429 */
    EXPECT_EQ(struct_words(texts[5], "LightingInputs") * 4u, (u32)sizeof(de__lighting_inputs_gpu));
    EXPECT_EQ(struct_words(texts[6], "ClusterBuildParams") * 4u, 32u);
    EXPECT_EQ(struct_words(texts[7], "DeIbl") * 4u, (u32)sizeof(de__ibl_gpu)); /* DENG-304 */
    for (u32 fi = 0; fi < 8u; ++fi)
        free(texts[fi]);
}

/* lighting.renderer_stage: the renderer registers "lighting" after the vis frame and the output
 * pass (DENG-304) reads its target, so the cluster and deferred passes run and "output.basic"
 * makes the view's colour; with r.cluster_debug the heatmap is the view's colour instead (no
 * output pass). */
TEST(lighting, renderer_stage) {
    de_tracking_allocator track;
    de_allocator a = de_tracking_allocator_init(&track, NULL, DE_MEM_RENDER);
    de_fp_env_init_thread();
    de_type_registry *types = NULL;
    de_world *w = NULL;
    de_gpu_device *d = NULL;
    de_renderer *r = NULL;
    ASSERT_EQ(de_type_registry_create(&a, &types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &a, .types = types}, &w), DE_OK);
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &d), DE_OK);
    ASSERT_EQ(
        de_renderer_create(&(de_renderer_desc){.device = d, .types = types, .allocator = &a}, &r),
        DE_OK);
    de_renderer_attach(r, w);
    const de_cvar pipelined = de_cvar_find("r.pipelined");
    ASSERT_TRUE(DE_HANDLE_VALID(pipelined));
    (void)de_cvar_set(pipelined, "0");
    const DeRenderComponents ids = *de__render_components(w);
    const de_builtin_component_ids bi = de_builtin_components(w);
    de_entity cam = DE_ENTITY_NULL;
    const de_component cc[2] = {bi.transform, ids.camera};
    ASSERT_EQ(de_entity_spawn_with(w, cc, 2, &cam), DE_OK);
    DeTransform *ct = (DeTransform *)de_entity_get(w, cam, bi.transform);
    ct->position = (de_dvec3){0, -10, 2};
    ct->rotation = (de_quat){0, 0, 0, 1};
    ct->scale = (de_vec3){1, 1, 1};
    const de_component mc[2] = {bi.transform, ids.mesh_renderer};
    for (u32 i = 0; i < 4; ++i) {
        de_entity e = DE_ENTITY_NULL;
        ASSERT_EQ(de_entity_spawn_with(w, mc, 2, &e), DE_OK);
        DeTransform *t = (DeTransform *)de_entity_get(w, e, bi.transform);
        t->position = (de_dvec3){(f64)i, 0, 0};
        t->rotation = (de_quat){0, 0, 0, 1};
        t->scale = (de_vec3){1, 1, 1};
        ((DeMeshRenderer *)de_entity_get(w, e, ids.mesh_renderer))->mesh.id = 1u;
    }
    const de_component lc[2] = {bi.transform, ids.light};
    de_entity le = DE_ENTITY_NULL;
    ASSERT_EQ(de_entity_spawn_with(w, lc, 2, &le), DE_OK);
    DeTransform *lt = (DeTransform *)de_entity_get(w, le, bi.transform);
    lt->rotation = (de_quat){0, 0, 0, 1};
    lt->scale = (de_vec3){1, 1, 1};
    *(DeLight *)de_entity_get(w, le, ids.light) =
        (DeLight){.kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = 100.0f};
#define RUN_FRAME()                                                                                \
    do {                                                                                           \
        ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f), DE_OK);   \
        ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f), DE_OK);       \
        ASSERT_EQ(de__render_extract_and_kick(r, w), DE_OK);                                       \
    } while (0)
    for (u32 pass = 0; pass < 2; ++pass) {
        const bool heat = pass == 1;
        if (heat && !cvar_set("r.cluster_debug", "1"))
            break; /* Shipping: no debug cvar */
        de__gpu_null_log_enable(d, true);
        RUN_FRAME();
        de__gpu_null_log_enable(d, false);
        u32 lighting = 0;
        bool deferred = false, heatmap = false, bounds = false, output = false;
        for (u32 i = 0; i < de__gpu_null_log_count(d); ++i) {
            const de__gpu_null_entry *e = de__gpu_null_log_entry(d, i);
            if (e->op != DE__GPU_NULL_OP_MARKER_PUSH || strncmp(e->list, "view:main", 9) != 0)
                continue;
            lighting += strncmp(e->text, "lighting.", 9) == 0;
            deferred |= strcmp(e->text, "lighting.deferred") == 0;
            heatmap |= strcmp(e->text, "lighting.heatmap") == 0;
            bounds |= strcmp(e->text, "lighting.cluster_bounds") == 0;
            output |= strcmp(e->text, "output.basic") == 0;
        }
        printf("%s: %u lighting passes%s\n", heat ? "r.cluster_debug 1" : "default", lighting,
               output ? ", output.basic" : "");
        if (!heat) {
            /* DENG-304: the output pass reads the lighting target, so lighting runs. */
            EXPECT_TRUE(deferred);
            EXPECT_TRUE(bounds);
            EXPECT_FALSE(heatmap);
            EXPECT_TRUE(output);
        } else {
            EXPECT_FALSE(output); /* the heatmap is the view's colour */
            EXPECT_TRUE(deferred);
            EXPECT_TRUE(heatmap);
            EXPECT_TRUE(bounds);
            (void)cvar_set("r.cluster_debug", "0");
        }
    }
#undef RUN_FRAME
    (void)de_cvar_set(pipelined, "1");
    de_renderer_destroy(r);
    de_world_destroy(w);
    de_gpu_device_destroy(d);
    de_type_registry_destroy(types);
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}
