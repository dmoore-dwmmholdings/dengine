#include "de_test.h"
#include "de_alloc_guard.h"
#include "anim/sync_groups.h"
#include <dengine/de_task.h>
#include <dengine/de_math.h>
#include <string.h>
#include <float.h>
static de__anim_sync_leaf leaf(u32 node, f32 demand, f32 duration, f32 speed) {
    return (de__anim_sync_leaf){de_name_from("feet"), node, demand, duration, speed};
}
TEST(anim_sync, highest_weight_threshold_and_stable_node_ties) {
    de__anim_sync_leaf leaves[] = {leaf(9, .5f, 1, 1), leaf(3, .5f, .6f, 1), leaf(1, 1e-4f, 1, 1),
                                   leaf(0, 1, 0, 1)};
    de__anim_sync_clock c = {0};
    ASSERT_TRUE(de__anim_sync_step(&c, de_name_from("feet"), leaves, 4, .3, false));
    EXPECT_EQ(c.leader, 3u);
    de__anim_sync_leaf reordered[] = {leaves[3], leaves[2], leaves[1], leaves[0]};
    de__anim_sync_clock other = {0};
    ASSERT_TRUE(de__anim_sync_step(&other, de_name_from("feet"), reordered, 4, .3, false));
    EXPECT_FLOAT_EQ(c.phase, other.phase, 0);
    leaves[0].demand = .75f;
    ASSERT_TRUE(de__anim_sync_step(&c, de_name_from("feet"), leaves, 4, .3, false));
    EXPECT_EQ(c.leader, 9u);
    EXPECT_FLOAT_EQ(c.phase, other.phase, 0);
}
TEST(anim_sync, different_duration_leaves_share_normalized_phase_and_leader_change_has_no_pop) {
    de__anim_sync_leaf leaves[] = {leaf(0, .5f, 1, 1), leaf(1, .5f, .6f, 1)};
    de__anim_sync_clock c = {0};
    ASSERT_TRUE(de__anim_sync_step(&c, de_name_from("feet"), leaves, 2, .25, false));
    f32 walk = -1, run = -1;
    ASSERT_TRUE(de__anim_sync_sample_time(&c, 1, true, &walk));
    ASSERT_TRUE(de__anim_sync_sample_time(&c, .6f, true, &run));
    EXPECT_FLOAT_EQ(walk, .25f, 1e-7f);
    EXPECT_FLOAT_EQ(run / .6f, walk, 1e-7f);
    leaves[1].demand = .75f;
    ASSERT_TRUE(de__anim_sync_step(&c, de_name_from("feet"), leaves, 2, .25, false));
    EXPECT_EQ(c.leader, 1u);
    EXPECT_FLOAT_EQ(c.phase, .25, 0);
    ASSERT_TRUE(de__anim_sync_step(&c, de_name_from("feet"), leaves, 2, .35, false));
    EXPECT_FLOAT_EQ(c.phase, .25 + .1 / (f64).6f, 1e-12);
    leaves[0].demand = 1;
    f64 previous = c.phase;
    ASSERT_TRUE(de__anim_sync_step(&c, de_name_from("feet"), leaves, 2, .45, false));
    EXPECT_FLOAT_EQ(c.phase, previous + .1, 1e-12);
}
TEST(anim_sync, pause_reverse_dormancy_seek_and_independent_groups) {
    de__anim_sync_leaf leaves[] = {leaf(0, 1, 1, 1), leaf(1, 1, .5f, 1)};
    leaves[1].group = de_name_from("hands");
    de__anim_sync_clock c = {0}, hands = {0};
    ASSERT_TRUE(de__anim_sync_step(&c, leaves[0].group, leaves, 2, .25, false));
    ASSERT_TRUE(de__anim_sync_step(&hands, leaves[1].group, leaves, 2, .25, false));
    EXPECT_FLOAT_EQ(c.phase, .25, 0);
    EXPECT_FLOAT_EQ(hands.phase, .5, 0);
    ASSERT_TRUE(de__anim_sync_step(&c, leaves[0].group, leaves, 2, .25, false));
    EXPECT_FLOAT_EQ(c.phase, .25, 0);
    leaves[0].speed = -1;
    ASSERT_TRUE(de__anim_sync_step(&c, leaves[0].group, leaves, 2, .5, false));
    EXPECT_FLOAT_EQ(c.phase, 0, 0);
    leaves[0].demand = 0;
    ASSERT_TRUE(de__anim_sync_step(&c, leaves[0].group, leaves, 2, 4, false));
    EXPECT_EQ(c.leader, DE__ANIM_SYNC_NONE);
    EXPECT_FLOAT_EQ(c.phase, 0, 0);
    leaves[0].demand = 1;
    ASSERT_TRUE(de__anim_sync_step(&c, leaves[0].group, leaves, 2, 4.25, false));
    EXPECT_FLOAT_EQ(c.phase, -.25, 0);
    f32 time = -1;
    ASSERT_TRUE(de__anim_sync_sample_time(&c, 2, true, &time));
    EXPECT_FLOAT_EQ(time, 1.5f, 0);
    ASSERT_TRUE(de__anim_sync_sample_time(&c, 2, false, &time));
    EXPECT_FLOAT_EQ(time, 0, 0);
    leaves[0].speed = 1;
    ASSERT_TRUE(de__anim_sync_step(&c, leaves[0].group, leaves, 2, 2.5, true));
    EXPECT_FLOAT_EQ(c.phase, 2.5, 0);
    ASSERT_TRUE(de__anim_sync_sample_time(&c, 2, false, &time));
    EXPECT_FLOAT_EQ(time, 2, 0);
}
static void advance(de__anim_sync_clock *c, u32 begin, u32 end, u32 actor) {
    de__anim_sync_leaf leaves[] = {leaf(0, .5f, 1, 1), leaf(1, .5f, .6f, 1)};
    for (u32 i = begin; i < end; ++i) {
        leaves[0].demand = (i + actor) % 7 < 3 ? .75f : .25f;
        if (!de__anim_sync_step(c, leaves[0].group, leaves, 2, (f64)i / 60, false))
            c->leader = DE__ANIM_SYNC_NONE;
    }
}
TEST(anim_sync, plain_value_restore_replays_leader_changes_without_allocations) {
    de_fp_env_init_thread();
    de__anim_sync_clock c = {0};
    advance(&c, 0, 29, 0);
    de__anim_sync_clock snapshot = c;
    advance(&c, 29, 256, 0);
    de__anim_sync_clock expected = c;
    c = snapshot;
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    advance(&c, 29, 256, 0);
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    EXPECT_MEMEQ(&c, &expected, sizeof c);
}
typedef struct sync_queries {
    de__anim_sync_clock *states;
} sync_queries;
static void evaluate(u32 begin, u32 end, void *user) {
    de_fp_env_init_thread();
    sync_queries *q = user;
    for (u32 i = begin; i < end; ++i)
        advance(&q->states[i], 0, 64, i);
}
TEST(anim_sync, serial_and_parallel_actor_clocks_match_exactly) {
    de_fp_env_init_thread();
    de_task_system *tasks = NULL;
    ASSERT_EQ(de_task_system_create(
                  &(de_task_system_desc){.worker_count = 3, .background_worker_count = 1}, &tasks),
              DE_OK);
    de__anim_sync_clock serial[512] = {0}, parallel[512] = {0};
    sync_queries q = {serial};
    evaluate(0, 512, &q);
    q.states = parallel;
    de_task_parallel_for(tasks, 512, 32, evaluate, &q);
    EXPECT_MEMEQ(serial, parallel, sizeof serial);
    de_task_system_destroy(tasks);
}
TEST(anim_sync, invalid_input_does_not_mutate_clock_or_output) {
    de__anim_sync_leaf leaves[] = {leaf(0, 1, 1, 1)};
    de__anim_sync_clock c = {0};
    ASSERT_TRUE(de__anim_sync_step(&c, leaves[0].group, leaves, 1, .25, false));
    de__anim_sync_clock before = c;
    leaves[0].duration = -1;
    EXPECT_FALSE(de__anim_sync_step(&c, leaves[0].group, leaves, 1, .5, false));
    EXPECT_MEMEQ(&c, &before, sizeof c);
    leaves[0].duration = 1;
    leaves[0].speed = FLT_MAX;
    EXPECT_FALSE(de__anim_sync_step(&c, leaves[0].group, leaves, 1, DBL_MAX, true));
    EXPECT_MEMEQ(&c, &before, sizeof c);
    EXPECT_FALSE(de__anim_sync_step(&c, (de_name){0}, leaves, 1, 0, false));
    EXPECT_MEMEQ(&c, &before, sizeof c);
    f32 out = 17;
    EXPECT_FALSE(de__anim_sync_sample_time(&c, -1, true, &out));
    EXPECT_FLOAT_EQ(out, 17, 0);
}
