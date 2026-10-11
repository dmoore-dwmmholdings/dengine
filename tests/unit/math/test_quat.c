/* test_quat.c: de_quat (DENG-241). Z-up, right-handed, Hamilton quaternions. */
#include "de_test.h"

#include <dengine/de_math.h>

#include <math.h>

static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 r = {x, y, z};
    return r;
}
static de_quat q4(f32 x, f32 y, f32 z, f32 w) {
    de_quat q;
    q.x = x;
    q.y = y;
    q.z = z;
    q.w = w;
    return q;
}

#define EXPECT_V3_NEAR(a, b, eps)                                                                  \
    do {                                                                                           \
        de_vec3 a_ = (a), b_ = (b);                                                                \
        EXPECT_FLOAT_EQ(a_.x, b_.x, eps);                                                          \
        EXPECT_FLOAT_EQ(a_.y, b_.y, eps);                                                          \
        EXPECT_FLOAT_EQ(a_.z, b_.z, eps);                                                          \
    } while (0)

/* Same rotation: q and -q are equal rotations, so compare |dot| with 1. */
#define EXPECT_SAME_ROTATION(a, b, eps) EXPECT_FLOAT_EQ(fabsf(de_quat_dot((a), (b))), 1.0, eps)

#define EXPECT_Q_BITS(a, b)                                                                        \
    do {                                                                                           \
        de_quat a_ = (a), b_ = (b);                                                                \
        EXPECT_MEMEQ(&a_.x, &b_.x, sizeof(f32) * 4);                                               \
    } while (0)

/* Deterministic xorshift32 so failures reproduce. */
static u32 g_rng = 0x9E3779B9u;
static f32 rnd(f32 lo, f32 hi) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return lo + (hi - lo) * (f32)(g_rng >> 8) * (1.0f / 16777216.0f);
}
static de_quat rnd_quat(void) {
    de_vec3 axis = de_v3_normalize_safe(v3(rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)), DE_AXIS_UP);
    return de_quat_axis_angle(axis, rnd(-DE_PI, DE_PI));
}

TEST(quat, identity) {
    de_quat q = de_quat_identity();
    EXPECT_FLOAT_EQ(q.x, 0, 0);
    EXPECT_FLOAT_EQ(q.w, 1, 0);
    EXPECT_V3_NEAR(de_quat_rotate(q, v3(1, 2, 3)), v3(1, 2, 3), 0);
}

TEST(quat, handedness_z_up) {
    /* +90 degrees about up turns right into forward (counter-clockwise seen from above). */
    de_quat q = de_quat_axis_angle(DE_AXIS_UP, DE_PI / 2);
    EXPECT_V3_NEAR(de_quat_rotate(q, DE_AXIS_RIGHT), DE_AXIS_FORWARD, 1e-6);
    EXPECT_V3_NEAR(de_quat_rotate(q, DE_AXIS_FORWARD), v3(-1, 0, 0), 1e-6);
    EXPECT_V3_NEAR(de_quat_rotate(q, DE_AXIS_UP), DE_AXIS_UP, 1e-6);
    /* Right-hand rule on the other axes. */
    EXPECT_V3_NEAR(de_quat_rotate(de_quat_axis_angle(DE_AXIS_RIGHT, DE_PI / 2), DE_AXIS_FORWARD),
                   DE_AXIS_UP, 1e-6);
    EXPECT_V3_NEAR(de_quat_rotate(de_quat_axis_angle(DE_AXIS_FORWARD, DE_PI / 2), DE_AXIS_UP),
                   DE_AXIS_RIGHT, 1e-6);
}

TEST(quat, axis_angle_unit) {
    for (int i = 0; i < 100; ++i) {
        de_quat q = rnd_quat();
        EXPECT_FLOAT_EQ(de_quat_dot(q, q), 1, 1e-6);
    }
    de_quat q = de_quat_axis_angle(DE_AXIS_RIGHT, DE_PI);
    EXPECT_FLOAT_EQ(q.x, 1, 1e-6);
    EXPECT_FLOAT_EQ(q.w, 0, 1e-6);
}

TEST(quat, from_euler_axes) {
    /* yaw about +Z turns forward toward -X (left). */
    EXPECT_V3_NEAR(de_quat_rotate(de_quat_from_euler(DE_PI / 2, 0, 0), DE_AXIS_FORWARD),
                   v3(-1, 0, 0), 1e-6);
    /* pitch about +X turns forward toward +Z (up). */
    EXPECT_V3_NEAR(de_quat_rotate(de_quat_from_euler(0, DE_PI / 2, 0), DE_AXIS_FORWARD), DE_AXIS_UP,
                   1e-6);
    /* roll about +Y (forward) turns up toward +X (right). */
    EXPECT_V3_NEAR(de_quat_rotate(de_quat_from_euler(0, 0, DE_PI / 2), DE_AXIS_UP), DE_AXIS_RIGHT,
                   1e-6);
    /* Order: q = yaw * pitch * roll, so roll applies first. */
    f32 y = 0.3f, p = -0.7f, r = 1.1f;
    de_quat expect = de_quat_mul(
        de_quat_mul(de_quat_axis_angle(DE_AXIS_UP, y), de_quat_axis_angle(DE_AXIS_RIGHT, p)),
        de_quat_axis_angle(DE_AXIS_FORWARD, r));
    EXPECT_SAME_ROTATION(de_quat_from_euler(y, p, r), expect, 1e-6);
    /* Intrinsic reading: after yaw 90, pitching up raises the yawed forward (-X) toward +Z. */
    EXPECT_V3_NEAR(de_quat_rotate(de_quat_from_euler(DE_PI / 2, DE_PI / 4, 0), DE_AXIS_FORWARD),
                   v3(-sqrtf(0.5f), 0, sqrtf(0.5f)), 1e-6);
}

TEST(quat, euler_roundtrip) {
    for (int i = 0; i < 1000; ++i) {
        f32 y = rnd(-3.1f, 3.1f), p = rnd(-1.5f, 1.5f), r = rnd(-3.1f, 3.1f);
        f32 y2, p2, r2;
        de_quat_to_euler(de_quat_from_euler(y, p, r), &y2, &p2, &r2);
        EXPECT_FLOAT_EQ(y2, y, 1e-4);
        EXPECT_FLOAT_EQ(p2, p, 1e-4);
        EXPECT_FLOAT_EQ(r2, r, 1e-4);
    }
    /* Gimbal lock: pitch +-90, roll folded into yaw; the rotation must still match. */
    for (int s = -1; s <= 1; s += 2) {
        de_quat q = de_quat_from_euler(0.4f, (f32)s * DE_PI / 2, 0.25f);
        f32 y2, p2, r2;
        de_quat_to_euler(q, &y2, &p2, &r2);
        EXPECT_FLOAT_EQ(p2, (f32)s * DE_PI / 2, 1e-3);
        EXPECT_FLOAT_EQ(r2, 0, 0);
        EXPECT_SAME_ROTATION(de_quat_from_euler(y2, p2, r2), q, 1e-5);
    }
    /* NULL outputs are allowed. */
    f32 only_pitch = 0;
    de_quat_to_euler(de_quat_from_euler(0, 0.5f, 0), NULL, &only_pitch, NULL);
    EXPECT_FLOAT_EQ(only_pitch, 0.5f, 1e-6);
}

TEST(quat, mul_composes_like_matrices) {
    for (int i = 0; i < 200; ++i) {
        de_quat a = rnd_quat(), b = rnd_quat();
        de_vec3 v = v3(rnd(-5, 5), rnd(-5, 5), rnd(-5, 5));
        /* (a * b) v = a (b v): b first. */
        EXPECT_V3_NEAR(de_quat_rotate(de_quat_mul(a, b), v),
                       de_quat_rotate(a, de_quat_rotate(b, v)), 1e-4);
        de_mat3 ma = de_quat_to_m3(a), mb = de_quat_to_m3(b);
        de_mat3 mab = de_m3_mul(&ma, &mb), qab = de_quat_to_m3(de_quat_mul(a, b));
        for (int k = 0; k < 9; ++k)
            EXPECT_FLOAT_EQ(mab.m[k], qab.m[k], 1e-5);
    }
    de_quat a = rnd_quat();
    EXPECT_Q_BITS(de_quat_mul(a, de_quat_identity()), a);
}

TEST(quat, conj_inverse_normalize) {
    de_quat a = rnd_quat();
    de_quat c = de_quat_conj(a);
    EXPECT_FLOAT_EQ(c.x, -a.x, 0);
    EXPECT_FLOAT_EQ(c.w, a.w, 0);
    EXPECT_SAME_ROTATION(de_quat_mul(a, c), de_quat_identity(), 1e-6);

    de_quat s = q4(a.x * 3, a.y * 3, a.z * 3, a.w * 3); /* non-unit */
    de_quat si = de_quat_inverse(s);
    de_quat one = de_quat_mul(s, si);
    EXPECT_FLOAT_EQ(one.x, 0, 1e-6);
    EXPECT_FLOAT_EQ(one.y, 0, 1e-6);
    EXPECT_FLOAT_EQ(one.z, 0, 1e-6);
    EXPECT_FLOAT_EQ(one.w, 1, 1e-6);

    de_quat n = de_quat_normalize(s);
    EXPECT_FLOAT_EQ(de_quat_dot(n, n), 1, 1e-6);
    EXPECT_SAME_ROTATION(n, a, 1e-6);
    EXPECT_Q_BITS(de_quat_normalize(q4(0, 0, 0, 0)), de_quat_identity());
    EXPECT_FLOAT_EQ(de_quat_dot(q4(1, 2, 3, 4), q4(5, 6, 7, 8)), 70, 0);
}

TEST(quat, rotate_matches_matrix) {
    for (int i = 0; i < 200; ++i) {
        de_quat q = rnd_quat();
        de_vec3 v = v3(rnd(-5, 5), rnd(-5, 5), rnd(-5, 5));
        de_mat3 m = de_quat_to_m3(q);
        EXPECT_V3_NEAR(de_quat_rotate(q, v), de_m3_mul_v3(&m, v), 1e-5);
        EXPECT_FLOAT_EQ(de_v3_length(de_quat_rotate(q, v)), de_v3_length(v), 1e-5);
    }
}

TEST(quat, slerp_endpoints_exact) {
    for (int i = 0; i < 100; ++i) {
        de_quat a = rnd_quat(), b = rnd_quat();
        if (de_quat_dot(a, b) < 0)
            b = q4(-b.x, -b.y, -b.z, -b.w);
        EXPECT_Q_BITS(de_quat_slerp(a, b, 0.0f), a);
        EXPECT_Q_BITS(de_quat_slerp(a, b, 1.0f), b);
    }
}

TEST(quat, slerp_shortest) {
    /* b2 = -b is the same rotation as b. slerp must take the short arc either way. */
    de_quat a = de_quat_identity();
    de_quat b = de_quat_axis_angle(DE_AXIS_UP, DE_PI / 2);
    de_quat b2 = q4(-b.x, -b.y, -b.z, -b.w);
    de_quat mid = de_quat_axis_angle(DE_AXIS_UP, DE_PI / 4);
    EXPECT_SAME_ROTATION(de_quat_slerp(a, b, 0.5f), mid, 1e-6);
    EXPECT_SAME_ROTATION(de_quat_slerp(a, b2, 0.5f), mid, 1e-6);
    EXPECT_SAME_ROTATION(de_quat_slerp(a, b2, 1.0f), b, 1e-6);
    EXPECT_TRUE(de_quat_dot(de_quat_slerp(a, b2, 0.5f), a) >= 0); /* stays on a's hemisphere */
    /* Constant angular speed: t = 0.25 of 90 degrees is 22.5 degrees. */
    de_vec3 r = de_quat_rotate(de_quat_slerp(a, b, 0.25f), DE_AXIS_RIGHT);
    EXPECT_FLOAT_EQ(atan2f(r.y, r.x), DE_PI / 8, 1e-6);
    /* Nearly parallel inputs (the lerp fallback) stay unit length. */
    de_quat c = de_quat_axis_angle(DE_AXIS_UP, 1e-4f);
    de_quat s = de_quat_slerp(a, c, 0.5f);
    EXPECT_FLOAT_EQ(de_quat_dot(s, s), 1, 1e-6);
    EXPECT_SAME_ROTATION(s, de_quat_axis_angle(DE_AXIS_UP, 5e-5f), 1e-6);
}

TEST(quat, nlerp) {
    de_quat a = de_quat_identity();
    de_quat b = de_quat_axis_angle(DE_AXIS_RIGHT, DE_PI / 2);
    de_quat b2 = q4(-b.x, -b.y, -b.z, -b.w);
    de_quat n = de_quat_nlerp(a, b2, 0.5f);
    EXPECT_FLOAT_EQ(de_quat_dot(n, n), 1, 1e-6);
    EXPECT_SAME_ROTATION(n, de_quat_axis_angle(DE_AXIS_RIGHT, DE_PI / 4), 1e-6); /* symmetric */
    EXPECT_SAME_ROTATION(de_quat_nlerp(a, b, 1.0f), b, 1e-6);
}

TEST(quat, m3_roundtrip) {
    for (int i = 0; i < 1000; ++i) {
        de_quat q = rnd_quat();
        de_mat3 m = de_quat_to_m3(q);
        EXPECT_SAME_ROTATION(de_quat_from_m3(&m), q, 1e-5);
        de_mat3 m2 = de_m3_from_quat(q);
        EXPECT_MEMEQ(m.m, m2.m, sizeof m.m);
    }
    /* Every Shepperd branch: 180 degrees about X, Y and Z (trace -1) and identity. */
    const de_vec3 axes[3] = {DE_AXIS_RIGHT, DE_AXIS_FORWARD, DE_AXIS_UP};
    for (int i = 0; i < 3; ++i) {
        de_quat q = de_quat_axis_angle(axes[i], DE_PI);
        de_mat3 m = de_quat_to_m3(q);
        EXPECT_SAME_ROTATION(de_quat_from_m3(&m), q, 1e-6);
    }
    de_mat3 id = de_m3_identity();
    EXPECT_SAME_ROTATION(de_quat_from_m3(&id), de_quat_identity(), 1e-6);
}
