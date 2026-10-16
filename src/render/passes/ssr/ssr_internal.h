/* Shared screen-space depth/history contract (DENG-339, DENG-341).
 * Render-thread APIs. All graph resources belong to one view/frame; never retain them.
 * History is linear pre-exposed HDR, independent of enabling SSR. Consumers correct exposure
 * by current/previous before using it, then remove current exposure for lighting overrides.
 * Persistent history uses storage-only GENERAL descriptors, avoiding sampled/UAV aliasing.
 */
#ifndef DE_RENDER_SSR_INTERNAL_H
#define DE_RENDER_SSR_INTERNAL_H
#include "render/passes/vis/vis_stages.h"
#include "render/lighting/lighting.h"
DE_EXTERN_C_BEGIN
typedef struct de__scene_history de__scene_history;
typedef struct de__ssr de__ssr;
typedef struct de__screen_space_context {
    de_view view;
    de_rg *graph;
    u64 frame;
    u32 width, height;
    de__vis_hzb_target closest;
    de_rg_resource previous_color, previous_surface;
    de_rg_resource write_color, write_surface;
    f32 previous_exposure, current_exposure;
    bool valid, capture_added;
} de__screen_space_context;
de_result de__scene_history_create(de_renderer *r, de__scene_history **out);
void de__scene_history_destroy(de__scene_history *h);
/** Builds a separate MAX pyramid from final depth; never changes the culling MIN pyramid. */
de__screen_space_context *de__scene_history_prepare(de__scene_history *h, const de__view_frame *vf,
                                                    const de__vis_context *vc);
/** Captures the lit HDR target before output.basic. Does not publish until complete succeeds. */
void de__scene_history_capture(de__scene_history *h, const de__view_frame *vf,
                               const de__lighting_context *lc);
void de__scene_history_complete(de__scene_history *h, de_view view, u64 frame, bool success);
de__screen_space_context *de__scene_history_context(de__scene_history *h, de_view view, de_rg *g,
                                                    u64 frame);
de_result de__ssr_create(de_renderer *r, de__ssr **out);
void de__ssr_destroy(de__ssr *s);
void de__ssr_view_complete(de__ssr *s, de_view view, u64 frame, bool success);
/** Reusable even when reflections=OFF. Probe-capture views deliberately have no history. */
de__scene_history *de__ssr_scene_history(de__ssr *s);
typedef struct de__ssr_info {
    u64 frame;
    u32 width, height, trace_width, trace_height, dispatches;
    bool history_valid;
    f32 previous_exposure, current_exposure;
    de_gpu_texture output;
    de_gpu_texture temporal, moments; /**< Owned persistent history; borrowed until resize. */
} de__ssr_info;
bool de__ssr_view_info(const de__ssr *s, de_view view, de__ssr_info *out);
DE_EXTERN_C_END
#endif
