#ifndef DE_RENDER_RT_REFLECTIONS_H
#define DE_RENDER_RT_REFLECTIONS_H
#include "render/rt/rt_internal.h"
#include "render/lighting/lighting.h"
typedef struct de__rt_reflections de__rt_reflections;
de_result de__rt_reflections_create(de_renderer *, de__rt_reflections **);
/** Immutable pipelines, once after effective non-capture view selection (render thread). */
de_result de__rt_reflections_pipelines(de__rt_reflections *);
void de__rt_reflections_destroy(de__rt_reflections *);
de_result de__rt_reflections_build(de__rt_reflections *, de_rg *, de__vis_context *,
                                   de__lighting_context *, de__lighting_inputs *);
typedef struct de__rt_reflections_debug {
    /** Optional render-thread diagnostic of resolved RGBA16F radiance before denoising.
     * May append graph readback passes declaring COPY_SRC reads; must not modify radiance.
     * Caller owns capture storage and user lifetime through graph completion. NULL disables
     * the hook with no capture allocation or GPU work. */
    void (*capture_raw)(void *user, de_rg *graph, de_view view, de_rg_resource radiance, u32 width,
                        u32 height);
    void *capture_user;
} de__rt_reflections_debug;
void de__rt_reflections_set_debug(de__rt_reflections *, const de__rt_reflections_debug *);
typedef struct de__rt_reflections_info {
    /* Lighting output (normally denoised in place), or explicit reference output.
     * Use capture_raw for pre-denoise estimator-convergence measurements. */
    de_gpu_texture radiance, distance;
    de_gpu_buffer rays, hits;
    u64 frame, rays_traced;
    u32 width, height, scale, samples;
} de__rt_reflections_info;
bool de__rt_reflections_get(const de__rt_reflections *, de_view, de__rt_reflections_info *);
/* Independent fixed-Hammersley integration mode for acceptance; zero restores production1spp. */
void de__rt_reflections_reference(de__rt_reflections *, u32 samples);
#endif
