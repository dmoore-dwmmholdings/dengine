/*
 * integrate.c: per-body kernels of the soft-step solver (DENG-217; see solver_internal.h):
 * solver-body setup, velocity and position integration per substep, and the end-of-step write
 * back to the body pool. Each kernel takes a slot range, so DENG-218 can split it into fixed
 * ranges; each slot is independent.
 *
 * Large worlds (SPEC §5.5, DENG-2 Q10): no world position is ever formed in f32. A solver body
 * integrates its center-of-mass displacement `dp` (f32) and its rotation since the step began
 * `dq`; finalize adds them to the f64 body position once per step:
 *
 *     position' = position + (f64)dp + R0 * local_com - R' * local_com     (all in f64)
 *     rotation' = normalize(dq * q0)
 *
 * so the center of mass moves by exactly dp and the origin follows the rotation about it.
 *
 * Determinism (SPEC §5.7): + - * / and de_dsqrt / de_dintegrate_quat / de_dnormalize_quat only;
 * slots in ascending order.
 */
#include "physics/solver/solver_internal.h"

/* ================================ Setup ================================================ */

/* R * M * R^T in f64 (M symmetric, column-major), rounded to f32. */
static de_mat3 rotate_inertia(de_quat q, const de_mat3 *m) {
    const de__dmat3 r = de__dmat3_from_quat(q);
    f64 t[9]; /* t = M * R^T: column c of R^T is row c of R */
    for (u32 c = 0; c < 3; ++c)
        for (u32 row = 0; row < 3; ++row)
            t[c * 3 + row] = (f64)m->m[0 * 3 + row] * r.m[0 * 3 + c] +
                             (f64)m->m[1 * 3 + row] * r.m[1 * 3 + c] +
                             (f64)m->m[2 * 3 + row] * r.m[2 * 3 + c];
    de_mat3 out;
    for (u32 c = 0; c < 3; ++c)
        for (u32 row = 0; row < 3; ++row)
            out.m[c * 3 + row] =
                (f32)(r.m[0 * 3 + row] * t[c * 3 + 0] + r.m[1 * 3 + row] * t[c * 3 + 1] +
                      r.m[2 * 3 + row] * t[c * 3 + 2]);
    return out;
}

/* Zeroes row and column `axis` (no angular response about that world axis). */
static void lock_axis(de_mat3 *m, u32 axis) {
    for (u32 k = 0; k < 3; ++k) {
        m->m[axis * 3 + k] = 0.0f;
        m->m[k * 3 + axis] = 0.0f;
    }
}

void de__integrate_init_body(const de__solver *s, u32 i, de__solver_body *out) {
    const de__body_pool *b = &s->p->bodies;
    const de__solver_ctx *x = &s->ctx;
    memset(out, 0, sizeof *out);
    out->v = b->linear_velocity[i];
    out->w = b->angular_velocity[i];
    out->dq.w = 1.0f;
    out->body = i;
    out->linear_damping = 1.0f;
    out->angular_damping = 1.0f;
    const u32 bf = b->flags[i];
    if (b->type[i] != (u8)DE_BODY_DYNAMIC) {
        out->flags = DE__SOLVER_BODY_KINEMATIC;
        return;
    }
    u32 flags = DE__SOLVER_BODY_DYNAMIC;
    out->inv_mass = b->inv_mass[i];
    out->inv_inertia = rotate_inertia(b->rotation[i], &b->local_inv_inertia[i]);
    if (bf & DE_BODY_FLAG_LOCK_ROT_X) {
        flags |= DE__SOLVER_BODY_LOCK_X;
        lock_axis(&out->inv_inertia, 0);
    }
    if (bf & DE_BODY_FLAG_LOCK_ROT_Y) {
        flags |= DE__SOLVER_BODY_LOCK_Y;
        lock_axis(&out->inv_inertia, 1);
    }
    if (bf & DE_BODY_FLAG_LOCK_ROT_Z) {
        flags |= DE__SOLVER_BODY_LOCK_Z;
        lock_axis(&out->inv_inertia, 2);
    }
    out->flags = flags;
    /* Damping as in Box2D v3: v *= 1 / (1 + h c) per substep (negative c counts as 0). */
    out->linear_damping = 1.0f / (1.0f + x->h * de__maxf(b->linear_damping[i], 0.0f));
    out->angular_damping = 1.0f / (1.0f + x->h * de__maxf(b->angular_damping[i], 0.0f));
    out->gravity_dv = de__v3_scale(s->p->gravity, x->h * b->gravity_scale[i]);
}

/* ================================ Substep kernels ====================================== */

static de_vec3 clamp_length(de_vec3 v, f32 max) {
    const f32 l2 = de__v3_dot(v, v);
    if (!(l2 > max * max))
        return v;
    return de__v3_scale(v, max / de_dsqrt(l2));
}

void de__integrate_velocities(de__solver *s, u32 begin, u32 end) {
    const f32 max_v = s->settings.max_linear_speed;
    const f32 max_w = DE__SOLVER_MAX_ROTATION_PER_SUBSTEP * s->ctx.inv_h;
    for (u32 k = begin; k < end; ++k) {
        de__solver_body *sb = &s->bodies[k];
        if ((sb->flags & DE__SOLVER_BODY_DYNAMIC) == 0u)
            continue; /* kinematic: velocity is an input */
        de_vec3 v = de__v3_add(de__v3_scale(sb->v, sb->linear_damping), sb->gravity_dv);
        de_vec3 w = de__v3_scale(sb->w, sb->angular_damping);
        if (sb->flags & DE__SOLVER_BODY_LOCK_X)
            w.x = 0.0f;
        if (sb->flags & DE__SOLVER_BODY_LOCK_Y)
            w.y = 0.0f;
        if (sb->flags & DE__SOLVER_BODY_LOCK_Z)
            w.z = 0.0f;
        sb->v = clamp_length(v, max_v);
        sb->w = clamp_length(w, max_w);
    }
}

void de__integrate_positions(de__solver *s, u32 begin, u32 end) {
    const f32 h = s->ctx.h;
    for (u32 k = begin; k < end; ++k) {
        de__solver_body *sb = &s->bodies[k];
        sb->dp = de__v3_add(sb->dp, de__v3_scale(sb->v, h));
        if (sb->w.x != 0.0f || sb->w.y != 0.0f || sb->w.z != 0.0f)
            sb->dq = de_dintegrate_quat(sb->dq, sb->w, h);
    }
}

/* ================================ Finalize ============================================= */

/* a * b (apply b, then a) in f64. */
static void quat_mul_d(de_quat a, de_quat b, f64 out[4]) {
    const f64 ax = a.x, ay = a.y, az = a.z, aw = a.w;
    const f64 bx = b.x, by = b.y, bz = b.z, bw = b.w;
    out[0] = aw * bx + ax * bw + ay * bz - az * by;
    out[1] = aw * by - ax * bz + ay * bw + az * bx;
    out[2] = aw * bz + ax * by - ay * bx + az * bw;
    out[3] = aw * bw - ax * bx - ay * by - az * bz;
}

void de__integrate_finalize(de__solver *s, u32 begin, u32 end) {
    de_physics_world *p = s->p;
    de__body_pool *b = &p->bodies;
    for (u32 k = begin; k < end; ++k) {
        const de__solver_body *sb = &s->bodies[k];
        const u32 i = sb->body;
        b->linear_velocity[i] = sb->v;
        b->angular_velocity[i] = sb->w;
        const bool rotated =
            sb->dq.x != 0.0f || sb->dq.y != 0.0f || sb->dq.z != 0.0f || sb->dq.w != 1.0f;
        de_dvec3 pos = de__d3_add(b->position[i], de__d3_from(sb->dp));
        if (rotated) {
            const de_quat q0 = b->rotation[i];
            f64 qd[4];
            quat_mul_d(sb->dq, q0, qd);
            de_quat qn;
            qn.x = (f32)qd[0];
            qn.y = (f32)qd[1];
            qn.z = (f32)qd[2];
            qn.w = (f32)qd[3];
            qn = de_dnormalize_quat(qn);
            const de_dvec3 lcom = de__d3_from(b->local_com[i]);
            const de__dmat3 r0 = de__dmat3_from_quat(q0);
            const de__dmat3 r1 = de__dmat3_from_quat(qn);
            const de_dvec3 shift = de__d3_sub(de__dmat3_mul(&r0, lcom), de__dmat3_mul(&r1, lcom));
            pos = de__d3_add(pos, shift);
            b->rotation[i] = qn;
        }
        b->position[i] = pos;
        de__body_refresh_aabb(p, i);
    }
}
