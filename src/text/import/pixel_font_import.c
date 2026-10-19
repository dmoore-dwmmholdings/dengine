#include "import_internal.h"
#include "core/de_toml_internal.h"
#include <stb_image.h>
#include <string.h>
static u32 nextcp(const char **p) {
    const u8 *s = (const u8 *)*p;
    u32 c = *s++, n = 0;
    if (c >= 240) {
        c &= 7;
        n = 3;
    } else if (c >= 224) {
        c &= 15;
        n = 2;
    } else if (c >= 192) {
        c &= 31;
        n = 1;
    }
    for (u32 i = 0; i < n; ++i) {
        if ((*s & 192) != 128)
            return UINT32_MAX;
        c = c << 6 | (*s++ & 63);
    }
    *p = (const char *)s;
    return c;
}
de_result de__font_bitmap(de_asset_import_ctx *c, bool pixel) {
    const de_allocator *a = de_asset_import_allocator(c);
    u64 size;
    const void *src = de_asset_import_source(c, &size);
    de__font_build b = {.mode = pixel ? DE_FONT_PIXEL : DE_FONT_BITMAP, .channels = 4};
    if (!pixel) {
        de__bmfont f;
        DE_TRY(de__bmfont_parse(src, size, a, &f));
        b.count = b.cmap_count = f.count;
        b.kern_count = f.kern_count;
        b.glyphs = f.glyphs;
        b.cmap = f.cmap;
        b.kerns = f.kerns;
        b.pages = f.pages;
        b.width = f.width;
        b.height = f.height;
        b.em = f.size;
        b.ascent = f.ascent;
        b.descent = f.ascent - f.line_height;
        b.pixels = DE_ALLOC(a, (u64)b.pages * sizeof *b.pixels, _Alignof(u8 *), DE_MEM_ASSET);
        if (!b.pixels)
            return DE_ERR_OUT_OF_MEMORY;
        for (u32 i = 0; i < b.pages; ++i) {
            const void *d;
            u64 n;
            DE_TRY(de_asset_import_read_file(c, f.files[i], &d, &n));
            if (n > INT32_MAX)
                return DE_ERR_UNSUPPORTED;
            int w, h, k;
            u8 *rgba = stbi_load_from_memory(d, (int)n, &w, &h, &k, 4);
            if (!rgba)
                return de_asset_import_fail(c, DE_ERR_PARSE, "BMFont page %s: %s", f.files[i],
                                            stbi_failure_reason());
            if ((u32)w != b.width || (u32)h != b.height) {
                stbi_image_free(rgba);
                return de_asset_import_fail(c, DE_ERR_PARSE, "BMFont page dimensions mismatch");
            }
            u64 z = (u64)w * (u64)h * 4;
            b.pixels[i] = DE_ALLOC(a, z, 1, DE_MEM_ASSET);
            if (!b.pixels[i]) {
                stbi_image_free(rgba);
                return DE_ERR_OUT_OF_MEMORY;
            }
            memcpy(b.pixels[i], rgba, (size_t)z);
            stbi_image_free(rgba);
        }
    } else {
        const char *map = de_asset_import_setting_string(c, "map", NULL);
        const void *d;
        u64 n;
        if (!map)
            return de_asset_import_fail(c, DE_ERR_INVALID_ARGUMENT, "pixel font requires map TOML");
        DE_TRY(de_asset_import_read_file(c, map, &d, &n));
        if (n > INT32_MAX)
            return DE_ERR_UNSUPPORTED;
        toml_result_t tr = de__toml_parse(a, d, (int)n);
        if (!tr.ok) {
            de__toml_free(tr);
            return de_asset_import_fail(c, DE_ERR_PARSE, "pixel char map TOML");
        }
        toml_datum_t cell = toml_get(tr.toptab, "cell"), chars = toml_get(tr.toptab, "chars"),
                     widths = toml_get(tr.toptab, "widths");
        if (cell.type != TOML_ARRAY || cell.u.arr.size != 2 ||
            cell.u.arr.elem[0].type != TOML_INT64 || cell.u.arr.elem[1].type != TOML_INT64 ||
            chars.type != TOML_STRING || cell.u.arr.elem[0].u.int64 <= 0 ||
            cell.u.arr.elem[1].u.int64 <= 0 || cell.u.arr.elem[0].u.int64 > 4096 ||
            cell.u.arr.elem[1].u.int64 > 4096) {
            de__toml_free(tr);
            return de_asset_import_fail(c, DE_ERR_PARSE, "pixel map cell/chars");
        }
        u32 cw = (u32)cell.u.arr.elem[0].u.int64, ch = (u32)cell.u.arr.elem[1].u.int64;
        int w, h, k;
        if (size > INT32_MAX) {
            de__toml_free(tr);
            return DE_ERR_UNSUPPORTED;
        }
        u8 *rgba = stbi_load_from_memory(src, (int)size, &w, &h, &k, 4);
        if (!rgba) {
            de__toml_free(tr);
            return de_asset_import_fail(c, DE_ERR_PARSE, "pixel atlas image");
        }
        b.width = (u32)w;
        b.height = (u32)h;
        b.pages = 1;
        b.em = (f32)ch;
        b.ascent = (f32)ch;
        b.pixels = DE_ALLOC(a, sizeof(u8 *), _Alignof(u8 *), DE_MEM_ASSET);
        u64 bytes = (u64)w * (u64)h * 4;
        if (!b.pixels) {
            stbi_image_free(rgba);
            de__toml_free(tr);
            return DE_ERR_OUT_OF_MEMORY;
        }
        b.pixels[0] = DE_ALLOC(a, bytes, 1, DE_MEM_ASSET);
        if (!b.pixels[0]) {
            stbi_image_free(rgba);
            de__toml_free(tr);
            return DE_ERR_OUT_OF_MEMORY;
        }
        memcpy(b.pixels[0], rgba, (size_t)bytes);
        stbi_image_free(rgba);
        u32 count = (u32)chars.u.str.len;
        b.glyphs =
            DE_ALLOC(a, (u64)count * sizeof *b.glyphs, _Alignof(de__font_glyph), DE_MEM_ASSET);
        b.cmap = DE_ALLOC(a, (u64)count * sizeof *b.cmap, _Alignof(de__font_cmap), DE_MEM_ASSET);
        if (!b.glyphs || !b.cmap) {
            de__toml_free(tr);
            return DE_ERR_OUT_OF_MEMORY;
        }
        const char *p = chars.u.s;
        u32 cols = b.width / cw;
        if (!cols || b.width % cw || b.height % ch) {
            de__toml_free(tr);
            return DE_ERR_PARSE;
        }
        while (*p) {
            u32 cp = nextcp(&p), i = b.count++, advance = cw;
            if (cp > 0x10ffff || i >= (u64)cols * (b.height / ch)) {
                de__toml_free(tr);
                return de_asset_import_fail(c, DE_ERR_PARSE,
                                            "pixel map exceeds atlas or malformed UTF-8");
            }
            if (widths.type == TOML_ARRAY && (int)i < widths.u.arr.size) {
                toml_datum_t v = widths.u.arr.elem[i];
                if (v.type != TOML_INT64 || v.u.int64 < 0 || v.u.int64 > cw) {
                    de__toml_free(tr);
                    return DE_ERR_PARSE;
                }
                advance = (u32)v.u.int64;
            }
            b.glyphs[i] = (de__font_glyph){.index = cp,
                                           .x = i % cols * cw,
                                           .y = i / cols * ch,
                                           .w = cw,
                                           .h = ch,
                                           .right = (f32)cw,
                                           .top = (f32)ch,
                                           .advance = (f32)advance};
            b.cmap[i] = (de__font_cmap){cp, cp};
        }
        b.cmap_count = b.count;
        de__toml_free(tr);
        for (u32 i = 1; i < b.count; ++i) {
            de__font_glyph g = b.glyphs[i];
            de__font_cmap m = b.cmap[i];
            u32 j = i;
            while (j && b.glyphs[j - 1].index > g.index) {
                b.glyphs[j] = b.glyphs[j - 1];
                b.cmap[j] = b.cmap[j - 1];
                --j;
            }
            if (j && b.glyphs[j - 1].index == g.index)
                return de_asset_import_fail(c, DE_ERR_PARSE, "duplicate pixel character");
            b.glyphs[j] = g;
            b.cmap[j] = m;
        }
    }
    return de__font_write(c, &b);
}
