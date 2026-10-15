/*
 * test_sss.c: screen-space separable subsurface scattering with the Burley profile (DENG-306;
 * src/render/passes/sss/sss.c, shaders/sss/). Executable test_render_sss; CTest names
 * render_sss.sss.<test>.
 *
 * CPU tests:
 *   sss_kernel_normalized        every table's weights sum to 1 (0.5 %) per channel, for several
 *                                channel shapes; bins are ordered and taps inside their bins
 *   sss_kernel_matches_burley    each bin's weight equals the profile R(r) 2 pi r integrated
 *                                numerically over the bin, within 2 % per channel; the profile
 *                                integrates to 1; the inverse distribution inverts it
 *   sss_quality_off_no_dispatch  a null-backend renderer with a few mesh renderers: with
 *                                de_render_settings.sss off the main view's graph has no sss pass;
 *                                on, the five sss passes run after lighting.deferred
 *   sss_layout_match             sss_separable.slang's constants and SssParams' size against sss.c
 *
 * GPU tests (_metal, _vulkan, _d3d12; SKIP without the device or the engine shaders): the vis
 * frame + lighting fixture of lighting_fixture.h with a sun straight down, planes of Standard
 * materials, and the SSS passes at DE_QUALITY_HIGH (25 taps) compared with the same frame at
 * DE_QUALITY_OFF:
 *   sss_unflagged_unchanged      a plain plane next to a subsurface one: every pixel without the
 *                                SUBSURFACE flag is bitwise the same; subsurface pixels away from
 *                                the edge keep their light (1 %)
 *   sss_depth_edge_no_bleed      a bright subsurface plane 2 m above a dark one: the bright pixels
 *                                at the edge keep their light (no dark bleeds in); the same planes
 *                                side by side at one height do bleed (the control)
 *   sss_world_space_scale        the side-by-side planes seen from 8 m and 4 m: the bleed's width
 *                                in metres stays (0.8 to 1.25 times), in pixels it doubles
 *                                (1.6 to 2.5 times)
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
#include "render/components_internal.h"
#include "render/formats/de_cooked_material.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/loaders/render_assets.h"
#include "render/materials/brdf_luts.h"
#include "render/materials/material_models.h"
#include "render/passes/sss/sss.h"
#include "render/passes/vis/gbuffer.h"
#include "render/passes/vis/vis_stages.h"
#include "render/renderer_internal.h"
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

#include "lighting_fixture.h"

/* ================================================================================================
 * CPU: the kernels
 * ============================================================================================== */

static const f64 k_shapes[][3] = {
    {0.01, 0.01, 0.01}, {0.2, 0.1, 0.05}, {0.003, 0.0015, 0.0006}, {1.0, 0.25, 0.04}};

TEST(sss, sss_kernel_normalized) {
    static const u32 taps[] = {3, 11, 17, 25, 63};
    de__sss_bin bins[DE__SSS_BINS(DE__SSS_TAPS_MAX)];
    f64 w[DE__SSS_BINS(DE__SSS_TAPS_MAX)][3];
    for (u32 t = 0; t < sizeof taps / sizeof taps[0]; ++t) {
        const u32 n = DE__SSS_BINS(taps[t]);
        de__sss_kernel_build(taps[t], bins);
        /* The table of every table holds the same bins. */
        EXPECT_EQ(
            memcmp(bins, de__sss_kernel_all() + de__sss_kernel_first(taps[t]), n * sizeof bins[0]),
            0);
        EXPECT_TRUE(bins[0].lo == 0.0f);
        EXPECT_TRUE(bins[0].pos == 0.0f);
        for (u32 k = 0; k < n; ++k) {
            EXPECT_TRUE(bins[k].lo <= bins[k].pos);
            EXPECT_TRUE(bins[k].pos <= bins[k].hi);
            if (k > 0)
                EXPECT_TRUE(bins[k].lo == bins[k - 1].hi);
        }
        for (u32 s = 0; s < sizeof k_shapes / sizeof k_shapes[0]; ++s) {
            de__sss_kernel_weights(bins, n, k_shapes[s], w);
            for (u32 c = 0; c < 3; ++c) {
                f64 sum = 0.0;
                for (u32 k = 0; k < n; ++k) {
                    EXPECT_TRUE(w[k][c] >= 0.0);
                    sum += w[k][c];
                }
                if (fabs(sum - 1.0) > 0.005)
                    printf("taps %u shape %u channel %u: sum %.6f\n", taps[t], s, c, sum);
                EXPECT_FLOAT_EQ(sum, 1.0, 0.005);
            }
        }
    }
    /* Taps by quality and cvar. */
    EXPECT_EQ(de__sss_taps(DE_QUALITY_OFF, 0), 0u);
    EXPECT_EQ(de__sss_taps(DE_QUALITY_LOW, 0), 11u);
    EXPECT_EQ(de__sss_taps(DE_QUALITY_MEDIUM, 0), 17u);
    EXPECT_EQ(de__sss_taps(DE_QUALITY_HIGH, 0), 25u);
    EXPECT_EQ(de__sss_taps(DE_QUALITY_ULTRA, 0), 25u);
    EXPECT_EQ(de__sss_taps(DE_QUALITY_HIGH, 8), 9u);
    EXPECT_EQ(de__sss_taps(DE_QUALITY_HIGH, 1000), 63u);
    EXPECT_EQ(de__sss_taps(DE_QUALITY_OFF, 25), 0u);
}

/* The profile's mass over [a, b]: Simpson over R(r) 2 pi r (no use of the closed form). */
static f64 profile_mass(f64 a, f64 b, f64 d) {
    if (b > 80.0 * d)
        b = 80.0 * d; /* e^(-80/3) of the slow lobe is left: 2e-12 */
    if (!(b > a))
        return 0.0;
    const u32 n = 20000;
    const f64 h = (b - a) / n;
    f64 s = 0.0;
    for (u32 i = 0; i <= n; ++i) {
        const f64 r = a + h * i;
        /* r R(r) is finite at 0: (e^0 + e^0) / (8 pi d) */
        const f64 f =
            r > 0.0 ? 2.0 * 3.14159265358979323846 * r * de__burley_profile(r, d) : 2.0 / (4.0 * d);
        s += f * (i == 0 || i == n ? 1.0 : (i % 2 ? 4.0 : 2.0));
    }
    return s * h / 3.0;
}

TEST(sss, sss_kernel_matches_burley) {
    /* The profile integrates to 1 over the plane, for any d. */
    EXPECT_FLOAT_EQ(profile_mass(0.0, 1e9, 0.01), 1.0, 1e-6);
    EXPECT_FLOAT_EQ(profile_mass(0.0, 1e9, 2.5), 1.0, 1e-6);
    /* The inverse distribution. */
    for (u32 i = 1; i < 100; ++i) {
        const f64 u = i / 100.0;
        EXPECT_FLOAT_EQ(de__burley_cdf(de__burley_inverse_unit(u), 1.0), u, 1e-9);
    }
    /* The mapping: s(0.8) = 1.05, d = mfp / s. */
    EXPECT_FLOAT_EQ(de__burley_scale(0.8), 1.05, 1e-12);
    EXPECT_FLOAT_EQ(de__burley_d(0.105, 0.8), 0.1, 1e-12);
    static const u32 taps[] = {11, 17, 25};
    de__sss_bin bins[DE__SSS_BINS(DE__SSS_TAPS_MAX)];
    f64 w[DE__SSS_BINS(DE__SSS_TAPS_MAX)][3];
    f64 worst = 0.0;
    for (u32 t = 0; t < sizeof taps / sizeof taps[0]; ++t) {
        const u32 n = DE__SSS_BINS(taps[t]);
        de__sss_kernel_build(taps[t], bins);
        for (u32 s = 0; s < sizeof k_shapes / sizeof k_shapes[0]; ++s) {
            const f64 *d = k_shapes[s];
            const f64 d_ref = fmax(d[0], fmax(d[1], d[2]));
            de__sss_kernel_weights(bins, n, d, w);
            for (u32 k = 0; k < n; ++k)
                for (u32 c = 0; c < 3; ++c) {
                    const f64 ref = profile_mass(bins[k].lo * d_ref, bins[k].hi * d_ref, d[c]);
                    /* 2 % of the bin's mass, or 2e-5 absolute for bins with almost none. */
                    const f64 e = fabs(w[k][c] - ref);
                    const f64 rel = ref > 1e-3 ? e / ref : e / 1e-3;
                    worst = rel > worst ? rel : worst;
                    if (rel > 0.02)
                        printf("taps %u shape %u bin %u ch %u: kernel %.6g burley %.6g\n", taps[t],
                               s, k, c, w[k][c], ref);
                    EXPECT_TRUE(rel <= 0.02);
                }
        }
    }
    printf("worst relative error against the integrated profile: %.2e\n", worst);
}

/* ================================================================================================
 * CPU: the renderer's stage on the null backend
 * ============================================================================================== */

static void main_view_passes(de_gpu_device *d, char *got, size_t cap) {
    got[0] = '\0';
    size_t at = 0;
    const u32 n = de__gpu_null_log_count(d);
    for (u32 i = 0; i < n; ++i) {
        const de__gpu_null_entry *e = de__gpu_null_log_entry(d, i);
        if (e->op != DE__GPU_NULL_OP_MARKER_PUSH || strncmp(e->list, "view:main", 9) != 0)
            continue;
        at += (size_t)snprintf(got + at, cap - at, "%s%s", at ? "," : "", e->text);
        if (at >= cap)
            break;
    }
}

TEST(sss, sss_quality_off_no_dispatch) {
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
    char got[2048];
    for (u32 pass = 0; pass < 2; ++pass) {
        de_render_settings s = de_renderer_get_settings(r);
        s.sss = pass == 0 ? DE_QUALITY_OFF : DE_QUALITY_HIGH;
        de_renderer_set_settings(r, &s);
        for (u32 i = 0; i < 2; ++i) {         /* the settings apply at the next frame */
            de__gpu_null_log_enable(d, true); /* clears it */
            ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_POST_UPDATE, 1.0f / 60.0f, 0.0f), DE_OK);
            ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_EXTRACT, 1.0f / 60.0f, 0.0f), DE_OK);
            ASSERT_EQ(de__render_extract_and_kick(r, w), DE_OK);
            de__gpu_null_log_enable(d, false);
        }
        EXPECT_EQ((int)de_renderer_get_settings(r).sss,
                  (int)(pass == 0 ? DE_QUALITY_OFF : DE_QUALITY_HIGH));
        main_view_passes(d, got, sizeof got);
        printf("sss %s: %s\n", pass == 0 ? "off" : "high", got);
        EXPECT_TRUE(strstr(got, "lighting.deferred") != NULL);
        if (pass == 0) {
            EXPECT_TRUE(strstr(got, "sss.") == NULL); /* no dispatch at all */
        } else {
            const char *l = strstr(got, "lighting.deferred");
            const char *h = strstr(got, ",sss.clear,sss.tiles,sss.args,sss.blur_h,sss.blur_v,");
            EXPECT_TRUE(h != NULL && l != NULL && h > l);
        }
    }
    de_renderer_destroy(r);
    de_world_destroy(w);
    de_gpu_device_destroy(d);
    de_type_registry_destroy(types);
    (void)de_cvar_set(pipelined, "1");
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}

/* ================================================================================================
 * GPU
 * ============================================================================================== */

/* One frame's results that two runs compare (copies: f->res is freed at the next frame). */
typedef struct snap {
    u32 width, height;
    f32 *lighting, *pos;
    u32 *gb2;
} snap;

static void snap_take(const lx *f, snap *s) {
    const lx_result *r = &f->res;
    const size_t n = (size_t)r->width * r->height;
    s->width = r->width;
    s->height = r->height;
    s->lighting = (f32 *)malloc(n * 16u);
    s->pos = (f32 *)malloc(n * 16u);
    s->gb2 = (u32 *)malloc(n * 4u);
    ASSERT_TRUE(s->lighting != NULL && s->pos != NULL && s->gb2 != NULL);
    memcpy(s->lighting, r->lighting, n * 16u);
    memcpy(s->pos, r->pos, n * 16u);
    memcpy(s->gb2, r->gb[2], n * 4u);
}

static void snap_free(snap *s) {
    free(s->lighting);
    free(s->pos);
    free(s->gb2);
    memset(s, 0, sizeof *s);
}

static u32 flags_at(const snap *s, size_t i) {
    return (s->gb2[i] >> 16) & 0xFFu; /* GB2 b: flags / 255 */
}

static bool surface_at(const snap *s, size_t i) {
    return s->pos[4 * i + 3] > 0.5f && s->lighting[4 * i + 3] > 0.5f;
}

/* A Standard material with subsurface amount 1 and mean free path `mfp` (metres). */
static u32 sss_material(f32 base, f32 r, f32 g, f32 b) {
    const u32 m = material_add(base, 0.6f, 0.0f, 0.5f);
    g_mat[m - 1u].standard.subsurface = 1.0f;
    g_mat[m - 1u].standard.subsurface_radius = (de_vec3){r, g, b};
    return m;
}

/* The sun straight down, the SSS state, and two runs: `off` at DE_QUALITY_OFF, `on` at HIGH. */
static void sss_open(lx *f, de_gpu_backend b) {
    lx_open(f, b);
    ASSERT_EQ(de__sss_create_ex(f->vis, &f->sss), DE_OK);
    if (!de__sss_enabled(f->sss))
        SKIP("subsurface scattering is off (no engine shader code for this backend)");
    world_light(&f->w, 0,
                &(DeLight){.kind = DE_LIGHT_DIRECTIONAL, .color = {1, 1, 1}, .intensity = 20000.0f},
                at(0, 0, 20));
}

static void run_both(lx *f, snap *off, snap *on) {
    f->sss_quality = DE_QUALITY_OFF;
    lx_run(f);
    EXPECT_TRUE(f->sss_frame == NULL); /* nothing added */
    snap_take(f, off);
    f->sss_quality = DE_QUALITY_HIGH;
    lx_run(f);
    EXPECT_TRUE(f->sss_frame != NULL);
    if (f->sss_frame != NULL)
        EXPECT_EQ(f->sss_frame->taps, 25u);
    snap_take(f, on);
}

/* sss_unflagged_unchanged: a plain plane (x < 0) beside a subsurface plane (x > 0). */
static void unflagged_unchanged(lx *f, de_gpu_backend b) {
    sss_open(f, b);
    const u32 plain = material_add(0.5f, 0.6f, 0.0f, 0.5f);
    const u32 skin = sss_material(0.8f, 0.3f, 0.15f, 0.07f);
    const u32 half = lx_mesh_plane(f, 4, 8, 1.5f); /* 6 m x 12 m */
    world_set(&f->w, 0, half, at(-3, 0, 0), plain);
    world_set(&f->w, 1, half, at(3, 0, 0), skin);
    snap off, on;
    run_both(f, &off, &on);
    size_t unflagged = 0, flagged = 0, differ = 0, interior = 0, interior_bad = 0;
    for (size_t i = 0; i < (size_t)on.width * on.height; ++i) {
        if (!surface_at(&on, i))
            continue;
        if ((flags_at(&on, i) & DE__GB_FLAG_SUBSURFACE) == 0u) {
            ++unflagged;
            differ += memcmp(&on.lighting[4 * i], &off.lighting[4 * i], 16) != 0;
            continue;
        }
        ++flagged;
        if (on.pos[4 * i] > 1.5f) { /* away from the plain plane: a constant field keeps */
            ++interior;
            const f32 e = fabsf(on.lighting[4 * i] - off.lighting[4 * i]) / off.lighting[4 * i];
            interior_bad += e > 0.01f;
        }
    }
    printf("unflagged %zu (differ %zu), subsurface %zu (interior %zu, off by > 1%%: %zu)\n",
           unflagged, differ, flagged, interior, interior_bad);
    EXPECT_GT(unflagged, 1000u);
    EXPECT_GT(flagged, 1000u);
    EXPECT_EQ(differ, 0u);
    EXPECT_GT(interior, 500u);
    EXPECT_EQ(interior_bad, 0u);
    snap_free(&off);
    snap_free(&on);
}

/* The darkening of the bright plane's pixels near x = 0 (ON against OFF): the mean over the
 * pixels with -0.1 < x < 0, and the darkening-weighted mean distance from the edge (metres and
 * pixels) of the pixels within 1 m darkened by more than 1 %. */
typedef struct bleed {
    f64 near_mean;
    f64 width_m, width_px;
    u32 near_count;
} bleed;

static bleed measure_bleed(const snap *off, const snap *on, f32 bright_z) {
    bleed r = {0};
    f64 sum_w = 0.0, sum_m = 0.0, sum_px = 0.0, near_sum = 0.0;
    /* The edge's column in each row: the last bright pixel. */
    for (u32 y = 0; y < on->height; ++y) {
        i32 edge = -1;
        for (u32 x = 0; x < on->width; ++x) {
            const size_t i = (size_t)y * on->width + x;
            if (surface_at(on, i) && fabsf(on->pos[4 * i + 2] - bright_z) < 0.01f &&
                on->pos[4 * i] < 0.0f)
                edge = (i32)x;
        }
        if (edge < 0)
            continue;
        for (u32 x = 0; x <= (u32)edge; ++x) {
            const size_t i = (size_t)y * on->width + x;
            if (!surface_at(on, i) || fabsf(on->pos[4 * i + 2] - bright_z) > 0.01f ||
                (flags_at(on, i) & DE__GB_FLAG_SUBSURFACE) == 0u)
                continue;
            const f64 dark = 1.0 - (f64)on->lighting[4 * i] / (f64)off->lighting[4 * i];
            const f64 xm = -(f64)on->pos[4 * i];
            if (xm < 0.1) {
                near_sum += dark;
                ++r.near_count;
            }
            /* Half-float rounding of the split targets leaves about 1e-3 of noise at every
             * pixel; only real darkening, and only within 1 m of the edge, counts. */
            if (dark > 0.01 && xm < 1.0) {
                sum_w += dark;
                sum_m += dark * xm;
                sum_px += dark * (f64)((u32)edge - x);
            }
        }
    }
    r.near_mean = r.near_count ? near_sum / r.near_count : 0.0;
    r.width_m = sum_w > 0.0 ? sum_m / sum_w : 0.0;
    r.width_px = sum_w > 0.0 ? sum_px / sum_w : 0.0;
    return r;
}

/* Bright (x < 0, at height `bright_z`; slot 0) and dark (x > 0, at 0; slot 1) subsurface planes.
 * Returns the half plane's mesh; `*bright_mat` the bright material. */
static u32 edge_scene(lx *f, f32 bright_z, u32 *bright_mat) {
    const u32 bright = sss_material(0.9f, 0.1f, 0.05f, 0.025f);
    const u32 dark = sss_material(0.05f, 0.1f, 0.05f, 0.025f);
    const u32 half = lx_mesh_plane(f, 4, 8, 1.5f);
    world_set(&f->w, 0, half, at(-3, 0, bright_z), bright);
    world_set(&f->w, 1, half, at(3, 0, 0), dark);
    *bright_mat = bright;
    return half;
}

/* sss_depth_edge_no_bleed: the bright plane 2 m up keeps its light at the edge; the control at
 * one height darkens there. */
static void depth_edge(lx *f, de_gpu_backend b) {
    sss_open(f, b);
    f->eye = (de_vec3){0.0f, 0.0f, 8.0f};
    u32 bright = 0;
    const u32 half = edge_scene(f, 2.0f, &bright);
    snap off, on;
    run_both(f, &off, &on);
    const bleed lifted = measure_bleed(&off, &on, 2.0f);
    snap_free(&off);
    snap_free(&on);
    /* The control: the same planes at one height. */
    world_set(&f->w, 0, half, at(-3, 0, 0), bright);
    run_both(f, &off, &on);
    const bleed level = measure_bleed(&off, &on, 0.0f);
    snap_free(&off);
    snap_free(&on);
    printf("edge darkening: 2 m apart %.4f (%u px), level %.4f (%u px)\n", lifted.near_mean,
           lifted.near_count, level.near_mean, level.near_count);
    EXPECT_GT(lifted.near_count, 20u);
    EXPECT_GT(level.near_count, 20u);
    EXPECT_TRUE(fabs(lifted.near_mean) < 0.01);
    EXPECT_TRUE(level.near_mean > 0.05);
}

/* sss_world_space_scale: the level planes from 8 m and from 4 m. */
static void world_space_scale(lx *f, de_gpu_backend b) {
    sss_open(f, b);
    u32 bright = 0;
    (void)edge_scene(f, 0.0f, &bright);
    snap off, on;
    f->eye = (de_vec3){0.0f, 0.0f, 8.0f};
    run_both(f, &off, &on);
    const bleed far8 = measure_bleed(&off, &on, 0.0f);
    snap_free(&off);
    snap_free(&on);
    f->eye = (de_vec3){0.0f, 0.0f, 4.0f};
    run_both(f, &off, &on);
    const bleed near4 = measure_bleed(&off, &on, 0.0f);
    snap_free(&off);
    snap_free(&on);
    printf("bleed width: from 8 m %.4f m %.2f px, from 4 m %.4f m %.2f px\n", far8.width_m,
           far8.width_px, near4.width_m, near4.width_px);
    ASSERT_TRUE(far8.width_m > 0.0);
    ASSERT_TRUE(far8.width_px > 0.0);
    const f64 metres = near4.width_m / far8.width_m, pixels = near4.width_px / far8.width_px;
    EXPECT_TRUE(metres > 0.8);
    EXPECT_TRUE(metres < 1.25);
    EXPECT_TRUE(pixels > 1.6);
    EXPECT_TRUE(pixels < 2.5);
}

#define GPU_TESTS(sfx, backend)                                                                    \
    TEST_F(sss, sss_unflagged_unchanged_##sfx, lx) {                                               \
        unflagged_unchanged(f, backend);                                                           \
    }                                                                                              \
    TEST_F(sss, sss_depth_edge_no_bleed_##sfx, lx) {                                               \
        depth_edge(f, backend);                                                                    \
    }                                                                                              \
    TEST_F(sss, sss_world_space_scale_##sfx, lx) {                                                 \
        world_space_scale(f, backend);                                                             \
    }

GPU_TESTS(metal, DE_GPU_METAL)
GPU_TESTS(vulkan, DE_GPU_VULKAN)
GPU_TESTS(d3d12, DE_GPU_D3D12)

/* ================================================================================================
 * Layout
 * ============================================================================================== */

static char *read_text(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return NULL;
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1u);
    if (buf != NULL && fread(buf, 1, (size_t)n, fp) == (size_t)n)
        buf[n] = '\0';
    fclose(fp);
    return buf;
}

/* The constants of sss_separable.slang against sss.h, and SssParams' size (its fields are all
 * 4-byte words). */
TEST(sss, sss_layout_match) {
    char path[1024];
    snprintf(path, sizeof path, "%s/sss/sss_separable.slang", DE_SSS_TEST_SHADER_DIR);
    char *text = read_text(path);
    ASSERT_TRUE(text != NULL);
    u32 n = 0;
    const de__sss_const *consts = de__sss_layout_consts(&n);
    u32 found = 0;
    for (char *q = strstr(text, "static const uint "); q != NULL;
         q = strstr(q + 1, "static const uint ")) {
        char name[64] = "", value[32] = "";
        ASSERT_EQ(sscanf(q + 18, "%63s = %31[0-9xXa-fA-F]", name, value), 2);
        bool known = false;
        for (u32 i = 0; i < n; ++i)
            if (strcmp(consts[i].name, name) == 0) {
                known = true;
                EXPECT_EQ((u32)strtoul(value, NULL, 0), consts[i].value);
            }
        if (!known)
            printf("constant %s has no C mirror\n", name);
        EXPECT_TRUE(known);
        ++found;
    }
    EXPECT_EQ(found, n);
    char *s = strstr(text, "struct SssParams");
    if (s != NULL)
        s = strchr(s, '{');
    ASSERT_TRUE(s != NULL);
    char *end = strchr(s, '}');
    ASSERT_TRUE(end != NULL);
    u32 words = 0; /* one 4-byte field per `public ` line */
    for (char *q = strstr(s, "public "); q != NULL && q < end; q = strstr(q + 1, "public "))
        ++words;
    EXPECT_EQ(words * 4u, de__sss_params_bytes());
    free(text);
    /* The profile constants of burley_profile.slang match the C profile. */
    EXPECT_FLOAT_EQ(de__burley_cdf(0.0, 1.0), 0.0, 1e-12);
    EXPECT_FLOAT_EQ(de__burley_cdf(1e9, 1.0), 1.0, 1e-12);
}
