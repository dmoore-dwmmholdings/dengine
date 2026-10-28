/*
 * import_tiled.c: the Tiled front end of the tilemap importer (DENG-80): `.tmj` (JSON) and `.tmx`
 * (XML) maps, embedded tilesets and external `.tsj` / `.tsx` ones. Both formats are read into the
 * same small structures (tl_object, tl_prop) and share everything after that, so equivalent maps
 * cook to the same bytes.
 *
 * Supported: orthogonal, finite maps; tile layers (CSV, base64 uncompressed / zlib / gzip / zstd,
 * TMX `<tile>` elements); object layers (objects become spawn records); group layers (flattened,
 * offsets added); per-tile animations; per-tile collision objects (a rectangle that covers the
 * whole tile marks it solid; other rectangles, ellipses, polygons and polylines become shapes);
 * the layer property `collision` (bool). Not supported (DE_ERR_UNSUPPORTED): infinite maps, other
 * orientations, image-collection tilesets. Image layers are skipped with a warning.
 */
#include "cook/importers/tilemap/tilemap_ir.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TL_MAX_TILESETS 256u
#define TL_MAX_GROUP_DEPTH 32u

typedef struct tl_tileset {
    u32 firstgid, count, first_tile, tw, th;
} tl_tileset;

typedef struct tl_ctx {
    tm_ir *ir;
    tl_tileset ts[TL_MAX_TILESETS];
    u32 ts_count;
    u32 tw, th;
} tl_ctx;

typedef enum tl_kind { TL_RECT, TL_ELLIPSE, TL_POINT, TL_POLYGON, TL_POLYLINE } tl_kind;

typedef struct tl_prop {
    const char *name, *type, *str;
    f64 num;
    bool b;
} tl_prop;

typedef struct tl_object {
    const char *name, *type;
    f64 x, y, w, h, rotation;
    u32 gid;
    tl_kind kind;
    f64 *xy; /* polygon / polyline points, relative to (x, y) */
    u32 count;
    tl_prop *props;
    u32 prop_count;
} tl_object;

/* ---- Shared ------------------------------------------------------------------------------------ */

static bool near(f64 a, f64 b) {
    return fabs(a - b) < 1e-6;
}

/* gid -> cell value (0 for gid 0). */
static de_result gid_cell(tl_ctx *c, u32 gid, const char *layer, u32 *cell) {
    u32 flips = gid & 0xE0000000u;
    u32 id = gid & 0x0FFFFFFFu; /* bit 28: hexagonal 120-degree rotation, ignored */
    if (id == 0) {
        *cell = 0;
        return DE_OK;
    }
    for (u32 i = c->ts_count; i-- > 0;) {
        const tl_tileset *t = &c->ts[i];
        if (id >= t->firstgid) {
            if (id - t->firstgid >= t->count)
                break;
            *cell = (t->first_tile + (id - t->firstgid) + 1u) | flips;
            return DE_OK;
        }
    }
    return tm_fail(c->ir, DE_ERR_PARSE, "tilemap: layer \"%s\": tile gid %u is in no tileset",
                   layer, id);
}

static const tl_tileset *gid_tileset(const tl_ctx *c, u32 gid) {
    u32 id = gid & 0x0FFFFFFFu;
    for (u32 i = c->ts_count; i-- > 0;)
        if (id >= c->ts[i].firstgid)
            return id - c->ts[i].firstgid < c->ts[i].count ? &c->ts[i] : NULL;
    return NULL;
}

/* A tile's collision object (pixels relative to the tile's top-left). */
static void tile_object(tl_ctx *c, u32 tile, const tl_object *o, f64 tw, f64 th) {
    tm_ir *ir = c->ir;
    switch (o->kind) {
    case TL_RECT:
        if (o->w <= 0 || o->h <= 0)
            return;
        if (near(o->x, 0) && near(o->y, 0) && near(o->w, tw) && near(o->h, th) &&
            near(o->rotation, 0)) {
            TM_AT(&ir->tiles, de__tm_tile, tile)->flags |= DE__TM_TILE_SOLID;
            return;
        }
        {
            f64 xy[8] = {o->x, o->y, o->x + o->w, o->y, o->x + o->w, o->y + o->h, o->x, o->y + o->h};
            tm_tile_shape(ir, tile, DE__TM_SHAPE_POLYGON, xy, 4, th);
        }
        return;
    case TL_ELLIPSE: {
        /* Eight points at 45-degree steps; a table, so every host cooks the same bytes. */
#define TL_S 0.70710678118654752440
        static const f64 k_cs[16] = {1, 0, TL_S, TL_S, 0, 1, -TL_S, TL_S, -1, 0, -TL_S, -TL_S,
                                     0, -1, TL_S, -TL_S};
#undef TL_S
        f64 xy[16];
        for (u32 k = 0; k < 8; ++k) {
            xy[2 * k] = o->x + o->w * 0.5 * (1.0 + k_cs[2 * k]);
            xy[2 * k + 1] = o->y + o->h * 0.5 * (1.0 + k_cs[2 * k + 1]);
        }
        tm_tile_shape(ir, tile, DE__TM_SHAPE_POLYGON, xy, 8, th);
        return;
    }
    case TL_POLYGON:
    case TL_POLYLINE: {
        f64 *xy = (f64 *)tm_alloc(ir->ar, sizeof(f64) * 2u * (o->count + 1u), 8);
        if (xy == NULL) {
            tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
            return;
        }
        for (u32 k = 0; k < o->count; ++k) {
            xy[2 * k] = o->x + o->xy[2 * k];
            xy[2 * k + 1] = o->y + o->xy[2 * k + 1];
        }
        tm_tile_shape(ir, tile,
                      o->kind == TL_POLYGON ? DE__TM_SHAPE_POLYGON : DE__TM_SHAPE_CHAIN, xy,
                      o->count, th);
        return;
    }
    case TL_POINT:
        return;
    }
}

static void add_fields(tm_ir *ir, de__tm_spawn *s, const tl_prop *props, u32 count,
                       const char *object) {
    s->field_first = ir->fields.count;
    for (u32 i = 0; i < count; ++i) {
        const tl_prop *p = &props[i];
        de__tm_field f;
        memset(&f, 0, sizeof f);
        f.path = tm_str(ir, p->name);
        const char *t = p->type != NULL ? p->type : "string";
        if (strcmp(t, "bool") == 0) {
            f.kind = DE__TM_FIELD_BOOL;
            f.number = p->b ? 1.0 : 0.0;
        } else if (strcmp(t, "int") == 0 || strcmp(t, "object") == 0) {
            f.kind = DE__TM_FIELD_INT;
            f.number = p->num;
        } else if (strcmp(t, "float") == 0) {
            f.kind = DE__TM_FIELD_FLOAT;
            f.number = p->num;
        } else if (strcmp(t, "string") == 0 || strcmp(t, "color") == 0 ||
                   strcmp(t, "file") == 0) {
            f.kind = DE__TM_FIELD_STRING;
            f.string = tm_str(ir, p->str != NULL ? p->str : "");
        } else {
            tm_warn(ir, "tilemap: object \"%s\": property \"%s\" of type \"%s\" skipped", object,
                    p->name, t);
            continue;
        }
        if (tm_vec_push(ir->ar, &ir->fields, &f) == NULL) {
            tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (fields)");
            return;
        }
    }
    s->field_count = ir->fields.count - s->field_first;
}

/* An object of an object layer -> a spawn record. (ox, oy): the layer's pixel offset. */
static void spawn_object(tl_ctx *c, const tl_object *o, const char *layer, f64 ox, f64 oy) {
    tm_ir *ir = c->ir;
    f64 px = o->x + ox, py = o->y + oy;
    f64 w = o->w, h = o->h;
    if (o->gid != 0) {
        /* Tile objects are anchored bottom-left. */
        px += w * 0.5;
        py -= h * 0.5;
    } else if (o->kind == TL_RECT || o->kind == TL_ELLIPSE) {
        px += w * 0.5;
        py += h * 0.5;
    }
    de__tm_spawn s;
    memset(&s, 0, sizeof s);
    const char *type = o->type != NULL ? o->type : "";
    s.prefab = type[0] != '\0'
                   ? tm_setting_map(ir->desc->entities, ir->desc->entity_count, type)
                   : 0;
    tm_dep(ir, s.prefab);
    s.name = tm_str(ir, type[0] != '\0' ? type : (o->name != NULL ? o->name : ""));
    s.group = tm_str(ir, layer);
    s.u = (f32)(px / ir->ppu);
    s.v = (f32)(-py / ir->ppu);
    s.w = (f32)(w / ir->ppu);
    s.h = (f32)(h / ir->ppu);
    add_fields(ir, &s, o->props, o->prop_count, o->name != NULL ? o->name : type);
    if (ir->error == DE_OK && tm_vec_push(ir->ar, &ir->spawns, &s) == NULL)
        tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (spawns)");
}

static bool prop_collision(const tl_prop *props, u32 count) {
    for (u32 i = 0; i < count; ++i)
        if (strcmp(props[i].name, "collision") == 0)
            return props[i].b;
    return false;
}

static tm_ir_layer *tile_layer(tl_ctx *c, const char *name, u32 w, u32 h, f64 ox, f64 oy,
                               f64 opacity, bool collide) {
    tm_ir *ir = c->ir;
    tm_ir_layer *l = tm_layer_add(ir, name, w, h, (f32)(c->tw / ir->ppu), (f32)(c->th / ir->ppu),
                                  (f32)(ox / ir->ppu), (f32)(-oy / ir->ppu));
    if (l != NULL) {
        l->opacity = (f32)opacity;
        if (collide || tm_solid_layer(ir, name))
            l->flags |= DE__TM_LAYER_COLLIDE;
    }
    return l;
}

/* Decodes base64 (+ compression) tile data into the layer's cells. */
static de_result layer_bytes(tl_ctx *c, tm_ir_layer *l, const char *name, const char *text,
                             size_t len, const char *compression) {
    tm_ir *ir = c->ir;
    u8 *raw = NULL;
    size_t raw_len = 0;
    DE_TRY(tm_base64(ir, text, len, &raw, &raw_len, name));
    size_t want = (size_t)l->width * l->height * 4u;
    u8 *data = (u8 *)tm_alloc(ir->ar, want + 1u, 4);
    if (data == NULL)
        return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (layer \"%s\")", name);
    DE_TRY(tm_decompress(ir, compression, raw, raw_len, data, want, name));
    for (size_t k = 0; k < (size_t)l->width * l->height; ++k)
        DE_TRY(gid_cell(c, de__tm_rd32(data + 4 * k), name, &l->cells[k]));
    return DE_OK;
}

/* CSV tile data (TMX). */
static de_result layer_csv(tl_ctx *c, tm_ir_layer *l, const char *name, const char *text) {
    size_t n = (size_t)l->width * l->height, k = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
            ++p;
        if (*p == '\0')
            break;
        if (*p < '0' || *p > '9')
            return tm_fail(c->ir, DE_ERR_PARSE, "tilemap: layer \"%s\": bad CSV tile data", name);
        u64 v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10u + (u64)(*p - '0');
            if (v > 0xFFFFFFFFu)
                return tm_fail(c->ir, DE_ERR_PARSE, "tilemap: layer \"%s\": gid out of range",
                               name);
            ++p;
        }
        if (k >= n)
            return tm_fail(c->ir, DE_ERR_PARSE, "tilemap: layer \"%s\": too many tiles", name);
        DE_TRY(gid_cell(c, (u32)v, name, &l->cells[k]));
        ++k;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
            ++p;
        if (*p == ',')
            ++p;
        else if (*p != '\0')
            return tm_fail(c->ir, DE_ERR_PARSE, "tilemap: layer \"%s\": bad CSV tile data", name);
    }
    if (k != n)
        return tm_fail(c->ir, DE_ERR_PARSE, "tilemap: layer \"%s\": %zu tiles, expected %zu", name,
                       k, n);
    return DE_OK;
}

static de_result check_map(tl_ctx *c, const char *orientation, bool infinite, f64 tw, f64 th) {
    tm_ir *ir = c->ir;
    if (orientation != NULL && strcmp(orientation, "orthogonal") != 0)
        return tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: %s maps are not supported (orthogonal only)",
                       orientation);
    if (infinite)
        return tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: infinite Tiled maps are not supported");
    if (!(tw >= 1 && tw <= 4096 && th >= 1 && th <= 4096))
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: bad map tile size");
    c->tw = (u32)tw;
    c->th = (u32)th;
    ir->ppu = ir->desc->pixels_per_unit > 0 ? (f32)ir->desc->pixels_per_unit : (f32)c->tw;
    return DE_OK;
}

static bool dim(f64 v, u32 max, u32 *out) {
    if (!(v >= 0 && v <= max) || v != floor(v))
        return false;
    *out = (u32)v;
    return true;
}

/* Registers a tileset; `tiles` are its per-tile records filled by the caller afterwards. */
static de_result tileset_begin(tl_ctx *c, u32 firstgid, const char *name, const char *base_rel,
                               const char *image, f64 iw, f64 ih, f64 tw, f64 th, f64 count,
                               f64 columns, f64 margin, f64 spacing, tl_tileset **out) {
    tm_ir *ir = c->ir;
    if (c->ts_count >= TL_MAX_TILESETS)
        return tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: more than %u tilesets", TL_MAX_TILESETS);
    if (image == NULL || image[0] == '\0')
        return tm_fail(ir, DE_ERR_UNSUPPORTED,
                       "tilemap: tileset \"%s\": image-collection tilesets are not supported",
                       name);
    u32 uiw, uih, utw, uth, ucount, ucols, um, us;
    if (!dim(iw, 65536, &uiw) || !dim(ih, 65536, &uih) || !dim(tw, 4096, &utw) ||
        !dim(th, 4096, &uth) || !dim(count, DE__TM_MAX_TILES, &ucount) ||
        !dim(columns, 65536, &ucols) || !dim(margin, 4096, &um) || !dim(spacing, 4096, &us) ||
        firstgid == 0 || ucols == 0)
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: tileset \"%s\": bad sizes", name);
    if (c->ts_count > 0) {
        const tl_tileset *prev = &c->ts[c->ts_count - 1];
        if (firstgid < prev->firstgid + prev->count)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: tileset \"%s\": overlapping firstgid", name);
    }
    u64 tex = tm_texture_id(ir, name, base_rel, image);
    if (ir->error != DE_OK)
        return ir->error;
    tl_tileset *t = &c->ts[c->ts_count++];
    t->firstgid = firstgid;
    t->count = ucount;
    t->tw = utw;
    t->th = uth;
    t->first_tile = tm_tileset_add(ir, name, tex, ucount, ucols, utw, uth, uiw, uih, um, us);
    *out = t;
    return ir->error;
}

static de_result tile_anim(tl_ctx *c, const tl_tileset *t, u32 local, const u32 *ids,
                           const u32 *durations, u32 n) {
    tm_ir *ir = c->ir;
    if (n == 0)
        return DE_OK;
    de__tm_anim a = {ir->frames.count, n, 0};
    for (u32 k = 0; k < n; ++k) {
        if (ids[k] >= t->count)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: animation frame tile %u out of range",
                           ids[k]);
        de__tm_frame f = {t->first_tile + ids[k], durations[k]};
        a.total_ms += durations[k];
        if (tm_vec_push(ir->ar, &ir->frames, &f) == NULL)
            return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
    }
    if (a.total_ms == 0) {
        tm_warn(ir, "tilemap: tile %u: animation with zero duration ignored", local);
        return DE_OK;
    }
    if (tm_vec_push(ir->ar, &ir->anims, &a) == NULL)
        return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
    TM_AT(&ir->tiles, de__tm_tile, t->first_tile + local)->anim = ir->anims.count;
    return DE_OK;
}

/* ---- JSON (.tmj / .tsj) -------------------------------------------------------------------------- */

static de_result j_props(tm_ir *ir, const tmj_val *owner, tl_prop **out, u32 *count) {
    *out = NULL;
    *count = 0;
    const tmj_val *arr = tmj_arr(owner, "properties");
    if (arr == NULL || arr->count == 0)
        return DE_OK;
    tl_prop *p = (tl_prop *)tm_alloc(ir->ar, sizeof(tl_prop) * arr->count, 8);
    if (p == NULL)
        return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
    u32 n = 0;
    for (u32 i = 0; i < arr->count; ++i) {
        const tmj_val *e = &arr->u.items[i];
        const char *name = tmj_str(e, "name", NULL);
        if (name == NULL)
            continue;
        tl_prop *q = &p[n++];
        q->name = name;
        q->type = tmj_str(e, "type", "string");
        const tmj_val *v = tmj_get(e, "value");
        if (v != NULL) {
            if (v->kind == TMJ_BOOL)
                q->b = v->u.b;
            else if (v->kind == TMJ_NUM)
                q->num = v->u.n;
            else if (v->kind == TMJ_STR)
                q->str = v->u.s;
        }
    }
    *out = p;
    *count = n;
    return DE_OK;
}

static de_result j_object(tm_ir *ir, const tmj_val *o, tl_object *out) {
    memset(out, 0, sizeof *out);
    out->name = tmj_str(o, "name", "");
    out->type = tmj_str(o, "type", NULL);
    if (out->type == NULL || out->type[0] == '\0')
        out->type = tmj_str(o, "class", "");
    out->x = tmj_num(o, "x", 0);
    out->y = tmj_num(o, "y", 0);
    out->w = tmj_num(o, "width", 0);
    out->h = tmj_num(o, "height", 0);
    out->rotation = tmj_num(o, "rotation", 0);
    f64 gid = tmj_num(o, "gid", 0);
    out->gid = gid >= 0 && gid <= 4294967295.0 ? (u32)gid : 0u;
    out->kind = TL_RECT;
    if (tmj_bool(o, "ellipse", false))
        out->kind = TL_ELLIPSE;
    if (tmj_bool(o, "point", false))
        out->kind = TL_POINT;
    const tmj_val *poly = tmj_arr(o, "polygon");
    const tmj_val *line = tmj_arr(o, "polyline");
    if (poly != NULL || line != NULL) {
        const tmj_val *pts = poly != NULL ? poly : line;
        out->kind = poly != NULL ? TL_POLYGON : TL_POLYLINE;
        out->xy = (f64 *)tm_alloc(ir->ar, sizeof(f64) * 2u * (pts->count + 1u), 8);
        if (out->xy == NULL)
            return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
        for (u32 k = 0; k < pts->count; ++k) {
            out->xy[2 * k] = tmj_num(&pts->u.items[k], "x", 0);
            out->xy[2 * k + 1] = tmj_num(&pts->u.items[k], "y", 0);
        }
        out->count = pts->count;
    }
    return j_props(ir, o, &out->props, &out->prop_count);
}

static de_result j_tileset(tl_ctx *c, const tmj_val *ts, u32 firstgid, const char *base_rel);
static de_result x_tileset(tl_ctx *c, const tmx_node *ts, u32 firstgid, const char *base_rel);

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

/* An external tileset file (`.tsj`/`.json` or `.tsx`), relative to the source directory. */
static de_result external_tileset(tl_ctx *c, u32 firstgid, const char *rel) {
    tm_ir *ir = c->ir;
    const char *text = NULL;
    size_t len = 0;
    DE_TRY(tm_read(ir, rel, &text, &len));
    char err[160];
    const char *base = tm_dirname(ir, rel);
    if (ends_with(rel, ".tsx")) {
        const tmx_node *root = tmx_parse(ir->ar, text, len, err, sizeof err);
        if (root == NULL)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: %s:%s", rel, err);
        if (strcmp(root->name, "tileset") != 0)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: %s: not a tileset", rel);
        return x_tileset(c, root, firstgid, base);
    }
    const tmj_val *root = tmj_parse(ir->ar, text, len, err, sizeof err);
    if (root == NULL)
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: %s:%s", rel, err);
    return j_tileset(c, root, firstgid, base);
}

static de_result j_tileset(tl_ctx *c, const tmj_val *ts, u32 firstgid, const char *base_rel) {
    tm_ir *ir = c->ir;
    const char *name = tmj_str(ts, "name", "");
    tl_tileset *t = NULL;
    DE_TRY(tileset_begin(c, firstgid, name, base_rel, tmj_str(ts, "image", NULL),
                         tmj_num(ts, "imagewidth", 0), tmj_num(ts, "imageheight", 0),
                         tmj_num(ts, "tilewidth", 0), tmj_num(ts, "tileheight", 0),
                         tmj_num(ts, "tilecount", 0), tmj_num(ts, "columns", 0),
                         tmj_num(ts, "margin", 0), tmj_num(ts, "spacing", 0), &t));
    const tmj_val *tiles = tmj_arr(ts, "tiles");
    for (u32 i = 0; tiles != NULL && i < tiles->count; ++i) {
        const tmj_val *tile = &tiles->u.items[i];
        f64 id = tmj_num(tile, "id", -1);
        if (!(id >= 0 && id < t->count))
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: tileset \"%s\": bad tile id", name);
        u32 local = (u32)id;
        const tmj_val *anim = tmj_arr(tile, "animation");
        if (anim != NULL && anim->count > 0) {
            u32 *ids = (u32 *)tm_alloc(ir->ar, sizeof(u32) * anim->count * 2u, 4);
            if (ids == NULL)
                return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
            for (u32 k = 0; k < anim->count; ++k) {
                f64 fid = tmj_num(&anim->u.items[k], "tileid", -1);
                f64 dur = tmj_num(&anim->u.items[k], "duration", 0);
                if (!(fid >= 0 && fid < 16777216.0) || !(dur >= 0 && dur < 1e9))
                    return tm_fail(ir, DE_ERR_PARSE, "tilemap: tileset \"%s\": bad animation",
                                   name);
                ids[k] = (u32)fid;
                ids[anim->count + k] = (u32)dur;
            }
            DE_TRY(tile_anim(c, t, local, ids, ids + anim->count, anim->count));
        }
        const tmj_val *og = tmj_get(tile, "objectgroup");
        const tmj_val *objs = tmj_arr(og, "objects");
        for (u32 k = 0; objs != NULL && k < objs->count; ++k) {
            tl_object o;
            DE_TRY(j_object(ir, &objs->u.items[k], &o));
            tile_object(c, t->first_tile + local, &o, t->tw, t->th);
            if (ir->error != DE_OK)
                return ir->error;
        }
    }
    return ir->error;
}

static de_result j_layers(tl_ctx *c, const tmj_val *layers, f64 ox, f64 oy, u32 depth) {
    tm_ir *ir = c->ir;
    if (depth > TL_MAX_GROUP_DEPTH)
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: group layers nested too deep");
    for (u32 i = 0; layers != NULL && i < layers->count; ++i) {
        const tmj_val *l = &layers->u.items[i];
        const char *type = tmj_str(l, "type", "");
        const char *name = tmj_str(l, "name", "");
        f64 lx = ox + tmj_num(l, "offsetx", 0), ly = oy + tmj_num(l, "offsety", 0);
        if (strcmp(type, "group") == 0) {
            DE_TRY(j_layers(c, tmj_arr(l, "layers"), lx, ly, depth + 1));
        } else if (strcmp(type, "tilelayer") == 0) {
            if (tmj_get(l, "chunks") != NULL)
                return tm_fail(ir, DE_ERR_UNSUPPORTED,
                               "tilemap: layer \"%s\": chunked (infinite) layers are not supported",
                               name);
            u32 w, h;
            if (!dim(tmj_num(l, "width", -1), DE__TM_MAX_DIM, &w) ||
                !dim(tmj_num(l, "height", -1), DE__TM_MAX_DIM, &h))
                return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": bad size", name);
            tl_prop *props;
            u32 pc;
            DE_TRY(j_props(ir, l, &props, &pc));
            tm_ir_layer *tl = tile_layer(c, name, w, h, lx, ly, tmj_num(l, "opacity", 1),
                                         prop_collision(props, pc));
            if (tl == NULL)
                return ir->error;
            const char *enc = tmj_str(l, "encoding", "csv");
            const tmj_val *data = tmj_get(l, "data");
            if (strcmp(enc, "csv") == 0) {
                if (data == NULL || data->kind != TMJ_ARR || data->count != (u64)w * h)
                    return tm_fail(ir, DE_ERR_PARSE,
                                   "tilemap: layer \"%s\": data must be %u gids", name, w * h);
                for (u32 k = 0; k < data->count; ++k) {
                    const tmj_val *g = &data->u.items[k];
                    if (g->kind != TMJ_NUM || !(g->u.n >= 0 && g->u.n <= 4294967295.0))
                        return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": bad gid", name);
                    DE_TRY(gid_cell(c, (u32)g->u.n, name, &tl->cells[k]));
                }
            } else if (strcmp(enc, "base64") == 0) {
                if (data == NULL || data->kind != TMJ_STR)
                    return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": data must be base64",
                                   name);
                DE_TRY(layer_bytes(c, tl, name, data->u.s, data->count,
                                   tmj_str(l, "compression", "")));
            } else {
                return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": unknown encoding \"%s\"",
                               name, enc);
            }
        } else if (strcmp(type, "objectgroup") == 0) {
            const tmj_val *objs = tmj_arr(l, "objects");
            for (u32 k = 0; objs != NULL && k < objs->count; ++k) {
                tl_object o;
                DE_TRY(j_object(ir, &objs->u.items[k], &o));
                if (o.gid != 0 && gid_tileset(c, o.gid) == NULL)
                    return tm_fail(ir, DE_ERR_PARSE, "tilemap: object gid %u is in no tileset",
                                   o.gid);
                spawn_object(c, &o, name, lx, ly);
                if (ir->error != DE_OK)
                    return ir->error;
            }
        } else {
            tm_warn(ir, "tilemap: layer \"%s\" of type \"%s\" skipped", name, type);
        }
    }
    return ir->error;
}

de_result tm_import_tmj(tm_ir *ir, const char *text, size_t len) {
    char err[160];
    const tmj_val *root = tmj_parse(ir->ar, text, len, err, sizeof err);
    if (root == NULL)
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: JSON %s", err);
    if (root->kind != TMJ_OBJ)
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: a Tiled map is a JSON object");
    tl_ctx *c = (tl_ctx *)tm_alloc(ir->ar, sizeof(tl_ctx), 8);
    if (c == NULL)
        return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
    c->ir = ir;
    DE_TRY(check_map(c, tmj_str(root, "orientation", "orthogonal"), tmj_bool(root, "infinite", false),
                     tmj_num(root, "tilewidth", 0), tmj_num(root, "tileheight", 0)));
    const tmj_val *tilesets = tmj_arr(root, "tilesets");
    for (u32 i = 0; tilesets != NULL && i < tilesets->count; ++i) {
        const tmj_val *ts = &tilesets->u.items[i];
        f64 fg = tmj_num(ts, "firstgid", 0);
        if (!(fg >= 1 && fg < 268435456.0))
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: tileset %u: bad firstgid", i);
        const char *src = tmj_str(ts, "source", NULL);
        if (src != NULL)
            DE_TRY(external_tileset(c, (u32)fg, src));
        else
            DE_TRY(j_tileset(c, ts, (u32)fg, ""));
    }
    return j_layers(c, tmj_arr(root, "layers"), 0, 0, 0);
}

/* ---- XML (.tmx / .tsx) --------------------------------------------------------------------------- */

static de_result x_props(tm_ir *ir, const tmx_node *owner, tl_prop **out, u32 *count) {
    *out = NULL;
    *count = 0;
    const tmx_node *props = tmx_child(owner, "properties");
    if (props == NULL || props->child_count == 0)
        return DE_OK;
    tl_prop *p = (tl_prop *)tm_alloc(ir->ar, sizeof(tl_prop) * props->child_count, 8);
    if (p == NULL)
        return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
    u32 n = 0;
    for (u32 i = 0; i < props->child_count; ++i) {
        const tmx_node *e = &props->children[i];
        if (strcmp(e->name, "property") != 0)
            continue;
        const char *name = tmx_attr_str(e, "name", NULL);
        if (name == NULL)
            continue;
        tl_prop *q = &p[n++];
        q->name = name;
        q->type = tmx_attr_str(e, "type", "string");
        const char *value = tmx_attr_str(e, "value", NULL);
        if (value == NULL)
            value = e->text; /* multi-line strings are element text */
        q->str = value;
        q->b = strcmp(value, "true") == 0;
        q->num = tmx_attr_num(e, "value", 0);
    }
    *out = p;
    *count = n;
    return DE_OK;
}

static de_result x_points(tm_ir *ir, const char *s, tl_object *o) {
    u32 n = 0;
    for (const char *p = s; *p; ++p)
        if (*p == ',')
            ++n;
    o->xy = (f64 *)tm_alloc(ir->ar, sizeof(f64) * 2u * (n + 1u), 8);
    if (o->xy == NULL)
        return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
    const char *p = s;
    u32 k = 0;
    while (*p && k < n) {
        char *end = NULL;
        f64 x = strtod(p, &end);
        if (end == p || *end != ',')
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: bad polygon points \"%s\"", s);
        p = end + 1;
        f64 y = strtod(p, &end);
        if (end == p || !(x == x) || !(y == y))
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: bad polygon points \"%s\"", s);
        o->xy[2 * k] = x;
        o->xy[2 * k + 1] = y;
        ++k;
        p = end;
        while (*p == ' ')
            ++p;
    }
    o->count = k;
    return DE_OK;
}

static de_result x_object(tm_ir *ir, const tmx_node *e, tl_object *out) {
    memset(out, 0, sizeof *out);
    out->name = tmx_attr_str(e, "name", "");
    out->type = tmx_attr_str(e, "type", NULL);
    if (out->type == NULL || out->type[0] == '\0')
        out->type = tmx_attr_str(e, "class", "");
    out->x = tmx_attr_num(e, "x", 0);
    out->y = tmx_attr_num(e, "y", 0);
    out->w = tmx_attr_num(e, "width", 0);
    out->h = tmx_attr_num(e, "height", 0);
    out->rotation = tmx_attr_num(e, "rotation", 0);
    f64 gid = tmx_attr_num(e, "gid", 0);
    out->gid = gid >= 0 && gid <= 4294967295.0 ? (u32)gid : 0u;
    out->kind = TL_RECT;
    if (tmx_child(e, "ellipse") != NULL)
        out->kind = TL_ELLIPSE;
    if (tmx_child(e, "point") != NULL)
        out->kind = TL_POINT;
    const tmx_node *poly = tmx_child(e, "polygon");
    const tmx_node *line = tmx_child(e, "polyline");
    if (poly != NULL || line != NULL) {
        out->kind = poly != NULL ? TL_POLYGON : TL_POLYLINE;
        DE_TRY(x_points(ir, tmx_attr_str(poly != NULL ? poly : line, "points", ""), out));
    }
    return x_props(ir, e, &out->props, &out->prop_count);
}

static de_result x_tileset(tl_ctx *c, const tmx_node *ts, u32 firstgid, const char *base_rel) {
    tm_ir *ir = c->ir;
    const char *name = tmx_attr_str(ts, "name", "");
    const tmx_node *img = tmx_child(ts, "image");
    tl_tileset *t = NULL;
    DE_TRY(tileset_begin(c, firstgid, name, base_rel, tmx_attr_str(img, "source", NULL),
                         tmx_attr_num(img, "width", 0), tmx_attr_num(img, "height", 0),
                         tmx_attr_num(ts, "tilewidth", 0), tmx_attr_num(ts, "tileheight", 0),
                         tmx_attr_num(ts, "tilecount", 0), tmx_attr_num(ts, "columns", 0),
                         tmx_attr_num(ts, "margin", 0), tmx_attr_num(ts, "spacing", 0), &t));
    for (u32 i = 0; i < ts->child_count; ++i) {
        const tmx_node *tile = &ts->children[i];
        if (strcmp(tile->name, "tile") != 0)
            continue;
        f64 id = tmx_attr_num(tile, "id", -1);
        if (!(id >= 0 && id < t->count))
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: tileset \"%s\": bad tile id", name);
        u32 local = (u32)id;
        const tmx_node *anim = tmx_child(tile, "animation");
        if (anim != NULL && anim->child_count > 0) {
            u32 *ids = (u32 *)tm_alloc(ir->ar, sizeof(u32) * anim->child_count * 2u, 4);
            if (ids == NULL)
                return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
            u32 n = 0;
            for (u32 k = 0; k < anim->child_count; ++k) {
                const tmx_node *fr = &anim->children[k];
                if (strcmp(fr->name, "frame") != 0)
                    continue;
                f64 fid = tmx_attr_num(fr, "tileid", -1);
                f64 dur = tmx_attr_num(fr, "duration", 0);
                if (!(fid >= 0 && fid < 16777216.0) || !(dur >= 0 && dur < 1e9))
                    return tm_fail(ir, DE_ERR_PARSE, "tilemap: tileset \"%s\": bad animation",
                                   name);
                ids[n] = (u32)fid;
                ids[anim->child_count + n] = (u32)dur;
                ++n;
            }
            DE_TRY(tile_anim(c, t, local, ids, ids + anim->child_count, n));
        }
        const tmx_node *og = tmx_child(tile, "objectgroup");
        for (u32 k = 0; og != NULL && k < og->child_count; ++k) {
            if (strcmp(og->children[k].name, "object") != 0)
                continue;
            tl_object o;
            DE_TRY(x_object(ir, &og->children[k], &o));
            tile_object(c, t->first_tile + local, &o, t->tw, t->th);
            if (ir->error != DE_OK)
                return ir->error;
        }
    }
    return ir->error;
}

static de_result x_layers(tl_ctx *c, const tmx_node *parent, f64 ox, f64 oy, u32 depth) {
    tm_ir *ir = c->ir;
    if (depth > TL_MAX_GROUP_DEPTH)
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: group layers nested too deep");
    for (u32 i = 0; i < parent->child_count; ++i) {
        const tmx_node *l = &parent->children[i];
        const char *name = tmx_attr_str(l, "name", "");
        f64 lx = ox + tmx_attr_num(l, "offsetx", 0), ly = oy + tmx_attr_num(l, "offsety", 0);
        if (strcmp(l->name, "group") == 0) {
            DE_TRY(x_layers(c, l, lx, ly, depth + 1));
        } else if (strcmp(l->name, "layer") == 0) {
            u32 w, h;
            if (!dim(tmx_attr_num(l, "width", -1), DE__TM_MAX_DIM, &w) ||
                !dim(tmx_attr_num(l, "height", -1), DE__TM_MAX_DIM, &h))
                return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": bad size", name);
            tl_prop *props;
            u32 pc;
            DE_TRY(x_props(ir, l, &props, &pc));
            tm_ir_layer *tl = tile_layer(c, name, w, h, lx, ly, tmx_attr_num(l, "opacity", 1),
                                         prop_collision(props, pc));
            if (tl == NULL)
                return ir->error;
            const tmx_node *data = tmx_child(l, "data");
            if (data == NULL)
                return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": no <data>", name);
            if (tmx_child(data, "chunk") != NULL)
                return tm_fail(ir, DE_ERR_UNSUPPORTED,
                               "tilemap: layer \"%s\": chunked (infinite) layers are not supported",
                               name);
            const char *enc = tmx_attr_str(data, "encoding", "");
            if (strcmp(enc, "csv") == 0) {
                DE_TRY(layer_csv(c, tl, name, data->text));
            } else if (strcmp(enc, "base64") == 0) {
                DE_TRY(layer_bytes(c, tl, name, data->text, data->text_len,
                                   tmx_attr_str(data, "compression", "")));
            } else if (enc[0] == '\0') {
                u32 k = 0;
                for (u32 t = 0; t < data->child_count; ++t) {
                    const tmx_node *e = &data->children[t];
                    if (strcmp(e->name, "tile") != 0)
                        continue;
                    if (k >= (u64)w * h)
                        return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": too many tiles",
                                       name);
                    f64 g = tmx_attr_num(e, "gid", 0);
                    if (!(g >= 0 && g <= 4294967295.0))
                        return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": bad gid", name);
                    DE_TRY(gid_cell(c, (u32)g, name, &tl->cells[k]));
                    ++k;
                }
            } else {
                return tm_fail(ir, DE_ERR_PARSE, "tilemap: layer \"%s\": unknown encoding \"%s\"",
                               name, enc);
            }
        } else if (strcmp(l->name, "objectgroup") == 0) {
            for (u32 k = 0; k < l->child_count; ++k) {
                if (strcmp(l->children[k].name, "object") != 0)
                    continue;
                tl_object o;
                DE_TRY(x_object(ir, &l->children[k], &o));
                if (o.gid != 0 && gid_tileset(c, o.gid) == NULL)
                    return tm_fail(ir, DE_ERR_PARSE, "tilemap: object gid %u is in no tileset",
                                   o.gid);
                spawn_object(c, &o, name, lx, ly);
                if (ir->error != DE_OK)
                    return ir->error;
            }
        } else if (strcmp(l->name, "imagelayer") == 0) {
            tm_warn(ir, "tilemap: layer \"%s\" of type \"imagelayer\" skipped", name);
        }
    }
    return ir->error;
}

de_result tm_import_tmx(tm_ir *ir, const char *text, size_t len) {
    char err[160];
    const tmx_node *root = tmx_parse(ir->ar, text, len, err, sizeof err);
    if (root == NULL)
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: XML %s", err);
    if (strcmp(root->name, "map") != 0)
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: the root element of a TMX file is <map>");
    tl_ctx *c = (tl_ctx *)tm_alloc(ir->ar, sizeof(tl_ctx), 8);
    if (c == NULL)
        return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
    c->ir = ir;
    DE_TRY(check_map(c, tmx_attr_str(root, "orientation", "orthogonal"),
                     tmx_attr_num(root, "infinite", 0) != 0, tmx_attr_num(root, "tilewidth", 0),
                     tmx_attr_num(root, "tileheight", 0)));
    for (u32 i = 0; i < root->child_count; ++i) {
        const tmx_node *ts = &root->children[i];
        if (strcmp(ts->name, "tileset") != 0)
            continue;
        f64 fg = tmx_attr_num(ts, "firstgid", 0);
        if (!(fg >= 1 && fg < 268435456.0))
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: tileset %u: bad firstgid", i);
        const char *src = tmx_attr_str(ts, "source", NULL);
        if (src != NULL)
            DE_TRY(external_tileset(c, (u32)fg, src));
        else
            DE_TRY(x_tileset(c, ts, (u32)fg, ""));
    }
    return x_layers(c, root, 0, 0, 0);
}
