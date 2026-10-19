#include "solver2d_test_util.h"
#include "physics/2d/joints2d/joint2d_internal.h"

static de_physics2d_world *world(void) {
    de_physics2d_desc d = {0};
    d.max_bodies = 128;
    d.plane = DE_PHYSICS2D_PLANE_TOP_DOWN;
    de_physics2d_world *p = NULL;
    ASSERT_EQ(de_physics2d_create(&d, &p), DE_OK);
    return p;
}
static de_body ball(de_physics2d_world *p, f32 x, f32 y) {
    return sv_body(p, DE_BODY_DYNAMIC, tu_circle(p, 0.2f, de__v2(0, 0)), tu_xf(x, y, 0), 0, 0, 0);
}
static void ticks(de_physics2d_world *p, u32 n) {
    for (u32 i = 0; i < n; ++i)
        de_physics2d_step(p, SV_DT);
}
static de_joint joint(de_physics2d_world *p, de_joint2d_desc *d) {
    de_joint j = {0};
    ASSERT_EQ(de_joint2d_create(p, d, &j), DE_OK);
    return j;
}
TEST(physics2d_joints, stable_handles_validation_and_cleanup) {
    de_physics2d_world *p = world();
    de_body b = ball(p, 0, 0);
    de_joint2d_desc d = {0};
    d.body_b = b;
    de_joint a = joint(p, &d);
    de_joint2d_destroy(p, a);
    de_joint c = joint(p, &d);
    EXPECT_NE(a.id, c.id);
    EXPECT_EQ(de_joint2d_set_motor(p, a, 1, 2), DE_ERR_INVALID_ARGUMENT);
    de_body2d_destroy(p, b);
    EXPECT_EQ(de_joint2d_set_motor(p, c, 1, 2), DE_ERR_INVALID_ARGUMENT);
    d.body_b.id = 0;
    EXPECT_EQ(de_joint2d_create(p, &d, &a), DE_ERR_INVALID_ARGUMENT);
    de_physics2d_destroy(p);
}
TEST(physics2d_joints, revolute_motor_and_limits) {
    de_physics2d_world *p = world();
    de_body b = ball(p, 0, 0);
    de_joint2d_desc d = {0};
    d.body_b = b;
    d.enable_motor = true;
    d.motor_speed = 2;
    d.max_motor_force = 100;
    de_joint j = joint(p, &d);
    ticks(p, 30);
    EXPECT_FLOAT_EQ(sv_angvel(p, b), 2, 0.02);
    de_joint2d_destroy(p, j);
    d.enable_limit = true;
    d.lower = -0.3f;
    d.upper = 0.3f;
    j = joint(p, &d);
    ticks(p, 180);
    de_transform2d t = sv_xf_of(p, b);
    f32 angle = de_datan2(t.rotation.s, t.rotation.c);
    EXPECT_TRUE(angle <= 0.335f);
    EXPECT_TRUE(angle >= -0.335f);
    de_physics2d_destroy(p);
}
TEST(physics2d_joints, distance_rope) {
    de_physics2d_world *p = sv_world(32, NULL, NULL);
    de_body b = ball(p, 0, -1);
    de_joint2d_desc d = {0};
    d.kind = DE_JOINT2D_DISTANCE;
    d.body_b = b;
    d.enable_limit = true;
    d.upper = 2;
    (void)joint(p, &d);
    ticks(p, 300);
    de_transform2d t = sv_xf_of(p, b);
    EXPECT_TRUE(de_dsqrt((f32)(t.x * t.x + t.y * t.y)) <= 2.001f);
    de_physics2d_destroy(p);
}
TEST(physics2d_joints, weld_rigid_pose) {
    de_physics2d_world *p = sv_world(32, NULL, NULL);
    de_body b = ball(p, 0, 0);
    de_joint2d_desc d = {0};
    d.kind = DE_JOINT2D_WELD;
    d.body_b = b;
    (void)joint(p, &d);
    ticks(p, 300);
    de_transform2d t = sv_xf_of(p, b);
    EXPECT_FLOAT_EQ(t.x, 0, 0.001);
    EXPECT_FLOAT_EQ(t.y, 0, 0.001);
    EXPECT_FLOAT_EQ(t.rotation.s, 0, 0.001);
    de_physics2d_destroy(p);
}
TEST(physics2d_joints, prismatic_motor_limits_and_lock) {
    de_physics2d_world *p = world();
    de_body b = ball(p, 0, 0);
    de_joint2d_desc d = {0};
    d.kind = DE_JOINT2D_PRISMATIC;
    d.body_b = b;
    d.enable_motor = true;
    d.motor_speed = 3;
    d.max_motor_force = 100;
    d.enable_limit = true;
    d.lower = -1;
    d.upper = 1;
    (void)joint(p, &d);
    ticks(p, 180);
    de_transform2d t = sv_xf_of(p, b);
    EXPECT_FLOAT_EQ(t.x, 1, 0.001);
    EXPECT_FLOAT_EQ(t.y, 0, 0.001);
    EXPECT_FLOAT_EQ(t.rotation.s, 0, 0.001);
    de_physics2d_destroy(p);
}
TEST(physics2d_joints, wheel_spring_and_motor) {
    de_physics2d_world *p = world();
    de_body b = ball(p, 0, 0.5f);
    de_joint2d_desc d = {0};
    d.kind = DE_JOINT2D_WHEEL;
    d.body_b = b;
    d.axis = de__v2(0, 1);
    d.hertz = 3;
    d.damping_ratio = 0.7f;
    d.enable_motor = true;
    d.motor_speed = 4;
    d.max_motor_force = 10;
    (void)joint(p, &d);
    ticks(p, 120);
    de_transform2d t = sv_xf_of(p, b);
    EXPECT_FLOAT_EQ(t.y, 0, 0.01);
    EXPECT_FLOAT_EQ(t.x, 0, 0.001);
    EXPECT_FLOAT_EQ(sv_angvel(p, b), 4, 0.05);
    de_physics2d_destroy(p);
}
TEST(physics2d_joints, mouse_target_and_force_limit) {
    de_physics2d_world *p = world();
    de_body b = ball(p, 0, 0);
    de_joint2d_desc d = {0};
    d.kind = DE_JOINT2D_MOUSE;
    d.body_b = b;
    d.max_motor_force = 10;
    de_joint j = joint(p, &d);
    de_dpoint2d target = {2, 1};
    ASSERT_EQ(de_joint2d_mouse_set_target(p, j, target), DE_OK);
    ticks(p, 180);
    de_transform2d t = sv_xf_of(p, b);
    EXPECT_FLOAT_EQ(t.x, 2, 0.01);
    EXPECT_FLOAT_EQ(t.y, 1, 0.01);
    de_physics2d_destroy(p);
}
TEST(physics2d_joints, break_force_and_collision_rule) {
    de_physics2d_world *p = world();
    de_body a = ball(p, 0, 0), b = ball(p, 1, 0);
    de_joint2d_desc d = {0};
    d.body_a = a;
    d.body_b = b;
    d.break_force = 0.01f;
    de_joint j = joint(p, &d);
    EXPECT_FALSE(de__joints2d_collide(p, tu_index(a), tu_index(b)));
    ticks(p, 1);
    EXPECT_TRUE(de_joint2d_is_broken(p, j));
    EXPECT_TRUE(de__joints2d_collide(p, tu_index(a), tu_index(b)));
    de_physics2d_destroy(p);
}
TEST(physics2d_joints, pendulum_period_within_one_percent) {
    de_physics2d_world *p = sv_world(32, NULL, NULL);
    de_body b = ball(p, 0.1f, -0.994987437f);
    de_joint2d_desc d = {0};
    d.kind = DE_JOINT2D_DISTANCE;
    d.body_b = b;
    d.upper = 1;
    (void)joint(p, &d);
    f64 previous = 0.1;
    u32 crossing[3] = {0};
    u32 found = 0;
    for (u32 i = 1; i <= 600; ++i) {
        de_physics2d_step(p, SV_DT);
        f64 x = sv_xf_of(p, b).x;
        if (previous > 0 && x <= 0 && found < 3)
            crossing[found++] = i;
        previous = x;
    }
    ASSERT_EQ(found, 3u);
    f64 period = (f64)(crossing[2] - crossing[0]) * SV_DT / 2;
    f64 analytic = 2 * 3.141592653589793 / de_dsqrt(9.81f);
    EXPECT_TRUE(fabs(period / analytic - 1) < 0.01);
    de_physics2d_destroy(p);
}
TEST(physics2d_joints, wheel_frequency_within_five_percent) {
    de_physics2d_world *p = world();
    de_body b = ball(p, 0, 0.5f);
    de_joint2d_desc d = {0};
    d.kind = DE_JOINT2D_WHEEL;
    d.body_b = b;
    d.axis = de__v2(0, 1);
    d.hertz = 3;
    d.damping_ratio = 0.01f;
    (void)joint(p, &d);
    f64 prev = 0.5;
    u32 first = 0, last = 0, count = 0;
    for (u32 i = 1; i <= 120; ++i) {
        de_physics2d_step(p, SV_DT);
        f64 y = sv_xf_of(p, b).y;
        if (prev > 0 && y <= 0) {
            if (count == 0)
                first = i;
            last = i;
            ++count;
        }
        prev = y;
    }
    EXPECT_TRUE(count >= 3);
    if (count >= 3) {
        f64 hz = (count - 1) / ((last - first) * (f64)SV_DT);
        EXPECT_TRUE(fabs(hz / 3 - 1) < 0.05);
    }
    de_physics2d_destroy(p);
}
TEST(physics2d_joints, no_allocation_in_steady_state) {
    de_tracking_allocator tracking;
    de_allocator a = de_tracking_allocator_init(&tracking, NULL, DE_MEM_PHYSICS);
    de_physics2d_world *p = sv_world(32, NULL, &a);
    de_body b = ball(p, 0, -1);
    de_joint2d_desc d = {0};
    d.kind = DE_JOINT2D_DISTANCE;
    d.body_b = b;
    d.upper = 1;
    (void)joint(p, &d);
    ticks(p, 1);
    de_tracking_stats before = de_tracking_allocator_stats(&tracking);
    ticks(p, 120);
    (void)de_physics2d_checksum(p);
    de_tracking_stats after = de_tracking_allocator_stats(&tracking);
    EXPECT_EQ(before.alloc_count, after.alloc_count);
    EXPECT_EQ(before.realloc_count, after.realloc_count);
    de_physics2d_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
}
TEST(physics2d_joints, mouse_force_is_bounded) {
    de_physics2d_world *p = world();
    de_body b = ball(p, 0, 0);
    de_joint2d_desc d = {0};
    d.kind = DE_JOINT2D_MOUSE;
    d.body_b = b;
    d.world_anchor_a.x = 100;
    d.world_anchor_a.y = 100;
    d.max_motor_force = 0.01f;
    (void)joint(p, &d);
    ticks(p, 1);
    de_vec2 v = sv_vel(p, b);
    f32 mass = p->bodies.mass[tu_index(b)];
    EXPECT_TRUE(de_dsqrt(de__v2_dot(v, v)) * mass <= 0.01f * SV_DT * 1.001f);
    de_physics2d_destroy(p);
}

TEST(physics2d_joints, checksum_changes_with_motor_and_mouse_target_before_stepping) {
    de_physics2d_world *p = world();
    de_body b = ball(p, 0, 0);
    de_joint2d_desc d = {.body_b = b};
    de_joint j = joint(p, &d);
    u64 before = de_physics2d_checksum(p);
    ASSERT_EQ(de_joint2d_set_motor(p, j, 2, 10), DE_OK);
    EXPECT_NE(de_physics2d_checksum(p), before);
    before = de_physics2d_checksum(p);
    ASSERT_EQ(de_joint2d_set_motor(p, j, 3, 10), DE_OK);
    EXPECT_NE(de_physics2d_checksum(p), before);
    before = de_physics2d_checksum(p);
    ASSERT_EQ(de_joint2d_set_motor(p, j, 3, 11), DE_OK);
    EXPECT_NE(de_physics2d_checksum(p), before);
    de_joint2d_destroy(p, j);
    d.kind = DE_JOINT2D_MOUSE;
    j = joint(p, &d);
    before = de_physics2d_checksum(p);
    ASSERT_EQ(de_joint2d_mouse_set_target(p, j, (de_dpoint2d){1, 2}), DE_OK);
    EXPECT_NE(de_physics2d_checksum(p), before);
    before = de_physics2d_checksum(p);
    ASSERT_EQ(de_joint2d_mouse_set_target(p, j, (de_dpoint2d){1 + 1e-12, 2}), DE_OK);
    EXPECT_NE(de_physics2d_checksum(p), before);
    de_physics2d_destroy(p);
}
static u64 description_checksum(de_joint2d_desc d) {
    de_physics2d_world *p = world();
    d.body_b = ball(p, 0, 0);
    (void)joint(p, &d);
    u64 checksum = de_physics2d_checksum(p);
    de_physics2d_destroy(p);
    return checksum;
}
TEST(physics2d_joints, checksum_covers_future_behavior_and_ignores_padding) {
    de_joint2d_desc d = {.axis = {0.6f, 0.8f},
                         .lower = -2,
                         .upper = 2,
                         .hertz = 3,
                         .damping_ratio = 0.5f,
                         .world_anchor_a = {1, 2},
                         .world_anchor_b = {3, 4}};
    u64 base = description_checksum(d);
#define DIFFERENT(field, value)                                                                    \
    do {                                                                                           \
        de_joint2d_desc variant = d;                                                               \
        variant.field = (value);                                                                   \
        EXPECT_NE(description_checksum(variant), base);                                            \
    } while (0)
    DIFFERENT(local_anchor_a.x, 0.25f);
    DIFFERENT(local_anchor_a.y, 0.25f);
    DIFFERENT(local_anchor_b.x, 0.25f);
    DIFFERENT(local_anchor_b.y, 0.25f);
    DIFFERENT(world_anchor_a.x, 1 + 1e-12);
    DIFFERENT(world_anchor_a.y, 2 + 1e-12);
    DIFFERENT(world_anchor_b.x, 3 + 1e-12);
    DIFFERENT(world_anchor_b.y, 4 + 1e-12);
    DIFFERENT(axis.x, 0.25f);
    DIFFERENT(axis.y, 0.25f);
    DIFFERENT(reference_angle, 0.25f);
    DIFFERENT(enable_limit, true);
    DIFFERENT(enable_motor, true);
    DIFFERENT(collide_connected, true);
    DIFFERENT(lower, -1);
    DIFFERENT(upper, 3);
    DIFFERENT(motor_speed, 1);
    DIFFERENT(max_motor_force, 1);
    DIFFERENT(hertz, 4);
    DIFFERENT(damping_ratio, 0.6f);
    DIFFERENT(break_force, 1);
#undef DIFFERENT
    de_joint2d_desc padded = d;
    for (size_t i = offsetof(de_joint2d_desc, collide_connected) + sizeof(bool);
         i < offsetof(de_joint2d_desc, lower); ++i)
        ((u8 *)&padded)[i] = 0xa5;
    EXPECT_EQ(description_checksum(padded), base);
    d.axis = (de_vec2){0, 0};
    base = description_checksum(d);
    d.axis = (de_vec2){2, 0};
    EXPECT_EQ(description_checksum(d), base);
}
