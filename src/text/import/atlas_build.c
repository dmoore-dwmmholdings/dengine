#include "import_internal.h"
#include <string.h>
/* MaxRects best-short-side fit. Stable first tie wins and rectangles never rotate. */
typedef struct rect {
    u32 x, y, w, h, page;
} rect;
static bool inside(rect a, rect b) {
    return a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h;
}
de_result de__font_pack(de_asset_import_ctx *c, de__font_build *b) {
    const de_allocator *a = de_asset_import_allocator(c);
    u32 cap = b->count * 8 + 16, n = 1;
    rect *r = DE_ALLOC(a, (u64)cap * sizeof *r, _Alignof(rect), DE_MEM_ASSET);
    if (!r)
        return DE_ERR_OUT_OF_MEMORY;
    r[0] = (rect){0, 0, b->width, b->height, 0};
    b->pages = 1;
    for (u32 g = 0; g < b->count; ++g) {
        de__font_glyph *v = &b->glyphs[g];
        if (!v->w || !v->h)
            continue;
        if (v->w > b->width || v->h > b->height)
            return de_asset_import_fail(c, DE_ERR_UNSUPPORTED, "font glyph %u exceeds page",
                                        v->index);
        u32 best = UINT32_MAX, score = UINT32_MAX;
        for (u32 i = 0; i < n; ++i)
            if (v->w <= r[i].w && v->h <= r[i].h) {
                u32 dx = r[i].w - v->w, dy = r[i].h - v->h, s = dx < dy ? dx : dy;
                if (s < score) {
                    score = s;
                    best = i;
                }
            }
        if (best == UINT32_MAX) {
            if (n >= cap || b->pages >= 256)
                return DE_ERR_UNSUPPORTED;
            best = n++;
            r[best] = (rect){0, 0, b->width, b->height, b->pages++};
        }
        rect used = {r[best].x, r[best].y, v->w, v->h, r[best].page};
        v->x = used.x;
        v->y = used.y;
        v->page = used.page;
        u32 old = n;
        for (u32 i = 0; i < old; ++i) {
            rect q = r[i];
            if (q.page != used.page || q.x >= used.x + used.w || used.x >= q.x + q.w ||
                q.y >= used.y + used.h || used.y >= q.y + q.h)
                continue;
            rect pieces[4];
            u32 k = 0;
            if (used.x > q.x)
                pieces[k++] = (rect){q.x, q.y, used.x - q.x, q.h, q.page};
            if (used.x + used.w < q.x + q.w)
                pieces[k++] =
                    (rect){used.x + used.w, q.y, q.x + q.w - used.x - used.w, q.h, q.page};
            if (used.y > q.y)
                pieces[k++] = (rect){q.x, q.y, q.w, used.y - q.y, q.page};
            if (used.y + used.h < q.y + q.h)
                pieces[k++] =
                    (rect){q.x, used.y + used.h, q.w, q.y + q.h - used.y - used.h, q.page};
            r[i].w = 0;
            for (u32 j = 0; j < k; ++j) {
                if (n >= cap)
                    return DE_ERR_OUT_OF_MEMORY;
                r[n++] = pieces[j];
            }
        }
        for (u32 i = 0; i < n;) {
            bool remove = !r[i].w;
            for (u32 j = 0; !remove && j < n; ++j)
                if (i != j && r[i].page == r[j].page && inside(r[i], r[j]) &&
                    (!inside(r[j], r[i]) || j < i))
                    remove = true;
            if (remove) {
                memmove(r + i, r + i + 1, (n - i - 1) * sizeof *r);
                --n;
            } else
                ++i;
        }
    }
    b->pixels = DE_ALLOC(a, (u64)b->pages * sizeof *b->pixels, _Alignof(u8 *), DE_MEM_ASSET);
    if (!b->pixels)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 p = 0; p < b->pages; ++p) {
        u64 bytes = (u64)b->width * b->height * b->channels;
        b->pixels[p] = DE_ALLOC(a, bytes, 1, DE_MEM_ASSET);
        if (!b->pixels[p])
            return DE_ERR_OUT_OF_MEMORY;
        memset(b->pixels[p], 0, (size_t)bytes);
    }
    return DE_OK;
}
static void put(u8 *p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
static void flt(u8 *p, f32 f) {
    u32 u;
    memcpy(&u, &f, 4);
    put(p, u);
}
de_result de__font_write(de_asset_import_ctx *c, const de__font_build *b) {
    u8 h[64] = {0};
    memcpy(h, "DFNT", 4);
    put(h + 4, 1);
    put(h + 8, b->mode);
    put(h + 12, b->count);
    put(h + 16, b->cmap_count);
    put(h + 20, b->kern_count);
    put(h + 24, b->pages);
    put(h + 28, b->source_size);
    flt(h + 32, b->em);
    flt(h + 36, b->range);
    flt(h + 40, b->ascent);
    flt(h + 44, b->descent);
    flt(h + 48, b->gap);
    put(h + 52, b->width);
    put(h + 56, b->height);
    put(h + 60, b->channels);
    DE_TRY(de_asset_import_write(c, h, 64));
    for (u32 i = 0; i < b->count; ++i) {
        const de__font_glyph *g = &b->glyphs[i];
        u8 q[48];
        put(q, g->index);
        put(q + 4, g->page);
        put(q + 8, g->x);
        put(q + 12, g->y);
        put(q + 16, g->w);
        put(q + 20, g->h);
        flt(q + 24, g->left);
        flt(q + 28, g->bottom);
        flt(q + 32, g->right);
        flt(q + 36, g->top);
        flt(q + 40, g->advance);
        flt(q + 44, g->bearing);
        DE_TRY(de_asset_import_write(c, q, 48));
    }
    for (u32 i = 0; i < b->cmap_count; ++i) {
        u8 q[8];
        put(q, b->cmap[i].codepoint);
        put(q + 4, b->cmap[i].glyph);
        DE_TRY(de_asset_import_write(c, q, 8));
    }
    for (u32 i = 0; i < b->kern_count; ++i) {
        u8 q[12];
        put(q, b->kerns[i].left);
        put(q + 4, b->kerns[i].right);
        flt(q + 8, b->kerns[i].amount);
        DE_TRY(de_asset_import_write(c, q, 12));
    }
    DE_TRY(de_asset_import_write(c, b->source, b->source_size));
    for (u32 i = 0; i < b->pages; ++i)
        DE_TRY(de_asset_import_write(c, b->pixels[i], (u64)b->width * b->height * b->channels));
    return DE_OK;
}
