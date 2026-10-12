/* test_sat.c: box-box and hull SAT, face clipping, feature ids and their stability (DENG-134,
 * SPEC §13.1). */
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
static const de__shape *box_hull(de_physics_world *p, f32 x, f32 y, f32 z) {
    de_vec3 pts[8];
    for (u32 v = 0; v < 8; ++v)
        pts[v] = (de_vec3){(v & 1u) ? x : -x, (v & 2u) ? y : -y, (v & 4u) ? z : -z};
    de_shape_desc d = desc_of(DE_SHAPE_CONVEX);
    d.points = pts;
    d.point_count = 8;
    return mk(p, &d);
}
static const de__shape *prism(de_physics_world *p, u32 sides, f32 r, f32 h) {
    de_vec3 pts[64];
    for (u32 i = 0; i < sides; ++i) {
        const f32 a = 6.2831853f * (f32)i / (f32)sides;
        pts[2 * i] = (de_vec3){r * de_dcos(a), r * de_dsin(a), -h};
        pts[2 * i + 1] = (de_vec3){r * de_dcos(a), r * de_dsin(a), h};
    }
    de_shape_desc d = desc_of(DE_SHAPE_CONVEX);
    d.points = pts;
    d.point_count = 2 * sides;
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
static de_quat qmul(de_quat a, de_quat b) {
    return de_dnormalize_quat((de_quat){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                                        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                                        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                                        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z});
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
static f32 min_sep(const de__contact_set *s) {
    f32 m = s->points[0].separation;
    for (u32 i = 1; i < s->point_count; ++i)
        if (s->points[i].separation < m)
            m = s->points[i].separation;
    return m;
}
static void sorted_ids(const de__contact_set *s, u32 *ids) {
    for (u32 i = 0; i < s->point_count; ++i) {
        u32 v = s->points[i].feature_id, j = i;
        while (j > 0 && ids[j - 1] > v) {
            ids[j] = ids[j - 1];
            --j;
        }
        ids[j] = v;
    }
}

/* ---- template ---------------------------------------------------------------------------- */

TEST(physics_sat, box_template) {
    de__np_box_poly bp;
    de__np_box_poly_init(&bp, (de_vec3){0.5f, 1.0f, 2.0f});
    const de__np_poly *p = &bp.poly;
    ASSERT_EQ(p->ecount, 24u);
    for (u32 e = 0; e < p->ecount; ++e) {
        const de__hull_edge *h = &p->edges[e];
        EXPECT_EQ(p->edges[h->twin].twin, e);
        EXPECT_EQ(p->edges[h->twin].origin, p->edges[h->next].origin);
        EXPECT_NE(p->edges[h->twin].face, h->face);
        const de__hull_face f = p->faces[h->face];
        EXPECT_TRUE(e >= (u32)f.first_edge && e < (u32)f.first_edge + (u32)f.edge_count);
        /* Counter-clockwise from outside, on the face plane. */
        const de_vec3 v0 = p->verts[h->origin], v1 = p->verts[p->edges[h->next].origin];
        const de_vec3 v2 = p->verts[p->edges[p->edges[h->next].next].origin];
        const de_vec3 n = p->planes[h->face].n;
        EXPECT_TRUE(de__v3_dot(de__v3_cross(de__v3_sub(v1, v0), de__v3_sub(v2, v1)), n) > 0.0f);
        EXPECT_FLOAT_EQ(de__v3_dot(n, v0) + p->planes[h->face].d, 0.0, 1e-6);
    }
    for (u32 f = 0; f < 6; ++f) {
        u32 e = p->faces[f].first_edge, k = 0;
        do {
            e = p->edges[e].next;
            ++k;
        } while (e != p->faces[f].first_edge && k < 10);
        EXPECT_EQ(k, 4u);
    }
    EXPECT_EQ(de__np_box_edge(0, 1), de__np_box_edge(1, 0));
}

/* ---- face contacts ----------------------------------------------------------------------- */

TEST(physics_sat, resting_box_four_points) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *ground = box(p, 5.0f, 5.0f, 0.5f), *b = box(p, 0.5f, 0.5f, 0.5f);
    de__contact_set s[CAP];
    ASSERT_EQ(collide(ground, pose(0, 0, 0, qid()), b, pose(1, 2, 0.99, qid()), 0.0f, s), 1u);
    ASSERT_EQ(s[0].point_count, 4u);
    EXPECT_FLOAT_EQ(s[0].normal.z, 1.0, 1e-6);
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(s[0].points[i].separation, -0.01, 1e-5);
        EXPECT_EQ(de__feature_a(s[0].points[i].feature_id), de__feat(DE__FEAT_FACE, 4));
        const u32 fb = de__feature_b(s[0].points[i].feature_id);
        EXPECT_EQ(de__feat_type(fb), DE__FEAT_VERTEX);
        EXPECT_EQ(de__feat_index(fb) & 4u, 0u); /* a bottom vertex */
    }
    /* The anchors: A's on its top face, B's at its bottom corners. */
    EXPECT_FLOAT_EQ(s[0].points[0].local_a.z, 0.5, 1e-6);
    EXPECT_FLOAT_EQ(s[0].points[0].local_b.z, -0.5, 1e-6);
    /* Swapped: the same contact seen from B. */
    de__contact_set t[CAP];
    ASSERT_EQ(collide(b, pose(1, 2, 0.99, qid()), ground, pose(0, 0, 0, qid()), 0.0f, t), 1u);
    EXPECT_FLOAT_EQ(t[0].normal.z, -1.0, 1e-6);
    EXPECT_EQ(t[0].points[0].feature_id, de__feature_flip(s[0].points[0].feature_id));
    /* Hovering: speculative points only within the margin. */
    ASSERT_EQ(collide(ground, pose(0, 0, 0, qid()), b, pose(1, 2, 1.02, qid()), 0.05f, s), 1u);
    EXPECT_EQ(s[0].point_count, 4u);
    EXPECT_FLOAT_EQ(s[0].points[0].separation, 0.02, 1e-5);
    EXPECT_EQ(collide(ground, pose(0, 0, 0, qid()), b, pose(1, 2, 1.02, qid()), 0.01f, s), 0u);
    de_physics_destroy(p);
}

TEST(physics_sat, clipping_counts) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *a = box(p, 0.5f, 0.5f, 0.5f), *b = box(p, 0.5f, 0.5f, 0.5f);
    const de__shape *ground = box(p, 5.0f, 5.0f, 0.5f);
    de__contact_set s[CAP];
    /* Equal squares, one turned 45 degrees: an octagon of edge-edge clip points. */
    ASSERT_EQ(
        collide(a, pose(0, 0, 0, qid()), b, pose(0, 0, 0.99, qaxis(0, 0, 1, 0.7853982f)), 0.0f, s),
        1u);
    EXPECT_EQ(s[0].point_count, 8u);
    for (u32 i = 0; i < s[0].point_count; ++i) {
        EXPECT_FLOAT_EQ(s[0].points[i].separation, -0.01, 1e-5);
        EXPECT_EQ(de__feat_type(de__feature_a(s[0].points[i].feature_id)), DE__FEAT_EDGE);
        EXPECT_EQ(de__feat_type(de__feature_b(s[0].points[i].feature_id)), DE__FEAT_EDGE);
    }
    /* Hanging over the ground's +X edge: two vertices and two clipped points. */
    ASSERT_EQ(collide(ground, pose(0, 0, 0, qid()), b, pose(4.8, 0, 0.99, qid()), 0.0f, s), 1u);
    ASSERT_EQ(s[0].point_count, 4u);
    u32 verts = 0, clipped = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u32 fb = de__feature_b(s[0].points[i].feature_id);
        if (de__feat_type(fb) == DE__FEAT_VERTEX)
            ++verts;
        if (de__feat_type(fb) == DE__FEAT_EDGE)
            ++clipped;
        const de_vec3 la = s[0].points[i].local_a;
        EXPECT_TRUE(la.x <= 5.0f + 1e-5f);
    }
    EXPECT_EQ(verts, 2u);
    EXPECT_EQ(clipped, 2u);
    de_physics_destroy(p);
}

TEST(physics_sat, edge_edge_one_point) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *a = box(p, 0.5f, 0.5f, 0.5f);
    const de__shape *ha = box_hull(p, 0.5f, 0.5f, 0.5f);
    const de_quat ra = qaxis(1, 0, 0, 0.7853982f), rb = qaxis(0, 1, 0, 0.7853982f);
    const f64 z = 2.0 * 0.70710678118654752 - 0.01;
    de__contact_set s[CAP];
    for (u32 k = 0; k < 3; ++k) {
        /* box-box, hull-hull, hull-box */
        const de__shape *x = k == 0 ? a : ha;
        const de__shape *y = k == 1 ? ha : a;
        ASSERT_EQ(collide(x, pose(0, 0, 0, ra), y, pose(0.05, -0.03, z, rb), 0.0f, s), 1u);
        ASSERT_EQ(s[0].point_count, 1u);
        EXPECT_FLOAT_EQ(s[0].points[0].separation, -0.01, 1e-5);
        EXPECT_FLOAT_EQ(s[0].normal.z, 1.0, 1e-5);
        const u32 id = s[0].points[0].feature_id;
        EXPECT_EQ(de__feat_type(de__feature_a(id)), DE__FEAT_EDGE);
        EXPECT_EQ(de__feat_type(de__feature_b(id)), DE__FEAT_EDGE);
        if (k == 0) {
            /* A's top edge runs along X (its end vertices differ in bit 0), B's bottom edge
             * along Y (bit 1). */
            de__np_box_poly bp;
            de__np_box_poly_init(&bp, (de_vec3){0.5f, 0.5f, 0.5f});
            const u32 ea = de__feat_index(de__feature_a(id)),
                      eb = de__feat_index(de__feature_b(id));
            const de__hull_edge *e1 = &bp.poly.edges[ea], *e2 = &bp.poly.edges[eb];
            EXPECT_EQ(e1->origin ^ bp.poly.edges[e1->next].origin, 1u);
            EXPECT_EQ(e2->origin ^ bp.poly.edges[e2->next].origin, 2u);
        }
    }
    de_physics_destroy(p);
}

TEST(physics_sat, feature_ids_stable_under_perturbation) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *shapes[2][2] = {
        {box(p, 5.0f, 5.0f, 0.5f), box(p, 0.4f, 0.4f, 0.4f)},
        {box_hull(p, 5.0f, 5.0f, 0.5f), box_hull(p, 0.4f, 0.4f, 0.4f)}};
    for (u32 kind = 0; kind < 2; ++kind) {
        const de__shape *ground = shapes[kind][0], *b = shapes[kind][1];
        const de__pose g = pose(0, 0, 0, qid());
        const f64 bx = 0.3, by = -0.2, bz = 0.895; /* 5 mm deep */
        de__contact_set s[CAP];
        ASSERT_EQ(collide(ground, g, b, pose(bx, by, bz, qid()), 0.02f, s), 1u);
        ASSERT_EQ(s[0].point_count, 4u);
        u32 base[4];
        sorted_ids(&s[0], base);
        const f32 mm = 0.001f, half_deg = 0.0087266f;
        u32 tried = 0;
        for (u32 c = 0; c < 27; ++c) {
            const f64 dx = mm * (f32)((i32)(c % 3u) - 1), dy = mm * (f32)((i32)((c / 3u) % 3u) - 1);
            const f64 dz = mm * (f32)((i32)(c / 9u) - 1);
            for (u32 r = 0; r < 7; ++r) {
                de_quat q = qid();
                if (r > 0) {
                    const f32 sgn = (r & 1u) ? half_deg : -half_deg;
                    const u32 axis = (r - 1u) / 2u;
                    q = qaxis(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f,
                              axis == 2 ? 1.0f : 0.0f, sgn);
                }
                ASSERT_EQ(collide(ground, g, b, pose(bx + dx, by + dy, bz + dz, q), 0.02f, s), 1u);
                ASSERT_EQ(s[0].point_count, 4u);
                u32 ids[4];
                sorted_ids(&s[0], ids);
                EXPECT_MEMEQ(ids, base, sizeof ids);
                ++tried;
            }
        }
        EXPECT_EQ(tried, 27u * 7u);
        /* Combined tilt about two axes. */
        const de_quat q2 = qmul(qaxis(1, 0, 0, half_deg), qaxis(0, 1, 0, -half_deg));
        ASSERT_EQ(collide(ground, g, b, pose(bx + mm, by - mm, bz, q2), 0.02f, s), 1u);
        u32 ids[4];
        sorted_ids(&s[0], ids);
        EXPECT_MEMEQ(ids, base, sizeof ids);
    }
    de_physics_destroy(p);
}

TEST(physics_sat, hull_hull_matches_box_box) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *ba = box(p, 0.6f, 0.4f, 0.3f), *bb = box(p, 0.3f, 0.5f, 0.45f);
    const de__shape *ha = box_hull(p, 0.6f, 0.4f, 0.3f), *hb = box_hull(p, 0.3f, 0.5f, 0.45f);
    de_rng r = de_rng_seed(42, 3);
    u32 hits = 0, normal_mismatch = 0;
    for (u32 i = 0; i < 1000; ++i) {
        const de__pose xa = pose(0, 0, 0, rnd_quat(&r));
        const de__pose xb = rnd_pose(&r, 0, 0, 0, -1, 1);
        de__contact_set s1[CAP], s2[CAP], s3[CAP];
        const u32 n1 = collide(ba, xa, bb, xb, 0.0f, s1);
        const u32 n2 = collide(ha, xa, hb, xb, 0.0f, s2);
        const u32 n3 = collide(ha, xa, bb, xb, 0.0f, s3);
        EXPECT_EQ(n1, n2);
        EXPECT_EQ(n1, n3);
        if (n1 != 1u || n2 != 1u || n3 != 1u)
            continue;
        ++hits;
        const de_vec3 a = s1[0].normal, b = s2[0].normal, c = s3[0].normal;
        if (de__v3_dot(a, b) < 0.999f || de__v3_dot(a, c) < 0.999f) {
            ++normal_mismatch; /* a near-tie between axes may pick another, equally deep one */
            continue;
        }
        EXPECT_FLOAT_EQ(min_sep(&s1[0]), min_sep(&s2[0]), 2e-5);
        EXPECT_FLOAT_EQ(min_sep(&s1[0]), min_sep(&s3[0]), 2e-5);
    }
    EXPECT_GT(hits, 300u);
    EXPECT_LT(normal_mismatch * 50u, hits); /* under 2% */
    de_physics_destroy(p);
}

TEST(physics_sat, polygon_faces_reduce_to_eight) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *a = prism(p, 16, 1.0f, 0.3f), *b = prism(p, 16, 1.0f, 0.3f);
    de__contact_set s[CAP];
    /* Face to face, B turned by half a sector: 32 clip points. */
    ASSERT_EQ(
        collide(a, pose(0, 0, 0, qid()), b, pose(0, 0, 0.59, qaxis(0, 0, 1, 0.19634954f)), 0.0f, s),
        1u);
    ASSERT_EQ(s[0].point_count, DE__CONTACT_MAX_POINTS);
    for (u32 i = 0; i < s[0].point_count; ++i) {
        EXPECT_FLOAT_EQ(s[0].points[i].separation, -0.01, 1e-5);
        for (u32 j = 0; j < i; ++j)
            EXPECT_NE(s[0].points[i].feature_id, s[0].points[j].feature_id);
    }
    /* The kept points spread around the rim: their centroid is near the axis. */
    f64 cx = 0.0, cy = 0.0;
    for (u32 i = 0; i < s[0].point_count; ++i) {
        cx += s[0].points[i].local_a.x;
        cy += s[0].points[i].local_a.y;
    }
    EXPECT_FLOAT_EQ(cx / 8.0, 0.0, 0.2);
    EXPECT_FLOAT_EQ(cy / 8.0, 0.0, 0.2);
    /* Tilted: the deepest point is always kept. */
    ASSERT_EQ(collide(a, pose(0, 0, 0, qid()), b,
                      pose(0, 0, 0.58, qmul(qaxis(1, 0, 0, 0.01f), qaxis(0, 0, 1, 0.19634954f))),
                      0.05f, s),
              1u);
    EXPECT_EQ(s[0].point_count, DE__CONTACT_MAX_POINTS);
    EXPECT_TRUE(min_sep(&s[0]) < -0.02f);
    de_physics_destroy(p);
}
