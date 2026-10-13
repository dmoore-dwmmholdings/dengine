/*
 * joint_slider.c: DE_JOINT_SLIDER (DENG-220). Rows: the motor and limit along A's X axis
 * (axis 0), B's anchor kept on A's X axis (two rows along A's Y and Z), and the angular lock
 * block. The linear rows use the prismatic Jacobian (the axis turns with A). See
 * joint_internal.h.
 */
#include "physics/joints/joint_internal.h"

static void slider_warm_start(de__jx *x, de__joint *j) {
    for (u32 k = 0; k < 3; ++k) {
        const de__jrow r = de__jrow_linear(x, de__jq_axis(x->qa, k));
        const f32 l = k == 0 ? j->motor[0] + j->lower[0] - j->upper[0] : j->eq[k];
        de__jrow_apply(x, &r, l);
    }
    de__jlock_apply(x, j->ang);
}

static void slider_solve(de__jx *x, de__joint *j) {
    const de_vec3 ax = de__jq_axis(x->qa, 0);
    const de__jrow r = de__jrow_linear(x, ax);
    const f32 offset = de__v3_dot(x->d, ax);
    de__jrow_motor(x, j, 0, &r, offset, &j->motor[0]);
    if (j->enable_limit)
        de__jrow_limits(x, &r, offset, j->lo[0], j->hi[0], &j->lower[0], &j->upper[0]);
    for (u32 k = 1; k < 3; ++k) {
        const de_vec3 n = de__jq_axis(x->qa, k);
        const de__jrow rk = de__jrow_linear(x, n);
        de__jrow_equality(x, &rk, de__v3_dot(x->d, n), &j->eq[k]);
    }
    de__jlock_solve(x, &j->ang);
}

static de_vec3 slider_impulse(const de__jx *x, const de__joint *j) {
    de_vec3 l = de__v3_scale(de__jq_axis(x->qa, 0), j->motor[0] + j->lower[0] - j->upper[0]);
    l = de__v3_add(l, de__v3_scale(de__jq_axis(x->qa, 1), j->eq[1]));
    return de__v3_add(l, de__v3_scale(de__jq_axis(x->qa, 2), j->eq[2]));
}

const de__joint_ops de__joint_slider_ops = {slider_warm_start, slider_solve, slider_impulse};
