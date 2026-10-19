#include "layout_internal.h"
#include <math.h>
f32 de_ui__length(de_ui_len length, f32 available, f32 scale, f32 automatic) {
    if (!isfinite(length.value))
        return automatic;
    if (length.unit == DE_UI_PX)
        return length.value * scale;
    if (length.unit == DE_UI_PERCENT)
        return isfinite(available) ? available * length.value * .01f : automatic;
    return automatic;
}
static f32 clamp_axis(f32 value, de_ui_len min, de_ui_len max, f32 available, f32 scale) {
    f32 lo = de_ui__length(min, available, scale, 0),
        hi = de_ui__length(max, available, scale, INFINITY);
    if (hi < lo)
        hi = lo;
    return fmaxf(0, fminf(fmaxf(value, lo), hi));
}
de_vec2 de_ui__measure_text(de_ui *ui, de_entity entity, de_vec2 available) {
    const DeUiText *text = de_entity_get_const(ui->world, entity, ui->ids.text);
    if (!text || !ui->desc.text || !text->style.font.id)
        return (de_vec2){0};
    de_text_style style = text->style;
    style.size_px = (style.size_px > 0 ? style.size_px : 16) * ui->scale;
    style.letter_spacing *= ui->scale;
    de_text_layout_desc desc = {.utf8 = de_ui__text_content(ui, text),
                                .style = &style,
                                .max_width =
                                    available.x > 0 && isfinite(available.x) ? available.x : 0,
                                .align = text->align,
                                .direction = DE_DIR_AUTO};
    de_text_layout *layout = NULL;
    if (de_text_layout_build(ui->desc.text, &desc, ui->alloc, &layout) != DE_OK)
        return (de_vec2){0};
    de_vec2 size = de_text_layout_size(layout);
    de_text_layout_free(layout);
    return size;
}
de_vec2 de_ui__measure_image(de_ui *ui, de_entity entity, de_vec2 available) {
    (void)available;
#if DE_HAS_RENDER
    const DeUiImage *image = de_entity_get_const(ui->world, entity, ui->ids.image);
    if (image && ui->desc.renderer) {
        u32 width = 0, height = 0;
        if (de_render_texture_size(ui->desc.renderer, image->texture, &width, &height))
            return (de_vec2){width * fabsf(image->uv_rect.z - image->uv_rect.x) * ui->scale,
                             height * fabsf(image->uv_rect.w - image->uv_rect.y) * ui->scale};
    }
#else
    (void)ui;
    (void)entity;
#endif
    return (de_vec2){0};
}
de_vec2 de_ui__measure(de_ui_layout *layout, u32 index, de_vec2 available) {
    de_ui *ui = layout->ui;
    de_ui_record *record = &ui->records[index];
    const DeUiNode *node = &record->node;
    if (node->display == DE_UI_NONE)
        return (de_vec2){0};
    if (!record->measure_dirty && record->measured_available.x == available.x &&
        record->measured_available.y == available.y)
        return record->intrinsic;
    ++ui->stats.nodes_measured;
    f32 top = de_ui__length(node->padding[0], available.y, ui->scale, 0),
        right = de_ui__length(node->padding[1], available.x, ui->scale, 0),
        bottom = de_ui__length(node->padding[2], available.y, ui->scale, 0),
        left = de_ui__length(node->padding[3], available.x, ui->scale, 0);
    const DeUiStyle *style = de_entity_get_const(ui->world, record->entity, ui->ids.style);
    f32 border = style ? style->border_width * ui->scale : 0;
    top += border;
    right += border;
    bottom += border;
    left += border;
    f32 width = de_ui__length(node->width, available.x, ui->scale, available.x),
        height = de_ui__length(node->height, available.y, ui->scale, available.y);
    width = clamp_axis(width, node->min_width, node->max_width, available.x, ui->scale);
    height = clamp_axis(height, node->min_height, node->max_height, available.y, ui->scale);
    de_vec2 content = {fmaxf(0, width - left - right), fmaxf(0, height - top - bottom)},
            intrinsic = {0};
    for (u32 i = 0; i < ui->measure_count; ++i)
        if (de_entity_has(ui->world, record->entity, ui->measures[i].component)) {
            de_vec2 size = ui->measures[i].measure(ui, record->entity, content);
            intrinsic.x = fmaxf(intrinsic.x, size.x);
            intrinsic.y = fmaxf(intrinsic.y, size.y);
        }
    de_vec2 children = {0};
    u32 count = 0;
    bool row = node->direction == DE_UI_ROW || node->direction == DE_UI_ROW_REVERSE;
    for (u32 child = record->first_child; child != DE_UI_NONE_INDEX;
         child = ui->records[child].next_sibling) {
        de_ui_record *r = &ui->records[child];
        if (r->node.display == DE_UI_NONE || de_entity_has(ui->world, r->entity, ui->ids.anchor) ||
            de_entity_has(ui->world, r->entity, ui->ids.popup))
            continue;
        de_vec2 size = de_ui__measure(layout, child, content);
        f32 mt = de_ui__length(r->node.margin[0], content.y, ui->scale, 0),
            mr = de_ui__length(r->node.margin[1], content.x, ui->scale, 0),
            mb = de_ui__length(r->node.margin[2], content.y, ui->scale, 0),
            ml = de_ui__length(r->node.margin[3], content.x, ui->scale, 0);
        if (node->display == DE_UI_GRID) {
            children.x = fmaxf(children.x, size.x + ml + mr);
            children.y = fmaxf(children.y, size.y + mt + mb);
        } else if (row) {
            children.x += size.x + ml + mr;
            children.y = fmaxf(children.y, size.y + mt + mb);
        } else {
            children.y += size.y + mt + mb;
            children.x = fmaxf(children.x, size.x + ml + mr);
        }
        ++count;
    }
    if (node->display == DE_UI_GRID)
        children = de_ui__grid_measure(layout, index, content);
    else if (count > 1) {
        if (row)
            children.x += (count - 1) * node->gap * ui->scale;
        else
            children.y += (count - 1) * node->gap * ui->scale;
    }
    intrinsic.x = fmaxf(intrinsic.x, children.x) + left + right;
    intrinsic.y = fmaxf(intrinsic.y, children.y) + top + bottom;
    intrinsic.x = clamp_axis(de_ui__length(node->width, available.x, ui->scale, intrinsic.x),
                             node->min_width, node->max_width, available.x, ui->scale);
    intrinsic.y = clamp_axis(de_ui__length(node->height, available.y, ui->scale, intrinsic.y),
                             node->min_height, node->max_height, available.y, ui->scale);
    record->intrinsic = intrinsic;
    record->measured_available = available;
    record->measure_dirty = false;
    return intrinsic;
}
