#include "ui/widgets/widgets_internal.h"
#include "ui/style/stylesheet_internal.h"
#include "ui/render/draw_list.h"
#include "ui/world/world_internal.h"
#include <math.h>
static bool contains(de_vec4 r, de_vec2 p) {
    return r.z > 0 && r.w > 0 && p.x >= r.x && p.y >= r.y && p.x < r.x + r.z && p.y < r.y + r.w;
}
static de_entity activation_target(de_ui *ui, de_entity entity) {
    de_entity original = entity;
    for (u32 depth = 0; entity.bits && depth < ui->capacity; ++depth) {
        const DeUiWidgetType *tag = de_entity_get_const(ui->world, entity, ui->style->widget);
        if (tag && (u32)tag->type < DE_UI_WIDGET_TYPE_COUNT) {
            de_ui_widget_entry *entry = &ui->widgets->table.entries[tag->type];
            if (entry->submit || entry->increment || entry->decrement || entry->cancel)
                return entity;
        }
        entity = de_entity_parent(ui->world, entity);
    }
    return original;
}
de_entity de_ui__hit_test(de_ui *ui, de_entity root, de_vec2 position) {
    if (!ui || !ui->widgets || !ui->render || !isfinite(position.x) || !isfinite(position.y))
        return (de_entity){0};
    const de_ui_draw_node *nodes = NULL;
    const de_ui_inherited *inherited = NULL;
    u32 count = de_ui__draw_order(ui, &nodes, &inherited);
    for (u32 i = count; i > 0; --i) {
        const de_ui_draw_node *draw = &nodes[i - 1];
        de_entity root_entity = {0};
        de_vec4 root_rect = {0};
        bool world = false;
        if (!de_ui__root_info(ui, draw->root, &root_entity, &root_rect, &world))
            continue;
        if (root.bits ? root.bits != root_entity.bits : world)
            continue;
        de_vec2 point = position;
        if (root.bits) {
            point.x += root_rect.x;
            point.y += root_rect.y;
        }
        de_entity entity = ui->records[draw->index].entity;
        if (!de_ui__widget_enabled(ui, entity))
            continue;
        const DeUiWidgetType *tag = de_entity_get_const(ui->world, entity, ui->style->widget);
        const DeUiNode *node = de_entity_get_const(ui->world, entity, ui->ids.node);
        if (!tag || !node || !contains(node->computed_rect, point) ||
            !contains(inherited[draw->index].clip, point))
            continue;
        return activation_target(ui, entity);
    }
    return (de_entity){0};
}
static void interaction(de_ui *ui, de_entity entity, bool hovered, bool pressed) {
    if (!de_entity_alive(ui->world, entity))
        return;
    const DeUiInteractionState *old = de_entity_get_const(ui->world, entity, ui->ids.state);
    if (old && old->hovered == hovered && old->pressed == pressed)
        return;
    DeUiInteractionState *state = old ? de_entity_get(ui->world, entity, ui->ids.state)
                                      : de_entity_add(ui->world, entity, ui->ids.state);
    if (state) {
        state->hovered = hovered;
        state->pressed = pressed;
    }
}
void de_ui__pointer_route(de_ui *ui, de_entity root, de_vec2 position, bool pressed, bool released,
                          bool down) {
    if (!ui || !ui->widgets)
        return;
    de_ui_pointer_state *pointer = &ui->widgets->pointer;
    pointer->root = root;
    pointer->position = position;
    de_entity hit = de_ui__hit_test(ui, root, position);
    if (pointer->captured.bits && !de_ui__widget_enabled(ui, pointer->captured)) {
        interaction(ui, pointer->captured, false, false);
        pointer->captured = (de_entity){0};
    }
    if (hit.bits != pointer->hovered.bits) {
        if (pointer->hovered.bits) {
            interaction(ui, pointer->hovered, false,
                        pointer->captured.bits == pointer->hovered.bits && down);
            de_ui__widget_event(ui, DE_UI_EV_HOVER, pointer->hovered, (de_name){0}, 0);
        }
        pointer->hovered = hit;
        if (hit.bits)
            de_ui__widget_event(ui, DE_UI_EV_HOVER, hit, (de_name){0}, 1);
    }
    if (pressed && !pointer->captured.bits && hit.bits)
        pointer->captured = hit;
    if (hit.bits)
        interaction(ui, hit, true, pointer->captured.bits == hit.bits && down);
    if (pointer->captured.bits) {
        interaction(ui, pointer->captured, hit.bits == pointer->captured.bits, down && !released);
        if ((down || released) &&
            de_entity_has(ui->world, pointer->captured, ui->widgets->slider)) {
            de_vec2 physical = position;
            if (root.bits) {
                const DeUiNode *node = de_entity_get_const(ui->world, root, ui->ids.node);
                if (node) {
                    physical.x += node->computed_rect.x;
                    physical.y += node->computed_rect.y;
                }
            }
            de_ui__slider_drag(ui, pointer->captured, physical);
        }
    }
    pointer->consumed = hit.bits || pointer->captured.bits;
    if (released && pointer->captured.bits) {
        de_entity captured = pointer->captured;
        pointer->captured = (de_entity){0};
        if (hit.bits == captured.bits && de_ui__widget_enabled(ui, captured)) {
            const DeUiWidgetType *tag = de_entity_get_const(ui->world, captured, ui->style->widget);
            de_ui_widget_action submit = tag && (u32)tag->type < DE_UI_WIDGET_TYPE_COUNT
                                             ? ui->widgets->table.entries[tag->type].submit
                                             : NULL;
            if (submit)
                submit(ui, captured);
            else if (!de_entity_has(ui->world, captured, ui->widgets->slider))
                de_ui__widget_event(ui, DE_UI_EV_CLICK, captured, (de_name){0}, 0);
        }
    }
    pointer->down = down;
}
bool de_ui_pointer_consumed(const de_ui *ui) {
    return ui && ui->widgets && ui->widgets->pointer.consumed;
}
void de_ui__pointer_system(de_system_ctx *ctx) {
    de_ui_widgets *widgets = ctx->user;
    de_ui *ui = widgets->ui;
    widgets->pointer.consumed = false;
    if (!ui->desc.input)
        return;
    de_vec2 point = de_mouse_position(ui->desc.input);
    if (ui->desc.window.id) {
        u32 logical_w = 0, logical_h = 0, pixel_w = 0, pixel_h = 0;
        de_window_size_points(ui->desc.window, &logical_w, &logical_h);
        de_window_size_pixels(ui->desc.window, &pixel_w, &pixel_h);
        if (logical_w && logical_h) {
            point.x *= (f32)pixel_w / logical_w;
            point.y *= (f32)pixel_h / logical_h;
        }
    }
    if (de_ui__world_pointer(ui, point, de_mouse_pressed(ui->desc.input, DE_MOUSE_LEFT),
                             de_mouse_released(ui->desc.input, DE_MOUSE_LEFT),
                             de_mouse_down(ui->desc.input, DE_MOUSE_LEFT)))
        return;
    point = de_ui__screen_pointer(ui, point);
    de_ui__pointer_route(ui, (de_entity){0}, point, de_mouse_pressed(ui->desc.input, DE_MOUSE_LEFT),
                         de_mouse_released(ui->desc.input, DE_MOUSE_LEFT),
                         de_mouse_down(ui->desc.input, DE_MOUSE_LEFT));
}
