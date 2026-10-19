#include "layout_internal.h"
#include <math.h>
#include <string.h>
static bool equal(de_vec4 a, de_vec4 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
static void publish(de_ui *ui, de_ui_record *record, de_vec4 rect) {
    if (!equal(rect, record->node.computed_rect)) {
        DeUiNode *node = de_entity_get(ui->world, record->entity, ui->ids.node);
        if (node)
            node->computed_rect = rect;
        record->node.computed_rect = rect;
    }
}
static void hide(de_ui_layout *layout, u32 index) {
    de_ui *ui = layout->ui;
    de_ui_record *r = &ui->records[index];
    if (r->dirty || !equal(r->node.computed_rect, (de_vec4){0}))
        ++ui->stats.nodes_laid_out;
    publish(ui, r, (de_vec4){0});
    r->dirty = false;
    for (u32 child = r->first_child; child != DE_UI_NONE_INDEX;
         child = ui->records[child].next_sibling)
        hide(layout, child);
}
void de_ui__arrange(de_ui_layout *layout, u32 index, de_vec4 rect, bool force) {
    de_ui *ui = layout->ui;
    de_ui_record *record = &ui->records[index];
    DeUiNode *node = &record->node;
    if (node->display == DE_UI_NONE) {
        hide(layout, index);
        return;
    }
    if (node->safe_area) {
        f32 x = fmaxf(rect.x, (f32)ui->safe.left), y = fmaxf(rect.y, (f32)ui->safe.top),
            right = fminf(rect.x + rect.z, (f32)ui->width - ui->safe.right),
            bottom = fminf(rect.y + rect.w, (f32)ui->height - ui->safe.bottom);
        rect = (de_vec4){x, y, fmaxf(0, right - x), fmaxf(0, bottom - y)};
    }
    f32 right = roundf(rect.x + fmaxf(0, rect.z)), bottom = roundf(rect.y + fmaxf(0, rect.w));
    rect.x = roundf(rect.x);
    rect.y = roundf(rect.y);
    rect.z = fmaxf(0, right - rect.x);
    rect.w = fmaxf(0, bottom - rect.y);
    bool changed = !equal(rect, node->computed_rect);
    if (!force && !record->dirty && !changed)
        return;
    ++ui->stats.nodes_laid_out;
    publish(ui, record, rect);
    record->dirty = false;
    f32 top = de_ui__length(node->padding[0], rect.w, ui->scale, 0),
        pr = de_ui__length(node->padding[1], rect.z, ui->scale, 0),
        pb = de_ui__length(node->padding[2], rect.w, ui->scale, 0),
        left = de_ui__length(node->padding[3], rect.z, ui->scale, 0);
    const DeUiStyle *style = de_entity_get_const(ui->world, record->entity, ui->ids.style);
    f32 border = style ? style->border_width * ui->scale : 0;
    top += border;
    pr += border;
    pb += border;
    left += border;
    de_vec4 content = {rect.x + left, rect.y + top, fmaxf(0, rect.z - left - pr),
                       fmaxf(0, rect.w - top - pb)};
    if (node->display == DE_UI_GRID)
        de_ui__grid(layout, index, content);
    else
        de_ui__flex(layout, index, content);
}
