/* test_query_cast_overlap.c: shape casts, overlaps and closest points (DENG-137, SPEC §13.1):
 * analytic times of impact, overlap sets and closest points against brute force, filters, no
 * allocation, and a 4-thread smoke test (run under TSan in CI: label tsan). */
#include "de_alloc_guard.h"
#include "de_test.h"

#include <dengine/de_physics.h>
#include <dengine/de_platform.h>

#include "physics/broadphase/broadphase.h"
#include "physics/query/query_internal.h"

#include <string.h>

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

static de_shape flat_mesh(de_physics_world *p, f32 half) {
    static de_vec3 v[4];
    static u32 ix[6] = {0, 1, 3, 0, 3, 2};
    v[0] = (de_vec3){-half, -half, 0};
    v[1] = (de_vec3){half, -half, 0};
    v[2] = (de_vec3){-half, half, 0};
    v[3] = (de_vec3){half, half, 0};
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_MESH;
    d.vertices = v;
    d.vertex_count = 4;
    d.indices = ix;
    d.index_count = 6;
    return shape_new(p, &d);
}

static de_shape flat_heightfield(de_physics_world *p, u32 n, f32 height) {
    static f32 h[16 * 16];
    ASSERT_LE(n, 16u);
    for (u32 i = 0; i < n * n; ++i)
        h[i] = height;
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_HEIGHTFIELD;
    d.heights = h;
    d.rows = n;
    d.cols = n;
    d.scale = (de_vec3){1, 1, 1};
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

static de_transform at(f64 x, f64 y, f64 z) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position = (de_dvec3){x, y, z};
    t.rotation = (de_quat){0, 0, 0, 1};
    t.scale = (de_vec3){1, 1, 1};
    return t;
}

static de_quat axis_angle(f32 x, f32 y, f32 z, f32 angle) {
    const f32 s = de_dsin(angle * 0.5f);
    const f32 c = de_dcos(angle * 0.5f);
    de_quat q = {x * s, y * s, z * s, c};
    return q;
}

static bool cast(const de_physics_world *p, de_shape s, de_transform from, f32 dx, f32 dy, f32 dz,
                 f32 dist, de_ray_hit *h) {
    return de_physics_shape_cast(p, s, &from, (de_vec3){dx, dy, dz}, dist, all_filter(), h);
}

/* Cast tolerance in distance: conservative advancement stops within about 1e-5 m. */
#define TOI_EPS 3e-5

/* ---- shape casts: analytic times of impact ---------------------------------------------- */

TEST(physics_query_cast, sphere_vs_sphere_toi) {
    de_physics_world *p = world_new(64);
    const de_body b = add(p, DE_BODY_STATIC, sphere(p, 1.0f), 0.0, 0.0, 0.0);
    const de_shape s = sphere(p, 0.5f);
    de_ray_hit h;
    ASSERT_TRUE(cast(p, s, at(-5.0, 0.0, 0.0), 1, 0, 0, 10.0f, &h));
    EXPECT_EQ(h.body.id, b.id);
    EXPECT_FLOAT_EQ(h.fraction * 10.0, 3.5, TOI_EPS);
    EXPECT_FLOAT_EQ(h.normal.x, -1.0, 1e-5);
    EXPECT_FLOAT_EQ(h.point.x, -1.0, TOI_EPS);
    /* Off-center: distance of centers 1.5 at contact. */
    ASSERT_TRUE(cast(p, s, at(-5.0, 0.9, 0.0), 1, 0, 0, 10.0f, &h));
    const f64 x = -de_dsqrt64(1.5 * 1.5 - 0.81);
    EXPECT_FLOAT_EQ(h.fraction * 10.0, x + 5.0, TOI_EPS);
    EXPECT_FLOAT_EQ(h.normal.x, x / 1.5, 1e-4);
    EXPECT_FLOAT_EQ(h.normal.y, 0.9 / 1.5, 1e-4);
    /* Passing by, too short, moving away. */
    EXPECT_FALSE(cast(p, s, at(-5.0, 1.6, 0.0), 1, 0, 0, 10.0f, &h));
    EXPECT_FALSE(cast(p, s, at(-5.0, 0.0, 0.0), 1, 0, 0, 3.4f, &h));
    EXPECT_FALSE(cast(p, s, at(-5.0, 0.0, 0.0), -1, 0, 0, 10.0f, &h));
    de_physics_destroy(p);
}

TEST(physics_query_cast, capsule_onto_box) {
    de_physics_world *p = world_new(64);
    add(p, DE_BODY_STATIC, box(p, 2.0f, 2.0f, 1.0f), 0.0, 0.0, 0.0);
    const de_shape c = capsule(p, 0.3f, 0.5f);
    de_ray_hit h;
    /* Upright, dropping onto the top face: the bottom of the capsule is at z - 0.8. */
    ASSERT_TRUE(cast(p, c, at(0.5, 0.3, 5.0), 0, 0, -1, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0, 5.0 - 0.8 - 1.0, TOI_EPS);
    EXPECT_FLOAT_EQ(h.normal.z, 1.0, 1e-5);
    EXPECT_FLOAT_EQ(h.point.z, 1.0, TOI_EPS);
    /* Lying along X, sliding sideways into the +X face. */
    de_transform t = at(5.0, 0.0, 0.0);
    t.rotation = axis_angle(0, 1, 0, 1.5707963f);
    ASSERT_TRUE(de_physics_shape_cast(p, c, &t, (de_vec3){-1, 0, 0}, 10.0f, all_filter(), &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0, 5.0 - 0.8 - 2.0, TOI_EPS);
    EXPECT_FLOAT_EQ(h.normal.x, 1.0, 1e-5);
    de_physics_destroy(p);
}

TEST(physics_query_cast, box_face_and_corner) {
    de_physics_world *p = world_new(64);
    add(p, DE_BODY_STATIC, box(p, 1.0f, 1.0f, 1.0f), 0.0, 0.0, 0.0);
    const de_shape b = box(p, 0.5f, 0.5f, 0.5f);
    de_ray_hit h;
    ASSERT_TRUE(cast(p, b, at(-4.0, 0.2, 0.1), 1, 0, 0, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0, 4.0 - 1.5, TOI_EPS);
    EXPECT_FLOAT_EQ(h.normal.x, -1.0, 1e-5);
    /* Rotated 45 degrees about Z: the leading edge is sqrt(0.5) from the center. */
    de_transform t = at(-4.0, 0.2, 0.1);
    t.rotation = axis_angle(0, 0, 1, 0.785398163f);
    ASSERT_TRUE(de_physics_shape_cast(p, b, &t, (de_vec3){1, 0, 0}, 10.0f, all_filter(), &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0, 4.0 - 1.0 - de_dsqrt64(0.5), TOI_EPS);
    EXPECT_FLOAT_EQ(h.normal.x, -1.0, 1e-4);
    /* A cylinder (GJK on a curved shape without radius) against the face. */
    ASSERT_TRUE(cast(p, cylinder(p, 0.4f, 0.3f), at(0.0, -5.0, 0.0), 0, 1, 0, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0, 5.0 - 1.0 - 0.4, 1e-4);
    de_physics_destroy(p);
}

TEST(physics_query_cast, terrain_and_compounds) {
    de_physics_world *p = world_new(64);
    add(p, DE_BODY_STATIC, flat_mesh(p, 5.0f), 0.0, 0.0, 0.0);
    add(p, DE_BODY_STATIC, flat_heightfield(p, 8, 0.25f), 20.0, 0.0, 0.0);
    const de_shape s = sphere(p, 0.5f);
    de_ray_hit h;
    ASSERT_TRUE(cast(p, s, at(1.0, 1.0, 3.0), 0, 0, -1, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0, 2.5, TOI_EPS);
    EXPECT_FLOAT_EQ(h.normal.z, 1.0, 1e-5);
    EXPECT_LT(h.sub_shape, 2u);
    ASSERT_TRUE(cast(p, s, at(23.5, 2.5, 3.0), 0, 0, -1, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction * 10.0, 3.0 - 0.5 - 0.25, TOI_EPS);
    EXPECT_FLOAT_EQ(h.point.z, 0.25, TOI_EPS);
    EXPECT_LT(h.sub_shape, 2u * 7u * 7u);
    /* From below: two-sided by default, passes with DE_PHYSICS_QUERY_IGNORE_BACK_FACES. */
    ASSERT_TRUE(cast(p, s, at(1.0, 1.0, -3.0), 0, 0, 1, 10.0f, &h));
    EXPECT_FLOAT_EQ(h.normal.z, -1.0, 1e-5);
    de_physics_query_filter qf;
    memset(&qf, 0, sizeof qf);
    qf.flags = DE_PHYSICS_QUERY_IGNORE_BACK_FACES;
    const de_transform below = at(1.0, 1.0, -3.0);
    EXPECT_FALSE(de_physics_shape_cast_ex(p, s, &below, (de_vec3){0, 0, 1}, 10.0f, &qf, &h));
    /* A compound cast shape onto a compound target: sub_shape is the target child. */
    de_shape kids[2] = {box(p, 0.5f, 0.5f, 0.5f), sphere(p, 0.5f)};
    de_transform kx[2];
    kx[0] = at(0.0, 0.0, 0.0);
    kx[1] = at(3.0, 0.0, 1.0);
    de_shape_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.kind = DE_SHAPE_COMPOUND;
    cd.children = kids;
    cd.child_xforms = kx;
    cd.child_count = 2;
    const de_shape comp = shape_new(p, &cd);
    add(p, DE_BODY_STATIC, comp, 40.0, 0.0, 0.0);
    ASSERT_TRUE(cast(p, s, at(43.0, 0.0, 6.0), 0, 0, -1, 10.0f, &h));
    EXPECT_EQ(h.sub_shape, 1u);
    EXPECT_FLOAT_EQ(h.fraction * 10.0, 6.0 - 2.0, TOI_EPS);
    ASSERT_TRUE(cast(p, comp, at(40.0, 0.0, 6.0), 0, 0, -1, 10.0f, &h));
    EXPECT_EQ(h.sub_shape, 0u);
    /* The cast's sphere child (1 m higher) meets the target sphere child first. */
    EXPECT_FLOAT_EQ(h.fraction * 10.0, 6.0 - 1.0, TOI_EPS);
    de_physics_destroy(p);
}

TEST(physics_query_cast, start_overlap_and_touching) {
    de_physics_world *p = world_new(64);
    add(p, DE_BODY_STATIC, box(p, 1.0f, 1.0f, 1.0f), 0.0, 0.0, 0.0);
    const de_shape s = sphere(p, 0.5f);
    de_ray_hit h;
    ASSERT_TRUE(cast(p, s, at(1.2, 0.0, 0.0), 0.6f, 0.8f, 0, 5.0f, &h));
    EXPECT_FLOAT_EQ(h.fraction, 0.0f, 0.0);
    EXPECT_FLOAT_EQ(h.normal.x, -0.6, 1e-6);
    EXPECT_FLOAT_EQ(h.normal.y, -0.8, 1e-6);
    EXPECT_FLOAT_EQ(h.point.x, 1.2, 0.0);
    /* Resting on the top face (2 mm gap) and moving away or sideways: no hit. */
    EXPECT_FALSE(cast(p, s, at(0.0, 0.0, 1.502), 0, 0, 1, 5.0f, &h));
    EXPECT_FALSE(cast(p, s, at(0.0, 0.0, 1.502), 1, 0, 0, 0.3f, &h));
    de_physics_destroy(p);
}

/* ---- random scenes: brute force -------------------------------------------------------------- */

typedef struct scene {
    de_shape shapes[8];
    u32 nshapes;
} scene;

static de_quat rnd_quat(de_rng *r) {
    const f32 x = 2.0f * de_rng_f32(r) - 1.0f;
    const f32 y = 2.0f * de_rng_f32(r) - 1.0f;
    const f32 z = 2.0f * de_rng_f32(r) - 1.0f;
    const f32 w = 2.0f * de_rng_f32(r) - 1.0f;
    de_quat q = {x, y, z, w};
    return q;
}

static void scene_build(de_physics_world *p, scene *s, u32 count, f32 extent, u64 seed) {
    u32 n = 0;
    s->shapes[n++] = sphere(p, 0.6f);
    s->shapes[n++] = capsule(p, 0.4f, 0.7f);
    s->shapes[n++] = box(p, 0.8f, 0.5f, 0.3f);
    s->shapes[n++] = cylinder(p, 0.5f, 0.6f);
    de_rng r = de_rng_seed(seed, 7);
    de_vec3 pts[12];
    for (u32 i = 0; i < 12; ++i) {
        const f32 x = 1.4f * de_rng_f32(&r) - 0.7f;
        const f32 y = 1.4f * de_rng_f32(&r) - 0.7f;
        const f32 z = 1.4f * de_rng_f32(&r) - 0.7f;
        pts[i] = (de_vec3){x, y, z};
    }
    de_shape_desc hd;
    memset(&hd, 0, sizeof hd);
    hd.kind = DE_SHAPE_CONVEX;
    hd.points = pts;
    hd.point_count = 12;
    s->shapes[n++] = shape_new(p, &hd);
    de_shape kids[2] = {s->shapes[0], s->shapes[2]};
    de_transform kx[2];
    kx[0] = at(0.0, 0.0, 0.0);
    kx[1] = at(0.0, 0.0, 1.0);
    de_shape_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.kind = DE_SHAPE_COMPOUND;
    cd.children = kids;
    cd.child_xforms = kx;
    cd.child_count = 2;
    s->shapes[n++] = shape_new(p, &cd);
    s->nshapes = n;
    for (u32 i = 0; i < count; ++i) {
        const u32 k = de_rng_range(&r, 0, n);
        const f32 x = 2.0f * extent * de_rng_f32(&r) - extent;
        const f32 y = 2.0f * extent * de_rng_f32(&r) - extent;
        const f32 z = 4.0f * de_rng_f32(&r);
        const de_quat q = rnd_quat(&r);
        const u32 layer = 1u << de_rng_range(&r, 0, 3);
        const bool trigger = de_rng_range(&r, 0, 8) == 0;
        de_body_desc d = body_desc(DE_BODY_STATIC, s->shapes[k], x, y, z);
        d.transform.rotation = q;
        d.filter.layer = layer;
        d.filter.mask = 0xFFFFFFFFu;
        d.flags = trigger ? (u32)DE_BODY_FLAG_TRIGGER : 0u;
        d.owner.bits = 500u + i;
        ASSERT_TRUE(DE_HANDLE_VALID(de_body_create(p, &d)));
    }
    /* A floor mesh and a heightfield. */
    add(p, DE_BODY_STATIC, flat_mesh(p, extent), 0.0, 0.0, -0.5);
    add(p, DE_BODY_STATIC, flat_heightfield(p, 12, 0.0f), -extent, -extent, -1.0);
}

static void query_filter(u32 n, de_physics_query_filter *qf, const de_physics_world *p) {
    memset(qf, 0, sizeof *qf);
    qf->filter.layer = 0xFFFFFFFFu;
    qf->filter.mask = (n % 3u == 0u) ? 0x3u : 0xFFFFFFFFu;
    qf->flags = (n % 4u == 0u) ? (u32)DE_PHYSICS_QUERY_INCLUDE_TRIGGERS : 0u;
    qf->ignore_body = (n % 5u == 0u) ? de__body_handle(p, n % 40u) : (de_body){0};
}

static void check_casts(de_physics_world *p, const scene *s, u64 seed, u32 count) {
    de_rng r = de_rng_seed(seed, 3);
    u32 hits = 0;
    for (u32 n = 0; n < count; ++n) {
        const f32 x = 24.0f * de_rng_f32(&r) - 12.0f;
        const f32 y = 24.0f * de_rng_f32(&r) - 12.0f;
        const f32 z = 6.0f * de_rng_f32(&r) - 1.0f;
        const f32 dx = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 dy = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 dz = 2.0f * de_rng_f32(&r) - 1.0f;
        const f32 len = 0.5f + 12.0f * de_rng_f32(&r);
        const u32 k = de_rng_range(&r, 0, s->nshapes);
        de_transform from = at(x, y, z);
        from.rotation = rnd_quat(&r);
        de_physics_query_filter qf;
        query_filter(n, &qf, p);
        /* Brute force: every accepted body, the per-body cast, min (fraction, index). */
        de__qcast c;
        ASSERT_TRUE(
            de__qcast_init(&c, de__shape_get(p, s->shapes[k]), &from, (de_vec3){dx, dy, dz}, len));
        de__qfilter f;
        de__qfilter_init(p, &qf, &f);
        bool want_hit = false;
        de_ray_hit want;
        memset(&want, 0, sizeof want);
        for (u32 i = 0; i < p->bodies.high_water; ++i) {
            de_ray_hit h;
            if (de__query_accept(p, i, &f) && de__query_cast_body(p, i, &c, 1.0, f.flags, &h) &&
                (!want_hit || de__q_hit_less(&h, &want))) {
                want_hit = true;
                want = h;
            }
        }
        de_ray_hit got;
        const bool hit =
            de_physics_shape_cast_ex(p, s->shapes[k], &from, (de_vec3){dx, dy, dz}, len, &qf, &got);
        ASSERT_EQ(hit, want_hit);
        if (hit) {
            ASSERT_TRUE(hit_same(&got, &want));
            ++hits;
        }
    }
    EXPECT_GT(hits, count / 4u);
}

TEST(physics_query_cast, casts_match_bruteforce) {
    de_physics_world *p = world_new(512);
    static scene s;
    scene_build(p, &s, 250, 12.0f, 31);
    check_casts(p, &s, 5, 400); /* no broadphase: every body */
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    check_casts(p, &s, 5, 400);
    check_casts(p, &s, 6, 400);
    de_physics_destroy(p);
}

static void check_overlaps(de_physics_world *p, const scene *s, u64 seed, u32 count) {
    static u32 want[600];
    static de_body bodies[600];
    static de_entity ents[600];
    de_rng r = de_rng_seed(seed, 4);
    u32 total = 0;
    for (u32 n = 0; n < count; ++n) {
        const f32 x = 24.0f * de_rng_f32(&r) - 12.0f;
        const f32 y = 24.0f * de_rng_f32(&r) - 12.0f;
        const f32 z = 5.0f * de_rng_f32(&r) - 1.0f;
        const u32 k = de_rng_range(&r, 0, s->nshapes);
        de_transform t = at(x, y, z);
        t.rotation = rnd_quat(&r);
        de_physics_query_filter qf;
        query_filter(n, &qf, p);
        de__qshape q;
        ASSERT_TRUE(de__qshape_init(&q, de__shape_get(p, s->shapes[k]), &t));
        de__qfilter f;
        de__qfilter_init(p, &qf, &f);
        u32 nw = 0;
        for (u32 i = 0; i < p->bodies.high_water; ++i)
            if (de__query_accept(p, i, &f) && de__query_overlap_body(p, i, &q))
                want[nw++] = i;
        const u32 ng = de_physics_overlap_ex(p, s->shapes[k], &t, &qf, bodies, ents, 600);
        ASSERT_EQ(ng, nw);
        for (u32 i = 0; i < nw; ++i) {
            ASSERT_EQ(de__handle_index(bodies[i].id), want[i]);
            ASSERT_EQ(ents[i].bits, p->bodies.owner[want[i]].bits);
        }
        /* A short array keeps the lowest indices. */
        if (nw > 1) {
            ASSERT_EQ(de_physics_overlap_ex(p, s->shapes[k], &t, &qf, bodies, NULL, 1), 1u);
            ASSERT_EQ(de__handle_index(bodies[0].id), want[0]);
        }
        total += nw;
    }
    EXPECT_GT(total, count / 2u);
}

TEST(physics_query_cast, overlaps_match_bruteforce) {
    de_physics_world *p = world_new(512);
    static scene s;
    scene_build(p, &s, 250, 12.0f, 32);
    check_overlaps(p, &s, 9, 400);
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    check_overlaps(p, &s, 9, 400);
    de_physics_destroy(p);
}

/* More results than one collection pass holds (64): several passes, still ascending. */
TEST(physics_query_cast, overlap_many_results) {
    de_physics_world *p = world_new(512);
    const de_shape s = sphere(p, 0.4f);
    for (u32 i = 0; i < 300; ++i) {
        const f64 x = (f64)(i % 20u) * 0.5, y = (f64)(i / 20u) * 0.5;
        add(p, DE_BODY_STATIC, s, x, y, 0.0);
    }
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    const de_shape big = box(p, 20.0f, 20.0f, 1.0f);
    static de_body out[400];
    const de_transform t = at(5.0, 5.0, 0.0);
    const u32 n = de_physics_overlap_ex(p, big, &t, NULL, out, NULL, 400);
    ASSERT_EQ(n, 300u);
    for (u32 i = 0; i < n; ++i)
        ASSERT_EQ(de__handle_index(out[i].id), i);
    EXPECT_EQ(de_physics_overlap_ex(p, big, &t, NULL, out, NULL, 150), 150u);
    EXPECT_EQ(de__handle_index(out[149].id), 149u);
    /* The plain API reports owner entities. */
    static de_entity ents[8];
    EXPECT_EQ(de_physics_overlap(p, big, &t, all_filter(), ents, 8), 8u);
    /* Exact analytic overlap of spheres: centers 0.8 apart touch (0.4 + 0.4). */
    const de_transform near = at(-0.79, 0.0, 0.0), far = at(-0.81, 0.0, 0.0);
    EXPECT_EQ(de_physics_overlap_ex(p, s, &near, NULL, out, NULL, 8), 1u);
    EXPECT_EQ(de_physics_overlap_ex(p, s, &far, NULL, out, NULL, 8), 0u);
    /* Unsupported query shapes. */
    EXPECT_EQ(de_physics_overlap_ex(p, flat_mesh(p, 1.0f), &t, NULL, out, NULL, 8), 0u);
    de_physics_destroy(p);
}

/* ---- closest point ------------------------------------------------------------------------- */

TEST(physics_query_cast, closest_point_analytic) {
    de_physics_world *p = world_new(64);
    const de_body b = add(p, DE_BODY_STATIC, box(p, 1.0f, 1.0f, 1.0f), 0.0, 0.0, 0.0);
    de_ray_hit h;
    /* Face, edge, corner regions. */
    ASSERT_TRUE(de_physics_closest_point(p, (de_dvec3){3.0, 0.5, 0.2}, 5.0f, all_filter(), &h));
    EXPECT_EQ(h.body.id, b.id);
    EXPECT_FLOAT_EQ(h.fraction * 5.0, 2.0, 1e-5);
    EXPECT_FLOAT_EQ(h.point.x, 1.0, 1e-5);
    EXPECT_FLOAT_EQ(h.point.y, 0.5, 1e-5);
    EXPECT_FLOAT_EQ(h.normal.x, 1.0, 1e-5);
    ASSERT_TRUE(de_physics_closest_point(p, (de_dvec3){2.0, 2.0, 0.0}, 5.0f, all_filter(), &h));
    EXPECT_FLOAT_EQ(h.fraction * 5.0, de_dsqrt64(2.0), 1e-5);
    ASSERT_TRUE(de_physics_closest_point(p, (de_dvec3){2.0, -3.0, 4.0}, 5.0f, all_filter(), &h));
    EXPECT_FLOAT_EQ(h.fraction * 5.0, de_dsqrt64(1.0 + 4.0 + 9.0), 1e-5);
    EXPECT_FLOAT_EQ(h.point.z, 1.0, 1e-5);
    /* Out of range; inside (distance 0, zero normal, the point itself). */
    EXPECT_FALSE(de_physics_closest_point(p, (de_dvec3){3.0, 0.0, 0.0}, 1.5f, all_filter(), &h));
    ASSERT_TRUE(de_physics_closest_point(p, (de_dvec3){0.5, 0.2, 0.1}, 1.0f, all_filter(), &h));
    EXPECT_FLOAT_EQ(h.fraction, 0.0f, 0.0);
    EXPECT_FLOAT_EQ(h.normal.x, 0.0f, 0.0);
    EXPECT_FLOAT_EQ(h.normal.z, 0.0f, 0.0);
    EXPECT_FLOAT_EQ(h.point.x, 0.5, 0.0);
    /* Sphere and capsule (radius kept out of GJK). */
    add(p, DE_BODY_STATIC, sphere(p, 0.5f), 10.0, 0.0, 0.0);
    add(p, DE_BODY_STATIC, capsule(p, 0.25f, 1.0f), 20.0, 0.0, 0.0);
    ASSERT_TRUE(de_physics_closest_point(p, (de_dvec3){10.0, 2.0, 0.0}, 5.0f, all_filter(), &h));
    EXPECT_FLOAT_EQ(h.fraction * 5.0, 1.5, 1e-5);
    EXPECT_FLOAT_EQ(h.point.y, 0.5, 1e-5);
    ASSERT_TRUE(de_physics_closest_point(p, (de_dvec3){21.0, 0.0, 3.0}, 5.0f, all_filter(), &h));
    EXPECT_FLOAT_EQ(h.fraction * 5.0, de_dsqrt64(1.0 + 4.0) - 0.25, 1e-5);
    /* Mesh triangles: on the plane, above it, and the tie at a shared edge (lower id). */
    add(p, DE_BODY_STATIC, flat_mesh(p, 2.0f), 30.0, 0.0, 0.0);
    ASSERT_TRUE(de_physics_closest_point(p, (de_dvec3){30.5, 0.2, 0.75}, 1.0f, all_filter(), &h));
    EXPECT_FLOAT_EQ(h.fraction, 0.75, 1e-6);
    EXPECT_FLOAT_EQ(h.normal.z, 1.0, 1e-6);
    ASSERT_TRUE(de_physics_closest_point(p, (de_dvec3){30.0, 0.0, 0.5}, 1.0f, all_filter(), &h));
    EXPECT_EQ(h.sub_shape, 0u);
    ASSERT_TRUE(de_physics_closest_point(p, (de_dvec3){30.5, 0.2, 0.0}, 0.0f, all_filter(), &h));
    EXPECT_FLOAT_EQ(h.fraction, 0.0f, 0.0);
    EXPECT_FLOAT_EQ(h.normal.z, 1.0, 1e-6);
    de_physics_destroy(p);
}

static void check_closest(de_physics_world *p, u64 seed, u32 count) {
    de_rng r = de_rng_seed(seed, 5);
    u32 hits = 0;
    for (u32 n = 0; n < count; ++n) {
        const f32 x = 26.0f * de_rng_f32(&r) - 13.0f;
        const f32 y = 26.0f * de_rng_f32(&r) - 13.0f;
        const f32 z = 7.0f * de_rng_f32(&r) - 2.0f;
        const f32 md = 3.0f * de_rng_f32(&r);
        de_physics_query_filter qf;
        query_filter(n, &qf, p);
        de__qfilter f;
        de__qfilter_init(p, &qf, &f);
        const de_dvec3 pt = {x, y, z};
        bool want_hit = false;
        de_ray_hit want;
        memset(&want, 0, sizeof want);
        for (u32 i = 0; i < p->bodies.high_water; ++i) {
            de_ray_hit h;
            if (de__query_accept(p, i, &f) && de__query_closest_body(p, i, pt, md, &h) &&
                (!want_hit || de__q_hit_less(&h, &want))) {
                want_hit = true;
                want = h;
            }
        }
        de_ray_hit got;
        const bool hit = de_physics_closest_point_ex(p, pt, md, &qf, &got);
        ASSERT_EQ(hit, want_hit);
        if (hit) {
            ASSERT_TRUE(hit_same(&got, &want));
            ++hits;
        }
    }
    EXPECT_GT(hits, count / 4u);
}

TEST(physics_query_cast, closest_point_matches_bruteforce) {
    de_physics_world *p = world_new(512);
    static scene s;
    scene_build(p, &s, 250, 12.0f, 33);
    check_closest(p, 11, 600);
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    check_closest(p, 11, 600);
    de_physics_destroy(p);
}

/* ---- filters, invalid input, large worlds -------------------------------------------------- */

TEST(physics_query_cast, filters_and_invalid_input) {
    de_physics_world *p = world_new(64);
    const de_shape s = sphere(p, 0.5f);
    de_body_desc d = body_desc(DE_BODY_STATIC, s, 2.0, 0.0, 0.0);
    d.filter = (de_collision_filter){2u, 0xFFFFFFFFu, 0u};
    const de_body a = de_body_create(p, &d);
    d = body_desc(DE_BODY_STATIC, s, 4.0, 0.0, 0.0);
    d.flags = DE_BODY_FLAG_TRIGGER;
    const de_body t = de_body_create(p, &d);
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    const de_shape q = sphere(p, 0.25f);
    de_ray_hit h;
    ASSERT_TRUE(cast(p, q, at(0.0, 0.0, 0.0), 1, 0, 0, 10.0f, &h));
    EXPECT_EQ(h.body.id, a.id);
    de_physics_query_filter qf;
    memset(&qf, 0, sizeof qf);
    qf.ignore_body = a;
    const de_transform o = at(0.0, 0.0, 0.0);
    EXPECT_FALSE(de_physics_shape_cast_ex(p, q, &o, (de_vec3){1, 0, 0}, 10.0f, &qf, &h));
    qf.flags = DE_PHYSICS_QUERY_INCLUDE_TRIGGERS;
    ASSERT_TRUE(de_physics_shape_cast_ex(p, q, &o, (de_vec3){1, 0, 0}, 10.0f, &qf, &h));
    EXPECT_EQ(h.body.id, t.id);
    /* Layer 1 only: a (layer 2) is filtered out. */
    EXPECT_FALSE(de_physics_shape_cast(p, q, &o, (de_vec3){1, 0, 0}, 10.0f,
                                       (de_collision_filter){1u, 1u, 0u}, &h));
    /* Invalid input. */
    EXPECT_FALSE(
        de_physics_shape_cast(p, (de_shape){0}, &o, (de_vec3){1, 0, 0}, 10.0f, all_filter(), &h));
    EXPECT_FALSE(de_physics_shape_cast(p, q, NULL, (de_vec3){1, 0, 0}, 10.0f, all_filter(), &h));
    EXPECT_FALSE(de_physics_shape_cast(p, q, &o, (de_vec3){0, 0, 0}, 10.0f, all_filter(), &h));
    EXPECT_FALSE(de_physics_shape_cast(p, flat_mesh(p, 1.0f), &o, (de_vec3){1, 0, 0}, 10.0f,
                                       all_filter(), &h));
    EXPECT_FALSE(de_physics_closest_point(p, o.position, -1.0f, all_filter(), &h));
    EXPECT_EQ(de_physics_overlap(p, q, &o, all_filter(), NULL, 4), 0u);
    de_physics_destroy(p);
}

TEST(physics_query_cast, far_origin_matches_origin) {
    for (u32 pass = 0; pass < 2; ++pass) {
        de_physics_world *p = world_new(64);
        const f64 off = pass == 0 ? 0.0 : 100000.0;
        de_body_desc d = body_desc(DE_BODY_STATIC, box(p, 1.0f, 2.0f, 0.5f), off + 3.0, off, 0.0);
        d.transform.rotation = axis_angle(0.0f, 0.6f, 0.8f, 0.7f);
        ASSERT_TRUE(DE_HANDLE_VALID(de_body_create(p, &d)));
        ASSERT_EQ(de__broadphase_update(p), DE_OK);
        const de_shape c = capsule(p, 0.3f, 0.4f);
        static f32 fr[2][100], cp[2][100];
        de_rng r = de_rng_seed(8, 8);
        for (u32 i = 0; i < 100; ++i) {
            const f32 y = 3.0f * de_rng_f32(&r) - 1.5f;
            const f32 z = 1.6f * de_rng_f32(&r) - 0.8f;
            de_ray_hit h;
            const bool hit = cast(p, c, at(off - 5.0, off + y, z), 1, 0.05f, 0, 20.0f, &h);
            fr[pass][i] = hit ? h.fraction : -1.0f;
            const bool hc = de_physics_closest_point(p, (de_dvec3){off + 3.0, off + y + 3.0, z},
                                                     5.0f, all_filter(), &h);
            cp[pass][i] = hc ? h.fraction : -1.0f;
            if (pass == 1) {
                EXPECT_FLOAT_EQ(fr[1][i], fr[0][i], 1e-4);
                EXPECT_FLOAT_EQ(cp[1][i], cp[0][i], 1e-4);
            }
        }
        de_physics_destroy(p);
    }
}

/* ---- allocation ------------------------------------------------------------------------------ */

TEST(physics_query_cast, no_heap_allocation) {
    de_physics_world *p = world_new(512);
    static scene s;
    scene_build(p, &s, 200, 12.0f, 34);
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    de_ray_hit h;
    static de_body out[64];
    (void)cast(p, s.shapes[0], at(0, 0, 5), 0, 0, -1, 10.0f, &h); /* warm-up */
    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    de_alloc_guard_begin(&g);
    de_rng r = de_rng_seed(2, 2);
    u32 total = 0;
    for (u32 i = 0; i < 200; ++i) {
        const f32 x = 24.0f * de_rng_f32(&r) - 12.0f;
        const f32 y = 24.0f * de_rng_f32(&r) - 12.0f;
        const de_shape sh = s.shapes[i % s.nshapes];
        total += cast(p, sh, at(x, y, 5.0), 0.2f, 0, -1, 10.0f, &h) ? 1u : 0u;
        const de_transform t = at(x, y, 1.0);
        total += de_physics_overlap_ex(p, sh, &t, NULL, out, NULL, 64);
        total += de_physics_closest_point(p, t.position, 2.0f, all_filter(), &h) ? 1u : 0u;
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_GT(total, 100u);
    de_physics_destroy(p);
}

/* ---- concurrency ----------------------------------------------------------------------------- */

#define SMOKE_QUERIES 300u
#define SMOKE_THREADS 4u

typedef struct smoke {
    const de_physics_world *p;
    const scene *s;
    de_ray_hit rays[SMOKE_QUERIES];
    de_ray_hit casts[SMOKE_QUERIES];
    de_ray_hit closest[SMOKE_QUERIES];
    u32 overlaps[SMOKE_QUERIES];
    u8 hit[SMOKE_QUERIES][3];
} smoke;

/* Runs the same mixed query list; results go into the thread's own record. */
static void smoke_run(smoke *m) {
    de_rng r = de_rng_seed(77, 77);
    static const de_physics_query_filter none = {{0u, 0u, 0u}, 0u, {0u}};
    for (u32 i = 0; i < SMOKE_QUERIES; ++i) {
        const f32 x = 24.0f * de_rng_f32(&r) - 12.0f;
        const f32 y = 24.0f * de_rng_f32(&r) - 12.0f;
        const f32 dx = 2.0f * de_rng_f32(&r) - 1.0f;
        const de_shape sh = m->s->shapes[i % m->s->nshapes];
        const de_dvec3 o = {x, y, 6.0};
        memset(&m->rays[i], 0, sizeof m->rays[i]);
        memset(&m->casts[i], 0, sizeof m->casts[i]);
        memset(&m->closest[i], 0, sizeof m->closest[i]);
        m->hit[i][0] =
            de_physics_raycast_ex(m->p, o, (de_vec3){dx, 0.3f, -1.0f}, 30.0f, &none, &m->rays[i]);
        const de_transform t = at(x, y, 6.0);
        m->hit[i][1] = de_physics_shape_cast_ex(m->p, sh, &t, (de_vec3){dx, 0.3f, -1.0f}, 30.0f,
                                                &none, &m->casts[i]);
        const de_transform t2 = at(x, y, 1.5);
        de_entity e[16];
        m->overlaps[i] = de_physics_overlap(m->p, sh, &t2, all_filter(), e, 16);
        m->hit[i][2] = de_physics_closest_point_ex(m->p, t2.position, 2.0f, &none, &m->closest[i]);
    }
}

static i32 smoke_thread(void *user) {
    smoke_run((smoke *)user);
    return 0;
}

TEST(physics_query_cast, concurrent_queries_4_threads) {
    de_physics_world *p = world_new(512);
    static scene s;
    scene_build(p, &s, 250, 12.0f, 35);
    ASSERT_EQ(de__broadphase_update(p), DE_OK);
    static smoke ref, th[SMOKE_THREADS];
    memset(&ref, 0, sizeof ref);
    ref.p = p;
    ref.s = &s;
    smoke_run(&ref);
    de_thread *t[SMOKE_THREADS];
    for (u32 i = 0; i < SMOKE_THREADS; ++i) {
        memset(&th[i], 0, sizeof th[i]);
        th[i].p = p;
        th[i].s = &s;
        ASSERT_EQ(de_thread_create(smoke_thread, &th[i], "query-smoke", DE_THREAD_NORMAL, 0, &t[i]),
                  DE_OK);
    }
    for (u32 i = 0; i < SMOKE_THREADS; ++i)
        EXPECT_EQ(de_thread_join(t[i]), 0);
    u32 hits = 0;
    for (u32 i = 0; i < SMOKE_THREADS; ++i) {
        for (u32 k = 0; k < SMOKE_QUERIES; ++k) {
            ASSERT_TRUE(hit_same(&th[i].rays[k], &ref.rays[k]));
            ASSERT_TRUE(hit_same(&th[i].casts[k], &ref.casts[k]));
            ASSERT_TRUE(hit_same(&th[i].closest[k], &ref.closest[k]));
        }
        ASSERT_MEMEQ(th[i].overlaps, ref.overlaps, sizeof ref.overlaps);
        ASSERT_MEMEQ(th[i].hit, ref.hit, sizeof ref.hit);
    }
    for (u32 i = 0; i < SMOKE_QUERIES; ++i)
        hits += ref.hit[i][0] + ref.hit[i][1] + ref.hit[i][2];
    EXPECT_GT(hits, SMOKE_QUERIES);
    de_physics_destroy(p);
}
