/* joint_test_util.h: scene helpers shared by the joint tests (DENG-220). Test-only. */
#ifndef DE_JOINT_TEST_UTIL_H
#define DE_JOINT_TEST_UTIL_H

#include "solver_test_util.h"

#include "physics/islands.h"
#include "physics/joints/joint_internal.h"

#define JT_PI 3.14159265358979f

/** A dynamic body with gravity scale 1 and `mass` (0 = from density). */
static inline de_body jt_dyn(de_physics_world *p, de_shape s, de_transform t, f32 mass) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = s;
    d.transform = t;
    d.mass = mass;
    d.gravity_scale = 1.0f;
    return de_body_create(p, &d);
}

/** A joint desc of `kind` between a and b (invalid = world) with frame positions fa, fb and the
 *  identity rotations. */
static inline de_joint_desc jt_desc(de_joint_kind kind, de_body a, de_body b, de_dvec3 fa,
                                    de_dvec3 fb) {
    de_joint_desc d;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    d.body_a = a;
    d.body_b = b;
    d.frame_a.position = fa;
    d.frame_a.rotation = st_quat_identity();
    d.frame_b.position = fb;
    d.frame_b.rotation = st_quat_identity();
    return d;
}

/** World position of a body-local point (f64). */
static inline de_dvec3 jt_world_point(const de_physics_world *p, de_body b, de_dvec3 local) {
    de_transform t;
    if (!de_body_get_transform(p, b, &t))
        return local;
    const de__dmat3 m = de__dmat3_from_quat(t.rotation);
    return de__d3_add(t.position, de__dmat3_mul(&m, local));
}

static inline f64 jt_dist(de_dvec3 a, de_dvec3 b) {
    return st_len(de__d3_sub(a, b));
}

/** Quaternion product (f32). */
static inline de_quat jt_qmul(de_quat a, de_quat b) {
    return de__jq_mul(a, b);
}

/** Twist, swing y / z of rotation b relative to rotation a, with frame rotations fa, fb. */
static inline void jt_angles(de_quat a, de_quat fa, de_quat b, de_quat fb, f32 *twist, f32 *sy,
                             f32 *sz) {
    const de_quat qa = jt_qmul(a, fa), qb = jt_qmul(b, fb);
    de_quat q = jt_qmul(de__jq_conj(qa), qb);
    if (q.w < 0.0f)
        q = de__jq(-q.x, -q.y, -q.z, -q.w);
    f32 swing;
    de__joint_swing_twist(q, twist, sy, sz, &swing);
}

/** Angle (rad) between two rotations. */
static inline f32 jt_rot_error(de_quat a, de_quat b) {
    const de_quat q = jt_qmul(de__jq_conj(a), b);
    const f32 v = de_dsqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    return 2.0f * de_datan2(v, q.w < 0.0f ? -q.w : q.w);
}

#endif /* DE_JOINT_TEST_UTIL_H */
