#include "render/post/settings.h"

#include <math.h>

static f32 parameter(f32 value, f32 fallback, f32 lo, f32 hi) {
    if (!isfinite(value) || value == 0 || (lo >= 0 && value < 0))
        value = fallback;
    return fminf(hi, fmaxf(lo, value));
}

de_post_settings de__post_settings_resolve(const de_post_settings *settings) {
    de_post_settings s = settings ? *settings : (de_post_settings){0};
    s.bloom_intensity = parameter(s.bloom_intensity, 0.04f, 0, 1000);
    s.bloom_threshold = parameter(s.bloom_threshold, 1, 0, 65504);
    s.exposure_compensation = parameter(s.exposure_compensation, 0, -32, 32);
    s.exposure_min_ev100 = parameter(s.exposure_min_ev100, -8, -32, 32);
    s.exposure_max_ev100 = parameter(s.exposure_max_ev100, 16, -32, 32);
    if (s.exposure_min_ev100 > s.exposure_max_ev100) {
        const f32 tmp = s.exposure_min_ev100;
        s.exposure_min_ev100 = s.exposure_max_ev100;
        s.exposure_max_ev100 = tmp;
    }
    s.exposure_speed_up = parameter(s.exposure_speed_up, 3, 0.001f, 1000);
    s.exposure_speed_down = parameter(s.exposure_speed_down, 1, 0.001f, 1000);
    s.dof_focal_length_mm = parameter(s.dof_focal_length_mm, 50, 1, 1000);
    s.dof_f_stop = parameter(s.dof_f_stop, 2.8f, 0.1f, 128);
    s.dof_focus_distance = parameter(s.dof_focus_distance, 10, 0.001f, 1e9f);
    s.motion_blur_shutter_angle = parameter(s.motion_blur_shutter_angle, 180, 0, 360);
    if (!s.motion_blur_samples)
        s.motion_blur_samples = 8;
    if (s.motion_blur_samples > 64)
        s.motion_blur_samples = 64;
    s.grading_lut_weight = parameter(s.grading_lut_weight, 1, 0, 1);
    s.grain_intensity = parameter(s.grain_intensity, 0.05f, 0, 1);
    s.grain_size = parameter(s.grain_size, 1, 0.1f, 16);
    s.vignette_intensity = parameter(s.vignette_intensity, 0.3f, 0, 1);
    s.vignette_smoothness = parameter(s.vignette_smoothness, 0.5f, 0, 1);
    s.vignette_roundness = parameter(s.vignette_roundness, 1, 0, 1);
    return s;
}

de_hdr_settings de__hdr_settings_resolve(const de_hdr_settings *settings) {
    de_hdr_settings s = settings ? *settings : (de_hdr_settings){0};
    s.paper_white_nits = parameter(s.paper_white_nits, 200, 1, 10000);
    s.peak_nits = parameter(s.peak_nits, 1000, s.paper_white_nits, 10000);
    s.ui_brightness_nits = parameter(s.ui_brightness_nits, 200, 1, s.peak_nits);
    return s;
}
