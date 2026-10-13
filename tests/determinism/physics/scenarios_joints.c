/*
 * scenarios_joints.c: determinism scenarios of the joints (DENG-220; SPEC §13.1, §13.2, §19.4).
 * Registered by tests/determinism/physics/CMakeLists.txt. The probe of each hashes the joints'
 * state (impulses, broken flags), which de_physics_checksum does not cover.
 *
 *   pendulum (600 ticks):    ball, distance (rope with slack), hinge, cone-twist and 6-DOF
 *                            pendulums from world pivots, a double pendulum of two hinges, and a
 *                            ball pendulum 25 km out.
 *   chain_20 (600 ticks):    a chain of 20 ball-jointed links falls from horizontal; a second chain
 *                            of 20 (cone-twist links, one breakable joint) hangs and is hit by a
 *                            ball; a joint is destroyed at tick 300 (wake, the pair collides again)
 *                            and a link body at tick 400 (its joints go with it); a ground.
 *   hinge_motor (600 ticks): a velocity-motor wheel whose target changes, a limited hinge beam
 *                            slammed into its stops, a door on a position motor whose target the
 *                            script moves (waking it), a slider with a velocity motor against its
 *                            limits, a 6-DOF with limits, and a kinematic arm carrying a box on a
 *                            fixed joint.
 *
 * SPEC §5.7: de_dmath / de_dvec3 only, bodies and joints created and scripted in a fixed order.
 * Built with DE_DETERMINISTIC_FLAGS. Goldens: golden/<name>.checksums (regenerate only with
 * test_det_physics --update-golden).
 */
#include "harness.h"

#include "physics/joints/joint_internal.h"

#include <string.h>

#define PI_F 3.14159265358979f

static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static de_dvec3 d3(f64 x, f64 y, f64 z) {
    de_dvec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static de_quat quat_axis_angle(de_vec3 axis, f32 angle) {
    const de_vec3 a = de_dnormalize_v3(axis);
    f32 s, c;
    de_dsincos(0.5f * angle, &s, &c);
    de_quat q;
    q.x = a.x * s;
    q.y = a.y * s;
    q.z = a.z * s;
    q.w = c;
    return q;
}

static de_quat quat_identity(void) {
    de_quat q;
    q.x = 0.0f;
    q.y = 0.0f;
    q.z = 0.0f;
    q.w = 1.0f;
    return q;
}

static de_transform xform(f64 x, f64 y, f64 z, de_quat q) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position = d3(x, y, z);
    t.rotation = q;
    t.scale = v3(1.0f, 1.0f, 1.0f);
    return t;
}

static de_shape box_shape(de_physics_world *p, f32 hx, f32 hy, f32 hz) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = v3(hx, hy, hz);
    return de_shape_create(p, &d);
}

static de_shape sphere_shape(de_physics_world *p, f32 r) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_SPHERE;
    d.radius = r;
    return de_shape_create(p, &d);
}

static de_body make(de_physics_world *p, de_body_type type, de_shape s, de_transform t, f32 mass,
                    u32 flags) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform = t;
    d.mass = mass;
    d.material.friction = 0.5f;
    d.gravity_scale = 1.0f;
    d.flags = flags;
    return de_body_create(p, &d);
}

static de_joint_desc jdesc(de_joint_kind kind, de_body a, de_body b, de_dvec3 fa, de_dvec3 fb) {
    de_joint_desc d;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    d.body_a = a;
    d.body_b = b;
    d.frame_a.position = fa;
    d.frame_a.rotation = quat_identity();
    d.frame_b.position = fb;
    d.frame_b.rotation = quat_identity();
    return d;
}

static u64 joints_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    return de__joints_hash(c->physics) ^ ((u64)de_physics_joint_breaks(c->physics, NULL, 0) << 56);
}

/* ================================ pendulum ============================================== */

static de_result pend_setup(de_phys_det_ctx *c) {
    de_physics_world *p = c->physics;
    const de_shape ball = sphere_shape(p, 0.1f);
    const de_shape rod = box_shape(p, 0.5f, 0.05f, 0.05f);
    const de_body none = {0};
    bool ok = true;
    /* Ball, distance (rope, slack at the start), cone-twist and 6-DOF pendulums released at
     * different angles, 2 m apart along Y. */
    const de_joint_kind kinds[4] = {DE_JOINT_BALL, DE_JOINT_DISTANCE, DE_JOINT_CONE_TWIST,
                                    DE_JOINT_6DOF};
    for (u32 k = 0; k < 4; ++k) {
        const f64 y = 2.0 * (f64)k;
        const f32 a = 0.3f + 0.25f * (f32)k;
        f32 s, co;
        de_dsincos(a, &s, &co);
        const de_body b = make(p, DE_BODY_DYNAMIC, ball,
                               xform(1.5 * s, y, 5.0 - 1.5 * co, quat_identity()), 0.0f, 0u);
        de_joint_desc d = jdesc(kinds[k], none, b, d3(0, y, 5), d3(-1.5 * s, 0, 1.5 * co));
        if (kinds[k] == DE_JOINT_DISTANCE) {
            d.frame_b.position = d3(0, 0, 0);
            d.limits_min[0] = 0.0f;
            d.limits_max[0] = 1.8f;
        }
        if (kinds[k] == DE_JOINT_CONE_TWIST) {
            d.frame_a.rotation = quat_axis_angle(v3(0, 1, 0), 0.5f * PI_F); /* X down */
            d.frame_b.rotation = quat_axis_angle(v3(0, 1, 0), 0.5f * PI_F - a);
            d.limits_min[3] = -0.2f;
            d.limits_max[3] = 0.2f;
            d.limits_max[4] = 0.4f;
            d.limits_max[5] = 0.6f;
        }
        if (kinds[k] == DE_JOINT_6DOF) {
            d.axis_mode[3] = DE_JOINT_AXIS_FREE;
            d.axis_mode[4] = DE_JOINT_AXIS_LIMITED;
            d.limits_min[4] = -0.6f;
            d.limits_max[4] = 0.6f;
            d.axis_mode[5] = DE_JOINT_AXIS_FREE;
        }
        ok = ok && DE_HANDLE_VALID(b) && DE_HANDLE_VALID(de_joint_create(p, &d));
        (void)de_body_set_velocity(p, b, v3(0.0f, 0.5f, 0.0f), v3(0.0f, 0.0f, 0.0f));
    }
    /* A double pendulum of two hinged rods about Y. */
    const de_quat hy = quat_axis_angle(v3(0, 0, 1), 0.5f * PI_F); /* frame X -> world Y */
    const de_body r1 = make(p, DE_BODY_DYNAMIC, rod, xform(0.5, 9, 5, quat_identity()), 0.0f, 0u);
    const de_body r2 = make(p, DE_BODY_DYNAMIC, rod, xform(1.5, 9, 5, quat_identity()), 0.0f, 0u);
    de_joint_desc h1 = jdesc(DE_JOINT_HINGE, none, r1, d3(0, 9, 5), d3(-0.5, 0, 0));
    h1.frame_a.rotation = hy;
    h1.frame_b.rotation = hy;
    de_joint_desc h2 = jdesc(DE_JOINT_HINGE, r1, r2, d3(0.5, 0, 0), d3(-0.5, 0, 0));
    h2.frame_a.rotation = hy;
    h2.frame_b.rotation = hy;
    ok = ok && DE_HANDLE_VALID(de_joint_create(p, &h1)) && DE_HANDLE_VALID(de_joint_create(p, &h2));
    /* A ball pendulum 25 km out. */
    const de_body far =
        make(p, DE_BODY_DYNAMIC, ball, xform(25001.0, -25000.0, 4.0, quat_identity()), 0.0f, 0u);
    const de_joint_desc fd =
        jdesc(DE_JOINT_BALL, none, far, d3(25000.0, -25000.0, 5.0), d3(-1.0, 0, 1.0));
    ok = ok && DE_HANDLE_VALID(far) && DE_HANDLE_VALID(de_joint_create(p, &fd));
    return ok ? DE_OK : DE_ERR_INVALID_ARGUMENT;
}

const de_phys_det_scenario de_phys_det_scenario_pendulum = {
    .name = "pendulum",
    .setup = pend_setup,
    .probe = joints_probe,
    .ticks = 600u,
    .max_bodies = 32u,
};
DE_PHYS_DET_REGISTER(pendulum)

/* ================================ chain_20 ============================================== */

typedef struct ch_state {
    de_body links_a[20], links_b[20];
    de_joint joints_b[20];
    de_shape ball;
} ch_state;

static de_result chain_setup(de_phys_det_ctx *c) {
    ch_state *st = (ch_state *)c->state;
    de_physics_world *p = c->physics;
    const de_shape ground = box_shape(p, 40.0f, 40.0f, 0.5f);
    const de_shape link = box_shape(p, 0.25f, 0.05f, 0.05f);
    st->ball = sphere_shape(p, 0.3f);
    bool ok = DE_HANDLE_VALID(
        make(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0.0f, 0u));
    /* Chain A: horizontal from (0, 0, 11), falls and swings over the ground. */
    de_body prev = {0};
    for (u32 i = 0; i < 20; ++i) {
        st->links_a[i] = make(p, DE_BODY_DYNAMIC, link,
                              xform(0.25 + 0.5 * (f64)i, 0, 11, quat_identity()), 0.0f, 0u);
        const de_joint_desc d = jdesc(DE_JOINT_BALL, prev, st->links_a[i],
                                      i == 0 ? d3(0, 0, 11) : d3(0.25, 0, 0), d3(-0.25, 0, 0));
        ok = ok && DE_HANDLE_VALID(st->links_a[i]) && DE_HANDLE_VALID(de_joint_create(p, &d));
        prev = st->links_a[i];
    }
    /* Chain B: hangs straight down from (0, 5, 11) on cone-twist joints; link 12 breaks. */
    const de_quat down = quat_axis_angle(v3(0, 1, 0), 0.5f * PI_F); /* local X -> world -Z */
    prev = (de_body){0};
    for (u32 i = 0; i < 20; ++i) {
        st->links_b[i] =
            make(p, DE_BODY_DYNAMIC, link, xform(0, 5, 11.0 - 0.25 - 0.5 * (f64)i, down), 0.0f, 0u);
        de_joint_desc d = jdesc(DE_JOINT_CONE_TWIST, prev, st->links_b[i],
                                i == 0 ? d3(0, 5, 11) : d3(0.25, 0, 0), d3(-0.25, 0, 0));
        if (i == 0)
            d.frame_a.rotation = down;
        d.limits_min[3] = -0.3f;
        d.limits_max[3] = 0.3f;
        d.limits_max[4] = 0.5f;
        d.limits_max[5] = 0.8f;
        d.collide_connected = (i % 2u) == 0u;
        d.break_force = i == 12 ? 900.0f : 0.0f;
        st->joints_b[i] = de_joint_create(p, &d);
        ok = ok && DE_HANDLE_VALID(st->links_b[i]) && DE_HANDLE_VALID(st->joints_b[i]);
        prev = st->links_b[i];
    }
    return ok ? DE_OK : DE_ERR_INVALID_ARGUMENT;
}

static void chain_script(de_phys_det_ctx *c, u64 tick) {
    ch_state *st = (ch_state *)c->state;
    de_physics_world *p = c->physics;
    switch (tick) {
    case 120: { /* a heavy ball thrown into chain B */
        const de_body b =
            make(p, DE_BODY_DYNAMIC, st->ball, xform(-4.0, 5.0, 6.0, quat_identity()), 60.0f, 0u);
        (void)de_body_set_velocity(p, b, v3(10.0f, 0.0f, 1.0f), v3(0.0f, 0.0f, 0.0f));
        break;
    }
    case 300: /* a joint of chain B destroyed: the lower part falls */
        de_joint_destroy(p, st->joints_b[5]);
        break;
    case 400: /* a link of chain A destroyed: its two joints go with it */
        de_body_destroy(p, st->links_a[10]);
        break;
    default:
        break;
    }
}

const de_phys_det_scenario de_phys_det_scenario_chain_20 = {
    .name = "chain_20",
    .setup = chain_setup,
    .script = chain_script,
    .probe = joints_probe,
    .ticks = 600u,
    .state_size = (u32)sizeof(ch_state),
    .max_bodies = 64u,
};
DE_PHYS_DET_REGISTER(chain_20)

/* ================================ hinge_motor =========================================== */

typedef struct hm_state {
    de_joint wheel, door, slider;
    de_body kin;
} hm_state;

static de_result hm_setup(de_phys_det_ctx *c) {
    hm_state *st = (hm_state *)c->state;
    de_physics_world *p = c->physics;
    const de_body none = {0};
    const de_shape ground = box_shape(p, 20.0f, 20.0f, 0.5f);
    const de_shape bar = box_shape(p, 0.5f, 0.1f, 0.1f);
    const de_shape slab = box_shape(p, 0.05f, 0.5f, 1.0f);
    const de_shape cube = box_shape(p, 0.2f, 0.2f, 0.2f);
    bool ok = DE_HANDLE_VALID(
        make(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0.0f, 0u));
    const de_quat up = quat_axis_angle(v3(0, 1, 0), -0.5f * PI_F); /* frame X -> world Z */
    const de_quat hy = quat_axis_angle(v3(0, 0, 1), 0.5f * PI_F);  /* frame X -> world Y */
    /* A wheel (bar) on a vertical axis, velocity motor. */
    const de_body wheel = make(p, DE_BODY_DYNAMIC, bar, xform(0, 0, 1, quat_identity()), 0.0f, 0u);
    de_joint_desc d = jdesc(DE_JOINT_HINGE, none, wheel, d3(0, 0, 1), d3(0, 0, 0));
    d.frame_a.rotation = up;
    d.frame_b.rotation = up;
    d.motor_mode[0] = DE_JOINT_MOTOR_VELOCITY;
    d.motor_target[0] = 4.0f;
    d.motor_max_force[0] = 60.0f;
    st->wheel = de_joint_create(p, &d);
    ok = ok && DE_HANDLE_VALID(st->wheel);
    /* A beam on a limited hinge about Y, dropped into its stops. */
    const de_body beam = make(p, DE_BODY_DYNAMIC, bar, xform(3.5, 0, 3, quat_identity()), 0.0f, 0u);
    d = jdesc(DE_JOINT_HINGE, none, beam, d3(3, 0, 3), d3(-0.5, 0, 0));
    d.frame_a.rotation = hy;
    d.frame_b.rotation = hy;
    d.enable_limit = true;
    d.limits_min[0] = -0.7f;
    d.limits_max[0] = 0.7f;
    ok = ok && DE_HANDLE_VALID(de_joint_create(p, &d));
    (void)de_body_set_velocity(p, beam, v3(0, 0, -2.0f), v3(0, 4.0f, 0));
    /* A door on a vertical hinge driven to an angle by a soft position motor; it may sleep. */
    const de_body door = make(p, DE_BODY_DYNAMIC, slab, xform(-3, 0.5, 1.2, quat_identity()), 0.0f,
                              DE_BODY_FLAG_ALLOW_SLEEP);
    d = jdesc(DE_JOINT_HINGE, none, door, d3(-3, 0, 1.2), d3(0, -0.5, 0));
    d.frame_a.rotation = up;
    d.frame_b.rotation = up;
    d.motor_mode[0] = DE_JOINT_MOTOR_POSITION;
    d.motor_target[0] = 1.0f;
    d.motor_max_force[0] = 300.0f;
    d.motor_hertz = 2.0f;
    d.motor_damping_ratio = 0.7f;
    st->door = de_joint_create(p, &d);
    ok = ok && DE_HANDLE_VALID(st->door);
    /* A slider along X with a velocity motor between its limits. */
    const de_body sl = make(p, DE_BODY_DYNAMIC, cube, xform(0, 4, 1, quat_identity()), 0.0f, 0u);
    d = jdesc(DE_JOINT_SLIDER, none, sl, d3(0, 4, 1), d3(0, 0, 0));
    d.enable_limit = true;
    d.limits_min[0] = -1.0f;
    d.limits_max[0] = 1.0f;
    d.motor_mode[0] = DE_JOINT_MOTOR_VELOCITY;
    d.motor_target[0] = 1.5f;
    d.motor_max_force[0] = 400.0f;
    st->slider = de_joint_create(p, &d);
    ok = ok && DE_HANDLE_VALID(st->slider);
    /* A 6-DOF: X limited, Y free, Z locked; twist limited, swing Y free, swing Z locked. */
    const de_body six = make(p, DE_BODY_DYNAMIC, cube, xform(4, 4, 2, quat_identity()), 0.0f, 0u);
    d = jdesc(DE_JOINT_6DOF, none, six, d3(4, 4, 2.4), d3(0, 0, 0.4));
    d.axis_mode[0] = DE_JOINT_AXIS_LIMITED;
    d.limits_min[0] = -0.3f;
    d.limits_max[0] = 0.3f;
    d.axis_mode[1] = DE_JOINT_AXIS_FREE;
    d.axis_mode[3] = DE_JOINT_AXIS_LIMITED;
    d.limits_min[3] = -0.5f;
    d.limits_max[3] = 0.5f;
    d.axis_mode[4] = DE_JOINT_AXIS_FREE;
    d.motor_mode[3] = DE_JOINT_MOTOR_VELOCITY;
    d.motor_target[3] = 1.0f;
    d.motor_max_force[3] = 5.0f;
    ok = ok && DE_HANDLE_VALID(de_joint_create(p, &d));
    (void)de_body_set_velocity(p, six, v3(1.0f, 0.5f, 0.0f), v3(0.0f, 1.0f, 0.0f));
    /* A kinematic arm carrying a box on a fixed joint. */
    st->kin = make(p, DE_BODY_KINEMATIC, bar, xform(-4, -4, 3, quat_identity()), 0.0f, 0u);
    const de_body carried =
        make(p, DE_BODY_DYNAMIC, cube, xform(-3.3, -4, 3, quat_identity()), 0.0f, 0u);
    d = jdesc(DE_JOINT_FIXED, st->kin, carried, d3(0.7, 0, 0), d3(0, 0, 0));
    ok = ok && DE_HANDLE_VALID(de_joint_create(p, &d));
    (void)de_body_set_velocity(p, st->kin, v3(0, 0, 0), v3(0, 0, 1.0f));
    return ok ? DE_OK : DE_ERR_INVALID_ARGUMENT;
}

static void hm_script(de_phys_det_ctx *c, u64 tick) {
    hm_state *st = (hm_state *)c->state;
    de_physics_world *p = c->physics;
    switch (tick) {
    case 150:
        (void)de_joint_set_motor_target(p, st->wheel, 0, -2.0f);
        (void)de_joint_set_motor_target(p, st->slider, 0, -1.5f);
        break;
    case 300: /* the door has settled (and may sleep); a new target wakes it */
        (void)de_joint_set_motor_target(p, st->door, 0, -0.5f);
        (void)de_body_set_velocity(p, st->kin, v3(0, 0, 0), v3(0, 0, -2.0f));
        break;
    case 450:
        (void)de_joint_set_motor_target(p, st->slider, 0, 1.5f);
        break;
    default:
        break;
    }
}

const de_phys_det_scenario de_phys_det_scenario_hinge_motor = {
    .name = "hinge_motor",
    .setup = hm_setup,
    .script = hm_script,
    .probe = joints_probe,
    .ticks = 600u,
    .state_size = (u32)sizeof(hm_state),
    .max_bodies = 32u,
};
DE_PHYS_DET_REGISTER(hinge_motor)
