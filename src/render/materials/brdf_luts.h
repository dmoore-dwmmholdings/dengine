/*
 * brdf_luts.h: the energy and sheen LUT of the Standard model (SPEC §12.6; DENG-299, DENG-300).
 * Internal to de_render; include as "render/materials/brdf_luts.h".
 *
 * One DE__BRDF_LUT_SIZE x DE__BRDF_LUT_ROWS RGBA32F texture, made once per renderer, x =
 * cos(theta_o) (texel centres (i + 0.5) / size):
 *   - rows 0 .. size - 1, the energy LUT (DENG-299): y = perceptual roughness (j / (size - 1));
 *     R, G the split-sum DFG scale and bias A, B; B, A their cosine-weighted averages over
 *     cos(theta_o) (shaders/brdf/energy.slang has the layout and the lookup);
 *   - rows size .. 2 size - 1, the sheen LUT (DENG-300): y = sheen roughness from
 *     DE__SHEEN_ROUGHNESS_MIN to 1; R the directional albedo of the Charlie sheen lobe for sheen
 *     colour 1, G its cosine-weighted average (shaders/brdf/sheen.slang). The engine shader
 * brdf_lut_gen (shaders/brdf/lut_gen.slang; built and embedded like every engine shader, DENG-421)
 * computes it on the graphics queue at create into a staging buffer that a copy moves into the
 * texture (no storage image), with a fixed Hammersley sequence: the values do not depend on time,
 * so every run makes the same LUT.
 *
 * Who reads it: anything that calls the Standard model's evaluate / sample / pdf passes the
 * bindless index (de__renderer_brdf_lut) to the shader, which puts it in SurfaceData.brdf_lut:
 * deferred lighting (DENG-303), IBL (DENG-304, which also reads A and B as the split-sum DFG),
 * RT (S16, S17). It is read with Load (no sampler).
 *
 * de__brdf_lut_reference computes the same table on the CPU in double precision ("reference
 * mode"), for tests.
 */
#ifndef DE_RENDER_BRDF_LUTS_H
#define DE_RENDER_BRDF_LUTS_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

typedef struct de_renderer de_renderer;

/** Texels per side (shaders/brdf/energy.slang DE_BRDF_LUT_SIZE). */
#define DE__BRDF_LUT_SIZE 32u
/** Rows of the texture: the energy rows, then the sheen rows (DENG-300). */
#define DE__BRDF_LUT_ROWS (2u * DE__BRDF_LUT_SIZE)
/** Smallest sheen roughness (sheen.slang DE_SHEEN_ROUGHNESS_MIN): the first sheen row. */
#define DE__SHEEN_ROUGHNESS_MIN 0.07
/** Samples per texel of the GPU generation (VNDF for energy, uniform hemisphere for sheen). */
#define DE__BRDF_LUT_SAMPLES 4096u
/** Smallest GGX alpha (energy.slang DE_GGX_ALPHA_MIN). */
#define DE__GGX_ALPHA_MIN 0.002

typedef struct de__brdf_luts de__brdf_luts;

/**
 * Makes the LUT texture and records and submits its generation on `dev`'s graphics queue
 * (later submissions on that queue see it). Without code for this backend (a build without the
 * engine shaders) it succeeds with no texture: de__brdf_luts_bindless is then
 * DE_GPU_BINDLESS_INVALID and the shaders use their analytic fallback. Main thread.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT; DE_ERR_OUT_OF_MEMORY; DE_ERR_INTERNAL (the texture, the
 *         pipeline or the submission failed).
 */
de_result de__brdf_luts_create(de_gpu_device *dev, const de_allocator *alloc, de__brdf_luts **out);
/** Destroys the texture (deferred until the GPU is done) and frees `l`. NULL is ignored. */
void de__brdf_luts_destroy(de__brdf_luts *l);

/** The LUT's sampled bindless index, or DE_GPU_BINDLESS_INVALID (NULL `l` ok). Any thread. */
u32 de__brdf_luts_bindless(const de__brdf_luts *l);
/** The texture (in DE_ACCESS_SHADER_READ once the generation ran), invalid without one. */
de_gpu_texture de__brdf_luts_texture(const de__brdf_luts *l);
/** The fence the generation signals, and the value (tests wait for it). */
de_gpu_fence de__brdf_luts_fence(const de__brdf_luts *l, u64 *value);

/**
 * The LUT on the CPU, double precision, with `samples` Hammersley VNDF samples per texel: the
 * same estimators as the shader. `out` holds DE__BRDF_LUT_SIZE x DE__BRDF_LUT_ROWS RGBA texels
 * (the energy rows, then the sheen rows).
 */
void de__brdf_lut_reference(u32 samples, float *out);

/** The renderer's LUT index for shaders (SurfaceData.brdf_lut), DE_GPU_BINDLESS_INVALID without
 * one. Any thread. */
u32 de__renderer_brdf_lut(const de_renderer *r);

DE_EXTERN_C_END

#endif /* DE_RENDER_BRDF_LUTS_H */
