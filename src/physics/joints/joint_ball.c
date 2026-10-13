/*
 * joint_ball.c: DE_JOINT_BALL (DENG-220): the point block only; rotation is free. See
 * joint_internal.h.
 */
#include "physics/joints/joint_internal.h"

static void ball_warm_start(de__jx *x, de__joint *j) {
    de__jpoint_apply(x, j->lin);
}

static void ball_solve(de__jx *x, de__joint *j) {
    de__jpoint_solve(x, &j->lin);
}

const de__joint_ops de__joint_ball_ops = {ball_warm_start, ball_solve, de__joint_point_impulse};
