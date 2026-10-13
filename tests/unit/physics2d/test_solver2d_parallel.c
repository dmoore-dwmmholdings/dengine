/*
 * test_solver2d_parallel.c: deterministic graph coloring and the colored parallel 2D solver
 * (DENG-254, SPEC §13.3, §13.2, §5.7, §8): coloring validity on every step of a pile, static and
 * kinematic sides use no color, a 30-link hub spills into the overflow set and still solves every
 * link, a 1,000-box block fits the colors; the same bits for tasks NULL and 1, 4 and 8 workers and
 * for grains 1, 7 and 64, tick by tick on a pile too; the golden vector of the scenarios
 * pyramid2d_20, bounce2d, sleep_wake2d and bullet2d
 * (tests/unit/physics2d/vectors/solver2d_golden.txt, made with no task system on macOS arm64;
 * x86-64 and every worker count must reproduce it); no heap allocation in steady state. Regenerate
 * the vector (never by hand) with DE_REGEN_GOLDEN=1
 * build/dev/tests/unit/physics2d/test_physics2d_solver_parallel
 *     --filter=physics2d_solver_parallel.golden_scenarios_no_tasks
 */
#include "solver2d_test_util.h"

#include <dengine/de_platform.h>

/* ---- Coloring ------------------------------------------------------------------------------- */

/* A box of static walls with a kinematic paddle and `n` mixed bodies dropped into it. */
static de_physics2d_world *pile_world(u32 n, de_task_system *ts) {
    de_physics2d_world *p = sv_world(n + 16u, ts, NULL);
    sv_capacity(p, 8u * n + 1024u, 4u * n + 1024u);
    const de_shape floor = tu_box(p, 12.0f, 0.5f, 0.0f);
    const de_shape wall = tu_box(p, 0.5f, 20.0f, 0.0f);
    (void)sv_body(p, DE_BODY_STATIC, floor, tu_xf(0, -0.5, 0.0f), 0.5f, 0.0f, 0);
    (void)sv_body(p, DE_BODY_STATIC, wall, tu_xf(-12.5, 19.5, 0.0f), 0.5f, 0.0f, 0);
    (void)sv_body(p, DE_BODY_STATIC, wall, tu_xf(12.5, 19.5, 0.0f), 0.5f, 0.0f, 0);
    const de_shape paddle = tu_box(p, 2.0f, 0.2f, 0.0f);
    const de_body k = sv_body(p, DE_BODY_KINEMATIC, paddle, tu_xf(0, 3.0, 0.0f), 0.5f, 0.0f, 0);
    (void)de_body2d_set_velocity(p, k, (de_vec2){0, 0}, 1.5f);
    const de_shape kinds[3] = {tu_box(p, 0.3f, 0.25f, 0.0f), tu_circle(p, 0.3f, (de_vec2){0, 0}),
                               tu_capsule(p, (de_vec2){-0.3f, 0}, (de_vec2){0.3f, 0}, 0.15f)};
    de_rng r = de_rng_seed(0x2D254, 7);
    for (u32 i = 0; i < n; ++i) {
        const f64 x = 22.0 * de_rng_f64(&r) - 11.0;
        const f64 y = 5.0 + 0.6 * (f64)i * 24.0 / (f64)n + 4.0 * de_rng_f64(&r);
        (void)sv_body(p, DE_BODY_DYNAMIC, kinds[i % 3u], tu_xf(x, y, 6.0f * de_rng_f32(&r)), 0.4f,
                      0.1f, DE_BODY_FLAG_ALLOW_SLEEP);
    }
    return p;
}

TEST(physics2d_solver_parallel, coloring_valid_every_step) {
    de_physics2d_world *p = pile_world(200, NULL);
    u32 max_colors = 0;
    for (u32 t = 0; t < 240; ++t) {
        de_physics2d_step(p, SV_DT);
        const de__solver2d *s = de__solver2d_get(p);
        ASSERT_TRUE(de__graph2d_color_validate(s));
        /* Static and kinematic sides set no color bit: every record has a writable side, and
         * the static slot is never written. */
        for (u32 i = 0; i < s->constraint_count; ++i) {
            const de__constraint2d *c = &s->constraints[i];
            ASSERT_NE(c->flags & (DE__CONSTRAINT2D_WRITE_A | DE__CONSTRAINT2D_WRITE_B), 0u);
            if (c->solver_a == DE__SOLVER2D_STATIC_SLOT)
                ASSERT_EQ(c->flags & DE__CONSTRAINT2D_WRITE_A, 0u);
            if ((s->bodies[c->solver_b].flags & DE__SOLVER2D_BODY_KINEMATIC) != 0u)
                ASSERT_EQ(c->flags & DE__CONSTRAINT2D_WRITE_B, 0u);
        }
        if (s->coloring->stats.colors > max_colors)
            max_colors = s->coloring->stats.colors;
        EXPECT_EQ(s->coloring->stats.overflow, 0u);
    }
    EXPECT_GT(max_colors, 2u);
    de_physics2d_destroy(p);
}

TEST(physics2d_solver_parallel, hub_overflows_and_still_solves) {
    de_physics2d_world *p = sv_world(64, NULL, NULL);
    de_physics2d_set_gravity(p, (de_vec2){0, 0});
    static sv_joints j;
    memset(&j, 0, sizeof j);
    sv_joints_register(p, &j);
    const de_shape c = tu_circle(p, 0.1f, (de_vec2){0, 0});
    const u32 hub = tu_index(sv_body(p, DE_BODY_DYNAMIC, c, tu_xf(0, 0, 0.0f), 0.5f, 0.0f, 0));
    for (u32 i = 0; i < 30; ++i) {
        const u32 b =
            tu_index(sv_body(p, DE_BODY_DYNAMIC, c, tu_xf(1.0 + i, 5, 0.0f), 0.5f, 0.0f, 0));
        (void)sv_joints_add(&j, hub, b);
    }
    de_physics2d_step(p, SV_DT);
    const de__solver2d *s = de__solver2d_get(p);
    EXPECT_EQ(s->coloring->stats.colors, DE__GRAPH2D_MAX_COLORS);
    EXPECT_EQ(s->coloring->stats.overflow, 6u);
    EXPECT_TRUE(de__graph2d_color_validate(s));
    for (u32 k = 0; k < j.count; ++k)
        EXPECT_EQ(j.solves[k], 8u); /* 4 substeps x (solve + relax) */
    de_physics2d_destroy(p);
}

TEST(physics2d_solver_parallel, thousand_box_block_fits_the_colors) {
    de_physics2d_world *p = sv_world(1100, NULL, NULL);
    sv_capacity(p, 16384u, 8192u);
    const de_shape g = tu_box(p, 40.0f, 0.5f, 0.0f);
    (void)sv_body(p, DE_BODY_STATIC, g, tu_xf(0, -0.5, 0.0f), 0.6f, 0.0f, 0);
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    for (u32 r = 0; r < 25; ++r)
        for (u32 k = 0; k < 40; ++k)
            (void)sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(-20.0 + 1.001 * k, 0.5 + 1.001 * r, 0.0f),
                          0.6f, 0.0f, 0);
    for (u32 t = 0; t < 3; ++t)
        de_physics2d_step(p, SV_DT);
    const de__solver2d *s = de__solver2d_get(p);
    printf("  1,000-box block: %u constraints, %u colors, largest %u, overflow %u\n",
           s->constraint_count, s->coloring->stats.colors, s->coloring->stats.largest,
           s->coloring->stats.overflow);
    EXPECT_GT(s->constraint_count, 1900u);
    EXPECT_LE(s->coloring->stats.colors, DE__GRAPH2D_MAX_COLORS);
    EXPECT_EQ(s->coloring->stats.overflow, 0u);
    de_physics2d_destroy(p);
}

/* ---- Worker counts, grains, golden scenarios ---------------------------------------------- */

#define SV_GOLDEN_HEADER                                                                           \
    "# Generated by test_physics2d_solver_parallel with DE_REGEN_GOLDEN=1 (no task system); "      \
    "never "                                                                                       \
    "edit\n"                                                                                       \
    "# by hand. Per scenario: a running XXH3 of the per-tick checksum and sleep state every 60\n"  \
    "# ticks, and the final checksum. Must match for every worker count and on x86-64.\n"

static u32 run_all(de_task_system *ts, u32 grain, tu_golden *out) {
    u32 n = 0;
    for (u32 k = 0; k < SV_SCENARIO_COUNT; ++k)
        n += sv_scenario_run((sv_scenario)k, ts, grain, out + n);
    return n;
}

TEST(physics2d_solver_parallel, golden_scenarios_no_tasks) {
    static tu_golden got[TU_GOLDEN_MAX];
    const u32 n = run_all(NULL, 0, got);
    ASSERT_LE(n, (u32)TU_GOLDEN_MAX);
    (void)tu_golden_check(DE_PHYSICS2D_SOLVER_GOLDEN, SV_GOLDEN_HEADER, got, n);
}

TEST(physics2d_solver_parallel, golden_scenarios_workers_1_4_8) {
    const char *regen = getenv("DE_REGEN_GOLDEN");
    if (regen != NULL && strcmp(regen, "1") == 0)
        SKIP("DE_REGEN_GOLDEN=1: the vector is written by golden_scenarios_no_tasks");
    const u32 workers[3] = {1, 4, 8};
    for (u32 w = 0; w < 3; ++w) {
        de_task_system *ts = sv_tasks(workers[w]);
        static tu_golden got[TU_GOLDEN_MAX];
        const u32 n = run_all(ts, 0, got);
        (void)tu_golden_check(DE_PHYSICS2D_SOLVER_GOLDEN, SV_GOLDEN_HEADER, got, n);
        de_task_system_destroy(ts);
    }
}

TEST(physics2d_solver_parallel, grain_invariance) {
    /* Small grains split every color and every body range into many chunks; the bits stay. The
     * default grain with 1, 4 and 8 workers is golden_scenarios_workers_1_4_8. */
    static tu_golden ref[TU_GOLDEN_MAX], got[TU_GOLDEN_MAX];
    const u32 n = run_all(NULL, 0, ref);
    const u32 workers[3] = {0, 4, 8};
    const u32 grains[3] = {1, 1, 7};
    for (u32 w = 0; w < 3; ++w) {
        de_task_system *ts = sv_tasks(workers[w]);
        const u32 m = run_all(ts, grains[w], got);
        ASSERT_EQ(m, n);
        for (u32 i = 0; i < n; ++i)
            EXPECT_TRUE(got[i].value == ref[i].value);
        de_task_system_destroy(ts);
    }
}

TEST(physics2d_solver_parallel, pile_worker_invariance_tick_by_tick) {
    /* One task system at a time: per-tick checksums of the pile with none, 1, 4 and 8 workers. */
    static u64 ref[180];
    const u32 workers[4] = {0, 1, 4, 8};
    for (u32 w = 0; w < 4; ++w) {
        de_task_system *ts = sv_tasks(workers[w]);
        de_physics2d_world *p = pile_world(150, ts);
        u32 bad = 0;
        for (u32 t = 0; t < 180; ++t) {
            de_physics2d_step(p, SV_DT);
            const u64 c = de_physics2d_checksum(p);
            if (w == 0)
                ref[t] = c;
            else if (c != ref[t])
                ++bad;
        }
        EXPECT_EQ(bad, 0u);
        de_physics2d_destroy(p);
        de_task_system_destroy(ts);
    }
}

/* ---- Allocation --------------------------------------------------------------------------- */

TEST(physics2d_solver_parallel, no_heap_allocation_in_steady_state) {
    const u32 workers[2] = {0, 4};
    for (u32 w = 0; w < 2; ++w) {
        de_task_system *ts = sv_tasks(workers[w]);
        de_tracking_allocator tr;
        de_allocator a = de_tracking_allocator_init(&tr, NULL, DE_MEM_PHYSICS);
        static sv_scene sc;
        sv_scene_build(&sc, SV_SLEEP_WAKE2D, ts, &a);
        sv_scene_script(&sc, SV_SLEEP_WAKE2D, 1);
        de_physics2d_step(sc.p, SV_DT); /* the first step creates the collision and solver state */
        const de_tracking_stats before = de_tracking_allocator_stats(&tr);
        /* Sleeping, waking, a thrown ball, a destroyed box and its index reused, the plate. */
        for (u32 t = 2; t <= k_sv_scenario_ticks[SV_SLEEP_WAKE2D]; ++t) {
            sv_scene_script(&sc, SV_SLEEP_WAKE2D, t);
            de_physics2d_step(sc.p, SV_DT);
        }
        const de_tracking_stats after = de_tracking_allocator_stats(&tr);
        EXPECT_EQ(after.alloc_count, before.alloc_count);
        EXPECT_EQ(after.realloc_count, before.realloc_count);
        de_physics2d_destroy(sc.p);
        EXPECT_EQ(de_tracking_allocator_stats(&tr).bytes_live, 0u);
        de_task_system_destroy(ts);
    }
}

TEST(physics2d_solver_parallel, scenarios_do_what_they_say) {
    static sv_scene sc;
    /* bullet2d: no bullet ever gets past its wall (x = 10; the kinematic slab moves to x = 6). */
    sv_scene_build(&sc, SV_BULLET2D, NULL, NULL);
    for (u32 t = 1; t <= k_sv_scenario_ticks[SV_BULLET2D]; ++t) {
        sv_scene_script(&sc, SV_BULLET2D, t);
        de_physics2d_step(sc.p, SV_DT);
        const f64 slab = sv_xf_of(sc.p, sc.platform).x;
        for (u32 i = 0; i < 4; ++i)
            SV_EXPECT_LT(sv_xf_of(sc.p, sc.bodies[i]).x, i == 3u ? slab : 10.0);
    }
    SV_EXPECT_LT(fabs(sv_xf_of(sc.p, sc.bodies[0]).x - 9.95), 1e-3); /* stopped at the segment */
    SV_EXPECT_LT(sv_vel(sc.p, sc.bodies[2]).x, -100.0f);             /* e = 0.4 bounced back */
    de_physics2d_destroy(sc.p);

    /* sleep_wake2d: each event wakes what it should. */
    sv_scene_build(&sc, SV_SLEEP_WAKE2D, NULL, NULL);
    for (u32 t = 1; t <= k_sv_scenario_ticks[SV_SLEEP_WAKE2D]; ++t) {
        sv_scene_script(&sc, SV_SLEEP_WAKE2D, t);
        de_physics2d_step(sc.p, SV_DT);
        const u32 pyr = sv_pyramid_sleeping(sc.p, &sc.py);
        const bool stack =
            de_body2d_is_sleeping(sc.p, sc.bodies[0]) && de_body2d_is_sleeping(sc.p, sc.bodies[2]);
        const bool rider = de_body2d_is_sleeping(sc.p, sc.bodies[3]);
        if (t == 100) {
            EXPECT_EQ(pyr, sc.py.count);
            EXPECT_TRUE(stack && rider);
        }
        if (t == 140)
            EXPECT_EQ(pyr, 0u); /* the ball */
        if (t == 201)
            EXPECT_FALSE(stack); /* user velocity */
        if (t == 260)
            EXPECT_TRUE(stack);
        if (t == 301 || t == 360)
            EXPECT_FALSE(rider); /* de_body2d_wake, then the moving platform */
        if (t == 440)
            EXPECT_TRUE(rider); /* the platform stopped at 400 */
        if (t == 461)
            EXPECT_FALSE(stack); /* a sleeping box destroyed */
        if (t == 600)
            SV_EXPECT_LT(sv_xf_of(sc.p, sc.bodies[0]).y, 1.0); /* the plate went: on the ground */
    }
    de_physics2d_destroy(sc.p);
}
