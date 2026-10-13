/*
 * de_cooked_env.h: the cooked HDR environment, a DTEX cube map followed by an ENVS trailer,
 * version 1 (SPEC §11.6, §12.5 step 5, §12.7; DENG-100). Internal; include as
 * "render/formats/de_cooked_env.h".
 *
 * A `texture` asset with `usage = "environment"` cooks to this. The texture importer
 * (tools/cook/importers/texture/env_prefilter.c, env_sh.c) writes it; the runtime texture loader
 * (DENG-105) uploads the cube and keeps the trailer; image-based lighting (S14) samples the cube
 * for specular and evaluates the SH9 irradiance for diffuse.
 *
 * ---- Blob ------------------------------------------------------------------------------------
 *
 *   [0, T)         a DTEX v1 blob (de_cooked_texture.h): dimension DE_TEX_CUBE, 6 layers (one
 *                  cube), flag CUBE, usage DE_COOKED_TEXTURE_USAGE_ENVIRONMENT, square faces of
 *                  a power-of-two size, the full mip chain to 1x1, and the format the texture
 *                  encode stage chose (DE_FMT_RGBA16_FLOAT, or BC6H by the compression rules). T
 *                  is the DTEX header's total size (offset 48), a multiple of 256.
 *   [T, T + 160)   the ENVS trailer below. The blob ends there.
 *
 * de_cooked_texture_parse on the whole blob fails (its total size is T, not the blob size):
 * parse it with de_cooked_env_parse, which checks both parts and fills the texture view of the
 * first T bytes. A DTEX header whose usage is ENVIRONMENT always has the trailer.
 *
 * ---- Cube faces ------------------------------------------------------------------------------
 *
 * Layer f of the DTEX is face f in the DE_TEX_CUBE order of the RHI (D3D12, Vulkan and Metal
 * agree): 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z. Within a face, texel (x, y) of an N x N mip (x
 * left to right, y top row first, as stored) has sc = 2 (x + 0.5) / N - 1 and
 * tc = 2 (y + 0.5) / N - 1, and looks along (not normalised):
 *
 *   face 0 +X: ( 1,  -tc, -sc)        face 3 -Y: ( sc, -1,  -tc)
 *   face 1 -X: (-1,  -tc,  sc)        face 4 +Z: ( sc, -tc,  1 )
 *   face 2 +Y: ( sc,  1,   tc)        face 5 -Z: (-sc, -tc, -1 )
 *
 * This is the GPU's own cube-map face selection table, applied to directions in engine world
 * space (SPEC §5.5: Z-up, right-handed, +X right, +Y forward, +Z up). So a shader samples the
 * cube with the world-space direction as it is, with no swizzle: face +Z is the sky above,
 * face -Z the ground below, face +Y the horizon straight ahead. de_cooked_env_face_direction
 * computes the table and de_cooked_env_direction_face inverts it.
 *
 * ---- Mips and roughness ----------------------------------------------------------------------
 *
 * Mip 0 holds the environment's radiance resampled to the cube, unfiltered. Mip k (k >= 1) holds
 * the radiance prefiltered with the GGX lobe of perceptual roughness r = k / (mip_count - 1)
 * (alpha = r^2), under the usual split-sum assumption N = V = R: texel direction R stores
 * sum(L(l) (n.l)) / sum(n.l) over GGX importance samples l around R. The last mip has r = 1. A
 * shader picks mip = r x mip_per_roughness (trilinear between mips); mip_per_roughness is in the
 * trailer (mip_count - 1), with `roughness_mapping` DE_COOKED_ENV_ROUGHNESS_LINEAR.
 *
 * ---- SH9 irradiance --------------------------------------------------------------------------
 *
 * Nine RGB coefficients E_k of the irradiance E(n) = integral of L(w) max(0, n.w) dw over the
 * sphere, with L the radiance of mip 0 (unclamped). So E(n) = sum_k E_k Y_k(n) for a unit
 * normal n in world space, and a constant environment L gives E = pi L everywhere. Lambertian
 * diffuse is then albedo / pi x E(n). The basis is the real SH basis of band 0 to 2 in this
 * order and with these constants (x, y, z the components of n, engine axes):
 *
 *   k 0: Y00  = 0.5 sqrt(1/pi)                      k 5: Y2-1 = 0.5 sqrt(15/pi) y z
 *   k 1: Y1-1 = sqrt(3/(4 pi)) y                    k 6: Y20  = 0.25 sqrt(5/pi) (3 z^2 - 1)
 *   k 2: Y10  = sqrt(3/(4 pi)) z                    k 7: Y21  = 0.5 sqrt(15/pi) x z
 *   k 3: Y11  = sqrt(3/(4 pi)) x                    k 8: Y22  = 0.25 sqrt(15/pi) (x^2 - y^2)
 *   k 4: Y2-2 = 0.5 sqrt(15/pi) x y
 *
 * The cosine-lobe convolution is already applied: E_k = A_l L_k with A_0 = pi, A_1 = 2 pi / 3,
 * A_2 = pi / 4, where L_k are the radiance projections. No windowing is applied (a shader may
 * window against ringing). de_cooked_env_sh9_basis and de_cooked_env_irradiance evaluate it.
 *
 * ---- ENVS trailer (DE_COOKED_ENV_TRAILER_SIZE = 160 bytes, little-endian) --------------------
 *
 *     0  u8[4]  magic "ENVS"
 *     4  u32    version (DE_COOKED_ENV_VERSION)
 *     8  u32    trailer size in bytes (160)
 *    12  u32    face size: the DTEX width and height
 *    16  u32    mip count: the DTEX mip count (the full chain, log2(face size) + 1)
 *    20  u32    roughness mapping: DE_COOKED_ENV_ROUGHNESS_LINEAR (0), mip = r x mip_per_roughness
 *    24  f32    mip_per_roughness = mip count - 1
 *    28  u32    sample count: GGX importance samples per prefiltered texel (`env_samples`)
 *    32  f32    rotation_deg: the rotation about +Z applied at cook time (`rotation_deg`); the
 *               cube and the SH are already rotated, this is for information
 *    36  f32    clamp_luminance: the firefly clamp applied to the prefilter input (0 = off)
 *    40  f32    mean_luminance: solid-angle-weighted mean of the Rec.709 luminance
 *               (0.2126 R + 0.7152 G + 0.0722 B) of mip 0. Divide an intensity by it to
 *               normalise environments to the same average brightness
 *    44  f32    max_luminance: the largest texel luminance of mip 0 (before BC or half rounding)
 *    48  f32    SH9 irradiance: 9 x RGB, coefficient k's red, green, blue at 48 + 12 k
 *   156  u32    reserved, 0
 *
 * Floats are IEEE-754 bit patterns. Bump DE_COOKED_ENV_VERSION on any change of the trailer or of
 * what the cube holds, and the texture importer's version with it.
 */
#ifndef DE_RENDER_FORMATS_COOKED_ENV_H
#define DE_RENDER_FORMATS_COOKED_ENV_H

#include "render/formats/de_cooked_le.h"
#include "render/formats/de_cooked_texture.h"

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>

DE_EXTERN_C_BEGIN

#define DE_COOKED_ENV_MAGIC        "ENVS"
#define DE_COOKED_ENV_VERSION      1u
#define DE_COOKED_ENV_TRAILER_SIZE 160u
/** Byte offsets of the trailer fields (from the start of the trailer). */
#define DE_COOKED_ENV_OFF_MAGIC             0u
#define DE_COOKED_ENV_OFF_VERSION           4u
#define DE_COOKED_ENV_OFF_SIZE              8u
#define DE_COOKED_ENV_OFF_FACE_SIZE         12u
#define DE_COOKED_ENV_OFF_MIP_COUNT         16u
#define DE_COOKED_ENV_OFF_ROUGHNESS_MAPPING 20u
#define DE_COOKED_ENV_OFF_MIP_PER_ROUGHNESS 24u
#define DE_COOKED_ENV_OFF_SAMPLE_COUNT      28u
#define DE_COOKED_ENV_OFF_ROTATION_DEG      32u
#define DE_COOKED_ENV_OFF_CLAMP_LUMINANCE   36u
#define DE_COOKED_ENV_OFF_MEAN_LUMINANCE    40u
#define DE_COOKED_ENV_OFF_MAX_LUMINANCE     44u
#define DE_COOKED_ENV_OFF_SH9               48u
#define DE_COOKED_ENV_OFF_RESERVED          156u

/** The number of SH coefficients (bands 0 to 2). */
#define DE_COOKED_ENV_SH_COUNT 9u

/** Roughness → mip mappings (trailer offset 20). */
enum {
    /** mip = r x mip_per_roughness, r the perceptual roughness, GGX alpha = r^2. */
    DE_COOKED_ENV_ROUGHNESS_LINEAR = 0
};

/** The cube faces, in DTEX layer order (DE_TEX_CUBE). */
enum {
    DE_COOKED_ENV_FACE_POS_X = 0,
    DE_COOKED_ENV_FACE_NEG_X = 1,
    DE_COOKED_ENV_FACE_POS_Y = 2,
    DE_COOKED_ENV_FACE_NEG_Y = 3,
    DE_COOKED_ENV_FACE_POS_Z = 4,
    DE_COOKED_ENV_FACE_NEG_Z = 5
};

/**
 * The direction (world space, not normalised) of face `face` (0 to 5) at face coordinates
 * sc, tc in [-1, 1] (sc left to right, tc top to bottom). See "Cube faces" above.
 */
static inline void de_cooked_env_face_direction(u32 face, f32 sc, f32 tc, f32 out[3]) {
    switch (face) {
    case DE_COOKED_ENV_FACE_POS_X:
        out[0] = 1.0f, out[1] = -tc, out[2] = -sc;
        break;
    case DE_COOKED_ENV_FACE_NEG_X:
        out[0] = -1.0f, out[1] = -tc, out[2] = sc;
        break;
    case DE_COOKED_ENV_FACE_POS_Y:
        out[0] = sc, out[1] = 1.0f, out[2] = tc;
        break;
    case DE_COOKED_ENV_FACE_NEG_Y:
        out[0] = sc, out[1] = -1.0f, out[2] = -tc;
        break;
    case DE_COOKED_ENV_FACE_POS_Z:
        out[0] = sc, out[1] = -tc, out[2] = 1.0f;
        break;
    default:
        out[0] = -sc, out[1] = -tc, out[2] = -1.0f;
        break;
    }
}

/**
 * The face a direction (any length but zero) falls on, and its face coordinates sc, tc in
 * [-1, 1]: the inverse of de_cooked_env_face_direction. The largest component picks the face;
 * ties go to X before Y before Z.
 */
static inline u32 de_cooked_env_direction_face(const f32 d[3], f32 *sc, f32 *tc) {
    const f32 ax = d[0] < 0.0f ? -d[0] : d[0];
    const f32 ay = d[1] < 0.0f ? -d[1] : d[1];
    const f32 az = d[2] < 0.0f ? -d[2] : d[2];
    if (ax >= ay && ax >= az) {
        const f32 inv = 1.0f / ax;
        *tc = -d[1] * inv;
        if (d[0] >= 0.0f) {
            *sc = -d[2] * inv;
            return DE_COOKED_ENV_FACE_POS_X;
        }
        *sc = d[2] * inv;
        return DE_COOKED_ENV_FACE_NEG_X;
    }
    if (ay >= az) {
        const f32 inv = 1.0f / ay;
        *sc = d[0] * inv;
        if (d[1] >= 0.0f) {
            *tc = d[2] * inv;
            return DE_COOKED_ENV_FACE_POS_Y;
        }
        *tc = -d[2] * inv;
        return DE_COOKED_ENV_FACE_NEG_Y;
    }
    const f32 inv = 1.0f / az;
    *tc = -d[1] * inv;
    if (d[2] >= 0.0f) {
        *sc = d[0] * inv;
        return DE_COOKED_ENV_FACE_POS_Z;
    }
    *sc = -d[0] * inv;
    return DE_COOKED_ENV_FACE_NEG_Z;
}

/** The SH9 basis (see "SH9 irradiance" above) at the unit direction (x, y, z). */
static inline void de_cooked_env_sh9_basis(f32 x, f32 y, f32 z, f32 out[9]) {
    out[0] = 0.282094791773878f;
    out[1] = 0.488602511902920f * y;
    out[2] = 0.488602511902920f * z;
    out[3] = 0.488602511902920f * x;
    out[4] = 1.092548430592079f * x * y;
    out[5] = 1.092548430592079f * y * z;
    out[6] = 0.315391565252520f * (3.0f * z * z - 1.0f);
    out[7] = 1.092548430592079f * x * z;
    out[8] = 0.546274215296040f * (x * x - y * y);
}

/** The trailer of a checked environment. */
typedef struct de_cooked_env_view {
    de_cooked_texture_view texture; /**< the DTEX part (the first dtex_size bytes) */
    u64 dtex_size;                  /**< T: where the trailer starts */
    u32 face_size;
    u32 mip_count;
    u32 roughness_mapping; /**< DE_COOKED_ENV_ROUGHNESS_LINEAR */
    f32 mip_per_roughness;
    u32 sample_count;
    f32 rotation_deg;
    f32 clamp_luminance; /**< 0 = off */
    f32 mean_luminance;
    f32 max_luminance;
    f32 sh9[DE_COOKED_ENV_SH_COUNT][3]; /**< irradiance coefficients, RGB */
} de_cooked_env_view;

/** The mip (fractional) for perceptual roughness `r` (clamped to [0, 1]). */
static inline f32 de_cooked_env_roughness_to_mip(const de_cooked_env_view *v, f32 r) {
    r = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
    return r * v->mip_per_roughness;
}

/** The irradiance E(n) (RGB) at unit normal (x, y, z): sum_k E_k Y_k(n). */
static inline void de_cooked_env_irradiance(const de_cooked_env_view *v, f32 x, f32 y, f32 z,
                                            f32 rgb[3]) {
    f32 b[9];
    de_cooked_env_sh9_basis(x, y, z, b);
    for (u32 c = 0; c < 3u; ++c) {
        f32 e = 0.0f;
        for (u32 k = 0; k < DE_COOKED_ENV_SH_COUNT; ++k)
            e += v->sh9[k][c] * b[k];
        rgb[c] = e;
    }
}

/* The checks of de_cooked_env_parse: NULL when the blob is valid, else the reason. */
static inline const char *de__cooked_env_check(const u8 *p, u64 size, de_cooked_env_view *out) {
    if (p == NULL || size < DE_COOKED_TEXTURE_HEADER_SIZE)
        return "shorter than the DTEX header";
    const u64 t = de_cooked_get_u64(p + 48);
    if (t > size || size - t != DE_COOKED_ENV_TRAILER_SIZE)
        return "the blob is not a DTEX blob followed by a 160-byte ENVS trailer";
    const char *why = NULL;
    if (!de_cooked_texture_parse(p, t, &out->texture, &why))
        return why;
    const de_cooked_texture_view *tv = &out->texture;
    if (tv->dim != (u32)DE_TEX_CUBE || tv->depth_or_layers != 6u ||
        tv->usage != (u32)DE_COOKED_TEXTURE_USAGE_ENVIRONMENT)
        return "the DTEX part is not one environment cube (6 faces, usage environment)";
    if ((tv->width & (tv->width - 1u)) != 0u ||
        tv->mip_count != de_cooked_texture_full_mip_count(tv->width, tv->height))
        return "the cube is not a power of two with the full mip chain";
    const u8 *e = p + t;
    if (memcmp(e, DE_COOKED_ENV_MAGIC, 4) != 0)
        return "not an ENVS trailer (magic)";
    if (de_cooked_get_u32(e + DE_COOKED_ENV_OFF_VERSION) != DE_COOKED_ENV_VERSION)
        return "unsupported ENVS version";
    if (de_cooked_get_u32(e + DE_COOKED_ENV_OFF_SIZE) != DE_COOKED_ENV_TRAILER_SIZE ||
        de_cooked_get_u32(e + DE_COOKED_ENV_OFF_RESERVED) != 0u)
        return "bad ENVS size or reserved field";
    out->dtex_size = t;
    out->face_size = de_cooked_get_u32(e + DE_COOKED_ENV_OFF_FACE_SIZE);
    out->mip_count = de_cooked_get_u32(e + DE_COOKED_ENV_OFF_MIP_COUNT);
    out->roughness_mapping = de_cooked_get_u32(e + DE_COOKED_ENV_OFF_ROUGHNESS_MAPPING);
    out->mip_per_roughness = de_cooked_get_f32(e + DE_COOKED_ENV_OFF_MIP_PER_ROUGHNESS);
    out->sample_count = de_cooked_get_u32(e + DE_COOKED_ENV_OFF_SAMPLE_COUNT);
    out->rotation_deg = de_cooked_get_f32(e + DE_COOKED_ENV_OFF_ROTATION_DEG);
    out->clamp_luminance = de_cooked_get_f32(e + DE_COOKED_ENV_OFF_CLAMP_LUMINANCE);
    out->mean_luminance = de_cooked_get_f32(e + DE_COOKED_ENV_OFF_MEAN_LUMINANCE);
    out->max_luminance = de_cooked_get_f32(e + DE_COOKED_ENV_OFF_MAX_LUMINANCE);
    for (u32 k = 0; k < DE_COOKED_ENV_SH_COUNT; ++k)
        for (u32 c = 0; c < 3u; ++c)
            out->sh9[k][c] = de_cooked_get_f32(e + DE_COOKED_ENV_OFF_SH9 + 12u * k + 4u * c);
    if (out->face_size != tv->width || out->mip_count != tv->mip_count)
        return "ENVS face size or mip count does not match the cube";
    if (out->roughness_mapping != (u32)DE_COOKED_ENV_ROUGHNESS_LINEAR ||
        out->mip_per_roughness != (f32)(out->mip_count - 1u))
        return "unknown roughness mapping";
    if (out->sample_count == 0u)
        return "sample count is 0";
    return NULL;
}

/**
 * Checks a cooked environment (the DTEX part with de_cooked_texture_parse, then the cube rules
 * and the trailer: magic, version, size, reserved field, face size and mip count against the
 * cube, the roughness mapping) and fills `*out`. Allocation-free. False on any error, with a
 * short reason in `*why` (nullable).
 */
static inline bool de_cooked_env_parse(const void *data, u64 size, de_cooked_env_view *out,
                                       const char **why) {
    memset(out, 0, sizeof *out);
    const char *w = de__cooked_env_check((const u8 *)data, size, out);
    if (w == NULL)
        return true;
    memset(out, 0, sizeof *out);
    if (why != NULL)
        *why = w;
    return false;
}

DE_EXTERN_C_END

#endif /* DE_RENDER_FORMATS_COOKED_ENV_H */
