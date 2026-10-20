#include "widgets_internal.h"
void de_ui__toggle_submit(de_ui *ui, de_entity entity) {
    if (!de_ui__widget_enabled(ui, entity) ||
        !de_entity_has(ui->world, entity, ui->widgets->toggle))
        return;
    DeUiToggle *toggle = de_entity_get(ui->world, entity, ui->widgets->toggle);
    toggle->on = !toggle->on;
    de_ui__widget_event(ui, DE_UI_EV_CHANGED, entity, (de_name){0}, toggle->on ? 1.f : 0.f);
}
