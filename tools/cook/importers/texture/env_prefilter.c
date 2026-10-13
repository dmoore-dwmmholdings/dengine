/*
 * env_prefilter.c: HDR environments (DENG-100, SPEC §11.6): source layouts, the resample to a
 * cube with rotation, the GGX specular prefilter, and the environment branch of the importer
 * (DTEX cube + ENVS trailer). See env_internal.h for the conventions and the determinism rules
 * and render/formats/de_cooked_env.h for the output.
 *
 * Built with the deterministic float flags (no FMA contraction; tools/cook/CMakeLists.txt).
 */
#include "env_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_task.h>

#include "cook/lib/de_cooker.h"
#include "render/formats/de_cooked_le.h"

#include <string.h>

#define ENV_PI 3.14159265358979323846

void de_env_for(de_task_system *ts, u32 count, u32 grain, de_range_fn fn, void *user) {
    if (count == 0)
        return;
    if (ts != NULL) {
        de_task_parallel_for(ts, count, grain, fn, user);
        return;
    }
    for (u32 b = 0; b < count; b += grain)
        fn(b, count - b > grain ? b + grain : count, user);
}

static u32 log2_u32(u32 v) {
    u32 n = 0;
    while (v > 1u) {
        v >>= 1;
        ++n;
    }
    return n;
}

/* ---- Source layouts -------------------------------------------------------------------------- */

de_result de_env_source_layout(const de_tex_image *img, de_env_layout *out) {
    if (img->cube) {
        if (img->layers != 6u || img->width != img->height)
            return DE_FAIL(DE_ERR_UNSUPPORTED,
                           "environment: a cube source must be one cube with square faces (got "
                           "%u layers of %ux%u)",
                           img->layers, img->width, img->height);
        *out = DE_ENV_LAYOUT_CUBE;
        return DE_OK;
    }
    if (img->layers != 1u)
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "environment: an array texture (%u layers) is not an environment",
                       img->layers);
    const u32 w = img->width, h = img->height;
    if (w == 2u * h) {
        *out = DE_ENV_LAYOUT_EQUIRECT;
        return DE_OK;
    }
    if (w % 4u == 0 && h % 3u == 0 && w / 4u == h / 3u) {
        *out = DE_ENV_LAYOUT_CROSS_H;
        return DE_OK;
    }
    if (w % 3u == 0 && h % 4u == 0 && w / 3u == h / 4u) {
        *out = DE_ENV_LAYOUT_CROSS_V;
        return DE_OK;
    }
    return DE_FAIL(DE_ERR_UNSUPPORTED,
                   "environment: a %ux%u image is neither equirectangular (2:1) nor a cube cross "
                   "(4:3 or 3:4)",
                   w, h);
}

/* The source, ready to sample: an equirectangular image, or 6 Y-up faces. */
typedef struct env_source {
    de_env_layout layout;
    const f32 *rgba; /**< equirect: width x height; cube: 6 faces of face x face */
    u32 width, height;
    u32 face;
    f32 *owned; /**< faces copied out of a cross, or NULL */
    u64 owned_bytes;
} env_source;

/* Copies face `f` of a cross at cell (cx, cy), turned 180 degrees when `flip`. */
static void cross_face(const de_tex_image *img, f32 *faces, u32 n, u32 f, u32 cx, u32 cy,
                       bool flip) {
    f32 *dst = faces + (u64)f * n * n * 4u;
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            u32 sx = flip ? n - 1u - x : x, sy = flip ? n - 1u - y : y;
            const f32 *s = img->rgba + (((u64)cy * n + sy) * img->width + (u64)cx * n + sx) * 4u;
            memcpy(dst + ((u64)y * n + x) * 4u, s, 4u * sizeof(f32));
        }
}

static de_result source_init(const de_allocator *a, const de_tex_image *img, env_source *src) {
    memset(src, 0, sizeof *src);
    DE_TRY(de_env_source_layout(img, &src->layout));
    switch (src->layout) {
    case DE_ENV_LAYOUT_EQUIRECT:
        src->rgba = img->rgba;
        src->width = img->width;
        src->height = img->height;
        return DE_OK;
    case DE_ENV_LAYOUT_CUBE:
        src->rgba = img->rgba;
        src->face = img->width;
        return DE_OK;
    default:
        break;
    }
    const bool h = src->layout == DE_ENV_LAYOUT_CROSS_H;
    const u32 n = h ? img->width / 4u : img->width / 3u;
    src->face = n;
    src->owned_bytes = (u64)n * n * 6u * 4u * sizeof(f32);
    src->owned = (f32 *)DE_ALLOC(a, (size_t)src->owned_bytes, DE_ALIGNOF(f32), DE_MEM_ASSET);
    if (src->owned == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "environment: out of memory for the cross faces");
    /* Faces +X -X +Y -Y +Z -Z: cells of the cross (see env_internal.h). */
    cross_face(img, src->owned, n, 0, 2, 1, false);
    cross_face(img, src->owned, n, 1, 0, 1, false);
    cross_face(img, src->owned, n, 2, 1, 0, false);
    cross_face(img, src->owned, n, 3, 1, 2, false);
    cross_face(img, src->owned, n, 4, 1, 1, false);
    if (h)
        cross_face(img, src->owned, n, 5, 3, 1, false);
    else
        cross_face(img, src->owned, n, 5, 1, 3, true);
    src->rgba = src->owned;
    return DE_OK;
}

static void source_free(const de_allocator *a, env_source *src) {
    if (src->owned != NULL)
        DE_FREE(a, src->owned, (size_t)src->owned_bytes);
    memset(src, 0, sizeof *src);
}

/* Bilinear lookup in one n x n f32 RGBA face at face coordinates sc, tc in [-1, 1]; texels
 * outside the face clamp to its edge. */
static void face_bilinear(const f32 *face, u32 n, f32 sc, f32 tc, f32 out[3]) {
    f32 px = (sc + 1.0f) * 0.5f * (f32)n - 0.5f;
    f32 py = (tc + 1.0f) * 0.5f * (f32)n - 0.5f;
    f32 fx0 = de_dfloor(px), fy0 = de_dfloor(py);
    f32 fx = px - fx0, fy = py - fy0;
    i32 x0 = (i32)fx0, y0 = (i32)fy0, last = (i32)n - 1;
    i32 x1 = x0 + 1, y1 = y0 + 1;
    x0 = x0 < 0 ? 0 : (x0 > last ? last : x0);
    x1 = x1 < 0 ? 0 : (x1 > last ? last : x1);
    y0 = y0 < 0 ? 0 : (y0 > last ? last : y0);
    y1 = y1 < 0 ? 0 : (y1 > last ? last : y1);
    const f32 *p00 = face + ((u64)y0 * n + (u64)x0) * 4u;
    const f32 *p10 = face + ((u64)y0 * n + (u64)x1) * 4u;
    const f32 *p01 = face + ((u64)y1 * n + (u64)x0) * 4u;
    const f32 *p11 = face + ((u64)y1 * n + (u64)x1) * 4u;
    for (u32 c = 0; c < 3u; ++c) {
        f32 top = p00[c] + (p10[c] - p00[c]) * fx;
        f32 bottom = p01[c] + (p11[c] - p01[c]) * fx;
        out[c] = top + (bottom - top) * fy;
    }
}

/* The source radiance along engine direction e (any length but zero). */
static void source_sample(const env_source *src, const f32 e[3], f32 out[3]) {
    if (src->layout == DE_ENV_LAYOUT_EQUIRECT) {
        const u32 w = src->width, h = src->height;
        f32 phi = de_datan2(e[0], e[1]);
        f32 theta = de_datan2(e[2], de_dsqrt(e[0] * e[0] + e[1] * e[1]));
        f32 u = 0.5f + phi * (f32)(0.5 / ENV_PI);
        f32 v = 0.5f - theta * (f32)(1.0 / ENV_PI);
        f32 px = u * (f32)w - 0.5f, py = v * (f32)h - 0.5f;
        f32 fx0 = de_dfloor(px), fy0 = de_dfloor(py);
        f32 fx = px - fx0, fy = py - fy0;
        i64 x0 = (i64)fx0, y0 = (i64)fy0;
        i64 x1 = x0 + 1, y1 = y0 + 1;
        /* Wrap around the horizon, clamp at the poles. */
        x0 = ((x0 % (i64)w) + (i64)w) % (i64)w;
        x1 = ((x1 % (i64)w) + (i64)w) % (i64)w;
        y0 = y0 < 0 ? 0 : (y0 >= (i64)h ? (i64)h - 1 : y0);
        y1 = y1 < 0 ? 0 : (y1 >= (i64)h ? (i64)h - 1 : y1);
        const f32 *p00 = src->rgba + ((u64)y0 * w + (u64)x0) * 4u;
        const f32 *p10 = src->rgba + ((u64)y0 * w + (u64)x1) * 4u;
        const f32 *p01 = src->rgba + ((u64)y1 * w + (u64)x0) * 4u;
        const f32 *p11 = src->rgba + ((u64)y1 * w + (u64)x1) * 4u;
        for (u32 c = 0; c < 3u; ++c) {
            f32 top = p00[c] + (p10[c] - p00[c]) * fx;
            f32 bottom = p01[c] + (p11[c] - p01[c]) * fx;
            out[c] = top + (bottom - top) * fy;
        }
        return;
    }
    /* Y-up source cube: engine (x, y, z) is source (x, z, -y). */
    const f32 s[3] = {e[0], e[2], -e[1]};
    f32 sc, tc;
    u32 f = de_cooked_env_direction_face(s, &sc, &tc);
    const u32 n = src->face;
    face_bilinear(src->rgba + (u64)f * n * n * 4u, n, sc, tc, out);
}

/* ---- Mip 0: resample with rotation ----------------------------------------------------------- */

typedef struct env_base_job {
    const env_source *src;
    f32 *dst; /**< 6 x n x n x 4 */
    u32 n;
    u32 k;            /**< subsamples per axis */
    f32 cos_r, sin_r; /**< the rotation about +Z */
} env_base_job;

/* One (face, row) per index. */
static void base_rows(u32 begin, u32 end, void *user) {
    const env_base_job *j = (const env_base_job *)user;
    const u32 n = j->n, k = j->k;
    const f32 inv_n = 2.0f / (f32)n, inv_k = 1.0f / (f32)k, inv_kk = 1.0f / (f32)(k * k);
    for (u32 i = begin; i < end; ++i) {
        const u32 face = i / n, y = i % n;
        f32 *row = j->dst + ((u64)face * n + y) * n * 4u;
        for (u32 x = 0; x < n; ++x) {
            f32 sum[3] = {0.0f, 0.0f, 0.0f};
            for (u32 sy = 0; sy < k; ++sy) {
                f32 tc = ((f32)y + ((f32)sy + 0.5f) * inv_k) * inv_n - 1.0f;
                for (u32 sx = 0; sx < k; ++sx) {
                    f32 sc = ((f32)x + ((f32)sx + 0.5f) * inv_k) * inv_n - 1.0f;
                    f32 d[3];
                    de_cooked_env_face_direction(face, sc, tc, d);
                    /* R^-1 d: the source direction whose radiance lands on d. */
                    const f32 e[3] = {j->cos_r * d[0] + j->sin_r * d[1],
                                      j->cos_r * d[1] - j->sin_r * d[0], d[2]};
                    f32 c[3];
                    source_sample(j->src, e, c);
                    for (u32 ch = 0; ch < 3u; ++ch)
                        sum[ch] += c[ch] > 0.0f ? c[ch] : 0.0f;
                }
            }
            f32 *t = row + (u64)x * 4u;
            t[0] = sum[0] * inv_kk;
            t[1] = sum[1] * inv_kk;
            t[2] = sum[2] * inv_kk;
            t[3] = 1.0f;
        }
    }
}

/* Source texels per output texel along one axis, as subsamples per axis in [1, 8]. */
static u32 subsamples(const env_source *src, u32 n) {
    /* An equirect texel spans 2 pi / width; a cube texel about (pi / 2) / n. */
    u32 per = src->layout == DE_ENV_LAYOUT_EQUIRECT ? (src->width + 4u * n - 1u) / (4u * n)
                                                    : (src->face + n - 1u) / n;
    return per < 1u ? 1u : (per > 8u ? 8u : per);
}

/* ---- GGX prefilter --------------------------------------------------------------------------- */

/* One importance sample in the tangent frame of N = V = R: the light direction, its n.l weight,
 * and the radiance chain level (l0) and blend (f) its PDF selects. */
typedef struct env_sample {
    f32 l[3];
    f32 w;
    u32 l0;
    f32 f;
} env_sample;

typedef struct env_pref_job {
    u32 mip_count;
    u32 n0;
    const f32 *chain[DE_COOKED_TEXTURE_MAX_MIPS]; /**< radiance chain, 6 faces per level */
    f32 *dst[DE_COOKED_TEXTURE_MAX_MIPS];         /**< output mips (1.. used) */
    const env_sample *samples[DE_COOKED_TEXTURE_MAX_MIPS];
    u32 sample_count[DE_COOKED_TEXTURE_MAX_MIPS];
    f64 weight_sum[DE_COOKED_TEXTURE_MAX_MIPS];
    u32 row_start[DE_COOKED_TEXTURE_MAX_MIPS + 1u]; /**< first job index of each mip */
} env_pref_job;

/* Van der Corput radical inverse in base 2. */
static f32 radical_inverse(u32 i) {
    i = (i << 16) | (i >> 16);
    i = ((i & 0x55555555u) << 1) | ((i & 0xAAAAAAAAu) >> 1);
    i = ((i & 0x33333333u) << 2) | ((i & 0xCCCCCCCCu) >> 2);
    i = ((i & 0x0F0F0F0Fu) << 4) | ((i & 0xF0F0F0F0u) >> 4);
    i = ((i & 0x00FF00FFu) << 8) | ((i & 0xFF00FF00u) >> 8);
    return (f32)((f64)i * (1.0 / 4294967296.0));
}

/*
 * The samples of mip `k`: Hammersley points (i / S, radical inverse of i) to GGX half vectors
 * H around N = (0, 0, 1) of alpha = r^2, reflected (L = 2 (V.H) H - V, V = N). With N = V the
 * PDF of L is D(n.h) / 4, so the solid angle of one sample is omega_s = 4 / (S D), and the chain
 * level that matches it is 0.5 log2(omega_s / omega_p) + 1 (omega_p: one mip-0 texel, 4 pi / 6
 * n0^2). Samples with n.l <= 0 are dropped.
 */
static u32 make_samples(env_sample *out, u32 count, f32 roughness, u32 n0, u32 mip_count,
                        f64 *weight_sum) {
    const f32 alpha = roughness * roughness;
    const f32 a2 = alpha * alpha;
    const f32 omega_p = (f32)(4.0 * ENV_PI / 6.0) / ((f32)n0 * (f32)n0);
    const f32 max_lod = (f32)(mip_count - 1u);
    const f32 inv_ln2 = (f32)(1.0 / 0.69314718055994530942);
    u32 kept = 0;
    f64 wsum = 0.0;
    for (u32 i = 0; i < count; ++i) {
        f32 xi1 = (f32)i / (f32)count, xi2 = radical_inverse(i);
        f32 phi = (f32)(2.0 * ENV_PI) * xi1;
        f32 cos2 = (1.0f - xi2) / (1.0f + (a2 - 1.0f) * xi2);
        f32 cos_t = de_dsqrt(cos2);
        f32 sin_t = de_dsqrt(1.0f - cos2 > 0.0f ? 1.0f - cos2 : 0.0f);
        f32 sp, cp;
        de_dsincos(phi, &sp, &cp);
        f32 h[3] = {sin_t * cp, sin_t * sp, cos_t};
        f32 l[3] = {2.0f * cos_t * h[0], 2.0f * cos_t * h[1], 2.0f * cos_t * h[2] - 1.0f};
        f32 ndotl = l[2];
        if (!(ndotl > 0.0f))
            continue;
        f32 dd = cos2 * (a2 - 1.0f) + 1.0f;
        f32 d = a2 / ((f32)ENV_PI * dd * dd);
        f32 omega_s = 4.0f / ((f32)count * d);
        f32 lod = 0.5f * de_dlog(omega_s / omega_p) * inv_ln2 + 1.0f;
        lod = lod > 0.0f ? (lod < max_lod ? lod : max_lod) : 0.0f;
        env_sample *s = &out[kept++];
        s->l[0] = l[0];
        s->l[1] = l[1];
        s->l[2] = l[2];
        s->w = ndotl;
        s->l0 = (u32)lod;
        s->f = lod - (f32)s->l0;
        if (s->l0 >= mip_count - 1u) {
            s->l0 = mip_count - 1u;
            s->f = 0.0f;
        }
        wsum += (f64)ndotl;
    }
    *weight_sum = wsum;
    return kept;
}

static void chain_sample(const env_pref_job *j, const f32 d[3], u32 l0, f32 f, f32 out[3]) {
    f32 sc, tc;
    u32 face = de_cooked_env_direction_face(d, &sc, &tc);
    u32 n = j->n0 >> l0;
    face_bilinear(j->chain[l0] + (u64)face * n * n * 4u, n, sc, tc, out);
    if (f > 0.0f) {
        f32 hi[3];
        u32 n1 = n >> 1;
        face_bilinear(j->chain[l0 + 1u] + (u64)face * n1 * n1 * 4u, n1, sc, tc, hi);
        for (u32 c = 0; c < 3u; ++c)
            out[c] += (hi[c] - out[c]) * f;
    }
}

/* One (mip >= 1, face, row) per index. Each texel's sum runs sequentially over its mip's
 * samples in table order. */
static void pref_rows(u32 begin, u32 end, void *user) {
    const env_pref_job *j = (const env_pref_job *)user;
    for (u32 i = begin; i < end; ++i) {
        u32 k = 1;
        while (i >= j->row_start[k + 1u])
            ++k;
        const u32 n = j->n0 >> k, local = i - j->row_start[k];
        const u32 face = local / n, y = local % n;
        const env_sample *smp = j->samples[k];
        const u32 count = j->sample_count[k];
        const f64 inv_w = j->weight_sum[k] > 0.0 ? 1.0 / j->weight_sum[k] : 0.0;
        f32 *row = j->dst[k] + ((u64)face * n + y) * n * 4u;
        const f32 tc = (2.0f * (f32)y + 1.0f) / (f32)n - 1.0f;
        for (u32 x = 0; x < n; ++x) {
            const f32 sc = (2.0f * (f32)x + 1.0f) / (f32)n - 1.0f;
            f32 nv[3];
            de_cooked_env_face_direction(face, sc, tc, nv);
            f32 inv_len = 1.0f / de_dsqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
            nv[0] *= inv_len;
            nv[1] *= inv_len;
            nv[2] *= inv_len;
            /* Tangent frame of N (Duff et al. 2017, branch on the sign of z). */
            const f32 sign = nv[2] >= 0.0f ? 1.0f : -1.0f;
            const f32 a = -1.0f / (sign + nv[2]);
            const f32 b = nv[0] * nv[1] * a;
            const f32 t[3] = {1.0f + sign * nv[0] * nv[0] * a, sign * b, -sign * nv[0]};
            const f32 bt[3] = {b, sign + nv[1] * nv[1] * a, -nv[1]};
            f64 sum[3] = {0.0, 0.0, 0.0};
            for (u32 s = 0; s < count; ++s) {
                const env_sample *p = &smp[s];
                f32 d[3];
                for (u32 c = 0; c < 3u; ++c)
                    d[c] = t[c] * p->l[0] + bt[c] * p->l[1] + nv[c] * p->l[2];
                f32 rad[3];
                chain_sample(j, d, p->l0, p->f, rad);
                for (u32 c = 0; c < 3u; ++c)
                    sum[c] += (f64)rad[c] * (f64)p->w;
            }
            f32 *o = row + (u64)x * 4u;
            o[0] = (f32)(sum[0] * inv_w);
            o[1] = (f32)(sum[1] * inv_w);
            o[2] = (f32)(sum[2] * inv_w);
            o[3] = 1.0f;
        }
    }
}

/* 2x2 box average of every face of level `src` (n x n) into `dst` (n/2 x n/2). */
static void box_down(const f32 *src, u32 n, f32 *dst) {
    const u32 h = n / 2u;
    for (u32 f = 0; f < 6u; ++f) {
        const f32 *s = src + (u64)f * n * n * 4u;
        f32 *d = dst + (u64)f * h * h * 4u;
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < h; ++x)
                for (u32 c = 0; c < 4u; ++c) {
                    const f32 *r0 = s + ((u64)(2u * y) * n + 2u * x) * 4u + c;
                    const f32 *r1 = r0 + (u64)n * 4u;
                    d[((u64)y * h + x) * 4u + c] = ((r0[0] + r0[4]) + (r1[0] + r1[4])) * 0.25f;
                }
    }
}

static u64 level_bytes(u32 n) {
    return (u64)n * n * 6u * 4u * sizeof(f32);
}

static de_result check_settings(const de_env_settings *s) {
    if (s->size < DE_ENV_MIN_SIZE || s->size > DE_ENV_MAX_SIZE || (s->size & (s->size - 1u)) != 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "environment: env_size %u is not a power of two from %u to %u", s->size,
                       DE_ENV_MIN_SIZE, DE_ENV_MAX_SIZE);
    if (s->samples < 1u || s->samples > DE_ENV_MAX_SAMPLES)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "environment: env_samples %u is not 1 to %u",
                       s->samples, DE_ENV_MAX_SAMPLES);
    if (!(s->clamp_luminance >= 0.0f) || !(s->rotation_deg == s->rotation_deg))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "environment: bad rotation_deg or clamp_luminance");
    return DE_OK;
}

de_result de_env_build(const de_allocator *a, de_task_system *ts, const de_tex_image *src_img,
                       const de_env_settings *s, de_tex_chain *out) {
    memset(out, 0, sizeof *out);
    DE_TRY(check_settings(s));
    if (src_img == NULL || src_img->rgba == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "environment: no source image");
    env_source src;
    DE_TRY(source_init(a, src_img, &src));

    const u32 n0 = s->size, mips = log2_u32(n0) + 1u;
    out->layers = 6;
    out->mip_count = mips;
    de_result r = DE_OK;
    for (u32 m = 0; m < mips && r == DE_OK; ++m) {
        u32 n = n0 >> m;
        out->width[m] = out->height[m] = n;
        out->rgba[m] = (f32 *)DE_ALLOC(a, (size_t)level_bytes(n), DE_ALIGNOF(f32), DE_MEM_ASSET);
        if (out->rgba[m] == NULL)
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "environment: out of memory for mip %u", m);
    }
    /* The radiance chain the prefilter reads: level 0 is mip 0 (or its clamped copy). */
    f32 *chain[DE_COOKED_TEXTURE_MAX_MIPS];
    memset(chain, 0, sizeof chain);
    const bool clamp = s->clamp_luminance > 0.0f;
    for (u32 m = clamp ? 0u : 1u; m < mips && r == DE_OK; ++m) {
        chain[m] = (f32 *)DE_ALLOC(a, (size_t)level_bytes(n0 >> m), DE_ALIGNOF(f32), DE_MEM_ASSET);
        if (chain[m] == NULL)
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "environment: out of memory for the radiance chain");
    }
    env_sample *tables = NULL;
    u64 table_bytes = (u64)s->samples * (mips - 1u) * sizeof(env_sample);
    if (r == DE_OK) {
        tables =
            (env_sample *)DE_ALLOC(a, (size_t)table_bytes, DE_ALIGNOF(env_sample), DE_MEM_ASSET);
        if (tables == NULL)
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "environment: out of memory for the samples");
    }
    if (r == DE_OK) {
        /* Mip 0. */
        f32 sr, cr;
        de_dsincos(s->rotation_deg * (f32)(ENV_PI / 180.0), &sr, &cr);
        env_base_job bj = {&src, out->rgba[0], n0, subsamples(&src, n0), cr, sr};
        de_env_for(ts, 6u * n0, 1u, base_rows, &bj);

        /* Radiance chain. */
        if (clamp) {
            const u64 texels = (u64)n0 * n0 * 6u;
            for (u64 i = 0; i < texels; ++i) {
                const f32 *p = out->rgba[0] + i * 4u;
                f32 *q = chain[0] + i * 4u;
                f32 lum = de_env_luminance(p);
                f32 k = lum > s->clamp_luminance ? s->clamp_luminance / lum : 1.0f;
                q[0] = p[0] * k;
                q[1] = p[1] * k;
                q[2] = p[2] * k;
                q[3] = 1.0f;
            }
        } else {
            chain[0] = out->rgba[0];
        }
        for (u32 m = 1; m < mips; ++m)
            box_down(chain[m - 1u], n0 >> (m - 1u), chain[m]);

        /* Mips 1 and up. */
        env_pref_job pj;
        memset(&pj, 0, sizeof pj);
        pj.mip_count = mips;
        pj.n0 = n0;
        u32 rows = 0;
        for (u32 m = 0; m < mips; ++m) {
            pj.chain[m] = chain[m];
            pj.dst[m] = out->rgba[m];
            pj.row_start[m] = rows;
            if (m >= 1u) {
                env_sample *t = tables + (u64)(m - 1u) * s->samples;
                f32 rough = (f32)m / (f32)(mips - 1u);
                pj.samples[m] = t;
                pj.sample_count[m] =
                    make_samples(t, s->samples, rough, n0, mips, &pj.weight_sum[m]);
                rows += 6u * (n0 >> m);
            }
        }
        pj.row_start[mips] = rows;
        pj.row_start[0] = pj.row_start[1];
        de_env_for(ts, rows, 1u, pref_rows, &pj);
    }
    if (tables != NULL)
        DE_FREE(a, tables, (size_t)table_bytes);
    for (u32 m = clamp ? 0u : 1u; m < mips; ++m)
        if (chain[m] != NULL)
            DE_FREE(a, chain[m], (size_t)level_bytes(n0 >> m));
    source_free(a, &src);
    if (r != DE_OK)
        de_tex_chain_free(a, out);
    return r;
}

/* ---- Importer branch ------------------------------------------------------------------------- */

void de_env_write_trailer(u8 *dst, u32 face_size, u32 mip_count, const de_env_settings *s,
                          const de_env_sh9 *sh) {
    memset(dst, 0, DE_COOKED_ENV_TRAILER_SIZE);
    memcpy(dst + DE_COOKED_ENV_OFF_MAGIC, DE_COOKED_ENV_MAGIC, 4);
    de_cooked_put_u32(dst + DE_COOKED_ENV_OFF_VERSION, DE_COOKED_ENV_VERSION);
    de_cooked_put_u32(dst + DE_COOKED_ENV_OFF_SIZE, DE_COOKED_ENV_TRAILER_SIZE);
    de_cooked_put_u32(dst + DE_COOKED_ENV_OFF_FACE_SIZE, face_size);
    de_cooked_put_u32(dst + DE_COOKED_ENV_OFF_MIP_COUNT, mip_count);
    de_cooked_put_u32(dst + DE_COOKED_ENV_OFF_ROUGHNESS_MAPPING, DE_COOKED_ENV_ROUGHNESS_LINEAR);
    de_cooked_put_f32(dst + DE_COOKED_ENV_OFF_MIP_PER_ROUGHNESS, (f32)(mip_count - 1u));
    de_cooked_put_u32(dst + DE_COOKED_ENV_OFF_SAMPLE_COUNT, s->samples);
    de_cooked_put_f32(dst + DE_COOKED_ENV_OFF_ROTATION_DEG, s->rotation_deg);
    de_cooked_put_f32(dst + DE_COOKED_ENV_OFF_CLAMP_LUMINANCE, s->clamp_luminance);
    de_cooked_put_f32(dst + DE_COOKED_ENV_OFF_MEAN_LUMINANCE, sh->mean_luminance);
    de_cooked_put_f32(dst + DE_COOKED_ENV_OFF_MAX_LUMINANCE, sh->max_luminance);
    for (u32 k = 0; k < DE_COOKED_ENV_SH_COUNT; ++k)
        for (u32 c = 0; c < 3u; ++c)
            de_cooked_put_f32(dst + DE_COOKED_ENV_OFF_SH9 + 12u * k + 4u * c, sh->irradiance[k][c]);
}

static de_result read_env_settings(de_asset_import_ctx *ctx, de_env_settings *s) {
    memset(s, 0, sizeof *s);
    i64 size = de_asset_import_setting_int(ctx, "env_size", (i64)DE_ENV_DEFAULT_SIZE);
    i64 samples = de_asset_import_setting_int(ctx, "env_samples", (i64)DE_ENV_DEFAULT_SAMPLES);
    if (size < (i64)DE_ENV_MIN_SIZE || size > (i64)DE_ENV_MAX_SIZE || (size & (size - 1)) != 0)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "texture: env_size must be a power of two from %u to %u",
                                    DE_ENV_MIN_SIZE, DE_ENV_MAX_SIZE);
    if (samples < 1 || samples > (i64)DE_ENV_MAX_SAMPLES)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "texture: env_samples must be an integer from 1 to %u",
                                    DE_ENV_MAX_SAMPLES);
    f64 rot = de_asset_import_setting_float(ctx, "rotation_deg", 0.0);
    if (!(rot >= -36000.0 && rot <= 36000.0))
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "texture: rotation_deg must be a number of degrees");
    f64 clamp = 0.0;
    if (de_asset_import_setting_has(ctx, "clamp_luminance")) {
        clamp = de_asset_import_setting_float(ctx, "clamp_luminance", -1.0);
        if (!(clamp > 0.0 && clamp < 3.0e38))
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "texture: clamp_luminance must be a number above 0");
    }
    s->size = (u32)size;
    s->samples = (u32)samples;
    s->rotation_deg = (f32)rot;
    s->clamp_luminance = (f32)clamp;
    return DE_OK;
}

de_result de_env_cook(de_asset_import_ctx *ctx, const de_allocator *a, const de_tex_settings *s,
                      const de_tex_image *img) {
    de_env_settings es;
    DE_TRY(read_env_settings(ctx, &es));
    de_task_system *ts = de_cooker_import_tasks(ctx);
    de_tex_chain chain;
    DE_TRY(de_env_build(a, ts, img, &es, &chain));
    de_env_sh9 sh;
    de_result r = de_env_sh9_project(a, ts, chain.rgba[0], es.size, &sh);
    if (r != DE_OK) {
        de_tex_chain_free(a, &chain);
        return r;
    }
    de_tex_dtex_desc d;
    memset(&d, 0, sizeof d);
    /* The encode stage picks the format (RGBA16F, or BC6H by the compression rules). */
    d.format = de_tex_choose_format(s, img);
    d.dim = DE_TEX_CUBE;
    d.width = d.height = es.size;
    d.layers = 6;
    d.mip_count = chain.mip_count;
    d.usage = DE_COOKED_TEXTURE_USAGE_ENVIRONMENT;
    de_tex_dtex t;
    r = de_tex_dtex_from_chain(a, &d, &chain, &t);
    de_tex_chain_free(a, &chain);
    if (r != DE_OK)
        return r;
    const u64 total = t.size + DE_COOKED_ENV_TRAILER_SIZE;
    u8 *blob = (u8 *)DE_ALLOC(a, (size_t)total, 16, DE_MEM_ASSET);
    if (blob == NULL) {
        de_tex_dtex_free(a, &t);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "environment: out of memory for %llu cooked bytes",
                       (unsigned long long)total);
    }
    memcpy(blob, t.data, (size_t)t.size);
    de_env_write_trailer(blob + t.size, es.size, d.mip_count, &es, &sh);
    de_tex_dtex_free(a, &t);
    r = de_asset_import_write(ctx, blob, total);
    DE_FREE(a, blob, (size_t)total);
    return r;
}
