#include "layout_internal.h"
#include <math.h>
typedef struct flex_item {
    u32 index;
    f32 main, cross, basis, before, after, cross_before, cross_after, minimum, maximum, grow,
        shrink;
    de_ui_align align;
    bool cross_auto, frozen;
} flex_item;
static f32 clamp(f32 value, f32 low, f32 high) {
    return fmaxf(low, fminf(value, fmaxf(low, high)));
}
void de_ui__flex(de_ui_layout *layout, u32 index, de_vec4 content) {
    de_ui *ui = layout->ui;
    de_ui_record *parent = &ui->records[index];
    DeUiNode *node = &parent->node;
    bool row = node->direction == DE_UI_ROW || node->direction == DE_UI_ROW_REVERSE,
         reverse = node->direction == DE_UI_ROW_REVERSE || node->direction == DE_UI_COLUMN_REVERSE;
    u32 capacity = 0;
    for (u32 c = parent->first_child; c != DE_UI_NONE_INDEX; c = ui->records[c].next_sibling)
        ++capacity;
    if (!capacity)
        return;
    flex_item *items =
        DE_ALLOC(&layout->allocator, (u64)capacity * sizeof *items, _Alignof(flex_item), DE_MEM_UI);
    if (!items)
        return;
    u32 count = 0;
    f32 extent = row ? content.z : content.w, cross_extent = row ? content.w : content.z,
        gap = node->gap * ui->scale;
    for (u32 c = parent->first_child; c != DE_UI_NONE_INDEX; c = ui->records[c].next_sibling) {
        de_ui_record *record = &ui->records[c];
        DeUiNode *child = &record->node;
        if (child->display == DE_UI_NONE) {
            de_ui__arrange(layout, c, (de_vec4){0}, false);
            continue;
        }
        if (de_entity_has(ui->world, record->entity, ui->ids.popup))
            continue;
        de_vec4 absolute;
        if (de_ui__absolute(layout, c, content, &absolute)) {
            de_ui__arrange(layout, c, absolute, false);
            continue;
        }
        de_vec2 preferred = de_ui__measure(layout, c, (de_vec2){content.z, content.w});
        f32 margin[4];
        for (u32 edge = 0; edge < 4; ++edge)
            margin[edge] = de_ui__length(child->margin[edge], (edge & 1) ? content.z : content.w,
                                         ui->scale, 0);
        u32 before = row ? (reverse ? 1 : 3) : (reverse ? 2 : 0),
            after = row ? (reverse ? 3 : 1) : (reverse ? 0 : 2);
        flex_item item = {.index = c,
                          .main = row ? preferred.x : preferred.y,
                          .cross = row ? preferred.y : preferred.x,
                          .before = margin[before],
                          .after = margin[after],
                          .cross_before = margin[row ? 0 : 3],
                          .cross_after = margin[row ? 2 : 1],
                          .grow = fmaxf(0, child->grow),
                          .shrink = fmaxf(0, child->shrink),
                          .align = child->align_self == DE_UI_ALIGN_AUTO ? node->align_items
                                                                         : child->align_self,
                          .cross_auto = (row ? child->height : child->width).unit == DE_UI_AUTO};
        item.minimum = fmaxf(
            0, de_ui__length(row ? child->min_width : child->min_height, extent, ui->scale, 0));
        item.maximum =
            de_ui__length(row ? child->max_width : child->max_height, extent, ui->scale, INFINITY);
        item.main = clamp(item.main, item.minimum, item.maximum);
        item.basis = item.main;
        items[count++] = item;
    }
    if (!count)
        return;
    f32 used = (count - 1) * gap;
    for (u32 i = 0; i < count; ++i)
        used += items[i].main + items[i].before + items[i].after;
    f32 remaining = extent - used;
    bool growing = remaining >= 0;
    for (u32 iteration = 0; iteration < count && fabsf(remaining) > .0001f; ++iteration) {
        f32 total = 0;
        for (u32 i = 0; i < count; ++i)
            if (!items[i].frozen)
                total += growing ? items[i].grow : items[i].shrink * items[i].basis;
        if (total <= 0)
            break;
        f32 consumed = 0;
        bool clamped = false;
        for (u32 i = 0; i < count; ++i) {
            flex_item *item = &items[i];
            if (item->frozen)
                continue;
            f32 weight = growing ? item->grow : item->shrink * item->basis;
            f32 proposed = item->main + remaining * weight / total,
                value = clamp(proposed, item->minimum, item->maximum);
            consumed += value - item->main;
            item->main = value;
            if (value != proposed) {
                item->frozen = true;
                clamped = true;
            }
        }
        remaining -= consumed;
        if (!clamped)
            break;
    }
    used = (count - 1) * gap;
    for (u32 i = 0; i < count; ++i)
        used += items[i].main + items[i].before + items[i].after;
    f32 free = fmaxf(0, extent - used), pen = 0;
    if (node->justify == DE_UI_ALIGN_CENTER)
        pen = free * .5f;
    else if (node->justify == DE_UI_ALIGN_END)
        pen = free;
    else if (node->justify == DE_UI_ALIGN_SPACE_BETWEEN && count > 1)
        gap += free / (count - 1);
    else if (node->justify == DE_UI_ALIGN_SPACE_AROUND) {
        f32 extra = free / count;
        gap += extra;
        pen = extra * .5f;
    }
    for (u32 i = 0; i < count; ++i) {
        flex_item *item = &items[i];
        DeUiNode *child = &ui->records[item->index].node;
        /* Re-measure text at its distributed width so AUTO height follows wrapping. */
        f32 cross_space = fmaxf(0, cross_extent - item->cross_before - item->cross_after);
        de_vec2 available =
            row ? (de_vec2){item->main, cross_space} : (de_vec2){cross_space, item->main};
        de_vec2 measured = de_ui__measure(layout, item->index, available);
        item->cross = row ? measured.y : measured.x;
        if (item->align == DE_UI_ALIGN_STRETCH && item->cross_auto)
            item->cross = cross_space;
        f32 cross_min = de_ui__length(row ? child->min_height : child->min_width, cross_extent,
                                      ui->scale, 0),
            cross_max = de_ui__length(row ? child->max_height : child->max_width, cross_extent,
                                      ui->scale, INFINITY);
        item->cross = clamp(item->cross, fmaxf(0, cross_min), cross_max);
        f32 cross = item->cross_before, extra = cross_space - item->cross;
        if (item->align == DE_UI_ALIGN_CENTER)
            cross += extra * .5f;
        else if (item->align == DE_UI_ALIGN_END)
            cross += extra;
        f32 main = reverse ? extent - pen - item->before - item->main : pen + item->before;
        de_vec4 rect =
            row ? (de_vec4){content.x + main, content.y + cross, item->main, item->cross}
                : (de_vec4){content.x + cross, content.y + main, item->cross, item->main};
        de_ui__arrange(layout, item->index, rect, false);
        pen += item->before + item->main + item->after + gap;
    }
}
