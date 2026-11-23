/* terrain.c: the CPU terrain model (DENG-580). See terrain.h. A port of the DIDE mock engine's
 * terrain (protocol/mock/src/terrain.rs): same order of operations, f32 math through de_dmath. */
#include "world/terrain/terrain.h"

#include <dengine/de_math.h>
#include <stdio.h>
#include <string.h>

#define RES   DE_TERRAIN_RES
#define N     DE_TERRAIN_SAMPLES
#define SLOTS DE_TERRAIN_LAYERS
#define TILE  DE_TERRAIN_TILE

const char *const de__terrain_tools[DE_TERRAIN_TOOL_COUNT] = {
    "raise", "lower",          "smooth",       "flatten", "ramp",  "noise",
    "terrace", "erodeHydraulic", "erodeThermal", "stamp",   "paint", "hole"};

static f32 clamp01(f32 v) {
    if (!(v > 0.0f)) /* also NaN, as Rust's saturating cast gives 0 */
        return 0.0f;
    return v > 1.0f ? 1.0f : v;
}

static f32 clampf(f32 v, f32 lo, f32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static f32 maxf(f32 a, f32 b) {
    return a > b ? a : b;
}

/* f64 round half away from zero, for values well inside i64. */
static f64 round64(f64 v) {
    return v >= 0.0 ? (f64)(i64)(v + 0.5) : -(f64)(i64)(-v + 0.5);
}

bool de__terrain_valid(const DeTerrain *t) {
    return t != NULL && t->resolution == RES && t->size > 0.0f && t->height > 0.0f;
}

f32 de__terrain_spacing(const DeTerrain *t) {
    return t->size / (f32)(RES - 1u);
}

static f32 metres(const DeTerrain *t, u32 i) {
    return (f32)t->heights[i] / 65535.0f * t->height;
}

static void set_metres(DeTerrain *t, u32 i, f32 m) {
    t->heights[i] = (u16)de_dround(clamp01(m / t->height) * 65535.0f);
}

static void world_of(const DeTerrain *t, f64 ox, f64 oz, u32 x, u32 y, f32 *wx, f32 *wz) {
    const f32 d = de__terrain_spacing(t);
    *wx = (f32)ox + (f32)x * d;
    *wz = (f32)oz + (f32)y * d;
}

bool de__terrain_nearest(const DeTerrain *t, f64 ox, f64 oz, f64 wx, f64 wz, u32 *x, u32 *y) {
    const f64 d = (f64)de__terrain_spacing(t);
    const f64 fx = (wx - ox) / d, fz = (wz - oz) / d;
    const f64 max = (f64)(RES - 1u);
    if (!(fx >= -1.0 && fx <= max + 1.0 && fz >= -1.0 && fz <= max + 1.0))
        return false; /* NaN or far off, checked before the cast */
    const f64 rx = round64(fx), rz = round64(fz);
    if (rx < 0.0 || rx > max || rz < 0.0 || rz > max)
        return false;
    *x = (u32)rx;
    *y = (u32)rz;
    return true;
}

static f32 slope_at(const DeTerrain *t, u32 x, u32 y) {
    const u32 x0 = x > 0 ? x - 1 : 0, x1 = x + 1 < RES ? x + 1 : RES - 1;
    const u32 y0 = y > 0 ? y - 1 : 0, y1 = y + 1 < RES ? y + 1 : RES - 1;
    const f32 d = de__terrain_spacing(t);
    const f32 gx = (metres(t, y * RES + x1) - metres(t, y * RES + x0)) /
                   (d * (f32)(x1 - x0 > 0 ? x1 - x0 : 1));
    const f32 gz = (metres(t, y1 * RES + x) - metres(t, y0 * RES + x)) /
                   (d * (f32)(y1 - y0 > 0 ? y1 - y0 : 1));
    return de_datan(de_dsqrt(gx * gx + gz * gz)) * (180.0f / DE_PI);
}

static f32 curvature_at(const DeTerrain *t, u32 x, u32 y) {
    const u32 x0 = x > 0 ? x - 1 : 0, x1 = x + 1 < RES ? x + 1 : RES - 1;
    const u32 y0 = y > 0 ? y - 1 : 0, y1 = y + 1 < RES ? y + 1 : RES - 1;
    const f32 d = de__terrain_spacing(t);
    return (metres(t, y * RES + x0) + metres(t, y * RES + x1) + metres(t, y0 * RES + x) +
            metres(t, y1 * RES + x) - 4.0f * metres(t, y * RES + x)) /
           (d * d);
}

static bool within(f32 v, f32 lo, f32 hi) {
    return hi <= lo || (v >= lo && v <= hi);
}

static void weights_at(const DeTerrain *t, u32 x, u32 y, bool rules, f32 *w) {
    const u32 i = y * RES + x;
    for (u32 s = 0; s < SLOTS; ++s)
        w[s] = (f32)t->splat[i * SLOTS + s] / 255.0f;
    if (!rules)
        return;
    const f32 slope = slope_at(t, x, y), h = metres(t, i), c = curvature_at(t, x, y);
    for (u32 r = 0; r < DE_TERRAIN_RULES; ++r) {
        const de_terrain_rule *q = &t->rules[r];
        if (!q->enabled || q->layer >= SLOTS)
            continue;
        if (within(slope, q->min_slope, q->max_slope) &&
            within(h, q->min_height, q->max_height) &&
            within(c, q->min_curvature, q->max_curvature))
            for (u32 s = 0; s < SLOTS; ++s)
                w[s] = s == q->layer ? 1.0f : 0.0f;
    }
}

bool de__terrain_sample_at(const DeTerrain *t, f64 ox, f64 oz, f64 wx, f64 wz,
                           de_terrain_sample *out) {
    u32 x, y;
    if (!de__terrain_nearest(t, ox, oz, wx, wz, &x, &y))
        return false;
    const u32 i = y * RES + x;
    out->height = metres(t, i);
    out->slope = slope_at(t, x, y);
    weights_at(t, x, y, true, out->weights);
    weights_at(t, x, y, false, out->baked);
    out->hole = t->holes[i] != 0;
    return true;
}

/* ---------- Strokes ---------- */

typedef struct area {
    u32 n;
    u16 idx[N];
    f32 w[N];
} area;

static f32 falloff(const de_terrain_stroke *s, f32 t) {
    if (t >= 1.0f)
        return 0.0f;
    const u32 n = s->falloff_count;
    if (n == 0)
        return 1.0f - t;
    if (n == 1)
        return s->falloff[0];
    const f32 p = t * (f32)(n - 1);
    u32 i = (u32)de_dfloor(p);
    if (i > n - 2)
        i = n - 2;
    const f32 f = p - (f32)i;
    return clamp01(s->falloff[i] * (1.0f - f) + s->falloff[i + 1] * f);
}

static void dab(const DeTerrain *t, f64 ox, f64 oz, const de_terrain_stroke *s, f32 cx, f32 cz,
                f32 pressure, area *out) {
    out->n = 0;
    if (!(s->radius > 0.0f))
        return;
    for (u32 y = 0; y < RES; ++y)
        for (u32 x = 0; x < RES; ++x) {
            f32 wx, wz;
            world_of(t, ox, oz, x, y, &wx, &wz);
            const f32 d = de_dsqrt((wx - cx) * (wx - cx) + (wz - cz) * (wz - cz));
            const f32 w =
                clamp01(s->strength) * falloff(s, d / s->radius) * clamp01(pressure);
            if (w > 0.0f) {
                out->idx[out->n] = (u16)(y * RES + x);
                out->w[out->n++] = w;
            }
        }
}

static void tile_area(const DeTerrain *t, f64 ox, f64 oz, const de_terrain_stroke *s, f32 cx,
                      f32 cz, area *out) {
    out->n = 0;
    u32 x, y;
    if (!de__terrain_nearest(t, ox, oz, (f64)cx, (f64)cz, &x, &y))
        return;
    const u32 tx = x / TILE, ty = y / TILE;
    for (u32 yy = ty * TILE; yy < (ty + 1) * TILE && yy < RES; ++yy)
        for (u32 xx = tx * TILE; xx < (tx + 1) * TILE && xx < RES; ++xx) {
            out->idx[out->n] = (u16)(yy * RES + xx);
            out->w[out->n++] = clamp01(s->strength);
        }
}

/* The 4-neighbours of i: left, right, up, down. Returns the count. */
static u32 neighbours(u32 i, u32 *out) {
    const u32 x = i % RES, y = i / RES;
    u32 n = 0;
    if (x > 0)
        out[n++] = i - 1;
    if (x + 1 < RES)
        out[n++] = i + 1;
    if (y > 0)
        out[n++] = i - RES;
    if (y + 1 < RES)
        out[n++] = i + RES;
    return n;
}

static void erode(DeTerrain *t, const area *ar, f32 talus, f32 rate, u32 iterations) {
    static f32 h[N]; /* the editor applies edits on the main thread only */
    if (iterations < 1)
        iterations = 1;
    if (iterations > DE_TERRAIN_MAX_ITERATIONS)
        iterations = DE_TERRAIN_MAX_ITERATIONS;
    for (u32 it = 0; it < iterations; ++it) {
        for (u32 i = 0; i < N; ++i)
            h[i] = metres(t, i);
        for (u32 k = 0; k < ar->n; ++k) {
            const u32 i = ar->idx[k];
            u32 nb[4];
            const u32 nn = neighbours(i, nb);
            if (nn == 0)
                continue;
            u32 low = nb[0];
            for (u32 j = 1; j < nn; ++j)
                if (h[nb[j]] < h[low]) /* the first lowest, as Rust's min_by */
                    low = nb[j];
            const f32 diff = h[i] - h[low] - talus;
            if (diff > 0.0f) {
                const f32 moved = diff * rate * ar->w[k];
                h[i] -= moved;
                h[low] += moved;
            }
        }
        for (u32 i = 0; i < N; ++i)
            set_metres(t, i, h[i]);
    }
}

static f32 hash(i32 x, i32 z, u32 seed) {
    u32 h = (u32)x * 374761393u ^ (u32)z * 668265263u ^ seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return ((f32)h / (f32)UINT32_MAX) * 2.0f - 1.0f;
}

f32 de__terrain_noise(f32 x, f32 z, u32 seed) {
    const f32 x0 = de_dfloor(x), z0 = de_dfloor(z);
    const f32 fx = x - x0, fz = z - z0;
    const f32 sx = fx * fx * (3.0f - 2.0f * fx), sz = fz * fz * (3.0f - 2.0f * fz);
    const i32 ix = (i32)x0, iz = (i32)z0;
    const f32 a = hash(ix, iz, seed) + (hash(ix + 1, iz, seed) - hash(ix, iz, seed)) * sx;
    const f32 b =
        hash(ix, iz + 1, seed) + (hash(ix + 1, iz + 1, seed) - hash(ix, iz + 1, seed)) * sx;
    return a + (b - a) * sz;
}

static f32 grid_at(const de_terrain_grid *g, f32 u, f32 v) {
    u = clamp01(u) * (f32)(g->w - 1);
    v = clamp01(v) * (f32)(g->h - 1);
    const u32 x0 = (u32)de_dfloor(u), y0 = (u32)de_dfloor(v);
    const u32 x1 = x0 + 1 < g->w ? x0 + 1 : g->w - 1, y1 = y0 + 1 < g->h ? y0 + 1 : g->h - 1;
    const f32 fx = u - (f32)x0, fy = v - (f32)y0;
#define G(x, y) g->v[(y) * g->w + (x)]
    const f32 a = G(x0, y0) + (G(x1, y0) - G(x0, y0)) * fx;
    const f32 b = G(x0, y1) + (G(x1, y1) - G(x0, y1)) * fx;
#undef G
    return a + (b - a) * fy;
}

/* Adds (or removes) weight w of slot l in one sample's weights, keeping the sum at 255. */
static void paint(u8 *px, u32 l, f32 w, bool erase) {
    f32 f[SLOTS];
    f32 others = 0.0f;
    for (u32 i = 0; i < SLOTS; ++i) {
        f[i] = (f32)px[i] / 255.0f;
        if (i != l)
            others += f[i];
    }
    const f32 nw = erase ? f[l] * (1.0f - w) : f[l] + (1.0f - f[l]) * w;
    if (erase && others <= 0.0f)
        return;
    const f32 rest = 1.0f - nw;
    for (u32 i = 0; i < SLOTS; ++i)
        f[i] = i == l ? nw : (others > 0.0f ? f[i] / others * rest : 0.0f);
    i32 out[SLOTS], sum = 0;
    for (u32 i = 0; i < SLOTS; ++i) {
        const f32 r = de_dround(f[i] * 255.0f);
        out[i] = r <= 0.0f ? 0 : (r >= 255.0f ? 255 : (i32)r);
        sum += out[i];
    }
    i32 v = out[l] + 255 - sum;
    out[l] = v < 0 ? 0 : (v > 255 ? 255 : v);
    for (u32 i = 0; i < SLOTS; ++i)
        px[i] = (u8)out[i];
}

static bool tool_is(const de_terrain_stroke *s, const char *name) {
    const size_t n = strlen(name);
    return s->tool_len == n && memcmp(s->tool, name, n) == 0;
}

static bool ramp(DeTerrain *t, f64 ox, f64 oz, const de_terrain_stroke *s) {
    const f32 *a = s->points, *b = s->points + s->point_count - 4;
    const f32 dx = b[0] - a[0], dz = b[2] - a[2];
    const f32 len2 = maxf(dx * dx + dz * dz, 1e-6f);
    for (u32 y = 0; y < RES; ++y)
        for (u32 x = 0; x < RES; ++x) {
            f32 wx, wz;
            world_of(t, ox, oz, x, y, &wx, &wz);
            const f32 u = clamp01(((wx - a[0]) * dx + (wz - a[2]) * dz) / len2);
            const f32 px = a[0] + dx * u, pz = a[2] + dz * u;
            const f32 d = de_dsqrt((wx - px) * (wx - px) + (wz - pz) * (wz - pz));
            const f32 w = clamp01(s->strength) * falloff(s, d / maxf(s->radius, 1e-3f));
            if (w > 0.0f) {
                const u32 i = y * RES + x;
                const f32 target = a[1] + (b[1] - a[1]) * u;
                const f32 m = metres(t, i);
                set_metres(t, i, m + (target - m) * w);
            }
        }
    return true;
}

bool de__terrain_stroke(DeTerrain *t, f64 ox, f64 oz, const de_terrain_stroke *s,
                        const de_terrain_grid *stamp, char *err, size_t err_cap) {
    if (s->point_count == 0 || s->point_count % 4 != 0) {
        snprintf(err, err_cap, "terrain.stroke needs points as [x, y, z, pressure] dabs");
        return false;
    }
    if (!de__terrain_valid(t)) {
        snprintf(err, err_cap, "the terrain grid is not %ux%u", RES, RES);
        return false;
    }
    const f32 step = maxf(s->radius * 0.1f, 0.1f);
    if (tool_is(s, "ramp"))
        return ramp(t, ox, oz, s);
    u32 tool = DE_TERRAIN_TOOL_COUNT;
    for (u32 k = 0; k < DE_TERRAIN_TOOL_COUNT; ++k)
        if (tool_is(s, de__terrain_tools[k]))
            tool = k;
    if (tool == DE_TERRAIN_TOOL_COUNT) {
        snprintf(err, err_cap, "unknown terrain tool `%.*s`", (int)s->tool_len, s->tool);
        return false;
    }
    const bool is_paint = tool_is(s, "paint");
    if (is_paint && (!s->has_layer || s->layer >= SLOTS)) {
        snprintf(err, err_cap, "paint needs a layer slot below %u", SLOTS);
        return false;
    }
    const bool erosion = tool_is(s, "erodeHydraulic") || tool_is(s, "erodeThermal");
    static area ar;     /* main thread only */
    static f32 before[N];
    for (u32 k = 0; k < s->point_count; k += 4) {
        const f32 cx = s->points[k], cy = s->points[k + 1], cz = s->points[k + 2];
        const f32 p = s->points[k + 3];
        if (erosion && s->area_tile)
            tile_area(t, ox, oz, s, cx, cz, &ar);
        else
            dab(t, ox, oz, s, cx, cz, p, &ar);
        if (tool_is(s, "raise") || tool_is(s, "lower")) {
            const f32 sign = tool_is(s, "raise") ? 1.0f : -1.0f;
            for (u32 j = 0; j < ar.n; ++j)
                set_metres(t, ar.idx[j], metres(t, ar.idx[j]) + sign * ar.w[j] * step);
        } else if (tool_is(s, "smooth")) {
            for (u32 i = 0; i < N; ++i)
                before[i] = metres(t, i);
            for (u32 j = 0; j < ar.n; ++j) {
                const u32 i = ar.idx[j];
                u32 nb[4];
                const u32 nn = neighbours(i, nb);
                f32 sum = before[i], n = 1.0f;
                for (u32 q = 0; q < nn; ++q) {
                    sum += before[nb[q]];
                    n += 1.0f;
                }
                set_metres(t, i, before[i] + (sum / n - before[i]) * ar.w[j]);
            }
        } else if (tool_is(s, "flatten")) {
            const f32 target = s->has_target ? s->target : cy;
            for (u32 j = 0; j < ar.n; ++j) {
                const f32 m = metres(t, ar.idx[j]);
                set_metres(t, ar.idx[j], m + (target - m) * ar.w[j]);
            }
        } else if (tool_is(s, "noise")) {
            const f32 scale = maxf(s->has_scale ? s->scale : 4.0f, 0.1f);
            const u32 seed = s->has_seed ? s->seed : 1u;
            for (u32 j = 0; j < ar.n; ++j) {
                const u32 i = ar.idx[j];
                f32 wx, wz;
                world_of(t, ox, oz, i % RES, i / RES, &wx, &wz);
                const f32 m = metres(t, i);
                set_metres(t, i, m + ar.w[j] * step * de__terrain_noise(wx / scale, wz / scale, seed));
            }
        } else if (tool_is(s, "terrace")) {
            const f32 st = maxf(s->has_step ? s->step : 2.0f, 0.01f);
            for (u32 j = 0; j < ar.n; ++j) {
                const f32 m = metres(t, ar.idx[j]);
                set_metres(t, ar.idx[j], m + (de_dround(m / st) * st - m) * ar.w[j]);
            }
        } else if (tool_is(s, "erodeThermal")) {
            const f32 talus = de__terrain_spacing(t) * de_dtan(35.0f * (DE_PI / 180.0f));
            erode(t, &ar, talus, 0.25f, s->has_iterations ? s->iterations : 10u);
        } else if (tool_is(s, "erodeHydraulic")) {
            erode(t, &ar, 0.0f, 0.1f, s->has_iterations ? s->iterations : 10u);
        } else if (tool_is(s, "stamp")) {
            if (stamp == NULL || stamp->w < 2 || stamp->h < 2) {
                snprintf(err, err_cap, "stamp needs a stamp heightmap");
                return false;
            }
            f32 sn, cs;
            de_dsincos((s->has_rotation ? s->rotation : 0.0f) * (DE_PI / 180.0f), &sn, &cs);
            const f32 scale = s->has_scale ? s->scale : 4.0f;
            for (u32 j = 0; j < ar.n; ++j) {
                const u32 i = ar.idx[j];
                f32 wx, wz;
                world_of(t, ox, oz, i % RES, i / RES, &wx, &wz);
                const f32 lx = (wx - cx) / s->radius, lz = (wz - cz) / s->radius;
                const f32 u = (lx * cs + lz * sn + 1.0f) * 0.5f;
                const f32 v = (-lx * sn + lz * cs + 1.0f) * 0.5f;
                const f32 m = metres(t, i);
                set_metres(t, i, m + (cy + grid_at(stamp, u, v) * scale - m) * ar.w[j]);
            }
        } else if (is_paint) {
            for (u32 j = 0; j < ar.n; ++j)
                paint(&t->splat[ar.idx[j] * SLOTS], s->layer, ar.w[j], s->erase);
        } else { /* hole */
            for (u32 j = 0; j < ar.n; ++j)
                if (ar.w[j] >= 0.5f)
                    t->holes[ar.idx[j]] = s->erase ? 0u : 1u;
        }
    }
    return true;
}

void de__terrain_bake(DeTerrain *t) {
    static u8 splat[N * SLOTS];
    memcpy(splat, t->splat, sizeof splat);
    for (u32 y = 0; y < RES; ++y)
        for (u32 x = 0; x < RES; ++x) {
            f32 w[SLOTS];
            weights_at(t, x, y, true, w);
            for (u32 s = 0; s < SLOTS; ++s)
                splat[(y * RES + x) * SLOTS + s] = (u8)de_dround(clampf(w[s], 0.0f, 1.0f) * 255.0f);
        }
    memcpy(t->splat, splat, sizeof splat);
    for (u32 r = 0; r < DE_TERRAIN_RULES; ++r)
        t->rules[r].enabled = false;
}

/* ---------- Heightmaps and tiles ---------- */

/* ---------- Carve (DENG-777) ---------- */

static bool finite32(f32 v) {
    return v == v && v - v == 0.0f; /* not NaN, not infinite */
}

/* The horizontal distance from (x, z) to the polyline and its height at the nearest point; the
 * first segment wins a tie. */
static void nearest_on(const f32 *path, u32 n, f32 x, f32 z, f32 *dist, f32 *at) {
    bool any = false;
    for (u32 i = 0; i + 1 < n; ++i) {
        const f32 *a = path + 3 * i, *b = path + 3 * (i + 1);
        const f32 dx = b[0] - a[0], dz = b[2] - a[2];
        const f32 len2 = dx * dx + dz * dz;
        const f32 s = len2 > 0.0f ? clampf(((x - a[0]) * dx + (z - a[2]) * dz) / len2, 0.0f, 1.0f)
                                  : 0.0f;
        const f32 px = a[0] + dx * s, pz = a[2] + dz * s;
        const f32 d = de_dsqrt((x - px) * (x - px) + (z - pz) * (z - pz));
        if (!any || d < *dist) {
            any = true;
            *dist = d;
            *at = a[1] + (b[1] - a[1]) * s;
        }
    }
}

bool de__terrain_carve(DeTerrain *t, f64 ox, f64 oy, f64 oz, const f32 *path, u32 n, f32 width,
                       f32 falloff, f32 depth) {
    if (path == NULL || n < 2 || !finite32(width) || !(width > 0.0f) || !finite32(falloff) ||
        falloff < 0.0f || !finite32(depth))
        return false;
    for (u32 i = 0; i < 3 * n; ++i)
        if (!finite32(path[i]))
            return false;
    const f32 half = width / 2.0f;
    for (u32 y = 0; y < RES; ++y)
        for (u32 x = 0; x < RES; ++x) {
            f32 wx, wz;
            world_of(t, ox, oz, x, y, &wx, &wz);
            f32 d = 0.0f, at = 0.0f;
            nearest_on(path, n, wx, wz, &d, &at);
            f32 w;
            if (d <= half) {
                w = 1.0f;
            } else if (falloff > 0.0f && d < half + falloff) {
                const f32 s = 1.0f - (d - half) / falloff;
                w = s * s * (3.0f - 2.0f * s);
            } else {
                continue;
            }
            const u32 i = y * RES + x;
            const f32 target = at - (f32)oy - depth;
            const f32 h = metres(t, i);
            set_metres(t, i, h + (target - h) * w);
        }
    return true;
}

void de__terrain_region(i32 tx, i32 ty, u32 *x0, u32 *y0, u32 *w, u32 *h) {
    if (tx < 0 || ty < 0) {
        *x0 = *y0 = 0;
        *w = *h = RES;
        return;
    }
    *x0 = (u32)tx * TILE;
    *y0 = (u32)ty * TILE;
    *w = TILE + 1 < RES - *x0 ? TILE + 1 : RES - *x0;
    *h = TILE + 1 < RES - *y0 ? TILE + 1 : RES - *y0;
}

void de__terrain_import(DeTerrain *t, const de_terrain_grid *g, i32 tx, i32 ty, bool metres_in) {
    u32 x0, y0, w, h;
    de__terrain_region(tx, ty, &x0, &y0, &w, &h);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const f32 u = (f32)x / (f32)(w > 1 ? w - 1 : 1);
            const f32 v = (f32)y / (f32)(h > 1 ? h - 1 : 1);
            const f32 s = grid_at(g, u, v);
            const u32 i = (y0 + y) * RES + x0 + x;
            if (metres_in)
                set_metres(t, i, s);
            else
                t->heights[i] = (u16)de_dround(clamp01(s) * 65535.0f);
        }
}

void de__terrain_export_metres(const DeTerrain *t, i32 tx, i32 ty, f32 *out) {
    u32 x0, y0, w, h;
    de__terrain_region(tx, ty, &x0, &y0, &w, &h);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x)
            out[y * w + x] = metres(t, (y0 + y) * RES + x0 + x);
}

u32 de__terrain_tile_indices(u32 tx, u32 ty, u16 *out) {
    u32 n = 0;
    for (u32 y = ty * TILE; y < (ty + 1) * TILE && y < RES; ++y)
        for (u32 x = tx * TILE; x < (tx + 1) * TILE && x < RES; ++x)
            out[n++] = (u16)(y * RES + x);
    return n;
}

bool de__terrain_tile_changed(const DeTerrain *a, const DeTerrain *b, u32 tx, u32 ty) {
    u16 idx[TILE * TILE];
    const u32 n = de__terrain_tile_indices(tx, ty, idx);
    for (u32 k = 0; k < n; ++k) {
        const u32 i = idx[k];
        if (a->heights[i] != b->heights[i] || a->holes[i] != b->holes[i] ||
            memcmp(&a->splat[i * SLOTS], &b->splat[i * SLOTS], SLOTS) != 0)
            return true;
    }
    return false;
}

bool de__terrain_rules_equal(const DeTerrain *a, const DeTerrain *b) {
    for (u32 r = 0; r < DE_TERRAIN_RULES; ++r) {
        const de_terrain_rule *x = &a->rules[r], *y = &b->rules[r];
        if (x->enabled != y->enabled || x->layer != y->layer || x->min_slope != y->min_slope ||
            x->max_slope != y->max_slope || x->min_height != y->min_height ||
            x->max_height != y->max_height || x->min_curvature != y->min_curvature ||
            x->max_curvature != y->max_curvature)
            return false;
    }
    return true;
}
