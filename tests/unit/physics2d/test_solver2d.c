/*
 * test_solver2d.c: the 2D soft-step solver (DENG-254, SPEC §13.3, §13.1): free fall against the
 * substep and analytic solutions, gravity scale, damping, speed clamps, an offset center of mass,
 * 100 km = origin; bounce heights against e^2 (max mixing, threshold, e = 0); incline friction
 * angles (+-2 degrees) and sqrt mixing; fixed rotation; kinematic bodies (exact motion, infinite
 * mass, carrying a box); a stack of 10; bullets (a 300 m/s circle against a 1 cm wall, a 1 cm
 * segment and a chain, a spinning box; a non-bullet tunnels); the constraint interface (order,
 * stages, write flags, refusals, class iterations).
 */
#include "solver2d_test_util.h"

#define PI_F 3.14159265359f

static f64 sv_y(const de_physics2d_world *p, de_body b) {
    return sv_xf_of(p, b).y;
}

/* ---- Integration -------------------------------------------------------------------------- */

TEST(physics2d_solver, free_fall_substep_and_analytic) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    const de_shape s = tu_circle(p, 0.25f, (de_vec2){0, 0});
    const de_body b = sv_body(p, DE_BODY_DYNAMIC, s, tu_xf(3.0, 100.0, 0.0f), 0.5f, 0.0f, 0);
    const u32 steps = 60;
    for (u32 i = 0; i < steps; ++i)
        de_physics2d_step(p, SV_DT);
    /* Substep solution: v_k = k g h, y_n = y0 - g h^2 n (n + 1) / 2 with n = 4 * steps. */
    const f64 h = (f64)SV_DT / 4.0, g = 9.81, n = 4.0 * steps;
    const f64 expect = 100.0 - g * h * h * n * (n + 1.0) / 2.0;
    EXPECT_FLOAT_EQ(sv_y(p, b), expect, 1e-4);
    /* The analytic fall differs by g h t / 2. */
    const f64 t = (f64)steps * (f64)SV_DT;
    EXPECT_FLOAT_EQ(sv_y(p, b), 100.0 - 0.5 * g * t * t - 0.5 * g * h * t, 1e-4);
    EXPECT_FLOAT_EQ(sv_vel(p, b).y, -g * t, 1e-3);
    EXPECT_TRUE(sv_xf_of(p, b).x == 3.0); /* no sideways drift, bit for bit */
    de_physics2d_destroy(p);
}

TEST(physics2d_solver, gravity_scale_and_damping) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    const de_shape s = tu_circle(p, 0.25f, (de_vec2){0, 0});
    de_body2d_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = s;
    d.transform = tu_xf(0, 50, 0);
    const de_body zero = de_body2d_create(p, &d); /* gravity scale 0: floats */
    d.transform = tu_xf(5, 50, 0);
    d.gravity_scale = 0.5f;
    const de_body half = de_body2d_create(p, &d);
    d.transform = tu_xf(10, 50, 0);
    d.gravity_scale = 0.0f;
    d.linear_velocity = (de_vec2){4.0f, 0.0f};
    d.angular_velocity = 3.0f;
    d.linear_damping = 2.0f;
    d.angular_damping = 1.0f;
    const de_body damped = de_body2d_create(p, &d);
    de_physics2d_step(p, SV_DT);
    EXPECT_TRUE(sv_y(p, zero) == 50.0);
    EXPECT_FLOAT_EQ(sv_vel(p, half).y, -0.5 * 9.81 * (f64)SV_DT, 1e-5);
    const f64 h = (f64)SV_DT / 4.0;
    EXPECT_FLOAT_EQ(sv_vel(p, damped).x, 4.0 / pow(1.0 + h * 2.0, 4.0), 1e-5);
    EXPECT_FLOAT_EQ(sv_angvel(p, damped), 3.0 / pow(1.0 + h * 1.0, 4.0), 1e-5);
    de_physics2d_destroy(p);
}

TEST(physics2d_solver, speed_clamps) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    de_physics2d_set_gravity(p, (de_vec2){0, 0});
    const de_shape s = tu_circle(p, 0.25f, (de_vec2){0, 0});
    const de_body b = sv_body(p, DE_BODY_DYNAMIC, s, tu_xf(0, 0, 0), 0.5f, 0.0f, 0);
    ASSERT_TRUE(de_body2d_set_velocity(p, b, (de_vec2){3000.0f, 4000.0f}, 1000.0f));
    de_physics2d_step(p, SV_DT);
    const de_vec2 v = sv_vel(p, b);
    EXPECT_FLOAT_EQ(sqrt((f64)v.x * v.x + (f64)v.y * v.y), 400.0, 1e-3);
    EXPECT_FLOAT_EQ(v.x / v.y, 0.75, 1e-5);
    /* A quarter turn per substep at most. */
    EXPECT_FLOAT_EQ(sv_angvel(p, b), 0.25 * 3.14159265359 * 240.0, 1e-2);
    de_physics2d_destroy(p);
}

TEST(physics2d_solver, offset_center_of_mass_spins_in_place) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    de_physics2d_set_gravity(p, (de_vec2){0, 0});
    const de_shape s = tu_circle(p, 0.5f, (de_vec2){2.0f, 1.0f}); /* center of mass at (2, 1) */
    const de_body b = sv_body(p, DE_BODY_DYNAMIC, s, tu_xf(10, 20, 0), 0.5f, 0.0f, 0);
    ASSERT_TRUE(de_body2d_set_velocity(p, b, (de_vec2){0, 0}, 2.0f));
    for (u32 i = 0; i < 120; ++i)
        de_physics2d_step(p, SV_DT);
    /* The center of mass stays at (12, 21); the origin turns about it by 4 rad. */
    const de_transform2d t = sv_xf_of(p, b);
    const de_vec2 r = de__rot2d_apply(t.rotation, (de_vec2){2.0f, 1.0f});
    EXPECT_FLOAT_EQ(t.x + r.x, 12.0, 1e-5);
    EXPECT_FLOAT_EQ(t.y + r.y, 21.0, 1e-5);
    EXPECT_FLOAT_EQ(atan2((f64)t.rotation.s, (f64)t.rotation.c), 4.0 - 2.0 * 3.14159265358979,
                    1e-4);
    de_physics2d_destroy(p);
}

/* A small scene (ball, box, rolling ball on a ramp) at `x0`, `y0`; positions after 240 ticks
 * relative to the scene origin. */
static void far_scene(f64 x0, f64 y0, f64 out[6]) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    const de_shape g = tu_box(p, 10.0f, 0.5f, 0.0f);
    (void)sv_body(p, DE_BODY_STATIC, g, tu_xf(x0, y0 - 0.5, 0.0f), 0.6f, 0.0f, 0);
    const de_shape ramp = tu_box(p, 3.0f, 0.1f, 0.0f);
    (void)sv_body(p, DE_BODY_STATIC, ramp, tu_xf(x0 + 5.0, y0 + 2.0, -0.4f), 0.6f, 0.0f, 0);
    const de_shape ball = tu_circle(p, 0.3f, (de_vec2){0, 0});
    const de_shape box = tu_box(p, 0.4f, 0.3f, 0.0f);
    const de_body b0 = sv_body(p, DE_BODY_DYNAMIC, ball, tu_xf(x0, y0 + 2.0, 0.0f), 0.5f, 0.5f, 0);
    const de_body b1 =
        sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(x0 - 3.0, y0 + 1.0, 0.7f), 0.5f, 0.0f, 0);
    const de_body b2 =
        sv_body(p, DE_BODY_DYNAMIC, ball, tu_xf(x0 + 4.0, y0 + 4.0, 0.0f), 0.5f, 0.0f, 0);
    for (u32 i = 0; i < 240; ++i)
        de_physics2d_step(p, SV_DT);
    const de_body bs[3] = {b0, b1, b2};
    for (u32 k = 0; k < 3; ++k) {
        const de_transform2d t = sv_xf_of(p, bs[k]);
        out[2 * k] = t.x - x0;
        out[2 * k + 1] = t.y - y0;
    }
    de_physics2d_destroy(p);
}

TEST(physics2d_solver, far_from_origin_moves_like_origin) {
    f64 a[6], b[6];
    far_scene(0.0, 0.0, a);
    far_scene(100000.0, -100000.0, b);
    f64 worst = 0.0;
    for (u32 i = 0; i < 6; ++i)
        worst = fmax(worst, fabs(a[i] - b[i]));
    printf("  100 km vs origin: worst %.3g m\n", worst);
    SV_EXPECT_LT(worst, 1e-4);
}

TEST(physics2d_solver, rests_on_static_chain_far_from_its_body_origin) {
    /* A chain whose segments lie 20 km from its body origin (a long level built in one body): the
     * static side's anchor offset is never formed in f32 (contact_solver2d.c), so a box rests on
     * it as on a chain at the origin. */
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    static const de_vec2 pts[3] = {{20010.0f, 0.0f}, {20000.0f, 0.0f}, {19990.0f, 0.0f}};
    (void)sv_body(p, DE_BODY_STATIC, de_shape2d_chain_create(p, pts, 3, false), tu_xf(0, 0, 0.0f),
                  0.6f, 0.0f, 0);
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    const de_body b = sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(20000.3, 0.501, 0.0f), 0.6f, 0.0f, 0);
    for (u32 i = 0; i < 180; ++i)
        de_physics2d_step(p, SV_DT);
    const de_transform2d t = sv_xf_of(p, b);
    SV_EXPECT_LT(fabs(t.x - 20000.3), 1e-3);
    SV_EXPECT_LT(fabs(t.y - 0.5), 0.01);
    SV_EXPECT_LT(fabs((f64)sv_vel(p, b).y), 1e-3);
    de_physics2d_destroy(p);
}

/* ---- Restitution -------------------------------------------------------------------------- */

/* Drops a ball (r = 0.5, bottom `drop` m above a ground top at y = 0) and returns the highest
 * bottom height after the first bounce (0 when it never rose again). */
static f64 bounce_height(f32 e_ball, f32 e_ground, f32 drop) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    const de_shape g = tu_box(p, 10.0f, 0.5f, 0.0f);
    const de_shape ball = tu_circle(p, 0.5f, (de_vec2){0, 0});
    (void)sv_body(p, DE_BODY_STATIC, g, tu_xf(0, -0.5, 0.0f), 0.5f, e_ground, 0);
    const de_body b =
        sv_body(p, DE_BODY_DYNAMIC, ball, tu_xf(0, 0.5 + (f64)drop, 0.0f), 0.5f, e_ball, 0);
    bool bounced = false;
    f64 apex = 0.0;
    for (u32 t = 0; t < 300; ++t) {
        de_physics2d_step(p, SV_DT);
        const f32 vy = sv_vel(p, b).y;
        const f64 bottom = sv_y(p, b) - 0.5;
        if (!bounced && vy > 0.0f)
            bounced = true;
        if (bounced) {
            if (bottom > apex)
                apex = bottom;
            if (vy < 0.0f)
                break;
        }
    }
    de_physics2d_destroy(p);
    return apex;
}

TEST(physics2d_solver, bounce_height_within_3_percent_of_e2) {
    const f32 es[3] = {0.8f, 0.5f, 0.95f};
    for (u32 i = 0; i < 3; ++i) {
        const f64 hgt = bounce_height(es[i], 0.0f, 1.0f);
        const f64 want = (f64)es[i] * (f64)es[i];
        printf("  e = %.2f: bounce %.4f m (e^2 = %.4f, %.2f%%)\n", (f64)es[i], hgt, want,
               100.0 * (hgt - want) / want);
        SV_EXPECT_LT(fabs(hgt - want), 0.03 * want);
    }
    /* Max mixing: the ground's e wins when larger. */
    const f64 mixed = bounce_height(0.0f, 0.8f, 1.0f);
    SV_EXPECT_LT(fabs(mixed - 0.64), 0.03 * 0.64);
}

TEST(physics2d_solver, no_bounce_without_restitution_or_below_threshold) {
    SV_EXPECT_LT(bounce_height(0.0f, 0.0f, 1.0f), 0.002);
    /* 2 cm drop: 0.63 m/s at impact, below the 1 m/s threshold. */
    SV_EXPECT_LT(bounce_height(0.9f, 0.0f, 0.02f), 0.002);
}

TEST(physics2d_solver, bounce_ratio_over_drops) {
    /* Twelve drop phases: each bounce apex over the previous stays within 3% of e^2 while the
     * impact speed is well above the threshold. */
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    const de_shape g = tu_box(p, 10.0f, 0.5f, 0.0f);
    const de_shape ball = tu_circle(p, 0.5f, (de_vec2){0, 0});
    (void)sv_body(p, DE_BODY_STATIC, g, tu_xf(0, -0.5, 0.0f), 0.5f, 0.0f, 0);
    const de_body b = sv_body(p, DE_BODY_DYNAMIC, ball, tu_xf(0, 4.5, 0.0f), 0.5f, 0.9f, 0);
    f64 prev = 4.0, apex = 0.0;
    u32 phases = 0;
    f32 last_vy = 0.0f;
    for (u32 t = 0; t < 2400 && phases < 12; ++t) {
        de_physics2d_step(p, SV_DT);
        const f32 vy = sv_vel(p, b).y;
        const f64 bottom = sv_y(p, b) - 0.5;
        if (vy > 0.0f)
            apex = fmax(apex, bottom);
        if (last_vy > 0.0f && vy <= 0.0f) {
            if (prev > 0.3) {
                const f64 ratio = apex / prev;
                SV_EXPECT_LT(fabs(ratio - 0.81), 0.03 * 0.81);
            }
            prev = apex;
            apex = 0.0;
            ++phases;
        }
        last_vy = vy;
    }
    EXPECT_GE(phases, 8u);
    de_physics2d_destroy(p);
}

/* ---- Friction ----------------------------------------------------------------------------- */

/* A box on a static incline of `deg` degrees (friction mu on both): how far it moved along the
 * slope in 2 s after a 0.5 s settle. */
static f64 incline_slide(f32 deg, f32 mu_box, f32 mu_ramp) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    const f32 a = deg * PI_F / 180.0f;
    const de_shape ramp = tu_box(p, 20.0f, 0.5f, 0.0f);
    (void)sv_body(p, DE_BODY_STATIC, ramp, tu_xf(0, 0, a), mu_ramp, 0.0f, 0);
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    f32 sn, cs;
    de_dsincos(a, &sn, &cs);
    /* On top of the ramp's surface (normal (-sin, cos)), 1 mm above it. */
    const f64 off = 0.5 + 0.5 + 0.001;
    const de_body b =
        sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(-(f64)sn * off, (f64)cs * off, a), mu_box, 0.0f, 0);
    for (u32 i = 0; i < 30; ++i)
        de_physics2d_step(p, SV_DT);
    const de_transform2d t0 = sv_xf_of(p, b);
    for (u32 i = 0; i < 120; ++i)
        de_physics2d_step(p, SV_DT);
    const de_transform2d t1 = sv_xf_of(p, b);
    de_physics2d_destroy(p);
    const f64 dx = t1.x - t0.x, dy = t1.y - t0.y;
    return sqrt(dx * dx + dy * dy);
}

TEST(physics2d_solver, incline_friction_angle_within_2_degrees) {
    const f32 mus[2] = {0.5f, 1.0f};
    for (u32 i = 0; i < 2; ++i) {
        const f32 deg = (f32)(atan((f64)mus[i]) * 180.0 / 3.14159265358979);
        const f64 hold = incline_slide(deg - 2.0f, mus[i], mus[i]);
        const f64 slide = incline_slide(deg + 2.0f, mus[i], mus[i]);
        printf("  mu %.1f (%.2f deg): -2 deg creeps %.4f m, +2 deg slides %.3f m in 2 s\n",
               (f64)mus[i], (f64)deg, hold, slide);
        SV_EXPECT_LT(hold, 0.01);
        SV_EXPECT_GT(slide, 0.2);
    }
}

TEST(physics2d_solver, friction_mixing_is_sqrt) {
    /* mu = sqrt(0.25 * 1.0) = 0.5: the friction angle of 0.5 (26.57 degrees). */
    SV_EXPECT_LT(incline_slide(24.5f, 0.25f, 1.0f), 0.01);
    SV_EXPECT_GT(incline_slide(28.6f, 0.25f, 1.0f), 0.2);
    /* Frictionless: slides at 5 degrees. */
    SV_EXPECT_GT(incline_slide(5.0f, 0.0f, 1.0f), 0.5);
}

/* ---- Fixed rotation, kinematic ------------------------------------------------------------ */

TEST(physics2d_solver, fixed_rotation_never_turns) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    const de_shape g = tu_box(p, 10.0f, 0.5f, 0.0f);
    (void)sv_body(p, DE_BODY_STATIC, g, tu_xf(0, -0.5, 0.0f), 0.6f, 0.0f, 0);
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    /* Dropped on a corner, with a spin: a free box tips, the fixed one does not. */
    const de_body fixed = sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(-3, 2, 0.5f), 0.6f, 0.0f,
                                  DE_BODY2D_FLAG_FIXED_ROTATION);
    const de_body free_box = sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(3, 2, 0.5f), 0.6f, 0.0f, 0);
    ASSERT_TRUE(de_body2d_set_velocity(p, fixed, (de_vec2){0, 0}, 5.0f));
    for (u32 i = 0; i < 120; ++i)
        de_physics2d_step(p, SV_DT);
    const de_transform2d tf = sv_xf_of(p, fixed), tb = sv_xf_of(p, free_box);
    EXPECT_TRUE(tf.rotation.c == de_rot2d_from_angle(0.5f).c);
    EXPECT_TRUE(tf.rotation.s == de_rot2d_from_angle(0.5f).s);
    EXPECT_TRUE(sv_angvel(p, fixed) == 0.0f);
    /* The fixed box rests on its corner at its spawn angle; the free one fell flat. */
    SV_EXPECT_LT(fabs(tb.rotation.s), 1e-3);
    SV_EXPECT_GT(tf.y, 0.6);
    de_physics2d_destroy(p);
}

TEST(physics2d_solver, kinematic_moves_by_velocity_and_carries) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    const de_shape pl = tu_box(p, 2.0f, 0.1f, 0.0f);
    const de_body k = sv_body(p, DE_BODY_KINEMATIC, pl, tu_xf(0, 0, 0.0f), 0.8f, 0.0f, 0);
    const de_shape box = tu_box(p, 0.3f, 0.3f, 0.0f);
    const de_body b = sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(0, 0.401, 0.0f), 0.8f, 0.0f, 0);
    ASSERT_TRUE(de_body2d_set_velocity(p, k, (de_vec2){1.0f, 0.5f}, 0.0f));
    const de_shape spin = tu_box(p, 0.5f, 0.1f, 0.0f);
    const de_body ks = sv_body(p, DE_BODY_KINEMATIC, spin, tu_xf(20, 0, 0.0f), 0.8f, 0.0f, 0);
    ASSERT_TRUE(de_body2d_set_velocity(p, ks, (de_vec2){0, 0}, 1.0f));
    for (u32 i = 0; i < 120; ++i)
        de_physics2d_step(p, SV_DT);
    /* Exact motion (a sum of f32 substep displacements) and infinite mass. */
    const de_transform2d tk = sv_xf_of(p, k);
    EXPECT_FLOAT_EQ(tk.x, 2.0, 1e-4);
    EXPECT_FLOAT_EQ(tk.y, 1.0, 1e-4);
    EXPECT_TRUE(sv_vel(p, k).x == 1.0f && sv_vel(p, k).y == 0.5f);
    EXPECT_FLOAT_EQ(atan2((f64)sv_xf_of(p, ks).rotation.s, (f64)sv_xf_of(p, ks).rotation.c), 2.0,
                    1e-4);
    /* The box rides along. */
    const de_transform2d tb = sv_xf_of(p, b);
    EXPECT_FLOAT_EQ(tb.x, 2.0, 0.02);
    EXPECT_FLOAT_EQ(tb.y, 1.4, 0.03);
    de_mass_props2d mp;
    ASSERT_TRUE(de_body2d_get_mass_props(p, k, &mp));
    EXPECT_TRUE(mp.mass == 0.0f);
    de_physics2d_destroy(p);
}

/* ---- Stacks -------------------------------------------------------------------------------- */

TEST(physics2d_solver, stack_of_10_stands) {
    de_physics2d_world *p = sv_world(32, NULL, NULL);
    const de_shape g = tu_box(p, 10.0f, 0.5f, 0.0f);
    (void)sv_body(p, DE_BODY_STATIC, g, tu_xf(0, -0.5, 0.0f), 0.6f, 0.0f, 0);
    const de_shape box = tu_box(p, 0.5f, 0.5f, 0.0f);
    de_body bs[10];
    for (u32 i = 0; i < 10; ++i)
        bs[i] = sv_body(p, DE_BODY_DYNAMIC, box, tu_xf(0, 0.5 + 1.001 * i, 0.0f), 0.6f, 0.0f, 0);
    for (u32 i = 0; i < 600; ++i)
        de_physics2d_step(p, SV_DT);
    const de_transform2d top = sv_xf_of(p, bs[9]);
    f64 motion = 0.0;
    for (u32 i = 0; i < 10; ++i)
        motion += fabs((f64)sv_vel(p, bs[i]).x) + fabs((f64)sv_vel(p, bs[i]).y) +
                  fabs((f64)sv_angvel(p, bs[i]));
    printf("  stack of 10 after 10 s: top at (%.4f, %.4f), residual motion %.3g\n", top.x, top.y,
           motion);
    /* Soft contacts: the settling starts a slow, damped rocking of about 2 cm at the top (as in
     * 3D, see physics: Dynamics); it is at rest after 10 s. */
    SV_EXPECT_LT(fabs(top.x), 0.025);
    SV_EXPECT_GT(top.y, 9.4);
    SV_EXPECT_LT(motion, 0.01);
    de_physics2d_destroy(p);
}

TEST(physics2d_solver, mass_ratio_1_to_100) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    const de_shape g = tu_box(p, 10.0f, 0.5f, 0.0f);
    (void)sv_body(p, DE_BODY_STATIC, g, tu_xf(0, -0.5, 0.0f), 0.6f, 0.0f, 0);
    const de_shape small = tu_box(p, 0.5f, 0.5f, 0.0f);
    de_body2d_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = small;
    d.gravity_scale = 1.0f;
    d.material.friction = 0.6f;
    d.transform = tu_xf(0, 0.5, 0.0f);
    const de_body lo = de_body2d_create(p, &d);
    d.transform = tu_xf(0, 1.501, 0.0f);
    d.mass = 100.0f;
    const de_body hi = de_body2d_create(p, &d);
    for (u32 i = 0; i < 300; ++i)
        de_physics2d_step(p, SV_DT);
    const de_transform2d t = sv_xf_of(p, hi);
    printf("  100 kg on 1 kg: drift %.4f m, top at %.4f\n", t.x, t.y);
    SV_EXPECT_LT(fabs(t.x), 0.01);
    SV_EXPECT_GT(t.y, 1.4);
    SV_EXPECT_LT(fabs((f64)sv_vel(p, hi).y) + fabs((f64)sv_vel(p, lo).y), 1e-3);
    de_physics2d_destroy(p);
}

/* ---- Bullets ------------------------------------------------------------------------------ */

/* A circle (r = 5 cm) at `speed` toward a static target at x = 10; whether it stays in front of
 * the target's front face `front` after 60 ticks. */
static bool bullet_stays(de_shape (*make)(de_physics2d_world *), f64 front, f32 speed, u32 flags,
                         de__bullets2d_stats *stats) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    de_physics2d_set_gravity(p, (de_vec2){0, 0});
    (void)sv_body(p, DE_BODY_STATIC, make(p), tu_xf(10.0, 0.0, 0.0f), 0.2f, 0.0f, 0);
    const de_shape c = tu_circle(p, 0.05f, (de_vec2){0, 0});
    const de_body b = sv_body(p, DE_BODY_DYNAMIC, c, tu_xf(0.0, 0.0, 0.0f), 0.2f, 0.0f, flags);
    ASSERT_TRUE(de_body2d_set_velocity(p, b, (de_vec2){speed, 0.0f}, 0.0f));
    bool ok = true;
    if (stats != NULL)
        memset(stats, 0, sizeof *stats);
    for (u32 i = 0; i < 60; ++i) {
        de_physics2d_step(p, SV_DT);
        if (stats != NULL) {
            de__bullets2d_stats st;
            de__bullets2d_stats_get(p, &st);
            stats->hits += st.hits;
            stats->bullets += st.bullets;
        }
        if (sv_xf_of(p, b).x + 0.05 > front + 1e-3)
            ok = false;
    }
    de_physics2d_destroy(p);
    return ok;
}

static de_shape thin_wall(de_physics2d_world *p) {
    return tu_box(p, 0.005f, 1.0f, 0.0f); /* 1 cm thick */
}
static de_shape short_segment(de_physics2d_world *p) {
    return tu_segment(p, (de_vec2){0, -0.005f}, (de_vec2){0, 0.005f}); /* 1 cm long */
}
static de_shape long_segment(de_physics2d_world *p) {
    return tu_segment(p, (de_vec2){0, -1.0f}, (de_vec2){0, 1.0f});
}
static de_shape wall_chain(de_physics2d_world *p) {
    /* Built top to bottom: the right side of each segment is -x, facing the bullet. */
    static const de_vec2 pts[4] = {{0, 1}, {0, 0.01f}, {0, -0.01f}, {0, -1}};
    return de_shape2d_chain_create(p, pts, 4, false);
}

TEST(physics2d_solver, bullet_does_not_tunnel) {
    de__bullets2d_stats st;
    EXPECT_TRUE(bullet_stays(thin_wall, 9.995, 300.0f, DE_BODY2D_FLAG_BULLET, &st));
    EXPECT_GT(st.hits, 0u);
    EXPECT_TRUE(bullet_stays(short_segment, 10.0, 300.0f, DE_BODY2D_FLAG_BULLET, &st));
    EXPECT_GT(st.hits, 0u);
    EXPECT_TRUE(bullet_stays(long_segment, 10.0, 300.0f, DE_BODY2D_FLAG_BULLET, NULL));
    EXPECT_TRUE(bullet_stays(wall_chain, 10.0, 300.0f, DE_BODY2D_FLAG_BULLET, NULL));
    /* Without the flag the same shot tunnels (5 m per tick against a 2 cm margin). */
    EXPECT_FALSE(bullet_stays(thin_wall, 9.995, 300.0f, 0, NULL));
}

TEST(physics2d_solver, spinning_bullet_box_and_resting_bullet) {
    de_physics2d_world *p = sv_world(16, NULL, NULL);
    const de_shape wall = thin_wall(p);
    (void)sv_body(p, DE_BODY_STATIC, wall, tu_xf(10.0, 0.0, 0.0f), 0.2f, 0.0f, 0);
    const de_shape g = tu_box(p, 30.0f, 0.5f, 0.0f);
    (void)sv_body(p, DE_BODY_STATIC, g, tu_xf(0.0, -10.5, 0.0f), 0.5f, 0.0f, 0);
    const de_shape bar = tu_box(p, 0.2f, 0.03f, 0.0f);
    const de_body b =
        sv_body(p, DE_BODY_DYNAMIC, bar, tu_xf(0.0, 0.0, 0.0f), 0.2f, 0.0f, DE_BODY2D_FLAG_BULLET);
    ASSERT_TRUE(de_body2d_set_velocity(p, b, (de_vec2){250.0f, 0.0f}, 60.0f));
    /* A bullet sliding on the ground keeps sliding (the fallback circle never hits). */
    const de_body slider = sv_body(p, DE_BODY_DYNAMIC, bar, tu_xf(-20.0, -9.97, 0.0f), 0.0f, 0.0f,
                                   DE_BODY2D_FLAG_BULLET);
    ASSERT_TRUE(de_body2d_set_velocity(p, slider, (de_vec2){3.0f, 0.0f}, 0.0f));
    for (u32 i = 0; i < 60; ++i) {
        de_physics2d_step(p, SV_DT);
        SV_EXPECT_LT(sv_xf_of(p, b).x, 9.995);
    }
    SV_EXPECT_GT(sv_xf_of(p, slider).x, -18.5);
    SV_EXPECT_LT(fabs(sv_xf_of(p, slider).y + 9.97), 0.01);
    de_physics2d_destroy(p);
}

/* ---- Constraint interface ------------------------------------------------------------------ */

TEST(physics2d_solver, constraint_interface_order_flags_refusals) {
    de_physics2d_world *p = sv_world(32, NULL, NULL);
    de_physics2d_set_gravity(p, (de_vec2){0, 0});
    static sv_joints j;
    memset(&j, 0, sizeof j);
    sv_joints_register(p, &j);
    const de_shape c = tu_circle(p, 0.2f, (de_vec2){0, 0});
    const de_body st = sv_body(p, DE_BODY_STATIC, c, tu_xf(0, 0, 0.0f), 0.5f, 0.0f, 0);
    const de_body k = sv_body(p, DE_BODY_KINEMATIC, c, tu_xf(5, 0, 0.0f), 0.5f, 0.0f, 0);
    const de_body d0 = sv_body(p, DE_BODY_DYNAMIC, c, tu_xf(10, 0, 0.0f), 0.5f, 0.0f, 0);
    const de_body d1 = sv_body(p, DE_BODY_DYNAMIC, c, tu_xf(15, 0, 0.0f), 0.5f, 0.0f, 0);
    (void)sv_joints_add(&j, tu_index(d1), tu_index(d0));             /* both writable */
    (void)sv_joints_add(&j, tu_index(st), tu_index(k));              /* refused */
    (void)sv_joints_add(&j, tu_index(k), tu_index(d0));              /* B writable */
    (void)sv_joints_add(&j, DE__CONSTRAINT2D_NO_BODY, tu_index(d1)); /* world joint */
    ASSERT_EQ(de__solver2d_set_class_iterations(p, DE__CONSTRAINT2D_CLASS_JOINT, 3), DE_OK);
    de_physics2d_step(p, SV_DT);
    const de__solver2d *s = de__solver2d_get(p);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(s->constraint_count, 3u);
    EXPECT_EQ(s->constraints[0].id, 0u);
    EXPECT_EQ(s->constraints[0].flags, (u32)(DE__CONSTRAINT2D_WRITE_A | DE__CONSTRAINT2D_WRITE_B));
    EXPECT_EQ(s->constraints[1].id, 2u);
    EXPECT_EQ(s->constraints[1].flags, (u32)DE__CONSTRAINT2D_WRITE_B);
    EXPECT_NE(s->constraints[1].solver_a, DE__SOLVER2D_STATIC_SLOT); /* kinematic: a real slot */
    EXPECT_EQ(s->constraints[2].solver_a, DE__SOLVER2D_STATIC_SLOT);
    /* 4 substeps x (3 solve + 3 relax passes). */
    EXPECT_EQ(j.solves[0], 24u);
    EXPECT_EQ(j.passes[0], 16u);
    EXPECT_EQ(j.solves[1], 0u);
    EXPECT_EQ(j.gathers, 1u);
    /* Registration rules. */
    EXPECT_EQ(de__solver2d_register_class(p, DE__CONSTRAINT2D_CLASS_JOINT, &k_sv_joints, &j, 4),
              DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de__solver2d_register_class(p, DE__CONSTRAINT2D_CLASS_COUNT, &k_sv_joints, &j, 4),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__solver2d_set_class_iterations(p, DE__CONSTRAINT2D_CLASS_JOINT_2, 2),
              DE_ERR_INVALID_ARGUMENT);
    de__solver2d_unregister_class(p, DE__CONSTRAINT2D_CLASS_JOINT);
    de_physics2d_step(p, SV_DT);
    EXPECT_EQ(de__solver2d_get(p)->constraint_count, 0u);
    de_physics2d_destroy(p);
}
