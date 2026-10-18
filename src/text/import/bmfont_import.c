#include "font_import.h"
#include <dengine/de_memory.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
static u32 read16(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8;
}
static int signed16(const u8 *p) {
    u32 v = read16(p);
    return v < 32768 ? (int)v : (int)v - 65536;
}
static bool field(const char *l, const char *k, char *out, u32 cap) {
    size_t n = strlen(k);
    const char *p = l;
    while ((p = strstr(p, k)) != NULL) {
        if ((p == l || p[-1] == ' ' || p[-1] == '\t') && p[n] == '=') {
            p += n + 1;
            bool q = *p == '"';
            if (q)
                ++p;
            u32 i = 0;
            while (*p && (q ? *p != '"' : *p != ' ' && *p != '\t' && *p != '\r')) {
                if (i + 1 >= cap)
                    return false;
                out[i++] = *p++;
            }
            out[i] = 0;
            return !q || *p == '"';
        }
        p += n;
    }
    return false;
}
static bool number(const char *l, const char *k, int *out) {
    char b[64], *e;
    if (!field(l, k, b, sizeof b))
        return false;
    errno = 0;
    long n = strtol(b, &e, 10);
    if (errno || *e || n < INT_MIN || n > INT_MAX)
        return false;
    *out = (int)n;
    return true;
}
#define NUM(k, v)                                                                                  \
    do {                                                                                           \
        if (!number(line, k, &v))                                                                  \
            goto bad;                                                                              \
    } while (0)
de_result de__bmfont_parse(const void *data, u64 size, const de_allocator *a, de__bmfont *out) {
    if (!out || (!data && size) || !a)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "BMFont arguments");
    memset(out, 0, sizeof *out);
    if (size < 4 || size > 16 * 1024 * 1024)
        return DE_FAIL(DE_ERR_PARSE, "BMFont offset 0: size");
    u32 cap = (u32)(size / 8 + 1);
    out->glyphs =
        DE_ALLOC(a, (u64)cap * sizeof *out->glyphs, _Alignof(de__font_glyph), DE_MEM_ASSET);
    out->cmap = DE_ALLOC(a, (u64)cap * sizeof *out->cmap, _Alignof(de__font_cmap), DE_MEM_ASSET);
    out->kerns = DE_ALLOC(a, (u64)cap * sizeof *out->kerns, _Alignof(de__font_kern), DE_MEM_ASSET);
    out->files = DE_ALLOC(a, 256 * 256, 1, DE_MEM_ASSET);
    if (!out->glyphs || !out->cmap || !out->kerns || !out->files)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "BMFont allocation");
    memset(out->files, 0, 256 * 256);
    const u8 *p = data;
    u64 off = 0;
    u32 lineno = 0;
    if (!memcmp(p, "BMF", 3)) {
        if (p[3] != 3)
            goto bad;
        off = 4;
        while (off < size) {
            if (size - off < 5)
                goto bad;
            u32 kind = p[off], n = de__font_u32(p + off + 1);
            off += 5;
            if (n > size - off)
                goto bad;
            const u8 *b = p + off;
            if (kind == 1) {
                if (n < 14)
                    goto bad;
                out->size = (f32)abs(signed16(b));
            } else if (kind == 2) {
                if (n != 15)
                    goto bad;
                out->line_height = (f32)read16(b);
                out->ascent = (f32)read16(b + 2);
                out->width = read16(b + 4);
                out->height = read16(b + 6);
                out->pages = read16(b + 8);
                if (out->pages > 256)
                    goto bad;
            } else if (kind == 3) {
                u32 x = 0, j = 0;
                while (x < n) {
                    if (j >= 256)
                        goto bad;
                    u32 st = x;
                    while (x < n && b[x])
                        ++x;
                    if (x == n || x - st >= 256)
                        goto bad;
                    memcpy(out->files[j++], b + st, x - st);
                    ++x;
                }
            } else if (kind == 4) {
                if (n % 20 || n / 20 > cap - out->count)
                    goto bad;
                for (u32 x = 0; x < n; x += 20) {
                    u32 i = out->count++, cp = de__font_u32(b + x);
                    de__font_glyph *g = &out->glyphs[i];
                    *g = (de__font_glyph){.index = cp,
                                          .page = b[x + 18],
                                          .x = read16(b + x + 4),
                                          .y = read16(b + x + 6),
                                          .w = read16(b + x + 8),
                                          .h = read16(b + x + 10),
                                          .left = (f32)signed16(b + x + 12),
                                          .top = -(f32)signed16(b + x + 14),
                                          .advance = (f32)signed16(b + x + 16)};
                    g->right = g->left + (f32)g->w;
                    g->bottom = g->top - (f32)g->h;
                    out->cmap[i] = (de__font_cmap){cp, cp};
                }
            } else if (kind == 5) {
                if (n % 10 || n / 10 > cap - out->kern_count)
                    goto bad;
                for (u32 x = 0; x < n; x += 10)
                    out->kerns[out->kern_count++] = (de__font_kern){
                        de__font_u32(b + x), de__font_u32(b + x + 4), (f32)signed16(b + x + 8)};
            }
            off += n;
        }
    } else {
        while (off < size) {
            char line[4096];
            u32 len = 0;
            ++lineno;
            while (off < size && p[off] != '\n') {
                if (len + 1 >= sizeof line)
                    goto bad;
                line[len++] = (char)p[off++];
            }
            if (off < size)
                ++off;
            line[len] = 0;
            int v = 0;
            if (!strncmp(line, "info ", 5)) {
                NUM("size", v);
                out->size = v < 0 ? -(f32)v : (f32)v;
            } else if (!strncmp(line, "common ", 7)) {
                NUM("lineHeight", v);
                out->line_height = (f32)v;
                NUM("base", v);
                out->ascent = (f32)v;
                NUM("scaleW", v);
                out->width = (u32)v;
                NUM("scaleH", v);
                out->height = (u32)v;
                NUM("pages", v);
                out->pages = (u32)v;
                if (out->pages > 256)
                    goto bad;
            } else if (!strncmp(line, "page ", 5)) {
                NUM("id", v);
                if (v < 0 || v >= 256 || !field(line, "file", out->files[v], 256))
                    goto bad;
            } else if (!strncmp(line, "char ", 5)) {
                if (out->count >= cap)
                    goto bad;
                u32 i = out->count++;
                de__font_glyph *g = &out->glyphs[i];
                memset(g, 0, sizeof *g);
                NUM("id", v);
                if (v < 0)
                    goto bad;
                g->index = (u32)v;
                out->cmap[i] = (de__font_cmap){(u32)v, (u32)v};
                NUM("x", v);
                g->x = (u32)v;
                NUM("y", v);
                g->y = (u32)v;
                NUM("width", v);
                g->w = (u32)v;
                NUM("height", v);
                g->h = (u32)v;
                NUM("xoffset", v);
                g->left = (f32)v;
                NUM("yoffset", v);
                g->top = -(f32)v;
                NUM("xadvance", v);
                g->advance = (f32)v;
                NUM("page", v);
                g->page = (u32)v;
                g->right = g->left + (f32)g->w;
                g->bottom = g->top - (f32)g->h;
            } else if (!strncmp(line, "kerning ", 8)) {
                if (out->kern_count >= cap)
                    goto bad;
                de__font_kern *k = &out->kerns[out->kern_count++];
                NUM("first", v);
                k->left = (u32)v;
                NUM("second", v);
                k->right = (u32)v;
                NUM("amount", v);
                k->amount = (f32)v;
            }
        }
    }
    if (!out->width || !out->height || out->width > 16384 || out->height > 16384 || !out->pages ||
        !out->count || out->size <= 0)
        goto bad;
    for (u32 i = 0; i < out->pages; ++i)
        if (!out->files[i][0])
            goto bad;
    for (u32 i = 0; i < out->count; ++i) {
        de__font_glyph *g = &out->glyphs[i];
        if (g->index > 0x10ffff || g->page >= out->pages || g->x > out->width ||
            g->y > out->height || g->w > out->width - g->x || g->h > out->height - g->y)
            goto bad;
    }
    /* Stable canonical order is independent of the source representation. */
    for (u32 i = 1; i < out->count; ++i) {
        de__font_glyph g = out->glyphs[i];
        de__font_cmap c = out->cmap[i];
        u32 j = i;
        while (j && out->glyphs[j - 1].index > g.index) {
            out->glyphs[j] = out->glyphs[j - 1];
            out->cmap[j] = out->cmap[j - 1];
            --j;
        }
        if (j && out->glyphs[j - 1].index == g.index)
            goto bad;
        out->glyphs[j] = g;
        out->cmap[j] = c;
    }
    for (u32 i = 1; i < out->kern_count; ++i) {
        de__font_kern k = out->kerns[i];
        u32 j = i;
        while (j && (out->kerns[j - 1].left > k.left ||
                     (out->kerns[j - 1].left == k.left && out->kerns[j - 1].right > k.right))) {
            out->kerns[j] = out->kerns[j - 1];
            --j;
        }
        out->kerns[j] = k;
    }
    return DE_OK;
bad:
    return DE_FAIL(DE_ERR_PARSE, "BMFont line %u offset %llu: malformed record", lineno,
                   (unsigned long long)off);
}
