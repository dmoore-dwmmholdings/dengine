/* GTAO's internal, RTAO-compatible output contract: full-size sampled R8_UNORM raw geometric
 * visibility, optional RG8_UNORM world-space octahedral bent normal mapped from [-1,1] to [0,1].
 * Material AO/min, RGB multi-bounce and Lagarde specular occlusion belong to lighting once.
 * AO owns independent unitless histories; it never mutates SSR/GI scene-colour history. */
#ifndef DE_RENDER_PASSES_AO_GTAO_INTERNAL_H
#define DE_RENDER_PASSES_AO_GTAO_INTERNAL_H
#include "render/passes/vis/vis_stages.h"
#include "render/lighting/lighting.h"
DE_EXTERN_C_BEGIN
typedef struct de__gtao de__gtao;
typedef struct de__gtao_params {
    u32 scene, slot, depth, normal;
    u32 motion, width, height, tw;
    u32 th, slices, steps, frame;
    u32 raw, spatial, previous, previous_surface;
    u32 history, history_surface, packed, pitch_ao;
    u32 pitch_bent, bent_offset, history_valid, bent;
    f32 radius, falloff, thickness, history_weight;
} de__gtao_params;
DE_STATIC_ASSERT(sizeof(de__gtao_params) == 112, "GtaoParams is 112 bytes");
void de__gtao_quality(de_quality quality, u32 width, u32 height, u32 *tw, u32 *th, u32 *slices,
                      u32 *steps);
typedef struct de__gtao_config {
    f32 radius, falloff, thickness;
    bool bent;
} de__gtao_config;
typedef struct de__gtao_info {
    u64 frame;
    u32 width, height, trace_width, trace_height, slices, steps, dispatches;
    bool history_valid;
    de_gpu_texture output, bent, history, surface;
} de__gtao_info;
de_result de__gtao_create_ex(de_gpu_device *dev, const de_allocator *alloc, de__vis_shared *vis,
                             de__gtao **out);
de_result de__gtao_create(de_renderer *r, de__gtao **out);
void de__gtao_destroy(de__gtao *s);
de_result de__gtao_build(de__gtao *s, const de__view_frame *vf, const de__vis_context *vc,
                         const de__gtao_config *config, de__lighting_inputs *inputs);
/** End each built graph before resetting its arena. Pre-execution abort permits retry;
 * execution attempts conservatively invalidate unpublished history on submit failure. */
void de__gtao_complete(de__gtao *s, de_view view, const de_rg *graph, bool attempted, bool success);
bool de__gtao_get(const de__gtao *s, de_view view, de__gtao_info *out);
DE_EXTERN_C_END
#endif
