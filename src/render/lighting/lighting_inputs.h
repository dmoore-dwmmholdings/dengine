/*
 * lighting_inputs.h: the optional inputs of lighting (SPEC §12.5 steps 5-7; DENG-303), the C side
 * of shaders/lighting/lighting_inputs.slang. Internal to de_render; include as
 * "render/lighting/lighting_inputs.h".
 *
 * A CONTRACT, append only. DENG-304 (indirect light) and the S15 tasks (shadows, AO, contact
 * shadows) each add ONE field here and the same field, with its accessor and neutral default, in
 * lighting_inputs.slang:
 *   - de__lighting_inputs: the render-graph texture a producer publishes for the frame (an
 *     invalid handle = no producer: the shader then uses the neutral value, 1 for visibility and
 *     occlusion, 0 for added light, so a missing producer never changes the image);
 *   - de__lighting_inputs_gpu: the bindless index the lighting pass passes to the shader.
 * A producer runs before lighting in the per-view stage order (views_internal.h) and sets its
 * field through de__lighting_view_inputs (lighting.h); the inputs are reset after each frame's
 * lighting. Every input texture is at the view's internal size, read with Load (no sampler),
 * DE_ACCESS_SHADER_READ.
 */
#ifndef DE_RENDER_LIGHTING_LIGHTING_INPUTS_H
#define DE_RENDER_LIGHTING_LIGHTING_INPUTS_H

#include <dengine/de_base.h>
#include <dengine/de_render.h>

DE_EXTERN_C_BEGIN

/** The inputs one view's lighting reads this frame (render-graph textures; invalid = none). */
typedef struct de__lighting_inputs {
    de_rg_resource ao;                 /**< R: ambient occlusion (S15) */
    de_rg_resource shadow_directional; /**< RGBA: directional lights 0..3 visibility (S15) */
    de_rg_resource shadow_local;       /**< R: local lights visibility (S15 defines the layout) */
    de_rg_resource contact_shadows;    /**< R: contact shadows of directional light 0 (S15) */
    de_rg_resource indirect_diffuse;   /**< RGB: indirect diffuse radiance (DENG-304) */
    de_rg_resource indirect_specular;  /**< RGB: indirect specular radiance (DENG-304) */
    /** The scale of the lighting output (pre-exposure; DENG-304 sets it). 0 = 1. */
    f32 pre_exposure;
    /** DENG-306: the view's subsurface scattering runs after lighting (its `sss` quality is not
     * DE_QUALITY_OFF): the frame gets a diffuse target, and SUBSURFACE pixels write their diffuse
     * light only there (the SSS pass adds it back blurred). The lighting stage sets it. */
    bool subsurface;
    /* ---- append new inputs here (and in de__lighting_inputs_gpu, lighting_inputs.slang) ---- */
} de__lighting_inputs;

/** lighting_inputs.slang LightingInputs (32 bytes): bindless indices, 0 = none. */
typedef struct de__lighting_inputs_gpu {
    u32 ao, shadow_directional, shadow_local, contact_shadows;
    u32 indirect_diffuse, indirect_specular, reserved0, reserved1;
} de__lighting_inputs_gpu;
DE_STATIC_ASSERT(sizeof(de__lighting_inputs_gpu) == 32, "LightingInputs is 32 bytes");

/** The LTC tables (ltc_data.c; shaders/lighting/ltc.slang DE_LTC_SIZE). */
#define DE__LTC_SIZE 64
/** [roughness][sqrt(1 - cos theta_v)]: M^-1 normalised ((1,1) = 1) as m00, m02, m20, m22. */
extern const float de__ltc_tab1[DE__LTC_SIZE][DE__LTC_SIZE][4];
/** [roughness][sqrt(1 - cos theta_v)]: the Schlick split of the GGX reflectance (F0 x + F90 y). */
extern const float de__ltc_tab2[DE__LTC_SIZE][DE__LTC_SIZE][2];

DE_EXTERN_C_END

#endif /* DE_RENDER_LIGHTING_LIGHTING_INPUTS_H */
