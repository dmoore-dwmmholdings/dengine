/*
 * gbuffer.h: the thin G-buffer the material resolve writes (SPEC §12.5 step 4; DENG-238). Internal
 * to de_render; include as "render/passes/vis/gbuffer.h". shaders/visbuffer/gbuffer.slang has the
 * same layout and the same encode / decode arithmetic; the test gbuffer.encode_decode_roundtrip
 * checks these C functions, and the GPU tests decode what the shaders wrote with them.
 *
 * A contract for lighting (S14), ray tracing (S16) and the denoisers (S17): change it only by
 * appending (a new target, or meaning for a spare bit).
 *
 *   GB0      DE_FMT_RGBA8_UNORM    rgb: base colour, sRGB-encoded (de__gb_srgb_encode); a: AO
 *            (written by a compute shader, which cannot store into an sRGB texture: readers decode
 *             with de__gb_srgb_decode / gbuffer.slang's gb_decode)
 *   GB1      DE_FMT_RGB10A2_UNORM  the normal (world space), octahedral, 11 + 11 bits: the high 10
 *                                  bits of x in r and of y in g, their low bits in a (bit 0 x,
 *                                  bit 1 y; a = (lx + 2 ly) / 3); b: perceptual roughness (10
 *                                  bits). Max normal error 0.082 degrees ("precise" encoding: the
 *                                  best of the 4 roundings); roughness steps of 1/1023
 *   GB2      DE_FMT_RGBA8_UNORM    r: metallic; g: material model id / 255 (DeGpuMaterial.model);
 *                                  b: flags / 255 (DE__GB_FLAG_*); a: specular
 * (de_standard_material specular, the F0 factor) motion   DE_FMT_RG16_FLOAT     NDC xy of the pixel
 * now minus where the surface point was in the previous frame (previous instance transform and
 * view-projection; object and camera motion). 0 for pixels with no surface. lighting
 * DE_FMT_RGBA16_FLOAT   the HDR lighting target: resolve writes emissive radiance (rgb; a = 1 where
 * a surface is), cleared to 0
 *
 * Pixels no triangle covers keep the clear values: GB0..GB2 0, motion 0, lighting 0 (a = 0).
 */
#ifndef DE_RENDER_PASSES_VIS_GBUFFER_H
#define DE_RENDER_PASSES_VIS_GBUFFER_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>

#include <math.h>

DE_EXTERN_C_BEGIN

/** The targets, in the order of de__vis_context.gbuffer. */
enum { DE__GB0 = 0, DE__GB1, DE__GB2, DE__GB_MOTION, DE__GB_LIGHTING, DE__GB_COUNT };
/** The format of target `t` (DE__GB0 ..). */
static inline de_gpu_format de__gb_format(u32 t) {
    switch (t) {
    case DE__GB1:
        return DE_FMT_RGB10A2_UNORM;
    case DE__GB_MOTION:
        return DE_FMT_RG16_FLOAT;
    case DE__GB_LIGHTING:
        return DE_FMT_RGBA16_FLOAT;
    default:
        return DE_FMT_RGBA8_UNORM; /* GB0, GB2 */
    }
}

/** GB2.b flags (8 bits). */
enum {
    DE__GB_FLAG_DOUBLE_SIDED = 1u,
    DE__GB_FLAG_TRANSMISSION = 2u, /**< transmission or diffuse transmission > 0 */
    DE__GB_FLAG_SUBSURFACE = 4u,
    DE__GB_FLAG_CLEARCOAT = 8u,
    DE__GB_FLAG_UNLIT = 16u
};

/** Bits of each octahedral normal component (10 in r / g, 1 in a). */
#define DE__GB_NORMAL_BITS 11u

/** Linear to sRGB (the IEC 61966-2-1 curve). */
static inline f32 de__gb_srgb_encode(f32 c) {
    c = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}
static inline f32 de__gb_srgb_decode(f32 c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static inline f32 de__gb_sign(f32 v) {
    return v >= 0.0f ? 1.0f : -1.0f;
}

/** The octahedral pair of unit vector `n`, in [-1, 1]^2. */
static inline void de__gb_oct_encode(const f32 n[3], f32 out[2]) {
    const f32 s = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]);
    f32 x = n[0] / s, y = n[1] / s;
    if (n[2] < 0.0f) {
        const f32 tx = (1.0f - fabsf(y)) * de__gb_sign(x), ty = (1.0f - fabsf(x)) * de__gb_sign(y);
        x = tx;
        y = ty;
    }
    out[0] = x;
    out[1] = y;
}
static inline void de__gb_oct_decode(f32 x, f32 y, f32 out[3]) {
    const f32 z = 1.0f - fabsf(x) - fabsf(y);
    if (z < 0.0f) {
        const f32 tx = (1.0f - fabsf(y)) * de__gb_sign(x), ty = (1.0f - fabsf(x)) * de__gb_sign(y);
        x = tx;
        y = ty;
    }
    const f32 l = sqrtf(x * x + y * y + z * z);
    out[0] = x / l;
    out[1] = y / l;
    out[2] = z / l;
}

/** The 11-bit codes of unit normal `n`: the best of the 4 roundings (Cigolle et al. 2014). */
static inline void de__gb_normal_encode(const f32 n[3], u32 *qx, u32 *qy) {
    const f32 m = (f32)((1u << DE__GB_NORMAL_BITS) - 1u);
    f32 e[2];
    de__gb_oct_encode(n, e);
    const f32 fx = (e[0] * 0.5f + 0.5f) * m, fy = (e[1] * 0.5f + 0.5f) * m;
    const u32 bx = (u32)floorf(fx), by = (u32)floorf(fy);
    f32 best = -2.0f;
    *qx = bx;
    *qy = by;
    for (u32 a = 0; a < 2; ++a)
        for (u32 b = 0; b < 2; ++b) {
            const u32 cx = bx + a, cy = by + b;
            if ((f32)cx > m || (f32)cy > m)
                continue;
            f32 d[3];
            de__gb_oct_decode((f32)cx / m * 2.0f - 1.0f, (f32)cy / m * 2.0f - 1.0f, d);
            const f32 dot = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
            if (dot > best) {
                best = dot;
                *qx = cx;
                *qy = cy;
            }
        }
}
static inline void de__gb_normal_decode(u32 qx, u32 qy, f32 out[3]) {
    const f32 m = (f32)((1u << DE__GB_NORMAL_BITS) - 1u);
    de__gb_oct_decode((f32)qx / m * 2.0f - 1.0f, (f32)qy / m * 2.0f - 1.0f, out);
}

/** GB1 as stored (a packed RGB10A2 word: r bits 0-9, g 10-19, b 20-29, a 30-31). */
static inline u32 de__gb1_pack(const f32 n[3], f32 roughness) {
    u32 qx, qy;
    de__gb_normal_encode(n, &qx, &qy);
    roughness = roughness < 0.0f ? 0.0f : roughness > 1.0f ? 1.0f : roughness;
    const u32 r = (u32)floorf(roughness * 1023.0f + 0.5f);
    return (qx >> 1) | ((qy >> 1) << 10) | (r << 20) | (((qx & 1u) | ((qy & 1u) << 1)) << 30);
}
static inline void de__gb1_unpack(u32 w, f32 n[3], f32 *roughness) {
    const u32 a = w >> 30;
    const u32 qx = ((w & 1023u) << 1) | (a & 1u), qy = (((w >> 10) & 1023u) << 1) | (a >> 1);
    de__gb_normal_decode(qx, qy, n);
    *roughness = (f32)((w >> 20) & 1023u) / 1023.0f;
}

DE_EXTERN_C_END

#endif /* DE_RENDER_PASSES_VIS_GBUFFER_H */
