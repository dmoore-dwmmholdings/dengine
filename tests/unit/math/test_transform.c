/* test_transform.c: large-world transforms, de_dvec3 and de_transform (DENG-244, SPEC §5.5).
 * Worlds reach +-100 km (DENG-2 Q10); positions there must keep sub-micrometre precision. */
#include "de_test.h"

#include <dengine/de_math.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

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
