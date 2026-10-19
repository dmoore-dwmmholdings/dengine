/* DENG-256: world-level bridge and wheel-car scenarios. Uses the existing checksum
 * harness file format and comparator; per-tick state includes 2D bodies, contacts and joints. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* portable getenv/sscanf in trusted test inputs */
#endif
#include "de_det_harness.h"
#include "de_test.h"
#include <dengine/de_physics.h>
#include <dengine/de_task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICKS 1000u
static de_body body(de_physics2d_world *p, de_body_type type, de_shape shape, f64 x, f64 y) {
    de_body2d_desc d = {0};
    d.type = type;
    d.shape = shape;
    d.transform.x = x;
    d.transform.y = y;
    d.transform.rotation.c = 1;
    d.gravity_scale = 1;
    d.material.friction = 0.7f;
    return de_body2d_create(p, &d);
}
static de_shape box(de_physics2d_world *p, f32 x, f32 y) {
    de_shape2d_desc d = {0};
    d.kind = DE_SHAPE2D_BOX;
    d.half_extents.x = x;
    d.half_extents.y = y;
    return de_shape2d_create(p, &d);
}
static de_shape circle(de_physics2d_world *p, f32 radius) {
    de_shape2d_desc d = {0};
    d.kind = DE_SHAPE2D_CIRCLE;
    d.radius = radius;
    return de_shape2d_create(p, &d);
}
static void make_joint(de_physics2d_world *p, de_joint2d_desc *d) {
    de_joint j = {0};
    ASSERT_EQ(de_joint2d_create(p, d, &j), DE_OK);
}
static void bridge(de_physics2d_world *p) {
    (void)body(p, DE_BODY_STATIC, box(p, 20, 0.5f), 12, -1);
    de_shape plank = box(p, 0.4f, 0.1f);
    de_body prev = {0};
    for (u32 i = 0; i < 30; ++i) {
        de_body link = body(p, DE_BODY_DYNAMIC, plank, 0.8 * (f64)i, 3);
        de_joint2d_desc d = {0};
        d.body_a = prev;
        d.body_b = link;
        d.local_anchor_a.x = 0.4f;
        d.local_anchor_b.x = -0.4f;
        if (i == 0) {
            d.world_anchor_a.x = -0.4;
            d.world_anchor_a.y = 3;
        }
        make_joint(p, &d);
        prev = link;
    }
    de_joint2d_desc end = {0};
    end.body_b = prev;
    end.local_anchor_b.x = 0.4f;
    end.world_anchor_a.x = 23.6;
    end.world_anchor_a.y = 3;
    make_joint(p, &end);
    de_shape load = box(p, 0.2f, 0.2f);
    for (u32 i = 0; i < 8; ++i)
        (void)body(p, DE_BODY_DYNAMIC, load, 2.5 * (f64)i + 1, 5 + 0.5 * (f64)i);
}
static void car(de_physics2d_world *p) {
    de_vec2 points[81];
    /* Chains face right: descending x gives the upper surface its front normal. */
    for (u32 i = 0; i < 81; ++i) {
        points[i].x = 40 - (f32)i;
        points[i].y = 0.25f * de_dsin((f32)i * 0.4f);
    }
    de_shape terrain = de_shape2d_chain_create(p, points, 81, false);
    ASSERT_TRUE(terrain.id != 0);
    (void)body(p, DE_BODY_STATIC, terrain, 0, 0);
    de_body chassis = body(p, DE_BODY_DYNAMIC, box(p, 1, 0.25f), 0, 2);
    de_shape tire = circle(p, 0.4f);
    for (u32 i = 0; i < 2; ++i) {
        f32 x = i == 0 ? -0.7f : 0.7f;
        de_body wheel = body(p, DE_BODY_DYNAMIC, tire, x, 1.5);
        de_joint2d_desc d = {0};
        d.kind = DE_JOINT2D_WHEEL;
        d.body_a = chassis;
        d.body_b = wheel;
        d.local_anchor_a.x = x;
        d.local_anchor_a.y = -0.5f;
        d.axis.y = 1;
        d.hertz = 4;
        d.damping_ratio = 0.7f;
        d.enable_motor = true;
        d.motor_speed = -8;
        d.max_motor_force = 15;
        make_joint(p, &d);
    }
}
static void run(const char *name, bool is_car, u32 workers) {
    de_fp_env_init_thread();
    de_task_system *tasks = NULL;
    if (workers) {
        de_task_system_desc td = {0};
        td.worker_count = workers;
        td.background_worker_count = 1;
        ASSERT_EQ(de_task_system_create(&td, &tasks), DE_OK);
    }
    de_physics2d_desc d = {0};
    d.max_bodies = 256;
    d.tasks = tasks;
    de_physics2d_world *p = NULL;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    if (is_car)
        car(p);
    else
        bridge(p);
    u64 hashes[TICKS];
    for (u32 i = 0; i < TICKS; ++i) {
        de_physics2d_step(p, 1.0f / 60.0f);
        hashes[i] = de_physics2d_checksum(p);
    }
    de_physics2d_destroy(p);
    if (tasks)
        de_task_system_destroy(tasks);
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/%s.checksums", DE_DET_OUT_DIR, name);
    ASSERT_EQ(de_det_write_checksums(path, name, 0, hashes, TICKS), DE_OK);
    (void)snprintf(path, sizeof path, "%s/%s.checksums", DE_P2D_GOLDEN_DIR, name);
    if (getenv("DE_REGEN_GOLDEN")) {
        ASSERT_EQ(workers, 0u);
        ASSERT_EQ(de_det_write_checksums(path, name, 0, hashes, TICKS), DE_OK);
        return;
    }
    u64 expected[TICKS];
    u32 count = 0;
    ASSERT_EQ(de_det_read_checksums(path, expected, TICKS, &count), DE_OK);
    ASSERT_EQ(count, TICKS);
    EXPECT_EQ(de_det_report(name, expected, hashes, TICKS), DE_DET_NO_DIVERGENCE);
}
TEST(bridge2d, workers_0) {
    run("bridge2d", false, 0);
}
TEST(bridge2d, workers_1) {
    run("bridge2d", false, 1);
}
TEST(bridge2d, workers_4) {
    run("bridge2d", false, 4);
}
TEST(car2d, workers_0) {
    run("car2d", true, 0);
}
TEST(car2d, workers_1) {
    run("car2d", true, 1);
}
TEST(car2d, workers_4) {
    run("car2d", true, 4);
}
