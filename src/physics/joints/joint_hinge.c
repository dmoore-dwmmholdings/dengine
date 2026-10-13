/*
 * joint_hinge.c: DE_JOINT_HINGE (DENG-220). Rows: the twist motor and limit about A's X axis
 * (axis 0), the two-row block that keeps B's X axis on A's X axis, then the point block. The angle
 * is the twist of B's frame in A's frame (swing-twist split, de_datan2). See joint_internal.h.
 */
#include "physics/joints/joint_internal.h"

static f32 hinge_angle(const de__jx *x) {
    f32 twist, sy, sz, swing;
    de__joint_swing_twist(de__joint_relative(x), &twist, &sy, &sz, &swing);
    return twist;
}

/* The two angular rows perpendicular to the hinge axis: impulse l0 p + l1 q. */
static void axes_apply(de__jx *x, de_vec3 p, de_vec3 q, f32 l0, f32 l1) {
    de__jlock_apply(x, de__v3_add(de__v3_scale(p, l0), de__v3_scale(q, l1)));
}

static void axes_solve(de__jx *x, de__joint *j) {
    const de_vec3 a1 = de__jq_axis(x->qa, 0), a2 = de__jq_axis(x->qb, 0);
    const de_vec3 p = de__jq_axis(x->qa, 1), q = de__jq_axis(x->qa, 2);
    const de_vec3 e = de__v3_cross(a1, a2);
    const f32 c0 = de__v3_dot(e, p), c1 = de__v3_dot(e, q);
    const de_vec3 w = de__v3_sub(x->wb, x->wa);
    const f32 d0 = de__v3_dot(w, p), d1 = de__v3_dot(w, q);
    const de_vec3 ip = de__v3_add(de__m3_mul(x->ia, p), de__m3_mul(x->ib, p));
    const de_vec3 iq = de__v3_add(de__m3_mul(x->ia, q), de__m3_mul(x->ib, q));
    const f32 k00 = de__v3_dot(p, ip), k01 = de__v3_dot(p, iq), k11 = de__v3_dot(q, iq);
    const de__soft s = x->soft;
    const f32 b0 = d0 + s.bias_rate * c0, b1 = d1 + s.bias_rate * c1;
    f32 x0, x1;
    const f32 det = k00 * k11 - k01 * k01;
    if (k00 > 0.0f && k11 > 0.0f && det > 1.0e-6f * k00 * k11) {
        const f32 inv = 1.0f / det;
        x0 = (k11 * b0 - k01 * b1) * inv;
        x1 = (k00 * b1 - k01 * b0) * inv;
    } else {
        x0 = k00 > 0.0f ? b0 / k00 : 0.0f;
        x1 = k11 > 0.0f ? b1 / k11 : 0.0f;
    }
    const f32 l0 = -s.mass_scale * x0 - s.impulse_scale * j->eq[4];
    const f32 l1 = -s.mass_scale * x1 - s.impulse_scale * j->eq[5];
    j->eq[4] += l0;
    j->eq[5] += l1;
    axes_apply(x, p, q, l0, l1);
}

static void hinge_warm_start(de__jx *x, de__joint *j) {
    const de_vec3 a1 = de__jq_axis(x->qa, 0);
    const de__jrow r = de__jrow_angular(x, a1);
    de__jrow_apply(x, &r, j->motor[0] + j->lower[0] - j->upper[0]);
    axes_apply(x, de__jq_axis(x->qa, 1), de__jq_axis(x->qa, 2), j->eq[4], j->eq[5]);
    de__jpoint_apply(x, j->lin);
}

static void hinge_solve(de__jx *x, de__joint *j) {
    const de_vec3 a1 = de__jq_axis(x->qa, 0);
    const de__jrow r = de__jrow_angular(x, a1);
    const f32 angle = hinge_angle(x);
    de__jrow_motor(x, j, 0, &r, angle, &j->motor[0]);
    if (j->enable_limit)
        de__jrow_limits(x, &r, angle, j->lo[0], j->hi[0], &j->lower[0], &j->upper[0]);
    axes_solve(x, j);
    de__jpoint_solve(x, &j->lin);
}

const de__joint_ops de__joint_hinge_ops = {hinge_warm_start, hinge_solve, de__joint_point_impulse};
