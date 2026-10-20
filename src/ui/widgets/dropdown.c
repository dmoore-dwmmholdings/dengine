#include "advanced_internal.h"
#include <math.h>
void de_ui__dropdown_cancel(de_ui *ui, de_entity e) {
    de_ui_popup_state *s = de_entity_get(ui->world, e, ui->advanced->popup);
    if (!s)
        return;
    de_entity popup = s->popup;
    s->popup = (de_entity){0};
    s->open = false;
    if (de_entity_alive(ui->world, popup))
        de_entity_despawn_recursive(ui->world, popup);
}
void de_ui__dropdown_submit(de_ui *ui, de_entity e) {
    de_ui_advanced *a = ui->advanced;
    const DeUiDropdown *source = de_entity_get_const(ui->world, e, a->dropdown);
    if (!source)
        return;
    DeUiDropdown drop = *source;
    drop.option_count = drop.option_count > 32 ? 32 : drop.option_count;
    if (!de_entity_has(ui->world, e, a->popup))
        de_entity_add(ui->world, e, a->popup);
    de_ui_popup_state *s = de_entity_get(ui->world, e, a->popup);
    if (s->open) {
        de_ui__dropdown_cancel(ui, e);
        return;
    }
    const DeUiNode *n = de_entity_get_const(ui->world, e, ui->ids.node);
    f32 width = n ? n->computed_rect.z / ui->scale : 160;
    de_entity popup = de_ui__advanced_node(ui, (de_entity){0}, width, 32 * (f32)drop.option_count);
    if (!popup.bits)
        return;
    DeUiNode *pn = de_entity_get(ui->world, popup, ui->ids.node);
    pn->direction = DE_UI_COLUMN;
    /* Popup depth already places this subtree above normal controls. Keep parent/rows
     * at the same z so hierarchy order paints labels over the popup background. */
    DeUiPopup *placement = de_entity_add(ui->world, popup, ui->ids.popup);
    placement->anchor = e;
    placement->modal = true;
    f32 bottom = (f32)ui->height - ui->safe.bottom;
    placement->placement =
        n && n->computed_rect.y + n->computed_rect.w + 32 * ui->scale * drop.option_count > bottom
            ? DE_UI_POPUP_TOP
            : DE_UI_POPUP_BOTTOM;
    DeUiStyle *style = de_entity_add(ui->world, popup, ui->ids.style);
    style->background = (de_vec4){.08f, .1f, .16f, 1};
    style->border_width = 1;
    style->border_color = (de_vec4){.3f, .4f, .6f, 1};
    const DeUiText *original = de_entity_get_const(ui->world, e, ui->ids.text);
    DeUiText text = original ? *original : (DeUiText){0};
    for (u32 i = 0; i < drop.option_count; ++i) {
        de_entity row = de_ui__advanced_node(ui, popup, width, 32);
        de_entity_add(ui->world, row, a->ui->widgets->button);
        de_entity_add(ui->world, row, ui->widgets->focusable);
        DeUiNode *rn = de_entity_get(ui->world, row, ui->ids.node);
        rn->style_class = de_name_from(i == drop.selected ? "ui_option_selected" : "ui_option");
        DeUiText *label = de_entity_add(ui->world, row, ui->ids.text);
        *label = text;
        label->literal = (de_str){0};
        label->text[0] = 0;
        label->loc_id = drop.options[i];
        de_ui_item_state *item = de_entity_add(ui->world, row, a->item);
        *item = (de_ui_item_state){.owner = e, .index = i};
    }
    s = de_entity_get(ui->world, e, a->popup);
    s->popup = popup;
    s->open = true;
}
void de_ui__dropdown_prepare(de_ui *ui, de_entity e) {
    de_ui_advanced *a = ui->advanced;
    de_ui__advanced_tag(ui, e, DE_UI_WIDGET_DROPDOWN);
    if (!de_entity_has(ui->world, e, ui->widgets->focusable))
        de_entity_add(ui->world, e, ui->widgets->focusable);
    if (!de_entity_has(ui->world, e, a->popup))
        de_entity_add(ui->world, e, a->popup);
    if (!de_entity_has(ui->world, e, ui->ids.text))
        de_entity_add(ui->world, e, ui->ids.text);
    DeUiDropdown *drop = de_entity_get(ui->world, e, a->dropdown);
    drop->option_count = drop->option_count > 32 ? 32 : drop->option_count;
    if (drop->selected >= drop->option_count)
        drop->selected = drop->option_count ? drop->option_count - 1 : 0;
    DeUiText *text = de_entity_get(ui->world, e, ui->ids.text);
    text->loc_id = drop->option_count ? drop->options[drop->selected] : (de_name){0};
    const de_ui_popup_state *s = de_entity_get_const(ui->world, e, a->popup);
    if (s->open) {
        const DeUiNode *popup = de_entity_get_const(ui->world, s->popup, ui->ids.node);
        if (!popup || popup->display == DE_UI_NONE) {
            de_ui__dropdown_cancel(ui, e);
            return;
        }
    }
    if (s->open && ui->desc.input && de_mouse_pressed(ui->desc.input, DE_MOUSE_LEFT)) {
        de_entity hit = ui->widgets->pointer.hovered;
        bool own = hit.bits == e.bits;
        for (u32 depth = 0; hit.bits && depth < ui->capacity; ++depth) {
            if (hit.bits == s->popup.bits) {
                own = true;
                break;
            }
            hit = de_entity_parent(ui->world, hit);
        }
        if (!own)
            de_ui__dropdown_cancel(ui, e);
    }
}
void de_ui__dropdown_event(de_ui *ui, const de_ui_event *event) {
    if (event->kind != DE_UI_EV_CLICK)
        return;
    de_ui_advanced *a = ui->advanced;
    const de_ui_item_state *item = de_entity_get_const(ui->world, event->target, a->item);
    if (!item)
        return;
    de_entity owner = item->owner;
    u32 index = item->index;
    DeUiDropdown *drop = de_entity_get(ui->world, owner, a->dropdown);
    if (!drop || index >= drop->option_count)
        return;
    bool changed = drop->selected != index;
    drop->selected = index;
    de_ui__dropdown_cancel(ui, owner);
    if (changed)
        de_ui__widget_event(ui, DE_UI_EV_CHANGED, owner, (de_name){0}, (f32)index);
}
