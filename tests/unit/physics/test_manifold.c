/* test_manifold.c: persistent contact manifolds (DENG-135, SPEC §13.1, §13.2): reduction to four
 * points against brute force, feature-id carry-over of impulses, GJK single-point accumulation,
 * touch flags (begin / persist / end, pair level, index reuse), triggers, the speculative margin,
 * composite keys and the per-body lists. */
#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/contact/manifold.h"
#include "physics/physics_internal.h"

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

static de_shape shape_box(de_physics_world *p, f32 x, f32 y, f32 z) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = (de_vec3){x, y, z};
    const de_shape s = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_shape shape_sphere(de_physics_world *p, f32 r) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_SPHERE;
    d.radius = r;
    const de_shape s = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_shape shape_cylinder(de_physics_world *p, f32 r, f32 hh) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CYLINDER;
    d.radius = r;
    d.half_height = hh;
    const de_shape s = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_quat quat_identity(void) {
    return (de_quat){0.0f, 0.0f, 0.0f, 1.0f};
}

static de_quat quat_axis_angle(de_vec3 axis, f32 angle) {
    const de_vec3 a = de_dnormalize_v3(axis);
    f32 s, c;
    de_dsincos(angle * 0.5f, &s, &c);
    return (de_quat){a.x * s, a.y * s, a.z * s, c};
}

static de_transform xform(f64 x, f64 y, f64 z, de_quat q) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position = (de_dvec3){x, y, z};
    t.rotation = q;
    t.scale = (de_vec3){1.0f, 1.0f, 1.0f};
    return t;
}

static de_body body_at(de_physics_world *p, de_body_type type, de_shape s, de_transform t,
                       u32 flags) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform = t;
    d.flags = flags;
    const de_body b = de_body_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    return b;
}

static u32 index_of(de_body b) {
    return de__handle_index(b.id);
}

static void move_to(de_physics_world *p, de_body b, f64 x, f64 y, f64 z, de_quat q) {
    const de_transform t = xform(x, y, z, q);
    ASSERT_TRUE(de_body_set_transform(p, b, &t));
}

static void step(de_physics_world *p) {
    de_physics_step(p, 1.0f / 60.0f);
}

static u32 manifold_count(const de_physics_world *p) {
    u32 n = 0;
    de__contacts_manifolds(p, NULL, &n);
    return n;
}

static u32 ended_count(const de_physics_world *p) {
    u32 n = 0;
    de__contacts_ended(p, NULL, &n);
    return n;
}

static const de__manifold *first_manifold(const de_physics_world *p) {
    const de__manifold *m = NULL;
    u32 n = 0;
    de__contacts_manifolds(p, &m, &n);
    return n > 0 ? m : NULL;
}

/* ---- reduction ---------------------------------------------------------------------------- */

/* Independent hull area for the brute force: project onto a plane basis (f64), Andrew's monotone
 * chain, shoelace. */
typedef struct p2 {
    f64 x, y;
} p2;

static f64 cross2(p2 o, p2 a, p2 b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

static f64 hull_area(p2 *pts, u32 n) {
    for (u32 i = 1; i < n; ++i) /* sort by x, then y */
        for (u32 j = i; j > 0 && (pts[j - 1].x > pts[j].x ||
                                  (pts[j - 1].x == pts[j].x && pts[j - 1].y > pts[j].y));
             --j) {
            const p2 t = pts[j];
            pts[j] = pts[j - 1];
            pts[j - 1] = t;
        }
    p2 h[16];
    u32 k = 0;
    for (u32 i = 0; i < n; ++i) {
        while (k >= 2 && cross2(h[k - 2], h[k - 1], pts[i]) <= 0.0)
            --k;
        h[k++] = pts[i];
    }
    for (u32 i = n - 1, t = k + 1; i-- > 0;) {
        while (k >= t && cross2(h[k - 2], h[k - 1], pts[i]) <= 0.0)
            --k;
        h[k++] = pts[i];
    }
    f64 a = 0.0;
    for (u32 i = 0; i + 1 < k; ++i)
        a += h[i].x * h[i + 1].y - h[i + 1].x * h[i].y;
    return (a < 0.0 ? -a : a) * 0.5;
}

static void plane_basis(de_vec3 n, de_dvec3 *u, de_dvec3 *v) {
    const de_dvec3 nd = {n.x, n.y, n.z};
    const de_dvec3 t = (n.x * n.x < 0.5f) ? (de_dvec3){1, 0, 0} : (de_dvec3){0, 1, 0};
    de_dvec3 a = {nd.y * t.z - nd.z * t.y, nd.z * t.x - nd.x * t.z, nd.x * t.y - nd.y * t.x};
    const f64 l = de_dsqrt64(a.x * a.x + a.y * a.y + a.z * a.z);
    a.x /= l;
    a.y /= l;
    a.z /= l;
    *u = a;
    *v = (de_dvec3){nd.y * a.z - nd.z * a.y, nd.z * a.x - nd.x * a.z, nd.x * a.y - nd.y * a.x};
}

static f64 subset_area(const de_vec3 *pos, const u32 *idx, u32 k, de_vec3 n) {
    de_dvec3 u, v;
    plane_basis(n, &u, &v);
    p2 pts[4];
    for (u32 i = 0; i < k; ++i) {
        const de_vec3 q = pos[idx[i]];
        pts[i].x = q.x * u.x + q.y * u.y + q.z * u.z;
        pts[i].y = q.x * v.x + q.y * v.y + q.z * v.z;
    }
    return hull_area(pts, k);
}

static f32 rf(de_rng *r, f32 lo, f32 hi) {
    const f32 u = de_rng_f32(r);
    return lo + (hi - lo) * u;
}

TEST(physics_manifold, reduce_keeps_up_to_four) {
    const de_vec3 pos[4] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    const f32 sep[4] = {0.0f, -0.1f, 0.0f, 0.0f};
    const u32 feat[4] = {4, 3, 2, 1};
    u32 out[4];
    for (u32 n = 0; n <= 4; ++n) {
        EXPECT_EQ(de__contact_reduce(pos, sep, feat, n, (de_vec3){0, 0, 1}, out), n);
        for (u32 i = 0; i < n; ++i)
            EXPECT_EQ(out[i], i);
    }
}

TEST(physics_manifold, reduce_vs_bruteforce) {
    de_rng r = de_rng_seed(0xDE135, 1);
    u32 checked = 0;
    for (u32 iter = 0; iter < 3000; ++iter) {
        const u32 n = 5u + iter % 4u;
        const f32 nx = rf(&r, -1.0f, 1.0f);
        const f32 ny = rf(&r, -1.0f, 1.0f);
        const f32 nz = rf(&r, -1.0f, 1.0f) + 0.01f;
        const de_vec3 nrm = de_dnormalize_v3((de_vec3){nx, ny, nz});
        de_dvec3 u, v;
        plane_basis(nrm, &u, &v);
        de_vec3 pos[8];
        f32 sep[8];
        u32 feat[8];
        for (u32 i = 0; i < n; ++i) {
            const f32 a = rf(&r, -1.0f, 1.0f);
            const f32 b = rf(&r, -1.0f, 1.0f);
            const f32 h = rf(&r, -0.01f, 0.01f);
            pos[i] = (de_vec3){(f32)(a * u.x + b * v.x) + h * nrm.x,
                               (f32)(a * u.y + b * v.y) + h * nrm.y,
                               (f32)(a * u.z + b * v.z) + h * nrm.z};
            sep[i] = rf(&r, -0.05f, 0.01f);
            feat[i] = 100u + 7u * i;
        }
        u32 out[4];
        ASSERT_EQ(de__contact_reduce(pos, sep, feat, n, nrm, out), 4u);
        /* The deepest point is kept. */
        u32 deepest = 0;
        for (u32 i = 1; i < n; ++i)
            if (sep[i] < sep[deepest])
                deepest = i;
        bool has_deepest = false;
        for (u32 i = 0; i < 4; ++i) {
            has_deepest = has_deepest || out[i] == deepest;
            if (i > 0)
                EXPECT_LT(out[i - 1], out[i]);
        }
        EXPECT_TRUE(has_deepest);
        /* The area is the largest of every four-point subset that holds the deepest point. */
        f64 best = 0.0;
        for (u32 i = 0; i < n; ++i)
            for (u32 j = i + 1; j < n; ++j)
                for (u32 k = j + 1; k < n; ++k) {
                    if (i == deepest || j == deepest || k == deepest)
                        continue;
                    const u32 idx[4] = {deepest, i, j, k};
                    const f64 a = subset_area(pos, idx, 4, nrm);
                    best = a > best ? a : best;
                }
        const f64 got = subset_area(pos, out, 4, nrm);
        EXPECT_TRUE(got >= best * (1.0 - 1e-4) - 1e-6);
        ++checked;
    }
    EXPECT_EQ(checked, 3000u);
}

TEST(physics_manifold, reduce_ties) {
    /* A square with every corner twice: equal areas, so the lower feature ids win. */
    const de_vec3 pos[8] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                            {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const f32 sep[8] = {-0.1f, 0, 0, 0, 0, 0, 0, 0};
    const u32 feat[8] = {50, 60, 70, 80, 51, 9, 8, 7};
    u32 out[4];
    ASSERT_EQ(de__contact_reduce(pos, sep, feat, 8, (de_vec3){0, 0, 1}, out), 4u);
    EXPECT_EQ(out[0], 0u); /* deepest */
    EXPECT_EQ(out[1], 5u);
    EXPECT_EQ(out[2], 6u);
    EXPECT_EQ(out[3], 7u);
    /* Collinear points (zero area): the spread decides, so both ends stay. */
    const de_vec3 line[6] = {{0.5f, 0, 0}, {0, 0, 0}, {0.2f, 0, 0},
                             {0.7f, 0, 0}, {1, 0, 0}, {0.4f, 0, 0}};
    const f32 lsep[6] = {-0.2f, 0, 0, 0, 0, 0};
    const u32 lfeat[6] = {1, 2, 3, 4, 5, 6};
    ASSERT_EQ(de__contact_reduce(line, lsep, lfeat, 6, (de_vec3){0, 0, 1}, out), 4u);
    bool has1 = false, has4 = false;
    for (u32 i = 0; i < 4; ++i) {
        has1 = has1 || out[i] == 1u;
        has4 = has4 || out[i] == 4u;
    }
    EXPECT_TRUE(out[0] == 0u && has1 && has4);
    /* The hull area helper. */
    EXPECT_FLOAT_EQ(de__contact_quad_area(pos[0], pos[2], pos[1], pos[3], (de_vec3){0, 0, 1}), 1.0f,
                    1e-6f);
    EXPECT_FLOAT_EQ(
        de__contact_quad_area(pos[0], pos[1], pos[3], (de_vec3){0.2f, 0.2f, 0}, (de_vec3){0, 0, 1}),
        0.5f, 1e-6f);
}

/* ---- carry-over and accumulation (pure) ---------------------------------------------------- */

static de__contact point(f32 x, f32 y, f32 sep, u32 feature) {
    de__contact c;
    memset(&c, 0, sizeof c);
    c.local_a = (de_vec3){x, y, 0.0f};
    c.local_b = (de_vec3){x, y, -sep};
    c.separation = sep;
    c.feature_id = feature;
    return c;
}

TEST(physics_manifold, carry_by_feature_id) {
    de__manifold old, cur;
    memset(&old, 0, sizeof old);
    memset(&cur, 0, sizeof cur);
    old.id = 17;
    old.serial = 5;
    old.point_count = 3;
    old.points[0] = point(0, 0, 0, 10);
    old.points[1] = point(1, 0, 0, 11);
    old.points[2] = point(1, 1, 0, 12);
    for (u32 i = 0; i < 3; ++i) {
        old.points[i].normal_impulse = 1.0f + (f32)i;
        old.points[i].tangent_impulse[0] = 10.0f + (f32)i;
        old.points[i].tangent_impulse[1] = 20.0f + (f32)i;
    }
    cur.flags = DE__MANIFOLD_BEGIN;
    cur.point_count = 3;
    cur.points[0] = point(0, 0, 0, 12);  /* matches old[2] */
    cur.points[1] = point(0, 1, 0, 99);  /* new feature */
    cur.points[2] = point(1, 0, 0, 10);  /* matches old[0] */
    cur.points[1].normal_impulse = 5.0f; /* garbage is cleared */
    de__manifold_carry(&cur, &old);
    EXPECT_EQ(cur.id, 17u);
    EXPECT_EQ(cur.serial, 5u);
    EXPECT_EQ(cur.flags, (u32)DE__MANIFOLD_PERSIST);
    EXPECT_TRUE(cur.points[0].normal_impulse == 3.0f && cur.points[0].tangent_impulse[0] == 12.0f &&
                cur.points[0].tangent_impulse[1] == 22.0f);
    EXPECT_TRUE(cur.points[1].normal_impulse == 0.0f && cur.points[1].tangent_impulse[0] == 0.0f &&
                cur.points[1].tangent_impulse[1] == 0.0f);
    EXPECT_TRUE(cur.points[2].normal_impulse == 1.0f && cur.points[2].tangent_impulse[1] == 20.0f);
}

static de__pose pose_at(f64 x, f64 y, f64 z) {
    de__pose p;
    p.position = (de_dvec3){x, y, z};
    p.rotation = quat_identity();
    return p;
}

/* A one-point candidate: A at the origin, B above it; feature (A face, B curved shape). */
static de__manifold gjk_candidate(f32 x, f32 y, f32 sep, u32 serial) {
    de__manifold m;
    memset(&m, 0, sizeof m);
    m.normal = (de_vec3){0, 0, 1};
    m.flags = DE__MANIFOLD_ACCUMULATE;
    m.point_count = 1;
    m.serial = serial;
    m.points[0] =
        point(x, y, sep, de__feature_id(de__feat(DE__FEAT_FACE, 4), de__feat(DE__FEAT_SHAPE, 0)));
    return m;
}

TEST(physics_manifold, accumulate_gjk_points) {
    const de__pose xa = pose_at(0, 0, 0), xb = pose_at(0, 0, 0);
    /* Tick 1: a single point gets a fresh id (serial 0 -> curved half index 1). */
    de__manifold m1 = gjk_candidate(1.0f, 0.0f, -0.001f, 0);
    de__manifold_accumulate(&m1, NULL, xa, xb, 0.02f);
    ASSERT_EQ(m1.point_count, 1u);
    EXPECT_EQ(de__feat_index(de__feature_b(m1.points[0].feature_id)), 1u);
    EXPECT_EQ(de__feature_a(m1.points[0].feature_id), de__feat(DE__FEAT_FACE, 4));
    EXPECT_EQ(m1.serial, 1u);
    m1.points[0].normal_impulse = 7.0f;

    /* Tick 2: a point at the other end of the rim: both kept. */
    de__manifold m2 = gjk_candidate(-1.0f, 0.0f, -0.002f, m1.serial);
    de__manifold_accumulate(&m2, &m1, xa, xb, 0.02f);
    ASSERT_EQ(m2.point_count, 2u);
    EXPECT_EQ(m2.points[0].feature_id, m1.points[0].feature_id);
    EXPECT_TRUE(m2.points[0].normal_impulse == 7.0f);
    EXPECT_NE(m2.points[1].feature_id, m1.points[0].feature_id);
    EXPECT_TRUE(m2.points[1].normal_impulse == 0.0f);

    /* Tick 3: a point 5 mm from the first one replaces it: its id and impulses survive. */
    de__manifold m3 = gjk_candidate(1.005f, 0.0f, -0.003f, m2.serial);
    de__manifold_accumulate(&m3, &m2, xa, xb, 0.02f);
    ASSERT_EQ(m3.point_count, 2u);
    EXPECT_EQ(m3.points[0].feature_id, m2.points[1].feature_id); /* surviving old point first */
    EXPECT_EQ(m3.points[1].feature_id, m1.points[0].feature_id);
    EXPECT_TRUE(m3.points[1].normal_impulse == 7.0f);
    EXPECT_TRUE(m3.points[1].separation == -0.003f);

    /* Ticks 4..6: more rim points; never more than four, the deepest kept, ids unique. */
    de__manifold prev = m3;
    const f32 xs[3] = {0.0f, 0.0f, 0.7f}, ys[3] = {1.0f, -1.0f, 0.7f};
    for (u32 t = 0; t < 3; ++t) {
        de__manifold m = gjk_candidate(xs[t], ys[t], t == 2 ? -0.01f : -0.001f, prev.serial);
        de__manifold_accumulate(&m, &prev, xa, xb, 0.02f);
        EXPECT_LE(m.point_count, 4u);
        for (u32 i = 0; i < m.point_count; ++i)
            for (u32 j = i + 1; j < m.point_count; ++j)
                EXPECT_NE(m.points[i].feature_id, m.points[j].feature_id);
        prev = m;
    }
    EXPECT_EQ(prev.point_count, 4u);
    bool deepest = false;
    for (u32 i = 0; i < prev.point_count; ++i)
        deepest = deepest || prev.points[i].separation == -0.01f;
    EXPECT_TRUE(deepest);

    /* B moved 3 cm sideways: every old point drifted past 2 cm and is dropped. */
    de__manifold m7 = gjk_candidate(0.3f, 0.3f, -0.001f, prev.serial);
    de__manifold_accumulate(&m7, &prev, xa, pose_at(0.03, 0, 0), 0.02f);
    EXPECT_EQ(m7.point_count, 1u);
    /* B lifted 3 cm: old points separate past the margin and are dropped. */
    de__manifold m8 = gjk_candidate(0.3f, 0.3f, -0.001f, prev.serial);
    de__manifold_accumulate(&m8, &prev, xa, pose_at(0, 0, 0.03), 0.02f);
    EXPECT_EQ(m8.point_count, 1u);
    /* B lifted 1 cm: still within the margin, kept with the new separation. */
    de__manifold m9 = gjk_candidate(0.3f, 0.3f, -0.001f, prev.serial);
    de__manifold_accumulate(&m9, &prev, xa, pose_at(0, 0, 0.01), 0.02f);
    EXPECT_EQ(m9.point_count, 4u);
}

/* ---- the pipeline: matching, touch state, triggers ----------------------------------------- */

TEST(physics_manifold, resting_box_keeps_features_and_impulses) {
    de_physics_world *p = world_new(64);
    const de_shape ground = shape_box(p, 5.0f, 5.0f, 0.5f);
    const de_shape cube = shape_box(p, 0.5f, 0.5f, 0.5f);
    (void)body_at(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0);
    (void)body_at(p, DE_BODY_DYNAMIC, cube, xform(0.1, -0.2, 0.499, quat_identity()), 0);
    step(p);
    ASSERT_EQ(manifold_count(p), 1u);
    const de__manifold *m0 = first_manifold(p);
    ASSERT_EQ(m0->point_count, 4u);
    EXPECT_EQ(m0->flags & (u32)(DE__MANIFOLD_BEGIN | DE__MANIFOLD_PAIR_BEGIN),
              (u32)(DE__MANIFOLD_BEGIN | DE__MANIFOLD_PAIR_BEGIN));
    EXPECT_FLOAT_EQ(m0->normal.z, 1.0f, 1e-6f);
    u32 ids[4];
    for (u32 i = 0; i < 4; ++i) {
        ids[i] = m0->points[i].feature_id;
        EXPECT_FLOAT_EQ(m0->points[i].separation, -0.001f, 1e-5f);
    }
    const u32 mid = m0->id;
    /* The solver's stand-in writes impulses once; nothing else touches them. */
    u32 n = 0;
    de__manifold *mm = de__contacts_manifolds_mut(p, &n);
    for (u32 i = 0; i < 4; ++i) {
        mm->points[i].normal_impulse = 1.5f + (f32)i;
        mm->points[i].tangent_impulse[0] = -0.25f * (f32)i;
        mm->points[i].tangent_impulse[1] = 0.125f;
    }
    for (u32 t = 0; t < 100; ++t) {
        step(p);
        ASSERT_EQ(manifold_count(p), 1u);
        EXPECT_EQ(ended_count(p), 0u);
        const de__manifold *m = first_manifold(p);
        EXPECT_EQ(m->flags, (u32)DE__MANIFOLD_PERSIST);
        EXPECT_EQ(m->id, mid);
        ASSERT_EQ(m->point_count, 4u);
        for (u32 i = 0; i < 4; ++i) {
            EXPECT_EQ(m->points[i].feature_id, ids[i]);
            EXPECT_TRUE(m->points[i].normal_impulse == 1.5f + (f32)i);
            EXPECT_TRUE(m->points[i].tangent_impulse[0] == -0.25f * (f32)i);
            EXPECT_TRUE(m->points[i].tangent_impulse[1] == 0.125f);
        }
    }
    de_physics_destroy(p);
}

TEST(physics_manifold, jittered_box_keeps_features) {
    /* +-0.5 mm / 0.3 degree jitter of a resting box: the four feature ids stay, impulses carry. */
    de_physics_world *p = world_new(64);
    const de_shape ground = shape_box(p, 5.0f, 5.0f, 0.5f);
    const de_shape cube = shape_box(p, 0.5f, 0.5f, 0.5f);
    (void)body_at(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0);
    const de_body b = body_at(p, DE_BODY_DYNAMIC, cube, xform(0, 0, 0.499, quat_identity()), 0);
    step(p);
    ASSERT_EQ(manifold_count(p), 1u);
    u32 n = 0;
    de__manifold *m = de__contacts_manifolds_mut(p, &n);
    ASSERT_EQ(m->point_count, 4u);
    u32 ids[4];
    for (u32 i = 0; i < 4; ++i) {
        ids[i] = m->points[i].feature_id;
        m->points[i].normal_impulse = 2.0f;
    }
    de_rng r = de_rng_seed(135, 2);
    for (u32 t = 0; t < 100; ++t) {
        const f32 dx = rf(&r, -0.0005f, 0.0005f);
        const f32 dz = rf(&r, -0.0005f, 0.0005f);
        const f32 ax = rf(&r, -1.0f, 1.0f);
        const f32 ay = rf(&r, -1.0f, 1.0f);
        const f32 ang = rf(&r, -0.005f, 0.005f);
        move_to(p, b, dx, 0.0, 0.499 + dz, quat_axis_angle((de_vec3){ax, ay, 0.01f}, ang));
        step(p);
        ASSERT_EQ(manifold_count(p), 1u);
        m = de__contacts_manifolds_mut(p, &n);
        ASSERT_EQ(m->point_count, 4u);
        for (u32 i = 0; i < 4; ++i) {
            bool found = false;
            for (u32 j = 0; j < 4; ++j)
                found = found || m->points[j].feature_id == ids[i];
            EXPECT_TRUE(found);
            EXPECT_TRUE(m->points[i].normal_impulse == 2.0f);
        }
    }
    de_physics_destroy(p);
}

TEST(physics_manifold, begin_end_once_per_transition) {
    de_physics_world *p = world_new(64);
    const de_shape ground = shape_box(p, 5.0f, 5.0f, 0.5f);
    const de_shape ball = shape_sphere(p, 0.5f);
    const de_body g = body_at(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0);
    const de_body s = body_at(p, DE_BODY_DYNAMIC, ball, xform(0, 0, 3.0, quat_identity()), 0);
    /* Out of contact, then touching for ticks 10..29, apart, touching again 40..44. */
    u32 begins = 0, pair_begins = 0, ends = 0, pair_ends = 0, persists = 0;
    for (u32 t = 0; t < 60; ++t) {
        const bool touch = (t >= 10 && t < 30) || (t >= 40 && t < 45);
        move_to(p, s, 0.01 * (f64)t, 0, touch ? 0.495 : 3.0, quat_identity());
        step(p);
        const de__manifold *m = NULL;
        u32 n = 0;
        de__contacts_manifolds(p, &m, &n);
        ASSERT_EQ(n, touch ? 1u : 0u);
        if (n == 1) {
            EXPECT_EQ(m->key.a, index_of(g));
            EXPECT_EQ(m->key.b, index_of(s));
            const bool first = t == 10 || t == 40;
            EXPECT_EQ((m->flags & (u32)DE__MANIFOLD_BEGIN) != 0, first);
            EXPECT_EQ((m->flags & (u32)DE__MANIFOLD_PAIR_BEGIN) != 0, first);
            EXPECT_EQ((m->flags & (u32)DE__MANIFOLD_PERSIST) != 0, !first);
            begins += (m->flags & (u32)DE__MANIFOLD_BEGIN) ? 1u : 0u;
            pair_begins += (m->flags & (u32)DE__MANIFOLD_PAIR_BEGIN) ? 1u : 0u;
            persists += (m->flags & (u32)DE__MANIFOLD_PERSIST) ? 1u : 0u;
        }
        const de__contact_end *e = NULL;
        u32 ne = 0;
        de__contacts_ended(p, &e, &ne);
        const bool ended = t == 30 || t == 45;
        ASSERT_EQ(ne, ended ? 1u : 0u);
        if (ne == 1) {
            EXPECT_EQ(e->flags, (u32)DE__CONTACT_END_PAIR);
            EXPECT_EQ(e->key.a, index_of(g));
            ++ends;
            pair_ends += (e->flags & (u32)DE__CONTACT_END_PAIR) ? 1u : 0u;
        }
    }
    EXPECT_EQ(begins, 2u);
    EXPECT_EQ(pair_begins, 2u);
    EXPECT_EQ(ends, 2u);
    EXPECT_EQ(pair_ends, 2u);
    EXPECT_EQ(persists, 19u + 4u);
    de_physics_destroy(p);
}

TEST(physics_manifold, destroy_and_index_reuse_end_the_contact) {
    de_physics_world *p = world_new(64);
    const de_shape ground = shape_box(p, 5.0f, 5.0f, 0.5f);
    const de_shape ball = shape_sphere(p, 0.5f);
    (void)body_at(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0);
    const de_body s = body_at(p, DE_BODY_DYNAMIC, ball, xform(0, 0, 0.49, quat_identity()), 0);
    step(p);
    step(p);
    ASSERT_EQ(manifold_count(p), 1u);
    const u32 old_id = first_manifold(p)->id;
    /* Same index, new body, still touching: the old contact ends (removed), a new one begins. */
    de_body_destroy(p, s);
    const de_body s2 = body_at(p, DE_BODY_DYNAMIC, ball, xform(1, 0, 0.49, quat_identity()), 0);
    ASSERT_EQ(index_of(s2), index_of(s));
    step(p);
    ASSERT_EQ(manifold_count(p), 1u);
    const de__manifold *m = first_manifold(p);
    EXPECT_EQ(m->flags & (u32)(DE__MANIFOLD_BEGIN | DE__MANIFOLD_PAIR_BEGIN),
              (u32)(DE__MANIFOLD_BEGIN | DE__MANIFOLD_PAIR_BEGIN));
    EXPECT_NE(m->id, old_id); /* an id is not reused in the tick that freed it */
    EXPECT_EQ(m->gen_b, de__handle_gen(s2.id));
    const de__contact_end *e = NULL;
    u32 ne = 0;
    de__contacts_ended(p, &e, &ne);
    ASSERT_EQ(ne, 1u);
    EXPECT_EQ(e->flags, (u32)(DE__CONTACT_END_PAIR | DE__CONTACT_END_REMOVED));
    EXPECT_EQ(e->id, old_id);
    EXPECT_EQ(e->gen_b, de__handle_gen(s.id));
    /* Destroyed without a replacement: ends once. */
    de_body_destroy(p, s2);
    step(p);
    EXPECT_EQ(manifold_count(p), 0u);
    ASSERT_EQ(ended_count(p), 1u);
    step(p);
    EXPECT_EQ(ended_count(p), 0u);
    de_physics_destroy(p);
}

TEST(physics_manifold, trigger_pairs_have_no_points) {
    de_physics_world *p = world_new(64);
    const de_shape zone = shape_box(p, 1.0f, 1.0f, 1.0f);
    const de_shape ball = shape_sphere(p, 0.25f);
    (void)body_at(p, DE_BODY_STATIC, zone, xform(0, 0, 0, quat_identity()),
                  (u32)DE_BODY_FLAG_TRIGGER);
    const de_body s = body_at(p, DE_BODY_DYNAMIC, ball, xform(0, 0, 3, quat_identity()), 0);
    u32 enter = 0, exit = 0;
    for (u32 t = 0; t < 40; ++t) {
        /* Inside for ticks 10..19; 1 cm outside (within the speculative margin) for 20..29. */
        f64 z = 3.0;
        if (t >= 10 && t < 20)
            z = 0.5;
        else if (t >= 20 && t < 30)
            z = 1.26;
        move_to(p, s, 0, 0, z, quat_identity());
        step(p);
        const de__manifold *m = NULL;
        u32 n = 0;
        de__contacts_manifolds(p, &m, &n);
        ASSERT_EQ(n, (t >= 10 && t < 20) ? 1u : 0u);
        if (n == 1) {
            EXPECT_TRUE((m->flags & (u32)DE__MANIFOLD_TRIGGER) != 0);
            EXPECT_EQ(m->point_count, 0u);
            enter += (m->flags & (u32)DE__MANIFOLD_PAIR_BEGIN) ? 1u : 0u;
        }
        const de__contact_end *e = NULL;
        u32 ne = 0;
        de__contacts_ended(p, &e, &ne);
        if (ne > 0) {
            EXPECT_EQ(e->flags, (u32)(DE__CONTACT_END_PAIR | DE__CONTACT_END_TRIGGER));
            ++exit;
        }
    }
    EXPECT_EQ(enter, 1u);
    EXPECT_EQ(exit, 1u);
    de__contact_stats st;
    de__contacts_stats(p, &st);
    EXPECT_EQ(st.points, 0u);
    de_physics_destroy(p);
}

TEST(physics_manifold, speculative_margin) {
    de_physics_world *p = world_new(64);
    const de_shape ground = shape_box(p, 5.0f, 5.0f, 0.5f);
    const de_shape ball = shape_sphere(p, 0.5f);
    (void)body_at(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0);
    const de_body s = body_at(p, DE_BODY_DYNAMIC, ball, xform(0, 0, 0.51, quat_identity()), 0);
    /* 1 cm above the ground: the AABBs do not overlap, but the pair is within the margin. */
    step(p);
    ASSERT_EQ(manifold_count(p), 1u);
    const de__manifold *m = first_manifold(p);
    ASSERT_EQ(m->point_count, 1u);
    EXPECT_FLOAT_EQ(m->points[0].separation, 0.01f, 1e-5f);
    EXPECT_FLOAT_EQ(de__contacts_speculative(p), DE__CONTACT_DEFAULT_SPECULATIVE, 0.0f);
    /* 3 cm above: nothing. */
    move_to(p, s, 0, 0, 0.53, quat_identity());
    step(p);
    EXPECT_EQ(manifold_count(p), 0u);
    EXPECT_EQ(ended_count(p), 1u);
    de_physics_destroy(p);

    /* An explicit margin of 5 cm finds it (the broadphase margin is 5 cm too). */
    p = world_new(64);
    de__contacts_desc cd = {0.05f, 0u};
    ASSERT_EQ(de__contacts_create(p, &cd), DE_OK);
    EXPECT_EQ(de__contacts_create(p, &cd), DE_ERR_ALREADY_EXISTS);
    const de_shape g2 = shape_box(p, 5.0f, 5.0f, 0.5f);
    const de_shape b2 = shape_sphere(p, 0.5f);
    (void)body_at(p, DE_BODY_STATIC, g2, xform(0, 0, -0.5, quat_identity()), 0);
    (void)body_at(p, DE_BODY_DYNAMIC, b2, xform(0, 0, 0.53, quat_identity()), 0);
    step(p);
    EXPECT_EQ(manifold_count(p), 1u);
    de_physics_destroy(p);

    /* Bad settings. */
    p = world_new(64);
    de__contacts_desc bad = {-1.0f, 0u};
    EXPECT_EQ(de__contacts_create(p, &bad), DE_ERR_INVALID_ARGUMENT);
    bad.speculative = 0.0f;
    bad.max_manifolds = 0x40000000u;
    EXPECT_EQ(de__contacts_create(p, &bad), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__contacts_create(NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(de__contacts_get(p) == NULL);
    de_physics_destroy(p);
}

TEST(physics_manifold, cylinder_accumulates_points) {
    /* An upright cylinder on a box gives one GJK point per tick; tilting it a little around
     * different axes walks that point around the rim, and the manifold collects up to four. */
    de_physics_world *p = world_new(64);
    const de_shape ground = shape_box(p, 5.0f, 5.0f, 0.5f);
    const de_shape cyl = shape_cylinder(p, 0.5f, 0.5f);
    (void)body_at(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0);
    const de_body c = body_at(p, DE_BODY_DYNAMIC, cyl, xform(0, 0, 0.499, quat_identity()), 0);
    const de_vec3 axes[6] = {{1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    u32 most = 0;
    for (u32 t = 0; t < 12; ++t) {
        move_to(p, c, 0, 0, 0.4995, quat_axis_angle(axes[t % 6u], 0.002f));
        step(p);
        ASSERT_EQ(manifold_count(p), 1u);
        const de__manifold *m = first_manifold(p);
        EXPECT_TRUE((m->flags & (u32)DE__MANIFOLD_ACCUMULATE) != 0);
        EXPECT_LE(m->point_count, 4u);
        for (u32 i = 0; i < m->point_count; ++i)
            for (u32 j = i + 1; j < m->point_count; ++j)
                EXPECT_NE(m->points[i].feature_id, m->points[j].feature_id);
        most = m->point_count > most ? m->point_count : most;
    }
    EXPECT_GE(most, 3u);
    /* Slide it 5 cm: the rim points drift past 2 cm and are dropped. */
    move_to(p, c, 0.05, 0, 0.4995, quat_axis_angle(axes[0], 0.002f));
    step(p);
    EXPECT_EQ(first_manifold(p)->point_count, 1u);
    de_physics_destroy(p);
}

TEST(physics_manifold, mesh_keys_and_body_lists) {
    de_physics_world *p = world_new(64);
    /* A 2 x 2 quad grid mesh (8 triangles) and two cubes resting on it across triangle edges. */
    de_vec3 verts[9];
    for (u32 j = 0; j < 3; ++j)
        for (u32 i = 0; i < 3; ++i)
            verts[j * 3 + i] = (de_vec3){(f32)i * 2.0f - 2.0f, (f32)j * 2.0f - 2.0f, 0.0f};
    u32 idx[24];
    u32 w = 0;
    for (u32 j = 0; j < 2; ++j)
        for (u32 i = 0; i < 2; ++i) {
            const u32 v0 = j * 3 + i, v1 = v0 + 1, v2 = v0 + 3, v3 = v0 + 4;
            idx[w++] = v0;
            idx[w++] = v1;
            idx[w++] = v3;
            idx[w++] = v0;
            idx[w++] = v3;
            idx[w++] = v2;
        }
    de_shape_desc md;
    memset(&md, 0, sizeof md);
    md.kind = DE_SHAPE_MESH;
    md.vertices = verts;
    md.vertex_count = 9;
    md.indices = idx;
    md.index_count = 24;
    const de_shape mesh = de_shape_create(p, &md);
    ASSERT_TRUE(DE_HANDLE_VALID(mesh));
    const de_shape cube = shape_box(p, 0.5f, 0.5f, 0.5f);
    const de_body g = body_at(p, DE_BODY_STATIC, mesh, xform(0, 0, 0, quat_identity()), 0);
    const de_body c1 = body_at(p, DE_BODY_DYNAMIC, cube, xform(0, 0, 0.499, quat_identity()), 0);
    const de_body c2 = body_at(p, DE_BODY_DYNAMIC, cube, xform(1.0, 0, 0.499, quat_identity()), 0);
    step(p);
    const de__manifold *m = NULL;
    u32 n = 0;
    de__contacts_manifolds(p, &m, &n);
    ASSERT_GE(n, 3u);
    u32 per_cube[2] = {0, 0}, cube_pair = 0;
    for (u32 i = 0; i < n; ++i) {
        if (i > 0)
            EXPECT_LT(de__contact_key_cmp(&m[i - 1].key, &m[i].key), 0);
        EXPECT_LE(m[i].point_count, 4u);
        if (m[i].key.a == index_of(g)) {
            EXPECT_NE(m[i].key.sub_a, DE__SUB_NONE); /* the triangle */
            EXPECT_EQ(m[i].key.sub_b, DE__SUB_NONE);
            per_cube[m[i].key.b == index_of(c1) ? 0 : 1]++;
        } else {
            ++cube_pair; /* the two cubes touch face to face */
        }
        EXPECT_TRUE(de__contacts_find(p, &m[i].key) == &m[i]);
    }
    EXPECT_GE(per_cube[0], 2u); /* across the triangles meeting at the origin */
    EXPECT_GE(per_cube[1], 1u);
    EXPECT_EQ(cube_pair, 1u);
    /* Per-body lists: ascending indices of the manifolds that name the body. */
    const de_body bodies[3] = {g, c1, c2};
    u32 total = 0;
    for (u32 k = 0; k < 3; ++k) {
        const u32 *list = NULL;
        u32 ln = 0;
        de__contacts_body(p, index_of(bodies[k]), &list, &ln);
        u32 expect = 0;
        for (u32 i = 0; i < n; ++i)
            if (m[i].key.a == index_of(bodies[k]) || m[i].key.b == index_of(bodies[k]))
                ++expect;
        ASSERT_EQ(ln, expect);
        for (u32 i = 0; i < ln; ++i) {
            EXPECT_TRUE(m[list[i]].key.a == index_of(bodies[k]) ||
                        m[list[i]].key.b == index_of(bodies[k]));
            if (i > 0)
                EXPECT_LT(list[i - 1], list[i]);
        }
        total += ln;
    }
    EXPECT_EQ(total, 2u * n);
    const u32 *list = NULL;
    u32 ln = 5;
    de__contacts_body(p, 60, &list, &ln);
    EXPECT_EQ(ln, 0u);
    de__contact_key missing = m[0].key;
    missing.sub_a = 99;
    EXPECT_TRUE(de__contacts_find(p, &missing) == NULL);
    de_physics_destroy(p);
}
