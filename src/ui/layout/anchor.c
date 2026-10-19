#include "layout_internal.h"
#include "ui/world/world_internal.h"
#include <math.h>
bool de_ui__absolute(de_ui_layout *layout, u32 index, de_vec4 parent, de_vec4 *rect) {
    de_ui *ui = layout->ui;
    de_ui_record *r = &ui->records[index];
    const DeUiAnchor *a = de_entity_get_const(ui->world, r->entity, ui->ids.anchor);
    if (!a)
        return false;
    f32 left = parent.x + parent.z * a->anchor_min.x + a->offset_min.x * ui->scale,
        top = parent.y + parent.w * a->anchor_min.y + a->offset_min.y * ui->scale;
    f32 right = parent.x + parent.z * a->anchor_max.x + a->offset_max.x * ui->scale,
        bottom = parent.y + parent.w * a->anchor_max.y + a->offset_max.y * ui->scale;
    de_vec2 preferred = de_ui__measure(layout, index, (de_vec2){parent.z, parent.w});
    *rect = (de_vec4){left, top, right > left ? right - left : preferred.x,
                      bottom > top ? bottom - top : preferred.y};
    return true;
}
void de_ui__popups(de_ui_layout *layout) {
    de_ui *ui = layout->ui;
    for (u32 i = 0; i < ui->record_count; ++i) {
        de_ui_record *r = &ui->records[i];
        if (!r->alive)
            continue;
        const DeUiPopup *popup = de_entity_get_const(ui->world, r->entity, ui->ids.popup);
        if (!popup)
            continue;
        u32 root = i;
        for (u32 depth = 0; depth < ui->record_count; ++depth) {
            u32 parent = de_ui__find(ui, ui->records[root].parent);
            if (parent == DE_UI_NONE_INDEX)
                break;
            root = parent;
        }
        de_vec4 safe = {(f32)ui->safe.left, (f32)ui->safe.top,
                        fmaxf(0, (f32)ui->width - ui->safe.left - ui->safe.right),
                        fmaxf(0, (f32)ui->height - ui->safe.top - ui->safe.bottom)};
        f32 previous_scale = ui->scale;
        bool previous_world_root = layout->world_root;
        layout->world_root = de_ui__world_root_extent(ui, ui->records[root].entity, &safe);
        if (layout->world_root)
            ui->scale = 1;
        u32 anchor = de_ui__find(ui, popup->anchor);
        de_vec4 a = anchor == DE_UI_NONE_INDEX ? (de_vec4){safe.x, safe.y, 0, 0}
                                               : ui->records[anchor].node.computed_rect;
        de_vec2 size = de_ui__measure(layout, i, (de_vec2){safe.z, safe.w});
        size.x = fminf(size.x, safe.z);
        size.y = fminf(size.y, safe.w);
        de_vec4 rect = {a.x, a.y + a.w, size.x, size.y};
        if (popup->placement == DE_UI_POPUP_TOP)
            rect.y = a.y - size.y;
        else if (popup->placement == DE_UI_POPUP_LEFT) {
            rect.x = a.x - size.x;
            rect.y = a.y;
        } else if (popup->placement == DE_UI_POPUP_RIGHT) {
            rect.x = a.x + a.z;
            rect.y = a.y;
        } else if (popup->placement == DE_UI_POPUP_OVERLAY)
            rect.y = a.y;
        rect.x = fmaxf(safe.x, fminf(rect.x, safe.x + safe.z - rect.z));
        rect.y = fmaxf(safe.y, fminf(rect.y, safe.y + safe.w - rect.w));
        de_ui__arrange(layout, i, rect, false);
        ui->scale = previous_scale;
        layout->world_root = previous_world_root;
    }
}
