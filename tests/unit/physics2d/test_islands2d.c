/*
 * test_islands2d.c: islands and sleeping of the 2D solver (DENG-254, SPEC §13.3, §13.1, §13.2):
 * union-find islands (id = lowest body index, static and kinematic bodies never link, joints link,
 * world joints do not, independent of the link order), the 20-row pyramid (stands for 10 s, falls
 * asleep within 3 s, keeps its pose and impulses asleep, wakes on impact), and the wake rules:
 * contact begin, user velocity and transform, de_body2d_wake, joint changes, allow_sleep, moving
 * kinematic bodies, destroyed bodies and ground, index reuse.
 */
#include "solver2d_test_util.h"

static void steps(de_physics2d_world *p, u32 n) {
    for (u32 i = 0; i < n; ++i)
        de_physics2d_step(p, SV_DT);
}

/* Steps until every box of `py` sleeps (at most `max` ticks); the tick count, or max + 1. */
static u32 steps_until_asleep(de_physics2d_world *p, const sv_pyramid *py, u32 max) {
    for (u32 t = 1; t <= max; ++t) {
        de_physics2d_step(p, SV_DT);
        if (sv_pyramid_sleeping(p, py) == py->count)
            return t;
    }
    return max + 1u;
}

/* ---- Union-find ----------------------------------------------------------------------------- */

TEST(physics2d_islands, stacks_static_ground_and_free_bodies) {
    de_physics2d_world *p = sv_world(32, NULL, NULL);
    const de_shape g = tu_box(p, 20.0f, 0.5f, 0.0f);
    (void)sv_body(p, DE_BODY_STATIC, g, tu_xf(0, -0.5, 0.0f), 0.6f, 0.0f, 0);
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    /* Free body first (index 1), then stack A at x = 5 (2, 3), stack B at x = -5 (4, 5, 6). */
    const de_body f = sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(0, 10, 0.0f), 0.6f, 0.0f, 0);
    de_body a[2], b[3];
    for (u32 i = 0; i < 2; ++i)
        a[i] = sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(5, 0.5 + 1.001 * i, 0.0f), 0.6f, 0.0f, 0);
    for (u32 i = 0; i < 3; ++i)
        b[i] = sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(-5, 0.5 + 1.001 * i, 0.0f), 0.6f, 0.0f, 0);
    steps(p, 2);
    const de__islands2d *is = de__islands2d_get(p);
    ASSERT_TRUE(is != NULL);
    ASSERT_EQ(is->island_count, 3u);
    /* Ascending id = lowest body index; the ground links nothing. */
    EXPECT_EQ(is->islands[0].id, tu_index(f));
    EXPECT_EQ(is->islands[0].body_count, 1u);
    EXPECT_EQ(is->islands[1].id, tu_index(a[0]));
    EXPECT_EQ(is->islands[1].body_count, 2u);
    EXPECT_EQ(is->islands[2].id, tu_index(b[0]));
    EXPECT_EQ(is->islands[2].body_count, 3u);
    for (u32 i = 0; i < 3; ++i)
        EXPECT_EQ(is->island_bodies[is->islands[2].first + i], tu_index(b[i]));
    /* Solve origin: the position of the island's lowest body at the start of this step. */
    SV_EXPECT_LT(fabs(is->islands[2].origin.x + 5.0), 1e-3);
    SV_EXPECT_LT(fabs(is->islands[2].origin.y - 0.5), 2e-2);
    EXPECT_EQ(is->islands[2].constraint_count, 3u); /* ground-b0, b0-b1, b1-b2 */
    de_physics2d_destroy(p);
}

TEST(physics2d_islands, kinematic_bodies_do_not_link_but_keep_awake) {
    de_physics2d_world *p = sv_world(32, NULL, NULL);
    const de_shape pl = tu_box(p, 3.0f, 0.1f, 0.0f);
    const de_body k = sv_body(p, DE_BODY_KINEMATIC, pl, tu_xf(0, 0, 0.0f), 0.8f, 0.0f, 0);
    const de_shape box = tu_box(p, 0.3f, 0.3f, 0.0f);
    de_body b[2];
    for (u32 i = 0; i < 2; ++i)
        b[i] = sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(-1.5 + 3.0 * i, 0.401, 0.0f), 0.8f, 0.0f,
                       DE_BODY_FLAG_ALLOW_SLEEP);
    ASSERT_TRUE(de_body2d_set_velocity(p, k, (de_vec2){0.3f, 0.0f}, 0.0f));
    steps(p, 120);
    const de__islands2d *is = de__islands2d_get(p);
    EXPECT_EQ(is->island_count, 2u); /* the platform links nothing */
    EXPECT_TRUE((is->islands[0].flags & DE__ISLAND2D_KEEP_AWAKE) != 0u);
    EXPECT_FALSE(de_body2d_is_sleeping(p, b[0]));
    /* Riding at the platform's speed: slow enough to sleep, but a moving platform keeps them. */
    ASSERT_TRUE(de_body2d_set_velocity(p, k, (de_vec2){0.0f, 0.0f}, 0.0f));
    steps(p, 60);
    EXPECT_TRUE(de_body2d_is_sleeping(p, b[0]));
    EXPECT_TRUE(de_body2d_is_sleeping(p, b[1]));
    /* The platform starts again: both wake through their contacts (a moving kinematic side). */
    ASSERT_TRUE(de_body2d_set_velocity(p, k, (de_vec2){0.3f, 0.0f}, 0.0f));
    steps(p, 1);
    EXPECT_FALSE(de_body2d_is_sleeping(p, b[0]));
    EXPECT_FALSE(de_body2d_is_sleeping(p, b[1]));
    de_physics2d_destroy(p);
}

/* Islands of four free bodies linked by fake joints added in `order` (0: forward, 1: reverse). */
static void joint_islands(u32 order, u32 out_ids[4], u32 *count) {
    de_physics2d_world *p = sv_world(32, NULL, NULL);
    de_physics2d_set_gravity(p, (de_vec2){0, 0});
    static sv_joints j;
    memset(&j, 0, sizeof j);
    sv_joints_register(p, &j);
    const de_shape c = tu_circle(p, 0.2f, (de_vec2){0, 0});
    u32 idx[6];
    for (u32 i = 0; i < 6; ++i)
        idx[i] = tu_index(sv_body(p, DE_BODY_DYNAMIC, c, tu_xf(3.0 * i, 0, 0.0f), 0.5f, 0.0f, 0));
    /* Links: 5-1, 1-3 (one island {1, 3, 5}), 4-world (alone), 0 and 2 free. */
    const u32 la[3] = {idx[5], idx[1], idx[4]};
    const u32 lb[3] = {idx[1], idx[3], DE__CONSTRAINT2D_NO_BODY};
    for (u32 k = 0; k < 3; ++k) {
        const u32 m = order == 0u ? k : 2u - k;
        (void)sv_joints_add(&j, la[m], lb[m]);
    }
    steps(p, 1);
    const de__islands2d *is = de__islands2d_get(p);
    *count = is->island_count;
    for (u32 i = 0; i < is->island_count && i < 4; ++i)
        out_ids[i] = is->islands[i].id * 100u + is->islands[i].body_count;
    de_physics2d_destroy(p);
}

TEST(physics2d_islands, joints_link_world_joints_do_not_order_independent) {
    u32 fwd[4] = {0}, rev[4] = {0}, nf = 0, nr = 0;
    joint_islands(0, fwd, &nf);
    joint_islands(1, rev, &nr);
    ASSERT_EQ(nf, 4u); /* {0}, {1, 3, 5}, {2}, {4} */
    EXPECT_EQ(fwd[0], 1u);
    EXPECT_EQ(fwd[1], 103u);
    EXPECT_EQ(fwd[2], 201u);
    EXPECT_EQ(fwd[3], 401u);
    ASSERT_EQ(nr, nf);
    for (u32 i = 0; i < 4; ++i)
        EXPECT_EQ(rev[i], fwd[i]);
}

/* ---- The 20-row pyramid --------------------------------------------------------------------- */

TEST(physics2d_islands, pyramid_20_stands_for_10_s) {
    /* Without sleeping: 210 boxes for 600 ticks. Every box stays in its column and row (soft
     * contacts compress the pyramid a few cm). */
    de_physics2d_world *p = sv_world(512, NULL, NULL);
    sv_capacity(p, 4096u, 2048u);
    static sv_pyramid py;
    sv_pyramid_build(&py, p, 20, 0.0, 0, true);
    ASSERT_EQ(py.count, 210u);
    static de_transform2d x0[210];
    for (u32 i = 0; i < py.count; ++i)
        x0[i] = sv_xf_of(p, py.boxes[i]);
    steps(p, 600);
    f64 worst_dx = 0.0, worst_dy = 0.0, worst_rot = 0.0, motion = 0.0;
    for (u32 i = 0; i < py.count; ++i) {
        const de_transform2d t = sv_xf_of(p, py.boxes[i]);
        worst_dx = fmax(worst_dx, fabs(t.x - x0[i].x));
        worst_dy = fmax(worst_dy, fabs(t.y - x0[i].y));
        worst_rot = fmax(worst_rot, fabs((f64)t.rotation.s));
        const de_vec2 v = sv_vel(p, py.boxes[i]);
        motion = fmax(motion, fabs((f64)v.x) + fabs((f64)v.y));
    }
    printf("  pyramid 20 after 10 s: worst dx %.4f m, dy %.4f m, sin %.5f, speed %.4f m/s\n",
           worst_dx, worst_dy, worst_rot, motion);
    SV_EXPECT_LT(worst_dx, 0.1);
    SV_EXPECT_LT(worst_dy, 0.15);
    SV_EXPECT_LT(worst_rot, 0.02);
    SV_EXPECT_LT(motion, 0.05);
    de_physics2d_destroy(p);
}

TEST(physics2d_islands, pyramid_20_sleeps_within_3_s_and_wakes_on_impact) {
    de_physics2d_world *p = sv_world(512, NULL, NULL);
    sv_capacity(p, 4096u, 2048u);
    static sv_pyramid py;
    sv_pyramid_build(&py, p, 20, 0.0, DE_BODY_FLAG_ALLOW_SLEEP, true);
    const de_shape ball = tu_circle(p, 0.4f, (de_vec2){0, 0}); /* shapes first */
    const u32 t = steps_until_asleep(p, &py, 180);
    printf("  pyramid 20 asleep after %u ticks (%.2f s)\n", t, (f64)t / 60.0);
    ASSERT_LE(t, 180u);

    /* Asleep: no solver work, the pose kept bit for bit, the manifolds kept with impulses. */
    static de_transform2d x0[210];
    for (u32 i = 0; i < py.count; ++i)
        x0[i] = sv_xf_of(p, py.boxes[i]);
    steps(p, 30);
    de__solver2d_stats st;
    de__solver2d_stats_get(p, &st);
    EXPECT_EQ(st.awake_bodies, 0u);
    EXPECT_EQ(st.constraints, 0u);
    EXPECT_EQ(st.contact_points, 0u);
    u32 same = 0;
    for (u32 i = 0; i < py.count; ++i) {
        const de_transform2d x = sv_xf_of(p, py.boxes[i]);
        same += (x.x == x0[i].x && x.y == x0[i].y && x.rotation.c == x0[i].rotation.c &&
                 x.rotation.s == x0[i].rotation.s)
                    ? 1u
                    : 0u;
    }
    EXPECT_EQ(same, py.count);
    u32 nm = 0;
    const de__manifold2d *m = NULL;
    de__contacts2d_manifolds(p, &m, &nm);
    u32 pushed = 0;
    for (u32 i = 0; i < nm; ++i)
        pushed += m[i].point_count > 0 && m[i].points[0].normal_impulse > 0.0f ? 1u : 0u;
    EXPECT_GE(nm, 400u);
    EXPECT_GT(pushed, 380u);

    /* A ball at 14 m/s: the whole island wakes in the tick its contact begins. */
    const de_body b = sv_body(p, DE_BODY_DYNAMIC, ball, tu_xf(-14.0, 0.9, 0.0f), 0.5f, 0.0f, 0);
    ASSERT_TRUE(de_body2d_set_velocity(p, b, (de_vec2){14.0f, 0.0f}, 0.0f));
    u32 woke = 0;
    for (u32 k = 1; k <= 120 && woke == 0; ++k) {
        de_physics2d_step(p, SV_DT);
        if (sv_pyramid_sleeping(p, &py) == 0u)
            woke = k;
        else
            EXPECT_EQ(sv_pyramid_sleeping(p, &py), py.count); /* all or nothing */
    }
    EXPECT_GT(woke, 0u);
    de__islands2d_stats is;
    de__islands2d_stats_get(p, &is);
    EXPECT_EQ(is.sleeping_bodies, 0u);
    /* It settles again. */
    EXPECT_LE(steps_until_asleep(p, &py, 600), 600u);
    de_physics2d_destroy(p);
}

/* ---- Wake rules ----------------------------------------------------------------------------- */

typedef struct small_scene {
    de_physics2d_world *p;
    de_body ground;
    de_body stack[3];
    de_body lone;
} small_scene;

/* A stack of 3 and a lone box on static ground, all allowed to sleep; steps until asleep. */
static void small_build(small_scene *s, u32 lone_flags) {
    memset(s, 0, sizeof *s);
    s->p = sv_world(32, NULL, NULL);
    const de_shape g = tu_box(s->p, 20.0f, 0.5f, 0.0f);
    s->ground = sv_body(s->p, DE_BODY_STATIC, g, tu_xf(0, -0.5, 0.0f), 0.6f, 0.0f, 0);
    const de_shape box = tu_box(s->p, 0.5f, 0.5f, 0.0f);
    for (u32 i = 0; i < 3; ++i)
        s->stack[i] = sv_body(s->p, DE_BODY_DYNAMIC, box, tu_xf(0, 0.5 + 1.001 * i, 0.0f), 0.6f,
                              0.0f, DE_BODY_FLAG_ALLOW_SLEEP);
    s->lone = sv_body(s->p, DE_BODY_DYNAMIC, box, tu_xf(6, 0.5, 0.0f), 0.6f, 0.0f, lone_flags);
    steps(s->p, 90);
}

static bool stack_sleeps(const small_scene *s) {
    return de_body2d_is_sleeping(s->p, s->stack[0]) && de_body2d_is_sleeping(s->p, s->stack[1]) &&
           de_body2d_is_sleeping(s->p, s->stack[2]);
}

static bool stack_awake(const small_scene *s) {
    return !de_body2d_is_sleeping(s->p, s->stack[0]) && !de_body2d_is_sleeping(s->p, s->stack[1]) &&
           !de_body2d_is_sleeping(s->p, s->stack[2]);
}

TEST(physics2d_islands, user_velocity_and_transform_wake) {
    small_scene s;
    small_build(&s, DE_BODY_FLAG_ALLOW_SLEEP);
    ASSERT_TRUE(stack_sleeps(&s));
    EXPECT_TRUE(de_body2d_is_sleeping(s.p, s.lone));
    /* A zero velocity and the same pose do not wake. */
    ASSERT_TRUE(de_body2d_set_velocity(s.p, s.stack[1], (de_vec2){0, 0}, 0.0f));
    const de_transform2d same = sv_xf_of(s.p, s.stack[2]);
    ASSERT_TRUE(de_body2d_set_transform(s.p, s.stack[2], &same));
    steps(s.p, 1);
    EXPECT_TRUE(stack_sleeps(&s));
    /* A velocity wakes the whole island, not the lone box. */
    ASSERT_TRUE(de_body2d_set_velocity(s.p, s.stack[1], (de_vec2){0.2f, 0}, 0.0f));
    steps(s.p, 1);
    EXPECT_TRUE(stack_awake(&s));
    EXPECT_TRUE(de_body2d_is_sleeping(s.p, s.lone));
    /* A teleport of the lone box wakes it. */
    de_transform2d moved = sv_xf_of(s.p, s.lone);
    moved.x += 0.25;
    ASSERT_TRUE(de_body2d_set_transform(s.p, s.lone, &moved));
    steps(s.p, 1);
    EXPECT_FALSE(de_body2d_is_sleeping(s.p, s.lone));
    de_physics2d_destroy(s.p);
}

TEST(physics2d_islands, wake_api_and_joint_hook) {
    small_scene s;
    small_build(&s, DE_BODY_FLAG_ALLOW_SLEEP);
    ASSERT_TRUE(stack_sleeps(&s));
    de_body2d_wake(s.p, s.stack[0]);
    EXPECT_TRUE(stack_awake(&s)); /* at once: the request is pending */
    steps(s.p, 90);
    ASSERT_TRUE(stack_sleeps(&s));
    /* The joint hook (DENG-256 calls it on joint create and destroy). */
    de__islands2d_wake_body(s.p, tu_index(s.lone));
    EXPECT_FALSE(de_body2d_is_sleeping(s.p, s.lone));
    EXPECT_TRUE(stack_sleeps(&s));
    /* Stale handles and awake bodies are ignored. */
    de_body2d_wake(s.p, (de_body){0});
    EXPECT_FALSE(de_body2d_is_sleeping(s.p, (de_body){0}));
    de_physics2d_destroy(s.p);
}

TEST(physics2d_islands, joint_gather_wakes_sleeping_side) {
    small_scene s;
    memset(&s, 0, sizeof s);
    s.p = sv_world(32, NULL, NULL);
    static sv_joints j;
    memset(&j, 0, sizeof j);
    sv_joints_register(s.p, &j);
    const de_shape g = tu_box(s.p, 20.0f, 0.5f, 0.0f);
    (void)sv_body(s.p, DE_BODY_STATIC, g, tu_xf(0, -0.5, 0.0f), 0.6f, 0.0f, 0);
    const de_shape box = tu_box(s.p, 0.5f, 0.5f, 0.0f);
    const de_body sleeper = sv_body(s.p, DE_BODY_DYNAMIC, box, tu_xf(0, 0.5, 0.0f), 0.6f, 0.0f,
                                    DE_BODY_FLAG_ALLOW_SLEEP);
    const de_body mover = sv_body(s.p, DE_BODY_DYNAMIC, box, tu_xf(5, 0.5, 0.0f), 0.6f, 0.0f, 0);
    steps(s.p, 60);
    ASSERT_TRUE(de_body2d_is_sleeping(s.p, sleeper));
    /* A joint between the awake box and the sleeper: the gather wakes it and gathers again. */
    (void)sv_joints_add(&j, tu_index(mover), tu_index(sleeper));
    steps(s.p, 1);
    EXPECT_FALSE(de_body2d_is_sleeping(s.p, sleeper));
    de__islands2d_stats is;
    de__islands2d_stats_get(s.p, &is);
    EXPECT_EQ(is.regathers, 1u);
    EXPECT_EQ(is.woken_islands, 1u);
    EXPECT_EQ(de__islands2d_get(s.p)->island_count, 1u); /* now one island */
    de_physics2d_destroy(s.p);
}

TEST(physics2d_islands, allow_sleep_false_keeps_island_awake) {
    small_scene s;
    small_build(&s, 0); /* the lone box may not sleep */
    EXPECT_FALSE(de_body2d_is_sleeping(s.p, s.lone));
    EXPECT_TRUE(stack_sleeps(&s));
    /* Put the lone box on the stack: their island has a body without the flag. */
    const de_transform2d top = {0.0, 3.504, {1.0f, 0.0f}};
    ASSERT_TRUE(de_body2d_set_transform(s.p, s.lone, &top));
    steps(s.p, 120);
    EXPECT_TRUE(stack_awake(&s));
    EXPECT_FALSE(de_body2d_is_sleeping(s.p, s.lone));
    de_physics2d_destroy(s.p);
}

TEST(physics2d_islands, destroyed_bodies_wake_and_index_reuse_starts_awake) {
    small_scene s;
    small_build(&s, DE_BODY_FLAG_ALLOW_SLEEP);
    ASSERT_TRUE(stack_sleeps(&s));
    /* Destroying the middle box wakes the rest of its island; its index comes back awake. */
    const u32 mid = tu_index(s.stack[1]);
    de_body2d_destroy(s.p, s.stack[1]);
    EXPECT_FALSE(de_body2d_is_sleeping(s.p, s.stack[0]));
    EXPECT_FALSE(de_body2d_is_sleeping(s.p, s.stack[2]));
    const de_shape box = tu_box(s.p, 0.5f, 0.5f, 0.0f);
    const de_body re = sv_body(s.p, DE_BODY_DYNAMIC, box, tu_xf(-6, 0.5, 0.0f), 0.6f, 0.0f,
                               DE_BODY_FLAG_ALLOW_SLEEP);
    EXPECT_EQ(tu_index(re), mid);
    EXPECT_FALSE(de_body2d_is_sleeping(s.p, re));
    steps(s.p, 120);
    EXPECT_TRUE(de_body2d_is_sleeping(s.p, s.stack[2])); /* fell onto stack[0] and slept again */
    EXPECT_TRUE(de_body2d_is_sleeping(s.p, s.lone));
    /* Destroying the ground wakes everything that rested on it (end records, END_REMOVED). */
    de_body2d_destroy(s.p, s.ground);
    steps(s.p, 1);
    EXPECT_FALSE(de_body2d_is_sleeping(s.p, s.lone));
    EXPECT_FALSE(de_body2d_is_sleeping(s.p, s.stack[0]));
    steps(s.p, 30);
    SV_EXPECT_LT(sv_xf_of(s.p, s.lone).y, 0.0); /* falling */
    de_physics2d_destroy(s.p);
}
