/* DENG-260: ECS-driven checksum scenarios on the shared per-tick file harness. */
#include "de_det_harness.h"
#include "ecs2d_test_util.h"
#include "platformer_character2d_course.h"
#include <stdio.h>
#include <stdlib.h>

static de_entity boxed(e2_env *e, de_body_type type, f64 x, f64 y, f32 hx, f32 hy) {
    de_entity entity = e2_body(e, type, x, y, 0.5f);
    DeCollider2D *col = de_entity_get(e->w, entity, e->col);
    col->kind = DE_SHAPE2D_BOX;
    col->radius = 0;
    col->half_extents = (de_vec2){hx, hy};
    return entity;
}
static void pyramid(e2_env *e) {
    (void)boxed(e, DE_BODY_STATIC, 10, -0.5, 20, 0.5f);
    for (u32 row = 0; row < 20; ++row)
        for (u32 i = 0; i < 20 - row; ++i)
            (void)boxed(e, DE_BODY_DYNAMIC, (f64)i + 0.5 * (f64)row, (f64)row + 0.5, 0.49f, 0.49f);
}
static void loop(e2_env *e) {
    const de_vec2 points[] = {{-10, -10}, {-10, 10}, {10, 10}, {10, -10}};
    de_shape terrain = de_shape2d_chain_create(e->p, points, 4, true);
    ASSERT_TRUE(terrain.id != 0);
    de_entity entity = e2_body(e, DE_BODY_STATIC, 0, 0, 0.5f);
    DeCollider2D *col = de_entity_get(e->w, entity, e->col);
    col->shape = terrain;
    for (u32 i = 0; i < 12; ++i) {
        de_entity c = e2_body(e, DE_BODY_DYNAMIC, -7 + (f64)i, 0.3 * (f64)i, 0.3f);
        DeRigidBody2D *rb = de_entity_get(e->w, c, e->rb);
        rb->linear_velocity = (de_vec2){3, (i & 1) ? 2.0f : -2.0f};
    }
}
static u64 combined(e2_env *e) {
    u64 p = de_physics2d_checksum(e->p), w = de_world_checksum(e->w, DE_COMP_SNAPSHOT);
    u8 b[16];
    for (u32 i = 0; i < 8; ++i) {
        b[i] = (u8)(p >> (8 * i));
        b[8 + i] = (u8)(w >> (8 * i));
    }
    return de_hash64(b, sizeof b, 0);
}
static void scenario(const char *name, u32 which, u32 workers) {
    de_task_system *tasks = sv_tasks(workers);
    e2_env e =
        e2_create(which == 1 ? DE_PHYSICS2D_PLANE_TOP_DOWN : DE_PHYSICS2D_PLANE_SIDE, NULL, tasks);
    de_entity character = DE_ENTITY_NULL;
    pc2d_course course = {0};
    if (which == 0)
        pyramid(&e);
    else if (which == 1)
        loop(&e);
    else {
        ASSERT_TRUE(pc2d_build(e.p, &course));
        character = de_entity_spawn(e.w);
        DeTransform *t = de_entity_add(e.w, character, e.builtins.transform);
        *t = de_transform_identity();
        de_dpoint2d start = pc2d_start();
        t->position.x = start.x;
        t->position.z = start.y;
        DeCharacter2D *ch = de_entity_add(e.w, character, e.ch);
        *ch = pc2d_character();
    }
    u64 hashes[1000];
    for (u32 tick = 0; tick < 1000; ++tick) {
        if (which == 2) {
            (void)de_body2d_set_velocity(e.p, course.platform, pc2d_platform_velocity(tick), 0);
            DeCharacter2D *ch = de_entity_get(e.w, character, e.ch);
            ch->desired_velocity = pc2d_input(tick);
        }
        e2_tick(&e, 1);
        hashes[tick] = combined(&e);
        /* Presentation at varying alphas may never alter simulation state. */
        ASSERT_EQ(
            de_world_run_schedule(e.w, DE_SCHEDULE_POST_UPDATE, 1.0f / 144.0f, (f32)(tick % 5) / 4),
            DE_OK);
        EXPECT_EQ(combined(&e), hashes[tick]);
    }
    e2_destroy(&e);
    if (tasks)
        de_task_system_destroy(tasks);
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/%s.checksums", DE_DET_OUT_DIR, name);
    ASSERT_EQ(de_det_write_checksums(path, name, 0, hashes, 1000), DE_OK);
    (void)snprintf(path, sizeof path, "%s/%s.checksums", DE_P2D_GOLDEN_DIR, name);
    if (getenv("DE_REGEN_GOLDEN")) {
        ASSERT_EQ(workers, 0u);
        ASSERT_EQ(de_det_write_checksums(path, name, 0, hashes, 1000), DE_OK);
        return;
    }
    u64 expected[1000];
    u32 count = 0;
    ASSERT_EQ(de_det_read_checksums(path, expected, 1000, &count), DE_OK);
    ASSERT_EQ(count, 1000u);
    EXPECT_EQ(de_det_report(name, expected, hashes, 1000), DE_DET_NO_DIVERGENCE);
}
TEST(pyramid2d_side, workers_0) {
    scenario("pyramid2d_side", 0, 0);
}
TEST(pyramid2d_side, workers_1) {
    scenario("pyramid2d_side", 0, 1);
}
TEST(pyramid2d_side, workers_4) {
    scenario("pyramid2d_side", 0, 4);
}
TEST(chain_terrain_top, workers_0) {
    scenario("chain_terrain_top", 1, 0);
}
TEST(chain_terrain_top, workers_1) {
    scenario("chain_terrain_top", 1, 1);
}
TEST(chain_terrain_top, workers_4) {
    scenario("chain_terrain_top", 1, 4);
}
TEST(platformer_character2d, workers_0) {
    scenario("platformer_character2d", 2, 0);
}
TEST(platformer_character2d, workers_1) {
    scenario("platformer_character2d", 2, 1);
}
TEST(platformer_character2d, workers_4) {
    scenario("platformer_character2d", 2, 4);
}
