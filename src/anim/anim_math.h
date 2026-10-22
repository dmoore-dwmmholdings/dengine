#ifndef DE_ANIM_MATH_H
#define DE_ANIM_MATH_H
#include "anim/pose.h"

/* Fast arithmetic stays in its own visual TU. Deterministic callers of these
 * inline recipes must compile with FP contraction disabled and initialize the
 * thread FP environment. The fixed runtime always selects the deterministic
 * branch; visual adapters remain separate from its call graph. */
de_vec3 de__anim_fast_lerp(de_vec3, de_vec3, f32);
de_quat de__anim_fast_normalize(de_quat);
de_quat de__anim_fast_nlerp(de_quat, de_quat, f32);
de_quat de__anim_fast_slerp(de_quat, de_quat, f32);

static inline f32 de__anim_lerp_scalar(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}
static inline de_vec3 de__anim_lerp(de_vec3 a, de_vec3 b, f32 t, bool deterministic) {
    if (!deterministic)
        return de__anim_fast_lerp(a, b, t);
    return (de_vec3){de__anim_lerp_scalar(a.x, b.x, t), de__anim_lerp_scalar(a.y, b.y, t),
                     de__anim_lerp_scalar(a.z, b.z, t)};
}
static inline de_quat de__anim_normalize(de_quat q, bool deterministic) {
    return deterministic ? de_dnormalize_quat(q) : de__anim_fast_normalize(q);
}
static inline de_quat de__anim_nlerp(de_quat a, de_quat b, f32 t, bool deterministic) {
    if (!deterministic)
        return de__anim_fast_nlerp(a, b, t);
    /* Keep the S11 cooked sampler's exact f32 recipe, including its degenerate
     * fallback. General normalize uses the more accurate de_dnormalize_quat. */
    f32 av[4] = {a.x, a.y, a.z, a.w}, bv[4] = {b.x, b.y, b.z, b.w}, r[4];
    f32 dot = av[0] * bv[0] + av[1] * bv[1] + av[2] * bv[2] + av[3] * bv[3];
    f32 sign = dot < 0 ? -1.f : 1.f;
    for (u32 i = 0; i < 4; ++i)
        r[i] = de__anim_lerp_scalar(av[i], sign * bv[i], t);
    f32 length2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3];
    if (!(length2 > 0))
        return a;
    f32 inv = 1.f / de_dsqrt(length2);
    return (de_quat){r[0] * inv, r[1] * inv, r[2] * inv, r[3] * inv};
}
/* Interpolation expects finite unit quaternions. Like de_math, slerp follows the
 * short arc and preserves endpoints (b may be negated to the same rotation). */
static inline de_quat de__anim_slerp(de_quat a, de_quat b, f32 t, bool deterministic) {
    if (!deterministic)
        return de__anim_fast_slerp(a, b, t);
    f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0) {
        b = (de_quat){-b.x, -b.y, -b.z, -b.w};
        dot = -dot;
    }
    if (t == 0)
        return a;
    if (t == 1)
        return b;
    if (dot > .9995f)
        return de__anim_nlerp(a, b, t, true);
    f32 theta = de_dacos(dot);
    f32 inv_sin = 1.f / de_dsin(theta);
    f32 wa = de_dsin((1.f - t) * theta) * inv_sin, wb = de_dsin(t * theta) * inv_sin;
    return (de_quat){wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z,
                     wa * a.w + wb * b.w};
}
static inline bool de__anim_pose_valid(const de__anim_local_pose *p) {
    return p && (!p->count || (p->rotations && p->translations && p->scales));
}
static inline de_quat de__anim_quat_product(de_quat a, de_quat b) {
    return (de_quat){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static inline de_vec3 de__anim_difference(de_vec3 value, de_vec3 reference) {
    return (de_vec3){value.x - reference.x, value.y - reference.y, value.z - reference.z};
}
static inline de_vec3 de__anim_add_delta(de_vec3 base, de_vec3 delta, f32 weight) {
    if (weight == 0)
        return base;
    return (de_vec3){base.x + delta.x * weight, base.y + delta.y * weight,
                     base.z + delta.z * weight};
}
static inline de_quat de__anim_rotation_delta(de_quat value, de_quat reference,
                                              bool deterministic) {
    de_quat inverse = {-reference.x, -reference.y, -reference.z, reference.w};
    return de__anim_normalize(de__anim_quat_product(inverse, value), deterministic);
}
static inline de_quat de__anim_add_rotation(de_quat base, de_quat delta, f32 weight,
                                            bool deterministic) {
    if (weight == 0)
        return base;
    de_quat scaled = de__anim_slerp((de_quat){0, 0, 0, 1}, delta, weight, deterministic);
    return de__anim_normalize(de__anim_quat_product(base, scaled), deterministic);
}
/* Exact in-place aliasing with either input is supported; partial overlapping
 * channel arrays are not. Invalid shapes/weights write nothing. Endpoints copy
 * exactly; interior weights interpolate root delta as one additional bone. */
static inline bool de__anim_pose_blend(de__anim_local_pose *out, const de__anim_local_pose *a,
                                       const de__anim_local_pose *b, f32 t, bool deterministic) {
    if (!de__anim_pose_valid(out) || !de__anim_pose_valid(a) || !de__anim_pose_valid(b) ||
        out->count != a->count || out->count != b->count || !(t >= 0 && t <= 1))
        return false;
    const de__anim_local_pose *endpoint = t == 0 ? a : t == 1 ? b : NULL;
    for (u32 j = 0; j < out->count; ++j) {
        if (endpoint) {
            out->rotations[j] = endpoint->rotations[j];
            out->translations[j] = endpoint->translations[j];
            out->scales[j] = endpoint->scales[j];
        } else {
            out->rotations[j] = de__anim_nlerp(a->rotations[j], b->rotations[j], t, deterministic);
            out->translations[j] =
                de__anim_lerp(a->translations[j], b->translations[j], t, deterministic);
            out->scales[j] = de__anim_lerp(a->scales[j], b->scales[j], t, deterministic);
        }
    }
    if (endpoint) {
        out->root_translation = endpoint->root_translation;
        out->root_rotation = endpoint->root_rotation;
    } else {
        out->root_translation =
            de__anim_lerp(a->root_translation, b->root_translation, t, deterministic);
        out->root_rotation = de__anim_nlerp(a->root_rotation, b->root_rotation, t, deterministic);
    }
    return true;
}
#endif
