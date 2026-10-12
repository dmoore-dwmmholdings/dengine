/* test_narrow_specialized.c: de__collide's specialized sphere / capsule paths, dispatch, swap
 * symmetry, large worlds, no allocation and the golden contact vector (DENG-134, SPEC §13.1).
 *
 * physics_narrow.golden_vector hashes the contact sets of 5,000 de_rng shape pairs (every kind
 * combination) and compares them with tests/unit/physics/vectors/narrow_golden.txt, a
 * cross-platform contract: x86-64 and arm64 must reproduce every line. Regenerate it only for a
 * deliberate algorithm change, never to make a platform pass:
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics/test_physics_narrow
 * --filter=physics_narrow.golden_vector */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen, sscanf */
#endif
#include "de_test.h"

#include <dengine/de_physics.h>

#include "de_alloc_guard.h"
#include "physics/narrow/narrowphase.h"
#include "physics/physics_internal.h"
#include "physics/shapes/shape_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_PHYSICS_NARROW_GOLDEN
#error "DE_PHYSICS_NARROW_GOLDEN (path of narrow_golden.txt) must be defined by the build"
#endif

/* ---- helpers ---------------------------------------------------------------------------- */

#define CAP 64u

static de_shape_desc desc_of(de_shape_kind k) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = k;
    return d;
}

static const de__shape *mk(de_physics_world *p, const de_shape_desc *d) {
    const de_shape h = de_shape_create(p, d);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    return de__shape_get(p, h);
}

static const de__shape *sphere(de_physics_world *p, f32 r) {
    de_shape_desc d = desc_of(DE_SHAPE_SPHERE);
    d.radius = r;
    return mk(p, &d);
}
static const de__shape *capsule(de_physics_world *p, f32 r, f32 h) {
    de_shape_desc d = desc_of(DE_SHAPE_CAPSULE);
    d.radius = r;
    d.half_height = h;
    return mk(p, &d);
}
static const de__shape *box(de_physics_world *p, f32 x, f32 y, f32 z) {
    de_shape_desc d = desc_of(DE_SHAPE_BOX);
    d.half_extents = (de_vec3){x, y, z};
    return mk(p, &d);
}
static const de__shape *cylinder(de_physics_world *p, f32 r, f32 h) {
    de_shape_desc d = desc_of(DE_SHAPE_CYLINDER);
    d.radius = r;
    d.half_height = h;
    return mk(p, &d);
}
static const de__shape *box_hull(de_physics_world *p, f32 x, f32 y, f32 z) {
    de_vec3 pts[8];
    for (u32 v = 0; v < 8; ++v)
        pts[v] = (de_vec3){(v & 1u) ? x : -x, (v & 2u) ? y : -y, (v & 4u) ? z : -z};
    de_shape_desc d = desc_of(DE_SHAPE_CONVEX);
    d.points = pts;
    d.point_count = 8;
    return mk(p, &d);
}

static de_quat qid(void) {
    return (de_quat){0.0f, 0.0f, 0.0f, 1.0f};
}
static de_quat qaxis(f32 x, f32 y, f32 z, f32 angle) {
    const de_vec3 a = de_dnormalize_v3((de_vec3){x, y, z});
    const f32 s = de_dsin(angle * 0.5f), c = de_dcos(angle * 0.5f);
    return de_dnormalize_quat((de_quat){a.x * s, a.y * s, a.z * s, c});
}
static de__pose pose(f64 x, f64 y, f64 z, de_quat q) {
    de__pose r;
    r.position = (de_dvec3){x, y, z};
    r.rotation = q;
    return r;
}
static f32 rnd(de_rng *r, f32 lo, f32 hi) {
    return lo + (hi - lo) * de_rng_f32(r);
}
/* Draws are sequenced statement by statement: C leaves the evaluation order of function
 * arguments and initializers unspecified (GCC and MSVC differ from Clang), and the random
 * streams must not depend on the compiler. */
static de_vec3 rnd3(de_rng *r, f32 lo, f32 hi) {
    const f32 x = rnd(r, lo, hi);
    const f32 y = rnd(r, lo, hi);
    const f32 z = rnd(r, lo, hi);
    return (de_vec3){x, y, z};
}
static de_quat rnd_quat(de_rng *r) {
    const f32 x = rnd(r, -1, 1);
    const f32 y = rnd(r, -1, 1);
    const f32 z = rnd(r, -1, 1);
    const f32 w = rnd(r, -1, 1);
    return de_dnormalize_quat((de_quat){x, y, z, w});
}
/* A pose at (ox, oy, oz) + a random offset in [lo, hi)^3, random rotation. */
static de__pose rnd_pose(de_rng *r, f64 ox, f64 oy, f64 oz, f32 lo, f32 hi) {
    const de_vec3 o = rnd3(r, lo, hi);
    const de_quat q = rnd_quat(r);
    return pose(ox + o.x, oy + o.y, oz + o.z, q);
}

static u32 collide(const de__shape *a, de__pose xa, const de__shape *b, de__pose xb, f32 spec,
                   de__contact_set *sets) {
    de__contact_buffer buf = {sets, CAP, 0, false};
    return de__collide(a, xa, b, xb, spec, &buf);
}

/* World position of a local point of a pose (f64). */
static de_dvec3 world(de__pose x, de_vec3 l) {
    const de__dmat3 m = de__dmat3_from_quat(x.rotation);
    return de__d3_add(x.position, de__dmat3_mul(&m, de__d3_from(l)));
}

static f32 min_sep(const de__contact_set *s) {
    f32 m = s->points[0].separation;
    for (u32 i = 1; i < s->point_count; ++i)
        if (s->points[i].separation < m)
            m = s->points[i].separation;
    return m;
}

/* ---- sphere / capsule ------------------------------------------------------------------- */

TEST(physics_narrow, sphere_sphere_exact) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *a = sphere(p, 0.5f), *b = sphere(p, 0.25f);
    const de_vec3 off = {0.6f, 0.2f, 0.1f};
    const f64 d = de_dsqrt64(0.36 + 0.04 + 0.01);
    const de__pose xa = pose(1, 2, 3, qaxis(1, 2, 3, 0.7f));
    const de__pose xb = pose(1.0 + off.x, 2.0 + off.y, 3.0 + off.z, qaxis(-1, 0, 2, 1.1f));
    de__contact_set s[CAP];
    ASSERT_EQ(collide(a, xa, b, xb, 0.0f, s), 1u);
    ASSERT_EQ(s[0].point_count, 1u);
    EXPECT_FLOAT_EQ(s[0].points[0].separation, d - 0.75, 1e-5);
    EXPECT_FLOAT_EQ(s[0].normal.x, off.x / d, 1e-5);
    EXPECT_FLOAT_EQ(s[0].normal.y, off.y / d, 1e-5);
    EXPECT_FLOAT_EQ(s[0].normal.z, off.z / d, 1e-5);
    const de_dvec3 wa = world(xa, s[0].points[0].local_a), wb = world(xb, s[0].points[0].local_b);
    EXPECT_FLOAT_EQ(wa.x, 1.0 + 0.5 * off.x / d, 1e-5);
    EXPECT_FLOAT_EQ(wa.z, 3.0 + 0.5 * off.z / d, 1e-5);
    EXPECT_FLOAT_EQ(wb.x, 1.0 + off.x - 0.25 * off.x / d, 1e-5);
    EXPECT_FLOAT_EQ(wb.y, 2.0 + off.y - 0.25 * off.y / d, 1e-5);
    EXPECT_EQ(s[0].points[0].feature_id,
              de__feature_id(de__feat(DE__FEAT_SHAPE, 0), de__feat(DE__FEAT_SHAPE, 0)));
    EXPECT_EQ(s[0].child_a, DE__SUB_NONE);
    EXPECT_EQ(s[0].sub_b, DE__SUB_NONE);
    /* Separated: only within the speculative distance. */
    const de__pose far = pose(1.85, 2, 3, qid());
    EXPECT_EQ(collide(a, pose(1, 2, 3, qid()), b, far, 0.05f, s), 0u);
    ASSERT_EQ(collide(a, pose(1, 2, 3, qid()), b, far, 0.2f, s), 1u);
    EXPECT_FLOAT_EQ(s[0].points[0].separation, 0.1, 1e-5);
    de_physics_destroy(p);
}

TEST(physics_narrow, coincident_centers_use_world_z) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *a = sphere(p, 0.5f), *b = sphere(p, 0.3f);
    de__contact_set s[CAP];
    ASSERT_EQ(collide(a, pose(4, 5, 6, qaxis(1, 1, 0, 2.0f)), b, pose(4, 5, 6, qid()), 0.0f, s),
              1u);
    EXPECT_FLOAT_EQ(s[0].normal.x, 0.0, 1e-6);
    EXPECT_FLOAT_EQ(s[0].normal.y, 0.0, 1e-6);
    EXPECT_FLOAT_EQ(s[0].normal.z, 1.0, 1e-6);
    EXPECT_FLOAT_EQ(s[0].points[0].separation, -0.8, 1e-6);
    /* Coincident capsule cores too. */
    const de__shape *c = capsule(p, 0.2f, 0.5f);
    ASSERT_EQ(collide(c, pose(0, 0, 0, qid()), c, pose(0, 0, 0, qid()), 0.0f, s), 1u);
    EXPECT_FLOAT_EQ(s[0].normal.z, 1.0, 1e-6);
    de_physics_destroy(p);
}

TEST(physics_narrow, sphere_capsule_features) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *c = capsule(p, 0.3f, 1.0f), *s = sphere(p, 0.2f);
    de__contact_set out[CAP];
    /* Beside the middle: the segment. */
    ASSERT_EQ(collide(c, pose(0, 0, 0, qid()), s, pose(0.45, 0, 0.2, qid()), 0.0f, out), 1u);
    EXPECT_FLOAT_EQ(out[0].points[0].separation, -0.05, 1e-6);
    EXPECT_FLOAT_EQ(out[0].normal.x, 1.0, 1e-6);
    EXPECT_EQ(de__feature_a(out[0].points[0].feature_id), de__feat(DE__FEAT_EDGE, 0));
    /* Above the +Z end: vertex 1. Sphere first: the pair is swapped back. */
    ASSERT_EQ(collide(s, pose(0, 0.1, 1.45, qid()), c, pose(0, 0, 0, qid()), 0.0f, out), 1u);
    EXPECT_EQ(de__feature_b(out[0].points[0].feature_id), de__feat(DE__FEAT_VERTEX, 1));
    const f64 d = de_dsqrt64(0.01 + 0.45 * 0.45);
    EXPECT_FLOAT_EQ(out[0].points[0].separation, d - 0.5, 1e-6);
    EXPECT_FLOAT_EQ(out[0].normal.z, -0.45 / d, 1e-5); /* from the sphere toward the capsule */
    /* A rotated capsule lying along X. */
    const de_quat ry = qaxis(0, 1, 0, 1.5707963f);
    ASSERT_EQ(collide(c, pose(0, 0, 0, ry), s, pose(-0.9, 0, 0.45, qid()), 0.0f, out), 1u);
    EXPECT_FLOAT_EQ(out[0].normal.z, 1.0, 1e-5);
    EXPECT_FLOAT_EQ(out[0].points[0].separation, -0.05, 1e-5);
    de_physics_destroy(p);
}

TEST(physics_narrow, capsule_capsule_parallel_two_points) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *a = capsule(p, 0.25f, 1.0f), *b = capsule(p, 0.25f, 0.5f);
    const de_quat ry = qaxis(0, 1, 0, 1.5707963f); /* axis along X */
    de__contact_set s[CAP];
    /* B lies on A, parallel, shifted along the axis so the end of B passes A's end. */
    ASSERT_EQ(collide(a, pose(0, 0, 0, ry), b, pose(0.8, 0, 0.49, ry), 0.0f, s), 1u);
    ASSERT_EQ(s[0].point_count, 2u);
    for (u32 i = 0; i < 2; ++i)
        EXPECT_FLOAT_EQ(s[0].points[i].separation, -0.01, 1e-5);
    EXPECT_FLOAT_EQ(s[0].normal.z, 1.0, 1e-5);
    EXPECT_NE(s[0].points[0].feature_id, s[0].points[1].feature_id);
    /* The x range of the two points is the overlap [0.3, 1.0]. */
    const de_dvec3 w0 = world(pose(0, 0, 0, ry), s[0].points[0].local_a);
    const de_dvec3 w1 = world(pose(0, 0, 0, ry), s[0].points[1].local_a);
    const f64 lo = w0.x < w1.x ? w0.x : w1.x, hi = w0.x < w1.x ? w1.x : w0.x;
    EXPECT_FLOAT_EQ(lo, 0.3, 1e-5);
    EXPECT_FLOAT_EQ(hi, 1.0, 1e-5);
    /* Crossed at 90 degrees: one point. */
    const de_quat rx = qaxis(1, 0, 0, 1.5707963f);
    ASSERT_EQ(collide(a, pose(0, 0, 0, ry), b, pose(0.2, 0, 0.45, rx), 0.0f, s), 1u);
    EXPECT_EQ(s[0].point_count, 1u);
    EXPECT_FLOAT_EQ(s[0].points[0].separation, -0.05, 1e-5);
    EXPECT_EQ(s[0].points[0].feature_id,
              de__feature_id(de__feat(DE__FEAT_EDGE, 0), de__feat(DE__FEAT_EDGE, 0)));
    de_physics_destroy(p);
}

TEST(physics_narrow, zero_length_capsule_is_a_sphere) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *c0 = capsule(p, 0.4f, 0.0f), *s0 = sphere(p, 0.4f);
    const de__shape *other[3] = {sphere(p, 0.3f), capsule(p, 0.2f, 0.6f), box(p, 0.5f, 0.4f, 0.3f)};
    de_rng r = de_rng_seed(11, 3);
    for (u32 i = 0; i < 200; ++i) {
        const de__shape *o = other[i % 3u];
        const de__pose xa = pose(0, 0, 0, rnd_quat(&r));
        const de__pose xb = rnd_pose(&r, 0, 0, 0, -0.9f, 0.9f);
        de__contact_set s1[CAP], s2[CAP];
        const u32 n1 = collide(c0, xa, o, xb, 0.01f, s1);
        const u32 n2 = collide(s0, xa, o, xb, 0.01f, s2);
        ASSERT_EQ(n1, n2);
        for (u32 k = 0; k < n1; ++k) {
            EXPECT_FLOAT_EQ(min_sep(&s1[k]), min_sep(&s2[k]), 1e-5);
            EXPECT_FLOAT_EQ(s1[k].normal.x, s2[k].normal.x, 1e-4);
            EXPECT_FLOAT_EQ(s1[k].normal.y, s2[k].normal.y, 1e-4);
            EXPECT_FLOAT_EQ(s1[k].normal.z, s2[k].normal.z, 1e-4);
        }
    }
    de_physics_destroy(p);
}

/* ---- sphere / capsule against boxes and hulls ------------------------------------------ */

TEST(physics_narrow, sphere_box_regions) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *b = box(p, 1.0f, 0.5f, 0.25f), *s = sphere(p, 0.2f);
    const de__pose xb = pose(0, 0, 0, qid());
    de__contact_set o[CAP];
    /* Face +Z. */
    ASSERT_EQ(collide(b, xb, s, pose(0.3, 0.1, 0.4, qid()), 0.0f, o), 1u);
    EXPECT_FLOAT_EQ(o[0].normal.z, 1.0, 1e-6);
    EXPECT_FLOAT_EQ(o[0].points[0].separation, -0.05, 1e-6);
    EXPECT_EQ(de__feature_a(o[0].points[0].feature_id), de__feat(DE__FEAT_FACE, 4));
    /* Edge (+X, +Z): parallel to Y. */
    ASSERT_EQ(collide(b, xb, s, pose(1.1, 0.0, 0.35, qid()), 0.0f, o), 1u);
    const f64 d = de_dsqrt64(0.01 + 0.01);
    EXPECT_FLOAT_EQ(o[0].points[0].separation, d - 0.2, 1e-6);
    EXPECT_FLOAT_EQ(o[0].normal.x, 0.1 / d, 1e-5);
    EXPECT_EQ(de__feat_type(de__feature_a(o[0].points[0].feature_id)), DE__FEAT_EDGE);
    EXPECT_EQ(de__feature_a(o[0].points[0].feature_id),
              de__feat(DE__FEAT_EDGE, de__np_box_edge(1u | 4u, 1u | 2u | 4u)));
    /* Vertex (-,-,-). */
    ASSERT_EQ(collide(b, xb, s, pose(-1.1, -0.6, -0.35, qid()), 0.0f, o), 1u);
    EXPECT_EQ(de__feature_a(o[0].points[0].feature_id), de__feat(DE__FEAT_VERTEX, 0));
    /* Center inside, nearest face -Y. */
    ASSERT_EQ(collide(b, xb, s, pose(0.2, -0.45, 0.0, qid()), 0.0f, o), 1u);
    EXPECT_FLOAT_EQ(o[0].normal.y, -1.0, 1e-6);
    EXPECT_FLOAT_EQ(o[0].points[0].separation, -0.25, 1e-6);
    EXPECT_EQ(de__feature_a(o[0].points[0].feature_id), de__feat(DE__FEAT_FACE, 3));
    de_physics_destroy(p);
}

TEST(physics_narrow, hull_matches_box_for_spheres_and_capsules) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *b = box(p, 0.6f, 0.4f, 0.3f), *h = box_hull(p, 0.6f, 0.4f, 0.3f);
    const de__shape *others[2] = {sphere(p, 0.25f), capsule(p, 0.15f, 0.4f)};
    de_rng r = de_rng_seed(5, 5);
    u32 hits = 0;
    for (u32 i = 0; i < 400; ++i) {
        const de__shape *o = others[i & 1u];
        const de__pose xa = pose(1, -2, 0.5, rnd_quat(&r));
        const de__pose xb = rnd_pose(&r, 1.0, -2.0, 0.5, -1, 1);
        de__contact_set s1[CAP], s2[CAP];
        const u32 n1 = collide(b, xa, o, xb, 0.02f, s1);
        const u32 n2 = collide(h, xa, o, xb, 0.02f, s2);
        ASSERT_EQ(n1, n2);
        if (n1 == 0)
            continue;
        ++hits;
        EXPECT_FLOAT_EQ(min_sep(&s1[0]), min_sep(&s2[0]), 2e-5);
        EXPECT_FLOAT_EQ(s1[0].normal.x, s2[0].normal.x, 1e-3);
        EXPECT_FLOAT_EQ(s1[0].normal.y, s2[0].normal.y, 1e-3);
        EXPECT_FLOAT_EQ(s1[0].normal.z, s2[0].normal.z, 1e-3);
        /* Every point lies on both surfaces' sides of the normal consistently. */
        for (u32 k = 0; k < s2[0].point_count; ++k) {
            const de_dvec3 wa = world(xa, s2[0].points[k].local_a);
            const de_dvec3 wb = world(xb, s2[0].points[k].local_b);
            const de_dvec3 d = de__d3_sub(wb, wa);
            const de_vec3 n = s2[0].normal;
            EXPECT_FLOAT_EQ(d.x * n.x + d.y * n.y + d.z * n.z, s2[0].points[k].separation, 1e-4);
        }
    }
    EXPECT_GT(hits, 100u);
    de_physics_destroy(p);
}

TEST(physics_narrow, capsule_lying_on_box_two_points) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *b = box(p, 2.0f, 2.0f, 0.5f), *c = capsule(p, 0.2f, 0.6f);
    const de__shape *h = box_hull(p, 2.0f, 2.0f, 0.5f);
    const de_quat ry = qaxis(0, 1, 0, 1.5707963f);
    de__contact_set s[CAP];
    for (u32 k = 0; k < 2; ++k) {
        const de__shape *a = k == 0 ? b : h;
        /* Resting with 1 cm of penetration. */
        ASSERT_EQ(collide(a, pose(0, 0, 0, qid()), c, pose(0.3, 0.1, 0.69, ry), 0.0f, s), 1u);
        ASSERT_EQ(s[0].point_count, 2u);
        EXPECT_FLOAT_EQ(s[0].normal.z, 1.0, 1e-5);
        for (u32 i = 0; i < 2; ++i) {
            EXPECT_FLOAT_EQ(s[0].points[i].separation, -0.01, 1e-5);
            const u32 fa = de__feature_a(s[0].points[i].feature_id);
            if (k == 0)
                EXPECT_EQ(fa, de__feat(DE__FEAT_FACE, 4)); /* box +Z */
            else
                EXPECT_EQ(de__feat_type(fa), DE__FEAT_FACE);
        }
        /* Hovering within the speculative distance. */
        ASSERT_EQ(collide(a, pose(0, 0, 0, qid()), c, pose(0.3, 0.1, 0.73, ry), 0.05f, s), 1u);
        EXPECT_EQ(s[0].point_count, 2u);
        EXPECT_FLOAT_EQ(s[0].points[0].separation, 0.03, 1e-5);
        /* Hanging over the +X edge: one end is clipped to it. */
        ASSERT_EQ(collide(a, pose(0, 0, 0, qid()), c, pose(2.3, 0.1, 0.69, ry), 0.0f, s), 1u);
        ASSERT_EQ(s[0].point_count, 2u);
        u32 clipped = 0;
        for (u32 i = 0; i < 2; ++i)
            if (de__feat_type(de__feature_a(s[0].points[i].feature_id)) == DE__FEAT_EDGE)
                ++clipped;
        EXPECT_EQ(clipped, 1u);
    }
    /* Deep: the core passes through the box. */
    ASSERT_EQ(collide(b, pose(0, 0, 0, qid()), c, pose(0, 0, 0.45, qaxis(0, 1, 0, 0.3f)), 0.0f, s),
              1u);
    EXPECT_TRUE(s[0].normal.z > 0.99f);
    EXPECT_TRUE(min_sep(&s[0]) < -0.2f);
    de_physics_destroy(p);
}

/* ---- dispatch, symmetry, large worlds ---------------------------------------------------- */

typedef struct shape_set {
    const de__shape *s[16];
    u32 n;
} shape_set;

static void make_all_kinds(de_physics_world *p, shape_set *set) {
    memset(set, 0, sizeof *set);
    set->s[set->n++] = sphere(p, 0.4f);
    set->s[set->n++] = capsule(p, 0.25f, 0.5f);
    set->s[set->n++] = box(p, 0.5f, 0.3f, 0.4f);
    set->s[set->n++] = cylinder(p, 0.35f, 0.45f);
    {
        de_rng r = de_rng_seed(77, 1);
        de_vec3 pts[24];
        for (u32 i = 0; i < 24; ++i) {
            const f32 x = rnd(&r, -0.5f, 0.5f);
            const f32 y = rnd(&r, -0.4f, 0.4f);
            pts[i] = (de_vec3){x, y, rnd(&r, -0.45f, 0.45f)};
        }
        de_shape_desc d = desc_of(DE_SHAPE_CONVEX);
        d.points = pts;
        d.point_count = 24;
        set->s[set->n++] = mk(p, &d);
    }
    {
        static de_vec3 v[25];
        static u32 idx[16 * 6];
        for (u32 y = 0; y < 5; ++y)
            for (u32 x = 0; x < 5; ++x)
                v[y * 5 + x] = (de_vec3){(f32)x * 0.5f - 1.0f, (f32)y * 0.5f - 1.0f,
                                         0.15f * de_dsin((f32)(x * 3 + y))};
        u32 k = 0;
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 4; ++x) {
                const u32 a = y * 5 + x;
                const u32 q[6] = {a, a + 1, a + 6, a, a + 6, a + 5};
                for (u32 j = 0; j < 6; ++j)
                    idx[k++] = q[j];
            }
        de_shape_desc d = desc_of(DE_SHAPE_MESH);
        d.vertices = v;
        d.vertex_count = 25;
        d.indices = idx;
        d.index_count = k;
        set->s[set->n++] = mk(p, &d);
    }
    {
        static f32 h[7 * 6];
        de_rng r = de_rng_seed(3, 9);
        for (u32 i = 0; i < 7 * 6; ++i)
            h[i] = rnd(&r, -0.2f, 0.2f);
        h[20] = DE_HEIGHTFIELD_HOLE;
        de_shape_desc d = desc_of(DE_SHAPE_HEIGHTFIELD);
        d.heights = h;
        d.rows = 6;
        d.cols = 7;
        d.scale = (de_vec3){0.4f, 0.4f, 1.0f};
        set->s[set->n++] = mk(p, &d);
    }
    {
        de_shape ch[3] = {set->s[0]->handle, set->s[2]->handle, set->s[4]->handle};
        de_transform xf[3];
        for (u32 i = 0; i < 3; ++i) {
            xf[i].position = (de_dvec3){(f64)i * 0.6 - 0.6, 0.1 * (f64)i, 0.0};
            xf[i].rotation = qaxis(0.3f, 1.0f, (f32)i, 0.4f * (f32)i);
            xf[i].scale = (de_vec3){1, 1, 1};
        }
        de_shape_desc d = desc_of(DE_SHAPE_COMPOUND);
        d.children = ch;
        d.child_xforms = xf;
        d.child_count = 3;
        set->s[set->n++] = mk(p, &d);
    }
}

/* B's pose: near A (shape centers within ~ the sum of the bounding radii). */
static de__pose near_pose(de_rng *r, const de__shape *a, de__pose xa, const de__shape *b,
                          f32 reach) {
    const de_aabb la = a->local_aabb, lb = b->local_aabb;
    const de_vec3 ca = {(la.min.x + la.max.x) * 0.5f, (la.min.y + la.max.y) * 0.5f,
                        (la.min.z + la.max.z) * 0.5f};
    const de_vec3 cb = {(lb.min.x + lb.max.x) * 0.5f, (lb.min.y + lb.max.y) * 0.5f,
                        (lb.min.z + lb.max.z) * 0.5f};
    const de_vec3 ea = de__v3_scale(de__v3_sub(la.max, la.min), 0.5f);
    const de_vec3 eb = de__v3_scale(de__v3_sub(lb.max, lb.min), 0.5f);
    const f32 ra = de__np_len(ea), rb = de__np_len(eb);
    const de_vec3 dir = de_dnormalize_v3(rnd3(r, -1, 1));
    const f32 dist = rnd(r, 0.0f, reach * (ra + rb));
    de__pose xb;
    xb.rotation = rnd_quat(r);
    const de_dvec3 wca = world(xa, ca);
    const de__dmat3 mb = de__dmat3_from_quat(xb.rotation);
    const de_dvec3 off = de__dmat3_mul(&mb, de__d3_from(cb));
    xb.position = de__d3_sub(de__d3_add(wca, de__d3_scale(de__d3_from(dir), (f64)dist)), off);
    return xb;
}

static bool v3_neg_eq(de_vec3 a, de_vec3 b) {
    return a.x == -b.x && a.y == -b.y && a.z == -b.z;
}
static bool v3_eq(de_vec3 a, de_vec3 b) {
    return memcmp(&a, &b, sizeof a) == 0;
}

TEST(physics_narrow, swap_symmetry_all_kinds) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    shape_set set;
    make_all_kinds(p, &set);
    de_rng r = de_rng_seed(2024, 7);
    u32 pairs_with_contacts = 0;
    for (u32 i = 0; i < 2000; ++i) {
        const de__shape *a = set.s[de_rng_range(&r, 0, set.n)];
        const de__shape *b = set.s[de_rng_range(&r, 0, set.n)];
        const de__pose xa = rnd_pose(&r, 0, 0, 0, -2, 2);
        const de__pose xb = near_pose(&r, a, xa, b, 0.8f);
        static de__contact_set s1[CAP], s2[CAP];
        const u32 n1 = collide(a, xa, b, xb, 0.02f, s1);
        const u32 n2 = collide(b, xb, a, xa, 0.02f, s2);
        ASSERT_EQ(n1, n2);
        if (n1 > 0)
            ++pairs_with_contacts;
        for (u32 k = 0; k < n1; ++k) {
            ASSERT_TRUE(v3_neg_eq(s1[k].normal, s2[k].normal));
            ASSERT_EQ(s1[k].point_count, s2[k].point_count);
            EXPECT_EQ(s1[k].child_a, s2[k].child_b);
            EXPECT_EQ(s1[k].child_b, s2[k].child_a);
            EXPECT_EQ(s1[k].sub_a, s2[k].sub_b);
            EXPECT_EQ(s1[k].sub_b, s2[k].sub_a);
            for (u32 j = 0; j < s1[k].point_count; ++j) {
                const de__contact_point *c1 = &s1[k].points[j], *c2 = &s2[k].points[j];
                EXPECT_TRUE(v3_eq(c1->local_a, c2->local_b));
                EXPECT_TRUE(v3_eq(c1->local_b, c2->local_a));
                EXPECT_TRUE(c1->separation == c2->separation);
                EXPECT_EQ(c1->feature_id, de__feature_flip(c2->feature_id));
            }
        }
    }
    EXPECT_GT(pairs_with_contacts, 500u);
    de_physics_destroy(p);
}

TEST(physics_narrow, contact_geometry_consistent) {
    /* For every pair of every kind: unit normal, finite values, and the anchors' world offset
     * along the normal equals the separation. */
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    shape_set set;
    make_all_kinds(p, &set);
    de_rng r = de_rng_seed(99, 1);
    for (u32 i = 0; i < 2000; ++i) {
        const de__shape *a = set.s[de_rng_range(&r, 0, set.n)];
        const de__shape *b = set.s[de_rng_range(&r, 0, set.n)];
        const de__pose xa = rnd_pose(&r, 0, 0, 0, -2, 2);
        const de__pose xb = near_pose(&r, a, xa, b, 0.8f);
        static de__contact_set s[CAP];
        const u32 n = collide(a, xa, b, xb, 0.02f, s);
        for (u32 k = 0; k < n; ++k) {
            const de_vec3 nn = s[k].normal;
            EXPECT_FLOAT_EQ(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z, 1.0, 1e-5);
            EXPECT_TRUE(s[k].point_count >= 1u && s[k].point_count <= DE__CONTACT_MAX_POINTS);
            for (u32 j = 0; j < s[k].point_count; ++j) {
                const de__contact_point *c = &s[k].points[j];
                EXPECT_TRUE(de__v3_finite(c->local_a) && de__v3_finite(c->local_b));
                EXPECT_TRUE(c->separation <= 0.02f + 1e-6f);
                const de_dvec3 d = de__d3_sub(world(xb, c->local_b), world(xa, c->local_a));
                EXPECT_FLOAT_EQ(d.x * nn.x + d.y * nn.y + d.z * nn.z, c->separation, 2e-4);
                for (u32 q = 0; q < j; ++q)
                    EXPECT_NE(c->feature_id, s[k].points[q].feature_id);
            }
        }
    }
    de_physics_destroy(p);
}

TEST(physics_narrow, terrain_pairs_have_no_contacts) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    shape_set set;
    make_all_kinds(p, &set);
    de__contact_set s[CAP];
    EXPECT_EQ(collide(set.s[5], pose(0, 0, 0, qid()), set.s[6], pose(0, 0, 0, qid()), 0.1f, s), 0u);
    EXPECT_EQ(collide(set.s[5], pose(0, 0, 0, qid()), set.s[5], pose(0.1, 0, 0, qid()), 0.1f, s),
              0u);
    de_physics_destroy(p);
}

TEST(physics_narrow, large_world_matches_origin) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    shape_set set;
    make_all_kinds(p, &set);
    de_rng r = de_rng_seed(31, 4);
    u32 compared = 0;
    for (u32 i = 0; i < 500; ++i) {
        const de__shape *a = set.s[de_rng_range(&r, 0, 5)];
        const de__shape *b = set.s[de_rng_range(&r, 0, 5)];
        const de__pose xa = pose(0.25, -0.5, 0.125, rnd_quat(&r));
        const de__pose xb = near_pose(&r, a, xa, b, 0.7f);
        const de_dvec3 far = {98765.0, -54321.0, 1024.0};
        de__pose fa = xa, fb = xb;
        fa.position = de__d3_add(fa.position, far);
        fb.position = de__d3_add(fb.position, far);
        static de__contact_set s1[CAP], s2[CAP];
        const u32 n1 = collide(a, xa, b, xb, 0.0f, s1);
        const u32 n2 = collide(a, fa, b, fb, 0.0f, s2);
        if (n1 != 1u || n2 != 1u)
            continue;
        ++compared;
        /* The f64 relative transform keeps the 100 km pair within f32 rounding of the near one
         * (an f32 world position would be off by ~8 mm). */
        EXPECT_FLOAT_EQ(min_sep(&s1[0]), min_sep(&s2[0]), 1e-4);
        EXPECT_FLOAT_EQ(s1[0].normal.z, s2[0].normal.z, 1e-3);
    }
    EXPECT_GT(compared, 100u);
    de_physics_destroy(p);
}

TEST(physics_narrow, no_heap_allocation) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    shape_set set;
    make_all_kinds(p, &set);
    static de__contact_set s[CAP];
    de_rng r = de_rng_seed(8, 8);
    de__pose xa[256], xb[256];
    u32 ia[256], ib[256];
    for (u32 i = 0; i < 256; ++i) {
        ia[i] = de_rng_range(&r, 0, set.n);
        ib[i] = de_rng_range(&r, 0, set.n);
        xa[i] = rnd_pose(&r, 0, 0, 0, -2, 2);
        xb[i] = near_pose(&r, set.s[ia[i]], xa[i], set.s[ib[i]], 0.8f);
    }
    (void)collide(set.s[0], xa[0], set.s[1], xb[0], 0.02f, s); /* warm-up */
    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    de_alloc_guard_begin(&g);
    u32 total = 0;
    for (u32 i = 0; i < 256; ++i)
        total += collide(set.s[ia[i]], xa[i], set.s[ib[i]], xb[i], 0.02f, s);
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_GT(total, 0u);
    de_physics_destroy(p);
}

TEST(physics_narrow, buffer_overflow_is_reported) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    shape_set set;
    make_all_kinds(p, &set);
    const de__shape *big = box(p, 2.0f, 2.0f, 0.5f);
    de__contact_set s[2];
    de__contact_buffer buf = {s, 2, 0, false};
    /* A large box sunk into the bumpy mesh touches many triangles. */
    const u32 n =
        de__collide(set.s[5], pose(0, 0, 0, qid()), big, pose(0, 0, 0.3, qid()), 0.0f, &buf);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(buf.count, 2u);
    EXPECT_TRUE(buf.overflow);
    de_physics_destroy(p);
}

/* ---- golden vector ---------------------------------------------------------------------- */

typedef struct hbuf {
    u64 h;
} hbuf;
static void hb_u32(hbuf *b, u32 v) {
    const u8 x[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)};
    b->h = de_hash64(x, 4, b->h);
}
static void hb_f32(hbuf *b, f32 v) {
    u32 u;
    memcpy(&u, &v, 4);
    hb_u32(b, u);
}
static void hb_v3(hbuf *b, de_vec3 v) {
    hb_f32(b, v.x);
    hb_f32(b, v.y);
    hb_f32(b, v.z);
}

/* Two variants of every kind. */
static void make_golden_shapes(de_physics_world *p, const de__shape *out[8][2]) {
    shape_set set;
    make_all_kinds(p, &set);
    for (u32 k = 0; k < 8; ++k)
        out[k][0] = set.s[k];
    out[0][1] = sphere(p, 0.15f);
    out[1][1] = capsule(p, 0.3f, 0.0f);
    out[2][1] = box(p, 1.0f, 0.2f, 0.35f);
    out[3][1] = cylinder(p, 0.2f, 0.8f);
    out[4][1] = box_hull(p, 0.3f, 0.6f, 0.2f);
    {
        static const de_vec3 v[4] = {{-1, -1, 0}, {1, -1, 0.2f}, {1, 1, 0}, {-1, 1, -0.2f}};
        static const u32 idx[6] = {0, 1, 2, 0, 2, 3};
        de_shape_desc d = desc_of(DE_SHAPE_MESH);
        d.vertices = v;
        d.vertex_count = 4;
        d.indices = idx;
        d.index_count = 6;
        out[5][1] = mk(p, &d);
    }
    {
        static f32 h[5 * 5];
        for (u32 i = 0; i < 25; ++i)
            h[i] = 0.1f * (f32)(i % 3u);
        de_shape_desc d = desc_of(DE_SHAPE_HEIGHTFIELD);
        d.heights = h;
        d.rows = 5;
        d.cols = 5;
        d.scale = (de_vec3){0.5f, 0.6f, 1.5f};
        out[6][1] = mk(p, &d);
    }
    {
        de_shape ch[2] = {out[1][0]->handle, out[3][0]->handle};
        de_transform xf[2];
        xf[0].position = (de_dvec3){0.0, 0.0, 0.5};
        xf[0].rotation = qid();
        xf[1].position = (de_dvec3){0.4, -0.3, -0.2};
        xf[1].rotation = qaxis(1, 0, 0, 0.9f);
        xf[0].scale = xf[1].scale = (de_vec3){1, 1, 1};
        de_shape_desc d = desc_of(DE_SHAPE_COMPOUND);
        d.children = ch;
        d.child_xforms = xf;
        d.child_count = 2;
        out[7][1] = mk(p, &d);
    }
}

#define GOLDEN_PAIRS 5000u

static void compute_golden(u64 hashes[8][8], u32 *sets_total, u32 counts[8][8]) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *shapes[8][2];
    make_golden_shapes(p, shapes);
    memset(counts, 0, sizeof(u32) * 64);
    for (u32 i = 0; i < 8; ++i)
        for (u32 j = 0; j < 8; ++j)
            hashes[i][j] = 0;
    *sets_total = 0;
    de_rng r = de_rng_seed(134, 22);
    static de__contact_set s[CAP];
    for (u32 i = 0; i < GOLDEN_PAIRS; ++i) {
        /* Every kind pair 78 times, plus random ones. */
        const u32 ka = i < 64u * 78u ? (i / 8u) % 8u : de_rng_range(&r, 0, 8);
        const u32 kb = i < 64u * 78u ? i % 8u : de_rng_range(&r, 0, 8);
        const de__shape *a = shapes[ka][de_rng_range(&r, 0, 2)];
        const de__shape *b = shapes[kb][de_rng_range(&r, 0, 2)];
        const f64 far = (i % 5u) == 0 ? 25000.0 : 0.0;
        const de__pose xa = rnd_pose(&r, far, 0, 0, -3, 3);
        const de__pose xb = near_pose(&r, a, xa, b, 0.75f);
        const f32 spec = (i & 1u) ? 0.02f : 0.0f;
        de__contact_buffer buf = {s, CAP, 0, false};
        const u32 n = de__collide(a, xa, b, xb, spec, &buf);
        hbuf h = {hashes[ka][kb]};
        hb_u32(&h, n);
        hb_u32(&h, buf.overflow ? 1u : 0u);
        for (u32 k = 0; k < n; ++k) {
            hb_v3(&h, s[k].normal);
            hb_u32(&h, s[k].point_count);
            hb_u32(&h, s[k].child_a);
            hb_u32(&h, s[k].child_b);
            hb_u32(&h, s[k].sub_a);
            hb_u32(&h, s[k].sub_b);
            for (u32 j = 0; j < s[k].point_count; ++j) {
                hb_v3(&h, s[k].points[j].local_a);
                hb_v3(&h, s[k].points[j].local_b);
                hb_f32(&h, s[k].points[j].separation);
                hb_u32(&h, s[k].points[j].feature_id);
            }
        }
        hashes[ka][kb] = h.h;
        counts[ka][kb] += n;
        *sets_total += n;
    }
    de_physics_destroy(p);
}

TEST(physics_narrow, golden_vector) {
    static u64 hashes[8][8];
    static u32 counts[8][8];
    u32 total = 0;
    compute_golden(hashes, &total, counts);
    /* Coverage: every pair of kinds except terrain against terrain makes contacts. */
    for (u32 i = 0; i < 8; ++i)
        for (u32 j = 0; j < 8; ++j) {
            const bool ti = i == DE_SHAPE_MESH || i == DE_SHAPE_HEIGHTFIELD;
            const bool tj = j == DE_SHAPE_MESH || j == DE_SHAPE_HEIGHTFIELD;
            if (ti && tj)
                EXPECT_EQ(counts[i][j], 0u);
            else if (counts[i][j] == 0)
                FAIL("a kind pair made no contacts in the golden vector");
        }
    char names[65][32];
    u64 values[65];
    u32 n = 0;
    for (u32 i = 0; i < 8; ++i)
        for (u32 j = 0; j < 8; ++j) {
            snprintf(names[n], sizeof names[n], "pair_%u_%u", i, j);
            values[n++] = hashes[i][j];
        }
    snprintf(names[n], sizeof names[n], "sets_total");
    values[n++] = total;
    const char *regen = getenv("DE_REGEN_GOLDEN");
    if (regen != NULL && strcmp(regen, "1") == 0) {
        FILE *out = fopen(DE_PHYSICS_NARROW_GOLDEN, "wb");
        ASSERT_TRUE(out != NULL);
        fprintf(out, "# narrow_golden.txt: hashes (XXH3-64) of the contact sets of 5000 de_rng\n"
                     "# shape pairs per kind pair (pair_<kind A>_<kind B>) and the total number\n"
                     "# of sets (DENG-134). Generated by test_physics_narrow with\n"
                     "# DE_REGEN_GOLDEN=1; never edit by hand. x86-64 and arm64 must reproduce\n"
                     "# every line.\n");
        for (u32 i = 0; i < n; ++i)
            fprintf(out, "%s %016llx\n", names[i], (unsigned long long)values[i]);
        fclose(out);
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_PHYSICS_NARROW_GOLDEN);
    }
    FILE *in = fopen(DE_PHYSICS_NARROW_GOLDEN, "rb");
    ASSERT_TRUE(in != NULL);
    char line[256];
    u32 lines = 0, bad = 0;
    while (fgets(line, sizeof line, in)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        char name[64];
        unsigned long long v = 0;
        ASSERT_EQ(sscanf(line, "%63s %llx", name, &v), 2);
        ASSERT_LT(lines, n);
        EXPECT_STREQ(name, names[lines]);
        if ((u64)v != values[lines]) {
            printf("  %s: expected %016llx got %016llx\n", name, v,
                   (unsigned long long)values[lines]);
            ++bad;
        }
        ++lines;
    }
    fclose(in);
    EXPECT_EQ(lines, n);
    EXPECT_EQ(bad, 0u);
}
