#ifndef DE_UI_BINDING_INTERNAL_H
#define DE_UI_BINDING_INTERNAL_H
#include "ui/widgets/widgets_internal.h"
typedef enum de_ui_binding_property {
    DE_UI_BIND_VALUE,
    DE_UI_BIND_ON,
    DE_UI_BIND_TEXT,
    DE_UI_BIND_VISIBLE,
    DE_UI_BIND_DISABLED,
    DE_UI_BIND_TINT,
    DE_UI_BIND_OPACITY
} de_ui_binding_property;
typedef struct de_ui_binding_cache {
    de_entity entity;
    DeUiBinding descriptor;
    u64 revision, locale_generation;
    de_component source, target, editor;
    de_field_info field;
    u32 offset, target_offset, target_size, editor_offset, editor_size, editor_length;
    de_ui_binding_property property;
    u8 *previous;
    char *text;
    u32 previous_capacity, text_capacity;
    bool valid, sampled, warned;
} de_ui_binding_cache;
typedef struct de_ui_binding_system {
    de_ui *ui;
    de_component component;
    de_query query;
    de_system system;
    de_event_reader events;
    de_arena *arena;
    de_ui_binding_cache *cache;
    de_entity *entities;
    u32 count, capacity, cache_capacity, warnings, writes, resolved;
    de_log_category log;
    f64 update_ms;
} de_ui_binding_system;
de_result de_ui__binding_init(de_ui *ui);
void de_ui__binding_attach(de_ui *ui);
void de_ui__binding_detach(de_ui *ui);
void de_ui__binding_shutdown(de_ui *ui);
#endif
