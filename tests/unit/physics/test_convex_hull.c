/* test_convex_hull.c: the deterministic quickhull and the shape golden vector (DENG-132).
 *
 * physics_hull.golden_vector compares hashes of 20 hulls from fixed de_rng point sets and of the
 * mass properties (and other built data) of every shape kind against
 * tests/unit/physics/vectors/shapes_golden.txt. That file is a cross-platform contract: x86-64
 * and arm64 must reproduce it bit for bit. Only a deliberate change of the algorithms rewrites
 * it (never by hand, and never to make one platform pass):
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics/test_physics_hull
 * --filter=physics_hull.golden_vector */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, fopen */
#endif
#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/physics_internal.h"
#include "physics/shapes/shape_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_PHYSICS_SHAPES_GOLDEN
#error "DE_PHYSICS_SHAPES_GOLDEN (path of shapes_golden.txt) must be defined by the build"
#endif

/* ---- helpers ---------------------------------------------------------------------------- */

typedef struct built_hull {
    de__hull h;
    void *block;
    size_t size;
} built_hull;

static de_result build(const de_vec3 *pts, u32 n, f32 tol, built_hull *out) {
    if (tol == 0.0f)
        tol = de__hull_auto_tolerance(pts, n);
    return de__hull_build(de_heap_allocator(), pts, n, tol, &out->h, &out->block, &out->size);
}

static void release(built_hull *b) {
    if (b->block != NULL)
        DE_FREE(de_heap_allocator(), b->block, b->size);
    b->block = NULL;
}

/* Structure and convexity checks. `all_inside`: every input point must be inside too (false
 * when the vertex limit truncated the hull). */
static void check_hull(const de__hull *h, const de_vec3 *pts, u32 n, bool all_inside) {
    ASSERT_GE(h->vertex_count, 4u);
    ASSERT_LE(h->vertex_count, DE_SHAPE_MAX_HULL_VERTICES);
    ASSERT_GE(h->face_count, 4u);
    u32 edges = 0;
    for (u32 f = 0; f < h->face_count; ++f) {
        const de__hull_face *fa = &h->faces[f];
        ASSERT_GE(fa->edge_count, 3u);
        ASSERT_EQ((u32)fa->first_edge, edges); /* contiguous, in face order */
        for (u32 k = 0; k < fa->edge_count; ++k) {
            const u32 e = fa->first_edge + k;
            const de__hull_edge *ed = &h->edges[e];
            EXPECT_EQ((u32)ed->face, f);
            EXPECT_EQ((u32)ed->next, fa->first_edge + (k + 1) % fa->edge_count);
            ASSERT_LT((u32)ed->twin, h->edge_count);
            const de__hull_edge *tw = &h->edges[ed->twin];
            EXPECT_EQ((u32)tw->twin, e);
            EXPECT_EQ((u32)tw->origin, (u32)h->edges[ed->next].origin);
            EXPECT_NE((u32)tw->face, f);
            ASSERT_LT((u32)ed->origin, h->vertex_count);
        }
        edges += fa->edge_count;
    }
    ASSERT_EQ(edges, h->edge_count);
    ASSERT_EQ(h->edge_count % 2u, 0u);
    /* Euler: V - E + F = 2 (E counts undirected edges). */
    EXPECT_EQ((i64)h->vertex_count - (i64)(h->edge_count / 2u) + (i64)h->face_count, 2);

    const f64 tol = (f64)h->tolerance;
    for (u32 f = 0; f < h->face_count; ++f) {
        const de_plane pl = h->planes[f];
        const f64 nl =
            de_dsqrt64((f64)pl.n.x * pl.n.x + (f64)pl.n.y * pl.n.y + (f64)pl.n.z * pl.n.z);
        EXPECT_FLOAT_EQ(nl, 1.0, 1e-6);
        /* Every vertex behind the plane within tolerance; the face's own vertices on it. */
        for (u32 v = 0; v < h->vertex_count; ++v) {
            const de_vec3 q = h->vertices[v];
            const f64 dist = (f64)pl.n.x * q.x + (f64)pl.n.y * q.y + (f64)pl.n.z * q.z + pl.d;
            EXPECT_TRUE(dist <= tol);
        }
        const de__hull_face *fa = &h->faces[f];
        for (u32 k = 0; k < fa->edge_count; ++k) {
            const de_vec3 q = h->vertices[h->edges[fa->first_edge + k].origin];
            const f64 dist = (f64)pl.n.x * q.x + (f64)pl.n.y * q.y + (f64)pl.n.z * q.z + pl.d;
            EXPECT_TRUE(dist >= -2.0 * tol);
        }
        if (all_inside) {
            for (u32 i = 0; i < n; ++i) {
                const f64 dist =
                    (f64)pl.n.x * pts[i].x + (f64)pl.n.y * pts[i].y + (f64)pl.n.z * pts[i].z + pl.d;
                EXPECT_TRUE(dist <= tol);
            }
        }
    }
    /* Vertices are input points, in ascending input order. */
    u32 j = 0;
    for (u32 v = 0; v < h->vertex_count; ++v) {
        while (j < n && memcmp(&pts[j], &h->vertices[v], sizeof(de_vec3)) != 0)
            ++j;
        EXPECT_LT(j, n);
        ++j;
    }
}

static f32 rnd(de_rng *r, f32 lo, f32 hi) {
    return lo + (hi - lo) * de_rng_f32(r);
}

/* The 20 point sets of the golden vector (and of random_clouds): size and distribution vary. */
static u32 point_set(u32 i, de_vec3 *out) {
    static const u32 sizes[20] = {8,   12,  20,  33,  50,  64,  100, 128,  200,  256,
                                  300, 400, 500, 600, 700, 800, 900, 1000, 1500, 2000};
    de_rng r = de_rng_seed(1000u + i, 132u);
    const u32 n = sizes[i];
    for (u32 k = 0; k < n; ++k) {
        de_vec3 p;
        switch (i % 4u) {
        case 0: /* box */
            p = (de_vec3){rnd(&r, -1, 1), rnd(&r, -2, 2), rnd(&r, -0.5f, 0.5f)};
            break;
        case 1: /* sphere surface */
            p = de_dnormalize_v3((de_vec3){rnd(&r, -1, 1), rnd(&r, -1, 1), rnd(&r, -1, 1)});
            break;
        case 2: /* lattice: many coplanar and colinear points */
            p = (de_vec3){(f32)de_rng_range(&r, 0, 4), (f32)de_rng_range(&r, 0, 3),
                          (f32)de_rng_range(&r, 0, 5)};
            break;
        default: /* flat-ish slab, off the origin */
            p = (de_vec3){10.0f + rnd(&r, -1, 1), -5.0f + rnd(&r, -1, 1), rnd(&r, -0.05f, 0.05f)};
            break;
        }
        out[k] = p;
    }
    return n;
}

/* ---- structure -------------------------------------------------------------------------- */

TEST(physics_hull, cube) {
    const de_vec3 c[8] = {{-1, -1, -1}, {1, -1, -1}, {-1, 1, -1}, {1, 1, -1},
                          {-1, -1, 1},  {1, -1, 1},  {-1, 1, 1},  {1, 1, 1}};
    built_hull b;
    ASSERT_EQ(build(c, 8, 0.0f, &b), DE_OK);
    EXPECT_EQ(b.h.vertex_count, 8u);
    EXPECT_EQ(b.h.face_count, 6u);
    EXPECT_EQ(b.h.edge_count, 24u);
    for (u32 f = 0; f < 6; ++f) {
        EXPECT_EQ((u32)b.h.faces[f].edge_count, 4u);
        const de_vec3 n = b.h.planes[f].n;
        /* Axis-aligned unit normals, planes at distance 1. */
        EXPECT_FLOAT_EQ(de__absf(n.x) + de__absf(n.y) + de__absf(n.z), 1.0f, 1e-6f);
        EXPECT_FLOAT_EQ(b.h.planes[f].d, -1.0f, 1e-6f);
    }
    EXPECT_TRUE(b.h.centroid.x == 0.0f && b.h.centroid.z == 0.0f);
    check_hull(&b.h, c, 8, true);
    release(&b);
}

TEST(physics_hull, coplanar_duplicates_interior) {
    /* Cube corners, plus face centers, edge midpoints, interior points, exact duplicates and
     * near-duplicates (1e-7 off): still a cube. */
    de_vec3 pts[128];
    u32 n = 0;
    de_rng r = de_rng_seed(2, 2);
    for (u32 k = 0; k < 3; ++k) /* the first points are interior, so they come first */
        pts[n++] = (de_vec3){rnd(&r, -0.9f, 0.9f), rnd(&r, -0.9f, 0.9f), rnd(&r, -0.9f, 0.9f)};
    for (u32 c = 0; c < 27; ++c) { /* the 3x3x3 lattice: corners, edge mids, face centers */
        pts[n++] = (de_vec3){(f32)(c % 3) - 1.0f, (f32)((c / 3) % 3) - 1.0f, (f32)(c / 9) - 1.0f};
    }
    for (u32 c = 0; c < 8; ++c) {
        const de_vec3 corner = {(c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f,
                                (c & 4) ? 1.0f : -1.0f};
        pts[n++] = corner;
        pts[n++] = (de_vec3){corner.x * (1.0f - 1e-7f), corner.y, corner.z};
    }
    for (u32 k = 0; k < 20; ++k)
        pts[n++] = (de_vec3){rnd(&r, -1, 1), rnd(&r, -1, 1), (k & 1) ? 1.0f : -1.0f};
    built_hull b;
    ASSERT_EQ(build(pts, n, 0.0f, &b), DE_OK);
    EXPECT_EQ(b.h.vertex_count, 8u);
    EXPECT_EQ(b.h.face_count, 6u);
    check_hull(&b.h, pts, n, true);
    release(&b);

    /* A 5x5x5 lattice is a cube too. */
    for (u32 c = 0; c < 125; ++c)
        pts[c] = (de_vec3){(f32)(c % 5), (f32)((c / 5) % 5), (f32)(c / 25)};
    ASSERT_EQ(build(pts, 125, 0.0f, &b), DE_OK);
    EXPECT_EQ(b.h.vertex_count, 8u);
    EXPECT_EQ(b.h.face_count, 6u);
    check_hull(&b.h, pts, 125, true);
    release(&b);

    /* Duplicated tetrahedron corners: a tetrahedron. */
    const de_vec3 tet[8] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                            {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    ASSERT_EQ(build(tet, 8, 0.0f, &b), DE_OK);
    EXPECT_EQ(b.h.vertex_count, 4u);
    EXPECT_EQ(b.h.face_count, 4u);
    check_hull(&b.h, tet, 8, true);
    release(&b);
}

TEST(physics_hull, random_clouds) {
    static de_vec3 pts[2000];
    for (u32 i = 0; i < 20; ++i) {
        const u32 n = point_set(i, pts);
        built_hull b;
        ASSERT_EQ(build(pts, n, 0.0f, &b), DE_OK);
        const bool truncated = b.h.vertex_count == DE_SHAPE_MAX_HULL_VERTICES;
        check_hull(&b.h, pts, n, !truncated);
        if (i % 4u == 2u) { /* lattice [0,3] x [0,2] x [0,4]: with this many draws, a box */
            if (n >= 600) {
                EXPECT_EQ(b.h.vertex_count, 8u);
                EXPECT_EQ(b.h.face_count, 6u);
            }
        }
        release(&b);
    }
}

TEST(physics_hull, degenerate_inputs) {
    built_hull b;
    const de_vec3 three[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    EXPECT_EQ(build(three, 3, 0.0f, &b), DE_ERR_INVALID_ARGUMENT);
    de_vec3 same[10];
    for (u32 i = 0; i < 10; ++i)
        same[i] = (de_vec3){1, 2, 3};
    EXPECT_EQ(build(same, 10, 0.0f, &b), DE_ERR_INVALID_ARGUMENT);
    de_vec3 line[10];
    for (u32 i = 0; i < 10; ++i)
        line[i] = (de_vec3){(f32)i, 2.0f * (f32)i, -1.0f * (f32)i};
    EXPECT_EQ(build(line, 10, 0.0f, &b), DE_ERR_INVALID_ARGUMENT);
    de_vec3 plane[25];
    for (u32 i = 0; i < 25; ++i)
        plane[i] = (de_vec3){(f32)(i % 5), (f32)(i / 5), (f32)(i % 5) + (f32)(i / 5)};
    EXPECT_EQ(build(plane, 25, 0.0f, &b), DE_ERR_INVALID_ARGUMENT);
    /* Within tolerance of a plane is a plane. */
    plane[12].z += 1e-6f;
    EXPECT_EQ(build(plane, 25, 0.0f, &b), DE_ERR_INVALID_ARGUMENT);
    /* A point well off the plane makes it a pyramid. */
    plane[12].z += 1.0f;
    ASSERT_EQ(build(plane, 25, 0.0f, &b), DE_OK);
    EXPECT_EQ(b.h.vertex_count, 5u);
    EXPECT_EQ(b.h.face_count, 5u);
    check_hull(&b.h, plane, 25, true);
    release(&b);
    /* Non-finite input and a bad tolerance. */
    de_vec3 bad[4] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    EXPECT_EQ(de__hull_build(de_heap_allocator(), bad, 4, -1.0f, &b.h, &b.block, &b.size),
              DE_ERR_INVALID_ARGUMENT);
    bad[2].y = INFINITY;
    EXPECT_EQ(build(bad, 4, 1e-4f, &b), DE_ERR_INVALID_ARGUMENT);

    /* Through the public API: the same refusals. */
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CONVEX;
    d.points = line;
    d.point_count = 10;
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape_create(p, &d)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    d.points = plane;
    d.point_count = 25;
    d.hull_tolerance = -1.0f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape_create(p, &d)));
    d.hull_tolerance = 0.5f; /* coarse: the apex at 1.0 still counts */
    EXPECT_TRUE(DE_HANDLE_VALID(de_shape_create(p, &d)));
    d.hull_tolerance = 2.0f; /* coarser than the apex height: flat */
    EXPECT_FALSE(DE_HANDLE_VALID(de_shape_create(p, &d)));
    de_physics_destroy(p);
}

TEST(physics_hull, deterministic_bytes) {
    static de_vec3 pts[2000], copy[2000];
    for (u32 i = 0; i < 20; i += 3) {
        const u32 n = point_set(i, pts);
        memcpy(copy, pts, sizeof(de_vec3) * n);
        built_hull a, b;
        ASSERT_EQ(build(pts, n, 0.0f, &a), DE_OK);
        ASSERT_EQ(build(copy, n, 0.0f, &b), DE_OK);
        ASSERT_EQ(a.size, b.size);
        EXPECT_MEMEQ(a.block, b.block, a.size);
        EXPECT_EQ(a.h.vertex_count, b.h.vertex_count);
        release(&a);
        release(&b);
    }
}

TEST(physics_hull, vertex_limit) {
    static de_vec3 pts[3000];
    de_rng r = de_rng_seed(77, 1);
    for (u32 i = 0; i < 3000; ++i)
        pts[i] = de_dnormalize_v3((de_vec3){rnd(&r, -1, 1), rnd(&r, -1, 1), rnd(&r, -1, 1)});
    built_hull b;
    ASSERT_EQ(build(pts, 3000, 0.0f, &b), DE_OK);
    EXPECT_LE(b.h.vertex_count, DE_SHAPE_MAX_HULL_VERTICES);
    EXPECT_GE(b.h.vertex_count, 200u);
    check_hull(&b.h, pts, 3000, false);
    /* The farthest points went in first: the truncated hull still reaches most of the sphere. */
    for (u32 f = 0; f < b.h.face_count; ++f)
        EXPECT_TRUE(b.h.planes[f].d < -0.9f);
    release(&b);
}

TEST(physics_hull, no_leak_and_oom) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    static de_vec3 pts[500];
    const u32 n = point_set(12, pts);
    bool done = false;
    for (i64 k = 0; k < 2000 && !done; ++k) {
        de_tracking_allocator_fail_after(&t, k);
        de__hull h;
        void *block;
        size_t size;
        const de_result r = de__hull_build(&a, pts, n, 1e-4f, &h, &block, &size);
        de_tracking_allocator_fail_after(&t, -1);
        if (r == DE_OK) {
            done = true;
            DE_FREE(&a, block, size);
        } else {
            EXPECT_EQ(r, DE_ERR_OUT_OF_MEMORY);
            EXPECT_TRUE(block == NULL);
        }
        EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
    }
    EXPECT_TRUE(done);
}

/* ---- golden vector ---------------------------------------------------------------------- */

typedef struct hbuf {
    u64 h;
} hbuf;

static void hb_bytes(hbuf *b, const void *data, size_t n) {
    b->h = de_hash64(data, n, b->h);
}
static void hb_u32(hbuf *b, u32 v) {
    u8 x[4] = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)};
    hb_bytes(b, x, 4);
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
static void hb_aabb(hbuf *b, de_aabb a) {
    hb_v3(b, a.min);
    hb_v3(b, a.max);
}

static u64 hash_hull(const de__hull *h) {
    hbuf b = {0};
    hb_u32(&b, h->vertex_count);
    hb_u32(&b, h->face_count);
    hb_u32(&b, h->edge_count);
    for (u32 i = 0; i < h->vertex_count; ++i)
        hb_v3(&b, h->vertices[i]);
    for (u32 i = 0; i < h->face_count; ++i) {
        hb_v3(&b, h->planes[i].n);
        hb_f32(&b, h->planes[i].d);
        hb_u32(&b, h->faces[i].first_edge);
        hb_u32(&b, h->faces[i].edge_count);
    }
    for (u32 i = 0; i < h->edge_count; ++i) {
        hb_u32(&b, h->edges[i].next);
        hb_u32(&b, h->edges[i].twin);
        hb_u32(&b, h->edges[i].origin);
        hb_u32(&b, h->edges[i].face);
    }
    hb_v3(&b, h->centroid);
    hb_f32(&b, h->tolerance);
    return b.h;
}

static u64 hash_bvh(const de__bvh *bvh) {
    hbuf b = {0};
    hb_u32(&b, bvh->node_count);
    hb_u32(&b, bvh->prim_count);
    hb_u32(&b, bvh->depth);
    for (u32 i = 0; i < bvh->node_count; ++i) {
        hb_aabb(&b, bvh->nodes[i].bounds);
        hb_u32(&b, bvh->nodes[i].first);
        hb_u32(&b, bvh->nodes[i].count);
    }
    for (u32 i = 0; i < bvh->prim_count; ++i)
        hb_u32(&b, bvh->prims[i]);
    return b.h;
}

/* Mass properties at density 750, local box and bounding radius of a shape. */
static u64 hash_shape(const de_physics_world *p, de_shape s) {
    de_mass_props m;
    ASSERT_EQ(de_shape_get_mass_props(p, s, 750.0f, &m), DE_OK);
    const de__shape *sh = de__shape_get(p, s);
    hbuf b = {0};
    hb_f32(&b, m.mass);
    hb_v3(&b, m.center);
    for (u32 i = 0; i < 9; ++i)
        hb_f32(&b, m.inertia.m[i]);
    hb_aabb(&b, sh->local_aabb);
    hb_f32(&b, sh->bounding_radius);
    hb_f32(&b, sh->margin);
    return b.h;
}

#define GOLDEN_MAX 64
typedef struct golden_entry {
    char name[32];
    u64 value;
} golden_entry;

static u32 compute_golden(golden_entry *out) {
    u32 n = 0;
    static de_vec3 pts[2000];
    for (u32 i = 0; i < 20; ++i) {
        const u32 np = point_set(i, pts);
        built_hull b;
        ASSERT_EQ(build(pts, np, 0.0f, &b), DE_OK);
        snprintf(out[n].name, sizeof out[n].name, "hull%02u", i);
        out[n++].value = hash_hull(&b.h);
        release(&b);
    }

    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    de_shape_desc d;
    de_shape s[8];
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_SPHERE;
    d.radius = 0.5f;
    s[0] = de_shape_create(p, &d);
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CAPSULE;
    d.radius = 0.3f;
    d.half_height = 0.7f;
    s[1] = de_shape_create(p, &d);
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = (de_vec3){0.5f, 1.0f, 1.5f};
    s[2] = de_shape_create(p, &d);
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CYLINDER;
    d.radius = 0.4f;
    d.half_height = 0.6f;
    s[3] = de_shape_create(p, &d);
    const u32 np = point_set(5, pts);
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CONVEX;
    d.points = pts;
    d.point_count = np;
    s[4] = de_shape_create(p, &d);
    /* Mesh: a bumpy 16 x 16 grid. */
    static de_vec3 mv[17 * 17];
    static u32 mi[16 * 16 * 6];
    for (u32 y = 0; y <= 16; ++y)
        for (u32 x = 0; x <= 16; ++x)
            mv[y * 17 + x] = (de_vec3){(f32)x * 0.5f, (f32)y * 0.5f,
                                       0.3f * de_dsin((f32)x * 0.9f) * de_dcos((f32)y * 0.4f)};
    u32 k = 0;
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            const u32 a = y * 17 + x;
            const u32 q[6] = {a, a + 1, a + 18, a, a + 18, a + 17};
            for (u32 j = 0; j < 6; ++j)
                mi[k++] = q[j];
        }
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_MESH;
    d.vertices = mv;
    d.vertex_count = 17 * 17;
    d.indices = mi;
    d.index_count = k;
    s[5] = de_shape_create(p, &d);
    /* Heightfield 13 x 11 with a hole. */
    static f32 hts[13 * 11];
    de_rng r = de_rng_seed(9, 9);
    for (u32 i = 0; i < 13 * 11; ++i)
        hts[i] = de_rng_f32(&r) * 4.0f - 1.0f;
    hts[40] = DE_HEIGHTFIELD_HOLE;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_HEIGHTFIELD;
    d.heights = hts;
    d.rows = 13;
    d.cols = 11;
    d.scale = (de_vec3){0.75f, 0.5f, 2.0f};
    s[6] = de_shape_create(p, &d);
    /* Compound of the primitives and the hull at fixed transforms. */
    de_transform xf[5];
    for (u32 i = 0; i < 5; ++i) {
        xf[i].position = (de_dvec3){(f64)i * 0.75 - 1.0, 0.25 * (f64)i, -0.5};
        xf[i].rotation = de_dnormalize_quat((de_quat){0.2f * (f32)i, 0.1f, -0.3f, 1.0f});
        xf[i].scale = (de_vec3){1, 1, 1};
    }
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_COMPOUND;
    d.children = s;
    d.child_xforms = xf;
    d.child_count = 5;
    s[7] = de_shape_create(p, &d);
    static const char *const names[8] = {"sphere", "capsule", "box",         "cylinder",
                                         "convex", "mesh",    "heightfield", "compound"};
    for (u32 i = 0; i < 8; ++i) {
        ASSERT_TRUE(DE_HANDLE_VALID(s[i]));
        snprintf(out[n].name, sizeof out[n].name, "mass_%s", names[i]);
        out[n++].value = hash_shape(p, s[i]);
    }
    const de__shape *mesh = de__shape_get(p, s[5]);
    snprintf(out[n].name, sizeof out[n].name, "mesh_bvh");
    out[n++].value = hash_bvh(&mesh->u.mesh.bvh);
    {
        hbuf b = {0};
        hb_bytes(&b, mesh->u.mesh.adjacent, sizeof(u32) * 3u * mesh->u.mesh.tri_count);
        hb_bytes(&b, mesh->u.mesh.edge_flags, mesh->u.mesh.tri_count);
        snprintf(out[n].name, sizeof out[n].name, "mesh_edges");
        out[n++].value = b.h;
    }
    {
        const de__heightfield *hf = &de__shape_get(p, s[6])->u.hf;
        hbuf b = {0};
        for (u32 i = 0; i < hf->blocks_x * hf->blocks_y; ++i) {
            hb_f32(&b, hf->block_min[i]);
            hb_f32(&b, hf->block_max[i]);
        }
        snprintf(out[n].name, sizeof out[n].name, "heightfield_blocks");
        out[n++].value = b.h;
    }
    snprintf(out[n].name, sizeof out[n].name, "compound_bvh");
    out[n++].value = hash_bvh(&de__shape_get(p, s[7])->u.compound.bvh);
    de_physics_destroy(p);
    return n;
}

TEST(physics_hull, golden_vector) {
    static golden_entry got[GOLDEN_MAX];
    const u32 n = compute_golden(got);
    const char *regen = getenv("DE_REGEN_GOLDEN");
    if (regen != NULL && strcmp(regen, "1") == 0) {
        FILE *out = fopen(DE_PHYSICS_SHAPES_GOLDEN, "wb");
        ASSERT_TRUE(out != NULL);
        fprintf(out, "# shapes_golden.txt: hashes (XXH3-64) of 20 convex hulls from fixed de_rng\n"
                     "# point sets and of the mass properties and built data of every shape\n"
                     "# kind (DENG-132). Generated by test_physics_hull with DE_REGEN_GOLDEN=1;\n"
                     "# never edit by hand. x86-64 and arm64 must reproduce every line.\n");
        for (u32 i = 0; i < n; ++i)
            fprintf(out, "%s %016llx\n", got[i].name, (unsigned long long)got[i].value);
        fclose(out);
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_PHYSICS_SHAPES_GOLDEN);
    }
    FILE *in = fopen(DE_PHYSICS_SHAPES_GOLDEN, "rb");
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
