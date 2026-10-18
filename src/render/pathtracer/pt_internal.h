#ifndef DE_RENDER_PT_INTERNAL_H
#define DE_RENDER_PT_INTERNAL_H
#include "render/views_internal.h"
#include "render/lighting/lighting.h"
typedef struct de__pt de__pt;
de_result de__pt_create(de_renderer *, de__pt **);
void de__pt_destroy(de__pt *);
bool de__pt_enabled(const de_renderer *, const de_render_settings *);
void de__pt_build(de__view_frame *, void *);
void de__pt_complete(de__pt *, de_view, u64, bool);
typedef struct de__pt_info {
    de_gpu_texture accumulation, output;
    u32 samples, width, height;
} de__pt_info;
bool de__pt_get(const de_renderer *, de_view, de__pt_info *);
/** Equal-area environment cell CDF; shared CPU oracle, capacity count+1 is not required. */
void de__pt_env_cdf(const f32 *weights, u32 count, f32 *cdf, f32 *pdf);
f32 de__pt_power(f32 a, f32 b);
#endif
