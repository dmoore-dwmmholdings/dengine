#ifndef DE_RENDER_DENOISE_INTERNAL_H
#define DE_RENDER_DENOISE_INTERNAL_H
#include "render/views_internal.h"

typedef struct de__denoise de__denoise;
typedef struct de_denoise_ctx de_denoise_ctx;
typedef enum de_denoise_clamp {
    DE_DENOISE_CLAMP_NONE,
    DE_DENOISE_CLAMP_LUMINANCE,
    DE_DENOISE_CLAMP_YCOCG
} de_denoise_clamp;
/** Append-only shared signal policy. identity includes per-light/channel ownership when needed. */
typedef struct de_denoise_signal_desc {
    u64 identity;
    de_gpu_format format;
    u32 channels; /**< 1/2: independent scalar signals; R drives variance/weights. 3/4: RGB. */
    f32 alpha_min;
    de_denoise_clamp clamp;
    u32 iterations;
    f32 depth_relative, normal_cos, normal_power, luminance_scale, hit_distance_scale;
    f32 roughness_threshold;
    bool zero_radius_passthrough;
    /** Filter independent-sample variance with squared spatial weights between wavelet levels. */
    bool propagate_variance;
} de_denoise_signal_desc;
/** Canonical motion is current-minus-previous NDC; normal is canonical encoded GB1.
 * material is RG32_FLOAT actual identity (low/high 16-bit words), never GB2's model id. Use the
 * material builder below for visibility-buffer views. linear_depth is for already-linear AOV
 * inputs.
 */
typedef struct de_denoise_inputs {
    de_rg_resource signal, depth, normal, motion, material, hit_distance;
    u32 width, height;
    de_mat4 inverse_view_proj, previous_inverse_view_proj;
    de_vec3 camera_position, previous_origin_delta;
    bool linear_depth;
    de_rg_resource validation_key; /**< Optional exact numeric low/high16 key words. */
    de_rg_resource position;       /**< Optional exact representative world-position guide. */
    de_rg_resource temporal_alpha; /**< Optional per-pixel EMA minimum. */
    bool normal_is_world; /**< Direct world-space XYZ plus roughness AOV, rather than GB1. */
} de_denoise_inputs;
typedef struct de_denoise_frame {
    de_rg *graph;
    de_view view;
    u64 frame;
    bool reset;
    de_denoise_inputs inputs;
} de_denoise_frame;
typedef struct de_denoise_result {
    de_rg_resource color, moments, variance, history_length, validity;
} de_denoise_result;

de_result de__denoise_create(de_renderer *, de__denoise **);
void de__denoise_destroy(de__denoise *);
void de__denoise_forget(de__denoise *, de_view);
de_result denoise_begin(de__denoise *, const de_denoise_frame *, const de_denoise_signal_desc *,
                        de_denoise_ctx **);
de_denoise_result denoise_add_reproject(de_denoise_ctx *);
de_denoise_result denoise_add_temporal(de_denoise_ctx *, de_denoise_result);
de_denoise_result denoise_add_atrous(de_denoise_ctx *, de_denoise_result);
/** Build actual material identity from canonical raw visibility rows and scene tables.
 * Cached once per graph generation/scene slot; reuse across all signals in that view.
 * Call begin/builders once per signal/frame. Pass reset for camera cut or invalid view history. */
de_rg_resource denoise_add_material_identity(de__denoise *, de_rg *, u32 width, u32 height,
                                             de_rg_resource visibility, de_rg_resource visible,
                                             de_rg_resource scene, u32 scene_slot,
                                             u32 visibility_row_pitch);
/** Current-minus-previous NDC to texture UV, reusable CPU convention oracle. */
de_vec2 de__denoise_previous_uv(de_vec2 uv, de_vec2 motion);
#endif
