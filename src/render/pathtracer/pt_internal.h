#ifndef DE_RENDER_PT_INTERNAL_H
#define DE_RENDER_PT_INTERNAL_H
#include "render/views_internal.h"
#include "render/lighting/lighting.h"
typedef struct de__pt de__pt;
de_result de__pt_create(de_renderer *, de__pt **);
void de__pt_destroy(de__pt *);
/** Render thread: initialize immutable pipelines only for a selected RT path-traced view.
 * A failed attempt is retained and reported; unrelated views never compile PT shaders. */
de_result de__pt_prepare(de_renderer *, const de_render_settings *);
/** Lazily compile the native Metal media integrator; failures are cached like base preparation. */
de_result de__pt_prepare_media(de_renderer *);
bool de__pt_enabled(const de_renderer *, const de_render_settings *);
void de__pt_build(de__view_frame *, void *);
void de__pt_complete(de__pt *, de_view, u64, bool);
typedef struct de__pt_info {
    de_gpu_texture accumulation, output;
    u32 samples, width, height;
    de_gpu_texture albedo, normal, depth, position, material;
    de_gpu_texture moments; /**< Welford M2 of raw path radiance, independently for R/G/B. */
    de_gpu_buffer directional_alias;
    u32 directional_count, directional_offset;
} de__pt_info;
bool de__pt_get(const de_renderer *, de_view, de__pt_info *);
/** Equal-area environment cell CDF; shared CPU oracle, capacity count+1 is not required. */
void de__pt_env_cdf(const f32 *weights, u32 count, f32 *cdf, f32 *pdf);
f32 de__pt_power(f32 a, f32 b);
void de__pt_sss_coefficients(de_vec3 radius, de_vec3 albedo, de_vec3 *sigma_s, de_vec3 *sigma_a);
f32 de__pt_hg(f32 cosine, f32 g);
typedef struct de__pt_media de__pt_media;
struct de__fog_snapshot;
/** One PT fog region as the shader reads it (PtFog, pt_volume.slang): the inverse affine (world to
 * fog space, row major 3x4), (half extent xyz or x < 0 for global, density), (albedo rgb, g) and
 * params = (height_falloff, noise, edge_fade, entity z). */
typedef struct de__pt_fog {
    f32 inverse[3][4], extent_density[4], albedo_g[4], params[4];
} de__pt_fog;
/** DENG-848: packs a DeFogVolume snapshot (a zero extent is global fog). */
void de__pt_fog_pack(const struct de__fog_snapshot *s, de__pt_fog *out);
/** True when the density varies inside the region (height falloff, noise, edge fade); a
 * homogeneous region keeps the analytic one-exponential path. */
bool de__pt_fog_heterogeneous(const de__pt_fog *fog);
/** CPU mirror of pt_fog_density: the density (1/m) at world point `p` (same formula as the froxel
 * fog_volume_density). */
f32 de__pt_fog_density(const de__pt_fog *fog, de_vec3 p);
/** CPU mirror of pt_fog_majorant: an upper bound of the density on the ray segment
 * origin + t * direction, t in [t0, t1]. */
f32 de__pt_fog_majorant(const de__pt_fog *fog, de_vec3 origin, de_vec3 direction, f32 t0, f32 t1);
typedef struct de__pt_media_params {
    u32 fog, count, features, max_steps, stats, stats_offset, unused0, unused1;
} de__pt_media_params;
de_result de__pt_media_create(de_renderer *, de__pt_media **);
void de__pt_media_destroy(de__pt_media *);
de_result de__pt_media_frame(de__pt_media *, de__view_frame *, de__pt_media_params *, u64 *);
void de__pt_media_begin(de__pt_media *, de_gpu_cmd *, const de__pt_media_params *);
void de__pt_media_end(de__pt_media *, de_gpu_cmd *, const de__pt_media_params *);
void de__pt_media_graph(de__pt_media *, de_rg *, de_rg_pass *);
typedef struct de__pt_denoise de__pt_denoise;
de_result de__pt_denoise_create(de_renderer *, de__pt_denoise **);
void de__pt_denoise_destroy(de__pt_denoise *);
void de__pt_denoise_forget(de__pt_denoise *, de_view);
void de__pt_forget(de__pt *, de_view);
de_rg_resource de__pt_denoise_build(de__pt_denoise *, de__view_frame *, const de__pt_info *,
                                    f32 exposure);
#endif
