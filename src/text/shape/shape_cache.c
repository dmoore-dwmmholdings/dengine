#include "shape_state.h"
#include <xxhash.h>
#include <string.h>
#include <stdio.h>
u64 de__shape_hash(de_text_system *t, de_str text, const de_text_style *s, de_text_dir dir,
                   de__shape_key *out) {
    memset(out, 0, sizeof *out);
    out->font = s->font.id;
    memcpy(&out->size, &s->size_px, 4);
    memcpy(&out->spacing, &s->letter_spacing, 4);
    out->features = s->disable_features;
    out->direction = (u32)dir;
    if (s->language)
        snprintf(out->language, sizeof out->language, "%s", s->language);
    struct de__text_runtime *r = t->runtime;
    de__text_lock(r);
    de__text_watch_font(t, s->font);
    u64 g = r->epoch;
    de_text_font *f = de_text__font_get(t, s->font);
    g = g * 1099511628211ULL + (f ? f->generation : 0);
    for (u32 i = 0; i < r->chain_count; ++i)
        if (r->chains[i].primary.id == s->font.id) {
            for (u32 j = 0; j < r->chains[i].count; ++j) {
                de_font font = r->chains[i].fonts[j];
                de__text_watch_font(t, font);
                f = de_text__font_get(t, font);
                g = (g ^ font.id) * 1099511628211ULL + (f ? f->generation : 0);
            }
            break;
        }
    out->generations = g;
    de__text_unlock(r);
    return XXH3_64bits_withSeed(text.ptr, text.len, XXH3_64bits(out, sizeof *out));
}
static u32 capacity(struct de__shape_system *s, u32 stripe) {
    i64 n = de_cvar_int(s->capacity);
    if (n < 0)
        n = 0;
    if (n > 4096)
        n = 4096;
    return (u32)n / 16 + (stripe < (u32)n % 16 ? 1u : 0u);
}
bool de__shape_cache_get(de_text_system *t, u64 hash, const de__shape_key *key, de_str text,
                         const de_allocator *a, de_shaped_run *out) {
    struct de__shape_system *s = t->shape;
    u32 index = (u32)hash & 15, cap = capacity(s, index);
    if (!cap)
        return false;
    de__shape_stripe *stripe = &s->stripes[index];
    de_mutex_lock(&stripe->mutex);
    for (u32 i = 0; i < cap; ++i) {
        de__shape_node *n = &stripe->nodes[i];
        if (n->block && n->hash == hash && n->len == text.len &&
            !memcmp(&n->key, key, sizeof *key) && !memcmp(n->block, text.ptr, text.len)) {
            u64 bytes = (u64)n->count * sizeof(de_shaped_glyph);
            de_shaped_glyph *g = DE_ALLOC(a, bytes, _Alignof(de_shaped_glyph), DE_MEM_UI);
            if (!g && bytes) {
                de_mutex_unlock(&stripe->mutex);
                return false;
            }
            u64 off = (text.len + 15) & ~15ULL;
            if (bytes)
                memcpy(g, n->block + off, (size_t)bytes);
            *out = (de_shaped_run){g, n->count, a, bytes};
            n->tick = ++stripe->clock;
            de_mutex_unlock(&stripe->mutex);
            return true;
        }
    }
    de_mutex_unlock(&stripe->mutex);
    return false;
}
void de__shape_cache_put(de_text_system *t, u64 hash, const de__shape_key *key, de_str text,
                         const de_shaped_run *run) {
    u64 off = (text.len + 15) & ~15ULL, bytes = (u64)run->count * sizeof(de_shaped_glyph);
    if (off + bytes > DE_SHAPE_BLOCK)
        return;
    struct de__shape_system *s = t->shape;
    u32 index = (u32)hash & 15, cap = capacity(s, index);
    if (!cap)
        return;
    de__shape_stripe *stripe = &s->stripes[index];
    de_mutex_lock(&stripe->mutex);
    u32 best = 0;
    for (u32 i = 0; i < cap; ++i) {
        if (!stripe->nodes[i].block) {
            best = i;
            break;
        }
        if (stripe->nodes[i].tick < stripe->nodes[best].tick)
            best = i;
    }
    de__shape_node *n = &stripe->nodes[best];
    if (!n->block)
        n->block = de_arena_push(stripe->arena, DE_SHAPE_BLOCK, 16);
    if (n->block) {
        memcpy(n->block, text.ptr, text.len);
        if (bytes)
            memcpy(n->block + off, run->glyphs, (size_t)bytes);
        n->key = *key;
        n->hash = hash;
        n->len = (u32)text.len;
        n->count = run->count;
        n->tick = ++stripe->clock;
    }
    de_mutex_unlock(&stripe->mutex);
}
de_result de_text__shape_init(de_text_system *t) {
    struct de__shape_system *s = DE_NEW(t->alloc, struct de__shape_system, DE_MEM_UI);
    if (!s)
        return DE_ERR_OUT_OF_MEMORY;
    memset(s, 0, sizeof *s);
    s->alloc = t->alloc;
    t->shape = s;
    de_mutex_init(&s->font_mutex);
    for (u32 i = 0; i < 16; ++i)
        de_mutex_init(&s->stripes[i].mutex);
    s->capacity = de_cvar_register(&(de_cvar_desc){.name = "t.shape_cache_entries",
                                                   .type = DE_CVAR_INT,
                                                   .default_number = 4096,
                                                   .min = 0,
                                                   .max = 4096,
                                                   .help = "Shaped text LRU entries"});
    if (de_tls_key_create(&s->tls, NULL) != DE_OK) {
        de_text__shape_destroy(t);
        return DE_ERR_OUT_OF_MEMORY;
    }
    for (u32 i = 0; i < 16; ++i) {
        de_result r = de_arena_create(
            &(de_arena_desc){.reserve_bytes = DE_SHAPE_NODES * DE_SHAPE_BLOCK + 65536,
                             .tag = DE_MEM_UI,
                             .name = "shape cache"},
            &s->stripes[i].arena);
        if (r != DE_OK) {
            de_text__shape_destroy(t);
            return r;
        }
    }
    return DE_OK;
}
void de_text__shape_destroy(de_text_system *t) {
    struct de__shape_system *s = t->shape;
    if (!s)
        return;
    for (u32 i = 0; i < s->font_count; ++i)
        hb_font_destroy(s->fonts[i].font);
    for (u32 i = 0; i < s->buffer_count; ++i)
        hb_buffer_destroy(s->buffers[i]);
    if (s->tls.id) {
        (void)de_tls_set(s->tls, NULL);
        de_tls_key_destroy(s->tls);
    }
    for (u32 i = 0; i < 16; ++i) {
        de_arena_destroy(s->stripes[i].arena);
        de_mutex_destroy(&s->stripes[i].mutex);
    }
    de_mutex_destroy(&s->font_mutex);
    DE_FREE(s->alloc, s, sizeof *s);
    t->shape = NULL;
}
