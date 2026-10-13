/*
 * test_large_world.c: large worlds (DENG-222, SPEC §5.5, §13.1; DENG-2 Q10: +-100 km without
 * rebasing). The same scenes run at the origin and 100 km out must behave the same:
 *   - helpers: de__phys_relative (f64 subtract, one f32 cast), f32 spacing, island origins;
 *   - test_stack_far_stable: a resting stack of 10 at (100 km, 100 km, 0) for 10 s;
 *   - test_offset_vs_origin_trajectory: tumbling, bouncing and sliding bodies for 600 ticks;
 *   - test_joint_far: pendulum period and a jointed chain at 100 km;
 *   - test_aabb_margin_far: world AABBs and broadphase pairs at 100 km.
 *   - ccd_bullet_far: a CCD bullet (DENG-219) stops at a thin wall 100 km out as at the origin.
 * Scenes stay small: every test runs well under a second natively and stays fast under the
 * sanitizers.
 */
#include "joint_test_util.h"

#include "physics/large_world.h"
#include "physics/shapes/shape_internal.h"

#define LW_FAR_X 100000.0
#define LW_FAR_Y (-100000.0)
#define LW_FAR_Z 50.0

/* |(far - offset) - origin| (f64). */
static f64 lw_dev(de_dvec3 far, de_dvec3 origin, de_dvec3 offset) {
    return st_len(de__d3_sub(de__d3_sub(far, offset), origin));
}

static f64 lw_maxd(f64 a, f64 b) {
    return a > b ? a : b;
}

/* Largest |q_a - q_b| component, sign-aligned. */
static f64 lw_qdev(de_quat a, de_quat b) {
    const f32 s = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) < 0.0f ? -1.0f : 1.0f;
    f64 m = de__absd((f64)(a.x - s * b.x));
    m = lw_maxd(m, de__absd((f64)(a.y - s * b.y)));
    m = lw_maxd(m, de__absd((f64)(a.z - s * b.z)));
    return lw_maxd(m, de__absd((f64)(a.w - s * b.w)));
}

/* ================================ Helpers =============================================== */

TEST(physics_large_world, relative_and_spacing) {
    /* f64 subtraction first, then one f32 rounding: millimetres survive 100 km out. */
    const de_dvec3 w = {100000.0012345678, -100000.25, 50.0};
    const de_dvec3 o = {100000.0, -100000.0, 0.0};
    const de_vec3 r = de__phys_relative(w, o);
    EXPECT_TRUE(r.x == (f32)(w.x - o.x));
    EXPECT_TRUE(r.y == -0.25f);
    EXPECT_TRUE(r.z == 50.0f);
    /* The f32 world path loses the millimetre: (f32)100000.0012 - (f32)100000 == 0. */
    EXPECT_TRUE((f32)w.x - (f32)o.x == 0.0f);
    EXPECT_TRUE(r.x > 0.0012f && r.x < 0.0013f);

    EXPECT_TRUE(de__phys_f32_spacing(1.0) == 1.0 / 8388608.0);         /* 2^-23 */
    EXPECT_TRUE(de__phys_f32_spacing(100000.0) == 1.0 / 128.0);        /* 2^-7 = 7.8 mm */
    EXPECT_TRUE(de__phys_f32_spacing(-100000.0) == 1.0 / 128.0);       /* symmetric */
    EXPECT_TRUE(de__phys_f32_spacing(DE__PHYS_MAX_RELATIVE) <= 0.002); /* ~2 mm */

    EXPECT_TRUE(de__phys_relative_ok((de_dvec3){1000.0, -1000.0, 0.0}));
    EXPECT_FALSE(de__phys_relative_ok((de_dvec3){LW_FAR_X, 0.0, 0.0})); /* a world position */
    EXPECT_FALSE(de__phys_relative_ok((de_dvec3){0.0, LW_FAR_Y, 0.0}));
}

/* Island origin = position of the island's lowest-index body at the start of the step. Two
 * separate stacks give two islands; static bodies have none. */
TEST(physics_large_world, island_origin_is_lowest_body) {
    de_physics_world *p = st_world(16, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape g = st_box(p, 20.0f, 20.0f, 0.5f);
    const de_shape c = st_box(p, 0.5f, 0.5f, 0.5f);
    const de_body ground = st_body(p, DE_BODY_STATIC, g,
                                   st_xform(LW_FAR_X, LW_FAR_Y, -0.5, st_quat_identity()), 0.6f, 0);
    de_body a[3], b[2];
    for (u32 i = 0; i < 3; ++i)
        a[i] = st_body(p, DE_BODY_DYNAMIC, c,
                       st_xform(LW_FAR_X - 5.0, LW_FAR_Y, 0.5 + 1.001 * (f64)i, st_quat_identity()),
                       0.6f, 0);
    for (u32 i = 0; i < 2; ++i)
        b[i] = st_body(p, DE_BODY_DYNAMIC, c,
                       st_xform(LW_FAR_X + 5.0, LW_FAR_Y, 0.5 + 1.001 * (f64)i, st_quat_identity()),
                       0.6f, 0);
    de_dvec3 o;
    u32 ia = 0, ib = 0, ig = 0;
    ASSERT_TRUE(de__body_resolve(p, a[2], &ia));
    ASSERT_TRUE(de__body_resolve(p, b[1], &ib));
    ASSERT_TRUE(de__body_resolve(p, ground, &ig));
    EXPECT_FALSE(de__phys_body_origin(p, ia, &o)); /* no step yet */
    for (u32 t = 0; t < 30; ++t)
        de_physics_step(p, ST_DT);
    const de_dvec3 a0 = st_pos(p, a[0]), b0 = st_pos(p, b[0]);
    de_physics_step(p, ST_DT);
    ASSERT_TRUE(de__phys_body_origin(p, ia, &o));
    EXPECT_MEMEQ(&o, &a0, sizeof o); /* bit for bit: the f64 position, not a rounded one */
    ASSERT_TRUE(de__phys_body_origin(p, ib, &o));
    EXPECT_MEMEQ(&o, &b0, sizeof o);
    EXPECT_FALSE(de__phys_body_origin(p, ig, &o)); /* static: no island */
    de_physics_destroy(p);
}

/* ================================ Resting stack ========================================= */

#define LW_STACK 10u

typedef struct lw_stack {
    de_physics_world *p;
    de_body boxes[LW_STACK];
} lw_stack;

static void lw_stack_build(lw_stack *s, f64 x0, f64 y0) {
    s->p = st_world(32, NULL, NULL);
    ASSERT_TRUE(s->p != NULL);
    const de_shape g = st_box(s->p, 10.0f, 10.0f, 0.5f);
    const de_shape c = st_box(s->p, 0.5f, 0.5f, 0.5f);
    ASSERT_TRUE(DE_HANDLE_VALID(
        st_body(s->p, DE_BODY_STATIC, g, st_xform(x0, y0, -0.5, st_quat_identity()), 0.6f, 0)));
    for (u32 i = 0; i < LW_STACK; ++i)
        s->boxes[i] = st_body(s->p, DE_BODY_DYNAMIC, c,
                              st_xform(x0, y0, 0.5 + 1.001 * (f64)i, st_quat_identity()), 0.6f, 0);
}

/*
 * AC: a resting 10-box stack at (100 km, 100 km, 0) for 10 s: the top box's drift < 1 mm and no
 * jitter (per-tick speed < 1e-3 m/s), matching the origin case. Soft contacts compress the stack
 * and make it rock while it settles (physics: Dynamics, "Behaviour"); that happens at the origin
 * too. So "matching the origin case" is checked tick by tick (every box, relative to its start,
 * within 1 mm of the origin stack), drift is the top box's movement once settled (5 s to 10 s),
 * and jitter is every box's speed over the last 2 s.
 */
TEST(physics_large_world, test_stack_far_stable) {
    static lw_stack near, far;
    lw_stack_build(&near, 0.0, 0.0);
    lw_stack_build(&far, LW_FAR_X, -LW_FAR_Y);
    const de_dvec3 off = {LW_FAR_X, -LW_FAR_Y, 0.0};
    f64 max_dev = 0.0, max_speed_late[2] = {0, 0};
    de_dvec3 top_settled[2] = {{0, 0, 0}, {0, 0, 0}};
    for (u32 t = 1; t <= 600; ++t) {
        de_physics_step(near.p, ST_DT);
        de_physics_step(far.p, ST_DT);
        for (u32 i = 0; i < LW_STACK; ++i)
            max_dev = lw_maxd(
                max_dev, lw_dev(st_pos(far.p, far.boxes[i]), st_pos(near.p, near.boxes[i]), off));
        if (t == 300) {
            top_settled[0] = st_pos(near.p, near.boxes[LW_STACK - 1u]);
            top_settled[1] = st_pos(far.p, far.boxes[LW_STACK - 1u]);
        }
        if (t > 480) {
            const lw_stack *ss[2] = {&near, &far};
            for (u32 k = 0; k < 2; ++k)
                for (u32 i = 0; i < LW_STACK; ++i)
                    max_speed_late[k] =
                        lw_maxd(max_speed_late[k], (f64)st_lenf(st_vel(ss[k]->p, ss[k]->boxes[i])));
        }
    }
    const f64 drift_near =
        st_len(de__d3_sub(st_pos(near.p, near.boxes[LW_STACK - 1u]), top_settled[0]));
    const f64 drift_far =
        st_len(de__d3_sub(st_pos(far.p, far.boxes[LW_STACK - 1u]), top_settled[1]));
    fprintf(stderr,
            "  stack far vs origin: max dev %.3g m; top drift 5-10 s: origin %.3g m, far %.3g m; "
            "max speed last 2 s: origin %.3g, far %.3g m/s\n",
            max_dev, drift_near, drift_far, max_speed_late[0], max_speed_late[1]);
    ST_LT(max_dev, 1e-3);
    ST_LT(drift_far, 1e-3);
    ST_LT(drift_near, 1e-3);
    ST_LT(max_speed_late[1], 1e-3);
    ST_LT(max_speed_late[0], 1e-3);
    de_physics_destroy(near.p);
    de_physics_destroy(far.p);
}

/* ================================ Trajectories ========================================== */

#define LW_TRAJ_N 8u

typedef struct lw_traj {
    de_physics_world *p;
    de_body bodies[LW_TRAJ_N];
} lw_traj;

/* Bodies of several kinds thrown, spinning, onto ground and a ramp; they bounce, slide, roll and
 * hit each other. Positions relative to (x0, y0, z0). */
static void lw_traj_build(lw_traj *s, f64 x0, f64 y0, f64 z0) {
    de_physics_world *p = st_world(32, NULL, NULL);
    s->p = p;
    ASSERT_TRUE(p != NULL);
    const de_shape g = st_box(p, 40.0f, 40.0f, 0.5f);
    const de_shape ramp = st_box(p, 3.0f, 2.0f, 0.2f);
    const de_shape cube = st_box(p, 0.4f, 0.3f, 0.25f);
    const de_shape ball = st_sphere(p, 0.35f);
    de_shape_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.kind = DE_SHAPE_CAPSULE;
    cd.radius = 0.2f;
    cd.half_height = 0.4f;
    const de_shape cap = de_shape_create(p, &cd);
    const de_quat tilt = st_quat_axis_angle((de_vec3){0, 1, 0}, 0.3f);
    ASSERT_TRUE(DE_HANDLE_VALID(
        st_body(p, DE_BODY_STATIC, g, st_xform(x0, y0, z0 - 0.5, st_quat_identity()), 0.5f, 0)));
    ASSERT_TRUE(DE_HANDLE_VALID(
        st_body(p, DE_BODY_STATIC, ramp, st_xform(x0 - 4.0, y0, z0 + 1.0, tilt), 0.3f, 0)));
    const de_shape kinds[3] = {cube, ball, cap};
    for (u32 i = 0; i < LW_TRAJ_N; ++i) {
        const f64 fi = (f64)i;
        const de_quat q = st_quat_axis_angle((de_vec3){1.0f, 0.5f * (f32)i, 0.3f}, 0.4f * (f32)i);
        s->bodies[i] =
            st_body(p, DE_BODY_DYNAMIC, kinds[i % 3u],
                    st_xform(x0 - 5.0 + 1.3 * fi, y0 + 0.4 * (fi - 3.5), z0 + 2.5 + 0.6 * fi, q),
                    0.4f + 0.05f * (f32)i, (i % 3u) == 1u ? 0.6f : 0.1f);
        ASSERT_TRUE(DE_HANDLE_VALID(s->bodies[i]));
        const de_vec3 v = {1.5f - 0.4f * (f32)i, 0.3f * (f32)(i % 3u) - 0.3f, -1.0f};
        const de_vec3 w = {0.7f * (f32)(i % 2u), 1.1f - 0.2f * (f32)i, 0.5f};
        ASSERT_TRUE(de_body_set_velocity(p, s->bodies[i], v, w));
    }
}

/* AC: the same scene at the origin and 100 km out agrees within 1e-4 m (relative to the offset)
 * after 600 ticks; checked every tick, with rotations too. */
TEST(physics_large_world, test_offset_vs_origin_trajectory) {
    static lw_traj near, far;
    const de_dvec3 off = {LW_FAR_X, LW_FAR_Y, LW_FAR_Z};
    lw_traj_build(&near, 0.0, 0.0, 0.0);
    lw_traj_build(&far, off.x, off.y, off.z);
    f64 max_dev = 0.0, max_qdev = 0.0, travel = 0.0;
    de_dvec3 start[LW_TRAJ_N];
    for (u32 i = 0; i < LW_TRAJ_N; ++i)
        start[i] = st_pos(near.p, near.bodies[i]);
    for (u32 t = 1; t <= 600; ++t) {
        de_physics_step(near.p, ST_DT);
        de_physics_step(far.p, ST_DT);
        for (u32 i = 0; i < LW_TRAJ_N; ++i) {
            max_dev = lw_maxd(
                max_dev, lw_dev(st_pos(far.p, far.bodies[i]), st_pos(near.p, near.bodies[i]), off));
            max_qdev = lw_maxd(
                max_qdev, lw_qdev(st_rot(far.p, far.bodies[i]), st_rot(near.p, near.bodies[i])));
        }
    }
    for (u32 i = 0; i < LW_TRAJ_N; ++i)
        travel = lw_maxd(travel, st_len(de__d3_sub(st_pos(near.p, near.bodies[i]), start[i])));
    fprintf(stderr, "  trajectories: max position dev %.3g m, rotation dev %.3g, travel %.2f m\n",
            max_dev, max_qdev, travel);
    ST_GT(travel, 2.0); /* the scene really moves */
    ST_LT(max_dev, 1e-4);
    ST_LT(max_qdev, 1e-4);
    de_physics_destroy(near.p);
    de_physics_destroy(far.p);
}

/* ================================ Joints ================================================ */

/* Period of a 1 m ball-joint pendulum (r = 2 cm sphere, 5 degrees) pivoting on a static body at
 * (x0, y0, z0 + 3): the zero crossings of x, averaged over two periods. */
static f64 lw_pendulum_period(f64 x0, f64 y0, f64 z0) {
    de_physics_world *p = st_world(8, NULL, NULL);
    const de_shape ball = st_sphere(p, 0.02f);
    const de_shape post = st_box(p, 0.05f, 0.05f, 0.05f);
    const f64 L = 1.0, th = 5.0 * 3.14159265358979 / 180.0;
    const f64 sx = L * de_dsin64(th), sz = -L * de_dcos64(th);
    const f64 pz = z0 + 3.0;
    const de_body pivot =
        st_body(p, DE_BODY_STATIC, post, st_xform(x0, y0, pz, st_quat_identity()), 0.5f, 0.0f);
    const de_body b = jt_dyn(p, ball, st_xform(x0 + sx, y0, pz + sz, st_quat_identity()), 0.0f);
    de_joint_desc d =
        jt_desc(DE_JOINT_BALL, pivot, b, (de_dvec3){0, 0, 0}, (de_dvec3){-sx, 0, -sz});
    EXPECT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
    f64 prev = sx, crossings[3] = {0.0, 0.0, 0.0};
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

#define LW_CHAIN 6u

/* A hanging chain: a static post, then LW_CHAIN boxes linked by hinge, ball and cone-twist
 * joints between bodies, pushed sideways. */
static de_physics_world *lw_chain(f64 x0, f64 y0, f64 z0, de_body *links) {
    de_physics_world *p = st_world(16, NULL, NULL);
    const de_shape post = st_box(p, 0.1f, 0.1f, 0.1f);
    const de_shape link = st_box(p, 0.1f, 0.1f, 0.25f);
    de_body prev = st_body(p, DE_BODY_STATIC, post, st_xform(x0, y0, z0 + 5.0, st_quat_identity()),
                           0.5f, 0.0f);
    const de_joint_kind kinds[3] = {DE_JOINT_HINGE, DE_JOINT_BALL, DE_JOINT_CONE_TWIST};
    for (u32 i = 0; i < LW_CHAIN; ++i) {
        const f64 z = z0 + 5.0 - 0.1 - 0.3 - 0.6 * (f64)i;
        links[i] = jt_dyn(p, link, st_xform(x0, y0, z, st_quat_identity()), 2.0f);
        const de_dvec3 fa = i == 0 ? (de_dvec3){0, 0, -0.1} : (de_dvec3){0, 0, -0.3};
        de_joint_desc d = jt_desc(kinds[i % 3u], prev, links[i], fa, (de_dvec3){0, 0, 0.3});
        if (kinds[i % 3u] == DE_JOINT_HINGE) /* hinge about world Y: frames turned X -> Y */
            d.frame_a.rotation = d.frame_b.rotation =
                st_quat_axis_angle((de_vec3){0, 0, 1}, 0.5f * JT_PI);
        if (kinds[i % 3u] == DE_JOINT_CONE_TWIST) {
            d.limits_min[3] = -0.5f;
            d.limits_max[3] = 0.5f;
            d.limits_max[4] = 0.8f;
            d.limits_max[5] = 0.8f;
        }
        EXPECT_TRUE(DE_HANDLE_VALID(de_joint_create(p, &d)));
        prev = links[i];
    }
    EXPECT_TRUE(de_body_set_velocity(p, links[LW_CHAIN - 1u], (de_vec3){2.0f, 1.0f, 0.0f},
                                     (de_vec3){0.0f, 0.0f, 1.0f}));
    return p;
}

/* AC: a pendulum 100 km out keeps its period within 1% of the origin case (it is the same within
 * a millisecond), and a jointed chain between bodies follows the same trajectory. */
TEST(physics_large_world, test_joint_far) {
    const f64 t0 = lw_pendulum_period(0.0, 0.0, 0.0);
    const f64 t1 = lw_pendulum_period(LW_FAR_X, LW_FAR_Y, LW_FAR_Z);
    const f64 t2 = lw_pendulum_period(-LW_FAR_X, -LW_FAR_Y, 0.0);
    fprintf(stderr, "  pendulum period: origin %.6f s, far %.6f s, opposite corner %.6f s\n", t0,
            t1, t2);
    ST_GT(t0, 1.9);
    ST_LT(de__absd(t1 - t0), 0.01 * t0);
    ST_LT(de__absd(t2 - t0), 0.01 * t0);
    ST_LT(de__absd(t1 - t0), 1e-3);

    de_body ln[LW_CHAIN], lf[LW_CHAIN];
    const de_dvec3 off = {LW_FAR_X, LW_FAR_Y, LW_FAR_Z};
    de_physics_world *pn = lw_chain(0.0, 0.0, 0.0, ln);
    de_physics_world *pf = lw_chain(off.x, off.y, off.z, lf);
    ASSERT_TRUE(pn != NULL && pf != NULL);
    f64 max_dev = 0.0, swing = 0.0;
    for (u32 t = 1; t <= 300; ++t) {
        de_physics_step(pn, ST_DT);
        de_physics_step(pf, ST_DT);
        for (u32 i = 0; i < LW_CHAIN; ++i)
            max_dev = lw_maxd(max_dev, lw_dev(st_pos(pf, lf[i]), st_pos(pn, ln[i]), off));
        swing = lw_maxd(swing, de__absd(st_pos(pn, ln[LW_CHAIN - 1u]).x));
    }
    fprintf(stderr, "  chain of %u: max dev %.3g m, swing %.2f m\n", LW_CHAIN, max_dev, swing);
    ST_GT(swing, 0.2);
    ST_LT(max_dev, 1e-4);
    de_physics_destroy(pn);
    de_physics_destroy(pf);
}

/* ================================ AABBs and broadphase ================================== */

/*
 * World AABBs stay f32 in world coordinates, rounded outward from f64. 100 km out they contain
 * the shape and are loose by at most one f32 spacing (2^-7 m = 7.8 mm) per side; the default fat
 * margin is several spacings; the broadphase finds every pair the origin scene finds, and the
 * narrowphase keeps exactly the same manifolds.
 */
TEST(physics_large_world, test_aabb_margin_far) {
    const f64 sp = de__phys_f32_spacing(LW_FAR_X);
    EXPECT_TRUE(sp == 1.0 / 128.0);
    /* The default fat margin covers the rounding with room to spare (no cvar change needed). */
    ST_GT((f64)DE__BP_DEFAULT_MARGIN, 4.0 * sp);

    de_physics_world *p = st_world(64, NULL, NULL);
    ASSERT_TRUE(p != NULL);
    const de_shape box = st_box(p, 0.5f, 0.3f, 0.2f);
    const de_shape ball = st_sphere(p, 0.4f);
    const de__shape *sb = de__shape_get(p, box);
    const de__shape *ss = de__shape_get(p, ball);
    ASSERT_TRUE(sb != NULL && ss != NULL);
    const de_dvec3 corners[4] = {{LW_FAR_X, LW_FAR_Y, LW_FAR_Z},
                                 {-LW_FAR_X, LW_FAR_Y, 0.0},
                                 {LW_FAR_X - 0.001, -LW_FAR_Y + 0.0037, -3.3},
                                 {99999.996, -99999.9921, 12.5}};
    f64 worst_loose = 0.0;
    for (u32 c = 0; c < 4; ++c) {
        for (u32 k = 0; k < 8; ++k) {
            const de__shape *sh = (k & 1u) ? ss : sb;
            const de_quat q =
                st_quat_axis_angle((de_vec3){0.3f, 1.0f, 0.2f * (f32)k}, 0.37f * (f32)k);
            const de_dvec3 pos = {corners[c].x + 0.123 * (f64)k, corners[c].y - 0.071 * (f64)k,
                                  corners[c].z};
            /* Exact bounds: the same shape at the origin (tight there) shifted in f64. */
            const de_aabb local = de__shape_world_aabb(sh, (de_dvec3){0, 0, 0}, q);
            const de_aabb far = de__shape_world_aabb(sh, pos, q);
            const f64 lo[3] = {pos.x + local.min.x, pos.y + local.min.y, pos.z + local.min.z};
            const f64 hi[3] = {pos.x + local.max.x, pos.y + local.max.y, pos.z + local.max.z};
            const f32 flo[3] = {far.min.x, far.min.y, far.min.z};
            const f32 fhi[3] = {far.max.x, far.max.y, far.max.z};
            for (u32 a = 0; a < 3; ++a) {
                const f64 s = de__phys_f32_spacing(a == 2 ? pos.z : (a == 0 ? pos.x : pos.y));
                /* Contains the shape (the origin box is tight to 1e-7 m), loose by at most one
                 * spacing. */
                EXPECT_TRUE((f64)flo[a] <= lo[a] + 1e-6);
                EXPECT_TRUE((f64)fhi[a] >= hi[a] - 1e-6);
                EXPECT_TRUE(lo[a] - (f64)flo[a] <= s + 1e-6);
                EXPECT_TRUE((f64)fhi[a] - hi[a] <= s + 1e-6);
                worst_loose =
                    lw_maxd(worst_loose, lw_maxd(lo[a] - (f64)flo[a], (f64)fhi[a] - hi[a]));
            }
        }
    }
    fprintf(stderr, "  world AABB at 100 km: loose by at most %.4f m (spacing %.4f m)\n",
            worst_loose, sp);
    de_physics_destroy(p);

    /* Pairs and manifolds: boxes and balls 1 to 3 cm apart (inside and outside the 2 cm
     * speculative margin) at the origin and 100 km out. */
    de_physics_world *w[2];
    const de_dvec3 offs[2] = {{0, 0, 0}, {LW_FAR_X, LW_FAR_Y, LW_FAR_Z}};
    for (u32 s = 0; s < 2; ++s) {
        w[s] = st_world(64, NULL, NULL);
        ASSERT_TRUE(w[s] != NULL);
        const de_shape g = st_box(w[s], 10.0f, 10.0f, 0.5f);
        const de_shape cb = st_box(w[s], 0.5f, 0.5f, 0.5f);
        const de_shape sp2 = st_sphere(w[s], 0.5f);
        const de_dvec3 o = offs[s];
        ASSERT_TRUE(DE_HANDLE_VALID(st_body(
            w[s], DE_BODY_STATIC, g, st_xform(o.x, o.y, o.z - 0.5, st_quat_identity()), 0.5f, 0)));
        for (u32 i = 0; i < 12; ++i) {
            const f64 gap = 0.005 + 0.0025 * (f64)i; /* 5 mm .. 32.5 mm */
            const f64 x = o.x - 6.0 + (f64)i * (1.0 + gap);
            de_body_desc d;
            memset(&d, 0, sizeof d);
            d.type = DE_BODY_DYNAMIC;
            d.shape = (i & 1u) ? sp2 : cb;
            d.transform = st_xform(x, o.y, o.z + 0.5 + 0.004 * (f64)(i % 3u), st_quat_identity());
            d.material.friction = 0.5f; /* gravity_scale 0: they stay where they are */
            ASSERT_TRUE(DE_HANDLE_VALID(de_body_create(w[s], &d)));
        }
        de_physics_step(w[s], ST_DT);
    }
    const de__bp_pair *pa = NULL, *pb = NULL;
    u32 na = 0, nb = 0;
    de__broadphase_pairs(w[0], &pa, &na);
    de__broadphase_pairs(w[1], &pb, &nb);
    u32 missing = 0;
    for (u32 i = 0; i < na; ++i) {
        bool found = false;
        for (u32 j = 0; j < nb && !found; ++j)
            found = pa[i].a == pb[j].a && pa[i].b == pb[j].b;
        missing += found ? 0u : 1u;
    }
    const de__manifold *ma = NULL, *mb = NULL;
    u32 nma = 0, nmb = 0;
    de__contacts_manifolds(w[0], &ma, &nma);
    de__contacts_manifolds(w[1], &mb, &nmb);
    fprintf(stderr, "  pairs: origin %u, far %u (missing %u); manifolds: origin %u, far %u\n", na,
            nb, missing, nma, nmb);
    EXPECT_EQ(missing, 0u);
    EXPECT_GE(nb, na);
    ASSERT_EQ(nma, nmb);
    ST_GT(nma, 5u);
    for (u32 i = 0; i < nma; ++i) {
        EXPECT_EQ(ma[i].key.a, mb[i].key.a);
        EXPECT_EQ(ma[i].key.b, mb[i].key.b);
        EXPECT_EQ(ma[i].point_count, mb[i].point_count);
    }
    de_physics_destroy(w[0]);
    de_physics_destroy(w[1]);
}

/* ================================ CCD =================================================== */

/* A 1 cm bullet (DE_BODY_FLAG_CCD, DENG-219) at 300 m/s hits a 1 cm static wall 3 m away, at the
 * origin and 100 km out: it stops at the wall in both, at the same place. Returns the bullet's
 * x relative to the wall's center after 4 steps. */
static f64 lw_bullet(f64 x0, f64 y0, f64 z0) {
    de_physics_world *p = st_world(8, NULL, NULL);
    EXPECT_TRUE(p != NULL);
    if (p == NULL)
        return 1.0;
    const de_shape wall = st_box(p, 0.005f, 5.0f, 5.0f);
    const de_shape ball = st_sphere(p, 0.005f);
    (void)st_body(p, DE_BODY_STATIC, wall, st_xform(x0, y0, z0, st_quat_identity()), 0.0f, 0.0f);
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = DE_BODY_DYNAMIC;
    d.shape = ball;
    d.transform = st_xform(x0 - 3.0, y0 + 0.37, z0 - 0.21, st_quat_identity());
    d.flags = DE_BODY_FLAG_CCD;
    const de_body b = de_body_create(p, &d);
    EXPECT_TRUE(de_body_set_velocity(p, b, (de_vec3){300.0f, 20.0f, -10.0f}, (de_vec3){0, 0, 0}));
    for (u32 t = 0; t < 4; ++t)
        de_physics_step(p, ST_DT);
    const f64 x = st_pos(p, b).x - x0;
    de_physics_destroy(p);
    return x;
}

TEST(physics_large_world, ccd_bullet_far) {
    const f64 near = lw_bullet(0.0, 0.0, 0.0);
    const f64 far = lw_bullet(LW_FAR_X, LW_FAR_Y, LW_FAR_Z);
    fprintf(stderr, "  bullet x after 4 steps: origin %.6f m, far %.6f m\n", near, far);
    ST_LT(near, -0.005); /* in front of the wall (its front face is x = -0.005) */
    ST_GT(near, -0.03);
    ST_LT(far, -0.005);
    ST_LT(de__absd(far - near), 1e-4);
}
