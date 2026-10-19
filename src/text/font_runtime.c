#include "fallback_internal.h"
#include "loc/loc_internal.h"
#include "render/text_render_internal.h"
#include "asset/slots_internal.h"
#include <string.h>
#include <math.h>
#if DE_HAS_COOKER
#include "cook/lib/de_cooker.h"
#endif
static f32 rf(const u8 *p) {
    u32 u = de__font_u32(p);
    f32 f;
    memcpy(&f, &u, 4);
    return f;
}
void de_text__font_free(de_text_font *f) {
    if (f)
        DE_FREE(f->alloc, f, f->block_bytes);
}
de_result de_text__font_decode(const de_allocator *a, const void *data, u64 size,
                               de_text_font **out) {
    if (!out)
        return DE_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (!data || size < 64 || memcmp(data, "DFNT", 4))
        return DE_FAIL(DE_ERR_PARSE, "font header");
    const u8 *p = data;
    u32 version = de__font_u32(p + 4), mode = de__font_u32(p + 8), ng = de__font_u32(p + 12),
        nc = de__font_u32(p + 16), nk = de__font_u32(p + 20), np = de__font_u32(p + 24),
        ns = de__font_u32(p + 28), w = de__font_u32(p + 52), h = de__font_u32(p + 56),
        ch = de__font_u32(p + 60);
    if (version != 1 || mode > 2 || !ng || ng > 65536 || nc > 0x110000 || nk > 1000000 || !np ||
        np > 256 || !w || !h || w > 16384 || h > 16384 || (ch != 1 && ch != 4))
        return DE_FAIL(DE_ERR_PARSE, "font counts/version");
    u64 pixels = (u64)np * w * h * ch,
        need = 64 + (u64)ng * 48 + (u64)nc * 8 + (u64)nk * 12 + ns + pixels;
    if (need != size || size > 1024ULL * 1024 * 1024)
        return DE_FAIL(DE_ERR_PARSE, "font byte lengths");
    if (!a)
        a = de_heap_allocator();
    u64 extra = (u64)ng * sizeof(de__font_glyph) + (u64)nc * sizeof(de__font_cmap) +
                (u64)nk * sizeof(de__font_kern) + (u64)np * sizeof(u8 *) + ns + pixels + 64;
    u64 total = sizeof(de_text_font) + extra;
    de_text_font *f = DE_ALLOC(a, total, _Alignof(de_text_font), DE_MEM_UI);
    if (!f)
        return DE_ERR_OUT_OF_MEMORY;
    memset(f, 0, sizeof *f);
    f->alloc = a;
    f->block_bytes = total;
    f->mode = mode;
    f->generation = 1;
    f->count = ng;
    f->cmap_count = nc;
    f->kern_count = nk;
    f->pages = np;
    f->width = w;
    f->height = h;
    f->channels = ch;
    f->em = rf(p + 32);
    f->range = rf(p + 36);
    f->ascent = rf(p + 40);
    f->descent = rf(p + 44);
    f->gap = rf(p + 48);
    f->source_size = ns;
    if (!isfinite(f->em) || f->em <= 0 || !isfinite(f->range) || !isfinite(f->ascent) ||
        !isfinite(f->descent) || !isfinite(f->gap))
        goto bad;
    u8 *mem = (u8 *)(f + 1);
    f->pixels = (u8 **)mem;
    mem += (u64)np * sizeof(u8 *);
    f->glyphs = (de__font_glyph *)mem;
    mem += (u64)ng * sizeof *f->glyphs;
    f->cmap = (de__font_cmap *)mem;
    mem += (u64)nc * sizeof *f->cmap;
    f->kerns = (de__font_kern *)mem;
    mem += (u64)nk * sizeof *f->kerns;
    p += 64;
    for (u32 i = 0; i < ng; ++i, p += 48) {
        de__font_glyph *g = &f->glyphs[i];
        *g = (de__font_glyph){de__font_u32(p),      de__font_u32(p + 4),  de__font_u32(p + 8),
                              de__font_u32(p + 12), de__font_u32(p + 16), de__font_u32(p + 20),
                              rf(p + 24),           rf(p + 28),           rf(p + 32),
                              rf(p + 36),           rf(p + 40),           rf(p + 44)};
        if ((i && g->index <= f->glyphs[i - 1].index) || g->page >= np || g->x > w || g->y > h ||
            g->w > w - g->x || g->h > h - g->y || !isfinite(g->advance) || !isfinite(g->left) ||
            !isfinite(g->right) || !isfinite(g->top) || !isfinite(g->bottom))
            goto bad;
    }
    for (u32 i = 0; i < nc; ++i, p += 8) {
        f->cmap[i] = (de__font_cmap){de__font_u32(p), de__font_u32(p + 4)};
        if (f->cmap[i].codepoint > 0x10ffff ||
            (i && f->cmap[i].codepoint <= f->cmap[i - 1].codepoint))
            goto bad;
    }
    for (u32 i = 0; i < nk; ++i, p += 12) {
        f->kerns[i] = (de__font_kern){de__font_u32(p), de__font_u32(p + 4), rf(p + 8)};
        if (!isfinite(f->kerns[i].amount))
            goto bad;
    }
    f->source = mem;
    memcpy(mem, p, ns);
    mem += ns;
    p += ns;
    if (mode == DE_FONT_MSDF) {
        if (ns < 12 || !stbtt_InitFont(&f->face, f->source, 0))
            goto bad;
    }
    for (u32 i = 0; i < np; ++i) {
        u64 bytes = (u64)w * h * ch;
        f->pixels[i] = mem;
        memcpy(mem, p, (size_t)bytes);
        mem += bytes;
        p += bytes;
        f->dirty[f->dirty_count++] = (de_text_dirty_rect){i, 0, 0, w, h};
    }
    *out = f;
    return DE_OK;
bad:
    de_text__font_free(f);
    return DE_FAIL(DE_ERR_PARSE, "font table/metrics");
}
static de_result font_load(de_asset_load_ctx *c) {
    const de_allocator *a = de_asset_load_ctx_user(c);
    u64 n;
    const void *p = de_asset_load_ctx_data(c, &n);
    de_text_font *f = NULL;
    DE_TRY(de_text__font_decode(a, p, n, &f));
    de_asset_load_ctx_set_object(c, f, f->block_bytes);
    return DE_OK;
}
static void font_unload(void *object, void *user) {
    (void)user;
    de_text__font_free(object);
}
de_result de_text__runtime_init(de_text_system *t) {
    t->runtime = DE_NEW(t->alloc, struct de__text_runtime, DE_MEM_UI);
    if (!t->runtime)
        return DE_ERR_OUT_OF_MEMORY;
    memset(t->runtime, 0, sizeof *t->runtime);
    atomic_flag_clear(&t->runtime->lock);
    t->runtime->epoch = 1;
    if (!t->assets)
        return DE_OK;
    de_asset_type_desc d = {0};
#if DE_HAS_COOKER
    (void)de_cooker_builtin_importer("font", &d);
#endif
    d.name = "font";
    d.load = font_load;
    d.unload = font_unload;
    d.user = (void *)t->alloc;
    if (!DE_HANDLE_VALID(de_asset_type_register(t->assets, &d))) {
        DE_FREE(t->alloc, t->runtime, sizeof *t->runtime);
        t->runtime = NULL;
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "font type already registered");
    }
    return DE_OK;
}
void de_text__runtime_destroy(de_text_system *t) {
    if (t->runtime)
        DE_FREE(t->alloc, t->runtime, sizeof *t->runtime);
}
void de_text_update(de_text_system *t) {
    if (!t || !t->runtime)
        return;
    de__loc_update(t->loc);
    struct de__text_runtime *r = t->runtime;
    de__text_lock(r);
    for (u32 i = 0; i < r->watch_count; ++i) {
        de__text_watch *w = &r->watch[i];
        de_text_font *f = de_text__font_get(t, w->handle);
        if (f != w->object) {
            w->object = f;
            ++w->generation;
            if (f)
                f->generation = w->generation;
            ++r->epoch;
        }
    }
    de__text_unlock(r);
    de_text__integrate_glyphs(t);
    de_text__publish_pages(t);
}
de_text_font *de_text__font_get(de_text_system *t, de_font font) {
    if (!t || !t->assets || !font.id)
        return NULL;
    de__slot *s = de__slot_get(t->assets, font.id);
    if (!s || s->type_hash != de_hash64_str("font"))
        return NULL;
    return de__slot_object(s);
}
/* These named integration points are filled by the runtime glyph and render owners. */
void de_text__publish_pages(de_text_system *t) {
#if DE_HAS_RENDER
    de_text__render_begin(t);
#else
    (void)t;
#endif
}

/* DENG-317 metadata and allocation-free packet ownership; no layout/font behavior changes. */
de_result de_text_get_page_info(de_text_system *t, de_font font, u32 page, de_text_page_info *out) {
    if (out)
        memset(out, 0, sizeof *out);
    if (!t || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "text page info: invalid argument");
    de__text_lock(t->runtime);
    const de_text_font *f = de_text__font_get(t, font);
    if (!f || page >= f->pages) {
        de__text_unlock(t->runtime);
        return DE_FAIL(DE_ERR_NOT_FOUND, "text page not ready");
    }
    *out = (de_text_page_info){.width = f->width,
                               .height = f->height,
                               .generation = f->generation,
                               .distance_range = f->range,
                               .bitmap = f->mode != DE_FONT_MSDF,
                               .pixel = f->mode == DE_FONT_PIXEL};
    de__text_unlock(t->runtime);
    out->texture = de_text_page_texture(t, font, page);
    return DE_OK;
}
bool de_text_pin_font(de_text_system *t, de_font font) {
    return t && de_text__font_get(t, font) && de__slot_ref_handle(t->assets, font.id);
}
void de_text_unpin_font(de_text_system *t, de_font font) {
    if (t && t->assets && font.id)
        (void)de__slot_unref_handle(t->assets, font.id);
}
