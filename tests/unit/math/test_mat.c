/* test_mat.c: de_mat3, de_mat4, de_affine, look-at and the reverse-Z projections (DENG-241). */
#include "de_test.h"

#include <dengine/de_math.h>

#include <math.h>

static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 r = {x, y, z};
    return r;
}
static de_vec4 v4(f32 x, f32 y, f32 z, f32 w) {
    de_vec4 r;
    r.x = x;
    r.y = y;
    r.z = z;
    r.w = w;
    return r;
}

#define EXPECT_V3_NEAR(a, b, eps)                                                                  \
    do {                                                                                           \
        de_vec3 a_ = (a), b_ = (b);                                                                \
        EXPECT_FLOAT_EQ(a_.x, b_.x, eps);                                                          \
        EXPECT_FLOAT_EQ(a_.y, b_.y, eps);                                                          \
        EXPECT_FLOAT_EQ(a_.z, b_.z, eps);                                                          \
    } while (0)

static u32 g_rng = 0x2545F491u;
static f32 rnd(f32 lo, f32 hi) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return lo + (hi - lo) * (f32)(g_rng >> 8) * (1.0f / 16777216.0f);
}
static de_vec3 rnd_v3(f32 lo, f32 hi) {
    return v3(rnd(lo, hi), rnd(lo, hi), rnd(lo, hi));
}
static de_quat rnd_quat(void) {
    return de_quat_axis_angle(de_v3_normalize_safe(rnd_v3(-1, 1), DE_AXIS_UP), rnd(-DE_PI, DE_PI));
}

/* max |a[i] - b[i]| over n floats */
static f32 max_diff(const f32 *a, const f32 *b, int n) {
    f32 d = 0;
    for (int i = 0; i < n; ++i)
        d = fmaxf(d, fabsf(a[i] - b[i]));
    return d;
}

/* ---- de_mat3 ---- */

TEST(mat, m3_basics) {
    de_mat3 id = de_m3_identity();
    const f32 expect_id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    EXPECT_MEMEQ(id.m, expect_id, sizeof expect_id);
    /* Column-major: columns (1,2,3), (4,5,6), (7,8,10). */
    de_mat3 a = {{1, 2, 3, 4, 5, 6, 7, 8, 10}};
    EXPECT_V3_NEAR(de_m3_mul_v3(&a, v3(1, 0, 0)), v3(1, 2, 3), 0);
    EXPECT_V3_NEAR(de_m3_mul_v3(&a, v3(0, 0, 1)), v3(7, 8, 10), 0);
    EXPECT_V3_NEAR(de_m3_mul_v3(&a, v3(1, 1, 1)), v3(12, 15, 19), 0);
    de_mat3 t = de_m3_transpose(&a);
    EXPECT_FLOAT_EQ(t.m[1], 4, 0); /* (1,0) of T = (0,1) of A */
    EXPECT_FLOAT_EQ(t.m[3], 2, 0);
    EXPECT_FLOAT_EQ(de_m3_determinant(&a), -3, 1e-5);
    de_mat3 ai = de_m3_inverse(&a);
    de_mat3 p = de_m3_mul(&a, &ai);
    EXPECT_TRUE(max_diff(p.m, expect_id, 9) <= 1e-5f);
    de_mat3 ia = de_m3_mul(&id, &a);
    EXPECT_MEMEQ(ia.m, a.m, sizeof a.m);
    /* mul order: (A B) v = A (B v). */
    de_mat3 b = de_quat_to_m3(rnd_quat());
    de_mat3 ab = de_m3_mul(&a, &b);
    de_vec3 v = v3(0.5f, -2, 3);
    EXPECT_V3_NEAR(de_m3_mul_v3(&ab, v), de_m3_mul_v3(&a, de_m3_mul_v3(&b, v)), 1e-5);
    /* Singular gives zero. */
    de_mat3 sing = {{1, 2, 3, 2, 4, 6, 0, 0, 1}};
    de_mat3 si = de_m3_inverse(&sing);
    const f32 zeros[9] = {0};
    EXPECT_MEMEQ(si.m, zeros, sizeof zeros);
}

/* ---- de_mat4 ---- */

TEST(mat, m4_basics) {
    de_mat4 id = de_m4_identity();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            EXPECT_FLOAT_EQ(id.m[c * 4 + r], c == r ? 1 : 0, 0);
    de_mat4 a;
    for (int i = 0; i < 16; ++i)
        a.m[i] = (f32)(i + 1);
    de_mat4 at = de_m4_transpose(&a);
    EXPECT_FLOAT_EQ(at.m[1], 5, 0); /* (1,0) of T = (0,1) of A = m[4] */
    EXPECT_FLOAT_EQ(at.m[12], 4, 0);
    de_mat4 tt = de_m4_transpose(&at);
    EXPECT_MEMEQ(tt.m, a.m, sizeof a.m);
    de_mat4 ia = de_m4_mul(&id, &a), ai = de_m4_mul(&a, &id);
    EXPECT_MEMEQ(ia.m, a.m, sizeof a.m);
    EXPECT_MEMEQ(ai.m, a.m, sizeof a.m);
    /* M v with column-major storage: column 0 is m[0..3]. */
    de_vec4 r = de_m4_mul_v4(&a, v4(1, 0, 0, 0));
    EXPECT_FLOAT_EQ(r.x, 1, 0);
    EXPECT_FLOAT_EQ(r.w, 4, 0);
    r = de_m4_mul_v4(&a, v4(1, 1, 1, 1));
    EXPECT_FLOAT_EQ(r.x, 1 + 5 + 9 + 13, 0);
    EXPECT_FLOAT_EQ(r.w, 4 + 8 + 12 + 16, 0);
    /* (A B) v = A (B v) */
    de_mat4 b = de_m4_trs(v3(1, 2, 3), rnd_quat(), v3(2, 1, 0.5f));
    de_mat4 ab = de_m4_mul(&a, &b);
    de_vec4 v = v4(0.5f, -1, 2, 1);
    de_vec4 lhs = de_m4_mul_v4(&ab, v), rhs = de_m4_mul_v4(&a, de_m4_mul_v4(&b, v));
    EXPECT_FLOAT_EQ(lhs.x, rhs.x, 1e-4);
    EXPECT_FLOAT_EQ(lhs.y, rhs.y, 1e-4);
    EXPECT_FLOAT_EQ(lhs.z, rhs.z, 1e-4);
    EXPECT_FLOAT_EQ(lhs.w, rhs.w, 1e-4);
}

TEST(mat, inverse_random) {
    /* 10k well-conditioned matrices: random entries in [-1, 1] plus a diagonal of 4 (diagonally
     * dominant), and random TRS matrices. M * inverse(M) must be within 1e-5 of identity. */
    de_mat4 id = de_m4_identity();
    f32 worst = 0;
    for (int i = 0; i < 10000; ++i) {
        de_mat4 m;
        if (i % 2 == 0) {
            for (int k = 0; k < 16; ++k)
                m.m[k] = rnd(-1, 1);
            for (int k = 0; k < 4; ++k)
                m.m[k * 5] += rnd(0, 1) < 0.5f ? -4.0f : 4.0f;
        } else {
            m = de_m4_trs(rnd_v3(-10, 10), rnd_quat(), rnd_v3(0.5f, 2));
        }
        de_mat4 inv = de_m4_inverse(&m);
        de_mat4 p = de_m4_mul(&m, &inv), q = de_m4_mul(&inv, &m);
        worst = fmaxf(worst, fmaxf(max_diff(p.m, id.m, 16), max_diff(q.m, id.m, 16)));
    }
    EXPECT_TRUE(worst <= 1e-5f);
    /* Singular gives zero. */
    de_mat4 s = de_m4_identity();
    s.m[10] = 0;
    de_mat4 si = de_m4_inverse(&s);
    const f32 zeros[16] = {0};
    EXPECT_MEMEQ(si.m, zeros, sizeof zeros);
}

TEST(mat, inverse_affine) {
    de_mat4 id = de_m4_identity();
    for (int i = 0; i < 1000; ++i) {
        de_mat4 m = de_m4_trs(rnd_v3(-10, 10), rnd_quat(), rnd_v3(0.5f, 2));
        de_mat4 inv = de_m4_inverse_affine(&m), gen = de_m4_inverse(&m);
        de_mat4 p = de_m4_mul(&m, &inv);
        EXPECT_TRUE(max_diff(p.m, id.m, 16) <= 1e-5f);
        EXPECT_TRUE(max_diff(inv.m, gen.m, 16) <= 1e-4f);
        EXPECT_FLOAT_EQ(inv.m[15], 1, 0);
        EXPECT_FLOAT_EQ(inv.m[3], 0, 0);
    }
    de_mat4 s = de_m4_trs(v3(1, 2, 3), de_quat_identity(), v3(1, 0, 1));
    de_mat4 si = de_m4_inverse_affine(&s);
    const f32 zeros[16] = {0};
    EXPECT_MEMEQ(si.m, zeros, sizeof zeros);
}

TEST(mat, mul_point_dir) {
    de_mat4 m = de_m4_trs(v3(10, 20, 30), de_quat_axis_angle(DE_AXIS_UP, DE_PI / 2), v3(2, 2, 2));
    /* scale, then rotate +X to +Y, then translate */
    EXPECT_V3_NEAR(de_m4_mul_point(&m, v3(1, 0, 0)), v3(10, 22, 30), 1e-5);
    EXPECT_V3_NEAR(de_m4_mul_dir(&m, v3(1, 0, 0)), v3(0, 2, 0), 1e-5);
    /* Projective: divides by w. */
    de_mat4 p = de_m4_identity();
    p.m[15] = 2; /* w = 2 */
    EXPECT_V3_NEAR(de_m4_mul_point(&p, v3(2, 4, 6)), v3(1, 2, 3), 0);
}

TEST(mat, trs_decompose) {
    for (int i = 0; i < 1000; ++i) {
        de_vec3 t = rnd_v3(-100, 100), s = rnd_v3(0.1f, 10);
        de_quat r = rnd_quat();
        de_mat4 m = de_m4_trs(t, r, s);
        /* T * R * S on a point. */
        de_vec3 p = rnd_v3(-1, 1);
        EXPECT_V3_NEAR(de_m4_mul_point(&m, p), de_v3_add(t, de_quat_rotate(r, de_v3_mul(s, p))),
                       1e-3);
        de_vec3 t2, s2;
        de_quat r2;
        ASSERT_TRUE(de_m4_decompose(&m, &t2, &r2, &s2));
        EXPECT_V3_NEAR(t2, t, 0);
        EXPECT_V3_NEAR(s2, s, 1e-4 * 10);
        EXPECT_FLOAT_EQ(fabsf(de_quat_dot(r2, r)), 1, 1e-5);
    }
    /* A mirror is reported on x. */
    de_quat r = de_quat_axis_angle(DE_AXIS_UP, 0.5f);
    de_mat4 m = de_m4_trs(v3(0, 0, 0), r, v3(-2, 3, 4));
    de_vec3 s2;
    de_quat r2;
    ASSERT_TRUE(de_m4_decompose(&m, NULL, &r2, &s2));
    EXPECT_V3_NEAR(s2, v3(-2, 3, 4), 1e-5);
    EXPECT_FLOAT_EQ(fabsf(de_quat_dot(r2, r)), 1, 1e-6);
    /* Zero scale fails, with identity rotation. */
    m = de_m4_trs(v3(1, 2, 3), r, v3(1, 0, 1));
    EXPECT_FALSE(de_m4_decompose(&m, NULL, &r2, NULL));
    EXPECT_FLOAT_EQ(r2.w, 1, 0);
}

/* ---- look-at and projections ---- */

TEST(mat, look_at_basis) {
    /* Camera at the origin looking along world +Y (forward), up = world +Z. */
    de_mat4 v = de_m4_look_at(v3(0, 0, 0), v3(0, 1, 0), DE_AXIS_UP);
    EXPECT_V3_NEAR(de_m4_mul_point(&v, v3(0, 5, 0)), v3(0, 0, -5), 1e-6);
    EXPECT_V3_NEAR(de_m4_mul_dir(&v, DE_AXIS_RIGHT), v3(1, 0, 0), 1e-6);    /* world +X -> +X */
    EXPECT_V3_NEAR(de_m4_mul_dir(&v, DE_AXIS_UP), v3(0, 1, 0), 1e-6);       /* world +Z -> +Y */
    EXPECT_V3_NEAR(de_m4_mul_dir(&v, DE_AXIS_FORWARD), v3(0, 0, -1), 1e-6); /* +Y -> -Z */

    /* General camera: eye maps to the origin, target to -Z at the right distance, and the view
     * matrix is a rigid transform (rotation + translation). */
    for (int i = 0; i < 200; ++i) {
        de_vec3 eye = rnd_v3(-50, 50), target = rnd_v3(-50, 50);
        de_mat4 m = de_m4_look_at(eye, target, DE_AXIS_UP);
        f32 dist = de_v3_length(de_v3_sub(target, eye));
        EXPECT_V3_NEAR(de_m4_mul_point(&m, eye), v3(0, 0, 0), 1e-4);
        EXPECT_V3_NEAR(de_m4_mul_point(&m, target), v3(0, 0, -dist), 1e-3);
        de_vec3 up_view = de_m4_mul_dir(&m, DE_AXIS_UP);
        EXPECT_FLOAT_EQ(up_view.x, 0, 1e-5); /* no roll: world up has no view-X part */
        EXPECT_TRUE(up_view.y >= 0.0f);
        de_mat4 l = m;
        l.m[12] = l.m[13] = l.m[14] = 0;
        de_mat4 lt = de_m4_transpose(&l), p = de_m4_mul(&l, &lt), id = de_m4_identity();
        EXPECT_TRUE(max_diff(p.m, id.m, 16) <= 1e-5f);
    }

    /* Up parallel to the view direction falls back to another axis: still a valid rotation. */
    de_mat4 down = de_m4_look_at(v3(0, 0, 10), v3(0, 0, 0), DE_AXIS_UP);
    EXPECT_V3_NEAR(de_m4_mul_point(&down, v3(0, 0, 0)), v3(0, 0, -10), 1e-5);
    for (int i = 0; i < 16; ++i)
        EXPECT_TRUE(isfinite(down.m[i]));
    EXPECT_FLOAT_EQ(de_v3_length(de_m4_mul_dir(&down, DE_AXIS_RIGHT)), 1, 1e-6);
}

TEST(mat, perspective_rz_depth) {
    f32 near_z = 0.1f;
    de_mat4 p = de_m4_perspective_rz(DE_PI / 3, 16.0f / 9.0f, near_z);
    /* Near plane -> NDC z = 1. */
    de_vec4 c = de_m4_mul_v4(&p, v4(0.3f, -0.2f, -near_z, 1));
    EXPECT_FLOAT_EQ(c.z / c.w, 1, 1e-6);
    EXPECT_TRUE(c.w > 0.0f);
    /* 1e6 * near -> z < 1e-5, still > 0. */
    c = de_m4_mul_v4(&p, v4(0, 0, -1e6f * near_z, 1));
    EXPECT_TRUE(c.z / c.w < 1e-5f);
    EXPECT_TRUE(c.z / c.w > 0.0f);
    /* Monotonic: farther is smaller. */
    f32 prev = 2;
    for (f32 z = near_z; z < 1e5f; z *= 1.7f) {
        de_vec3 n = de_m4_mul_point(&p, v3(0, 0, -z));
        EXPECT_TRUE(n.z < prev);
        EXPECT_TRUE(n.z <= 1.0f);
        EXPECT_TRUE(n.z > 0.0f);
        prev = n.z;
    }
    /* Behind the camera: w < 0. */
    c = de_m4_mul_v4(&p, v4(0, 0, 1, 1));
    EXPECT_TRUE(c.w < 0.0f);
    c = de_m4_mul_v4(&p, v4(5, 5, 0.01f, 1));
    EXPECT_TRUE(c.w < 0.0f);
    /* fov / aspect: the top edge of the view cone maps to NDC y = 1, the right edge to x = 1. */
    f32 z = -10;
    f32 top = -z * tanf(DE_PI / 6);
    de_vec3 n = de_m4_mul_point(&p, v3(top * 16.0f / 9.0f, top, z));
    EXPECT_FLOAT_EQ(n.x, 1, 1e-5);
    EXPECT_FLOAT_EQ(n.y, 1, 1e-5);
}

TEST(mat, ortho_rz) {
    de_mat4 o = de_m4_ortho_rz(-4, 2, -1, 3, 0.5f, 100);
    EXPECT_V3_NEAR(de_m4_mul_point(&o, v3(-4, -1, -0.5f)), v3(-1, -1, 1), 1e-6);
    EXPECT_V3_NEAR(de_m4_mul_point(&o, v3(2, 3, -100)), v3(1, 1, 0), 1e-6);
    EXPECT_V3_NEAR(de_m4_mul_point(&o, v3(-1, 1, -50.25f)), v3(0, 0, 0.5f), 1e-6);
    de_vec4 c = de_m4_mul_v4(&o, v4(1, 1, -7, 1));
    EXPECT_FLOAT_EQ(c.w, 1, 0);
}

/* ---- de_affine ---- */

TEST(mat, affine) {
    de_affine id = de_affine_identity();
    EXPECT_V3_NEAR(de_affine_transform_point(&id, v3(1, 2, 3)), v3(1, 2, 3), 0);
    for (int i = 0; i < 500; ++i) {
        de_vec3 t = rnd_v3(-10, 10), s = rnd_v3(0.5f, 2);
        de_quat r = rnd_quat();
        de_affine a = de_affine_from_trs(t, r, s);
        de_mat4 m = de_m4_trs(t, r, s);
        de_mat4 am = de_affine_to_m4(&a);
        EXPECT_MEMEQ(am.m, m.m, sizeof m.m); /* same layout, same arithmetic */
        de_affine back = de_affine_from_m4(&m);
        EXPECT_MEMEQ(back.m, a.m, sizeof a.m);

        de_vec3 p = rnd_v3(-5, 5);
        EXPECT_V3_NEAR(de_affine_transform_point(&a, p), de_m4_mul_point(&m, p), 1e-5);
        EXPECT_V3_NEAR(de_affine_transform_dir(&a, p), de_m4_mul_dir(&m, p), 1e-5);

        de_affine b = de_affine_from_trs(rnd_v3(-10, 10), rnd_quat(), rnd_v3(0.5f, 2));
        de_affine ab = de_affine_mul(&a, &b);
        EXPECT_V3_NEAR(de_affine_transform_point(&ab, p),
                       de_affine_transform_point(&a, de_affine_transform_point(&b, p)), 1e-3);

        de_affine ai = de_affine_inverse(&a);
        de_affine one = de_affine_mul(&a, &ai);
        EXPECT_TRUE(max_diff(one.m, id.m, 12) <= 1e-5f);
    }
    /* Translation is column 3: m[9..11]. */
    de_affine tr = de_affine_from_trs(v3(7, 8, 9), de_quat_identity(), v3(1, 1, 1));
    EXPECT_FLOAT_EQ(tr.m[9], 7, 0);
    EXPECT_FLOAT_EQ(tr.m[11], 9, 0);
    EXPECT_V3_NEAR(de_affine_transform_dir(&tr, v3(1, 0, 0)), v3(1, 0, 0), 0);
    /* Singular gives zero. */
    de_affine sing = de_affine_from_trs(v3(1, 1, 1), de_quat_identity(), v3(0, 1, 1));
    de_affine si = de_affine_inverse(&sing);
    const f32 zeros[12] = {0};
    EXPECT_MEMEQ(si.m, zeros, sizeof zeros);
}
