/*
 * env_internal.h: HDR environments of the texture importer (DENG-100, SPEC §11.6): a source
 * image (equirectangular, a cube cross, or a cube) to a cube map with GGX-prefiltered mips and
 * SH9 irradiance, cooked as a DTEX cube with an ENVS trailer (render/formats/de_cooked_env.h).
 * Private to tools/cook/importers/texture/ and its tests.
 *
 *   de_tex_image ─ de_env_build (env_prefilter.c) ─┬─ mip 0: the source resampled to the cube,
 *                                                  │  rotated about +Z (supersampled)
 *                                                  └─ mips 1..n: GGX prefilter of mip 0
 *   mip 0 ─ de_env_sh9_project (env_sh.c) ─ SH9 irradiance, mean and max luminance
 *   chain + SH ─ de_env_cook (env_prefilter.c) ─ encode stage (texture_encode.c) ─ DTEX + ENVS
 *
 * Determinism (SPEC §11.5): the output is the same bytes for every worker count and on every
 * host. The work runs through de_task_parallel_for in fixed chunks (one (face, row) per chunk);
 * each texel's sums run sequentially in a fixed sample order; the SH sums go per (face, row)
 * into their own slot and are reduced in slot order on the calling thread. Transcendentals are
 * de_d* (de_math.h), and the files compile without FMA contraction (DE_DETERMINISTIC_FLAGS).
 * Without a task system the same chunks run in order on the calling thread.
 */
#ifndef DE_TEXTURE_ENV_INTERNAL_H
#define DE_TEXTURE_ENV_INTERNAL_H

#include "texture_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_task.h>

#include "render/formats/de_cooked_env.h"

DE_EXTERN_C_BEGIN

/** Setting defaults and limits. */
#define DE_ENV_DEFAULT_SIZE    256u
#define DE_ENV_MIN_SIZE        4u
#define DE_ENV_MAX_SIZE        4096u
#define DE_ENV_DEFAULT_SAMPLES 1024u
#define DE_ENV_MAX_SAMPLES     65536u

/** The environment settings (`env_size`, `env_samples`, `rotation_deg`, `clamp_luminance`). */
typedef struct de_env_settings {
    u32 size;            /**< cube face size, a power of two in [4, 4096] */
    u32 samples;         /**< GGX samples per prefiltered texel, 1 to 65536 */
    f32 rotation_deg;    /**< rotation about +Z (counter-clockwise seen from above) */
    f32 clamp_luminance; /**< > 0: prefilter input texels brighter than this are scaled down to
                              it (firefly clamp); 0 = off */
} de_env_settings;

/** How the source image holds the environment (from its shape). */
typedef enum de_env_layout {
    DE_ENV_LAYOUT_EQUIRECT = 0, /**< 2:1 latitude-longitude */
    DE_ENV_LAYOUT_CROSS_H = 1,  /**< 4:3 horizontal cross */
    DE_ENV_LAYOUT_CROSS_V = 2,  /**< 3:4 vertical cross */
    DE_ENV_LAYOUT_CUBE = 3      /**< a cube texture (KTX2): 6 layers */
} de_env_layout;

/**
 * The layout of `img`: a cube (6 layers) is a cube; a single image of aspect 2:1 is
 * equirectangular, 4:3 a horizontal cross, 3:4 a vertical cross. DE_ERR_UNSUPPORTED (with a
 * message) for any other shape, arrays, and a cube array.
 */
de_result de_env_source_layout(const de_tex_image *img, de_env_layout *out);

/**
 * Builds the environment chain from `src` (linear radiance, f32 RGBA): `out` gets 6 layers and
 * the full chain of s->size (mip_count = log2(size) + 1), alpha 1.
 *
 * - Mip 0: each texel averages k x k directions spread over the texel, k = ceil(source texels
 *   per output texel) in [1, 8], each sampled bilinearly from the source at R^-1 d, where R is the
 *   rotation by rotation_deg about +Z. Negative source values become 0.
 *   - Equirectangular: the centre column looks along +Y (forward), x grows toward +X (to the
 *     right, clockwise seen from above), the top row is +Z: for azimuth phi = 2 pi (u - 0.5) and
 *     elevation theta = pi (0.5 - v), d = (sin phi cos theta, cos phi cos theta, sin theta).
 *   - Cross and cube sources are Y-up (the convention of glTF and of most cube-map tools: their
 *     +Y face is the sky), faces in the GPU orientation; the source direction (x, y, z) is the
 *     engine direction (x, -z, y), the axis conversion of the model importer (DENG-2). Horizontal
 *     cross: +Y above column 1; -X, +Z, +X, -Z in row 1; -Y below column 1. Vertical cross: +Y
 *     at (1, 0); -X, +Z, +X in row 1; -Y at (1, 2); -Z at (1, 3), turned 180 degrees.
 * - Mip k >= 1: GGX prefilter of perceptual roughness r = k / (mip_count - 1), alpha = r^2,
 *   N = V = R: s->samples Hammersley points, GGX half vectors, reflected; samples with n.l <= 0
 *   are skipped; each reads the box-filtered radiance chain of mip 0 trilinearly at the mip that
 *   matches its PDF (lod = 0.5 log2(omega_s / omega_p) + 1, Karis), weighted by n.l. With
 *   clamp_luminance > 0 the radiance chain is clamped first (mip 0 itself is not).
 *
 * Every allocation is made on the calling thread from `a`. `ts` may be NULL.
 * DE_ERR_INVALID_ARGUMENT (bad settings or source), DE_ERR_UNSUPPORTED (layout),
 * DE_ERR_OUT_OF_MEMORY.
 */
de_result de_env_build(const de_allocator *a, de_task_system *ts, const de_tex_image *src,
                       const de_env_settings *s, de_tex_chain *out);

/** The SH9 result of de_env_sh9_project. */
typedef struct de_env_sh9 {
    f32 irradiance[DE_COOKED_ENV_SH_COUNT][3]; /**< E_k (de_cooked_env.h) */
    f32 mean_luminance;
    f32 max_luminance;
} de_env_sh9;

/**
 * Projects a cube (6 faces of size x size f32 RGBA, face after face: mip 0 of de_env_build) onto
 * SH9 and convolves it with the cosine lobe (de_cooked_env.h, "SH9 irradiance"). Each texel is
 * weighted by its solid angle, 4 / size^2 / (1 + sc^2 + tc^2)^1.5 in f64, and the weights are
 * scaled to sum to 4 pi exactly. One partial sum per (face, row), reduced in order.
 * DE_ERR_OUT_OF_MEMORY.
 */
de_result de_env_sh9_project(const de_allocator *a, de_task_system *ts, const f32 *faces, u32 size,
                             de_env_sh9 *out);

/** Rec.709 luminance. */
static inline f32 de_env_luminance(const f32 *rgb) {
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

/** Runs `fn` over [0, count) in chunks of `grain`: through de_task_parallel_for, or, without a
 * task system, chunk after chunk on the calling thread. */
void de_env_for(de_task_system *ts, u32 count, u32 grain, de_range_fn fn, void *user);

/**
 * The environment branch of the texture importer (`usage = "environment"`): reads the env_*
 * settings, builds the chain, projects SH9, encodes the chain with the format the encode stage
 * picks for `s` and `img` (de_tex_choose_format), and writes the DTEX cube plus the ENVS trailer.
 * Runs on the cooker's task system (de_cooker_import_tasks) when it has one.
 */
de_result de_env_cook(de_asset_import_ctx *ctx, const de_allocator *a, const de_tex_settings *s,
                      const de_tex_image *img);

/** Writes the 160-byte ENVS trailer for a cube of `face_size` with `mip_count` mips. */
void de_env_write_trailer(u8 *dst, u32 face_size, u32 mip_count, const de_env_settings *s,
                          const de_env_sh9 *sh);

DE_EXTERN_C_END

#endif /* DE_TEXTURE_ENV_INTERNAL_H */
