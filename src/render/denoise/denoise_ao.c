#include "render/denoise/denoise_policy.h"
de_result de__denoise_ao(de__signal_denoiser *s, de__view_frame *vf, const de__vis_context *vc,
                         de_rg_resource raw, de_rg_resource output, u32 width, u32 height,
                         u32 scale, bool reference) {
    return de__denoise_policy_build(s, vf->graph, vc, NULL, raw, (de_rg_resource){0},
                                    (de_rg_resource){0}, output, width, height, 1, scale, 1,
                                    reference);
}
