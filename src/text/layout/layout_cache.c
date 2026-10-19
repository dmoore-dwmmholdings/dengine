#include "layout_internal.h"
#include <xxhash.h>
#include "text/fallback_internal.h"
#include <math.h>
#include <string.h>
#define STRIPES 16u
#define ENTRIES 16u
typedef struct cache_entry {
    u64 hash, used;
    de_text_layout *layout;
} cache_entry;
typedef struct cache_stripe {
    de_mutex mutex;
    u64 clock;
    cache_entry entries[ENTRIES];
} cache_stripe;
struct de__layout_cache {
    cache_stripe stripes[STRIPES];
};
static void retain(de_text_layout *layout) {
    de_atomic_fetch_add_u32(&layout->refs, 1, DE_ATOMIC_RELAXED);
}
void de_text_layout_free(de_text_layout *layout) {
    if (!layout || de_atomic_fetch_sub_u32(&layout->refs, 1, DE_ATOMIC_ACQ_REL) != 1)
        return;
    const de_allocator *a = layout->alloc;
    if (layout->utf8)
        DE_FREE(a, layout->utf8, (u64)layout->byte_count + 1);
    if (layout->glyphs)
        DE_FREE(a, layout->glyphs, (u64)layout->glyph_capacity * sizeof *layout->glyphs);
    if (layout->clusters)
        DE_FREE(a, layout->clusters, ((u64)layout->byte_count + 1) * sizeof *layout->clusters);
    if (layout->quads)
        DE_FREE(a, layout->quads, (u64)layout->glyph_capacity * sizeof *layout->quads);
    DE_FREE(a, layout, sizeof *layout);
}
de_result de__text_layout_init(de_text_system *t) {
    t->layout = DE_NEW(t->alloc, struct de__layout_cache, DE_MEM_UI);
    if (!t->layout)
        return DE_ERR_OUT_OF_MEMORY;
    memset(t->layout, 0, sizeof *t->layout);
    for (u32 i = 0; i < STRIPES; ++i)
        de_mutex_init(&t->layout->stripes[i].mutex);
    return DE_OK;
}
void de__text_layout_destroy(de_text_system *t) {
    if (!t->layout)
        return;
    for (u32 i = 0; i < STRIPES; ++i) {
        cache_stripe *s = &t->layout->stripes[i];
        for (u32 j = 0; j < ENTRIES; ++j)
            de_text_layout_free(s->entries[j].layout);
        de_mutex_destroy(&s->mutex);
    }
    DE_FREE(t->alloc, t->layout, sizeof *t->layout);
    t->layout = NULL;
}
static de__layout_key make_key(de_text_system *t, const de_text_layout_desc *d,
                               const de_allocator *a) {
    de__layout_key k;
    memset(&k, 0, sizeof k);
    k.style.font = d->style->font;
    k.style.size_px = d->style->size_px;
    k.style.color = d->style->color;
    k.style.letter_spacing = d->style->letter_spacing;
    k.style.line_height = d->style->line_height;
    k.style.outline_width = d->style->outline_width;
    k.style.outline_color = d->style->outline_color;
    memcpy(k.style.shadow_offset, d->style->shadow_offset, sizeof k.style.shadow_offset);
    k.style.shadow_color = d->style->shadow_color;
    k.style.disable_features = d->style->disable_features;
    if (d->style->language)
        memcpy(k.language, d->style->language, strlen(d->style->language));
    de__text_lock(t->runtime);
    k.fallback_epoch = t->runtime->epoch;
    de__text_unlock(t->runtime);
    k.max_width = d->max_width;
    k.align = d->align;
    k.direction = d->direction;
    k.pixel_snap = d->pixel_snap;
    k.alloc = a;
    return k;
}
de_result de_text_layout_build(de_text_system *t, const de_text_layout_desc *d,
                               const de_allocator *a, de_text_layout **out) {
    if (!t || !d || !d->style || !out || (!d->utf8.ptr && d->utf8.len) || d->utf8.len > INT32_MAX ||
        !isfinite(d->max_width) || d->max_width < 0 || !isfinite(d->style->size_px) ||
        d->style->size_px <= 0 || !isfinite(d->style->letter_spacing) ||
        !isfinite(d->style->line_height) || d->style->line_height < 0 ||
        d->align > DE_ALIGN_JUSTIFY || d->direction > DE_DIR_RTL ||
        (d->style->language && strlen(d->style->language) >= 64))
        return DE_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (!a)
        a = de_heap_allocator();
    de__layout_key key = make_key(t, d, a);
    u64 hash = XXH3_64bits_withSeed(d->utf8.ptr, d->utf8.len, XXH3_64bits(&key, sizeof key));
    cache_stripe *s = &t->layout->stripes[hash % STRIPES];
    de_mutex_lock(&s->mutex);
    u32 oldest = 0;
    u64 age = UINT64_MAX;
    for (u32 i = 0; i < ENTRIES; ++i) {
        cache_entry *e = &s->entries[i];
        de_text_layout *l = e->layout;
        if (l && e->hash == hash && l->byte_count == d->utf8.len &&
            !memcmp(&l->key, &key, sizeof key) &&
            (!d->utf8.len || !memcmp(l->utf8, d->utf8.ptr, d->utf8.len))) {
            if (de__text_layout_stale(t, l)) {
                de_text_layout *fresh = NULL;
                de_result result = de__text_layout_refresh(t, l, &fresh);
                if (result != DE_OK) {
                    de_mutex_unlock(&s->mutex);
                    return result;
                }
                e->layout = fresh;
                de_text_layout_free(l);
                l = fresh;
            }
            e->used = ++s->clock;
            retain(l);
            *out = l;
            de_mutex_unlock(&s->mutex);
            return DE_OK;
        }
        if (!l) {
            oldest = i;
            age = 0;
        } else if (age && e->used < age) {
            oldest = i;
            age = e->used;
        }
    }
    de_text_layout *l = NULL;
    de_result result = de__text_paragraph(t, d, &key, &l);
    if (result == DE_OK) {
        cache_entry *e = &s->entries[oldest];
        de_text_layout_free(e->layout);
        *e = (cache_entry){hash, ++s->clock, l};
        retain(l);
        *out = l;
    }
    de_mutex_unlock(&s->mutex);
    return result;
}
de_vec2 de_text_layout_size(const de_text_layout *l) {
    return l ? l->size : (de_vec2){0};
}
u32 de_text_layout_quads(const de_text_layout *l, de_glyph_quad *out, u32 max) {
    if (!l)
        return 0;
    u32 n = l->quad_count < max ? l->quad_count : max;
    if (out && n)
        memcpy(out, l->quads, (size_t)n * sizeof *out);
    return n;
}
u32 de_text_layout_hit_test(const de_text_layout *l, de_vec2 p) {
    if (!l || !l->cluster_count)
        return 0;
    u32 line = p.y <= 0 ? 0 : (u32)(p.y / l->line_height);
    if (line >= l->line_count)
        line = l->line_count - 1;
    const de__layout_cluster *best = NULL;
    f32 distance = INFINITY;
    for (u32 i = 0; i < l->cluster_count; ++i) {
        const de__layout_cluster *c = &l->clusters[i];
        if (c->line != line)
            continue;
        f32 mid = (c->x0 + c->x1) * .5f, delta = fabsf(p.x - mid);
        if (delta < distance) {
            best = c;
            distance = delta;
        }
    }
    if (!best)
        return l->byte_count;
    f32 mid = (best->x0 + best->x1) * .5f;
    if (fabsf(p.x - mid) < .0001f)
        return best->start;
    return (best->rtl ? p.x > mid : p.x < mid) ? best->start : best->end;
}
de_vec4 de_text_layout_caret(const de_text_layout *l, u32 offset) {
    if (!l)
        return (de_vec4){0};
    if (offset > l->byte_count)
        offset = l->byte_count;
    const de__layout_cluster *last = NULL;
    for (u32 i = 0; i < l->cluster_count; ++i) {
        const de__layout_cluster *c = &l->clusters[i];
        if (c->start <= offset && offset < c->end)
            return (de_vec4){c->rtl ? c->x1 : c->x0, c->line * l->line_height, 1, l->line_height};
        if (!last || c->end > last->end)
            last = c;
    }
    if (last)
        return (de_vec4){last->rtl ? last->x0 : last->x1, last->line * l->line_height, 1,
                         l->line_height};
    return (de_vec4){0, 0, 1, l->line_height};
}
