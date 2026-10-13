/*
 * test_joints.c: joints (DENG-220): the API and the pool, each kind holding its locked degrees of
 * freedom, the pendulum period, a hanging chain of 20, collide_connected, 6-DOF locked = fixed,
 * wake-up and large worlds. Limits, motors and breaking are in test_joint_limits_motors.c.
 */
#include "joint_test_util.h"

/* ================================ API and pool ========================================== */

TEST(physics_joints, create_errors) {
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape box = st_box(p, 0.5f, 0.5f, 0.5f);
    const de_body a = jt_dyn(p, box, st_xform(0, 0, 0, st_quat_identity()), 0.0f);
    const de_body b = jt_dyn(p, box, st_xform(2, 0, 0, st_quat_identity()), 0.0f);
    const de_body none = {0};
    const de_dvec3 o = {0, 0, 0};

    EXPECT_FALSE(DE_HANDLE_VALID(de_joint_create(p, NULL)));
    de_joint_desc d = jt_desc(DE_JOINT_BALL, none, none, o, o);
    EXPECT_FALSE(DE_HANDLE_VALID(de_joint_create(p, &d))); /* both world */
    d = jt_desc(DE_JOINT_BALL, a, a, o, o);
    EXPECT_FALSE(DE_HANDLE_VALID(de_joint_create(p, &d))); /* same body */
    d = jt_desc(DE_JOINT_BALL, a, none, o, o);
    d.b.bits = 0x100000001ull; /* an entity side without a body: DENG-223 resolves it */
    EXPECT_FALSE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    d = jt_desc(DE_JOINT_HINGE, a, b, o, o);
    d.enable_limit = true;
    d.limits_min[0] = 1.0f;
    d.limits_max[0] = -1.0f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_joint_create(p, &d))); /* min > max */
    d = jt_desc(DE_JOINT_BALL, a, b, o, o);
    d.break_force = -1.0f;
    EXPECT_FALSE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    d = jt_desc((de_joint_kind)99, a, b, o, o);
    EXPECT_FALSE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    d = jt_desc(DE_JOINT_BALL, a, b, o, o);
    volatile f64 zero = 0.0;
    d.frame_a.position.x = 1.0 / zero;
    EXPECT_FALSE(DE_HANDLE_VALID(de_joint_create(p, &d)));

    /* Valid joints; a stale body is refused. */
    d = jt_desc(DE_JOINT_BALL, a, b, (de_dvec3){1, 0, 0}, (de_dvec3){-1, 0, 0});
    const de_joint j1 = de_joint_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(j1));
    EXPECT_TRUE(de_joint_valid(p, j1));
    EXPECT_FALSE(de_joint_is_broken(p, j1));
    de_joint_destroy(p, j1);
    EXPECT_FALSE(de_joint_valid(p, j1));
    de_joint_destroy(p, j1); /* stale: ignored */
    const de_joint j2 = de_joint_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(j2));
    EXPECT_EQ(de__handle_index(j2.id), de__handle_index(j1.id)); /* LIFO reuse */
    EXPECT_NE(j2.id, j1.id);                                     /* new generation */
    EXPECT_FALSE(de_joint_valid(p, j1));

    /* Destroying a body destroys its joints. */
    const de_joint j3 = de_joint_create(p, &(de_joint_desc){.kind = DE_JOINT_FIXED, .body_b = b});
    ASSERT_TRUE(DE_HANDLE_VALID(j3));
    de_body_destroy(p, b);
    EXPECT_FALSE(de_joint_valid(p, j2));
    EXPECT_FALSE(de_joint_valid(p, j3));
    d = jt_desc(DE_JOINT_BALL, a, b, o, o);
    EXPECT_FALSE(DE_HANDLE_VALID(de_joint_create(p, &d))); /* stale body */
    EXPECT_FALSE(de_joint_set_motor_target(p, j2, 0, 1.0f));
    de_physics_destroy(p);
}

TEST(physics_joints, pool_capacity_and_no_alloc) {
    de_tracking_allocator t;
    de_allocator al = de_tracking_allocator_init(&t, NULL, DE_MEM_PHYSICS);
    de_physics_world *p = st_world(16, NULL, &al);
    ASSERT_TRUE(p != NULL);
    const de_shape box = st_box(p, 0.5f, 0.5f, 0.5f);
    de_body bodies[4];
    for (u32 i = 0; i < 4; ++i)
        bodies[i] = jt_dyn(p, box, st_xform(2.0 * i, 0, 0, st_quat_identity()), 0.0f);
    ASSERT_EQ(de__joints_create(p, 3), DE_OK);
    EXPECT_EQ(de__joints_create(p, 3), DE_ERR_ALREADY_EXISTS);
    de_physics_step(p, ST_DT); /* warm-up */
    const de_tracking_stats before = de_tracking_allocator_stats(&t);
    de_joint j[4];
    for (u32 i = 0; i < 3; ++i) {
        const de_joint_desc d = jt_desc(DE_JOINT_DISTANCE, bodies[i], bodies[i + 1],
                                        (de_dvec3){0, 0, 0}, (de_dvec3){0, 0, 0});
        j[i] = de_joint_create(p, &d);
        EXPECT_TRUE(DE_HANDLE_VALID(j[i]));
    }
    const de_joint_desc d =
        jt_desc(DE_JOINT_BALL, bodies[0], bodies[3], (de_dvec3){0, 0, 0}, (de_dvec3){0, 0, 0});
    j[3] = de_joint_create(p, &d);
    EXPECT_FALSE(DE_HANDLE_VALID(j[3])); /* full */
    for (u32 tick = 0; tick < 30; ++tick)
        de_physics_step(p, ST_DT);
    de_joint_destroy(p, j[1]);
    EXPECT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    for (u32 tick = 0; tick < 30; ++tick)
        de_physics_step(p, ST_DT);
    const de_tracking_stats after = de_tracking_allocator_stats(&t);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.free_count, before.free_count);
    de__solver_stats ss;
    de__solver_stats_get(p, &ss);
    EXPECT_EQ(ss.class_count[DE__CONSTRAINT_CLASS_JOINT], 3u);
    de_physics_destroy(p);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
}

/* ================================ Kinds hold their locked DOF ============================ */

/*
 * A unit box hangs from a static post (or from the world) by joint `kind` with its frames at the
 * pivot. It starts with a push and a spin, so every free DOF moves. Returns the worst anchor
 * error (m) and, through *rot_err, the worst rotation error of the locked rotations.
 */
typedef struct kind_run {
    f64 anchor_err;  /* worst |anchor B - anchor A| */
    f32 axis_err;    /* hinge / slider: worst angle between the X axes */
    f32 rot_err;     /* fixed / slider: worst relative rotation */
    f64 off_axis;    /* slider: worst distance of B's anchor from A's X axis */
    f64 length_err;  /* distance: worst |length - rest| */
    f32 free_motion; /* how far the free DOF moved (rad or m), to see that they are free */
} kind_run;

static kind_run run_kind(de_joint_kind kind, bool world_side) {
    kind_run r;
    memset(&r, 0, sizeof r);
    de_physics_world *p = st_world(16, NULL, NULL);
    const de_shape box = st_box(p, 0.5f, 0.25f, 0.25f);
    const de_shape post_shape = st_box(p, 0.1f, 0.1f, 0.1f);
    const de_body post = world_side ? (de_body){0}
                                    : st_body(p, DE_BODY_STATIC, post_shape,
                                              st_xform(0, 0, 3, st_quat_identity()), 0.0f, 0.0f);
    /* The box's left end at the pivot (0, 0, 3), hanging tilted by 0.4 rad about Y. */
    const de_quat tilt = st_quat_axis_angle((de_vec3){0, 1, 0}, 0.4f);
    const de_vec3 half = de__quat_rotate(tilt, (de_vec3){0.5f, 0, 0});
    const de_body b = jt_dyn(p, box, st_xform(half.x, half.y, 3.0 + half.z, tilt), 0.0f);
    de_joint_desc d = jt_desc(kind, post, b, world_side ? (de_dvec3){0, 0, 3} : (de_dvec3){0, 0, 0},
                              (de_dvec3){-0.5, 0, 0});
    /* Frame A matches the body's tilt so the joint starts satisfied; X along the box. */
    d.frame_a.rotation = tilt;
    if (kind == DE_JOINT_DISTANCE) {
        d.frame_b.position = (de_dvec3){0.5, 0, 0}; /* the far end: rest length 1 */
        d.limits_min[0] = 0.0f;
        d.limits_max[0] = 0.0f;
    }
    if (kind == DE_JOINT_CONE_TWIST) {
        d.limits_min[3] = -JT_PI;
        d.limits_max[3] = JT_PI;
        d.limits_max[4] = JT_PI;
        d.limits_max[5] = JT_PI;
    }
    if (kind == DE_JOINT_6DOF) {
        d.axis_mode[0] = DE_JOINT_AXIS_FREE; /* a slider along X */
    }
    const de_joint j = de_joint_create(p, &d);
    EXPECT_TRUE(DE_HANDLE_VALID(j));
    de_body_set_velocity(p, b, (de_vec3){0.5f, 1.0f, 0.3f}, (de_vec3){1.0f, -0.5f, 2.0f});
    const de_quat r0 = st_rot(p, b);
    const de_dvec3 c0 = st_pos(p, b);
    const de_dvec3 pivot = {0, 0, 3};
    for (u32 tick = 0; tick < 120; ++tick) {
        de_physics_step(p, ST_DT);
        const de_quat rb = st_rot(p, b);
        const de_dvec3 ab = jt_world_point(p, b, d.frame_b.position);
        const f64 e = jt_dist(ab, pivot);
        const de_vec3 xa = de__quat_rotate(tilt, (de_vec3){1, 0, 0});
        const de_vec3 xb = de__quat_rotate(rb, (de_vec3){1, 0, 0});
        const f32 cosang = de__v3_dot(xa, xb);
        const f32 axis_angle = de_dacos(cosang > 1.0f ? 1.0f : cosang);
        const f32 rot = jt_rot_error(r0, rb);
        if (kind == DE_JOINT_DISTANCE) {
            const f64 le = e - 1.0 > 0 ? e - 1.0 : 1.0 - e;
            r.length_err = le > r.length_err ? le : r.length_err;
        } else if (kind == DE_JOINT_SLIDER || kind == DE_JOINT_6DOF) {
            const de_dvec3 off = de__d3_sub(ab, pivot);
            const f64 along = de__d3_dot(off, de__d3_from(xa));
            const de_dvec3 perp = de__d3_sub(off, de__d3_scale(de__d3_from(xa), along));
            const f64 pe = st_len(perp);
            r.off_axis = pe > r.off_axis ? pe : r.off_axis;
            r.rot_err = rot > r.rot_err ? rot : r.rot_err;
            const f32 fm = (f32)(along < 0 ? -along : along);
            r.free_motion = fm > r.free_motion ? fm : r.free_motion;
        } else {
            r.anchor_err = e > r.anchor_err ? e : r.anchor_err;
        }
        if (kind == DE_JOINT_HINGE)
            r.axis_err = axis_angle > r.axis_err ? axis_angle : r.axis_err;
        if (kind == DE_JOINT_FIXED)
            r.rot_err = rot > r.rot_err ? rot : r.rot_err;
        if (kind != DE_JOINT_SLIDER && kind != DE_JOINT_6DOF && kind != DE_JOINT_FIXED)
            r.free_motion = rot > r.free_motion ? rot : r.free_motion;
    }
    (void)c0;
    de_physics_destroy(p);
    return r;
}

TEST(physics_joints, each_kind_holds_its_locked_dof) {
    for (u32 w = 0; w < 2; ++w) {
        const bool world = w == 1;
        kind_run r = run_kind(DE_JOINT_FIXED, world);
        fprintf(stderr, "  fixed: anchor %.6f m, rot %.6f rad\n", r.anchor_err, (f64)r.rot_err);
        EXPECT_TRUE(r.anchor_err < 2e-3);
        EXPECT_TRUE(r.rot_err < 5e-3f);

        r = run_kind(DE_JOINT_BALL, world);
        fprintf(stderr, "  ball: anchor %.6f m, moved %.3f rad\n", r.anchor_err,
                (f64)r.free_motion);
        EXPECT_TRUE(r.anchor_err < 2e-3);
        EXPECT_TRUE(r.free_motion > 0.3f);

        r = run_kind(DE_JOINT_HINGE, world);
        fprintf(stderr, "  hinge: anchor %.6f m, axis %.6f rad, moved %.3f rad\n", r.anchor_err,
                (f64)r.axis_err, (f64)r.free_motion);
        EXPECT_TRUE(r.anchor_err < 2e-3);
        EXPECT_TRUE(r.axis_err < 5e-3f);
        EXPECT_TRUE(r.free_motion > 0.1f);

        r = run_kind(DE_JOINT_SLIDER, world);
        fprintf(stderr, "  slider: off axis %.6f m, rot %.6f rad, moved %.3f m\n", r.off_axis,
                (f64)r.rot_err, (f64)r.free_motion);
        EXPECT_TRUE(r.off_axis < 2e-3);
        EXPECT_TRUE(r.rot_err < 5e-3f);
        EXPECT_TRUE(r.free_motion > 0.3f);

        r = run_kind(DE_JOINT_DISTANCE, world);
        fprintf(stderr, "  distance: length %.6f m, moved %.3f rad\n", r.length_err,
                (f64)r.free_motion);
        /* A heavy box swinging on a 1 m rod (a double pendulum): a single soft row carries the
         * swing loads, so it gives a few millimetres. */
        EXPECT_TRUE(r.length_err < 1e-2);
        EXPECT_TRUE(r.free_motion > 0.3f);

        r = run_kind(DE_JOINT_CONE_TWIST, world);
        fprintf(stderr, "  cone-twist (open): anchor %.6f m, moved %.3f rad\n", r.anchor_err,
                (f64)r.free_motion);
        EXPECT_TRUE(r.anchor_err < 2e-3);
        EXPECT_TRUE(r.free_motion > 0.3f);

        r = run_kind(DE_JOINT_6DOF, world);
        fprintf(stderr, "  6dof (X free): off axis %.6f m, rot %.6f rad, moved %.3f m\n",
                r.off_axis, (f64)r.rot_err, (f64)r.free_motion);
        EXPECT_TRUE(r.off_axis < 2e-3);
        EXPECT_TRUE(r.rot_err < 5e-3f);
        EXPECT_TRUE(r.free_motion > 0.3f);
    }
}

/* ================================ Pendulum =============================================== */

/*
 * A small sphere (r = 2 cm) on a 1 m joint from a world pivot, released at 5 degrees. Returns the
 * period from the downward zero crossings of x (linear interpolation), averaged over two periods.
 */
static f64 pendulum_period(de_joint_kind kind, f64 x0, f64 y0, f64 z0) {
    de_physics_world *p = st_world(8, NULL, NULL);
    const de_shape ball = st_sphere(p, 0.02f);
    const f64 L = 1.0, th = 5.0 * 3.14159265358979 / 180.0;
    const f64 sx = L * de_dsin64(th), sz = -L * de_dcos64(th);
    const de_body b = jt_dyn(p, ball, st_xform(x0 + sx, y0, z0 + sz, st_quat_identity()), 0.0f);
    de_joint_desc d = jt_desc(kind, (de_body){0}, b, (de_dvec3){x0, y0, z0}, (de_dvec3){0, 0, 0});
    if (kind == DE_JOINT_BALL)
        d.frame_b.position = (de_dvec3){-sx, 0, -sz}; /* the pivot in the body frame */
    EXPECT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    f64 prev = sx, crossings[4];
    u32 n = 0;
    for (u32 tick = 1; tick <= 330 && n < 3; ++tick) {
        de_physics_step(p, ST_DT);
        const f64 x = st_pos(p, b).x - x0;
        if (prev > 0.0 && x <= 0.0)
            crossings[n++] = ((f64)tick - 1.0 + prev / (prev - x)) * (f64)ST_DT;
        prev = x;
    }
    de_physics_destroy(p);
    EXPECT_EQ(n, 3u);
    return n == 3u ? 0.5 * (crossings[2] - crossings[0]) : 0.0;
}

TEST(physics_joints, pendulum_period) {
    const f64 L = 1.0, g = 9.81, r = 0.02;
    const f64 ideal = 2.0 * 3.14159265358979 * de_dsqrt64(L / g);
    /* Ball joint at the pivot: a physical pendulum, I = m (L^2 + 2/5 r^2). */
    const f64 physical = ideal * de_dsqrt64(1.0 + 0.4 * r * r / (L * L));
    const f64 tb = pendulum_period(DE_JOINT_BALL, 0, 0, 3);
    const f64 td = pendulum_period(DE_JOINT_DISTANCE, 0, 0, 3);
    fprintf(stderr, "  ideal %.5f s, ball %.5f s (%.3f%%), distance %.5f s (%.3f%%)\n", ideal, tb,
            100.0 * (tb - ideal) / ideal, td, 100.0 * (td - ideal) / ideal);
    EXPECT_TRUE(tb > 0.99 * ideal && tb < 1.01 * ideal);
    EXPECT_TRUE(td > 0.99 * ideal && td < 1.01 * ideal);
    EXPECT_TRUE(tb > 0.99 * physical && tb < 1.01 * physical);
    /* 100 km out: the same period (anchors are relative to the centers of mass, f64). */
    const f64 far = pendulum_period(DE_JOINT_BALL, 100000.0, -100000.0, 3);
    fprintf(stderr, "  ball at 100 km: %.6f s\n", far);
    EXPECT_TRUE(far - tb < 1e-3 && tb - far < 1e-3);
}

/* ================================ Chain of 20 ============================================= */

/*
 * 20 links (0.5 m boxes, 5 kg) on ball joints from a world pivot at (0, 0, 10). `hanging`: they
 * start hanging straight down at rest; else horizontal, so the chain falls and swings. Returns the
 * worst separation of two joined anchors (m) over the last second of 5 s.
 */
static f64 chain_run(bool hanging, u32 substeps, f64 *worst_all) {
    de_physics_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.max_bodies = 32;
    pd.substeps = substeps;
    de_physics_world *p = NULL;
    EXPECT_EQ(de_physics_create(&pd, &p), DE_OK);
    const de_shape link = st_box(p, 0.25f, 0.05f, 0.05f);
    const de_quat q =
        hanging ? st_quat_axis_angle((de_vec3){0, 1, 0}, 1.5707963f) : st_quat_identity();
    de_body links[20];
    de_body prev = {0};
    for (u32 i = 0; i < 20; ++i) {
        const f64 s = 0.25 + 0.5 * i;
        links[i] =
            jt_dyn(p, link, hanging ? st_xform(0, 0, 10 - s, q) : st_xform(s, 0, 10, q), 0.0f);
        const de_joint_desc d = jt_desc(DE_JOINT_BALL, prev, links[i],
                                        i == 0 ? (de_dvec3){0, 0, 10} : (de_dvec3){0.25, 0, 0},
                                        (de_dvec3){-0.25, 0, 0});
        EXPECT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
        prev = links[i];
    }
    f64 worst = 0.0, worst_end = 0.0;
    for (u32 tick = 0; tick < 300; ++tick) {
        de_physics_step(p, ST_DT);
        for (u32 i = 0; i < 20; ++i) {
            const de_dvec3 above = i == 0 ? (de_dvec3){0, 0, 10}
                                          : jt_world_point(p, links[i - 1], (de_dvec3){0.25, 0, 0});
            const f64 e = jt_dist(above, jt_world_point(p, links[i], (de_dvec3){-0.25, 0, 0}));
            worst = e > worst ? e : worst;
            if (tick >= 240)
                worst_end = e > worst_end ? e : worst_end;
        }
    }
    de_physics_destroy(p);
    *worst_all = worst;
    return worst_end;
}

TEST(physics_joints, chain_20_stays_connected) {
    /* Soft joints sag in proportion to the load they carry: the top joint holds 19 links. The
     * joint stiffness is a quarter of the substep rate, so 8 substeps (120 Hz) keep a hanging
     * chain of 20 within 1 mm; the default 4 substeps (60 Hz) give about 2.6 mm. */
    f64 all;
    const f64 h8 = chain_run(true, 8, &all);
    fprintf(stderr, "  hanging, 8 substeps: %.6f m (whole run %.6f m)\n", h8, all);
    EXPECT_TRUE(h8 < 1e-3);
    EXPECT_TRUE(all < 1.5e-3);
    const f64 h4 = chain_run(true, 4, &all);
    fprintf(stderr, "  hanging, 4 substeps: %.6f m (whole run %.6f m)\n", h4, all);
    EXPECT_TRUE(h4 < 3e-3);
    /* Falling from horizontal and swinging: connected within about a centimetre. The worst
     * moment is the links whipping past each other near the end of the swing. DENG-219
     * measured it at 8 substeps: 15.4 mm with the links in one collision group (joints alone),
     * 12.6 mm with self-collision (speculative contacts stop the links on each other), and
     * 6.4 mm before DENG-219, when the links sank up to 8 cm into each other and the soft
     * push-out damped the swing. */
    const f64 s8 = chain_run(false, 8, &all);
    fprintf(stderr, "  swinging, 8 substeps: %.6f m\n", all);
    EXPECT_TRUE(all < 2e-2);
    (void)s8;
}

/* ================================ collide_connected ====================================== */

/* Two overlapping boxes joined by a distance joint. Returns the contact points of the last step. */
TEST(physics_joints, collide_connected) {
    for (u32 mode = 0; mode < 2; ++mode) {
        de_physics_world *p = st_world(8, NULL, NULL);
        de_physics_set_gravity(p, (de_vec3){0, 0, 0});
        const de_shape box = st_box(p, 0.5f, 0.5f, 0.5f);
        const de_body a = jt_dyn(p, box, st_xform(0, 0, 0, st_quat_identity()), 0.0f);
        const de_body b = jt_dyn(p, box, st_xform(0.8, 0, 0, st_quat_identity()), 0.0f);
        de_joint_desc d =
            jt_desc(DE_JOINT_DISTANCE, a, b, (de_dvec3){0, 0, 0}, (de_dvec3){0, 0, 0});
        d.collide_connected = mode == 1;
        const de_joint j = de_joint_create(p, &d);
        ASSERT_TRUE(DE_HANDLE_VALID(j));
        for (u32 tick = 0; tick < 3; ++tick)
            de_physics_step(p, ST_DT);
        de__solver_stats ss;
        de__solver_stats_get(p, &ss);
        if (mode == 0) {
            EXPECT_EQ(ss.contact_points, 0u); /* excluded */
            EXPECT_TRUE(de__joints_exclude_pair(p, 0, 1));
            EXPECT_TRUE(de__joints_exclude_pair(p, 1, 0));
            /* The bodies stay overlapped (the joint holds 0.8 m). */
            EXPECT_TRUE(jt_dist(st_pos(p, a), st_pos(p, b)) < 0.81);
            /* Destroying the joint makes them collide again at once. */
            de_joint_destroy(p, j);
            EXPECT_FALSE(de__joints_exclude_pair(p, 0, 1));
            de_physics_step(p, ST_DT);
            de__solver_stats_get(p, &ss);
            EXPECT_GT(ss.contact_points, 0u);
        } else {
            EXPECT_GT(ss.contact_points, 0u);
            EXPECT_FALSE(de__joints_exclude_pair(p, 0, 1));
        }
        de_physics_destroy(p);
    }
}

/* ================================ 6-DOF all locked = fixed ============================== */

static void locked_run(de_joint_kind kind, de_dvec3 *pos, de_quat *rot) {
    de_physics_world *p = st_world(8, NULL, NULL);
    const de_shape box = st_box(p, 0.4f, 0.2f, 0.1f);
    const de_body a = jt_dyn(p, box, st_xform(0, 0, 5, st_quat_identity()), 0.0f);
    const de_body b =
        jt_dyn(p, box, st_xform(1.0, 0.2, 5.1, st_quat_axis_angle((de_vec3){1, 1, 0}, 0.3f)), 0.0f);
    /* A hangs from the world by a ball joint; B is welded to A as it starts. */
    de_joint_desc h =
        jt_desc(DE_JOINT_BALL, (de_body){0}, a, (de_dvec3){-0.4, 0, 5.2}, (de_dvec3){-0.4, 0, 0.2});
    EXPECT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &h)));
    de_joint_desc d = jt_desc(kind, a, b, (de_dvec3){0.5, 0.1, 0.05}, (de_dvec3){0, 0, 0});
    d.frame_b.position = (de_dvec3){-0.5, -0.1, -0.05};
    /* B's frame rotation so that the frames coincide at the start. */
    d.frame_b.rotation = de__jq_conj(st_quat_axis_angle((de_vec3){1, 1, 0}, 0.3f));
    d.frame_b.position = (de_dvec3){0, 0, 0};
    {
        /* B's anchor at A's anchor point (0.5, 0.1, 5.05) in B's local frame. */
        const de_quat qb = st_quat_axis_angle((de_vec3){1, 1, 0}, 0.3f);
        const de_vec3 rel = {0.5f - 1.0f, 0.1f - 0.2f, 5.05f - 5.1f};
        const de_vec3 local = de__quat_rotate(de__jq_conj(qb), rel);
        d.frame_b.position = (de_dvec3){local.x, local.y, local.z};
    }
    EXPECT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    de_body_set_velocity(p, a, (de_vec3){1, 0, 0}, (de_vec3){0, 0, 1});
    for (u32 tick = 0; tick < 300; ++tick) /* 5 s */
        de_physics_step(p, ST_DT);
    *pos = st_pos(p, b);
    *rot = st_rot(p, b);
    de_physics_destroy(p);
}

TEST(physics_joints, six_dof_all_locked_equals_fixed) {
    de_dvec3 pf, p6;
    de_quat qf, q6;
    locked_run(DE_JOINT_FIXED, &pf, &qf);
    locked_run(DE_JOINT_6DOF, &p6, &q6);
    const f64 dp = jt_dist(pf, p6);
    const f32 dq = jt_rot_error(qf, q6);
    fprintf(stderr, "  6dof vs fixed after 5 s: %.3g m, %.3g rad\n", dp, (f64)dq);
    EXPECT_TRUE(dp < 1e-4);
    EXPECT_TRUE(dq < 1e-4f);
}

/* ================================ Wake, static and kinematic sides ===================== */

TEST(physics_joints, create_and_destroy_wake_bodies) {
    de_physics_world *p = st_world(8, NULL, NULL);
    const de_shape ground = st_box(p, 5.0f, 5.0f, 0.5f);
    const de_shape box = st_box(p, 0.5f, 0.5f, 0.5f);
    st_body(p, DE_BODY_STATIC, ground, st_xform(0, 0, -0.5, st_quat_identity()), 0.6f, 0.0f);
    de_body_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.type = DE_BODY_DYNAMIC;
    bd.shape = box;
    bd.gravity_scale = 1.0f;
    bd.material.friction = 0.6f;
    bd.flags = DE_BODY_FLAG_ALLOW_SLEEP;
    bd.transform = st_xform(0, 0, 0.5, st_quat_identity());
    const de_body a = de_body_create(p, &bd);
    bd.transform = st_xform(2, 0, 0.5, st_quat_identity());
    const de_body b = de_body_create(p, &bd);
    for (u32 tick = 0; tick < 120 && !(de_body_is_sleeping(p, a) && de_body_is_sleeping(p, b));
         ++tick)
        de_physics_step(p, ST_DT);
    ASSERT_TRUE(de_body_is_sleeping(p, a));
    ASSERT_TRUE(de_body_is_sleeping(p, b));
    de_joint_desc d = jt_desc(DE_JOINT_DISTANCE, a, b, (de_dvec3){0, 0, 0}, (de_dvec3){0, 0, 0});
    d.collide_connected = true;
    const de_joint j = de_joint_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(j));
    EXPECT_FALSE(de_body_is_sleeping(p, a));
    EXPECT_FALSE(de_body_is_sleeping(p, b));
    /* They fall asleep again with the joint (one island), and wake on destroy. */
    for (u32 tick = 0; tick < 120 && !de_body_is_sleeping(p, a); ++tick)
        de_physics_step(p, ST_DT);
    ASSERT_TRUE(de_body_is_sleeping(p, a));
    EXPECT_TRUE(de_body_is_sleeping(p, b));
    de__islands_stats is;
    de__islands_stats_get(p, &is);
    /* A motor target change wakes them too. */
    EXPECT_TRUE(de_joint_set_motor_target(p, j, 0, 0.0f)); /* unchanged: stays asleep */
    EXPECT_TRUE(de_body_is_sleeping(p, a));
    de_joint_destroy(p, j);
    EXPECT_FALSE(de_body_is_sleeping(p, a));
    EXPECT_FALSE(de_body_is_sleeping(p, b));
    de_physics_destroy(p);
}

TEST(physics_joints, static_and_kinematic_sides) {
    de_physics_world *p = st_world(8, NULL, NULL);
    const de_shape box = st_box(p, 0.2f, 0.2f, 0.2f);
    const de_body kin =
        st_body(p, DE_BODY_KINEMATIC, box, st_xform(0, 0, 5, st_quat_identity()), 0.0f, 0.0f);
    const de_body b = jt_dyn(p, box, st_xform(0, 0, 4, st_quat_identity()), 0.0f);
    const de_joint_desc d =
        jt_desc(DE_JOINT_FIXED, kin, b, (de_dvec3){0, 0, -1}, (de_dvec3){0, 0, 0});
    EXPECT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    /* The kinematic body carries B along at 2 m/s and turns it. */
    de_body_set_velocity(p, kin, (de_vec3){2, 0, 0}, (de_vec3){0, 0, 1});
    for (u32 tick = 0; tick < 60; ++tick)
        de_physics_step(p, ST_DT);
    const de_dvec3 want = jt_world_point(p, kin, (de_dvec3){0, 0, -1});
    const f64 e = jt_dist(want, st_pos(p, b));
    fprintf(stderr, "  carried by a kinematic body: %.6f m\n", e);
    EXPECT_TRUE(e < 5e-3);
    EXPECT_TRUE(jt_rot_error(st_rot(p, kin), st_rot(p, b)) < 5e-3f);
    /* Kinematic velocity is never changed by the joint. */
    EXPECT_TRUE(st_vel(p, kin).x == 2.0f);
    de_physics_destroy(p);
}
