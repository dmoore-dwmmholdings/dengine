/*
 * joint_fixed.c: DE_JOINT_FIXED (DENG-220): the point block (anchors together) and the angular
 * lock block (frames aligned). See joint_internal.h.
 */
#include "physics/joints/joint_internal.h"

static void fixed_warm_start(de__jx *x, de__joint *j) {
    de__jlock_apply(x, j->ang);
    de__jpoint_apply(x, j->lin);
}

static void fixed_solve(de__jx *x, de__joint *j) {
    de__jlock_solve(x, &j->ang);
    de__jpoint_solve(x, &j->lin);
}

const de__joint_ops de__joint_fixed_ops = {fixed_warm_start, fixed_solve, de__joint_point_impulse};
