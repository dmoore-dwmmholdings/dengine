#ifndef DE_UI_FOCUS_INTERNAL_H
#define DE_UI_FOCUS_INTERNAL_H
#include "ui/widgets/widgets_internal.h"
typedef enum de_ui_nav_dir {
    DE_UI_NAV_NONE,
    DE_UI_NAV_UP,
    DE_UI_NAV_DOWN,
    DE_UI_NAV_LEFT,
    DE_UI_NAV_RIGHT
} de_ui_nav_dir;
typedef struct de_ui_nav_sample {
    de_vec2 navigate, pointer;
    f32 dt;
    bool submit, cancel, next, previous;
} de_ui_nav_sample;
typedef struct de_ui_focus_scope {
    de_entity popup, restore;
} de_ui_focus_scope;
typedef struct de_ui_focus {
    de_ui *ui;
    de_component focusable;
    de_entity focused, scope;
    de_entity *nodes, *order;
    de_ui_focus_scope *scopes;
    u32 node_count, count, scope_count;
    de_ui_nav_sample *samples;
    u32 sample_count, sample_capacity;
    de_system update, capture;
    de_action_map map;
    de_action navigate, submit, cancel, next, previous, scroll, pointer;
    de_ui_input_mode mode;
    u32 old_flags;
    bool owns_push, owns_input, ring, pointer_known;
    de_vec2 last_pointer;
    de_ui_nav_dir held;
    f64 held_time, next_repeat;
} de_ui_focus;
de_result de_ui__focus_init(de_ui *ui);
void de_ui__focus_attach(de_ui *ui);
void de_ui__focus_detach(de_ui *ui);
void de_ui__focus_shutdown(de_ui *ui);
void de_ui__focus_collect(de_ui *ui);
bool de_ui__focus_allowed(de_ui *ui, de_entity entity);
bool de_ui__focus_descendant(de_ui *ui, de_entity entity, de_entity ancestor);
de_entity de_ui__spatial_nav(de_ui *ui, de_entity from, de_ui_nav_dir direction);
de_entity de_ui__tab_nav(de_ui *ui, de_entity from, bool previous);
void de_ui__focus_move(de_ui *ui, de_ui_nav_dir direction);
void de_ui__focus_sample(de_ui *ui, const de_ui_nav_sample *sample);
void de_ui__actions_attach(de_ui *ui);
void de_ui__actions_detach(de_ui *ui);
void de_ui__actions_context(de_ui *ui);
#endif
