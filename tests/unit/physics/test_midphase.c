/* test_midphase.c: meshes, heightfields and compounds against convex shapes: internal-edge
 * (ghost contact) suppression, one-sided triangles, holes, triangle and child order (DENG-134,
 * SPEC §13.1). */
#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/narrow/narrowphase.h"
#include "physics/physics_internal.h"
#include "physics/shapes/shape_internal.h"

#include <string.h>

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
static const de__shape *mesh(de_physics_world *p, const de_vec3 *v, u32 nv, const u32 *idx,
                             u32 ni) {
    de_shape_desc d = desc_of(DE_SHAPE_MESH);
    d.vertices = v;
    d.vertex_count = nv;
    d.indices = idx;
    d.index_count = ni;
    return mk(p, &d);
}
/* Flat n x n grid of `cell` meters centered on the origin, z = 0, two triangles per cell. */
static const de__shape *grid(de_physics_world *p, u32 n, f32 cell) {
    static de_vec3 v[65 * 65];
    static u32 idx[64 * 64 * 6];
    const f32 o = -0.5f * cell * (f32)n;
    for (u32 y = 0; y <= n; ++y)
        for (u32 x = 0; x <= n; ++x)
            v[y * (n + 1) + x] = (de_vec3){o + cell * (f32)x, o + cell * (f32)y, 0.0f};
    u32 k = 0;
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            const u32 a = y * (n + 1) + x;
            const u32 q[6] = {a, a + 1, a + n + 2, a, a + n + 2, a + n + 1};
            for (u32 j = 0; j < 6; ++j)
                idx[k++] = q[j];
        }
    return mesh(p, v, (n + 1) * (n + 1), idx, k);
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
static u32 collide(const de__shape *a, de__pose xa, const de__shape *b, de__pose xb, f32 spec,
                   de__contact_set *sets) {
    de__contact_buffer buf = {sets, CAP, 0, false};
    return de__collide(a, xa, b, xb, spec, &buf);
}

/* Every set's normal is within ~0.8 degrees of `n`. */
static bool all_normals(const de__contact_set *s, u32 count, de_vec3 n) {
    for (u32 i = 0; i < count; ++i)
        if (de__v3_dot(s[i].normal, n) < 0.9999f)
            return false;
    return count > 0;
}

/* ---- internal edges ---------------------------------------------------------------------- */

TEST(physics_midphase, mesh_no_ghost_normals) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *g = grid(p, 8, 1.0f);
    const de__shape *movers[6] = {box(p, 0.5f, 0.5f, 0.5f), box_hull(p, 0.5f, 0.3f, 0.4f),
                                  sphere(p, 0.3f),          capsule(p, 0.2f, 0.6f),
                                  cylinder(p, 0.4f, 0.3f),  box(p, 0.35f, 1.2f, 0.25f)};
    const f32 zs[6] = {0.5f, 0.4f, 0.3f, 0.2f, 0.3f, 0.25f};
    const de_quat lie = qaxis(0, 1, 0, 1.5707963f);
    u32 sets_seen = 0;
    for (u32 m = 0; m < 6; ++m) {
        for (u32 step = 0; step <= 40; ++step) {
            /* Slide across cell edges (x = -1, 0, 1) and diagonals. */
            const f64 x = -1.0 + 0.05 * (f64)step, y = 0.37 - 0.013 * (f64)step;
            for (u32 r = 0; r < 4; ++r) {
                de_quat q = qaxis(0, 0, 1, 0.3926991f * (f32)r);
                if (m == 3)
                    q = qmul(q, lie); /* the capsule lies down */
                const f64 z = (f64)zs[m] - 0.005;
                de__contact_set s[CAP];
                const u32 n =
                    collide(g, pose(0, 0, 0, qid()), movers[m], pose(x, y, z, q), 0.01f, s);
                ASSERT_TRUE(all_normals(s, n, (de_vec3){0, 0, 1}));
                sets_seen += n;
                /* The other order: the normal points down. */
                const u32 n2 =
                    collide(movers[m], pose(x, y, z, q), g, pose(0, 0, 0, qid()), 0.01f, s);
                ASSERT_EQ(n2, n);
                ASSERT_TRUE(all_normals(s, n2, (de_vec3){0, 0, -1}));
            }
        }
    }
    EXPECT_GT(sets_seen, 1000u);
    de_physics_destroy(p);
}

TEST(physics_midphase, mesh_tilted_box_slides_without_ghosts) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *g = grid(p, 8, 1.0f), *b = box(p, 0.5f, 0.5f, 0.5f);
    /* Tilted by 0.5 degree about Y, so one bottom edge leads; deep enough that the lowest edge
     * sinks 1 cm. */
    const de_quat q = qaxis(0, 1, 0, 0.0087266f);
    for (u32 step = 0; step <= 100; ++step) {
        const f64 x = -1.5 + 0.03 * (f64)step;
        de__contact_set s[CAP];
        const u32 n = collide(g, pose(0, 0, 0, qid()), b, pose(x, 0.2, 0.494, q), 0.0f, s);
        ASSERT_TRUE(all_normals(s, n, (de_vec3){0, 0, 1}));
    }
    de_physics_destroy(p);
}

TEST(physics_midphase, convex_ridge_keeps_its_edge_normal) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    /* A roof: two 45-degree slopes meeting at a ridge along Y (x = 0, z = 1). */
    static const de_vec3 v[6] = {{-1, -1, 0}, {0, -1, 1}, {1, -1, 0},
                                 {-1, 1, 0},  {0, 1, 1},  {1, 1, 0}};
    static const u32 idx[12] = {0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4};
    const de__shape *roof = mesh(p, v, 6, idx, 12);
    const de__shape *s = sphere(p, 0.3f);
    de__contact_set o[CAP];
    const u32 n = collide(roof, pose(0, 0, 0, qid()), s, pose(0, 0.2, 1.25, qid()), 0.0f, o);
    ASSERT_GT(n, 0u);
    for (u32 i = 0; i < n; ++i) {
        EXPECT_TRUE(o[i].normal.z > 0.999f); /* the ridge's own normal, not a slope's */
        EXPECT_EQ(de__feat_type(de__feature_a(o[i].points[0].feature_id)), DE__FEAT_EDGE);
        EXPECT_FLOAT_EQ(o[i].points[0].separation, -0.05, 1e-5);
    }
    /* In a valley (the roof upside down): each slope pushes along its own face normal. */
    static const de_vec3 vv[6] = {{-1, -1, 1}, {0, -1, 0}, {1, -1, 1},
                                  {-1, 1, 1},  {0, 1, 0},  {1, 1, 1}};
    const de__shape *valley = mesh(p, vv, 6, idx, 12);
    const u32 m = collide(valley, pose(0, 0, 0, qid()), s, pose(0, 0.2, 0.4, qid()), 0.0f, o);
    ASSERT_EQ(m, 2u);
    const f32 k = 0.70710678f;
    for (u32 i = 0; i < 2; ++i) {
        EXPECT_FLOAT_EQ(o[i].normal.z, k, 1e-5);
        EXPECT_FLOAT_EQ(de__absf(o[i].normal.x), k, 1e-5);
        EXPECT_EQ(de__feature_a(o[i].points[0].feature_id), de__feat(DE__FEAT_FACE, 0));
    }
    de_physics_destroy(p);
}

TEST(physics_midphase, one_sided_triangles) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *g = grid(p, 2, 1.0f), *s = sphere(p, 0.3f);
    de__contact_set o[CAP];
    EXPECT_GT(collide(g, pose(0, 0, 0, qid()), s, pose(0.2, 0.3, 0.25, qid()), 0.0f, o), 0u);
    /* Center just behind the surface: no contact (and no push to the wrong side). */
    EXPECT_EQ(collide(g, pose(0, 0, 0, qid()), s, pose(0.2, 0.3, -0.05, qid()), 0.0f, o), 0u);
    /* A flipped mesh (rotated 180 degrees about X) faces down. */
    EXPECT_EQ(collide(g, pose(0, 0, 0, qaxis(1, 0, 0, 3.1415927f)), s, pose(0.2, 0.3, 0.25, qid()),
                      0.0f, o),
              0u);
    de_physics_destroy(p);
}

/* ---- order ------------------------------------------------------------------------------- */

TEST(physics_midphase, triangles_in_index_order_across_chunks) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *g = grid(p, 40, 0.25f); /* 3200 triangles */
    const de__shape *b = box(p, 3.6f, 3.6f, 0.5f);
    static de__contact_set s[4096];
    de__contact_buffer buf = {s, 4096, 0, false};
    const u32 n =
        de__collide(g, pose(0, 0, 0, qid()), b, pose(0.05, -0.07, 0.49, qid()), 0.0f, &buf);
    EXPECT_FALSE(buf.overflow);
    EXPECT_GT(n, 600u); /* well past one 256-triangle chunk */
    for (u32 i = 0; i < n; ++i) {
        EXPECT_EQ(s[i].sub_b, DE__SUB_NONE);
        EXPECT_NE(s[i].sub_a, DE__SUB_NONE);
        if (i > 0)
            EXPECT_LT(s[i - 1].sub_a, s[i].sub_a);
    }
    ASSERT_TRUE(all_normals(s, n, (de_vec3){0, 0, 1}));
    de_physics_destroy(p);
}

/* ---- heightfield ------------------------------------------------------------------------- */

static const de__shape *flat_hf(de_physics_world *p, f32 *h, u32 n, i32 hole_col, i32 hole_row) {
    for (u32 i = 0; i < n * n; ++i)
        h[i] = 0.0f;
    if (hole_col >= 0)
        h[(u32)hole_row * n + (u32)hole_col] = DE_HEIGHTFIELD_HOLE;
    de_shape_desc d = desc_of(DE_SHAPE_HEIGHTFIELD);
    d.heights = h;
    d.rows = n;
    d.cols = n;
    d.scale = (de_vec3){1.0f, 1.0f, 1.0f};
    return mk(p, &d);
}

TEST(physics_midphase, heightfield_sub_ids_and_holes) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    static f32 h[20 * 20];
    const de__shape *hf = flat_hf(p, h, 20, 5, 5);
    const de__shape *s = sphere(p, 0.1f);
    de__contact_set o[CAP];
    /* Cell (2, 3), above the diagonal: triangle 1. */
    ASSERT_EQ(collide(hf, pose(0, 0, 0, qid()), s, pose(2.3, 3.6, 0.09, qid()), 0.0f, o), 1u);
    EXPECT_EQ(o[0].sub_a, 2u * (3u * 19u + 2u) + 1u);
    EXPECT_FLOAT_EQ(o[0].points[0].separation, -0.01, 1e-6);
    EXPECT_FLOAT_EQ(o[0].normal.z, 1.0, 1e-6);
    /* Below the diagonal: triangle 0. */
    ASSERT_EQ(collide(hf, pose(0, 0, 0, qid()), s, pose(2.6, 3.3, 0.09, qid()), 0.0f, o), 1u);
    EXPECT_EQ(o[0].sub_a, 2u * (3u * 19u + 2u));
    /* Over the hole: the four cells around sample (5, 5) have no surface. */
    EXPECT_EQ(collide(hf, pose(0, 0, 0, qid()), s, pose(5.0, 5.0, 0.05, qid()), 0.0f, o), 0u);
    EXPECT_EQ(collide(hf, pose(0, 0, 0, qid()), s, pose(4.5, 5.5, 0.05, qid()), 0.0f, o), 0u);
    /* Far above the blocks' heights: nothing. */
    EXPECT_EQ(collide(hf, pose(0, 0, 0, qid()), s, pose(8.5, 8.5, 3.0, qid()), 0.0f, o), 0u);
    /* Outside the heightfield. */
    EXPECT_EQ(collide(hf, pose(0, 0, 0, qid()), s, pose(-1.0, 3.0, 0.0, qid()), 0.0f, o), 0u);
    de_physics_destroy(p);
}

TEST(physics_midphase, heightfield_no_ghost_normals) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    static f32 h[12 * 12];
    const de__shape *hf = flat_hf(p, h, 12, -1, -1);
    const de__shape *movers[3] = {box(p, 0.5f, 0.5f, 0.5f), capsule(p, 0.2f, 0.7f),
                                  sphere(p, 0.3f)};
    const f32 zs[3] = {0.5f, 0.2f, 0.3f};
    for (u32 m = 0; m < 3; ++m)
        for (u32 step = 0; step <= 60; ++step) {
            const f64 x = 2.0 + 0.05 * (f64)step, y = 3.1 + 0.02 * (f64)step;
            const de_quat q = m == 1 ? qaxis(0, 1, 0, 1.5707963f) : qaxis(0, 0, 1, 0.2f * (f32)m);
            de__contact_set o[CAP];
            const u32 n = collide(hf, pose(0, 0, 0, qid()), movers[m],
                                  pose(x, y, (f64)zs[m] - 0.004, q), 0.0f, o);
            ASSERT_TRUE(all_normals(o, n, (de_vec3){0, 0, 1}));
        }
    de_physics_destroy(p);
}

TEST(physics_midphase, heightfield_edge_flags) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    static f32 h[8 * 8];
    const de__shape *hf = flat_hf(p, h, 8, 6, 6);
    const de__heightfield *f = &hf->u.hf;
    /* Flat interior: every edge inactive. */
    EXPECT_EQ(de__np_hf_edge_flags(f, 3, 3, 0), 0u);
    EXPECT_EQ(de__np_hf_edge_flags(f, 3, 3, 1), 0u);
    /* Boundary edges are active: bottom (row 0) of triangle 0, left (col 0) of triangle 1. */
    EXPECT_EQ(de__np_hf_edge_flags(f, 0, 0, 0), (u8)DE__MESH_EDGE0_ACTIVE);
    EXPECT_EQ(de__np_hf_edge_flags(f, 0, 0, 1), (u8)DE__MESH_EDGE2_ACTIVE);
    /* Last column / row. */
    EXPECT_EQ(de__np_hf_edge_flags(f, 6, 2, 0) & DE__MESH_EDGE1_ACTIVE, (u32)DE__MESH_EDGE1_ACTIVE);
    EXPECT_EQ(de__np_hf_edge_flags(f, 2, 6, 1) & DE__MESH_EDGE1_ACTIVE, (u32)DE__MESH_EDGE1_ACTIVE);
    /* Next to the hole cells around (6, 6): cell (4, 5)'s right edge faces cell (5, 5), a hole. */
    EXPECT_EQ(de__np_hf_edge_flags(f, 4, 5, 0) & DE__MESH_EDGE1_ACTIVE, (u32)DE__MESH_EDGE1_ACTIVE);
    /* A ridge along row 3 (raised samples): the edges along it are convex, so active. */
    for (u32 c = 0; c < 8; ++c)
        h[3 * 8 + c] = 0.5f;
    de_shape_desc d = desc_of(DE_SHAPE_HEIGHTFIELD);
    d.heights = h;
    d.rows = 8;
    d.cols = 8;
    d.scale = (de_vec3){1, 1, 1};
    const de__shape *ridge = mk(p, &d);
    /* Cell (2, 2) triangle 1's top edge runs along row 3. */
    EXPECT_EQ(de__np_hf_edge_flags(&ridge->u.hf, 2, 2, 1) & DE__MESH_EDGE1_ACTIVE,
              (u32)DE__MESH_EDGE1_ACTIVE);
    de_physics_destroy(p);
}

/* ---- compounds --------------------------------------------------------------------------- */

TEST(physics_midphase, compound_children) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *b = box(p, 0.4f, 0.4f, 0.4f);
    const de_shape ch[2] = {b->handle, b->handle};
    de_transform xf[2];
    xf[0].position = (de_dvec3){-1.0, 0.0, 0.0};
    xf[1].position = (de_dvec3){1.0, 0.0, 0.0};
    xf[0].rotation = xf[1].rotation = qid();
    xf[0].scale = xf[1].scale = (de_vec3){1, 1, 1};
    de_shape_desc d = desc_of(DE_SHAPE_COMPOUND);
    d.children = ch;
    d.child_xforms = xf;
    d.child_count = 2;
    const de__shape *c = mk(p, &d);
    const de__shape *s = sphere(p, 0.2f), *big = sphere(p, 1.2f);
    de__contact_set o[CAP];
    /* On child 1 only; the anchor is in the compound's frame. */
    ASSERT_EQ(collide(c, pose(0, 0, 0, qid()), s, pose(1.1, 0, 0.59, qid()), 0.0f, o), 1u);
    EXPECT_EQ(o[0].child_a, 1u);
    EXPECT_EQ(o[0].child_b, DE__SUB_NONE);
    EXPECT_FLOAT_EQ(o[0].points[0].local_a.x, 1.1, 1e-6);
    EXPECT_FLOAT_EQ(o[0].points[0].local_a.z, 0.4, 1e-6);
    EXPECT_FLOAT_EQ(o[0].points[0].separation, -0.01, 1e-6);
    /* Swapped order: the child index moves to B. */
    ASSERT_EQ(collide(s, pose(1.1, 0, 0.59, qid()), c, pose(0, 0, 0, qid()), 0.0f, o), 1u);
    EXPECT_EQ(o[0].child_b, 1u);
    EXPECT_EQ(o[0].child_a, DE__SUB_NONE);
    /* A big sphere over the middle touches both, child 0 first. */
    ASSERT_EQ(collide(c, pose(0, 0, 0, qid()), big, pose(0, 0, 1.4, qid()), 0.0f, o), 2u);
    EXPECT_EQ(o[0].child_a, 0u);
    EXPECT_EQ(o[1].child_a, 1u);
    /* Compound on compound: A's children outer, B's inner. */
    ASSERT_EQ(collide(c, pose(0, 0, 0, qid()), c, pose(0, 0, 0.79, qid()), 0.0f, o), 2u);
    EXPECT_EQ(o[0].child_a, 0u);
    EXPECT_EQ(o[0].child_b, 0u);
    EXPECT_EQ(o[1].child_a, 1u);
    EXPECT_EQ(o[1].child_b, 1u);
    EXPECT_EQ(o[0].point_count, 4u);
    /* Compound against a mesh: triangle ids on A, child ids on B. */
    const de__shape *g = grid(p, 4, 1.0f);
    const u32 n = collide(g, pose(0, 0, 0, qid()), c, pose(0.1, 0.1, 0.39, qid()), 0.0f, o);
    ASSERT_GT(n, 1u);
    for (u32 i = 0; i < n; ++i) {
        EXPECT_NE(o[i].sub_a, DE__SUB_NONE);
        EXPECT_TRUE(o[i].child_b == 0u || o[i].child_b == 1u);
        EXPECT_TRUE(o[i].normal.z > 0.9999f);
    }
    de_physics_destroy(p);
}

TEST(physics_midphase, static_compound_with_mesh_child) {
    de_physics_world *p;
    ASSERT_EQ(de_physics_create(NULL, &p), DE_OK);
    const de__shape *g = grid(p, 2, 1.0f), *b = box(p, 0.5f, 0.5f, 0.5f);
    const de_shape ch[2] = {g->handle, b->handle};
    de_transform xf[2];
    xf[0].position = (de_dvec3){0.0, 0.0, 0.0};
    xf[1].position = (de_dvec3){3.0, 0.0, 0.5};
    xf[0].rotation = xf[1].rotation = qid();
    xf[0].scale = xf[1].scale = (de_vec3){1, 1, 1};
    de_shape_desc d = desc_of(DE_SHAPE_COMPOUND);
    d.children = ch;
    d.child_xforms = xf;
    d.child_count = 2;
    const de__shape *c = mk(p, &d);
    const de__shape *s = sphere(p, 0.25f);
    de__contact_set o[CAP];
    ASSERT_EQ(collide(c, pose(10, 0, 0, qid()), s, pose(10.3, 0.2, 0.24, qid()), 0.0f, o), 1u);
    EXPECT_EQ(o[0].child_a, 0u);
    EXPECT_NE(o[0].sub_a, DE__SUB_NONE);
    EXPECT_FLOAT_EQ(o[0].normal.z, 1.0, 1e-6);
    ASSERT_EQ(collide(c, pose(10, 0, 0, qid()), s, pose(13.0, 0.0, 1.24, qid()), 0.0f, o), 1u);
    EXPECT_EQ(o[0].child_a, 1u);
    EXPECT_EQ(o[0].sub_a, DE__SUB_NONE);
    de_physics_destroy(p);
}
