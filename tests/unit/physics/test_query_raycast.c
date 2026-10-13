/* test_query_raycast.c: ray queries (DENG-137, SPEC §13.1): every shape kind against analytic
 * results, watertight mesh edges, heightfield holes and DDA, the start-inside rule, raycast_all
 * ordering against brute force, filters, the 100 km origin, no allocation, and the checked-in
 * vector of 10,000 rays and 1,000 shape casts.
 *
 * physics_query_ray.golden_vector hashes the hits of 10,000 de_rng rays and 1,000 de_rng shape
 * casts against a mixed scene (every shape kind, filters, triggers, a cluster 25 km out) and
 * compares with tests/unit/physics/vectors/query_golden.txt. x86-64 and arm64 must reproduce it
 * bit for bit. Regenerate (never by hand, only for a deliberate change) with
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics/test_physics_query_ray
 * --filter=physics_query_ray.golden_vector */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen */
#endif

#include "de_alloc_guard.h"
#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/broadphase/broadphase.h"
#include "physics/query/query_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_PHYSICS_QUERY_GOLDEN
#error "DE_PHYSICS_QUERY_GOLDEN (path of query_golden.txt) must be defined by the build"
#endif

/* ---- helpers ------------------------------------------------------------------------------ */

static de_physics_world *world_new(u32 max_bodies) {
    de_physics_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = max_bodies;
    de_physics_world *p = NULL;
    ASSERT_EQ(de_physics_create(&d, &p), DE_OK);
    return p;
}

static de_shape shape_new(de_physics_world *p, const de_shape_desc *d) {
    const de_shape s = de_shape_create(p, d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_shape sphere(de_physics_world *p, f32 r) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_SPHERE;
    d.radius = r;
    return shape_new(p, &d);
}

static de_shape capsule(de_physics_world *p, f32 r, f32 hh) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CAPSULE;
    d.radius = r;
    d.half_height = hh;
    return shape_new(p, &d);
}

static de_shape box(de_physics_world *p, f32 x, f32 y, f32 z) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = (de_vec3){x, y, z};
    return shape_new(p, &d);
}

static de_shape cylinder(de_physics_world *p, f32 r, f32 hh) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CYLINDER;
    d.radius = r;
    d.half_height = hh;
    return shape_new(p, &d);
}

static de_shape hull_box(de_physics_world *p, f32 x, f32 y, f32 z) {
    de_vec3 pts[8];
    for (u32 i = 0; i < 8; ++i)
        pts[i] = (de_vec3){(i & 1) ? x : -x, (i & 2) ? y : -y, (i & 4) ? z : -z};
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CONVEX;
    d.points = pts;
    d.point_count = 8;
    return shape_new(p, &d);
}

/* An n x n grid of unit cells in the XY plane (z = height(i, j)), two triangles per cell,
 * counter-clockwise from +Z. Vertex (i, j) is at (i * cell, j * cell). */
#define GRID_MAX 33u
static de_vec3 g_grid_v[GRID_MAX * GRID_MAX];
static u32 g_grid_i[(GRID_MAX - 1u) * (GRID_MAX - 1u) * 6u];

static de_shape grid_mesh(de_physics_world *p, u32 n, f32 cell, f32 bump, u64 seed) {
    ASSERT_LE(n, GRID_MAX);
    de_rng r = de_rng_seed(seed, 3);
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const f32 z = bump * de_rng_f32(&r);
            g_grid_v[j * n + i] = (de_vec3){(f32)i * cell, (f32)j * cell, z};
        }
    }
    u32 k = 0;
    for (u32 j = 0; j + 1 < n; ++j) {
        for (u32 i = 0; i + 1 < n; ++i) {
            const u32 a = j * n + i, b = a + 1, c = a + n, d = c + 1;
            g_grid_i[k++] = a;
            g_grid_i[k++] = b;
            g_grid_i[k++] = d;
            g_grid_i[k++] = a;
            g_grid_i[k++] = d;
            g_grid_i[k++] = c;
        }
    }
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_MESH;
    d.vertices = g_grid_v;
    d.vertex_count = n * n;
    d.indices = g_grid_i;
    d.index_count = k;
    return shape_new(p, &d);
}

static de_body_desc body_desc(de_body_type type, de_shape s, f64 x, f64 y, f64 z) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform.position = (de_dvec3){x, y, z};
    d.transform.rotation = (de_quat){0, 0, 0, 1};
    d.transform.scale = (de_vec3){1, 1, 1};
    return d;
}

static de_body add(de_physics_world *p, de_body_type type, de_shape s, f64 x, f64 y, f64 z) {
    const de_body_desc d = body_desc(type, s, x, y, z);
    const de_body b = de_body_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    return b;
}

static de_body add_rot(de_physics_world *p, de_body_type type, de_shape s, f64 x, f64 y, f64 z,
                       de_quat q) {
    de_body_desc d = body_desc(type, s, x, y, z);
    d.transform.rotation = q;
    const de_body b = de_body_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    return b;
}

static u32 idx(de_body b) {
    return de__handle_index(b.id);
}

/* Field by field (de_ray_hit has padding, so no memcmp), floats bit for bit. */
static bool hit_same(const de_ray_hit *a, const de_ray_hit *b) {
    return a->entity.bits == b->entity.bits && a->body.id == b->body.id &&
           memcmp(&a->point, &b->point, sizeof a->point) == 0 &&
           memcmp(&a->normal, &b->normal, sizeof a->normal) == 0 &&
           memcmp(&a->fraction, &b->fraction, sizeof a->fraction) == 0 &&
           a->sub_shape == b->sub_shape;
}

static de_collision_filter all_filter(void) {
    de_collision_filter f = {0xFFFFFFFFu, 0xFFFFFFFFu, 0u};
    return f;
}

/* Rotation of `angle` radians about a unit axis, through de_dmath. */
static de_quat axis_angle(f32 x, f32 y, f32 z, f32 angle) {
    const f32 s = de_dsin(angle * 0.5f);
    const f32 c = de_dcos(angle * 0.5f);
    de_quat q = {x * s, y * s, z * s, c};
    return q;
}

static bool ray(const de_physics_world *p, f64 ox, f64 oy, f64 oz, f32 dx, f32 dy, f32 dz, f32 dist,
                de_ray_hit *h) {
    return de_physics_raycast(p, (de_dvec3){ox, oy, oz}, (de_vec3){dx, dy, dz}, dist, all_filter(),
                              h);
}

/* ---- per shape: analytic hits ------------------------------------------------------------- */

TEST(physics_query_ray, sphere_analytic) {
    de_physics_world *p = world_new(64);
    const de_body b = add(p, DE_BODY_STATIC, sphere(p, 1.5f), 3.0, -2.0, 1.0);
    de_ray_hit h;
    ASSERT_TRUE(ray(p, -7.0, -2.0, 1.0, 1, 0, 0, 20.0f, &h));
    EXPECT_EQ(h.body.id, b.id);
    EXPECT_FLOAT_EQ(h.fraction * 20.0, 8.5, 1e-5);
    EXPECT_FLOAT_EQ(h.point.x, 1.5, 1e-5);
    EXPECT_FLOAT_EQ(h.normal.x, -1.0, 1e-6);
    EXPECT_EQ(h.sub_shape, DE_SUB_SHAPE_NONE);
    EXPECT_EQ(h.entity.bits, 0u);
    /* Off-center: the analytic entry point on a sphere of radius 1.5. */
    ASSERT_TRUE(ray(p, -7.0, -1.0, 1.0, 1, 0, 0, 20.0f, &h));
    const f64 x = 3.0 - de_dsqrt64(1.5 * 1.5 - 1.0);
    EXPECT_FLOAT_EQ(h.point.x, x, 1e-5);
    EXPECT_FLOAT_EQ(h.fraction * 20.0, x + 7.0, 1e-5);
    /* Too short, pointing away, passing by. */
    EXPECT_FALSE(ray(p, -7.0, -2.0, 1.0, 1, 0, 0, 8.0f, &h));
    EXPECT_FALSE(ray(p, -7.0, -2.0, 1.0, -1, 0, 0, 20.0f, &h));
    EXPECT_FALSE(ray(p, -7.0, 0.0, 1.0, 1, 0, 0, 20.0f, &h));
    de_physics_destroy(p);
}

TEST(physics_query_ray, capsule_side_and_caps) {
    de_physics_world *p = world_new(64);
    add(p, DE_BODY_STATIC, capsule(p, 0.5f, 1.0f), 0.0, 0.0, 0.0);
    de_ray_hit h;
    ASSERT_TRUE(ray(p, -5.0, 0.0, 0.25, 1, 0, 0, 10.0f, &h)); /* side */
    EXPECT_FLOAT_EQ(h.fraction * 10.0, 4.5, 1e-5);
    EXPECT_FLOAT_EQ(h.normal.x, -1.0, 1e-6);
    EXPECT_FLOAT_EQ(h.normal.z, 0.0, 1e-6);
    ASSERT_TRUE(ray(p, 0.0, 0.0, 5.0, 0, 0, -1, 10.0f, &h)); /* top cap */
    EXPECT_FLOAT_EQ(h.point.z, 1.5, 1e-5);
    EXPECT_FLOAT_EQ(h.normal.z, 1.0, 1e-6);
    ASSERT_TRUE(ray(p, 0.3, 0.0, -5.0, 0, 0, 1, 10.0f, &h)); /* bottom cap, off axis */
    EXPECT_FLOAT_EQ(h.point.z, -1.0 - de_dsqrt64(0.25 - 0.09), 1e-5);
    /* Over the cap: misses. */
    EXPECT_FALSE(ray(p, -5.0, 0.0, 1.51, 1, 0, 0, 10.0f, &h));
    de_physics_destroy(p);
}

TEST(physics_query_ray, box_rotated) {
    de_physics_world *p = world_new(64);
    /* 45 degrees about Z: the +X ray meets an edge at x = -sqrt(2). */
    add_rot(p, DE_BODY_STATIC, box(p, 1, 1, 1), 0.0, 0.0, 0.0, axis_angle(0, 0, 1, 0.785398163f));
    de_ray_hit h;
    ASSERT_TRUE(ray(p, -10.0, 0.25, 0.0, 1, 0, 0, 20.0f, &h));
    EXPECT_FLOAT_EQ(h.point.x, -de_dsqrt64(2.0) + 0.25, 1e-5);
    EXPECT_FLOAT_EQ(h.normal.x, -0.70710678, 1e-5); /* the face toward (-1, +1) */
    EXPECT_FLOAT_EQ(h.normal.y, 0.70710678, 1e-5);
    /* Top face from above. */
    ASSERT_TRUE(ray(p, 0.2, 0.1, 4.0, 0, 0, -1, 20.0f, &h));
    EXPECT_FLOAT_EQ(h.point.z, 1.0, 1e-5);
    EXPECT_FLOAT_EQ(h.normal.z, 1.0, 1e-6);
    de_physics_destroy(p);
}

TEST(physics_query_ray, cylinder_side_and_caps) {
    de_physics_world *p = world_new(64);
    add(p, DE_BODY_STATIC, cylinder(p, 0.75f, 1.25f), 0.0, 0.0, 0.0);
    de_ray_hit h;
    ASSERT_TRUE(ray(p, 0.0, -6.0, 1.0, 0, 1, 0, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.point.y, -0.75, 1e-5);
    EXPECT_FLOAT_EQ(h.normal.y, -1.0, 1e-6);
    ASSERT_TRUE(ray(p, 0.5, 0.2, 6.0, 0, 0, -1, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.point.z, 1.25, 1e-5);
    EXPECT_FLOAT_EQ(h.normal.z, 1.0, 1e-6);
    ASSERT_TRUE(ray(p, 0.5, 0.2, -6.0, 0, 0, 1, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.normal.z, -1.0, 1e-6);
    /* Diagonal into the rim region: the cap wins when the side entry is above the top. */
    ASSERT_TRUE(ray(p, -2.0, 0.0, 3.0, 1, 0, -1, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.point.z, 1.25, 1e-5);
    EXPECT_FALSE(ray(p, 0.8, 0.0, 6.0, 0, 0, -1, 10.0f, &h));
    de_physics_destroy(p);
}

/* A hull built from a box's corners answers every ray like the box. */
TEST(physics_query_ray, hull_matches_box) {
    de_physics_world *p = world_new(64);
    const de_quat q = axis_angle(0.267261f, 0.534522f, 0.801784f, 0.9f);
    const de_body bb = add_rot(p, DE_BODY_STATIC, box(p, 0.7f, 1.1f, 0.4f), 0.0, 0.0, 0.0, q);
    const de_body bh = add_rot(p, DE_BODY_STATIC, hull_box(p, 0.7f, 1.1f, 0.4f), 0.0, 0.0, 0.0, q);
    de_rng r = de_rng_seed(11, 1);
    u32 hits = 0;
    de__qray qr;
    for (u32 i = 0; i < 2000; ++i) {
        const f32 ox = 6.0f * de_rng_f32(&r) - 3.0f;
        const f32 oy = 6.0f * de_rng_f32(&r) - 3.0f;
        const f32 oz = 6.0f * de_rng_f32(&r) - 3.0f;
        const f32 dx = -ox + 0.6f * de_rng_f32(&r) - 0.3f;
        const f32 dy = -oy + 0.6f * de_rng_f32(&r) - 0.3f;
        const f32 dz = -oz + 0.6f * de_rng_f32(&r) - 0.3f;
        ASSERT_TRUE(de__qray_init(&qr, (de_dvec3){ox, oy, oz}, (de_vec3){dx, dy, dz}, 8.0f));
        de_ray_hit a, b;
        const bool ha = de__query_ray_body(p, idx(bb), &qr, 1.0, 0, &a);
        const bool hb = de__query_ray_body(p, idx(bh), &qr, 1.0, 0, &b);
        ASSERT_EQ(ha, hb);
        if (!ha)
            continue;
        ++hits;
        EXPECT_FLOAT_EQ(a.fraction, b.fraction, 2e-6);
        EXPECT_FLOAT_EQ(a.normal.x, b.normal.x, 1e-4);
        EXPECT_FLOAT_EQ(a.normal.y, b.normal.y, 1e-4);
        EXPECT_FLOAT_EQ(a.normal.z, b.normal.z, 1e-4);
    }
    EXPECT_GT(hits, 1000u);
    de_physics_destroy(p);
}

/* ---- start inside ------------------------------------------------------------------------- */

TEST(physics_query_ray, start_inside_every_solid) {
    de_physics_world *p = world_new(64);
    de_shape kids[2] = {sphere(p, 0.5f), box(p, 0.3f, 0.3f, 0.3f)};
    de_transform kx[2];
    memset(kx, 0, sizeof kx);
    kx[0].rotation = (de_quat){0, 0, 0, 1};
    kx[1].rotation = (de_quat){0, 0, 0, 1};
    kx[1].position = (de_dvec3){1.0, 0.0, 0.0};
    de_shape_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.kind = DE_SHAPE_COMPOUND;
    cd.children = kids;
    cd.child_xforms = kx;
    cd.child_count = 2;
    const de_shape shapes[6] = {sphere(p, 1.0f),          capsule(p, 0.5f, 0.5f),
                                box(p, 1.0f, 1.0f, 1.0f), cylinder(p, 0.6f, 0.6f),
                                hull_box(p, 1, 1, 1),     shape_new(p, &cd)};
    for (u32 i = 0; i < 6; ++i) {
        const de_body b = add(p, DE_BODY_STATIC, shapes[i], 10.0 * i, 0.0, 0.0);
        de_ray_hit h;
        const de_vec3 dir = {0.6f, 0.8f, 0.0f};
        ASSERT_TRUE(de_physics_raycast(p, (de_dvec3){10.0 * i + (i == 5 ? 1.0 : 0.0), 0.1, 0.0},
                                       dir, 3.0f, all_filter(), &h));
        EXPECT_EQ(h.body.id, b.id);
        EXPECT_FLOAT_EQ(h.fraction, 0.0f, 0.0);
        EXPECT_FLOAT_EQ(h.normal.x, -0.6, 1e-6);
        EXPECT_FLOAT_EQ(h.normal.y, -0.8, 1e-6);
        EXPECT_FLOAT_EQ(h.point.x, 10.0 * i + (i == 5 ? 1.0 : 0.0), 0.0);
        EXPECT_EQ(h.sub_shape, i == 5 ? 1u : DE_SUB_SHAPE_NONE); /* compound: the child */
    }
    de_physics_destroy(p);
}

/* ---- meshes -------------------------------------------------------------------------------- */

/* Rays aimed exactly at shared edges and vertices of a flat grid never slip through. */
TEST(physics_query_ray, mesh_watertight_edges) {
    de_physics_world *p = world_new(64);
    const de_body b = add(p, DE_BODY_STATIC, grid_mesh(p, 9, 1.0f, 0.0f, 1), 0.0, 0.0, 0.0);
    u32 tested = 0;
    for (u32 j = 1; j < 8; ++j) {
        for (u32 i = 1; i < 8; ++i) {
            /* vertex, horizontal edge, vertical edge, diagonal */
            const f64 xs[4] = {i, i + 0.5, i, i + 0.5};
            const f64 ys[4] = {j, j, j + 0.5, j + 0.5};
            for (u32 k = 0; k < 4; ++k) {
                de_ray_hit h;
                ASSERT_TRUE(ray(p, xs[k] - 0.3, ys[k] + 0.2, 2.0, 0.3f, -0.2f, -2.0f, 10.0f, &h));
                EXPECT_EQ(h.body.id, b.id);
                EXPECT_FLOAT_EQ(h.point.z, 0.0, 1e-9);
                EXPECT_FLOAT_EQ(h.normal.z, 1.0, 1e-6);
                ASSERT_LT(h.sub_shape, 8u * 8u * 2u);
                ++tested;
            }
        }
    }
    EXPECT_EQ(tested, 196u);
    /* A bumpy grid: thousands of rays through random points, none slips through. */
    de_physics_world *q = world_new(64);
    add(q, DE_BODY_STATIC, grid_mesh(q, 17, 0.5f, 0.3f, 7), 0.0, 0.0, 0.0);
    de_rng r = de_rng_seed(5, 5);
    for (u32 n = 0; n < 4000; ++n) {
        const f32 x = 0.01f + 7.98f * de_rng_f32(&r);
        const f32 y = 0.01f + 7.98f * de_rng_f32(&r);
        de_ray_hit h;
        ASSERT_TRUE(ray(q, x, y, 5.0, 0, 0, -1, 10.0f, &h));
        ASSERT_TRUE(ray(q, x, y, -5.0, 0, 0, 1, 10.0f, &h)); /* from below: back faces */
        EXPECT_FLOAT_EQ(h.normal.z, -1.0, 0.3);
    }
    de_physics_destroy(q);
    de_physics_destroy(p);
}

TEST(physics_query_ray, mesh_back_faces_and_ties) {
    de_physics_world *p = world_new(64);
    const de_body b = add(p, DE_BODY_STATIC, grid_mesh(p, 3, 1.0f, 0.0f, 1), 0.0, 0.0, 0.0);
    de_physics_query_filter qf;
    memset(&qf, 0, sizeof qf);
    qf.flags = DE_PHYSICS_QUERY_IGNORE_BACK_FACES;
    de_ray_hit h;
    /* From below: hit two-sided (normal faces the ray), passes with the flag. */
    ASSERT_TRUE(ray(p, 0.3, 0.6, -1.0, 0, 0, 1, 5.0f, &h));
    EXPECT_FLOAT_EQ(h.normal.z, -1.0, 1e-6);
    EXPECT_FALSE(
        de_physics_raycast_ex(p, (de_dvec3){0.3, 0.6, -1.0}, (de_vec3){0, 0, 1}, 5.0f, &qf, &h));
    ASSERT_TRUE(
        de_physics_raycast_ex(p, (de_dvec3){0.3, 0.6, 1.0}, (de_vec3){0, 0, -1}, 5.0f, &qf, &h));
    EXPECT_EQ(h.body.id, b.id);
    /* Exactly on the diagonal of cell 0: both triangles give the same t; the lower id wins. */
    ASSERT_TRUE(ray(p, 0.5, 0.5, 1.0, 0, 0, -1, 5.0f, &h));
    EXPECT_EQ(h.sub_shape, 0u);
    de_physics_destroy(p);
}

/* ---- heightfields ------------------------------------------------------------------------- */

#define HF_N 12u
static f32 g_hf[HF_N * HF_N];
static de_vec3 g_hf_v[HF_N * HF_N];
static u32 g_hf_i[(HF_N - 1u) * (HF_N - 1u) * 6u];

/* A heightfield and a mesh with the same triangles in sub-shape order (hole cells degenerate). */
static void hf_and_mesh(de_physics_world *p, de_shape *hf, de_shape *mesh) {
    de_rng r = de_rng_seed(9, 9);
    for (u32 i = 0; i < HF_N * HF_N; ++i) {
        const f32 h = 2.0f * de_rng_f32(&r) - 1.0f;
        g_hf[i] = h;
    }
    g_hf[5 * HF_N + 6] = DE_HEIGHTFIELD_HOLE;
    g_hf[2 * HF_N + 2] = DE_HEIGHTFIELD_HOLE;
    const de_vec3 sc = {0.75f, 0.5f, 1.0f};
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_HEIGHTFIELD;
    d.heights = g_hf;
    d.rows = HF_N;
    d.cols = HF_N;
    d.scale = sc;
    *hf = shape_new(p, &d);
    for (u32 row = 0; row < HF_N; ++row)
        for (u32 col = 0; col < HF_N; ++col) {
            const f32 h = g_hf[row * HF_N + col];
            g_hf_v[row * HF_N + col] = (de_vec3){(f32)col * sc.x, (f32)row * sc.y,
                                                 h == DE_HEIGHTFIELD_HOLE ? 0.0f : h * sc.z};
        }
    u32 k = 0;
    for (u32 row = 0; row + 1 < HF_N; ++row) {
        for (u32 col = 0; col + 1 < HF_N; ++col) {
            const u32 c0 = row * HF_N + col, c1 = c0 + 1, c2 = c0 + HF_N, c3 = c2 + 1;
            const bool hole = g_hf[c0] == DE_HEIGHTFIELD_HOLE || g_hf[c1] == DE_HEIGHTFIELD_HOLE ||
                              g_hf[c2] == DE_HEIGHTFIELD_HOLE || g_hf[c3] == DE_HEIGHTFIELD_HOLE;
            const u32 t[6] = {c0, c1, c3, c0, c3, c2};
            for (u32 m = 0; m < 6; ++m)
                g_hf_i[k++] = hole ? c0 : t[m];
        }
    }
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_MESH;
    d.vertices = g_hf_v;
    d.vertex_count = HF_N * HF_N;
    d.indices = g_hf_i;
    d.index_count = k;
    *mesh = shape_new(p, &d);
}

/* The 2D DDA finds exactly what testing every triangle finds (the same mesh, brute force). */
TEST(physics_query_ray, heightfield_dda_matches_mesh) {
    de_physics_world *p = world_new(64);
    de_shape hf, mesh;
    hf_and_mesh(p, &hf, &mesh);
    const de_quat q = axis_angle(0.0f, 0.0f, 1.0f, 0.3f);
    const de_body bh = add_rot(p, DE_BODY_STATIC, hf, 5.0, -3.0, 1.0, q);
    const de_body bm = add_rot(p, DE_BODY_STATIC, mesh, 5.0, -3.0, 1.0, q);
    de_rng r = de_rng_seed(12, 4);
    u32 hits = 0;
    for (u32 n = 0; n < 5000; ++n) {
        const f32 ox = 18.0f * de_rng_f32(&r) - 4.0f;
        const f32 oy = 14.0f * de_rng_f32(&r) - 8.0f;
        const f32 oz = 6.0f * de_rng_f32(&r) - 2.0f;
        const f32 dx = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 dy = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 dz = 1.2f * de_rng_f32(&r) - 1.0f; /* mostly down */
        const f32 len = 1.0f + 15.0f * de_rng_f32(&r);
        const u32 flags = (n & 1u) ? (u32)DE_PHYSICS_QUERY_IGNORE_BACK_FACES : 0u;
        de__qray qr;
        if (!de__qray_init(&qr, (de_dvec3){ox, oy, oz}, (de_vec3){dx, dy, dz}, len))
            continue;
        de_ray_hit a, b;
        const bool ha = de__query_ray_body(p, idx(bh), &qr, 1.0, flags, &a);
        const bool hb = de__query_ray_body(p, idx(bm), &qr, 1.0, flags, &b);
        ASSERT_EQ(ha, hb);
        if (!ha)
            continue;
        ++hits;
        ASSERT_EQ(a.sub_shape, b.sub_shape);
        ASSERT_FLOAT_EQ(a.fraction, b.fraction, 0.0);
        ASSERT_MEMEQ(&a.normal, &b.normal, sizeof a.normal);
    }
    EXPECT_GT(hits, 300u);
    de_physics_destroy(p);
}

TEST(physics_query_ray, heightfield_holes_and_ids) {
    de_physics_world *p = world_new(64);
    static f32 h[4 * 4];
    for (u32 i = 0; i < 16; ++i)
        h[i] = 0.0f;
    h[1 * 4 + 1] = DE_HEIGHTFIELD_HOLE; /* cells (0..1, 0..1) are holes */
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_HEIGHTFIELD;
    d.heights = h;
    d.rows = 4;
    d.cols = 4;
    d.scale = (de_vec3){1, 1, 1};
    add(p, DE_BODY_STATIC, shape_new(p, &d), 0.0, 0.0, 0.0);
    de_ray_hit hit;
    EXPECT_FALSE(ray(p, 0.5, 0.5, 1.0, 0, 0, -1, 5.0f, &hit)); /* cell (0, 0): hole */
    EXPECT_FALSE(ray(p, 1.5, 1.2, 1.0, 0, 0, -1, 5.0f, &hit)); /* cell (1, 1): hole */
    ASSERT_TRUE(ray(p, 2.7, 1.2, 1.0, 0, 0, -1, 5.0f, &hit));  /* cell (2, 1), lower triangle */
    EXPECT_EQ(hit.sub_shape, 2u * (1u * 3u + 2u) + 0u);
    ASSERT_TRUE(ray(p, 2.2, 1.7, 1.0, 0, 0, -1, 5.0f, &hit)); /* cell (2, 1), upper triangle */
    EXPECT_EQ(hit.sub_shape, 2u * (1u * 3u + 2u) + 1u);
    /* A grazing ray across the hole lands on the far side. */
    ASSERT_TRUE(ray(p, 0.5, 0.5, 0.1, 1, 0, -0.04f, 10.0f, &hit));
    EXPECT_TRUE(hit.point.x >= 2.0);
    de_physics_destroy(p);
}

/* ---- compounds ---------------------------------------------------------------------------- */

TEST(physics_query_ray, compound_children) {
    de_physics_world *p = world_new(64);
    de_shape kids[3] = {sphere(p, 0.5f), box(p, 0.5f, 0.5f, 0.5f), capsule(p, 0.25f, 0.5f)};
    de_transform kx[3];
    memset(kx, 0, sizeof kx);
    for (u32 i = 0; i < 3; ++i) {
        kx[i].rotation = (de_quat){0, 0, 0, 1};
        kx[i].position = (de_dvec3){2.0 * i, 0.0, 0.0};
    }
    kx[2].rotation = axis_angle(0, 1, 0, 1.5707963f); /* capsule along X */
    de_shape_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.kind = DE_SHAPE_COMPOUND;
    cd.children = kids;
    cd.child_xforms = kx;
    cd.child_count = 3;
    add(p, DE_BODY_STATIC, shape_new(p, &cd), 0.0, 0.0, 0.0);
    de_ray_hit h;
    ASSERT_TRUE(ray(p, -5.0, 0.0, 0.0, 1, 0, 0, 20.0f, &h));
    EXPECT_EQ(h.sub_shape, 0u);
    EXPECT_FLOAT_EQ(h.point.x, -0.5, 1e-5);
    ASSERT_TRUE(ray(p, 2.0, 0.1, 5.0, 0, 0, -1, 20.0f, &h));
    EXPECT_EQ(h.sub_shape, 1u);
    EXPECT_FLOAT_EQ(h.point.z, 0.5, 1e-5);
    ASSERT_TRUE(ray(p, 9.0, 0.0, 0.0, -1, 0, 0, 20.0f, &h));
    EXPECT_EQ(h.sub_shape, 2u);
    EXPECT_FLOAT_EQ(h.point.x, 4.0 + 0.75, 1e-5);
    de_physics_destroy(p);
}

/* ---- ordering, brute force, filters -------------------------------------------------------- */

#define SCENE_BODIES 400u

typedef struct scene {
    de_shape shapes[16];
    u32 nshapes;
} scene;

static void scene_shapes(de_physics_world *p, scene *s, u64 seed) {
    u32 n = 0;
    s->shapes[n++] = sphere(p, 0.6f);
    s->shapes[n++] = capsule(p, 0.4f, 0.7f);
    s->shapes[n++] = box(p, 0.8f, 0.5f, 0.3f);
    s->shapes[n++] = cylinder(p, 0.5f, 0.6f);
    de_rng r = de_rng_seed(seed, 77);
    de_vec3 pts[14];
    for (u32 i = 0; i < 14; ++i) {
        const f32 x = 1.6f * de_rng_f32(&r) - 0.8f;
        const f32 y = 1.6f * de_rng_f32(&r) - 0.8f;
        const f32 z = 1.6f * de_rng_f32(&r) - 0.8f;
        pts[i] = (de_vec3){x, y, z};
    }
    de_shape_desc hd;
    memset(&hd, 0, sizeof hd);
    hd.kind = DE_SHAPE_CONVEX;
    hd.points = pts;
    hd.point_count = 14;
    s->shapes[n++] = shape_new(p, &hd);
    de_shape kids[3] = {s->shapes[0], s->shapes[2], s->shapes[3]};
    de_transform kx[3];
    memset(kx, 0, sizeof kx);
    for (u32 i = 0; i < 3; ++i) {
        kx[i].rotation = axis_angle(0, 0, 1, 0.4f * (f32)i);
        kx[i].position = (de_dvec3){0.0, 0.0, 1.2 * i};
    }
    de_shape_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.kind = DE_SHAPE_COMPOUND;
    cd.children = kids;
    cd.child_xforms = kx;
    cd.child_count = 3;
    s->shapes[n++] = shape_new(p, &cd);
    s->nshapes = n;
}

static de_quat rnd_quat(de_rng *r) {
    const f32 x = 2.0f * de_rng_f32(r) - 1.0f;
    const f32 y = 2.0f * de_rng_f32(r) - 1.0f;
    const f32 z = 2.0f * de_rng_f32(r) - 1.0f;
    const f32 w = 2.0f * de_rng_f32(r) - 1.0f;
    de_quat q = {x, y, z, w};
    return q;
}

/* A random scene of convex bodies and compounds plus a mesh and a heightfield floor, around
 * (cx, cy, cz). Bodies get layers 1, 2 or 4, some a group, some the trigger flag. */
static void scene_build(de_physics_world *p, scene *s, u32 count, f64 cx, f64 cy, f64 cz,
                        u64 seed) {
    de_rng r = de_rng_seed(seed, 1);
    for (u32 i = 0; i < count; ++i) {
        const u32 k = de_rng_range(&r, 0, s->nshapes);
        const f32 x = 40.0f * de_rng_f32(&r) - 20.0f;
        const f32 y = 40.0f * de_rng_f32(&r) - 20.0f;
        const f32 z = 10.0f * de_rng_f32(&r);
        const de_quat q = rnd_quat(&r);
        const u32 t = de_rng_range(&r, 0, 3);
        const u32 layer = 1u << de_rng_range(&r, 0, 3);
        const u32 group = de_rng_range(&r, 0, 8) == 0 ? 7u : 0u;
        const bool trigger = de_rng_range(&r, 0, 10) == 0;
        de_body_desc d =
            body_desc(t == 0 ? DE_BODY_STATIC : (t == 1 ? DE_BODY_KINEMATIC : DE_BODY_DYNAMIC),
                      s->shapes[k], cx + x, cy + y, cz + z);
        d.transform.rotation = q;
        d.filter.layer = layer;
        d.filter.mask = 0xFFFFFFFFu;
        d.filter.group = group;
        d.flags = trigger ? (u32)DE_BODY_FLAG_TRIGGER : 0u;
        d.owner.bits = 1000u + i;
        ASSERT_TRUE(DE_HANDLE_VALID(de_body_create(p, &d)));
    }
}

static int cmp_hit(const void *a, const void *b) {
    const de_ray_hit *x = (const de_ray_hit *)a, *y = (const de_ray_hit *)b;
    if (de__q_hit_less(x, y))
        return -1;
    return de__q_hit_less(y, x) ? 1 : 0;
}

/* Brute force: every body in index order, the per-body test, then sort by (fraction, index). */
static u32 brute_rays(const de_physics_world *p, const de__qray *qr, const de__qfilter *f,
                      de_ray_hit *out, u32 max) {
    u32 n = 0;
    for (u32 i = 0; i < p->bodies.high_water; ++i) {
        if (!de__query_accept(p, i, f))
            continue;
        de_ray_hit h;
        if (de__query_ray_body(p, i, qr, 1.0, f->flags, &h)) {
            ASSERT_LT(n, max);
            out[n++] = h;
        }
    }
    qsort(out, n, sizeof *out, cmp_hit);
    return n;
}

static void check_rays_vs_brute(de_physics_world *p, u64 seed, u32 count) {
    static de_ray_hit got[SCENE_BODIES], want[SCENE_BODIES];
    de_rng r = de_rng_seed(seed, 2);
    u32 total = 0;
    for (u32 n = 0; n < count; ++n) {
        const f32 ox = 50.0f * de_rng_f32(&r) - 25.0f;
        const f32 oy = 50.0f * de_rng_f32(&r) - 25.0f;
        const f32 oz = 14.0f * de_rng_f32(&r) - 2.0f;
        const f32 dx = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 dy = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 dz = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 len = 60.0f * de_rng_f32(&r) + 0.5f;
        de_physics_query_filter qf;
        memset(&qf, 0, sizeof qf);
        qf.filter.layer = 0xFFFFFFFFu;
        qf.filter.mask = (n % 3u == 0u) ? 0x5u : 0xFFFFFFFFu;
        qf.filter.group = (n % 5u == 0u) ? 7u : 0u;
        qf.flags = (n % 4u == 0u) ? (u32)DE_PHYSICS_QUERY_INCLUDE_TRIGGERS : 0u;
        qf.ignore_body = (n % 7u == 0u) ? de__body_handle(p, n % 50u) : (de_body){0};
        const de_dvec3 o = {ox, oy, oz};
        const de_vec3 dir = {dx, dy, dz};
        de__qray qr;
        if (!de__qray_init(&qr, o, dir, len))
            continue;
        de__qfilter f;
        de__qfilter_init(p, &qf, &f);
        const u32 nw = brute_rays(p, &qr, &f, want, SCENE_BODIES);
        const u32 ng = de_physics_raycast_all_ex(p, o, dir, len, &qf, got, SCENE_BODIES);
        ASSERT_EQ(ng, nw);
        for (u32 i = 0; i < nw; ++i)
            ASSERT_TRUE(hit_same(&got[i], &want[i]));
        de_ray_hit c;
        const bool hc = de_physics_raycast_ex(p, o, dir, len, &qf, &c);
        ASSERT_EQ(hc, nw > 0);
        if (hc)
            ASSERT_TRUE(hit_same(&c, &want[0]));
        /* A short output array keeps the first hits of the same order. */
        if (nw > 2) {
            const u32 n2 = de_physics_raycast_all_ex(p, o, dir, len, &qf, got, 2);
            ASSERT_EQ(n2, 2u);
            ASSERT_TRUE(hit_same(&got[0], &want[0]) && hit_same(&got[1], &want[1]));
        }
        total += nw;
    }
    EXPECT_GT(total, count / 8u); /* the rays really hit the scene */
}

TEST(physics_query_ray, raycast_all_matches_bruteforce) {
    de_physics_world *p = world_new(1024);
    static scene s;
    scene_shapes(p, &s, 3);
    scene_build(p, &s, SCENE_BODIES, 0.0, 0.0, 0.0, 21);
    /* No broadphase yet: every body in index order. */
    ASSERT_TRUE(de__broadphase_get(p) == NULL);
    check_rays_vs_brute(p, 100, 1500);
    /* Through the broadphase trees: the same results. */
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    check_rays_vs_brute(p, 100, 1500);
    check_rays_vs_brute(p, 101, 1500);
    de_physics_destroy(p);
}

TEST(physics_query_ray, filters_layers_groups_triggers_ignore) {
    de_physics_world *p = world_new(64);
    const de_shape s = sphere(p, 0.5f);
    de_body_desc d = body_desc(DE_BODY_STATIC, s, 2.0, 0.0, 0.0);
    d.filter = (de_collision_filter){2u, 0xFFFFFFFFu, 0u};
    const de_body a = de_body_create(p, &d); /* layer 2 */
    d = body_desc(DE_BODY_DYNAMIC, s, 4.0, 0.0, 0.0);
    d.filter = (de_collision_filter){1u, 0x1u, 5u};
    const de_body b = de_body_create(p, &d); /* layer 1, mask 1, group 5 */
    d = body_desc(DE_BODY_STATIC, s, 6.0, 0.0, 0.0);
    d.flags = DE_BODY_FLAG_TRIGGER;
    const de_body t = de_body_create(p, &d); /* trigger, default filter */
    d = body_desc(DE_BODY_STATIC, s, 8.0, 0.0, 0.0);
    const de_body e = de_body_create(p, &d); /* default filter */
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    const de_dvec3 o = {0, 0, 0};
    const de_vec3 x = {1, 0, 0};
    de_ray_hit h[8];
    /* All-zero filter = the body default {1, all}: a (layer 2) passes (default mask), b needs the
     * query's layer in its mask 1: passes; the trigger is skipped. */
    de_collision_filter zero = {0, 0, 0};
    u32 n = de_physics_raycast_all(p, o, x, 20.0f, zero, h, 8);
    ASSERT_EQ(n, 3u);
    EXPECT_EQ(h[0].body.id, a.id);
    EXPECT_EQ(h[1].body.id, b.id);
    EXPECT_EQ(h[2].body.id, e.id);
    /* Mask without layer 2 drops a; a query on layer 2 drops b (b's mask is 1). */
    n = de_physics_raycast_all(p, o, x, 20.0f, (de_collision_filter){1u, ~2u, 0u}, h, 8);
    ASSERT_EQ(n, 2u);
    EXPECT_EQ(h[0].body.id, b.id);
    n = de_physics_raycast_all(p, o, x, 20.0f, (de_collision_filter){2u, ~0u, 0u}, h, 8);
    ASSERT_EQ(n, 2u);
    EXPECT_EQ(h[0].body.id, a.id);
    EXPECT_EQ(h[1].body.id, e.id);
    /* Same non-zero group: excluded. */
    n = de_physics_raycast_all(p, o, x, 20.0f, (de_collision_filter){1u, ~0u, 5u}, h, 8);
    ASSERT_EQ(n, 2u);
    /* Triggers on request; the ignored body never. */
    de_physics_query_filter qf;
    memset(&qf, 0, sizeof qf);
    qf.flags = DE_PHYSICS_QUERY_INCLUDE_TRIGGERS;
    qf.ignore_body = a;
    n = de_physics_raycast_all_ex(p, o, x, 20.0f, &qf, h, 8);
    ASSERT_EQ(n, 3u);
    EXPECT_EQ(h[0].body.id, b.id);
    EXPECT_EQ(h[1].body.id, t.id);
    EXPECT_EQ(h[2].body.id, e.id);
    /* A stale ignore handle ignores nothing. */
    de_body_destroy(p, a);
    qf.ignore_body = a;
    n = de_physics_raycast_all_ex(p, o, x, 20.0f, &qf, h, 8);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(de_physics_raycast_all_ex(p, o, x, 20.0f, NULL, h, 8), 2u);
    de_physics_destroy(p);
}

TEST(physics_query_ray, invalid_arguments) {
    de_physics_world *p = world_new(64);
    add(p, DE_BODY_STATIC, sphere(p, 1.0f), 0.0, 0.0, 0.0);
    de_ray_hit h;
    const de_dvec3 o = {-5, 0, 0};
    EXPECT_FALSE(de_physics_raycast(NULL, o, (de_vec3){1, 0, 0}, 10.0f, all_filter(), &h));
    EXPECT_FALSE(de_physics_raycast(p, o, (de_vec3){1, 0, 0}, 10.0f, all_filter(), NULL));
    EXPECT_FALSE(de_physics_raycast(p, o, (de_vec3){0, 0, 0}, 10.0f, all_filter(), &h));
    EXPECT_FALSE(de_physics_raycast(p, o, (de_vec3){1, 0, 0}, 0.0f, all_filter(), &h));
    EXPECT_FALSE(de_physics_raycast(p, o, (de_vec3){1, 0, 0}, -1.0f, all_filter(), &h));
    EXPECT_EQ(de_physics_raycast_all(p, o, (de_vec3){1, 0, 0}, 10.0f, all_filter(), &h, 0), 0u);
    /* A non-unit direction is normalized: fraction is of max_dist. */
    ASSERT_TRUE(de_physics_raycast(p, o, (de_vec3){7, 0, 0}, 10.0f, all_filter(), &h));
    EXPECT_FLOAT_EQ(h.fraction, 0.4, 1e-6);
    /* A hit exactly at the end of the segment counts. */
    ASSERT_TRUE(de_physics_raycast(p, o, (de_vec3){1, 0, 0}, 4.0f, all_filter(), &h));
    EXPECT_FLOAT_EQ(h.fraction, 1.0f, 0.0);
    de_physics_destroy(p);
}

/* Tree queries see a teleported body at its new place once the broadphase follows it (the next
 * update, or de__broadphase_sync). */
TEST(physics_query_ray, teleport_then_sync) {
    de_physics_world *p = world_new(64);
    const de_body b = add(p, DE_BODY_KINEMATIC, sphere(p, 0.5f), 0.0, 0.0, 0.0);
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    de_transform t;
    ASSERT_TRUE(de_body_get_transform(p, b, &t));
    t.position = (de_dvec3){0.0, 30.0, 0.0};
    ASSERT_TRUE(de_body_set_transform(p, b, &t));
    de__broadphase_sync(p);
    de_ray_hit h;
    ASSERT_TRUE(ray(p, -5.0, 30.0, 0.0, 1, 0, 0, 10.0f, &h));
    EXPECT_EQ(h.body.id, b.id);
    EXPECT_FLOAT_EQ(h.point.x, -0.5, 1e-5);
    EXPECT_FALSE(ray(p, -5.0, 0.0, 0.0, 1, 0, 0, 10.0f, &h));
    de_physics_destroy(p);
}

/* ---- large worlds --------------------------------------------------------------------------- */

TEST(physics_query_ray, far_origin_matches_origin) {
    const f64 far = 100000.0;
    for (u32 pass = 0; pass < 2; ++pass) {
        de_physics_world *p = world_new(64);
        const de_quat q = axis_angle(0.0f, 0.6f, 0.8f, 0.7f);
        const f64 off = pass == 0 ? 0.0 : far;
        add_rot(p, DE_BODY_STATIC, box(p, 1.0f, 2.0f, 0.5f), off + 3.0, off - 1.0, 0.0, q);
        ASSERT_EQ(de__broadphase_update(p), DE_OK);
        static f32 fr[2][200];
        de_rng r = de_rng_seed(4, 4);
        for (u32 i = 0; i < 200; ++i) {
            const f32 y = 3.0f * de_rng_f32(&r) - 2.5f;
            const f32 z = 1.6f * de_rng_f32(&r) - 0.8f;
            de_ray_hit h;
            const bool hit = de_physics_raycast(p, (de_dvec3){off - 5.0, off + y, z},
                                                (de_vec3){1, 0.05f, 0}, 20.0f, all_filter(), &h);
            fr[pass][i] = hit ? h.fraction : -1.0f;
            if (pass == 1)
                EXPECT_FLOAT_EQ(fr[1][i], fr[0][i], 1e-4);
        }
        de_physics_destroy(p);
    }
}

/* ---- allocation ----------------------------------------------------------------------------- */

TEST(physics_query_ray, no_heap_allocation) {
    de_physics_world *p = world_new(1024);
    static scene s;
    scene_shapes(p, &s, 5);
    scene_build(p, &s, 200, 0.0, 0.0, 0.0, 8);
    de_shape hf, mesh;
    hf_and_mesh(p, &hf, &mesh);
    add(p, DE_BODY_STATIC, hf, -5.0, -5.0, -1.0);
    add(p, DE_BODY_STATIC, mesh, 3.0, 2.0, -1.0);
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    static de_ray_hit hits[64];
    de_ray_hit h;
    (void)ray(p, 0, 0, 20, 0, 0, -1, 30.0f, &h); /* warm-up */
    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    de_alloc_guard_begin(&g);
    de_rng r = de_rng_seed(1, 1);
    u32 total = 0;
    for (u32 i = 0; i < 300; ++i) {
        const f32 x = 40.0f * de_rng_f32(&r) - 20.0f;
        const f32 y = 40.0f * de_rng_f32(&r) - 20.0f;
        const f32 dx = 2.0f * de_rng_f32(&r) - 1.0f;
        total += ray(p, x, y, 15.0, dx, 0.3f, -1.0f, 40.0f, &h) ? 1u : 0u;
        total += de_physics_raycast_all(p, (de_dvec3){x, y, 15.0}, (de_vec3){dx, 0.3f, -1.0f},
                                        40.0f, all_filter(), hits, 64);
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_GT(total, 100u);
    de_physics_destroy(p);
}

/* ---- golden vector -------------------------------------------------------------------------- */

typedef struct hasher {
    u8 buf[256];
    u32 len;
} hasher;

static void put_u32(hasher *h, u32 v) {
    h->buf[h->len++] = (u8)v;
    h->buf[h->len++] = (u8)(v >> 8);
    h->buf[h->len++] = (u8)(v >> 16);
    h->buf[h->len++] = (u8)(v >> 24);
}
static void put_u64(hasher *h, u64 v) {
    put_u32(h, (u32)v);
    put_u32(h, (u32)(v >> 32));
}
static void put_f32(hasher *h, f32 v) {
    u32 u;
    memcpy(&u, &v, sizeof u);
    put_u32(h, u);
}
static void put_f64(hasher *h, f64 v) {
    u64 u;
    memcpy(&u, &v, sizeof u);
    put_u64(h, u);
}

/* Folds one query result (or a miss) into the running hash. */
static u64 hash_hit(u64 seed, u32 n, bool hit, const de_ray_hit *x) {
    hasher h;
    h.len = 0;
    put_u32(&h, n);
    put_u32(&h, hit ? 1u : 0u);
    if (hit) {
        put_u64(&h, x->entity.bits);
        put_u32(&h, x->body.id);
        put_f64(&h, x->point.x);
        put_f64(&h, x->point.y);
        put_f64(&h, x->point.z);
        put_f32(&h, x->normal.x);
        put_f32(&h, x->normal.y);
        put_f32(&h, x->normal.z);
        put_f32(&h, x->fraction);
        put_u32(&h, x->sub_shape);
    }
    return de_hash64(h.buf, h.len, seed);
}

typedef struct golden_entry {
    char name[32];
    u64 value;
} golden_entry;

/* The mixed scene of the vector: random convex bodies and compounds near the origin and 25 km
 * out, a bumpy mesh, and a heightfield with holes, through the broadphase. */
static de_physics_world *golden_scene(scene *s) {
    de_physics_world *p = world_new(1024);
    scene_shapes(p, s, 2026);
    scene_build(p, s, 300, 0.0, 0.0, 0.0, 2026);
    scene_build(p, s, 100, 25000.0, -25000.0, 0.0, 2027);
    de_shape hf, mesh;
    hf_and_mesh(p, &hf, &mesh);
    add_rot(p, DE_BODY_STATIC, hf, -12.0, -10.0, -1.5, axis_angle(0, 0, 1, 0.2f));
    add(p, DE_BODY_STATIC, grid_mesh(p, 17, 1.25f, 0.8f, 99), 2.0, -8.0, -1.0);
    add(p, DE_BODY_STATIC, grid_mesh(p, 9, 2.0f, 0.5f, 98), 24990.0, -25010.0, -1.0);
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    return p;
}

/* One query origin: near the origin, or (one in five) in the cluster 25 km out. */
static de_dvec3 rnd_origin(de_rng *r, u32 n) {
    const f32 x = 50.0f * de_rng_f32(r) - 25.0f;
    const f32 y = 50.0f * de_rng_f32(r) - 25.0f;
    const f32 z = 16.0f * de_rng_f32(r) - 3.0f;
    const f64 off = (n % 5u == 4u) ? 25000.0 : 0.0;
    de_dvec3 o = {off + x, -off + y, z};
    return o;
}

static u32 compute_golden(golden_entry *out) {
    static scene s;
    de_physics_world *p = golden_scene(&s);
    de_rng r = de_rng_seed(2026, 137);
    u64 rays = 0, rays_all = 0, casts = 0;
    u32 ray_hits = 0, all_hits = 0, cast_hits = 0;
    static de_ray_hit hits[16];
    for (u32 n = 0; n < 10000; ++n) {
        const de_dvec3 o = rnd_origin(&r, n);
        const f32 dx = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 dy = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 dz = 1.4f * de_rng_f32(&r) - 1.0f; /* mostly down */
        const f32 len = 0.5f + 50.0f * de_rng_f32(&r);
        const u32 mode = de_rng_range(&r, 0, 4);
        de_physics_query_filter qf;
        memset(&qf, 0, sizeof qf);
        qf.filter.layer = mode == 1 ? 2u : 1u;
        qf.filter.mask = mode == 2 ? 0x3u : 0xFFFFFFFFu;
        qf.flags =
            mode == 3
                ? (u32)(DE_PHYSICS_QUERY_INCLUDE_TRIGGERS | DE_PHYSICS_QUERY_IGNORE_BACK_FACES)
                : 0u;
        de_ray_hit h;
        const bool hit = de_physics_raycast_ex(p, o, (de_vec3){dx, dy, dz}, len, &qf, &h);
        ray_hits += hit ? 1u : 0u;
        rays = hash_hit(rays, n, hit, &h);
        if (n < 1000u) {
            const u32 k =
                de_physics_raycast_all_ex(p, o, (de_vec3){dx, dy, dz}, len, &qf, hits, 16);
            all_hits += k;
            for (u32 i = 0; i < k; ++i)
                rays_all = hash_hit(rays_all, n * 16u + i, true, &hits[i]);
        }
    }
    for (u32 n = 0; n < 1000; ++n) {
        const de_dvec3 o = rnd_origin(&r, n);
        const f32 dx = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 dy = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 dz = 1.4f * de_rng_f32(&r) - 1.0f;
        const f32 len = 0.5f + 20.0f * de_rng_f32(&r);
        const u32 k = de_rng_range(&r, 0, s.nshapes);
        de_transform from;
        memset(&from, 0, sizeof from);
        from.position = o;
        from.rotation = rnd_quat(&r);
        de_ray_hit h;
        const bool hit = de_physics_shape_cast(p, s.shapes[k], &from, (de_vec3){dx, dy, dz}, len,
                                               all_filter(), &h);
        cast_hits += hit ? 1u : 0u;
        casts = hash_hit(casts, n, hit, &h);
    }
    u32 m = 0;
    snprintf(out[m].name, sizeof out[m].name, "rays");
    out[m++].value = rays;
    snprintf(out[m].name, sizeof out[m].name, "ray_hits");
    out[m++].value = ray_hits;
    snprintf(out[m].name, sizeof out[m].name, "rays_all");
    out[m++].value = rays_all;
    snprintf(out[m].name, sizeof out[m].name, "rays_all_hits");
    out[m++].value = all_hits;
    snprintf(out[m].name, sizeof out[m].name, "casts");
    out[m++].value = casts;
    snprintf(out[m].name, sizeof out[m].name, "cast_hits");
    out[m++].value = cast_hits;
    printf("  10000 rays: %u hits; 1000 raycast_all: %u hits; 1000 casts: %u hits\n", ray_hits,
           all_hits, cast_hits);
    de_physics_destroy(p);
    return m;
}

TEST(physics_query_ray, golden_vector) {
    golden_entry got[8];
    const u32 n = compute_golden(got);
    EXPECT_GT(got[1].value, 1000u); /* the scene is really hit */
    EXPECT_GT(got[5].value, 200u);
    const char *regen = getenv("DE_REGEN_GOLDEN");
    if (regen != NULL && strcmp(regen, "1") == 0) {
        FILE *out = fopen(DE_PHYSICS_QUERY_GOLDEN, "wb");
        ASSERT_TRUE(out != NULL);
        fprintf(out, "# query_golden.txt: hashes (XXH3-64) of the hits of 10,000 de_rng rays\n"
                     "# (closest hit; raycast_all for the first 1,000) and 1,000 de_rng shape\n"
                     "# casts against a mixed scene (DENG-137), and the hit counts. Generated by\n"
                     "# test_physics_query_ray with DE_REGEN_GOLDEN=1; never edit by hand.\n"
                     "# x86-64 and arm64 must reproduce every line.\n");
        for (u32 i = 0; i < n; ++i)
            fprintf(out, "%s %016llx\n", got[i].name, (unsigned long long)got[i].value);
        fclose(out);
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_PHYSICS_QUERY_GOLDEN);
    }
    FILE *in = fopen(DE_PHYSICS_QUERY_GOLDEN, "rb");
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
