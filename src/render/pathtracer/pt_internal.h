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
void de__pt_sss_coefficients(de_vec3 radius, de_vec3 albedo, de_vec3 *sigma_s, de_vec3 *sigma_a);
f32 de__pt_hg(f32 cosine, f32 g);
typedef struct de__pt_media de__pt_media;
typedef struct de__pt_media_params {
    u32 fog, count, features, max_steps, stats, stats_offset, unused0, unused1;
} de__pt_media_params;
de_result de__pt_media_create(de_renderer *, de__pt_media **);
void de__pt_media_destroy(de__pt_media *);
de_result de__pt_media_frame(de__pt_media *, de__view_frame *, de__pt_media_params *, u64 *);
void de__pt_media_begin(de__pt_media *, de_gpu_cmd *, const de__pt_media_params *);
void de__pt_media_end(de__pt_media *, de_gpu_cmd *, const de__pt_media_params *);
void de__pt_media_graph(de__pt_media *, de_rg *, de_rg_pass *);
#endif
