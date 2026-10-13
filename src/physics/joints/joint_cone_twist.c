/*
 * joint_cone_twist.c: DE_JOINT_CONE_TWIST (DENG-220), for ragdoll shoulders and hips. Rows: the
 * twist motor and limit (axis 3) along the twist Jacobian (twist_axis), the swing cone, then the
 * point block.
 *
 * Cone. The swing of B's frame in A's frame (swing-twist split) is a rotation by angle theta about
 * the unit direction (0, uy, uz) of A's YZ plane. The elliptical cone with half-angles ay =
 * limits_max[4] (about Y) and az = limits_max[5] (about Z) allows theta_max = 1 / sqrt((uy / ay)^2
 * + (uz / az)^2) in that direction. One upper-limit row about the swing axis keeps theta <=
 * theta_max (the ellipse's radial direction, not its exact normal). With no swing the direction
 * is undefined and the row rests. See joint_internal.h.
 */
#include "physics/joints/joint_internal.h"

/* The swing axis (world) and the cone error theta_max - theta; false when there is no swing. */
static bool cone(const de__jx *x, const de__joint *j, de_vec3 *axis, f32 *C, f32 *twist) {
    f32 sy, sz, swing;
    de__joint_swing_twist(de__joint_relative(x), twist, &sy, &sz, &swing);
    if (!(swing > 1.0e-5f))
        return false;
    const f32 uy = sy / swing, uz = sz / swing;
    const f32 ay = de__maxf(j->hi[4], 1.0e-3f), az = de__maxf(j->hi[5], 1.0e-3f);
    const f32 ey = uy / ay, ez = uz / az;
    const f32 max = 1.0f / de_dsqrt(ey * ey + ez * ez);
    *axis = de__quat_rotate_f(x->qa, de__v3(0.0f, uy, uz));
    *C = max - swing;
    return true;
}

/* The twist row's Jacobian (DENG-398). With q = swing * twist, the twist angle changes at
 * w_rel . (xa + xb) / (1 + xa . xb), where xa, xb are the X axes of frames A and B: turning B about
 * its own X axis, and circling it around A's X axis, both twist it at the full rate. The axis xb
 * alone (DENG-220) missed the second, so a limb circling its cone pressed into the twist limit
 * along a direction that also pushes the circling: the limits pumped energy into it. */
static de_vec3 twist_axis(const de__jx *x) {
    const de_vec3 xa = de__jq_axis(x->qa, 0), xb = de__jq_axis(x->qb, 0);
    const f32 c = 1.0f + de__v3_dot(xa, xb);
    if (!(c > 1.0e-3f))
        return xb; /* swung by almost pi: no twist rate; keep DENG-220's axis */
    return de__v3_scale(de__v3_add(xa, xb), 1.0f / c);
}

static void cone_warm_start(de__jx *x, de__joint *j) {
    const de__jrow t = de__jrow_angular(x, twist_axis(x));
    de__jrow_apply(x, &t, j->motor[3] + j->lower[3] - j->upper[3]);
    de_vec3 axis;
    f32 C, twist;
    if (cone(x, j, &axis, &C, &twist)) {
        const de__jrow r = de__jrow_angular(x, axis);
        de__jrow_apply(x, &r, -j->upper[4]);
    }
    de__jpoint_apply(x, j->lin);
}

static void cone_solve(de__jx *x, de__joint *j) {
    if (!de__jx_axis_rows(x, j, 3)) {
        de__jpoint_solve(x, &j->lin); /* extra pass without a twist motor: the point block only */
        return;
    }
    de_vec3 axis;
    f32 C, twist;
    const bool has_swing = cone(x, j, &axis, &C, &twist);
    const de__jrow t = de__jrow_angular(x, twist_axis(x));
    de__jrow_motor(x, j, 3, &t, twist, &j->motor[3]);
    de__jrow_limits(x, &t, twist, j->lo[3], j->hi[3], &j->lower[3], &j->upper[3]);
    if (has_swing) {
        const de__jrow r = de__jrow_neg(de__jrow_angular(x, axis));
        de__jrow_limit(x, &r, C, &j->upper[4]);
    } else {
        j->upper[4] = 0.0f;
    }
    de__jpoint_solve(x, &j->lin);
}

const de__joint_ops de__joint_cone_twist_ops = {cone_warm_start, cone_solve,
                                                de__joint_point_impulse};
