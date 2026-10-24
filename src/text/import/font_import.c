#include "import_internal.h"
#include "text/msdf/msdf_wrap.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stb_truetype.h>
static u32 be16(const u8 *p) {
    return (u32)p[0] << 8 | p[1];
}
static u32 be32(const u8 *p) {
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
/* stb expects a trusted sfnt; reject truncated directories/tables before it sees them. */
static bool sfnt(const u8 *p, u64 n) {
    if (n < 12)
        return false;
    u32 count = be16(p + 4);
    if (count > 256 || 12 + (u64)count * 16 > n)
        return false;
    for (u32 i = 0; i < count; ++i) {
        const u8 *t = p + 12 + 16 * i;
        u32 o = be32(t + 8), z = be32(t + 12);
        if (o > n || z > n - o)
            return false;
    }
    return true;
}
static u32 utf8(const char **p) {
    const u8 *s = (const u8 *)*p;
    u32 c = *s++, k = 0;
    if (c >= 0xf0) {
        c &= 7;
        k = 3;
    } else if (c >= 0xe0) {
        c &= 15;
        k = 2;
    } else if (c >= 0xc0) {
        c &= 31;
        k = 1;
    }
    for (u32 i = 0; i < k; ++i) {
        if ((*s & 0xc0) != 0x80)
            return UINT32_MAX;
        c = (c << 6) | (*s++ & 63);
    }
    *p = (const char *)s;
    return c;
}
de_result de_font_import(de_asset_import_ctx *c) {
    const char *mode = de_asset_import_setting_string(c, "mode", "msdf");
    if (!strcmp(mode, "bitmap"))
        return de__font_bitmap(c, false);
    if (!strcmp(mode, "pixel"))
        return de__font_bitmap(c, true);
    if (strcmp(mode, "msdf"))
        return de_asset_import_fail(c, DE_ERR_INVALID_ARGUMENT, "font mode %s", mode);
    u64 n;
    const u8 *src = de_asset_import_source(c, &n);
    if (n > UINT32_MAX || !sfnt(src, n))
        return de_asset_import_fail(c, DE_ERR_PARSE, "font offset 0: invalid SFNT tables");
    stbtt_fontinfo f;
    if (!stbtt_InitFont(&f, src, 0))
        return de_asset_import_fail(c, DE_ERR_PARSE, "unsupported TrueType/CFF font");
    const de_allocator *a = de_asset_import_allocator(c);
    u8 *bits = DE_ALLOC(a, 0x110000 / 8, 1, DE_MEM_ASSET);
    if (!bits)
        return DE_ERR_OUT_OF_MEMORY;
    memset(bits, 0, 0x110000 / 8);
    const char *const *sets;
    u32 set_count = de_asset_import_setting_string_array(c, "charset", &sets);
    if (set_count) {
        for (u32 i = 0; i < set_count; ++i)
            DE_TRY(de__font_charset(sets[i], bits, 0x110000 / 8));
    } else
        DE_TRY(de__font_charset(de_asset_import_setting_string(c, "charset", "ascii"), bits,
                                0x110000 / 8));
    const char *const *ranges;
    u32 nr = de_asset_import_setting_string_array(c, "ranges", &ranges);
    for (u32 i = 0; i < nr; ++i) {
        const char *p = ranges[i];
        char *end;
        errno = 0;
        unsigned long lo = strtoul(p, &end, 0);
        if (end == p || *end != '-' || errno)
            return de_asset_import_fail(c, DE_ERR_INVALID_ARGUMENT,
                                        "font range must be start-end (decimal or 0x hex)");
        p = end + 1;
        unsigned long hi = strtoul(p, &end, 0);
        if (end == p || *end || errno || lo > hi || hi > 0x10ffff)
            return de_asset_import_fail(c, DE_ERR_INVALID_ARGUMENT, "invalid font codepoint range");
        for (u32 cp = (u32)lo; cp <= (u32)hi; ++cp)
            if (cp < 0xd800 || cp > 0xdfff)
                bits[cp >> 3] |= (u8)(1u << (cp & 7));
    }
    const de_asset_setting_value *numeric = de_asset_import_setting_value(c, "ranges");
    if (numeric && numeric->type == DE_ASSET_SETTING_ARRAY) {
        for (u32 i = 0; i < numeric->u.values.count; ++i) {
            const de_asset_setting_value *pair = &numeric->u.values.items[i];
            if (pair->type != DE_ASSET_SETTING_ARRAY || pair->u.values.count != 2 ||
                pair->u.values.items[0].type != DE_ASSET_SETTING_INT ||
                pair->u.values.items[1].type != DE_ASSET_SETTING_INT)
                return de_asset_import_fail(c, DE_ERR_INVALID_ARGUMENT,
                                            "font ranges[%u] must contain two integer codepoints",
                                            i);
            i64 lo = pair->u.values.items[0].u.i, hi = pair->u.values.items[1].u.i;
            if (lo < 0 || lo > hi || hi > 0x10ffff)
                return de_asset_import_fail(c, DE_ERR_INVALID_ARGUMENT,
                                            "invalid font codepoint range at ranges[%u]", i);
            for (u32 cp = (u32)lo; cp <= (u32)hi; ++cp)
                if (cp < 0xd800 || cp > 0xdfff)
                    bits[cp >> 3] |= (u8)(1u << (cp & 7));
        }
    } else if (numeric && numeric->type != DE_ASSET_SETTING_STRING_ARRAY)
        return de_asset_import_fail(
            c, DE_ERR_INVALID_ARGUMENT,
            "font ranges must be an array of string ranges or integer pairs");
    const char *chars = de_asset_import_setting_string(c, "chars", "");
    while (*chars) {
        u32 cp = utf8(&chars);
        if (cp > 0x10ffff)
            return de_asset_import_fail(c, DE_ERR_PARSE, "font chars: malformed UTF-8");
        bits[cp >> 3] |= (u8)(1u << (cp & 7));
    }
    de__font_build b = {.mode = DE_FONT_MSDF, .channels = 4, .source = src, .source_size = (u32)n};
    i64 em = de_asset_import_setting_int(c, "em_px", 48),
        range = de_asset_import_setting_int(c, "px_range", 6),
        page = de_asset_import_setting_int(c, "page_size", 1024);
    if (em < 8 || em > 512 || range < 1 || range > 64 || page < 32 || page > 4096)
        return de_asset_import_fail(c, DE_ERR_INVALID_ARGUMENT, "font atlas settings out of range");
    b.em = (f32)em;
    b.range = (f32)range;
    b.width = b.height = (u32)page;
    b.glyphs =
        DE_ALLOC(a, (u64)f.numGlyphs * sizeof *b.glyphs, _Alignof(de__font_glyph), DE_MEM_ASSET);
    b.cmap = DE_ALLOC(a, 0x110000ULL * sizeof *b.cmap, _Alignof(de__font_cmap), DE_MEM_ASSET);
    u8 *used = DE_ALLOC(a, (u64)f.numGlyphs, 1, DE_MEM_ASSET);
    if (!b.glyphs || !b.cmap || !used)
        return DE_ERR_OUT_OF_MEMORY;
    memset(used, 0, (size_t)f.numGlyphs);
    for (u32 cp = 0; cp < 0x110000; ++cp)
        if (bits[cp >> 3] & (1u << (cp & 7))) {
            int g = stbtt_FindGlyphIndex(&f, (int)cp);
            if (g) {
                used[g] = 1;
                b.cmap[b.cmap_count++] = (de__font_cmap){cp, (u32)g};
            }
        }
    used[0] = 1;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f, &asc, &desc, &gap);
    u32 upem = be16(src + f.head + 18);
    if (!upem)
        return DE_ERR_PARSE;
    f32 inv = 1.0f / (f32)upem;
    b.ascent = (f32)asc * inv;
    b.descent = (f32)desc * inv;
    b.gap = (f32)gap * inv;
    double scale = (double)em / upem;
    for (int g = 0; g < f.numGlyphs; ++g)
        if (used[g]) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0, adv, lsb;
            stbtt_GetGlyphHMetrics(&f, g, &adv, &lsb);
            stbtt_GetGlyphBox(&f, g, &x0, &y0, &x1, &y1);
            de__font_glyph *v = &b.glyphs[b.count++];
            *v = (de__font_glyph){
                .index = (u32)g, .advance = (f32)adv * inv, .bearing = (f32)lsb * inv};
            if (x0 != x1 && y0 != y1) {
                int lx = (int)floor(x0 * scale) - (int)range,
                    by = (int)floor(y0 * scale) - (int)range,
                    rx = (int)ceil(x1 * scale) + (int)range,
                    ty = (int)ceil(y1 * scale) + (int)range;
                v->w = (u32)(rx - lx);
                v->h = (u32)(ty - by);
                v->left = (f32)lx / b.em;
                v->bottom = (f32)by / b.em;
                v->right = (f32)rx / b.em;
                v->top = (f32)ty / b.em;
            }
        }
    DE_TRY(de__font_pack(c, &b));
    for (u32 i = 0; i < b.count; ++i) {
        de__font_glyph *g = &b.glyphs[i];
        if (!g->w || !g->h)
            continue;
        stbtt_vertex *v = NULL;
        int count = stbtt_GetGlyphShape(&f, (int)g->index, &v);
        if (count <= 0)
            continue;
        de__msdf_vertex *edges =
            DE_ALLOC(a, (u64)count * sizeof *edges, _Alignof(de__msdf_vertex), DE_MEM_ASSET);
        float *scratch =
            DE_ALLOC(a, (u64)g->w * g->h * 4 * sizeof(float), _Alignof(float), DE_MEM_ASSET);
        u8 *rgba = DE_ALLOC(a, (u64)g->w * g->h * 4, 1, DE_MEM_ASSET);
        if (!edges || !scratch || !rgba) {
            stbtt_FreeShape(&f, v);
            return DE_ERR_OUT_OF_MEMORY;
        }
        for (int j = 0; j < count; ++j)
            edges[j] =
                (de__msdf_vertex){v[j].type, v[j].x, v[j].y, v[j].cx, v[j].cy, v[j].cx1, v[j].cy1};
        stbtt_FreeShape(&f, v);
        de_result r = de__msdf_generate(edges, (u32)count, scale, -g->left * upem,
                                        -g->bottom * upem, b.range, g->w, g->h, scratch, rgba);
        if (r != DE_OK)
            return de_asset_import_fail(c, r, "MSDF glyph %u", g->index);
        for (u32 y = 0; y < g->h; ++y)
            memcpy(b.pixels[g->page] + 4 * ((u64)(g->y + y) * b.width + g->x),
                   rgba + 4 * (u64)y * g->w, 4 * g->w);
        DE_FREE(a, edges, (u64)count * sizeof *edges);
        DE_FREE(a, scratch, (u64)g->w * g->h * 4 * sizeof(float));
        DE_FREE(a, rgba, (u64)g->w * g->h * 4);
    }
    return de__font_write(c, &b);
}
de_asset_type_desc de_font_import_type_desc(void) {
    static const char *const exts[] = {".ttf", ".otf", ".fnt", ".png"};
    return (de_asset_type_desc){.name = "font",
                                .extensions = exts,
                                .extension_count = 4,
                                .importer_version = 3,
                                .import = de_font_import};
}
