#include "advanced_internal.h"
#include <math.h>
static f32 clamp(f32 x, f32 hi) {
    return isfinite(x) ? fmaxf(0, fminf(x, fmaxf(0, hi))) : 0;
}
static bool inside(de_vec4 r, de_vec2 p) {
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.z && p.y < r.y + r.w;
}
static de_vec2 limit(de_ui *ui, de_entity e) {
    const DeUiNode *n = de_entity_get_const(ui->world, e, ui->ids.node);
    const de_ui_scroll_state *s = de_entity_get_const(ui->world, e, ui->advanced->scroll_state);
    const DeUiScroll *v = de_entity_get_const(ui->world, e, ui->advanced->scroll);
    if (!n || !s || !v)
        return (de_vec2){0};
    return (de_vec2){
        v->horizontal
            ? fmaxf(0, s->extent.x - n->computed_rect.z + (v->vertical ? 10 * ui->scale : 0))
            : 0,
        v->vertical
            ? fmaxf(0, s->extent.y - n->computed_rect.w + (v->horizontal ? 10 * ui->scale : 0))
            : 0};
}
void de_ui_scroll_to(de_ui *ui, de_entity e, de_vec2 offset) {
    if (!ui || !ui->world || !ui->advanced)
        return;
    const DeUiScroll *old = de_entity_get_const(ui->world, e, ui->advanced->scroll);
    if (!old)
        return;
    de_vec2 max = limit(ui, e);
    offset.x = clamp(offset.x, max.x);
    offset.y = clamp(offset.y, max.y);
    if (old->offset.x == offset.x && old->offset.y == offset.y)
        return;
    DeUiScroll *v = de_entity_get(ui->world, e, ui->advanced->scroll);
    v->offset = offset;
    de_ui__dirty(ui, de_ui__find(ui, e));
}
void de_ui__scroll_content(de_ui *ui, de_entity e, de_vec4 *content) {
    if (!ui->advanced)
        return;
    const DeUiScroll *v = de_entity_get_const(ui->world, e, ui->advanced->scroll);
    if (!v)
        return;
    content->x -= v->offset.x;
    content->y -= v->offset.y;
    if (v->vertical)
        content->z = fmaxf(0, content->z - 10 * ui->scale);
    if (v->horizontal)
        content->w = fmaxf(0, content->w - 10 * ui->scale);
}
void de_ui__scroll_prepare(de_ui *ui, de_entity e, f32 dt) {
    de_ui_advanced *a = ui->advanced;
    if (!de_entity_has(ui->world, e, a->scroll_state))
        de_entity_add(ui->world, e, a->scroll_state);
    de_ui__advanced_tag(ui, e, DE_UI_WIDGET_SCROLL);
    DeUiNode *n = de_entity_get(ui->world, e, ui->ids.node);
    n->clip = true;
    const DeUiScroll *v = de_entity_get_const(ui->world, e, a->scroll);
    de_vec2 offset = v->offset, extent = {0};
    const DeUiList *list = de_entity_get_const(ui->world, e, a->list);
    if (list)
        extent.y = (f32)list->item_count * fmaxf(1, list->item_height) * ui->scale;
    else
        for (de_entity c = de_entity_children_first(ui->world, e); c.bits;
             c = de_entity_children_next(ui->world, c)) {
            const DeUiNode *child = de_entity_get_const(ui->world, c, ui->ids.node);
            if (!child || child->display == DE_UI_NONE)
                continue;
            extent.x = fmaxf(extent.x, child->computed_rect.x + child->computed_rect.z -
                                           n->computed_rect.x + offset.x);
            extent.y = fmaxf(extent.y, child->computed_rect.y + child->computed_rect.w -
                                           n->computed_rect.y + offset.y);
            if (child->shrink != 0) {
                DeUiNode *w = de_entity_get(ui->world, c, ui->ids.node);
                w->shrink = 0;
            }
        }
    de_ui_scroll_state *s = de_entity_get(ui->world, e, a->scroll_state);
    s->extent = extent;
    if (dt > 0 && (fabsf(s->velocity.x) > .1f || fabsf(s->velocity.y) > .1f) && !s->dragging) {
        offset.x += s->velocity.x * dt;
        offset.y += s->velocity.y * dt;
        f32 decay = expf(-12 * dt);
        s->velocity.x *= decay;
        s->velocity.y *= decay;
    }
    de_ui_scroll_to(ui, e, offset);
}
static de_vec4 bar(de_ui *ui, de_entity e, bool vertical) {
    const DeUiNode *n = de_entity_get_const(ui->world, e, ui->ids.node);
    const DeUiScroll *v = de_entity_get_const(ui->world, e, ui->advanced->scroll);
    const de_ui_scroll_state *s = de_entity_get_const(ui->world, e, ui->advanced->scroll_state);
    de_vec4 r = n->computed_rect;
    f32 gutter = 10 * ui->scale;
    f32 view = vertical ? r.w : r.z, extent = vertical ? s->extent.y : s->extent.x,
        offset = vertical ? v->offset.y : v->offset.x;
    f32 track = fmaxf(0, view - (vertical ? v->horizontal : v->vertical) * gutter),
        size = fminf(track, fmaxf(16 * ui->scale, track * view / fmaxf(view, extent)));
    f32 max = vertical ? limit(ui, e).y : limit(ui, e).x,
        pos = max > 0 ? offset / max * (track - size) : 0;
    return vertical ? (de_vec4){r.x + r.z - gutter, r.y + pos, gutter, size}
                    : (de_vec4){r.x + pos, r.y + r.w - gutter, size, gutter};
}
void de_ui__scroll_frame(de_ui *ui, de_entity e, f32 dt) {
    de_input *in = ui->desc.input;
    if (!in || !de_ui__widget_enabled(ui, e))
        return;
    de_ui_advanced *a = ui->advanced;
    const DeUiScroll *v = de_entity_get_const(ui->world, e, a->scroll);
    if (!v || !de_entity_has(ui->world, e, a->scroll_state))
        return;
    const DeUiNode *n = de_entity_get_const(ui->world, e, ui->ids.node);
    de_vec2 p = ui->widgets->pointer.position, offset = v->offset;
    de_ui_scroll_state *s = de_entity_get(ui->world, e, a->scroll_state);
    if (inside(n->computed_rect, p)) {
        de_vec2 wheel = de_mouse_wheel(in);
        offset.x -= wheel.x * 40 * ui->scale;
        offset.y -= wheel.y * 40 * ui->scale;
        if (wheel.x || wheel.y) {
            s->velocity = (de_vec2){-wheel.x * 80 * ui->scale, -wheel.y * 80 * ui->scale};
            ui->widgets->pointer.consumed = true;
        }
    }
    if (de_mouse_pressed(in, DE_MOUSE_LEFT)) {
        if (v->vertical && inside(bar(ui, e, true), p))
            s->dragging = 1;
        else if (v->horizontal && inside(bar(ui, e, false), p))
            s->dragging = 2;
        if (s->dragging) {
            s->drag_start = s->dragging == 1 ? p.y : p.x;
            s->offset_start = s->dragging == 1 ? offset.y : offset.x;
            s->velocity = (de_vec2){0};
        }
    }
    if (s->dragging && de_mouse_down(in, DE_MOUSE_LEFT)) {
        bool vertical = s->dragging == 1;
        de_vec4 b = bar(ui, e, vertical);
        f32 view = vertical ? n->computed_rect.w : n->computed_rect.z, knob = vertical ? b.w : b.z,
            max = vertical ? limit(ui, e).y : limit(ui, e).x;
        f32 value = s->offset_start +
                    ((vertical ? p.y : p.x) - s->drag_start) * max / fmaxf(1, view - knob);
        if (vertical)
            offset.y = value;
        else
            offset.x = value;
        ui->widgets->pointer.consumed = true;
    }
    if (!de_mouse_down(in, DE_MOUSE_LEFT))
        s->dragging = 0;
    de_ui_scroll_to(ui, e, offset);
    (void)dt;
}
void de_ui__scroll_draw(de_ui *ui, de_entity e, de_vec4 r, de_ui_draw_emitter *em) {
    const DeUiScroll *v = de_entity_get_const(ui->world, e, ui->advanced->scroll);
    if (!v || !de_entity_has(ui->world, e, ui->advanced->scroll_state))
        return;
    de_vec4 track = {.08f, .1f, .15f, 1}, thumb = {.35f, .42f, .55f, 1};
    f32 g = 10 * ui->scale;
    if (v->vertical) {
        de_ui__emit_rect(em, (de_vec4){r.x + r.z - g, r.y, g, r.w}, track, (de_vec4){0}, 0, 0);
        de_ui__emit_rect(em, bar(ui, e, true), thumb, (de_vec4){0}, 0, 3 * ui->scale);
    }
    if (v->horizontal) {
        de_ui__emit_rect(em, (de_vec4){r.x, r.y + r.w - g, r.z, g}, track, (de_vec4){0}, 0, 0);
        de_ui__emit_rect(em, bar(ui, e, false), thumb, (de_vec4){0}, 0, 3 * ui->scale);
    }
}
void de_ui__scroll_into_view(de_ui *ui, de_entity container, de_entity child) {
    const DeUiNode *c = de_entity_get_const(ui->world, container, ui->ids.node),
                   *n = de_entity_get_const(ui->world, child, ui->ids.node);
    const DeUiScroll *v = de_entity_get_const(ui->world, container, ui->advanced->scroll);
    if (!c || !n || !v)
        return;
    de_vec2 p = v->offset;
    f32 right = c->computed_rect.x + c->computed_rect.z - (v->vertical ? 10 * ui->scale : 0),
        bottom = c->computed_rect.y + c->computed_rect.w - (v->horizontal ? 10 * ui->scale : 0);
    if (n->computed_rect.x < c->computed_rect.x)
        p.x += n->computed_rect.x - c->computed_rect.x;
    else if (n->computed_rect.x + n->computed_rect.z > right)
        p.x += n->computed_rect.x + n->computed_rect.z - right;
    if (n->computed_rect.y < c->computed_rect.y)
        p.y += n->computed_rect.y - c->computed_rect.y;
    else if (n->computed_rect.y + n->computed_rect.w > bottom)
        p.y += n->computed_rect.y + n->computed_rect.w - bottom;
    de_ui_scroll_to(ui, container, p);
}
