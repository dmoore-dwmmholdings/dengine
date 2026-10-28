/*
 * tilemap_ir.c: the import arena, the map IR helpers, the layer codecs and the DTMP writer of the
 * tilemap importer (DENG-80). See tilemap_ir.h.
 */
#include "cook/importers/tilemap/tilemap_ir.h"

#include <dengine/de_asset.h>

#include <stb_image.h> /* stbi_zlib_decode_buffer, stbi_zlib_decode_noheader_buffer */
#include <zstd.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* ---- Arena ----------------------------------------------------------------------------------- */

struct tm_block {
    tm_block *next;
    size_t size, used;
};

#define TM_BLOCK_MIN ((size_t)64 * 1024)

void tm_arena_init(tm_arena *ar, const de_allocator *a, u64 limit) {
    memset(ar, 0, sizeof *ar);
    ar->a = a != NULL ? a : de_heap_allocator();
    ar->limit = limit != 0 ? limit : (u64)512 * 1024 * 1024;
}

void tm_arena_free(tm_arena *ar) {
    tm_block *b = ar->head;
    while (b != NULL) {
        tm_block *next = b->next;
        DE_FREE(ar->a, b, b->size);
        b = next;
    }
    ar->head = NULL;
    ar->taken = 0;
}

void *tm_alloc(tm_arena *ar, size_t size, size_t align) {
    if (align < 8)
        align = 8;
    tm_block *b = ar->head;
    if (b != NULL) {
        size_t at = (b->used + align - 1) & ~(align - 1);
        if (at <= b->size && size <= b->size - at) {
            b->used = at + size;
            u8 *p = (u8 *)b + at;
            memset(p, 0, size);
            return p;
        }
    }
    const size_t header = (sizeof(tm_block) + 15u) & ~(size_t)15u;
    if (size > SIZE_MAX / 2 - header - align) {
        ar->failed = true;
        return NULL;
    }
    size_t need = header + size + align;
    size_t bytes = need > TM_BLOCK_MIN ? need : TM_BLOCK_MIN;
    if ((u64)bytes > ar->limit - ar->taken) {
        ar->failed = true;
        return NULL;
    }
    tm_block *nb = (tm_block *)DE_ALLOC(ar->a, bytes, 16, DE_MEM_ASSET);
    if (nb == NULL) {
        ar->failed = true;
        return NULL;
    }
    ar->taken += bytes;
    nb->size = bytes;
    nb->used = header;
    /* A big allocation gets its own block behind the current one, so the current block keeps
     * serving small ones. */
    if (b != NULL && need > TM_BLOCK_MIN) {
        nb->next = b->next;
        b->next = nb;
    } else {
        nb->next = b;
        ar->head = nb;
    }
    size_t at = (nb->used + align - 1) & ~(align - 1);
    nb->used = at + size;
    u8 *p = (u8 *)nb + at;
    memset(p, 0, size);
    return p;
}

char *tm_strndup(tm_arena *ar, const char *s, size_t n) {
    char *p = (char *)tm_alloc(ar, n + 1, 1);
    if (p != NULL && n > 0)
        memcpy(p, s, n);
    return p;
}

void tm_vec_init(tm_vec *v, u32 elem) {
    memset(v, 0, sizeof *v);
    v->elem = elem;
}

void *tm_vec_push(tm_arena *ar, tm_vec *v, const void *item) {
    if (v->count == v->cap) {
        if (v->cap >= (1u << 26))
            return NULL;
        u32 cap = v->cap != 0 ? v->cap * 2u : 16u;
        void *d = tm_alloc(ar, (size_t)cap * v->elem, 8);
        if (d == NULL)
            return NULL;
        if (v->count != 0)
            memcpy(d, v->data, (size_t)v->count * v->elem);
        v->data = d;
        v->cap = cap;
    }
    u8 *slot = (u8 *)v->data + (size_t)v->count * v->elem;
    if (item != NULL)
        memcpy(slot, item, v->elem);
    else
        memset(slot, 0, v->elem);
    v->count++;
    return slot;
}

/* ---- Errors, strings, settings --------------------------------------------------------------- */

de_result tm_fail(tm_ir *ir, de_result code, const char *fmt, ...) {
    if (code == DE_OK)
        code = DE_ERR_INTERNAL;
    if (ir->error == DE_OK) {
        ir->error = code;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(ir->out->message, sizeof ir->out->message, fmt, ap);
        va_end(ap);
    }
    return ir->error;
}

void tm_warn(tm_ir *ir, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    ir->out->warnings++;
    if (ir->desc->warn != NULL)
        ir->desc->warn(ir->desc->warn_user, msg);
}

u32 tm_str(tm_ir *ir, const char *s) {
    if (s == NULL || s[0] == '\0')
        return 0;
    size_t n = strlen(s);
    const char *tab = (const char *)ir->strings.data;
    /* Linear search: maps have few distinct strings (layer, tileset and object names). */
    for (u32 i = 1; i + n < ir->strings.count + 0u; ++i) {
        if ((i == 1 || tab[i - 1] == '\0') && memcmp(tab + i, s, n + 1) == 0)
            return i;
    }
    if (ir->strings.count + n + 1 > DE__TM_MAX_STRING_BYTES) {
        tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: string table too large");
        return 0;
    }
    u32 at = ir->strings.count;
    for (size_t k = 0; k <= n; ++k)
        if (tm_vec_push(ir->ar, &ir->strings, s + k) == NULL) {
            tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (strings)");
            return 0;
        }
    return at;
}

void tm_dep(tm_ir *ir, u64 id) {
    if (id == 0)
        return;
    de__tm_import_result *o = ir->out;
    for (u32 i = 0; i < o->dependency_count; ++i)
        if (o->dependencies[i] == id)
            return;
    if (o->dependency_count < sizeof o->dependencies / sizeof o->dependencies[0])
        o->dependencies[o->dependency_count++] = id;
}

bool tm_solid_layer(const tm_ir *ir, const char *name) {
    for (u32 i = 0; i < ir->desc->solid_layer_count; ++i)
        if (ir->desc->solid_layers[i] != NULL && strcmp(ir->desc->solid_layers[i], name) == 0)
            return true;
    return false;
}

u64 tm_setting_map(const char *const *entries, u32 count, const char *key) {
    size_t n = strlen(key);
    for (u32 i = 0; i < count; ++i) {
        const char *e = entries[i];
        if (e != NULL && strncmp(e, key, n) == 0 && e[n] == '=' && e[n + 1] != '\0')
            return de_asset_id_from(e + n + 1).hash;
    }
    return 0;
}

const char *tm_dirname(tm_ir *ir, const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL)
        return "";
    return tm_strndup(ir->ar, path, (size_t)(slash - path) + 1);
}

/* Normalises `dir` + `rel`: removes `.` segments and folds `..` into the segment before it. With
 * `keep_up` leading `..` segments stay (a path relative to a directory); without, a path that
 * climbs above the root gives NULL. Absolute paths (`/x`, `C:`, `x:/`) give NULL. */
static const char *join(tm_ir *ir, const char *dir, const char *rel, bool keep_up) {
    if (rel[0] == '/' || rel[0] == '\\' || strchr(rel, ':') != NULL)
        return NULL;
    size_t cap = strlen(dir) + strlen(rel) + 2;
    char *buf = (char *)tm_alloc(ir->ar, cap, 1);
    char *outp = (char *)tm_alloc(ir->ar, cap, 1);
    if (buf == NULL || outp == NULL)
        return NULL;
    snprintf(buf, cap, "%s%s", dir, rel);
    for (char *c = buf; *c; ++c)
        if (*c == '\\')
            *c = '/';
    /* Segment stack in `outp`. */
    size_t len = 0;
    u32 ups = 0; /* leading `..` segments kept in outp */
    const char *p = buf;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t n = e != NULL ? (size_t)(e - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.')) {
            /* skip */
        } else if (n == 2 && p[0] == '.' && p[1] == '.') {
            if (len > (size_t)ups * 3u) {
                /* drop the last real segment */
                size_t k = len;
                while (k > 0 && outp[k - 1] == '/')
                    --k;
                while (k > 0 && outp[k - 1] != '/')
                    --k;
                len = k;
            } else if (keep_up) {
                memcpy(outp + len, "../", 3);
                len += 3;
                ++ups;
            } else {
                return NULL;
            }
        } else {
            memcpy(outp + len, p, n);
            len += n;
            if (e != NULL) {
                outp[len++] = '/';
            }
        }
        if (e == NULL)
            break;
        p = e + 1;
    }
    outp[len] = '\0';
    return outp;
}

const char *tm_join(tm_ir *ir, const char *dir, const char *rel) {
    return join(ir, dir, rel, true);
}

u64 tm_texture_id(tm_ir *ir, const char *tileset_name, const char *base_rel, const char *image) {
    u64 id = tm_setting_map(ir->desc->tilesets, ir->desc->tileset_count, tileset_name);
    if (id == 0) {
        /* The image relative to the asset root: source dir + (base_rel + image). */
        const char *src_dir = tm_dirname(ir, ir->desc->source_path ? ir->desc->source_path : "");
        const char *rel = tm_join(ir, base_rel, image);
        const char *full = rel != NULL ? join(ir, src_dir, rel, false) : NULL;
        if (full == NULL || full[0] == '\0') {
            tm_fail(ir, DE_ERR_INVALID_ARGUMENT,
                    "tilemap: tileset \"%s\": image \"%s\" is outside the asset root; map it with "
                    "the `tilesets` setting",
                    tileset_name, image);
            return 0;
        }
        size_t n = strlen(full);
        const char *slash = strrchr(full, '/');
        const char *dot = strrchr(full, '.');
        if (dot != NULL && (slash == NULL || dot > slash))
            n = (size_t)(dot - full);
        char *idp = tm_strndup(ir->ar, full, n);
        if (idp == NULL) {
            tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
            return 0;
        }
        id = de_asset_id_from(idp).hash;
        if (id == 0) {
            de_error_clear();
            tm_fail(ir, DE_ERR_INVALID_ARGUMENT,
                    "tilemap: tileset \"%s\": \"%s\" is not a valid asset ID; map it with the "
                    "`tilesets` setting",
                    tileset_name, idp);
            return 0;
        }
    }
    tm_dep(ir, id);
    return id;
}

de_result tm_read(tm_ir *ir, const char *rel, const char **text, size_t *len) {
    if (ir->desc->read == NULL)
        return tm_fail(ir, DE_ERR_NOT_FOUND, "tilemap: cannot read external file \"%s\"", rel);
    const void *data = NULL;
    u64 size = 0;
    de_result r = ir->desc->read(ir->desc->read_user, rel, &data, &size);
    if (r != DE_OK)
        return tm_fail(ir, r, "tilemap: cannot read \"%s\"", rel);
    *text = (const char *)data;
    *len = (size_t)size;
    return DE_OK;
}

/* ---- Layers, tilesets, shapes ------------------------------------------------------------------ */

tm_ir_layer *tm_layer_add(tm_ir *ir, const char *name, u32 width, u32 height, f32 cell_w,
                          f32 cell_h, f32 origin_u, f32 origin_v) {
    if (width > DE__TM_MAX_DIM || height > DE__TM_MAX_DIM || ir->layers.count >= DE__TM_MAX_LAYERS) {
        tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: layer \"%s\" is too large (%ux%u)", name, width,
                height);
        return NULL;
    }
    if (!(cell_w > 0.0f) || !(cell_h > 0.0f) || !de__tm_finite(cell_w) ||
        !de__tm_finite(cell_h) || !de__tm_finite(origin_u) || !de__tm_finite(origin_v)) {
        tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": bad grid size or offset", name);
        return NULL;
    }
    u64 cells = (u64)width * height;
    tm_ir_layer *l = (tm_ir_layer *)tm_vec_push(ir->ar, &ir->layers, NULL);
    u32 *c = cells != 0 ? (u32 *)tm_alloc(ir->ar, (size_t)cells * 4u, 4) : NULL;
    if (l == NULL || (cells != 0 && c == NULL)) {
        tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (layer \"%s\")", name);
        return NULL;
    }
    l->name = tm_str(ir, name);
    l->width = width;
    l->height = height;
    l->cell_w = cell_w;
    l->cell_h = cell_h;
    l->origin_u = origin_u;
    l->origin_v = origin_v;
    l->opacity = 1.0f;
    l->source_index = ir->layers.count - 1;
    l->cells = c;
    return l;
}

u32 tm_tileset_add(tm_ir *ir, const char *name, u64 texture, u32 count, u32 columns, u32 tw,
                   u32 th, u32 iw, u32 ih, u32 margin, u32 spacing) {
    u32 first = ir->tiles.count;
    if (count > DE__TM_MAX_TILES - first) {
        tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: tileset \"%s\" has too many tiles", name);
        return first;
    }
    if (columns == 0 || tw == 0 || th == 0 || iw == 0 || ih == 0) {
        tm_fail(ir, DE_ERR_PARSE, "tilemap: tileset \"%s\": zero tile or image size", name);
        return first;
    }
    de__tm_tileset ts;
    memset(&ts, 0, sizeof ts);
    ts.texture = texture;
    ts.first_tile = first;
    ts.tile_count = count;
    ts.columns = columns;
    ts.image_w = iw;
    ts.image_h = ih;
    ts.name = tm_str(ir, name);
    if (tm_vec_push(ir->ar, &ir->tilesets, &ts) == NULL) {
        tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (tilesets)");
        return first;
    }
    for (u32 i = 0; i < count; ++i) {
        de__tm_tile t;
        memset(&t, 0, sizeof t);
        u32 col = i % columns, row = i / columns;
        t.u = (f32)((f64)(margin + (u64)col * (tw + spacing)) / iw);
        t.v = (f32)((f64)(margin + (u64)row * (th + spacing)) / ih);
        t.du = (f32)((f64)tw / iw);
        t.dv = (f32)((f64)th / ih);
        t.w = (f32)((f64)tw / ir->ppu);
        t.h = (f32)((f64)th / ir->ppu);
        t.tileset = ir->tilesets.count - 1;
        if (tm_vec_push(ir->ar, &ir->tiles, &t) == NULL) {
            tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (tiles)");
            return first;
        }
    }
    return first;
}

static bool add_shape(tm_ir *ir, de__tm_tile *t, u32 kind, const de__tm_point *pts, u32 n) {
    if (t->shape_count == 0)
        t->shape_first = ir->shapes.count;
    else if (t->shape_first + t->shape_count != ir->shapes.count) {
        tm_fail(ir, DE_ERR_PARSE, "tilemap: tile shapes are not contiguous");
        return false;
    }
    de__tm_shape s = {kind, ir->points.count, n};
    if (tm_vec_push(ir->ar, &ir->shapes, &s) == NULL) {
        tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (shapes)");
        return false;
    }
    for (u32 i = 0; i < n; ++i)
        if (tm_vec_push(ir->ar, &ir->points, &pts[i]) == NULL) {
            tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (points)");
            return false;
        }
    t->shape_count++;
    return true;
}

static bool convex(const de__tm_point *p, u32 n) {
    int sign = 0;
    for (u32 i = 0; i < n; ++i) {
        const de__tm_point *a = &p[i], *b = &p[(i + 1) % n], *c = &p[(i + 2) % n];
        f64 cross = ((f64)b->x - a->x) * ((f64)c->y - b->y) - ((f64)b->y - a->y) * ((f64)c->x - b->x);
        if (cross > 1e-9) {
            if (sign < 0)
                return false;
            sign = 1;
        } else if (cross < -1e-9) {
            if (sign > 0)
                return false;
            sign = -1;
        }
    }
    return sign != 0;
}

void tm_tile_shape(tm_ir *ir, u32 tile, u32 kind, const f64 *xy, u32 count, f64 th) {
    if (tile >= ir->tiles.count || count == 0)
        return;
    if (count > 4096u) {
        tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: collision shape with %u points", count);
        return;
    }
    /* One extra slot to close a loop that is split into open pieces. */
    de__tm_point *pts = (de__tm_point *)tm_alloc(ir->ar, sizeof(de__tm_point) * (count + 1u), 4);
    if (pts == NULL) {
        tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (shape)");
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        f64 x = xy[2 * i] / ir->ppu, y = (th - xy[2 * i + 1]) / ir->ppu;
        pts[i].x = (f32)x;
        pts[i].y = (f32)y;
        if (!de__tm_finite(pts[i].x) || !de__tm_finite(pts[i].y)) {
            tm_fail(ir, DE_ERR_PARSE, "tilemap: collision shape point is not finite");
            return;
        }
    }
    de__tm_tile *t = TM_AT(&ir->tiles, de__tm_tile, tile);
    if (kind == DE__TM_SHAPE_POLYGON) {
        if (count < 3) {
            tm_warn(ir, "tilemap: tile %u: collision polygon with %u points ignored", tile, count);
            return;
        }
        if (count <= 8 && convex(pts, count)) {
            add_shape(ir, t, DE__TM_SHAPE_POLYGON, pts, count);
            return;
        }
        kind = DE__TM_SHAPE_CHAIN_LOOP; /* concave or more than 8 points */
    }
    if (kind == DE__TM_SHAPE_CHAIN_LOOP) {
        if (count <= 8) {
            add_shape(ir, t, DE__TM_SHAPE_CHAIN_LOOP, pts, count);
            return;
        }
        pts[count] = pts[0]; /* unrolled: open pieces that end where they start */
        count += 1;
    } else if (count < 2) {
        tm_warn(ir, "tilemap: tile %u: collision polyline with %u point ignored", tile, count);
        return;
    }
    /* Open chain pieces of at most 8 points, each starting on the previous piece's last point. */
    for (u32 start = 0; start + 1 < count; start += 7u) {
        u32 n = count - start < 8u ? count - start : 8u;
        if (!add_shape(ir, t, DE__TM_SHAPE_CHAIN, pts + start, n))
            return;
    }
}

/* ---- Codecs ------------------------------------------------------------------------------------ */

static int b64(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

de_result tm_base64(tm_ir *ir, const char *text, size_t len, u8 **out, size_t *out_len,
                    const char *name) {
    u8 *o = (u8 *)tm_alloc(ir->ar, len / 4u * 3u + 3u, 1);
    if (o == NULL)
        return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (layer \"%s\")", name);
    size_t n = 0;
    u32 acc = 0, bits = 0, pad = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        if (c == '=') {
            ++pad;
            continue;
        }
        int v = b64(c);
        if (v < 0 || pad != 0)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": invalid base64 data", name);
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o[n++] = (u8)(acc >> bits);
        }
    }
    if (pad > 2)
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": invalid base64 padding", name);
    *out = o;
    *out_len = n;
    return DE_OK;
}

de_result tm_decompress(tm_ir *ir, const char *compression, const u8 *in, size_t in_len,
                        u8 *out, size_t out_len, const char *name) {
    if (in_len > INT_MAX || out_len > INT_MAX)
        return tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: layer \"%s\" is too large", name);
    if (compression == NULL || compression[0] == '\0') {
        if (in_len != out_len)
            return tm_fail(ir, DE_ERR_PARSE,
                           "tilemap: layer \"%s\": %zu bytes of tile data, expected %zu", name,
                           in_len, out_len);
        memcpy(out, in, out_len);
        return DE_OK;
    }
    int got = -1;
    if (strcmp(compression, "zlib") == 0) {
        got = stbi_zlib_decode_buffer((char *)out, (int)out_len, (const char *)in, (int)in_len);
    } else if (strcmp(compression, "gzip") == 0) {
        /* RFC 1952 header, then raw deflate (the 8-byte CRC/size trailer is not read). */
        size_t p = 10;
        if (in_len < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": bad gzip header", name);
        u8 flg = in[3];
        if (flg & 4u) {
            if (p + 2 > in_len)
                return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": bad gzip header", name);
            p += 2u + ((size_t)in[p] | ((size_t)in[p + 1] << 8));
        }
        for (u32 k = 0; k < 2; ++k)
            if (flg & (k == 0 ? 8u : 16u)) {
                while (p < in_len && in[p] != 0)
                    ++p;
                ++p;
            }
        if (flg & 2u)
            p += 2;
        if (p >= in_len)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": bad gzip header", name);
        got = stbi_zlib_decode_noheader_buffer((char *)out, (int)out_len, (const char *)(in + p),
                                               (int)(in_len - p));
    } else if (strcmp(compression, "zstd") == 0) {
        size_t r = ZSTD_decompress(out, out_len, in, in_len);
        got = ZSTD_isError(r) ? -1 : (int)r;
    } else {
        return tm_fail(ir, DE_ERR_PARSE,
                       "tilemap: layer \"%s\": unknown compression \"%s\" (supported: zlib, gzip, "
                       "zstd, none)",
                       name, compression);
    }
    if (got < 0 || (size_t)got != out_len)
        return tm_fail(ir, DE_ERR_PARSE,
                       "tilemap: layer \"%s\": %s data does not decode to %zu bytes", name,
                       compression, out_len);
    return DE_OK;
}

/* ---- Writer ------------------------------------------------------------------------------------ */

typedef struct tm_buf {
    u8 *p;
    size_t at;
} tm_buf;

static void put32(tm_buf *b, u32 v) {
    de__tm_wr32(b->p + b->at, v);
    b->at += 4;
}
static void putf(tm_buf *b, f32 f) {
    de__tm_wrf(b->p + b->at, f);
    b->at += 4;
}

de_result tm_ir_write(tm_ir *ir) {
    /* Chunks and flags. */
    u64 chunks = 0;
    for (u32 i = 0; i < ir->layers.count; ++i) {
        tm_ir_layer *l = TM_AT(&ir->layers, tm_ir_layer, i);
        u64 cx = (l->width + DE__TM_CHUNK - 1) / DE__TM_CHUNK;
        u64 cy = (l->height + DE__TM_CHUNK - 1) / DE__TM_CHUNK;
        chunks += cx * cy;
        for (u64 k = 0; k < (u64)l->width * l->height; ++k) {
            u32 t = l->cells[k] & DE__TM_CELL_TILE_MASK;
            if (t != 0 && t <= ir->tiles.count &&
                TM_AT(&ir->tiles, de__tm_tile, t - 1)->anim != 0)
                l->flags |= DE__TM_LAYER_ANIMATED;
        }
    }
    if (chunks > DE__TM_MAX_CHUNKS)
        return tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: map too large (%llu chunks)",
                       (unsigned long long)chunks);
    if (ir->frames.count > DE__TM_MAX_RECORDS || ir->shapes.count > DE__TM_MAX_RECORDS ||
        ir->points.count > DE__TM_MAX_RECORDS || ir->spawns.count > DE__TM_MAX_RECORDS ||
        ir->fields.count > DE__TM_MAX_RECORDS)
        return tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: too many records");
    const u64 size = DE__TM_HEADER_SIZE + (u64)ir->tilesets.count * DE__TM_TILESET_SIZE +
                     (u64)ir->tiles.count * DE__TM_TILE_SIZE +
                     (u64)ir->layers.count * DE__TM_LAYER_SIZE +
                     (u64)ir->anims.count * DE__TM_ANIM_SIZE +
                     (u64)ir->frames.count * DE__TM_FRAME_SIZE +
                     (u64)ir->shapes.count * DE__TM_SHAPE_SIZE +
                     (u64)ir->points.count * DE__TM_POINT_SIZE +
                     (u64)ir->spawns.count * DE__TM_SPAWN_SIZE +
                     (u64)ir->fields.count * DE__TM_FIELD_SIZE + chunks * DE__TM_CHUNK_CELLS * 4u +
                     ir->strings.count;
    if (size > (u64)SIZE_MAX / 2)
        return tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: cooked map too large");
    const de_allocator *a = ir->desc->allocator != NULL ? ir->desc->allocator : de_heap_allocator();
    u8 *p = (u8 *)DE_ALLOC(a, (size_t)size, 16, DE_MEM_ASSET);
    if (p == NULL)
        return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (cooked map)");
    memset(p, 0, (size_t)size);
    tm_buf b = {p, 0};
    put32(&b, DE__TM_MAGIC);
    put32(&b, DE__TM_VERSION);
    put32(&b, 0);
    putf(&b, ir->ppu);
    put32(&b, ir->tilesets.count);
    put32(&b, ir->tiles.count);
    put32(&b, ir->layers.count);
    put32(&b, (u32)chunks);
    put32(&b, ir->anims.count);
    put32(&b, ir->frames.count);
    put32(&b, ir->shapes.count);
    put32(&b, ir->points.count);
    put32(&b, ir->spawns.count);
    put32(&b, ir->fields.count);
    put32(&b, ir->strings.count);
    put32(&b, 0);
    for (u32 i = 0; i < ir->tilesets.count; ++i) {
        const de__tm_tileset *t = TM_AT(&ir->tilesets, de__tm_tileset, i);
        put32(&b, (u32)t->texture);
        put32(&b, (u32)(t->texture >> 32));
        put32(&b, t->first_tile);
        put32(&b, t->tile_count);
        put32(&b, t->columns);
        put32(&b, t->image_w);
        put32(&b, t->image_h);
        put32(&b, t->name);
    }
    for (u32 i = 0; i < ir->tiles.count; ++i) {
        const de__tm_tile *t = TM_AT(&ir->tiles, de__tm_tile, i);
        putf(&b, t->u);
        putf(&b, t->v);
        putf(&b, t->du);
        putf(&b, t->dv);
        putf(&b, t->w);
        putf(&b, t->h);
        put32(&b, t->tileset);
        put32(&b, t->anim);
        put32(&b, t->shape_first);
        put32(&b, t->shape_count);
        put32(&b, t->flags);
    }
    u32 chunk_first = 0;
    for (u32 i = 0; i < ir->layers.count; ++i) {
        const tm_ir_layer *l = TM_AT(&ir->layers, tm_ir_layer, i);
        u32 cx = (l->width + DE__TM_CHUNK - 1) / DE__TM_CHUNK;
        u32 cy = (l->height + DE__TM_CHUNK - 1) / DE__TM_CHUNK;
        put32(&b, l->name);
        put32(&b, l->flags);
        put32(&b, l->width);
        put32(&b, l->height);
        put32(&b, cx);
        put32(&b, cy);
        put32(&b, chunk_first);
        putf(&b, l->cell_w);
        putf(&b, l->cell_h);
        putf(&b, l->origin_u);
        putf(&b, l->origin_v);
        putf(&b, l->opacity);
        put32(&b, l->source_index);
        put32(&b, 0);
        chunk_first += cx * cy;
    }
    for (u32 i = 0; i < ir->anims.count; ++i) {
        const de__tm_anim *an = TM_AT(&ir->anims, de__tm_anim, i);
        put32(&b, an->frame_first);
        put32(&b, an->frame_count);
        put32(&b, an->total_ms);
    }
    for (u32 i = 0; i < ir->frames.count; ++i) {
        const de__tm_frame *f = TM_AT(&ir->frames, de__tm_frame, i);
        put32(&b, f->tile);
        put32(&b, f->duration_ms);
    }
    for (u32 i = 0; i < ir->shapes.count; ++i) {
        const de__tm_shape *s = TM_AT(&ir->shapes, de__tm_shape, i);
        put32(&b, s->kind);
        put32(&b, s->point_first);
        put32(&b, s->point_count);
    }
    for (u32 i = 0; i < ir->points.count; ++i) {
        const de__tm_point *pt = TM_AT(&ir->points, de__tm_point, i);
        putf(&b, pt->x);
        putf(&b, pt->y);
    }
    for (u32 i = 0; i < ir->spawns.count; ++i) {
        const de__tm_spawn *s = TM_AT(&ir->spawns, de__tm_spawn, i);
        put32(&b, (u32)s->prefab);
        put32(&b, (u32)(s->prefab >> 32));
        put32(&b, s->name);
        put32(&b, s->group);
        putf(&b, s->u);
        putf(&b, s->v);
        putf(&b, s->w);
        putf(&b, s->h);
        put32(&b, s->field_first);
        put32(&b, s->field_count);
    }
    for (u32 i = 0; i < ir->fields.count; ++i) {
        const de__tm_field *f = TM_AT(&ir->fields, de__tm_field, i);
        put32(&b, f->path);
        put32(&b, f->kind);
        put32(&b, f->string);
        put32(&b, 0);
        u64 bits;
        f64 num = f->number == 0.0 ? 0.0 : f->number; /* canonical +0 */
        memcpy(&bits, &num, sizeof bits);
        put32(&b, (u32)bits);
        put32(&b, (u32)(bits >> 32));
    }
    for (u32 i = 0; i < ir->layers.count; ++i) {
        const tm_ir_layer *l = TM_AT(&ir->layers, tm_ir_layer, i);
        u32 cxn = (l->width + DE__TM_CHUNK - 1) / DE__TM_CHUNK;
        u32 cyn = (l->height + DE__TM_CHUNK - 1) / DE__TM_CHUNK;
        for (u32 cy = 0; cy < cyn; ++cy)
            for (u32 cx = 0; cx < cxn; ++cx)
                for (u32 y = 0; y < DE__TM_CHUNK; ++y)
                    for (u32 x = 0; x < DE__TM_CHUNK; ++x) {
                        u32 gx = cx * DE__TM_CHUNK + x, gy = cy * DE__TM_CHUNK + y;
                        put32(&b, gx < l->width && gy < l->height
                                      ? l->cells[(size_t)gy * l->width + gx]
                                      : 0u);
                    }
    }
    memcpy(b.p + b.at, ir->strings.data, ir->strings.count);
    b.at += ir->strings.count;
    ir->out->data = p;
    ir->out->size = size;
    return DE_OK;
}

/* ---- Entry ------------------------------------------------------------------------------------- */

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m)
        return false;
    for (size_t i = 0; i < m; ++i) {
        char c = s[n - m + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != suffix[i])
            return false;
    }
    return true;
}

static de__tm_format sniff(const de__tm_import_desc *d) {
    const char *path = d->source_path != NULL ? d->source_path : "";
    if (ends_with(path, ".tmx"))
        return DE__TM_FORMAT_TMX;
    if (ends_with(path, ".ldtk"))
        return DE__TM_FORMAT_LDTK;
    if (ends_with(path, ".tmj") || ends_with(path, ".json"))
        return DE__TM_FORMAT_TMJ;
    const char *s = (const char *)d->source;
    size_t i = 0;
    while (i < d->size && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t' ||
                           (u8)s[i] == 0xEF || (u8)s[i] == 0xBB || (u8)s[i] == 0xBF))
        ++i;
    if (i < d->size && s[i] == '<')
        return DE__TM_FORMAT_TMX;
    /* LDtk projects carry `"__header__"` and `"defs"`; Tiled maps do not. */
    for (size_t k = i; k + 6 <= d->size && k < i + 4096u; ++k)
        if (memcmp(s + k, "\"defs\"", 6) == 0)
            return DE__TM_FORMAT_LDTK;
    return DE__TM_FORMAT_TMJ;
}

de_result de__tm_import(const de__tm_import_desc *desc, de__tm_import_result *out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "tilemap import: NULL result");
    memset(out, 0, sizeof *out);
    if (desc == NULL || (desc->source == NULL && desc->size != 0)) {
        snprintf(out->message, sizeof out->message, "tilemap: NULL source");
        return DE_ERR_INVALID_ARGUMENT;
    }
    tm_arena ar;
    tm_arena_init(&ar, desc->allocator, desc->memory_limit);
    tm_ir ir;
    memset(&ir, 0, sizeof ir);
    ir.ar = &ar;
    ir.desc = desc;
    ir.out = out;
    tm_vec_init(&ir.tilesets, sizeof(de__tm_tileset));
    tm_vec_init(&ir.tiles, sizeof(de__tm_tile));
    tm_vec_init(&ir.layers, sizeof(tm_ir_layer));
    tm_vec_init(&ir.anims, sizeof(de__tm_anim));
    tm_vec_init(&ir.frames, sizeof(de__tm_frame));
    tm_vec_init(&ir.shapes, sizeof(de__tm_shape));
    tm_vec_init(&ir.points, sizeof(de__tm_point));
    tm_vec_init(&ir.spawns, sizeof(de__tm_spawn));
    tm_vec_init(&ir.fields, sizeof(de__tm_field));
    tm_vec_init(&ir.strings, 1);
    const char zero = '\0';
    if (tm_vec_push(&ar, &ir.strings, &zero) == NULL) {
        tm_arena_free(&ar);
        snprintf(out->message, sizeof out->message, "tilemap: out of memory");
        return DE_ERR_OUT_OF_MEMORY;
    }
    if (desc->pixels_per_unit < 0.0 || desc->pixels_per_unit != desc->pixels_per_unit ||
        desc->pixels_per_unit > 1e6) {
        tm_fail(&ir, DE_ERR_INVALID_ARGUMENT, "tilemap: pixels_per_unit must be positive");
    } else {
        de__tm_format f = desc->format != DE__TM_FORMAT_AUTO ? desc->format : sniff(desc);
        const char *text = desc->source != NULL ? (const char *)desc->source : "";
        size_t len = (size_t)desc->size;
        de_result r = f == DE__TM_FORMAT_TMX    ? tm_import_tmx(&ir, text, len)
                      : f == DE__TM_FORMAT_LDTK ? tm_import_ldtk(&ir, text, len)
                                                : tm_import_tmj(&ir, text, len);
        if (r == DE_OK && ar.failed)
            r = tm_fail(&ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
        if (r == DE_OK && ir.error == DE_OK)
            (void)tm_ir_write(&ir);
        else if (ir.error == DE_OK)
            tm_fail(&ir, r, "tilemap: import failed");
    }
    tm_arena_free(&ar);
    if (ir.error != DE_OK) {
        de__tm_import_free(desc->allocator, out);
        out->warnings = ir.out->warnings;
        return ir.error;
    }
    return DE_OK;
}

void de__tm_import_free(const de_allocator *a, de__tm_import_result *r) {
    if (r == NULL)
        return;
    if (r->data != NULL)
        DE_FREE(a != NULL ? a : de_heap_allocator(), r->data, (size_t)r->size);
    r->data = NULL;
    r->size = 0;
}
