/*
 * anim_clip_compress.c: animation clips of the model importer (DENG-104, SPEC §11.6, §21.5):
 * one IR animation over one skeleton becomes one DCLP blob (src/anim/formats/de_cooked_clip.h).
 *
 * Steps:
 *   1. Tracks: one per (skeleton joint, channel) that the animation drives, in joint order, then
 *      translation, rotation, scale. Morph weights and channels on other nodes are skipped (a
 *      warning).
 *   2. Time table: a uniform grid of round(duration x anim_sample_rate) intervals from 0 to the
 *      duration, merged with every source key time (a grid time closer than 10 us to a key time
 *      gives way to the key time). So a source key is always a sample: linear and step sources are
 *      reproduced at their keys, and cubic splines are followed at the sample rate.
 *   3. Sampling: every track at every time, in f64, honouring the source interpolation (step,
 *      linear with shortest-arc slerp, cubic Hermite). Rotations are normalised and made
 *      hemisphere-continuous (neighbouring samples have a non-negative dot product).
 *   4. Per track, with a local tolerance (metres, radians or relative scale):
 *        - constant: every sample within the tolerance of one value: store that value;
 *        - uniform scale: x, y, z equal within a quarter of the tolerance: one component;
 *        - quantization: rotations smallest-three in 48 bits, translations and scales 16 bits over
 *          the track's [min, min + extent]; when that alone costs more than 3/4 of the tolerance,
 *          raw f32 only. The quantized values depend only on the samples, so they are computed
 *          once per track and reused by every pass;
 *        - key reduction: greedy from the first sample; each segment is the longest (up to
 *          MAX_SEGMENT samples) whose decoded linear interpolation stays within the tolerance at
 *          every sample in between. Its end is found by galloping and a binary search (DENG-399),
 *          so a segment of L samples costs O(L log L) evaluations, not O(L^2);
 *        - the quantized and the raw f32 encodings are both reduced, and the smaller is kept
 *          (quantized on a tie); the raw reduction stops as soon as it cannot be smaller.
 *   5. Model-space check: every sample is decoded with the sampler's own helpers
 *      (de_cooked_clip.h), the joints are composed to model space next to the source pose, and
 *      each joint's position must be within `anim_error` and its orientation within
 *      `anim_error_angle` (at 90 % of each, a margin for the f32 decode). The tracks of a joint
 *      that fails, and of its ancestors, get half the tolerance (a quarter, an eighth... when the
 *      joint is 2, 4... times over the limit) and are encoded again; after 8 passes they are
 *      stored raw with every key. Errors therefore never add up past the settings, whatever the
 *      chain length.
 *   The initial tolerance of a track: translation anim_error (divided by the parent's scale);
 *   rotation and scale min(anim_error_angle, anim_error / reach), where reach is the distance
 *   from the joint to its farthest descendant (its "tip") in the rest pose.
 *
 * Cost: about samples x tracks x (log2(MAX_SEGMENT) + 27 smallest-three candidates) for the
 * encoding, plus samples x joints per check pass; passes are few (one to three for most clips).
 *
 * Root motion: the tracks of the skeleton's root joint (the top joint of the skin) carry
 * DE_COOKED_CLIP_TRACK_ROOT. Loop hint: every track's first and last samples are within its
 * tolerance. Everything runs in a fixed order with no hash-map iteration, and the trigonometry
 * goes through de_dmath (no libm transcendentals), so a cook is byte-identical. This file is
 * compiled with the deterministic flags (no FMA contraction), like the sampler it mirrors.
 */
#include "anim_cook.h"
#include "model_internal.h"
#include "model_ir.h"

#include <dengine/de_base.h>
#include <dengine/de_math.h>

#include "anim/formats/de_cooked_clip.h"
#include "anim/formats/de_cooked_skeleton.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A grid time this close to a source key time (seconds, plus a relative part) is merged. */
#define TIME_EPS_ABS 1e-5
#define TIME_EPS_REL 1e-6
/* Model-space check passes and when tracks fall back to raw keys. */
#define CHECK_PASSES 12u
#define RAW_AFTER    8u
/* Margin of the model-space check against the settings. */
#define CHECK_MARGIN 0.9
/* The smallest factor one check pass applies to a tolerance. */
#define MIN_TIGHTEN (1.0 / 1024.0)
/* Longest run of samples between two kept keys (bounds the cost of key reduction). */
#define MAX_SEGMENT 256u
/* Share of a track's tolerance that quantization may use; the rest is for key reduction. */
#define QUANT_SHARE 0.75

/* de_dsqrt64 and de_dsqrt, inline for the normal positive numbers that are nearly all of the
 * calls here: for those, both are the correctly rounded IEEE sqrt, and no FTZ/DAZ mode changes
 * it. Anything else (0, subnormal, negative, NaN, inf) goes to the de_dmath function itself. */
static f64 sqrt64(f64 x) {
    return x >= DBL_MIN && x <= DBL_MAX ? sqrt(x) : de_dsqrt64(x);
}

static f32 sqrt32(f32 x) {
    return x >= FLT_MIN && x <= FLT_MAX ? sqrtf(x) : de_dsqrt(x);
}

static f64 absd(f64 x) {
    return x < 0.0 ? -x : x;
}

/* ---- Source evaluation (f64) ----------------------------------------------------------------- */

/* acos for d in [0, 1]: de_dacos for the start, then Newton steps on de_dcos64 (deterministic). */
static f64 acos64(f64 d) {
    f64 th = (f64)de_dacos((f32)d);
    for (u32 i = 0; i < 3; ++i) {
        f64 s = de_dsin64(th);
        if (!(s > 1e-300))
            break;
        th += (de_dcos64(th) - d) / s;
    }
    return th;
}

static void quat_normalize(f64 q[4]) {
    f64 n = sqrt64(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0.0) {
        for (u32 i = 0; i < 4; ++i)
            q[i] /= n;
    } else {
        q[0] = q[1] = q[2] = 0.0;
        q[3] = 1.0;
    }
}

/*
 * The angle between unit quaternions a and b seen as 4D vectors, b already on a's side
 * (dot >= 0): half the rotation angle between them. Small angles come from the chord
 * (|a - b| = 2 sin(angle / 2), asin by its series), so they keep full precision.
 */
static f64 quat_half_angle(const f64 a[4], const f64 b[4]) {
    f64 c2 = 0.0, d = 0.0;
    for (u32 i = 0; i < 4; ++i) {
        c2 += (a[i] - b[i]) * (a[i] - b[i]);
        d += a[i] * b[i];
    }
    f64 x = 0.5 * sqrt64(c2);
    if (x < 0.05) {
        f64 x2 = x * x;
        return 2.0 * x * (1.0 + x2 * (1.0 / 6.0 + x2 * (3.0 / 40.0 + x2 * (5.0 / 112.0))));
    }
    return acos64(d > 1.0 ? 1.0 : d < 0.0 ? 0.0 : d);
}

/* Shortest-arc slerp between two unit quaternions: the keys, b on a's side, and the angle. */
typedef struct slerp_span {
    f64 a[4], b[4], th, sn;
} slerp_span;

static void slerp_prepare(const f64 a[4], const f64 b_in[4], slerp_span *sp) {
    f64 d = a[0] * b_in[0] + a[1] * b_in[1] + a[2] * b_in[2] + a[3] * b_in[3];
    for (u32 i = 0; i < 4; ++i) {
        sp->a[i] = a[i];
        sp->b[i] = d < 0.0 ? -b_in[i] : b_in[i];
    }
    sp->th = quat_half_angle(sp->a, sp->b);
    sp->sn = sp->th > 1e-9 ? de_dsin64(sp->th) : 0.0;
}

static void slerp_at(const slerp_span *sp, f64 s, f64 out[4]) {
    f64 wa = 1.0 - s, wb = s;
    if (sp->th > 1e-9) {
        wa = de_dsin64((1.0 - s) * sp->th) / sp->sn;
        wb = de_dsin64(s * sp->th) / sp->sn;
    }
    for (u32 i = 0; i < 4; ++i)
        out[i] = wa * sp->a[i] + wb * sp->b[i];
    quat_normalize(out);
}

static const f32 *key_value(const de_model_anim_channel *c, u32 k) {
    u32 n = c->components;
    return c->interp == DE_MODEL_INTERP_CUBIC ? c->values + ((u64)k * 3u + 1u) * n
                                              : c->values + (u64)k * n;
}

/* The slerp span of the key interval chan_eval used last (the samples come in time order, so
 * consecutive samples mostly share one; its angle then costs one acos instead of one per
 * sample). */
typedef struct chan_cache {
    u32 lo; /* the interval's first key + 1; 0: none */
    slerp_span span;
} chan_cache;

/* The source channel at time t (seconds), `components` values. */
static void chan_eval(const de_model_anim_channel *c, f64 t, f64 out[4], chan_cache *cache) {
    u32 n = c->components, kc = c->key_count;
    bool rot = c->path == DE_MODEL_ANIM_ROTATION;
    const f32 *v;
    if (kc == 1 || !(t > (f64)c->times[0])) {
        v = key_value(c, 0);
    } else if (!(t < (f64)c->times[kc - 1u])) {
        v = key_value(c, kc - 1u);
    } else {
        u32 lo = 0, hi = kc - 1u; /* times[lo] < t < times[hi] */
        while (hi - lo > 1u) {
            u32 mid = lo + (hi - lo) / 2u;
            if ((f64)c->times[mid] <= t)
                lo = mid;
            else
                hi = mid;
        }
        f64 t0 = c->times[lo], t1 = c->times[hi], dt = t1 - t0;
        f64 s = dt > 0.0 ? (t - t0) / dt : 1.0;
        if (c->interp == DE_MODEL_INTERP_STEP) {
            v = key_value(c, lo);
        } else if (c->interp == DE_MODEL_INTERP_CUBIC) {
            const f32 *p0 = key_value(c, lo), *p1 = key_value(c, hi);
            const f32 *b0 = c->values + ((u64)lo * 3u + 2u) * n; /* out-tangent of lo */
            const f32 *a1 = c->values + ((u64)hi * 3u) * n;      /* in-tangent of hi */
            f64 s2 = s * s, s3 = s2 * s;
            f64 h00 = 2.0 * s3 - 3.0 * s2 + 1.0, h10 = s3 - 2.0 * s2 + s;
            f64 h01 = -2.0 * s3 + 3.0 * s2, h11 = s3 - s2;
            for (u32 i = 0; i < n; ++i)
                out[i] = h00 * p0[i] + h10 * dt * b0[i] + h01 * p1[i] + h11 * dt * a1[i];
            if (rot)
                quat_normalize(out);
            return;
        } else {
            const f32 *p0 = key_value(c, lo), *p1 = key_value(c, hi);
            if (rot) {
                if (cache->lo != lo + 1u) {
                    f64 a[4], b[4];
                    for (u32 i = 0; i < 4; ++i) {
                        a[i] = p0[i];
                        b[i] = p1[i];
                    }
                    quat_normalize(a);
                    quat_normalize(b);
                    slerp_prepare(a, b, &cache->span);
                    cache->lo = lo + 1u;
                }
                slerp_at(&cache->span, s, out);
            } else {
                for (u32 i = 0; i < n; ++i)
                    out[i] = (f64)p0[i] + ((f64)p1[i] - (f64)p0[i]) * s;
            }
            return;
        }
    }
    for (u32 i = 0; i < n; ++i)
        out[i] = v[i];
    if (rot)
        quat_normalize(out);
}

/* ---- Tracks ---------------------------------------------------------------------------------- */

typedef struct track {
    u32 joint, channel, src_comps;
    const de_model_anim_channel *src_ch;
    f64 *src; /* N x src_comps samples */
    f64 tol, tol0;
    bool force_raw;
    /* The encoding. */
    u32 enc, comps, flags, key_count;
    u8 *bytes; /* the track's data */
    u64 byte_count;
    /* Scratch, N entries each. */
    f64 *vals;  /* N x comps: the values to encode: src, or uvals for a uniform scale */
    f64 *uvals; /* N (scale tracks only): the uniform scale per sample */
    f32 *dec;   /* qdec or rdec: the decoded values key reduction works on */
    f32 *qdec;  /* N x 4: decoded quantized values per sample */
    f32 *rdec;  /* N x 4: the raw f32 values per sample */
    u16 *codes; /* N x 4: quantized codes (QUANT16: comps; SMALLEST3: 3 words) */
    /* What qdec/codes and rdec hold (encoding x 8 + components + 1; 0: nothing yet), the
     * QUANT16 range and the largest quantization error (see quantize). */
    u32 qkey, rkey;
    f32 qmin[3], qext[3];
    f64 qworst;
    u16 *keys;  /* kept sample indices (raw f32) */
    u16 *qkeys; /* kept sample indices (quantized) */
} track;

typedef struct clip_ctx {
    de_model_ir *ir;
    const de_model_skeleton *skel;
    const de_model_anim_settings *s;
    u32 n;          /* samples */
    f32 *times;     /* N */
    u8 *time_bytes; /* N x f32 little-endian, for the sampler's view */
    de_cooked_clip_view view;
    track *tracks;
    u32 track_count;
} clip_ctx;

/* Error of decoded value v (comps floats) against source sample src (src_comps). Translation:
 * distance (m); scale: largest component difference; rotation: angle (rad). */
static f64 track_err(const track *tk, const f32 *v, const f64 *src) {
    if (tk->channel == DE_COOKED_CLIP_CH_ROTATION) {
        f64 a[4], b[4], na = 0.0, nb = 0.0, d = 0.0;
        for (u32 i = 0; i < 4; ++i) {
            a[i] = v[i];
            b[i] = src[i];
            na += a[i] * a[i];
            nb += b[i] * b[i];
            d += a[i] * b[i];
        }
        if (!(na > 0.0) || !(nb > 0.0))
            return 4.0;
        quat_normalize(a);
        quat_normalize(b);
        if (d < 0.0)
            for (u32 i = 0; i < 4; ++i)
                b[i] = -b[i];
        return 2.0 * quat_half_angle(a, b);
    }
    if (tk->channel == DE_COOKED_CLIP_CH_TRANSLATION) {
        f64 e = 0.0;
        for (u32 i = 0; i < 3; ++i)
            e += ((f64)v[i] - src[i]) * ((f64)v[i] - src[i]);
        return sqrt64(e);
    }
    f64 e = 0.0;
    for (u32 i = 0; i < 3; ++i) {
        f64 d = absd((f64)v[tk->comps == 1u ? 0u : i] - src[i]);
        e = d > e ? d : e;
    }
    return e;
}

static void s3_pack(u32 big, const u32 code[3], u16 w[3], f32 dec[4]) {
    u64 bits = (u64)big << 45 | (u64)code[0] << 30 | (u64)code[1] << 15 | (u64)code[2];
    w[0] = (u16)(bits & 0xFFFFu);
    w[1] = (u16)((bits >> 16) & 0xFFFFu);
    w[2] = (u16)((bits >> 32) & 0xFFFFu);
    u8 b[6] = {(u8)w[0], (u8)(w[0] >> 8), (u8)w[1], (u8)(w[1] >> 8), (u8)w[2], (u8)(w[2] >> 8)};
    de_cooked_clip_s3_decode(b, dec);
}

/* Smallest-three code of quaternion q (three u16 words) and the value the sampler decodes. Each
 * component is rounded to the nearest step, then the 27 codes one step around are tried and the
 * one whose decoded rotation is closest to q is kept (the dropped component absorbs the rounding
 * of the others, so the nearest codes are not always the best). The candidates are decoded with
 * the decoder's own f32 arithmetic (de_cooked_clip_s3_decode), from the three values each kept
 * component can take; the winner is then packed and decoded by the decoder itself. */
void de_model_clip_s3_encode(const f64 q_in[4], u16 w[3], f32 dec[4]) {
    f64 q[4] = {q_in[0], q_in[1], q_in[2], q_in[3]};
    quat_normalize(q);
    u32 big = 0;
    for (u32 i = 1; i < 4; ++i)
        if (absd(q[i]) > absd(q[big]))
            big = i;
    f64 sign = q[big] < 0.0 ? -1.0 : 1.0;
    u32 code[3][3] = {{0}}, at[3] = {0, 1, 2};
    f32 cv[3][3] = {{0}}, sq[3][3] = {{0}};
    u32 k = 0;
    for (u32 i = 0; i < 4; ++i) {
        if (i == big)
            continue;
        f64 x = (sign * q[i] + (f64)DE_COOKED_CLIP_S3_RANGE) / (f64)DE_COOKED_CLIP_S3_STEP;
        i32 base = (i32)floor(x + 0.5);
        for (i32 d = 0; d < 3; ++d) {
            i32 v = base + d - 1;
            u32 c = v<0 ? 0u : v>(i32) DE_COOKED_CLIP_S3_MAX ? DE_COOKED_CLIP_S3_MAX : (u32)v;
            code[k][d] = c;
            cv[k][d] = (f32)c * DE_COOKED_CLIP_S3_STEP - DE_COOKED_CLIP_S3_RANGE;
            sq[k][d] = cv[k][d] * cv[k][d];
        }
        at[k++] = i;
    }
    /* The score is cos^2 of the half angle to q: dot^2 / |decoded|^2. The decoder's
     * rest = ((1 - c0^2) - c1^2) - c2^2 in f32, so its partial differences are shared by the
     * candidates of the outer loops. */
    f64 best = -1.0;
    u32 bd[3] = {1, 1, 1};
    for (u32 d0 = 0; d0 < 3; ++d0) {
        f32 r0 = 1.0f - sq[0][d0];
        f64 dot0 = q[at[0]] * (f64)cv[0][d0], n0 = (f64)cv[0][d0] * (f64)cv[0][d0];
        for (u32 d1 = 0; d1 < 3; ++d1) {
            f32 r1 = r0 - sq[1][d1];
            f64 dot1 = dot0 + q[at[1]] * (f64)cv[1][d1];
            f64 n1 = n0 + (f64)cv[1][d1] * (f64)cv[1][d1];
            for (u32 d2 = 0; d2 < 3; ++d2) {
                f32 rest = r1 - sq[2][d2];
                f64 bigv = rest > 0.0f ? (f64)sqrt32(rest) : 0.0;
                f64 dot = dot1 + q[at[2]] * (f64)cv[2][d2] + q[big] * bigv;
                f64 n = n1 + (f64)cv[2][d2] * (f64)cv[2][d2] + bigv * bigv;
                f64 score = n > 0.0 ? dot * dot / n : 0.0;
                if (score > best) {
                    best = score;
                    bd[0] = d0;
                    bd[1] = d1;
                    bd[2] = d2;
                }
            }
        }
    }
    const u32 pick[3] = {code[0][bd[0]], code[1][bd[1]], code[2][bd[2]]};
    s3_pack(big, pick, w, dec);
}

/* The decoded value at sample i of the segment between kept samples a and b (the sampler's
 * arithmetic, de_cooked_clip_track_sample). */
static void seg_value(const clip_ctx *cc, const track *tk, u32 a, u32 b, u32 i, f32 out[4]) {
    const f32 *va = tk->dec + 4u * (u64)a, *vb = tk->dec + 4u * (u64)b;
    f32 alpha = (cc->times[i] - cc->times[a]) / (cc->times[b] - cc->times[a]);
    if (tk->channel == DE_COOKED_CLIP_CH_ROTATION) {
        de_cooked_clip_nlerp(va, vb, alpha, out);
        return;
    }
    for (u32 c = 0; c < tk->comps; ++c)
        out[c] = va[c] + (vb[c] - va[c]) * alpha;
}

/* Whether the segment between kept samples a and b reproduces every sample in between within the
 * track's tolerance. The middle sample goes first: an overlong segment is usually furthest off
 * there, so a failing probe of segment_end usually stops after one evaluation. */
static bool seg_ok(const clip_ctx *cc, const track *tk, u32 a, u32 b) {
    if (b - a < 2u)
        return true;
    u32 mid = a + (b - a) / 2u;
    f32 v[4] = {0, 0, 0, 0};
    seg_value(cc, tk, a, b, mid, v);
    if (track_err(tk, v, tk->src + (u64)mid * tk->src_comps) > tk->tol)
        return false;
    for (u32 i = a + 1u; i < b; ++i) {
        if (i == mid)
            continue;
        seg_value(cc, tk, a, b, i, v);
        if (track_err(tk, v, tk->src + (u64)i * tk->src_comps) > tk->tol)
            return false;
    }
    return true;
}

/* The end of the segment that starts at kept sample a: a sample b in (a, limit] with seg_ok(a, b)
 * and, when b < limit, not seg_ok(a, b + 1). Galloping, then a binary search, so a segment of
 * length L costs O(L log L) evaluations instead of the O(L^2) of growing it one sample at a
 * time. The result is deterministic (it depends only on seg_ok). */
static u32 segment_end(const clip_ctx *cc, const track *tk, u32 a, u32 limit) {
    u32 good = a + 1u, bad = 0, step = 2u;
    while (good < limit) {
        u32 probe = limit - a > step ? a + step : limit;
        if (!seg_ok(cc, tk, a, probe)) {
            bad = probe;
            break;
        }
        good = probe;
        step *= 2u;
    }
    if (bad == 0)
        return good;
    while (bad - good > 1u) {
        u32 mid = good + (bad - good) / 2u;
        if (seg_ok(cc, tk, a, mid))
            good = mid;
        else
            bad = mid;
    }
    return good;
}

static void put_u16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put_u32(u8 *p, u32 v) {
    put_u16(p, (u16)v);
    put_u16(p + 2, (u16)(v >> 16));
}

static void put_f32(u8 *p, f32 f) {
    u32 b;
    memcpy(&b, &f, 4);
    put_u32(p, b);
}

/* Quantizes every sample of the track with encoding `enc` (QUANT16, SMALLEST3 or RAW) and points
 * tk->dec at the decoded values: tk->qdec (and tk->codes) for QUANT16 and SMALLEST3, tk->rdec for
 * RAW; QUANT16 also fills qmin and qext. Returns the largest error of a decoded sample against the
 * source (0 for RAW, which nothing compares). The values to encode depend only on the component
 * count (a uniform scale has one), so a result is kept and reused while the encoding and the
 * component count are the same: the passes of the model-space check do not quantize again. */
static f64 quantize(track *tk, u32 n, u32 enc, f32 qmin[3], f32 qext[3]) {
    u32 comps = tk->comps, sc = tk->src_comps;
    u32 key = enc * 8u + comps + 1u;
    if (enc == DE_COOKED_CLIP_ENC_RAW) {
        tk->dec = tk->rdec;
        if (tk->rkey == key)
            return 0.0;
        for (u32 i = 0; i < n; ++i) {
            if (tk->channel == DE_COOKED_CLIP_CH_ROTATION) {
                f64 q[4];
                memcpy(q, tk->vals + 4u * (u64)i, sizeof q);
                quat_normalize(q);
                for (u32 c = 0; c < 4; ++c)
                    tk->rdec[4u * (u64)i + c] = (f32)q[c];
            } else {
                for (u32 c = 0; c < comps; ++c)
                    tk->rdec[4u * (u64)i + c] = (f32)tk->vals[(u64)i * comps + c];
            }
        }
        tk->rkey = key;
        return 0.0;
    }
    tk->dec = tk->qdec;
    if (tk->qkey != key) {
        if (enc == DE_COOKED_CLIP_ENC_SMALLEST3) {
            for (u32 i = 0; i < n; ++i)
                de_model_clip_s3_encode(tk->vals + 4u * (u64)i, tk->codes + 4u * (u64)i,
                                        tk->qdec + 4u * (u64)i);
        } else {
            f32 step[3];
            for (u32 c = 0; c < comps; ++c) {
                f64 lo = tk->vals[c], hi = lo;
                for (u32 i = 1; i < n; ++i) {
                    f64 x = tk->vals[(u64)i * comps + c];
                    lo = x < lo ? x : lo;
                    hi = x > hi ? x : hi;
                }
                tk->qmin[c] = (f32)lo;
                f64 ext = hi - (f64)tk->qmin[c];
                tk->qext[c] = (f32)ext;
                if ((f64)tk->qext[c] < ext)
                    tk->qext[c] = nextafterf(tk->qext[c], HUGE_VALF);
                step[c] = tk->qext[c] * (1.0f / 65535.0f);
            }
            for (u32 i = 0; i < n; ++i)
                for (u32 c = 0; c < comps; ++c) {
                    f64 x = step[c] > 0.0f
                                ? (tk->vals[(u64)i * comps + c] - (f64)tk->qmin[c]) / (f64)step[c]
                                : 0.0;
                    f64 r = floor(x + 0.5);
                    u16 q = r <= 0.0                           ? (u16)0
                            : r >= (f64)DE_COOKED_CLIP_Q16_MAX ? (u16)DE_COOKED_CLIP_Q16_MAX
                                                               : (u16)r;
                    tk->codes[4u * (u64)i + c] = q;
                    tk->qdec[4u * (u64)i + c] = tk->qmin[c] + (f32)q * step[c];
                }
        }
        f64 worst = 0.0;
        for (u32 i = 0; i < n; ++i) {
            f64 e = track_err(tk, tk->qdec + 4u * (u64)i, tk->src + (u64)i * sc);
            worst = e > worst ? e : worst;
        }
        tk->qworst = worst;
        tk->qkey = key;
    }
    for (u32 c = 0; c < 3; ++c) {
        qmin[c] = tk->qmin[c];
        qext[c] = tk->qext[c];
    }
    return tk->qworst;
}

/* Key reduction into `keys` (greedy from the first sample; the last sample is always a key).
 * Returns the key count, or 0 when the track's bytes in encoding tk->enc would reach `give_up`
 * (0: no limit) before the reduction ends: that encoding then loses against one of `give_up`
 * bytes whatever the remaining keys are, so they are not searched. */
static u32 reduce_keys(const clip_ctx *cc, const track *tk, u16 *keys, u64 give_up) {
    u32 n = cc->n, kc = 0;
    keys[kc++] = 0;
    if (tk->force_raw) {
        for (u32 i = 1; i < n; ++i)
            keys[kc++] = (u16)i;
        return kc;
    }
    u32 a = 0;
    while (a < n - 1u) {
        if (give_up != 0 && de_cooked_clip_track_bytes(tk->enc, tk->comps, kc + 1u) >= give_up)
            return 0;
        u32 limit = n - 1u - a > MAX_SEGMENT ? a + MAX_SEGMENT : n - 1u;
        u32 b = segment_end(cc, tk, a, limit);
        keys[kc++] = (u16)b;
        a = b;
    }
    return kc;
}

/* The bytes of the track in encoding tk->enc (decoded values tk->dec) with the `kc` keys of
 * `keys`, into tk->bytes. */
static void write_track(track *tk, const u16 *keys, u32 kc, const f32 qmin[3], const f32 qext[3]) {
    u32 comps = tk->comps;
    u8 *p = tk->bytes;
    if (tk->enc == DE_COOKED_CLIP_ENC_QUANT16) {
        for (u32 c = 0; c < comps; ++c) {
            put_f32(p + 4u * c, qmin[c]);
            put_f32(p + 4u * (comps + c), qext[c]);
        }
        p += 8u * comps;
    }
    u64 ib = de_cooked_clip_index_bytes(kc);
    memset(p, 0, (size_t)ib);
    for (u32 k = 0; k < kc; ++k)
        put_u16(p + 2u * k, keys[k]);
    p += ib;
    for (u32 k = 0; k < kc; ++k) {
        u64 i = keys[k];
        switch (tk->enc) {
        case DE_COOKED_CLIP_ENC_QUANT16:
            for (u32 c = 0; c < comps; ++c)
                put_u16(p + 2u * ((u64)k * comps + c), tk->codes[4u * i + c]);
            break;
        case DE_COOKED_CLIP_ENC_SMALLEST3:
            for (u32 w = 0; w < 3; ++w)
                put_u16(p + 6u * (u64)k + 2u * w, tk->codes[4u * i + w]);
            break;
        default:
            for (u32 c = 0; c < comps; ++c)
                put_f32(p + 4u * ((u64)k * comps + c), tk->dec[4u * i + c]);
            break;
        }
    }
}

/* Encodes one track at its current tolerance into tk->bytes. */
static void encode_track(clip_ctx *cc, track *tk) {
    u32 n = cc->n;
    u32 sc = tk->src_comps;
    bool rot = tk->channel == DE_COOKED_CLIP_CH_ROTATION;
    bool scale = tk->channel == DE_COOKED_CLIP_CH_SCALE;
    tk->flags &= ~DE_COOKED_CLIP_TRACK_UNIFORM;
    tk->comps = sc;

    /* Constant: one value within the tolerance of every sample. */
    f32 cv[4] = {0, 0, 0, 0};
    bool is_const = n == 1u;
    if (!is_const && !tk->force_raw) {
        if (rot) {
            for (u32 c = 0; c < 4; ++c)
                cv[c] = (f32)tk->src[c];
        } else {
            for (u32 c = 0; c < sc; ++c) {
                f64 lo = tk->src[c], hi = lo;
                for (u32 i = 1; i < n; ++i) {
                    f64 x = tk->src[(u64)i * sc + c];
                    lo = x < lo ? x : lo;
                    hi = x > hi ? x : hi;
                }
                cv[c] = (f32)(0.5 * (lo + hi));
            }
        }
        is_const = true;
        for (u32 i = 0; i < n && is_const; ++i)
            is_const = track_err(tk, cv, tk->src + (u64)i * sc) <= tk->tol;
    } else if (is_const) {
        for (u32 c = 0; c < sc; ++c)
            cv[c] = (f32)tk->src[c];
    }
    if (is_const) {
        if (rot) {
            f64 q[4] = {cv[0], cv[1], cv[2], cv[3]};
            quat_normalize(q);
            for (u32 c = 0; c < 4; ++c)
                cv[c] = (f32)q[c];
        }
        if (scale) {
            /* One value for x, y and z, if it is within the tolerance too. */
            f32 m = (f32)(((f64)cv[0] + (f64)cv[1] + (f64)cv[2]) / 3.0);
            f32 mv[4] = {m, m, m, 0};
            tk->comps = 1;
            bool uni = true;
            for (u32 i = 0; i < n && uni; ++i)
                uni = track_err(tk, mv, tk->src + (u64)i * sc) <= tk->tol;
            if (uni) {
                tk->flags |= DE_COOKED_CLIP_TRACK_UNIFORM;
                cv[0] = m;
            } else {
                tk->comps = 3;
            }
        }
        tk->enc = DE_COOKED_CLIP_ENC_CONST;
        tk->key_count = 1;
        tk->byte_count = 4u * tk->comps;
        for (u32 c = 0; c < tk->comps; ++c)
            put_f32(tk->bytes + 4u * c, cv[c]);
        return;
    }

    /* Uniform scale: one component. */
    if (scale && !tk->force_raw) {
        bool uni = true;
        for (u32 i = 0; i < n && uni; ++i) {
            const f64 *s = tk->src + (u64)i * 3u;
            f64 m = (s[0] + s[1] + s[2]) / 3.0;
            for (u32 c = 0; c < 3; ++c)
                uni = uni && absd(s[c] - m) <= 0.25 * tk->tol;
        }
        if (uni) {
            tk->comps = 1;
            tk->flags |= DE_COOKED_CLIP_TRACK_UNIFORM;
        }
    }
    u32 comps = tk->comps;
    tk->vals = tk->src;
    if (comps == 1u && sc == 3u) {
        tk->vals = tk->uvals;
        for (u32 i = 0; i < n; ++i) {
            const f64 *s = tk->src + (u64)i * 3u;
            tk->vals[i] = (s[0] + s[1] + s[2]) / 3.0;
        }
    }

    /* The quantized encoding, when its rounding leaves room for key reduction, and raw f32: the
     * smaller is kept (quantized on a tie). Raw is reduced only until it is no longer smaller. */
    f32 qmin[3] = {0, 0, 0}, qext[3] = {0, 0, 0};
    u32 qenc = rot ? DE_COOKED_CLIP_ENC_SMALLEST3 : DE_COOKED_CLIP_ENC_QUANT16;
    bool try_q = !tk->force_raw && quantize(tk, n, qenc, qmin, qext) <= QUANT_SHARE * tk->tol;
    u64 qbytes = 0;
    u32 qkeys = 0;
    if (try_q) {
        tk->enc = qenc;
        qkeys = reduce_keys(cc, tk, tk->qkeys, 0);
        qbytes = de_cooked_clip_track_bytes(qenc, comps, qkeys);
    }
    (void)quantize(tk, n, DE_COOKED_CLIP_ENC_RAW, qmin, qext);
    tk->enc = DE_COOKED_CLIP_ENC_RAW;
    u32 rkeys = reduce_keys(cc, tk, tk->keys, qbytes);
    if (rkeys != 0) {
        write_track(tk, tk->keys, rkeys, qmin, qext);
        tk->key_count = rkeys;
        tk->byte_count = de_cooked_clip_track_bytes(DE_COOKED_CLIP_ENC_RAW, comps, rkeys);
        return;
    }
    /* The quantized encoding wins: its codes and decoded values are still there (quantize). */
    (void)quantize(tk, n, qenc, qmin, qext);
    tk->enc = qenc;
    write_track(tk, tk->qkeys, qkeys, qmin, qext);
    tk->key_count = qkeys;
    tk->byte_count = qbytes;
}

/* The sampler's view of one encoded track. */
static de_cooked_clip_track track_view(const track *tk) {
    de_cooked_clip_track t;
    memset(&t, 0, sizeof t);
    t.joint = tk->joint;
    t.channel = tk->channel;
    t.encoding = tk->enc;
    t.components = tk->comps;
    t.flags = tk->flags;
    t.key_count = tk->key_count;
    de_cooked_clip_track_bind(&t, tk->bytes);
    return t;
}

/* ---- Model-space check ----------------------------------------------------------------------- */

static void pose_affine(const de_cooked_joint_pose *p, f64 out[12]) {
    f64 t[3], q[4], s[3];
    for (u32 k = 0; k < 3; ++k) {
        t[k] = p->t[k];
        s[k] = p->s[k];
    }
    for (u32 k = 0; k < 4; ++k)
        q[k] = p->r[k];
    de_model_affine_from_trs(t, q, s, out);
}

/* Largest angle (rad, as a chord) between the normalised columns of two linear parts. */
static f64 orient_err(const f64 a[12], const f64 b[12]) {
    f64 e = 0.0;
    for (u32 c = 0; c < 3; ++c) {
        f64 la = 0.0, lb = 0.0, d = 0.0;
        for (u32 r = 0; r < 3; ++r) {
            la += a[c * 3 + r] * a[c * 3 + r];
            lb += b[c * 3 + r] * b[c * 3 + r];
        }
        if (!(la > 0.0) || !(lb > 0.0))
            continue;
        la = sqrt64(la);
        lb = sqrt64(lb);
        for (u32 r = 0; r < 3; ++r) {
            f64 x = a[c * 3 + r] / la - b[c * 3 + r] / lb;
            d += x * x;
        }
        d = sqrt64(d);
        e = d > e ? d : e;
    }
    return e;
}

/* Scratch of the model-space check, allocated once per clip. */
typedef struct check_scratch {
    bool *fail;
    f64 *excess;   /* per joint: largest error over its limit (position or orientation ratio) */
    f64 *scale;    /* per joint: the factor its tracks' tolerances get (1: unchanged) */
    i32 *track_of; /* per joint x 3 channels: track index, -1 */
    de_cooked_clip_track *views; /* per track */
    de_cooked_joint_pose *dec;   /* per joint */
    f64 *mdec, *msrc;            /* per joint: 12 */
} check_scratch;

/* Decodes every sample with the sampler's helpers, composes the decoded and the source poses to
 * model space, and marks the joints whose error is too large. Returns their number. */
static u32 check_model_space(clip_ctx *cc, const f64 *rest, check_scratch *x,
                             de_model_clip_stats *st) {
    const de_model_skeleton *sk = cc->skel;
    u32 jc = sk->joint_count, failing = 0;
    f64 tol_p = CHECK_MARGIN * (f64)cc->s->error, tol_r = CHECK_MARGIN * (f64)cc->s->error_angle;
    memset(x->fail, 0, sizeof(bool) * jc);
    for (u32 j = 0; j < jc; ++j)
        x->excess[j] = 0.0;
    st->max_position = 0.0;
    st->max_angle = 0.0;
    for (u32 k = 0; k < cc->track_count; ++k)
        x->views[k] = track_view(&cc->tracks[k]);
    for (u32 i = 0; i < cc->n; ++i) {
        for (u32 j = 0; j < jc; ++j) {
            const f64 *r = rest + 10u * (u64)j;
            for (u32 c = 0; c < 3; ++c) {
                x->dec[j].t[c] = (f32)r[c];
                x->dec[j].s[c] = (f32)r[7 + c];
            }
            for (u32 c = 0; c < 4; ++c)
                x->dec[j].r[c] = (f32)r[3 + c];
        }
        for (u32 k = 0; k < cc->track_count; ++k) {
            f32 v[4] = {0, 0, 0, 0};
            de_cooked_clip_track_sample(&cc->view, &x->views[k], cc->times[i], v);
            de_cooked_clip_apply(&x->views[k], v, &x->dec[cc->tracks[k].joint]);
        }
        for (u32 j = 0; j < jc; ++j) {
            f64 ld[12], ls[12];
            pose_affine(&x->dec[j], ld);
            /* The source local transform: the rest pose, with the tracks' f64 samples. */
            f64 v[10];
            memcpy(v, rest + 10u * (u64)j, sizeof v);
            for (u32 ch = 0; ch < 3; ++ch) {
                i32 k = x->track_of[3u * j + ch];
                if (k < 0)
                    continue;
                const track *tk = &cc->tracks[k];
                const f64 *src = tk->src + (u64)i * tk->src_comps;
                memcpy(v + (ch == 0 ? 0 : ch == 1 ? 3 : 7), src, sizeof(f64) * tk->src_comps);
            }
            de_model_affine_from_trs(v, v + 3, v + 7, ls);
            u16 p = sk->parents[j];
            f64 *a = x->mdec + 12u * (u64)j, *b = x->msrc + 12u * (u64)j;
            if (p != DE_COOKED_SKEL_NO_PARENT) {
                de_model_affine_mul(x->mdec + 12u * p, ld, a);
                de_model_affine_mul(x->msrc + 12u * p, ls, b);
            } else {
                memcpy(a, ld, sizeof ld);
                memcpy(b, ls, sizeof ls);
            }
            f64 dp = sqrt64((a[9] - b[9]) * (a[9] - b[9]) + (a[10] - b[10]) * (a[10] - b[10]) +
                            (a[11] - b[11]) * (a[11] - b[11]));
            f64 dr = orient_err(a, b);
            st->max_position = dp > st->max_position ? dp : st->max_position;
            st->max_angle = dr > st->max_angle ? dr : st->max_angle;
            f64 ex = dp / tol_p > dr / tol_r ? dp / tol_p : dr / tol_r;
            x->excess[j] = ex > x->excess[j] ? ex : x->excess[j];
            if ((dp > tol_p || dr > tol_r) && !x->fail[j]) {
                x->fail[j] = true;
                ++failing;
            }
        }
    }
    return failing;
}

/* ---- Time table ------------------------------------------------------------------------------ */

typedef struct time_cand {
    f64 t;
    u32 key; /* 1 for a source key time */
} time_cand;

static int cand_cmp(const void *pa, const void *pb) {
    const time_cand *a = (const time_cand *)pa, *b = (const time_cand *)pb;
    if (a->t != b->t)
        return a->t < b->t ? -1 : 1;
    return a->key < b->key ? -1 : a->key > b->key ? 1 : 0;
}

static de_result clip_too_long(const de_model_anim *anim, f64 duration,
                               const de_model_anim_settings *s) {
    return DE_FAIL(DE_ERR_UNSUPPORTED,
                   "animation \"%s\": %.3f s at %g Hz with its source key times needs more than "
                   "%u samples; lower `anim_sample_rate` or split the clip",
                   anim->name, duration, (f64)s->sample_rate, DE_COOKED_CLIP_MAX_TIMES);
}

/* ---- Entry points ---------------------------------------------------------------------------- */

static u32 channel_of_path(de_model_anim_path p) {
    return p == DE_MODEL_ANIM_TRANSLATION ? DE_COOKED_CLIP_CH_TRANSLATION
           : p == DE_MODEL_ANIM_ROTATION  ? DE_COOKED_CLIP_CH_ROTATION
                                          : DE_COOKED_CLIP_CH_SCALE;
}

i32 de_model_anim_pick_skeleton(const de_model_ir *ir, const de_model_anim *anim,
                                const de_model_skeleton *skels, u32 skel_count) {
    i32 best = -1;
    u32 best_n = 0;
    for (u32 k = 0; k < skel_count; ++k) {
        u32 n = 0;
        for (u32 c = 0; c < anim->channel_count; ++c) {
            const de_model_anim_channel *ch = &anim->channels[c];
            if (ch->path != DE_MODEL_ANIM_WEIGHTS && ch->node < ir->node_count &&
                skels[k].joint_of_node[ch->node] >= 0 && ch->key_count != 0)
                ++n;
        }
        if (n > best_n) {
            best_n = n;
            best = (i32)k;
        }
    }
    return best;
}

de_result de_model_clip_write(de_model_buf *b, de_model_ir *ir, const de_model_anim *anim,
                              const de_model_skeleton *skel, u64 skeleton_id,
                              const de_model_anim_settings *s, de_model_clip_stats *stats) {
    de_model_clip_stats st_local;
    de_model_clip_stats *st = stats != NULL ? stats : &st_local;
    memset(st, 0, sizeof *st);
    u32 jc = skel->joint_count;
    clip_ctx cc;
    memset(&cc, 0, sizeof cc);
    cc.ir = ir;
    cc.skel = skel;
    cc.s = s;

    /* 1. The source channel of each (joint, channel). */
    const de_model_anim_channel **by =
        (const de_model_anim_channel **)de_model_ir_array(ir, (u64)jc * 3u, sizeof(void *));
    if (by == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    u32 weights = 0, outside = 0, dups = 0;
    f64 duration = 0.0;
    u64 key_total = 0;
    for (u32 c = 0; c < anim->channel_count; ++c) {
        const de_model_anim_channel *ch = &anim->channels[c];
        if (ch->path == DE_MODEL_ANIM_WEIGHTS) {
            ++weights;
            continue;
        }
        if (ch->key_count == 0)
            continue;
        if (ch->node >= ir->node_count || skel->joint_of_node[ch->node] < 0) {
            ++outside;
            continue;
        }
        u32 slot = (u32)skel->joint_of_node[ch->node] * 3u + channel_of_path(ch->path);
        if (by[slot] != NULL) {
            ++dups;
            continue;
        }
        for (u32 k = 0; k < ch->key_count; ++k) {
            f32 t = ch->times[k];
            if (!(t >= 0.0f) || !(t < 1e7f) || (k > 0 && !(t >= ch->times[k - 1u])))
                return DE_FAIL(DE_ERR_PARSE,
                               "animation \"%s\": key times must be finite, not negative and "
                               "increasing",
                               anim->name);
        }
        for (u64 v = 0; v < (u64)ch->key_count * ch->components *
                                (ch->interp == DE_MODEL_INTERP_CUBIC ? 3u : 1u);
             ++v)
            if (!isfinite(ch->values[v]))
                return DE_FAIL(DE_ERR_PARSE, "animation \"%s\": a key value is not finite",
                               anim->name);
        by[slot] = ch;
        key_total += ch->key_count;
        if ((f64)ch->times[ch->key_count - 1u] > duration)
            duration = ch->times[ch->key_count - 1u];
    }
    if (weights != 0)
        de_model_ir_warn(ir, "animation \"%s\": %u morph weight channel(s) not cooked", anim->name,
                         weights);
    if (outside != 0)
        de_model_ir_warn(ir,
                         "animation \"%s\": %u channel(s) on nodes outside skeleton \"%s\" not "
                         "cooked",
                         anim->name, outside, ir->skins[skel->skin].name);
    if (dups != 0)
        de_model_ir_warn(ir, "animation \"%s\": %u duplicate channel(s) ignored", anim->name, dups);

    /* 2. The time table. */
    /* The grid alone must fit; the merged table is checked once the shared key times are
     * merged (channels keyed at the same times add one sample, not one per channel). */
    f64 grid_f = duration * (f64)s->sample_rate;
    if (!(grid_f < (f64)DE_COOKED_CLIP_MAX_TIMES))
        return clip_too_long(anim, duration, s);
    u32 grid = duration > 0.0 ? (u32)floor(grid_f + 0.5) : 0u;
    if (duration > 0.0 && grid == 0)
        grid = 1;
    u64 ncand = (u64)grid + 1u + key_total;
    time_cand *cand = (time_cand *)de_model_ir_array(ir, ncand, sizeof(time_cand));
    if (cand == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    u64 nc = 0;
    for (u32 k = 0; k <= grid; ++k) {
        cand[nc].t = grid == 0 ? 0.0 : (k == grid ? duration : duration * (f64)k / (f64)grid);
        cand[nc++].key = 0;
    }
    for (u32 slot = 0; slot < jc * 3u; ++slot)
        if (by[slot] != NULL)
            for (u32 k = 0; k < by[slot]->key_count; ++k) {
                cand[nc].t = by[slot]->times[k];
                cand[nc++].key = 1;
            }
    qsort(cand, (size_t)nc, sizeof *cand, cand_cmp);
    cc.times = (f32 *)de_model_ir_array(ir, nc, sizeof(f32));
    u8 *is_key = (u8 *)de_model_ir_array(ir, nc, 1);
    if (cc.times == NULL || is_key == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    u32 n = 0;
    for (u64 i = 0; i < nc; ++i) {
        f32 t = (f32)cand[i].t;
        if (n == 0) {
            cc.times[n] = 0.0f;
            is_key[n++] = (u8)(cand[i].key && t == 0.0f);
            continue;
        }
        f64 eps = TIME_EPS_ABS + TIME_EPS_REL * (f64)t;
        if ((f64)t > (f64)cc.times[n - 1u] + eps) {
            cc.times[n] = t;
            is_key[n++] = (u8)cand[i].key;
        } else if (cand[i].key && !is_key[n - 1u] && n >= 2u &&
                   (f64)t > (f64)cc.times[n - 2u] + eps) {
            cc.times[n - 1u] = t; /* the key time replaces the grid time */
            is_key[n - 1u] = 1;
        }
    }
    /* The last time is the duration (a grid point closer to it than eps gave way). */
    if (n > 1u && cc.times[n - 1u] != (f32)duration) {
        if ((f64)duration > (f64)cc.times[n - 1u])
            cc.times[n++] = (f32)duration;
        else
            cc.times[n - 1u] = (f32)duration;
    }
    if (n > DE_COOKED_CLIP_MAX_TIMES)
        return clip_too_long(anim, duration, s);
    cc.n = n;
    cc.time_bytes = (u8 *)de_model_ir_array(ir, (u64)n * 4u, 1);
    if (cc.time_bytes == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 i = 0; i < n; ++i)
        put_f32(cc.time_bytes + 4u * i, cc.times[i]);
    cc.view.times = cc.time_bytes;
    cc.view.time_count = n;
    cc.view.duration = cc.times[n - 1u];
    cc.view.joint_count = jc;

    /* 3. Tracks and their samples. */
    cc.tracks = (track *)de_model_ir_array(ir, (u64)jc * 3u + 1u, sizeof(track));
    f64 *rest = (f64 *)de_model_ir_array(ir, (u64)jc * 10u, sizeof(f64));
    if (cc.tracks == NULL || rest == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 j = 0; j < jc; ++j) {
        const de_model_node *nd = &ir->nodes[skel->nodes[j]];
        for (u32 c = 0; c < 3; ++c) {
            rest[10u * j + c] = nd->translation[c];
            rest[10u * j + 7u + c] = nd->scale[c];
        }
        for (u32 c = 0; c < 4; ++c)
            rest[10u * j + 3u + c] = nd->rotation[c];
    }
    for (u32 slot = 0; slot < jc * 3u; ++slot) {
        const de_model_anim_channel *ch = by[slot];
        if (ch == NULL)
            continue;
        track *tk = &cc.tracks[cc.track_count++];
        tk->joint = slot / 3u;
        tk->channel = slot % 3u;
        tk->src_ch = ch;
        tk->src_comps = tk->channel == DE_COOKED_CLIP_CH_ROTATION ? 4u : 3u;
        tk->flags = tk->joint == skel->root_joint ? DE_COOKED_CLIP_TRACK_ROOT : 0u;
        tk->src = (f64 *)de_model_ir_array(ir, (u64)n * tk->src_comps, sizeof(f64));
        tk->vals = tk->src;
        if (tk->channel == DE_COOKED_CLIP_CH_SCALE) {
            tk->uvals = (f64 *)de_model_ir_array(ir, n, sizeof(f64));
            if (tk->uvals == NULL)
                return DE_ERR_OUT_OF_MEMORY;
        }
        tk->qdec = (f32 *)de_model_ir_array(ir, (u64)n * 4u, sizeof(f32));
        tk->rdec = (f32 *)de_model_ir_array(ir, (u64)n * 4u, sizeof(f32));
        tk->dec = tk->qdec;
        tk->codes = (u16 *)de_model_ir_array(ir, (u64)n * 4u, sizeof(u16));
        tk->keys = (u16 *)de_model_ir_array(ir, n, sizeof(u16));
        tk->qkeys = (u16 *)de_model_ir_array(ir, n, sizeof(u16));
        tk->bytes = (u8 *)de_model_ir_array(
            ir, 24u + de_cooked_clip_index_bytes(n) + 16u * (u64)n + 16u, 1);
        if (tk->src == NULL || tk->qdec == NULL || tk->rdec == NULL || tk->codes == NULL ||
            tk->keys == NULL || tk->qkeys == NULL || tk->bytes == NULL)
            return DE_ERR_OUT_OF_MEMORY;
        chan_cache cache;
        memset(&cache, 0, sizeof cache);
        for (u32 i = 0; i < n; ++i) {
            f64 v[4] = {0, 0, 0, 1};
            chan_eval(ch, (f64)cc.times[i], v, &cache);
            f64 *dst = tk->src + (u64)i * tk->src_comps;
            for (u32 c = 0; c < tk->src_comps; ++c)
                dst[c] = v[c];
            /* Hemisphere continuity: the same rotation, on the side of the previous sample. */
            if (tk->src_comps == 4u && i > 0) {
                const f64 *pr = dst - 4;
                if (pr[0] * dst[0] + pr[1] * dst[1] + pr[2] * dst[2] + pr[3] * dst[3] < 0.0)
                    for (u32 c = 0; c < 4; ++c)
                        dst[c] = -dst[c];
            }
        }
        /* The initial local tolerance. */
        u16 p = skel->parents[tk->joint];
        f64 pscale = 1.0;
        if (p != DE_COOKED_SKEL_NO_PARENT) {
            const f64 *m = skel->rest_model + 12u * p;
            pscale = 0.0;
            for (u32 c = 0; c < 3; ++c) {
                f64 l = sqrt64(m[c * 3] * m[c * 3] + m[c * 3 + 1] * m[c * 3 + 1] +
                               m[c * 3 + 2] * m[c * 3 + 2]);
                pscale = l > pscale ? l : pscale;
            }
            if (!(pscale > 1e-12))
                pscale = 1.0;
        }
        f64 reach = skel->reach[tk->joint];
        f64 ang = (f64)s->error_angle;
        if (reach > 0.0 && (f64)s->error / reach < ang)
            ang = (f64)s->error / reach;
        tk->tol = tk->channel == DE_COOKED_CLIP_CH_TRANSLATION ? (f64)s->error / pscale : ang;
        tk->tol0 = tk->tol;
    }

    /* 4 and 5. Encode, check in model space, tighten what fails. */
    check_scratch x;
    x.fail = (bool *)de_model_ir_array(ir, jc, sizeof(bool));
    x.excess = (f64 *)de_model_ir_array(ir, jc, sizeof(f64));
    x.scale = (f64 *)de_model_ir_array(ir, jc, sizeof(f64));
    x.track_of = (i32 *)de_model_ir_array(ir, (u64)jc * 3u, sizeof(i32));
    x.views = (de_cooked_clip_track *)de_model_ir_array(ir, cc.track_count + 1u,
                                                        sizeof(de_cooked_clip_track));
    x.dec = (de_cooked_joint_pose *)de_model_ir_array(ir, jc, sizeof(de_cooked_joint_pose));
    x.mdec = (f64 *)de_model_ir_array(ir, (u64)jc * 12u, sizeof(f64));
    x.msrc = (f64 *)de_model_ir_array(ir, (u64)jc * 12u, sizeof(f64));
    if (x.fail == NULL || x.excess == NULL || x.scale == NULL || x.track_of == NULL ||
        x.views == NULL || x.dec == NULL || x.mdec == NULL || x.msrc == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 i = 0; i < jc * 3u; ++i)
        x.track_of[i] = -1;
    for (u32 k = 0; k < cc.track_count; ++k)
        x.track_of[3u * cc.tracks[k].joint + cc.tracks[k].channel] = (i32)k;
    for (u32 k = 0; k < cc.track_count; ++k)
        encode_track(&cc, &cc.tracks[k]);
    u32 failing = 0;
    for (u32 pass = 1; pass <= CHECK_PASSES; ++pass) {
        st->iterations = pass;
        failing = check_model_space(&cc, rest, &x, st);
        if (failing == 0 || pass == CHECK_PASSES)
            break;
        /* A failing joint halves the tolerances of its chain; once per doubling of its excess
         * past 2, so a chain far over the limit gets there in one or two passes instead of
         * one pass per halving. An ancestor takes the smallest factor of its failing
         * descendants (the factors are powers of two: exact and deterministic). */
        for (u32 j = 0; j < jc; ++j)
            x.scale[j] = 1.0;
        for (u32 j = 0; j < jc; ++j) {
            if (!x.fail[j])
                continue;
            f64 f = 0.5;
            for (f64 e = x.excess[j]; e > 2.0 && f > MIN_TIGHTEN; e *= 0.5)
                f *= 0.5;
            for (u32 a = j; a != DE_COOKED_SKEL_NO_PARENT && x.scale[a] > f; a = skel->parents[a])
                x.scale[a] = f;
        }
        for (u32 k = 0; k < cc.track_count; ++k) {
            track *tk = &cc.tracks[k];
            if (!(x.scale[tk->joint] < 1.0))
                continue;
            tk->tol *= x.scale[tk->joint];
            tk->force_raw = pass >= RAW_AFTER;
            encode_track(&cc, tk);
        }
    }
    if (failing != 0)
        de_model_ir_warn(ir,
                         "animation \"%s\": %u joint(s) stay above anim_error (largest error "
                         "%.3g m, %.3g rad)",
                         anim->name, failing, st->max_position, st->max_angle);

    /* Loop hint: every track ends where it starts. */
    bool loop = n > 1u;
    for (u32 k = 0; k < cc.track_count && loop; ++k) {
        const track *tk = &cc.tracks[k];
        f32 first[4] = {0, 0, 0, 0};
        for (u32 c = 0; c < tk->src_comps; ++c)
            first[c] = (f32)tk->src[c];
        track tmp = *tk;
        tmp.comps = tk->src_comps;
        loop = track_err(&tmp, first, tk->src + (u64)(n - 1u) * tk->src_comps) <= tk->tol0;
    }

    /* The blob. */
    u64 start = b->size;
    bool any_root = false;
    for (u32 k = 0; k < cc.track_count; ++k)
        any_root = any_root || (cc.tracks[k].flags & DE_COOKED_CLIP_TRACK_ROOT) != 0;
    de_model_buf_put(b, DE_COOKED_CLIP_MAGIC, 4);
    de_model_buf_u32(b, DE_COOKED_CLIP_VERSION);
    de_model_buf_u32(b, loop ? DE_COOKED_CLIP_LOOP : 0u);
    de_model_buf_u32(b, jc);
    de_model_buf_u32(b, cc.track_count);
    de_model_buf_u32(b, n);
    de_model_buf_u32(b, any_root ? skel->root_joint : DE_COOKED_NONE);
    de_model_buf_f32(b, s->sample_rate);
    de_model_buf_f32(b, cc.times[n - 1u]);
    u64 at_offsets = b->size;
    de_model_buf_put(b, NULL, 20); /* 3 offsets, data bytes, total: patched below */
    de_model_buf_u64(b, skeleton_id);
    de_model_buf_u64(b, 0);
    de_model_buf_u64(b, 0);
    u64 trk = b->size - start;
    u64 data_off = 0;
    for (u32 k = 0; k < cc.track_count; ++k) {
        const track *tk = &cc.tracks[k];
        u8 r[DE_COOKED_CLIP_TRACK_SIZE];
        memset(r, 0, sizeof r);
        put_u16(r, (u16)tk->joint);
        r[2] = (u8)tk->channel;
        r[3] = (u8)tk->enc;
        r[4] = (u8)tk->comps;
        r[5] = (u8)tk->flags;
        put_u32(r + 8, tk->key_count);
        put_u32(r + 12, (u32)data_off);
        de_model_buf_put(b, r, sizeof r);
        data_off += (tk->byte_count + 3u) & ~(u64)3u;
    }
    u64 tim = b->size - start;
    de_model_buf_put(b, cc.time_bytes, (u64)n * 4u);
    u64 dat = b->size - start;
    for (u32 k = 0; k < cc.track_count; ++k) {
        const track *tk = &cc.tracks[k];
        de_model_buf_put(b, tk->bytes, tk->byte_count);
        de_model_buf_align(b, 4);
        ++st->tracks;
        if (tk->enc == DE_COOKED_CLIP_ENC_CONST)
            ++st->const_tracks;
        else
            st->keys += tk->key_count;
        if (tk->flags & DE_COOKED_CLIP_TRACK_UNIFORM)
            ++st->uniform_tracks;
        if (tk->enc == DE_COOKED_CLIP_ENC_RAW)
            ++st->raw_tracks;
    }
    de_model_buf_patch_u32(b, at_offsets, (u32)trk);
    de_model_buf_patch_u32(b, at_offsets + 4, (u32)tim);
    de_model_buf_patch_u32(b, at_offsets + 8, (u32)dat);
    de_model_buf_patch_u32(b, at_offsets + 12, (u32)(b->size - start - dat));
    de_model_buf_patch_u32(b, at_offsets + 16, (u32)(b->size - start));
    st->times = n;
    st->bytes = b->size - start;
    return b->failed ? DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for a clip") : DE_OK;
}
