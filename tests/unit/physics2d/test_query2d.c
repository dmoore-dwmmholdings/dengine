/*
 * test_query2d.c: the 2D scene queries (DENG-258): rays against every shape kind (analytic
 * distances, normals, inside starts), one-sided chains, raycast_all ordering and truncation,
 * brute-force equality (with and without the broadphase), filters, overlaps (shape and box, the
 * chunked collection), closest points, shape casts (analytic time of impact, never past the
 * contact, brute force), large worlds, argument checks, no allocation, and the golden vector of
 * 10,000 de_rng rays and 1,000 shape casts (tests/unit/physics2d/vectors/query2d_golden.txt).
 * Regenerate it (never by hand) with
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics2d/test_physics2d_query
 * --filter=physics2d_query.golden_vector
 *
 * The tests never step the world (the results do not depend on the solver); the broadphase is
 * created with de__bp2d_ensure where a test needs the tree walk.
 */
#include "p2d_collide_test_util.h"

#include "physics/2d/query2d_internal.h"

#ifndef DE_PHYSICS2D_QUERY_GOLDEN
#error "DE_PHYSICS2D_QUERY_GOLDEN (path of query2d_golden.txt) must be defined by the build"
#endif

#define EPS 1e-5

/* ---- Helpers -------------------------------------------------------------------------------- */

static de_physics2d_world *make_world(u32 max_bodies) {
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = max_bodies;
    de_physics2d_world *p = NULL;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    return p;
}

static de_body sbody(de_physics2d_world *p, de_shape s, f64 x, f64 y, f32 angle) {
    return tu_body(p, DE_BODY_STATIC, s, tu_xf(x, y, angle), 0);
}

static de_dpoint2d dp(f64 x, f64 y) {
    de_dpoint2d r;
    r.x = x;
    r.y = y;
    return r;
}

static bool ray(de_physics2d_world *p, f64 ox, f64 oy, f32 dx, f32 dy, f32 len, de_ray_hit2d *h) {
    return de_physics2d_raycast_ex(p, dp(ox, oy), de__v2(dx, dy), len, NULL, h);
}

static void make_bp(de_physics2d_world *p) {
    de__bp2d *bp = NULL;
    ASSERT_EQ(de__bp2d_ensure(p, &bp), DE_OK);
    de__bp2d_sync(p);
}

/* ---- Rays: analytic ------------------------------------------------------------------------- */

TEST(physics2d_query, ray_circle_analytic) {
    de_physics2d_world *p = make_world(64);
    const de_body b = sbody(p, tu_circle(p, 1.0f, de__v2(0.5f, 0.0f)), 3.0, 1.0, 0.7f);
    /* The center is (3, 1) + rot(0.7) * (0.5, 0). */
    const double cx = 3.0 + 0.5 * cos(0.7), cy = 1.0 + 0.5 * sin(0.7);
    de_rng rng = de_rng_seed(11, 1);
    u32 hits = 0;
    for (u32 i = 0; i < 400; ++i) {
        const double ang = de_rng_f64(&rng) * 6.283185307179586;
        const double ox = cx + 4.0 * cos(ang), oy = cy + 4.0 * sin(ang);
        /* Aim near the center, sometimes past it. */
        const double ax = cx + (de_rng_f64(&rng) - 0.5) * 3.0 - ox;
        const double ay = cy + (de_rng_f64(&rng) - 0.5) * 3.0 - oy;
        const double al = sqrt(ax * ax + ay * ay);
        const double dx = ax / al, dy = ay / al;
        de_ray_hit2d h;
        const bool hit = ray(p, ox, oy, (f32)dx, (f32)dy, 10.0f, &h);
        /* Analytic, with the f32 direction the query normalizes. */
        const double fx = (double)(f32)dx, fy = (double)(f32)dy, fl = sqrt(fx * fx + fy * fy);
        const double ux = fx / fl, uy = fy / fl;
        const double mx = ox - cx, my = oy - cy;
        const double bq = mx * ux + my * uy, cq = mx * mx + my * my - 1.0, disc = bq * bq - cq;
        const bool want = disc >= 0.0 && bq < 0.0;
        EXPECT_EQ(hit, want);
        if (!hit || !want)
            continue;
        ++hits;
        const double s = -bq - sqrt(disc);
        EXPECT_FLOAT_EQ((double)h.fraction * 10.0, s, EPS);
        const double px = ox + s * ux, py = oy + s * uy;
        EXPECT_FLOAT_EQ(h.point.x, px, EPS);
        EXPECT_FLOAT_EQ(h.point.y, py, EPS);
        EXPECT_FLOAT_EQ(h.normal.x, px - cx, EPS);
        EXPECT_FLOAT_EQ(h.normal.y, py - cy, EPS);
        EXPECT_EQ(h.body.id, b.id);
        EXPECT_EQ(h.sub_shape, DE_SUB_SHAPE_NONE);
    }
    EXPECT_GT(hits, 100u);
    /* Inside: fraction 0, normal -dir, point = origin. */
    de_ray_hit2d h;
    ASSERT_TRUE(ray(p, cx, cy + 0.5, 0.0f, 2.0f, 5.0f, &h));
    TU_EXPECT_FEQ(h.fraction, 0.0f);
    TU_EXPECT_FEQ(h.normal.x, 0.0f);
    TU_EXPECT_FEQ(h.normal.y, -1.0f);
    TU_EXPECT_FEQ(h.point.y, cy + 0.5);
    de_physics2d_destroy(p);
}

TEST(physics2d_query, ray_box_and_rounded_box) {
    de_physics2d_world *p = make_world(64);
    (void)sbody(p, tu_box(p, 1.0f, 0.5f, 0.0f), 0.0, 0.0, 0.0f);
    de_ray_hit2d h;
    /* Faces. */
    ASSERT_TRUE(ray(p, -5.0, 0.2, 1.0f, 0.0f, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 4.0, EPS);
    TU_EXPECT_FEQ(h.normal.x, -1.0f);
    ASSERT_TRUE(ray(p, 0.3, 3.0, 0.0f, -1.0f, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 2.5, EPS);
    TU_EXPECT_FEQ(h.normal.y, 1.0f);
    /* Diagonal onto the corner region: enters through the top face at x = 0.9. */
    ASSERT_TRUE(ray(p, -0.1, 1.5, 1.0f, -1.0f, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.point.x, 0.9, EPS);
    EXPECT_FLOAT_EQ(h.point.y, 0.5, EPS);
    /* Misses beside it and short of it. */
    EXPECT_FALSE(ray(p, -5.0, 0.6, 1.0f, 0.0f, 10.0f, &h));
    EXPECT_FALSE(ray(p, -5.0, 0.0, 1.0f, 0.0f, 3.9f, &h));
    EXPECT_TRUE(ray(p, -5.0, 0.0, 1.0f, 0.0f, 4.0f, &h));
    de_physics2d_destroy(p);

    /* A rotated rounded box (r = 0.2): faces sit r out, corners are arcs. */
    p = make_world(64);
    (void)sbody(p, tu_box(p, 1.0f, 0.5f, 0.2f), 2.0, -1.0, 0.5f);
    const double c = cos(0.5), s = sin(0.5);
    /* Along the box's -x axis toward its +x face. */
    const double ox = 2.0 + 5.0 * c, oy = -1.0 + 5.0 * s;
    ASSERT_TRUE(ray(p, ox, oy, (f32)-c, (f32)-s, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 5.0 - 1.2, EPS);
    EXPECT_FLOAT_EQ(h.normal.x, c, EPS);
    EXPECT_FLOAT_EQ(h.normal.y, s, EPS);
    /* At the corner (1, 0.5) local, along the diagonal: the arc of radius 0.2. */
    const double lx = 1.0 + 3.0, ly = 0.5 + 3.0; /* local origin of the ray, then toward -(1,1) */
    const double wx = 2.0 + c * lx - s * ly, wy = -1.0 + s * lx + c * ly;
    const double dlx = -0.7071067811865476, dly = -0.7071067811865476;
    const double dwx = c * dlx - s * dly, dwy = s * dlx + c * dly;
    ASSERT_TRUE(ray(p, wx, wy, (f32)dwx, (f32)dwy, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 3.0 * 1.4142135623730951 - 0.2, 2.0 * EPS);
    de_physics2d_destroy(p);
}

TEST(physics2d_query, ray_capsule_polygon_segment) {
    de_physics2d_world *p = make_world(64);
    /* Capsule from (-1, 0) to (1, 0), r 0.5, at (0, 5). */
    const de_body cap = sbody(p, tu_capsule(p, de__v2(-1, 0), de__v2(1, 0), 0.5f), 0.0, 5.0, 0.0f);
    de_ray_hit2d h;
    ASSERT_TRUE(ray(p, 0.3, 8.0, 0.0f, -1.0f, 10.0f, &h)); /* side */
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 2.5, EPS);
    TU_EXPECT_FEQ(h.normal.y, 1.0f);
    EXPECT_EQ(h.body.id, cap.id);
    ASSERT_TRUE(ray(p, -4.0, 5.0, 1.0f, 0.0f, 10.0f, &h)); /* cap */
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 2.5, EPS);
    TU_EXPECT_FEQ(h.normal.x, -1.0f);
    ASSERT_TRUE(ray(p, -1.3, 8.0, 0.0f, -1.0f, 10.0f, &h)); /* cap, off center */
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 3.0 - 0.4, EPS);
    EXPECT_FLOAT_EQ(h.normal.x, -0.6, EPS);
    EXPECT_FLOAT_EQ(h.normal.y, 0.8, EPS);

    /* A hexagon. */
    const de_vec2 hex[6] = {{1, 0},  {0.5f, 0.8660254f},   {-0.5f, 0.8660254f},
                            {-1, 0}, {-0.5f, -0.8660254f}, {0.5f, -0.8660254f}};
    (void)sbody(p, tu_polygon(p, hex, 6, 0.0f), 10.0, 0.0, 0.0f);
    ASSERT_TRUE(ray(p, 10.0, 5.0, 0.0f, -1.0f, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 5.0 - 0.8660254, EPS);
    ASSERT_TRUE(ray(p, 15.0, 0.0, -1.0f, 0.0f, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 4.0, EPS);

    /* A two-sided segment from both sides. */
    (void)sbody(p, tu_segment(p, de__v2(-1, 0), de__v2(1, 0)), 20.0, 0.0, 0.0f);
    ASSERT_TRUE(ray(p, 20.2, 2.0, 0.0f, -1.0f, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 2.0, EPS);
    TU_EXPECT_FEQ(h.normal.y, 1.0f);
    ASSERT_TRUE(ray(p, 20.2, -2.0, 0.0f, 1.0f, 10.0f, &h));
    TU_EXPECT_FEQ(h.normal.y, -1.0f);
    /* Parallel: no hit. */
    EXPECT_FALSE(ray(p, 17.0, 0.0, 1.0f, 0.0f, 2.5f, &h));
    de_physics2d_destroy(p);
}

TEST(physics2d_query, ray_chain_one_sided) {
    de_physics2d_world *p = make_world(64);
    /* A zigzag running right to left: collides from above. */
    const de_vec2 pts[5] = {{4, 0}, {2, 1}, {0, 0}, {-2, 1}, {-4, 0}};
    const de_body ch = sbody(p, de_shape2d_chain_create(p, pts, 5, false), 0.0, 0.0, 0.0f);
    de_ray_hit2d h;
    ASSERT_TRUE(ray(p, 1.0, 5.0, 0.0f, -1.0f, 10.0f, &h));
    EXPECT_EQ(h.body.id, ch.id);
    EXPECT_EQ(h.sub_shape, 1u); /* (2, 1) -> (0, 0) */
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 4.5, EPS);
    EXPECT_FLOAT_EQ(h.normal.x, -0.4472136, EPS); /* right side of (-2, -1) */
    EXPECT_FLOAT_EQ(h.normal.y, 0.8944272, EPS);
    /* From below: passes through every segment. */
    EXPECT_FALSE(ray(p, 1.0, -5.0, 0.0f, 1.0f, 10.0f, &h));
    /* A low horizontal ray from the left: hits the first segment facing it (front sides only). */
    ASSERT_TRUE(ray(p, -6.0, 0.25, 1.0f, 0.0f, 20.0f, &h));
    EXPECT_EQ(h.sub_shape, 3u); /* (-2, 1) -> (-4, 0): faces up-left */
    EXPECT_FLOAT_EQ(h.point.x, -3.5, EPS);
    /* Exactly through a shared vertex: the lower segment index wins the tie. */
    ASSERT_TRUE(ray(p, 2.0, 5.0, 0.0f, -1.0f, 10.0f, &h));
    EXPECT_EQ(h.sub_shape, 0u);
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 4.0, EPS);
    /* A closed loop counter-clockwise: solid inside, but a ray from inside passes out. */
    const de_vec2 sq[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    (void)sbody(p, de_shape2d_chain_create(p, sq, 4, true), 20.0, 0.0, 0.0f);
    EXPECT_FALSE(ray(p, 20.0, 0.0, 1.0f, 0.0f, 0.9f, &h));
    ASSERT_TRUE(ray(p, 25.0, 0.0, -1.0f, 0.0f, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 4.0, EPS);
    de_physics2d_destroy(p);
}

/* ---- raycast_all ------------------------------------------------------------------------------
 */

TEST(physics2d_query, raycast_all_order_and_truncation) {
    de_physics2d_world *p = make_world(64);
    const de_shape c = tu_circle(p, 0.5f, de__v2(0, 0));
    de_body b[6];
    /* Created out of order: index order != distance order. */
    const f64 xs[6] = {5.0, 2.0, 8.0, 3.0, 2.0, 11.0};
    for (u32 i = 0; i < 6; ++i)
        b[i] = sbody(p, c, xs[i], 0.0, 0.0f);
    for (u32 pass = 0; pass < 2; ++pass) {
        if (pass == 1)
            make_bp(p);
        de_ray_hit2d hits[8];
        const u32 n = de_physics2d_raycast_all(p, dp(0.0, 0.0), de__v2(1, 0), 10.0f, NULL, hits, 8);
        ASSERT_EQ(n, 5u); /* x = 11 is out of reach (10.5) */
        /* 2.0 twice (ties: lower index first), 3, 5, 8. */
        EXPECT_EQ(hits[0].body.id, b[1].id);
        EXPECT_EQ(hits[1].body.id, b[4].id);
        EXPECT_EQ(hits[2].body.id, b[3].id);
        EXPECT_EQ(hits[3].body.id, b[0].id);
        EXPECT_EQ(hits[4].body.id, b[2].id);
        for (u32 i = 1; i < n; ++i)
            EXPECT_TRUE(hits[i - 1].fraction <= hits[i].fraction);
        /* Truncated: the first three of that order. */
        de_ray_hit2d few[3];
        ASSERT_EQ(de_physics2d_raycast_all(p, dp(0.0, 0.0), de__v2(1, 0), 10.0f, NULL, few, 3), 3u);
        EXPECT_EQ(few[0].body.id, b[1].id);
        EXPECT_EQ(few[1].body.id, b[4].id);
        EXPECT_EQ(few[2].body.id, b[3].id);
        /* The closest ray is the first of raycast_all. */
        de_ray_hit2d one;
        ASSERT_TRUE(ray(p, 0.0, 0.0, 1.0f, 0.0f, 10.0f, &one));
        EXPECT_EQ(one.body.id, b[1].id);
        EXPECT_EQ(de_physics2d_raycast_all(p, dp(0.0, 0.0), de__v2(1, 0), 10.0f, NULL, NULL, 3),
                  0u);
    }
    de_physics2d_destroy(p);
}

/* ---- Brute force ------------------------------------------------------------------------------
 */

/* A random scene of every kind (static and kinematic; chains, segments, rounded polygons). */
static void random_scene(de_physics2d_world *p, de_rng *rng, u32 count, f64 ox, f64 oy) {
    de_shape shapes[8];
    shapes[0] = tu_circle(p, 0.4f, de__v2(0.1f, -0.2f));
    shapes[1] = tu_capsule(p, de__v2(-0.6f, 0), de__v2(0.6f, 0.2f), 0.25f);
    shapes[2] = tu_box(p, 0.7f, 0.3f, 0.0f);
    shapes[3] = tu_box(p, 0.5f, 0.5f, 0.1f);
    const de_vec2 pent[5] = {{0.6f, 0}, {0.2f, 0.6f}, {-0.5f, 0.4f}, {-0.5f, -0.4f}, {0.2f, -0.6f}};
    shapes[4] = tu_polygon(p, pent, 5, 0.05f);
    shapes[5] = tu_segment(p, de__v2(-0.8f, -0.1f), de__v2(0.8f, 0.3f));
    const de_vec2 wave[6] = {{3, 0}, {2, 0.5f}, {1, 0}, {0, 0.6f}, {-1, 0.1f}, {-2, 0.4f}};
    shapes[6] = de_shape2d_chain_create(p, wave, 6, false);
    const de_vec2 loop[5] = {{-1, -1}, {1, -1}, {1.5f, 0.5f}, {0, 1.2f}, {-1.2f, 0.3f}};
    shapes[7] = de_shape2d_chain_create(p, loop, 5, true);
    for (u32 i = 0; i < count; ++i) {
        const u32 k = de_rng_range(rng, 0, 8);
        const f64 x = ox + (de_rng_f64(rng) - 0.5) * 40.0;
        const f64 y = oy + (de_rng_f64(rng) - 0.5) * 40.0;
        const f32 a = (f32)((de_rng_f64(rng) - 0.5) * 6.28);
        const de_body_type type = (i % 3u == 0u) ? DE_BODY_KINEMATIC : DE_BODY_STATIC;
        const de_body b = tu_body(p, type, shapes[k], tu_xf(x, y, a), 0);
        ASSERT_TRUE(DE_HANDLE_VALID(b));
    }
}

/* The closest ray over every body, by the public key (f32 fraction, body index). */
static bool brute_ray(const de_physics2d_world *p, de_dpoint2d o, de_vec2 d, f32 len,
                      de_ray_hit2d *out) {
    de__q2ray r;
    if (!de__q2ray_init(&r, de__d2_make(o.x, o.y), d, len))
        return false;
    bool found = false;
    de__q2hit best;
    memset(&best, 0, sizeof best);
    for (u32 i = 0; i < p->bodies.high_water; ++i) {
        if (!de__body2d_alive(p, i) || !DE_HANDLE_VALID(p->bodies.shape[i]))
            continue;
        de__q2hit h;
        if (!de__q2_ray_body(p, i, &r, 1.0, &h))
            continue;
        if (!found || (f32)h.t < (f32)best.t || ((f32)h.t == (f32)best.t && i < best.body)) {
            found = true;
            best = h;
        }
    }
    if (found)
        *out = de__q2hit_public(p, &best);
    return found;
}

static bool same_hit(const de_ray_hit2d *a, const de_ray_hit2d *b) {
    return a->body.id == b->body.id && a->fraction == b->fraction && a->point.x == b->point.x &&
           a->point.y == b->point.y && a->normal.x == b->normal.x && a->normal.y == b->normal.y &&
           a->sub_shape == b->sub_shape;
}

TEST(physics2d_query, rays_equal_brute_force) {
    de_physics2d_world *p = make_world(512);
    de_rng rng = de_rng_seed(2024, 7);
    random_scene(p, &rng, 300, 0.0, 0.0);
    u32 mismatches[2] = {0, 0}, hits = 0;
    for (u32 pass = 0; pass < 2; ++pass) {
        if (pass == 1)
            make_bp(p);
        de_rng r2 = de_rng_seed(99, 3);
        for (u32 i = 0; i < 2000; ++i) {
            const de_dpoint2d o =
                dp((de_rng_f64(&r2) - 0.5) * 44.0, (de_rng_f64(&r2) - 0.5) * 44.0);
            const de_vec2 d = de__v2((f32)(de_rng_f64(&r2) - 0.5), (f32)(de_rng_f64(&r2) - 0.5));
            const f32 len = (f32)(1.0 + de_rng_f64(&r2) * 30.0);
            de_ray_hit2d a, b;
            const bool ha = de_physics2d_raycast_ex(p, o, d, len, NULL, &a);
            const bool hb = brute_ray(p, o, d, len, &b);
            if (ha != hb || (ha && !same_hit(&a, &b)))
                ++mismatches[pass];
            if (ha && pass == 0)
                ++hits;
            /* raycast_all: every hit is a brute-force per-body hit, in order. */
            de_ray_hit2d all[64];
            const u32 n = de_physics2d_raycast_all(p, o, d, len, NULL, all, 64);
            if (ha) {
                ASSERT_GT(n, 0u);
                EXPECT_TRUE(same_hit(&all[0], &a));
            } else {
                EXPECT_EQ(n, 0u);
            }
            for (u32 k = 1; k < n; ++k)
                EXPECT_TRUE(all[k - 1].fraction < all[k].fraction ||
                            (all[k - 1].fraction == all[k].fraction &&
                             tu_index(all[k - 1].body) < tu_index(all[k].body)));
        }
    }
    EXPECT_EQ(mismatches[0], 0u);
    EXPECT_EQ(mismatches[1], 0u);
    EXPECT_GT(hits, 500u);
    de_physics2d_destroy(p);
}

/* The first cast hit over every body, by (t, body index). */
static bool brute_cast(const de_physics2d_world *p, de_shape shape, const de_transform2d *from,
                       de_vec2 dir, f32 len, de_ray_hit2d *out) {
    de__q2ray r;
    if (!de__q2ray_init(&r, de__d2_make(from->x, from->y), dir, len))
        return false;
    de__q2cast c;
    if (!de__q2cast_init(&c, de__shape2d_get(p, shape), de__d2_make(from->x, from->y),
                         from->rotation, de__d2_scale(r.dir, r.length)))
        return false;
    bool found = false;
    de__q2hit best;
    memset(&best, 0, sizeof best);
    for (u32 i = 0; i < p->bodies.high_water; ++i) {
        if (!de__body2d_alive(p, i) || !DE_HANDLE_VALID(p->bodies.shape[i]))
            continue;
        de__q2hit h;
        if (!de__q2_cast_body(p, i, &c, &h))
            continue;
        if (!found || de__q2hit_before(&h, &best)) {
            found = true;
            best = h;
        }
    }
    if (found)
        *out = de__q2hit_public(p, &best);
    return found;
}

/* Surface separation of `shape` at (pos, rot) and body b (GJK on the cores in a frame at pos;
 * chains: segment `sub` only, as the others may be passed from behind). */
static f64 separation(const de_physics2d_world *p, de_shape shape, de__d2 pos, de_rot2d rot, u32 b,
                      u32 sub) {
    const de__shape2d *qs = de__shape2d_get(p, shape);
    const de__shape2d *bs = de__shape2d_get(p, p->bodies.shape[b]);
    de__q2proxy q;
    de__q2proxy_shape(&q, qs, de__rot2d_normalize(rot), de__d2_make(0.0, 0.0));
    const de__d2 off = de__d2_sub(p->bodies.position[b], pos);
    const de_rot2d br = p->bodies.rotation[b];
    f64 best = 1e30;
    const u32 n = bs->kind == DE_SHAPE2D_CHAIN ? bs->u.chain.segment_count : 1u;
    for (u32 i = 0; i < n; ++i) {
        de__q2proxy t;
        if (bs->kind == DE_SHAPE2D_CHAIN && i != sub)
            continue;
        if (bs->kind == DE_SHAPE2D_CHAIN) {
            const de__chain_segment *sg = &bs->u.chain.segments[i];
            const de__d2 a = de__d2_add(off, de__rot2d_apply_d(br, de__d2_from(sg->p1)));
            const de__d2 e = de__d2_add(off, de__rot2d_apply_d(br, de__d2_from(sg->p2)));
            de__q2proxy_segment(&t, de__v2((f32)a.x, (f32)a.y), de__v2((f32)e.x, (f32)e.y));
        } else {
            de__q2proxy_shape(&t, bs, br, off);
        }
        de__q2dist g;
        de__q2_gjk(&q, &t, &g);
        const f64 sep = (g.overlap ? 0.0 : (f64)g.distance) - (f64)q.radius - (f64)t.radius;
        if (sep < best)
            best = sep;
    }
    return best;
}

TEST(physics2d_query, casts_equal_brute_force_and_never_pass_contact) {
    de_physics2d_world *p = make_world(512);
    de_rng rng = de_rng_seed(77, 1);
    random_scene(p, &rng, 200, 0.0, 0.0);
    de_shape qs[4];
    qs[0] = tu_circle(p, 0.3f, de__v2(0, 0));
    qs[1] = tu_capsule(p, de__v2(0, -0.4f), de__v2(0, 0.4f), 0.2f);
    qs[2] = tu_box(p, 0.4f, 0.25f, 0.0f);
    qs[3] = tu_box(p, 0.3f, 0.3f, 0.05f);
    u32 mism[2] = {0, 0}, hits = 0;
    f64 worst_sep = 1.0, worst_gap = -1.0;
    for (u32 pass = 0; pass < 2; ++pass) {
        if (pass == 1)
            make_bp(p);
        de_rng r2 = de_rng_seed(5, 5);
        for (u32 i = 0; i < 600; ++i) {
            const de_shape s = qs[i % 4u];
            const de_transform2d from =
                tu_xf((de_rng_f64(&r2) - 0.5) * 40.0, (de_rng_f64(&r2) - 0.5) * 40.0,
                      (f32)(de_rng_f64(&r2) * 6.0));
            const de_vec2 d = de__v2((f32)(de_rng_f64(&r2) - 0.5), (f32)(de_rng_f64(&r2) - 0.5));
            const f32 len = (f32)(0.5 + de_rng_f64(&r2) * 15.0);
            de_ray_hit2d a, b;
            const bool ha = de_physics2d_shape_cast(p, s, &from, d, len, NULL, &a);
            const bool hb = brute_cast(p, s, &from, d, len, &b);
            if (ha != hb || (ha && !same_hit(&a, &b)))
                ++mism[pass];
            if (!ha || pass == 1)
                continue;
            ++hits;
            if (a.fraction == 0.0f)
                continue;
            /* At the reported time the shapes are within the cast tolerance of touching and
             * not past it (f32 noise aside). */
            const f64 dl = sqrt((f64)d.x * d.x + (f64)d.y * d.y);
            const f64 tl = (f64)a.fraction * len;
            const de__d2 at = de__d2_make(from.x + (f64)d.x / dl * tl, from.y + (f64)d.y / dl * tl);
            const f64 sep = separation(p, s, at, from.rotation, tu_index(a.body), a.sub_shape);
            if (sep < worst_sep)
                worst_sep = sep;
            if (sep > worst_gap)
                worst_gap = sep;
        }
    }
    EXPECT_EQ(mism[0], 0u);
    EXPECT_EQ(mism[1], 0u);
    EXPECT_GT(hits, 100u);
    printf("  cast separations at the hit: [%.3g, %.3g] m\n", worst_sep, worst_gap);
    EXPECT_TRUE(worst_sep > -2e-6);
    EXPECT_TRUE(worst_gap < 2e-5);
    de_physics2d_destroy(p);
}

/* ---- Casts: analytic ----------------------------------------------------------------------------
 */

TEST(physics2d_query, shape_cast_analytic) {
    de_physics2d_world *p = make_world(64);
    const de_body target = sbody(p, tu_circle(p, 1.0f, de__v2(0, 0)), 5.0, 0.3, 0.0f);
    const de_shape c = tu_circle(p, 0.5f, de__v2(0, 0));
    de_ray_hit2d h;
    de_transform2d from = tu_xf(0.0, 0.0, 0.0f);
    ASSERT_TRUE(de_physics2d_shape_cast(p, c, &from, de__v2(1, 0), 10.0f, NULL, &h));
    /* Centers 1.5 apart at contact: x = 5 - sqrt(1.5^2 - 0.3^2). */
    const double tx = 5.0 - sqrt(2.25 - 0.09);
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, tx, EPS);
    EXPECT_EQ(h.body.id, target.id);
    /* Normal from the target toward the caster at contact. */
    const double nx = (tx - 5.0) / 1.5, ny = (0.0 - 0.3) / 1.5;
    EXPECT_FLOAT_EQ(h.normal.x, nx, 1e-4);
    EXPECT_FLOAT_EQ(h.normal.y, ny, 1e-4);
    EXPECT_FLOAT_EQ(h.point.x, 5.0 + nx, 1e-4);
    EXPECT_FLOAT_EQ(h.point.y, 0.3 + ny, 1e-4);
    /* A box dropped flat onto a box: exactly the gap. */
    (void)sbody(p, tu_box(p, 2.0f, 0.5f, 0.0f), 20.0, 0.0, 0.0f);
    const de_shape b = tu_box(p, 0.5f, 0.25f, 0.0f);
    from = tu_xf(20.3, 3.0, 0.0f);
    ASSERT_TRUE(de_physics2d_shape_cast(p, b, &from, de__v2(0, -1), 5.0f, NULL, &h));
    EXPECT_FLOAT_EQ(h.fraction * 5.0f, 3.0 - 0.25 - 0.5, EPS);
    EXPECT_FLOAT_EQ(h.normal.y, 1.0, 1e-6);
    /* Moving away or sideways: no hit. */
    from = tu_xf(20.0, 0.8, 0.0f);
    EXPECT_FALSE(de_physics2d_shape_cast(p, b, &from, de__v2(0, 1), 5.0f, NULL, &h));
    /* Starting overlapping: fraction 0, normal -dir, point = start. */
    from = tu_xf(20.0, 0.6, 0.0f);
    ASSERT_TRUE(de_physics2d_shape_cast(p, b, &from, de__v2(1, 0), 5.0f, NULL, &h));
    TU_EXPECT_FEQ(h.fraction, 0.0f);
    TU_EXPECT_FEQ(h.normal.x, -1.0f);
    TU_EXPECT_FEQ(h.point.x, 20.0);
    TU_EXPECT_FEQ(h.point.y, 0.6);
    /* A capsule onto a tilted segment: the cast's rounding meets the line. */
    (void)sbody(p, tu_segment(p, de__v2(-3, 0), de__v2(3, 0)), 40.0, 0.0, 0.3f);
    const de_shape cap = tu_capsule(p, de__v2(0, -0.5f), de__v2(0, 0.5f), 0.25f);
    from = tu_xf(40.0, 4.0, 0.0f);
    ASSERT_TRUE(de_physics2d_shape_cast(p, cap, &from, de__v2(0, -1), 6.0f, NULL, &h));
    /* Lowest core point (40, y - 0.5) at distance 0.25 from the line through (40, 0) at 0.3 rad:
     * cos(0.3) (y - 0.5) = 0.25. */
    EXPECT_FLOAT_EQ(4.0 - h.fraction * 6.0f, 0.5 + 0.25 / cos(0.3), EPS);
    EXPECT_FLOAT_EQ(h.normal.x, -sin(0.3), 1e-4);
    EXPECT_FLOAT_EQ(h.normal.y, cos(0.3), 1e-4);
    /* A chain cannot be cast. */
    const de_vec2 pts[2] = {{0, 0}, {1, 0}};
    const de_shape chain = de_shape2d_chain_create(p, pts, 2, false);
    EXPECT_FALSE(de_physics2d_shape_cast(p, chain, &from, de__v2(0, -1), 6.0f, NULL, &h));
    de_physics2d_destroy(p);
}

/* ---- Overlaps
 * ------------------------------------------------------------------------------------ */

TEST(physics2d_query, overlap_shape_and_box) {
    de_physics2d_world *p = make_world(512);
    de_rng rng = de_rng_seed(31, 2);
    random_scene(p, &rng, 250, 0.0, 0.0);
    const de_shape probe = tu_circle(p, 0.8f, de__v2(0, 0));
    u32 found_total = 0;
    for (u32 pass = 0; pass < 2; ++pass) {
        if (pass == 1)
            make_bp(p);
        de_rng r2 = de_rng_seed(8, 8);
        for (u32 i = 0; i < 300; ++i) {
            const f64 x = (de_rng_f64(&r2) - 0.5) * 40.0, y = (de_rng_f64(&r2) - 0.5) * 40.0;
            const de_transform2d at = tu_xf(x, y, 0.0f);
            de_body got[64];
            de_entity ents[64];
            const u32 n = de_physics2d_overlap(p, probe, &at, NULL, got, ents, 64);
            /* Oracle: a circle overlaps a body iff the body's closest point to its center is
             * within its radius. */
            u32 k = 0;
            for (u32 b = 0; b < p->bodies.high_water; ++b) {
                if (!de__body2d_alive(p, b))
                    continue;
                de__q2hit h;
                if (!de__q2_point_body(p, b, de__d2_make(x, y), 0.8 + 1e-3, &h))
                    continue;
                const bool want = h.t <= 0.8 - 1e-5;
                const bool maybe = h.t <= 0.8 + 1e-5;
                const bool have = k < n && tu_index(got[k]) == b;
                if (have)
                    ++k;
                if (want)
                    EXPECT_TRUE(have);
                if (!maybe)
                    EXPECT_FALSE(have);
            }
            EXPECT_EQ(k, n); /* ascending index, nothing else */
            for (u32 j = 0; j < n; ++j)
                EXPECT_EQ(ents[j].bits, 0u);
            found_total += n;
        }
    }
    EXPECT_GT(found_total, 50u);

    /* A box query: exact against the shapes, not their boxes. */
    de_physics2d_destroy(p);
    p = make_world(64);
    const de_body circ = sbody(p, tu_circle(p, 1.0f, de__v2(0, 0)), 0.0, 0.0, 0.0f);
    de_aabb2d box;
    box.min = de__v2(0.8f, 0.8f); /* inside the circle's box, outside the circle */
    box.max = de__v2(2.0f, 2.0f);
    de_body got[4];
    EXPECT_EQ(de_physics2d_overlap_aabb(p, box, NULL, got, NULL, 4), 0u);
    box.min = de__v2(0.6f, 0.6f);
    ASSERT_EQ(de_physics2d_overlap_aabb(p, box, NULL, got, NULL, 4), 1u);
    EXPECT_EQ(got[0].id, circ.id);
    box.min = de__v2(3.0f, 3.0f); /* empty */
    EXPECT_EQ(de_physics2d_overlap_aabb(p, box, NULL, got, NULL, 4), 0u);
    de_physics2d_destroy(p);
}

TEST(physics2d_query, overlap_collects_more_than_a_chunk) {
    de_physics2d_world *p = make_world(512);
    const de_shape c = tu_circle(p, 0.2f, de__v2(0, 0));
    /* 200 bodies in a 10 x 20 grid, every one inside the query box; destroy some to scatter the
     * indices, and fill in again (LIFO reuse). */
    de_body b[200];
    for (u32 i = 0; i < 200; ++i)
        b[i] = sbody(p, c, (f64)(i % 10u), (f64)(i / 10u), 0.0f);
    for (u32 i = 0; i < 200; i += 7u)
        de_body2d_destroy(p, b[i]);
    for (u32 i = 0; i < 200; i += 7u)
        b[i] = sbody(p, c, 0.5 + (f64)(i % 10u), 0.5 + (f64)(i / 10u), 0.0f);
    for (u32 pass = 0; pass < 2; ++pass) {
        if (pass == 1)
            make_bp(p);
        de_aabb2d box;
        box.min = de__v2(-1.0f, -1.0f);
        box.max = de__v2(11.0f, 21.0f);
        de_body got[256];
        ASSERT_EQ(de_physics2d_overlap_aabb(p, box, NULL, got, NULL, 256), 200u);
        for (u32 i = 1; i < 200; ++i)
            EXPECT_LT(tu_index(got[i - 1]), tu_index(got[i]));
        /* Truncated: the lowest 130 indices. */
        de_body few[130];
        ASSERT_EQ(de_physics2d_overlap_aabb(p, box, NULL, few, NULL, 130), 130u);
        for (u32 i = 0; i < 130; ++i)
            EXPECT_EQ(few[i].id, got[i].id);
    }
    de_physics2d_destroy(p);
}

/* ---- Closest points
 * ------------------------------------------------------------------------------ */

TEST(physics2d_query, closest_point_analytic) {
    de_physics2d_world *p = make_world(64);
    const de_body c = sbody(p, tu_circle(p, 1.0f, de__v2(0, 0)), 0.0, 0.0, 0.0f);
    de_ray_hit2d h;
    ASSERT_TRUE(de_physics2d_closest_point(p, dp(3.0, 4.0), 10.0f, NULL, &h));
    EXPECT_EQ(h.body.id, c.id);
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 4.0, EPS);
    EXPECT_FLOAT_EQ(h.point.x, 0.6, EPS);
    EXPECT_FLOAT_EQ(h.point.y, 0.8, EPS);
    EXPECT_FLOAT_EQ(h.normal.x, 0.6, EPS);
    EXPECT_FLOAT_EQ(h.normal.y, 0.8, EPS);
    EXPECT_FALSE(de_physics2d_closest_point(p, dp(3.0, 4.0), 3.9f, NULL, &h));
    /* Inside: distance 0, the point itself, zero normal. */
    ASSERT_TRUE(de_physics2d_closest_point(p, dp(0.2, 0.1), 1.0f, NULL, &h));
    TU_EXPECT_FEQ(h.fraction, 0.0f);
    TU_EXPECT_FEQ(h.point.x, 0.2);
    TU_EXPECT_FEQ(h.normal.x, 0.0f);
    TU_EXPECT_FEQ(h.normal.y, 0.0f);
    /* max_dist 0 inside works too. */
    ASSERT_TRUE(de_physics2d_closest_point(p, dp(0.2, 0.1), 0.0f, NULL, &h));

    /* A rotated rounded box: a face and a corner. */
    (void)sbody(p, tu_box(p, 1.0f, 0.5f, 0.1f), 10.0, 0.0, 0.0f);
    ASSERT_TRUE(de_physics2d_closest_point(p, dp(10.3, 2.0), 5.0f, NULL, &h));
    EXPECT_FLOAT_EQ(h.fraction * 5.0f, 2.0 - 0.6, EPS);
    EXPECT_FLOAT_EQ(h.point.y, 0.6, EPS);
    ASSERT_TRUE(de_physics2d_closest_point(p, dp(14.0, 4.5), 10.0f, NULL, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0f, 5.0 - 0.1, EPS); /* corner (11, 0.5): 3-4-5 */
    EXPECT_FLOAT_EQ(h.normal.x, 0.6, EPS);
    /* A chain: the nearest segment, two-sided. */
    const de_vec2 pts[3] = {{0, 0}, {2, 0}, {2, 2}};
    const de_body ch = sbody(p, de_shape2d_chain_create(p, pts, 3, false), 30.0, 0.0, 0.0f);
    ASSERT_TRUE(de_physics2d_closest_point(p, dp(31.0, -0.5), 2.0f, NULL, &h));
    EXPECT_EQ(h.body.id, ch.id);
    EXPECT_EQ(h.sub_shape, 0u);
    EXPECT_FLOAT_EQ(h.fraction * 2.0f, 0.5, EPS);
    ASSERT_TRUE(de_physics2d_closest_point(p, dp(32.4, 1.5), 2.0f, NULL, &h));
    EXPECT_EQ(h.sub_shape, 1u);
    EXPECT_FLOAT_EQ(h.fraction * 2.0f, 0.4, EPS);
    de_physics2d_destroy(p);
}

/* ---- Filters
 * ---------------------------------------------------------------------------------------- */

TEST(physics2d_query, filters_triggers_and_ignore) {
    de_physics2d_world *p = make_world(64);
    const de_shape c = tu_circle(p, 0.5f, de__v2(0, 0));
    de_body2d_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_STATIC;
    d.shape = c;
    d.transform = tu_xf(2.0, 0.0, 0.0f);
    d.filter.layer = 1u << 3;
    d.filter.mask = 0xFFFFFFFFu;
    const de_body layered = de_body2d_create(p, &d);
    d.transform = tu_xf(4.0, 0.0, 0.0f);
    d.filter.layer = DE_COLLISION_FILTER_DEFAULT_LAYER;
    d.flags = DE_BODY_FLAG_TRIGGER;
    const de_body trig = de_body2d_create(p, &d);
    d.flags = 0;
    d.transform = tu_xf(6.0, 0.0, 0.0f);
    d.filter.group = 7;
    const de_body grouped = de_body2d_create(p, &d);

    de_ray_hit2d h;
    de_physics_query_filter f;
    memset(&f, 0, sizeof f);
    /* Default filter: layer 1 query sees the layered body (its mask has every bit) ... */
    ASSERT_TRUE(de_physics2d_raycast_ex(p, dp(0, 0), de__v2(1, 0), 10.0f, &f, &h));
    EXPECT_EQ(h.body.id, layered.id);
    /* ... unless the query's mask excludes layer 3. */
    f.filter.layer = 1;
    f.filter.mask = ~(1u << 3);
    ASSERT_TRUE(de_physics2d_raycast_ex(p, dp(0, 0), de__v2(1, 0), 10.0f, &f, &h));
    EXPECT_EQ(h.body.id, grouped.id); /* the trigger is skipped */
    f.flags = DE_PHYSICS_QUERY_INCLUDE_TRIGGERS;
    ASSERT_TRUE(de_physics2d_raycast_ex(p, dp(0, 0), de__v2(1, 0), 10.0f, &f, &h));
    EXPECT_EQ(h.body.id, trig.id);
    f.ignore_body = trig;
    ASSERT_TRUE(de_physics2d_raycast_ex(p, dp(0, 0), de__v2(1, 0), 10.0f, &f, &h));
    EXPECT_EQ(h.body.id, grouped.id);
    /* A shared group excludes. */
    f.filter.group = 7;
    EXPECT_FALSE(de_physics2d_raycast_ex(p, dp(0, 0), de__v2(1, 0), 10.0f, &f, &h));
    /* The short form takes a plain filter and optional outputs. */
    de_collision_filter cf;
    memset(&cf, 0, sizeof cf);
    de_vec2 pt, nrm;
    de_entity e;
    ASSERT_TRUE(de_physics2d_raycast(p, de__v2(0, 0), de__v2(1, 0), 10.0f, cf, &e, &pt, &nrm));
    EXPECT_FLOAT_EQ(pt.x, 1.5, EPS);
    TU_EXPECT_FEQ(nrm.x, -1.0f);
    ASSERT_TRUE(de_physics2d_raycast(p, de__v2(0, 0), de__v2(1, 0), 10.0f, cf, NULL, NULL, NULL));
    de_physics2d_destroy(p);
}

TEST(physics2d_query, invalid_arguments) {
    de_physics2d_world *p = make_world(64);
    const de_shape c = tu_circle(p, 0.5f, de__v2(0, 0));
    (void)sbody(p, c, 2.0, 0.0, 0.0f);
    de_ray_hit2d h;
    de_ray_hit2d all[4];
    EXPECT_FALSE(de_physics2d_raycast_ex(NULL, dp(0, 0), de__v2(1, 0), 5.0f, NULL, &h));
    EXPECT_FALSE(de_physics2d_raycast_ex(p, dp(0, 0), de__v2(1, 0), 5.0f, NULL, NULL));
    EXPECT_FALSE(de_physics2d_raycast_ex(p, dp(0, 0), de__v2(0, 0), 5.0f, NULL, &h));
    EXPECT_FALSE(de_physics2d_raycast_ex(p, dp(0, 0), de__v2(1, 0), 0.0f, NULL, &h));
    EXPECT_FALSE(de_physics2d_raycast_ex(p, dp(0, 0), de__v2(1, 0), -1.0f, NULL, &h));
    EXPECT_FALSE(de_physics2d_raycast_ex(p, dp(NAN, 0), de__v2(1, 0), 5.0f, NULL, &h));
    EXPECT_FALSE(de_physics2d_raycast_ex(p, dp(0, 0), de__v2(INFINITY, 0), 5.0f, NULL, &h));
    EXPECT_EQ(de_physics2d_raycast_all(p, dp(0, 0), de__v2(0, 0), 5.0f, NULL, all, 4), 0u);
    EXPECT_EQ(de_physics2d_raycast_all(p, dp(0, 0), de__v2(1, 0), 5.0f, NULL, all, 0), 0u);
    de_transform2d from = tu_xf(0, 0, 0);
    EXPECT_FALSE(de_physics2d_shape_cast(p, c, NULL, de__v2(1, 0), 5.0f, NULL, &h));
    EXPECT_FALSE(de_physics2d_shape_cast(p, c, &from, de__v2(1, 0), 0.0f, NULL, &h));
    de_shape stale = c;
    stale.id += 1u << 20;
    EXPECT_FALSE(de_physics2d_shape_cast(p, stale, &from, de__v2(1, 0), 5.0f, NULL, &h));
    from.x = NAN;
    EXPECT_FALSE(de_physics2d_shape_cast(p, c, &from, de__v2(1, 0), 5.0f, NULL, &h));
    from.x = 2.0;
    de_body got[4];
    EXPECT_EQ(de_physics2d_overlap(p, c, &from, NULL, NULL, NULL, 4), 0u);
    EXPECT_EQ(de_physics2d_overlap(p, c, &from, NULL, got, NULL, 0), 0u);
    EXPECT_EQ(de_physics2d_overlap(p, c, NULL, NULL, got, NULL, 4), 0u);
    EXPECT_EQ(de_physics2d_overlap(p, c, &from, NULL, got, NULL, 4), 1u);
    EXPECT_FALSE(de_physics2d_closest_point(p, dp(0, 0), -1.0f, NULL, &h));
    EXPECT_FALSE(de_physics2d_closest_point(p, dp(0, 0), 5.0f, NULL, NULL));
    de_physics2d_destroy(p);
}

/* ---- Large worlds
 * -------------------------------------------------------------------------------- */

TEST(physics2d_query, far_from_origin) {
    /* The same scene at the origin and 100 km out: the same hits, relative to the offset. */
    const f64 off = 100000.0;
    de_physics2d_world *a = make_world(512);
    de_physics2d_world *b = make_world(512);
    de_rng ra = de_rng_seed(4, 4), rb = de_rng_seed(4, 4);
    random_scene(a, &ra, 120, 0.0, 0.0);
    random_scene(b, &rb, 120, off, -off);
    const de_shape qa = tu_capsule(a, de__v2(0, -0.3f), de__v2(0, 0.3f), 0.2f);
    const de_shape qb = tu_capsule(b, de__v2(0, -0.3f), de__v2(0, 0.3f), 0.2f);
    de_rng r2 = de_rng_seed(6, 6);
    f64 worst = 0.0;
    u32 diff_body = 0, hits = 0;
    for (u32 i = 0; i < 1500; ++i) {
        const f64 x = (de_rng_f64(&r2) - 0.5) * 40.0, y = (de_rng_f64(&r2) - 0.5) * 40.0;
        const de_vec2 d = de__v2((f32)(de_rng_f64(&r2) - 0.5), (f32)(de_rng_f64(&r2) - 0.5));
        de_ray_hit2d ha, hb;
        bool ka, kb;
        if (i % 2u == 0u) {
            ka = de_physics2d_raycast_ex(a, dp(x, y), d, 20.0f, NULL, &ha);
            kb = de_physics2d_raycast_ex(b, dp(x + off, y - off), d, 20.0f, NULL, &hb);
        } else {
            const de_transform2d fa = tu_xf(x, y, 0.3f), fb = tu_xf(x + off, y - off, 0.3f);
            ka = de_physics2d_shape_cast(a, qa, &fa, d, 10.0f, NULL, &ha);
            kb = de_physics2d_shape_cast(b, qb, &fb, d, 10.0f, NULL, &hb);
        }
        if (ka != kb) {
            ++diff_body;
            continue;
        }
        if (!ka)
            continue;
        ++hits;
        if (tu_index(ha.body) != tu_index(hb.body)) {
            ++diff_body;
            continue;
        }
        const f64 e = fabs((f64)ha.fraction - (f64)hb.fraction) * 20.0 +
                      fabs(ha.point.x - (hb.point.x - off)) + fabs(ha.point.y - (hb.point.y + off));
        if (e > worst)
            worst = e;
    }
    EXPECT_GT(hits, 300u);
    EXPECT_EQ(diff_body, 0u);
    EXPECT_TRUE(worst < 2e-4); /* f32 shape data near 100 km: well under a millimeter */
    de_physics2d_destroy(a);
    de_physics2d_destroy(b);
}

/* ---- Allocation
 * ------------------------------------------------------------------------------------ */

TEST(physics2d_query, no_allocation) {
    de_tracking_allocator t;
    de_allocator al = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &al;
    d.max_bodies = 512;
    de_physics2d_world *p = NULL;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    de_rng rng = de_rng_seed(1, 1);
    random_scene(p, &rng, 200, 0.0, 0.0);
    const de_shape q = tu_box(p, 0.3f, 0.3f, 0.05f);
    for (u32 pass = 0; pass < 2; ++pass) {
        if (pass == 1)
            make_bp(p);
        const de_tracking_stats before = de_tracking_allocator_stats(&t);
        de_rng r2 = de_rng_seed(2, 2);
        for (u32 i = 0; i < 300; ++i) {
            const f64 x = (de_rng_f64(&r2) - 0.5) * 40.0, y = (de_rng_f64(&r2) - 0.5) * 40.0;
            const de_vec2 dir = de__v2((f32)(de_rng_f64(&r2) - 0.5), 0.3f);
            de_ray_hit2d h, all[32];
            de_body got[200];
            (void)de_physics2d_raycast_ex(p, dp(x, y), dir, 20.0f, NULL, &h);
            (void)de_physics2d_raycast_all(p, dp(x, y), dir, 20.0f, NULL, all, 32);
            const de_transform2d at = tu_xf(x, y, 0.2f);
            (void)de_physics2d_shape_cast(p, q, &at, dir, 10.0f, NULL, &h);
            (void)de_physics2d_overlap(p, q, &at, NULL, got, NULL, 200);
            de_aabb2d box;
            box.min = de__v2((f32)x - 20.0f, (f32)y - 20.0f);
            box.max = de__v2((f32)x + 20.0f, (f32)y + 20.0f);
            (void)de_physics2d_overlap_aabb(p, box, NULL, got, NULL, 200);
            (void)de_physics2d_closest_point(p, dp(x, y), 5.0f, NULL, &h);
        }
        const de_tracking_stats after = de_tracking_allocator_stats(&t);
        EXPECT_EQ(after.alloc_count, before.alloc_count);
        EXPECT_EQ(after.realloc_count, before.realloc_count);
    }
    de_physics2d_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}

/* ---- Golden vector
 * ------------------------------------------------------------------------------- */

static void h_f64(tu_hash *s, f64 v) {
    u64 u;
    memcpy(&u, &v, sizeof u);
    tu_h_u32(s, (u32)u);
    tu_h_u32(s, (u32)(u >> 32));
}

static void h_hit(tu_hash *s, bool hit, const de_ray_hit2d *h) {
    tu_h_u32(s, hit ? 1u : 0u);
    if (!hit)
        return;
    tu_h_u32(s, tu_index(h->body));
    tu_h_f32(s, h->fraction);
    h_f64(s, h->point.x);
    h_f64(s, h->point.y);
    tu_h_v2(s, h->normal);
    tu_h_u32(s, h->sub_shape);
}

static void add_line(tu_golden *lines, u32 *n, const char *name, u64 v) {
    ASSERT_LT(*n, (u32)TU_GOLDEN_MAX);
    snprintf(lines[*n].name, sizeof lines[*n].name, "%s", name);
    lines[*n].value = v;
    ++*n;
}

static const char k_header[] =
    "# query2d_golden.txt: 10,000 de_rng rays and 1,000 de_rng shape casts (circle, capsule, box,\n"
    "# rounded box, hexagon) against a 400-body scene of every 2D shape kind (static and\n"
    "# kinematic, chains, segments, rounded polygons): XXH3-64 of every result (hit flag, body\n"
    "# index, fraction, f64 point, normal, sub-shape) per block of 1,000 rays / 100 casts, and "
    "the\n"
    "# hit counts (DENG-258).\n"
    "# Generated by test_physics2d_query with DE_REGEN_GOLDEN=1; never edit by hand.\n"
    "# x86-64 and arm64 must reproduce every line.\n";

TEST(physics2d_query, golden_vector) {
    de_physics2d_world *p = make_world(1024);
    de_rng rng = de_rng_seed(0x2d2d, 258);
    random_scene(p, &rng, 400, 0.0, 0.0);
    make_bp(p);
    de_shape qs[5];
    qs[0] = tu_circle(p, 0.3f, de__v2(0.05f, 0));
    qs[1] = tu_capsule(p, de__v2(0, -0.4f), de__v2(0, 0.4f), 0.2f);
    qs[2] = tu_box(p, 0.4f, 0.25f, 0.0f);
    qs[3] = tu_box(p, 0.3f, 0.3f, 0.05f);
    const de_vec2 hex[6] = {{0.4f, 0},  {0.2f, 0.35f},   {-0.2f, 0.35f},
                            {-0.4f, 0}, {-0.2f, -0.35f}, {0.2f, -0.35f}};
    qs[4] = tu_polygon(p, hex, 6, 0.0f);

    static tu_golden lines[TU_GOLDEN_MAX];
    u32 n = 0;
    u32 ray_hits = 0, cast_hits = 0, inside = 0;
    tu_hash block = {0};
    de_rng r2 = de_rng_seed(0x5eed, 1);
    for (u32 i = 0; i < 10000; ++i) {
        const de_dpoint2d o = dp((de_rng_f64(&r2) - 0.5) * 44.0, (de_rng_f64(&r2) - 0.5) * 44.0);
        const de_vec2 d = de__v2((f32)(de_rng_f64(&r2) - 0.5), (f32)(de_rng_f64(&r2) - 0.5));
        const f32 len = (f32)(0.5 + de_rng_f64(&r2) * 25.0);
        de_ray_hit2d h;
        memset(&h, 0, sizeof h);
        const bool hit = de_physics2d_raycast_ex(p, o, d, len, NULL, &h);
        ray_hits += hit ? 1u : 0u;
        inside += hit && h.fraction == 0.0f ? 1u : 0u;
        h_hit(&block, hit, &h);
        if ((i + 1u) % 1000u == 0u) {
            char name[32];
            snprintf(name, sizeof name, "rays%05u", i - 999u);
            add_line(lines, &n, name, block.h);
            block.h = 0;
        }
    }
    for (u32 i = 0; i < 1000; ++i) {
        const de_transform2d from =
            tu_xf((de_rng_f64(&r2) - 0.5) * 44.0, (de_rng_f64(&r2) - 0.5) * 44.0,
                  (f32)(de_rng_f64(&r2) * 6.283));
        const de_vec2 d = de__v2((f32)(de_rng_f64(&r2) - 0.5), (f32)(de_rng_f64(&r2) - 0.5));
        const f32 len = (f32)(0.5 + de_rng_f64(&r2) * 15.0);
        de_ray_hit2d h;
        memset(&h, 0, sizeof h);
        const bool hit = de_physics2d_shape_cast(p, qs[i % 5u], &from, d, len, NULL, &h);
        cast_hits += hit ? 1u : 0u;
        h_hit(&block, hit, &h);
        if ((i + 1u) % 100u == 0u) {
            char name[32];
            snprintf(name, sizeof name, "casts%04u", i - 99u);
            add_line(lines, &n, name, block.h);
            block.h = 0;
        }
    }
    add_line(lines, &n, "ray_hits", ray_hits);
    add_line(lines, &n, "ray_inside", inside);
    add_line(lines, &n, "cast_hits", cast_hits);
    EXPECT_GT(ray_hits, 3000u);
    EXPECT_GT(cast_hits, 300u);
    de_physics2d_destroy(p);
    if (tu_golden_check(DE_PHYSICS2D_QUERY_GOLDEN, k_header, lines, n))
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_PHYSICS2D_QUERY_GOLDEN);
}
