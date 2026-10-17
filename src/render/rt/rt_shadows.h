#ifndef DE_RENDER_RT_SHADOWS_H
#define DE_RENDER_RT_SHADOWS_H
#include "render/rt/rt_internal.h"
#include "render/lighting/lighting.h"
typedef struct de__rt_shadows de__rt_shadows;
/* C mirror of light_sampling.slang, for physical sampling acceptance and future CPU tools. */
de__rt_ray de__rt_shadow_ray(const de__cluster_light *, de_vec3 position, de_vec3 normal,
                             de_vec2 u);
de_result de__rt_shadows_create(de_renderer *, de__rt_shadows **);
void de__rt_shadows_destroy(de__rt_shadows *);
de_result de__rt_shadows_build(void *, de_rg *, de__vis_context *, de__lighting_context *,
                               de__lighting_inputs *);
typedef struct de__rt_shadows_info {
    de_gpu_texture visibility, distance;
    de_gpu_buffer keys;
    u32 width, height, slots, scale, spp;
    u64 rays, frame;
} de__rt_shadows_info;
bool de__rt_shadows_get(de__rt_shadows *, de_view, de__rt_shadows_info *);
#endif
