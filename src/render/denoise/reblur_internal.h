#ifndef DE_RENDER_DENOISE_REBLUR_INTERNAL_H
#define DE_RENDER_DENOISE_REBLUR_INTERNAL_H
#include "render/denoise/denoise_internal.h"
#include "render/passes/vis/vis_stages.h"
#include "render/lighting/lighting.h"
typedef struct de__radiance_denoiser de__radiance_denoiser;
de_result de__radiance_denoiser_create(de_renderer *, de__radiance_denoiser **);
void de__radiance_denoiser_destroy(de__radiance_denoiser *);
de_result de__denoise_diffuse(de__radiance_denoiser *, de_rg *, const de__vis_context *,
                              const de__lighting_context *, de_rg_resource radiance,
                              de_rg_resource distance, bool reference);
de_result de__denoise_specular(de__radiance_denoiser *, de_rg *, const de__vis_context *,
                               const de__lighting_context *, de_rg_resource radiance,
                               de_rg_resource distance, bool reflection, bool reference,
                               de_rg_resource rays, u32 scale);
/* DENG-74 can reuse the same policy for a per-pixel indirect input; DDGI atlases excluded. */
de_result de__reblur_build(de__radiance_denoiser *, de_rg *, const de__vis_context *,
                           const de__lighting_context *, de_rg_resource, de_rg_resource, u32 kind,
                           bool reference, de_rg_resource rays, u32 scale);
f32 de__reblur_roughness_alpha(f32 roughness);
de_vec2 de__reblur_virtual_uv(de_vec3 camera, de_vec3 position, de_vec3 normal, f32 distance,
                              const de_mat4 *previous, de_vec2 uv, de_vec2 motion, f32 roughness);
#endif
