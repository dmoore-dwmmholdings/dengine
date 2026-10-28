/*
 * tilemap_cooked.h: the cooked `tilemap` format (DTMP v1, SPEC §21.4; DENG-80). Header-only: the
 * importer (tools/cook/importers/tilemap/) writes it and the runtime (src/gameplay/tilemap/) reads
 * it through de__tm_view_parse, which checks every count, offset and index before anything uses
 * them.
 *
 * Little-endian, 4-byte aligned records, no padding bytes left undefined. Cooking the same input
 * twice gives the same bytes. Layout, in this order:
 *
 *   header          de__tm_header (64 bytes)
 *   tilesets[n]     8 u32: texture asset ID lo, hi, first_tile, tile_count, columns, image_w,
 *                   image_h, name (string offset)
 *   tiles[n]        11 x 4 bytes: f32 u, v, du, dv (texture UV rect, image space, v down), f32
 *                   w, h (sprite size in map units), u32 tileset, anim (index + 1, 0 = none),
 *                   shape_first, shape_count, flags (DE__TM_TILE_*)
 *   layers[n]       14 x 4 bytes: u32 name, flags (DE__TM_LAYER_*), width, height (cells),
 *                   chunks_x, chunks_y, chunk_first, f32 cell_w, cell_h (map units), origin_u,
 *                   origin_v (map position of the top-left corner of cell (0, 0)), opacity,
 *                   u32 source_index, reserved
 *   anims[n]        3 u32: frame_first, frame_count, total_ms
 *   frames[n]       2 u32: tile (global tile index), duration_ms
 *   shapes[n]       3 u32: kind (DE__TM_SHAPE_*), point_first, point_count
 *   points[n]       2 f32: x, y in tile-local map units (origin: the tile's bottom-left, v up)
 *   spawns[n]       10 x 4 bytes: u32 prefab asset ID lo, hi (0 = not mapped), name (string),
 *                   group (string: the layer or level it came from), f32 u, v (map position),
 *                   w, h (map units), u32 field_first, field_count
 *   fields[n]       6 x 4 bytes: u32 path (string, a reflection path `Type.field.sub`), kind
 *                   (DE__TM_FIELD_*), string (string offset, kind STRING), reserved, f64 number
 *   cells           chunk_count x 32 x 32 u32, chunk by chunk (each layer's chunks row-major from
 *                   its chunk_first), each chunk row-major (y down). Cell value: 0 = empty, else
 *                   (global tile index + 1) | DE_TILEMAP_FLIP_* bits, plus DE__TM_CELL_SOLID for
 *                   an IntGrid cell that collides. Cells outside the layer are 0.
 *   strings         string_bytes bytes of NUL-terminated UTF-8; offset 0 is always "".
 *
 * Map space: u right, v up, in map units (meters: `pixels_per_unit` source pixels make one unit).
 * Cell (x, y) of a layer covers u in [origin_u + x * cell_w, origin_u + (x + 1) * cell_w] and v in
 * [origin_v - (y + 1) * cell_h, origin_v - y * cell_h]. A tile's sprite is anchored at the
 * bottom-left corner of its cell (Tiled's rule for tiles larger than the grid).
 */
#ifndef DE_GAMEPLAY_TILEMAP_COOKED_H
#define DE_GAMEPLAY_TILEMAP_COOKED_H

#include <dengine/de_base.h>

#include <string.h>

#define DE__TM_MAGIC   0x504D5444u /* "DTMP" */
#define DE__TM_VERSION 1u

#define DE__TM_CHUNK       32u
#define DE__TM_CHUNK_CELLS (DE__TM_CHUNK * DE__TM_CHUNK)

/* Cell value bits (the DE_TILEMAP_FLIP_* bits and DE_TILEMAP_TILE_MASK are public in
 * de_gameplay.h; these are the same values). */
#define DE__TM_CELL_TILE_MASK 0x00FFFFFFu
#define DE__TM_CELL_SOLID     (1u << 28) /* IntGrid value that collides (cooked only) */
#define DE__TM_CELL_FLIP_D    (1u << 29)
#define DE__TM_CELL_FLIP_V    (1u << 30)
#define DE__TM_CELL_FLIP_H    (1u << 31)
#define DE__TM_CELL_FLIPS     (DE__TM_CELL_FLIP_D | DE__TM_CELL_FLIP_V | DE__TM_CELL_FLIP_H)

/* Tile flags. */
#define DE__TM_TILE_SOLID 1u /* a collision rectangle covers the whole tile */

/* Layer flags. */
#define DE__TM_LAYER_COLLIDE  1u /* every non-empty tile collides */
#define DE__TM_LAYER_INTGRID  2u /* cells may carry DE__TM_CELL_SOLID */
#define DE__TM_LAYER_ANIMATED 4u /* uses at least one animated tile (as cooked) */

/* Shape kinds. */
#define DE__TM_SHAPE_POLYGON    1u /* convex polygon, 3..8 points */
#define DE__TM_SHAPE_CHAIN      2u /* open chain, 2..n points */
#define DE__TM_SHAPE_CHAIN_LOOP 3u /* closed chain, 3..n points */

/* Spawn field kinds. */
#define DE__TM_FIELD_BOOL   0u
#define DE__TM_FIELD_INT    1u
#define DE__TM_FIELD_FLOAT  2u
#define DE__TM_FIELD_STRING 3u

/* Record sizes in bytes. */
#define DE__TM_HEADER_SIZE  64u
#define DE__TM_TILESET_SIZE 32u
#define DE__TM_TILE_SIZE    44u
#define DE__TM_LAYER_SIZE   56u
#define DE__TM_ANIM_SIZE    12u
#define DE__TM_FRAME_SIZE   8u
#define DE__TM_SHAPE_SIZE   12u
#define DE__TM_POINT_SIZE   8u
#define DE__TM_SPAWN_SIZE   40u
#define DE__TM_FIELD_SIZE   24u

/* Limits the parser enforces (and the importer refuses to exceed). */
#define DE__TM_MAX_TILES      (1u << 20)
#define DE__TM_MAX_LAYERS     4096u
#define DE__TM_MAX_DIM        65536u
#define DE__TM_MAX_CHUNKS     (1u << 18)
#define DE__TM_MAX_RECORDS    (1u << 22)
#define DE__TM_MAX_STRING_BYTES (1u << 26)

typedef struct de__tm_header {
    u32 magic, version, flags;
    f32 pixels_per_unit;
    u32 tileset_count, tile_count, layer_count, chunk_count;
    u32 anim_count, frame_count, shape_count, point_count;
    u32 spawn_count, field_count, string_bytes, reserved;
} de__tm_header;

typedef struct de__tm_tileset {
    u64 texture; /* asset ID hash, 0 = none */
    u32 first_tile, tile_count, columns, image_w, image_h, name;
} de__tm_tileset;

typedef struct de__tm_tile {
    f32 u, v, du, dv;
    f32 w, h;
    u32 tileset, anim, shape_first, shape_count, flags;
} de__tm_tile;

typedef struct de__tm_layer {
    u32 name, flags, width, height, chunks_x, chunks_y, chunk_first;
    f32 cell_w, cell_h, origin_u, origin_v, opacity;
    u32 source_index, reserved;
} de__tm_layer;

typedef struct de__tm_anim {
    u32 frame_first, frame_count, total_ms;
} de__tm_anim;

typedef struct de__tm_frame {
    u32 tile, duration_ms;
} de__tm_frame;

typedef struct de__tm_shape {
    u32 kind, point_first, point_count;
} de__tm_shape;

typedef struct de__tm_point {
    f32 x, y;
} de__tm_point;

typedef struct de__tm_spawn {
    u64 prefab;
    u32 name, group;
    f32 u, v, w, h;
    u32 field_first, field_count;
} de__tm_spawn;

typedef struct de__tm_field {
    u32 path, kind, string, reserved;
    f64 number;
} de__tm_field;

/* A checked view of cooked bytes. Pointers point into the bytes. */
typedef struct de__tm_view {
    de__tm_header h;
    const u8 *tilesets, *tiles, *layers, *anims, *frames, *shapes, *points, *spawns, *fields;
    const u8 *cells;
    const char *strings;
} de__tm_view;

/* ---- Little-endian access -------------------------------------------------------------------- */

static inline u32 de__tm_rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static inline f32 de__tm_rdf(const u8 *p) {
    u32 u = de__tm_rd32(p);
    f32 f;
    memcpy(&f, &u, sizeof f);
    return f;
}
static inline void de__tm_wr32(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
static inline void de__tm_wrf(u8 *p, f32 f) {
    u32 u;
    memcpy(&u, &f, sizeof u);
    de__tm_wr32(p, u);
}

/* ---- Record readers (indices must be checked by the caller against the header counts) -------- */

static inline de__tm_tileset de__tm_tileset_at(const de__tm_view *v, u32 i) {
    const u8 *p = v->tilesets + (size_t)i * DE__TM_TILESET_SIZE;
    de__tm_tileset t;
    t.texture = (u64)de__tm_rd32(p) | ((u64)de__tm_rd32(p + 4) << 32);
    t.first_tile = de__tm_rd32(p + 8);
    t.tile_count = de__tm_rd32(p + 12);
    t.columns = de__tm_rd32(p + 16);
    t.image_w = de__tm_rd32(p + 20);
    t.image_h = de__tm_rd32(p + 24);
    t.name = de__tm_rd32(p + 28);
    return t;
}
static inline de__tm_tile de__tm_tile_at(const de__tm_view *v, u32 i) {
    const u8 *p = v->tiles + (size_t)i * DE__TM_TILE_SIZE;
    de__tm_tile t;
    t.u = de__tm_rdf(p);
    t.v = de__tm_rdf(p + 4);
    t.du = de__tm_rdf(p + 8);
    t.dv = de__tm_rdf(p + 12);
    t.w = de__tm_rdf(p + 16);
    t.h = de__tm_rdf(p + 20);
    t.tileset = de__tm_rd32(p + 24);
    t.anim = de__tm_rd32(p + 28);
    t.shape_first = de__tm_rd32(p + 32);
    t.shape_count = de__tm_rd32(p + 36);
    t.flags = de__tm_rd32(p + 40);
    return t;
}
static inline de__tm_layer de__tm_layer_at(const de__tm_view *v, u32 i) {
    const u8 *p = v->layers + (size_t)i * DE__TM_LAYER_SIZE;
    de__tm_layer l;
    l.name = de__tm_rd32(p);
    l.flags = de__tm_rd32(p + 4);
    l.width = de__tm_rd32(p + 8);
    l.height = de__tm_rd32(p + 12);
    l.chunks_x = de__tm_rd32(p + 16);
    l.chunks_y = de__tm_rd32(p + 20);
    l.chunk_first = de__tm_rd32(p + 24);
    l.cell_w = de__tm_rdf(p + 28);
    l.cell_h = de__tm_rdf(p + 32);
    l.origin_u = de__tm_rdf(p + 36);
    l.origin_v = de__tm_rdf(p + 40);
    l.opacity = de__tm_rdf(p + 44);
    l.source_index = de__tm_rd32(p + 48);
    l.reserved = de__tm_rd32(p + 52);
    return l;
}
static inline de__tm_anim de__tm_anim_at(const de__tm_view *v, u32 i) {
    const u8 *p = v->anims + (size_t)i * DE__TM_ANIM_SIZE;
    de__tm_anim a = {de__tm_rd32(p), de__tm_rd32(p + 4), de__tm_rd32(p + 8)};
    return a;
}
static inline de__tm_frame de__tm_frame_at(const de__tm_view *v, u32 i) {
    const u8 *p = v->frames + (size_t)i * DE__TM_FRAME_SIZE;
    de__tm_frame f = {de__tm_rd32(p), de__tm_rd32(p + 4)};
    return f;
}
static inline de__tm_shape de__tm_shape_at(const de__tm_view *v, u32 i) {
    const u8 *p = v->shapes + (size_t)i * DE__TM_SHAPE_SIZE;
    de__tm_shape s = {de__tm_rd32(p), de__tm_rd32(p + 4), de__tm_rd32(p + 8)};
    return s;
}
static inline de__tm_point de__tm_point_at(const de__tm_view *v, u32 i) {
    const u8 *p = v->points + (size_t)i * DE__TM_POINT_SIZE;
    de__tm_point pt = {de__tm_rdf(p), de__tm_rdf(p + 4)};
    return pt;
}
static inline de__tm_spawn de__tm_spawn_at(const de__tm_view *v, u32 i) {
    const u8 *p = v->spawns + (size_t)i * DE__TM_SPAWN_SIZE;
    de__tm_spawn s;
    s.prefab = (u64)de__tm_rd32(p) | ((u64)de__tm_rd32(p + 4) << 32);
    s.name = de__tm_rd32(p + 8);
    s.group = de__tm_rd32(p + 12);
    s.u = de__tm_rdf(p + 16);
    s.v = de__tm_rdf(p + 20);
    s.w = de__tm_rdf(p + 24);
    s.h = de__tm_rdf(p + 28);
    s.field_first = de__tm_rd32(p + 32);
    s.field_count = de__tm_rd32(p + 36);
    return s;
}
static inline de__tm_field de__tm_field_at(const de__tm_view *v, u32 i) {
    const u8 *p = v->fields + (size_t)i * DE__TM_FIELD_SIZE;
    de__tm_field f;
    f.path = de__tm_rd32(p);
    f.kind = de__tm_rd32(p + 4);
    f.string = de__tm_rd32(p + 8);
    f.reserved = de__tm_rd32(p + 12);
    u64 bits = (u64)de__tm_rd32(p + 16) | ((u64)de__tm_rd32(p + 20) << 32);
    memcpy(&f.number, &bits, sizeof f.number);
    return f;
}
/* The cells of chunk `chunk` (checked by the caller). */
static inline const u8 *de__tm_chunk_cells(const de__tm_view *v, u32 chunk) {
    return v->cells + (size_t)chunk * DE__TM_CHUNK_CELLS * 4u;
}
/* String at offset `off`: "" when out of range (the parser checked the table ends in NUL). */
static inline const char *de__tm_str(const de__tm_view *v, u32 off) {
    return off < v->h.string_bytes ? v->strings + off : "";
}

/* ---- Parser ------------------------------------------------------------------------------------ */

static inline bool de__tm_fail(const char **why, const char *msg) {
    if (why != NULL)
        *why = msg;
    return false;
}

static inline bool de__tm_finite(f32 f) {
    return f == f && f - f == 0.0f;
}

/**
 * Checks `data` and fills `*v`. Every index a record holds is checked against its table, every
 * layer's chunk range against the cells, strings end inside the table, sizes and positions are
 * finite. On failure returns false with a static reason in `*why` (nullable).
 */
static inline bool de__tm_view_parse(const void *data, u64 size, de__tm_view *v, const char **why) {
    memset(v, 0, sizeof *v);
    const u8 *b = (const u8 *)data;
    if (b == NULL || size < DE__TM_HEADER_SIZE)
        return de__tm_fail(why, "tilemap: truncated header");
    de__tm_header *h = &v->h;
    u32 w[16];
    for (u32 i = 0; i < 16; ++i)
        w[i] = de__tm_rd32(b + 4u * i);
    h->magic = w[0];
    h->version = w[1];
    h->flags = w[2];
    h->pixels_per_unit = de__tm_rdf(b + 12);
    h->tileset_count = w[4];
    h->tile_count = w[5];
    h->layer_count = w[6];
    h->chunk_count = w[7];
    h->anim_count = w[8];
    h->frame_count = w[9];
    h->shape_count = w[10];
    h->point_count = w[11];
    h->spawn_count = w[12];
    h->field_count = w[13];
    h->string_bytes = w[14];
    h->reserved = w[15];
    if (h->magic != DE__TM_MAGIC)
        return de__tm_fail(why, "tilemap: not a cooked tilemap (magic)");
    if (h->version != DE__TM_VERSION)
        return de__tm_fail(why, "tilemap: unsupported cooked version");
    if (!de__tm_finite(h->pixels_per_unit) || h->pixels_per_unit <= 0.0f)
        return de__tm_fail(why, "tilemap: bad pixels_per_unit");
    if (h->tile_count > DE__TM_MAX_TILES || h->layer_count > DE__TM_MAX_LAYERS ||
        h->chunk_count > DE__TM_MAX_CHUNKS || h->tileset_count > DE__TM_MAX_TILES ||
        h->anim_count > DE__TM_MAX_RECORDS || h->frame_count > DE__TM_MAX_RECORDS ||
        h->shape_count > DE__TM_MAX_RECORDS || h->point_count > DE__TM_MAX_RECORDS ||
        h->spawn_count > DE__TM_MAX_RECORDS || h->field_count > DE__TM_MAX_RECORDS ||
        h->string_bytes > DE__TM_MAX_STRING_BYTES || h->string_bytes == 0)
        return de__tm_fail(why, "tilemap: counts out of range");
    u64 off = DE__TM_HEADER_SIZE;
    const u64 sizes[10] = {
        (u64)h->tileset_count * DE__TM_TILESET_SIZE, (u64)h->tile_count * DE__TM_TILE_SIZE,
        (u64)h->layer_count * DE__TM_LAYER_SIZE,     (u64)h->anim_count * DE__TM_ANIM_SIZE,
        (u64)h->frame_count * DE__TM_FRAME_SIZE,     (u64)h->shape_count * DE__TM_SHAPE_SIZE,
        (u64)h->point_count * DE__TM_POINT_SIZE,     (u64)h->spawn_count * DE__TM_SPAWN_SIZE,
        (u64)h->field_count * DE__TM_FIELD_SIZE,     (u64)h->chunk_count * DE__TM_CHUNK_CELLS * 4u};
    const u8 **ptrs[10] = {&v->tilesets, &v->tiles,  &v->layers, &v->anims,  &v->frames,
                           &v->shapes,   &v->points, &v->spawns, &v->fields, &v->cells};
    for (u32 i = 0; i < 10; ++i) {
        if (sizes[i] > size - off)
            return de__tm_fail(why, "tilemap: truncated section");
        *ptrs[i] = b + off;
        off += sizes[i];
    }
    if ((u64)h->string_bytes != size - off)
        return de__tm_fail(why, "tilemap: string table size mismatch");
    v->strings = (const char *)(b + off);
    if (v->strings[0] != '\0' || v->strings[h->string_bytes - 1] != '\0')
        return de__tm_fail(why, "tilemap: string table not NUL-terminated");

    for (u32 i = 0; i < h->tileset_count; ++i) {
        de__tm_tileset t = de__tm_tileset_at(v, i);
        if (t.first_tile > h->tile_count || t.tile_count > h->tile_count - t.first_tile ||
            t.name >= h->string_bytes)
            return de__tm_fail(why, "tilemap: bad tileset record");
    }
    for (u32 i = 0; i < h->tile_count; ++i) {
        de__tm_tile t = de__tm_tile_at(v, i);
        if (t.tileset >= h->tileset_count || t.anim > h->anim_count ||
            t.shape_first > h->shape_count || t.shape_count > h->shape_count - t.shape_first ||
            !de__tm_finite(t.u) || !de__tm_finite(t.v) || !de__tm_finite(t.du) ||
            !de__tm_finite(t.dv) || !de__tm_finite(t.w) || !de__tm_finite(t.h) || t.w < 0.0f ||
            t.h < 0.0f)
            return de__tm_fail(why, "tilemap: bad tile record");
    }
    u32 expected_chunk = 0;
    for (u32 i = 0; i < h->layer_count; ++i) {
        de__tm_layer l = de__tm_layer_at(v, i);
        if (l.name >= h->string_bytes || l.width > DE__TM_MAX_DIM || l.height > DE__TM_MAX_DIM ||
            l.chunks_x != (l.width + DE__TM_CHUNK - 1) / DE__TM_CHUNK ||
            l.chunks_y != (l.height + DE__TM_CHUNK - 1) / DE__TM_CHUNK ||
            l.chunk_first != expected_chunk ||
            (u64)l.chunks_x * l.chunks_y > (u64)h->chunk_count - l.chunk_first ||
            !de__tm_finite(l.cell_w) || !de__tm_finite(l.cell_h) || l.cell_w <= 0.0f ||
            l.cell_h <= 0.0f || !de__tm_finite(l.origin_u) || !de__tm_finite(l.origin_v) ||
            !de__tm_finite(l.opacity))
            return de__tm_fail(why, "tilemap: bad layer record");
        expected_chunk += l.chunks_x * l.chunks_y;
    }
    if (expected_chunk != h->chunk_count)
        return de__tm_fail(why, "tilemap: chunk count mismatch");
    for (u32 i = 0; i < h->anim_count; ++i) {
        de__tm_anim a = de__tm_anim_at(v, i);
        if (a.frame_count == 0 || a.frame_first > h->frame_count ||
            a.frame_count > h->frame_count - a.frame_first || a.total_ms == 0)
            return de__tm_fail(why, "tilemap: bad animation record");
        u64 total = 0;
        for (u32 k = 0; k < a.frame_count; ++k) {
            de__tm_frame f = de__tm_frame_at(v, a.frame_first + k);
            if (f.tile >= h->tile_count)
                return de__tm_fail(why, "tilemap: bad animation frame");
            total += f.duration_ms;
        }
        if (total != a.total_ms)
            return de__tm_fail(why, "tilemap: animation duration mismatch");
    }
    for (u32 i = 0; i < h->shape_count; ++i) {
        de__tm_shape s = de__tm_shape_at(v, i);
        u32 min = s.kind == DE__TM_SHAPE_CHAIN ? 2u : 3u;
        if (s.kind < DE__TM_SHAPE_POLYGON || s.kind > DE__TM_SHAPE_CHAIN_LOOP ||
            s.point_count < min || s.point_first > h->point_count ||
            s.point_count > h->point_count - s.point_first ||
            (s.kind == DE__TM_SHAPE_POLYGON && s.point_count > 8u))
            return de__tm_fail(why, "tilemap: bad shape record");
    }
    for (u32 i = 0; i < h->point_count; ++i) {
        de__tm_point p = de__tm_point_at(v, i);
        if (!de__tm_finite(p.x) || !de__tm_finite(p.y))
            return de__tm_fail(why, "tilemap: bad point");
    }
    for (u32 i = 0; i < h->spawn_count; ++i) {
        de__tm_spawn s = de__tm_spawn_at(v, i);
        if (s.name >= h->string_bytes || s.group >= h->string_bytes ||
            s.field_first > h->field_count || s.field_count > h->field_count - s.field_first ||
            !de__tm_finite(s.u) || !de__tm_finite(s.v) || !de__tm_finite(s.w) ||
            !de__tm_finite(s.h))
            return de__tm_fail(why, "tilemap: bad spawn record");
    }
    for (u32 i = 0; i < h->field_count; ++i) {
        de__tm_field f = de__tm_field_at(v, i);
        if (f.path >= h->string_bytes || f.kind > DE__TM_FIELD_STRING ||
            f.string >= h->string_bytes)
            return de__tm_fail(why, "tilemap: bad field record");
    }
    const u64 cells = (u64)h->chunk_count * DE__TM_CHUNK_CELLS;
    for (u64 i = 0; i < cells; ++i) {
        u32 c = de__tm_rd32(v->cells + i * 4u);
        u32 tile = c & DE__TM_CELL_TILE_MASK;
        if (tile > h->tile_count || (c & 0x0F000000u) != 0u)
            return de__tm_fail(why, "tilemap: bad cell");
    }
    return true;
}

#endif /* DE_GAMEPLAY_TILEMAP_COOKED_H */
