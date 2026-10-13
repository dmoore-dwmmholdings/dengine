/*
 * test_joint_limits_motors.c: joint limits, motors, the cone-twist ellipse, 6-DOF limits, the
 * break force and worker-count invariance (DENG-220). Carries the tsan label: scenes stay small.
 */
#include "joint_test_util.h"

#include <dengine/de_task.h>

#include "physics/solver/graph_color.h"

#define DEG (JT_PI / 180.0f)

static de_physics_world *zero_g_world(u32 max_bodies) {
    de_physics_world *p = st_world(max_bodies, NULL, NULL);
    if (p != NULL)
        de_physics_set_gravity(p, (de_vec3){0, 0, 0});
    return p;
}

/* Twist of body b's frame relative to the world frame `fa` (B's frame rotation `fb`). */
static f32 twist_of(const de_physics_world *p, de_body b, de_quat fa, de_quat fb) {
    f32 tw, sy, sz;
    jt_angles(st_quat_identity(), fa, st_rot(p, b), fb, &tw, &sy, &sz);
    return tw;
}

/* ================================ Hinge ================================================== */

/*
 * A 1 m beam (40 kg) on a world hinge about +Y at (0, 0, 5), horizontal along +X at the start.
 * Gravity turns it about +Y (positive twist). Returns the hinge's frame rotation.
 */
static de_quat hinge_y(void) {
    return st_quat_axis_angle((de_vec3){0, 0, 1}, 0.5f * JT_PI); /* frame X -> world Y */
}

static de_body beam(de_physics_world *p, f32 mass) {
    const de_shape s = st_box(p, 0.5f, 0.1f, 0.1f);
    return jt_dyn(p, s, st_xform(0.5, 0, 5, st_quat_identity()), mass);
}

TEST(physics_joint_limits, hinge_limit_holds_under_load) {
    for (u32 side = 0; side < 2; ++side) {
        de_physics_world *p = st_world(8, NULL, NULL);
        const de_body b = beam(p, 200.0f);
        de_joint_desc d =
            jt_desc(DE_JOINT_HINGE, (de_body){0}, b, (de_dvec3){0, 0, 5}, (de_dvec3){-0.5, 0, 0});
        d.frame_a.rotation = hinge_y();
        d.frame_b.rotation = hinge_y();
        d.enable_limit = true;
        d.limits_min[0] = -45.0f * DEG;
        d.limits_max[0] = 45.0f * DEG;
        ASSERT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
        /* Slam it into the limit at 6 rad/s (down, or up against gravity). */
        const f32 w = side == 0 ? 6.0f : -6.0f;
        de_body_set_velocity(p, b, (de_vec3){0, 0, -w * 0.5f}, (de_vec3){0, w, 0});
        f32 worst = 0.0f, last = 0.0f;
        for (u32 tick = 0; tick < 120; ++tick) {
            de_physics_step(p, ST_DT);
            const f32 a = twist_of(p, b, hinge_y(), hinge_y());
            const f32 over = side == 0 ? a - 45.0f * DEG : -45.0f * DEG - a;
            worst = over > worst ? over : worst;
            last = a;
        }
        fprintf(stderr, "  side %u: overshoot %.3f deg, rests at %.3f deg\n", side,
                (f64)(worst / DEG), (f64)(last / DEG));
        EXPECT_TRUE(worst < 2.0f * DEG);
        if (side == 0) /* gravity holds it on the limit */
            EXPECT_TRUE(last > 43.0f * DEG && last < 47.0f * DEG);
        de_physics_destroy(p);
    }
}

/* A 1 m bar (40 kg) on a vertical world hinge through its center. */
static de_body spinner(de_physics_world *p, de_joint_desc *d) {
    const de_shape s = st_box(p, 0.5f, 0.1f, 0.1f);
    const de_body b = jt_dyn(p, s, st_xform(0, 0, 1, st_quat_identity()), 0.0f);
    *d = jt_desc(DE_JOINT_HINGE, (de_body){0}, b, (de_dvec3){0, 0, 1}, (de_dvec3){0, 0, 0});
    d->frame_a.rotation = st_quat_axis_angle((de_vec3){0, 1, 0}, -0.5f * JT_PI); /* X -> Z */
    d->frame_b.rotation = d->frame_a.rotation;
    return b;
}

TEST(physics_joint_limits, hinge_velocity_motor) {
    de_physics_world *p = st_world(8, NULL, NULL);
    de_joint_desc d;
    const de_body b = spinner(p, &d);
    d.motor_mode[0] = DE_JOINT_MOTOR_VELOCITY;
    d.motor_target[0] = 3.0f;
    d.motor_max_force[0] = 500.0f; /* N m; the bar's inertia is about 3.4 kg m^2 */
    const de_joint j = de_joint_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(j));
    for (u32 tick = 0; tick < 30; ++tick) /* 0.5 s */
        de_physics_step(p, ST_DT);
    const f32 w = st_angvel(p, b).z;
    fprintf(stderr, "  velocity motor after 0.5 s: %.5f rad/s\n", (f64)w);
    EXPECT_TRUE(w > 2.97f && w < 3.03f);
    /* A new target through the setter; reverse within 0.5 s. */
    EXPECT_TRUE(de_joint_set_motor_target(p, j, 0, -2.0f));
    for (u32 tick = 0; tick < 30; ++tick)
        de_physics_step(p, ST_DT);
    EXPECT_TRUE(st_angvel(p, b).z < -1.98f && st_angvel(p, b).z > -2.02f);
    de_physics_destroy(p);

    /* A weak motor accelerates at max torque / inertia and does not reach 3 rad/s in 0.5 s. */
    p = st_world(8, NULL, NULL);
    const de_body b2 = spinner(p, &d);
    d.motor_mode[0] = DE_JOINT_MOTOR_VELOCITY;
    d.motor_target[0] = 3.0f;
    d.motor_max_force[0] = 4.0f;
    ASSERT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    for (u32 tick = 0; tick < 30; ++tick)
        de_physics_step(p, ST_DT);
    de_mass_props mp;
    ASSERT_TRUE(de_body_get_mass_props(p, b2, &mp));
    const f32 expect = 4.0f / mp.inertia.m[8] * 0.5f;
    fprintf(stderr, "  weak motor: %.4f rad/s (torque / inertia * t = %.4f)\n",
            (f64)st_angvel(p, b2).z, (f64)expect);
    EXPECT_TRUE(st_angvel(p, b2).z > 0.97f * expect && st_angvel(p, b2).z < 1.03f * expect);
    de_physics_destroy(p);
}

TEST(physics_joint_limits, hinge_position_motor_converges) {
    for (u32 mode = 0; mode < 2; ++mode) {
        de_physics_world *p = st_world(8, NULL, NULL);
        de_joint_desc d;
        const de_body b = spinner(p, &d);
        d.motor_mode[0] = DE_JOINT_MOTOR_POSITION;
        d.motor_target[0] = 0.5f;
        d.motor_max_force[0] = 200.0f;
        d.motor_hertz = mode == 0 ? 0.0f : 3.0f; /* stiff, or a 3 Hz critically damped spring */
        d.motor_damping_ratio = 1.0f;
        ASSERT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
        const de_quat f = d.frame_a.rotation;
        for (u32 tick = 0; tick < 90; ++tick)
            de_physics_step(p, ST_DT);
        const f32 a = twist_of(p, b, f, f);
        fprintf(stderr, "  position motor (hertz %g) after 1.5 s: %.5f rad, w %.5f\n",
                (f64)d.motor_hertz, (f64)a, (f64)st_angvel(p, b).z);
        EXPECT_TRUE(a > 0.49f && a < 0.51f);
        EXPECT_TRUE(st_angvel(p, b).z < 0.05f && st_angvel(p, b).z > -0.05f);
        de_physics_destroy(p);
    }
}

/* ================================ Slider, distance ======================================= */

TEST(physics_joint_limits, slider_limit_and_motors) {
    de_physics_world *p = st_world(8, NULL, NULL);
    const de_shape s = st_box(p, 0.2f, 0.2f, 0.2f);
    const de_body b = jt_dyn(p, s, st_xform(0, 0, 2, st_quat_identity()), 0.0f);
    de_joint_desc d =
        jt_desc(DE_JOINT_SLIDER, (de_body){0}, b, (de_dvec3){0, 0, 2}, (de_dvec3){0, 0, 0});
    d.enable_limit = true;
    d.limits_min[0] = -0.5f;
    d.limits_max[0] = 0.5f;
    d.motor_mode[0] = DE_JOINT_MOTOR_VELOCITY;
    d.motor_target[0] = 1.0f;
    d.motor_max_force[0] = 2000.0f;
    const de_joint j = de_joint_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(j));
    f64 worst = 0.0;
    for (u32 tick = 0; tick < 60; ++tick) {
        de_physics_step(p, ST_DT);
        const f64 x = st_pos(p, b).x;
        worst = x > worst ? x : worst;
    }
    const de_dvec3 at = st_pos(p, b);
    fprintf(stderr, "  slider: at %.5f (max %.5f), z %.6f\n", at.x, worst, at.z - 2.0);
    EXPECT_TRUE(worst < 0.51);
    EXPECT_TRUE(at.x > 0.49);
    EXPECT_TRUE(at.z - 2.0 < 2e-3 && 2.0 - at.z < 2e-3); /* gravity is across the axis */
    de_physics_destroy(p);

    /* Position motor to -0.2 m. */
    p = st_world(8, NULL, NULL);
    const de_shape s2 = st_box(p, 0.2f, 0.2f, 0.2f);
    const de_body b2 = jt_dyn(p, s2, st_xform(0, 0, 2, st_quat_identity()), 0.0f);
    d.body_b = b2;
    d.motor_mode[0] = DE_JOINT_MOTOR_POSITION;
    d.motor_target[0] = -0.2f;
    ASSERT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    for (u32 tick = 0; tick < 60; ++tick)
        de_physics_step(p, ST_DT);
    fprintf(stderr, "  slider position motor: %.5f\n", st_pos(p, b2).x);
    EXPECT_TRUE(st_pos(p, b2).x > -0.21 && st_pos(p, b2).x < -0.19);
    de_physics_destroy(p);
}

TEST(physics_joint_limits, distance_rope_and_spring) {
    /* A rope (0 .. 2 m): the ball falls from 1 m below the pivot and is caught at 2 m. */
    de_physics_world *p = st_world(8, NULL, NULL);
    const de_shape ball = st_sphere(p, 0.1f);
    const de_body b = jt_dyn(p, ball, st_xform(0, 0, 4, st_quat_identity()), 0.0f);
    de_joint_desc d =
        jt_desc(DE_JOINT_DISTANCE, (de_body){0}, b, (de_dvec3){0, 0, 5}, (de_dvec3){0, 0, 0});
    d.limits_min[0] = 0.0f;
    d.limits_max[0] = 2.0f;
    ASSERT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    f64 longest = 0.0;
    for (u32 tick = 0; tick < 90; ++tick) {
        de_physics_step(p, ST_DT);
        const f64 l = 5.0 - st_pos(p, b).z;
        longest = l > longest ? l : longest;
    }
    fprintf(stderr, "  rope: longest %.5f m, at %.5f m\n", longest, 5.0 - st_pos(p, b).z);
    EXPECT_TRUE(longest < 2.01);
    EXPECT_TRUE(5.0 - st_pos(p, b).z > 1.99);
    de_physics_destroy(p);

    /* A 2 Hz spring of rest length 1 m: it settles g / (2 pi 2)^2 = 6.2 cm longer. */
    p = st_world(8, NULL, NULL);
    const de_shape ball2 = st_sphere(p, 0.1f);
    const de_body b2 = jt_dyn(p, ball2, st_xform(0, 0, 4, st_quat_identity()), 0.0f);
    d.body_b = b2;
    d.limits_min[0] = 1.0f;
    d.limits_max[0] = 1.0f;
    d.hertz = 2.0f;
    d.damping_ratio = 1.0f;
    ASSERT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    for (u32 tick = 0; tick < 120; ++tick)
        de_physics_step(p, ST_DT);
    const f64 stretch = 5.0 - st_pos(p, b2).z - 1.0;
    const f64 want = 9.81 / ((2.0 * 3.14159265 * 2.0) * (2.0 * 3.14159265 * 2.0));
    fprintf(stderr, "  spring: stretch %.5f m (expected %.5f m)\n", stretch, want);
    EXPECT_TRUE(stretch > 0.95 * want && stretch < 1.05 * want);
    de_physics_destroy(p);
}

/* ================================ Cone-twist ============================================= */

/*
 * A capsule hangs from a world pivot by a cone-twist joint whose X axis points down. Without
 * gravity it is spun about the swing direction phi (angle in A's YZ plane) and must stop on the
 * ellipse. Returns the largest swing reached.
 */
static f32 cone_swing(f32 phi, f32 ay, f32 az, f32 *twist_max) {
    de_physics_world *p = zero_g_world(8);
    de_shape_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.kind = DE_SHAPE_CAPSULE;
    sd.radius = 0.1f;
    sd.half_height = 0.4f;
    const de_shape cap = de_shape_create(p, &sd);
    const de_body b = jt_dyn(p, cap, st_xform(0, 0, 4.5, st_quat_identity()), 0.0f);
    const de_quat down = st_quat_axis_angle((de_vec3){0, 1, 0}, 0.5f * JT_PI); /* X -> -Z */
    de_joint_desc d =
        jt_desc(DE_JOINT_CONE_TWIST, (de_body){0}, b, (de_dvec3){0, 0, 5}, (de_dvec3){0, 0, 0.5});
    d.frame_a.rotation = down;
    d.frame_b.rotation = down;
    d.limits_min[3] = -0.2f;
    d.limits_max[3] = 0.2f;
    d.limits_max[4] = ay;
    d.limits_max[5] = az;
    EXPECT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    f32 s, c;
    de_dsincos(phi, &s, &c);
    /* Swing axis (0, c, s) in A's frame, plus a twist spin about the hanging axis. */
    const de_vec3 axis = de__quat_rotate(down, (de_vec3){0, c, s});
    const de_vec3 twist_axis = de__quat_rotate(down, (de_vec3){1, 0, 0});
    const de_vec3 w = de__v3_add(de__v3_scale(axis, 2.0f), de__v3_scale(twist_axis, 2.0f));
    const de_vec3 v = de__v3_cross(w, (de_vec3){0, 0, -0.5f});
    de_body_set_velocity(p, b, v, w);
    f32 worst = 0.0f, tmax = 0.0f;
    for (u32 tick = 0; tick < 60; ++tick) {
        de_physics_step(p, ST_DT);
        f32 tw, sy, sz;
        jt_angles(st_quat_identity(), down, st_rot(p, b), down, &tw, &sy, &sz);
        const f32 sw = de_dsqrt(sy * sy + sz * sz);
        worst = sw > worst ? sw : worst;
        const f32 at = tw < 0.0f ? -tw : tw;
        tmax = at > tmax ? at : tmax;
    }
    de_physics_destroy(p);
    *twist_max = tmax;
    return worst;
}

TEST(physics_joint_limits, cone_twist_ellipse) {
    const f32 ay = 0.3f, az = 0.8f;
    for (u32 k = 0; k < 8; ++k) {
        const f32 phi = (f32)k * JT_PI / 8.0f;
        f32 s, c;
        de_dsincos(phi, &s, &c);
        const f32 ey = c / ay, ez = s / az;
        const f32 limit = 1.0f / de_dsqrt(ey * ey + ez * ez);
        f32 tmax;
        const f32 got = cone_swing(phi, ay, az, &tmax);
        fprintf(stderr, "  phi %5.1f deg: swing %.4f, ellipse %.4f, twist %.4f\n", (f64)(phi / DEG),
                (f64)got, (f64)limit, (f64)tmax);
        EXPECT_TRUE(got < limit + 2.0f * DEG);
        EXPECT_TRUE(got > limit - 3.0f * DEG); /* it does reach the cone */
        EXPECT_TRUE(tmax < 0.2f + 2.0f * DEG);
    }
}

/* ================================ 6-DOF limits =========================================== */

TEST(physics_joint_limits, six_dof_axes) {
    de_physics_world *p = zero_g_world(8);
    const de_shape s = st_box(p, 0.2f, 0.2f, 0.2f);
    const de_body b = jt_dyn(p, s, st_xform(0, 0, 2, st_quat_identity()), 0.0f);
    de_joint_desc d =
        jt_desc(DE_JOINT_6DOF, (de_body){0}, b, (de_dvec3){0, 0, 2}, (de_dvec3){0, 0, 0});
    d.axis_mode[0] = DE_JOINT_AXIS_LIMITED; /* X in [-0.3, 0.3] */
    d.limits_min[0] = -0.3f;
    d.limits_max[0] = 0.3f;
    d.axis_mode[1] = DE_JOINT_AXIS_FREE;    /* Y free; Z locked */
    d.axis_mode[3] = DE_JOINT_AXIS_LIMITED; /* twist in [-0.5, 0.5]; swings locked */
    d.limits_min[3] = -0.5f;
    d.limits_max[3] = 0.5f;
    ASSERT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    de_body_set_velocity(p, b, (de_vec3){2, 1, 1}, (de_vec3){3, 1, 1});
    f64 xmax = 0.0, zmax = 0.0;
    f32 tmax = 0.0f, smax = 0.0f;
    for (u32 tick = 0; tick < 60; ++tick) {
        de_physics_step(p, ST_DT);
        const de_dvec3 at = st_pos(p, b);
        xmax = at.x > xmax ? at.x : xmax;
        const f64 dz = at.z - 2.0 < 0 ? 2.0 - at.z : at.z - 2.0;
        zmax = dz > zmax ? dz : zmax;
        f32 tw, sy, sz;
        jt_angles(st_quat_identity(), st_quat_identity(), st_rot(p, b), st_quat_identity(), &tw,
                  &sy, &sz);
        tmax = tw > tmax ? tw : tmax;
        const f32 sw = de_dsqrt(sy * sy + sz * sz);
        smax = sw > smax ? sw : smax;
    }
    const f64 y = st_pos(p, b).y;
    fprintf(stderr, "  6dof: x max %.4f, z err %.5f, y %.3f, twist max %.4f, swing %.5f\n", xmax,
            zmax, y, (f64)tmax, (f64)smax);
    EXPECT_TRUE(xmax < 0.31 && xmax > 0.29);
    EXPECT_TRUE(zmax < 2e-3);
    EXPECT_TRUE(y > 0.9); /* free: kept moving at 1 m/s */
    EXPECT_TRUE(tmax < 0.5f + 2.0f * DEG && tmax > 0.45f);
    EXPECT_TRUE(smax < 5e-3f);
    de_physics_destroy(p);
}

/* ================================ Break force ============================================= */

/* A 64 kg box hangs from the world by a fixed joint with break_force = factor * m g. */
static void break_run(f32 factor, bool *broke, u32 *events, f32 *force) {
    de_physics_world *p = st_world(8, NULL, NULL);
    const de_shape s = st_box(p, 0.2f, 0.2f, 0.2f);
    const de_body b = jt_dyn(p, s, st_xform(0, 0, 2, st_quat_identity()), 0.0f);
    de_mass_props mp;
    EXPECT_TRUE(de_body_get_mass_props(p, b, &mp));
    de_joint_desc d =
        jt_desc(DE_JOINT_FIXED, (de_body){0}, b, (de_dvec3){0, 0, 2.2}, (de_dvec3){0, 0, 0.2});
    d.break_force = factor * mp.mass * 9.81f;
    d.b.bits = 7u; /* reported back in the event (body_b is set, so it needs no resolving) */
    const de_joint j = de_joint_create(p, &d);
    EXPECT_TRUE(DE_HANDLE_VALID(j));
    *events = 0;
    *force = 0.0f;
    for (u32 tick = 0; tick < 60; ++tick) {
        de_physics_step(p, ST_DT);
        de_joint_break_event e[2];
        const u32 n = de_physics_joint_breaks(p, e, 2);
        if (n > 0) {
            EXPECT_EQ(n, 1u);
            EXPECT_EQ(e[0].joint.id, j.id);
            EXPECT_EQ(e[0].body_b.id, b.id);
            EXPECT_FALSE(DE_HANDLE_VALID(e[0].body_a));
            EXPECT_EQ(e[0].b.bits, 7u);
            *force = e[0].force / (mp.mass * 9.81f);
        }
        *events += n;
    }
    *broke = de_joint_is_broken(p, j);
    if (*broke) /* it fell */
        EXPECT_TRUE(st_pos(p, b).z < 0.5);
    EXPECT_TRUE(de_joint_valid(p, j));
    de_physics_destroy(p);
}

TEST(physics_joint_limits, break_force_threshold) {
    bool broke;
    u32 events;
    f32 force;
    break_run(1.2f, &broke, &events, &force);
    EXPECT_FALSE(broke);
    EXPECT_EQ(events, 0u);
    break_run(0.8f, &broke, &events, &force);
    fprintf(stderr, "  broke at %.4f m g\n", (f64)force);
    EXPECT_TRUE(broke);
    EXPECT_EQ(events, 1u); /* reported in the step it broke in only */
    EXPECT_TRUE(force > 0.8f);
}

/* ================================ Worker-count invariance =============================== */

static de_task_system *tasks_new(u32 workers) {
    if (workers == 0)
        return NULL;
    de_fp_env_init_thread();
    de_task_system_desc d;
    memset(&d, 0, sizeof d);
    d.worker_count = workers;
    d.background_worker_count = 1;
    de_task_system *ts = NULL;
    if (de_task_system_create(&d, &ts) != DE_OK)
        return NULL;
    return ts;
}

/* Every kind at once, with limits and motors, 8 chains of 6 links (so colors hold several
 * records), on a ground; 60 steps. Returns the physics checksum xor the joints hash. */
static u64 mixed_run(u32 workers, u32 grain) {
    de_task_system *ts = tasks_new(workers);
    de_physics_world *p = st_world(128, ts, NULL);
    if (grain > 0)
        st_solver(p)->coloring->grain = grain;
    const de_shape ground = st_box(p, 20.0f, 20.0f, 0.5f);
    st_body(p, DE_BODY_STATIC, ground, st_xform(0, 0, -0.5, st_quat_identity()), 0.6f, 0.0f);
    const de_shape link = st_box(p, 0.2f, 0.05f, 0.05f);
    for (u32 c = 0; c < 8; ++c) {
        de_body prev = {0};
        for (u32 i = 0; i < 6; ++i) {
            const de_body b =
                jt_dyn(p, link, st_xform(0.2 + 0.4 * i, 1.0 * c, 4, st_quat_identity()), 0.0f);
            de_joint_desc d = jt_desc((de_joint_kind)((c + i) % 7u), prev, b,
                                      i == 0 ? (de_dvec3){0, 1.0 * c, 4} : (de_dvec3){0.2, 0, 0},
                                      (de_dvec3){-0.2, 0, 0});
            d.enable_limit = true;
            d.limits_min[0] = -0.4f;
            d.limits_max[0] = 0.4f;
            if (d.kind == DE_JOINT_DISTANCE) {
                d.limits_min[0] = 0.0f;
                d.limits_max[0] = 0.05f;
            }
            d.limits_min[3] = -0.3f;
            d.limits_max[3] = 0.3f;
            d.limits_max[4] = 0.5f;
            d.limits_max[5] = 0.7f;
            d.axis_mode[3] = DE_JOINT_AXIS_LIMITED;
            d.axis_mode[4] = DE_JOINT_AXIS_FREE;
            d.motor_mode[0] = DE_JOINT_MOTOR_VELOCITY;
            d.motor_target[0] = 0.5f;
            d.motor_max_force[0] = 20.0f;
            d.break_force = c == 3 ? 400.0f : 0.0f;
            EXPECT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
            prev = b;
        }
    }
    for (u32 tick = 0; tick < 60; ++tick)
        de_physics_step(p, ST_DT);
    const u64 h = de_physics_checksum(p) ^ de__joints_hash(p);
    de_physics_destroy(p);
    if (ts != NULL)
        de_task_system_destroy(ts);
    return h;
}

TEST(physics_joint_limits, worker_count_invariance) {
    const u64 ref = mixed_run(0, 0);
    EXPECT_EQ(mixed_run(1, 0), ref);
    EXPECT_EQ(mixed_run(4, 0), ref);
    EXPECT_EQ(mixed_run(4, 1), ref);
    EXPECT_EQ(mixed_run(0, 3), ref);
}
