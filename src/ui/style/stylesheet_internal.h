#ifndef DE_UI_STYLESHEET_INTERNAL_H
#define DE_UI_STYLESHEET_INTERNAL_H
#include "ui/ui_internal.h"
#include "core/de_toml_internal.h"
#define DE_UI_STYLE_COUNT(a) ((u32)(sizeof(a) / sizeof((a)[0])))
#define DE_UI_STYLE_SHEETS   32u
#define DE_UI_STYLE_RULES    512u
#define DE_UI_STYLE_TWEENS   4096u
/* Reflection field destinations keep rule parsing, transitions and explicit tweens on one path. */
typedef enum de_ui_style_target {
    DE_UI_TARGET_STYLE,
    DE_UI_TARGET_NODE,
    DE_UI_TARGET_TEXT
} de_ui_style_target;
typedef enum de_ui_style_kind {
    DE_UI_PROP_FLOAT,
    DE_UI_PROP_COLOR,
    DE_UI_PROP_LENGTH,
    DE_UI_PROP_ASSET,
    DE_UI_PROP_EDGES,
    DE_UI_PROP_VEC4,
    DE_UI_PROP_EASING
} de_ui_style_kind;
typedef struct de_ui_style_property {
    const char *name;
    de_ui_style_target target;
    de_ui_style_kind kind;
    u32 offset;
} de_ui_style_property;
typedef struct de_ui_style_value {
    f32 number[4];
    de_ui_unit unit;
    de_asset_id asset;
} de_ui_style_value;
typedef struct de_ui_style_rule {
    de_name widget, class_name;
    u32 state, specificity, order;
    toml_datum_t properties;
} de_ui_style_rule;
typedef struct de_ui_sheet_document {
    const de_allocator *alloc;
    toml_result_t document;
    de_ui_style_rule *rules;
    u32 rule_count;
    u32 references;
} de_ui_sheet_document;
typedef struct de_ui_compiled_rule {
    de_ui_style_value values[24];
    u32 mask;
    f32 transition;
    de_ui_easing easing;
} de_ui_compiled_rule;
typedef struct de_ui_style_sheet_slot {
    de_blob handle;
    u64 fingerprint;
    de_ui_sheet_document *sheet;
    de_ui_compiled_rule *compiled;
} de_ui_style_sheet_slot;
typedef struct de_ui_style_cache {
    de_entity entity;
    de_name class_name;
    u32 widget, state;
    u64 revision;
    u64 binding_revision;
    bool initialized;
    DeUiNode authored_node, applied_node;
    DeUiStyle authored_style, applied_style;
    de_text_style authored_text, applied_text;
} de_ui_style_cache;
typedef struct de_ui_property_tween {
    de_entity entity;
    de_component component;
    u32 offset;
    f32 from, to, elapsed, duration;
    de_ui_easing easing;
    bool active, stylesheet;
} de_ui_property_tween;
typedef struct de_ui_style_system {
    de_ui *ui;
    de_ui_style_sheet_slot sheets[DE_UI_STYLE_SHEETS];
    u32 sheet_count;
    u32 builtin_count;
    de_ui_style_cache *cache;
    de_entity *entities;
    u32 entity_count;
    de_ui_property_tween *tweens;
    u64 revision;
    de_name theme;
    de_component widget;
    de_query nodes;
    de_system system;
    u32 entities_restyled, active_tweens;
} de_ui_style_system;
extern const de_ui_style_property de_ui__style_properties[];
extern const u32 de_ui__style_property_count;
de_result de_ui__sheet_parse(const de_allocator *alloc, const char *bytes, u64 length,
                             de_ui_sheet_document **out);
void de_ui__sheet_release(de_ui_sheet_document *sheet);
de_result de_ui__load_stylesheet_source(de_ui *ui, const char *bytes, u64 length, bool builtin);
de_result de_ui__style_init(de_ui *ui);
void de_ui__style_shutdown(de_ui *ui);
void de_ui__style_attach(de_ui *ui);
void de_ui__style_detach(de_ui *ui);
void de_ui__style_tick(de_system_ctx *ctx);
de_result de_ui__style_value(de_ui_style_system *state, de_ui_sheet_document *sheet,
                             const de_ui_style_property *property, toml_datum_t datum,
                             de_ui_style_value *out);
de_result de_ui__style_asset_register(de_asset_system *assets);
void de_ui__sheets_refresh(de_ui_style_system *state);
void de_ui__sheets_shutdown(de_ui_style_system *state);
de_result de_ui__compile_sheet(de_ui_style_system *state, de_ui_style_sheet_slot *slot);
de_result de_ui__tween_path(de_ui *ui, de_entity entity, const char *property, f32 to, f32 seconds,
                            de_ui_easing easing);
f32 de_ui__ease(de_ui_easing easing, f32 progress);
void de_ui__tweens_tick(de_ui_style_system *state, f32 dt);
de_result de_ui__tween_field(de_ui_style_system *state, de_entity entity, de_component component,
                             u32 offset, f32 value, f32 seconds, de_ui_easing easing,
                             bool stylesheet);
void de_ui__style_tween_sample(de_ui_style_system *state, de_entity entity, de_component component,
                               u32 offset, f32 value);
#endif
