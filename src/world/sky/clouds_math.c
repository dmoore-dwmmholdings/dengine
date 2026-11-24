/*
 * clouds_math.c: the CPU side of the volumetric clouds (DENG-208) that every build has: the
 * DeSky layer resolved with its defaults, the quality table and the double-precision tile
 * offsets. Pure; no allocation. See clouds_internal.h.
 */
#include "world/sky/clouds_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static f32 clamp01(f32 v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

de__clouds_settings de__clouds_resolve(const DeSky *sky) {
    de__clouds_settings c = {0};
    if (sky == NULL)
        return c;
    c.coverage = clamp01(sky->cloud_coverage == sky->cloud_coverage ? sky->cloud_coverage : 0.0f);
    c.on = sky->clouds && c.coverage > 0.0f;
    c.bottom_m = sky->cloud_bottom_m > 0.0f ? sky->cloud_bottom_m : DE__CLOUDS_BOTTOM_M;
    c.top_m = sky->cloud_top_m > 0.0f ? sky->cloud_top_m : DE__CLOUDS_TOP_M;
    if (c.top_m < c.bottom_m + DE__CLOUDS_MIN_DEPTH_M)
        c.top_m = c.bottom_m + DE__CLOUDS_MIN_DEPTH_M;
    c.density = sky->cloud_density > 0.0f ? sky->cloud_density : 1.0f;
    c.erosion = sky->cloud_erosion > 0.0f ? clamp01(sky->cloud_erosion) : DE__CLOUDS_EROSION;
    const f32 speed = sky->cloud_wind_speed_mps > 0.0f ? sky->cloud_wind_speed_mps : 0.0f;
    const f64 a = (f64)sky->cloud_wind_dir_deg * (3.14159265358979323846 / 180.0);
    c.wind_x = (f32)(sin(a) * (f64)speed); /* clockwise from north (+y) */
    c.wind_y = (f32)(cos(a) * (f64)speed);
    return c;
}

bool de__clouds_enabled(const de__clouds_settings *c, const de_render_settings *rs) {
    return c != NULL && rs != NULL && c->on && c->coverage > 0.0f &&
           rs->volumetrics != DE_QUALITY_OFF;
}

de__clouds_quality de__clouds_quality_for(de_quality q) {
    switch (q) {
    case DE_QUALITY_LOW:
        return (de__clouds_quality){48u, 4u, 2u, 8u, false, false};
    case DE_QUALITY_MEDIUM:
        return (de__clouds_quality){64u, 5u, 2u, 12u, true, false};
    case DE_QUALITY_HIGH:
        return (de__clouds_quality){96u, 6u, 3u, 16u, true, true};
    case DE_QUALITY_ULTRA:
        return (de__clouds_quality){128u, 6u, 3u, 24u, true, true};
    default:
        return (de__clouds_quality){0u, 0u, 0u, 0u, false, false};
    }
}

f32 de__clouds_shadow_weight(f32 u, f32 v) {
    if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f))
        return 0.0f;
    const f32 k = 1.0f / DE__CLOUDS_SHADOW_FADE;
    return clamp01(fminf(u, 1.0f - u) * k) * clamp01(fminf(v, 1.0f - v) * k);
}

f32 de__clouds_shadow_blend(f32 coarse, f32 fine, f32 w) {
    w = clamp01(w);
    return coarse + (fine - coarse) * w;
}

f32 de__clouds_shadow_snap(f64 camera_world, f64 shift, f64 extent, u32 n) {
    const f64 texel = extent / (f64)n;
    return (f32)(floor((camera_world + shift) / texel) * texel - camera_world);
}

f64 de__clouds_shadow_shift(f64 sun_axis, f64 sun_z, f64 from_z, f64 bottom_z) {
    if (!(sun_z > 0.02))
        return 0.0;
    const f64 v = sun_axis * fmax(bottom_z - from_z, 0.0) / sun_z;
    const f64 m = (f64)DE__CLOUDS_SHADOW_SHIFT_MAX_M;
    return v < -m ? -m : (v > m ? m : v);
}

f64 de__clouds_shadow_travel(f64 sun_axis, f64 sun_z, f64 from_z, f64 bottom_z) {
    if (!(sun_z > 0.02))
        return 0.0;
    return sun_axis * fmax(bottom_z - from_z, 0.0) / sun_z;
}

void de__clouds_shadow_centres(f64 sun_axis, f64 sun_z, f64 camera_z, f64 bottom_z,
                               bool has_target, f64 target, f64 *fine, f64 *coarse) {
    f64 f = de__clouds_shadow_travel(sun_axis, sun_z, camera_z, bottom_z);
    if (has_target && isfinite(target)) {
        const f64 r = (f64)DE__CLOUDS_SHADOW_FINE_INNER_M;
        f = f < target - r ? target - r : (f > target + r ? target + r : f);
    }
    const f64 m = (f64)DE__CLOUDS_SHADOW_SHIFT_MAX_M;
    const f64 inside = f < -m ? -m : (f > m ? m : f);
    *fine = f;
    *coarse = f - inside; /* 0 while the fine cascade fits in the camera-centred coarse one */
}

static f64 median_sorted(f64 *v, u32 n) {
    for (u32 i = 1; i < n; ++i) { /* insertion sort: at most DE__CLOUDS_GROUND_SAMPLES */
        const f64 t = v[i];
        u32 k = i;
        for (; k > 0u && v[k - 1u] > t; --k)
            v[k] = v[k - 1u];
        v[k] = t;
    }
    return (n & 1u) != 0u ? v[n / 2u] : 0.5 * (v[n / 2u - 1u] + v[n / 2u]);
}

bool de__clouds_receiver_crossing(const f32 *s, u32 n, f64 camera_z, de_vec3 sun, f64 bottom_z,
                                  f64 *x, f64 *y) {
    f64 dist[DE__CLOUDS_GROUND_SAMPLES], cx[DE__CLOUDS_GROUND_SAMPLES],
        cy[DE__CLOUDS_GROUND_SAMPLES];
    u32 hits = 0;
    if (s == NULL || x == NULL || y == NULL || !((f64)sun.z > 0.02))
        return false;
    for (u32 i = 0; i < n && hits < DE__CLOUDS_GROUND_SAMPLES; ++i) {
        const f32 *p = s + (size_t)i * 4u;
        if (!(p[3] > 0.5f) || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
            continue;
        const f64 d = (f64)p[0] * p[0] + (f64)p[1] * p[1] + (f64)p[2] * p[2];
        const f64 qx = (f64)p[0] + de__clouds_shadow_travel(sun.x, sun.z, camera_z + p[2], bottom_z);
        const f64 qy = (f64)p[1] + de__clouds_shadow_travel(sun.y, sun.z, camera_z + p[2], bottom_z);
        u32 k = hits++; /* insertion by distance */
        for (; k > 0u && dist[k - 1u] > d; --k) {
            dist[k] = dist[k - 1u];
            cx[k] = cx[k - 1u];
            cy[k] = cy[k - 1u];
        }
        dist[k] = d;
        cx[k] = qx;
        cy[k] = qy;
    }
    if (hits == 0u)
        return false;
    const u32 near = (hits + 1u) / 2u;
    *x = median_sorted(cx, near);
    *y = median_sorted(cy, near);
    return true;
}

f32 de__clouds_tile_offset(f64 world, f64 drift, f64 tile) {
    if (!(tile > 0.0))
        return 0.0f;
    f64 r = fmod(world + drift, tile);
    if (r < 0.0)
        r += tile;
    if (r >= tile)
        r -= tile;
    f32 o = (f32)r;
    return o >= (f32)tile ? 0.0f : o;
}

/* ---- DENG-852: the blue-noise tile ---- */

#define BN_N DE__CLOUDS_BLUE_N
#define BN_COUNT (BN_N * BN_N)
#define BN_R 7 /* the Gaussian's reach (sigma 1.5): exp(-24.5 / 4.5) is 0.4% */

typedef struct bn_state {
    f32 energy[BN_COUNT];
    u8 on[BN_COUNT];
    f32 kernel[2 * BN_R + 1][2 * BN_R + 1];
} bn_state;

/* Adds (sign 1) or removes (sign -1) the point `i` in the energy field, wrapping at the edges. */
static void bn_splat(bn_state *b, u32 i, f32 sign) {
    const int x = (int)(i % BN_N), y = (int)(i / BN_N);
    for (int dy = -BN_R; dy <= BN_R; ++dy)
        for (int dx = -BN_R; dx <= BN_R; ++dx) {
            const u32 xx = (u32)((x + dx + (int)BN_N) % (int)BN_N);
            const u32 yy = (u32)((y + dy + (int)BN_N) % (int)BN_N);
            b->energy[yy * BN_N + xx] += sign * b->kernel[dy + BN_R][dx + BN_R];
        }
}

static void bn_set(bn_state *b, u32 i, u8 v) {
    if (b->on[i] == v)
        return;
    b->on[i] = v;
    bn_splat(b, i, v ? 1.0f : -1.0f);
}

/* The point (`ones`: a set one, else an unset one) with the highest (`max`) or lowest energy: the
 * tightest cluster or the largest void. The first on ties. */
static u32 bn_find(const bn_state *b, u8 ones, bool max) {
    u32 best = BN_COUNT;
    f32 bv = 0.0f;
    for (u32 i = 0; i < BN_COUNT; ++i) {
        if (b->on[i] != ones)
            continue;
        const f32 e = b->energy[i];
        if (best == BN_COUNT || (max ? e > bv : e < bv)) {
            best = i;
            bv = e;
        }
    }
    return best;
}

void de__clouds_blue_noise(u8 *out) {
    if (out == NULL)
        return;
    bn_state *b = (bn_state *)calloc(1, sizeof *b);
    u16 *rank = (u16 *)calloc(BN_COUNT, sizeof *rank);
    if (b == NULL || rank == NULL) {
        for (u32 i = 0; i < BN_COUNT; ++i)
            out[i] = (u8)((i * 2654435761u) >> 24); /* no memory: white noise */
        free(b);
        free(rank);
        return;
    }
    for (int dy = -BN_R; dy <= BN_R; ++dy)
        for (int dx = -BN_R; dx <= BN_R; ++dx)
            b->kernel[dy + BN_R][dx + BN_R] =
                expf(-(f32)(dx * dx + dy * dy) / (2.0f * 1.5f * 1.5f));
    /* An LCG picks the starting ~10% of the texels, so the tile is the same every run. */
    u32 seed = 0x9E3779B9u;
    const u32 initial = BN_COUNT / 10u;
    for (u32 n = 0; n < initial;) {
        seed = seed * 1664525u + 1013904223u;
        const u32 i = (seed >> 8) % BN_COUNT;
        if (!b->on[i]) {
            bn_set(b, i, 1);
            ++n;
        }
    }
    /* Relax: move the tightest cluster's point into the largest void until it would not move. */
    for (u32 it = 0; it < BN_COUNT; ++it) {
        const u32 c = bn_find(b, 1, true);
        bn_set(b, c, 0);
        const u32 v = bn_find(b, 0, false);
        bn_set(b, v, 1);
        if (v == c)
            break;
    }
    /* Phase 1: remove the points from the tightest cluster; the last one removed has rank 0. */
    u8 *saved = (u8 *)malloc(BN_COUNT);
    f32 *saved_e = (f32 *)malloc(sizeof(f32) * BN_COUNT);
    if (saved == NULL || saved_e == NULL) {
        free(saved);
        free(saved_e);
        free(rank);
        free(b);
        for (u32 i = 0; i < BN_COUNT; ++i)
            out[i] = (u8)((i * 2654435761u) >> 24);
        return;
    }
    memcpy(saved, b->on, BN_COUNT);
    memcpy(saved_e, b->energy, sizeof(f32) * BN_COUNT);
    for (u32 r = initial; r-- > 0u;) {
        const u32 c = bn_find(b, 1, true);
        bn_set(b, c, 0);
        rank[c] = (u16)r;
    }
    /* Phases 2 and 3: from the relaxed pattern, insert at the largest void in rank order. */
    memcpy(b->on, saved, BN_COUNT);
    memcpy(b->energy, saved_e, sizeof(f32) * BN_COUNT);
    for (u32 r = initial; r < BN_COUNT; ++r) {
        const u32 v = bn_find(b, 0, false);
        bn_set(b, v, 1);
        rank[v] = (u16)r;
    }
    for (u32 i = 0; i < BN_COUNT; ++i)
        out[i] = (u8)(((u32)rank[i] * 256u) / BN_COUNT);
    free(saved);
    free(saved_e);
    free(rank);
    free(b);
}
