/*
 * light_data.c: the arithmetic of the clustered lights in C (DENG-303): physical units and
 * attenuation (shaders/lighting/lights.slang), the cluster grid and bounds (lighting_common.slang,
 * cluster_build.slang), the light-vs-cluster test (light_cull.slang) and the LTC polygon integral
 * (ltc.slang). The operations follow the shaders' order, so the tests can compare GPU results with
 * them closely; lighting_pass.c packs the grid into the lighting constants with them.
 */
#include "render/lighting/lighting.h"

#include <math.h>
#include <string.h>

#define LD_PI 3.14159265358979323846f

static f32 clampf(f32 v, f32 lo, f32 hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
static f32 saturatef(f32 v) {
    return clampf(v, 0.0f, 1.0f);
}

/* ---------- Units and attenuation ---------- */

f32 de__light_point_candela(f32 lumens) {
    return lumens / (4.0f * LD_PI);
}

f32 de__light_spot_candela(f32 lumens, f32 outer) {
    const f32 c = cosf(clampf(outer, 1e-3f, LD_PI));
    const f32 d = 1.0f - c;
    return lumens / (2.0f * LD_PI * (d > 1e-6f ? d : 1e-6f));
}

f32 de__light_window(f32 d, f32 r) {
    const f32 x = d / r;
    const f32 w = saturatef(1.0f - x * x * x * x);
    return w * w;
}

f32 de__light_derived_range(f32 candela, f32 cutoff) {
    return sqrtf((candela > 0.0f ? candela : 0.0f) / (cutoff > 1e-9f ? cutoff : 1e-9f));
}

f32 de__light_spot_factor(f32 c, f32 inner, f32 outer) {
    outer = clampf(outer, 1e-3f, LD_PI);
    inner = clampf(inner, 0.0f, outer);
    const f32 co = cosf(outer), ci = cosf(inner);
    const f32 dd = ci - co;
    const f32 t = saturatef((c - co) * (1.0f / (dd > 1e-4f ? dd : 1e-4f)));
    return t * t;
}

f32 de__light_point_illuminance(f32 lumens, f32 d, f32 r, f32 rs, f32 cutoff) {
    const f32 i = de__light_point_candela(lumens);
    if (!(r > 0.0f))
        r = de__light_derived_range(i, cutoff);
    if (!(d < r))
        return 0.0f;
    f32 d2 = d * d;
    const f32 m = rs * rs > 1e-8f ? rs * rs : 1e-8f;
    if (d2 < m)
        d2 = m;
    return i * de__light_window(d, r) / d2;
}

/* ---------- The cluster grid ---------- */

de__cluster_grid de__cluster_grid_make(u32 width, u32 height, u32 tile_px, u32 slices, f32 near_z,
                                       f32 far_z) {
    de__cluster_grid g;
    memset(&g, 0, sizeof g);
    g.width = width > 0 ? width : 1u;
    g.height = height > 0 ? height : 1u;
    g.tile_px = tile_px > 0 ? tile_px : 64u;
    g.slices = slices > 0 ? slices : 1u;
    g.tiles_x = (g.width + g.tile_px - 1u) / g.tile_px;
    g.tiles_y = (g.height + g.tile_px - 1u) / g.tile_px;
    g.count = g.tiles_x * g.tiles_y * g.slices;
    g.near_z = near_z > 1e-4f ? near_z : 1e-4f;
    g.far_z = far_z > g.near_z * 1.01f ? far_z : g.near_z * 1.01f;
    if (g.slices > 1u) {
        g.log_scale = (f32)(g.slices - 1u) / logf(g.far_z / g.near_z);
        g.log_bias = -logf(g.near_z) * g.log_scale;
    } else {
        g.log_scale = 0.0f;
        g.log_bias = 0.0f;
    }
    return g;
}

u32 de__cluster_slice(const de__cluster_grid *g, f32 depth) {
    const f32 d = depth > g->near_z ? depth : g->near_z;
    const f32 k = floorf(logf(d) * g->log_scale + g->log_bias);
    return (u32)clampf(k, 0.0f, (f32)(g->slices - 1u));
}

f32 de__cluster_slice_depth(const de__cluster_grid *g, u32 k) {
    if (k >= g->slices)
        return 1e30f;
    if (g->log_scale == 0.0f)
        return g->near_z;
    return expf(((f32)k - g->log_bias) / g->log_scale);
}

static void unproject(const f32 m[4][4], f32 x, f32 y, f32 z, f32 out[3]) {
    const f32 c[4] = {x, y, z, 1.0f};
    f32 v[4];
    for (u32 r = 0; r < 4; ++r)
        v[r] = m[r][0] * c[0] + m[r][1] * c[1] + m[r][2] * c[2] + m[r][3] * c[3];
    for (u32 k = 0; k < 3; ++k)
        out[k] = v[k] / v[3];
}

static void at_depth(const f32 a[3], const f32 b[3], f32 depth, f32 out[3]) {
    const f32 da = -a[2], db = -b[2];
    const f32 t = fabsf(db - da) > 1e-12f ? (depth - da) / (db - da) : 0.0f;
    for (u32 k = 0; k < 3; ++k)
        out[k] = a[k] + (b[k] - a[k]) * t;
}

void de__cluster_bounds(const de__cluster_grid *g, const f32 inv_proj[4][4], u32 c, f32 lo[3],
                        f32 hi[3]) {
    const u32 tx = c % g->tiles_x;
    const u32 ty = (c / g->tiles_x) % g->tiles_y;
    const u32 k = c / (g->tiles_x * g->tiles_y);
    const f32 d0 = k == 0 ? g->near_z : de__cluster_slice_depth(g, k);
    const f32 d1 = k + 1u >= g->slices ? 1e20f : de__cluster_slice_depth(g, k + 1u);
    const f32 sx = (f32)g->width, sy = (f32)g->height;
    for (u32 j = 0; j < 3; ++j) {
        lo[j] = 1e30f;
        hi[j] = -1e30f;
    }
    for (u32 i = 0; i < 4; ++i) {
        f32 px = (f32)((tx + (i & 1u)) * g->tile_px), py = (f32)((ty + (i >> 1)) * g->tile_px);
        px = px < sx ? px : sx;
        py = py < sy ? py : sy;
        const f32 nx = px / sx * 2.0f - 1.0f, ny = 1.0f - py / sy * 2.0f;
        f32 a[3], b[3], p0[3], p1[3];
        unproject(inv_proj, nx, ny, 1.0f, a);
        unproject(inv_proj, nx, ny, 0.5f, b);
        at_depth(a, b, d0, p0);
        at_depth(a, b, d1, p1);
        for (u32 j = 0; j < 3; ++j) {
            const f32 mn = p0[j] < p1[j] ? p0[j] : p1[j], mx = p0[j] > p1[j] ? p0[j] : p1[j];
            lo[j] = mn < lo[j] ? mn : lo[j];
            hi[j] = mx > hi[j] ? mx : hi[j];
        }
    }
}

static u32 kind_of(const de__cluster_light *l) {
    u32 k;
    memcpy(&k, &l->shape[3], 4);
    return k;
}

bool de__cluster_light_touches(const de__cluster_light *l, const f32 lo[3], const f32 hi[3]) {
    const f32 *c = l->bound;
    const f32 r = l->bound[3];
    f32 d2 = 0.0f;
    for (u32 j = 0; j < 3; ++j) {
        const f32 q = clampf(c[j], lo[j], hi[j]) - c[j];
        d2 += q * q;
    }
    if (d2 > r * r)
        return false;
    if (kind_of(l) != DE__LIGHT_SPOT)
        return true;
    f32 sc[3], h[3], v[3];
    for (u32 j = 0; j < 3; ++j) {
        sc[j] = 0.5f * (lo[j] + hi[j]);
        h[j] = 0.5f * (hi[j] - lo[j]);
        v[j] = sc[j] - c[j];
    }
    const f32 sr = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    const f32 vlen2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    const f32 v1 = v[0] * l->axis[0] + v[1] * l->axis[1] + v[2] * l->axis[2];
    const f32 cos_o = l->color[3];
    const f32 sin_o = sqrtf(saturatef(1.0f - cos_o * cos_o));
    const f32 q = vlen2 - v1 * v1;
    const f32 dist = cos_o * sqrtf(q > 0.0f ? q : 0.0f) - v1 * sin_o;
    const bool angle_cull = dist > sr;
    const bool front_cull = v1 > sr + r;
    const bool back_cull = v1 < -sr;
    return !(angle_cull || front_cull || back_cull);
}

/* ---------- LTC ---------- */

/* (a x b).z theta / sin(theta) / (2 pi) of unit vectors a, b (Heitz 2017's fit). */
static f32 ltc_edge(const f32 a[3], const f32 b[3]) {
    const f32 x = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const f32 y = fabsf(x);
    const f32 aa = 0.8543985f + (0.4965155f + 0.0145206f * y) * y;
    const f32 bb = 3.4175940f + (4.1616724f + y) * y;
    const f32 v = aa / bb;
    const f32 m = 1.0f - x * x;
    const f32 ts = x > 0.0f ? v : 0.5f / sqrtf(m > 1e-7f ? m : 1e-7f) - v;
    return (a[0] * b[1] - a[1] * b[0]) * ts;
}

static void norm3(f32 v[3]) {
    const f32 l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.0f)
        for (u32 j = 0; j < 3; ++j)
            v[j] /= l;
}

f32 de__ltc_polygon(const f32 p[][3], u32 n) {
    f32 c[17][3];
    u32 m = 0;
    for (u32 i = 0; i < n && i < 16u; ++i) {
        const f32 *a = p[i], *b = p[(i + 1u) % n];
        const bool ia = a[2] >= 0.0f, ib = b[2] >= 0.0f;
        if (ia && m < 16u) {
            memcpy(c[m], a, sizeof(f32) * 3u);
            ++m;
        }
        if (ia != ib && m < 16u) {
            const f32 t = a[2] / (a[2] - b[2]);
            for (u32 j = 0; j < 3; ++j)
                c[m][j] = a[j] + (b[j] - a[j]) * t;
            ++m;
        }
    }
    if (m < 3u)
        return 0.0f;
    for (u32 i = 0; i < m; ++i)
        norm3(c[i]);
    f32 sum = 0.0f;
    for (u32 i = 0; i < m; ++i)
        sum += ltc_edge(c[i], c[(i + 1u) % m]);
    return fabsf(sum); /* the fit of ltc_edge already holds the 1 / (2 pi) */
}
