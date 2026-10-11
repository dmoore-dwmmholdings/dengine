/* test_vec.c: de_vec2 / de_vec3 / de_vec4 (DENG-241). */
#include "de_test.h"

#include <dengine/de_math.h>

#include <math.h>

#define EPS 1e-6

static de_vec2 v2(f32 x, f32 y) {
    de_vec2 r = {x, y};
    return r;
}
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

#define EXPECT_V2(a, ex, ey)                                                                       \
    do {                                                                                           \
        de_vec2 a_ = (a);                                                                          \
        EXPECT_FLOAT_EQ(a_.x, (ex), EPS);                                                          \
        EXPECT_FLOAT_EQ(a_.y, (ey), EPS);                                                          \
    } while (0)
#define EXPECT_V3(a, ex, ey, ez)                                                                   \
    do {                                                                                           \
        de_vec3 a_ = (a);                                                                          \
        EXPECT_FLOAT_EQ(a_.x, (ex), EPS);                                                          \
        EXPECT_FLOAT_EQ(a_.y, (ey), EPS);                                                          \
        EXPECT_FLOAT_EQ(a_.z, (ez), EPS);                                                          \
    } while (0)
#define EXPECT_V4(a, ex, ey, ez, ew)                                                               \
    do {                                                                                           \
        de_vec4 a_ = (a);                                                                          \
        EXPECT_FLOAT_EQ(a_.x, (ex), EPS);                                                          \
        EXPECT_FLOAT_EQ(a_.y, (ey), EPS);                                                          \
        EXPECT_FLOAT_EQ(a_.z, (ez), EPS);                                                          \
        EXPECT_FLOAT_EQ(a_.w, (ew), EPS);                                                          \
    } while (0)

TEST(vec, v2_arithmetic) {
    de_vec2 a = v2(1, -2), b = v2(3, 4);
    EXPECT_V2(de_v2_add(a, b), 4, 2);
    EXPECT_V2(de_v2_sub(a, b), -2, -6);
    EXPECT_V2(de_v2_mul(a, b), 3, -8);
    EXPECT_V2(de_v2_scale(a, -2), -2, 4);
    EXPECT_FLOAT_EQ(de_v2_dot(a, b), -5, EPS);
    EXPECT_FLOAT_EQ(de_v2_length(b), 5, EPS);
    EXPECT_FLOAT_EQ(de_v2_length_sq(b), 25, EPS);
    EXPECT_V2(de_v2_lerp(a, b, 0.25f), 1.5f, -0.5f);
    EXPECT_V2(de_v2_min(a, b), 1, -2);
    EXPECT_V2(de_v2_max(a, b), 3, 4);
    EXPECT_V2(de_v2_abs(a), 1, 2);
}

TEST(vec, v2_normalize_and_eq) {
    EXPECT_V2(de_v2_normalize(v2(3, 4)), 0.6f, 0.8f);
    EXPECT_V2(de_v2_normalize_safe(v2(0, 0), v2(1, 0)), 1, 0);
    EXPECT_V2(de_v2_normalize_safe(v2(0, -2), v2(1, 0)), 0, -1);
    EXPECT_TRUE(de_v2_eq_eps(v2(1, 2), v2(1.05f, 1.95f), 0.1f));
    EXPECT_FALSE(de_v2_eq_eps(v2(1, 2), v2(1.2f, 2), 0.1f));
}

TEST(vec, v3_arithmetic) {
    de_vec3 a = v3(1, -2, 3), b = v3(4, 5, -6);
    EXPECT_V3(de_v3_add(a, b), 5, 3, -3);
    EXPECT_V3(de_v3_sub(a, b), -3, -7, 9);
    EXPECT_V3(de_v3_mul(a, b), 4, -10, -18);
    EXPECT_V3(de_v3_scale(a, 0.5f), 0.5f, -1, 1.5f);
    EXPECT_FLOAT_EQ(de_v3_dot(a, b), 4 - 10 - 18, EPS);
    EXPECT_FLOAT_EQ(de_v3_length_sq(a), 14, EPS);
    EXPECT_FLOAT_EQ(de_v3_length(v3(2, 3, 6)), 7, EPS);
    EXPECT_V3(de_v3_lerp(a, b, 0.5f), 2.5f, 1.5f, -1.5f);
    EXPECT_V3(de_v3_lerp(a, b, 2.0f), 7, 12, -15); /* not clamped */
    EXPECT_V3(de_v3_min(a, b), 1, -2, -6);
    EXPECT_V3(de_v3_max(a, b), 4, 5, 3);
    EXPECT_V3(de_v3_abs(a), 1, 2, 3);
}

TEST(vec, v3_cross_right_handed) {
    /* Z-up right-handed: right x forward = up, forward x up = right, up x right = forward. */
    EXPECT_V3(de_v3_cross(DE_AXIS_RIGHT, DE_AXIS_FORWARD), 0, 0, 1);
    EXPECT_V3(de_v3_cross(DE_AXIS_FORWARD, DE_AXIS_UP), 1, 0, 0);
    EXPECT_V3(de_v3_cross(DE_AXIS_UP, DE_AXIS_RIGHT), 0, 1, 0);
    de_vec3 a = v3(1, 2, 3), b = v3(-4, 0.5f, 2);
    de_vec3 c = de_v3_cross(a, b);
    EXPECT_FLOAT_EQ(de_v3_dot(c, a), 0, 1e-5);
    EXPECT_FLOAT_EQ(de_v3_dot(c, b), 0, 1e-5);
    EXPECT_V3(de_v3_cross(a, a), 0, 0, 0);
}

TEST(vec, v3_normalize_and_eq) {
    EXPECT_V3(de_v3_normalize(v3(0, 3, 4)), 0, 0.6f, 0.8f);
    EXPECT_FLOAT_EQ(de_v3_length(de_v3_normalize(v3(-7, 1e-3f, 12))), 1, 1e-6);
    EXPECT_V3(de_v3_normalize_safe(v3(0, 0, 0), DE_AXIS_UP), 0, 0, 1);
    EXPECT_V3(de_v3_normalize_safe(v3(INFINITY, 0, 0), DE_AXIS_UP), 0, 0, 1);
    EXPECT_V3(de_v3_normalize_safe(v3(5, 0, 0), DE_AXIS_UP), 1, 0, 0);
    EXPECT_TRUE(de_v3_eq_eps(v3(1, 2, 3), v3(1, 2, 3), 0));
    EXPECT_TRUE(de_v3_eq_eps(v3(1, 2, 3), v3(1.01f, 1.99f, 3), 0.02f));
    EXPECT_FALSE(de_v3_eq_eps(v3(1, 2, 3), v3(1, 2, 3.1f), 0.05f));
}

TEST(vec, v4_arithmetic) {
    de_vec4 a = v4(1, -2, 3, -4), b = v4(2, 2, -1, 0.5f);
    EXPECT_V4(de_v4_add(a, b), 3, 0, 2, -3.5f);
    EXPECT_V4(de_v4_sub(a, b), -1, -4, 4, -4.5f);
    EXPECT_V4(de_v4_mul(a, b), 2, -4, -3, -2);
    EXPECT_V4(de_v4_scale(a, 2), 2, -4, 6, -8);
    EXPECT_FLOAT_EQ(de_v4_dot(a, b), 2 - 4 - 3 - 2, EPS);
    EXPECT_FLOAT_EQ(de_v4_length_sq(a), 30, EPS);
    EXPECT_FLOAT_EQ(de_v4_length(v4(1, 1, 1, 1)), 2, EPS);
    EXPECT_V4(de_v4_lerp(a, b, 1.0f), 2, 2, -1, 0.5f);
    EXPECT_V4(de_v4_min(a, b), 1, -2, -1, -4);
    EXPECT_V4(de_v4_max(a, b), 2, 2, 3, 0.5f);
    EXPECT_V4(de_v4_abs(a), 1, 2, 3, 4);
}

TEST(vec, v4_normalize_and_eq) {
    EXPECT_V4(de_v4_normalize(v4(2, 0, 0, 0)), 1, 0, 0, 0);
    EXPECT_V4(de_v4_normalize(v4(1, 1, 1, 1)), 0.5f, 0.5f, 0.5f, 0.5f);
    EXPECT_V4(de_v4_normalize_safe(v4(0, 0, 0, 0), v4(0, 0, 0, 1)), 0, 0, 0, 1);
    EXPECT_TRUE(de_v4_eq_eps(v4(1, 2, 3, 4), v4(1, 2, 3, 4.001f), 0.01f));
    EXPECT_FALSE(de_v4_eq_eps(v4(1, 2, 3, 4), v4(1, 2, 3, 4.1f), 0.01f));
}

TEST(vec, type_layout) {
    /* The SIMD paths (DENG-242) and GPU uploads rely on these sizes and alignments. */
    EXPECT_EQ(sizeof(de_vec3), 12u);
    EXPECT_EQ(sizeof(de_vec4), 16u);
    EXPECT_EQ(DE_ALIGNOF(de_vec4), 16u);
    EXPECT_EQ(sizeof(de_quat), 16u);
    EXPECT_EQ(sizeof(de_mat3), 36u);
    EXPECT_EQ(sizeof(de_mat4), 64u);
    EXPECT_EQ(DE_ALIGNOF(de_mat4), 16u);
    EXPECT_EQ(sizeof(de_affine), 48u);
    EXPECT_EQ(sizeof(de_plane), 16u);
}
