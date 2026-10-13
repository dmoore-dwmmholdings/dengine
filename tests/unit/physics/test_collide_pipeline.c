/* test_collide_pipeline.c: the collision pipeline of de_physics_step end to end (DENG-135, SPEC
 * §13.1, §13.2, §5.7, §8): a scene of every shape kind on scripted motion with body churn and
 * triggers; manifold invariants and begin / end bookkeeping every tick; identical per-tick
 * checksums with tasks == NULL, 1 and 4 workers (also when the manifold pool overflows); checksum
 * sensitivity; no allocation after warm-up. */
#include "de_test.h"

#include <dengine/de_physics.h>
#include <dengine/de_task.h>

#include "physics/contact/manifold.h"
#include "physics/physics_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- scene -------------------------------------------------------------------------------- */

#define SC_MOVERS 72u
#define SC_TICKS  240u

typedef struct scene {
    de_physics_world *p;
    de_shape kinds[8]; /* sphere, capsule, box, cylinder, hull, compound, small box, thin box */
    de_body movers[SC_MOVERS];
    u32 kind_of[SC_MOVERS];
} scene;

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

static de_shape mk(de_physics_world *p, const de_shape_desc *d) {
    const de_shape s = de_shape_create(p, d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_body body(de_physics_world *p, de_body_type type, de_shape s, de_transform t, u32 flags) {
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

/* Mover k at tick t: a circle around its own center with a vertical bob through the ground. */
static de_transform mover_pose(u32 k, u32 t) {
    const f32 phase = 0.37f * (f32)k;
    const f32 w = 0.02f + 0.003f * (f32)(k % 7u);
    const f32 ang = phase + w * (f32)t;
    const f32 cx = -6.0f + 1.5f * (f32)(k % 9u);
    const f32 cy = -6.0f + 1.5f * (f32)(k / 9u);
    const f32 r = 0.6f + 0.1f * (f32)(k % 3u);
    const f64 x = (f64)(cx + r * de_dcos(ang));
    const f64 y = (f64)(cy + r * de_dsin(ang));
    const f64 z = (f64)(0.45f + 0.3f * de_dsin(1.3f * ang));
    const de_quat q = quat_axis_angle((de_vec3){de_dsin(phase), de_dcos(phase), 0.5f}, 0.5f * ang);
    return xform(x, y, z, q);
}

static void scene_build(scene *s, const de_physics_desc *pd) {
    memset(s, 0, sizeof *s);
    ASSERT_EQ(de_physics_create(pd, &s->p), DE_OK);
    de_physics_world *p = s->p;
    de_shape_desc d;

    /* Statics: a ground box, a terrain heightfield next to it, a mesh ramp, a compound. */
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = (de_vec3){5.0f, 8.0f, 0.5f};
    (void)body(p, DE_BODY_STATIC, mk(p, &d), xform(-3.0, 0.0, -0.5, quat_identity()), 0);
    static f32 heights[9 * 9];
    for (u32 j = 0; j < 9; ++j)
        for (u32 i = 0; i < 9; ++i)
            heights[j * 9 + i] = 0.1f * de_dsin(0.9f * (f32)i) * de_dcos(0.7f * (f32)j);
    heights[4 * 9 + 4] = DE_HEIGHTFIELD_HOLE;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_HEIGHTFIELD;
    d.heights = heights;
    d.rows = 9;
    d.cols = 9;
    d.scale = (de_vec3){1.0f, 2.0f, 1.0f};
    (void)body(p, DE_BODY_STATIC, mk(p, &d), xform(2.0, -8.0, 0.0, quat_identity()), 0);
    static const de_vec3 ramp_v[4] = {{-3, -1, 0}, {3, -1, 0}, {3, 1, 0.8f}, {-3, 1, 0.8f}};
    static const u32 ramp_i[6] = {0, 1, 2, 0, 2, 3};
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_MESH;
    d.vertices = ramp_v;
    d.vertex_count = 4;
    d.indices = ramp_i;
    d.index_count = 6;
    (void)body(p, DE_BODY_STATIC, mk(p, &d), xform(-3.0, 4.5, -0.3, quat_identity()), 0);

    /* Mover shapes. */
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_SPHERE;
    d.radius = 0.35f;
    s->kinds[0] = mk(p, &d);
    d.kind = DE_SHAPE_CAPSULE;
    d.radius = 0.2f;
    d.half_height = 0.3f;
    s->kinds[1] = mk(p, &d);
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = (de_vec3){0.3f, 0.25f, 0.2f};
    s->kinds[2] = mk(p, &d);
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CYLINDER;
    d.radius = 0.25f;
    d.half_height = 0.25f;
    s->kinds[3] = mk(p, &d);
    static const de_vec3 hull_pts[8] = {{-0.3f, -0.2f, -0.2f}, {0.3f, -0.25f, -0.2f},
                                        {0.0f, 0.35f, -0.2f},  {0.0f, 0.0f, 0.35f},
                                        {0.2f, 0.2f, 0.1f},    {-0.2f, 0.1f, 0.15f},
                                        {0.1f, -0.1f, -0.25f}, {-0.25f, -0.25f, 0.05f}};
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CONVEX;
    d.points = hull_pts;
    d.point_count = 8;
    s->kinds[4] = mk(p, &d);
    const de_shape children[2] = {s->kinds[0], s->kinds[2]};
    const de_transform child_x[2] = {xform(0.0, 0.0, 0.3, quat_identity()),
                                     xform(0.0, 0.0, -0.2, quat_identity())};
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_COMPOUND;
    d.children = children;
    d.child_xforms = child_x;
    d.child_count = 2;
    s->kinds[5] = mk(p, &d);
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = (de_vec3){0.15f, 0.15f, 0.15f};
    s->kinds[6] = mk(p, &d);
    d.half_extents = (de_vec3){0.6f, 0.4f, 0.05f};
    s->kinds[7] = mk(p, &d);

    for (u32 k = 0; k < SC_MOVERS; ++k) {
        s->kind_of[k] = k % 8u;
        /* Every 10th mover is kinematic, every 13th a trigger. */
        const de_body_type type = (k % 10u == 9u) ? DE_BODY_KINEMATIC : DE_BODY_DYNAMIC;
        const u32 flags = (k % 13u == 5u) ? (u32)DE_BODY_FLAG_TRIGGER : 0u;
        s->movers[k] = body(p, type, s->kinds[s->kind_of[k]], mover_pose(k, 0), flags);
    }
}

/* Moves every mover; every 40 ticks mover (t / 40) % SC_MOVERS is destroyed and recreated (its
 * index is reused at once). */
static void scene_tick(scene *s, u32 t) {
    for (u32 k = 0; k < SC_MOVERS; ++k) {
        const de_transform x = mover_pose(k, t);
        ASSERT_TRUE(de_body_set_transform(s->p, s->movers[k], &x));
    }
    if (t > 0 && t % 40u == 0u) {
        const u32 k = (t / 40u * 7u) % SC_MOVERS;
        de_body_destroy(s->p, s->movers[k]);
        s->movers[k] =
            body(s->p, DE_BODY_DYNAMIC, s->kinds[(s->kind_of[k] + 1u) % 8u], mover_pose(k, t), 0);
        s->kind_of[k] = (s->kind_of[k] + 1u) % 8u;
    }
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

/* Runs the scene; checksums[t] after tick t. */
static void run(u32 workers, u32 max_manifolds, u64 *checksums, u32 ticks, de__contact_stats *st) {
    de_task_system *ts = tasks_new(workers);
    de_physics_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.max_bodies = 256;
    pd.tasks = ts;
    static scene s;
    scene_build(&s, &pd);
    if (max_manifolds != 0) {
        de__contacts_desc cd = {0.0f, max_manifolds};
        ASSERT_EQ(de__contacts_create(s.p, &cd), DE_OK);
    }
    for (u32 t = 0; t < ticks; ++t) {
        scene_tick(&s, t);
        de_physics_step(s.p, 1.0f / 60.0f);
        checksums[t] = de_physics_checksum(s.p);
    }
    if (st != NULL)
        de__contacts_stats(s.p, st);
    de_physics_destroy(s.p);
    de_task_system_destroy(ts);
}

/* ---- tests -------------------------------------------------------------------------------- */

static bool key_eq(const de__contact_key *a, const de__contact_key *b) {
    return de__contact_key_cmp(a, b) == 0;
}

TEST(physics_pipeline, manifold_invariants_and_touch_bookkeeping) {
    de_physics_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.max_bodies = 256;
    static scene s;
    scene_build(&s, &pd);
    de__manifold *prev = (de__manifold *)malloc(sizeof(de__manifold) * 4096u);
    ASSERT_TRUE(prev != NULL);
    u32 n_prev = 0;
    u32 ticks_with_contacts = 0, total_begin = 0, total_end = 0, trigger_ticks = 0, accum = 0;
    u32 spec_far = 0; /* DENG-219: points beyond the 2 cm margin (fast pairs) */
    u32 kinds_seen = 0, pairs_seen = 0;
    for (u32 t = 0; t < SC_TICKS; ++t) {
        scene_tick(&s, t);
        de_physics_step(s.p, 1.0f / 60.0f);
        const de__manifold *m = NULL;
        u32 n = 0;
        de__contacts_manifolds(s.p, &m, &n);
        const de__contact_end *e = NULL;
        u32 ne = 0;
        de__contacts_ended(s.p, &e, &ne);
        ticks_with_contacts += n > 0 ? 1u : 0u;
        de__contact_stats st;
        de__contacts_stats(s.p, &st);
        EXPECT_EQ(st.manifolds, n);
        EXPECT_EQ(st.ends, ne);
        EXPECT_EQ(st.dropped, 0u);
        pairs_seen = st.pairs > pairs_seen ? st.pairs : pairs_seen;
        u32 points = 0;
        for (u32 i = 0; i < n; ++i) {
            const de__manifold *x = &m[i];
            if (i > 0)
                ASSERT_LT(de__contact_key_cmp(&m[i - 1].key, &x->key), 0);
            ASSERT_LT(x->key.a, x->key.b);
            ASSERT_TRUE(de__body_alive(s.p, x->key.a) && de__body_alive(s.p, x->key.b));
            EXPECT_EQ(x->gen_a, s.p->bodies.generation[x->key.a]);
            EXPECT_EQ(x->gen_b, s.p->bodies.generation[x->key.b]);
            const f32 nl = de__v3_dot(x->normal, x->normal);
            EXPECT_FLOAT_EQ(nl, 1.0f, 1e-4f);
            const bool trig = ((s.p->bodies.flags[x->key.a] | s.p->bodies.flags[x->key.b]) &
                               (u32)DE_BODY_FLAG_TRIGGER) != 0;
            EXPECT_EQ((x->flags & (u32)DE__MANIFOLD_TRIGGER) != 0, trig);
            if (trig) {
                EXPECT_EQ(x->point_count, 0u);
                ++trigger_ticks;
            } else {
                EXPECT_TRUE(x->point_count >= 1u && x->point_count <= 4u);
            }
            accum += (x->flags & (u32)DE__MANIFOLD_ACCUMULATE) ? 1u : 0u;
            points += x->point_count;
            for (u32 a = 0; a < x->point_count; ++a) {
                /* DENG-219: the 2 cm margin, or up to physics.ccd.speculative_max (1 m) for a
                 * pair that approaches fast (per-pair speculative distance). */
                EXPECT_TRUE(x->points[a].separation <= 1.0000001f);
                spec_far += x->points[a].separation > 0.0200001f ? 1u : 0u;
                for (u32 b = a + 1; b < x->point_count; ++b)
                    EXPECT_NE(x->points[a].feature_id, x->points[b].feature_id);
            }
            for (u32 j = 0; j < i; ++j)
                ASSERT_NE(m[j].id, x->id); /* ids unique among live manifolds */
            /* BEGIN exactly when the key (same bodies) was not live last tick. */
            bool was = false;
            for (u32 j = 0; j < n_prev; ++j)
                if (key_eq(&prev[j].key, &x->key) && prev[j].gen_a == x->gen_a &&
                    prev[j].gen_b == x->gen_b &&
                    ((prev[j].flags ^ x->flags) & (u32)DE__MANIFOLD_TRIGGER) == 0) {
                    was = true;
                    EXPECT_EQ(prev[j].id, x->id);
                }
            EXPECT_EQ((x->flags & (u32)DE__MANIFOLD_BEGIN) != 0, !was);
            EXPECT_EQ((x->flags & (u32)DE__MANIFOLD_PERSIST) != 0, was);
            total_begin += was ? 0u : 1u;
            kinds_seen |= 1u << (u32)de__shape_get(s.p, s.p->bodies.shape[x->key.a])->kind;
            kinds_seen |= 1u << (u32)de__shape_get(s.p, s.p->bodies.shape[x->key.b])->kind;
        }
        EXPECT_EQ(st.points, points);
        /* END exactly for the keys live last tick and not now. */
        u32 vanished = 0;
        for (u32 j = 0; j < n_prev; ++j) {
            bool still = false;
            for (u32 i = 0; i < n; ++i)
                still = still || (key_eq(&prev[j].key, &m[i].key) && prev[j].gen_a == m[i].gen_a &&
                                  prev[j].gen_b == m[i].gen_b &&
                                  ((prev[j].flags ^ m[i].flags) & (u32)DE__MANIFOLD_TRIGGER) == 0);
            if (still)
                continue;
            bool listed = false;
            for (u32 k = 0; k < ne; ++k)
                listed = listed || (key_eq(&e[k].key, &prev[j].key) && e[k].id == prev[j].id);
            EXPECT_TRUE(listed);
            ++vanished;
        }
        EXPECT_EQ(vanished, ne);
        for (u32 k = 1; k < ne; ++k)
            EXPECT_LE(de__contact_key_cmp(&e[k - 1].key, &e[k].key), 0);
        total_end += ne;
        /* Per-body lists cover every manifold twice. */
        u32 listed = 0;
        for (u32 b = 0; b < s.p->bodies.high_water; ++b) {
            u32 ln = 0;
            de__contacts_body(s.p, b, NULL, &ln);
            listed += ln;
        }
        EXPECT_EQ(listed, 2u * n);
        ASSERT_LE(n, 4096u);
        memcpy(prev, m, sizeof(de__manifold) * n);
        n_prev = n;
    }
    free(prev);
    /* The scene really exercises the pipeline. */
    EXPECT_EQ(ticks_with_contacts, SC_TICKS);
    EXPECT_GT(pairs_seen, DE__CONTACT_PAIR_GRAIN); /* parallel runs use several chunks */
    EXPECT_GT(total_begin, 100u);
    EXPECT_GT(total_end, 100u);
    EXPECT_GT(trigger_ticks, 0u);
    EXPECT_GT(accum, 0u);
    printf("  points beyond the 2 cm margin (per-pair speculative distance): %u\n", spec_far);
    EXPECT_EQ(kinds_seen, 0xFFu); /* every shape kind takes part */
    de_physics_destroy(s.p);
}

TEST(physics_pipeline, worker_count_invariance) {
    static u64 c0[SC_TICKS], c1[SC_TICKS], c4[SC_TICKS];
    run(0, 0, c0, SC_TICKS, NULL);
    run(1, 0, c1, SC_TICKS, NULL);
    run(4, 0, c4, SC_TICKS, NULL);
    for (u32 t = 0; t < SC_TICKS; ++t) {
        ASSERT_EQ(c1[t], c0[t]);
        ASSERT_EQ(c4[t], c0[t]);
        if (t > 0)
            EXPECT_NE(c0[t], c0[t - 1]);
    }
    /* A second run gives the same checksums. */
    static u64 again[SC_TICKS];
    run(0, 0, again, SC_TICKS, NULL);
    EXPECT_MEMEQ(again, c0, sizeof c0);
}

TEST(physics_pipeline, overflow_keeps_lowest_keys_and_stays_deterministic) {
    static u64 c0[60], c4[60];
    de__contact_stats s0, s4;
    run(0, 24, c0, 60, &s0);
    run(4, 24, c4, 60, &s4);
    EXPECT_MEMEQ(c0, c4, sizeof c0);
    EXPECT_GT(s0.overflow_ticks, 0u);
    EXPECT_EQ(s0.overflow_ticks, s4.overflow_ticks);
    EXPECT_LE(s0.manifolds, 24u);
    EXPECT_EQ(s0.serial_reruns, 0u); /* no task system: the pass is serial from the start */
    EXPECT_GT(s4.serial_reruns, 0u);

    /* The kept manifolds are a prefix (in key order) of the unlimited result. */
    de_physics_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.max_bodies = 256;
    static scene a, b;
    scene_build(&a, &pd);
    scene_build(&b, &pd);
    de__contacts_desc cd = {0.0f, 24u};
    ASSERT_EQ(de__contacts_create(b.p, &cd), DE_OK);
    for (u32 t = 0; t < 30; ++t) {
        scene_tick(&a, t);
        scene_tick(&b, t);
        de_physics_step(a.p, 1.0f / 60.0f);
        de_physics_step(b.p, 1.0f / 60.0f);
        const de__manifold *ma = NULL, *mb = NULL;
        u32 na = 0, nb = 0;
        de__contacts_manifolds(a.p, &ma, &na);
        de__contacts_manifolds(b.p, &mb, &nb);
        ASSERT_GT(na, 24u);
        ASSERT_LE(nb, 24u);
        ASSERT_GT(nb, 0u);
        for (u32 i = 0; i < nb; ++i)
            EXPECT_TRUE(key_eq(&ma[i].key, &mb[i].key));
        de__contact_stats st;
        de__contacts_stats(b.p, &st);
        EXPECT_GT(st.dropped, 0u);
        EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    }
    de_physics_destroy(a.p);
    de_physics_destroy(b.p);
}

TEST(physics_pipeline, checksum_covers_contacts) {
    de_physics_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.max_bodies = 256;
    static scene a, b;
    scene_build(&a, &pd);
    scene_build(&b, &pd);
    EXPECT_EQ(de_physics_checksum(a.p), de_physics_checksum(b.p));
    for (u32 t = 0; t < 5; ++t) {
        scene_tick(&a, t);
        scene_tick(&b, t);
        de_physics_step(a.p, 1.0f / 60.0f);
        de_physics_step(b.p, 1.0f / 60.0f);
    }
    ASSERT_EQ(de_physics_checksum(a.p), de_physics_checksum(b.p));
    u32 n = 0;
    de__manifold *m = de__contacts_manifolds_mut(b.p, &n);
    ASSERT_GT(n, 1u);
    u32 solid = 0;
    while (solid < n && m[solid].point_count == 0)
        ++solid;
    ASSERT_LT(solid, n);
    const u64 base = de_physics_checksum(b.p);
    /* Every contact field is part of it. */
    de__contact *pt = &m[solid].points[0];
    f32 *fields[] = {&pt->normal_impulse, &pt->tangent_impulse[0], &pt->tangent_impulse[1],
                     &pt->separation,     &pt->local_a.x,          &pt->local_b.z,
                     &m[solid].normal.y};
    for (u32 f = 0; f < sizeof fields / sizeof fields[0]; ++f) {
        const f32 keep = *fields[f];
        *fields[f] = keep + 0.5f;
        EXPECT_NE(de_physics_checksum(b.p), base);
        *fields[f] = keep;
        EXPECT_EQ(de_physics_checksum(b.p), base);
    }
    u32 *ufields[] = {&pt->feature_id, &m[solid].id, &m[solid].flags, &m[solid].key.sub_a,
                      &m[solid].serial};
    for (u32 f = 0; f < sizeof ufields / sizeof ufields[0]; ++f) {
        const u32 keep = *ufields[f];
        *ufields[f] = keep ^ 0x40u;
        EXPECT_NE(de_physics_checksum(b.p), base);
        *ufields[f] = keep;
    }
    EXPECT_EQ(de_physics_checksum(b.p), base);
    de_physics_destroy(a.p);
    de_physics_destroy(b.p);
}

TEST(physics_pipeline, no_alloc_after_warmup) {
    const u32 worker_counts[2] = {0, 4};
    for (u32 w = 0; w < 2; ++w) {
        de_task_system *ts = tasks_new(worker_counts[w]);
        de_tracking_allocator t;
        de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
        de_physics_desc pd;
        memset(&pd, 0, sizeof pd);
        pd.max_bodies = 256;
        pd.allocator = &a;
        pd.tasks = ts;
        static scene s;
        scene_build(&s, &pd);
        scene_tick(&s, 0);
        de_physics_step(s.p, 1.0f / 60.0f); /* warm-up: creates the broadphase and contacts */
        const de_tracking_stats before = de_tracking_allocator_stats(&t);
        for (u32 tick = 1; tick < 200; ++tick) {
            scene_tick(&s, tick); /* includes destroy + create */
            de_physics_step(s.p, 1.0f / 60.0f);
            (void)de_physics_checksum(s.p);
        }
        const de_tracking_stats after = de_tracking_allocator_stats(&t);
        EXPECT_EQ(after.alloc_count, before.alloc_count);
        EXPECT_EQ(after.realloc_count, before.realloc_count);
        EXPECT_EQ(after.free_count, before.free_count);
        de_physics_destroy(s.p);
        EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
        de_task_system_destroy(ts);
    }
}

TEST(physics_pipeline, contacts_create_errors_and_oom) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.max_bodies = 128;
    pd.allocator = &a;
    de_physics_world *p = NULL;
    ASSERT_EQ(de_physics_create(&pd, &p), DE_OK);
    /* Fail each allocation of create in turn (broadphase blocks, then the contacts blocks):
     * nothing is left behind and the slot stays empty. */
    for (i64 k = 0; k < 6; ++k) {
        de_tracking_allocator_fail_after(&t, k);
        const de_result r = de__contacts_create(p, NULL);
        de_tracking_allocator_fail_after(&t, -1);
        if (r == DE_OK)
            break;
        EXPECT_EQ(r, DE_ERR_OUT_OF_MEMORY);
        EXPECT_TRUE(de__contacts_get(p) == NULL);
    }
    ASSERT_TRUE(de__contacts_get(p) != NULL);
    de__contact_stats st;
    de__contacts_stats(p, &st);
    EXPECT_EQ(st.max_manifolds, DE__CONTACT_MIN_MANIFOLDS);
    /* A step whose contacts cannot be created leaves the error context and goes on. */
    de_physics_world *q = NULL;
    ASSERT_EQ(de_physics_create(&pd, &q), DE_OK);
    de_tracking_allocator_fail_after(&t, 0);
    de_physics_step(q, 1.0f / 60.0f);
    de_tracking_allocator_fail_after(&t, -1);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_physics_tick(q), 1u);
    de_physics_step(q, 1.0f / 60.0f);
    EXPECT_TRUE(de__contacts_get(q) != NULL);
    de_physics_destroy(q);
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}
