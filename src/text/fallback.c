#include "fallback_internal.h"
#include <string.h>
u32 de_text__codepoint(const de_text_font *f, u32 cp) {
    if (!f || cp > 0x10ffff)
        return 0;
    if (f->mode == DE_FONT_MSDF)
        return (u32)stbtt_FindGlyphIndex(&f->face, (int)cp);
    u32 lo = 0, hi = f->cmap_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (f->cmap[mid].codepoint == cp)
            return f->cmap[mid].glyph;
        if (f->cmap[mid].codepoint < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}
void de_text_set_fallbacks(de_text_system *t, de_font primary, const de_font *fonts, u32 count) {
    if (!t || !primary.id || count > 8 || (!fonts && count)) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                      "fallback chain requires primary and at most 8 fonts");
        return;
    }
    struct de__text_runtime *r = t->runtime;
    de__text_lock(r);
    u32 i;
    for (i = 0; i < r->chain_count; ++i)
        if (r->chains[i].primary.id == primary.id)
            break;
    if (i == r->chain_count) {
        if (i == DE_TEXT_MAX_CHAINS) {
            de__text_unlock(r);
            (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "fallback chain capacity");
            return;
        }
        ++r->chain_count;
    }
    de__text_chain *c = &r->chains[i];
    memset(c, 0, sizeof *c);
    c->primary = primary;
    c->count = count;
    if (count)
        memcpy(c->fonts, fonts, count * sizeof *fonts);
    ++r->epoch;
    de__text_unlock(r);
}
de__text_resolver de__text_resolve_begin(de_text_system *t, de_font primary) {
    de__text_resolver resolver = {.primary = primary};
    if (!t || !primary.id)
        return resolver;
    resolver.text = t;
    struct de__text_runtime *r = t->runtime;
    de__text_lock(r);
    de__text_chain *c = NULL;
    for (u32 i = 0; i < r->chain_count; ++i)
        if (r->chains[i].primary.id == primary.id) {
            c = &r->chains[i];
            break;
        }
    de__text_watch_font(t, primary);
    if (c)
        for (u32 i = 0; i < c->count; ++i)
            de__text_watch_font(t, c->fonts[i]);
    resolver.chain = c;
    return resolver;
}
void de__text_resolve_end(const de__text_resolver *resolver) {
    if (resolver->text)
        de__text_unlock(resolver->text->runtime);
}
bool de__text_resolve_next(const de__text_resolver *resolver, u32 cp, de_font *out, u32 *glyph) {
    *out = resolver->primary;
    *glyph = 0;
    de_text_system *t = resolver->text;
    if (!t)
        return false;
    struct de__text_runtime *r = t->runtime;
    de__text_chain *c = resolver->chain;
    de__text_cp_cache *slot = NULL;
    if (c) {
        u32 hash = (cp * 2654435761u) & (DE_TEXT_CACHE - 1);
        for (u32 i = 0; i < DE_TEXT_CACHE; ++i) {
            de__text_cp_cache *s = &c->cache[(hash + i) & (DE_TEXT_CACHE - 1)];
            if (s->epoch != r->epoch) {
                slot = s;
                break;
            }
            if (s->cp == cp) {
                *out = (de_font){s->font};
                *glyph = s->glyph;
                return s->found;
            }
        }
    }
    bool found = false;
    for (u32 i = 0; i < 1 + (c ? c->count : 0); ++i) {
        de_font f = i ? c->fonts[i - 1] : resolver->primary;
        de_text_font *font = de_text__font_get(t, f);
        u32 g = de_text__codepoint(font, cp);
        if (g) {
            *out = f;
            *glyph = g;
            found = true;
            break;
        }
    }
    if (slot)
        *slot = (de__text_cp_cache){cp, *glyph, out->id, r->epoch, found};
    return found;
}
bool de_text__resolve(de_text_system *t, de_font primary, u32 cp, de_font *out, u32 *glyph) {
    if (out)
        *out = primary;
    if (glyph)
        *glyph = 0;
    if (!t || !out || !glyph || !primary.id)
        return false;
    de__text_resolver resolver = de__text_resolve_begin(t, primary);
    bool found = de__text_resolve_next(&resolver, cp, out, glyph);
    de__text_resolve_end(&resolver);
    return found;
}

void de__text_watch_font(de_text_system *t, de_font handle) {
    struct de__text_runtime *r = t->runtime;
    u32 i;
    for (i = 0; i < r->watch_count; ++i)
        if (r->watch[i].handle.id == handle.id)
            break;
    if (i == r->watch_count) {
        if (i == 256)
            return;
        ++r->watch_count;
        r->watch[i] = (de__text_watch){.handle = handle};
    }
    de__text_refresh_watch(t, &r->watch[i]);
}
void de__text_refresh_watch(de_text_system *t, de__text_watch *w) {
    struct de__text_runtime *r = t->runtime;
    de_text_font *f = de_text__font_get(t, w->handle);
    const u64 identity = f ? f->identity : 0;
    if (w->object != f || w->identity != identity) {
        w->object = f;
        w->identity = identity;
        ++w->generation;
        /* A first watch may follow runtime integrations; never replace their newer counter. */
        if (f && f->generation < w->generation)
            f->generation = w->generation;
        ++r->epoch;
    }
    if (f && w->generation < f->generation)
        w->generation = f->generation;
}
