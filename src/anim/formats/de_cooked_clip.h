/*
 * de_cooked_clip.h: the cooked animation clip format DCLP, version 1 (SPEC §11.6, §21.5;
 * DENG-104). Internal; include as "anim/formats/de_cooked_clip.h".
 *
 * The bytes of an `anim_clip` asset, a sub-asset `<model>#anim:<name>` of a model. The model
 * importer (tools/cook/importers/model/anim_clip_compress.c) writes it, and clip_decode.c is the
 * reference sampler (S19 playback and the S27 animator build on it). A clip animates the joints
 * of one skeleton (de_cooked_skeleton.h), named by asset ID in the header. Values are local joint
 * transforms in engine coordinates.
 *
 * Time: a table of sample times, strictly increasing from 0 to the duration. It holds the source's
 * key times and a uniform grid at the importer's `anim_sample_rate`. Each track keeps a subset of
 * the table (key reduction) and refers to its keys by table index. Between two keys the value is
 * interpolated linearly (rotations: normalised lerp on the shorter arc). Outside [0, duration] the
 * clip holds its end values; looping is the caller's job.
 *
 * Tracks: one per (joint, channel) that the source animates, sorted by joint, then channel. A
 * joint or channel without a track keeps the skeleton's rest pose. Encodings:
 *   - CONST: one value (comps x f32).
 *   - QUANT16 (translation, scale): comps x f32 min, comps x f32 extent, then per key comps x u16;
 *     value = min + q * (extent / 65535).
 *   - RAW (any channel): per key comps x f32.
 *   - SMALLEST3 (rotation): per key 48 bits, written as three little-endian u16 words w0 w1 w2 of
 *     the value b = w0 | w1 << 16 | w2 << 32: bits 45..46 the index of the largest component
 *     (x, y, z, w), bits 30..44, 15..29 and 0..14 the other three in order, each 15 bits over
 *     [-1/sqrt2, 1/sqrt2]. The largest component is positive and is rebuilt as
 *     sqrt(1 - a^2 - b^2 - c^2). Its sign may differ between neighbouring keys; the sampler takes
 *     the shorter arc, so a sign flip never turns into a long-way interpolation.
 * Non-constant tracks start with u16 key time indices (strictly increasing, the first 0 and the
 * last time_count - 1), padded to 4 bytes, then the values. Scale tracks whose three components
 * are equal have one component (DE_COOKED_CLIP_TRACK_UNIFORM). Tracks of the skeleton's root joint
 * (root motion, S27) carry DE_COOKED_CLIP_TRACK_ROOT.
 *
 * Every field is little-endian (de_canim_get_* in de_cooked_skeleton.h). Layout:
 *
 *   header      DE_COOKED_CLIP_HEADER_SIZE (80) bytes:
 *                 0  u8[4] magic "DCLP"
 *                 4  u32   format version (DE_COOKED_CLIP_VERSION)
 *                 8  u32   flags (DE_COOKED_CLIP_LOOP: the first and last poses match)
 *                12  u32   joint count of the skeleton
 *                16  u32   track count T
 *                20  u32   time count N (>= 1)
 *                24  u32   root joint (the skeleton's), DE_COOKED_NONE if no track is flagged
 *                28  f32   sample rate of the cook (Hz)
 *                32  f32   duration (s), the last time
 *                36  u32   track table offset (T x 16 bytes)
 *                40  u32   time table offset (N x f32)
 *                44  u32   data offset
 *                48  u32   data bytes
 *                52  u32   total size of the blob
 *                56  u64   skeleton asset ID (`<model>#skeleton:<name>`)
 *                64  u64   reserved, 0
 *                72  u64   reserved, 0
 *   tracks      T x 16: u16 joint, u8 channel (DE_COOKED_CLIP_CH_*), u8 encoding
 *               (DE_COOKED_CLIP_ENC_*), u8 components (1 or 3 for T/S, 4 for R), u8 flags, u16 0,
 *               u32 key count K (1 for CONST), u32 offset into the data (4-byte aligned).
 *   times       N x f32.
 *   data        per track, in track order, each 4-byte aligned.
 *
 * Bump DE_COOKED_CLIP_VERSION on any change.
 *
 * Determinism: the sampling helpers below use only + - * / and de_dsqrt (no <math.h>), so the
 * deterministic animator (S27) can reuse them with de_dmath. The cooker evaluates its error with
 * the same helpers.
 */
#ifndef DE_ANIM_FORMATS_COOKED_CLIP_H
#define DE_ANIM_FORMATS_COOKED_CLIP_H

#include "anim/formats/de_cooked_skeleton.h"

#include <dengine/de_base.h>
#include <dengine/de_math.h>

#include <string.h>

DE_EXTERN_C_BEGIN

#define DE_COOKED_CLIP_MAGIC       "DCLP"
#define DE_COOKED_CLIP_VERSION     1u
#define DE_COOKED_CLIP_HEADER_SIZE 80u
#define DE_COOKED_CLIP_TRACK_SIZE  16u
/** Time indices are u16. */
#define DE_COOKED_CLIP_MAX_TIMES 0xFFFFu

/** Header flag: the first and last poses are equal within the cook's error, so the clip loops. */
#define DE_COOKED_CLIP_LOOP 1u

/** Track channels. */
enum {
    DE_COOKED_CLIP_CH_TRANSLATION = 0,
    DE_COOKED_CLIP_CH_ROTATION = 1,
    DE_COOKED_CLIP_CH_SCALE = 2,
    DE_COOKED_CLIP_CH_COUNT = 3
};

/** Track encodings. */
enum {
    DE_COOKED_CLIP_ENC_CONST = 0,
    DE_COOKED_CLIP_ENC_QUANT16 = 1,
    DE_COOKED_CLIP_ENC_RAW = 2,
    DE_COOKED_CLIP_ENC_SMALLEST3 = 3,
    DE_COOKED_CLIP_ENC_COUNT = 4
};

/** Track flags. */
#define DE_COOKED_CLIP_TRACK_ROOT    1u /**< a track of the skeleton's root joint (root motion) */
#define DE_COOKED_CLIP_TRACK_UNIFORM 2u /**< scale with one component for x, y and z */

/** Smallest-three range and step: components lie in [-1/sqrt2, 1/sqrt2], 15 bits each. */
#define DE_COOKED_CLIP_S3_RANGE 0.70710678f
#define DE_COOKED_CLIP_S3_STEP  4.3159689e-5f /* 2 / sqrt2 / 32767, rounded to f32 */
#define DE_COOKED_CLIP_S3_MAX   32767u
#define DE_COOKED_CLIP_Q16_MAX  65535u

/** One decoded track record (pointers into the blob). */
typedef struct de_cooked_clip_track {
    u32 joint, channel, encoding, components, flags, key_count;
    const u8 *times; /**< K x u16 time indices (NULL for CONST) */
    const u8 *values;
    f32 min[3], step[3]; /**< QUANT16 only */
} de_cooked_clip_track;

/** A checked view of a DCLP blob (pointers into it). */
typedef struct de_cooked_clip_view {
    const u8 *base;
    u64 size;
    u32 flags, joint_count, track_count, time_count, root_joint;
    f32 sample_rate, duration;
    u64 skeleton_id;
    const u8 *tracks, *times, *data;
    u32 data_size;
} de_cooked_clip_view;

/** Bytes of the u16 time indices of a K-key track, padded to 4. */
static inline u64 de_cooked_clip_index_bytes(u64 k) {
    return (k * 2u + 3u) & ~(u64)3u;
}

/** Bytes of the data of a track (without padding after the values). */
static inline u64 de_cooked_clip_track_bytes(u32 encoding, u32 comps, u64 k) {
    switch (encoding) {
    case DE_COOKED_CLIP_ENC_CONST:
        return 4u * comps;
    case DE_COOKED_CLIP_ENC_QUANT16:
        return 8u * comps + de_cooked_clip_index_bytes(k) + 2u * comps * k;
    case DE_COOKED_CLIP_ENC_RAW:
        return de_cooked_clip_index_bytes(k) + 4u * comps * k;
    case DE_COOKED_CLIP_ENC_SMALLEST3:
        return de_cooked_clip_index_bytes(k) + 6u * k;
    default:
        return 0;
    }
}

/** Points `t` (whose channel, encoding, components and key_count are set) at its data `d`. */
static inline void de_cooked_clip_track_bind(de_cooked_clip_track *t, const u8 *d) {
    t->times = NULL;
    if (t->encoding == DE_COOKED_CLIP_ENC_CONST) {
        t->values = d;
        return;
    }
    if (t->encoding == DE_COOKED_CLIP_ENC_QUANT16) {
        for (u32 c = 0; c < t->components; ++c) {
            t->min[c] = de_canim_get_f32(d + 4u * c);
            t->step[c] = de_canim_get_f32(d + 4u * (t->components + c)) * (1.0f / 65535.0f);
        }
        d += 8u * t->components;
    }
    t->times = d;
    t->values = d + de_cooked_clip_index_bytes(t->key_count);
}

/** Track `i` (< track_count) of a parsed clip. */
static inline de_cooked_clip_track de_cooked_clip_get_track(const de_cooked_clip_view *v, u32 i) {
    const u8 *r = v->tracks + (u64)i * DE_COOKED_CLIP_TRACK_SIZE;
    de_cooked_clip_track t;
    memset(&t, 0, sizeof t);
    t.joint = de_canim_get_u16(r);
    t.channel = r[2];
    t.encoding = r[3];
    t.components = r[4];
    t.flags = r[5];
    t.key_count = de_canim_get_u32(r + 8);
    de_cooked_clip_track_bind(&t, v->data + de_canim_get_u32(r + 12));
    return t;
}

/**
 * Checks a DCLP blob and fills `out`: magic, version, sizes, offsets, the time table (from 0,
 * strictly increasing, the last equal to the duration), every track record (joint, channel,
 * encoding and component count valid, sorted by joint then channel without duplicates, data in
 * range, key time indices strictly increasing from 0 to N - 1). Allocation-free. False on any
 * error, with a short reason in `*why` (nullable).
 */
static inline bool de_cooked_clip_parse(const void *data, u64 size, de_cooked_clip_view *out,
                                        const char **why) {
    const char *w = NULL;
    const u8 *p = (const u8 *)data;
    memset(out, 0, sizeof *out);
    if (p == NULL || size < DE_COOKED_CLIP_HEADER_SIZE) {
        w = "shorter than the header";
        goto fail;
    }
    if (memcmp(p, DE_COOKED_CLIP_MAGIC, 4) != 0) {
        w = "not a cooked clip (magic)";
        goto fail;
    }
    if (de_canim_get_u32(p + 4) != DE_COOKED_CLIP_VERSION) {
        w = "unsupported format version";
        goto fail;
    }
    out->base = p;
    out->size = size;
    out->flags = de_canim_get_u32(p + 8);
    out->joint_count = de_canim_get_u32(p + 12);
    out->track_count = de_canim_get_u32(p + 16);
    out->time_count = de_canim_get_u32(p + 20);
    out->root_joint = de_canim_get_u32(p + 24);
    out->sample_rate = de_canim_get_f32(p + 28);
    out->duration = de_canim_get_f32(p + 32);
    u32 trk = de_canim_get_u32(p + 36), tim = de_canim_get_u32(p + 40);
    u32 dat = de_canim_get_u32(p + 44);
    out->data_size = de_canim_get_u32(p + 48);
    u32 total = de_canim_get_u32(p + 52);
    out->skeleton_id = de_canim_get_u64(p + 56);
    if (total != size || out->joint_count > DE_COOKED_SKEL_MAX_JOINTS || out->time_count == 0 ||
        out->time_count > DE_COOKED_CLIP_MAX_TIMES ||
        out->track_count > out->joint_count * DE_COOKED_CLIP_CH_COUNT ||
        (out->root_joint != DE_COOKED_NONE && out->root_joint >= out->joint_count) ||
        !de_canim_range_ok(size, trk, (u64)out->track_count * DE_COOKED_CLIP_TRACK_SIZE) ||
        !de_canim_range_ok(size, tim, (u64)out->time_count * 4u) ||
        !de_canim_range_ok(size, dat, out->data_size) || trk < DE_COOKED_CLIP_HEADER_SIZE ||
        (tim & 3u) != 0 || (dat & 3u) != 0 || !(out->duration >= 0.0f) ||
        !(out->sample_rate > 0.0f)) {
        w = "bad sizes or offsets";
        goto fail;
    }
    out->tracks = p + trk;
    out->times = p + tim;
    out->data = p + dat;
    f32 prev = de_canim_get_f32(out->times);
    if (prev != 0.0f) {
        w = "the time table does not start at 0";
        goto fail;
    }
    for (u32 i = 1; i < out->time_count; ++i) {
        f32 t = de_canim_get_f32(out->times + 4u * i);
        if (!(t > prev)) {
            w = "the time table is not strictly increasing";
            goto fail;
        }
        prev = t;
    }
    if (prev != out->duration) {
        w = "the last time is not the duration";
        goto fail;
    }
    u32 last_key = 0;
    for (u32 i = 0; i < out->track_count; ++i) {
        const u8 *r = out->tracks + (u64)i * DE_COOKED_CLIP_TRACK_SIZE;
        u32 joint = de_canim_get_u16(r), ch = r[2], enc = r[3], comps = r[4];
        u32 k = de_canim_get_u32(r + 8), off = de_canim_get_u32(r + 12);
        u32 key = joint * DE_COOKED_CLIP_CH_COUNT + ch + 1u;
        bool comps_ok = ch == DE_COOKED_CLIP_CH_ROTATION
                            ? comps == 4u
                            : (comps == 3u || (comps == 1u && ch == DE_COOKED_CLIP_CH_SCALE));
        bool enc_ok = enc == DE_COOKED_CLIP_ENC_CONST || enc == DE_COOKED_CLIP_ENC_RAW ||
                      (enc == DE_COOKED_CLIP_ENC_QUANT16 && ch != DE_COOKED_CLIP_CH_ROTATION) ||
                      (enc == DE_COOKED_CLIP_ENC_SMALLEST3 && ch == DE_COOKED_CLIP_CH_ROTATION);
        bool k_ok = enc == DE_COOKED_CLIP_ENC_CONST ? k == 1u : (k >= 2u && k <= out->time_count);
        if (joint >= out->joint_count || ch >= DE_COOKED_CLIP_CH_COUNT || !comps_ok || !enc_ok ||
            !k_ok || key <= last_key || (off & 3u) != 0 ||
            !de_canim_range_ok(out->data_size, off, de_cooked_clip_track_bytes(enc, comps, k))) {
            w = "bad track record";
            goto fail;
        }
        last_key = key;
        if (enc == DE_COOKED_CLIP_ENC_CONST)
            continue;
        de_cooked_clip_track t = de_cooked_clip_get_track(out, i);
        u32 pi = de_canim_get_u16(t.times);
        bool ok = pi == 0;
        for (u32 j = 1; j < k && ok; ++j) {
            u32 ti = de_canim_get_u16(t.times + 2u * j);
            ok = ti > pi;
            pi = ti;
        }
        if (!ok || pi != out->time_count - 1u) {
            w = "bad key times";
            goto fail;
        }
    }
    return true;
fail:
    memset(out, 0, sizeof *out);
    if (why != NULL)
        *why = w;
    return false;
}

/** Time `i` (< time_count) of the table. */
static inline f32 de_cooked_clip_time(const de_cooked_clip_view *v, u32 i) {
    return de_canim_get_f32(v->times + 4u * i);
}

/* ---- Decoding primitives (shared by the sampler and the cooker's error check) ---------------- */

/** Decodes one smallest-three rotation from its 6 bytes into q (x, y, z, w). */
static inline void de_cooked_clip_s3_decode(const u8 *p, f32 q[4]) {
    u64 b = (u64)de_canim_get_u16(p) | ((u64)de_canim_get_u16(p + 2) << 16) |
            ((u64)de_canim_get_u16(p + 4) << 32);
    u32 largest = (u32)(b >> 45) & 3u;
    f32 c[3];
    c[0] = (f32)(u32)((b >> 30) & 0x7FFFu) * DE_COOKED_CLIP_S3_STEP - DE_COOKED_CLIP_S3_RANGE;
    c[1] = (f32)(u32)((b >> 15) & 0x7FFFu) * DE_COOKED_CLIP_S3_STEP - DE_COOKED_CLIP_S3_RANGE;
    c[2] = (f32)(u32)(b & 0x7FFFu) * DE_COOKED_CLIP_S3_STEP - DE_COOKED_CLIP_S3_RANGE;
    f32 rest = 1.0f - c[0] * c[0] - c[1] * c[1] - c[2] * c[2];
    f32 big = rest > 0.0f ? de_dsqrt(rest) : 0.0f;
    u32 k = 0;
    for (u32 i = 0; i < 4; ++i)
        q[i] = i == largest ? big : c[k++];
}

/** Value of key `k` of a non-constant track into `out` (components floats). */
static inline void de_cooked_clip_key_value(const de_cooked_clip_track *t, u32 k, f32 out[4]) {
    switch (t->encoding) {
    case DE_COOKED_CLIP_ENC_QUANT16:
        for (u32 c = 0; c < t->components; ++c)
            out[c] =
                t->min[c] +
                (f32)de_canim_get_u16(t->values + 2u * ((u64)k * t->components + c)) * t->step[c];
        break;
    case DE_COOKED_CLIP_ENC_RAW:
        for (u32 c = 0; c < t->components; ++c)
            out[c] = de_canim_get_f32(t->values + 4u * ((u64)k * t->components + c));
        break;
    case DE_COOKED_CLIP_ENC_SMALLEST3:
        de_cooked_clip_s3_decode(t->values + 6u * (u64)k, out);
        break;
    default: /* CONST */
        for (u32 c = 0; c < t->components; ++c)
            out[c] = de_canim_get_f32(t->values + 4u * c);
        break;
    }
}

/** Normalised lerp of two unit quaternions on the shorter arc. */
static inline void de_cooked_clip_nlerp(const f32 a[4], const f32 b[4], f32 alpha, f32 out[4]) {
    f32 d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    f32 sign = d < 0.0f ? -1.0f : 1.0f;
    f32 r[4];
    for (u32 i = 0; i < 4; ++i)
        r[i] = a[i] + (sign * b[i] - a[i]) * alpha;
    f32 len2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3];
    if (!(len2 > 0.0f)) {
        for (u32 i = 0; i < 4; ++i)
            out[i] = a[i];
        return;
    }
    f32 inv = 1.0f / de_dsqrt(len2);
    for (u32 i = 0; i < 4; ++i)
        out[i] = r[i] * inv;
}

/**
 * Samples one track at time `t` (seconds; clamped to [0, duration]) into `out`: `components`
 * floats (rotation x, y, z, w; a uniform scale gives one float). Binary search over the track's
 * key times, then lerp, or nlerp for rotations.
 */
static inline void de_cooked_clip_track_sample(const de_cooked_clip_view *v,
                                               const de_cooked_clip_track *t, f32 time,
                                               f32 out[4]) {
    if (t->encoding == DE_COOKED_CLIP_ENC_CONST) {
        de_cooked_clip_key_value(t, 0, out);
        return;
    }
    u32 lo = 0, hi = t->key_count - 1u;
    f32 t_lo = de_cooked_clip_time(v, de_canim_get_u16(t->times));
    f32 t_hi = de_cooked_clip_time(v, de_canim_get_u16(t->times + 2u * hi));
    if (!(time > t_lo)) {
        de_cooked_clip_key_value(t, 0, out);
        return;
    }
    if (!(time < t_hi)) {
        de_cooked_clip_key_value(t, hi, out);
        return;
    }
    /* Invariant: time(lo) < time < time(hi). */
    while (hi - lo > 1u) {
        u32 mid = lo + (hi - lo) / 2u;
        f32 tm = de_cooked_clip_time(v, de_canim_get_u16(t->times + 2u * mid));
        if (tm <= time) {
            lo = mid;
            t_lo = tm;
        } else {
            hi = mid;
            t_hi = tm;
        }
    }
    f32 a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0};
    de_cooked_clip_key_value(t, lo, a);
    if (time == t_lo) {
        for (u32 c = 0; c < t->components; ++c)
            out[c] = a[c];
        return;
    }
    de_cooked_clip_key_value(t, hi, b);
    f32 alpha = (time - t_lo) / (t_hi - t_lo);
    if (t->channel == DE_COOKED_CLIP_CH_ROTATION) {
        de_cooked_clip_nlerp(a, b, alpha, out);
        return;
    }
    for (u32 c = 0; c < t->components; ++c)
        out[c] = a[c] + (b[c] - a[c]) * alpha;
}

/** Writes a sampled track value into a joint pose (uniform scale fills x, y and z). */
static inline void de_cooked_clip_apply(const de_cooked_clip_track *t, const f32 v[4],
                                        de_cooked_joint_pose *pose) {
    switch (t->channel) {
    case DE_COOKED_CLIP_CH_TRANSLATION:
        for (u32 c = 0; c < 3; ++c)
            pose->t[c] = v[c];
        break;
    case DE_COOKED_CLIP_CH_ROTATION:
        for (u32 c = 0; c < 4; ++c)
            pose->r[c] = v[c];
        break;
    default:
        for (u32 c = 0; c < 3; ++c)
            pose->s[c] = t->components == 1u ? v[0] : v[c];
        break;
    }
}

/* ---- The reference sampler (clip_decode.c, in de_anim) --------------------------------------- */

/**
 * Samples `clip` at `time` (seconds, clamped to [0, duration]) into `out`, an array of
 * skel->joint_count local joint poses: every joint gets the skeleton's rest pose, then each track
 * overwrites its channel. Returns false (and writes nothing) when the clip does not fit the
 * skeleton (joint counts differ). Allocation-free; uses no <math.h> function.
 */
bool de__anim_clip_sample(const de_cooked_clip_view *clip, const de_cooked_skeleton_view *skel,
                          f32 time, de_cooked_joint_pose *out);

DE_EXTERN_C_END

#endif /* DE_ANIM_FORMATS_COOKED_CLIP_H */
