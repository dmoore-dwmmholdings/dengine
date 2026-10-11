/* affine.c: de_affine (3x4) operations, scalar reference (SPEC §5.4, DENG-241).
 *
 * Column-major [L | t]: element (r, c) is m[c * 3 + r]; m[0..8] is the linear part L (the same
 * layout as de_mat3), m[9..11] the translation t. */
#include "core/math/math_internal.h"

static de_affine de__affine_make(const de_mat3 *l, de_vec3 t) {
    de_affine a;
    for (int i = 0; i < 9; ++i)
        a.m[i] = l->m[i];
    a.m[9] = t.x;
    a.m[10] = t.y;
    a.m[11] = t.z;
    return a;
}

static de_vec3 de__affine_t(const de_affine *a) {
    return de__v3(a->m[9], a->m[10], a->m[11]);
}

de_affine de_affine_identity(void) {
    de_affine a = {{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}};
    return a;
}

de_affine de_affine_from_trs(de_vec3 t, de_quat r, de_vec3 s) {
    de_mat3 l = de__rs_m3(r, s);
    return de__affine_make(&l, t);
}

de_affine de_affine_mul(const de_affine *a, const de_affine *b) {
    /* [La ta] [Lb tb] = [La Lb, La tb + ta] */
    de_mat3 la = de__affine_linear(a), lb = de__affine_linear(b);
    de_mat3 l = de_m3_mul(&la, &lb);
    de_vec3 t = de_v3_add(de_m3_mul_v3(&la, de__affine_t(b)), de__affine_t(a));
    return de__affine_make(&l, t);
}

de_affine de_affine_inverse(const de_affine *a) {
    /* [L t]^-1 = [L^-1, -L^-1 t] */
    de_mat3 l = de__affine_linear(a);
    if (de_m3_determinant(&l) == 0.0f) {
        de_affine z = {{0}};
        return z;
    }
    de_mat3 li = de_m3_inverse(&l);
    de_vec3 t = de_v3_scale(de_m3_mul_v3(&li, de__affine_t(a)), -1.0f);
    return de__affine_make(&li, t);
}

de_mat4 de_affine_to_m4(const de_affine *a) {
    de_mat4 m;
    for (int c = 0; c < 4; ++c) {
        for (int i = 0; i < 3; ++i)
            m.m[c * 4 + i] = a->m[c * 3 + i];
        m.m[c * 4 + 3] = c == 3 ? 1.0f : 0.0f;
    }
    return m;
}

de_affine de_affine_from_m4(const de_mat4 *m) {
    de_affine a;
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 3; ++i)
            a.m[c * 3 + i] = m->m[c * 4 + i];
    return a;
}

de_vec3 de_affine_transform_point(const de_affine *a, de_vec3 p) {
    return de__v3(a->m[0] * p.x + a->m[3] * p.y + a->m[6] * p.z + a->m[9],
                  a->m[1] * p.x + a->m[4] * p.y + a->m[7] * p.z + a->m[10],
                  a->m[2] * p.x + a->m[5] * p.y + a->m[8] * p.z + a->m[11]);
}

de_vec3 de_affine_transform_dir(const de_affine *a, de_vec3 d) {
    return de__v3(a->m[0] * d.x + a->m[3] * d.y + a->m[6] * d.z,
                  a->m[1] * d.x + a->m[4] * d.y + a->m[7] * d.z,
                  a->m[2] * d.x + a->m[5] * d.y + a->m[8] * d.z);
}
