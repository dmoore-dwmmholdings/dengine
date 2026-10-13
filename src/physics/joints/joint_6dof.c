/*
 * joint_6dof.c: DE_JOINT_6DOF (DENG-220). Each axis is locked, limited or free (axis_mode).
 *
 * - Linear axes 0..2 along A's X, Y, Z: when all three are locked, the point block (as the fixed
 *   joint); otherwise one prismatic row per axis (locked: held at 0; limited: [lo, hi]).
 * - Angular axes 3..5: when all three are locked, the angular lock block (as the fixed joint);
 *   otherwise the twist (about B's X axis) and the two swing components (rotation vector about
 *   A's Y and Z, swing-twist split) each get a row.
 * - Motors on every axis that is not locked; then limits, then locked rows (point and lock blocks
 *   last). With every axis locked the rows are exactly the fixed joint's.
 * See joint_internal.h.
 */
#include "physics/joints/joint_internal.h"

/* Coordinates and rows of the angular axes 3..5. */
static void angular(const de__jx *x, f32 coord[3], de_vec3 axis[3]) {
    f32 twist, sy, sz, swing;
    de__joint_swing_twist(de__joint_relative(x), &twist, &sy, &sz, &swing);
    coord[0] = twist;
    coord[1] = sy;
    coord[2] = sz;
    axis[0] = de__jq_axis(x->qb, 0);
    axis[1] = de__jq_axis(x->qa, 1);
    axis[2] = de__jq_axis(x->qa, 2);
}

static f32 axis_total(const de__joint *j, u32 k) {
    return j->eq[k] + j->lower[k] - j->upper[k] + j->motor[k];
}

static void d6_warm_start(de__jx *x, de__joint *j) {
    if (j->angular_block) {
        de__jlock_apply(x, j->ang);
    } else {
        f32 coord[3];
        de_vec3 axis[3];
        angular(x, coord, axis);
        for (u32 k = 0; k < 3; ++k) {
            const de__jrow r = de__jrow_angular(x, axis[k]);
            de__jrow_apply(x, &r, axis_total(j, k + 3u));
        }
    }
    if (j->linear_block) {
        de__jpoint_apply(x, j->lin);
    } else {
        for (u32 k = 0; k < 3; ++k) {
            const de__jrow r = de__jrow_linear(x, de__jq_axis(x->qa, k));
            de__jrow_apply(x, &r, axis_total(j, k));
        }
    }
}

static void d6_row(de__jx *x, de__joint *j, u32 k, const de__jrow *r, f32 coord) {
    switch (j->axis_mode[k]) {
    case DE_JOINT_AXIS_LOCKED:
        de__jrow_equality(x, r, coord, &j->eq[k]);
        break;
    case DE_JOINT_AXIS_LIMITED:
        de__jrow_limits(x, r, coord, j->lo[k], j->hi[k], &j->lower[k], &j->upper[k]);
        break;
    default:
        break;
    }
}

static void d6_solve(de__jx *x, de__joint *j) {
    f32 coord[3] = {0.0f, 0.0f, 0.0f};
    de_vec3 axis[3];
    de__jrow ang_rows[3], lin_rows[3];
    f32 lin_coord[3] = {0.0f, 0.0f, 0.0f};
    memset(axis, 0, sizeof axis);
    memset(ang_rows, 0, sizeof ang_rows);
    memset(lin_rows, 0, sizeof lin_rows);
    if (!j->angular_block) {
        angular(x, coord, axis);
        for (u32 k = 0; k < 3; ++k)
            ang_rows[k] = de__jrow_angular(x, axis[k]);
    }
    if (!j->linear_block) {
        for (u32 k = 0; k < 3; ++k) {
            const de_vec3 n = de__jq_axis(x->qa, k);
            lin_rows[k] = de__jrow_linear(x, n);
            lin_coord[k] = de__v3_dot(x->d, n);
        }
    }
    /* Motors. */
    for (u32 k = 0; k < 6; ++k) {
        if (j->axis_mode[k] == (u8)DE_JOINT_AXIS_LOCKED)
            continue;
        if (k < 3)
            de__jrow_motor(x, j, k, &lin_rows[k], lin_coord[k], &j->motor[k]);
        else
            de__jrow_motor(x, j, k, &ang_rows[k - 3u], coord[k - 3u], &j->motor[k]);
    }
    /* Angular rows, then linear rows (the point block last). */
    if (j->angular_block) {
        de__jlock_solve(x, &j->ang);
    } else {
        for (u32 k = 0; k < 3; ++k)
            d6_row(x, j, k + 3u, &ang_rows[k], coord[k]);
    }
    if (j->linear_block) {
        de__jpoint_solve(x, &j->lin);
    } else {
        for (u32 k = 0; k < 3; ++k)
            d6_row(x, j, k, &lin_rows[k], lin_coord[k]);
    }
}

static de_vec3 d6_impulse(const de__jx *x, const de__joint *j) {
    if (j->linear_block)
        return j->lin;
    de_vec3 l = de__v3(0.0f, 0.0f, 0.0f);
    for (u32 k = 0; k < 3; ++k)
        l = de__v3_add(l, de__v3_scale(de__jq_axis(x->qa, k), axis_total(j, k)));
    return l;
}

const de__joint_ops de__joint_6dof_ops = {d6_warm_start, d6_solve, d6_impulse};
