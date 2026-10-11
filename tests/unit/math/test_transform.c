/* test_transform.c: large-world transforms, de_dvec3 and de_transform (DENG-244, SPEC §5.5).
 * Worlds reach +-100 km (DENG-2 Q10); positions there must keep sub-micrometre precision.
 *
 * transform.golden_bitwise (DENG-364) compares de_dv3_relative and the deterministic
 * de_transform_* functions against tests/unit/math/vectors/transform_golden.txt, bit for bit.
 * Like dmath_golden.txt, that file is the cross-platform contract: x86-64 and arm64 must
 * reproduce it without regenerating it. A mismatch is a determinism bug. Only a deliberate change
 * of the formulas (with a reason in the commit message) rewrites it:
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/math/test_math --filter=transform.golden_bitwise
 *
 * File format: a section per function, "@ <name> <inputs> <outputs> <types> <count>", then
 * <count> lines of hex bit patterns, the inputs then the outputs. <types> has one letter per
 * value: f = f32 (8 hex digits), d = f64 (16 hex digits). A de_transform is 10 values, "dddfffffff"
 * (position, rotation x y z w, scale). */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen */
#endif
#include "de_test.h"
#include "fpenv_util.h"

#include <dengine/de_math.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_TRANSFORM_GOLDEN
#error "DE_TRANSFORM_GOLDEN (path of transform_golden.txt) must be defined by the build"
#endif

#define KM 1000.0

/* Float comparisons that print both values when they fail (EXPECT_LT and friends take integers
 * and pointers only). */
#define EXPECT_DCMP(a, op, b)                                                                      \
    do {                                                                                           \
        f64 a_ = (f64)(a), b_ = (f64)(b);                                                          \
        if (!(a_ op b_))                                                                           \
            printf("  %s = %.9g, expected %s %.9g\n", #a, a_, #op, b_);                            \
        EXPECT_TRUE(a_ op b_);                                                                     \
    } while (0)
#define EXPECT_DLT(a, b) EXPECT_DCMP(a, <, b)
#define EXPECT_DLE(a, b) EXPECT_DCMP(a, <=, b)
#define EXPECT_DGT(a, b) EXPECT_DCMP(a, >, b)

static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 r = {x, y, z};
    return r;
}
static de_dvec3 dv3(f64 x, f64 y, f64 z) {
    de_dvec3 r = {x, y, z};
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
static de_transform tr(de_dvec3 p, de_quat r, de_vec3 s) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position = p;
    t.rotation = r;
    t.scale = s;
    return t;
}

/* Largest component difference. */
static f64 dv3_err(de_dvec3 a, de_dvec3 b) {
    f64 e = fabs(a.x - b.x);
    if (fabs(a.y - b.y) > e)
        e = fabs(a.y - b.y);
    if (fabs(a.z - b.z) > e)
        e = fabs(a.z - b.z);
    return e;
}
static f64 v3_err(de_vec3 a, de_vec3 b) {
    return dv3_err(de_dv3_from_v3(a), de_dv3_from_v3(b));
}
/* 1 - |dot|: 0 for the same rotation (q and -q are equal rotations). */
static f64 rot_err(de_quat a, de_quat b) {
    return 1.0 - fabs((f64)a.x * b.x + (f64)a.y * b.y + (f64)a.z * b.z + (f64)a.w * b.w);
}

/* Deterministic xorshift64 so failures reproduce. */
static u64 g_rng = 0x9E3779B97F4A7C15ull;
static f64 rnd(f64 lo, f64 hi) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return lo + (hi - lo) * (f64)(g_rng >> 11) * (1.0 / 9007199254740992.0);
}
static de_dvec3 rnd_dv3(f64 r) {
    return dv3(rnd(-r, r), rnd(-r, r), rnd(-r, r));
}
static de_quat rnd_quat(void) {
    de_vec3 axis =
        de_v3_normalize_safe(v3((f32)rnd(-1, 1), (f32)rnd(-1, 1), (f32)rnd(-1, 1)), DE_AXIS_UP);
    return de_quat_axis_angle(axis, (f32)rnd(-DE_PI, DE_PI));
}
static de_vec3 rnd_scale(void) {
    return v3((f32)rnd(0.1, 10.0), (f32)rnd(0.1, 10.0), (f32)rnd(0.1, 10.0));
}
static de_vec3 rnd_uniform_scale(f64 lo, f64 hi) {
    f32 s = (f32)rnd(lo, hi);
    return v3(s, s, s);
}

TEST(transform, dv3_ops) {
    de_dvec3 a = dv3(1, 2, 3), b = dv3(4, -6, 8);
    EXPECT_FLOAT_EQ(dv3_err(de_dv3_add(a, b), dv3(5, -4, 11)), 0.0, 0);
    EXPECT_FLOAT_EQ(dv3_err(de_dv3_sub(a, b), dv3(-3, 8, -5)), 0.0, 0);
    EXPECT_FLOAT_EQ(dv3_err(de_dv3_scale(a, -2.0), dv3(-2, -4, -6)), 0.0, 0);
    EXPECT_FLOAT_EQ(de_dv3_dot(a, b), 4.0 - 12.0 + 24.0, 0);
    EXPECT_FLOAT_EQ(de_dv3_length(dv3(2, 3, 6)), 7.0, 0);
    EXPECT_FLOAT_EQ(de_dv3_distance(dv3(100 * KM, 0, 0), dv3(100 * KM + 3, 4, 0)), 5.0, 0);

    /* lerp is exact at both ends, even 100 km out where a + (b - a) * 1 can miss b. */
    de_dvec3 p = dv3(100 * KM + 0.1, -73 * KM - 0.3, 1e-3);
    de_dvec3 q = dv3(-99 * KM + 0.7, 100 * KM - 0.9, 7.1);
    EXPECT_FLOAT_EQ(dv3_err(de_dv3_lerp(p, q, 0.0), p), 0.0, 0);
    EXPECT_FLOAT_EQ(dv3_err(de_dv3_lerp(p, q, 1.0), q), 0.0, 0);
    EXPECT_DLT(dv3_err(de_dv3_lerp(p, q, 0.5), de_dv3_scale(de_dv3_add(p, q), 0.5)), 1e-10);

    de_vec3 f = v3(0.1f, -2.5f, 1e30f);
    de_dvec3 fd = de_dv3_from_v3(f);
    EXPECT_FLOAT_EQ(fd.x, (f64)0.1f, 0);
    EXPECT_FLOAT_EQ(fd.z, (f64)1e30f, 0);
    EXPECT_FLOAT_EQ(v3_err(de_dv3_to_v3(fd), f), 0.0, 0);

    EXPECT_TRUE(de_dv3_eq_eps(a, dv3(1.0 + 1e-10, 2.0, 3.0 - 1e-10), 1e-9));
    EXPECT_FALSE(de_dv3_eq_eps(a, dv3(1.0, 2.0 + 1e-8, 3.0), 1e-9));
    EXPECT_FALSE(de_dv3_eq_eps(a, dv3(NAN, 2.0, 3.0), 1e9));
}

TEST(transform, relative_precision_100km) {
    /* The acceptance case: object 1 mm from a camera at (100 km, 100 km, 0). */
    de_dvec3 cam = dv3(100 * KM, 100 * KM, 0);
    de_dvec3 obj = dv3(100 * KM + 0.001, 100 * KM, 0);
    de_vec3 rel = de_dv3_relative(obj, cam);
    EXPECT_DLT(fabs((f64)rel.x - 0.001), 1e-6);
    EXPECT_FLOAT_EQ(rel.y, 0.0f, 0);
    EXPECT_FLOAT_EQ(rel.z, 0.0f, 0);

    /* The float path this function exists to avoid: at 100 km an f32 step is 7.8 mm, so the
     * 1 mm offset vanishes. (Guards the test itself: the case must be one that floats fail.) */
    de_vec3 naive = de_v3_sub(de_dv3_to_v3(obj), de_dv3_to_v3(cam));
    EXPECT_DGT(fabs((f64)naive.x - 0.001), 1e-4);

    /* Random positions anywhere in +-100 km, camera within 1 km of them: the error is the f32
     * rounding of the (small) difference only. */
    f64 worst = 0.0;
    for (int i = 0; i < 10000; ++i) {
        de_dvec3 c = rnd_dv3(100 * KM);
        de_dvec3 d = rnd_dv3(i % 2 ? 1 * KM : 0.01);
        de_dvec3 o = de_dv3_add(c, d);
        de_vec3 r = de_dv3_relative(o, c);
        de_dvec3 exact = de_dv3_sub(o, c);
        f64 tol = de_dv3_length(exact) * 1.2e-7 + 1e-10; /* half an f32 ulp, plus slack */
        f64 e = dv3_err(de_dv3_from_v3(r), exact);
        if (e / tol > worst)
            worst = e / tol;
    }
    printf("relative_precision_100km: worst error / tolerance = %.3f\n", worst);
    EXPECT_DLE(worst, 1.0);
}

TEST(transform, mul_inverse) {
    f64 pos_left = 0, rot_left = 0, scale_left = 0, pos_right_unit = 0, pos_right_uniform = 0;
    for (int i = 0; i < 10000; ++i) {
        /* Any scale, non-uniform included: inverse(t) * t is the identity. */
        de_transform t = tr(rnd_dv3(100 * KM), rnd_quat(), rnd_scale());
        de_transform inv = de_transform_inverse(&t);
        de_transform id = de_transform_mul(&inv, &t);
        f64 e = de_dv3_length(id.position);
        if (e > pos_left)
            pos_left = e;
        e = rot_err(id.rotation, de_quat_identity());
        if (e > rot_left)
            rot_left = e;
        e = v3_err(id.scale, v3(1, 1, 1));
        if (e > scale_left)
            scale_left = e;

        /* Scale 1: t * inverse(t) is the identity too, within f64 rounding. */
        de_transform u = tr(t.position, t.rotation, v3(1, 1, 1));
        de_transform uinv = de_transform_inverse(&u);
        de_transform uid = de_transform_mul(&u, &uinv);
        e = de_dv3_length(uid.position);
        if (e > pos_right_unit)
            pos_right_unit = e;

        /* Uniform scale: limited by 1 / scale rounded to f32 (about 6e-8 relative). */
        de_transform s = tr(t.position, t.rotation, rnd_uniform_scale(0.1, 10.0));
        de_transform sinv = de_transform_inverse(&s);
        de_transform sid = de_transform_mul(&s, &sinv);
        e = de_dv3_length(sid.position) / de_dv3_length(t.position);
        if (e > pos_right_uniform)
            pos_right_uniform = e;
    }
    printf("mul_inverse: inv*t pos %.3g m, rot %.3g, scale %.3g; t*inv pos %.3g m (scale 1), "
           "%.3g relative (uniform scale)\n",
           pos_left, rot_left, scale_left, pos_right_unit, pos_right_uniform);
    EXPECT_DLT(pos_left, 1e-9);
    EXPECT_DLT(rot_left, 1e-6);
    EXPECT_DLT(scale_left, 1e-6);
    EXPECT_DLT(pos_right_unit, 1e-9);
    EXPECT_DLT(pos_right_uniform, 2.5e-7);

    /* Inverse of the identity is the identity, exactly. */
    de_transform i0 = de_transform_identity();
    de_transform i1 = de_transform_inverse(&i0);
    EXPECT_FLOAT_EQ(de_dv3_length(i1.position), 0.0, 0);
    EXPECT_FLOAT_EQ(rot_err(i1.rotation, i0.rotation), 0.0, 0);
    EXPECT_FLOAT_EQ(v3_err(i1.scale, i0.scale), 0.0, 0);
}

TEST(transform, point_and_dir) {
    /* +90 degrees about up, scale 2, at 100 km: local +X goes to world +Y. */
    de_dvec3 base = dv3(100 * KM, -100 * KM, 50);
    de_transform t = tr(base, de_quat_axis_angle(DE_AXIS_UP, DE_PI / 2), v3(2, 2, 2));
    de_dvec3 p = de_transform_point(&t, dv3(0.001, 0, 0));
    EXPECT_DLT(dv3_err(p, dv3(base.x, base.y + 0.002, base.z)), 1e-9);
    de_vec3 d = de_transform_dir(&t, v3(1, 0, 0));
    EXPECT_DLT(v3_err(d, v3(0, 2, 0)), 1e-6);

    /* A non-unit f32 rotation does not stretch a long lever arm: the rotation is normalized in
     * f64. |q| = 1 + 1e-4 would add 20 m over 100 km if used as is. */
    de_quat r = rnd_quat();
    de_quat rs = q4(r.x * 1.0001f, r.y * 1.0001f, r.z * 1.0001f, r.w * 1.0001f);
    de_transform a = tr(dv3(0, 0, 0), r, v3(1, 1, 1));
    de_transform b = tr(dv3(0, 0, 0), rs, v3(1, 1, 1));
    de_dvec3 far = dv3(100 * KM, 0, 0);
    EXPECT_DLT(fabs(de_dv3_length(de_transform_point(&b, far)) - 100 * KM), 1e-8);
    EXPECT_DLT(dv3_err(de_transform_point(&a, far), de_transform_point(&b, far)), 0.05);

    /* point agrees with the matrix of the transform (relative to its own position). */
    de_transform m = tr(rnd_dv3(100 * KM), rnd_quat(), rnd_scale());
    de_mat4 mm = de_transform_to_m4_relative(&m, m.position);
    de_vec3 lp = v3(1.5f, -2.0f, 0.25f);
    de_dvec3 wp = de_transform_point(&m, de_dv3_from_v3(lp));
    EXPECT_DLT(v3_err(de_dv3_relative(wp, m.position), de_m4_mul_point(&mm, lp)), 1e-4);
    EXPECT_DLT(v3_err(de_transform_dir(&m, lp), de_m4_mul_dir(&mm, lp)), 1e-4);
}

TEST(transform, m4_relative_matches) {
    f64 worst_m4 = 0, worst_aff = 0, worst_pt = 0;
    for (int i = 0; i < 2000; ++i) {
        de_transform t = tr(rnd_dv3(100 * KM), rnd_quat(), rnd_scale());
        de_dvec3 origin = de_dv3_add(t.position, rnd_dv3(1 * KM));
        de_mat4 got = de_transform_to_m4_relative(&t, origin);
        de_mat4 want = de_m4_trs(de_dv3_relative(t.position, origin), t.rotation, t.scale);
        for (int k = 0; k < 16; ++k) {
            f64 e = fabs((f64)got.m[k] - want.m[k]);
            if (e > worst_m4)
                worst_m4 = e;
        }
        de_affine ga = de_transform_to_affine_relative(&t, origin);
        de_mat4 gam = de_affine_to_m4(&ga);
        for (int k = 0; k < 16; ++k) {
            f64 e = fabs((f64)gam.m[k] - want.m[k]);
            if (e > worst_aff)
                worst_aff = e;
        }
        /* A local point through the relative matrix lands where the f64 path puts it, relative
         * to the origin: 1 km of f32 plus 10 m of local offset, about 1e-4 m of rounding. */
        de_vec3 lp = v3((f32)rnd(-1, 1), (f32)rnd(-1, 1), (f32)rnd(-1, 1));
        de_vec3 via_m4 = de_m4_mul_point(&got, lp);
        de_vec3 via_f64 = de_dv3_relative(de_transform_point(&t, de_dv3_from_v3(lp)), origin);
        f64 e = v3_err(via_m4, via_f64);
        if (e > worst_pt)
            worst_pt = e;
    }
    printf("m4_relative_matches: m4 %.3g, affine %.3g, point %.3g m\n", worst_m4, worst_aff,
           worst_pt);
    EXPECT_DLE(worst_m4, 1e-6);
    EXPECT_DLE(worst_aff, 1e-6);
    EXPECT_DLT(worst_pt, 5e-4);

    /* The acceptance case once more as a matrix: 1 mm from a camera 100 km out. */
    de_dvec3 cam = dv3(100 * KM, 100 * KM, 0);
    de_transform obj = tr(dv3(100 * KM + 0.001, 100 * KM, 0), de_quat_identity(), v3(1, 1, 1));
    de_mat4 m = de_transform_to_m4_relative(&obj, cam);
    EXPECT_DLT(fabs((f64)m.m[12] - 0.001), 1e-6);
    EXPECT_FLOAT_EQ(m.m[13], 0.0f, 0);

    /* from_affine undoes to_affine_relative (to f32 rounding of the relative part). */
    for (int i = 0; i < 200; ++i) {
        de_transform t = tr(rnd_dv3(100 * KM), rnd_quat(), rnd_scale());
        de_dvec3 origin = de_dv3_add(t.position, rnd_dv3(100.0));
        de_affine a = de_transform_to_affine_relative(&t, origin);
        de_transform back;
        ASSERT_TRUE(de_transform_from_affine(&a, origin, &back));
        EXPECT_DLT(dv3_err(back.position, t.position), 1e-4);
        EXPECT_DLT(rot_err(back.rotation, t.rotation), 1e-5);
        EXPECT_DLT(v3_err(back.scale, t.scale), 1e-4);
    }
    de_affine zero = {{0}};
    de_transform z;
    EXPECT_FALSE(de_transform_from_affine(&zero, dv3(1, 2, 3), &z));
    EXPECT_FLOAT_EQ(rot_err(z.rotation, de_quat_identity()), 0.0, 0);
    EXPECT_FLOAT_EQ(dv3_err(z.position, dv3(1, 2, 3)), 0.0, 0);
}

TEST(transform, lerp_endpoints) {
    for (int i = 0; i < 1000; ++i) {
        de_transform a = tr(rnd_dv3(100 * KM), rnd_quat(), rnd_scale());
        de_transform b = tr(rnd_dv3(100 * KM), rnd_quat(), rnd_scale());
        de_transform l0 = de_transform_lerp(&a, &b, 0.0f);
        de_transform l1 = de_transform_lerp(&a, &b, 1.0f);
        /* Position and scale exact at both ends; rotation the same rotation. */
        EXPECT_FLOAT_EQ(dv3_err(l0.position, a.position), 0.0, 0);
        EXPECT_FLOAT_EQ(dv3_err(l1.position, b.position), 0.0, 0);
        EXPECT_FLOAT_EQ(v3_err(l0.scale, a.scale), 0.0, 0);
        EXPECT_FLOAT_EQ(v3_err(l1.scale, b.scale), 0.0, 0);
        EXPECT_DLT(rot_err(l0.rotation, a.rotation), 1e-6);
        EXPECT_DLT(rot_err(l1.rotation, b.rotation), 1e-6);
        de_transform lh = de_transform_lerp(&a, &b, 0.5f);
        EXPECT_DLT(dv3_err(lh.position, de_dv3_lerp(a.position, b.position, 0.5)), 1e-9);
        EXPECT_DLT(rot_err(lh.rotation, de_quat_nlerp(a.rotation, b.rotation, 0.5f)), 1e-6);
    }

    /* Shortest path: b and -b interpolate the same way. */
    de_quat qa = de_quat_identity();
    de_quat qb = de_quat_axis_angle(DE_AXIS_UP, DE_PI / 2);
    de_transform a = tr(dv3(0, 0, 0), qa, v3(1, 1, 1));
    de_transform b = tr(dv3(0, 0, 0), qb, v3(1, 1, 1));
    de_transform bn = tr(dv3(0, 0, 0), q4(-qb.x, -qb.y, -qb.z, -qb.w), v3(1, 1, 1));
    de_transform m1 = de_transform_lerp(&a, &b, 0.5f);
    de_transform m2 = de_transform_lerp(&a, &bn, 0.5f);
    de_quat q45 = de_quat_axis_angle(DE_AXIS_UP, DE_PI / 4);
    EXPECT_DLT(rot_err(m1.rotation, q45), 1e-6);
    EXPECT_DLT(rot_err(m2.rotation, q45), 1e-6);
    EXPECT_DGT(m2.rotation.w, 0.0f); /* followed a, not -b */
}

TEST(transform, compose_hierarchy_3_levels) {
    /* root (100 km out) -> body -> hand, uniform scales: composing is exact up to the f32
     * rounding of the composed rotations, and it does not depend on how far out the root is. */
    f64 worst_assoc = 0, worst_chain = 0, worst_shift = 0, worst_rot = 0;
    for (int i = 0; i < 2000; ++i) {
        de_transform a = tr(rnd_dv3(100 * KM), rnd_quat(), rnd_uniform_scale(0.5, 2.0));
        de_transform b = tr(rnd_dv3(100.0), rnd_quat(), rnd_uniform_scale(0.5, 2.0));
        de_transform c = tr(rnd_dv3(10.0), rnd_quat(), rnd_uniform_scale(0.5, 2.0));
        de_transform ab = de_transform_mul(&a, &b);
        de_transform bc = de_transform_mul(&b, &c);
        de_transform w1 = de_transform_mul(&ab, &c);
        de_transform w2 = de_transform_mul(&a, &bc);
        f64 e = dv3_err(w1.position, w2.position);
        if (e > worst_assoc)
            worst_assoc = e;
        e = rot_err(w1.rotation, w2.rotation);
        if (e > worst_rot)
            worst_rot = e;
        EXPECT_DLT(v3_err(w1.scale, w2.scale), 1e-5);

        /* A point in the hand's space, through the composed transform and level by level. */
        de_dvec3 p = rnd_dv3(1.0);
        de_dvec3 chain = de_transform_point(&a, de_transform_point(&b, de_transform_point(&c, p)));
        e = dv3_err(de_transform_point(&w1, p), chain);
        if (e > worst_chain)
            worst_chain = e;

        /* The same hierarchy with the root at the origin: identical up to f64 rounding of the
         * root position (1.5e-11 m at 100 km). This is what keeps far objects from jittering. */
        de_transform a0 = tr(dv3(0, 0, 0), a.rotation, a.scale);
        de_transform ab0 = de_transform_mul(&a0, &b);
        de_transform w0 = de_transform_mul(&ab0, &c);
        e = dv3_err(de_dv3_sub(w1.position, a.position), w0.position);
        if (e > worst_shift)
            worst_shift = e;
    }
    printf("compose_hierarchy_3_levels: assoc %.3g m, rot %.3g, chain %.3g m, shift %.3g m\n",
           worst_assoc, worst_rot, worst_chain, worst_shift);
    /* f32 rotations of ab and bc are rounded (about 6e-8 rad); over the 10 m level that is
     * about 1e-6 m. */
    EXPECT_DLT(worst_assoc, 1e-4);
    EXPECT_DLT(worst_rot, 1e-6);
    EXPECT_DLT(worst_chain, 1e-4);
    EXPECT_DLT(worst_shift, 1e-9);

    /* Non-uniform scale: the child's origin still goes through the full parent transform, bit for
     * bit (only the child's axes drop the shear). */
    de_transform p = tr(rnd_dv3(100 * KM), rnd_quat(), v3(1, 3, 0.5f));
    de_transform ch = tr(rnd_dv3(50.0), rnd_quat(), v3(2, 1, 1));
    de_transform w = de_transform_mul(&p, &ch);
    de_dvec3 origin = de_transform_point(&p, ch.position);
    EXPECT_MEMEQ(&w.position, &origin, sizeof origin);
    EXPECT_DLT(v3_err(w.scale, v3(2, 3, 0.5f)), 1e-7);

    /* Identity is neutral on both sides, exactly. */
    de_transform id = de_transform_identity();
    de_transform l = de_transform_mul(&id, &p), r = de_transform_mul(&p, &id);
    EXPECT_FLOAT_EQ(dv3_err(l.position, p.position), 0.0, 0);
    EXPECT_FLOAT_EQ(dv3_err(r.position, p.position), 0.0, 0);
    EXPECT_DLT(rot_err(l.rotation, p.rotation), 1e-7);
    EXPECT_FLOAT_EQ(v3_err(l.scale, p.scale), 0.0, 0);
}

/* ---- Golden vectors (DENG-364) ---- */

#define TGOLDEN_COUNT        4096 /* per function named by the issue */
#define TGOLDEN_COUNT_HELPER 1024 /* dir and lerp, which DENG-364 adds on top */
#define TG_MAXV              32   /* values per line, inputs + outputs */

static u32 tg_b32(f32 x) {
    u32 u;
    memcpy(&u, &x, sizeof u);
    return u;
}
static f32 tg_f32(u64 u) {
    u32 v = (u32)u;
    f32 x;
    memcpy(&x, &v, sizeof x);
    return x;
}
static u64 tg_b64(f64 x) {
    u64 u;
    memcpy(&u, &x, sizeof u);
    return u;
}
static f64 tg_f64(u64 u) {
    f64 x;
    memcpy(&x, &u, sizeof x);
    return x;
}

/* A de_transform as 10 values at v[0..9]. */
static de_transform tg_tr_in(const u64 *v) {
    de_transform t = tr(dv3(tg_f64(v[0]), tg_f64(v[1]), tg_f64(v[2])),
                        q4(tg_f32(v[3]), tg_f32(v[4]), tg_f32(v[5]), tg_f32(v[6])),
                        v3(tg_f32(v[7]), tg_f32(v[8]), tg_f32(v[9])));
    return t;
}
static void tg_tr_out(const de_transform *t, u64 *v) {
    v[0] = tg_b64(t->position.x);
    v[1] = tg_b64(t->position.y);
    v[2] = tg_b64(t->position.z);
    v[3] = tg_b32(t->rotation.x);
    v[4] = tg_b32(t->rotation.y);
    v[5] = tg_b32(t->rotation.z);
    v[6] = tg_b32(t->rotation.w);
    v[7] = tg_b32(t->scale.x);
    v[8] = tg_b32(t->scale.y);
    v[9] = tg_b32(t->scale.z);
}
static de_dvec3 tg_dv3_in(const u64 *v) {
    return dv3(tg_f64(v[0]), tg_f64(v[1]), tg_f64(v[2]));
}
static void tg_dv3_out(de_dvec3 p, u64 *v) {
    v[0] = tg_b64(p.x);
    v[1] = tg_b64(p.y);
    v[2] = tg_b64(p.z);
}
static void tg_v3_out(de_vec3 p, u64 *v) {
    v[0] = tg_b32(p.x);
    v[1] = tg_b32(p.y);
    v[2] = tg_b32(p.z);
}

/* ---- Input generation (used only to write the file). splitmix64, independent of everything
 * else in the engine, and only + - * / and sqrt in f64, so a regeneration on any platform gives
 * the same inputs. ---- */

typedef struct tg_gen {
    u64 s;
} tg_gen;

static u64 tg_next(tg_gen *g) {
    u64 z = (g->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static f64 tg_uniform(tg_gen *g, f64 lo, f64 hi) {
    return lo + (hi - lo) * ((f64)(tg_next(g) >> 11) * (1.0 / 9007199254740992.0));
}

/* A world position: near the origin, anywhere in +-100 km, or beyond (+-10 000 km). */
static de_dvec3 tg_pos(tg_gen *g) {
    static const f64 range[4] = {10.0, 100.0 * KM, 100.0 * KM, 1e7};
    f64 r = range[tg_next(g) % 4];
    return dv3(tg_uniform(g, -r, r), tg_uniform(g, -r, r), tg_uniform(g, -r, r));
}

/* A rotation: half unit quaternions (normalized in f64, then rounded to f32, as stored by
 * gameplay code), half arbitrary non-unit ones, which the f64 paths normalize. */
static de_quat tg_quat(tg_gen *g) {
    f64 x = tg_uniform(g, -1, 1), y = tg_uniform(g, -1, 1);
    f64 z = tg_uniform(g, -1, 1), w = tg_uniform(g, -1, 1);
    if (tg_next(g) % 2) {
        f64 n = sqrt(x * x + y * y + z * z + w * w);
        x /= n;
        y /= n;
        z /= n;
        w /= n;
    } else {
        x *= 1.5;
        y *= 1.5;
        z *= 1.5;
        w *= 1.5;
    }
    return q4((f32)x, (f32)y, (f32)z, (f32)w);
}

/* A scale in [0.25, 4]: uniform a quarter of the time, otherwise non-uniform, and one time in
 * eight with a mirrored (negative) x. */
static de_vec3 tg_scale(tg_gen *g) {
    u64 kind = tg_next(g) % 8;
    f32 x = (f32)tg_uniform(g, 0.25, 4.0);
    if (kind < 2)
        return v3(x, x, x);
    de_vec3 s = v3(x, (f32)tg_uniform(g, 0.25, 4.0), (f32)tg_uniform(g, 0.25, 4.0));
    if (kind == 7)
        s.x = -s.x;
    return s;
}

/* Hand-picked transforms first, then random ones. */
static de_transform tg_transform(tg_gen *g, int i) {
    const f32 h = 0.70710677f; /* sqrt(1/2) */
    switch (i) {
    case 0:
        return de_transform_identity();
    case 1:
        return tr(dv3(100 * KM, -100 * KM, 1.5), q4(0, 0, h, h), v3(1, 1, 1));
    case 2:
        return tr(dv3(1, 2, 3), q4(0, 0, 0, -1), v3(2, 2, 2)); /* -identity rotation */
    case 3:
        return tr(dv3(-5e4, 7e4, -0.25), q4(0, 0, 0, 0), v3(1, 3, 0.5f)); /* zero quaternion */
    case 4:
        return tr(dv3(0.001, 0, 0), q4(h, 0, 0, h), v3(-1, 1, 1)); /* mirror */
    case 5:
        return tr(dv3(1e7, 1e7, -1e7), q4(0.5f, 0.5f, 0.5f, 0.5f), v3(0.25f, 4, 1));
    default:
        return tr(tg_pos(g), tg_quat(g), tg_scale(g));
    }
}

static void tg_gen_relative(tg_gen *g, int i, u64 *in) {
    de_dvec3 p = tg_pos(g), o;
    switch (i) {
    case 0: /* the 1 mm acceptance case of relative_precision_100km */
        p = dv3(100 * KM + 0.001, 100 * KM, 0);
        o = dv3(100 * KM, 100 * KM, 0);
        break;
    case 1:
        o = p;
        break;
    default:
        /* Usually a camera near the object (what rendering does), sometimes anywhere. */
        o = tg_next(g) % 4 ? de_dv3_add(p, dv3(tg_uniform(g, -1e3, 1e3), tg_uniform(g, -1e3, 1e3),
                                               tg_uniform(g, -1e3, 1e3)))
                           : tg_pos(g);
        break;
    }
    tg_dv3_out(p, in);
    tg_dv3_out(o, in + 3);
}
static void tg_gen_mul(tg_gen *g, int i, u64 *in) {
    de_transform a = tg_transform(g, i), b = tg_transform(g, i < 6 ? (i + 1) % 6 : i);
    if (i >= 6 && tg_next(g) % 2)
        b.position = dv3(tg_uniform(g, -50, 50), tg_uniform(g, -50, 50), tg_uniform(g, -50, 50));
    tg_tr_out(&a, in);
    tg_tr_out(&b, in + 10);
}
static void tg_gen_inverse(tg_gen *g, int i, u64 *in) {
    de_transform a = tg_transform(g, i);
    tg_tr_out(&a, in);
}
static void tg_gen_point(tg_gen *g, int i, u64 *in) {
    de_transform a = tg_transform(g, i);
    de_dvec3 p = tg_next(g) % 2
                     ? tg_pos(g)
                     : dv3(tg_uniform(g, -10, 10), tg_uniform(g, -10, 10), tg_uniform(g, -10, 10));
    tg_tr_out(&a, in);
    tg_dv3_out(p, in + 10);
}
static void tg_gen_dir(tg_gen *g, int i, u64 *in) {
    de_transform a = tg_transform(g, i);
    tg_tr_out(&a, in);
    tg_v3_out(
        v3((f32)tg_uniform(g, -10, 10), (f32)tg_uniform(g, -10, 10), (f32)tg_uniform(g, -10, 10)),
        in + 10);
}
static void tg_gen_lerp(tg_gen *g, int i, u64 *in) {
    static const f32 edge_t[6] = {0.0f, 1.0f, 0.5f, 0.25f, -0.5f, 1.5f};
    de_transform a = tg_transform(g, i), b = tg_transform(g, i < 6 ? (i + 2) % 6 : i);
    f32 t = i < 6 ? edge_t[i] : (f32)tg_uniform(g, tg_next(g) % 8 ? 0.0 : -0.5, 1.0);
    tg_tr_out(&a, in);
    tg_tr_out(&b, in + 10);
    in[20] = tg_b32(t);
}
static void tg_gen_affine(tg_gen *g, int i, u64 *in) {
    de_transform a = tg_transform(g, i);
    de_dvec3 o = tg_next(g) % 4 ? de_dv3_add(a.position,
                                             dv3(tg_uniform(g, -1e3, 1e3), tg_uniform(g, -1e3, 1e3),
                                                 tg_uniform(g, -1e3, 1e3)))
                                : tg_pos(g);
    tg_tr_out(&a, in);
    tg_dv3_out(o, in + 10);
}

/* ---- Evaluation ---- */

static void tg_ev_relative(const u64 *in, u64 *out) {
    tg_v3_out(de_dv3_relative(tg_dv3_in(in), tg_dv3_in(in + 3)), out);
}
static void tg_ev_mul(const u64 *in, u64 *out) {
    de_transform a = tg_tr_in(in), b = tg_tr_in(in + 10);
    de_transform r = de_transform_mul(&a, &b);
    tg_tr_out(&r, out);
}
static void tg_ev_inverse(const u64 *in, u64 *out) {
    de_transform a = tg_tr_in(in);
    de_transform r = de_transform_inverse(&a);
    tg_tr_out(&r, out);
}
static void tg_ev_point(const u64 *in, u64 *out) {
    de_transform a = tg_tr_in(in);
    tg_dv3_out(de_transform_point(&a, tg_dv3_in(in + 10)), out);
}
static void tg_ev_dir(const u64 *in, u64 *out) {
    de_transform a = tg_tr_in(in);
    de_vec3 d = v3(tg_f32(in[10]), tg_f32(in[11]), tg_f32(in[12]));
    tg_v3_out(de_transform_dir(&a, d), out);
}
static void tg_ev_lerp(const u64 *in, u64 *out) {
    de_transform a = tg_tr_in(in), b = tg_tr_in(in + 10);
    de_transform r = de_transform_lerp(&a, &b, tg_f32(in[20]));
    tg_tr_out(&r, out);
}
static void tg_ev_affine(const u64 *in, u64 *out) {
    de_transform a = tg_tr_in(in);
    de_affine m = de_transform_to_affine_relative(&a, tg_dv3_in(in + 10));
    for (int k = 0; k < 12; ++k)
        out[k] = tg_b32(m.m[k]);
}

#define TG_T "dddfffffff" /* one de_transform */

typedef struct tg_fn {
    const char *name;
    const char *types; /* inputs then outputs */
    int nin, count;
    void (*eval)(const u64 *in, u64 *out);
    void (*gen)(tg_gen *g, int i, u64 *in);
} tg_fn;

static const tg_fn TG_FNS[] = {
    /* name, types (inputs then outputs), inputs, count, eval, gen */
    {"dv3_relative", "ddddddfff", 6, TGOLDEN_COUNT, tg_ev_relative, tg_gen_relative},
    {"transform_mul", TG_T TG_T TG_T, 20, TGOLDEN_COUNT, tg_ev_mul, tg_gen_mul},
    {"transform_inverse", TG_T TG_T, 10, TGOLDEN_COUNT, tg_ev_inverse, tg_gen_inverse},
    {"transform_point", TG_T "dddddd", 13, TGOLDEN_COUNT, tg_ev_point, tg_gen_point},
    {"transform_dir", TG_T "ffffff", 13, TGOLDEN_COUNT_HELPER, tg_ev_dir, tg_gen_dir},
    {"transform_lerp", TG_T TG_T "f" TG_T, 21, TGOLDEN_COUNT_HELPER, tg_ev_lerp, tg_gen_lerp},
    {"transform_to_affine_relative", TG_T "dddffffffffffff", 13, TGOLDEN_COUNT, tg_ev_affine,
     tg_gen_affine},
};
#define TG_NFNS ((int)(sizeof TG_FNS / sizeof TG_FNS[0]))

static const tg_fn *tg_find(const char *name) {
    for (int i = 0; i < TG_NFNS; i++) {
        if (strcmp(TG_FNS[i].name, name) == 0)
            return &TG_FNS[i];
    }
    return NULL;
}

static int tg_nvals(const tg_fn *f) {
    return (int)strlen(f->types);
}

/* Seeded per function, so adding a function does not change the others. */
static tg_gen tg_gen_for(const tg_fn *f) {
    tg_gen g = {0x364DE364ull};
    for (const char *p = f->name; *p; p++)
        g.s = g.s * 131u + (u8)*p;
    return g;
}

static void tg_regen(void) {
    FILE *out = fopen(DE_TRANSFORM_GOLDEN, "wb");
    ASSERT_TRUE(out != NULL);
    fprintf(out,
            "# transform_golden.txt: de_dv3_relative and de_transform_* reference results\n"
            "# (DENG-364). Generated by test_math with DE_REGEN_GOLDEN=1; never edit by hand.\n"
            "# x86-64 and arm64 must reproduce every line bit for bit. Section:\n"
            "# @ <name> <inputs> <outputs> <types> <count>, then one line per case: the inputs,\n"
            "# then the outputs, as hex bit patterns (types: f = f32, d = f64).\n");
    for (int k = 0; k < TG_NFNS; k++) {
        const tg_fn *f = &TG_FNS[k];
        int nv = tg_nvals(f);
        tg_gen g = tg_gen_for(f);
        fprintf(out, "@ %s %d %d %s %d\n", f->name, f->nin, nv - f->nin, f->types, f->count);
        for (int i = 0; i < f->count; i++) {
            u64 v[TG_MAXV] = {0};
            f->gen(&g, i, v);
            f->eval(v, v + f->nin);
            for (int j = 0; j < nv; j++) {
                if (f->types[j] == 'f')
                    fprintf(out, j ? " %08x" : "%08x", (unsigned)v[j]);
                else
                    fprintf(out, j ? " %016llx" : "%016llx", (unsigned long long)v[j]);
            }
            fputc('\n', out);
        }
    }
    fclose(out);
}

/* Compares every line of the golden file; returns the number of mismatching results. */
static int tg_check(const char *mode) {
    FILE *in = fopen(DE_TRANSFORM_GOLDEN, "rb");
    ASSERT_TRUE(in != NULL);
    char line[1024];
    const tg_fn *f = NULL;
    int left = 0, bad = 0, sections = 0, lineno = 0;
    while (fgets(line, sizeof line, in)) {
        lineno++;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        if (line[0] == '@') {
            EXPECT_EQ(left, 0);
            char name[64], types[TG_MAXV + 1];
            int nin, nout, count;
            ASSERT_EQ(sscanf(line, "@ %63s %d %d %32s %d", name, &nin, &nout, types, &count), 5);
            f = tg_find(name);
            if (!f) {
                printf("transform_golden.txt:%d: unknown function '%s'\n", lineno, name);
                FAIL("unknown function in the golden file");
            }
            ASSERT_EQ(nin, f->nin);
            ASSERT_EQ(nin + nout, tg_nvals(f));
            ASSERT_TRUE(strcmp(types, f->types) == 0);
            EXPECT_GE(count, f->count);
            left = count;
            sections++;
            continue;
        }
        ASSERT_TRUE(f != NULL);
        ASSERT_GT(left, 0);
        left--;
        int nv = tg_nvals(f);
        u64 v[TG_MAXV] = {0};
        char *p = line;
        for (int j = 0; j < nv; j++) {
            char *end;
            v[j] = strtoull(p, &end, 16);
            ASSERT_TRUE(end != p);
            p = end;
        }
        u64 got[TG_MAXV] = {0};
        f->eval(v, got);
        for (int j = 0; j < nv - f->nin; j++) {
            if (got[j] != v[f->nin + j]) {
                if (bad < 20)
                    printf("  [%s] %s line %d: out[%d] expected %llx got %llx\n", mode, f->name,
                           lineno, j, (unsigned long long)v[f->nin + j],
                           (unsigned long long)got[j]);
                bad++;
            }
        }
    }
    fclose(in);
    EXPECT_EQ(left, 0);
    EXPECT_EQ(sections, TG_NFNS);
    return bad;
}

TEST(transform, golden_bitwise) {
    const char *regen = getenv("DE_REGEN_GOLDEN");
    if (regen && strcmp(regen, "1") == 0) {
        tg_regen();
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_TRANSFORM_GOLDEN);
    }
    /* The contract is stated for the de_fp_env_init_thread mode; the inputs hold no denormals,
     * so the default mode must give the same bits too. */
    EXPECT_EQ(tg_check("default FP mode"), 0);
    u64 saved = de_test_fpenv_get();
    de_fp_env_init_thread();
    EXPECT_EQ(tg_check("de_fp_env_init_thread"), 0);
    de_test_fpenv_set(saved);
}
