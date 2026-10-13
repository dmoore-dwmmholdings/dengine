/*
 * test_narrow2d.c: the 2D narrowphase (DENG-253, SPEC §13.3, §5.7): each pair function against
 * analytic results, swap symmetry, rounded polygons, far poses, chain segments with ghost
 * vertices (one-sided, open ends, smooth sliding and rolling across joints), and the golden vector
 * of 5,000 de_rng shape pairs (tests/unit/physics2d/vectors/narrow2d_golden.txt). Regenerate it
 * (never by hand) with
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics2d/test_physics2d_narrow
 * --filter=physics2d_narrow.golden_vector
 */
#include "p2d_collide_test_util.h"

#ifndef DE_PHYSICS2D_NARROW_GOLDEN
#error "DE_PHYSICS2D_NARROW_GOLDEN (path of narrow2d_golden.txt) must be defined by the build"
#endif

/* ---- Fixture -------------------------------------------------------------------------------- */

typedef struct nfx {
    de_physics2d_world *p;
} nfx;

static void nfx_setup(nfx *f) {
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = 64;
    ASSERT_EQ(de_physics2d_create(&d, &f->p), DE_OK);
}
static void nfx_teardown(nfx *f) {
    de_physics2d_destroy(f->p);
}

static const de__shape2d *S(de_physics2d_world *p, de_shape s) {
    const de__shape2d *x = de__shape2d_get(p, s);
    ASSERT_TRUE(x != NULL);
    return x;
}

static u32 collide(de_physics2d_world *p, de_shape a, de__xf2d xa, de_shape b, de__xf2d xb,
                   f32 spec, de__contact_set2d *out) {
    return de__collide2d(S(p, a), xa, S(p, b), xb, 0, spec, out);
}

/* World position of a local anchor. */
static void world_of(de__xf2d x, de_vec2 local, double *wx, double *wy) {
    *wx = x.p.x + (double)x.q.c * local.x - (double)x.q.s * local.y;
    *wy = x.p.y + (double)x.q.s * local.x + (double)x.q.c * local.y;
}

/* Every point: separation == dot(world_b - world_a, normal) within tol, normal unit. */
static void check_consistent(const de__contact_set2d *c, de__xf2d xa, de__xf2d xb, double tol) {
    if (c->point_count == 0)
        return;
    const double nl = sqrt((double)c->normal.x * c->normal.x + (double)c->normal.y * c->normal.y);
    EXPECT_FLOAT_EQ(nl, 1.0, 1e-5);
    for (u32 i = 0; i < c->point_count; ++i) {
        double ax, ay, bx, by;
        world_of(xa, c->points[i].local_a, &ax, &ay);
        world_of(xb, c->points[i].local_b, &bx, &by);
        const double s = (bx - ax) * c->normal.x + (by - ay) * c->normal.y;
        EXPECT_FLOAT_EQ(s, (double)c->points[i].separation, tol);
    }
}

/* ---- Circles and capsules ---------------------------------------------------------------------
 */

TEST_F(physics2d_narrow, circle_circle, nfx) {
    de_physics2d_world *p = f->p;
    const de_shape a = tu_circle(p, 0.5f, (de_vec2){0, 0});
    const de_shape b = tu_circle(p, 0.25f, (de_vec2){0.1f, 0});
    de__contact_set2d c;
    /* Centers 1 m apart along the 30 degree line: separation 1 - 0.75. */
    const de__xf2d xa = tu_pose(2, 3, 0);
    const de__xf2d xb = tu_pose(2 + 0.86602540378 - 0.1, 3.5, 0);
    ASSERT_EQ(collide(p, a, xa, b, xb, 0.3f, &c), 1u);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.25f, 1e-6);
    EXPECT_FLOAT_EQ(c.normal.x, 0.8660254f, 1e-6);
    EXPECT_FLOAT_EQ(c.normal.y, 0.5f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].local_a.x, 0.4330127f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].local_b.x, 0.1f - 0.2165064f, 1e-6);
    check_consistent(&c, xa, xb, 1e-6);
    /* Beyond the speculative distance: nothing. */
    EXPECT_EQ(collide(p, a, xa, b, xb, 0.2f, &c), 0u);
    /* Overlap. */
    ASSERT_EQ(collide(p, a, tu_pose(0, 0, 0), b, tu_pose(0.4, 0, 0), 0.0f, &c), 1u);
    EXPECT_FLOAT_EQ(c.points[0].separation, -0.25f, 1e-6);
    /* Coincident centers: a fixed fallback normal, separation -(ra + rb). */
    ASSERT_EQ(collide(p, a, tu_pose(0, 0, 0), b, tu_pose(-0.1, 0, 0), 0.0f, &c), 1u);
    EXPECT_FLOAT_EQ(c.points[0].separation, -0.75f, 1e-6);
    EXPECT_FLOAT_EQ(fabs((double)c.normal.x) + fabs((double)c.normal.y), 1.0, 1e-6);
}

TEST_F(physics2d_narrow, capsule_circle, nfx) {
    de_physics2d_world *p = f->p;
    const de_shape cap = tu_capsule(p, (de_vec2){-1, 0}, (de_vec2){1, 0}, 0.2f);
    const de_shape cir = tu_circle(p, 0.3f, (de_vec2){0, 0});
    de__contact_set2d c;
    /* Over the middle: normal +y, separation 0.6 - 0.5. */
    ASSERT_EQ(collide(p, cap, tu_pose(0, 0, 0), cir, tu_pose(0.3, 0.6, 0), 0.2f, &c), 1u);
    EXPECT_FLOAT_EQ(c.normal.y, 1.0f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.1f, 1e-6);
    /* Beyond the end cap: along the diagonal. */
    ASSERT_EQ(collide(p, cap, tu_pose(0, 0, 0), cir, tu_pose(1.4, 0.3, 0), 0.01f, &c), 1u);
    EXPECT_FLOAT_EQ(c.normal.x, 0.8f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.0f, 1e-6);
    /* The circle first: the same contact, mirrored. */
    de__contact_set2d d;
    ASSERT_EQ(collide(p, cir, tu_pose(1.4, 0.3, 0), cap, tu_pose(0, 0, 0), 0.01f, &d), 1u);
    TU_EXPECT_FEQ(d.normal.x, -c.normal.x);
    TU_EXPECT_FEQ(d.points[0].separation, c.points[0].separation);
    /* Rotated capsule (vertical): circle to its right. */
    const de__xf2d xa = tu_pose(5, 5, 1.5707963f);
    ASSERT_EQ(collide(p, cap, xa, cir, tu_pose(5.6, 5.2, 0), 0.2f, &c), 1u);
    EXPECT_FLOAT_EQ(c.normal.x, 1.0f, 1e-5);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.1f, 1e-5);
    check_consistent(&c, xa, tu_pose(5.6, 5.2, 0), 1e-5);
}

TEST_F(physics2d_narrow, capsule_capsule, nfx) {
    de_physics2d_world *p = f->p;
    const de_shape a = tu_capsule(p, (de_vec2){-1, 0}, (de_vec2){1, 0}, 0.1f);
    const de_shape b = tu_capsule(p, (de_vec2){-0.5f, 0}, (de_vec2){0.5f, 0}, 0.15f);
    de__contact_set2d c;
    /* Parallel, B above A, shifted right past A's end: two points clipped to A's extent. */
    const de__xf2d xb = tu_pose(0.8, 0.3, 0);
    ASSERT_EQ(collide(p, a, tu_pose(0, 0, 0), b, xb, 0.1f, &c), 2u);
    EXPECT_FLOAT_EQ(c.normal.y, 1.0f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.05f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[1].separation, 0.05f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].local_a.x, 0.3f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[1].local_a.x, 1.0f, 1e-6);
    EXPECT_NE(c.points[0].feature_id, c.points[1].feature_id);
    check_consistent(&c, tu_pose(0, 0, 0), xb, 1e-6);
    /* Feature ids do not change when the overlap is clipped differently. */
    de__contact_set2d d;
    ASSERT_EQ(collide(p, a, tu_pose(0, 0, 0), b, tu_pose(0.2, 0.3, 0), 0.1f, &d), 2u);
    EXPECT_EQ(d.points[0].feature_id, c.points[0].feature_id);
    EXPECT_EQ(d.points[1].feature_id, c.points[1].feature_id);
    /* Below A: normal -y. */
    ASSERT_EQ(collide(p, a, tu_pose(0, 0, 0), b, tu_pose(0, -0.3, 0), 0.1f, &c), 2u);
    EXPECT_FLOAT_EQ(c.normal.y, -1.0f, 1e-6);
    /* Crossing (an X): one point. */
    ASSERT_EQ(collide(p, a, tu_pose(0, 0, 0), b, tu_pose(0, 0, 1.0f), 0.0f, &c), 1u);
    EXPECT_TRUE(c.points[0].separation < 0.0f);
    /* End to end along one line: one point along x. */
    ASSERT_EQ(collide(p, a, tu_pose(0, 0, 0), b, tu_pose(1.7, 0, 0), 0.1f, &c), 1u);
    EXPECT_FLOAT_EQ(c.normal.x, 1.0f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.2f - 0.25f, 1e-6);
    /* T shape: B vertical above A's middle: one point. */
    ASSERT_EQ(collide(p, a, tu_pose(0, 0, 0), b, tu_pose(0, 0.74, 1.5707963f), 0.1f, &c), 1u);
    EXPECT_FLOAT_EQ(c.normal.y, 1.0f, 1e-5);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.74f - 0.5f - 0.25f, 1e-5);
}

/* ---- Polygons ---------------------------------------------------------------------------------
 */

TEST_F(physics2d_narrow, polygon_circle, nfx) {
    de_physics2d_world *p = f->p;
    const de_shape box = tu_box(p, 1.0f, 0.5f, 0.0f);
    const de_shape cir = tu_circle(p, 0.25f, (de_vec2){0, 0});
    de__contact_set2d c;
    /* Face region. */
    ASSERT_EQ(collide(p, box, tu_pose(0, 0, 0), cir, tu_pose(0.3, 0.8, 0), 0.1f, &c), 1u);
    EXPECT_FLOAT_EQ(c.normal.y, 1.0f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.05f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].local_a.y, 0.5f, 1e-6);
    /* Vertex region: along the diagonal from the corner. */
    ASSERT_EQ(collide(p, box, tu_pose(0, 0, 0), cir, tu_pose(1.3, 0.9, 0), 0.5f, &c), 1u);
    EXPECT_FLOAT_EQ(c.normal.x, 0.6f, 1e-6);
    EXPECT_FLOAT_EQ(c.normal.y, 0.8f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.25f, 1e-6);
    /* Center inside: the face of least penetration. */
    ASSERT_EQ(collide(p, box, tu_pose(0, 0, 0), cir, tu_pose(0.1, 0.4, 0), 0.0f, &c), 1u);
    EXPECT_FLOAT_EQ(c.normal.y, 1.0f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].separation, -0.35f, 1e-6);
    /* Rounded box: the radius adds to the polygon. */
    const de_shape rbox = tu_box(p, 1.0f, 0.5f, 0.1f);
    ASSERT_EQ(collide(p, rbox, tu_pose(0, 0, 0), cir, tu_pose(0.3, 0.9, 0), 0.1f, &c), 1u);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.05f, 1e-6);
    ASSERT_EQ(collide(p, rbox, tu_pose(0, 0, 0), cir, tu_pose(1.3, 0.9, 0), 0.5f, &c), 1u);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.15f, 1e-6);
    check_consistent(&c, tu_pose(0, 0, 0), tu_pose(1.3, 0.9, 0), 1e-6);
}

TEST_F(physics2d_narrow, box_on_box, nfx) {
    de_physics2d_world *p = f->p;
    const de_shape ground = tu_box(p, 5.0f, 0.5f, 0.0f);
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    de__contact_set2d c;
    /* A unit box 1 cm into the ground: two points at its bottom corners. */
    const de__xf2d xa = tu_pose(0, 0, 0), xb = tu_pose(0.25, 0.99, 0);
    ASSERT_EQ(collide(p, ground, xa, box, xb, 0.02f, &c), 2u);
    EXPECT_FLOAT_EQ(c.normal.x, 0.0f, 1e-7);
    EXPECT_FLOAT_EQ(c.normal.y, 1.0f, 1e-7);
    for (u32 i = 0; i < 2; ++i) {
        EXPECT_FLOAT_EQ(c.points[i].separation, -0.01f, 1e-6);
        EXPECT_FLOAT_EQ(c.points[i].local_b.y, -0.5f, 1e-6);
        EXPECT_FLOAT_EQ(c.points[i].local_a.y, 0.5f, 1e-6);
    }
    EXPECT_FLOAT_EQ(fabs((double)c.points[0].local_b.x - c.points[1].local_b.x), 1.0, 1e-6);
    EXPECT_NE(c.points[0].feature_id, c.points[1].feature_id);
    check_consistent(&c, xa, xb, 1e-6);
    /* Within the speculative margin: still two points (positive separation). */
    ASSERT_EQ(collide(p, ground, xa, box, tu_pose(0.25, 1.015, 0), 0.02f, &c), 2u);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.015f, 1e-6);
    /* Beyond it: nothing. */
    EXPECT_EQ(collide(p, ground, xa, box, tu_pose(0.25, 1.03, 0), 0.02f, &c), 0u);
    /* Tilted 0.3 rad on one corner: the deep corner is in, the other is far up and dropped. */
    ASSERT_EQ(collide(p, ground, xa, box, tu_pose(0.0, 1.13, 0.3f), 0.02f, &c), 1u);
    EXPECT_FLOAT_EQ(c.normal.y, 1.0f, 1e-6);
    /* The swap is exact. */
    de__contact_set2d d;
    ASSERT_EQ(collide(p, box, xb, ground, xa, 0.02f, &d), 2u);
    ASSERT_EQ(collide(p, ground, xa, box, xb, 0.02f, &c), 2u);
    TU_EXPECT_FEQ(d.normal.y, -c.normal.y);
    for (u32 i = 0; i < 2; ++i) {
        EXPECT_MEMEQ(&d.points[i].local_a, &c.points[i].local_b, sizeof(de_vec2));
        EXPECT_EQ(d.points[i].feature_id, de__f2_swap(c.points[i].feature_id));
    }
}

TEST_F(physics2d_narrow, rounded_and_vertex_vertex, nfx) {
    de_physics2d_world *p = f->p;
    const de_shape a = tu_box(p, 0.5f, 0.5f, 0.0f);
    const de_shape r = tu_box(p, 0.4f, 0.4f, 0.1f); /* rounded: same outline, round corners */
    de__contact_set2d c;
    /* Face to face: the radius is part of the surface. */
    ASSERT_EQ(collide(p, a, tu_pose(0, 0, 0), r, tu_pose(0, 1.005, 0), 0.02f, &c), 2u);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.005f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[1].separation, 0.005f, 1e-6);
    /* Corner to rounded corner along the diagonal: one point, normal along the diagonal, the
     * true distance (not the face separation). */
    const double g = 0.01; /* gap along the diagonal between the corner and the arc */
    const double off = 0.5 + 0.4 + (0.1 + g) / sqrt(2.0);
    const de__xf2d xb = tu_pose(off, off, 0);
    ASSERT_EQ(collide(p, a, tu_pose(0, 0, 0), r, xb, 0.02f, &c), 1u);
    EXPECT_FLOAT_EQ(c.normal.x, 0.70710678f, 1e-5);
    EXPECT_FLOAT_EQ(c.normal.y, 0.70710678f, 1e-5);
    EXPECT_FLOAT_EQ(c.points[0].separation, (f32)g, 1e-5);
    check_consistent(&c, tu_pose(0, 0, 0), xb, 1e-5);
    /* Sharp corners apart along the diagonal. */
    const de_shape b = tu_box(p, 0.5f, 0.5f, 0.0f);
    ASSERT_EQ(collide(p, a, tu_pose(0, 0, 0), b, tu_pose(1.01, 1.01, 0), 0.02f, &c), 1u);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.01f * 1.41421356f, 1e-5);
    EXPECT_EQ(de__f2_type(de__f2_a(c.points[0].feature_id)), (u32)DE__F2_VERTEX);
    EXPECT_EQ(de__f2_type(de__f2_b(c.points[0].feature_id)), (u32)DE__F2_VERTEX);
}

TEST_F(physics2d_narrow, polygon_capsule_and_segments, nfx) {
    de_physics2d_world *p = f->p;
    const de_shape box = tu_box(p, 1.0f, 0.5f, 0.0f);
    const de_shape cap = tu_capsule(p, (de_vec2){-0.5f, 0}, (de_vec2){0.5f, 0}, 0.2f);
    const de_shape seg = tu_segment(p, (de_vec2){-2, 0}, (de_vec2){2, 0});
    const de_shape cir = tu_circle(p, 0.3f, (de_vec2){0, 0});
    de__contact_set2d c;
    /* A capsule lying on a box: two points. */
    ASSERT_EQ(collide(p, box, tu_pose(0, 0, 0), cap, tu_pose(0.2, 0.69, 0), 0.02f, &c), 2u);
    EXPECT_FLOAT_EQ(c.normal.y, 1.0f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].separation, -0.01f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[1].separation, -0.01f, 1e-6);
    /* Standing upright: one point. */
    ASSERT_GE(collide(p, box, tu_pose(0, 0, 0), cap, tu_pose(0.2, 1.19, 1.5707963f), 0.02f, &c),
              1u);
    EXPECT_FLOAT_EQ(c.points[0].separation, -0.01f, 1e-5);
    /* A box on a (two-sided) segment, from above and from below. */
    ASSERT_EQ(collide(p, seg, tu_pose(0, 0, 0), box, tu_pose(0.5, 0.49, 0), 0.02f, &c), 2u);
    EXPECT_FLOAT_EQ(c.normal.y, 1.0f, 1e-6);
    EXPECT_FLOAT_EQ(c.points[0].separation, -0.01f, 1e-6);
    ASSERT_EQ(collide(p, seg, tu_pose(0, 0, 0), box, tu_pose(0.5, -0.49, 0), 0.02f, &c), 2u);
    EXPECT_FLOAT_EQ(c.normal.y, -1.0f, 1e-6);
    /* A circle on a segment; a capsule on a segment. */
    ASSERT_EQ(collide(p, seg, tu_pose(0, 0, 0), cir, tu_pose(1, 0.31, 0), 0.02f, &c), 1u);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.01f, 1e-6);
    ASSERT_EQ(collide(p, seg, tu_pose(0, 0, 0), cap, tu_pose(1, 0.21, 0), 0.02f, &c), 2u);
    EXPECT_FLOAT_EQ(c.points[0].separation, 0.01f, 1e-6);
    /* Segment against segment (never paired by the pipeline, but defined): crossing. */
    const de_shape seg2 = tu_segment(p, (de_vec2){0, -1}, (de_vec2){0, 1});
    ASSERT_EQ(collide(p, seg, tu_pose(0, 0, 0), seg2, tu_pose(0.5, 0, 0), 0.0f, &c), 1u);
}

/* ---- Symmetry and far poses --------------------------------------------------------------------
 */

/* A pool of shapes of every kind (chains too). */
#define POOL_N 24
typedef struct shape_pool {
    de_shape s[POOL_N];
    u32 n;
} shape_pool;

static void make_pool(de_physics2d_world *p, shape_pool *sp, de_rng *r) {
    sp->n = 0;
    for (u32 k = 0; k < POOL_N - 2u; ++k) {
        const f32 a = 0.2f + de_rng_f32(r), b = 0.2f + de_rng_f32(r);
        de_shape s;
        switch (k % 7u) {
        case 0:
            s = tu_circle(p, 0.5f * a, (de_vec2){0.2f * b - 0.1f, 0.1f * a});
            break;
        case 1:
            s = tu_capsule(p, (de_vec2){-a, 0.1f * b}, (de_vec2){a, -0.2f * b}, 0.3f * b);
            break;
        case 2:
            s = tu_box(p, a, b, 0.0f);
            break;
        case 3:
            s = tu_box(p, a, b, 0.1f * a);
            break;
        case 4: {
            de_vec2 pts[8];
            for (u32 i = 0; i < 8; ++i) {
                f32 sn, cs;
                de_dsincos(0.785398163f * (f32)i + 0.3f * de_rng_f32(r), &sn, &cs);
                const f32 rr = 0.4f + 0.6f * de_rng_f32(r);
                pts[i] = (de_vec2){rr * cs * a * 1.5f, rr * sn * b * 1.5f};
            }
            s = tu_polygon(p, pts, 8, (k % 2u) ? 0.05f : 0.0f);
            break;
        }
        case 5:
            s = tu_segment(p, (de_vec2){-a, -b}, (de_vec2){a, 0.5f * b});
            break;
        default: {
            de_vec2 pts[5] = {{-0.6f * a, 0.3f},
                              {0.1f, -0.5f * b},
                              {0.7f * a, 0.1f},
                              {0.2f, 0.6f},
                              {-0.4f, 0.5f * a}};
            s = tu_polygon(p, pts, 5, 0.02f);
            break;
        }
        }
        ASSERT_TRUE(DE_HANDLE_VALID(s));
        sp->s[sp->n++] = s;
    }
    de_vec2 terrain[12];
    for (u32 i = 0; i < 12; ++i)
        terrain[i] = (de_vec2){3.0f - 0.55f * (f32)i, 0.4f * de_rng_f32(r) - 0.2f};
    sp->s[sp->n++] = de_shape2d_chain_create(p, terrain, 12, false);
    de_vec2 ring[8];
    for (u32 i = 0; i < 8; ++i) {
        f32 sn, cs;
        de_dsincos(0.785398163f * (f32)i, &sn, &cs);
        ring[i] = (de_vec2){1.5f * cs, 1.5f * sn};
    }
    sp->s[sp->n++] = de_shape2d_chain_create(p, ring, 8, true);
    ASSERT_TRUE(DE_HANDLE_VALID(sp->s[POOL_N - 2]) && DE_HANDLE_VALID(sp->s[POOL_N - 1]));
}

/* Random pair k: shapes, poses (B near A, or near a chain segment), child, speculative. */
typedef struct rand_pair {
    const de__shape2d *a, *b;
    de__xf2d xa, xb;
    u32 child;
    f32 spec;
} rand_pair;

static void random_pair(de_physics2d_world *p, const shape_pool *sp, de_rng *r, f64 origin_x,
                        f64 origin_y, rand_pair *out) {
    out->a = de__shape2d_get(p, sp->s[de_rng_range(r, 0, sp->n)]);
    out->b = de__shape2d_get(p, sp->s[de_rng_range(r, 0, sp->n - 2u)]); /* B never a chain */
    const f32 ang_a = 6.2831853f * de_rng_f32(r);
    const f32 ang_b = 6.2831853f * de_rng_f32(r);
    out->xa =
        tu_pose(origin_x + 4.0 * de_rng_f64(r) - 2.0, origin_y + 4.0 * de_rng_f64(r) - 2.0, ang_a);
    out->child = 0;
    de__d2 center = out->xa.p;
    f32 reach = out->a->bounding_radius;
    if (out->a->kind == DE_SHAPE2D_CHAIN) {
        const de__chain2d *ch = &out->a->u.chain;
        out->child = de_rng_range(r, 0, ch->segment_count);
        const de__chain_segment *g = &ch->segments[out->child];
        const de_vec2 mid = {0.5f * (g->p1.x + g->p2.x), 0.5f * (g->p1.y + g->p2.y)};
        center = de__d2_add(out->xa.p, de__rot2d_apply_d(out->xa.q, de__d2_from(mid)));
        reach = 0.3f;
    }
    const f32 dist = (reach + out->b->bounding_radius) * 1.2f * de_rng_f32(r);
    f32 sn, cs;
    de_dsincos(6.2831853f * de_rng_f32(r), &sn, &cs);
    out->xb = tu_pose(center.x + (f64)(dist * cs), center.y + (f64)(dist * sn), ang_b);
    const u32 sk = de_rng_range(r, 0, 4);
    out->spec = sk == 0 ? 0.0f : (sk == 1 ? 0.02f : (sk == 2 ? 0.1f : 0.5f));
}

TEST_F(physics2d_narrow, swap_symmetry_exact, nfx) {
    de_physics2d_world *p = f->p;
    de_rng r = de_rng_seed(253, 1);
    shape_pool sp;
    make_pool(p, &sp, &r);
    u32 touching = 0;
    for (u32 k = 0; k < 3000; ++k) {
        rand_pair q;
        random_pair(p, &sp, &r, 0.0, 0.0, &q);
        de__contact_set2d ab, ba;
        const u32 n1 = de__collide2d(q.a, q.xa, q.b, q.xb, q.child, q.spec, &ab);
        const u32 n2 = de__collide2d(q.b, q.xb, q.a, q.xa, q.child, q.spec, &ba);
        ASSERT_EQ(n1, n2);
        if (n1 == 0)
            continue;
        ++touching;
        TU_EXPECT_FEQ(ab.normal.x, -ba.normal.x);
        TU_EXPECT_FEQ(ab.normal.y, -ba.normal.y);
        for (u32 i = 0; i < n1; ++i) {
            EXPECT_MEMEQ(&ab.points[i].local_a, &ba.points[i].local_b, sizeof(de_vec2));
            EXPECT_MEMEQ(&ab.points[i].local_b, &ba.points[i].local_a, sizeof(de_vec2));
            TU_EXPECT_FEQ(ab.points[i].separation, ba.points[i].separation);
            EXPECT_EQ(ab.points[i].feature_id, de__f2_swap(ba.points[i].feature_id));
            EXPECT_TRUE(ab.points[i].separation <= q.spec);
        }
        check_consistent(&ab, q.xa, q.xb, 2e-4);
    }
    EXPECT_GT(touching, 600u);
}

TEST_F(physics2d_narrow, far_poses_match_near, nfx) {
    de_physics2d_world *p = f->p;
    de_rng r = de_rng_seed(253, 2);
    shape_pool sp;
    make_pool(p, &sp, &r);
    u32 touching = 0;
    for (u32 k = 0; k < 1000; ++k) {
        rand_pair q;
        random_pair(p, &sp, &r, 0.0, 0.0, &q);
        /* Near poses on an f32 grid, so adding the offset is exact in f64. */
        q.xa.p = de__d2_make((f64)(f32)q.xa.p.x, (f64)(f32)q.xa.p.y);
        q.xb.p = de__d2_make((f64)(f32)q.xb.p.x, (f64)(f32)q.xb.p.y);
        rand_pair far = q;
        /* 100 km away. */
        far.xa.p = de__d2_add(q.xa.p, de__d2_make(-100000.0, 65536.0));
        far.xb.p = de__d2_add(q.xb.p, de__d2_make(-100000.0, 65536.0));
        de__contact_set2d a, b;
        const u32 n1 = de__collide2d(q.a, q.xa, q.b, q.xb, q.child, q.spec, &a);
        const u32 n2 = de__collide2d(far.a, far.xa, far.b, far.xb, far.child, far.spec, &b);
        ASSERT_EQ(n1, n2);
        if (n1 > 0)
            ++touching;
        EXPECT_MEMEQ(&a, &b, sizeof a);
    }
    EXPECT_GT(touching, 200u);
}

/* ---- Chains ------------------------------------------------------------------------------------
 */

/* All contact sets of `b` against every segment of chain shape `ch` (brute force). */
static u32 chain_contacts(de_physics2d_world *p, de_shape ch, de__xf2d xc, de_shape b, de__xf2d xb,
                          f32 spec, de__contact_set2d *sets, u32 *children, u32 cap) {
    const de__shape2d *cs = S(p, ch);
    u32 n = 0;
    for (u32 i = 0; i < cs->u.chain.segment_count; ++i) {
        de__contact_set2d c;
        if (de__collide2d(cs, xc, S(p, b), xb, i, spec, &c) == 0)
            continue;
        ASSERT_LT(n, cap);
        children[n] = i;
        sets[n++] = c;
    }
    return n;
}

TEST_F(physics2d_narrow, chain_one_sided, nfx) {
    de_physics2d_world *p = f->p;
    /* Ground built right to left: collides from above. */
    const de_vec2 pts[3] = {{4, 0}, {0, 0}, {-4, 0}};
    const de_shape ch = de_shape2d_chain_create(p, pts, 3, false);
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    const de_shape cir = tu_circle(p, 0.5f, (de_vec2){0, 0});
    const de_shape cap = tu_capsule(p, (de_vec2){-0.5f, 0}, (de_vec2){0.5f, 0}, 0.25f);
    de__contact_set2d sets[8];
    u32 ch_idx[8];
    const de__xf2d xc = tu_pose(0, 0, 0);
    const de_shape shapes[3] = {box, cir, cap};
    const f32 half[3] = {0.5f, 0.5f, 0.25f};
    for (u32 s = 0; s < 3; ++s) {
        /* From above: contact with normal +y. */
        u32 n = chain_contacts(p, ch, xc, shapes[s], tu_pose(1.5, half[s] - 0.01, 0), 0.02f, sets,
                               ch_idx, 8);
        ASSERT_GE(n, 1u);
        for (u32 i = 0; i < n; ++i) {
            EXPECT_FLOAT_EQ(sets[i].normal.y, 1.0f, 1e-6);
            EXPECT_FLOAT_EQ(sets[i].points[0].separation, -0.01f, 1e-5);
        }
        /* From below (behind): nothing. */
        n = chain_contacts(p, ch, xc, shapes[s], tu_pose(1.5, -half[s] + 0.01, 0), 0.02f, sets,
                           ch_idx, 8);
        EXPECT_EQ(n, 0u);
        /* Deep, center slightly behind: still nothing (one-sided, no pull-through). */
        n = chain_contacts(p, ch, xc, shapes[s], tu_pose(1.5, -0.05, 0), 0.02f, sets, ch_idx, 8);
        EXPECT_EQ(n, 0u);
    }
}

TEST_F(physics2d_narrow, chain_box_slides_without_snagging, nfx) {
    de_physics2d_world *p = f->p;
    /* A flat floor of 40 segments of uneven length, right to left. */
    de_vec2 pts[41];
    f32 x = 10.0f;
    for (u32 i = 0; i < 41; ++i) {
        pts[i] = (de_vec2){x, 0.0f};
        x -= 0.37f + 0.11f * (f32)(i % 3u);
    }
    const de_shape ch = de_shape2d_chain_create(p, pts, 41, false);
    const de_shape box = tu_box(p, 0.5f, 0.25f, 0.0f);
    const de_shape rbox = tu_box(p, 0.45f, 0.2f, 0.05f);
    const de_shape cap = tu_capsule(p, (de_vec2){-0.4f, 0}, (de_vec2){0.4f, 0}, 0.25f);
    const de_shape tilted = tu_box(p, 0.3f, 0.3f, 0.0f);
    const de_shape shapes[4] = {box, rbox, cap, tilted};
    const f32 lift[4] = {0.249f, 0.249f, 0.249f, 0.0f};
    de__contact_set2d sets[16];
    u32 ch_idx[16];
    double worst = 0.0;
    u32 samples = 0;
    for (u32 s = 0; s < 4; ++s) {
        for (u32 k = 0; k < 400; ++k) {
            const f64 bx = 8.0 - 0.04 * (f64)k;
            /* The tilted box rests on its corner: normals must still be +y. */
            const f32 ang = s == 3 ? 0.4f : 0.0f;
            const f64 by = s == 3 ? 0.3 * (0.9210609940 + 0.3894183423) - 0.002 : (f64)lift[s];
            const u32 n = chain_contacts(p, ch, tu_pose(0, 0, 0), shapes[s], tu_pose(bx, by, ang),
                                         0.02f, sets, ch_idx, 16);
            ASSERT_GE(n, 1u);
            for (u32 i = 0; i < n; ++i) {
                const double dev = tu_angle_deg(sets[i].normal, (de_vec2){0.0f, 1.0f});
                if (dev > worst)
                    worst = dev;
                ++samples;
            }
        }
    }
    printf("  %u contact sets, worst normal deviation %.6f deg\n", samples, worst);
    EXPECT_TRUE(worst < 1.0);
}

/* Closest point of the polyline pts[0..n) to q (test-side, double). */
static void closest_on_polyline(const de_vec2 *pts, u32 n, double qx, double qy, double *cx,
                                double *cy) {
    double best = 1e300;
    for (u32 i = 0; i + 1 < n; ++i) {
        const double ax = pts[i].x, ay = pts[i].y;
        const double ex = (double)pts[i + 1].x - ax, ey = (double)pts[i + 1].y - ay;
        double t = ((qx - ax) * ex + (qy - ay) * ey) / (ex * ex + ey * ey);
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        const double px = ax + t * ex, py = ay + t * ey;
        const double d = (qx - px) * (qx - px) + (qy - py) * (qy - py);
        if (d < best) {
            best = d;
            *cx = px;
            *cy = py;
        }
    }
}

TEST_F(physics2d_narrow, chain_circle_rolls_smoothly, nfx) {
    de_physics2d_world *p = f->p;
    /* A hill (convex arc of 60 segments, radius 20) and flat run-outs, right to left. */
    de_vec2 pts[64];
    u32 n = 0;
    pts[n++] = (de_vec2){30.0f, 0.0f};
    for (u32 i = 0; i <= 60; ++i) {
        const f32 ang = 1.0471976f - 1.0471976f * (f32)i / 30.0f; /* 60 .. -60 degrees */
        f32 sn, cs;
        de_dsincos(ang + 1.5707963f, &sn, &cs);
        /* From the right foot over the top to the left foot. */
        pts[n++] = (de_vec2){-20.0f * cs, 20.0f * sn - 10.0f};
    }
    pts[n++] = (de_vec2){-30.0f, 0.0f};
    const de_shape ch = de_shape2d_chain_create(p, pts, n, false);
    ASSERT_TRUE(DE_HANDLE_VALID(ch));
    const f32 rad = 0.5f;
    const de_shape cir = tu_circle(p, rad, (de_vec2){0, 0});
    de__contact_set2d sets[8];
    u32 ch_idx[8];
    double worst = 0.0;
    u32 multi = 0;
    for (u32 k = 0; k < 2000; ++k) {
        /* Walk the center along the offset curve: closest point + (rad - 1 mm) * normal. */
        const double qx = 18.0 - 36.0 * (double)k / 1999.0;
        double cx = 0, cy = 0;
        closest_on_polyline(pts, n, qx, 20.0, &cx, &cy);
        double nx = qx - cx, ny = 20.0 - cy;
        const double l = sqrt(nx * nx + ny * ny);
        nx /= l;
        ny /= l;
        const double px = cx + nx * (rad - 0.001), py = cy + ny * (rad - 0.001);
        double kx = 0, ky = 0;
        closest_on_polyline(pts, n, px, py, &kx, &ky);
        de_vec2 ideal = {(f32)(px - kx), (f32)(py - ky)};
        const u32 m = chain_contacts(p, ch, tu_pose(0, 0, 0), cir, tu_pose(px, py, 0), 0.02f, sets,
                                     ch_idx, 8);
        ASSERT_GE(m, 1u);
        if (m > 1)
            ++multi;
        for (u32 i = 0; i < m; ++i) {
            const double dev = tu_angle_deg(sets[i].normal, ideal);
            if (dev > worst)
                worst = dev;
        }
    }
    printf("  worst normal deviation %.6f deg, %u positions with two sets\n", worst, multi);
    EXPECT_TRUE(worst < 1.0);
    EXPECT_EQ(multi, 0u); /* convex joints: each vertex region belongs to one segment */
}

TEST_F(physics2d_narrow, chain_open_ends_and_corners, nfx) {
    de_physics2d_world *p = f->p;
    const de_vec2 pts[3] = {{2, 0}, {0, 0}, {-2, 0}};
    const de_shape ch = de_shape2d_chain_create(p, pts, 3, false);
    const de_shape cir = tu_circle(p, 0.5f, (de_vec2){0, 0});
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    de__contact_set2d sets[8];
    u32 ch_idx[8];
    /* Past the open end at (-2, 0), up and to the left: the end vertex collides (free corner). */
    u32 n =
        chain_contacts(p, ch, tu_pose(0, 0, 0), cir, tu_pose(-2.3, 0.4, 0), 0.02f, sets, ch_idx, 8);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(ch_idx[0], 1u);
    EXPECT_FLOAT_EQ(sets[0].normal.x, -0.6f, 1e-5);
    EXPECT_FLOAT_EQ(sets[0].normal.y, 0.8f, 1e-5);
    /* The other open end, (2, 0). */
    n = chain_contacts(p, ch, tu_pose(0, 0, 0), cir, tu_pose(2.3, 0.4, 0), 0.02f, sets, ch_idx, 8);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(ch_idx[0], 0u);
    EXPECT_FLOAT_EQ(sets[0].normal.x, 0.6f, 1e-5);
    /* A box past the open end, its corner over the end: contact (not skipped). */
    n = chain_contacts(p, ch, tu_pose(0, 0, 0), box, tu_pose(-2.45, 0.49, 0), 0.02f, sets, ch_idx,
                       8);
    ASSERT_GE(n, 1u);
    /* Over the shared vertex (0, 0): exactly one segment reports. */
    n = chain_contacts(p, ch, tu_pose(0, 0, 0), cir, tu_pose(0.0, 0.49, 0), 0.02f, sets, ch_idx, 8);
    EXPECT_EQ(n, 1u);
    /* A valley (concave corner): a circle in it touches both faces. */
    const de_vec2 v[3] = {{2, 2}, {0, 0}, {-2, 2}};
    const de_shape valley = de_shape2d_chain_create(p, v, 3, false);
    n = chain_contacts(p, valley, tu_pose(0, 0, 0), cir, tu_pose(0.0, 0.7, 0), 0.02f, sets, ch_idx,
                       8);
    ASSERT_EQ(n, 2u);
    EXPECT_FLOAT_EQ(sets[0].normal.x, -0.70710678f, 1e-5);
    EXPECT_FLOAT_EQ(sets[1].normal.x, 0.70710678f, 1e-5);
    /* A loop (counter-clockwise square): solid inside, so a box touching it from outside
     * collides and one inside does not. */
    const de_vec2 sq[4] = {{-3, -3}, {3, -3}, {3, 3}, {-3, 3}};
    const de_shape loop = de_shape2d_chain_create(p, sq, 4, true);
    n = chain_contacts(p, loop, tu_pose(0, 0, 0), box, tu_pose(0.0, 3.49, 0), 0.02f, sets, ch_idx,
                       8);
    ASSERT_EQ(n, 1u);
    EXPECT_FLOAT_EQ(sets[0].normal.y, 1.0f, 1e-6);
    n = chain_contacts(p, loop, tu_pose(0, 0, 0), box, tu_pose(0.0, 2.51, 0), 0.02f, sets, ch_idx,
                       8);
    EXPECT_EQ(n, 0u);
    /* A box over the loop's corner (3, 3), diagonally outside: one contact (vertex region). */
    n = chain_contacts(p, loop, tu_pose(0, 0, 0), cir, tu_pose(3.3, 3.4, 0), 0.02f, sets, ch_idx,
                       8);
    ASSERT_EQ(n, 1u);
    EXPECT_FLOAT_EQ(sets[0].normal.x, 0.6f, 1e-5);
}

/* ---- Golden vector ------------------------------------------------------------------------- */

#define GOLDEN_PAIRS 5000u
#define GOLDEN_BLOCK 100u

static u32 compute_golden(tu_golden *out) {
    de_physics2d_world *p;
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = 16;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    de_rng r = de_rng_seed(0x2D253, 7);
    shape_pool sp;
    make_pool(p, &sp, &r);
    u32 n = 0;
    tu_hash block = {0};
    u32 touching = 0, points = 0;
    tu_hash per_kind[5][5];
    memset(per_kind, 0, sizeof per_kind);
    for (u32 k = 0; k < GOLDEN_PAIRS; ++k) {
        rand_pair q;
        random_pair(p, &sp, &r, 1000.0 * (f64)(k % 7u), -500.0 * (f64)(k % 5u), &q);
        de__contact_set2d c;
        const u32 m = de__collide2d(q.a, q.xa, q.b, q.xb, q.child, q.spec, &c);
        touching += m > 0 ? 1u : 0u;
        points += m;
        tu_h_set(&block, &c);
        tu_h_set(&per_kind[de__shape2d_rank(q.a->kind)][de__shape2d_rank(q.b->kind)], &c);
        if ((k + 1u) % GOLDEN_BLOCK == 0u) {
            snprintf(out[n].name, sizeof out[n].name, "pairs%04u", k + 1u - GOLDEN_BLOCK);
            out[n++].value = block.h;
            block.h = 0;
        }
    }
    static const char *const kn[5] = {"circle", "capsule", "polygon", "segment", "chain"};
    for (u32 a = 0; a < 5; ++a)
        for (u32 b = 0; b < 5; ++b) {
            if (per_kind[a][b].h == 0)
                continue;
            snprintf(out[n].name, sizeof out[n].name, "%s_%s", kn[a], kn[b]);
            out[n++].value = per_kind[a][b].h;
        }
    snprintf(out[n].name, sizeof out[n].name, "touching");
    out[n++].value = touching;
    snprintf(out[n].name, sizeof out[n].name, "points");
    out[n++].value = points;
    de_physics2d_destroy(p);
    return n;
}

TEST(physics2d_narrow, golden_vector) {
    static tu_golden got[TU_GOLDEN_MAX];
    const u32 n = compute_golden(got);
    ASSERT_LE(n, (u32)TU_GOLDEN_MAX);
    /* Both counts are checked by the vector; sanity bounds here say the pairs are meaningful. */
    EXPECT_GT(got[n - 2u].value, (u64)(GOLDEN_PAIRS / 4u));
    EXPECT_LT(got[n - 2u].value, (u64)(GOLDEN_PAIRS * 3u / 4u));
    if (tu_golden_check(DE_PHYSICS2D_NARROW_GOLDEN,
                        "# narrow2d_golden.txt: XXH3-64 of the contact sets of 5,000 de_rng shape\n"
                        "# pairs (every kind, chains with ghost vertices, far and near poses,\n"
                        "# speculative 0 to 0.5 m), per block of 100 pairs and per kind pair, and\n"
                        "# the touching pair and point counts (DENG-253). Generated by\n"
                        "# test_physics2d_narrow with DE_REGEN_GOLDEN=1; never edit by hand.\n"
                        "# x86-64 and arm64 must reproduce every line.\n",
                        got, n))
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_PHYSICS2D_NARROW_GOLDEN);
}
