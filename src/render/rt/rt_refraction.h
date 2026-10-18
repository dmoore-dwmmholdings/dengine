#ifndef DE_RENDER_RT_REFRACTION_H
#define DE_RENDER_RT_REFRACTION_H
#include "render/rt/rt_internal.h"
#include "render/lighting/lighting.h"
#include "render/passes/forward/forward.h"
typedef struct de__rt_refraction de__rt_refraction;
typedef struct de__rt_refraction_frame de__rt_refraction_frame;
de_result de__rt_refraction_create(de_renderer *, de__rt_refraction **);
void de__rt_refraction_destroy(de__rt_refraction *);
void de__rt_refraction_reset(de__rt_refraction *, de_view);
de__rt_refraction_frame *de__rt_refraction_prepare(de__rt_refraction *, de_rg *, de__vis_context *,
                                                   de__lighting_context *);
void de__rt_refraction_declare(de__rt_refraction_frame *, de_rg_pass *);
void de__rt_refraction_clear(de_gpu_cmd *, de__rt_refraction_frame *, de__fwd_params *);
void de__rt_refraction_trace(de_gpu_cmd *, de__rt_refraction_frame *, const de__fwd_params *);
typedef struct de__rt_refraction_info {
    de_gpu_buffer capture, radiance, path, rays, hits, diagnostic;
    u32 width, height, events, draws;
    u64 frame, rays_traced;
} de__rt_refraction_info;
bool de__rt_refraction_get(const de__rt_refraction *, de_view, de__rt_refraction_info *);
#endif
