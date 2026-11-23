/*
 * DENG-271: world partition internals. The cell math is header-only (static inline) so the world
 * cook (tools/cook/world_cells.c) shares it without linking de_streaming.
 *
 * Cells are 256 m squares on the x/y plane (Z is up). Cell (cx, cy) covers
 * [cx*256, cx*256+256) x [cy*256, cy*256+256); its origin is (cx*256, cy*256, 0).
 * A cell's asset and bundle are both named "world/<world>/cell_<cx>_<cy>", cx and cy in decimal
 * with a '-' for negatives and no '+' or leading zeros.
 */
#ifndef DE_STREAMING_INTERNAL_H
#define DE_STREAMING_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_math.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DE__CELL_SIZE 256.0
/** |cx|, |cy| stay below this (2^31 / 256 cells would be 550 million km; this is 2^24 cells). */
#define DE__CELL_COORD_MAX 16777215

static inline i32 de__cell_coord(f64 v) {
    f64 c = floor(v / DE__CELL_SIZE);
    if (!(c > -(f64)DE__CELL_COORD_MAX)) /* also NaN */
        return -DE__CELL_COORD_MAX;
    if (c > (f64)DE__CELL_COORD_MAX)
        return DE__CELL_COORD_MAX;
    return (i32)c;
}

static inline de_dvec3 de__cell_origin(i32 cx, i32 cy) {
    de_dvec3 o;
    o.x = (f64)cx * DE__CELL_SIZE;
    o.y = (f64)cy * DE__CELL_SIZE;
    o.z = 0.0;
    return o;
}

/** Squared 2D distance from (x, y) to the nearest point of cell (cx, cy); 0 inside it. */
static inline f64 de__cell_dist2(i32 cx, i32 cy, f64 x, f64 y) {
    f64 x0 = (f64)cx * DE__CELL_SIZE, y0 = (f64)cy * DE__CELL_SIZE;
    f64 dx = x < x0 ? x0 - x : (x > x0 + DE__CELL_SIZE ? x - (x0 + DE__CELL_SIZE) : 0.0);
    f64 dy = y < y0 ? y0 - y : (y > y0 + DE__CELL_SIZE ? y - (y0 + DE__CELL_SIZE) : 0.0);
    return dx * dx + dy * dy;
}

/** Writes "cell_<cx>_<cy>". Returns false when it does not fit. */
static inline bool de__cell_leaf(char *buf, size_t cap, i32 cx, i32 cy) {
    int n = snprintf(buf, cap, "cell_%d_%d", (int)cx, (int)cy);
    return n > 0 && (size_t)n < cap;
}

/** Writes "world/<world>/cell_<cx>_<cy>". Returns false when it does not fit. */
static inline bool de__cell_name(char *buf, size_t cap, const char *world, i32 cx, i32 cy) {
    int n = snprintf(buf, cap, "world/%s/cell_%d_%d", world, (int)cx, (int)cy);
    return n > 0 && (size_t)n < cap;
}

/* Parses a canonical decimal i32 at *s up to `end` char; advances *s. */
static inline bool de__cell_parse_int(const char **s, char end, i32 *out) {
    const char *p = *s;
    bool neg = false;
    if (*p == '-') {
        neg = true;
        ++p;
    }
    if (*p < '0' || *p > '9')
        return false;
    if (*p == '0' && (p[1] >= '0' && p[1] <= '9'))
        return false; /* leading zero */
    i64 v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > DE__CELL_COORD_MAX)
            return false;
        ++p;
    }
    if (*p != end || (neg && v == 0))
        return false;
    *out = (i32)(neg ? -v : v);
    *s = p;
    return true;
}

/**
 * Parses a bundle name "world/<world>/cell_<cx>_<cy>" of world `world`. False for any other
 * name, also for non-canonical numbers ("+3", "03", "-0").
 */
static inline bool de__cell_name_parse(const char *name, const char *world, i32 *cx, i32 *cy) {
    size_t wl = strlen(world);
    if (strncmp(name, "world/", 6) != 0 || strncmp(name + 6, world, wl) != 0 ||
        strncmp(name + 6 + wl, "/cell_", 6) != 0)
        return false;
    const char *p = name + 12 + wl;
    i32 x = 0, y = 0;
    if (!de__cell_parse_int(&p, '_', &x))
        return false;
    ++p;
    if (!de__cell_parse_int(&p, '\0', &y))
        return false;
    *cx = x;
    *cy = y;
    return true;
}

/** One cell and a sort key. */
typedef struct de__cell_key {
    f64 dist2;
    i32 cx, cy;
} de__cell_key;

/** Order: nearest first, then cx, then cy. */
static inline int de__cell_key_cmp(const de__cell_key *a, const de__cell_key *b) {
    if (a->dist2 != b->dist2)
        return a->dist2 < b->dist2 ? -1 : 1;
    if (a->cx != b->cx)
        return a->cx < b->cx ? -1 : 1;
    if (a->cy != b->cy)
        return a->cy < b->cy ? -1 : 1;
    return 0;
}

/* ---- cell_grid.c ---- */

/** Insertion sort by de__cell_key_cmp (stable, no allocation; n is small in practice). */
void de__cell_keys_sort(de__cell_key *keys, u32 n);

/**
 * Writes the cells whose rectangle is within `radius` of (x, y), ordered by de__cell_key_cmp,
 * at most `cap` of them. Returns how many cells there are in total (may exceed `cap`).
 */
u32 de__cells_in_radius(f64 x, f64 y, f64 radius, de__cell_key *out, u32 cap);

/* ---- far_proxy.c ---- */

typedef struct de__far_proxy {
    f32 alpha;   /**< 1 = proxy only, 0 = cell only */
    bool active; /**< the proxy is drawn */
} de__far_proxy;

/** The cell started unloaded or was unloaded: proxy fully visible. */
void de__far_proxy_reset(de__far_proxy *fp);
/** Advances one frame; `cell_loaded` is the cell's state, `fade` the fade time in seconds. */
void de__far_proxy_step(de__far_proxy *fp, bool cell_loaded, f32 dt, f32 fade);

#endif /* DE_STREAMING_INTERNAL_H */
