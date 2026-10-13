/* test_solver_friction_restitution.c: contact materials of the soft-step solver (DENG-217, SPEC
 * §13.1): bounce heights against e^2 (restitution max-mixed, threshold), e = 0 does not bounce,
 * a box on an incline stays below the friction angle and slides above it (+-2 degrees),
 * sqrt friction mixing, and anisotropic friction picks its direction from the body axis. */
#include "solver_test_util.h"

#define PI_F 3.14159265359f

/* ---- bounce ------------------------------------------------------------------------------ */

/* Drops a ball (r = 0.5, bottom `drop` m above a ground top at z = 0) and returns the highest
 * bottom height after the first bounce (0 when it never rose again). `spec` > 0 sets the
 * speculative margin (0.1 m: the contact exists before the ball, at 4.4 m/s or 7.4 cm per tick,
 * reaches the ground; per-body margins come with CCD, DENG-219); 0 keeps the default 2 cm. */
static f64 bounce_height(f32 e_ball, f32 e_ground, f32 drop, f32 spec) {
    de_physics_world *p = st_world(16, NULL, NULL);
    if (spec > 0.0f)
        st_margins(p, spec);
    const de_shape g = st_box(p, 10.0f, 10.0f, 0.5f);
    const de_shape ball = st_sphere(p, 0.5f);
    (void)st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.5f, e_ground);
    const de_body b = st_body(p, DE_BODY_DYNAMIC, ball,
                              st_xform(0, 0, 0.5 + (f64)drop, st_quat_identity()), 0.5f, e_ball);
    bool bounced = false;
    f64 apex = 0.0;
    for (u32 t = 0; t < 240; ++t) {
        de_physics_step(p, ST_DT);
        const f32 vz = st_vel(p, b).z;
        const f64 bottom = st_pos(p, b).z - 0.5;
        if (!bounced && vz > 0.0f)
            bounced = true;
        if (bounced) {
            if (bottom > apex)
                apex = bottom;
            if (vz < 0.0f)
                break;
        }
    }
    de_physics_destroy(p);
    return apex;
}

TEST(physics_solver_contact, restitution_bounce_height) {
    /* e = 0.8 from 1 m reaches 0.64 m +-3% (the AC), with the default and a large margin. */
    const f64 h = bounce_height(0.8f, 0.0f, 1.0f, 0.0f);
    const f64 hs = bounce_height(0.8f, 0.0f, 1.0f, 0.1f);
    fprintf(stderr, "e = 0.8 from 1 m: %.4f m (default margin), %.4f m (0.1 m margin)\n", h, hs);
    EXPECT_FLOAT_EQ(h, 0.64, 0.64 * 0.03);
    EXPECT_FLOAT_EQ(hs, 0.64, 0.64 * 0.03);
    /* Whatever substep the impact falls in: drops 1.000 .. 1.066 m (a tick and more of phase). */
    for (u32 i = 0; i < 12; ++i) {
        const f32 drop = 1.0f + 0.006f * (f32)i;
        EXPECT_FLOAT_EQ(bounce_height(0.8f, 0.0f, drop, 0.1f) / (f64)drop, 0.64, 0.64 * 0.03);
    }
    /* max(e_a, e_b): the ground's restitution counts the same. */
    EXPECT_FLOAT_EQ(bounce_height(0.0f, 0.8f, 1.0f, 0.1f), hs, 1e-9);
    /* e = 0.5: a quarter of the height. */
    EXPECT_FLOAT_EQ(bounce_height(0.5f, 0.0f, 1.0f, 0.1f), 0.25, 0.25 * 0.03);
    /* e = 0: no bounce. */
    ST_LT(bounce_height(0.0f, 0.0f, 1.0f, 0.1f), 0.005);
    ST_LT(bounce_height(0.0f, 0.0f, 1.0f, 0.0f), 0.005);
    /* Below the threshold (1 m/s): a 2 cm drop arrives at 0.63 m/s and does not bounce. */
    ST_LT(bounce_height(0.8f, 0.0f, 0.02f, 0.1f), 0.002);
}

/* ---- incline ----------------------------------------------------------------------------- */

typedef struct incline_result {
    f64 slide;   /* distance moved down the slope */
    f64 lateral; /* distance moved across the slope */
} incline_result;

/* A unit box resting on a static slab tilted by `deg` about +X (downhill = -Y), both with
 * friction `mu`, for `ticks`. `box_yaw` turns the box about the slab normal; `axis` / `mu2` make
 * the box's friction anisotropic. */
static incline_result incline(f32 deg, f32 mu, f32 ground_mu, u32 ticks, f32 box_yaw, de_vec3 axis,
                              f32 mu2) {
    de_physics_world *p = st_world(16, NULL, NULL);
    const f32 th = deg * PI_F / 180.0f;
    const de_quat tilt = st_quat_axis_angle((de_vec3){1, 0, 0}, th);
    const de_shape slab = st_box(p, 20.0f, 20.0f, 0.5f);
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    (void)st_body(p, DE_BODY_STATIC, slab, st_xform(0, 0, 0, tilt), ground_mu, 0.0f);
    /* The box sits on the slab's top face: its center is 1 m along the slab normal. */
    const de_vec3 n = de__quat_rotate(tilt, (de_vec3){0, 0, 1});
    const de_quat yaw = st_quat_axis_angle((de_vec3){0, 0, 1}, box_yaw);
    const de_quat q = de_dnormalize_quat(
        (de_quat){tilt.w * yaw.x + tilt.x * yaw.w + tilt.y * yaw.z - tilt.z * yaw.y,
                  tilt.w * yaw.y - tilt.x * yaw.z + tilt.y * yaw.w + tilt.z * yaw.x,
                  tilt.w * yaw.z + tilt.x * yaw.y - tilt.y * yaw.x + tilt.z * yaw.w,
                  tilt.w * yaw.w - tilt.x * yaw.x - tilt.y * yaw.y - tilt.z * yaw.z});
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = cube;
    d.gravity_scale = 1.0f;
    d.material.friction = mu;
    d.friction_axis = axis;
    d.friction_secondary = mu2;
    d.transform = st_xform((f64)n.x * 1.0005, (f64)n.y * 1.0005, (f64)n.z * 1.0005, q);
    const de_body b = de_body_create(p, &d);
    const de_dvec3 p0 = st_pos(p, b);
    for (u32 t = 0; t < ticks; ++t)
        de_physics_step(p, ST_DT);
    const de_dvec3 d1 = de__d3_sub(st_pos(p, b), p0);
    /* Downhill direction: -Y tilted with the slab. */
    const de_vec3 down = de__quat_rotate(tilt, (de_vec3){0, -1, 0});
    const de_vec3 across = de__quat_rotate(tilt, (de_vec3){1, 0, 0});
    incline_result r;
    r.slide = d1.x * down.x + d1.y * down.y + d1.z * down.z;
    r.lateral = d1.x * across.x + d1.y * across.y + d1.z * across.z;
    de_physics_destroy(p);
    return r;
}

TEST(physics_solver_contact, incline_friction_angle) {
    /* mu = 0.5: friction angle atan(0.5) = 26.57 degrees. */
    const de_vec3 none = {0, 0, 0};
    const incline_result stay = incline(24.57f, 0.5f, 0.5f, 120, 0.0f, none, 0.0f);
    const incline_result slide = incline(28.57f, 0.5f, 0.5f, 120, 0.0f, none, 0.0f);
    fprintf(stderr, "incline mu 0.5: 24.57 deg moved %.5f m, 28.57 deg moved %.4f m\n", stay.slide,
            slide.slide);
    ST_LT(de__absd(stay.slide), 0.005);
    /* a = g (sin - mu cos) = 0.39 m/s^2: about 0.78 m in 2 s. */
    ST_GT(slide.slide, 0.4);
    ST_LT(slide.slide, 1.0);
    /* mu = 1 (45 degrees). */
    ST_LT(de__absd(incline(43.0f, 1.0f, 1.0f, 120, 0.0f, none, 0.0f).slide), 0.005);
    ST_GT(incline(47.0f, 1.0f, 1.0f, 120, 0.0f, none, 0.0f).slide, 0.4);
    /* sqrt mixing: sqrt(0.25 * 1) = 0.5, the same friction angle as 0.5 / 0.5. */
    ST_LT(de__absd(incline(24.57f, 0.25f, 1.0f, 120, 0.0f, none, 0.0f).slide), 0.005);
    ST_GT(incline(28.57f, 0.25f, 1.0f, 120, 0.0f, none, 0.0f).slide, 0.4);
    /* Frictionless: free slide, g sin(20) = 3.36 m/s^2, 6.7 m in 2 s. */
    EXPECT_FLOAT_EQ(incline(20.0f, 0.0f, 0.5f, 120, 0.0f, none, 0.0f).slide, 6.7, 0.2);
}

TEST(physics_solver_contact, anisotropic_friction_direction) {
    /* The box's friction is 0.05 along its local Y axis and 1.0 across it; the slab's is 1.0.
     * Mixed: sqrt(0.05) = 0.22 along the axis (below tan 20 = 0.36), 1.0 across. */
    const de_vec3 axis_y = {0, 1, 0};
    const incline_result along = incline(20.0f, 0.05f, 1.0f, 120, 0.0f, axis_y, 1.0f);
    const incline_result across = incline(20.0f, 0.05f, 1.0f, 120, PI_F * 0.5f, axis_y, 1.0f);
    fprintf(stderr, "anisotropic: axis downhill moved %.4f m, axis across moved %.5f m\n",
            along.slide, across.slide);
    /* Axis downhill: slides with a = g (sin 20 - 0.22 cos 20) = 1.33 m/s^2 (2.7 m in 2 s). */
    EXPECT_FLOAT_EQ(along.slide, 2.66, 0.2);
    ST_LT(de__absd(along.lateral), 0.01);
    /* Turned 90 degrees: the low-friction axis is across the slope, so it holds. */
    ST_LT(de__absd(across.slide), 0.005);
    /* An axis 45 degrees off the slope: it slides, and drifts sideways toward the slippery axis
     * (maximum-dissipation friction). */
    const incline_result diag = incline(20.0f, 0.05f, 1.0f, 120, PI_F * 0.25f, axis_y, 1.0f);
    fprintf(stderr, "anisotropic, axis at 45 degrees: down %.4f m, sideways %.4f m\n", diag.slide,
            diag.lateral);
    ST_GT(diag.slide, 0.2);
    ST_GT(de__absd(diag.lateral), 0.1);
    /* A zero axis is isotropic: friction 0.05 everywhere, so it slides in both orientations. */
    const de_vec3 none = {0, 0, 0};
    ST_GT(incline(20.0f, 0.05f, 1.0f, 120, PI_F * 0.5f, none, 1.0f).slide, 1.0);
}

TEST(physics_solver_contact, sliding_sphere_starts_rolling) {
    /* A solid sphere launched sliding at 3 m/s: friction turns it into rolling at 5/7 of that
     * speed (angular momentum about the contact point is kept), and the rolling contact neither
     * lifts nor sinks (core anchors: the sphere's center does not move when it rotates). */
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape g = st_box(p, 50.0f, 50.0f, 0.5f);
    const de_shape ball = st_sphere(p, 0.3f);
    (void)st_body(p, DE_BODY_STATIC, g, st_xform(0, 0, -0.5, st_quat_identity()), 0.5f, 0.0f);
    const de_body b =
        st_body(p, DE_BODY_DYNAMIC, ball, st_xform(0, 0, 0.3, st_quat_identity()), 0.5f, 0.0f);
    de_body_set_velocity(p, b, (de_vec3){3.0f, 0.0f, 0.0f}, (de_vec3){0.0f, 0.0f, 0.0f});
    f64 max_dz = 0.0;
    for (u32 t = 0; t < 120; ++t) {
        de_physics_step(p, ST_DT);
        const f64 dz = de__absd(st_pos(p, b).z - 0.3);
        max_dz = dz > max_dz ? dz : max_dz;
    }
    const de_vec3 v = st_vel(p, b), w = st_angvel(p, b);
    fprintf(stderr, "rolling: v %.4f (expected %.4f), w.y %.4f, max |dz| %.2g\n", v.x,
            3.0 * 5.0 / 7.0, w.y, max_dz);
    EXPECT_FLOAT_EQ(v.x, 3.0f * 5.0f / 7.0f, 0.02f);
    EXPECT_FLOAT_EQ(w.y, v.x / 0.3f, 0.05f); /* rolls without slipping: v = w x r */
    ST_LT(de__absf(v.z), 1e-3f);
    ST_LT(max_dz, 0.002);
    de_physics_destroy(p);
}

TEST(physics_solver_contact, body_material_validation) {
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape cube = st_box(p, 0.5f, 0.5f, 0.5f);
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = cube;
    d.material.friction = -0.1f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, &d)));
    d.material.friction = 0.5f;
    d.material.restitution = -1.0f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, &d)));
    d.material.restitution = 0.5f;
    d.friction_secondary = -1.0f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, &d)));
    d.friction_secondary = 0.2f;
    d.friction_axis = (de_vec3){0.0f, 3.0f, 4.0f};
    const de_body b = de_body_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    const u32 i = de__handle_index(b.id);
    EXPECT_FLOAT_EQ(p->bodies.friction_axis[i].y, 0.6f, 1e-6f); /* normalized */
    EXPECT_FLOAT_EQ(p->bodies.friction_axis[i].z, 0.8f, 1e-6f);
    d.friction_axis = (de_vec3){0.0f, 0.0f, 0.0f};
    const de_body iso = de_body_create(p, &d);
    const u32 k = de__handle_index(iso.id);
    EXPECT_TRUE(p->bodies.friction_axis[k].x == 0.0f && p->bodies.friction_axis[k].y == 0.0f &&
                p->bodies.friction_axis[k].z == 0.0f);
    de_physics_destroy(p);
}
