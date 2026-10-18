#include "render/denoise/reblur_internal.h"
#include <math.h>
f32 de__reblur_roughness_alpha(f32 roughness) {
    f32 r = fminf(fmaxf(roughness, 0), 1);
    return fmaxf(.05f, .5f * (1 - r) * (1 - r) * (1 - r));
}
de_vec2 de__reblur_virtual_uv(de_vec3 camera, de_vec3 position, de_vec3 normal, f32 distance,
                              const de_mat4 *previous, de_vec2 uv, de_vec2 motion, f32 roughness) {
    de_vec3 incident = de_v3_normalize(de_v3_sub(position, camera));
    de_vec3 reflected = de_v3_sub(incident, de_v3_scale(normal, 2 * de_v3_dot(incident, normal)));
    de_vec3 actual = de_v3_add(position, de_v3_scale(reflected, distance));
    de_vec3 virtual = de_v3_sub(
        actual, de_v3_scale(normal, 2 * de_v3_dot(de_v3_sub(actual, position), normal)));
    f32 v[4] = {virtual.x, virtual.y, virtual.z, 1}, clip[4] = {0};
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            clip[r] += previous->m[c * 4 + r] * v[c];
    de_vec2 target = {clip[0] / fmaxf(fabsf(clip[3]), 1e-20f) * .5f + .5f,
                      -clip[1] / fmaxf(fabsf(clip[3]), 1e-20f) * .5f + .5f},
            surface = de__denoise_previous_uv(uv, motion);
    f32 blend = fminf(fmaxf(roughness * roughness * 4, 0), 1);
    return (de_vec2){target.x + (surface.x - target.x) * blend,
                     target.y + (surface.y - target.y) * blend};
}
de_result de__denoise_specular(de__radiance_denoiser *s, de_rg *g, const de__vis_context *vc,
                               const de__lighting_context *lc, de_rg_resource radiance,
                               de_rg_resource distance, bool reflection, bool reference,
                               de_rg_resource rays, u32 scale) {
    return de__reblur_build(s, g, vc, lc, radiance, distance, reflection ? 2 : 1, reference, rays,
                            scale);
}
