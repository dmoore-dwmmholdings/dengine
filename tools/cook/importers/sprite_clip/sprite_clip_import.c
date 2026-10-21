#include "sprite_clip_import.h"
#include "anim/sprite_clip_format.h"
#include "core/de_toml_internal.h"
#include <float.h>
#include <stdlib.h>
#include <string.h>
static de_result invalid(de_asset_import_ctx *c, toml_datum_t d, const char *message) {
    return de_asset_import_fail(c, DE_ERR_PARSE, "%s:%d: %s", de_asset_import_source_path(c),
                                d.lineno > 0 ? d.lineno : 1, message);
}
static bool integer(toml_datum_t d, u32 *out, bool positive) {
    if (d.type != TOML_INT64 || d.u.int64 < (positive ? 1 : 0) || d.u.int64 > UINT32_MAX)
        return false;
    *out = (u32)d.u.int64;
    return true;
}
static bool numbers(toml_datum_t d, f64 *out, u32 count) {
    if (d.type != TOML_ARRAY || d.u.arr.size != (i32)count)
        return false;
    for (u32 i = 0; i < count; ++i) {
        toml_datum_t n = d.u.arr.elem[i];
        if (n.type != TOML_INT64 && n.type != TOML_FP64)
            return false;
        out[i] = n.type == TOML_INT64 ? (f64)n.u.int64 : n.u.fp64;
        if (!(out[i] >= -FLT_MAX && out[i] <= FLT_MAX))
            return false;
    }
    return true;
}
static bool string(toml_datum_t d) {
    return d.type == TOML_STRING && d.u.str.len > 0 && strlen(d.u.s) == (size_t)d.u.str.len;
}
static int tag_order(const void *a, const void *b) {
    u64 x = ((const de__sprite_tag *)a)->name, y = ((const de__sprite_tag *)b)->name;
    return x < y ? -1 : x > y;
}
static int event_order(const void *a, const void *b) {
    const de__sprite_event *x = a, *y = b;
    if (x->frame != y->frame)
        return x->frame < y->frame ? -1 : 1;
    return x->name < y->name ? -1 : x->name > y->name;
}
static de_result build(de_asset_import_ctx *c, toml_datum_t root) {
    const de_allocator *a = de_asset_import_allocator(c);
    toml_datum_t atlas = toml_get(root, "atlas"), grid = toml_get(root, "grid"),
                 explicit = toml_get(root, "frames"), extent = toml_get(root, "atlas_size"),
                 tags = toml_get(root, "tags"), events = toml_get(root, "events"),
                 duration = toml_get(root, "default_ms");
    if (!string(atlas))
        return invalid(c, atlas, "atlas must name a texture asset");
    de_asset_id atlas_id = de_asset_id_from(atlas.u.s);
    if (!atlas_id.hash)
        return invalid(c, atlas, "invalid atlas asset ID");
    u32 ms = 100, columns = 0, rows = 0, count = 0, width = 0, height = 0;
    f64 cell[2] = {0}, size[2] = {0};
    if (duration.type != TOML_UNKNOWN && !integer(duration, &ms, true))
        return invalid(c, duration, "default_ms must be a positive integer");
    if (grid.type != TOML_UNKNOWN) {
        if (grid.type != TOML_TABLE || !integer(toml_get(grid, "columns"), &columns, true) ||
            !integer(toml_get(grid, "rows"), &rows, true) ||
            !numbers(toml_get(grid, "frame_size"), cell, 2) || !(cell[0] > 0 && cell[1] > 0))
            return invalid(c, grid, "grid needs positive columns, rows and frame_size");
        u64 cells = (u64)columns * rows;
        toml_datum_t nc = toml_get(grid, "count");
        if (nc.type == TOML_UNKNOWN) {
            if (cells > DE__SPRITE_FRAME_LIMIT)
                return invalid(c, grid, "grid frame count exceeds limit");
            count = (u32)cells;
        } else if (!integer(nc, &count, true) || count > cells)
            return invalid(c, nc, "grid count must fit the sheet");
        size[0] = cell[0] * columns;
        size[1] = cell[1] * rows;
    }
    if (extent.type != TOML_UNKNOWN && !numbers(extent, size, 2))
        return invalid(c, extent, "atlas_size must contain width and height");
    if (!(size[0] >= 1 && size[1] >= 1 && size[0] <= UINT32_MAX && size[1] <= UINT32_MAX))
        return invalid(c, extent, "atlas_size is required without grid dimensions");
    width = (u32)size[0];
    height = (u32)size[1];
    if (size[0] != width || size[1] != height)
        return invalid(c, extent, "atlas dimensions must be integers");
    if (grid.type != TOML_UNKNOWN && (cell[0] * columns > width || cell[1] * rows > height))
        return invalid(c, grid, "grid exceeds atlas_size");
    if (explicit.type != TOML_UNKNOWN) {
        if (explicit.type != TOML_ARRAY || explicit.u.arr.size <= 0 ||
            explicit.u.arr.size > (i32)DE__SPRITE_FRAME_LIMIT)
            return invalid(c, explicit, "frames must be a nonempty bounded array");
        count = (u32) explicit.u.arr.size;
    }
    if (!count || count > DE__SPRITE_FRAME_LIMIT)
        return invalid(c, grid, "provide frames or a bounded grid");
    if ((tags.type != TOML_UNKNOWN &&
         (tags.type != TOML_ARRAY || tags.u.arr.size > (i32)DE__SPRITE_TAG_LIMIT)) ||
        (events.type != TOML_UNKNOWN &&
         (events.type != TOML_ARRAY || events.u.arr.size > (i32)DE__SPRITE_EVENT_LIMIT)))
        return invalid(c, root, "tags/events must be bounded arrays");
    u32 nt = tags.type == TOML_ARRAY ? (u32)tags.u.arr.size : 0,
        ne = events.type == TOML_ARRAY ? (u32)events.u.arr.size : 0;
    de__sprite_frame *frames =
        DE_ALLOC(a, (size_t)count * sizeof *frames, DE_ALIGNOF(de__sprite_frame), DE_MEM_ASSET);
    de__sprite_tag *tt =
        nt ? DE_ALLOC(a, (size_t)nt * sizeof *tt, DE_ALIGNOF(de__sprite_tag), DE_MEM_ASSET) : NULL;
    de__sprite_event *ee =
        ne ? DE_ALLOC(a, (size_t)ne * sizeof *ee, DE_ALIGNOF(de__sprite_event), DE_MEM_ASSET)
           : NULL;
    if (!frames || (nt && !tt) || (ne && !ee))
        return DE_ERR_OUT_OF_MEMORY;
    u64 total = 0;
    for (u32 i = 0; i < count; ++i) {
        f64 rect[4] = {(i % (columns ? columns : 1)) * cell[0],
                       (i / (columns ? columns : 1)) * cell[1], cell[0], cell[1]},
            pivot[2] = {.5, .5};
        u32 frame_ms = ms;
        toml_datum_t frame = explicit.type == TOML_ARRAY ? explicit.u.arr.elem[i] : grid;
        if (explicit.type == TOML_ARRAY) {
            if (frame.type != TOML_TABLE || !numbers(toml_get(frame, "rect"), rect, 4))
                return invalid(c, frame, "frame needs rect=[x,y,width,height]");
            toml_datum_t fm = toml_get(frame, "ms"), fp = toml_get(frame, "pivot");
            if (fm.type != TOML_UNKNOWN && !integer(fm, &frame_ms, true))
                return invalid(c, fm, "frame ms must be a positive integer");
            if (fp.type != TOML_UNKNOWN && !numbers(fp, pivot, 2))
                return invalid(c, fp, "pivot must contain two finite numbers");
        }
        if (!(rect[0] >= 0 && rect[1] >= 0 && rect[2] > 0 && rect[3] > 0 &&
              rect[0] + rect[2] <= width && rect[1] + rect[3] <= height))
            return invalid(c, frame, "frame rectangle exceeds atlas bounds");
        frames[i] = (de__sprite_frame){.rect = {(f32)(rect[0] / width), (f32)(rect[1] / height),
                                                (f32)(rect[2] / width), (f32)(rect[3] / height)},
                                       .pivot = {(f32)pivot[0], (f32)pivot[1]},
                                       .ms = frame_ms};
        total += frame_ms;
    }
    for (u32 i = 0; i < nt; ++i) {
        toml_datum_t t = tags.u.arr.elem[i], name = toml_get(t, "name"),
                     direction = toml_get(t, "direction");
        if (t.type != TOML_TABLE || !string(name) ||
            !integer(toml_get(t, "from"), &tt[i].from, false) ||
            !integer(toml_get(t, "to"), &tt[i].to, false) || tt[i].from > tt[i].to ||
            tt[i].to >= count)
            return invalid(c, t, "tag needs a name and valid inclusive frame range");
        tt[i].name = de_hash64_str(name.u.s);
        tt[i].direction = 0;
        if (direction.type != TOML_UNKNOWN) {
            if (!string(direction))
                return invalid(c, direction, "invalid tag direction");
            if (!strcmp(direction.u.s, "reverse"))
                tt[i].direction = 1;
            else if (!strcmp(direction.u.s, "pingpong"))
                tt[i].direction = 2;
            else if (strcmp(direction.u.s, "forward"))
                return invalid(c, direction, "unknown tag direction");
        }
    }
    for (u32 i = 0; i < ne; ++i) {
        toml_datum_t e = events.u.arr.elem[i], name = toml_get(e, "name");
        if (e.type != TOML_TABLE || !string(name) ||
            !integer(toml_get(e, "frame"), &ee[i].frame, false) || ee[i].frame >= count)
            return invalid(c, e, "event needs a name and valid frame");
        ee[i].name = de_hash64_str(name.u.s);
    }
    if (nt)
        qsort(tt, nt, sizeof *tt, tag_order);
    if (ne)
        qsort(ee, ne, sizeof *ee, event_order);
    for (u32 i = 0; i < nt; ++i)
        if (!tt[i].name || (i && tt[i].name == tt[i - 1].name))
            return invalid(c, tags, "duplicate tag name/hash");
    for (u32 i = 0; i < ne; ++i)
        if (!ee[i].name || (i && ee[i].frame == ee[i - 1].frame && ee[i].name == ee[i - 1].name))
            return invalid(c, events, "duplicate frame event/name");
    u32 ot = 64 + count * 32, oe = ot + nt * 24, bytes = oe + ne * 16;
    u8 *out = DE_ALLOC(a, bytes, 8, DE_MEM_ASSET);
    if (!out)
        return DE_ERR_OUT_OF_MEMORY;
    memset(out, 0, bytes);
    memcpy(out, "DSCP", 4);
    de_cooked_put_u32(out + 4, DE__SPRITE_CLIP_VERSION);
    de_cooked_put_u32(out + 8, bytes);
    de_cooked_put_u32(out + 12, count);
    de_cooked_put_u32(out + 16, nt);
    de_cooked_put_u32(out + 20, ne);
    de_cooked_put_u32(out + 24, 64);
    de_cooked_put_u32(out + 28, ot);
    de_cooked_put_u32(out + 32, oe);
    de_cooked_put_u32(out + 36, width);
    de_cooked_put_u32(out + 40, height);
    de_cooked_put_u64(out + 48, atlas_id.hash);
    de_cooked_put_u64(out + 56, total);
    for (u32 i = 0; i < count; ++i) {
        u8 *p = out + 64 + i * 32;
        for (u32 k = 0; k < 4; ++k)
            de_cooked_put_f32(p + k * 4, frames[i].rect[k]);
        de_cooked_put_u32(p + 16, frames[i].ms);
        de_cooked_put_f32(p + 20, frames[i].pivot[0]);
        de_cooked_put_f32(p + 24, frames[i].pivot[1]);
    }
    for (u32 i = 0; i < nt; ++i) {
        u8 *p = out + ot + i * 24;
        de_cooked_put_u64(p, tt[i].name);
        de_cooked_put_u32(p + 8, tt[i].from);
        de_cooked_put_u32(p + 12, tt[i].to);
        de_cooked_put_u32(p + 16, tt[i].direction);
    }
    for (u32 i = 0; i < ne; ++i) {
        u8 *p = out + oe + i * 16;
        de_cooked_put_u32(p, ee[i].frame);
        de_cooked_put_u64(p + 8, ee[i].name);
    }
    de__sprite_clip_view checked;
    if (!de__sprite_clip_parse(out, bytes, &checked, NULL))
        return invalid(c, root, "invalid normalized clip");
    DE_TRY(de_asset_import_add_dependency(c, atlas_id));
    return de_asset_import_write(c, out, bytes);
}
de_result de_sprite_clip_import(de_asset_import_ctx *c) {
    u64 size;
    const char *source = de_asset_import_source(c, &size);
    if (size > 1024 * 1024)
        return de_asset_import_fail(c, DE_ERR_PARSE, "%s:1: sprite clip source exceeds 1MiB",
                                    de_asset_import_source_path(c));
    toml_result_t parsed = de__toml_parse(de_asset_import_allocator(c), source, (int)size);
    de_result r = parsed.ok ? build(c, parsed.toptab)
                            : de_asset_import_fail(c, DE_ERR_PARSE, "%s: %s",
                                                   de_asset_import_source_path(c), parsed.errmsg);
    de__toml_free(parsed);
    return r;
}
de_asset_type_desc de_sprite_clip_import_type_desc(void) {
    static const char *extensions[] = {".sprite.toml"};
    return (de_asset_type_desc){.name = "sprite_clip",
                                .extensions = extensions,
                                .extension_count = 1,
                                .importer_version = 1,
                                .import = de_sprite_clip_import};
}
