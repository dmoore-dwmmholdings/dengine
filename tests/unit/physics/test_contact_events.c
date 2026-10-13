/* test_contact_events.c: contact and trigger events of physics.step (DENG-223): one BEGIN and one
 * END per transition, triggers without a collision response, body-pair order, impulses, points,
 * normals, and ENDs that name despawned entities. */
#include "ecs_test_util.h"

#define MAX_EV 64u

/* Events of one kind that involve entity `x` among ev[0..n). */
static u32 count_kind(const de_contact_event *ev, u32 n, de_contact_kind kind, de_entity x) {
    u32 c = 0;
    for (u32 i = 0; i < n && i < MAX_EV; ++i)
        if (ev[i].kind == kind && (ev[i].a.bits == x.bits || ev[i].b.bits == x.bits))
            ++c;
    return c;
}

/* true when the event is about entities x and y (in either order). */
static bool is_pair(const de_contact_event *ev, de_entity x, de_entity y) {
    return (ev->a.bits == x.bits && ev->b.bits == y.bits) ||
           (ev->a.bits == y.bits && ev->b.bits == x.bits);
}

/* The summed normal impulse of the manifolds of body pair (i, j) times the substeps. */
static f32 pair_impulse(const de_physics_world *p, u32 i, u32 j) {
    const de__manifold *m;
    u32 n;
    de__contacts_manifolds(p, &m, &n);
    const u32 a = i < j ? i : j, b = i < j ? j : i;
    f32 sum = 0.0f;
    for (u32 k = 0; k < n; ++k)
        if (m[k].key.a == a && m[k].key.b == b)
            for (u32 q = 0; q < m[k].point_count; ++q)
                sum += m[k].points[q].normal_impulse;
    return sum * (f32)p->substeps;
}

TEST(physics_contact_events, begin_and_end_once) {
    pe_env e;
    pe_init(&e, 0);
    const de_entity ground = pe_ground(&e);
    const de_entity ball = pe_spawn_body(&e, DE_BODY_DYNAMIC, st_sphere(e.p, 0.5f), 0.0, 0.0, 2.0);
    u32 begins = 0, ends = 0, others = 0;
    de_contact_event first;
    memset(&first, 0, sizeof first);
    for (u32 t = 0; t < 150; ++t) {
        ASSERT_EQ(pe_tick(&e), DE_OK);
        de_contact_event ev[MAX_EV];
        const u32 n = pe_read_contacts(&e, ev, MAX_EV);
        for (u32 i = 0; i < n; ++i) {
            if (ev[i].kind == DE_CONTACT_BEGIN) {
                if (begins == 0)
                    first = ev[i];
                ++begins;
            } else if (ev[i].kind == DE_CONTACT_END) {
                ++ends;
            } else {
                ++others;
            }
        }
    }
    EXPECT_EQ(begins, 1u); /* bounces and rest keep the one contact */
    EXPECT_EQ(ends, 0u);
    EXPECT_EQ(others, 0u);
    /* a / b in body index order (rigid bodies are made before static colliders, so the ball has
     * the lower index), normal from a to b, point on the ground's top under the ball. */
    EXPECT_LT(pe_index(&e, ball), pe_index(&e, ground));
    EXPECT_EQ(first.a.bits, ball.bits);
    EXPECT_EQ(first.b.bits, ground.bits);
    EXPECT_FLOAT_EQ(first.normal.z, -1.0f, 1e-4);
    EXPECT_FLOAT_EQ(first.point.x, 0.0, 1e-3);
    EXPECT_FLOAT_EQ(first.point.z, 0.0, 0.05);

    /* Lift the ball away: one END. */
    de_transform *xf = (de_transform *)de_entity_get(e.w, ball, e.xf);
    xf->position = de__d3(0.0, 0.0, 10.0);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    de_contact_event ev[MAX_EV];
    u32 n = pe_read_contacts(&e, ev, MAX_EV);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ((u32)ev[0].kind, (u32)DE_CONTACT_END);
    EXPECT_EQ(ev[0].a.bits, ball.bits);
    EXPECT_EQ(ev[0].b.bits, ground.bits);
    EXPECT_TRUE(ev[0].impulse == 0.0f);
    for (u32 t = 0; t < 10; ++t) {
        ASSERT_EQ(pe_tick(&e), DE_OK);
        n = pe_read_contacts(&e, ev, MAX_EV);
        EXPECT_EQ(n, 0u);
    }
    pe_free(&e);
}

TEST(physics_contact_events, triggers_enter_exit_without_response) {
    pe_env e;
    pe_init(&e, 0);
    /* A trigger slab between z = 1 and z = 3, with no ground below. */
    const de_entity zone = pe_spawn_static(&e, st_box(e.p, 2.0f, 2.0f, 1.0f), 0.0, 0.0, 2.0);
    ((DeCollider *)de_entity_get(e.w, zone, e.col))->is_trigger = true;
    const de_shape ball = st_sphere(e.p, 0.25f);
    const de_entity in = pe_spawn_body(&e, DE_BODY_DYNAMIC, ball, 0.0, 0.0, 4.0);
    const de_entity free_fall = pe_spawn_body(&e, DE_BODY_DYNAMIC, ball, 20.0, 0.0, 4.0);
    u32 enter = 0, exit = 0, solid = 0;
    for (u32 t = 0; t < 90; ++t) {
        ASSERT_EQ(pe_tick(&e), DE_OK);
        de_contact_event ev[MAX_EV];
        const u32 n = pe_read_contacts(&e, ev, MAX_EV);
        enter += count_kind(ev, n, DE_TRIGGER_ENTER, in);
        exit += count_kind(ev, n, DE_TRIGGER_EXIT, in);
        solid += count_kind(ev, n, DE_CONTACT_BEGIN, in) + count_kind(ev, n, DE_CONTACT_END, in);
        for (u32 i = 0; i < n; ++i) {
            if (ev[i].kind == DE_TRIGGER_ENTER) {
                EXPECT_TRUE(is_pair(&ev[i], zone, in));
                EXPECT_FLOAT_EQ(ev[i].point.x, 0.0, 1e-9); /* the visitor's origin */
                EXPECT_FLOAT_EQ(ev[i].point.z, 3.25, 0.2);
            }
        }
        /* No collision response: the same fall as the ball outside. */
        EXPECT_MEMEQ(&pe_xf(&e, in)->position.z, &pe_xf(&e, free_fall)->position.z, sizeof(f64));
    }
    EXPECT_EQ(enter, 1u);
    EXPECT_EQ(exit, 1u);
    EXPECT_EQ(solid, 0u);
    ST_LT(pe_xf(&e, in)->position.z, 0.0);
    pe_free(&e);
}

TEST(physics_contact_events, order_and_impulse) {
    pe_env e;
    pe_init(&e, 0);
    /* Four ground tiles, then four boxes resting exactly on them (spawned in reverse order, so
     * the body pairs are not in spawn order). */
    const de_shape tile = st_box(e.p, 1.0f, 1.0f, 0.5f);
    const de_shape cube = st_box(e.p, 0.5f, 0.5f, 0.5f); /* 1000 kg at the default density */
    de_entity tiles[4], boxes[4];
    for (u32 i = 0; i < 4; ++i)
        tiles[i] = pe_spawn_static(&e, tile, (f64)i * 3.0, 0.0, -0.5);
    for (u32 i = 4; i-- > 0;)
        boxes[i] = pe_spawn_body(&e, DE_BODY_DYNAMIC, cube, (f64)i * 3.0, 0.0, 0.5);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    de_contact_event ev[MAX_EV];
    const u32 n = pe_read_contacts(&e, ev, MAX_EV);
    ASSERT_EQ(n, 4u);
    u64 last = 0;
    for (u32 i = 0; i < n; ++i) {
        EXPECT_EQ((u32)ev[i].kind, (u32)DE_CONTACT_BEGIN);
        const u32 ia = pe_index(&e, ev[i].a), ib = pe_index(&e, ev[i].b);
        EXPECT_LT(ia, ib);
        const u64 key = ((u64)ia << 32) | ib;
        EXPECT_GT(key, last); /* body-pair order */
        last = key;
        /* The step's impulse: the manifold impulses (one substep's) times the substeps. */
        EXPECT_TRUE(ev[i].impulse == pair_impulse(e.p, ia, ib));
        ST_GT(ev[i].impulse, 0.0f);
        /* Box (lower index) to tile: down. */
        EXPECT_FLOAT_EQ(ev[i].normal.z, -1.0f, 1e-4);
        EXPECT_FLOAT_EQ(ev[i].point.z, 0.0, 0.01);
    }

    /* At rest, the step's impulse holds the weight for one tick: m g dt. */
    for (u32 t = 0; t < 60; ++t) {
        ASSERT_EQ(pe_tick(&e), DE_OK);
        (void)pe_read_contacts(&e, ev, MAX_EV);
    }
    const f32 mgdt = 1000.0f * 9.81f * ST_DT;
    EXPECT_FLOAT_EQ(pair_impulse(e.p, pe_index(&e, boxes[0]), pe_index(&e, tiles[0])), mgdt,
                    mgdt * 0.05f);

    /* Despawning two boxes in one tick sends two ENDs, in body-pair order, naming the despawned
     * entities. */
    u32 bi[4], ti[4];
    for (u32 i = 0; i < 4; ++i) {
        bi[i] = pe_index(&e, boxes[i]);
        ti[i] = pe_index(&e, tiles[i]);
    }
    de_entity_despawn(e.w, boxes[2]);
    de_entity_despawn(e.w, boxes[0]);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    const u32 m = pe_read_contacts(&e, ev, MAX_EV);
    ASSERT_EQ(m, 2u);
    const bool two_first = (bi[2] < ti[2] ? bi[2] : ti[2]) < (bi[0] < ti[0] ? bi[0] : ti[0]);
    const u32 f = two_first ? 2u : 0u, g = two_first ? 0u : 2u;
    EXPECT_EQ((u32)ev[0].kind, (u32)DE_CONTACT_END);
    EXPECT_EQ((u32)ev[1].kind, (u32)DE_CONTACT_END);
    EXPECT_TRUE(is_pair(&ev[0], tiles[f], boxes[f]));
    EXPECT_TRUE(is_pair(&ev[1], tiles[g], boxes[g]));
    EXPECT_FALSE(de_entity_alive(e.w, boxes[f]));

    /* Despawn and spawn in one tick (the body index is reused): END, then BEGIN. */
    de_entity_despawn(e.w, boxes[1]);
    const de_entity again = pe_spawn_body(&e, DE_BODY_DYNAMIC, cube, 3.0, 0.0, 0.5);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_EQ(pe_index(&e, again), bi[1]);
    const u32 k = pe_read_contacts(&e, ev, MAX_EV);
    ASSERT_EQ(k, 2u);
    EXPECT_EQ((u32)ev[0].kind, (u32)DE_CONTACT_END);
    EXPECT_TRUE(is_pair(&ev[0], tiles[1], boxes[1]));
    EXPECT_EQ((u32)ev[1].kind, (u32)DE_CONTACT_BEGIN);
    EXPECT_TRUE(is_pair(&ev[1], tiles[1], again));
    pe_free(&e);
}

TEST(physics_contact_events, bodies_outside_the_ecs) {
    pe_env e;
    pe_init(&e, 0);
    /* A ground body made directly (no owner) and an ECS ball: the event names the ball only. */
    st_body(e.p, DE_BODY_STATIC, st_box(e.p, 5.0f, 5.0f, 0.5f),
            st_xform(0.0, 0.0, -0.5, st_quat_identity()), 0.5f, 0.0f);
    const de_entity ball = pe_spawn_body(&e, DE_BODY_DYNAMIC, st_sphere(e.p, 0.5f), 0.0, 0.0, 0.5);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    de_contact_event ev[MAX_EV];
    ASSERT_EQ(pe_read_contacts(&e, ev, MAX_EV), 1u);
    EXPECT_EQ(ev[0].a.bits, 0u);
    EXPECT_EQ(ev[0].b.bits, ball.bits);
    /* Two bodies outside the ECS send nothing. */
    st_body(e.p, DE_BODY_DYNAMIC, st_sphere(e.p, 0.5f), st_xform(3.0, 0.0, 0.5, st_quat_identity()),
            0.5f, 0.0f);
    ASSERT_EQ(pe_tick(&e), DE_OK);
    EXPECT_EQ(pe_read_contacts(&e, ev, MAX_EV), 0u);
    pe_free(&e);
}
