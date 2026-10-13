/*
 * joint_distance.c: DE_JOINT_DISTANCE (DENG-220). One direction, the line between the anchors:
 * the motor (axis 0), then the range [lo, hi] as a lower and an upper limit row (a spring when
 * hertz > 0, rigid otherwise). Rotation is free. Anchors closer than 1e-6 m have no direction and
 * are skipped. See joint_internal.h.
 */
#include "physics/joints/joint_internal.h"

static bool direction(const de__jx *x, de_vec3 *u, f32 *len) {
    const f32 l2 = de__v3_dot(x->d, x->d);
    if (!(l2 > 1.0e-12f))
        return false;
    *len = de_dsqrt(l2);
    *u = de__v3_scale(x->d, 1.0f / *len);
    return true;
}

static void distance_warm_start(de__jx *x, de__joint *j) {
    de_vec3 u;
    f32 len;
    if (!direction(x, &u, &len))
        return;
    const de__jrow r = de__jrow_point(x, u);
    de__jrow_apply(x, &r, j->motor[0] + j->lower[0] - j->upper[0]);
}

static void distance_solve(de__jx *x, de__joint *j) {
    de_vec3 u;
    f32 len;
    if (!direction(x, &u, &len))
        return;
    const de__jrow r = de__jrow_point(x, u);
    de__jrow_motor(x, j, 0, &r, len, &j->motor[0]);
    de__jrow_limits(x, &r, len, j->lo[0], j->hi[0], &j->lower[0], &j->upper[0]);
}

static de_vec3 distance_impulse(const de__jx *x, const de__joint *j) {
    de_vec3 u;
    f32 len;
    if (!direction(x, &u, &len))
        return de__v3(0.0f, 0.0f, 0.0f);
    return de__v3_scale(u, j->motor[0] + j->lower[0] - j->upper[0]);
}

const de__joint_ops de__joint_distance_ops = {distance_warm_start, distance_solve,
                                              distance_impulse};
