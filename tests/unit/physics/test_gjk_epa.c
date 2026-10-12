/* test_gjk_epa.c: GJK distance, EPA depth and the GJK/EPA contact path (DENG-134, SPEC §13.1). */
#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/narrow/narrowphase.h"
#include "physics/physics_internal.h"
#include "physics/shapes/shape_internal.h"

#include <string.h>

#define CAP 16u

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
static const de__shape *box(de_physics_world *p, f32 x, f32 y, f32 z) {
    de_shape_desc d = desc_of(DE_SHAPE_BOX);
    d.half_extents = (de_vec3){x, y, z};
    return mk(p, &d);
}
static const de__shape *sphere(de_physics_world *p, f32 r) {
    de_shape_desc d = desc_of(DE_SHAPE_SPHERE);
    d.radius = r;
    return mk(p, &d);
}
static const de__shape *cylinder(de_physics_world *p, f32 r, f32 h) {
    de_shape_desc d = desc_of(DE_SHAPE_CYLINDER);
    d.radius = r;
    d.half_height = h;
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

static f64 absd(f64 x) {
    return x < 0.0 ? -x : x;
}

/* ---- brute force ------------------------------------------------------------------------- */

/* Distance of p to the solid box of half extents e (its own frame). */
static f64 point_box(de_dvec3 p, de_vec3 e) {
    const f64 q[3] = {p.x, p.y, p.z}, h[3] = {e.x, e.y, e.z};
    f64 d2 = 0.0;
    for (u32 i = 0; i < 3; ++i) {
        const f64 o = absd(q[i]) - h[i];
        if (o > 0.0)
            d2 += o * o;
    }
    return de_dsqrt64(d2);
}

static de_vec3 corner(de_vec3 e, u32 v) {
    return (de_vec3){(v & 1u) ? e.x : -e.x, (v & 2u) ? e.y : -e.y, (v & 4u) ? e.z : -e.z};
}

static f64 box_box_depth(de_vec3 ea, de_vec3 eb, const de__np_rel *rel);

/* Exact distance between separated boxes A (pair frame) and B (mapped by rel): vertex-solid and
 * edge-edge minima. */
static f64 box_box_distance(de_vec3 ea, de_vec3 eb, const de__np_rel *rel) {
    f64 best = 1e30;
    for (u32 v = 0; v < 8; ++v) {
        const de_vec3 pb = de__np_to_a(rel, corner(eb, v));
        const f64 d1 = point_box(de__d3_from(pb), ea);
        const f64 d2 = point_box(de__d3_from(de__np_to_b(rel, corner(ea, v))), eb);
        best = d1 < best ? d1 : best;
        best = d2 < best ? d2 : best;
    }
    for (u32 v = 0; v < 8; ++v)
        for (u32 i = 0; i < 3; ++i) {
            if (v & (1u << i))
                continue;
            const de_vec3 a0 = corner(ea, v), a1 = corner(ea, v | (1u << i));
            for (u32 w = 0; w < 8; ++w)
                for (u32 j = 0; j < 3; ++j) {
                    if (w & (1u << j))
                        continue;
                    const de_vec3 b0 = de__np_to_a(rel, corner(eb, w));
                    const de_vec3 b1 = de__np_to_a(rel, corner(eb, w | (1u << j)));
                    f32 s, t;
                    de_vec3 ca, cb;
                    de__np_segments_closest(a0, a1, b0, b1, &s, &t, &ca, &cb);
                    const f64 d =
                        de_dsqrt64(de__d3_dot(de__d3_sub(de__d3_from(cb), de__d3_from(ca)),
                                              de__d3_sub(de__d3_from(cb), de__d3_from(ca))));
                    best = d < best ? d : best;
                }
        }
    return best;
}

/* Minimum translation depth of overlapping boxes by the 15 axes (0 when separated). */
static f64 box_box_depth(de_vec3 ea, de_vec3 eb, const de__np_rel *rel) {
    const f32 *m = rel->r.m;
    de_dvec3 axes[15];
    u32 n = 0;
    for (u32 i = 0; i < 3; ++i)
        axes[n++] = de__d3(i == 0, i == 1, i == 2);
    for (u32 j = 0; j < 3; ++j)
        axes[n++] = de__d3(m[j * 3], m[j * 3 + 1], m[j * 3 + 2]);
    for (u32 i = 0; i < 3; ++i)
        for (u32 j = 0; j < 3; ++j) {
            const de_dvec3 c = de__d3_cross(axes[i], axes[3 + j]);
            const f64 l2 = de__d3_dot(c, c);
            if (l2 > 1e-6)
                axes[n++] = de__d3_scale(c, 1.0 / de_dsqrt64(l2));
        }
    const f64 h[3] = {ea.x, ea.y, ea.z}, g[3] = {eb.x, eb.y, eb.z};
    f64 depth = 1e30;
    for (u32 k = 0; k < n; ++k) {
        const de_dvec3 l = axes[k];
        f64 ra = 0.0, rb = 0.0;
        for (u32 i = 0; i < 3; ++i) {
            ra += h[i] * absd(i == 0 ? l.x : (i == 1 ? l.y : l.z));
            rb += g[i] * absd(l.x * m[i * 3] + l.y * m[i * 3 + 1] + l.z * m[i * 3 + 2]);
        }
        const f64 o = ra + rb - absd(de__d3_dot(l, de__d3_from(rel->t)));
        if (o < depth)
            depth = o;
    }
    return depth > 0.0 ? depth : 0.0;
}

/* ---- distance ---------------------------------------------------------------------------- */

TEST(physics_gjk, distance_matches_brute_force) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    de_rng r = de_rng_seed(17, 2);
    u32 checked = 0;
    for (u32 i = 0; i < 2000; ++i) {
        const de_vec3 ea = rnd3(&r, 0.1f, 1);
        const de_vec3 eb = rnd3(&r, 0.1f, 1);
        const de__shape *a = box(p, ea.x, ea.y, ea.z), *b = box(p, eb.x, eb.y, eb.z);
        de__np_rel rel;
        de__np_rel_init(&rel, pose(0, 0, 0, rnd_quat(&r)), rnd_pose(&r, 0, 0, 0, -3, 3), 0.0f);
        de__gjk_proxy pa, pb;
        de__gjk_proxy_shape(&pa, a, true, NULL);
        de__gjk_proxy_shape(&pb, b, true, &rel);
        de__gjk_result g;
        de__gjk_distance(&pa, &pb, &g);
        const f64 bf = box_box_distance(ea, eb, &rel);
        EXPECT_LE(g.iterations, DE__GJK_MAX_ITER);
        /* Vertex-solid and edge-edge distances are exact only for separated boxes (crossing
         * boxes may have no vertex inside the other). */
        if (box_box_depth(ea, eb, &rel) > 0.0) {
            EXPECT_TRUE(g.overlap || g.distance < 1e-6);
        } else if (bf < 1e-3) {
            EXPECT_TRUE(g.overlap || g.distance < 2e-3);
        } else {
            ASSERT_FALSE(g.overlap);
            EXPECT_FLOAT_EQ(g.distance, bf, 1e-5);
            /* The witnesses are that far apart. */
            const de_dvec3 d = de__d3_sub(g.point_b, g.point_a);
            EXPECT_FLOAT_EQ(de_dsqrt64(de__d3_dot(d, d)), g.distance, 1e-9);
            ++checked;
        }
        ASSERT_EQ(de_shape_destroy(p, a->handle), DE_OK);
        ASSERT_EQ(de_shape_destroy(p, b->handle), DE_OK);
    }
    EXPECT_GT(checked, 1000u);
    de_physics_destroy(p);
}

TEST(physics_gjk, point_to_cylinder_analytic) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *c = cylinder(p, 0.5f, 0.75f), *s = sphere(p, 0.1f);
    de_rng r = de_rng_seed(4, 4);
    for (u32 i = 0; i < 1000; ++i) {
        const de_vec3 q = rnd3(&r, -2, 2);
        const f64 rxy = de_dsqrt64((f64)q.x * q.x + (f64)q.y * q.y);
        const f64 dr = rxy - 0.5, dz = absd(q.z) - 0.75;
        f64 expect;
        if (dr <= 0.0 && dz <= 0.0)
            continue; /* inside */
        expect = dr > 0.0 && dz > 0.0 ? de_dsqrt64(dr * dr + dz * dz) : (dr > 0.0 ? dr : dz);
        if (expect < 1e-3)
            continue;
        de__np_rel rel;
        de__np_rel_init(&rel, pose(0, 0, 0, qid()), pose(q.x, q.y, q.z, qid()), 0.0f);
        de__gjk_proxy pa, pb;
        de__gjk_proxy_shape(&pa, c, true, NULL);
        de__gjk_proxy_shape(&pb, s, true, &rel); /* the sphere's core: its center */
        de__gjk_result g;
        de__gjk_distance(&pa, &pb, &g);
        ASSERT_FALSE(g.overlap);
        EXPECT_FLOAT_EQ(g.distance, expect, 1e-5);
    }
    de_physics_destroy(p);
}

/* ---- EPA --------------------------------------------------------------------------------- */

TEST(physics_gjk, epa_matches_sat_box_box) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    de_rng r = de_rng_seed(23, 9);
    u32 checked = 0;
    for (u32 i = 0; i < 2000; ++i) {
        const de_vec3 ea = rnd3(&r, 0.2f, 1);
        const de_vec3 eb = rnd3(&r, 0.2f, 1);
        const de__shape *a = box(p, ea.x, ea.y, ea.z), *b = box(p, eb.x, eb.y, eb.z);
        const de__pose xa = pose(0, 0, 0, rnd_quat(&r));
        const de__pose xb = rnd_pose(&r, 0, 0, 0, -1.2f, 1.2f);
        de__np_rel rel;
        de__np_rel_init(&rel, xa, xb, 0.0f);
        const f64 depth = box_box_depth(ea, eb, &rel);
        if (depth > 1e-3) {
            ++checked;
            de__gjk_proxy pa, pb;
            de__gjk_proxy_shape(&pa, a, true, NULL);
            de__gjk_proxy_shape(&pb, b, true, &rel);
            de__gjk_result g;
            de__gjk_distance(&pa, &pb, &g);
            ASSERT_TRUE(g.overlap);
            de__epa_result e;
            de__epa_penetration(&pa, &pb, &g, rel.up, &e);
            EXPECT_TRUE(e.ok);
            EXPECT_LE(e.iterations, DE__EPA_MAX_ITER);
            EXPECT_FLOAT_EQ(e.depth, depth, 1e-4);
            /* The GJK/EPA contact and the SAT contact report the same depth. */
            de__np_manifold m1, m2;
            ASSERT_TRUE(de__np_convex_gjk(a, b, &rel, &m1));
            ASSERT_TRUE(de__np_box_box(a, b, &rel, &m2));
            EXPECT_FLOAT_EQ(m1.pts[0].sep, -depth, 1e-4);
            f32 ms = m2.pts[0].sep;
            for (u32 k = 1; k < m2.count; ++k)
                ms = m2.pts[k].sep < ms ? m2.pts[k].sep : ms;
            /* SAT's clipped points may lie inside the deepest corner (it can be clipped off),
             * never deeper. */
            EXPECT_TRUE(ms >= -(f32)depth - 1e-4f);
            if (de__v3_dot(m1.normal, m2.normal) > 0.999f && m2.count == 1u)
                EXPECT_FLOAT_EQ(ms, -depth, 1e-4);
        }
        ASSERT_EQ(de_shape_destroy(p, a->handle), DE_OK);
        ASSERT_EQ(de_shape_destroy(p, b->handle), DE_OK);
    }
    EXPECT_GT(checked, 500u);
    de_physics_destroy(p);
}

TEST(physics_gjk, degenerate_simplices) {
    de__gjk_proxy pa, pb;
    de__gjk_result g;
    de__epa_result e;
    const de_vec3 up = {0, 0, 1};
    /* Two equal points: overlap, EPA cannot build a volume. */
    const de_vec3 pt[1] = {{1, 2, 3}};
    de__gjk_proxy_points(&pa, pt, 1, NULL);
    de__gjk_proxy_points(&pb, pt, 1, NULL);
    de__gjk_distance(&pa, &pb, &g);
    EXPECT_TRUE(g.overlap);
    de__epa_penetration(&pa, &pb, &g, up, &e);
    EXPECT_FALSE(e.ok);
    EXPECT_TRUE(e.depth == 0.0);
    EXPECT_TRUE(de__d3_finite(e.normal) && de__d3_finite(e.point_a));
    /* Coplanar overlapping triangles: flat difference. */
    const de_vec3 t1[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const de_vec3 t2[3] = {{0.2f, 0.2f, 0}, {1.2f, 0.2f, 0}, {0.2f, 1.2f, 0}};
    de__gjk_proxy_points(&pa, t1, 3, NULL);
    de__gjk_proxy_points(&pb, t2, 3, NULL);
    de__gjk_distance(&pa, &pb, &g);
    EXPECT_TRUE(g.overlap);
    de__epa_penetration(&pa, &pb, &g, up, &e);
    EXPECT_TRUE(de__d3_finite(e.normal));
    EXPECT_TRUE(e.depth >= 0.0 && e.depth < 1e-6);
    /* Collinear segments, separated along their line. */
    const de_vec3 s1[2] = {{0, 0, 0}, {1, 0, 0}}, s2[2] = {{1.5f, 0, 0}, {3, 0, 0}};
    de__gjk_proxy_points(&pa, s1, 2, NULL);
    de__gjk_proxy_points(&pb, s2, 2, NULL);
    de__gjk_distance(&pa, &pb, &g);
    EXPECT_FALSE(g.overlap);
    EXPECT_FLOAT_EQ(g.distance, 0.5, 1e-9);
    /* A point exactly on a box face: touching. */
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *b = box(p, 1, 1, 1);
    const de_vec3 on[1] = {{0.25f, -0.5f, 1.0f}};
    de__gjk_proxy_shape(&pa, b, true, NULL);
    de__gjk_proxy_points(&pb, on, 1, NULL);
    de__gjk_distance(&pa, &pb, &g);
    EXPECT_TRUE(g.overlap || g.distance < 1e-9);
    if (g.overlap) {
        de__epa_penetration(&pa, &pb, &g, up, &e);
        EXPECT_TRUE(de__d3_finite(e.normal));
        EXPECT_TRUE(e.depth < 1e-6);
    }
    /* A point inside a tetrahedron: a full simplex. */
    const de_vec3 tet[4] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const de_vec3 in[1] = {{0.1f, 0.2f, 0.3f}};
    de__gjk_proxy_points(&pa, tet, 4, NULL);
    de__gjk_proxy_points(&pb, in, 1, NULL);
    de__gjk_distance(&pa, &pb, &g);
    ASSERT_TRUE(g.overlap);
    de__epa_penetration(&pa, &pb, &g, up, &e);
    EXPECT_TRUE(e.ok);
    EXPECT_FLOAT_EQ(e.depth, 0.1, 1e-6); /* nearest face: x = 0 */
    EXPECT_FLOAT_EQ(e.normal.x, -1.0, 1e-6);
    de_physics_destroy(p);
}

/* ---- the GJK/EPA contact path ------------------------------------------------------------ */

static u32 collide(const de__shape *a, de__pose xa, const de__shape *b, de__pose xb, f32 spec,
                   de__contact_set *sets) {
    de__contact_buffer buf = {sets, CAP, 0, false};
    return de__collide(a, xa, b, xb, spec, &buf);
}

TEST(physics_gjk, cylinder_contacts) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *ground = box(p, 5, 5, 0.5f), *c = cylinder(p, 0.5f, 0.75f);
    const de__shape *s = sphere(p, 0.25f);
    de__contact_set o[CAP];
    /* Standing on its cap. */
    ASSERT_EQ(collide(ground, pose(0, 0, 0, qid()), c, pose(1, 1, 1.24, qid()), 0.0f, o), 1u);
    EXPECT_EQ(o[0].point_count, 1u);
    EXPECT_FLOAT_EQ(o[0].normal.z, 1.0, 1e-4);
    EXPECT_FLOAT_EQ(o[0].points[0].separation, -0.01, 1e-4);
    EXPECT_EQ(de__feature_b(o[0].points[0].feature_id), de__feat(DE__FEAT_SHAPE, 0));
    EXPECT_EQ(de__feat_type(de__feature_a(o[0].points[0].feature_id)), DE__FEAT_FACE);
    /* Lying on its side. */
    ASSERT_EQ(collide(ground, pose(0, 0, 0, qid()), c, pose(1, 1, 0.99, qaxis(1, 0, 0, 1.5707963f)),
                      0.0f, o),
              1u);
    EXPECT_FLOAT_EQ(o[0].normal.z, 1.0, 1e-3);
    EXPECT_FLOAT_EQ(o[0].points[0].separation, -0.01, 1e-3);
    /* Separated, within the speculative distance. */
    ASSERT_EQ(collide(ground, pose(0, 0, 0, qid()), c, pose(1, 1, 1.27, qid()), 0.05f, o), 1u);
    EXPECT_FLOAT_EQ(o[0].points[0].separation, 0.02, 1e-5);
    /* Sphere beside the cylinder's side: analytic. */
    ASSERT_EQ(collide(c, pose(0, 0, 0, qid()), s, pose(0.6, 0.3, 0.2, qid()), 0.1f, o), 1u);
    const f64 d = de_dsqrt64(0.36 + 0.09);
    EXPECT_FLOAT_EQ(o[0].points[0].separation, d - 0.5 - 0.25, 1e-5);
    EXPECT_FLOAT_EQ(o[0].normal.x, 0.6 / d, 1e-5);
    /* Sphere sunk into the side. */
    ASSERT_EQ(collide(c, pose(0, 0, 0, qid()), s, pose(0.7, 0.0, 0.0, qid()), 0.0f, o), 1u);
    EXPECT_FLOAT_EQ(o[0].points[0].separation, -0.05, 1e-3);
    EXPECT_FLOAT_EQ(o[0].normal.x, 1.0, 1e-3);
    /* Coincident sphere and cylinder centers: finite, deep. */
    ASSERT_EQ(collide(c, pose(0, 0, 0, qid()), s, pose(0, 0, 0, qid()), 0.0f, o), 1u);
    EXPECT_TRUE(de__v3_finite(o[0].normal));
    EXPECT_TRUE(o[0].points[0].separation < -0.5f);
    de_physics_destroy(p);
}

TEST(physics_gjk, bounded_iterations) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    de_rng r = de_rng_seed(61, 5);
    const de__shape *cyl[3] = {cylinder(p, 0.5f, 0.2f), cylinder(p, 0.05f, 2.0f),
                               cylinder(p, 1.0f, 1.0f)};
    for (u32 i = 0; i < 1000; ++i) {
        const de__shape *a = cyl[i % 3u], *b = cyl[(i / 3u) % 3u];
        de__np_rel rel;
        de__np_rel_init(&rel, pose(0, 0, 0, rnd_quat(&r)), rnd_pose(&r, 0, 0, 0, -1.5f, 1.5f),
                        0.0f);
        de__gjk_proxy pa, pb;
        de__gjk_proxy_shape(&pa, a, true, NULL);
        de__gjk_proxy_shape(&pb, b, true, &rel);
        de__gjk_result g;
        de__gjk_distance(&pa, &pb, &g);
        EXPECT_LE(g.iterations, DE__GJK_MAX_ITER);
        EXPECT_TRUE(g.distance >= 0.0 && de__d3_finite(g.point_a));
        if (g.overlap) {
            de__epa_result e;
            de__epa_penetration(&pa, &pb, &g, rel.up, &e);
            EXPECT_LE(e.iterations, DE__EPA_MAX_ITER);
            EXPECT_TRUE(e.depth >= 0.0 && de__d3_finite(e.normal) && de__d3_finite(e.point_b));
        }
    }
    de_physics_destroy(p);
}
