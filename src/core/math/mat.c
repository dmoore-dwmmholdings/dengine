/* mat.c: de_mat3 and de_mat4 operations, scalar reference (SPEC §5.4, DENG-241).
 *
 * Column-major: element (row r, column c) is m[c * N + r]. The projection and view builders are
 * in proj.c. */
#include "core/math/math_internal.h"
#include "core/math/math_simd.h"

/* ---- de_mat3 ---- */

de_mat3 de_m3_identity(void) {
    de_mat3 m = {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
    return m;
}

de_mat3 de_m3_mul(const de_mat3 *a, const de_mat3 *b) {
    de_mat3 r;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 3; ++i)
            r.m[c * 3 + i] = a->m[0 * 3 + i] * b->m[c * 3 + 0] + a->m[1 * 3 + i] * b->m[c * 3 + 1] +
                             a->m[2 * 3 + i] * b->m[c * 3 + 2];
    return r;
}

de_vec3 de_m3_mul_v3(const de_mat3 *m, de_vec3 v) {
    return de__v3(m->m[0] * v.x + m->m[3] * v.y + m->m[6] * v.z,
                  m->m[1] * v.x + m->m[4] * v.y + m->m[7] * v.z,
                  m->m[2] * v.x + m->m[5] * v.y + m->m[8] * v.z);
}

de_mat3 de_m3_transpose(const de_mat3 *m) {
    de_mat3 r;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 3; ++i)
            r.m[c * 3 + i] = m->m[i * 3 + c];
    return r;
}

f32 de_m3_determinant(const de_mat3 *m) {
    /* det = col0 . (col1 x col2) */
    return de_v3_dot(de__m3_col(m, 0), de_v3_cross(de__m3_col(m, 1), de__m3_col(m, 2)));
}

de_mat3 de_m3_inverse(const de_mat3 *m) {
    /* The rows of the inverse are the cross products of the columns, over the determinant. */
    de_vec3 c0 = de__m3_col(m, 0), c1 = de__m3_col(m, 1), c2 = de__m3_col(m, 2);
    de_vec3 r0 = de_v3_cross(c1, c2);
    de_vec3 r1 = de_v3_cross(c2, c0);
    de_vec3 r2 = de_v3_cross(c0, c1);
    f32 det = de_v3_dot(c0, r0);
    de_mat3 inv = {{0}};
    if (det == 0.0f)
        return inv;
    f32 s = 1.0f / det;
    inv.m[0] = r0.x * s;
    inv.m[3] = r0.y * s;
    inv.m[6] = r0.z * s;
    inv.m[1] = r1.x * s;
    inv.m[4] = r1.y * s;
    inv.m[7] = r1.z * s;
    inv.m[2] = r2.x * s;
    inv.m[5] = r2.y * s;
    inv.m[8] = r2.z * s;
    return inv;
}

de_mat3 de_m3_from_quat(de_quat q) {
    return de_quat_to_m3(q);
}

/* ---- de_mat4 ---- */

de_mat4 de_m4_identity(void) {
    de_mat4 m = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    return m;
}

de_mat4 de__m4_mul_scalar(const de_mat4 *a, const de_mat4 *b) {
    de_mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 4; ++i)
            r.m[c * 4 + i] = a->m[0 * 4 + i] * b->m[c * 4 + 0] + a->m[1 * 4 + i] * b->m[c * 4 + 1] +
                             a->m[2 * 4 + i] * b->m[c * 4 + 2] + a->m[3 * 4 + i] * b->m[c * 4 + 3];
    return r;
}

de_mat4 de_m4_transpose(const de_mat4 *m) {
    de_mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int i = 0; i < 4; ++i)
            r.m[c * 4 + i] = m->m[i * 4 + c];
    return r;
}

de_mat4 de__m4_inverse_scalar(const de_mat4 *mat) {
    /* Cofactor expansion through the 2x2 sub-determinants of the top and bottom half. Valid for
     * either storage order, because inverse(transpose(M)) = transpose(inverse(M)). */
    const f32 *m = mat->m;
    f32 s0 = m[0] * m[5] - m[4] * m[1];
    f32 s1 = m[0] * m[6] - m[4] * m[2];
    f32 s2 = m[0] * m[7] - m[4] * m[3];
    f32 s3 = m[1] * m[6] - m[5] * m[2];
    f32 s4 = m[1] * m[7] - m[5] * m[3];
    f32 s5 = m[2] * m[7] - m[6] * m[3];
    f32 c5 = m[10] * m[15] - m[14] * m[11];
    f32 c4 = m[9] * m[15] - m[13] * m[11];
    f32 c3 = m[9] * m[14] - m[13] * m[10];
    f32 c2 = m[8] * m[15] - m[12] * m[11];
    f32 c1 = m[8] * m[14] - m[12] * m[10];
    f32 c0 = m[8] * m[13] - m[12] * m[9];
    f32 det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    de_mat4 r = {{0}};
    if (det == 0.0f)
        return r;
    f32 inv = 1.0f / det;
    r.m[0] = (m[5] * c5 - m[6] * c4 + m[7] * c3) * inv;
    r.m[1] = (-m[1] * c5 + m[2] * c4 - m[3] * c3) * inv;
    r.m[2] = (m[13] * s5 - m[14] * s4 + m[15] * s3) * inv;
    r.m[3] = (-m[9] * s5 + m[10] * s4 - m[11] * s3) * inv;
    r.m[4] = (-m[4] * c5 + m[6] * c2 - m[7] * c1) * inv;
    r.m[5] = (m[0] * c5 - m[2] * c2 + m[3] * c1) * inv;
    r.m[6] = (-m[12] * s5 + m[14] * s2 - m[15] * s1) * inv;
    r.m[7] = (m[8] * s5 - m[10] * s2 + m[11] * s1) * inv;
    r.m[8] = (m[4] * c4 - m[5] * c2 + m[7] * c0) * inv;
    r.m[9] = (-m[0] * c4 + m[1] * c2 - m[3] * c0) * inv;
    r.m[10] = (m[12] * s4 - m[13] * s2 + m[15] * s0) * inv;
    r.m[11] = (-m[8] * s4 + m[9] * s2 - m[11] * s0) * inv;
    r.m[12] = (-m[4] * c3 + m[5] * c1 - m[6] * c0) * inv;
    r.m[13] = (m[0] * c3 - m[1] * c1 + m[2] * c0) * inv;
    r.m[14] = (-m[12] * s3 + m[13] * s1 - m[14] * s0) * inv;
    r.m[15] = (m[8] * s3 - m[9] * s1 + m[10] * s0) * inv;
    return r;
}

de_mat4 de_m4_inverse_affine(const de_mat4 *m) {
    /* [L t; 0 1]^-1 = [L^-1  -L^-1 t; 0 1] */
    de_mat3 l = {
        {m->m[0], m->m[1], m->m[2], m->m[4], m->m[5], m->m[6], m->m[8], m->m[9], m->m[10]}};
    de_mat4 r = {{0}};
    if (de_m3_determinant(&l) == 0.0f)
        return r;
    de_mat3 li = de_m3_inverse(&l);
    de_vec3 t = de_m3_mul_v3(&li, de__v3(m->m[12], m->m[13], m->m[14]));
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 3; ++i)
            r.m[c * 4 + i] = li.m[c * 3 + i];
    r.m[12] = -t.x;
    r.m[13] = -t.y;
    r.m[14] = -t.z;
    r.m[15] = 1.0f;
    return r;
}

de_vec4 de__m4_mul_v4_scalar(const de_mat4 *m, de_vec4 v) {
    de_vec4 r;
    r.x = m->m[0] * v.x + m->m[4] * v.y + m->m[8] * v.z + m->m[12] * v.w;
    r.y = m->m[1] * v.x + m->m[5] * v.y + m->m[9] * v.z + m->m[13] * v.w;
    r.z = m->m[2] * v.x + m->m[6] * v.y + m->m[10] * v.z + m->m[14] * v.w;
    r.w = m->m[3] * v.x + m->m[7] * v.y + m->m[11] * v.z + m->m[15] * v.w;
    return r;
}

de_vec3 de_m4_mul_point(const de_mat4 *m, de_vec3 p) {
    f32 x = m->m[0] * p.x + m->m[4] * p.y + m->m[8] * p.z + m->m[12];
    f32 y = m->m[1] * p.x + m->m[5] * p.y + m->m[9] * p.z + m->m[13];
    f32 z = m->m[2] * p.x + m->m[6] * p.y + m->m[10] * p.z + m->m[14];
    f32 w = m->m[3] * p.x + m->m[7] * p.y + m->m[11] * p.z + m->m[15];
    if (w == 1.0f) /* affine: skip the divide, keep the result exact */
        return de__v3(x, y, z);
    f32 inv = 1.0f / w;
    return de__v3(x * inv, y * inv, z * inv);
}

de_vec3 de_m4_mul_dir(const de_mat4 *m, de_vec3 d) {
    return de__v3(m->m[0] * d.x + m->m[4] * d.y + m->m[8] * d.z,
                  m->m[1] * d.x + m->m[5] * d.y + m->m[9] * d.z,
                  m->m[2] * d.x + m->m[6] * d.y + m->m[10] * d.z);
}

de_mat4 de_m4_trs(de_vec3 t, de_quat r, de_vec3 s) {
    de_mat3 rs = de__rs_m3(r, s);
    de_mat4 m;
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < 3; ++i)
            m.m[c * 4 + i] = rs.m[c * 3 + i];
        m.m[c * 4 + 3] = 0.0f;
    }
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    m.m[15] = 1.0f;
    return m;
}

bool de_m4_decompose(const de_mat4 *m, de_vec3 *t, de_quat *r, de_vec3 *s) {
    de_mat3 l = {
        {m->m[0], m->m[1], m->m[2], m->m[4], m->m[5], m->m[6], m->m[8], m->m[9], m->m[10]}};
    f32 sc[3];
    bool ok = true;
    for (int c = 0; c < 3; ++c) {
        sc[c] = de_v3_length(de__m3_col(&l, c));
        if (!(sc[c] > 0.0f))
            ok = false;
    }
    if (ok && de_m3_determinant(&l) < 0.0f)
        sc[0] = -sc[0]; /* a mirror: report it on x, so the rest is a proper rotation */
    if (t)
        *t = de__v3(m->m[12], m->m[13], m->m[14]);
    if (s)
        *s = de__v3(sc[0], sc[1], sc[2]);
    if (r) {
        if (ok) {
            for (int c = 0; c < 3; ++c)
                for (int i = 0; i < 3; ++i)
                    l.m[c * 3 + i] /= sc[c];
            *r = de_quat_from_m3(&l);
        } else {
            *r = de_quat_identity();
        }
    }
    return ok;
}
