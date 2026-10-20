#include "shape_state.h"
#include <string.h>
#include <math.h>
static hb_buffer_t *thread_buffer(struct de__shape_system *s) {
    uintptr_t index = (uintptr_t)de_tls_get(s->tls);
    if (index)
        return s->buffers[index - 1];
    de_mutex_lock(&s->font_mutex);
    if (s->buffer_count == 128) {
        de_mutex_unlock(&s->font_mutex);
        return NULL;
    }
    hb_buffer_t *b = hb_buffer_create();
    if (!hb_buffer_allocation_successful(b)) {
        hb_buffer_destroy(b);
        de_mutex_unlock(&s->font_mutex);
        return NULL;
    }
    u32 slot = s->buffer_count++;
    s->buffers[slot] = b;
    de_mutex_unlock(&s->font_mutex);
    if (de_tls_set(s->tls, (void *)(uintptr_t)(slot + 1)) != DE_OK)
        return NULL;
    return b;
}
static hb_font_t *font_for(de_text_system *t, de_font handle, u32 *upem) {
    struct de__shape_system *s = t->shape;
    de_mutex_lock(&s->font_mutex);
    de__text_lock(t->runtime);
    de__text_watch_font(t, handle);
    de_text_font *f = de_text__font_get(t, handle);
    if (!f || !f->source_size) {
        de__text_unlock(t->runtime);
        de_mutex_unlock(&s->font_mutex);
        return NULL;
    }
    u32 i;
    for (i = 0; i < s->font_count; ++i)
        if (s->fonts[i].handle.id == handle.id)
            break;
    if (i == s->font_count) {
        if (i == 256) {
            de__text_unlock(t->runtime);
            de_mutex_unlock(&s->font_mutex);
            return NULL;
        }
        ++s->font_count;
        s->fonts[i].handle = handle;
    }
    de__hb_font *slot = &s->fonts[i];
    if (!slot->font || slot->generation != f->generation || slot->identity != f) {
        de__font_source_ref(f->source_owner);
        hb_blob_t *blob =
            hb_blob_create((const char *)f->source, f->source_size, HB_MEMORY_MODE_READONLY,
                           f->source_owner, de__font_source_unref);
        hb_face_t *face = hb_face_create(blob, 0);
        hb_blob_destroy(blob);
        hb_font_t *font = hb_font_create(face);
        hb_ot_font_set_funcs(font);
        slot->upem = hb_face_get_upem(face);
        hb_font_set_scale(font, (int)slot->upem, (int)slot->upem);
        hb_font_make_immutable(font);
        hb_face_destroy(face);
        if (slot->font)
            hb_font_destroy(slot->font);
        slot->font = font;
        slot->generation = f->generation;
        slot->identity = f;
    }
    *upem = slot->upem;
    hb_font_t *font = hb_font_reference(slot->font);
    de__text_unlock(t->runtime);
    de_mutex_unlock(&s->font_mutex);
    return font;
}
void de_text__shape_free(de_shaped_run *r) {
    if (r && r->glyphs) {
        DE_FREE(r->alloc, r->glyphs, r->bytes);
        memset(r, 0, sizeof *r);
    }
}
de_result de_text__shape(de_text_system *t, de_str text, const de_text_style *style,
                         de_text_dir dir, const de_allocator *a, de_shaped_run *out) {
    if (!t || !style || !out || (!text.ptr && text.len) || text.len > INT32_MAX ||
        !isfinite(style->size_px) || style->size_px <= 0 || !isfinite(style->letter_spacing) ||
        dir > DE_DIR_RTL || (style->language && strlen(style->language) >= 64))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "shape arguments");
    memset(out, 0, sizeof *out);
    if (!a)
        a = de_heap_allocator();
    if (!text.len) {
        out->alloc = a;
        return DE_OK;
    }
    de__shape_key key;
    u64 hash = de__shape_hash(t, text, style, dir, &key);
    if (de__shape_cache_get(t, hash, &key, text, a, out))
        return DE_OK;
    de_text_item *items = NULL;
    u32 count = 0;
    DE_TRY(de_text__itemize(t, style->font, text, a, &items, &count));
    u32 cap = (u32)text.len * 4 + 16;
    u64 bytes = (u64)cap * sizeof(de_shaped_glyph);
    de_shaped_glyph *glyphs = DE_ALLOC(a, bytes, _Alignof(de_shaped_glyph), DE_MEM_UI);
    if (!glyphs) {
        DE_FREE(a, items, text.len * sizeof *items);
        return DE_ERR_OUT_OF_MEMORY;
    }
    *out = (de_shaped_run){glyphs, 0, a, bytes};
    hb_buffer_t *buffer = thread_buffer(t->shape);
    if (!buffer) {
        DE_FREE(a, items, text.len * sizeof *items);
        de_text__shape_free(out);
        return DE_ERR_OUT_OF_MEMORY;
    }
    for (u32 j = 0; j < count; ++j) {
        de_text_item *it = &items[dir == DE_DIR_RTL ? count - 1 - j : j];
        u32 upem = 0;
        hb_font_t *font = font_for(t, it->font, &upem);
        if (font) {
            hb_buffer_clear_contents(buffer);
            hb_buffer_set_direction(buffer,
                                    dir == DE_DIR_RTL ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
            hb_buffer_set_script(buffer, it->script);
            hb_buffer_set_language(
                buffer, hb_language_from_string(style->language ? style->language : "und", -1));
            hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
            hb_buffer_add_utf8(buffer, text.ptr, (int)text.len, it->start,
                               (int)(it->end - it->start));
            hb_feature_t features[4] = {
                {HB_TAG('k', 'e', 'r', 'n'), !(style->disable_features & DE_TEXT_DISABLE_KERN), 0,
                 UINT32_MAX},
                {HB_TAG('l', 'i', 'g', 'a'),
                 !(style->disable_features & DE_TEXT_DISABLE_LIGA) && style->letter_spacing == 0, 0,
                 UINT32_MAX},
                {HB_TAG('c', 'l', 'i', 'g'),
                 !(style->disable_features & DE_TEXT_DISABLE_CLIG) && style->letter_spacing == 0, 0,
                 UINT32_MAX},
                {HB_TAG('c', 'a', 'l', 't'), !(style->disable_features & DE_TEXT_DISABLE_CALT), 0,
                 UINT32_MAX}};
            hb_shape(font, buffer, features, 4);
            unsigned n;
            hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &n);
            hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buffer, NULL);
            if (n > cap - out->count) {
                hb_font_destroy(font);
                DE_FREE(a, items, text.len * sizeof *items);
                de_text__shape_free(out);
                return DE_ERR_UNSUPPORTED;
            }
            f32 scale = style->size_px / (f32)upem;
            for (u32 k = 0; k < n; ++k)
                glyphs[out->count++] =
                    (de_shaped_glyph){info[k].codepoint,
                                      info[k].cluster,
                                      it->font,
                                      (f32)pos[k].x_advance * scale + style->letter_spacing,
                                      (f32)pos[k].y_advance * scale,
                                      (f32)pos[k].x_offset * scale,
                                      (f32)pos[k].y_offset * scale};
            hb_font_destroy(font);
        } else {
            u32 at = it->start;
            while (at < it->end) {
                u32 start = at, cp = de__text_utf8(text.ptr, (u32)text.len, &at);
                de_text_font *f = de_text__font_get(t, it->font);
                u32 gid = de_text__codepoint(f, cp);
                de_text_glyph_entry g;
                (void)de_text__glyph(t, it->font, gid, &g);
                f32 scale = f && f->em > 0 ? style->size_px / f->em : 1;
                glyphs[out->count++] = (de_shaped_glyph){
                    gid, start, it->font, g.advance * scale + style->letter_spacing, 0, 0, 0};
            }
        }
    }
    DE_FREE(a, items, text.len * sizeof *items);
    de__shape_cache_put(t, hash, &key, text, out);
    return DE_OK;
}
