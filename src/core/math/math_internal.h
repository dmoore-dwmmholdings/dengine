/* math_internal.h: helpers shared by the fast-math sources in src/core/math/ (DENG-241).
 *
 * Fast math only. Deterministic sources (dmath*.c, DENG-243) must not include this header: it
 * pulls in <math.h>, whose transcendentals differ between libm implementations (SPEC §5.7). */
#ifndef DE_CORE_MATH_INTERNAL_H
#define DE_CORE_MATH_INTERNAL_H

#include <dengine/de_math.h>

#include <float.h>
#include <math.h>

/* Constructors. Compound literals would do in C, but these read better in long expressions. */
static inline de_vec3 de__v3(f32 x, f32 y, f32 z) {
    de_vec3 r = {x, y, z};
    return r;
}

static inline de_quat de__quat(f32 x, f32 y, f32 z, f32 w) {
    de_quat q;
    q.x = x;
    q.y = y;
    q.z = z;
    q.w = w;
    return q;
}

/* Column c of a matrix as a vector (column-major: element (r, c) is m[c * N + r]). */
static inline de_vec3 de__m3_col(const de_mat3 *m, int c) {
    return de__v3(m->m[c * 3 + 0], m->m[c * 3 + 1], m->m[c * 3 + 2]);
}

/* The linear part of an affine transform as a de_mat3 (both store 3-float columns, in order). */
static inline de_mat3 de__affine_linear(const de_affine *a) {
    de_mat3 r;
    for (int i = 0; i < 9; ++i)
        r.m[i] = a->m[i];
    return r;
}

/* Rotation matrix times diag(s): column c scaled by s[c]. Shared by the TRS builders. */
static inline de_mat3 de__rs_m3(de_quat r, de_vec3 s) {
    de_mat3 m = de_quat_to_m3(r);
    const f32 sc[3] = {s.x, s.y, s.z};
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 3; ++i)
            m.m[c * 3 + i] *= sc[c];
    return m;
}

#endif
