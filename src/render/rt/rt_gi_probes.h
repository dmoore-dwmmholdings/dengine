#ifndef DE_RENDER_RT_GI_PROBES_H
#define DE_RENDER_RT_GI_PROBES_H
#include "render/rt/rt_internal.h"
#include "render/probes/irradiance_internal.h"
typedef struct de__rt_gi de__rt_gi;
de_result de__rt_gi_create(de_renderer *, de__rt_gi **);
void de__rt_gi_destroy(de__rt_gi *);
void de__rt_gi_complete(de__rt_gi *, de_view, de_rg *, u64 frame, bool success);
void de__rt_gi_prepare(de__rt_gi *, const de__render_world_frame *);
de_vec2 de__ddgi_oct_encode(de_vec3);
de_vec3 de__ddgi_oct_decode(de_vec2);
f32 de__ddgi_hysteresis(f32 previous, f32 current, f32 hysteresis, bool valid);
typedef struct de__rt_gi_info {
    de__gi_layout layout;
    de__gi_gpu gpu;
    de_gpu_texture irradiance, distance;
    de_gpu_buffer classification, rays, hits, metadata;
    u32 cascades, volumes, probes_updated, rays_per_probe, cursor;
    u64 frame, rays_traced, invalidated;
} de__rt_gi_info;
bool de__rt_gi_get(const de__rt_gi *, de_view, u32 volume, de__rt_gi_info *);
#endif
