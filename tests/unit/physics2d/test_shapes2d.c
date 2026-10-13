/* test_shapes2d.c: 2D shapes, the polygon hull builder, chains, mass properties, AABBs and the
 * 2D shape golden vector (DENG-252, SPEC §13.3).
 *
 * physics2d_shapes.golden_vector compares hashes of 50 polygon hulls from fixed de_rng point sets
 * (and their mass properties), and of the other shape kinds, against
 * tests/unit/physics2d/vectors/shapes2d_golden.txt. That file is a cross-platform contract: x86-64
 * and arm64 must reproduce it bit for bit. Only a deliberate change of the algorithms rewrites it
 * (never by hand, and never to make one platform pass):
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics2d/test_physics2d_shapes
 * --filter=physics2d_shapes.golden_vector */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen */
#endif
#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/2d/p2d_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_PHYSICS2D_SHAPES_GOLDEN
#error "DE_PHYSICS2D_SHAPES_GOLDEN (path of shapes2d_golden.txt) must be defined by the build"
#endif

#define PI_D 3.14159265358979323846

/* ---- helpers ---------------------------------------------------------------------------- */

typedef struct sh_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_physics2d_world *p;
} sh_fx;

static void sh_fx_setup(sh_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_PHYSICS);
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &f->alloc;
    d.max_bodies = 16;
    ASSERT_EQ(de_physics2d_create(&d, &f->p), DE_OK);
}

static void sh_fx_teardown(sh_fx *f) {
    de_physics2d_destroy(f->p);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
}

static de_shape2d_desc desc_of(de_shape2d_kind kind) {
    de_shape2d_desc d;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    return d;
}

static de_shape poly_shape(de_physics2d_world *p, const de_vec2 *pts, u32 n, f32 radius) {
    de_shape2d_desc d = desc_of(DE_SHAPE2D_POLYGON);
    d.points = pts;
    d.point_count = n;
    d.radius = radius;
    return de_shape2d_create(p, &d);
}

static void expect_rel(f64 got, f64 want, f64 rel) {
    const f64 scale = fabs(want) > 1e-30 ? fabs(want) : 1.0;
    EXPECT_FLOAT_EQ(got / scale, want / scale, rel);
}

/* Structure checks of a built polygon: CCW, convex, unit outward normals, centroid inside, every
 * input point inside (within tol). */
static void check_polygon(const de__polygon2d *poly, const de_vec2 *pts, u32 n, f64 tol) {
    ASSERT_GE(poly->count, 3u);
    ASSERT_LE(poly->count, DE_SHAPE2D_MAX_POLYGON_VERTICES);
    const u32 c = poly->count;
    for (u32 i = 0; i < c; ++i) {
        const de_vec2 a = poly->vertices[i], b = poly->vertices[(i + 1) % c],
                      d = poly->vertices[(i + 2) % c];
        const f64 cr = ((f64)b.x - a.x) * ((f64)d.y - b.y) - ((f64)b.y - a.y) * ((f64)d.x - b.x);
        EXPECT_TRUE(cr > 0.0); /* strictly left turns: CCW and convex */
        const de_vec2 nr = poly->normals[i];
        EXPECT_FLOAT_EQ((f64)nr.x * nr.x + (f64)nr.y * nr.y, 1.0, 1e-6);
        /* The normal is the right perpendicular of the edge. */
        EXPECT_FLOAT_EQ(((f64)b.x - a.x) * nr.x + ((f64)b.y - a.y) * nr.y, 0.0, 1e-5);
        for (u32 j = 0; j < n; ++j) {
            const f64 dist = ((f64)pts[j].x - a.x) * nr.x + ((f64)pts[j].y - a.y) * nr.y;
            EXPECT_TRUE(dist <= tol);
            if (dist > tol)
                printf("  point %u outside edge %u by %g\n", j, i, dist);
        }
        const f64 cd = ((f64)poly->centroid.x - a.x) * nr.x + ((f64)poly->centroid.y - a.y) * nr.y;
        EXPECT_TRUE(cd < 0.0);
    }
}

static de__polygon2d build_ok(const de_vec2 *pts, u32 n) {
    de__polygon2d poly;
    ASSERT_EQ(de__poly2d_build(pts, n, de__poly2d_auto_tolerance(pts, n), 0.0f, &poly), DE_OK);
    return poly;
}

static de_result build_rc(const de_vec2 *pts, u32 n) {
    de__polygon2d poly;
    return de__poly2d_build(pts, n, de__poly2d_auto_tolerance(pts, n), 0.0f, &poly);
}

/* ---- polygon hull ----------------------------------------------------------------------- */

TEST(physics2d_shapes, hull_square_interior_duplicates_edges) {
    /* Corners, interior points, duplicates and edge midpoints, in a scrambled order. */
    const de_vec2 pts[] = {{0.2f, 0.1f}, {1, 1},  {-1, 1}, {0, -1},       {1, -1},
                           {-1, -1},     {1, 1},  {0, 0},  {1, 0},        {-1, 0.5f},
                           {-0.5f, 1},   {1, -1}, {0, 1},  {-0.3f, 0.7f}, {-1, -1}};
    const u32 n = (u32)(sizeof pts / sizeof pts[0]);
    const de__polygon2d poly = build_ok(pts, n);
    ASSERT_EQ(poly.count, 4u);
    /* Counter-clockwise from the lowest (x, y). */
    const de_vec2 want[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (u32 i = 0; i < 4; ++i)
        EXPECT_TRUE(poly.vertices[i].x == want[i].x && poly.vertices[i].y == want[i].y);
    EXPECT_TRUE(poly.normals[0].x == 0.0f && poly.normals[0].y == -1.0f);
    EXPECT_TRUE(poly.normals[1].x == 1.0f && poly.normals[1].y == 0.0f);
    EXPECT_TRUE(poly.centroid.x == 0.0f && poly.centroid.y == 0.0f);
    check_polygon(&poly, pts, n, 1e-6);
}

TEST(physics2d_shapes, hull_more_than_8_inputs) {
    /* A regular octagon plus 60 interior points, interleaved: the hull is the octagon. */
    de_vec2 pts[68];
    u32 n = 0;
    de_rng r = de_rng_seed(5, 1);
    for (u32 i = 0; i < 8; ++i) {
        f32 s, c;
        de_dsincos(0.785398163f * (f32)i + 0.1f, &s, &c);
        pts[n++] = (de_vec2){3.0f * c + 10.0f, 3.0f * s - 4.0f};
        for (u32 k = 0; k < 7 + (i < 4 ? 1u : 0u); ++k) {
            const f32 rad = 2.5f * de_rng_f32(&r);
            de_dsincos(6.2831853f * de_rng_f32(&r), &s, &c);
            pts[n++] = (de_vec2){rad * c + 10.0f, rad * s - 4.0f};
        }
    }
    ASSERT_EQ(n, 68u);
    const de__polygon2d poly = build_ok(pts, n);
    EXPECT_EQ(poly.count, 8u);
    check_polygon(&poly, pts, n, 1e-5);

    /* A 12-gon has a hull of 12 vertices: refused. So is a dense random disc. */
    de_vec2 g[12];
    for (u32 i = 0; i < 12; ++i) {
        f32 s, c;
        de_dsincos(0.523598776f * (f32)i, &s, &c);
        g[i] = (de_vec2){c, s};
    }
    EXPECT_EQ(build_rc(g, 12), DE_ERR_INVALID_ARGUMENT);
    /* The first 8 of them are fine (an open fan closes into an 8-gon). */
    const de__polygon2d fan = build_ok(g, 8);
    EXPECT_EQ(fan.count, 8u);
    check_polygon(&fan, g, 8, 1e-6);

    /* Up to DE_SHAPE2D_MAX_POLYGON_INPUT inputs; one more is refused. */
    static de_vec2 many[DE_SHAPE2D_MAX_POLYGON_INPUT + 1];
    for (u32 i = 0; i < DE_SHAPE2D_MAX_POLYGON_INPUT + 1; ++i)
        many[i] = pts[i % 68];
    EXPECT_EQ(build_ok(many, DE_SHAPE2D_MAX_POLYGON_INPUT).count, 8u);
    EXPECT_EQ(build_rc(many, DE_SHAPE2D_MAX_POLYGON_INPUT + 1), DE_ERR_INVALID_ARGUMENT);
}

TEST(physics2d_shapes, hull_degenerate_and_winding) {
    /* Collinear, duplicates, two points, nearly collinear within the tolerance. */
    const de_vec2 line[4] = {{0, 0}, {1, 1}, {2, 2}, {3, 3}};
    EXPECT_EQ(build_rc(line, 4), DE_ERR_INVALID_ARGUMENT);
    const de_vec2 same[3] = {{1, 2}, {1, 2}, {1, 2}};
    EXPECT_EQ(build_rc(same, 3), DE_ERR_INVALID_ARGUMENT);
    const de_vec2 two[4] = {{0, 0}, {1, 0}, {0, 0}, {1, 0}};
    EXPECT_EQ(build_rc(two, 4), DE_ERR_INVALID_ARGUMENT);
    const de_vec2 thin[3] = {{0, 0}, {10, 0}, {5, 1e-4f}}; /* tol = 1e-3 here */
    EXPECT_EQ(build_rc(thin, 3), DE_ERR_INVALID_ARGUMENT);
    const de_vec2 ok_thin[3] = {{0, 0}, {10, 0}, {5, 5e-3f}};
    EXPECT_EQ(build_ok(ok_thin, 3).count, 3u);

    /* A clockwise triangle comes out counter-clockwise. */
    const de_vec2 cw[3] = {{0, 0}, {0, 1}, {1, 0}};
    const de__polygon2d tri = build_ok(cw, 3);
    EXPECT_TRUE(tri.vertices[0].x == 0.0f && tri.vertices[0].y == 0.0f);
    EXPECT_TRUE(tri.vertices[1].x == 1.0f && tri.vertices[1].y == 0.0f);
    EXPECT_TRUE(tri.vertices[2].x == 0.0f && tri.vertices[2].y == 1.0f);
    check_polygon(&tri, cw, 3, 1e-7);

    /* A vertex within the tolerance of its neighbours' line is removed. */
    const de_vec2 bump[5] = {{0, 0}, {2, 0}, {2, 2}, {1, 2.00005f}, {0, 2}};
    EXPECT_EQ(build_ok(bump, 5).count, 4u);
    /* Points within the weld tolerance of an earlier one are dropped (the earlier one wins). */
    const de_vec2 weld[4] = {{0, 0}, {1, 0}, {0, 1}, {1.00001f, 0}};
    const de__polygon2d w = build_ok(weld, 4);
    EXPECT_EQ(w.count, 3u);
    EXPECT_TRUE(w.vertices[1].x == 1.0f);

    /* Same input, same bytes; a permutation of distinct points gives the same polygon. */
    const de_vec2 a[6] = {{0.3f, -1},     {2, 0.25f}, {1.5f, 2},
                          {-0.5f, 1.75f}, {-1, 0.5f}, {0.5f, 0.5f}};
    const de_vec2 b[6] = {a[4], a[2], a[5], a[0], a[3], a[1]};
    const de__polygon2d pa = build_ok(a, 6), pa2 = build_ok(a, 6), pb = build_ok(b, 6);
    EXPECT_MEMEQ(&pa, &pa2, sizeof pa);
    EXPECT_MEMEQ(&pa, &pb, sizeof pa);
    check_polygon(&pa, a, 6, 1e-6);
}

TEST_F(physics2d_shapes, polygon_create_refusals, sh_fx) {
    de_physics2d_world *p = f->p;
    const de_vec2 tri[3] = {{0, 0}, {1, 0}, {0, 1}};
    de_shape2d_desc d = desc_of(DE_SHAPE2D_POLYGON);
    d.points = tri;
    d.point_count = 2;
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    d.point_count = 3;
    d.radius = -0.1f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    d.radius = 0.0f;
    d.hull_tolerance = NAN;
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    d.hull_tolerance = 0.0f;
    const de_vec2 bad[3] = {{0, 0}, {INFINITY, 0}, {0, 1}};
    d.points = bad;
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    d.points = NULL;
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    /* An explicit tolerance welds more: (1.1, 0.05) joins (1, 0). */
    const de_vec2 near[4] = {{0, 0}, {1, 0}, {0, 1}, {1.1f, 0.05f}};
    d.points = near;
    d.point_count = 4;
    const de_shape s4 = de_shape2d_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s4));
    EXPECT_EQ(de__shape2d_get(p, s4)->u.polygon.count, 4u);
    d.hull_tolerance = 0.2f;
    const de_shape s3 = de_shape2d_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s3));
    EXPECT_EQ(de__shape2d_get(p, s3)->u.polygon.count, 3u);
    EXPECT_EQ(p->shape_live_count, 2u);
}

/* ---- other kinds: refusals -------------------------------------------------------------- */

TEST_F(physics2d_shapes, invalid_descs, sh_fx) {
    de_physics2d_world *p = f->p;
    de_shape2d_desc d = desc_of(DE_SHAPE2D_CIRCLE);
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d))); /* radius 0 */
    d.radius = 1.0f;
    d.center.x = NAN;
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    d = desc_of(DE_SHAPE2D_CAPSULE);
    d.p2 = (de_vec2){1, 0};
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d))); /* radius 0 */
    d = desc_of(DE_SHAPE2D_BOX);
    d.half_extents = (de_vec2){1, 0};
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    d.half_extents = (de_vec2){1, 1};
    d.angle = INFINITY;
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    d.angle = 0.0f;
    d.radius = -1.0f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    /* A box too small to exist at its position in f32. */
    d.radius = 0.0f;
    d.center = (de_vec2){1e7f, 0};
    d.half_extents = (de_vec2){1e-4f, 1};
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    d = desc_of(DE_SHAPE2D_SEGMENT);
    d.p1 = d.p2 = (de_vec2){1, 1};
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    d = desc_of(DE_SHAPE2D_CHAIN);
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    d = desc_of((de_shape2d_kind)42);
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &d)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, NULL)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(NULL, &d)));
    EXPECT_EQ(p->shape_live_count, 0u);

    /* Getters on stale handles. */
    de_aabb2d box;
    de_mass_props2d m;
    de_shape2d_kind k;
    EXPECT_FALSE(de_shape2d_get_local_aabb(p, (de_shape){0}, &box));
    EXPECT_FALSE(de_shape2d_get_kind(p, (de_shape){0x00100001u}, &k));
    EXPECT_EQ(de_shape2d_get_mass_props(p, (de_shape){0}, 1.0f, &m), DE_ERR_INVALID_ARGUMENT);
    d = desc_of(DE_SHAPE2D_CIRCLE);
    d.radius = 1.0f;
    const de_shape c = de_shape2d_create(p, &d);
    EXPECT_EQ(de_shape2d_get_mass_props(p, c, -1.0f, &m), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_shape2d_get_mass_props(p, c, 1.0f, NULL), DE_ERR_INVALID_ARGUMENT);
    /* Density 0 is the default (1 kg/m^2). */
    ASSERT_EQ(de_shape2d_get_mass_props(p, c, 0.0f, &m), DE_OK);
    EXPECT_FLOAT_EQ(m.mass, PI_D, 1e-6);
}

TEST(physics2d_shapes, oom_no_leak) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &a;
    d.max_bodies = 4;
    de_physics2d_world *p;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    const de_vec2 pts[5] = {{0, 0}, {-1, 0}, {-2, 1}, {-3, 1}, {-4, 0}};
    de_shape2d_desc sd = desc_of(DE_SHAPE2D_BOX);
    sd.half_extents = (de_vec2){1, 1};
    /* shape struct, slot arrays (first shape), chain data: fail each in turn. */
    for (i64 n = 0; n < 3; ++n) {
        de_tracking_allocator_fail_after(&t, n);
        EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_chain_create(p, pts, 5, false)));
        EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
        de_tracking_allocator_fail_after(&t, -1);
    }
    for (i64 n = 0; n < 2; ++n) {
        de_tracking_allocator_fail_after(&t, n);
        EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_create(p, &sd)));
        de_tracking_allocator_fail_after(&t, -1);
    }
    EXPECT_EQ(p->shape_live_count, 0u);
    ASSERT_TRUE(DE_HANDLE_VALID(de_shape2d_chain_create(p, pts, 5, true)));
    ASSERT_TRUE(DE_HANDLE_VALID(de_shape2d_create(p, &sd)));
    /* Many shapes: slot arrays grow (copy, free the old ones). */
    for (u32 i = 0; i < 40; ++i)
        ASSERT_TRUE(DE_HANDLE_VALID(de_shape2d_create(p, &sd)));
    EXPECT_EQ(p->shape_live_count, 42u);
    de_physics2d_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}

/* ---- mass properties -------------------------------------------------------------------- */

TEST_F(physics2d_shapes, mass_analytic_circle_box_capsule, sh_fx) {
    de_physics2d_world *p = f->p;
    de_mass_props2d m;
    const f64 rho = 2.5;
    static const f32 radii[] = {0.01f, 0.5f, 1.0f, 7.25f, 300.0f};
    for (u32 i = 0; i < 5; ++i) {
        const f64 r = radii[i];
        /* Circle, off center. */
        de_shape2d_desc d = desc_of(DE_SHAPE2D_CIRCLE);
        d.radius = radii[i];
        d.center = (de_vec2){1.5f * (f32)i, -2.0f};
        const de_shape c = de_shape2d_create(p, &d);
        ASSERT_EQ(de_shape2d_get_mass_props(p, c, (f32)rho, &m), DE_OK);
        const f64 ca = PI_D * r * r;
        expect_rel(m.area, ca, 1e-6);
        expect_rel(m.mass, rho * ca, 1e-6);
        expect_rel(m.inertia, rho * ca * r * r / 2.0, 1e-5);
        EXPECT_TRUE(m.center.x == d.center.x && m.center.y == d.center.y);

        /* Box: half extents (r, 0.6 r), rotated and offset by a few r (the offset is in units
         * of r, so f32 rounding of the vertices stays far below 1e-5 of the size); inertia about
         * its center. */
        d = desc_of(DE_SHAPE2D_BOX);
        d.half_extents = (de_vec2){radii[i], 0.6f * radii[i]};
        d.center = (de_vec2){-3.0f * radii[i], 0.25f * (f32)i * radii[i]};
        d.angle = 0.37f * (f32)i;
        const de_shape b = de_shape2d_create(p, &d);
        ASSERT_EQ(de_shape2d_get_mass_props(p, b, (f32)rho, &m), DE_OK);
        const f64 hx = d.half_extents.x, hy = d.half_extents.y;
        const f64 ba = 4.0 * hx * hy;
        expect_rel(m.area, ba, 1e-5);
        expect_rel(m.inertia, rho * ba * (4.0 * hx * hx + 4.0 * hy * hy) / 12.0, 1e-5);
        EXPECT_FLOAT_EQ(m.center.x, d.center.x, 1e-6 * r);
        EXPECT_FLOAT_EQ(m.center.y, d.center.y, 1e-6 * r);

        /* Capsule: a 2r x L rectangle plus two half discs (each centroid 4r / 3 pi out). */
        d = desc_of(DE_SHAPE2D_CAPSULE);
        d.radius = radii[i];
        d.p1 = (de_vec2){-radii[i], 2.0f * radii[i]};
        d.p2 = (de_vec2){2.0f * radii[i], 6.0f * radii[i]};
        const de_shape cap = de_shape2d_create(p, &d);
        ASSERT_EQ(de_shape2d_get_mass_props(p, cap, (f32)rho, &m), DE_OK);
        const f64 len = 5.0 * r;
        const f64 rect = 2.0 * r * len, half = PI_D * r * r / 2.0, dc = 4.0 * r / (3.0 * PI_D);
        const f64 i_rect = rect * (len * len + 4.0 * r * r) / 12.0;
        const f64 i_half =
            half * r * r / 2.0 - half * dc * dc + half * (len / 2 + dc) * (len / 2 + dc);
        expect_rel(m.area, rect + 2.0 * half, 1e-5);
        expect_rel(m.inertia, rho * (i_rect + 2.0 * i_half), 1e-5);
        EXPECT_FLOAT_EQ(m.center.x, 0.5 * r, 1e-6 * r);
        EXPECT_FLOAT_EQ(m.center.y, 4.0 * r, 1e-6 * r);
    }
    /* A capsule with p1 == p2 is a circle. */
    de_shape2d_desc d = desc_of(DE_SHAPE2D_CAPSULE);
    d.radius = 0.75f;
    d.p1 = d.p2 = (de_vec2){2, 3};
    ASSERT_EQ(de_shape2d_get_mass_props(p, de_shape2d_create(p, &d), 1.0f, &m), DE_OK);
    expect_rel(m.inertia, PI_D * 0.75 * 0.75 * 0.75 * 0.75 / 2.0, 1e-6);
}

TEST_F(physics2d_shapes, mass_rounded_and_polygons, sh_fx) {
    de_physics2d_world *p = f->p;
    de_mass_props2d m;
    /* Rounded box: core + 4 edge strips + 4 quarter discs, inertia derived independently. */
    const f64 hx = 1.5, hy = 0.5, r = 0.25;
    de_shape2d_desc d = desc_of(DE_SHAPE2D_BOX);
    d.half_extents = (de_vec2){(f32)hx, (f32)hy};
    d.radius = (f32)r;
    ASSERT_EQ(de_shape2d_get_mass_props(p, de_shape2d_create(p, &d), 1.0f, &m), DE_OK);
    const f64 a_core = 4 * hx * hy, a_side = 2 * hy * r, a_top = 2 * hx * r;
    const f64 i_core = a_core * (4 * hx * hx + 4 * hy * hy) / 12.0;
    const f64 i_side = a_side * (r * r + 4 * hy * hy) / 12.0 + a_side * (hx + r / 2) * (hx + r / 2);
    const f64 i_top = a_top * (4 * hx * hx + r * r) / 12.0 + a_top * (hy + r / 2) * (hy + r / 2);
    const f64 i_corners = PI_D * r * r * r * r / 2.0 + 8.0 * (hx + hy) * r * r * r / 3.0 +
                          PI_D * r * r * (hx * hx + hy * hy);
    expect_rel(m.area, a_core + 2 * a_side + 2 * a_top + PI_D * r * r, 1e-6);
    expect_rel(m.inertia, i_core + 2 * i_side + 2 * i_top + i_corners, 1e-6);
    EXPECT_FLOAT_EQ(m.center.x, 0.0, 1e-7);
    EXPECT_FLOAT_EQ(m.center.y, 0.0, 1e-7);

    /* Steiner: a convex polygon rounded by r has area A + perimeter * r + pi r^2. */
    const de_vec2 pts[5] = {{0, 0}, {3, -0.5f}, {4, 1.5f}, {2, 3}, {-0.5f, 2}};
    for (u32 k = 0; k < 4; ++k) {
        const f32 rr = 0.1f * (f32)k;
        const de_shape s = poly_shape(p, pts, 5, rr);
        ASSERT_TRUE(DE_HANDLE_VALID(s));
        const de__polygon2d *poly = &de__shape2d_get(p, s)->u.polygon;
        f64 area = 0.0, perim = 0.0;
        for (u32 i = 0; i < poly->count; ++i) {
            const de_vec2 a = poly->vertices[i], b = poly->vertices[(i + 1) % poly->count];
            area += 0.5 * ((f64)a.x * b.y - (f64)a.y * b.x);
            perim +=
                sqrt(((f64)b.x - a.x) * ((f64)b.x - a.x) + ((f64)b.y - a.y) * ((f64)b.y - a.y));
        }
        ASSERT_EQ(de_shape2d_get_mass_props(p, s, 1.0f, &m), DE_OK);
        expect_rel(m.area, area + perim * rr + PI_D * (f64)rr * rr, 1e-6);
        EXPECT_TRUE(m.inertia > 0.0f);
    }

    /* A polygon of a box's corners equals the box. */
    const de_vec2 corners[4] = {{-2, -1}, {2, -1}, {2, 1}, {-2, 1}};
    de_mass_props2d mb;
    ASSERT_EQ(de_shape2d_get_mass_props(p, poly_shape(p, corners, 4, 0.0f), 3.0f, &m), DE_OK);
    d = desc_of(DE_SHAPE2D_BOX);
    d.half_extents = (de_vec2){2, 1};
    ASSERT_EQ(de_shape2d_get_mass_props(p, de_shape2d_create(p, &d), 3.0f, &mb), DE_OK);
    EXPECT_FLOAT_EQ(m.mass, mb.mass, 1e-5);
    EXPECT_FLOAT_EQ(m.inertia, mb.inertia, 1e-5);

    /* A rounded octagon translated by 1000 m: the centroid follows, area and inertia about the
     * centroid stay (up to the f32 rounding of the far vertices). */
    de_vec2 oct[8], oct_far[8];
    for (u32 i = 0; i < 8; ++i) {
        f32 s, c;
        de_dsincos(0.785398163f * (f32)i, &s, &c);
        oct[i] = (de_vec2){c, s};
        oct_far[i] = (de_vec2){c + 1000.0f, s - 1000.0f};
    }
    de_mass_props2d mf;
    ASSERT_EQ(de_shape2d_get_mass_props(p, poly_shape(p, oct, 8, 0.5f), 1.0f, &m), DE_OK);
    ASSERT_EQ(de_shape2d_get_mass_props(p, poly_shape(p, oct_far, 8, 0.5f), 1.0f, &mf), DE_OK);
    EXPECT_FLOAT_EQ(mf.center.x, 1000.0, 1e-3);
    EXPECT_FLOAT_EQ(mf.center.y, -1000.0, 1e-3);
    expect_rel(mf.inertia, m.inertia, 5e-4);
    expect_rel(mf.area, m.area, 5e-4);

    /* Segments and chains have no mass. */
    d = desc_of(DE_SHAPE2D_SEGMENT);
    d.p1 = (de_vec2){0, 0};
    d.p2 = (de_vec2){1, 0};
    ASSERT_EQ(de_shape2d_get_mass_props(p, de_shape2d_create(p, &d), 5.0f, &m), DE_OK);
    EXPECT_TRUE(m.mass == 0.0f && m.area == 0.0f && m.inertia == 0.0f);
}

/* ---- AABBs and support ------------------------------------------------------------------ */

/* Exact f64 extent of a shape along unit direction u (from its support), at (pos, rot). */
static f64 extent_along(const de__shape2d *s, de__d2 pos, de_rot2d rot, f64 ux, f64 uy) {
    const de_vec2 local_dir = de__rot2d_apply_inv(rot, (de_vec2){(f32)ux, (f32)uy});
    /* The core support in f64, plus the radius. */
    const de_vec2 c = de__shape2d_support_core(s, local_dir);
    const de__d2 w = de__d2_add(pos, de__rot2d_apply_d(rot, de__d2_from(c)));
    return w.x * ux + w.y * uy + (f64)s->radius;
}

TEST_F(physics2d_shapes, aabb_under_rotation, sh_fx) {
    de_physics2d_world *p = f->p;
    de_shape shapes[6];
    de_shape2d_desc d = desc_of(DE_SHAPE2D_CIRCLE);
    d.radius = 0.7f;
    d.center = (de_vec2){0.5f, -0.25f};
    shapes[0] = de_shape2d_create(p, &d);
    d = desc_of(DE_SHAPE2D_CAPSULE);
    d.radius = 0.3f;
    d.p1 = (de_vec2){-1, 0.2f};
    d.p2 = (de_vec2){1.5f, -0.4f};
    shapes[1] = de_shape2d_create(p, &d);
    d = desc_of(DE_SHAPE2D_BOX);
    d.half_extents = (de_vec2){1.2f, 0.4f};
    d.center = (de_vec2){0.3f, 0.1f};
    d.angle = 0.4f;
    d.radius = 0.05f;
    shapes[2] = de_shape2d_create(p, &d);
    const de_vec2 pts[6] = {{0, 0}, {2, -0.5f}, {3, 1}, {1.5f, 2.5f}, {-0.5f, 1.5f}, {1, 1}};
    shapes[3] = poly_shape(p, pts, 6, 0.1f);
    d = desc_of(DE_SHAPE2D_SEGMENT);
    d.p1 = (de_vec2){-2, -1};
    d.p2 = (de_vec2){1, 2};
    shapes[4] = de_shape2d_create(p, &d);
    shapes[5] = de_shape2d_chain_create(p, pts, 5, true);
    de_rng r = de_rng_seed(77, 3);
    for (u32 k = 0; k < 6; ++k) {
        ASSERT_TRUE(DE_HANDLE_VALID(shapes[k]));
        const de__shape2d *s = de__shape2d_get(p, shapes[k]);
        for (u32 t = 0; t < 200; ++t) {
            const f64 far = (t % 4u == 3u) ? 1e5 : 10.0;
            const de__d2 pos =
                de__d2_make((de_rng_f64(&r) * 2.0 - 1.0) * far, (de_rng_f64(&r) * 2.0 - 1.0) * far);
            const de_rot2d rot =
                t == 0 ? de__rot2d_identity() : de_rot2d_from_angle(6.2831853f * de_rng_f32(&r));
            const de_aabb2d box = de__shape2d_world_aabb(s, pos, rot);
            if (s->kind == DE_SHAPE2D_CHAIN) {
                /* Contains every point (Arvo of the local box: not tight under rotation). */
                for (u32 i = 0; i < 5; ++i) {
                    const de__d2 w = de__d2_add(pos, de__rot2d_apply_d(rot, de__d2_from(pts[i])));
                    EXPECT_TRUE(w.x >= box.min.x && w.x <= box.max.x);
                    EXPECT_TRUE(w.y >= box.min.y && w.y <= box.max.y);
                }
                continue;
            }
            /* Contains the exact extents along +-x, +-y, and is tight to an f32 step. */
            const f64 xmax = extent_along(s, pos, rot, 1, 0),
                      xmin = -extent_along(s, pos, rot, -1, 0);
            const f64 ymax = extent_along(s, pos, rot, 0, 1),
                      ymin = -extent_along(s, pos, rot, 0, -1);
            const f64 slack = 2e-6 * (1.0 + far) + 1e-6;
            EXPECT_TRUE(box.min.x <= xmin && box.min.x >= xmin - slack);
            EXPECT_TRUE(box.max.x >= xmax && box.max.x <= xmax + slack);
            EXPECT_TRUE(box.min.y <= ymin && box.min.y >= ymin - slack);
            EXPECT_TRUE(box.max.y >= ymax && box.max.y <= ymax + slack);
        }
    }
    /* Local boxes include the rounding; the transform of an empty box stays empty. */
    de_aabb2d lb;
    ASSERT_TRUE(de_shape2d_get_local_aabb(p, shapes[0], &lb));
    EXPECT_FLOAT_EQ(lb.min.x, -0.2, 1e-6);
    EXPECT_FLOAT_EQ(lb.max.x, 1.2, 1e-6);
    EXPECT_FLOAT_EQ(lb.min.y, -0.95, 1e-6);
    EXPECT_FLOAT_EQ(lb.max.y, 0.45, 1e-6);
    ASSERT_TRUE(de_shape2d_get_local_aabb(p, shapes[3], &lb));
    EXPECT_FLOAT_EQ(lb.min.x, -0.6, 1e-6);
    EXPECT_FLOAT_EQ(lb.max.y, 2.6, 1e-6);
    EXPECT_TRUE(de__aabb2d_is_empty(
        de__aabb2d_transform(de__aabb2d_empty(), de__d2_make(1, 1), de__rot2d_identity())));
}

TEST_F(physics2d_shapes, support_functions, sh_fx) {
    de_physics2d_world *p = f->p;
    const de_vec2 pts[7] = {{0, 0},        {2, -0.5f}, {3, 1},      {1.5f, 2.5f},
                            {-0.5f, 1.5f}, {1, 1},     {0.5f, 0.5f}};
    const de_shape s = poly_shape(p, pts, 7, 0.2f);
    const de__shape2d *sh = de__shape2d_get(p, s);
    de_rng r = de_rng_seed(3, 3);
    for (u32 t = 0; t < 500; ++t) {
        f32 sn, cs;
        de_dsincos(6.2831853f * de_rng_f32(&r), &sn, &cs);
        const de_vec2 dir = {cs * 3.0f, sn * 3.0f};
        const de_vec2 core = de__shape2d_support_core(sh, dir);
        f32 best = -1e30f;
        for (u32 i = 0; i < sh->u.polygon.count; ++i)
            best = fmaxf(best, de__v2_dot(sh->u.polygon.vertices[i], dir));
        EXPECT_TRUE(de__v2_dot(core, dir) == best);
        const de_vec2 full = de__shape2d_support(sh, dir);
        EXPECT_FLOAT_EQ(full.x, core.x + 0.2 * cs, 1e-5);
        EXPECT_FLOAT_EQ(full.y, core.y + 0.2 * sn, 1e-5);
    }
    /* Ties go to the lowest index; a zero dir pushes along +x. */
    const de_vec2 z = de__shape2d_support(sh, (de_vec2){0, 0});
    EXPECT_TRUE(z.x == sh->u.polygon.vertices[0].x + 0.2f && z.y == sh->u.polygon.vertices[0].y);
    de_shape2d_desc d = desc_of(DE_SHAPE2D_CAPSULE);
    d.radius = 0.5f;
    d.p1 = (de_vec2){0, -1};
    d.p2 = (de_vec2){0, 1};
    const de__shape2d *cap = de__shape2d_get(p, de_shape2d_create(p, &d));
    EXPECT_TRUE(de__shape2d_support_core(cap, (de_vec2){1, 0}).y == -1.0f);
    EXPECT_TRUE(de__shape2d_support_core(cap, (de_vec2){1, 0.01f}).y == 1.0f);
    const de_vec2 cs = de__shape2d_support(cap, (de_vec2){0, -2});
    EXPECT_TRUE(cs.x == 0.0f && cs.y == -1.5f);
    d = desc_of(DE_SHAPE2D_CIRCLE);
    d.radius = 2.0f;
    d.center = (de_vec2){1, 1};
    const de__shape2d *circ = de__shape2d_get(p, de_shape2d_create(p, &d));
    const de_vec2 c2 = de__shape2d_support(circ, (de_vec2){-1, 0});
    EXPECT_TRUE(c2.x == -1.0f && c2.y == 1.0f);
    d = desc_of(DE_SHAPE2D_SEGMENT);
    d.p1 = (de_vec2){0, 0};
    d.p2 = (de_vec2){2, 0};
    const de__shape2d *seg = de__shape2d_get(p, de_shape2d_create(p, &d));
    EXPECT_TRUE(de__shape2d_support(seg, (de_vec2){1, 1}).x == 2.0f);
    EXPECT_TRUE(seg->u.segment.normal.x == 0.0f && seg->u.segment.normal.y == -1.0f);
    EXPECT_TRUE(de__shape2d_kind_is_convex(DE_SHAPE2D_SEGMENT));
    EXPECT_FALSE(de__shape2d_kind_is_convex(DE_SHAPE2D_CHAIN));
}

/* ---- chains ----------------------------------------------------------------------------- */

TEST_F(physics2d_shapes, chain_ghost_vertices, sh_fx) {
    de_physics2d_world *p = f->p;
    /* Ground running right to left: normals point up (+y). */
    const de_vec2 pts[4] = {{3, 0}, {1, 0}, {0, 1}, {-2, 1}};
    const de_shape open = de_shape2d_chain_create(p, pts, 4, false);
    ASSERT_TRUE(DE_HANDLE_VALID(open));
    const de__shape2d *s = de__shape2d_get(p, open);
    EXPECT_EQ(s->kind, DE_SHAPE2D_CHAIN);
    EXPECT_FALSE(s->has_area);
    const de__chain2d *c = &s->u.chain;
    ASSERT_EQ(c->segment_count, 3u);
    EXPECT_EQ(c->point_count, 4u);
    EXPECT_FALSE(c->loop);
    EXPECT_TRUE(c->points != pts && c->points[3].x == -2.0f);
    const de__chain_segment *g = c->segments;
    /* Segment 0: open start, extended ghost; ghost2 = point 2. */
    EXPECT_EQ(g[0].flags, (u32)DE__CHAIN_GHOST1_OPEN);
    EXPECT_TRUE(g[0].ghost1.x == 5.0f && g[0].ghost1.y == 0.0f);
    EXPECT_TRUE(g[0].p1.x == 3.0f && g[0].p2.x == 1.0f);
    EXPECT_TRUE(g[0].ghost2.x == 0.0f && g[0].ghost2.y == 1.0f);
    EXPECT_TRUE(g[0].normal.x == 0.0f && g[0].normal.y == 1.0f);
    /* Segment 1: both neighbours real. */
    EXPECT_EQ(g[1].flags, 0u);
    EXPECT_TRUE(g[1].ghost1.x == 3.0f && g[1].ghost2.x == -2.0f);
    EXPECT_FLOAT_EQ(g[1].normal.x, 0.70710678, 1e-7);
    EXPECT_FLOAT_EQ(g[1].normal.y, 0.70710678, 1e-7);
    /* Segment 2: open end. */
    EXPECT_EQ(g[2].flags, (u32)DE__CHAIN_GHOST2_OPEN);
    EXPECT_TRUE(g[2].ghost1.x == 1.0f && g[2].ghost1.y == 0.0f);
    EXPECT_TRUE(g[2].ghost2.x == -4.0f && g[2].ghost2.y == 1.0f);
    EXPECT_TRUE(g[2].normal.x == 0.0f && g[2].normal.y == 1.0f);

    /* A loop: one segment per point, ghosts wrap around, no open flags. A counter-clockwise
     * loop has outward normals (solid inside). */
    const de_vec2 sq[4] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
    const de_shape loop = de_shape2d_chain_create(p, sq, 4, true);
    ASSERT_TRUE(DE_HANDLE_VALID(loop));
    c = &de__shape2d_get(p, loop)->u.chain;
    ASSERT_EQ(c->segment_count, 4u);
    EXPECT_TRUE(c->loop);
    g = c->segments;
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_EQ(g[i].flags, 0u);
        const de_vec2 want1 = sq[(i + 3) % 4], wp1 = sq[i], wp2 = sq[(i + 1) % 4],
                      want2 = sq[(i + 2) % 4];
        EXPECT_TRUE(g[i].ghost1.x == want1.x && g[i].ghost1.y == want1.y);
        EXPECT_TRUE(g[i].p1.x == wp1.x && g[i].p1.y == wp1.y);
        EXPECT_TRUE(g[i].p2.x == wp2.x && g[i].p2.y == wp2.y);
        EXPECT_TRUE(g[i].ghost2.x == want2.x && g[i].ghost2.y == want2.y);
        /* Outward: the midpoint plus the normal is outside the square. */
        const f32 mx = 0.5f * (wp1.x + wp2.x) + g[i].normal.x,
                  my = 0.5f * (wp1.y + wp2.y) + g[i].normal.y;
        EXPECT_TRUE(mx < 0.0f || mx > 2.0f || my < 0.0f || my > 2.0f);
    }
    /* A 3-point loop: each segment's ghosts are the third point. */
    const de_shape tri = de_shape2d_chain_create(p, sq, 3, true);
    ASSERT_TRUE(DE_HANDLE_VALID(tri));
    g = de__shape2d_get(p, tri)->u.chain.segments;
    EXPECT_TRUE(g[0].ghost1.x == 2.0f && g[0].ghost1.y == 2.0f);
    EXPECT_TRUE(g[0].ghost2.x == 2.0f && g[0].ghost2.y == 2.0f);
    /* Two points make one open segment with both ends free. */
    const de_shape one = de_shape2d_chain_create(p, sq, 2, false);
    ASSERT_TRUE(DE_HANDLE_VALID(one));
    EXPECT_EQ(de__shape2d_get(p, one)->u.chain.segments[0].flags,
              (u32)(DE__CHAIN_GHOST1_OPEN | DE__CHAIN_GHOST2_OPEN));

    /* Refusals: too few points, repeated points (also last == first of a loop), non-finite. */
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_chain_create(p, sq, 1, false)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_chain_create(p, sq, 2, true)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_chain_create(p, NULL, 4, false)));
    const de_vec2 dup[3] = {{0, 0}, {1, 0}, {1, 0}};
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_chain_create(p, dup, 3, false)));
    const de_vec2 closed[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}};
    EXPECT_TRUE(DE_HANDLE_VALID(de_shape2d_chain_create(p, closed, 4, false)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_chain_create(p, closed, 4, true)));
    const de_vec2 nan_pts[3] = {{0, 0}, {NAN, 0}, {1, 1}};
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_chain_create(p, nan_pts, 3, false)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape2d_chain_create(NULL, sq, 3, false)));
    de_shape2d_kind k;
    ASSERT_TRUE(de_shape2d_get_kind(p, loop, &k));
    EXPECT_EQ(k, DE_SHAPE2D_CHAIN);
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
static void hb_v2(hbuf *b, de_vec2 v) {
    hb_f32(b, v.x);
    hb_f32(b, v.y);
}

/* Everything a built shape holds that later code reads, plus its mass at density 2.5. */
static u64 hash_shape(const de_physics2d_world *p, de_shape s) {
    const de__shape2d *sh = de__shape2d_get(p, s);
    de_mass_props2d m;
    ASSERT_EQ(de_shape2d_get_mass_props(p, s, 2.5f, &m), DE_OK);
    hbuf b = {0};
    hb_u32(&b, (u32)sh->kind);
    hb_f32(&b, m.mass);
    hb_f32(&b, m.area);
    hb_v2(&b, m.center);
    hb_f32(&b, m.inertia);
    hb_v2(&b, sh->local_aabb.min);
    hb_v2(&b, sh->local_aabb.max);
    hb_f32(&b, sh->radius);
    hb_f32(&b, sh->bounding_radius);
    switch (sh->kind) {
    case DE_SHAPE2D_BOX:
    case DE_SHAPE2D_POLYGON: {
        const de__polygon2d *poly = &sh->u.polygon;
        hb_u32(&b, poly->count);
        for (u32 i = 0; i < poly->count; ++i) {
            hb_v2(&b, poly->vertices[i]);
            hb_v2(&b, poly->normals[i]);
        }
        hb_v2(&b, poly->centroid);
        break;
    }
    case DE_SHAPE2D_SEGMENT:
        hb_v2(&b, sh->u.segment.normal);
        break;
    case DE_SHAPE2D_CHAIN: {
        const de__chain2d *c = &sh->u.chain;
        hb_u32(&b, c->segment_count);
        for (u32 i = 0; i < c->segment_count; ++i) {
            hb_v2(&b, c->segments[i].ghost1);
            hb_v2(&b, c->segments[i].p1);
            hb_v2(&b, c->segments[i].p2);
            hb_v2(&b, c->segments[i].ghost2);
            hb_v2(&b, c->segments[i].normal);
            hb_u32(&b, c->segments[i].flags);
        }
        break;
    }
    default:
        break;
    }
    return b.h;
}

/* Point set k of the golden vector (and of random_point_sets): five families. Returns the
 * count; *radius gets the rounding radius. */
static u32 point_set(u32 k, de_vec2 *out, f32 *radius) {
    de_rng r = de_rng_seed(2024, k);
    const f32 scale = 0.05f + 20.0f * de_rng_f32(&r);
    const f32 ox = (de_rng_f32(&r) * 2.0f - 1.0f) * 500.0f;
    const f32 oy = (de_rng_f32(&r) * 2.0f - 1.0f) * 500.0f;
    *radius = (k % 3u == 0u) ? 0.0f : 0.2f * scale * de_rng_f32(&r);
    u32 n = 0;
    switch (k % 5u) {
    case 0: { /* few uniform points in a square */
        const u32 m = 3u + de_rng_range(&r, 0, 6);
        for (u32 i = 0; i < m; ++i) {
            const f32 x = de_rng_f32(&r) * 2.0f - 1.0f;
            const f32 y = de_rng_f32(&r) * 2.0f - 1.0f;
            out[n++] = (de_vec2){x * scale + ox, y * scale + oy};
        }
        break;
    }
    case 1: { /* points on a circle (5..11: hulls above 8 are refused) */
        const u32 m = 5u + de_rng_range(&r, 0, 7);
        for (u32 i = 0; i < m; ++i) {
            f32 s, c;
            de_dsincos(6.2831853f * ((f32)i + 0.4f * de_rng_f32(&r)) / (f32)m, &s, &c);
            out[n++] = (de_vec2){c * scale + ox, s * scale + oy};
        }
        break;
    }
    case 2: { /* a convex 3..8-gon, interior points and duplicates, shuffled */
        const u32 m = 3u + de_rng_range(&r, 0, 6);
        for (u32 i = 0; i < m; ++i) {
            f32 s, c;
            de_dsincos(6.2831853f * ((f32)i + 0.3f * de_rng_f32(&r)) / (f32)m, &s, &c);
            out[n++] = (de_vec2){c * scale + ox, 0.6f * s * scale + oy};
        }
        const u32 inner = de_rng_range(&r, 0, 30);
        for (u32 i = 0; i < inner; ++i) {
            f32 s, c;
            const f32 rad = 0.4f * de_rng_f32(&r);
            de_dsincos(6.2831853f * de_rng_f32(&r), &s, &c);
            out[n++] = (de_vec2){rad * c * scale + ox, 0.6f * rad * s * scale + oy};
        }
        for (u32 i = 0; i < 4; ++i) {
            const u32 src = de_rng_range(&r, 0, n);
            out[n] = out[src];
            ++n;
        }
        for (u32 i = n - 1; i > 0; --i) {
            const u32 j = de_rng_range(&r, 0, i + 1);
            const de_vec2 t = out[i];
            out[i] = out[j];
            out[j] = t;
        }
        break;
    }
    case 3: { /* nearly collinear: noise of 1e-6 .. 1e-2 of the scale across a line */
        const u32 m = 3u + de_rng_range(&r, 0, 8);
        const f32 noise = (k % 2u == 0u) ? 1e-6f : 1e-2f;
        for (u32 i = 0; i < m; ++i) {
            const f32 t = de_rng_f32(&r) * 2.0f - 1.0f;
            const f32 e = (de_rng_f32(&r) * 2.0f - 1.0f) * noise;
            out[n++] =
                (de_vec2){(t * 0.8f - e * 0.6f) * scale + ox, (t * 0.6f + e * 0.8f) * scale + oy};
        }
        break;
    }
    default: { /* a rectangle with points on its edges, jittered by 1e-7 of the scale */
        const f32 w = 0.2f + de_rng_f32(&r), h = 0.2f + de_rng_f32(&r);
        const de_vec2 c4[4] = {{-w, -h}, {w, -h}, {w, h}, {-w, h}};
        for (u32 i = 0; i < 4; ++i) {
            out[n++] = (de_vec2){c4[i].x * scale + ox, c4[i].y * scale + oy};
            for (u32 j = 1; j < 4; ++j) {
                const f32 t = (f32)j / 4.0f;
                const de_vec2 a = c4[i], b = c4[(i + 1) % 4];
                const f32 jit = (de_rng_f32(&r) * 2.0f - 1.0f) * 1e-7f;
                out[n++] = (de_vec2){(a.x + (b.x - a.x) * t + jit) * scale + ox,
                                     (a.y + (b.y - a.y) * t - jit) * scale + oy};
            }
        }
        break;
    }
    }
    return n;
}

TEST_F(physics2d_shapes, random_point_sets, sh_fx) {
    /* Every point set: built polygons are valid and contain their input; failures are only the
     * documented ones. */
    de_physics2d_world *p = f->p;
    de_vec2 pts[64];
    u32 ok = 0, refused = 0;
    for (u32 k = 0; k < 50; ++k) {
        f32 radius;
        const u32 n = point_set(k, pts, &radius);
        const de_shape s = poly_shape(p, pts, n, radius);
        if (!DE_HANDLE_VALID(s)) {
            EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
            ++refused;
            continue;
        }
        ++ok;
        const de__shape2d *sh = de__shape2d_get(p, s);
        const f64 tol = 2.0 * de__poly2d_auto_tolerance(pts, n);
        check_polygon(&sh->u.polygon, pts, n, tol);
        de_mass_props2d m;
        ASSERT_EQ(de_shape2d_get_mass_props(p, s, 1.0f, &m), DE_OK);
        EXPECT_TRUE(m.area > 0.0f && m.inertia > 0.0f);
        EXPECT_EQ(de_shape2d_destroy(p, s), DE_OK);
    }
    EXPECT_GE(ok, 30u);
    EXPECT_GE(refused, 3u);
}

#define GOLDEN_MAX 64
typedef struct golden_entry {
    char name[32];
    u64 value;
} golden_entry;

static u32 compute_golden(golden_entry *out) {
    u32 n = 0;
    de_physics2d_world *p;
    ASSERT_EQ(de_physics2d_create(NULL, &p), DE_OK);
    de_vec2 pts[64];
    for (u32 k = 0; k < 50; ++k) {
        f32 radius;
        const u32 np = point_set(k, pts, &radius);
        const de_shape s = poly_shape(p, pts, np, radius);
        snprintf(out[n].name, sizeof out[n].name, "poly%02u", k);
        if (DE_HANDLE_VALID(s)) {
            out[n++].value = hash_shape(p, s);
        } else {
            hbuf b = {0};
            hb_u32(&b, (u32)de_error_last()->result);
            out[n++].value = b.h;
        }
    }
    de_shape2d_desc d = desc_of(DE_SHAPE2D_CIRCLE);
    d.radius = 0.45f;
    d.center = (de_vec2){0.1f, -0.2f};
    const de_shape circ = de_shape2d_create(p, &d);
    d = desc_of(DE_SHAPE2D_CAPSULE);
    d.radius = 0.3f;
    d.p1 = (de_vec2){-0.7f, 0.1f};
    d.p2 = (de_vec2){0.9f, 0.35f};
    const de_shape cap = de_shape2d_create(p, &d);
    d = desc_of(DE_SHAPE2D_BOX);
    d.half_extents = (de_vec2){0.5f, 1.25f};
    d.center = (de_vec2){2.0f, -1.0f};
    d.angle = 0.6f;
    const de_shape box = de_shape2d_create(p, &d);
    d.radius = 0.1f;
    const de_shape rbox = de_shape2d_create(p, &d);
    d = desc_of(DE_SHAPE2D_SEGMENT);
    d.p1 = (de_vec2){-3.0f, 0.5f};
    d.p2 = (de_vec2){4.0f, -1.5f};
    const de_shape seg = de_shape2d_create(p, &d);
    de_vec2 terrain[40];
    de_rng r = de_rng_seed(11, 2);
    for (u32 i = 0; i < 40; ++i)
        terrain[i] = (de_vec2){20.0f - (f32)i * 0.75f, 2.0f * de_rng_f32(&r) - 1.0f};
    const de_shape chain_open = de_shape2d_chain_create(p, terrain, 40, false);
    de_vec2 ring[24];
    for (u32 i = 0; i < 24; ++i) {
        f32 s, c;
        de_dsincos(0.261799388f * (f32)i, &s, &c);
        ring[i] = (de_vec2){30.0f * c + 100.0f, 20.0f * s - 50.0f};
    }
    const de_shape chain_loop = de_shape2d_chain_create(p, ring, 24, true);
    const de_shape kinds[7] = {circ, cap, box, rbox, seg, chain_open, chain_loop};
    static const char *const names[7] = {"circle",  "capsule",    "box",       "rounded_box",
                                         "segment", "chain_open", "chain_loop"};
    for (u32 i = 0; i < 7; ++i) {
        ASSERT_TRUE(DE_HANDLE_VALID(kinds[i]));
        snprintf(out[n].name, sizeof out[n].name, "shape_%s", names[i]);
        out[n++].value = hash_shape(p, kinds[i]);
    }
    /* World boxes of every kind at a far, rotated transform. */
    {
        hbuf b = {0};
        const de_rot2d rot = de_rot2d_from_angle(2.3f);
        for (u32 i = 0; i < 7; ++i) {
            const de_aabb2d wb = de__shape2d_world_aabb(de__shape2d_get(p, kinds[i]),
                                                        de__d2_make(-54321.123, 98765.4), rot);
            hb_v2(&b, wb.min);
            hb_v2(&b, wb.max);
        }
        snprintf(out[n].name, sizeof out[n].name, "world_aabbs");
        out[n++].value = b.h;
    }
    de_physics2d_destroy(p);
    return n;
}

TEST(physics2d_shapes, golden_vector) {
    static golden_entry got[GOLDEN_MAX];
    const u32 n = compute_golden(got);
    const char *regen = getenv("DE_REGEN_GOLDEN");
    if (regen != NULL && strcmp(regen, "1") == 0) {
        FILE *out = fopen(DE_PHYSICS2D_SHAPES_GOLDEN, "wb");
        ASSERT_TRUE(out != NULL);
        fprintf(out, "# shapes2d_golden.txt: hashes (XXH3-64) of 50 polygon hulls from fixed\n"
                     "# de_rng point sets (with their mass properties at density 2.5, or the\n"
                     "# refusal code), of every other 2D shape kind and of far rotated world\n"
                     "# boxes (DENG-252). Generated by test_physics2d_shapes with\n"
                     "# DE_REGEN_GOLDEN=1; never edit by hand. x86-64 and arm64 must reproduce\n"
                     "# every line.\n");
        for (u32 i = 0; i < n; ++i)
            fprintf(out, "%s %016llx\n", got[i].name, (unsigned long long)got[i].value);
        fclose(out);
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_PHYSICS2D_SHAPES_GOLDEN);
    }
    FILE *in = fopen(DE_PHYSICS2D_SHAPES_GOLDEN, "rb");
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
        EXPECT_STREQ(name, got[lines].name);
        if ((u64)v != got[lines].value) {
            printf("  %s: expected %016llx got %016llx\n", name, v,
                   (unsigned long long)got[lines].value);
            ++bad;
        }
        ++lines;
    }
    fclose(in);
    EXPECT_EQ(lines, n);
    EXPECT_EQ(bad, 0u);
}
