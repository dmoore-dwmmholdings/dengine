#include "advanced_internal.h"
#include <math.h>
void de_ui__list_prepare(de_ui *ui, de_entity e) {
    de_ui_advanced *a = ui->advanced;
    if (!de_entity_has(ui->world, e, a->scroll)) {
        DeUiScroll *v = de_entity_add(ui->world, e, a->scroll);
        v->vertical = true;
    }
    if (!de_entity_has(ui->world, e, a->list_state))
        de_entity_add(ui->world, e, a->list_state);
    de_ui__advanced_tag(ui, e, DE_UI_WIDGET_LIST);
    const DeUiList *p = de_entity_get_const(ui->world, e, a->list);
    DeUiList list = *p;
    const DeUiNode *n = de_entity_get_const(ui->world, e, ui->ids.node);
    const DeUiScroll *v = de_entity_get_const(ui->world, e, a->scroll);
    f32 height = isfinite(list.item_height) && list.item_height > 0 ? list.item_height : 1;
    f32 pixels = height * ui->scale;
    u32 visible = (u32)ceilf(fmaxf(0, n->computed_rect.w) / pixels);
    u32 needed = list.item_count < visible + 2 ? list.item_count : visible + 2;
    /* Capacity is bounded by the UI node budget, never by the data item count. */
    if (needed > ui->capacity)
        needed = ui->capacity;
    u32 count = 0;
    for (de_entity c = de_entity_children_first(ui->world, e); c.bits;) {
        de_entity next = de_entity_children_next(ui->world, c);
        const de_ui_item_state *row = de_entity_get_const(ui->world, c, a->item);
        if (row && row->owner.bits == e.bits) {
            if (count >= needed)
                de_entity_despawn_recursive(ui->world, c);
            else
                ++count;
        }
        c = next;
    }
    while (count < needed) {
        de_entity row = {0};
        if (list.item_template.id && ui->desc.assets)
            de_prefab_instantiate(ui->desc.assets, ui->world, list.item_template, e, &row);
        else
            row = de_ui__advanced_node(ui, e, 0, height);
        if (!row.bits)
            break;
        if (!de_entity_has(ui->world, row, ui->ids.node))
            de_entity_add(ui->world, row, ui->ids.node);
        de_ui_item_state *state = de_entity_add(ui->world, row, a->item);
        *state = (de_ui_item_state){.owner = e, .slot = count};
        ++count;
    }
    u32 first = (u32)floorf(fmaxf(0, v->offset.y) / pixels);
    if (first)
        --first;
    if (first > list.item_count - count)
        first = list.item_count - count;
    for (de_entity c = de_entity_children_first(ui->world, e); c.bits;
         c = de_entity_children_next(ui->world, c)) {
        const de_ui_item_state *old = de_entity_get_const(ui->world, c, a->item);
        if (!old || old->owner.bits != e.bits || !count)
            continue;
        u32 index = first + (old->slot + count - first % count) % count;
        bool bind = !old->bound || old->index != index;
        if (bind) {
            de_ui_item_state *row = de_entity_get(ui->world, c, a->item);
            row->index = index;
            row->bound = true;
        }
        if (!de_entity_has(ui->world, c, ui->ids.anchor))
            de_entity_add(ui->world, c, ui->ids.anchor);
        DeUiAnchor *anchor = de_entity_get(ui->world, c, ui->ids.anchor);
        *anchor = (DeUiAnchor){.anchor_max = {1, 0},
                               .offset_min = {0, (f32)index * height},
                               .offset_max = {0, (f32)(index + 1) * height}};
        DeUiNode *node = de_entity_get(ui->world, c, ui->ids.node);
        node->height = (de_ui_len){DE_UI_PX, height};
        node->shrink = 0;
        if (bind)
            de_ui__widget_event(ui, DE_UI_EV_BIND_ITEM, c, (de_name){0}, (f32)index);
    }
    de_ui_list_state *state = de_entity_get(ui->world, e, a->list_state);
    state->count = count;
    state->height = pixels;
}
