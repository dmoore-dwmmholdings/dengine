/* de_ui.h — retained in-game UI (ECS entities) + immediate-mode debug UI. SPEC.md §17.1–17.2. */
#ifndef DE_UI_H
#define DE_UI_H
#include "de_base.h"
#include "de_math.h"
#include "de_ecs.h"
#include "de_asset.h"
#include "de_text.h"
#include "de_platform.h"
#include "de_render.h"
DE_EXTERN_C_BEGIN

typedef struct de_ui de_ui;
typedef struct de_renderer de_renderer;
typedef struct de_input de_input;

typedef struct de_ui_desc {
    const de_allocator *allocator;
    de_renderer *renderer;
    de_text_system *text;
    de_input *input;
    f32 reference_height;    /* UI scale reference, e.g. 1080 */
    bool pixel_art;          /* integer scaling + bitmap fonts */
    de_window window;        /**< Optional window for output size and safe insets. */
    u32 max_nodes;           /**< Zero:65536 retained nodes. Capacity is allocated at create. */
    de_asset_system *assets; /**< Optional stylesheet and font/texture asset source. */
    de_view view; /**< The view the UI draws on (and points into); zero: the renderer's main view. */
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

/** Widget tags drive stylesheet type selectors. Widgets infer tags when none is authored;
 * an explicitly supplied selector takes precedence over content and behavior components. */
typedef enum de_ui_widget_type {
    DE_UI_WIDGET_PANEL,
    DE_UI_WIDGET_BUTTON,
    DE_UI_WIDGET_TOGGLE,
    DE_UI_WIDGET_SLIDER,
    DE_UI_WIDGET_TEXT,
    DE_UI_WIDGET_IMAGE,
    DE_UI_WIDGET_PROGRESS,
    DE_UI_WIDGET_INPUT,
    DE_UI_WIDGET_SCROLL,
    DE_UI_WIDGET_DROPDOWN,
    DE_UI_WIDGET_TOOLTIP,
    DE_UI_WIDGET_LIST,
    DE_UI_WIDGET_TABS,
    DE_UI_WIDGET_TREE,
    DE_UI_WIDGET_CANVAS
} de_ui_widget_type;
typedef struct DeUiWidgetType {
    de_ui_widget_type type;
} DeUiWidgetType;
typedef enum de_ui_easing {
    DE_UI_EASE_LINEAR,
    DE_UI_EASE_QUAD_IN,
    DE_UI_EASE_QUAD_OUT,
    DE_UI_EASE_QUAD_IN_OUT,
    DE_UI_EASE_CUBIC_IN,
    DE_UI_EASE_CUBIC_OUT,
    DE_UI_EASE_CUBIC_IN_OUT,
    DE_UI_EASE_BACK_OUT,
    DE_UI_EASE_ELASTIC_OUT
} de_ui_easing;
/** Handle for a registered ui_style asset. It can be passed to de_ui_load_stylesheet.
 * Loading/refcounting remains the caller's responsibility, as with ordinary blob assets. */
DE_API de_blob de_ui_stylesheet(de_ui *ui, de_asset_id id);
/** Switch variable overlays; NULL or empty selects the base variables. Names are at most
 * DE_UI_THEME_NAME_MAX - 1 bytes (DE_ERR_INVALID_ARGUMENT otherwise). While an editor preview
 * is active (de_ui_preview_theme), the name is stored and applied when the preview ends. */
DE_API de_result de_ui_set_theme(de_ui *ui, const char *name);

/* ---- Theme inspection and editor preview (DENG-781) ----
 * Main thread. A theme is a [theme.<name>.vars] overlay in a loaded stylesheet. */
#define DE_UI_THEME_NAME_MAX  64u
#define DE_UI_THEME_VALUE_MAX 128u
typedef struct de_ui_theme_info {
    char name[DE_UI_THEME_NAME_MAX];
    u32 sheets; /* loaded stylesheets that declare this theme */
} de_ui_theme_info;
/** Writes the themes of every loaded stylesheet, without duplicates, sorted by name (byte
 * order), up to `capacity`. `count` gets the full count. Names that are too long for
 * de_ui_theme_info are left out. NULL `out` is allowed with a capacity of 0. */
DE_API de_result de_ui_list_themes(const de_ui *ui, de_ui_theme_info *out, u32 capacity,
                                   u32 *count);
typedef struct de_ui_theme_var {
    char name[DE_UI_THEME_NAME_MAX];
    /* The value as a TOML literal: "#ff8800" (quoted), 12, 0.5, true or [1, 2, 3, 4]. */
    char value[DE_UI_THEME_VALUE_MAX];
    u32 sheet;      /* index of the stylesheet, in load order (built-in sheets first) */
    bool themed;    /* the value comes from the theme overlay, not from [vars] */
    bool truncated; /* `value` (or `name`) was cut to fit */
} de_ui_theme_var;
/** DENG-786: where a loaded stylesheet came from. */
typedef struct de_ui_sheet_info {
    de_asset_id asset; /* the ui_style or blob asset; {0} when loaded from source text */
    const char *path;  /* the asset's manifest path; "" when there is none. Valid until the next
                        * manifest load or destroy */
    bool builtin;      /* an embedded engine sheet */
} de_ui_sheet_info;
/** Describes the stylesheet at `sheet`, an index from de_ui_theme_var.sheet (load order,
 * built-in sheets first). DE_ERR_NOT_FOUND: no sheet has that index. */
DE_API de_result de_ui_get_sheet_info(const de_ui *ui, u32 sheet, de_ui_sheet_info *out);

/* ---- Stylesheet files, sheet list and parse errors (DENG-780) ---- */
/** The file name suffix of stylesheet sources, for example "ui/hud.uistyle.toml". */
#define DE_UI_STYLESHEET_SUFFIX DE_ASSET_STYLESHEET_SUFFIX
/** True when `path` (either separator) names a file that ends with DE_UI_STYLESHEET_SUFFIX and
 * has a name before it. The match is exact bytes (case-sensitive). */
DE_API bool de_ui_is_stylesheet_path(const char *path);
#define DE_UI_SHEET_ERROR_MAX 160u
/** A stylesheet error. `line` and `column` are 1-based, 0 when not known. `message` is the
 * reason without the position, cut to fit. */
typedef struct de_ui_sheet_error {
    u32 line, column;
    char message[DE_UI_SHEET_ERROR_MAX];
} de_ui_sheet_error;
/** Parses and validates stylesheet text without loading it. DE_OK clears `out`; DE_ERR_PARSE
 * fills it, and the error context message has the same position ("UI stylesheet line L,
 * column C: reason"). Asset references are not resolved. Any thread. */
DE_API de_result de_ui_check_stylesheet(const char *bytes, u64 length, de_ui_sheet_error *out);
/** One stylesheet of de_ui_list_sheets. */
typedef struct de_ui_sheet_status {
    de_ui_sheet_info info;
    u32 index;          /* list position; for a loaded sheet, the de_ui_theme_var.sheet index */
    const char *source; /* the asset's manifest `source` relative to the asset root; "" when
                         * none. Valid like info.path */
    bool loaded;        /* false: the first load failed and no rules are in use */
    bool has_error;     /* `error` is set: a failed load, or a rejected reload (old rules kept) */
    de_ui_sheet_error error;
} de_ui_sheet_status;
/** Lists the stylesheets: the loaded sheets in load order (built-in sheets first), then the
 * sheets whose first de_ui_load_stylesheet failed. Writes up to `capacity` entries; `count`
 * gets the full count. `out` may be NULL with `capacity` 0. A good reload clears an error; a
 * failed sheet leaves the list when a later de_ui_load_stylesheet of it succeeds. Main thread. */
DE_API de_result de_ui_list_sheets(const de_ui *ui, de_ui_sheet_status *out, u32 capacity,
                                   u32 *count);
/** Writes the variables in effect for `theme` (NULL: the active theme; "": the base
 * variables): for each sheet in load order, its [vars] in file order with the overlay values
 * applied, then the variables that only the overlay declares. `count` gets the full count.
 * DE_ERR_NOT_FOUND: a non-empty theme that no loaded sheet declares. */
DE_API de_result de_ui_theme_vars(const de_ui *ui, const char *theme, de_ui_theme_var *out,
                                  u32 capacity, u32 *count);
/** Editor preview: shows `name` ("" is the base variables) without changing the theme the
 * game selected with de_ui_set_theme. NULL ends the preview and applies the selected theme.
 * DE_ERR_NOT_FOUND: a non-empty name that no loaded sheet declares. A stylesheet error under
 * the theme returns that error and keeps the current state. Ending a preview when none is
 * active is DE_OK. */
DE_API de_result de_ui_preview_theme(de_ui *ui, const char *name);
typedef struct de_ui_theme_state {
    char active[DE_UI_THEME_NAME_MAX];   /* the theme the stylesheets are compiled with */
    char selected[DE_UI_THEME_NAME_MAX]; /* the game's de_ui_set_theme choice */
    bool previewing;
} de_ui_theme_state;
DE_API de_result de_ui_get_theme_state(const de_ui *ui, de_ui_theme_state *out);
typedef struct de_ui_style_stats {
    u32 entities_restyled, active_tweens;
} de_ui_style_stats;
DE_API de_ui_style_stats de_ui_get_style_stats(const de_ui *ui);

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
    de_texture background_image;
    de_vec4 nine_slice; /**< Left,top,right,bottom source pixel borders; zero = plain image. */
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
/** A viewport for de_ui_layout_preview: what an editor previews the UI at. */
typedef struct de_ui_preview_viewport {
    u32 width, height; /**< Output pixels; with pixel_art, the internal (low-resolution) pixels. */
    f32 ui_scale;      /**< The ui.scale value to apply; zero or less: the current ui.scale. */
    de_window_insets safe; /**< Safe-area insets in output pixels. */
    bool pixel_art;        /**< Integer scale round(ui_scale), as de_ui_desc.pixel_art. */
} de_ui_preview_viewport;
typedef struct de_ui_preview_rect {
    de_entity entity;
    de_vec4 rect; /**< x, y, w, h in the preview viewport's pixels, as DeUiNode.computed_rect */
} de_ui_preview_rect;
/**
 * Lays the UI out at `viewport` and reports the rects of `root` (a DeUiNode entity) and its UI
 * descendants in pre-order: the root, then each child's subtree in child order (DENG-768). Every
 * root is laid out, as a frame would, then the live viewport is laid out again, so the world's
 * DeUiNode.computed_rect values are unchanged afterwards. World panels keep their own extent.
 * `*count` gets the number of nodes in the subtree; the first `capacity` go to `out`. `scale`
 * (optional) gets the layout scale used. Main thread, outside a running schedule.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL `ui`, `viewport` or `count`, `out` NULL with a
 *         capacity, a zero width or height, a non-finite ui_scale, or `ui` not attached) or
 *         DE_ERR_NOT_FOUND (`root` is not a live UI node).
 */
DE_API de_result de_ui_layout_preview(de_ui *ui, de_entity root,
                                      const de_ui_preview_viewport *viewport,
                                      de_ui_preview_rect *out, u32 capacity, u32 *count,
                                      f32 *scale);
/** Text to measure with de_ui_measure_text (DENG-782). Lengths are in layout pixels, the units
 * of DeUiNode.computed_rect and de_ui_layout_preview. */
typedef struct de_ui_text_measure_desc {
    de_str utf8;
    const de_text_style *style; /**< As DeUiText.style: size_px <= 0 is 16. */
    de_text_align align;
    f32 wrap_width; /**< Zero: no wrap. */
    f32 box_height; /**< Zero: no height limit, overflow_y stays false. */
    f32 scale;      /**< Layout scale, from de_ui_layout_preview; <= 0: current (1 before layout). */
} de_ui_text_measure_desc;
typedef struct de_ui_text_metrics {
    de_vec2 size; /**< Width and height of the laid-out text. */
    f32 line_height, scale;
    u32 line_count;
    bool overflow_x; /**< wrap_width is set and a line cannot break narrow enough. */
    bool overflow_y; /**< box_height is set and the text is taller. */
} de_ui_text_metrics;
/**
 * Measures text with the UI's text system as a DeUiText node is measured: the style's size and
 * letter spacing are multiplied by the layout scale. `lines` (optional) gets up to `capacity`
 * line breaks (de_text_layout_lines). For the editor's overflow preview. Main thread.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL `ui`, `desc`, `desc->style` or `out`, `lines` NULL
 *         with a capacity, a negative or non-finite wrap_width or box_height, a non-finite scale,
 *         or no text system in de_ui_desc.text), or the de_text_layout_build error.
 */
DE_API de_result de_ui_measure_text(de_ui *ui, const de_ui_text_measure_desc *desc,
                                    de_text_line *lines, u32 capacity, de_ui_text_metrics *out);

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
typedef struct DeUiProgress {
    f32 value;
    bool indeterminate;
} DeUiProgress;
/** True after routing a pointer over UI or while UI owns its capture; reset each PreUpdate. */
DE_API bool de_ui_pointer_consumed(const de_ui *ui);
typedef struct DeUiToggle {
    bool on;
} DeUiToggle;
typedef struct DeUiTextInput {
    char buffer[256]; /**< Legacy inline input. After growth use text_input_get/set. */
    u32 length, caret;
    bool password, multiline;
} DeUiTextInput;
/** Semantic committed editor operations for game journals/replay. Selection offsets are
 * UTF-8 bytes and snap to grapheme boundaries. REPLACE replaces the current selection. */
typedef enum de_ui_text_edit_kind {
    DE_UI_TEXT_SELECT,
    DE_UI_TEXT_REPLACE,
    DE_UI_TEXT_UNDO,
    DE_UI_TEXT_REDO
} de_ui_text_edit_kind;
typedef struct de_ui_text_edit {
    de_ui_text_edit_kind kind;
    de_str text;
    u32 caret, anchor;
} de_ui_text_edit;
/** Observer runs synchronously after successful edits/selection changes. Copy text before
 * returning; do not reenter UI editing here. Programmatic set() is not an edit command.
 * IME preedit is transient and not recorded; committed text is recorded. */
typedef void (*de_ui_text_edit_observer)(de_entity entity, const de_ui_text_edit *edit, void *user);
DE_API void de_ui_set_text_edit_observer(de_ui *ui, de_ui_text_edit_observer observer, void *user);
DE_API bool de_ui_text_input_apply(de_ui *ui, de_entity entity, const de_ui_text_edit *edit);
/** Set a committed UTF-8 document (no embedded NUL), copying into UI-owned storage.
 * Clears undo history; does not emit CHANGED. Existing inline inputs remain supported.
 * Runtime documents are not ECS snapshots/serialized data: save via get/set explicitly.
 * On allocation failure the previous document is preserved. */
DE_API de_result de_ui_text_input_set(de_ui *ui, de_entity entity, de_str text);
/** Borrowed committed document, valid until the next edit/set/removal/detach. */
DE_API de_str de_ui_text_input_get(const de_ui *ui, de_entity entity);
/** Undo/redo a committed edit, restoring selection and emitting CHANGED. */
DE_API bool de_ui_text_input_undo(de_ui *ui, de_entity entity);
DE_API bool de_ui_text_input_redo(de_ui *ui, de_entity entity);
typedef struct DeUiScroll {
    de_vec2 offset;
    bool horizontal, vertical;
} DeUiScroll;
/** Options are localization IDs; selection is clamped to the live option count. */
typedef struct DeUiDropdown {
    u32 selected, option_count;
    de_name options[32];
} DeUiDropdown;
typedef struct DeUiList {
    u32 item_count;
    f32 item_height;
    de_prefab item_template;
} DeUiList;
typedef struct DeUiTabs {
    u32 active;
} DeUiTabs;
typedef struct DeUiTooltip {
    de_name loc_id;
    f32 delay;
} DeUiTooltip;
/** Scroll offsets are physical pixels. Values are clamped to current content bounds. */
DE_API void de_ui_scroll_offset(de_ui *ui, de_entity container, de_vec2 offset);
/** Reveal a child within its scroll container, clamping the resulting offset. */
DE_API void de_ui_scroll_to(de_ui *ui, de_entity container, de_entity child);
typedef struct DeUiFocusable {
    i32 tab_index;
    de_entity up, down, left, right;
} DeUiFocusable; /* null = spatial */
/* Binds a widget property to a reflected field on another entity, e.g. "DeHealth.current". */
typedef enum de_ui_binding_mode { DE_UI_BIND_ONE_WAY, DE_UI_BIND_TWO_WAY } de_ui_binding_mode;
/** Active tint/opacity bindings take precedence over stylesheet state/theme values.
 * Text tint controls the resolved text color; unchanged sources do not trigger writes. */
typedef struct DeUiBinding {
    de_entity source;
    char path[64];
    char property[32];
    de_ui_binding_mode mode; /**< Zero: source to widget; TWO_WAY also accepts CHANGED. */
    char format[32];         /**< Optional S28 inline message pattern, with argument `value`. */
} DeUiBinding;
/* Renders this UI subtree into the 3D world as a panel. */
typedef struct DeUiWorldPanel {
    de_vec2 size_m;
    f32 pixels_per_meter;
    bool lit, depth_test;
} DeUiWorldPanel;

/** One UI node under an editor pick point (DENG-779). */
typedef struct de_ui_pick_hit {
    de_entity entity; /**< the UI node */
    de_entity root;   /**< its UI root: a screen root or a DeUiWorldPanel root */
    de_vec4 rect;     /**< its DeUiNode.computed_rect, in the root's pixels */
    u32 depth;        /**< levels below `root`; 0 is the root itself */
} de_ui_pick_hit;
/**
 * Editor hit test (DENG-779): every UI node under `point`, topmost first, so that a click in an
 * editor viewport can select one and Alt-click can step down the stack.
 * - `root` null: the screen UI, every UI root that is not a DeUiWorldPanel. `point` is in
 *   viewport pixels, the space of DeUiNode.computed_rect.
 * - `root` a UI root (a UI node with no UI parent): only its subtree. For a world panel, `point`
 *   is in the panel's pixels, (0, 0) at its top-left, as de_ui_pick_ray reports it.
 * The order is the draw order reversed: later roots, then popups, then higher z_order, then later
 * in tree order come first. A node is hit when its display is not DE_UI_NONE, its rect has an area
 * and holds the point, and the clip rects of its clipping ancestors (and, for a screen root, the
 * viewport) hold it. Unlike pointer routing, every UI node can be hit, also one that is disabled,
 * is not a widget or draws nothing, and no renderer is needed. The rects are those of the last
 * layout. `*count` gets the number of hits; the first `capacity` go to `out`. Main thread,
 * outside a running schedule.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL `ui` or `count`, `out` NULL with a capacity, a
 *         non-finite point, or `ui` not attached) or DE_ERR_NOT_FOUND (`root` is not a live UI
 *         root).
 */
DE_API de_result de_ui_pick(de_ui *ui, de_entity root, de_vec2 point, de_ui_pick_hit *out,
                            u32 capacity, u32 *count);
/** The world panel an editor ray hit (DENG-779). */
typedef struct de_ui_panel_pick {
    de_entity panel; /**< the DeUiWorldPanel root; null: no panel was hit */
    de_vec2 pixel;   /**< the hit in the panel's pixels, (0, 0) at its top-left */
    f32 distance;    /**< along the ray, in units of `direction` */
} de_ui_panel_pick;
/**
 * Editor world-panel pick (DENG-779): the nearest DeUiWorldPanel root that the ray from `origin`
 * along `direction` (world space; for example the editor camera's ray through a viewport pixel)
 * hits inside its extent, from either side, then de_ui_pick on that panel at the hit pixel.
 * Panels with no DeGlobalTransform are skipped. When no panel is hit, `*count` is 0 and
 * `panel->panel` is null. `panel` is optional. Main thread, outside a running schedule.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL `ui` or `count`, `out` NULL with a capacity, a
 *         non-finite origin or direction, a zero direction, or `ui` not attached).
 */
DE_API de_result de_ui_pick_ray(de_ui *ui, de_dvec3 origin, de_vec3 direction,
                                de_ui_panel_pick *panel, de_ui_pick_hit *out, u32 capacity,
                                u32 *count);
/**
 * The world-space corners of a UI node on a world panel, for the editor's selection outline and
 * handles (DENG-779): its computed_rect's top-left, top-right, bottom-right and bottom-left,
 * placed through its panel's DeGlobalTransform as the panel is drawn. `panel` (optional) gets
 * the panel root. Main thread, outside a running schedule.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL `ui` or `corners`, or `ui` not attached) or
 *         DE_ERR_NOT_FOUND (`node` is not a live UI node, or its root is not a world panel with a
 *         valid size and a DeGlobalTransform).
 */
DE_API de_result de_ui_node_world_corners(de_ui *ui, de_entity node, de_dvec3 corners[4],
                                          de_entity *panel);

typedef enum de_ui_event_kind {
    DE_UI_EV_CLICK,
    DE_UI_EV_CHANGED,
    DE_UI_EV_SUBMIT,
    DE_UI_EV_FOCUS,
    DE_UI_EV_BLUR,
    DE_UI_EV_HOVER,
    DE_UI_EV_BIND_ITEM /**< target is pooled row, value is the newly visible item index. */
} de_ui_event_kind;
typedef struct de_ui_event {
    de_ui_event_kind kind;
    de_entity target;
    de_name action;
    f32 value;
} de_ui_event;

/** UI action context policy. NONE releases the context; NON_MODAL passes bindings through;
 * MODAL consumes bound inputs. A visible modal popup temporarily enforces MODAL. */
typedef enum de_ui_input_mode {
    DE_UI_INPUT_NONE,
    DE_UI_INPUT_NON_MODAL,
    DE_UI_INPUT_MODAL
} de_ui_input_mode;
/** Defaults to NON_MODAL. Changes take effect at the next input frame boundary. */
DE_API void de_ui_set_input_mode(de_ui *ui, de_ui_input_mode mode);
/** Logical focus survives mouse movement; this reports whether its ring is visible. */
DE_API bool de_ui_focus_visible(const de_ui *ui);
/** Null clears focus. Non-null must be enabled, visible, focusable and in the active scope.
 * Main thread, outside parallel schedules. Focus/blur events are emitted on change. */
DE_API void de_ui_set_focus(de_ui *ui, de_entity e);
DE_API de_entity de_ui_focused(const de_ui *ui);
/** Animate a reflected f32 field; accepts de_ui_easing values. Replacing the same field
 * begins from its current value. Invalid arguments set the error context. Main thread. */
DE_API void de_ui_tween(de_ui *ui, de_entity e, const char *property, f32 to, f32 seconds,
                        u32 easing);

/** Retained draw-list statistics from the last built/extracted/rendered frame. */
typedef struct de_ui_render_stats {
    u32 quads, draw_calls, roots, truncated;
    f32 build_extract_ms;
} de_ui_render_stats;
DE_API de_ui_render_stats de_ui_get_render_stats(const de_ui *ui);

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
