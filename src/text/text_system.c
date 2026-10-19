#include "font_runtime_internal.h"
#include "loc/loc_internal.h"
#include "shape/shaper_internal.h"
#include "glyph_cache_internal.h"
#include "layout/layout_internal.h"
#include "render/text_render_internal.h"
#include <string.h>
de_result de_text_create(const de_allocator *a, de_asset_system *assets, de_text_system **out) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "text output is NULL");
    *out = NULL;
    if (!a)
        a = de_heap_allocator();
    de_text_system *t = DE_NEW(a, de_text_system, DE_MEM_UI);
    if (!t)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "text system allocation");
    memset(t, 0, sizeof *t);
    t->alloc = a;
    t->assets = assets;
    de_result r = de_text__runtime_init(t);
    if (r != DE_OK) {
        DE_FREE(a, t, sizeof *t);
        return r;
    }
    r = de_text__shape_init(t);
    if (r != DE_OK) {
        de_text__runtime_destroy(t);
        DE_FREE(a, t, sizeof *t);
        return r;
    }
    r = de_text__glyph_cache_init(t);
    if (r != DE_OK) {
        de_text__shape_destroy(t);
        de_text__runtime_destroy(t);
        DE_FREE(a, t, sizeof *t);
        return r;
    }
    r = de__text_layout_init(t);
    if (r != DE_OK) {
        de__text_layout_destroy(t);
        de_text__glyph_cache_destroy(t);
        de_text__shape_destroy(t);
        de_text__runtime_destroy(t);
        DE_FREE(a, t, sizeof *t);
        return r;
    }
    r = de__loc_create(a, assets, &t->loc);
    if (r != DE_OK) {
        de__text_layout_destroy(t);
        de_text__glyph_cache_destroy(t);
        de_text__shape_destroy(t);
        de_text__runtime_destroy(t);
        DE_FREE(a, t, sizeof *t);
        return r;
    }
    t->localize = de_loc;
    *out = t;
    return DE_OK;
}
void de_text_destroy(de_text_system *t) {
    if (t) {
        de__loc_destroy(t->loc);
#if DE_HAS_RENDER
        de_text__render_destroy(t);
#endif
        de__text_layout_destroy(t);
        de_text__glyph_cache_destroy(t);
        de_text__shape_destroy(t);
        de_text__runtime_destroy(t);
        DE_FREE(t->alloc, t, sizeof *t);
    }
}
