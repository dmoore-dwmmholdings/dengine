#include "fallback_internal.h"
#include "glyph_cache_internal.h"
#include <string.h>
bool de_text__glyph(de_text_system *t, de_font handle, u32 index, de_text_glyph_entry *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof *out);
    out->missing = true;
    if (!t)
        return false;
    struct de__text_runtime *r = t->runtime;
    de__text_lock(r);
    de__text_watch_font(t, handle);
    de_text_font *f = de_text__font_get(t, handle);
    if (!f) {
        de__text_unlock(r);
        return false;
    }
    if (de_text__runtime_glyph(t, handle, index, out)) {
        de__text_unlock(r);
        return true;
    }
    out->generation = f->generation;
    u32 lo = 0, hi = f->count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        de__font_glyph *g = &f->glyphs[mid];
        if (g->index == index) {
            out->page = g->page;
            out->x = g->x;
            out->y = g->y;
            out->w = g->w;
            out->h = g->h;
            out->uv =
                (de_vec4){(f32)g->x / (f32)f->width, (f32)g->y / (f32)f->height,
                          (f32)(g->x + g->w) / (f32)f->width, (f32)(g->y + g->h) / (f32)f->height};
            out->plane = (de_vec4){g->left, g->bottom, g->right, g->top};
            out->advance = g->advance;
            out->missing = false;
            de__text_unlock(r);
            return true;
        }
        if (g->index < index)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (f->mode == DE_FONT_MSDF && index < (u32)f->face.numGlyphs) {
        int advance, bearing;
        stbtt_GetGlyphHMetrics(&f->face, (int)index, &advance, &bearing);
        float scale = stbtt_ScaleForMappingEmToPixels(&f->face, 1);
        out->advance = (f32)advance * scale;
    }
    de__text_unlock(r);
    de_text__glyph_miss(t, handle, index);
    return false;
}
de_result de_text_font_metrics(de_text_system *t, de_font handle, f32 size, de_font_metrics *out) {
    if (!out || !t || size < 0)
        return DE_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    struct de__text_runtime *r = t->runtime;
    de__text_lock(r);
    de__text_watch_font(t, handle);
    de_text_font *f = de_text__font_get(t, handle);
    if (!f) {
        de__text_unlock(r);
        return DE_ERR_NOT_FOUND;
    }
    f32 scale = f->mode == DE_FONT_MSDF ? size : size / f->em;
    *out = (de_font_metrics){f->ascent * scale, f->descent * scale, f->gap * scale, f->generation};
    de__text_unlock(r);
    return DE_OK;
}
