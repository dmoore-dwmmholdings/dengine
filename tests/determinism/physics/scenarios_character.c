/*
 * scenarios_character.c: determinism scenario of the kinematic capsule character controller
 * (DENG-221; SPEC §13.1, §13.2, §19.4). Registered by tests/determinism/physics/CMakeLists.txt.
 *
 *   character_course (1,200 ticks): five characters on scripted inputs, moved with
 *   de_character_move before each step:
 *     A: up a 30 degree ramp, off its top edge, over a 0.3 m curb with jumps, then circles;
 *     B: up a flight of stairs (0.2 m rise) onto a landing, down again fast (ground snap), then
 *        up and down the stairs;
 *     C: into a 60 degree slope (blocked), jumping against it, sliding off, then diagonals;
 *     D: on a kinematic platform that translates (reversing every 150 ticks) and spins, walking
 *        slow circles on it;
 *     E: pushing a 50 kg box, then a 150 kg box that has fallen asleep.
 *   The probe hashes every character's outputs (velocity, grounded, ground normal).
 *
 * SPEC §5.7: de_dmath / de_dvec3 only, characters moved in a fixed order. Built with
 * DE_DETERMINISTIC_FLAGS. Golden: golden/character_course.checksums (regenerate only with
 * test_det_physics --update-golden).
 */
#include "harness.h"

#include <string.h>

#define CC_CHARACTERS 5u

typedef struct cc_state {
    de_body ch[CC_CHARACTERS];
    DeCharacter io[CC_CHARACTERS];
    de_body platform;
    de_body box_light, box_heavy;
} cc_state;

static de_vec3 v3(f32 x, f32 y, f32 z) {
    de_vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static de_quat quat_axis_angle(f32 ax, f32 ay, f32 az, f32 angle) {
    f32 s, c;
    de_dsincos(0.5f * angle, &s, &c);
    de_quat q;
    q.x = ax * s;
    q.y = ay * s;
    q.z = az * s;
    q.w = c;
    return q;
}

static de_transform xform(f64 x, f64 y, f64 z, de_quat q) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position.x = x;
    t.position.y = y;
    t.position.z = z;
    t.rotation = q;
    t.scale = v3(1.0f, 1.0f, 1.0f);
    return t;
}

static de_quat identity(void) {
    return quat_axis_angle(0.0f, 0.0f, 1.0f, 0.0f);
}

static de_body box_body(de_physics_world *p, de_body_type type, f32 hx, f32 hy, f32 hz,
                        de_transform t, f32 mass, u32 flags) {
    de_shape_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.kind = DE_SHAPE_BOX;
    sd.half_extents = v3(hx, hy, hz);
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = de_shape_create(p, &sd);
    d.transform = t;
    d.mass = mass;
    d.material.friction = 0.5f;
    d.gravity_scale = 1.0f;
    d.flags = flags;
    return de_body_create(p, &d);
}

/* A static slab rising toward +x at `degrees` whose top face starts on the ground at foot_x. */
static de_body ramp(de_physics_world *p, f64 foot_x, f64 y, f32 degrees, f32 half_len) {
    const f32 a = degrees * 0.0174532925f;
    f32 s, c;
    de_dsincos(a, &s, &c);
    const f32 t = 0.25f;
    const f64 cx = foot_x + (f64)c * (f64)half_len + (f64)s * (f64)t;
    const f64 cz = (f64)s * (f64)half_len - (f64)c * (f64)t;
    return box_body(p, DE_BODY_STATIC, half_len, 3.0f, t,
                    xform(cx, y, cz, quat_axis_angle(0.0f, 1.0f, 0.0f, -a)), 0.0f, 0u);
}

static DeCharacter params(void) {
    DeCharacter c;
    memset(&c, 0, sizeof c);
    c.radius = 0.3f;
    c.height = 1.8f;
    c.step_height = 0.35f;
    c.max_slope_rad = 0.785398163f;
    c.skin_width = 0.02f;
    c.ground_entity = DE_ENTITY_NULL;
    return c;
}

static de_result cc_setup(de_phys_det_ctx *c) {
    cc_state *st = (cc_state *)c->state;
    de_physics_world *p = c->physics;
    /* Ground (top z = 0). */
    if (!DE_HANDLE_VALID(box_body(p, DE_BODY_STATIC, 60.0f, 60.0f, 0.5f,
                                  xform(0.0, 0.0, -0.5, identity()), 0.0f, 0u)))
        return DE_ERR_INVALID_ARGUMENT;
    /* A: 30 degree ramp at y = 0 (foot x = 4, top 1.5 m at x = 9.2) and a 0.3 m curb. */
    (void)ramp(p, 4.0, 0.0, 30.0f, 3.0f);
    (void)box_body(p, DE_BODY_STATIC, 0.5f, 1.5f, 0.15f, xform(15.0, 0.0, 0.15, identity()), 0.0f,
                   0u);
    /* B: six steps (rise 0.2, run 0.4) from x = 2 at y = 10, then a landing 1.2 m high. */
    for (u32 k = 0; k < 6u; ++k) {
        const f32 top = 0.2f * (f32)(k + 1u);
        (void)box_body(p, DE_BODY_STATIC, 0.2f, 1.5f, 0.5f * top,
                       xform(2.2 + 0.4 * (f64)k, 10.0, 0.5 * (f64)top, identity()), 0.0f, 0u);
    }
    (void)box_body(p, DE_BODY_STATIC, 6.0f, 1.5f, 0.6f, xform(10.4, 10.0, 0.6, identity()), 0.0f,
                   0u);
    /* C: 60 degree slope at y = -10 (foot x = 4). */
    (void)ramp(p, 4.0, -10.0, 60.0f, 3.0f);
    /* D: kinematic platform (top z = 1.25) at (-15, 0). */
    st->platform = box_body(p, DE_BODY_KINEMATIC, 2.5f, 2.5f, 0.25f,
                            xform(-15.0, 0.0, 1.0, identity()), 0.0f, 0u);
    /* E: boxes of 50 kg and 150 kg (1 m cubes) at y = -20. */
    st->box_light = box_body(p, DE_BODY_DYNAMIC, 0.5f, 0.5f, 0.5f,
                             xform(-8.0, -20.0, 0.5, identity()), 50.0f, 0u);
    st->box_heavy =
        box_body(p, DE_BODY_DYNAMIC, 0.5f, 0.5f, 0.5f, xform(-2.0, -22.3, 0.5, identity()), 150.0f,
                 (u32)DE_BODY_FLAG_ALLOW_SLEEP);
    if (!DE_HANDLE_VALID(st->platform) || !DE_HANDLE_VALID(st->box_light) ||
        !DE_HANDLE_VALID(st->box_heavy))
        return DE_ERR_INVALID_ARGUMENT;

    const f64 stand = 0.9 + 0.02;
    const f64 starts[CC_CHARACTERS][3] = {{0.0, 0.0, stand},
                                          {0.0, 10.0, stand},
                                          {0.0, -10.0, stand},
                                          {-14.0, 0.5, 1.25 + stand},
                                          {-11.0, -20.0, stand}};
    for (u32 i = 0; i < CC_CHARACTERS; ++i) {
        st->io[i] = params();
        const de_transform at = xform(starts[i][0], starts[i][1], starts[i][2], identity());
        st->ch[i] = de_character_create(p, &st->io[i], &at, DE_ENTITY_NULL);
        if (!DE_HANDLE_VALID(st->ch[i]))
            return DE_ERR_INVALID_ARGUMENT;
    }
    return DE_OK;
}

/* Walking velocity of `speed` toward `angle` (radians from +x). */
static de_vec3 heading(f32 speed, f32 angle) {
    f32 s, c;
    de_dsincos(angle, &s, &c);
    return v3(speed * c, speed * s, 0.0f);
}

static de_vec3 input_of(u32 who, u64 tick) {
    const u32 t = (u32)tick;
    switch (who) {
    case 0: /* A */
        if (t < 300u)
            return v3(3.0f, 0.0f, 0.0f);
        if (t < 450u)
            return v3(3.0f, 0.0f, t % 90u < 3u ? 5.0f : 0.0f);
        if (t < 600u)
            return v3(0.0f, -3.0f, t % 90u < 3u ? 5.0f : 0.0f);
        return heading(3.0f, 0.01f * (f32)(t - 600u));
    case 1: /* B */
        if (t < 400u)
            return v3(2.0f, 0.0f, 0.0f);
        if (t < 600u)
            return v3(-4.0f, 0.0f, 0.0f);
        return v3((t / 100u) % 2u == 0u ? 3.0f : -3.0f, 0.0f, 0.0f);
    case 2: /* C */
        if (t < 300u)
            return v3(3.0f, 0.0f, 0.0f);
        if (t < 400u)
            return v3(3.0f, 0.0f, t % 40u < 2u ? 6.0f : 0.0f);
        if (t < 800u)
            return heading(2.5f, 0.785398163f * (f32)((t / 100u) % 4u));
        return v3(-2.0f, 0.5f, 0.0f);
    case 3: /* D */
        return heading(0.5f, 0.02f * (f32)t);
    default: /* E */
        if (t < 500u)
            return v3(2.0f, 0.0f, 0.0f);
        if (t < 600u)
            return v3(0.0f, -1.5f, 0.0f);
        return v3(-2.0f, 0.0f, 0.0f);
    }
}

static void cc_script(de_phys_det_ctx *c, u64 tick) {
    cc_state *st = (cc_state *)c->state;
    de_physics_world *p = c->physics;
    if (tick % 150u == 0u) {
        const f32 sign = (tick / 150u) % 2u == 0u ? 1.0f : -1.0f;
        (void)de_body_set_velocity(p, st->platform, v3(0.8f * sign, 0.3f * sign, 0.0f),
                                   v3(0.0f, 0.0f, 0.4f));
    }
    for (u32 i = 0; i < CC_CHARACTERS; ++i) {
        st->io[i].desired_velocity = input_of(i, tick);
        (void)de_character_move(p, st->ch[i], &st->io[i], c->dt, NULL);
    }
}

static u64 mix(u64 h, u32 v) {
    return (h ^ (u64)v) * 0x100000001B3ull;
}

static u32 bits(f32 f) {
    u32 u;
    memcpy(&u, &f, sizeof u);
    return u;
}

/* Every character's outputs. */
static u64 cc_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    const cc_state *st = (const cc_state *)c->state;
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < CC_CHARACTERS; ++i) {
        const DeCharacter *io = &st->io[i];
        h = mix(h, bits(io->velocity.x));
        h = mix(h, bits(io->velocity.y));
        h = mix(h, bits(io->velocity.z));
        h = mix(h, io->grounded ? 1u : 0u);
        h = mix(h, bits(io->ground_normal.x));
        h = mix(h, bits(io->ground_normal.y));
        h = mix(h, bits(io->ground_normal.z));
    }
    return h;
}

const de_phys_det_scenario de_phys_det_scenario_character_course = {
    .name = "character_course",
    .setup = cc_setup,
    .script = cc_script,
    .probe = cc_probe,
    .ticks = 1200u,
    .state_size = (u32)sizeof(cc_state),
    .max_bodies = 64u,
};
DE_PHYS_DET_REGISTER(character_course)
