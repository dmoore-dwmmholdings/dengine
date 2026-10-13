/*
 * scenarios_islands.c: determinism scenarios of islands, sleeping and the graph-colored solver
 * (DENG-218; SPEC §13.1, §13.2, §19.4). Registered by tests/determinism/physics/CMakeLists.txt.
 *
 *   pyramid_10 (600 ticks):  a pyramid of 10 rows (55 unit boxes) that may not sleep, so the
 *                            colored solve of a deep stack runs for 10 s, and a 1:10 mass-ratio
 *                            box pair beside it.
 *   sleep_wake (600 ticks):  bodies that may sleep: a 6-row pyramid, a stack of 4 boxes on a
 *                            static plate, a box on a kinematic platform. They fall asleep and are
 *                            woken by a thrown ball (contact), a user velocity, de_body_wake, a
 *                            destroyed sleeping box (index reuse afterwards), the platform starting
 *                            to move, and the plate under the stack being destroyed; each falls
 *                            asleep again. The probe hashes which bodies sleep.
 *
 * SPEC §5.7: de_dmath / de_dvec3 only, bodies created and scripted in a fixed order. Built with
 * DE_DETERMINISTIC_FLAGS. Goldens: golden/<name>.checksums (regenerate only with
 * test_det_physics --update-golden).
 */
#include "harness.h"

#include <string.h>

static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static de_transform xform_at(f64 x, f64 y, f64 z) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position.x = x;
    t.position.y = y;
    t.position.z = z;
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
                    f32 friction, f32 mass, u32 flags) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform = t;
    d.mass = mass;
    d.material.friction = friction;
    d.gravity_scale = 1.0f;
    d.flags = flags;
    return de_body_create(p, &d);
}

/* Rows of unit boxes centered at (x0, y0) on z = z0: row r holds rows - r boxes 1.05 m apart,
 * each on two below, 1 mm gaps. Bottom row first, left to right. */
static bool pyramid(de_physics_world *p, de_shape cube, u32 rows, f64 x0, f64 y0, f64 z0, u32 flags,
                    de_body *out, u32 *count) {
    for (u32 r = 0; r < rows; ++r) {
        const u32 n = rows - r;
        for (u32 k = 0; k < n; ++k) {
            const f64 x = x0 + 1.05 * ((f64)k - 0.5 * (f64)(n - 1u));
            const de_body b = make(p, DE_BODY_DYNAMIC, cube,
                                   xform_at(x, y0, z0 + 0.5 + 1.001 * (f64)r), 0.6f, 0.0f, flags);
            if (!DE_HANDLE_VALID(b))
                return false;
            if (out != NULL)
                out[(*count)++] = b;
        }
    }
    return true;
}

/* ================================ pyramid_10 ============================================ */

static de_result py10_setup(de_phys_det_ctx *c) {
    de_physics_world *p = c->physics;
    const de_shape ground = box_shape(p, 20.0f, 20.0f, 0.5f);
    const de_shape cube = box_shape(p, 0.5f, 0.5f, 0.5f);
    if (!DE_HANDLE_VALID(make(p, DE_BODY_STATIC, ground, xform_at(0, 0, -0.5), 0.6f, 0, 0)))
        return DE_ERR_INVALID_ARGUMENT;
    if (!pyramid(p, cube, 10, 0.0, 0.0, 0.0, 0u, NULL, NULL))
        return DE_ERR_INVALID_ARGUMENT;
    /* 10 kg on 1 kg, beside the pyramid. */
    if (!DE_HANDLE_VALID(make(p, DE_BODY_DYNAMIC, cube, xform_at(0, 4, 0.5), 0.6f, 1.0f, 0u)) ||
        !DE_HANDLE_VALID(make(p, DE_BODY_DYNAMIC, cube, xform_at(0, 4, 1.501), 0.6f, 10.0f, 0u)))
        return DE_ERR_INVALID_ARGUMENT;
    return DE_OK;
}

const de_phys_det_scenario de_phys_det_scenario_pyramid_10 = {
    .name = "pyramid_10",
    .setup = py10_setup,
    .ticks = 600u,
    .max_bodies = 64u,
};
DE_PHYS_DET_REGISTER(pyramid_10)

/* ================================ sleep_wake ============================================ */

#define SW_PYR 21u /* 6 rows */

typedef struct sw_state {
    de_shape cube, ball_shape;
    de_body pyr[SW_PYR];
    u32 pyr_count;
    de_body stack[4];
    de_body plate;    /* static, under the stack */
    de_body platform; /* kinematic */
    de_body rider;    /* on the platform */
    de_body ball;
} sw_state;

static de_result sw_setup(de_phys_det_ctx *c) {
    sw_state *st = (sw_state *)c->state;
    de_physics_world *p = c->physics;
    const u32 sleep = DE_BODY_FLAG_ALLOW_SLEEP;
    st->cube = box_shape(p, 0.5f, 0.5f, 0.5f);
    st->ball_shape = sphere_shape(p, 0.4f);
    const de_shape ground = box_shape(p, 25.0f, 25.0f, 0.5f);
    const de_shape plate = box_shape(p, 1.5f, 1.5f, 0.25f);
    const de_shape deck = box_shape(p, 2.0f, 2.0f, 0.25f);
    if (!DE_HANDLE_VALID(make(p, DE_BODY_STATIC, ground, xform_at(0, 0, -0.5), 0.6f, 0, 0)))
        return DE_ERR_INVALID_ARGUMENT;
    if (!pyramid(p, st->cube, 6, 0.0, 0.0, 0.0, sleep, st->pyr, &st->pyr_count))
        return DE_ERR_INVALID_ARGUMENT;
    /* A stack of 4 on a static plate 2 m above the ground. */
    st->plate = make(p, DE_BODY_STATIC, plate, xform_at(8.0, 0.0, 1.75), 0.6f, 0, 0);
    for (u32 k = 0; k < 4; ++k)
        st->stack[k] = make(p, DE_BODY_DYNAMIC, st->cube, xform_at(8.0, 0.0, 2.5 + 1.001 * (f64)k),
                            0.6f, 0.0f, sleep);
    /* A box on a kinematic platform that starts moving later. */
    st->platform = make(p, DE_BODY_KINEMATIC, deck, xform_at(-8.0, 6.0, 0.25), 0.8f, 0, 0);
    st->rider = make(p, DE_BODY_DYNAMIC, st->cube, xform_at(-8.0, 6.0, 1.001), 0.8f, 0.0f, sleep);
    if (!DE_HANDLE_VALID(st->plate) || !DE_HANDLE_VALID(st->stack[3]) ||
        !DE_HANDLE_VALID(st->platform) || !DE_HANDLE_VALID(st->rider))
        return DE_ERR_INVALID_ARGUMENT;
    return DE_OK;
}

static void sw_script(de_phys_det_ctx *c, u64 tick) {
    sw_state *st = (sw_state *)c->state;
    de_physics_world *p = c->physics;
    const de_vec3 zero = v3(0.0f, 0.0f, 0.0f);
    switch (tick) {
    case 90: /* a ball thrown at the pyramid's second row */
        st->ball = make(p, DE_BODY_DYNAMIC, st->ball_shape, xform_at(-7.0, 0.0, 1.6), 0.5f, 0.0f,
                        DE_BODY_FLAG_ALLOW_SLEEP);
        (void)de_body_set_velocity(p, st->ball, v3(11.0f, 0.3f, 0.0f), zero);
        break;
    case 150: /* the user pushes the top of the stack */
        (void)de_body_set_velocity(p, st->stack[3], v3(0.0f, 0.6f, 0.0f), zero);
        break;
    case 230: /* explicit wake */
        de_body_wake(p, st->stack[0]);
        break;
    case 300: /* the platform starts moving */
        (void)de_body_set_velocity(p, st->platform, v3(0.5f, 0.0f, 0.0f), v3(0.0f, 0.0f, 0.2f));
        break;
    case 360:
        (void)de_body_set_velocity(p, st->platform, zero, zero);
        break;
    case 380: /* a box of the pyramid destroyed (the rest of its island wakes) */
        de_body_destroy(p, st->pyr[st->pyr_count - 1u]);
        break;
    case 420: /* its index reused by a new box dropped beside the pyramid */
        st->pyr[st->pyr_count - 1u] = make(p, DE_BODY_DYNAMIC, st->cube, xform_at(4.5, -3.0, 3.0),
                                           0.6f, 0.0f, DE_BODY_FLAG_ALLOW_SLEEP);
        break;
    case 470: /* the plate under the stack is destroyed: the stack falls */
        de_body_destroy(p, st->plate);
        break;
    default:
        break;
    }
}

/* Which bodies sleep: one bit per live body, in index order, folded into a u64. */
static u64 sw_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    const sw_state *st = (const sw_state *)c->state;
    u64 h = 0x9E3779B97F4A7C15ull;
    const de_body *lists[3] = {st->pyr, st->stack, &st->rider};
    const u32 counts[3] = {st->pyr_count, 4u, 1u};
    u32 sleeping = 0;
    for (u32 l = 0; l < 3; ++l)
        for (u32 k = 0; k < counts[l]; ++k) {
            const u64 bit = de_body_is_sleeping(c->physics, lists[l][k]) ? 1u : 0u;
            sleeping += (u32)bit;
            h = (h ^ bit) * 0x100000001B3ull;
        }
    const u64 ball = de_body_is_sleeping(c->physics, st->ball) ? 1u : 0u;
    h = (h ^ ball) * 0x100000001B3ull;
    return h ^ ((u64)sleeping << 48);
}

const de_phys_det_scenario de_phys_det_scenario_sleep_wake = {
    .name = "sleep_wake",
    .setup = sw_setup,
    .script = sw_script,
    .probe = sw_probe,
    .ticks = 600u,
    .state_size = (u32)sizeof(sw_state),
    .max_bodies = 64u,
};
DE_PHYS_DET_REGISTER(sleep_wake)
