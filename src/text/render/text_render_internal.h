#ifndef DE_TEXT_RENDER_INTERNAL_H
#define DE_TEXT_RENDER_INTERNAL_H
#include "text/glyph_cache_internal.h"
#if DE_HAS_RENDER
#include "render/renderer_internal.h"
#include "render/views_internal.h"
#include "render/shaders/engine_shaders.h"
#include "render/graph/rg_internal.h"
#include "render/loaders/upload_ring.h"
#include "render/lighting/lighting.h"
#define DE_TEXT_DRAW_CAP    65536
#define DE_TEXT_PAGE_CAP    1024
#define DE_TEXT_FRAME_BYTES (32u * 1024u * 1024u)
/* Exactly12float4s; Slang reads raw byte offsets to work with uploader raw buffers. */
typedef struct de__text_gpu_glyph {
    de_vec4 rect, uv, color, outline, shadow, clip, origin, right, up;
    u32 texture, sampler, mode, flags;
    f32 range, outline_width, shadow_x, shadow_y;
    de_vec4 reserved;
} de__text_gpu_glyph;
_Static_assert(sizeof(de__text_gpu_glyph) == 192, "text glyph stride");
typedef struct de__text_item {
    de__text_gpu_glyph gpu;
    de_font font;
    u32 page;
    de_view view;
    de_dvec3 position;
} de__text_item;
typedef struct de__text_queue {
    u64 frame;
    bool valid;
    u32 screen_count, world_count;
    de__text_item *screen, *world;
} de__text_queue;
typedef struct de__text_page {
    de_font font;
    const de_text_font *identity;
    u32 page;
    de_gpu_texture texture;
    u32 sampled, storage;
    bool initialized;
} de__text_page;
typedef struct de__text_push {
    u32 data, first, view_offset, lighting_view, lighting_entry;
    u32 lights, dir_lights, grid, list, counters, ltc1, ltc2, ltc_sampler;
    u32 world;
} de__text_push;
typedef struct de__text_view {
    de_mat4 view_proj, view;
    de_vec4 camera, viewport;
} de__text_view;
struct de__text_render {
    de_text_system *text;
    de_renderer *renderer;
    de_gpu_device *device;
    de_world *world;
    de_system extract;
    de_component component;
    de_mutex mutex;
    u64 next_frame, current_frame;
    de__text_queue queues[3];
    de__text_page pages[DE_TEXT_PAGE_CAP];
    u32 page_count;
    de_gpu_buffer frames[3];
    u8 *mapped[3];
    u64 gpu_frame[3];
    u32 cursor[3];
    de_gpu_sampler linear, nearest;
    u32 linear_index, nearest_index;
    de_gpu_pipeline ui_pipeline, world_pipeline, upload_pipeline;
    de__engine_shader_code code, upload_code;
    de_cvar white_nits;
    de_text_render_stats stats;
};
void de_text__render_destroy(de_text_system *t);
void de_text__render_begin(de_text_system *t);
de__text_page *de__text_page_ensure(struct de__text_render *r, de_font font, u32 page);
void de__text_atlas_sync(struct de__text_render *r, de__view_frame *vf);
de_result de__text_world_attach(struct de__text_render *r, de_world *w);
void de__text_world_detach(struct de__text_render *r);
void de__text_world_stage(de__view_frame *vf, void *user);
void de__text_ui_stage(de__view_frame *vf, void *user);
void de__text_draw_stage(struct de__text_render *r, de__view_frame *vf, bool world);
#endif
#endif
