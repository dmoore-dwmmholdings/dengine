/*
 * tilemap_ir.h: internals of the tilemap importer (DENG-80): the import arena, growable arrays,
 * the strict JSON reader (tilemap_json.c), the TMX-subset XML reader (tilemap_xml.c), the map IR
 * the front ends fill (import_tiled.c, import_ldtk.c) and the DTMP writer (tilemap_ir.c).
 * Not used outside tools/cook/importers/tilemap/ and its tests.
 */
#ifndef DE_COOK_IMPORTERS_TILEMAP_IR_H
#define DE_COOK_IMPORTERS_TILEMAP_IR_H

#include "cook/importers/tilemap/tilemap_import.h"
#include "gameplay/tilemap/tilemap_cooked.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <stdarg.h>

/* ---- Arena ----------------------------------------------------------------------------------- */

typedef struct tm_block tm_block;
/** Bump allocator over `a`; everything is freed at once. `limit` caps the bytes taken from `a`. */
typedef struct tm_arena {
    const de_allocator *a;
    tm_block *head;
    u64 taken, limit;
    bool failed; /* an allocation failed (out of memory or over the limit) */
} tm_arena;

void tm_arena_init(tm_arena *ar, const de_allocator *a, u64 limit);
void tm_arena_free(tm_arena *ar);
/** Zeroed memory, or NULL (and ar->failed). */
void *tm_alloc(tm_arena *ar, size_t size, size_t align);
/** A NUL-terminated copy of `s[0..n)`. */
char *tm_strndup(tm_arena *ar, const char *s, size_t n);

/** A growable array in the arena (old storage is left to the arena when it grows). */
typedef struct tm_vec {
    void *data;
    u32 count, cap, elem;
} tm_vec;
void tm_vec_init(tm_vec *v, u32 elem);
/** Appends one element (copied from `item`, or zeroed when NULL); returns it, or NULL. */
void *tm_vec_push(tm_arena *ar, tm_vec *v, const void *item);
#define TM_AT(v, T, i) (&((T *)(v)->data)[i])

/* ---- JSON (tilemap_json.c) --------------------------------------------------------------------- */

typedef enum tmj_kind { TMJ_NULL, TMJ_BOOL, TMJ_NUM, TMJ_STR, TMJ_ARR, TMJ_OBJ } tmj_kind;

typedef struct tmj_member tmj_member;
typedef struct tmj_val {
    tmj_kind kind;
    u32 count; /* string length, array items or object members */
    union {
        bool b;
        f64 n;
        const char *s; /* NUL-terminated, UTF-8 */
        struct tmj_val *items;
        tmj_member *members;
    } u;
} tmj_val;
struct tmj_member {
    const char *key;
    u32 key_len;
    tmj_val val;
};

#define TMJ_MAX_DEPTH 64u

/**
 * Parses strict JSON (RFC 8259: no comments, no trailing commas, no leading zeros, escapes and
 * \u surrogate pairs decoded to UTF-8, unescaped control characters refused, nesting at most
 * TMJ_MAX_DEPTH). Everything is allocated from `ar`. Returns NULL with a message in `err`
 * ("line:column: ...").
 */
const tmj_val *tmj_parse(tm_arena *ar, const char *text, size_t len, char *err, size_t err_cap);
/** The member `key` of an object (the last one when repeated), or NULL. */
const tmj_val *tmj_get(const tmj_val *obj, const char *key);
/** Typed getters: `fallback` when `obj` has no member `key` of that kind. */
f64 tmj_num(const tmj_val *obj, const char *key, f64 fallback);
const char *tmj_str(const tmj_val *obj, const char *key, const char *fallback);
bool tmj_bool(const tmj_val *obj, const char *key, bool fallback);
/** The array member `key` (NULL when missing or not an array). */
const tmj_val *tmj_arr(const tmj_val *obj, const char *key);

/* ---- XML (tilemap_xml.c) ----------------------------------------------------------------------- */

typedef struct tmx_attr {
    const char *name, *value;
} tmx_attr;
typedef struct tmx_node tmx_node;
struct tmx_node {
    const char *name;
    tmx_attr *attrs;
    u32 attr_count;
    tmx_node *children;
    u32 child_count;
    const char *text; /* character data of this element (entities decoded), "" when none */
    u32 text_len;
};

#define TMX_MAX_DEPTH 64u

/**
 * Parses the XML subset Tiled writes: an optional `<?xml ...?>` declaration, comments,
 * `<!DOCTYPE>` (skipped), elements with single- or double-quoted attributes, character data and
 * CDATA, the five predefined entities and numeric character references. Nesting at most
 * TMX_MAX_DEPTH. Returns the root element, or NULL with a message in `err`.
 */
const tmx_node *tmx_parse(tm_arena *ar, const char *text, size_t len, char *err, size_t err_cap);
const char *tmx_attr_str(const tmx_node *n, const char *name, const char *fallback);
f64 tmx_attr_num(const tmx_node *n, const char *name, f64 fallback);
/** The first child element called `name` after child index `*from` (nullable; updated). */
const tmx_node *tmx_child(const tmx_node *n, const char *name);

/* ---- Map IR ------------------------------------------------------------------------------------ */

typedef struct tm_ir_layer {
    u32 name, flags, width, height, source_index;
    f32 cell_w, cell_h, origin_u, origin_v, opacity;
    u32 *cells; /* width * height, row-major, y down */
} tm_ir_layer;

typedef struct tm_ir {
    tm_arena *ar;
    const de__tm_import_desc *desc;
    de__tm_import_result *out;
    f32 ppu;
    tm_vec tilesets; /* de__tm_tileset */
    tm_vec tiles;    /* de__tm_tile */
    tm_vec layers;   /* tm_ir_layer */
    tm_vec anims;    /* de__tm_anim */
    tm_vec frames;   /* de__tm_frame */
    tm_vec shapes;   /* de__tm_shape */
    tm_vec points;   /* de__tm_point */
    tm_vec spawns;   /* de__tm_spawn */
    tm_vec fields;   /* de__tm_field */
    tm_vec strings;  /* char; starts with "" */
    de_result error;
} tm_ir;

/** Records the first failure (code and message) and returns `code`. */
de_result tm_fail(tm_ir *ir, de_result code, const char *fmt, ...) DE_PRINTF_FMT(3, 4);
/** Counts a warning and hands it to the desc's callback. */
void tm_warn(tm_ir *ir, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
/** Interns a string into the string table (equal strings share one offset); 0 for "". */
u32 tm_str(tm_ir *ir, const char *s);
/** Records an asset-ID dependency (deduplicated). */
void tm_dep(tm_ir *ir, u64 id);
/** True when `name` is in the solid_layers setting. */
bool tm_solid_layer(const tm_ir *ir, const char *name);
/** The asset ID of a `key=value` map setting entry for `key`, or 0. */
u64 tm_setting_map(const char *const *entries, u32 count, const char *key);
/**
 * The texture asset ID of a tileset: its `tilesets` setting entry, else the image path `image`
 * (relative to `base_rel`, the directory of the file that named it, below the asset root) without
 * its extension. 0 (and a failure) when the path leaves the asset root.
 */
u64 tm_texture_id(tm_ir *ir, const char *tileset_name, const char *base_rel, const char *image);
/** Directory part of `path` (`maps/a.tmj` -> `maps/`), into the arena. */
const char *tm_dirname(tm_ir *ir, const char *path);
/** `dir` + `rel`, normalised (`.` and `..` removed); NULL when it leaves the root. */
const char *tm_join(tm_ir *ir, const char *dir, const char *rel);

/** Adds a layer with zeroed cells; returns it or NULL (failure recorded). */
tm_ir_layer *tm_layer_add(tm_ir *ir, const char *name, u32 width, u32 height, f32 cell_w,
                          f32 cell_h, f32 origin_u, f32 origin_v);
/** Adds a tileset with `count` tiles of `tw` x `th` pixels from an image of `iw` x `ih`
 * (`columns` per row, `margin`/`spacing` in pixels); returns its first global tile index. */
u32 tm_tileset_add(tm_ir *ir, const char *name, u64 texture, u32 count, u32 columns, u32 tw,
                   u32 th, u32 iw, u32 ih, u32 margin, u32 spacing);
/** Adds a shape to tile `tile` (points in tile pixels, y down from the tile's top-left; `th`
 * the tile height in pixels). Splits chains longer than 8 points; polygons with more than 8
 * points become closed chains. */
void tm_tile_shape(tm_ir *ir, u32 tile, u32 kind, const f64 *xy, u32 count, f64 th);

/* Base64 and the layer codecs (tilemap_ir.c). `name` names the layer in messages. */
de_result tm_base64(tm_ir *ir, const char *text, size_t len, u8 **out, size_t *out_len,
                    const char *name);
de_result tm_decompress(tm_ir *ir, const char *compression, const u8 *in, size_t in_len,
                        u8 *out, size_t out_len, const char *name);

/** Writes the IR as DTMP into ir->out (bytes from desc->allocator). */
de_result tm_ir_write(tm_ir *ir);

/* Front ends. */
de_result tm_import_tmj(tm_ir *ir, const char *text, size_t len);
de_result tm_import_tmx(tm_ir *ir, const char *text, size_t len);
de_result tm_import_ldtk(tm_ir *ir, const char *text, size_t len);

/** Reads an external file relative to the source (desc->read); fails the IR on error. */
de_result tm_read(tm_ir *ir, const char *rel, const char **text, size_t *len);

#endif /* DE_COOK_IMPORTERS_TILEMAP_IR_H */
