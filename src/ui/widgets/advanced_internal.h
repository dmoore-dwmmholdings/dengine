#ifndef DE_UI_ADVANCED_INTERNAL_H
#define DE_UI_ADVANCED_INTERNAL_H
#include "widgets_internal.h"
#include "ui/render/draw_list.h"
#include <string.h>
/* Runtime components contain no owning pointers: archetype moves/copies are safe. */
typedef struct de_ui_editor {
    u32 anchor, displayed_caret, preedit_start, preedit_end;
    char display[2048];
    f32 scroll_x, blink, last_click;
    bool dragging, initialized;
} de_ui_editor;
typedef struct de_ui_scroll_state {
    de_vec2 extent, velocity;
    i32 dragging;
    f32 drag_start, offset_start;
} de_ui_scroll_state;
typedef struct de_ui_popup_state { de_entity popup; bool open; } de_ui_popup_state;
typedef struct de_ui_item_state { de_entity owner; u32 index, slot; bool bound; } de_ui_item_state;
typedef struct de_ui_list_state { u32 count; f32 height; } de_ui_list_state;
typedef struct de_ui_tab_state { de_entity headers; u32 count, active; } de_ui_tab_state;
typedef struct de_ui_tooltip_state { de_entity popup; f32 elapsed; } de_ui_tooltip_state;
typedef struct de_ui_advanced {
    de_ui *ui;
    de_component input, editor, dropdown, popup, scroll, scroll_state, list, list_state;
    de_component tabs, tab_state, tooltip, tooltip_state, item, header;
    de_entity active_input;
    de_entity *entities;
    u32 count;
    de_system prepare, input_system;
    de_event_reader events;
    de_ui_widget_draw_fn base_draw;
    void *base_user;
    f32 time;
    de_result (*clipboard_set)(const char *);
    size_t (*clipboard_get)(char *,size_t);
} de_ui_advanced;
de_result de_ui__advanced_init(de_ui *ui);
void de_ui__advanced_shutdown(de_ui *ui);
void de_ui__advanced_attach(de_ui *ui);
void de_ui__advanced_detach(de_ui *ui);
de_component de_ui__advanced_component(de_ui *ui,const char *name,u32 size,u32 align,
                                       const de_field_info *fields,u32 count,bool authored);
void de_ui__advanced_tag(de_ui *ui,de_entity e,de_ui_widget_type type);
void de_ui__input_refresh(de_ui *ui,de_entity e);
void de_ui__input_prepare(de_ui *ui,de_entity e);
void de_ui__input_frame(de_ui *ui,de_entity e,f32 dt);
void de_ui__input_submit(de_ui *ui,de_entity e);
void de_ui__input_cancel(de_ui *ui,de_entity e);
void de_ui__input_draw(de_ui *ui,de_entity e,de_vec4 r,de_ui_draw_emitter *em);
void de_ui__input_geometry(de_ui *ui,de_entity e,de_vec2 *offset,f32 *width,void *user);
bool de_ui__input_replace(DeUiTextInput *input,de_ui_editor *state,de_str text);
void de_ui__input_move(DeUiTextInput *input,de_ui_editor *state,i32 direction,bool extend,bool word);
void de_ui__dropdown_prepare(de_ui *ui,de_entity e);
void de_ui__dropdown_submit(de_ui *ui,de_entity e);
void de_ui__dropdown_cancel(de_ui *ui,de_entity e);
void de_ui__dropdown_event(de_ui *ui,const de_ui_event *event);
void de_ui__scroll_prepare(de_ui *ui,de_entity e,f32 dt);
void de_ui__scroll_frame(de_ui *ui,de_entity e,f32 dt);
void de_ui__scroll_draw(de_ui *ui,de_entity e,de_vec4 r,de_ui_draw_emitter *em);
void de_ui__scroll_content(de_ui *ui,de_entity e,de_vec4 *content);
void de_ui__scroll_into_view(de_ui *ui,de_entity container,de_entity child);
void de_ui__list_prepare(de_ui *ui,de_entity e);
void de_ui__tabs_prepare(de_ui *ui,de_entity e);
void de_ui__tabs_event(de_ui *ui,const de_ui_event *event);
void de_ui__tabs_submit(de_ui *ui,de_entity e);
void de_ui__tabs_increment(de_ui *ui,de_entity e);
void de_ui__tabs_decrement(de_ui *ui,de_entity e);
void de_ui__tooltip_prepare(de_ui *ui,de_entity e,f32 dt);
de_entity de_ui__advanced_node(de_ui *ui,de_entity parent,f32 width,f32 height);
#endif
