#include "focus_internal.h"
#include <math.h>
#include <float.h>
static f32 gap(f32 a, f32 size_a, f32 b, f32 size_b) {
    return fmaxf(0, fmaxf(b - a - size_a, a - b - size_b));
}
de_entity de_ui__spatial_nav(de_ui *ui, de_entity from, de_ui_nav_dir direction) {
    if (!ui || !ui->focus || direction == DE_UI_NAV_NONE)
        return DE_ENTITY_NULL;
    de_ui_focus *f = ui->focus;
    const DeUiFocusable *links = de_entity_get_const(ui->world, from, f->focusable);
    de_entity explicit_target = {0};
    if (links) {
        switch (direction) {
        case DE_UI_NAV_UP:
            explicit_target = links->up;
            break;
        case DE_UI_NAV_DOWN:
            explicit_target = links->down;
            break;
        case DE_UI_NAV_LEFT:
            explicit_target = links->left;
            break;
        case DE_UI_NAV_RIGHT:
            explicit_target = links->right;
            break;
        default:
            break;
        }
    }
    if (explicit_target.bits && de_ui__focus_allowed(ui, explicit_target))
        return explicit_target;
    const DeUiNode *source = de_entity_get_const(ui->world, from, ui->ids.node);
    if (!source)
        return de_ui__tab_nav(ui, DE_ENTITY_NULL, false);
    de_vec4 a = source->computed_rect;
    f32 best = FLT_MAX;
    de_entity result = {0};
    for (u32 i = 0; i < f->count; ++i) {
        de_entity e = f->order[i];
        if (e.bits == from.bits || !de_ui__focus_allowed(ui, e))
            continue;
        const DeUiNode *node = de_entity_get_const(ui->world, e, ui->ids.node);
        de_vec4 b = node->computed_rect;
        if (b.z <= 0 || b.w <= 0)
            continue;
        f32 dx = b.x + b.z * .5f - a.x - a.z * .5f;
        f32 dy = b.y + b.w * .5f - a.y - a.w * .5f;
        bool horizontal = direction == DE_UI_NAV_LEFT || direction == DE_UI_NAV_RIGHT;
        f32 primary = horizontal ? dx : dy;
        if (direction == DE_UI_NAV_LEFT || direction == DE_UI_NAV_UP)
            primary = -primary;
        if (primary <= 0)
            continue;
        f32 edge = horizontal ? gap(a.x, a.z, b.x, b.z) : gap(a.y, a.w, b.y, b.w);
        f32 perpendicular = horizontal ? gap(a.y, a.w, b.y, b.w) : gap(a.x, a.z, b.x, b.z);
        f32 score = edge + 2 * perpendicular;
        /* Strict improvement retains tree order on equal scores. */
        if (isfinite(score) && score < best) {
            best = score;
            result = e;
        }
    }
    return result;
}
de_entity de_ui__tab_nav(de_ui *ui, de_entity from, bool previous) {
    de_ui_focus *f = ui->focus;
    i32 from_index = 0;
    u32 from_order = UINT32_MAX;
    const DeUiFocusable *source = de_entity_get_const(ui->world, from, f->focusable);
    if (source)
        from_index = source->tab_index;
    for (u32 i = 0; i < f->count; ++i)
        if (f->order[i].bits == from.bits)
            from_order = i;
    de_entity best = {0}, wrap = {0};
    i32 best_index = 0, wrap_index = 0;
    u32 best_order = 0, wrap_order = 0;
    for (u32 i = 0; i < f->count; ++i) {
        de_entity e = f->order[i];
        const DeUiFocusable *focusable = de_entity_get_const(ui->world, e, f->focusable);
        if (!focusable || focusable->tab_index < 0 || !de_ui__focus_allowed(ui, e))
            continue;
        i32 index = focusable->tab_index;
        bool lower = index < from_index || (index == from_index && i < from_order);
        bool higher = index > from_index || (index == from_index && i > from_order);
        bool eligible = from_order == UINT32_MAX || (previous ? lower : higher);
        bool better = !best.bits ||
                      (previous ? (index > best_index || (index == best_index && i > best_order))
                                : (index < best_index || (index == best_index && i < best_order)));
        if (eligible && better) {
            best = e;
            best_index = index;
            best_order = i;
        }
        bool wrap_better =
            !wrap.bits ||
            (previous ? (index > wrap_index || (index == wrap_index && i > wrap_order))
                      : (index < wrap_index || (index == wrap_index && i < wrap_order)));
        if (wrap_better) {
            wrap = e;
            wrap_index = index;
            wrap_order = i;
        }
    }
    return best.bits ? best : wrap;
}
