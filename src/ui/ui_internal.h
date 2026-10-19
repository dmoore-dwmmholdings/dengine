#ifndef DE_UI_INTERNAL_H
#define DE_UI_INTERNAL_H
#include <dengine/de_ui.h>
#include <dengine/de_debug.h>
#include <dengine/de_reflect.h>
#include <dengine/de_memory.h>
#define DE_UI_LAYOUT_SYSTEM  "ui.layout"
#define DE_UI_STYLE_SYSTEM   "ui.style"
#define DE_UI_EXTRACT_SYSTEM "ui.extract"
#define DE_UI_MAX_MEASURES   32u
#define DE_UI_COMPONENTS     9u
#define DE_UI_NONE_INDEX     UINT32_MAX
typedef struct de_ui_components {
    de_component node, anchor, grid, grid_item, popup, style, state, image, text;
} de_ui_components;
typedef de_vec2 (*de_ui_measure_fn)(de_ui *ui, de_entity entity, de_vec2 available);
typedef struct de_ui_measure_entry {
    de_component component;
    de_ui_measure_fn measure;
} de_ui_measure_entry;
typedef struct de_ui_record {
    de_entity entity, parent;
    u64 seen, text_hash;
    u32 hash_slot, first_child, next_sibling, previous_parent;
    DeUiNode node;
    de_vec2 intrinsic;
    de_vec2 measured_available;
    u32 font_generation, component_mask;
    u64 locale_generation;
    bool dirty, measure_dirty, alive;
} de_ui_record;
struct de_ui {
    const de_allocator *alloc;
    de_ui_desc desc;
    de_world *world;
    de_ui_components ids;
    de_builtin_component_ids hierarchy;
    de_resource_id resource;
    de_event_type event;
    de_system layout_system;
    de_query all_nodes;
    de_ui_measure_entry measures[DE_UI_MAX_MEASURES];
    u32 measure_count, capacity, hash_capacity, record_count;
    de_ui_record *records;
    u32 *hash, *free_records;
    u32 free_count;
    u64 frame;
    de_arena *scratch;
    de_cvar scale_cvar;
    u32 width, height;
    de_window_insets safe;
    bool viewport_override, viewport_dirty;
    f32 scale;
    de_ui_layout_stats stats;
    void *render;
    struct de_ui_style_system *style;
    struct de_ui_widgets *widgets;
    struct de_ui_focus *focus;
};
typedef struct de_ui_resource {
    de_ui *ui;
} de_ui_resource;
de_result de_ui__reflect(de_ui *ui, de_world *world);
de_result de_ui__register_measure(de_ui *ui, de_component component, de_ui_measure_fn fn);
de_str de_ui__text_content(de_ui *ui, const DeUiText *text);
void de_ui__layout_system(de_system_ctx *ctx);
u32 de_ui__find(const de_ui *ui, de_entity entity);
void de_ui__dirty(de_ui *ui, u32 index);
#if defined(DE_UI_RETAINED_RENDER)
de_result de_ui__render_init(de_ui *ui);
void de_ui__render_shutdown(de_ui *ui);
de_result de_ui__render_attach(de_ui *ui);
#endif
#endif
