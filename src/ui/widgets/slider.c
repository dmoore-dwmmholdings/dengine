#include "widgets_internal.h"
#include <math.h>
static void value_set(de_ui *ui, de_entity entity, f32 value) {
    const DeUiSlider *slider = de_entity_get_const(ui->world, entity, ui->widgets->slider);
    if (!slider || !isfinite(value) || !isfinite(slider->min) || !isfinite(slider->max))
        return;
    f32 min = slider->min, max = fmaxf(min, slider->max);
    if (isfinite(slider->step) && slider->step > 0)
        value = min + roundf((value - min) / slider->step) * slider->step;
    value = fmaxf(min, fminf(max, value));
    if (value == slider->value)
        return;
    ((DeUiSlider *)de_entity_get(ui->world, entity, ui->widgets->slider))->value = value;
    de_ui__widget_event(ui, DE_UI_EV_CHANGED, entity, (de_name){0}, value);
}
static void increment(de_ui *ui, de_entity entity, f32 sign) {
    if (!de_ui__widget_enabled(ui, entity))
        return;
    const DeUiSlider *slider = de_entity_get_const(ui->world, entity, ui->widgets->slider);
    if (!slider)
        return;
    f32 step = isfinite(slider->step) && slider->step > 0
                   ? slider->step
                   : fmaxf(0, slider->max - slider->min) * .01f;
    value_set(ui, entity, slider->value + sign * step);
}
void de_ui__slider_increment(de_ui *ui, de_entity entity) {
    increment(ui, entity, 1);
}
void de_ui__slider_decrement(de_ui *ui, de_entity entity) {
    increment(ui, entity, -1);
}
void de_ui__slider_drag(de_ui *ui, de_entity entity, de_vec2 position) {
    if (!de_ui__widget_enabled(ui, entity))
        return;
    const DeUiSlider *slider = de_entity_get_const(ui->world, entity, ui->widgets->slider);
    const DeUiNode *node = de_entity_get_const(ui->world, entity, ui->ids.node);
    if (!slider || !node)
        return;
    bool vertical = node->direction == DE_UI_COLUMN || node->direction == DE_UI_COLUMN_REVERSE;
    f32 extent = vertical ? node->computed_rect.w : node->computed_rect.z;
    if (extent <= 0)
        return;
    f32 ratio =
        (vertical ? position.y - node->computed_rect.y : position.x - node->computed_rect.x) /
        extent;
    if (node->direction == DE_UI_ROW_REVERSE || node->direction == DE_UI_COLUMN_REVERSE)
        ratio = 1 - ratio;
    value_set(ui, entity, slider->min + fmaxf(0, fminf(1, ratio)) * (slider->max - slider->min));
}
