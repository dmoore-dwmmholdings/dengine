#include "widgets_internal.h"
void de_ui__widget_event(de_ui *ui, de_ui_event_kind kind, de_entity entity, de_name action,
                         f32 value) {
    de_ui_event event = {kind, entity, action, value};
    de_event_send(ui->world, ui->event, &event);
}
void de_ui__button_submit(de_ui *ui, de_entity entity) {
    if (!de_ui__widget_enabled(ui, entity))
        return;
    const DeUiButton *button = de_entity_get_const(ui->world, entity, ui->widgets->button);
    if (button)
        de_ui__widget_event(ui, DE_UI_EV_CLICK, entity, button->on_click, 0);
}
