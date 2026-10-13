/*
 * texture_mips.c: linear-space mip generation of the texture importer (DENG-98, SPEC §11.6).
 *
 * The filter is a separable Kaiser-windowed sinc (radius 3 in destination texels, alpha 4; the
 * filter NVIDIA Texture Tools uses for mips). For a reduction by scale s = source / destination,
 * destination texel i takes source texels j with weights kaiser(((j + 0.5) - (i + 0.5) s) / s),
 * normalized to sum 1; edges read by wrap or clamp. The weights of one axis are computed once per
 * (source size, destination size) pair, in f64 with de_dsin64 (deterministic on every host) and
 * stored as f32. Filtering runs on the calling thread in a fixed order, so the output does not
 * depend on threads (SPEC §11.5).
 *
 * The importer gives this file linear values: sRGB colour is decoded before (de_tex_srgb_to_linear)
 * and encoded after (texture_encode.c), so a 2x2 checker of sRGB 0 and 255 filters to linear 0.5
 * (sRGB 188), not 128. Normal maps hold vectors in [-1, 1] and are renormalized in every mip.
 * Alpha-tested textures (alpha_coverage) get an alpha scale per mip that keeps the share of
 * texels at or above the cutoff equal to mip 0's (Castaño, "Computing alpha mipmaps").
 *
 * Built with the deterministic float flags (no FMA contraction; tools/cook/CMakeLists.txt).
 */
#include "texture_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>

#include <string.h>

/* ---- sRGB ------------------------------------------------------------------------------------ */

f32 de_tex_srgb_to_linear(f32 c) {
    if (!(c > 0.0f))
        return 0.0f;
    if (c >= 1.0f)
        return 1.0f;
    if (c <= 0.04045f)
        return c / 12.92f;
    return de_dpow((c + 0.055f) / 1.055f, 2.4f);
}

f32 de_tex_linear_to_srgb(f32 l) {
    if (!(l > 0.0f))
        return 0.0f;
    if (l >= 1.0f)
        return 1.0f;
    if (l <= 0.0031308f)
        return l * 12.92f;
    return 1.055f * de_dpow(l, 1.0f / 2.4f) - 0.055f;
}

/* ---- Filter weights -------------------------------------------------------------------------- */

enum { KAISER_RADIUS = 3 };
#define KAISER_ALPHA 4.0
#define TEX_PI       3.14159265358979323846

/* Modified Bessel function of the first kind, order 0 (power series). */
static f64 bessel_i0(f64 x) {
    f64 sum = 1.0, term = 1.0, q = x * 0.5;
    for (u32 k = 1; k < 64; ++k) {
        f64 t = q / (f64)k;
        term *= t * t;
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

static f64 kaiser(f64 x) {
    if (x < 0.0)
        x = -x;
    if (x >= (f64)KAISER_RADIUS)
        return 0.0;
    f64 sinc = x < 1e-9 ? 1.0 : de_dsin64(TEX_PI * x) / (TEX_PI * x);
    f64 t = x / (f64)KAISER_RADIUS;
    return sinc * bessel_i0(KAISER_ALPHA * de_dsqrt64(1.0 - t * t)) / bessel_i0(KAISER_ALPHA);
}

/* The taps of one axis: destination texel i reads `count[i]` source texels from `first[i]`
 * (wrapped or clamped), with `weight[i * stride ..]`. */
typedef struct axis_filter {
    u32 dst, stride;
    i32 *first;
    u32 *count;
    f32 *weight;
    u64 bytes;
    void *block;
} axis_filter;

static void axis_free(const de_allocator *a, axis_filter *f) {
    if (f->block != NULL)
        DE_FREE(a, f->block, (size_t)f->bytes);
    memset(f, 0, sizeof *f);
}

static de_result axis_build(const de_allocator *a, u32 src, u32 dst, axis_filter *f) {
    memset(f, 0, sizeof *f);
    f64 s = (f64)src / (f64)dst;
    if (s < 1.0)
        s = 1.0;
    /* Taps per destination texel: the support 2 R s, plus two at each end. */
    u32 stride = (u32)(2.0 * KAISER_RADIUS * s) + 5u;
    f->dst = dst;
    f->stride = stride;
    u64 nw = (u64)dst * stride;
    /* The f32 weights, first taps and counts, then f64 scratch for one texel's raw weights. */
    f->bytes =
        nw * sizeof(f32) + (u64)dst * (sizeof(i32) + sizeof(u32)) + 8u + (u64)stride * sizeof(f64);
    f->block = DE_ALLOC(a, (size_t)f->bytes, DE_ALIGNOF(f64), DE_MEM_ASSET);
    if (f->block == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "texture: out of memory for filter weights");
    memset(f->block, 0, (size_t)f->bytes);
    f->weight = (f32 *)f->block;
    f->first = (i32 *)(void *)(f->weight + nw);
    f->count = (u32 *)(void *)(f->first + dst);
    u64 raw_off =
        de_cooked_texture_align((u64)((u8 *)(void *)(f->count + dst) - (u8 *)f->block), 8u);
    f64 *w = (f64 *)(void *)((u8 *)f->block + raw_off);
    for (u32 i = 0; i < dst; ++i) {
        f64 c = ((f64)i + 0.5) * s;
        f64 lo = c - (f64)KAISER_RADIUS * s, hi = c + (f64)KAISER_RADIUS * s;
        f64 sum = 0.0;
        u32 n = 0;
        i32 first = 0;
        for (i32 j = (i32)lo - 2; (f64)j - 0.5 <= hi && n < stride; ++j) {
            f64 k = kaiser(((f64)j + 0.5 - c) / s);
            if (n == 0 && k == 0.0)
                continue; /* leading zero taps */
            if (n == 0)
                first = j;
            w[n++] = k;
            sum += k;
        }
        while (n > 1 && w[n - 1] == 0.0)
            --n; /* trailing zero taps */
        if (n == 0 || !(sum > 0.0)) {
            /* Cannot happen for this kernel; keep the nearest texel. */
            first = (i32)c;
            w[0] = 1.0;
            sum = 1.0;
            n = 1;
        }
        f->first[i] = first;
        f->count[i] = n;
        for (u32 k = 0; k < n; ++k)
            f->weight[(u64)i * stride + k] = (f32)(w[k] / sum);
    }
    return DE_OK;
}

static u32 address(i32 j, u32 n, de_tex_address mode) {
    if (mode == DE_TEX_ADDRESS_CLAMP)
        return j < 0 ? 0u : ((u32)j >= n ? n - 1u : (u32)j);
    i32 m = j % (i32)n;
    return (u32)(m < 0 ? m + (i32)n : m);
}

/* ---- Resampling ------------------------------------------------------------------------------ */

de_result de_tex_resample(const de_allocator *a, const f32 *src, u32 sw, u32 sh, f32 *dst, u32 dw,
                          u32 dh, u32 layers, de_tex_address mode) {
    axis_filter fx, fy;
    DE_TRY(axis_build(a, sw, dw, &fx));
    de_result r = axis_build(a, sh, dh, &fy);
    if (r != DE_OK) {
        axis_free(a, &fx);
        return r;
    }
    /* Horizontal pass into tmp (dw x sh), then vertical into dst (dw x dh), per layer. */
    u64 tmp_bytes = (u64)dw * sh * 4u * sizeof(f32);
    f32 *tmp = (f32 *)DE_ALLOC(a, (size_t)tmp_bytes, DE_ALIGNOF(f32), DE_MEM_ASSET);
    if (tmp == NULL) {
        axis_free(a, &fx);
        axis_free(a, &fy);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "texture: out of memory resampling %ux%u", sw, sh);
    }
    for (u32 l = 0; l < layers; ++l) {
        const f32 *s = src + (u64)l * sw * sh * 4u;
        f32 *d = dst + (u64)l * dw * dh * 4u;
        for (u32 y = 0; y < sh; ++y) {
            const f32 *row = s + (u64)y * sw * 4u;
            for (u32 x = 0; x < dw; ++x) {
                f32 acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                const f32 *w = fx.weight + (u64)x * fx.stride;
                for (u32 k = 0; k < fx.count[x]; ++k) {
                    const f32 *t = row + (u64)address(fx.first[x] + (i32)k, sw, mode) * 4u;
                    for (u32 c = 0; c < 4; ++c)
                        acc[c] += w[k] * t[c];
                }
                memcpy(tmp + ((u64)y * dw + x) * 4u, acc, sizeof acc);
            }
        }
        for (u32 y = 0; y < dh; ++y) {
            const f32 *w = fy.weight + (u64)y * fy.stride;
            for (u32 x = 0; x < dw; ++x) {
                f32 acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (u32 k = 0; k < fy.count[y]; ++k) {
                    const f32 *t =
                        tmp + ((u64)address(fy.first[y] + (i32)k, sh, mode) * dw + x) * 4u;
                    for (u32 c = 0; c < 4; ++c)
                        acc[c] += w[k] * t[c];
                }
                memcpy(d + ((u64)y * dw + x) * 4u, acc, sizeof acc);
            }
        }
    }
    DE_FREE(a, tmp, (size_t)tmp_bytes);
    axis_free(a, &fx);
    axis_free(a, &fy);
    return DE_OK;
}

/* ---- Normal maps and alpha coverage ---------------------------------------------------------- */

static void renormalize(f32 *rgba, u64 texels) {
    for (u64 i = 0; i < texels; ++i) {
        f32 *t = rgba + i * 4u;
        f64 x = t[0], y = t[1], z = t[2];
        f64 len = de_dsqrt64(x * x + y * y + z * z);
        if (!(len > 1e-6)) {
            t[0] = 0.0f;
            t[1] = 0.0f;
            t[2] = 1.0f;
        } else {
            t[0] = (f32)(x / len);
            t[1] = (f32)(y / len);
            t[2] = (f32)(z / len);
        }
    }
}

f32 de_tex_alpha_coverage(const f32 *rgba, u32 width, u32 height, f32 cutoff) {
    u64 n = (u64)width * height, hit = 0;
    for (u64 i = 0; i < n; ++i)
        if (rgba[i * 4u + 3u] >= cutoff)
            ++hit;
    return n != 0 ? (f32)((f64)hit / (f64)n) : 0.0f;
}

/* Alpha as the GPU will see it: clamped to 1, then rounded to `levels` steps when the output
 * format stores it with fixed precision (255 for 8-bit UNORM; 0 = kept as is). */
static f32 stored_alpha(f32 v, u32 levels) {
    v = v > 1.0f ? 1.0f : v;
    if (levels == 0 || !(v > 0.0f))
        return v;
    return (f32)(u32)(v * (f32)levels + 0.5f) / (f32)levels;
}

/* Coverage of one image when its alpha is scaled by `s`. */
static f64 scaled_coverage(const f32 *rgba, u64 n, f32 cutoff, f32 s, u32 levels) {
    u64 hit = 0;
    for (u64 i = 0; i < n; ++i)
        if (stored_alpha(rgba[i * 4u + 3u] * s, levels) >= cutoff)
            ++hit;
    return (f64)hit / (f64)n;
}

/* Scales the alpha of one image so its coverage at `cutoff` is as close to `target` as possible:
 * bisection for the smallest scale that reaches the target (coverage grows with the scale), then
 * the closer of it and the largest scale below. */
static void keep_coverage(f32 *rgba, u32 w, u32 h, f32 cutoff, f64 target, u32 levels) {
    u64 n = (u64)w * h;
    if (n == 0 || !(target > 0.0) || !(target < 1.0))
        return;
    f32 lo = 0.0f, hi = 1.0f;
    while (scaled_coverage(rgba, n, cutoff, hi, levels) < target && hi < 65536.0f)
        hi *= 2.0f;
    for (u32 it = 0; it < 32; ++it) {
        f32 mid = 0.5f * (lo + hi);
        if (mid <= lo || mid >= hi)
            break;
        if (scaled_coverage(rgba, n, cutoff, mid, levels) >= target)
            hi = mid;
        else
            lo = mid;
    }
    f64 ch = scaled_coverage(rgba, n, cutoff, hi, levels);
    f64 cl = scaled_coverage(rgba, n, cutoff, lo, levels);
    f32 s = (ch - target) <= (target - cl) ? hi : lo;
    for (u64 i = 0; i < n; ++i) {
        f32 v = rgba[i * 4u + 3u] * s;
        rgba[i * 4u + 3u] = v > 1.0f ? 1.0f : v;
    }
}

/* ---- Chain ----------------------------------------------------------------------------------- */

void de_tex_chain_free(const de_allocator *a, de_tex_chain *c) {
    if (c == NULL)
        return;
    for (u32 m = 0; m < c->mip_count; ++m)
        if (c->rgba[m] != NULL)
            DE_FREE(a, c->rgba[m],
                    (size_t)((u64)c->width[m] * c->height[m] * c->layers * 4u * sizeof(f32)));
    memset(c, 0, sizeof *c);
}

static de_result chain_level(const de_allocator *a, de_tex_chain *c, u32 m, u32 w, u32 h) {
    u64 bytes = (u64)w * h * c->layers * 4u * sizeof(f32);
    c->rgba[m] = (f32 *)DE_ALLOC(a, (size_t)bytes, DE_ALIGNOF(f32), DE_MEM_ASSET);
    if (c->rgba[m] == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "texture: out of memory for mip %u (%ux%u)", m, w, h);
    c->width[m] = w;
    c->height[m] = h;
    c->mip_count = m + 1u;
    return DE_OK;
}

de_result de_tex_mips_build(const de_allocator *a, const de_tex_image *base,
                            const de_tex_mip_params *p, de_tex_chain *out) {
    memset(out, 0, sizeof *out);
    out->layers = base->layers != 0 ? base->layers : 1u;
    u32 count = p->full_chain ? de_cooked_texture_full_mip_count(base->width, base->height) : 1u;
    de_result r = chain_level(a, out, 0, base->width, base->height);
    if (r != DE_OK)
        return r;
    u64 texels0 = (u64)base->width * base->height;
    memcpy(out->rgba[0], base->rgba, (size_t)(texels0 * out->layers * 4u * sizeof(f32)));
    if (p->normal_map)
        renormalize(out->rgba[0], texels0 * out->layers);
    for (u32 m = 1; m < count; ++m) {
        u32 pw = out->width[m - 1], ph = out->height[m - 1];
        u32 w = pw > 1u ? pw / 2u : 1u, h = ph > 1u ? ph / 2u : 1u;
        r = chain_level(a, out, m, w, h);
        if (r == DE_OK)
            r = de_tex_resample(a, out->rgba[m - 1], pw, ph, out->rgba[m], w, h, out->layers,
                                p->address);
        if (r != DE_OK) {
            de_tex_chain_free(a, out);
            return r;
        }
        if (p->normal_map)
            renormalize(out->rgba[m], (u64)w * h * out->layers);
        if (p->alpha_coverage > 0.0f)
            for (u32 l = 0; l < out->layers; ++l) {
                f64 t = scaled_coverage(out->rgba[0] + (u64)l * texels0 * 4u, texels0,
                                        p->alpha_coverage, 1.0f, p->alpha_levels);
                keep_coverage(out->rgba[m] + (u64)l * w * h * 4u, w, h, p->alpha_coverage, t,
                              p->alpha_levels);
            }
    }
    return DE_OK;
}
