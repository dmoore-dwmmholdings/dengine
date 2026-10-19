/* de_ui.h — retained in-game UI (ECS entities) + immediate-mode debug UI. SPEC.md §17.1–17.2. */
#ifndef DE_UI_H
#define DE_UI_H
#include "de_base.h"
#include "de_math.h"
#include "de_ecs.h"
#include "de_asset.h"
#include "de_text.h"
#include "de_platform.h"
DE_EXTERN_C_BEGIN

typedef struct de_ui de_ui;
typedef struct de_renderer de_renderer;
typedef struct de_input de_input;

typedef struct de_ui_desc {
    const de_allocator *allocator;
    de_renderer *renderer;
    de_text_system *text;
    de_input *input;
    f32 reference_height; /* UI scale reference, e.g. 1080 */
    bool pixel_art;       /* integer scaling + bitmap fonts */
    de_window window;     /**< Optional window for output size and safe insets. */
    u32 max_nodes;        /**< Zero:65536 retained nodes. Capacity is allocated at create. */
} de_ui_desc;

DE_API de_result de_ui_create(const de_ui_desc *desc, de_ui **out);
DE_API void de_ui_destroy(de_ui *ui);
DE_API void de_ui_attach(de_ui *ui, de_world *w); /* layout, style, input routing, extract */
DE_API de_result de_ui_load_stylesheet(de_ui *ui, de_blob stylesheet); /* TOML asset */

/* ---------- Retained UI: components on entities ---------- */
typedef enum de_ui_unit { DE_UI_AUTO, DE_UI_PX, DE_UI_PERCENT, DE_UI_FR } de_ui_unit;
typedef struct de_ui_len {
    de_ui_unit unit;
    f32 value;
} de_ui_len;

typedef enum de_ui_display { DE_UI_FLEX, DE_UI_GRID, DE_UI_NONE } de_ui_display;
typedef enum de_ui_dir {
    DE_UI_ROW,
    DE_UI_COLUMN,
    DE_UI_ROW_REVERSE,
    DE_UI_COLUMN_REVERSE
} de_ui_dir;
typedef enum de_ui_align {
    DE_UI_ALIGN_START,
    DE_UI_ALIGN_CENTER,
    DE_UI_ALIGN_END,
    DE_UI_ALIGN_STRETCH,
    DE_UI_ALIGN_SPACE_BETWEEN,
    DE_UI_ALIGN_SPACE_AROUND,
    DE_UI_ALIGN_AUTO /**< align_self only: inherit parent's align_items. */
} de_ui_align;

typedef struct DeUiNode {
    de_ui_display display;
    de_ui_dir direction;
    de_ui_align justify, align_items, align_self;
    de_ui_len width, height, min_width, min_height, max_width, max_height;
    de_ui_len margin[4], padding[4]; /* top, right, bottom, left */
    f32 gap, grow, shrink;
    de_name style_class;
    i32 z_order;
    bool clip, safe_area;
    de_vec4 computed_rect; /* output: x, y, w, h in screen pixels */
} DeUiNode;

/** Anchors are normalized parent-content coordinates; offsets are reference pixels. */
typedef struct DeUiAnchor {
    de_vec2 anchor_min, anchor_max;
    de_vec2 offset_min, offset_max;
} DeUiAnchor;
typedef struct DeUiGrid {
    de_ui_len columns[8];
    u32 column_count;
    de_ui_len rows[8];
    u32 row_count;
    f32 column_gap, row_gap;
} DeUiGrid;
typedef struct DeUiGridItem {
    u8 column, row, column_span, row_span;
} DeUiGridItem;
typedef enum de_ui_popup_placement {
    DE_UI_POPUP_BOTTOM,
    DE_UI_POPUP_TOP,
    DE_UI_POPUP_LEFT,
    DE_UI_POPUP_RIGHT,
    DE_UI_POPUP_OVERLAY
} de_ui_popup_placement;
typedef struct DeUiPopup {
    de_entity anchor;
    u8 placement;
    bool modal;
} DeUiPopup;
typedef struct DeUiStyle {
    de_vec4 background, border_color;
    f32 border_width, corner_radius, opacity;
    de_vec4 text_color;
} DeUiStyle;
typedef struct DeUiInteractionState {
    bool hovered, pressed, focused, disabled;
} DeUiInteractionState;
/** Override physical output dimensions and safe insets (also usable without a window/GPU). */
DE_API void de_ui_set_viewport(de_ui *ui, u32 width, u32 height, de_window_insets safe);
typedef struct de_ui_layout_stats {
    u32 nodes_laid_out, nodes_measured, dirty_nodes, total_nodes;
    f32 scale;
} de_ui_layout_stats;
DE_API de_ui_layout_stats de_ui_get_layout_stats(const de_ui *ui);

typedef struct DeUiImage {
    de_texture texture;
    de_vec4 uv_rect;
    de_vec4 nine_slice;
    de_vec4 tint;
} DeUiImage;
typedef struct DeUiText {
    de_name loc_id;
    de_str literal;
    de_text_style style;
    de_text_align align;
    char text[512]; /**< Authored serialized text; literal is an optional runtime override. */
} DeUiText;
typedef struct DeUiButton {
    bool disabled;
    de_name on_click;
} DeUiButton; /* sends de_ui_event */
typedef struct DeUiSlider {
    f32 value, min, max, step;
} DeUiSlider;
typedef struct DeUiToggle {
    bool on;
} DeUiToggle;
typedef struct DeUiTextInput {
    char buffer[256];
    u32 length, caret;
    bool password, multiline;
} DeUiTextInput;
typedef struct DeUiScroll {
    de_vec2 offset;
    bool horizontal, vertical;
} DeUiScroll;
typedef struct DeUiFocusable {
    i32 tab_index;
    de_entity up, down, left, right;
} DeUiFocusable; /* null = spatial */
/* Binds a widget property to a reflected field on another entity, e.g. "DeHealth.current". */
typedef struct DeUiBinding {
    de_entity source;
    char path[64];
    char property[32];
} DeUiBinding;
/* Renders this UI subtree into the 3D world as a panel. */
typedef struct DeUiWorldPanel {
    de_vec2 size_m;
    f32 pixels_per_meter;
    bool lit, depth_test;
} DeUiWorldPanel;

typedef enum de_ui_event_kind {
    DE_UI_EV_CLICK,
    DE_UI_EV_CHANGED,
    DE_UI_EV_SUBMIT,
    DE_UI_EV_FOCUS,
    DE_UI_EV_BLUR,
    DE_UI_EV_HOVER
} de_ui_event_kind;
typedef struct de_ui_event {
    de_ui_event_kind kind;
    de_entity target;
    de_name action;
    f32 value;
} de_ui_event;

DE_API void de_ui_set_focus(de_ui *ui, de_entity e);
DE_API de_entity de_ui_focused(const de_ui *ui);
DE_API void de_ui_tween(de_ui *ui, de_entity e, const char *property, f32 to, f32 seconds,
                        u32 easing);

/* ---------- Debug UI (Debug/Dev builds; backend per SPEC Q1) ---------- */
DE_API bool de_dbg_begin(const char *window_title, bool *open);
DE_API void de_dbg_end(void);
DE_API void de_dbg_text(const char *fmt, ...);
DE_API bool de_dbg_button(const char *label);
DE_API bool de_dbg_checkbox(const char *label, bool *v);
DE_API bool de_dbg_slider_f32(const char *label, f32 *v, f32 min, f32 max);
DE_API bool de_dbg_input_text(const char *label, char *buf, u32 buf_bytes);
DE_API bool de_dbg_tree_node(const char *label);
DE_API void de_dbg_tree_pop(void);
DE_API void de_dbg_plot(const char *label, const f32 *values, u32 count, f32 min, f32 max);
/* Edits any reflected object (used by the ECS inspector and cvar panel). */
DE_API bool de_dbg_inspect(const de_type_registry *types, de_type_id type, void *obj);

DE_EXTERN_C_END
#endif
