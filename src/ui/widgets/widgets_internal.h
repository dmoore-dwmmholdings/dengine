#ifndef DE_UI_WIDGETS_INTERNAL_H
#define DE_UI_WIDGETS_INTERNAL_H
#include "ui/ui_internal.h"
#include <dengine/de_input.h>
#define DE_UI_WIDGET_TYPE_COUNT 15u
typedef void (*de_ui_widget_action)(de_ui *ui, de_entity entity);
typedef struct de_ui_widget_entry {
    de_ui_widget_action submit, increment, decrement, cancel;
} de_ui_widget_entry;
typedef void (*de_ui_scroll_into_view_fn)(de_ui *ui, de_entity container, de_entity child);
typedef struct de_ui_widget_table {
    de_ui_widget_entry entries[DE_UI_WIDGET_TYPE_COUNT];
    de_ui_scroll_into_view_fn scroll_into_view;
    de_ui_widget_action request_focus;
    de_entity (*focused)(const de_ui *ui);
    de_ui_widget_action refresh_text; /**< Refresh display after a committed-buffer write. */
} de_ui_widget_table;
typedef struct de_ui_pointer_state {
    de_entity hovered, captured, root;
    de_vec2 position;
    bool down, consumed;
} de_ui_pointer_state;
typedef struct de_ui_widgets {
    de_ui *ui;
    de_component button, toggle, slider, progress, focusable;
    de_ui_widget_table table;
    de_ui_pointer_state pointer;
    de_entity *entities;
    u32 entity_count;
    de_system update_system, pointer_system;
    f32 time;
} de_ui_widgets;
void de_ui__image_register(de_ui *ui);
de_result de_ui__widgets_init(de_ui *ui);
void de_ui__widgets_shutdown(de_ui *ui);
void de_ui__widgets_attach(de_ui *ui);
void de_ui__widgets_detach(de_ui *ui);
de_ui_widget_table *de_ui__widget_table(de_ui *ui);
bool de_ui__widget_enabled(de_ui *ui, de_entity entity);
/** A zero root targets screen roots. Otherwise position is root-local physical pixels. */
de_entity de_ui__hit_test(de_ui *ui, de_entity root, de_vec2 position);
void de_ui__pointer_route(de_ui *ui, de_entity root, de_vec2 position, bool pressed, bool released,
                          bool down);
void de_ui__pointer_system(de_system_ctx *ctx);
void de_ui__widget_event(de_ui *ui, de_ui_event_kind kind, de_entity entity, de_name action,
                         f32 value);
void de_ui__button_submit(de_ui *ui, de_entity entity);
void de_ui__toggle_submit(de_ui *ui, de_entity entity);
void de_ui__slider_increment(de_ui *ui, de_entity entity);
void de_ui__slider_decrement(de_ui *ui, de_entity entity);
void de_ui__slider_drag(de_ui *ui, de_entity entity, de_vec2 position);
void de_ui__widgets_register_draw(de_ui *ui);
#endif
