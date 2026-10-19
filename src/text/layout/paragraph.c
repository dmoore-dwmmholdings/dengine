#include "layout_internal.h"
#include <math.h>
#include <string.h>
#define NO_GLYPH UINT32_MAX
typedef struct work_char {
    u32 first, last, end;
    f32 advance;
    bool boundary;
} work_char;
static bool hard_cp(u32 cp) {
    de__lb c = (de__lb)de__text_unicode(cp).lb;
    /* UAX #9 also ends paragraphs at U+001C..U+001E. Never shape beyond
     * the paragraph whose level array SheenBidi returned. */
    return c == LB_BK || c == LB_CR || c == LB_LF || c == LB_NL ||
           SBCodepointGetBidiType(cp) == SBBidiTypeB;
}
static u32 cp_index(const u32 *offsets, u32 count, u32 byte) {
    u32 lo = 0, hi = count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (offsets[mid] < byte)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
static de_result allocate_layout(const de__layout_key *key, u32 bytes, de_text_layout **out) {
    if (bytes > (UINT32_MAX - 16u) / 4u)
        return DE_ERR_UNSUPPORTED;
    const de_allocator *a = key->alloc;
    de_text_layout *l = DE_NEW(a, de_text_layout, DE_MEM_UI);
    if (!l)
        return DE_ERR_OUT_OF_MEMORY;
    memset(l, 0, sizeof *l);
    l->alloc = a;
    l->key = *key;
    l->byte_count = bytes;
    l->glyph_capacity = bytes * 4 + 16;
    de_atomic_store_u32(&l->refs, 1, DE_ATOMIC_RELAXED);
    l->utf8 = DE_ALLOC(a, (u64)bytes + 1, 1, DE_MEM_UI);
    l->glyphs = DE_ALLOC(a, (u64)l->glyph_capacity * sizeof *l->glyphs,
                         _Alignof(de__layout_position), DE_MEM_UI);
    l->quads =
        DE_ALLOC(a, (u64)l->glyph_capacity * sizeof *l->quads, _Alignof(de_glyph_quad), DE_MEM_UI);
    l->clusters = DE_ALLOC(a, ((u64)bytes + 1) * sizeof *l->clusters, _Alignof(de__layout_cluster),
                           DE_MEM_UI);
    if (!l->utf8 || !l->glyphs || !l->quads || !l->clusters) {
        de_text_layout_free(l);
        return DE_ERR_OUT_OF_MEMORY;
    }
    *out = l;
    return DE_OK;
}
static void track_font(de_text_system *t, de_text_layout *l, de_font font) {
    for (u32 i = 0; i < l->font_count; ++i)
        if (l->fonts[i].font.id == font.id)
            return;
    if (l->font_count == 16)
        return;
    de_text_font *f = de_text__font_get(t, font);
    l->fonts[l->font_count++] = (de__layout_font){font, f ? f->generation : 0, f};
}
void de__text_layout_quads_build(de_text_system *t, de_text_layout *l) {
    l->quad_count = 0;
    l->font_count = 0;
    track_font(t, l, l->key.style.font);
    for (u32 i = 0; i < l->glyph_count; ++i) {
        de__layout_position *p = &l->glyphs[i];
        track_font(t, l, p->shape.font);
        de_text_glyph_entry e;
        bool ready = de_text__glyph(t, p->shape.font, p->shape.glyph, &e);
        de_text_font *font = de_text__font_get(t, p->shape.font);
        if (!ready || e.missing || !e.w || !e.h || !font)
            continue;
        f32 scale =
            font->mode == DE_FONT_MSDF ? l->key.style.size_px : l->key.style.size_px / font->em;
        de_glyph_quad q = {.pos = {p->x + e.plane.x * scale, p->y - e.plane.w * scale,
                                   (e.plane.z - e.plane.x) * scale,
                                   (e.plane.w - e.plane.y) * scale},
                           .uv = e.uv,
                           .atlas_page = e.page,
                           .color = l->key.style.color,
                           .font = p->shape.font};
        if (l->key.pixel_snap) {
            q.pos.x = roundf(q.pos.x);
            q.pos.y = roundf(q.pos.y);
            q.pos.z = roundf(q.pos.z);
            q.pos.w = roundf(q.pos.w);
        }
        l->quads[l->quad_count++] = q;
    }
    /* Glyph lookup can publish a generation only through update; snapshot after queries. */
    for (u32 i = 0; i < l->font_count; ++i) {
        de_text_font *font = de_text__font_get(t, l->fonts[i].font);
        l->fonts[i].identity = font;
        l->fonts[i].generation = font ? font->generation : 0;
    }
}
bool de__text_layout_stale(de_text_system *t, const de_text_layout *l) {
    for (u32 i = 0; i < l->font_count; ++i) {
        de_text_font *font = de_text__font_get(t, l->fonts[i].font);
        if (font != l->fonts[i].identity || (font ? font->generation : 0) != l->fonts[i].generation)
            return true;
    }
    return false;
}
de_result de__text_layout_refresh(de_text_system *t, const de_text_layout *source,
                                  de_text_layout **out) {
    de_text_layout *l = NULL;
    DE_TRY(allocate_layout(&source->key, source->byte_count, &l));
    memcpy(l->utf8, source->utf8, (size_t)source->byte_count + 1);
    l->glyph_count = source->glyph_count;
    l->cluster_count = source->cluster_count;
    l->line_count = source->line_count;
    l->size = source->size;
    l->ascent = source->ascent;
    l->line_height = source->line_height;
    memcpy(l->glyphs, source->glyphs, (size_t)l->glyph_count * sizeof *l->glyphs);
    memcpy(l->clusters, source->clusters, (size_t)l->cluster_count * sizeof *l->clusters);
    de__text_layout_quads_build(t, l);
    *out = l;
    return DE_OK;
}
static de_result shape_paragraph(de_text_system *t, de_text_layout *l, const de_text_layout_desc *d,
                                 const u32 *offsets, u32 count, u32 start, u32 end,
                                 const SBLevel *levels, work_char *work) {
    for (u32 at = start; at < end;) {
        u32 next = at + 1;
        while (next < end && levels[next - start] == levels[at - start])
            ++next;
        u32 byte_start = offsets[at], byte_end = offsets[next];
        de_shaped_run shape;
        DE_TRY(de_text__shape(t, (de_str){d->utf8.ptr + byte_start, byte_end - byte_start},
                              d->style, (levels[at - start] & 1) ? DE_DIR_RTL : DE_DIR_LTR,
                              l->alloc, &shape));
        if (shape.count > l->glyph_capacity - l->glyph_count) {
            de_text__shape_free(&shape);
            return DE_ERR_UNSUPPORTED;
        }
        for (u32 i = 0; i < shape.count; ++i) {
            de_shaped_glyph glyph = shape.glyphs[i];
            glyph.cluster += byte_start;
            u32 cp = cp_index(offsets, count, glyph.cluster);
            if (cp >= end)
                continue;
            u32 index = l->glyph_count++;
            l->glyphs[index] = (de__layout_position){.shape = glyph, .cp = cp, .next = NO_GLYPH};
            if (work[cp].first == NO_GLYPH)
                work[cp].first = index;
            else
                l->glyphs[work[cp].last].next = index;
            work[cp].last = index;
            work[cp].advance += glyph.x_advance;
            work[cp].boundary = true;
        }
        work[next].boundary = true;
        de_text__shape_free(&shape);
        at = next;
    }
    u32 next = end;
    for (u32 i = end; i > start;) {
        --i;
        if (work[i].first != NO_GLYPH) {
            work[i].end = next;
            next = i;
        }
    }
    return DE_OK;
}
static void place_line(de_text_layout *l, SBParagraphRef paragraph, u32 paragraph_start, u32 start,
                       u32 end, u32 visible_end, f32 width, f32 extent, bool last, const u32 *cp,
                       const u32 *offsets, work_char *work) {
    u32 line = l->line_count++;
    f32 baseline = line * l->line_height + l->ascent;
    if (l->key.pixel_snap)
        baseline = roundf(baseline);
    bool rtl = (SBParagraphGetBaseLevel(paragraph) & 1) != 0;
    f32 pen = 0;
    if (l->key.align == DE_ALIGN_CENTER)
        pen = (extent - width) * .5f;
    else if ((l->key.align == DE_ALIGN_END && !rtl) || (l->key.align == DE_ALIGN_START && rtl))
        pen = extent - width;
    u32 spaces = 0;
    for (u32 i = start; i < visible_end; ++i)
        if (cp[i] == ' ')
            ++spaces;
    f32 extra = l->key.align == DE_ALIGN_JUSTIFY && !last && spaces && extent > width
                    ? (extent - width) / spaces
                    : 0;
    f32 right = pen + width + extra * spaces;
    if (right > l->size.x)
        l->size.x = right;
    SBLineRef visual = SBParagraphCreateLine(paragraph, start - paragraph_start, end - start);
    if (!visual)
        return;
    const SBRun *runs = SBLineGetRunsPtr(visual);
    for (u32 run = 0; run < SBLineGetRunCount(visual); ++run) {
        const SBRun *r = &runs[run];
        for (u32 j = 0; j < r->length; ++j) {
            u32 index = paragraph_start +
                        (u32)(r->level & 1 ? r->offset + r->length - 1 - j : r->offset + j);
            if (work[index].first == NO_GLYPH)
                continue;
            f32 before = pen;
            for (u32 glyph = work[index].first; glyph != NO_GLYPH; glyph = l->glyphs[glyph].next) {
                de__layout_position *g = &l->glyphs[glyph];
                g->x = pen + g->shape.x_offset;
                g->y = baseline - g->shape.y_offset;
                if (l->key.pixel_snap) {
                    g->x = roundf(g->x);
                    g->y = roundf(g->y);
                }
                pen += g->shape.x_advance;
            }
            if (index < visible_end && cp[index] == ' ')
                pen += extra;
            f32 x0 = before, x1 = pen;
            if (l->key.pixel_snap) {
                x0 = roundf(x0);
                x1 = roundf(x1);
            }
            l->clusters[l->cluster_count++] = (de__layout_cluster){
                offsets[index], offsets[work[index].end], line, x0, x1, (r->level & 1) != 0};
        }
    }
    SBLineRelease(visual);
}
de_result de__text_paragraph(de_text_system *t, const de_text_layout_desc *d,
                             const de__layout_key *key, de_text_layout **out) {
    de_text_layout *l = NULL;
    DE_TRY(allocate_layout(key, (u32)d->utf8.len, &l));
    if (d->utf8.len)
        memcpy(l->utf8, d->utf8.ptr, d->utf8.len);
    l->utf8[d->utf8.len] = 0;
    de_font_metrics metrics;
    de_result result = de_text_font_metrics(t, d->style->font, d->style->size_px, &metrics);
    if (result != DE_OK) {
        de_text_layout_free(l);
        return result;
    }
    l->ascent = metrics.ascent;
    l->line_height = (metrics.ascent - metrics.descent + metrics.line_gap) *
                     (d->style->line_height > 0 ? d->style->line_height : 1);
    if (l->line_height <= 0)
        l->line_height = d->style->size_px;
    if (d->pixel_snap) {
        l->ascent = roundf(l->ascent);
        l->line_height = roundf(l->line_height);
        if (l->line_height < 1)
            l->line_height = 1;
    }
    u64 capacity = (u64)l->byte_count + 1;
    u32 *integers = DE_ALLOC(l->alloc, capacity * 2 * sizeof(u32), _Alignof(u32), DE_MEM_UI);
    u8 *breaks = DE_ALLOC(l->alloc, capacity * 2, 1, DE_MEM_UI);
    work_char *work = DE_ALLOC(l->alloc, capacity * sizeof *work, _Alignof(work_char), DE_MEM_UI);
    f32 *prefix = DE_ALLOC(l->alloc, capacity * sizeof *prefix, _Alignof(f32), DE_MEM_UI);
    if (!integers || !breaks || !work || !prefix) {
        result = DE_ERR_OUT_OF_MEMORY;
        goto done;
    }
    u32 *cp = integers, *offsets = integers + capacity;
    u8 *graphemes = breaks + capacity;
    memset(work, 0, (size_t)capacity * sizeof *work);
    for (u32 i = 0; i < capacity; ++i)
        work[i].first = NO_GLYPH;
    u32 count = 0, byte = 0;
    while (byte < d->utf8.len) {
        offsets[count] = byte;
        cp[count++] = de__text_utf8(d->utf8.ptr, (u32)d->utf8.len, &byte);
    }
    offsets[count] = (u32)d->utf8.len;
    result = de__text_linebreak(cp, count, breaks, l->alloc);
    if (result != DE_OK)
        goto done;
    de__text_graphemes(cp, count, graphemes);
    for (u32 start = 0;;) {
        u32 end = start;
        while (end < count && !hard_cp(cp[end]))
            ++end;
        if (end == start) {
            ++l->line_count;
        } else {
            de__bidi bidi;
            result = de__text_bidi(cp + start, end - start, d->direction, &bidi);
            if (result != DE_OK)
                goto done;
            result = shape_paragraph(t, l, d, offsets, count, start, end,
                                     SBParagraphGetLevelsPtr(bidi.paragraph), work);
            if (result != DE_OK) {
                de__text_bidi_free(&bidi);
                goto done;
            }
            prefix[start] = 0;
            for (u32 i = start; i < end; ++i) {
                if (cp[i] == '\t' && work[i].first != NO_GLYPH) {
                    u32 first = work[i].first;
                    l->glyphs[first].shape.x_advance += d->style->size_px * 2 - work[i].advance;
                    work[i].advance = d->style->size_px * 2;
                }
                prefix[i + 1] = prefix[i] + work[i].advance;
            }
            for (u32 line = start; line < end;) {
                u32 stop = end, last_break = line, last_cluster = line;
                if (d->max_width > 0)
                    for (u32 i = line; i < end; ++i) {
                        if (prefix[i + 1] - prefix[line] > d->max_width && last_cluster > line) {
                            stop = last_break > line ? last_break : last_cluster;
                            break;
                        }
                        if (work[i + 1].boundary && graphemes[i + 1]) {
                            last_cluster = i + 1;
                            if (breaks[i + 1])
                                last_break = i + 1;
                        }
                    }
                if (stop <= line)
                    stop = end;
                u32 visible = stop;
                while (visible > line && (cp[visible - 1] == ' ' || cp[visible - 1] == '\t'))
                    --visible;
                f32 width = prefix[visible] - prefix[line];
                f32 extent = d->max_width > 0 ? d->max_width : prefix[end] - prefix[start];
                place_line(l, bidi.paragraph, start, line, stop, visible, width, extent,
                           stop == end, cp, offsets, work);
                line = stop;
            }
            de__text_bidi_free(&bidi);
        }
        if (end == count)
            break;
        start = end + 1;
        if (cp[end] == '\r' && start < count && cp[start] == '\n')
            ++start;
    }
    l->size.y = l->line_count * l->line_height;
    de__text_layout_quads_build(t, l);
    *out = l;
done:
    if (integers)
        DE_FREE(l->alloc, integers, capacity * 2 * sizeof(u32));
    if (breaks)
        DE_FREE(l->alloc, breaks, capacity * 2);
    if (work)
        DE_FREE(l->alloc, work, capacity * sizeof *work);
    if (prefix)
        DE_FREE(l->alloc, prefix, capacity * sizeof *prefix);
    if (result != DE_OK)
        de_text_layout_free(l);
    return result;
}
