/* test_sleep.c: sleeping and waking (DENG-218, SPEC §13.1): a resting pyramid falls asleep within
 * 2 s and then costs no solver work; it wakes, all at once, when a sphere hits it, when the user
 * changes a body or calls de_body_wake, when a joint links it to an awake body, when its ground
 * or one of its bodies is destroyed; bodies without DE_BODY_FLAG_ALLOW_SLEEP keep their island
 * awake; a moving kinematic body keeps what it touches awake; index reuse starts awake. */
#include "island_test_util.h"

#define PYR_ROWS 10u
#define PYR_N    55u /* 10 + 9 + ... + 1 */

/* Steps until every box of `py` sleeps; returns the tick count, or `max` + 1 when it never does. */
static u32 run_until_asleep(de_physics_world *p, const it_pyramid *py, u32 max) {
    for (u32 t = 1; t <= max; ++t) {
        de_physics_step(p, ST_DT);
        if (it_pyramid_sleeping(p, py) == py->count)
            return t;
    }
    return max + 1u;
}

static bool same_pose(de_transform a, de_transform b) {
    return memcmp(&a.position, &b.position, sizeof a.position) == 0 &&
           memcmp(&a.rotation, &b.rotation, sizeof a.rotation) == 0;
}

TEST(physics_sleep, pyramid_sleeps_within_2s_and_costs_nothing) {
    de_physics_world *p = st_world(128, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    static it_pyramid py;
    it_pyramid_build(&py, p, PYR_ROWS, 0.0, true, true);
    ASSERT_EQ(py.count, PYR_N);
    const u32 t = run_until_asleep(p, &py, 120);
    printf("  pyramid of %u boxes asleep after %u ticks (%.2f s)\n", py.count, t,
           (double)t * (double)ST_DT);
    ASSERT_TRUE(t <= 120u); /* 2 s */
    de__islands_stats is;
    de__islands_stats_get(p, &is);
    EXPECT_EQ(is.sleeping_bodies, PYR_N);
    EXPECT_EQ(is.slept_islands, 1u);
    for (u32 i = 0; i < py.count; ++i) {
        EXPECT_TRUE(st_lenf(st_vel(p, py.boxes[i])) == 0.0f);
        EXPECT_TRUE(st_lenf(st_angvel(p, py.boxes[i])) == 0.0f);
    }
    /* The pyramid is still standing: the top box is near its start height. */
    const de_dvec3 top = st_pos(p, py.boxes[py.count - 1u]);
    ST_GT(top.z, 9.0);

    static de_transform before[PYR_N];
    for (u32 i = 0; i < py.count; ++i)
        ASSERT_TRUE(de_body_get_transform(p, py.boxes[i], &before[i]));
    for (u32 k = 0; k < 60; ++k) {
        de_physics_step(p, ST_DT);
        de__solver_stats st;
        de__solver_stats_get(p, &st);
        /* No solver bodies, no constraints, no contact points: sleeping costs no solver work. */
        EXPECT_EQ(st.awake_bodies, 0u);
        EXPECT_EQ(st.constraints, 0u);
        EXPECT_EQ(st.contact_points, 0u);
    }
    for (u32 i = 0; i < py.count; ++i) {
        de_transform now;
        ASSERT_TRUE(de_body_get_transform(p, py.boxes[i], &now));
        EXPECT_TRUE(same_pose(now, before[i]));
    }
    /* Manifolds between sleeping bodies are kept, impulses included. */
    de__contact_stats cs;
    de__contacts_stats(p, &cs);
    ST_GT((double)cs.manifolds, 50.0);
    const de__manifold *m = NULL;
    u32 mc = 0;
    de__contacts_manifolds(p, &m, &mc);
    f32 impulse = 0.0f;
    for (u32 i = 0; i < mc; ++i)
        for (u32 k = 0; k < m[i].point_count; ++k)
            impulse += m[i].points[k].normal_impulse;
    ST_GT(impulse, 0.0f);
    de_physics_destroy(p);
}

TEST(physics_sleep, sphere_hit_wakes_the_whole_island_at_once) {
    de_physics_world *p = st_world(128, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    static it_pyramid py;
    it_pyramid_build(&py, p, PYR_ROWS, 0.0, true, true);
    ASSERT_TRUE(run_until_asleep(p, &py, 120) <= 120u);
    /* A ball thrown at the side of the second row. */
    const de_body ball = st_body(p, DE_BODY_DYNAMIC, st_sphere(p, 0.4f),
                                 st_xform(-8.0, 0.0, 1.6, st_quat_identity()), 0.5f, 0.0f);
    ASSERT_TRUE(
        de_body_set_velocity(p, ball, (de_vec3){12.0f, 0.0f, 0.0f}, (de_vec3){0.0f, 0.0f, 0.0f}));
    u32 woke_at = 0;
    for (u32 t = 1; t <= 120 && woke_at == 0u; ++t) {
        de_physics_step(p, ST_DT);
        const u32 n = it_pyramid_sleeping(p, &py);
        EXPECT_TRUE(n == 0u || n == PYR_N); /* never partly awake */
        if (n == 0u)
            woke_at = t;
    }
    ASSERT_TRUE(woke_at != 0u);
    de__islands_stats is;
    de__islands_stats_get(p, &is);
    EXPECT_EQ(is.sleeping_bodies, 0u);
    EXPECT_GE(is.woken_islands, 1u);
    EXPECT_GE(is.regathers, 1u); /* the ball's contact woke it during the gather */
    de__solver_stats st;
    de__solver_stats_get(p, &st);
    EXPECT_EQ(st.awake_bodies, PYR_N + 1u);
    /* The hit box moves. */
    f32 fastest = 0.0f;
    for (u32 k = 0; k < 3; ++k) {
        de_physics_step(p, ST_DT);
        for (u32 i = 0; i < py.count; ++i)
            fastest = de__maxf(fastest, st_lenf(st_vel(p, py.boxes[i])));
    }
    printf("  woke at tick %u; fastest box %.3f m/s\n", woke_at, (double)fastest);
    ST_GT(fastest, 0.1f);
    de_physics_destroy(p);
}

TEST(physics_sleep, user_changes_and_de_body_wake) {
    de_physics_world *p = st_world(128, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    static it_pyramid py;
    it_pyramid_build(&py, p, 4, 0.0, true, true);
    ASSERT_TRUE(run_until_asleep(p, &py, 120) <= 120u);

    /* Setting a zero velocity changes nothing: it stays asleep. */
    ASSERT_TRUE(de_body_set_velocity(p, py.boxes[0], (de_vec3){0, 0, 0}, (de_vec3){0, 0, 0}));
    de_physics_step(p, ST_DT);
    EXPECT_EQ(it_pyramid_sleeping(p, &py), py.count);

    /* A velocity wakes the island at the next step. */
    ASSERT_TRUE(de_body_set_velocity(p, py.boxes[py.count - 1u], (de_vec3){0.0f, 2.0f, 0.0f},
                                     (de_vec3){0, 0, 0}));
    de_physics_step(p, ST_DT);
    EXPECT_EQ(it_pyramid_sleeping(p, &py), 0u);
    ASSERT_TRUE(run_until_asleep(p, &py, 300) <= 300u);

    /* A teleport wakes it (the top box is lifted 10 cm and falls back). */
    de_transform t;
    ASSERT_TRUE(de_body_get_transform(p, py.boxes[py.count - 1u], &t));
    t.position.z += 0.1;
    ASSERT_TRUE(de_body_set_transform(p, py.boxes[py.count - 1u], &t));
    de_physics_step(p, ST_DT);
    EXPECT_EQ(it_pyramid_sleeping(p, &py), 0u);
    ASSERT_TRUE(run_until_asleep(p, &py, 300) <= 300u);

    /* de_body_wake answers at once and wakes every body of the island. */
    de_body_wake(p, py.boxes[1]);
    EXPECT_EQ(it_pyramid_sleeping(p, &py), 0u);
    de_physics_step(p, ST_DT);
    de__solver_stats st;
    de__solver_stats_get(p, &st);
    EXPECT_EQ(st.awake_bodies, py.count);
    /* A stale handle and an awake body are ignored. */
    de_body_wake(p, (de_body){0});
    de_body_wake(p, py.boxes[0]);
    EXPECT_FALSE(de_body_is_sleeping(p, (de_body){0}));
    EXPECT_FALSE(de_body_is_sleeping(p, py.ground)); /* static bodies never sleep */
    de_physics_destroy(p);
}

TEST(physics_sleep, allow_sleep_false_keeps_its_island_awake) {
    de_physics_world *p = st_world(128, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    /* Pyramid A: no body may sleep. Pyramid B: one body may not sleep. Pyramid C: all may. */
    static it_pyramid a, b, c;
    it_pyramid_build(&a, p, 3, -10.0, false, true);
    it_pyramid_build(&b, p, 3, 0.0, true, false);
    it_pyramid_build(&c, p, 3, 10.0, true, false);
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    /* B's top box: replace it by one without the flag (same place). */
    de_transform top;
    ASSERT_TRUE(de_body_get_transform(p, b.boxes[b.count - 1u], &top));
    de_body_destroy(p, b.boxes[b.count - 1u]);
    b.boxes[b.count - 1u] = st_body(p, DE_BODY_DYNAMIC, cube, top, 0.6f, 0.0f);
    for (u32 t = 0; t < 180; ++t)
        de_physics_step(p, ST_DT);
    EXPECT_EQ(it_pyramid_sleeping(p, &a), 0u);
    EXPECT_EQ(it_pyramid_sleeping(p, &b), 0u);
    EXPECT_EQ(it_pyramid_sleeping(p, &c), c.count);
    de__islands_stats is;
    de__islands_stats_get(p, &is);
    EXPECT_EQ(is.islands, 2u); /* A and B are awake islands; C sleeps */
    de_physics_destroy(p);
}

TEST(physics_sleep, joints_wake_islands) {
    de_physics_world *p = st_world(128, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    static it_joints j;
    memset(&j, 0, sizeof j);
    it_joints_register(p, &j);
    static it_pyramid a, b;
    it_pyramid_build(&a, p, 3, -5.0, true, true);
    it_pyramid_build(&b, p, 3, 5.0, true, false);
    /* A free ball in zero gravity that never sleeps (no flag), far away. */
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = st_sphere(p, 0.3f);
    d.transform = st_xform(0.0, 20.0, 5.0, st_quat_identity());
    const de_body ball = de_body_create(p, &d);
    /* A joint between the two pyramids' tops, inactive for now; one from A to the ball. */
    const u32 ab =
        it_joints_add(&j, it_index(a.boxes[a.count - 1u]), it_index(b.boxes[b.count - 1u]));
    const u32 aball = it_joints_add(&j, it_index(a.boxes[0]), it_index(ball));
    j.active[ab] = false;
    j.active[aball] = false;
    for (u32 t = 0;
         t < 120 && (it_pyramid_sleeping(p, &a) < a.count || it_pyramid_sleeping(p, &b) < b.count);
         ++t)
        de_physics_step(p, ST_DT);
    ASSERT_EQ(it_pyramid_sleeping(p, &a), a.count);
    ASSERT_EQ(it_pyramid_sleeping(p, &b), b.count);

    /* A joint between two sleeping islands does not wake them by itself (nothing to solve)... */
    j.active[ab] = true;
    de_physics_step(p, ST_DT);
    EXPECT_EQ(it_pyramid_sleeping(p, &a), a.count);
    EXPECT_EQ(it_pyramid_sleeping(p, &b), b.count);
    EXPECT_EQ(j.solves[ab], 0u);
    /* ...so joint create calls de__islands_wake_body (DENG-220): both islands wake, and then the
     * joint links them into one island. */
    de__islands_wake_body(p, j.a[ab]);
    de__islands_wake_body(p, j.b[ab]);
    EXPECT_FALSE(de_body_is_sleeping(p, a.boxes[0]));
    de_physics_step(p, ST_DT);
    EXPECT_EQ(it_pyramid_sleeping(p, &a), 0u);
    EXPECT_EQ(it_pyramid_sleeping(p, &b), 0u);
    EXPECT_GT(j.solves[ab], 0u);
    de__islands_stats is;
    de__islands_stats_get(p, &is);
    EXPECT_EQ(is.islands, 2u); /* A + B joined, and the ball */
    for (u32 t = 0; t < 120 && it_pyramid_sleeping(p, &a) < a.count; ++t)
        de_physics_step(p, ST_DT);
    ASSERT_EQ(it_pyramid_sleeping(p, &a), a.count);
    EXPECT_EQ(it_pyramid_sleeping(p, &b), b.count); /* one island: they slept together */

    /* A joint to an awake body wakes the island during the gather, without any hook. */
    j.active[ab] = false;
    j.active[aball] = true;
    de_physics_step(p, ST_DT);
    EXPECT_EQ(it_pyramid_sleeping(p, &a), 0u);
    EXPECT_EQ(it_pyramid_sleeping(p, &b), 0u); /* B slept in A's island: it wakes too */
    EXPECT_GT(j.solves[aball], 0u);
    de_physics_destroy(p);
}

TEST(physics_sleep, destroyed_bodies_wake_what_they_touched) {
    de_physics_world *p = st_world(128, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    static it_pyramid py;
    it_pyramid_build(&py, p, 4, 0.0, true, true);
    ASSERT_TRUE(run_until_asleep(p, &py, 120) <= 120u);

    /* A sleeping body destroyed: the rest of its island wakes. */
    de_body_destroy(p, py.boxes[py.count - 1u]);
    --py.count;
    EXPECT_EQ(it_pyramid_sleeping(p, &py), 0u);
    de_physics_step(p, ST_DT);
    EXPECT_EQ(it_pyramid_sleeping(p, &py), 0u);
    ASSERT_TRUE(run_until_asleep(p, &py, 300) <= 300u);

    /* The reused index starts awake and falls. */
    const de_body fresh = it_sleeper(p, st_box(p, 0.5f, 0.5f, 0.5f),
                                     st_xform(20.0, 0.0, 5.0, st_quat_identity()), 0.5f);
    EXPECT_FALSE(de_body_is_sleeping(p, fresh));
    de_physics_step(p, ST_DT);
    ST_LT(st_vel(p, fresh).z, -0.1f);
    EXPECT_EQ(it_pyramid_sleeping(p, &py), py.count);

    /* The static ground under the sleeping pyramid is destroyed: the pyramid wakes and falls. */
    const de_dvec3 z0 = st_pos(p, py.boxes[0]);
    de_body_destroy(p, py.ground);
    de_physics_step(p, ST_DT);
    EXPECT_EQ(it_pyramid_sleeping(p, &py), 0u);
    for (u32 t = 0; t < 30; ++t)
        de_physics_step(p, ST_DT);
    ST_LT(st_pos(p, py.boxes[0]).z, z0.z - 0.5);
    de_physics_destroy(p);
}

TEST(physics_sleep, kinematic_bodies_and_sleep) {
    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape plate = st_box(p, 2.0f, 2.0f, 0.25f);
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    /* A box on a still kinematic plate sleeps; one on a slowly moving plate never does. */
    const de_body still = st_body(p, DE_BODY_KINEMATIC, plate,
                                  st_xform(-5.0, 0.0, 0.0, st_quat_identity()), 0.8f, 0.0f);
    const de_body moving = st_body(p, DE_BODY_KINEMATIC, plate,
                                   st_xform(5.0, 0.0, 0.0, st_quat_identity()), 0.8f, 0.0f);
    ASSERT_TRUE(de_body_set_velocity(p, moving, (de_vec3){0.01f, 0.0f, 0.0f}, (de_vec3){0, 0, 0}));
    const de_body on_still =
        it_sleeper(p, cube, st_xform(-5.0, 0.0, 0.751, st_quat_identity()), 0.8f);
    const de_body on_moving =
        it_sleeper(p, cube, st_xform(5.0, 0.0, 0.751, st_quat_identity()), 0.8f);
    for (u32 t = 0; t < 120; ++t)
        de_physics_step(p, ST_DT);
    EXPECT_TRUE(de_body_is_sleeping(p, on_still));
    EXPECT_FALSE(de_body_is_sleeping(p, on_moving));
    EXPECT_FALSE(de_body_is_sleeping(p, still)); /* kinematic bodies never sleep */
    /* The still plate starts moving: the box on it wakes at the next step. */
    ASSERT_TRUE(de_body_set_velocity(p, still, (de_vec3){0.0f, 0.0f, 0.5f}, (de_vec3){0, 0, 0}));
    de_physics_step(p, ST_DT);
    EXPECT_FALSE(de_body_is_sleeping(p, on_still));
    de_physics_destroy(p);
}
