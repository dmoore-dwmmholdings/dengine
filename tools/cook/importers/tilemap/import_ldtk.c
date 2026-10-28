/*
 * import_ldtk.c: the LDtk front end of the tilemap importer (DENG-80): LDtk 1.x project files
 * (`.ldtk`), with levels inline or in external `.ldtkl` files, and multi-world projects.
 *
 * Each layer instance of each level becomes one cooked layer, named `<level>/<layer>`, placed at
 * the level's world position plus the layer's total offset, in draw order (LDtk lists layers top
 * first). Tile layers take `gridTiles`; IntGrid and auto-layers take the baked `autoLayerTiles`
 * (the auto-tiles LDtk computed from the source data; the rules are not run here). IntGrid values
 * other than 0 mark their cells solid. Entity layers become spawn records. Tilesets come from
 * `defs.tilesets` (embedded atlases are not supported).
 */
#include "cook/importers/tilemap/tilemap_ir.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LD_MAX_TILESETS 256u

typedef struct ld_tileset {
    f64 uid;
    u32 first_tile, count, columns, grid;
} ld_tileset;

typedef struct ld_ctx {
    tm_ir *ir;
    ld_tileset ts[LD_MAX_TILESETS];
    u32 ts_count;
    const char *dir; /* the project's directory, relative to the source directory: "" */
} ld_ctx;

static bool ld_int(f64 v, f64 max, u32 *out) {
    if (!(v >= 0 && v <= max) || v != floor(v))
        return false;
    *out = (u32)v;
    return true;
}

static const ld_tileset *find_tileset(const ld_ctx *c, f64 uid) {
    for (u32 i = 0; i < c->ts_count; ++i)
        if (c->ts[i].uid == uid)
            return &c->ts[i];
    return NULL;
}

static de_result tilesets(ld_ctx *c, const tmj_val *defs) {
    tm_ir *ir = c->ir;
    const tmj_val *arr = tmj_arr(defs, "tilesets");
    for (u32 i = 0; arr != NULL && i < arr->count; ++i) {
        const tmj_val *t = &arr->u.items[i];
        const char *name = tmj_str(t, "identifier", "");
        const char *rel = tmj_str(t, "relPath", NULL);
        if (rel == NULL) {
            /* Embedded atlases (LDtk's internal icons) and tilesets without an image. */
            const char *embed = tmj_str(t, "embedAtlas", NULL);
            if (embed != NULL)
                tm_warn(ir, "tilemap: LDtk tileset \"%s\": embedded atlas skipped", name);
            continue;
        }
        if (c->ts_count >= LD_MAX_TILESETS)
            return tm_fail(ir, DE_ERR_UNSUPPORTED, "tilemap: more than %u LDtk tilesets",
                           LD_MAX_TILESETS);
        u32 iw, ih, grid, spacing, padding, cw, ch;
        if (!ld_int(tmj_num(t, "pxWid", -1), 65536, &iw) ||
            !ld_int(tmj_num(t, "pxHei", -1), 65536, &ih) ||
            !ld_int(tmj_num(t, "tileGridSize", -1), 4096, &grid) ||
            !ld_int(tmj_num(t, "spacing", 0), 4096, &spacing) ||
            !ld_int(tmj_num(t, "padding", 0), 4096, &padding) ||
            !ld_int(tmj_num(t, "__cWid", -1), 65536, &cw) ||
            !ld_int(tmj_num(t, "__cHei", -1), 65536, &ch) || grid == 0 || cw == 0 || ch == 0 ||
            (u64)cw * ch > DE__TM_MAX_TILES)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: LDtk tileset \"%s\": bad sizes", name);
        u64 tex = tm_texture_id(ir, name, "", rel);
        if (ir->error != DE_OK)
            return ir->error;
        ld_tileset *s = &c->ts[c->ts_count++];
        s->uid = tmj_num(t, "uid", -1);
        s->count = cw * ch;
        s->columns = cw;
        s->grid = grid;
        s->first_tile = tm_tileset_add(ir, name, tex, s->count, cw, grid, grid, iw, ih, padding,
                                       spacing);
        if (ir->error != DE_OK)
            return ir->error;
    }
    return DE_OK;
}

/* Places `tiles` (gridTiles or autoLayerTiles) into the layer's cells. */
static de_result place_tiles(ld_ctx *c, tm_ir_layer *l, const char *name, const tmj_val *tiles,
                             const ld_tileset *ts, u32 grid) {
    tm_ir *ir = c->ir;
    bool stacked = false;
    for (u32 i = 0; tiles != NULL && i < tiles->count; ++i) {
        const tmj_val *t = &tiles->u.items[i];
        const tmj_val *px = tmj_arr(t, "px");
        u32 id, x, y, f;
        if (px == NULL || px->count != 2 || px->u.items[0].kind != TMJ_NUM ||
            px->u.items[1].kind != TMJ_NUM || !ld_int(tmj_num(t, "t", -1), 16777215.0, &id) ||
            !ld_int(tmj_num(t, "f", 0), 3, &f) || !ld_int(px->u.items[0].u.n, 1e9, &x) ||
            !ld_int(px->u.items[1].u.n, 1e9, &y))
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: LDtk layer \"%s\": bad tile %u", name, i);
        if (ts == NULL)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: LDtk layer \"%s\" has tiles but no tileset",
                           name);
        if (id >= ts->count)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: LDtk layer \"%s\": tile id %u out of range",
                           name, id);
        u32 cx = x / grid, cy = y / grid;
        if (cx >= l->width || cy >= l->height)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: LDtk layer \"%s\": tile outside the layer",
                           name);
        u32 *cell = &l->cells[(size_t)cy * l->width + cx];
        if ((*cell & DE__TM_CELL_TILE_MASK) != 0)
            stacked = true;
        u32 v = (ts->first_tile + id + 1u) | (*cell & DE__TM_CELL_SOLID);
        if (f & 1u)
            v |= DE__TM_CELL_FLIP_H;
        if (f & 2u)
            v |= DE__TM_CELL_FLIP_V;
        *cell = v;
    }
    if (stacked)
        tm_warn(ir, "tilemap: LDtk layer \"%s\": stacked tiles in one cell; the last one is kept",
                name);
    return DE_OK;
}

static de_result entity_fields(tm_ir *ir, de__tm_spawn *s, const tmj_val *fields,
                               const char *entity) {
    s->field_first = ir->fields.count;
    for (u32 i = 0; fields != NULL && i < fields->count; ++i) {
        const tmj_val *f = &fields->u.items[i];
        const char *id = tmj_str(f, "__identifier", "");
        const char *type = tmj_str(f, "__type", "");
        const tmj_val *v = tmj_get(f, "__value");
        if (v == NULL || v->kind == TMJ_NULL || id[0] == '\0')
            continue;
        /* `__` stands for `.` (LDtk identifiers cannot contain dots). */
        size_t n = strlen(id);
        char *path = (char *)tm_alloc(ir->ar, n + 1, 1);
        if (path == NULL)
            return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
        size_t w = 0;
        for (size_t k = 0; k < n; ++k) {
            if (id[k] == '_' && id[k + 1] == '_') {
                path[w++] = '.';
                ++k;
            } else {
                path[w++] = id[k];
            }
        }
        path[w] = '\0';
        de__tm_field rec;
        memset(&rec, 0, sizeof rec);
        rec.path = tm_str(ir, path);
        if (strcmp(type, "Int") == 0 && v->kind == TMJ_NUM) {
            rec.kind = DE__TM_FIELD_INT;
            rec.number = v->u.n;
        } else if (strcmp(type, "Float") == 0 && v->kind == TMJ_NUM) {
            rec.kind = DE__TM_FIELD_FLOAT;
            rec.number = v->u.n;
        } else if (strcmp(type, "Bool") == 0 && v->kind == TMJ_BOOL) {
            rec.kind = DE__TM_FIELD_BOOL;
            rec.number = v->u.b ? 1.0 : 0.0;
        } else if (v->kind == TMJ_STR &&
                   (strcmp(type, "String") == 0 || strcmp(type, "Multilines") == 0 ||
                    strcmp(type, "Color") == 0 || strcmp(type, "FilePath") == 0 ||
                    strncmp(type, "LocalEnum.", 10) == 0 || strncmp(type, "Enum(", 5) == 0)) {
            rec.kind = DE__TM_FIELD_STRING;
            rec.string = tm_str(ir, v->u.s);
        } else {
            tm_warn(ir, "tilemap: LDtk entity \"%s\": field \"%s\" of type \"%s\" skipped", entity,
                    id, type);
            continue;
        }
        if (tm_vec_push(ir->ar, &ir->fields, &rec) == NULL)
            return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (fields)");
    }
    s->field_count = ir->fields.count - s->field_first;
    return ir->error;
}

static de_result level(ld_ctx *c, const tmj_val *lv, const tmj_val *layers_src) {
    tm_ir *ir = c->ir;
    const char *lname = tmj_str(lv, "identifier", "");
    const f64 wx = tmj_num(lv, "worldX", 0), wy = tmj_num(lv, "worldY", 0);
    const tmj_val *layers = tmj_arr(layers_src, "layerInstances");
    if (layers == NULL)
        return DE_OK; /* a level without layers (LDtk writes null for unsaved external ones) */
    for (u32 i = layers->count; i-- > 0;) {
        const tmj_val *li = &layers->u.items[i];
        const char *lid = tmj_str(li, "__identifier", "");
        const char *type = tmj_str(li, "__type", "");
        char name[256];
        snprintf(name, sizeof name, "%s/%s", lname, lid);
        u32 cw, ch, grid;
        if (!ld_int(tmj_num(li, "__cWid", -1), DE__TM_MAX_DIM, &cw) ||
            !ld_int(tmj_num(li, "__cHei", -1), DE__TM_MAX_DIM, &ch) ||
            !ld_int(tmj_num(li, "__gridSize", -1), 4096, &grid) || grid == 0)
            return tm_fail(ir, DE_ERR_PARSE, "tilemap: LDtk layer \"%s\": bad size", name);
        const f64 ox = wx + tmj_num(li, "__pxTotalOffsetX", 0);
        const f64 oy = wy + tmj_num(li, "__pxTotalOffsetY", 0);
        if (strcmp(type, "Entities") == 0) {
            const tmj_val *ents = tmj_arr(li, "entityInstances");
            for (u32 k = 0; ents != NULL && k < ents->count; ++k) {
                const tmj_val *e = &ents->u.items[k];
                const char *eid = tmj_str(e, "__identifier", "");
                const tmj_val *px = tmj_arr(e, "px");
                if (px == NULL || px->count != 2 || px->u.items[0].kind != TMJ_NUM ||
                    px->u.items[1].kind != TMJ_NUM)
                    return tm_fail(ir, DE_ERR_PARSE, "tilemap: LDtk entity \"%s\": bad px", eid);
                de__tm_spawn s;
                memset(&s, 0, sizeof s);
                s.prefab = tm_setting_map(ir->desc->entities, ir->desc->entity_count, eid);
                tm_dep(ir, s.prefab);
                s.name = tm_str(ir, eid);
                s.group = tm_str(ir, name);
                s.u = (f32)((ox + px->u.items[0].u.n) / ir->ppu);
                s.v = (f32)(-(oy + px->u.items[1].u.n) / ir->ppu);
                s.w = (f32)(tmj_num(e, "width", 0) / ir->ppu);
                s.h = (f32)(tmj_num(e, "height", 0) / ir->ppu);
                if (!de__tm_finite(s.u) || !de__tm_finite(s.v) || !de__tm_finite(s.w) ||
                    !de__tm_finite(s.h))
                    return tm_fail(ir, DE_ERR_PARSE, "tilemap: LDtk entity \"%s\": bad position",
                                   eid);
                DE_TRY(entity_fields(ir, &s, tmj_arr(e, "fieldInstances"), eid));
                if (tm_vec_push(ir->ar, &ir->spawns, &s) == NULL)
                    return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory (spawns)");
            }
            continue;
        }
        bool intgrid = strcmp(type, "IntGrid") == 0;
        if (!intgrid && strcmp(type, "Tiles") != 0 && strcmp(type, "AutoLayer") != 0) {
            tm_warn(ir, "tilemap: LDtk layer \"%s\" of type \"%s\" skipped", name, type);
            continue;
        }
        tm_ir_layer *l = tm_layer_add(ir, name, cw, ch, (f32)(grid / ir->ppu),
                                      (f32)(grid / ir->ppu), (f32)(ox / ir->ppu),
                                      (f32)(-oy / ir->ppu));
        if (l == NULL)
            return ir->error;
        l->opacity = (f32)tmj_num(li, "__opacity", 1);
        if (tm_solid_layer(ir, lid) || tm_solid_layer(ir, name))
            l->flags |= DE__TM_LAYER_COLLIDE;
        if (intgrid) {
            l->flags |= DE__TM_LAYER_INTGRID;
            const tmj_val *csv = tmj_arr(li, "intGridCsv");
            if (csv == NULL || csv->count != (u64)cw * ch)
                return tm_fail(ir, DE_ERR_PARSE,
                               "tilemap: LDtk layer \"%s\": intGridCsv must have %u values", name,
                               cw * ch);
            for (u32 k = 0; k < csv->count; ++k) {
                const tmj_val *v = &csv->u.items[k];
                if (v->kind != TMJ_NUM)
                    return tm_fail(ir, DE_ERR_PARSE, "tilemap: LDtk layer \"%s\": bad IntGrid value",
                                   name);
                if (v->u.n != 0)
                    l->cells[k] |= DE__TM_CELL_SOLID;
            }
        }
        const tmj_val *uid = tmj_get(li, "__tilesetDefUid");
        const ld_tileset *ts =
            uid != NULL && uid->kind == TMJ_NUM ? find_tileset(c, uid->u.n) : NULL;
        if (ts != NULL && ts->grid != grid)
            return tm_fail(ir, DE_ERR_UNSUPPORTED,
                           "tilemap: LDtk layer \"%s\": tileset grid %u differs from layer grid %u",
                           name, ts->grid, grid);
        DE_TRY(place_tiles(c, l, name, tmj_arr(li, "gridTiles"), ts, grid));
        DE_TRY(place_tiles(c, l, name, tmj_arr(li, "autoLayerTiles"), ts, grid));
    }
    return ir->error;
}

static de_result levels(ld_ctx *c, const tmj_val *arr) {
    tm_ir *ir = c->ir;
    for (u32 i = 0; arr != NULL && i < arr->count; ++i) {
        const tmj_val *lv = &arr->u.items[i];
        const tmj_val *src = lv;
        const tmj_val *li = tmj_get(lv, "layerInstances");
        const char *ext = tmj_str(lv, "externalRelPath", NULL);
        if ((li == NULL || li->kind == TMJ_NULL) && ext != NULL) {
            const char *text = NULL;
            size_t len = 0;
            DE_TRY(tm_read(ir, ext, &text, &len));
            char err[160];
            src = tmj_parse(ir->ar, text, len, err, sizeof err);
            if (src == NULL)
                return tm_fail(ir, DE_ERR_PARSE, "tilemap: %s:%s", ext, err);
        }
        DE_TRY(level(c, lv, src));
    }
    return DE_OK;
}

de_result tm_import_ldtk(tm_ir *ir, const char *text, size_t len) {
    char err[160];
    const tmj_val *root = tmj_parse(ir->ar, text, len, err, sizeof err);
    if (root == NULL)
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: JSON %s", err);
    if (root->kind != TMJ_OBJ || tmj_get(root, "defs") == NULL)
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: not an LDtk project (no `defs`)");
    ld_ctx *c = (ld_ctx *)tm_alloc(ir->ar, sizeof(ld_ctx), 8);
    if (c == NULL)
        return tm_fail(ir, DE_ERR_OUT_OF_MEMORY, "tilemap: out of memory");
    c->ir = ir;
    c->dir = "";
    f64 grid = tmj_num(root, "defaultGridSize", 16);
    if (!(grid >= 1 && grid <= 4096))
        return tm_fail(ir, DE_ERR_PARSE, "tilemap: LDtk defaultGridSize out of range");
    ir->ppu = ir->desc->pixels_per_unit > 0 ? (f32)ir->desc->pixels_per_unit : (f32)grid;
    DE_TRY(tilesets(c, tmj_get(root, "defs")));
    const tmj_val *worlds = tmj_arr(root, "worlds");
    if (worlds != NULL && worlds->count > 0) {
        for (u32 i = 0; i < worlds->count; ++i)
            DE_TRY(levels(c, tmj_arr(&worlds->u.items[i], "levels")));
        return ir->error;
    }
    return levels(c, tmj_arr(root, "levels"));
}
