/*
 * scenarios_solver.c: determinism scenarios of the soft-step solver (DENG-217; SPEC §13.1,
 * §13.2, §19.4). Registered by tests/determinism/physics/CMakeLists.txt.
 *
 *   box_drop (600 ticks):      28 dynamic bodies of every convex kind (boxes, hulls, capsules,
 *                              spheres, cylinders, a compound) dropped with random rotations and
 *                              velocities onto a ground box and a static ramp; mixed friction and
 *                              restitution, damping, one anisotropic box, one rotation-locked
 *                              box, a kinematic platform sweeping through, a body destroyed and
 *                              recreated at tick 300 (index reuse), and a small pile 25 km out.
 *   sphere_bounce (600 ticks): 16 spheres with restitution 0..0.95 dropped from 1-4 m, some
 *                              thrown and spinning (rolling friction), onto ground and a tilted
 *                              plate; a capsule tumbling among them.
 *   stack_10 (600 ticks):      a stack of 10 unit boxes and a 1:100 mass-ratio pair settling and
 *                              resting for 10 s.
 *
 * SPEC §5.7: de_dmath / de_dvec3 / de_rng only, one random draw per statement, bodies created
 * and scripted in a fixed order. Built with DE_DETERMINISTIC_FLAGS. Goldens:
 * golden/<name>.checksums (regenerate only with test_det_physics --update-golden).
 */
#include "harness.h"

#include <string.h>

static f32 rf(de_rng *r, f32 lo, f32 hi) {
    const f32 u = de_rng_f32(r);
    return lo + (hi - lo) * u;
}

static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static de_dvec3 dv3(f64 x, f64 y, f64 z) {
    de_dvec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static de_quat quat_identity(void) {
    de_quat q;
    q.x = 0.0f;
    q.y = 0.0f;
    q.z = 0.0f;
    q.w = 1.0f;
    return q;
}

static de_quat quat_axis_angle(de_vec3 axis, f32 angle) {
    const de_vec3 a = de_dnormalize_v3(axis);
    f32 s, c;
    de_dsincos(angle * 0.5f, &s, &c);
    de_quat q;
    q.x = a.x * s;
    q.y = a.y * s;
    q.z = a.z * s;
    q.w = c;
    return q;
}

/* A random rotation: a random axis and angle, one draw per statement. */
static de_quat random_rotation(de_rng *r) {
    const f32 ax = rf(r, -1.0f, 1.0f);
    const f32 ay = rf(r, -1.0f, 1.0f);
    const f32 az = rf(r, -1.0f, 1.0f);
    const f32 ang = rf(r, 0.0f, 3.1415926f);
    return quat_axis_angle(v3(ax, ay, az + 0.01f), ang);
}

static de_transform xform(de_dvec3 p, de_quat q) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position = p;
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

static de_shape round_shape(de_physics_world *p, de_shape_kind kind, f32 radius, f32 half_height) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    d.radius = radius;
    d.half_height = half_height;
    return de_shape_create(p, &d);
}

typedef struct body_params {
    de_body_type type;
    de_transform xf;
    de_vec3 v, w;
    f32 friction, restitution, mass, linear_damping, angular_damping;
    u32 flags;
    de_vec3 friction_axis;
    f32 friction_secondary;
} body_params;

static body_params params(de_body_type type, de_transform xf, f32 friction, f32 restitution) {
    body_params b;
    memset(&b, 0, sizeof b);
    b.type = type;
    b.xf = xf;
    b.friction = friction;
    b.restitution = restitution;
    return b;
}

static de_body make(de_physics_world *p, de_shape s, const body_params *b) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = b->type;
    d.shape = s;
    d.transform = b->xf;
    d.linear_velocity = b->v;
    d.angular_velocity = b->w;
    d.mass = b->mass;
    d.material.friction = b->friction;
    d.material.restitution = b->restitution;
    d.linear_damping = b->linear_damping;
    d.angular_damping = b->angular_damping;
    d.gravity_scale = 1.0f;
    d.flags = b->flags;
    d.friction_axis = b->friction_axis;
    d.friction_secondary = b->friction_secondary;
    return de_body_create(p, &d);
}

/* ================================ box_drop ============================================== */

#define BD_DROPS 28u
#define BD_FAR   4u
#define BD_FAR_X 25000.0
#define BD_FAR_Y -18000.0

typedef struct bd_state {
    de_shape kinds[6]; /* box, flat box, hull, capsule, sphere, cylinder */
    de_shape compound;
    de_body drops[BD_DROPS];
    de_body platform;
    u32 kind_of[BD_DROPS];
} bd_state;

static de_result bd_setup(de_phys_det_ctx *c) {
    bd_state *st = (bd_state *)c->state;
    de_physics_world *p = c->physics;
    de_rng r = de_rng_seed(c->seed, 217);
    const de_shape ground = box_shape(p, 30.0f, 30.0f, 0.5f);
    const de_shape ramp = box_shape(p, 3.0f, 2.0f, 0.25f);
    body_params g = params(DE_BODY_STATIC, xform(dv3(0, 0, -0.5), quat_identity()), 0.7f, 0.1f);
    (void)make(p, ground, &g);
    g.xf = xform(dv3(-4.0, 3.0, 0.8), quat_axis_angle(v3(0.0f, 1.0f, 0.0f), 0.35f));
    (void)make(p, ramp, &g);

    st->kinds[0] = box_shape(p, 0.4f, 0.3f, 0.25f);
    st->kinds[1] = box_shape(p, 0.6f, 0.5f, 0.08f);
    de_vec3 cloud[10];
    for (u32 i = 0; i < 10; ++i) {
        const f32 x = rf(&r, -0.35f, 0.35f);
        const f32 y = rf(&r, -0.35f, 0.35f);
        const f32 z = rf(&r, -0.3f, 0.3f);
        cloud[i] = v3(x, y, z);
    }
    de_shape_desc hd;
    memset(&hd, 0, sizeof hd);
    hd.kind = DE_SHAPE_CONVEX;
    hd.points = cloud;
    hd.point_count = 10;
    st->kinds[2] = de_shape_create(p, &hd);
    st->kinds[3] = round_shape(p, DE_SHAPE_CAPSULE, 0.2f, 0.3f);
    st->kinds[4] = round_shape(p, DE_SHAPE_SPHERE, 0.3f, 0.0f);
    st->kinds[5] = round_shape(p, DE_SHAPE_CYLINDER, 0.25f, 0.3f);
    const de_shape children[2] = {st->kinds[0], st->kinds[4]};
    const de_transform child_x[2] = {xform(dv3(0.0, 0.0, -0.1), quat_identity()),
                                     xform(dv3(0.35, 0.0, 0.2), quat_identity())};
    de_shape_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.kind = DE_SHAPE_COMPOUND;
    cd.children = children;
    cd.child_xforms = child_x;
    cd.child_count = 2;
    st->compound = de_shape_create(p, &cd);
    for (u32 k = 0; k < 6; ++k)
        if (!DE_HANDLE_VALID(st->kinds[k]))
            return DE_ERR_INVALID_ARGUMENT;
    if (!DE_HANDLE_VALID(st->compound))
        return DE_ERR_INVALID_ARGUMENT;

    for (u32 k = 0; k < BD_DROPS; ++k) {
        const f32 x = rf(&r, -5.0f, 5.0f);
        const f32 y = rf(&r, -5.0f, 5.0f);
        const f32 z = rf(&r, 1.0f, 6.0f);
        const de_quat q = random_rotation(&r);
        const f32 fr = rf(&r, 0.2f, 0.9f);
        const f32 re = rf(&r, 0.0f, 0.4f);
        body_params b = params(DE_BODY_DYNAMIC, xform(dv3((f64)x, (f64)y, (f64)z), q), fr, re);
        const f32 vx = rf(&r, -2.0f, 2.0f);
        const f32 vy = rf(&r, -2.0f, 2.0f);
        const f32 wz = rf(&r, -3.0f, 3.0f);
        b.v = v3(vx, vy, 0.0f);
        b.w = v3(0.5f * wz, -0.25f * wz, wz);
        b.linear_damping = (k % 5u == 0u) ? 0.3f : 0.0f;
        b.angular_damping = (k % 3u == 0u) ? 0.5f : 0.0f;
        if (k == 5u) { /* anisotropic friction */
            b.friction_axis = v3(1.0f, 0.0f, 0.0f);
            b.friction_secondary = 0.05f;
        }
        if (k == 9u) /* upright only */
            b.flags = DE_BODY_FLAG_LOCK_ROT_X | DE_BODY_FLAG_LOCK_ROT_Y;
        st->kind_of[k] = k % 7u;
        const de_shape s = st->kind_of[k] < 6u ? st->kinds[st->kind_of[k]] : st->compound;
        st->drops[k] = make(p, s, &b);
        if (!DE_HANDLE_VALID(st->drops[k]))
            return DE_ERR_INVALID_ARGUMENT;
    }
    /* A kinematic platform that sweeps across the ground (script sets its velocity). */
    body_params kp =
        params(DE_BODY_KINEMATIC, xform(dv3(-8.0, -6.0, 0.15), quat_identity()), 0.8f, 0.0f);
    st->platform = make(p, box_shape(p, 1.0f, 1.0f, 0.15f), &kp);

    /* Far out: a ground box and a small pile. */
    g.xf = xform(dv3(BD_FAR_X, BD_FAR_Y, -0.5), quat_identity());
    (void)make(p, ground, &g);
    for (u32 k = 0; k < BD_FAR; ++k) {
        const f32 x = rf(&r, -0.5f, 0.5f);
        const f32 y = rf(&r, -0.5f, 0.5f);
        const de_quat q = random_rotation(&r);
        body_params b = params(
            DE_BODY_DYNAMIC,
            xform(dv3(BD_FAR_X + (f64)x, BD_FAR_Y + (f64)y, 0.6 + 0.9 * (f64)k), q), 0.6f, 0.0f);
        if (!DE_HANDLE_VALID(make(p, st->kinds[k % 3u], &b)))
            return DE_ERR_INVALID_ARGUMENT;
    }
    return DE_OK;
}

static void bd_script(de_phys_det_ctx *c, u64 tick) {
    bd_state *st = (bd_state *)c->state;
    de_physics_world *p = c->physics;
    /* The platform goes +X for 4 s, then back. */
    const f32 vx = tick < 240u ? 4.0f : -4.0f;
    de_body_set_velocity(p, st->platform, v3(vx, 0.5f, 0.0f), v3(0.0f, 0.0f, 0.3f));
    if (tick == 300u) {
        /* Index reuse: the destroyed slot is taken by a new body at once. */
        de_body_destroy(p, st->drops[3]);
        body_params b =
            params(DE_BODY_DYNAMIC, xform(dv3(1.0, 1.0, 4.0), quat_identity()), 0.5f, 0.2f);
        b.w = v3(1.0f, 2.0f, 0.5f);
        st->drops[3] = make(p, st->kinds[0], &b);
    }
}

const de_phys_det_scenario de_phys_det_scenario_box_drop = {
    .name = "box_drop",
    .setup = bd_setup,
    .script = bd_script,
    .ticks = 600u,
    .state_size = (u32)sizeof(bd_state),
    .max_bodies = 64u,
};
DE_PHYS_DET_REGISTER(box_drop)

/* ================================ sphere_bounce ========================================= */

#define SB_BALLS 16u

static de_result sb_setup(de_phys_det_ctx *c) {
    de_physics_world *p = c->physics;
    de_rng r = de_rng_seed(c->seed, 218);
    /* Large: rolling balls keep going (no rolling resistance) and must not roll off. */
    const de_shape ground = box_shape(p, 60.0f, 60.0f, 0.5f);
    body_params g = params(DE_BODY_STATIC, xform(dv3(0, 0, -0.5), quat_identity()), 0.6f, 0.2f);
    (void)make(p, ground, &g);
    g.xf = xform(dv3(4.0, -3.0, 0.5), quat_axis_angle(v3(1.0f, 0.3f, 0.0f), 0.4f));
    g.restitution = 0.5f;
    (void)make(p, box_shape(p, 2.0f, 2.0f, 0.1f), &g);
    const de_shape small = round_shape(p, DE_SHAPE_SPHERE, 0.2f, 0.0f);
    const de_shape big = round_shape(p, DE_SHAPE_SPHERE, 0.45f, 0.0f);
    for (u32 k = 0; k < SB_BALLS; ++k) {
        const f32 x = rf(&r, -6.0f, 6.0f);
        const f32 y = rf(&r, -6.0f, 6.0f);
        const f32 z = rf(&r, 1.0f, 4.0f);
        const f32 e = 0.95f * (f32)k / (f32)(SB_BALLS - 1u);
        body_params b =
            params(DE_BODY_DYNAMIC, xform(dv3((f64)x, (f64)y, (f64)z), quat_identity()), 0.5f, e);
        if (k % 2u == 1u) { /* thrown and spinning: rolls after landing */
            const f32 vx = rf(&r, -3.0f, 3.0f);
            const f32 vy = rf(&r, -3.0f, 3.0f);
            b.v = v3(vx, vy, 1.0f);
            b.w = v3(-vy, vx, 2.0f);
        }
        if (k % 4u == 2u)
            b.angular_damping = 0.2f;
        if (k == 4u) /* lands on the tilted plate */
            b.xf = xform(dv3(4.0, -3.0, 3.0), quat_identity());
        if (!DE_HANDLE_VALID(make(p, (k % 3u == 0u) ? big : small, &b)))
            return DE_ERR_INVALID_ARGUMENT;
    }
    body_params cb =
        params(DE_BODY_DYNAMIC,
               xform(dv3(0.0, 0.0, 2.5), quat_axis_angle(v3(1.0f, 1.0f, 0.0f), 0.7f)), 0.4f, 0.3f);
    cb.w = v3(2.0f, 0.0f, 1.0f);
    if (!DE_HANDLE_VALID(make(p, round_shape(p, DE_SHAPE_CAPSULE, 0.25f, 0.4f), &cb)))
        return DE_ERR_INVALID_ARGUMENT;
    return DE_OK;
}

const de_phys_det_scenario de_phys_det_scenario_sphere_bounce = {
    .name = "sphere_bounce",
    .setup = sb_setup,
    .ticks = 600u,
    .max_bodies = 32u,
};
DE_PHYS_DET_REGISTER(sphere_bounce)

/* ================================ stack_10 ============================================== */

static de_result st10_setup(de_phys_det_ctx *c) {
    de_physics_world *p = c->physics;
    const de_shape ground = box_shape(p, 10.0f, 10.0f, 0.5f);
    const de_shape cube = box_shape(p, 0.5f, 0.5f, 0.5f);
    body_params g = params(DE_BODY_STATIC, xform(dv3(0, 0, -0.5), quat_identity()), 0.6f, 0.0f);
    (void)make(p, ground, &g);
    for (u32 k = 0; k < 10u; ++k) {
        body_params b =
            params(DE_BODY_DYNAMIC, xform(dv3(0.0, 0.0, 0.5 + 1.001 * (f64)k), quat_identity()),
                   0.6f, 0.0f);
        if (!DE_HANDLE_VALID(make(p, cube, &b)))
            return DE_ERR_INVALID_ARGUMENT;
    }
    /* 100 kg on 1 kg. */
    body_params light =
        params(DE_BODY_DYNAMIC, xform(dv3(4.0, 0.0, 0.5), quat_identity()), 0.6f, 0.0f);
    light.mass = 1.0f;
    body_params heavy =
        params(DE_BODY_DYNAMIC, xform(dv3(4.0, 0.0, 1.501), quat_identity()), 0.6f, 0.0f);
    heavy.mass = 100.0f;
    if (!DE_HANDLE_VALID(make(p, cube, &light)) || !DE_HANDLE_VALID(make(p, cube, &heavy)))
        return DE_ERR_INVALID_ARGUMENT;
    return DE_OK;
}

const de_phys_det_scenario de_phys_det_scenario_stack_10 = {
    .name = "stack_10",
    .setup = st10_setup,
    .ticks = 600u,
    .max_bodies = 32u,
};
DE_PHYS_DET_REGISTER(stack_10)
