#ifndef DE_UI_DRAW_LIST_H
#define DE_UI_DRAW_LIST_H
#include "ui/ui_internal.h"
/* CPU node/style reads are confined to PostUpdate; Extract copies only this owned packet. */
typedef enum de_ui_quad_kind { DE_UI_QUAD_RECT, DE_UI_QUAD_IMAGE, DE_UI_QUAD_TEXT } de_ui_quad_kind;
typedef struct de_ui_gpu_quad {
    de_vec4 rect, uv, color, border, params, outline, shadow, shadow_offset;
    u32 texture, sampler, kind, flags;
} de_ui_gpu_quad;
_Static_assert(sizeof(de_ui_gpu_quad) == 144, "UI shader stride");
typedef struct de_ui_draw_cmd {
    de_ui_gpu_quad gpu;
    de_vec4 clip;
    de_texture image;
    de_font font;
    de_gpu_texture texture_lease;
    u32 page, root, pins;
} de_ui_draw_cmd;
typedef struct de_ui_root_list {
    de_entity entity;
    u32 first, count;
    bool world, transform_valid;
    DeUiWorldPanel panel;
    DeGlobalTransform transform;
} de_ui_root_list;
typedef struct de_ui_draw_list {
    de_ui_draw_cmd *commands;
    de_ui_root_list *roots;
    u32 count, root_count;
    u64 generation, last_gpu_fence;
    bool valid, camera_valid;
    de_view_frame_info camera;
} de_ui_draw_list;
typedef struct de_ui_draw_node {
    u32 index, root, popup, tree;
    i32 z;
} de_ui_draw_node;
typedef struct de_ui_inherited {
    de_vec4 clip;
    f32 opacity;
    u32 popup;
} de_ui_inherited;
typedef struct de_ui_text_cache {
    de_entity entity;
    de_text_layout *layout;
    u64 hash;
    u32 generation;
} de_ui_text_cache;
typedef struct de_ui_draw_emitter {
    struct de_ui_render *owner;
    const de_ui_draw_node *node;
    f32 opacity;
} de_ui_draw_emitter;
typedef void (*de_ui_text_geometry_fn)(de_ui *ui, de_entity entity, de_vec2 *physical_offset,
                                       f32 *wrap_width, void *user);
/** Optional physical-pixel text origin/wrap override; NULL preserves ordinary labels. */
void de_ui__set_text_geometry(de_ui *ui, de_ui_text_geometry_fn fn, void *user);
void de_ui__text_geometry(de_ui *ui, de_entity entity, f32 default_width, de_vec2 *offset,
                          f32 *wrap_width);
typedef void (*de_ui_widget_draw_fn)(de_ui *ui, de_entity entity, de_vec4 physical_rect,
                                     de_ui_draw_emitter *emitter, void *user);
/** Main thread, after render_attach; re-register on reattach. Geometry/insets are physical pixels.
 */
void de_ui__set_widget_draw(de_ui *ui, de_ui_widget_draw_fn fn, void *user);
void de_ui__emit_rect(de_ui_draw_emitter *emitter, de_vec4 rect, de_vec4 fill, de_vec4 border,
                      f32 border_width, f32 radius);

#define DE_UI_RENDER_SNAPSHOTS 4u
#define DE_UI_RENDER_GPU_SLOTS 4u
struct de_ui_render {
    de_ui *ui;
    u32 capacity;
    de_ui_draw_list build, snapshots[DE_UI_RENDER_SNAPSHOTS];
    de_ui_draw_node *nodes, *sort;
    de_ui_inherited *inherited;
    de_ui_text_cache *text_cache;
    de_glyph_quad *glyphs;
    u32 node_count, published;
    de_mutex mutex;
    bool mutex_ready, overflow_logged, hook_registered;
    de_system build_system, extract_system;
    de_cvar max_quads;
    de_ui_text_geometry_fn text_geometry;
    void *text_geometry_user;
    de_ui_widget_draw_fn widget_draw;
    void *widget_user;
    struct de_ui_world_state *world;
    de_view_frame_info pixel_layout;
    bool pixel_layout_valid;
    u64 stats_frame;
    de_ui_render_stats stats;
    de_log_category log;
#if DE_HAS_RENDER
    struct de_ui_pass_state *pass;
    de_gpu_device *device;
    de_gpu_sampler linear, nearest;
    de_gpu_pipeline pipeline;
    de_gpu_format pipeline_format;
    de_gpu_buffer buffers[DE_UI_RENDER_GPU_SLOTS];
    u64 upload_frame[DE_UI_RENDER_GPU_SLOTS];
#endif
};
de_vec4 de_ui__clip_intersect(de_vec4 a, de_vec4 b);
u32 de_ui__nine_slice(de_vec4 rect, de_vec4 uv, de_vec4 insets, u32 width, u32 height, f32 scale,
                      de_ui_gpu_quad out[9]);
void de_ui__build_draw_list(de_system_ctx *ctx);
void de_ui__extract_draw_list(de_system_ctx *ctx);
/** Main-thread retained order/clip pointers, valid until next PostUpdate (pointer routing). */
u32 de_ui__draw_order(const de_ui *ui, const de_ui_draw_node **nodes,
                      const de_ui_inherited **inherited);
bool de_ui__root_info(const de_ui *ui, u32 ordinal, de_entity *entity, de_vec4 *rect, bool *world);
/** Caller holds render mutex when used by a render consumer. */
const de_ui_draw_list *de_ui__packet_for_frame(const struct de_ui_render *r, u64 extract_token);
de_result de_ui__pass_init(struct de_ui_render *r);
void de_ui__pass_destroy(struct de_ui_render *r);
void de_ui__packet_unpin(struct de_ui_render *r, de_ui_draw_list *packet);
void de_ui__render_hook(de_rg *graph, de_view view, void *user);
#endif
