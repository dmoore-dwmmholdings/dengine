/*
 * scenarios_large_world.c: the large-world determinism scenario (DENG-222; SPEC §5.5, §13.2,
 * §19.4; DENG-2 Q10). Registered by tests/determinism/physics/CMakeLists.txt.
 *
 *   stack_far (600 ticks): everything 100 km out, at (100 km, -100 km, 50 m): a pyramid of 8 rows
 *                          (36 unit boxes) on a static ground, hit near its top by a ball thrown
 *                          at tick 120 (it shifts the top rows and bounces back); a ball-joint
 *                          pendulum on a static post and a hinged pair of planks swinging from
 *                          it. Island origins, contact anchors and joint anchors are all formed
 *                          relative to bodies 100 km from the world origin. The probe hashes the
 *                          joints (de_physics_checksum does not cover them yet).
 *
 * SPEC §5.7: de_dmath / de_dvec3 only, bodies created and scripted in a fixed order. Built with
 * DE_DETERMINISTIC_FLAGS. Golden: golden/stack_far.checksums (regenerate only with
 * test_det_physics --update-golden).
 */
#include "harness.h"

#include "physics/joints/joint_internal.h"

#include <string.h>

#define SF_X 100000.0
#define SF_Y (-100000.0)
#define SF_Z 50.0

static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static de_transform at(f64 x, f64 y, f64 z) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position.x = SF_X + x;
    t.position.y = SF_Y + y;
    t.position.z = SF_Z + z;
    t.rotation.w = 1.0f;
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

static de_body make(de_physics_world *p, de_body_type type, de_shape s, de_transform t,
                    f32 friction, f32 restitution) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform = t;
    d.material.friction = friction;
    d.material.restitution = restitution;
    d.gravity_scale = 1.0f;
    return de_body_create(p, &d);
}

static de_joint_desc joint(de_joint_kind kind, de_body a, de_body b, de_dvec3 fa, de_dvec3 fb) {
    de_joint_desc d;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    d.body_a = a;
    d.body_b = b;
    d.frame_a.position = fa;
    d.frame_a.rotation.w = 1.0f;
    d.frame_b.position = fb;
    d.frame_b.rotation.w = 1.0f;
    return d;
}

typedef struct sf_state {
    de_shape ball;
} sf_state;

static de_result sf_setup(de_phys_det_ctx *c) {
    sf_state *st = (sf_state *)c->state;
    de_physics_world *p = c->physics;
    const de_shape ground = box_shape(p, 20.0f, 20.0f, 0.5f);
    const de_shape cube = box_shape(p, 0.5f, 0.5f, 0.5f);
    const de_shape post = box_shape(p, 0.1f, 0.1f, 2.0f);
    const de_shape bob = sphere_shape(p, 0.15f);
    const de_shape plank = box_shape(p, 0.3f, 0.1f, 0.4f);
    st->ball = sphere_shape(p, 0.4f);
    if (!DE_HANDLE_VALID(make(p, DE_BODY_STATIC, ground, at(0, 0, -0.5), 0.6f, 0.0f)))
        return DE_ERR_INVALID_ARGUMENT;
    /* A pyramid of 8 rows: row r holds 8 - r boxes 1.05 m apart, 1 mm gaps. */
    for (u32 r = 0; r < 8u; ++r) {
        const u32 n = 8u - r;
        for (u32 k = 0; k < n; ++k) {
            const f64 x = 1.05 * ((f64)k - 0.5 * (f64)(n - 1u));
            if (!DE_HANDLE_VALID(
                    make(p, DE_BODY_DYNAMIC, cube, at(x, 0.0, 0.5 + 1.001 * (f64)r), 0.6f, 0.0f)))
                return DE_ERR_INVALID_ARGUMENT;
        }
    }
    /* A post 6 m beside it: a ball-joint pendulum (1.5 m, released at 30 degrees) and a pair of
     * planks on hinges about Y, pushed sideways. */
    const de_body pst = make(p, DE_BODY_STATIC, post, at(0.0, 6.0, 2.0), 0.5f, 0.0f);
    const de_body pend = make(p, DE_BODY_DYNAMIC, bob, at(0.75, 6.4, 4.0 - 1.299), 0.5f, 0.0f);
    const de_body p1 = make(p, DE_BODY_DYNAMIC, plank, at(0.0, 5.6, 3.5), 0.5f, 0.0f);
    const de_body p2 = make(p, DE_BODY_DYNAMIC, plank, at(0.0, 5.6, 2.6), 0.5f, 0.0f);
    if (!DE_HANDLE_VALID(pst) || !DE_HANDLE_VALID(pend) || !DE_HANDLE_VALID(p1) ||
        !DE_HANDLE_VALID(p2))
        return DE_ERR_INVALID_ARGUMENT;
    const de_dvec3 top = {0.0, 0.4, 2.0}; /* on the post, body-local */
    const de_dvec3 pend_pivot = {-0.75, 0.0, 1.299};
    de_joint_desc jd = joint(DE_JOINT_BALL, pst, pend, top, pend_pivot);
    if (!DE_HANDLE_VALID(de_joint_create(p, &jd)))
        return DE_ERR_INVALID_ARGUMENT;
    const de_quat x_to_y = {0.0f, 0.0f, 0.70710678f, 0.70710678f}; /* hinge axis X -> world Y */
    jd = joint(DE_JOINT_HINGE, pst, p1, (de_dvec3){0.0, -0.4, 2.0}, (de_dvec3){0.0, 0.0, 0.5});
    jd.frame_a.rotation = x_to_y;
    jd.frame_b.rotation = x_to_y;
    if (!DE_HANDLE_VALID(de_joint_create(p, &jd)))
        return DE_ERR_INVALID_ARGUMENT;
    jd = joint(DE_JOINT_HINGE, p1, p2, (de_dvec3){0.0, 0.0, -0.45}, (de_dvec3){0.0, 0.0, 0.45});
    jd.frame_a.rotation = x_to_y;
    jd.frame_b.rotation = x_to_y;
    if (!DE_HANDLE_VALID(de_joint_create(p, &jd)))
        return DE_ERR_INVALID_ARGUMENT;
    if (!de_body_set_velocity(p, p2, v3(2.5f, 0.0f, 0.0f), v3(0.0f, 0.0f, 0.0f)))
        return DE_ERR_INVALID_ARGUMENT;
    return DE_OK;
}

static void sf_script(de_phys_det_ctx *c, u64 tick) {
    sf_state *st = (sf_state *)c->state;
    if (tick != 120u)
        return;
    /* A ball thrown at the pyramid's top rows, slightly off-axis. */
    const de_body b = make(c->physics, DE_BODY_DYNAMIC, st->ball, at(-9.0, 0.3, 6.0), 0.5f, 0.2f);
    (void)de_body_set_velocity(c->physics, b, v3(13.0f, -0.2f, 2.5f), v3(0.0f, 3.0f, 0.0f));
}

static u64 sf_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    return de__joints_hash(c->physics);
}

const de_phys_det_scenario de_phys_det_scenario_stack_far = {
    .name = "stack_far",
    .setup = sf_setup,
    .script = sf_script,
    .probe = sf_probe,
    .ticks = 600u,
    .state_size = (u32)sizeof(sf_state),
    .max_bodies = 64u,
};
DE_PHYS_DET_REGISTER(stack_far)
