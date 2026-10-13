/* test_solver_basic.c: integration of the soft-step solver without contacts (DENG-217, SPEC
 * §13.1, §5.5): free fall against the substep (symplectic Euler) solution and the analytic one,
 * gravity scale, damping, kinematic motion and infinite mass, rotation locks, speed clamps, a
 * spinning body with an offset center of mass, and the same motion 100 km from the origin. */
#include "solver_test_util.h"

static de_body free_body(de_physics_world *p, de_body_type type, de_shape s, de_transform t,
                         f32 gravity_scale) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform = t;
    d.gravity_scale = gravity_scale;
    return de_body_create(p, &d);
}

TEST(physics_solver_basic, free_fall_matches_substep_integration) {
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape ball = st_sphere(p, 0.5f);
    const de_body b =
        free_body(p, DE_BODY_DYNAMIC, ball, st_xform(0, 0, 100, st_quat_identity()), 1.0f);
    const de_body half =
        free_body(p, DE_BODY_DYNAMIC, ball, st_xform(10, 0, 100, st_quat_identity()), 0.5f);
    const de_body none =
        free_body(p, DE_BODY_DYNAMIC, ball, st_xform(20, 0, 100, st_quat_identity()), 0.0f);
    const f64 g = 9.81, h = (1.0 / 60.0) / 4.0;
    for (u32 tick = 1; tick <= 60; ++tick) {
        de_physics_step(p, ST_DT);
        const f64 n = 4.0 * (f64)tick, t = n * h;
        /* Symplectic Euler per substep: v_n = -g h n, z_n = z0 - g h^2 n (n + 1) / 2. */
        const f64 z_sub = 100.0 - g * h * h * n * (n + 1.0) * 0.5;
        const f64 z_exact = 100.0 - 0.5 * g * t * t;
        const f64 z = st_pos(p, b).z;
        EXPECT_FLOAT_EQ(z, z_sub, 1e-4);
        /* The substep error bound: |z - z_exact| = g h t / 2. */
        ST_LT(de__absd(z - z_exact), 0.5 * g * h * t + 1e-4);
        EXPECT_FLOAT_EQ(st_vel(p, b).z, -g * t, 1e-4);
        EXPECT_FLOAT_EQ(100.0 - st_pos(p, half).z, 0.5 * (100.0 - z), 1e-4);
    }
    EXPECT_TRUE(st_pos(p, none).z == 100.0);
    EXPECT_TRUE(st_pos(p, b).x == 0.0 && st_pos(p, b).y == 0.0);
    /* The world AABB follows the body (queries between steps see it). */
    EXPECT_FLOAT_EQ(p->bodies.world_aabb[de__handle_index(b.id)].min.z, st_pos(p, b).z - 0.5, 1e-4);
    de_physics_destroy(p);
}

TEST(physics_solver_basic, damping) {
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    de_physics_set_gravity(p, (de_vec3){0, 0, 0});
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = cube;
    d.transform = st_xform(0, 0, 0, st_quat_identity());
    d.linear_velocity = (de_vec3){10.0f, 0.0f, 0.0f};
    d.angular_velocity = (de_vec3){0.0f, 0.0f, 4.0f};
    d.linear_damping = 0.5f;
    d.angular_damping = 2.0f;
    const de_body b = de_body_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    for (u32 tick = 0; tick < 60; ++tick)
        de_physics_step(p, ST_DT);
    /* 240 substeps of v *= 1 / (1 + h c). */
    const f64 h = 1.0 / 240.0;
    f64 lin = 10.0, ang = 4.0;
    for (u32 k = 0; k < 240; ++k) {
        lin /= 1.0 + h * 0.5;
        ang /= 1.0 + h * 2.0;
    }
    EXPECT_FLOAT_EQ(st_vel(p, b).x, lin, 1e-4);
    EXPECT_FLOAT_EQ(st_angvel(p, b).z, ang, 1e-4);
    ST_LT(de__absd(lin - 10.0 * 0.6065), 0.02); /* close to exp(-c t) */
    de_physics_destroy(p);
}

TEST(physics_solver_basic, kinematic_moves_by_velocity_with_infinite_mass) {
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_KINEMATIC;
    d.shape = cube;
    d.transform = st_xform(-3, 0, 0, st_quat_identity());
    d.linear_velocity = (de_vec3){2.0f, 0.0f, 0.0f};
    d.angular_velocity = (de_vec3){0.0f, 0.0f, 0.5f};
    d.gravity_scale = 1.0f; /* kinematic bodies ignore gravity */
    const de_body k = de_body_create(p, &d);
    /* A dynamic box in its way, no gravity: the kinematic body pushes it and does not slow. */
    const de_body box =
        free_body(p, DE_BODY_DYNAMIC, cube, st_xform(0, 0, 0, st_quat_identity()), 0.0f);
    for (u32 tick = 1; tick <= 90; ++tick) {
        de_physics_step(p, ST_DT);
        EXPECT_FLOAT_EQ(st_pos(p, k).x, -3.0 + 2.0 * (f64)tick / 60.0, 1e-5);
        EXPECT_TRUE(st_vel(p, k).x == 2.0f && st_angvel(p, k).z == 0.5f);
    }
    EXPECT_FLOAT_EQ(st_pos(p, k).y, 0.0, 1e-9);
    EXPECT_FLOAT_EQ(st_pos(p, k).z, 0.0, 1e-9);
    /* Rotated by 0.5 rad/s * 1.5 s about +Z. */
    const de_quat q = st_rot(p, k);
    EXPECT_FLOAT_EQ(q.z, de_dsin(0.375f), 1e-4);
    EXPECT_FLOAT_EQ(q.w, de_dcos(0.375f), 1e-4);
    /* The box was hit and moves at least as fast as the pusher. */
    ST_GT(st_pos(p, box).x, 0.5);
    ST_GT(st_vel(p, box).x, 1.9f);
    de_physics_destroy(p);
}

TEST(physics_solver_basic, rotation_locks) {
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    const de_shape ground = st_box(p, 5.0f, 5.0f, 0.5f);
    (void)st_body(p, DE_BODY_STATIC, ground, st_xform(0, 0, -0.5, st_quat_identity()), 0.8f, 0.0f);
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = cube;
    d.gravity_scale = 1.0f;
    d.material.friction = 0.8f;
    /* All locked, dropped on an edge: it never rotates (it lands flat-tilted and stays so). */
    const de_quat tilt = st_quat_axis_angle((de_vec3){1, 0, 0}, 0.5f);
    d.transform = st_xform(-2, 0, 1.5, tilt);
    d.angular_velocity = (de_vec3){1.0f, 2.0f, 3.0f};
    d.flags = DE_BODY_FLAG_LOCK_ROT_X | DE_BODY_FLAG_LOCK_ROT_Y | DE_BODY_FLAG_LOCK_ROT_Z;
    const de_body all = de_body_create(p, &d);
    /* Z locked only, in the air: x and y spin stays, z spin goes. */
    d.transform = st_xform(2, 0, 50, st_quat_identity());
    d.flags = DE_BODY_FLAG_LOCK_ROT_Z;
    const de_body z_only = de_body_create(p, &d);
    /* Unlocked twin of `all`: it does tip over. */
    d.transform = st_xform(0, 3, 1.5, tilt);
    d.angular_velocity = (de_vec3){0.0f, 0.0f, 0.0f};
    d.flags = 0;
    const de_body free_twin = de_body_create(p, &d);
    for (u32 tick = 0; tick < 120; ++tick) {
        de_physics_step(p, ST_DT);
        const de_quat q = st_rot(p, all);
        EXPECT_TRUE(q.x == tilt.x && q.y == tilt.y && q.z == tilt.z && q.w == tilt.w);
        const de_vec3 w = st_angvel(p, all);
        EXPECT_TRUE(w.x == 0.0f && w.y == 0.0f && w.z == 0.0f);
        const de_vec3 wz = st_angvel(p, z_only);
        EXPECT_TRUE(wz.x == 1.0f && wz.y == 2.0f && wz.z == 0.0f);
    }
    ST_LT(st_pos(p, all).z, 1.0);                   /* it fell and rests on its edge */
    ST_LT(de__absf(st_rot(p, free_twin).x), 0.05f); /* the unlocked twin tipped flat */
    de_physics_destroy(p);
}

TEST(physics_solver_basic, speed_clamps) {
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    de_physics_set_gravity(p, (de_vec3){0, 0, 0});
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = cube;
    d.transform = st_xform(0, 0, 0, st_quat_identity());
    d.linear_velocity = (de_vec3){600.0f, 800.0f, 0.0f};
    d.angular_velocity = (de_vec3){0.0f, 0.0f, 1000.0f};
    const de_body fast = de_body_create(p, &d);
    d.type = DE_BODY_KINEMATIC;
    d.transform = st_xform(0, 100, 0, st_quat_identity());
    const de_body kin = de_body_create(p, &d);
    de_physics_step(p, ST_DT);
    const de_vec3 v = st_vel(p, fast);
    EXPECT_FLOAT_EQ(st_lenf(v), DE__SOLVER_DEFAULT_MAX_LINEAR_SPEED, 1e-2);
    EXPECT_FLOAT_EQ(v.x / v.y, 0.75f, 1e-5); /* direction kept */
    /* Angular: at most a quarter turn (pi / 4) per substep. */
    EXPECT_FLOAT_EQ(st_angvel(p, fast).z, 0.25f * 3.14159265f * 240.0f, 0.5f);
    /* Kinematic velocities are inputs: never clamped. */
    EXPECT_TRUE(st_vel(p, kin).x == 600.0f && st_angvel(p, kin).z == 1000.0f);
    /* The clamp is a setting (cvar physics.solver.max_linear_speed). */
    st_solver(p)->settings.max_linear_speed = 50.0f;
    de_physics_step(p, ST_DT);
    EXPECT_FLOAT_EQ(st_lenf(st_vel(p, fast)), 50.0f, 1e-3);
    de_physics_destroy(p);
}

TEST(physics_solver_basic, offset_center_of_mass_spins_in_place) {
    /* A compound whose only child sits 1 m along +X: the center of mass is (1, 0, 0) in the body
     * frame. Spinning freely, the center of mass stays put and the origin orbits it. */
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    de_physics_set_gravity(p, (de_vec3){0, 0, 0});
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    de_transform child = st_xform(1, 0, 0, st_quat_identity());
    de_shape_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.kind = DE_SHAPE_COMPOUND;
    sd.children = &cube;
    sd.child_xforms = &child;
    sd.child_count = 1;
    const de_shape comp = de_shape_create(p, &sd);
    ASSERT_TRUE(DE_HANDLE_VALID(comp));
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = comp;
    d.transform = st_xform(5, 5, 5, st_quat_identity());
    d.angular_velocity = (de_vec3){0.0f, 0.0f, 3.14159265f}; /* half a turn per second */
    const de_body b = de_body_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    for (u32 tick = 0; tick < 60; ++tick) {
        de_physics_step(p, ST_DT);
        const de_quat q = st_rot(p, b);
        const de_dvec3 o = st_pos(p, b);
        const de_vec3 c = de__quat_rotate(q, (de_vec3){1, 0, 0});
        EXPECT_FLOAT_EQ(o.x + (f64)c.x, 6.0, 1e-5);
        EXPECT_FLOAT_EQ(o.y + (f64)c.y, 5.0, 1e-5);
        EXPECT_FLOAT_EQ(o.z, 5.0, 1e-9);
    }
    /* After a half turn the origin is on the other side of the center of mass. */
    EXPECT_FLOAT_EQ(st_pos(p, b).x, 7.0, 1e-3);
    EXPECT_FLOAT_EQ(st_pos(p, b).y, 5.0, 1e-3);
    de_physics_destroy(p);
}

TEST(physics_solver_basic, large_world_same_motion) {
    /* SPEC §5.5: a falling, spinning body 100 km out moves exactly as one at the origin (the
     * solver integrates f32 deltas and adds them to the f64 position once per step). */
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape cube = st_box(p, 0.5f, 0.25f, 0.125f);
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = cube;
    d.gravity_scale = 1.0f;
    d.linear_velocity = (de_vec3){3.0f, -1.0f, 4.0f};
    d.angular_velocity = (de_vec3){0.3f, 2.0f, -1.0f};
    d.transform = st_xform(0, 0, 0, st_quat_identity());
    const de_body near = de_body_create(p, &d);
    d.transform = st_xform(100000.0, -100000.0, 0, st_quat_identity());
    const de_body far = de_body_create(p, &d);
    for (u32 tick = 0; tick < 120; ++tick)
        de_physics_step(p, ST_DT);
    const de_dvec3 a = st_pos(p, near), b = st_pos(p, far);
    EXPECT_FLOAT_EQ(b.x - 100000.0, a.x, 1e-9);
    EXPECT_FLOAT_EQ(b.y + 100000.0, a.y, 1e-9);
    EXPECT_TRUE(b.z == a.z);
    const de_quat qa = st_rot(p, near), qb = st_rot(p, far);
    EXPECT_TRUE(qa.x == qb.x && qa.y == qb.y && qa.z == qb.z && qa.w == qb.w);
    de_physics_destroy(p);
}

TEST(physics_solver_basic, static_bodies_never_move) {
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    const de_body s = st_body(p, DE_BODY_STATIC, cube, st_xform(1, 2, 3, st_quat_identity()), 0, 0);
    const de_body falling =
        st_body(p, DE_BODY_DYNAMIC, cube, st_xform(1, 2, 5, st_quat_identity()), 0.5f, 0.0f);
    for (u32 tick = 0; tick < 120; ++tick)
        de_physics_step(p, ST_DT);
    EXPECT_TRUE(st_pos(p, s).x == 1.0 && st_pos(p, s).y == 2.0 && st_pos(p, s).z == 3.0);
    EXPECT_FLOAT_EQ(st_pos(p, falling).z, 4.0, 0.01); /* landed on it */
    de_physics_destroy(p);
}
