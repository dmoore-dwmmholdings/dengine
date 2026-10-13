/*
 * test_manifold2d.c: persistent 2D manifolds and the collision pipeline of de_physics2d_step
 * (DENG-253, SPEC §13.2, §13.3, §5.7, §8): persistence and stable feature ids of a resting box,
 * warm-start carry-over, touch flags and end records, triggers, chain sub-pairs, per-body lists,
 * the checksum, capacity overflow, worker-count invariance, no allocation after warm-up, and the
 * pipeline vector of a 1,000-body kinematic sweep over 300 ticks
 * (tests/unit/physics2d/vectors/pipeline2d_golden.txt). Regenerate it (never by hand) with
 *     DE_REGEN_GOLDEN=1 build/dev/tests/unit/physics2d/test_physics2d_manifold
 * --filter=physics2d_manifold.pipeline_golden_no_tasks
 */
#include "p2d_collide_test_util.h"

#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#ifndef DE_PHYSICS2D_PIPELINE_GOLDEN
#error "DE_PHYSICS2D_PIPELINE_GOLDEN (path of pipeline2d_golden.txt) must be defined by the build"
#endif

#define DT (1.0f / 60.0f)

static de_physics2d_world *world_new(u32 max_bodies, de_task_system *ts, const de_allocator *a) {
    de_physics2d_desc d;
    memset(&d, 0, sizeof d);
    d.max_bodies = max_bodies;
    d.tasks = ts;
    d.allocator = a;
    de_physics2d_world *p = NULL;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    return p;
}

static de_task_system *tasks_new(u32 workers) {
    if (workers == 0)
        return NULL;
    de_fp_env_init_thread();
    de_task_system_desc td;
    memset(&td, 0, sizeof td);
    td.worker_count = workers;
    td.background_worker_count = 1;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&td, &ts), DE_OK);
    return ts;
}

static const de__manifold2d *find_pair(de_physics2d_world *p, de_body a, de_body b, u32 child) {
    de__contact_key2d k;
    const u32 ia = tu_index(a), ib = tu_index(b);
    k.a = ia < ib ? ia : ib;
    k.b = ia < ib ? ib : ia;
    k.child = child;
    return de__contacts2d_find(p, &k);
}

/* ---- Persistence ---------------------------------------------------------------------------- */

TEST(physics2d_manifold, box_resting_on_box_keeps_feature_ids) {
    de_physics2d_world *p = world_new(16, NULL, NULL);
    const de_shape ground = tu_box(p, 4.0f, 0.5f, 0.0f);
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    const de_body g = tu_body(p, DE_BODY_STATIC, ground, tu_xf(0, 0, 0), 0);
    const de_body b = tu_body(p, DE_BODY_DYNAMIC, box, tu_xf(0.3, 0.995, 0), 0);
    /* An equal box on top of the first one. */
    const de_body c = tu_body(p, DE_BODY_DYNAMIC, box, tu_xf(0.3, 1.99, 0), 0);
    de_rng r = de_rng_seed(253, 20);
    u32 ids_bg[2] = {0, 0}, ids_bc[2] = {0, 0}, mid_bg = 0, mid_bc = 0;
    for (u32 t = 0; t < 100; ++t) {
        /* Jitter of a resting pair: +-1 mm, +-0.5 degree. */
        if (t > 0) {
            const de_transform2d xb =
                tu_xf(0.3 + 0.002 * de_rng_f64(&r) - 0.001, 0.995 + 0.002 * de_rng_f64(&r) - 0.001,
                      0.0174533f * de_rng_f32(&r) - 0.00872665f);
            const de_transform2d xc =
                tu_xf(0.3 + 0.002 * de_rng_f64(&r) - 0.001, 1.99 + 0.002 * de_rng_f64(&r) - 0.001,
                      0.0174533f * de_rng_f32(&r) - 0.00872665f);
            ASSERT_TRUE(de_body2d_set_transform(p, b, &xb));
            ASSERT_TRUE(de_body2d_set_transform(p, c, &xc));
        }
        de_physics2d_step(p, DT);
        const de__manifold2d *m1 = find_pair(p, g, b, DE__P2D_CHILD_NONE);
        const de__manifold2d *m2 = find_pair(p, b, c, DE__P2D_CHILD_NONE);
        ASSERT_TRUE(m1 != NULL && m2 != NULL);
        ASSERT_EQ(m1->point_count, 2u);
        ASSERT_EQ(m2->point_count, 2u);
        if (t == 0) {
            EXPECT_TRUE((m1->flags & DE__MANIFOLD2D_BEGIN) != 0);
            EXPECT_TRUE((m1->flags & DE__MANIFOLD2D_PAIR_BEGIN) != 0);
            ids_bg[0] = m1->points[0].feature_id;
            ids_bg[1] = m1->points[1].feature_id;
            ids_bc[0] = m2->points[0].feature_id;
            ids_bc[1] = m2->points[1].feature_id;
            mid_bg = m1->id;
            mid_bc = m2->id;
            EXPECT_NE(ids_bg[0], ids_bg[1]);
        } else {
            EXPECT_EQ(m1->flags & (DE__MANIFOLD2D_BEGIN | DE__MANIFOLD2D_PAIR_BEGIN), 0u);
            EXPECT_TRUE((m1->flags & DE__MANIFOLD2D_PERSIST) != 0);
            EXPECT_EQ(m1->id, mid_bg);
            EXPECT_EQ(m2->id, mid_bc);
            EXPECT_EQ(m1->points[0].feature_id, ids_bg[0]);
            EXPECT_EQ(m1->points[1].feature_id, ids_bg[1]);
            EXPECT_EQ(m2->points[0].feature_id, ids_bc[0]);
            EXPECT_EQ(m2->points[1].feature_id, ids_bc[1]);
        }
        EXPECT_FLOAT_EQ(m1->normal.y, 1.0f, 2e-4);
        const de__contact_end2d *e;
        u32 ne;
        de__contacts2d_ended(p, &e, &ne);
        EXPECT_EQ(ne, 0u);
    }
    de_physics2d_destroy(p);
}

TEST(physics2d_manifold, impulses_carry_by_feature_id) {
    de_physics2d_world *p = world_new(16, NULL, NULL);
    const de_shape ground = tu_box(p, 4.0f, 0.5f, 0.0f);
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    const de_body g = tu_body(p, DE_BODY_STATIC, ground, tu_xf(0, 0, 0), 0);
    const de_body b = tu_body(p, DE_BODY_DYNAMIC, box, tu_xf(0.3, 0.995, 0), 0);
    de_physics2d_step(p, DT);
    u32 n = 0;
    de__manifold2d *m = de__contacts2d_manifolds_mut(p, &n);
    ASSERT_EQ(n, 1u);
    ASSERT_EQ(m[0].point_count, 2u);
    m[0].points[0].normal_impulse = 3.0f;
    m[0].points[0].tangent_impulse = -1.0f;
    m[0].points[1].normal_impulse = 5.0f;
    m[0].points[1].tangent_impulse = 2.0f;
    const u32 f0 = m[0].points[0].feature_id;
    /* Slide a little: the same features touch, impulses follow them. */
    const de_transform2d xb = tu_xf(0.35, 0.996, 0.001f);
    ASSERT_TRUE(de_body2d_set_transform(p, b, &xb));
    de_physics2d_step(p, DT);
    const de__manifold2d *m1 = find_pair(p, g, b, DE__P2D_CHILD_NONE);
    ASSERT_TRUE(m1 != NULL);
    for (u32 i = 0; i < m1->point_count; ++i) {
        const bool first = m1->points[i].feature_id == f0;
        TU_EXPECT_FEQ(m1->points[i].normal_impulse, first ? 3.0f : 5.0f);
        TU_EXPECT_FEQ(m1->points[i].tangent_impulse, first ? -1.0f : 2.0f);
    }

    /* The rules on their own: by id, then the nearest unmatched old point within 1 cm. */
    de__manifold2d old, cur;
    memset(&old, 0, sizeof old);
    memset(&cur, 0, sizeof cur);
    old.id = 42;
    old.point_count = 2;
    old.points[0] = (de__contact2d){{0, 0}, {0, 0}, 0.0f, 7u, 1.0f, 0.5f};
    old.points[1] = (de__contact2d){{1, 0}, {0, 0}, 0.0f, 8u, 2.0f, 0.25f};
    cur.flags = DE__MANIFOLD2D_BEGIN;
    cur.point_count = 2;
    cur.points[0] = (de__contact2d){{1.005f, 0}, {0, 0}, 0.0f, 99u, 0, 0}; /* near old[1] */
    cur.points[1] = (de__contact2d){{5, 5}, {0, 0}, 0.0f, 7u, 0, 0};       /* id of old[0] */
    de__manifold2d_carry(&cur, &old);
    EXPECT_EQ(cur.id, 42u);
    EXPECT_EQ(cur.flags, (u32)DE__MANIFOLD2D_PERSIST);
    TU_EXPECT_FEQ(cur.points[1].normal_impulse, 1.0f);
    TU_EXPECT_FEQ(cur.points[0].normal_impulse, 2.0f);
    TU_EXPECT_FEQ(cur.points[0].tangent_impulse, 0.25f);
    cur.points[0].local_a = (de_vec2){1.5f, 0};
    de__manifold2d_carry(&cur, &old);
    TU_EXPECT_FEQ(cur.points[0].normal_impulse, 0.0f); /* too far: starts at zero */
    de_physics2d_destroy(p);
}

/* ---- Touch flags, end records, triggers ---------------------------------------------------- */

TEST(physics2d_manifold, touch_flags_and_end_records) {
    de_physics2d_world *p = world_new(16, NULL, NULL);
    const de_shape cir = tu_circle(p, 0.5f, (de_vec2){0, 0});
    const de_body a = tu_body(p, DE_BODY_STATIC, cir, tu_xf(0, 0, 0), 0);
    de_body b = tu_body(p, DE_BODY_DYNAMIC, cir, tu_xf(3, 0, 0), 0);
    const de__manifold2d *m;
    const de__contact_end2d *e;
    u32 ne;
    de_physics2d_step(p, DT);
    EXPECT_TRUE(find_pair(p, a, b, DE__P2D_CHILD_NONE) == NULL);
    /* Within the 2 cm margin: begins (a speculative contact). */
    de_transform2d x = tu_xf(1.015, 0, 0);
    ASSERT_TRUE(de_body2d_set_transform(p, b, &x));
    de_physics2d_step(p, DT);
    m = find_pair(p, a, b, DE__P2D_CHILD_NONE);
    ASSERT_TRUE(m != NULL);
    EXPECT_EQ(m->flags, (u32)(DE__MANIFOLD2D_BEGIN | DE__MANIFOLD2D_PAIR_BEGIN));
    EXPECT_FLOAT_EQ(m->points[0].separation, 0.015f, 1e-6);
    const u32 id = m->id;
    /* Persist. */
    x = tu_xf(0.99, 0, 0);
    ASSERT_TRUE(de_body2d_set_transform(p, b, &x));
    de_physics2d_step(p, DT);
    m = find_pair(p, a, b, DE__P2D_CHILD_NONE);
    ASSERT_TRUE(m != NULL);
    EXPECT_EQ(m->flags, (u32)DE__MANIFOLD2D_PERSIST);
    EXPECT_EQ(m->id, id);
    /* Separate: an end record with the pair-level flag. */
    x = tu_xf(1.5, 0, 0);
    ASSERT_TRUE(de_body2d_set_transform(p, b, &x));
    de_physics2d_step(p, DT);
    EXPECT_TRUE(find_pair(p, a, b, DE__P2D_CHILD_NONE) == NULL);
    de__contacts2d_ended(p, &e, &ne);
    ASSERT_EQ(ne, 1u);
    EXPECT_EQ(e[0].id, id);
    EXPECT_EQ(e[0].flags, (u32)DE__CONTACT2D_END_PAIR);
    de_physics2d_step(p, DT);
    de__contacts2d_ended(p, &e, &ne);
    EXPECT_EQ(ne, 0u);
    /* Touch again, then destroy the body: REMOVED; a new body in the same index begins anew. */
    x = tu_xf(0.99, 0, 0);
    ASSERT_TRUE(de_body2d_set_transform(p, b, &x));
    de_physics2d_step(p, DT);
    ASSERT_TRUE(find_pair(p, a, b, DE__P2D_CHILD_NONE) != NULL);
    const u32 bi = tu_index(b);
    de_body2d_destroy(p, b);
    b = tu_body(p, DE_BODY_DYNAMIC, cir, tu_xf(0.98, 0, 0), 0);
    ASSERT_EQ(tu_index(b), bi);
    de_physics2d_step(p, DT);
    de__contacts2d_ended(p, &e, &ne);
    ASSERT_EQ(ne, 1u);
    EXPECT_EQ(e[0].flags, (u32)(DE__CONTACT2D_END_PAIR | DE__CONTACT2D_END_REMOVED));
    m = find_pair(p, a, b, DE__P2D_CHILD_NONE);
    ASSERT_TRUE(m != NULL);
    EXPECT_EQ(m->flags, (u32)(DE__MANIFOLD2D_BEGIN | DE__MANIFOLD2D_PAIR_BEGIN));
    EXPECT_NE(m->id, e[0].id); /* an id is not reused in the tick that freed it */
    /* Destroyed without a replacement. */
    de_body2d_destroy(p, b);
    de_physics2d_step(p, DT);
    de__contacts2d_ended(p, &e, &ne);
    ASSERT_EQ(ne, 1u);
    EXPECT_TRUE((e[0].flags & DE__CONTACT2D_END_REMOVED) != 0);
    u32 n = 0;
    (void)de__contacts2d_manifolds_mut(p, &n);
    EXPECT_EQ(n, 0u);
    de_physics2d_destroy(p);
}

TEST(physics2d_manifold, triggers_report_overlap_only) {
    de_physics2d_world *p = world_new(16, NULL, NULL);
    const de_shape box = tu_box(p, 1.0f, 1.0f, 0.0f);
    const de_shape cir = tu_circle(p, 0.5f, (de_vec2){0, 0});
    const de_body t = tu_body(p, DE_BODY_STATIC, box, tu_xf(0, 0, 0), DE_BODY_FLAG_TRIGGER);
    const de_body b = tu_body(p, DE_BODY_DYNAMIC, cir, tu_xf(1.51, 0, 0), 0);
    de_physics2d_step(p, DT);
    /* 1 cm apart: a solid pair would have a speculative contact, a trigger none. */
    EXPECT_TRUE(find_pair(p, t, b, DE__P2D_CHILD_NONE) == NULL);
    de_transform2d x = tu_xf(1.3, 0, 0);
    ASSERT_TRUE(de_body2d_set_transform(p, b, &x));
    de_physics2d_step(p, DT);
    const de__manifold2d *m = find_pair(p, t, b, DE__P2D_CHILD_NONE);
    ASSERT_TRUE(m != NULL);
    EXPECT_EQ(m->flags,
              (u32)(DE__MANIFOLD2D_TRIGGER | DE__MANIFOLD2D_BEGIN | DE__MANIFOLD2D_PAIR_BEGIN));
    EXPECT_EQ(m->point_count, 0u);
    /* Fully inside: still one overlap. */
    x = tu_xf(0.1, 0, 0);
    ASSERT_TRUE(de_body2d_set_transform(p, b, &x));
    de_physics2d_step(p, DT);
    m = find_pair(p, t, b, DE__P2D_CHILD_NONE);
    ASSERT_TRUE(m != NULL);
    EXPECT_EQ(m->flags, (u32)(DE__MANIFOLD2D_TRIGGER | DE__MANIFOLD2D_PERSIST));
    /* Out: an end record flagged as a trigger. */
    x = tu_xf(5, 0, 0);
    ASSERT_TRUE(de_body2d_set_transform(p, b, &x));
    de_physics2d_step(p, DT);
    const de__contact_end2d *e;
    u32 ne;
    de__contacts2d_ended(p, &e, &ne);
    ASSERT_EQ(ne, 1u);
    EXPECT_EQ(e[0].flags, (u32)(DE__CONTACT2D_END_PAIR | DE__CONTACT2D_END_TRIGGER));
    de_physics2d_destroy(p);
}

/* ---- Chains in the pipeline ----------------------------------------------------------------- */

TEST(physics2d_manifold, chain_subpairs_and_body_lists) {
    de_physics2d_world *p = world_new(64, NULL, NULL);
    de_vec2 pts[200];
    for (u32 i = 0; i < 200; ++i)
        pts[i] = (de_vec2){50.0f - 0.5f * (f32)i, 0.0f};
    const de_shape ch = de_shape2d_chain_create(p, pts, 200, false);
    /* The chain body is rotated and offset: the midphase works in its frame. */
    const de_body cb = tu_body(p, DE_BODY_KINEMATIC, ch, tu_xf(3, -1, 0.1f), 0);
    const de_shape box = tu_box(p, 0.6f, 0.25f, 0.0f);
    const de_shape cir = tu_circle(p, 0.3f, (de_vec2){0, 0});
    de_body movers[20];
    for (u32 i = 0; i < 20; ++i) {
        /* On the chain line: chain local (x, 0) is world (3 + x c, -1 + x s). */
        const f64 lx = -40.0 + 4.0 * (f64)i;
        const f32 c = de_rot2d_from_angle(0.1f).c, s = de_rot2d_from_angle(0.1f).s;
        movers[i] = tu_body(p, DE_BODY_DYNAMIC, (i & 1u) ? box : cir,
                            tu_xf(3.0 + lx * c - 0.24 * s, -1.0 + lx * s + 0.24 * c, 0.1f), 0);
    }
    de_physics2d_step(p, DT);
    const de__contact_key2d *sub;
    u32 ns;
    de__contacts2d_subpairs(p, &sub, &ns);
    /* Brute force: every segment whose box overlaps the mover's box (+ margin) in the chain
     * frame, sorted by key. */
    const de__chain2d *chain = &de__shape2d_get(p, ch)->u.chain;
    u32 k = 0;
    for (u32 i = 0; i < 20; ++i) {
        const u32 ci = tu_index(cb), mi = tu_index(movers[i]);
        const de_aabb2d box2 = de__contacts2d_chain_box(p, ci, mi, de__contacts2d_speculative(p));
        for (u32 g = 0; g < chain->segment_count; ++g) {
            const de__chain_segment *s = &chain->segments[g];
            const de_aabb2d sb = {de__v2_min(s->p1, s->p2), de__v2_max(s->p1, s->p2)};
            if (!de__aabb2d_overlap(&sb, &box2))
                continue;
            ASSERT_LT(k, ns);
            EXPECT_EQ(sub[k].a, ci < mi ? ci : mi);
            EXPECT_EQ(sub[k].b, ci < mi ? mi : ci);
            EXPECT_EQ(sub[k].child, g);
            ++k;
        }
    }
    EXPECT_EQ(k, ns);
    /* Every mover rests on the chain: normals along the chain normal. */
    const de__manifold2d *m;
    u32 nm;
    de__contacts2d_manifolds(p, &m, &nm);
    ASSERT_GE(nm, 20u);
    const de_vec2 up = {-de_rot2d_from_angle(0.1f).s, de_rot2d_from_angle(0.1f).c};
    for (u32 i = 0; i < nm; ++i) {
        ASSERT_NE(m[i].key.child, DE__P2D_CHILD_NONE);
        if (i > 0)
            EXPECT_LT(de__contact_key2d_cmp(&m[i - 1].key, &m[i].key), 0);
        /* A = the chain body (lower index): normal from chain to mover. */
        EXPECT_TRUE(tu_angle_deg(m[i].normal, up) < 1.0);
    }
    /* Per-body lists. */
    for (u32 i = 0; i < 20; ++i) {
        const u32 *items;
        u32 cnt;
        de__contacts2d_body(p, tu_index(movers[i]), &items, &cnt);
        ASSERT_GE(cnt, 1u);
        for (u32 j = 0; j < cnt; ++j)
            EXPECT_TRUE(m[items[j]].key.b == tu_index(movers[i]));
    }
    const u32 *items;
    u32 cnt;
    de__contacts2d_body(p, tu_index(cb), &items, &cnt);
    EXPECT_EQ(cnt, nm);
    de_physics2d_destroy(p);
}

/* ---- The sweep scene (pipeline vector, worker counts, allocation) -------------------------- */

typedef struct sweep {
    de_physics2d_world *p;
    de_body movers[1000];
    u32 mover_count;
    f64 x0[1000], y0[1000];
    f32 w[1000], amp[1000], spin[1000];
} sweep;

static void sweep_pose(const sweep *s, u32 k, u32 t, de_transform2d *out) {
    const f32 ph = s->w[k] * (f32)t + 0.37f * (f32)k;
    f32 sn, cs;
    de_dsincos(ph, &sn, &cs);
    out->x = s->x0[k] + (f64)(s->amp[k] * cs);
    out->y = s->y0[k] + (f64)(0.5f * s->amp[k] * sn);
    out->rotation = de_rot2d_from_angle(s->spin[k] * (f32)t);
}

static void sweep_build(sweep *s, u32 movers, de_task_system *ts, const de_allocator *a) {
    memset(s, 0, sizeof *s);
    de_physics2d_world *p = world_new(movers + 64u, ts, a);
    s->p = p;
    /* A wide fat margin: the movers stay in their fat boxes for several ticks, so the tree is
     * rebuilt less often (sanitizer run time). The pairs, and so the vector, do not depend on
     * the margin (broadphase2d.h). */
    de__bp2d_desc bd = {0.3f, 0u};
    ASSERT_EQ(de__bp2d_create(p, &bd), DE_OK);
    de_rng r = de_rng_seed(0x2D253, 30);
    /* Static ground: a wavy chain over the whole width, some boxes and segments. */
    static de_vec2 ground[401];
    for (u32 i = 0; i < 401; ++i) {
        f32 sn, cs;
        de_dsincos(0.05f * (f32)i, &sn, &cs);
        ground[i] = (de_vec2){100.0f - 0.5f * (f32)i, 0.6f * sn};
    }
    (void)tu_body(p, DE_BODY_STATIC, de_shape2d_chain_create(p, ground, 401, false), tu_xf(0, 0, 0),
                  0);
    const de_shape slab = tu_box(p, 3.0f, 0.25f, 0.0f);
    const de_shape seg = tu_segment(p, (de_vec2){-2, 0}, (de_vec2){2, 0.3f});
    for (u32 i = 0; i < 20; ++i)
        (void)tu_body(p, DE_BODY_STATIC, slab, tu_xf(-95.0 + 10.0 * i, 6.0 + 4.0 * (i % 3u), 0.1f),
                      0);
    for (u32 i = 0; i < 10; ++i)
        (void)tu_body(p, (i & 1u) ? DE_BODY_KINEMATIC : DE_BODY_STATIC, seg,
                      tu_xf(-90.0 + 20.0 * i, 12.0, -0.2f), 0);
    de_shape kinds[6];
    kinds[0] = tu_circle(p, 0.35f, (de_vec2){0, 0});
    kinds[1] = tu_capsule(p, (de_vec2){-0.4f, 0}, (de_vec2){0.4f, 0}, 0.2f);
    kinds[2] = tu_box(p, 0.4f, 0.3f, 0.0f);
    kinds[3] = tu_box(p, 0.35f, 0.25f, 0.06f);
    const de_vec2 hexp[6] = {{0.5f, 0},  {0.25f, 0.43f},   {-0.25f, 0.43f},
                             {-0.5f, 0}, {-0.25f, -0.43f}, {0.25f, -0.43f}};
    kinds[4] = tu_polygon(p, hexp, 6, 0.0f);
    kinds[5] = tu_circle(p, 0.15f, (de_vec2){0.05f, 0});
    s->mover_count = movers;
    for (u32 k = 0; k < movers; ++k) {
        s->x0[k] = 196.0 * de_rng_f64(&r) - 98.0;
        s->y0[k] = 0.5 + 16.0 * de_rng_f64(&r);
        s->w[k] = 0.005f + 0.02f * de_rng_f32(&r); /* up to ~4 m/s */
        s->amp[k] = 0.5f + 3.0f * de_rng_f32(&r);
        s->spin[k] = 0.1f * de_rng_f32(&r) - 0.05f;
        de_transform2d xf;
        sweep_pose(s, k, 0, &xf);
        const u32 flags = (k % 23u) == 0u ? (u32)DE_BODY_FLAG_TRIGGER : 0u;
        s->movers[k] = tu_body(p, DE_BODY_DYNAMIC, kinds[k % 6u], xf, flags);
        ASSERT_TRUE(DE_HANDLE_VALID(s->movers[k]));
    }
}

static void sweep_tick(sweep *s, u32 t) {
    for (u32 k = 0; k < s->mover_count; ++k) {
        de_transform2d xf;
        sweep_pose(s, k, t, &xf);
        ASSERT_TRUE(de_body2d_set_transform(s->p, s->movers[k], &xf));
    }
}

#define SWEEP_TICKS 300u

/* Brute-force check of the broadphase output and the chain midphase after a step. */
static void check_brute_force(de_physics2d_world *p) {
    const f32 spec = de__contacts2d_speculative(p);
    const de__bp2d_pair *pairs;
    u32 np;
    de__bp2d_pairs(p, &pairs, &np);
    const de__body2d_pool *b = &p->bodies;
    u32 k = 0;
    for (u32 i = 0; i < b->high_water; ++i) {
        if (!b->alive[i] || de__aabb2d_is_empty(b->world_aabb[i]))
            continue;
        for (u32 j = i + 1u; j < b->high_water; ++j) {
            if (!b->alive[j] || de__aabb2d_is_empty(b->world_aabb[j]) ||
                !de__bp2d_should_collide(p, i, j) ||
                !de__aabb2d_within(&b->world_aabb[i], &b->world_aabb[j], spec))
                continue;
            ASSERT_LT(k, np);
            ASSERT_EQ(pairs[k].a, i);
            ASSERT_EQ(pairs[k].b, j);
            ++k;
        }
    }
    ASSERT_EQ(k, np);
    /* Sub-pairs: one per non-chain pair, one per overlapping segment for chain pairs. */
    const de__contact_key2d *sub;
    u32 ns;
    de__contacts2d_subpairs(p, &sub, &ns);
    u32 s = 0;
    for (u32 q = 0; q < np; ++q) {
        const de__shape2d *sa = de__shape2d_get(p, b->shape[pairs[q].a]);
        const de__shape2d *sb = de__shape2d_get(p, b->shape[pairs[q].b]);
        const bool ca = sa->kind == DE_SHAPE2D_CHAIN, cb = sb->kind == DE_SHAPE2D_CHAIN;
        if (!ca && !cb) {
            ASSERT_LT(s, ns);
            ASSERT_EQ(sub[s].child, DE__P2D_CHILD_NONE);
            ++s;
            continue;
        }
        const u32 chain = ca ? pairs[q].a : pairs[q].b, other = ca ? pairs[q].b : pairs[q].a;
        const de__chain2d *c = &(ca ? sa : sb)->u.chain;
        const de_aabb2d box = de__contacts2d_chain_box(p, chain, other, spec);
        for (u32 g = 0; g < c->segment_count; ++g) {
            const de__chain_segment *x = &c->segments[g];
            const de_aabb2d sbx = {de__v2_min(x->p1, x->p2), de__v2_max(x->p1, x->p2)};
            if (!de__aabb2d_overlap(&sbx, &box))
                continue;
            ASSERT_LT(s, ns);
            ASSERT_EQ(sub[s].a, pairs[q].a);
            ASSERT_EQ(sub[s].b, pairs[q].b);
            ASSERT_EQ(sub[s].child, g);
            ++s;
        }
    }
    ASSERT_EQ(s, ns);
}

/* Runs the 1,000-body sweep for 300 ticks; fills the vector entries. */
static u32 run_sweep(u32 workers, bool brute_force, tu_golden *out) {
    de_task_system *ts = tasks_new(workers);
    static sweep s;
    sweep_build(&s, 1000, ts, NULL);
    tu_hash block = {0};
    u64 manifolds = 0, points = 0, begins = 0, ends = 0, chain_m = 0;
    u32 n = 0;
    for (u32 t = 0; t < SWEEP_TICKS; ++t) {
        sweep_tick(&s, t);
        de_physics2d_step(s.p, DT);
        const u64 ck = de_physics2d_checksum(s.p);
        tu_h_u32(&block, (u32)ck);
        tu_h_u32(&block, (u32)(ck >> 32));
        de__contact2d_stats st;
        de__contacts2d_stats_get(s.p, &st);
        manifolds += st.manifolds;
        points += st.points;
        begins += st.begins;
        ends += st.ends;
        EXPECT_EQ(st.dropped, 0u);
        const de__manifold2d *m;
        u32 nm;
        de__contacts2d_manifolds(s.p, &m, &nm);
        for (u32 i = 0; i < nm; ++i)
            chain_m += m[i].key.child != DE__P2D_CHILD_NONE ? 1u : 0u;
        if (brute_force && (t == 0 || t == 150 || t == SWEEP_TICKS - 1u))
            check_brute_force(s.p);
        if ((t + 1u) % 30u == 0u) {
            snprintf(out[n].name, sizeof out[n].name, "ticks%03u", t - 29u);
            out[n++].value = block.h;
            block.h = 0;
        }
    }
    snprintf(out[n].name, sizeof out[n].name, "final_checksum");
    out[n++].value = de_physics2d_checksum(s.p);
    const char *names[5] = {"manifolds", "points", "begins", "ends", "chain_manifolds"};
    const u64 vals[5] = {manifolds, points, begins, ends, chain_m};
    for (u32 i = 0; i < 5; ++i) {
        snprintf(out[n].name, sizeof out[n].name, "%s", names[i]);
        out[n++].value = vals[i];
    }
    /* The scene is meaningful: many contacts of every flavour. */
    EXPECT_GT(manifolds, 100000u);
    EXPECT_GT(begins, 2000u);
    EXPECT_GT(ends, 2000u);
    EXPECT_GT(chain_m, 2000u);
    de_physics2d_destroy(s.p);
    de_task_system_destroy(ts);
    return n;
}

static const char k_pipeline_header[] =
    "# pipeline2d_golden.txt: a 1,000-body kinematic sweep over a wavy chain, slabs and segments\n"
    "# (circles, capsules, boxes, rounded boxes, hexagons, triggers; poses set every tick), 300\n"
    "# ticks: XXH3-64 of the de_physics2d_checksum of every tick per block of 30 ticks, the final\n"
    "# checksum and the summed manifold, point, begin, end and chain-manifold counts (DENG-253).\n"
    "# Generated by test_physics2d_manifold with DE_REGEN_GOLDEN=1 (no task system); never edit\n"
    "# by hand. x86-64 and arm64, and every worker count, must reproduce every line.\n";

TEST(physics2d_manifold, pipeline_golden_no_tasks) {
    static tu_golden got[TU_GOLDEN_MAX];
    const u32 n = run_sweep(0, true, got);
    if (tu_golden_check(DE_PHYSICS2D_PIPELINE_GOLDEN, k_pipeline_header, got, n))
        SKIP("DE_REGEN_GOLDEN=1: rewrote " DE_PHYSICS2D_PIPELINE_GOLDEN);
}

TEST(physics2d_manifold, pipeline_golden_four_workers) {
    static tu_golden got[TU_GOLDEN_MAX];
    const u32 n = run_sweep(4, false, got);
    const char *regen = getenv("DE_REGEN_GOLDEN");
    if (regen != NULL && strcmp(regen, "1") == 0)
        SKIP("DE_REGEN_GOLDEN=1: the vector is written by pipeline_golden_no_tasks");
    (void)tu_golden_check(DE_PHYSICS2D_PIPELINE_GOLDEN, k_pipeline_header, got, n);
}

TEST(physics2d_manifold, worker_count_invariance) {
    /* A smaller scene, tick by tick: no task system, 1 and 4 workers (the full sweep runs with
     * none and 4 against the vector). */
    static u64 ck[3][120];
    const u32 workers[3] = {0, 1, 4};
    for (u32 w = 0; w < 3; ++w) {
        de_task_system *ts = tasks_new(workers[w]);
        static sweep s;
        sweep_build(&s, 300, ts, NULL);
        for (u32 t = 0; t < 120; ++t) {
            sweep_tick(&s, t);
            de_physics2d_step(s.p, DT);
            ck[w][t] = de_physics2d_checksum(s.p);
        }
        de_physics2d_destroy(s.p);
        de_task_system_destroy(ts);
    }
    for (u32 t = 0; t < 120; ++t) {
        ASSERT_EQ(ck[1][t], ck[0][t]);
        ASSERT_EQ(ck[2][t], ck[0][t]);
        if (t > 0)
            EXPECT_NE(ck[0][t], ck[0][t - 1]);
    }
}

TEST(physics2d_manifold, no_alloc_after_warmup) {
    const u32 workers[2] = {0, 4};
    for (u32 w = 0; w < 2; ++w) {
        de_task_system *ts = tasks_new(workers[w]);
        de_tracking_allocator tr;
        de_allocator a = de_tracking_allocator_init(&tr, NULL, DE_MEM_PHYSICS);
        static sweep s;
        sweep_build(&s, 300, ts, &a);
        sweep_tick(&s, 0);
        de_physics2d_step(s.p, DT); /* warm-up: creates the broadphase and the contacts */
        const de_tracking_stats before = de_tracking_allocator_stats(&tr);
        for (u32 t = 1; t < 100; ++t) {
            sweep_tick(&s, t);
            if (t % 25u == 0u) { /* destroy and create bodies too */
                const de_body victim = s.movers[t];
                de_transform2d xf;
                ASSERT_TRUE(de_body2d_get_transform(s.p, victim, &xf));
                de_body2d_desc bd;
                memset(&bd, 0, sizeof bd);
                bd.type = DE_BODY_DYNAMIC;
                bd.shape = s.p->bodies.shape[tu_index(victim)];
                bd.transform = xf;
                de_body2d_destroy(s.p, victim);
                s.movers[t] = de_body2d_create(s.p, &bd);
            }
            de_physics2d_step(s.p, DT);
            (void)de_physics2d_checksum(s.p);
        }
        const de_tracking_stats after = de_tracking_allocator_stats(&tr);
        EXPECT_EQ(after.alloc_count, before.alloc_count);
        EXPECT_EQ(after.realloc_count, before.realloc_count);
        EXPECT_EQ(after.free_count, before.free_count);
        de_physics2d_destroy(s.p);
        EXPECT_EQ(de_tracking_allocator_stats(&tr).bytes_live, 0u);
        de_task_system_destroy(ts);
    }
}

TEST(physics2d_manifold, checksum_covers_contacts) {
    static sweep a, b;
    sweep_build(&a, 120, NULL, NULL);
    sweep_build(&b, 120, NULL, NULL);
    EXPECT_EQ(de_physics2d_checksum(a.p), de_physics2d_checksum(b.p));
    for (u32 t = 0; t < 5; ++t) {
        sweep_tick(&a, t);
        sweep_tick(&b, t);
        de_physics2d_step(a.p, DT);
        de_physics2d_step(b.p, DT);
    }
    ASSERT_EQ(de_physics2d_checksum(a.p), de_physics2d_checksum(b.p));
    u32 n = 0;
    de__manifold2d *m = de__contacts2d_manifolds_mut(b.p, &n);
    u32 solid = 0;
    while (solid < n && m[solid].point_count == 0)
        ++solid;
    ASSERT_LT(solid, n);
    const u64 base = de_physics2d_checksum(b.p);
    de__contact2d *pt = &m[solid].points[0];
    f32 *fields[] = {&pt->normal_impulse, &pt->tangent_impulse, &pt->separation,
                     &pt->local_a.x,      &pt->local_b.y,       &m[solid].normal.x};
    for (u32 f = 0; f < sizeof fields / sizeof fields[0]; ++f) {
        const f32 keep = *fields[f];
        *fields[f] = keep + 0.5f;
        EXPECT_NE(de_physics2d_checksum(b.p), base);
        *fields[f] = keep;
    }
    u32 *ufields[] = {&pt->feature_id, &m[solid].id, &m[solid].flags, &m[solid].key.child,
                      &m[solid].gen_a};
    for (u32 f = 0; f < sizeof ufields / sizeof ufields[0]; ++f) {
        const u32 keep = *ufields[f];
        *ufields[f] = keep ^ 0x40u;
        EXPECT_NE(de_physics2d_checksum(b.p), base);
        *ufields[f] = keep;
    }
    EXPECT_EQ(de_physics2d_checksum(b.p), base);
    de_physics2d_destroy(a.p);
    de_physics2d_destroy(b.p);
}

TEST(physics2d_manifold, overflow_keeps_lowest_keys) {
    static sweep a, b;
    sweep_build(&a, 300, NULL, NULL);
    sweep_build(&b, 300, NULL, NULL);
    de__contacts2d_desc cd = {0.0f, 40u, 0u};
    ASSERT_EQ(de__contacts2d_create(b.p, &cd), DE_OK);
    for (u32 t = 0; t < 10; ++t) {
        sweep_tick(&a, t);
        sweep_tick(&b, t);
        de_physics2d_step(a.p, DT);
        de_physics2d_step(b.p, DT);
        const de__manifold2d *ma, *mb;
        u32 na, nb;
        de__contacts2d_manifolds(a.p, &ma, &na);
        de__contacts2d_manifolds(b.p, &mb, &nb);
        ASSERT_GT(na, 40u);
        ASSERT_EQ(nb, 40u);
        for (u32 i = 0; i < nb; ++i)
            EXPECT_EQ(de__contact_key2d_cmp(&ma[i].key, &mb[i].key), 0);
        de__contact2d_stats st;
        de__contacts2d_stats_get(b.p, &st);
        EXPECT_GT(st.dropped, 0u);
    }
    de_physics2d_destroy(a.p);
    de_physics2d_destroy(b.p);
}
