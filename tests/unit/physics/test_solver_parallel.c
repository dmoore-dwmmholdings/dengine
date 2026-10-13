/* test_solver_parallel.c: the colored parallel solver (DENG-218, SPEC §13.2, §5.7, §8): the same
 * bits for tasks == NULL and 1, 4 and 8 workers and for any chunk size, through sleeping, waking,
 * joints and index reuse; no heap allocation in steady-state stepping; and an informational
 * speed-up of 4 workers on a 2,000-body pile. This executable carries the `tsan` label: its scenes
 * are small so that the TSan Debug builds stay fast. */
#include "island_test_util.h"

#include <dengine/de_platform.h>

/* A 5-row pyramid that may sleep, a ball thrown at it once it sleeps, a box shower, three spheres
 * linked by fake joints in zero gravity, and a body destroyed and recreated (index reuse). */
typedef struct mixed {
    it_pyramid py;
    it_joints joints;
    de_body ball;
    de_body shower[12];
    de_shape cube;
    de_shape ball_shape;
} mixed;

static void mixed_build(mixed *m, de_physics_world *p) {
    memset(m, 0, sizeof *m);
    it_joints_register(p, &m->joints);
    it_pyramid_build(&m->py, p, 5, 0.0, true, true);
    m->cube = st_box(p, 0.3f, 0.3f, 0.3f);
    m->ball_shape = st_sphere(p, 0.35f); /* shapes allocate: made up front */
    for (u32 i = 0; i < 12; ++i)
        m->shower[i] =
            it_sleeper(p, m->cube,
                       st_xform(-8.0 + 0.5 * (f64)(i % 3u), 4.0, 2.0 + 0.7 * (f64)i,
                                st_quat_axis_angle((de_vec3){1.0f, 0.3f, 0.2f}, 0.3f * (f32)i)),
                       0.4f);
    const de_shape s = st_sphere(p, 0.25f);
    u32 idx[3];
    for (u32 i = 0; i < 3; ++i) {
        de_body_desc d;
        memset(&d, 0, sizeof d);
        d.type = DE_BODY_DYNAMIC;
        d.shape = s;
        d.transform = st_xform(10.0 + 2.0 * (f64)i, -6.0, 8.0, st_quat_identity());
        d.linear_velocity = (de_vec3){(f32)i - 1.0f, 0.5f, 0.0f};
        idx[i] = it_index(de_body_create(p, &d));
    }
    (void)it_joints_add(&m->joints, idx[0], idx[1]);
    (void)it_joints_add(&m->joints, idx[1], idx[2]);
    (void)it_joints_add(&m->joints, idx[2], DE__CONSTRAINT_NO_BODY);
}

static void mixed_script(mixed *m, de_physics_world *p, u32 tick) {
    if (tick == 50) {
        m->ball = st_body(p, DE_BODY_DYNAMIC, m->ball_shape,
                          st_xform(-6.0, 0.0, 1.5, st_quat_identity()), 0.5f, 0.0f);
        (void)de_body_set_velocity(p, m->ball, (de_vec3){10.0f, 0.0f, 0.0f}, (de_vec3){0, 0, 0});
    }
    if (tick == 60) {
        de_body_destroy(p, m->shower[3]);
        m->shower[3] = it_sleeper(p, m->cube, st_xform(-7.0, 4.5, 6.0, st_quat_identity()), 0.4f);
    }
}

static u64 run_mixed(u32 workers, u32 grain, u32 body_grain, u32 ticks, u32 *sleeping_seen) {
    de_task_system *ts = it_tasks_new(workers);
    de_physics_world *p = st_world(128, ts, NULL);
    if (p == NULL)
        return 0;
    static mixed m;
    mixed_build(&m, p);
    de__solver *s = st_solver(p);
    if (grain != 0u)
        s->coloring->grain = grain;
    if (body_grain != 0u)
        s->coloring->body_grain = body_grain;
    u64 sum = 0;
    u32 slept = 0;
    for (u32 t = 0; t < ticks; ++t) {
        mixed_script(&m, p, t);
        de_physics_step(p, ST_DT);
        de__islands_stats is;
        de__islands_stats_get(p, &is);
        slept = is.sleeping_bodies > slept ? is.sleeping_bodies : slept;
        sum = sum * 31u + de_physics_checksum(p);
        sum = sum * 31u + is.sleeping_bodies;
    }
    if (sleeping_seen != NULL)
        *sleeping_seen = slept;
    de_physics_destroy(p);
    de_task_system_destroy(ts);
    return sum;
}

TEST(physics_solver_parallel, worker_count_and_grain_invariance) {
    enum { TICKS = 90 };
    u32 slept = 0;
    const u64 ref = run_mixed(0, 0, 0, TICKS, &slept);
    EXPECT_GT(slept, 10u); /* the scene does exercise sleeping */
    const u32 workers[3] = {1, 4, 8};
    for (u32 w = 0; w < 3; ++w)
        EXPECT_EQ(run_mixed(workers[w], 0, 0, TICKS, NULL), ref);
    /* Small chunks: every color and the per-body kernels split over the workers. */
    EXPECT_EQ(run_mixed(4, 1, 1, TICKS, NULL), ref);
    EXPECT_EQ(run_mixed(8, 3, 5, TICKS, NULL), ref);
    EXPECT_EQ(run_mixed(0, 2, 7, TICKS, NULL), ref); /* the grain never matters, also serially */
}

TEST(physics_solver_parallel, no_heap_allocation_in_steady_state) {
    de_task_system *ts = it_tasks_new(4);
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics_world *p = st_world(128, ts, &a);
    ASSERT_TRUE(p != NULL);
    static mixed m;
    mixed_build(&m, p);
    st_solver(p)->coloring->grain = 2;
    st_solver(p)->coloring->body_grain = 4;
    de_physics_step(p, ST_DT); /* warm-up: broadphase, contacts, solver */
    const de_tracking_stats before = de_tracking_allocator_stats(&t);
    for (u32 tick = 1; tick < 150; ++tick) {
        mixed_script(&m, p, tick);
        de_physics_step(p, ST_DT);
    }
    const de_tracking_stats after = de_tracking_allocator_stats(&t);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(after.free_count, before.free_count);
    de__islands_stats is;
    de__islands_stats_get(p, &is);
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
    de_task_system_destroy(ts);
}

/* Informational (SPEC §13.2 acceptance): the speed-up of 4 workers on a 2,000-body pile. A
 * 20 x 10 x 10 block of boxes resting on the ground (each layer shifted half a box), 12 steps.
 * Debug builds (also the sanitizer jobs) run a 500-body block for one step. The timing is logged,
 * not asserted; both runs must give the same bits. */
static u64 pile_run(u32 workers, u32 nx, u32 steps, f64 *ms_per_step, u32 *constraints,
                    u32 *colors) {
    de_task_system *ts = it_tasks_new(workers);
    de_physics_world *p = st_world(2100, ts, NULL);
    if (p == NULL)
        return 0;
    it_capacity(p, 32768, 32768);
    const de_shape g = st_box(p, 40.0f, 40.0f, 0.5f);
    const de_shape c = st_box(p, 0.5f, 0.5f, 0.5f);
    (void)st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.5f, 0.0f);
    for (u32 z = 0; z < 10; ++z)
        for (u32 y = 0; y < 10; ++y)
            for (u32 x = 0; x < nx; ++x) {
                const f64 off = (z & 1u) ? 0.5 : 0.0;
                (void)st_body(
                    p, DE_BODY_DYNAMIC, c,
                    st_xform(1.001 * x + off, 1.001 * y + off, 0.5 + 1.001 * z, st_quat_identity()),
                    0.5f, 0.0f);
            }
    de_physics_step(p, ST_DT); /* warm-up */
    u64 sum = de_physics_checksum(p);
    const u64 t0 = de_time_now_ns();
    for (u32 k = 0; k < steps; ++k) {
        de_physics_step(p, ST_DT);
        sum = sum * 31u + de_physics_checksum(p);
    }
    *ms_per_step = (f64)(de_time_now_ns() - t0) * 1e-6 / (f64)steps;
    const de__solver *s = de__solver_get(p);
    *constraints = s->constraint_count;
    *colors = s->coloring->color_count;
    de_physics_destroy(p);
    de_task_system_destroy(ts);
    return sum;
}

TEST(physics_solver_parallel, bench_2000_body_pile_speedup) {
#if defined(DE_DEBUG)
    /* Debug and sanitizer builds: timings mean nothing there; a 500-body block, one step. */
    const u32 nx = 5, steps = 1;
    const char *cfg = "Debug";
#else
    const u32 nx = 20, steps = 12;
    const char *cfg = "optimized";
#endif
    f64 ms1 = 0, ms4 = 0;
    u32 constraints = 0, colors = 0;
    const u64 a = pile_run(0, nx, steps, &ms1, &constraints, &colors);
    const u64 b = pile_run(4, nx, steps, &ms4, &constraints, &colors);
    EXPECT_EQ(a, b);
    printf("  [bench] solver %s: %u-body pile, %u constraints in %u colors: tasks NULL %.2f "
           "ms/step, 4 workers %.2f ms/step, speed-up %.2fx (whole step, collision included)\n",
           cfg, nx * 100u, constraints, colors, ms1, ms4, ms4 > 0.0 ? ms1 / ms4 : 0.0);
}
