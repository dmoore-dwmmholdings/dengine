/*
 * DENG-271: world partition internals. The cell math is header-only (static inline) so the world
 * cook (tools/cook/world_cells.c) shares it without linking de_streaming.
 *
 * Cells are 256 m squares on the x/y plane (Z is up). Cell (cx, cy) covers
 * [cx*256, cx*256+256) x [cy*256, cy*256+256); its origin is (cx*256, cy*256, 0).
 * A cell's asset and bundle are both named "world/<world>/cell_<cx>_<cy>", cx and cy in decimal
 * with a '-' for negatives and no '+' or leading zeros.
 *
 * DENG-857: a DeTerrain's 3 x 3 tiles are blobs "world/<world>/terrain_<id>_<tx>_<ty>", each in
 * its own bundle; every cell bundle a tile overlaps depends on it (de__ttile_*, below).
 *
 * DENG-858: a cell whose entities reference entities of other cells (or are referenced from
 * them) has a cross-cell reference table, the blob "world/<world>/xref_<cx>_<cy>" in its own
 * cell bundle (de__xref_*, below).
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

/**
 * DENG-830: writes "world/<world>/proxy_<cx>_<cy>", the far proxy model (and its bundle) of a
 * cell; its mesh is the sub-asset "<that>#proxy". Returns false when it does not fit.
 */
static inline bool de__cell_proxy_name(char *buf, size_t cap, const char *world, i32 cx, i32 cy) {
    int n = snprintf(buf, cap, "world/%s/proxy_%d_%d", world, (int)cx, (int)cy);
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

/* "world/<world><kind><cx>_<cy>" with kind "/cell_" or "/proxy_" (DENG-830). */
static inline bool de__cell_kind_parse(const char *name, const char *world, const char *kind,
                                       i32 *cx, i32 *cy) {
    size_t wl = strlen(world), kl = strlen(kind);
    if (strncmp(name, "world/", 6) != 0 || strncmp(name + 6, world, wl) != 0 ||
        strncmp(name + 6 + wl, kind, kl) != 0)
        return false;
    const char *p = name + 6 + wl + kl;
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

/**
 * Parses a bundle name "world/<world>/cell_<cx>_<cy>" of world `world`. False for any other
 * name, also for non-canonical numbers ("+3", "03", "-0").
 */
static inline bool de__cell_name_parse(const char *name, const char *world, i32 *cx, i32 *cy) {
    return de__cell_kind_parse(name, world, "/cell_", cx, cy);
}

/** The same for a far proxy bundle "world/<world>/proxy_<cx>_<cy>" (DENG-830). */
static inline bool de__cell_proxy_parse(const char *name, const char *world, i32 *cx, i32 *cy) {
    return de__cell_kind_parse(name, world, "/proxy_", cx, cy);
}

/* ---- Terrain tiles (DENG-857) ----------------------------------------------------------- */

/** Samples on a side of a DeTerrain grid and of one tile (as DE_TERRAIN_RES, DE_TERRAIN_TILE). */
#define DE__TTILE_RES   33u
#define DE__TTILE_STEP  16u
/** Tiles on a side: ceil(33 / 16) = 3, as the terrain module's undo tiles. */
#define DE__TTILE_COUNT 3u
/** The tile blob: magic, version, then the fields of de__ttile_view, then the samples. */
#define DE__TTILE_MAGIC   "DETT"
#define DE__TTILE_VERSION 1u
#define DE__TTILE_HEADER  80u

/**
 * Writes "terrain_<id>_<tx>_<ty>" (`id` as 16 lowercase hex digits), the leaf of a terrain tile
 * blob. Returns false when it does not fit.
 */
static inline bool de__ttile_leaf(char *buf, size_t cap, u64 terrain, u32 tx, u32 ty) {
    int n = snprintf(buf, cap, "terrain_%016llx_%u_%u", (unsigned long long)terrain,
                     (unsigned)tx, (unsigned)ty);
    return n > 0 && (size_t)n < cap;
}

/** Writes "world/<world>/terrain_<id>_<tx>_<ty>", a tile's asset and bundle name. */
static inline bool de__ttile_name(char *buf, size_t cap, const char *world, u64 terrain, u32 tx,
                                  u32 ty) {
    char leaf[64];
    if (!de__ttile_leaf(leaf, sizeof leaf, terrain, tx, ty))
        return false;
    int n = snprintf(buf, cap, "world/%s/%s", world, leaf);
    return n > 0 && (size_t)n < cap;
}

/** Parses a tile bundle name of world `world`; false for any other or non-canonical name. */
static inline bool de__ttile_name_parse(const char *name, const char *world, u64 *terrain,
                                        u32 *tx, u32 *ty) {
    size_t wl = strlen(world);
    if (strncmp(name, "world/", 6) != 0 || strncmp(name + 6, world, wl) != 0 ||
        strncmp(name + 6 + wl, "/terrain_", 9) != 0)
        return false;
    const char *p = name + 6 + wl + 9;
    u64 v = 0;
    for (int i = 0; i < 16; ++i, ++p) {
        u32 d;
        if (*p >= '0' && *p <= '9')
            d = (u32)(*p - '0');
        else if (*p >= 'a' && *p <= 'f')
            d = (u32)(*p - 'a' + 10);
        else
            return false;
        v = (v << 4) | d;
    }
    if (*p != '_')
        return false;
    ++p;
    i32 x = 0, y = 0;
    if (!de__cell_parse_int(&p, '_', &x))
        return false;
    ++p;
    if (!de__cell_parse_int(&p, '\0', &y))
        return false;
    if (x < 0 || y < 0 || (u32)x >= DE__TTILE_COUNT || (u32)y >= DE__TTILE_COUNT)
        return false;
    *terrain = v;
    *tx = (u32)x;
    *ty = (u32)y;
    return true;
}

/** The first sample and the sample count of tile `t` on one axis (its far edge included). */
static inline void de__ttile_span(u32 t, u32 *first, u32 *count) {
    u32 f = t * DE__TTILE_STEP, e = f + DE__TTILE_STEP;
    if (e > DE__TTILE_RES - 1u)
        e = DE__TTILE_RES - 1u;
    *first = f;
    *count = e - f + 1u;
}

/**
 * A parsed tile blob. Grid column i is world +x, row j is world +y (Z up): sample (i, j) of the
 * tile is at origin + ((x0 + i) * spacing, (y0 + j) * spacing, heights / 65535 * height).
 * `heights` are little-endian u16 (read with de__ttile_height), `holes` one byte per sample, and
 * `splat` DE_TERRAIN_LAYERS (4) bytes per sample, all in row order.
 */
typedef struct de__ttile_view {
    u64 terrain; /**< the terrain entity's scene ID */
    u32 tx, ty, x0, y0, w, h;
    f64 origin[3];
    f64 spacing, height;
    const u8 *heights, *holes, *splat;
} de__ttile_view;

static inline u32 de__ttile_u32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline u64 de__ttile_u64(const u8 *p) {
    return (u64)de__ttile_u32(p) | (u64)de__ttile_u32(p + 4) << 32;
}

static inline f64 de__ttile_f64(const u8 *p) {
    u64 v = de__ttile_u64(p);
    f64 d;
    memcpy(&d, &v, sizeof d);
    return d;
}

static inline u16 de__ttile_height(const de__ttile_view *v, u32 i) {
    return (u16)(v->heights[2u * i] | v->heights[2u * i + 1u] << 8);
}

/** The blob size of a w x h tile. */
static inline u64 de__ttile_bytes(u32 w, u32 h) {
    return DE__TTILE_HEADER + (u64)w * h * 7u;
}

/** Parses a tile blob; false when it is not a version 1 tile or its size does not match. */
static inline bool de__ttile_parse(const void *data, u64 size, de__ttile_view *out) {
    const u8 *b = (const u8 *)data;
    if (b == NULL || size < DE__TTILE_HEADER || memcmp(b, DE__TTILE_MAGIC, 4) != 0 ||
        de__ttile_u32(b + 4) != DE__TTILE_VERSION)
        return false;
    de__ttile_view v;
    v.terrain = de__ttile_u64(b + 8);
    v.tx = de__ttile_u32(b + 16);
    v.ty = de__ttile_u32(b + 20);
    v.x0 = de__ttile_u32(b + 24);
    v.y0 = de__ttile_u32(b + 28);
    v.w = de__ttile_u32(b + 32);
    v.h = de__ttile_u32(b + 36);
    for (int i = 0; i < 3; ++i)
        v.origin[i] = de__ttile_f64(b + 40 + 8 * i);
    v.spacing = de__ttile_f64(b + 64);
    v.height = de__ttile_f64(b + 72);
    if (v.w == 0 || v.h == 0 || v.w > DE__TTILE_RES || v.h > DE__TTILE_RES ||
        size != de__ttile_bytes(v.w, v.h))
        return false;
    v.heights = b + DE__TTILE_HEADER;
    v.holes = v.heights + 2u * v.w * v.h;
    v.splat = v.holes + v.w * v.h;
    *out = v;
    return true;
}

/* ---- Cross-cell entity references (DENG-858) ------------------------------------------ */

/**
 * The reference table blob, little-endian:
 *   0  "DEXR"   4 u32 version   8 u32 entities in the cell scene
 *   12 u32 anchor count (A)     16 u32 reference count (R)     20 u32 0
 *   24 A x u64: the scene IDs of the cell's entities that are a reference's source or target,
 *      ascending
 *   then R records of DE__XREF_REC bytes: u64 source scene ID (an entity of this cell), u64
 *      target scene ID, i32 target cx, i32 target cy, the component type name and the field
 *      name, each NUL-padded to DE__XREF_NAME bytes. Ordered by source, component, field.
 * Scene IDs are the world's (`de-cook world` keeps them unique across the world).
 */
#define DE__XREF_MAGIC   "DEXR"
#define DE__XREF_VERSION 1u
#define DE__XREF_HEADER  24u
#define DE__XREF_NAME    48u
#define DE__XREF_REC     (24u + 2u * DE__XREF_NAME)

/** Writes "xref_<cx>_<cy>", the leaf of a cell's reference table. */
static inline bool de__xref_leaf(char *buf, size_t cap, i32 cx, i32 cy) {
    int n = snprintf(buf, cap, "xref_%d_%d", (int)cx, (int)cy);
    return n > 0 && (size_t)n < cap;
}

/** Writes "world/<world>/xref_<cx>_<cy>", the table's asset name (in the cell's bundle). */
static inline bool de__xref_name(char *buf, size_t cap, const char *world, i32 cx, i32 cy) {
    int n = snprintf(buf, cap, "world/%s/xref_%d_%d", world, (int)cx, (int)cy);
    return n > 0 && (size_t)n < cap;
}

/** The blob size of a table with `anchors` anchors and `refs` references. */
static inline u64 de__xref_bytes(u32 anchors, u32 refs) {
    return DE__XREF_HEADER + 8u * (u64)anchors + (u64)DE__XREF_REC * refs;
}

/** A checked table (pointers into the blob). */
typedef struct de__xref_view {
    u32 entity_count, anchor_count, ref_count;
    const u8 *anchors, *refs;
} de__xref_view;

/** One reference of a table. The names point into the blob and are NUL-terminated. */
typedef struct de__xref_rec {
    u64 source, target;
    i32 tcx, tcy;
    const char *component, *field;
} de__xref_rec;

/** Parses a table; false when it is not a version 1 table, its size does not match or a name
 * is not NUL-terminated. */
static inline bool de__xref_parse(const void *data, u64 size, de__xref_view *out) {
    const u8 *b = (const u8 *)data;
    if (b == NULL || size < DE__XREF_HEADER || memcmp(b, DE__XREF_MAGIC, 4) != 0 ||
        de__ttile_u32(b + 4) != DE__XREF_VERSION)
        return false;
    de__xref_view v;
    v.entity_count = de__ttile_u32(b + 8);
    v.anchor_count = de__ttile_u32(b + 12);
    v.ref_count = de__ttile_u32(b + 16);
    if (size != de__xref_bytes(v.anchor_count, v.ref_count))
        return false;
    v.anchors = b + DE__XREF_HEADER;
    v.refs = v.anchors + 8u * (u64)v.anchor_count;
    for (u32 i = 0; i < v.ref_count; ++i) {
        const u8 *r = v.refs + (u64)DE__XREF_REC * i;
        if (memchr(r + 24, 0, DE__XREF_NAME) == NULL || r[24] == 0 ||
            memchr(r + 24 + DE__XREF_NAME, 0, DE__XREF_NAME) == NULL ||
            r[24 + DE__XREF_NAME] == 0)
            return false;
    }
    *out = v;
    return true;
}

static inline u64 de__xref_anchor(const de__xref_view *v, u32 i) {
    return de__ttile_u64(v->anchors + 8u * i);
}

static inline de__xref_rec de__xref_at(const de__xref_view *v, u32 i) {
    const u8 *r = v->refs + (u64)DE__XREF_REC * i;
    de__xref_rec o;
    o.source = de__ttile_u64(r);
    o.target = de__ttile_u64(r + 8);
    o.tcx = (i32)de__ttile_u32(r + 16);
    o.tcy = (i32)de__ttile_u32(r + 20);
    o.component = (const char *)(r + 24);
    o.field = (const char *)(r + 24 + DE__XREF_NAME);
    return o;
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
