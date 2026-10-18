#ifndef DE_RENDER_DENOISE_POLICY_H
#define DE_RENDER_DENOISE_POLICY_H
#include "render/denoise/denoise_internal.h"
#include "render/passes/vis/vis_stages.h"
#include "render/lighting/lighting.h"
typedef struct de__signal_denoiser de__signal_denoiser;
de_result de__signal_denoiser_create(de_renderer *, de__signal_denoiser **);
void de__signal_denoiser_destroy(de__signal_denoiser *);
f32 de__shadow_penumbra(f32 source_radius, f32 blocker_distance, f32 light_distance,
                        f32 pixels_per_metre, bool directional);
de_result de__denoise_shadow(de__signal_denoiser *, de_rg *, const de__vis_context *,
                             const de__lighting_context *, de_rg_resource raw,
                             de_rg_resource distance, de_rg_resource keys, de_rg_resource output,
                             u32 width, u32 height, u32 slots, u32 scale, bool reference);
de_result de__denoise_ao(de__signal_denoiser *, de__view_frame *, const de__vis_context *,
                         de_rg_resource raw, de_rg_resource output, u32 width, u32 height,
                         u32 scale, bool reference);
/* Internal common adapter shared by the two policy implementations. */
de_result de__denoise_policy_build(de__signal_denoiser *, de_rg *, const de__vis_context *,
                                   const de__lighting_context *, de_rg_resource, de_rg_resource,
                                   de_rg_resource, de_rg_resource, u32, u32, u32, u32, u32, bool);

#endif
