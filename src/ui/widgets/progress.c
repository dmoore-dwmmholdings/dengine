#include "widgets_internal.h"
#include "ui/render/draw_list.h"
#include <math.h>
static de_vec4 inset(de_vec4 rect, f32 amount) {
    rect.x += amount;
    rect.y += amount;
    rect.z = fmaxf(0, rect.z - 2 * amount);
    rect.w = fmaxf(0, rect.w - 2 * amount);
    return rect;
}
static void widget_draw(de_ui *ui, de_entity entity, de_vec4 rect, de_ui_draw_emitter *emitter,
                        void *user) {
    (void)user;
    const DeUiStyle *style = de_entity_get_const(ui->world, entity, ui->ids.style);
    de_vec4 color = style ? style->text_color : (de_vec4){.1f, .3f, 1, 1};
    f32 border = style ? style->border_width * ui->scale : 0,
        radius = style ? style->corner_radius * ui->scale : 0;
    rect = inset(rect, border);
    const DeUiProgress *progress = de_entity_get_const(ui->world, entity, ui->widgets->progress);
    if (progress) {
        f32 value = isfinite(progress->value) ? fmaxf(0, fminf(1, progress->value)) : 0;
        if (progress->indeterminate) {
            f32 width = rect.z * .25f,
                offset = fmodf(ui->widgets->time * .6f, 1) * (rect.z + width) - width;
            f32 start = fmaxf(0, offset), end = fminf(rect.z, offset + width);
            rect.x += start;
            rect.z = fmaxf(0, end - start);
        } else
            rect.z *= value;
        if (rect.z > 0)
            de_ui__emit_rect(emitter, rect, color, (de_vec4){0}, 0, radius);
        return;
    }
    const DeUiToggle *toggle = de_entity_get_const(ui->world, entity, ui->widgets->toggle);
    if (toggle) {
        f32 diameter = fmaxf(0, rect.w - 4 * ui->scale),
            travel = fmaxf(0, rect.z - diameter - 4 * ui->scale);
        de_vec4 knob = {rect.x + 2 * ui->scale + (toggle->on ? travel : 0), rect.y + 2 * ui->scale,
                        diameter, diameter};
        if (!toggle->on)
            color = (de_vec4){.55f, .61f, .7f, 1};
        de_ui__emit_rect(emitter, knob, color, (de_vec4){0}, 0, diameter * .5f);
        return;
    }
    const DeUiSlider *slider = de_entity_get_const(ui->world, entity, ui->widgets->slider);
    const DeUiNode *node = de_entity_get_const(ui->world, entity, ui->ids.node);
    if (slider && node) {
        f32 range = slider->max - slider->min,
            ratio = isfinite(range) && range > 0 ? (slider->value - slider->min) / range : 0;
        ratio = isfinite(ratio) ? fmaxf(0, fminf(1, ratio)) : 0;
        bool vertical = node->direction == DE_UI_COLUMN || node->direction == DE_UI_COLUMN_REVERSE,
             reverse =
                 node->direction == DE_UI_ROW_REVERSE || node->direction == DE_UI_COLUMN_REVERSE;
        de_vec4 fill = rect;
        if (vertical) {
            fill.w *= ratio;
            if (reverse)
                fill.y = rect.y + rect.w - fill.w;
        } else {
            fill.z *= ratio;
            if (reverse)
                fill.x = rect.x + rect.z - fill.z;
        }
        de_ui__emit_rect(emitter, fill, color, (de_vec4){0}, 0, radius);
        f32 size = vertical ? rect.z : rect.w, center = reverse ? 1 - ratio : ratio;
        de_vec4 knob =
            vertical ? (de_vec4){rect.x, rect.y + center * fmaxf(0, rect.w - size), size, size}
                     : (de_vec4){rect.x + center * fmaxf(0, rect.z - size), rect.y, size, size};
        de_ui__emit_rect(emitter, knob, (de_vec4){.85f, .9f, .98f, 1}, (de_vec4){0}, 0, size * .5f);
    }
}
void de_ui__widgets_register_draw(de_ui *ui) {
    de_ui__set_widget_draw(ui, widget_draw, NULL);
}
