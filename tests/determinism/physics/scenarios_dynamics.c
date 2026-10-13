/*
 * scenarios_dynamics.c: the final S23 dynamics scenario set (DENG-224; SPEC §13.2, §13.4, §19.4,
 * §5.7). Registered by tests/determinism/physics/CMakeLists.txt. 1,000 ticks at 60 Hz each, run
 * with no task system, 1 and 4 workers (DE_PHYS_DET_REGISTER).
 *
 *   tower_20:       a column of 20 unit cubes of 25 kg (sleeping allowed) that settles and sleeps,
 *                   is knocked down by a 200 kg ball at tick 300, and is hit again by another at
 *                   tick 650; the probe hashes which cubes sleep.
 *   ragdoll_stairs: five 15-bone humanoid ragdolls (tests/unit/physics/ragdoll_test_util.h)
 *                   dropped onto a flight of ten stairs at ticks 0, 40, 80, 120 and 160, with
 *                   random poses and velocities. Ragdoll 2 is powered and switches its motor
 *                   targets every 100 ticks; ragdoll 4 collides with itself. Ragdoll 1 is
 *                   destroyed at tick 600 and built again at tick 610 (index reuse). The probe
 *                   hashes the joints (impulses, broken flags).
 *   chain_bridge:   a bridge of 16 planks on hinges, hung from the world by four rope joints with
 *                   slack, loaded by ten boxes and balls dropped every 40 ticks, then a 900 kg
 *                   block at tick 600 that breaks the middle hinge (break force 25 kN; the
 *                   load before peaks at about 12 kN). The probe
 *                   hashes the joints and the break count.
 *   bullet_spray:   a gun sprays CCD bullets (spheres, capsules, boxes; 150-400 m/s, random spin)
 *                   from a pool of 24, one every 3 ticks until tick 900, at a 1 cm wall, a mesh
 *                   wall and a stack of crates; four bullets are destroyed and created again every
 *                   200 ticks. The probe hashes the CCD counters.
 *   character_walk: a character entity driven through FixedUpdate (physics.step) by the input
 *                   recording data/character_walk.deir (DENG-89 replay, so live input never enters
 *                   the run): up a 20 degree ramp onto a plateau, off its edge, up stairs, onto a
 *                   moving platform (a kinematic entity driven by DeTransform targets), against a
 *                   55 degree slope, pushing a crate, with jumps. The probe hashes the replayed
 *                   tick view (de_input_tick_checksum); the ECS checksum holds DeCharacter and
 *                   DeTransform. Built when the input module is (DE_PHYS_DET_HAS_INPUT).
 *                   character_walk.live_equals_replay records the same session live (events
 *                   injected per tick, recorded after each tick), checks that the recording is
 *                   byte for byte data/character_walk.deir and that the live run's checksums equal
 *                   the replay's. In update mode (--update-golden) it rewrites the .deir: run it
 *                   first when the input script changes, then regenerate the goldens.
 *
 * The other scenarios of the S23 set already exist: pyramid_10 (scenarios_islands.c), stack_10
 * (scenarios_solver.c), chain_20 (scenarios_joints.c), character_course (scenarios_character.c),
 * bullet_wall (scenarios_ccd.c) and stack_far (scenarios_large_world.c).
 *
 * SPEC §5.7: de_dmath / de_dvec3 / de_rng only, one random draw per statement, bodies, ragdolls
 * and entities created and scripted in a fixed order. Built with DE_DETERMINISTIC_FLAGS. Goldens:
 * golden/<name>.checksums (regenerate only with test_det_physics --update-golden).
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#endif
#include "harness.h"

#include "physics/ccd_internal.h"
#include "physics/joints/joint_internal.h"
#include "physics/ragdoll_test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DY_TICKS 1000u
#define DY_PI    3.14159265358979f

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

static de_shape round_shape(de_physics_world *p, de_shape_kind kind, f32 r, f32 half_height) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    d.radius = r;
    d.half_height = half_height;
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

static u64 mix(u64 h, u64 v) {
    return (h ^ v) * 0x100000001B3ull;
}

static u64 joints_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    return mix(de__joints_hash(c->physics), (u64)de_physics_joint_breaks(c->physics, NULL, 0));
}

/* ================================ tower_20 ============================================== */

#define TW_CUBES 20u

typedef struct tw_state {
    de_body cubes[TW_CUBES];
    de_shape ball;
} tw_state;

static de_result tw_setup(de_phys_det_ctx *c) {
    tw_state *st = (tw_state *)c->state;
    de_physics_world *p = c->physics;
    const de_shape ground = box_shape(p, 60.0f, 60.0f, 0.5f);
    const de_shape cube = box_shape(p, 0.5f, 0.5f, 0.5f);
    st->ball = round_shape(p, DE_SHAPE_SPHERE, 0.3f, 0.0f);
    if (!DE_HANDLE_VALID(
            make(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0.7f, 0.0f, 0u)))
        return DE_ERR_INVALID_ARGUMENT;
    for (u32 k = 0; k < TW_CUBES; ++k) {
        st->cubes[k] =
            make(p, DE_BODY_DYNAMIC, cube, xform(0.0, 0.0, 0.5 + 1.001 * (f64)k, quat_identity()),
                 0.6f, 25.0f, (u32)DE_BODY_FLAG_ALLOW_SLEEP);
        if (!DE_HANDLE_VALID(st->cubes[k]))
            return DE_ERR_INVALID_ARGUMENT;
    }
    return DE_OK;
}

static void tw_script(de_phys_det_ctx *c, u64 tick) {
    tw_state *st = (tw_state *)c->state;
    de_physics_world *p = c->physics;
    if (tick == 300u) {
        const de_body b = make(p, DE_BODY_DYNAMIC, st->ball, xform(-6.0, 0.2, 6.2, quat_identity()),
                               0.5f, 200.0f, 0u);
        (void)de_body_set_velocity(p, b, v3(12.0f, 0.0f, 1.0f), v3(0.0f, 0.0f, 0.0f));
    }
    if (tick == 650u) {
        const de_body b = make(p, DE_BODY_DYNAMIC, st->ball, xform(0.3, -7.0, 0.9, quat_identity()),
                               0.5f, 200.0f, 0u);
        (void)de_body_set_velocity(p, b, v3(0.0f, 14.0f, 2.0f), v3(0.0f, 0.0f, 0.0f));
    }
}

static u64 tw_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    const tw_state *st = (const tw_state *)c->state;
    u64 h = 0xC0FFEE20ull;
    for (u32 i = 0; i < TW_CUBES; ++i)
        h = mix(h, de_body_is_sleeping(c->physics, st->cubes[i]) ? 1u : 0u);
    return h;
}

const de_phys_det_scenario de_phys_det_scenario_tower_20 = {
    .name = "tower_20",
    .setup = tw_setup,
    .script = tw_script,
    .probe = tw_probe,
    .ticks = DY_TICKS,
    .state_size = (u32)sizeof(tw_state),
    .max_bodies = 64u,
};
DE_PHYS_DET_REGISTER(tower_20)

/* ================================ ragdoll_stairs ======================================== */

#define RS_RAGDOLLS 5u
#define RS_STEPS    10u

typedef struct rs_state {
    de_rng rng;
    de_ragdoll_bone humanoid[RT_BONES], powered[RT_BONES];
    de_ragdoll ragdolls[RS_RAGDOLLS];
} rs_state;

/* Height of the stair top under x (the flight rises toward +X: run 0.4 m, rise 0.25 m). */
static f64 rs_step_top(f64 x) {
    if (x < 0.0)
        return 0.0;
    const f64 k = x / 0.4;
    u32 n = (u32)k + 1u;
    if (n > RS_STEPS)
        n = RS_STEPS;
    return 0.25 * (f64)n;
}

static de_result rs_drop(de_phys_det_ctx *c, rs_state *st, u32 i, f64 x, f64 y, f64 above) {
    const f32 yaw = rf(&st->rng, -DY_PI, DY_PI);
    const f32 lean = rf(&st->rng, 0.2f, 1.2f);
    const f32 lean_dir = rf(&st->rng, -DY_PI, DY_PI);
    const f32 vx = rf(&st->rng, -2.5f, -0.5f);
    const f32 vy = rf(&st->rng, -0.5f, 0.5f);
    const f32 wx = rf(&st->rng, -2.0f, 2.0f);
    const f32 wy = rf(&st->rng, -2.0f, 2.0f);
    const f32 wz = rf(&st->rng, -1.0f, 1.0f);
    f32 ls, lc;
    de_dsincos(lean_dir, &ls, &lc);
    const de_quat q =
        de__jq_mul(quat_axis_angle(v3(lc, ls, 0.0f), lean), quat_axis_angle(v3(0, 0, 1), yaw));
    de_ragdoll_desc d =
        rt_desc(i == 2u ? st->powered : st->humanoid, xform(x, y, rs_step_top(x) + above, q));
    d.linear_velocity = v3(vx, vy, 0.0f);
    d.angular_velocity = v3(wx, wy, wz);
    d.self_collision = i == 4u;
    d.angular_damping = 0.1f;
    st->ragdolls[i] = de_ragdoll_create(c->physics, &d);
    return DE_HANDLE_VALID(st->ragdolls[i]) ? DE_OK : DE_ERR_INVALID_ARGUMENT;
}

static de_result rs_setup(de_phys_det_ctx *c) {
    rs_state *st = (rs_state *)c->state;
    de_physics_world *p = c->physics;
    st->rng = de_rng_seed(c->seed, 224u);
    rt_humanoid(st->humanoid, 0.0f);
    rt_humanoid(st->powered, 150.0f);
    const de_shape ground = box_shape(p, 20.0f, 20.0f, 0.5f);
    if (!DE_HANDLE_VALID(
            make(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0.7f, 0.0f, 0u)))
        return DE_ERR_INVALID_ARGUMENT;
    for (u32 k = 0; k < RS_STEPS; ++k) {
        const f32 h = 0.125f * (f32)(k + 1u);
        const de_shape s = box_shape(p, 0.2f, 3.0f, h);
        if (!DE_HANDLE_VALID(make(p, DE_BODY_STATIC, s,
                                  xform(0.2 + 0.4 * (f64)k, 0.0, (f64)h, quat_identity()), 0.6f,
                                  0.0f, 0u)))
            return DE_ERR_INVALID_ARGUMENT;
    }
    /* The landing at the top (x 4..7, top 2.5 m). */
    const de_shape landing = box_shape(p, 1.5f, 3.0f, 1.25f);
    if (!DE_HANDLE_VALID(make(p, DE_BODY_STATIC, landing, xform(5.5, 0.0, 1.25, quat_identity()),
                              0.6f, 0.0f, 0u)))
        return DE_ERR_INVALID_ARGUMENT;
    return rs_drop(c, st, 0u, 3.0, -2.2, 1.5);
}

static void rs_script(de_phys_det_ctx *c, u64 tick) {
    rs_state *st = (rs_state *)c->state;
    if (tick > 0u && tick <= 160u && tick % 40u == 0u) {
        const u32 i = (u32)(tick / 40u);
        const f64 x = 2.2 + 0.7 * (f64)i;
        (void)rs_drop(c, st, i, x, -2.2 + 1.1 * (f64)i, 1.5 + 0.2 * (f64)i);
    }
    if (tick == 600u) {
        de_ragdoll_destroy(c->physics, st->ragdolls[1]);
    }
    if (tick == 610u)
        (void)rs_drop(c, st, 1u, 5.5, 0.0, 2.0);
    /* The powered ragdoll curls up and stretches out again every 100 ticks. */
    if (tick % 100u == 50u) {
        de_quat target[RT_BONES];
        const f32 k = (tick / 100u) % 2u == 0u ? 1.0f : 0.0f;
        for (u32 i = 0; i < RT_BONES; ++i)
            target[i] = quat_identity();
        target[RT_CHEST] = quat_axis_angle(v3(1, 0, 0), -0.35f * k);
        target[RT_HEAD] = quat_axis_angle(v3(1, 0, 0), -0.4f * k);
        target[RT_UARM_L] = quat_axis_angle(v3(0, 1, 0), 0.9f * k);
        target[RT_UARM_R] = quat_axis_angle(v3(0, 1, 0), -0.9f * k);
        target[RT_FARM_L] = quat_axis_angle(v3(0, 0, -1), 1.8f * k);
        target[RT_FARM_R] = quat_axis_angle(v3(0, 0, 1), 1.8f * k);
        target[RT_THIGH_L] = quat_axis_angle(v3(1, 0, 0), -1.1f * k);
        target[RT_THIGH_R] = quat_axis_angle(v3(1, 0, 0), -1.1f * k);
        target[RT_SHIN_L] = quat_axis_angle(v3(1, 0, 0), 1.9f * k);
        target[RT_SHIN_R] = quat_axis_angle(v3(1, 0, 0), 1.9f * k);
        (void)de_ragdoll_set_motor_targets(c->physics, st->ragdolls[2], target, RT_BONES);
    }
}

const de_phys_det_scenario de_phys_det_scenario_ragdoll_stairs = {
    .name = "ragdoll_stairs",
    .setup = rs_setup,
    .script = rs_script,
    .probe = joints_probe,
    .ticks = DY_TICKS,
    .state_size = (u32)sizeof(rs_state),
    .max_bodies = 160u,
};
DE_PHYS_DET_REGISTER(ragdoll_stairs)

/* ================================ chain_bridge ========================================== */

#define CB_PLANKS 16u
#define CB_DROPS  10u

typedef struct cb_state {
    de_rng rng;
    de_body planks[CB_PLANKS];
    de_shape cube, ball, block;
} cb_state;

static de_result cb_setup(de_phys_det_ctx *c) {
    cb_state *st = (cb_state *)c->state;
    de_physics_world *p = c->physics;
    st->rng = de_rng_seed(c->seed, 2241u);
    const de_shape ground = box_shape(p, 20.0f, 20.0f, 0.5f);
    const de_shape plank = box_shape(p, 0.3f, 0.75f, 0.05f);
    st->cube = box_shape(p, 0.15f, 0.15f, 0.15f);
    st->ball = round_shape(p, DE_SHAPE_SPHERE, 0.2f, 0.0f);
    st->block = box_shape(p, 0.4f, 0.5f, 0.3f);
    bool ok = DE_HANDLE_VALID(
        make(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0.7f, 0.0f, 0u));
    const de_quat hy = quat_axis_angle(v3(0, 0, 1), 0.5f * DY_PI); /* hinge X -> world Y */
    const de_body none = {0};
    for (u32 i = 0; i < CB_PLANKS; ++i) {
        const f64 x = -4.5 + 0.6 * (f64)i;
        st->planks[i] =
            make(p, DE_BODY_DYNAMIC, plank, xform(x, 0.0, 3.0, quat_identity()), 0.6f, 10.0f, 0u);
        ok = ok && DE_HANDLE_VALID(st->planks[i]);
        if (i == 0u || !ok)
            continue;
        de_joint_desc d;
        memset(&d, 0, sizeof d);
        d.kind = DE_JOINT_HINGE;
        d.body_a = st->planks[i - 1u];
        d.body_b = st->planks[i];
        d.frame_a = xform(0.3, 0.0, 0.0, hy);
        d.frame_b = xform(-0.3, 0.0, 0.0, hy);
        d.enable_limit = true;
        d.limits_min[0] = -0.6f;
        d.limits_max[0] = 0.6f;
        d.break_force = i == CB_PLANKS / 2u ? 25000.0f : 0.0f;
        ok = ok && DE_HANDLE_VALID(de_joint_create(p, &d));
    }
    /* Ropes with slack from both end planks' corners to the world. */
    for (u32 e = 0; ok && e < 2u; ++e) {
        for (u32 side = 0; side < 2u; ++side) {
            const f64 sx = e == 0u ? -1.0 : 1.0;
            const f64 sy = side == 0u ? -0.7 : 0.7;
            de_joint_desc d;
            memset(&d, 0, sizeof d);
            d.kind = DE_JOINT_DISTANCE;
            d.body_a = none;
            d.body_b = st->planks[e == 0u ? 0u : CB_PLANKS - 1u];
            d.frame_a = xform(sx * 5.2, sy, 3.3, quat_identity());
            d.frame_b = xform(sx * 0.3, sy, 0.0, quat_identity());
            d.limits_min[0] = 0.0f;
            d.limits_max[0] = 0.75f;
            ok = ok && DE_HANDLE_VALID(de_joint_create(p, &d));
        }
    }
    return ok ? DE_OK : DE_ERR_INVALID_ARGUMENT;
}

static void cb_script(de_phys_det_ctx *c, u64 tick) {
    cb_state *st = (cb_state *)c->state;
    de_physics_world *p = c->physics;
    if (tick >= 30u && tick < 30u + 40u * CB_DROPS && (tick - 30u) % 40u == 0u) {
        const u32 n = (u32)((tick - 30u) / 40u);
        const f32 x = rf(&st->rng, -3.5f, 3.5f);
        const f32 y = rf(&st->rng, -0.4f, 0.4f);
        const f32 yaw = rf(&st->rng, 0.0f, 3.0f);
        const bool cube = (n % 2u) == 0u;
        (void)make(p, DE_BODY_DYNAMIC, cube ? st->cube : st->ball,
                   xform((f64)x, (f64)y, 5.5, quat_axis_angle(v3(0, 0, 1), yaw)), 0.5f,
                   cube ? 40.0f : 30.0f, 0u);
    }
    if (tick == 600u)
        (void)make(p, DE_BODY_DYNAMIC, st->block, xform(0.2, 0.0, 5.0, quat_identity()), 0.5f,
                   900.0f, 0u);
}

const de_phys_det_scenario de_phys_det_scenario_chain_bridge = {
    .name = "chain_bridge",
    .setup = cb_setup,
    .script = cb_script,
    .probe = joints_probe,
    .ticks = DY_TICKS,
    .state_size = (u32)sizeof(cb_state),
    .max_bodies = 64u,
};
DE_PHYS_DET_REGISTER(chain_bridge)

/* ================================ bullet_spray ========================================== */

#define BS_POOL   24u
#define BS_CRATES 12u

typedef struct bs_state {
    de_rng rng;
    de_shape kinds[3]; /* sphere, capsule, box */
    de_body bullets[BS_POOL];
    u32 next;
} bs_state;

static de_body bs_bullet(de_physics_world *p, bs_state *st, u32 i) {
    return make(p, DE_BODY_DYNAMIC, st->kinds[i % 3u],
                xform(-10.0 - (f64)i, -20.0, 0.5, quat_identity()), 0.4f, 0.05f,
                (u32)DE_BODY_FLAG_CCD);
}

static de_result bs_setup(de_phys_det_ctx *c) {
    bs_state *st = (bs_state *)c->state;
    de_physics_world *p = c->physics;
    st->rng = de_rng_seed(c->seed, 2242u);
    const de_shape ground = box_shape(p, 30.0f, 30.0f, 0.5f);
    const de_shape wall = box_shape(p, 0.005f, 4.0f, 2.5f);
    bool ok = DE_HANDLE_VALID(
        make(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0.6f, 0.0f, 0u));
    ok = ok && DE_HANDLE_VALID(make(p, DE_BODY_STATIC, wall,
                                    xform(14.0, -5.0, 2.5, quat_identity()), 0.4f, 0.0f, 0u));
    /* A two-triangle mesh wall facing -X at x = 16, y 0..8. */
    const de_vec3 verts[4] = {v3(16.0f, 0.0f, 0.0f), v3(16.0f, 0.0f, 5.0f), v3(16.0f, 8.0f, 5.0f),
                              v3(16.0f, 8.0f, 0.0f)};
    const u32 idx[6] = {0, 1, 2, 0, 2, 3};
    de_shape_desc md;
    memset(&md, 0, sizeof md);
    md.kind = DE_SHAPE_MESH;
    md.vertices = verts;
    md.vertex_count = 4;
    md.indices = idx;
    md.index_count = 6;
    const de_shape mesh = de_shape_create(p, &md);
    ok = ok && DE_HANDLE_VALID(mesh) &&
         DE_HANDLE_VALID(
             make(p, DE_BODY_STATIC, mesh, xform(0, 0, 0, quat_identity()), 0.4f, 0.0f, 0u));
    /* A wall of crates (3 wide, 4 high) at x = 9, y = 8 (no CCD; struck by bullets). */
    const de_shape crate = box_shape(p, 0.3f, 0.3f, 0.3f);
    for (u32 k = 0; ok && k < BS_CRATES; ++k) {
        const f64 y = 7.4 + 0.62 * (f64)(k % 3u);
        const f64 z = 0.3 + 0.6 * (f64)(k / 3u);
        ok = DE_HANDLE_VALID(make(p, DE_BODY_DYNAMIC, crate, xform(9.0, y, z, quat_identity()),
                                  0.6f, 20.0f, (u32)DE_BODY_FLAG_ALLOW_SLEEP));
    }
    st->kinds[0] = round_shape(p, DE_SHAPE_SPHERE, 0.012f, 0.0f);
    st->kinds[1] = round_shape(p, DE_SHAPE_CAPSULE, 0.008f, 0.025f);
    st->kinds[2] = box_shape(p, 0.02f, 0.01f, 0.012f);
    for (u32 i = 0; ok && i < BS_POOL; ++i) {
        st->bullets[i] = bs_bullet(p, st, i);
        ok = DE_HANDLE_VALID(st->bullets[i]);
    }
    return ok ? DE_OK : DE_ERR_INVALID_ARGUMENT;
}

static void bs_fire(de_physics_world *p, bs_state *st, de_body b) {
    const f32 yaw = rf(&st->rng, -0.6f, 0.75f);
    const f32 pitch = rf(&st->rng, -0.04f, 0.2f);
    const f32 speed = rf(&st->rng, 150.0f, 400.0f);
    const f32 ax = rf(&st->rng, -1.0f, 1.0f);
    const f32 ay = rf(&st->rng, -1.0f, 1.0f);
    const f32 angle = rf(&st->rng, 0.0f, 6.28f);
    const f32 wx = rf(&st->rng, -60.0f, 60.0f);
    const f32 wy = rf(&st->rng, -60.0f, 60.0f);
    const f32 wz = rf(&st->rng, -60.0f, 60.0f);
    const de_transform at = xform(0.0, 0.0, 1.5, quat_axis_angle(v3(ax, ay, 1.0f), angle));
    (void)de_body_set_transform(p, b, &at);
    f32 sy, cy, sp, cp;
    de_dsincos(yaw, &sy, &cy);
    de_dsincos(pitch, &sp, &cp);
    (void)de_body_set_velocity(p, b, v3(speed * cy * cp, speed * sy * cp, speed * sp),
                               v3(wx, wy, wz));
}

static void bs_script(de_phys_det_ctx *c, u64 tick) {
    bs_state *st = (bs_state *)c->state;
    de_physics_world *p = c->physics;
    if (tick > 0u && tick % 200u == 0u) {
        /* Index churn: four bullets are destroyed and made again (LIFO slots). */
        for (u32 k = 0; k < 4u; ++k)
            de_body_destroy(p, st->bullets[(st->next + 7u * k) % BS_POOL]);
        for (u32 k = 0; k < 4u; ++k) {
            const u32 i = (st->next + 7u * k) % BS_POOL;
            st->bullets[i] = bs_bullet(p, st, i);
        }
    }
    if (tick < 900u && tick % 3u == 0u) {
        bs_fire(p, st, st->bullets[st->next]);
        st->next = (st->next + 1u) % BS_POOL;
    }
}

static u64 bs_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    de__ccd_stats cs;
    de__ccd_stats_get(c->physics, &cs);
    u64 h = 0x9E3779B97F4A7C15ull;
    h = mix(h, cs.bullets);
    h = mix(h, cs.hits);
    h = mix(h, cs.fallbacks);
    h = mix(h, cs.predicted);
    return h;
}

const de_phys_det_scenario de_phys_det_scenario_bullet_spray = {
    .name = "bullet_spray",
    .setup = bs_setup,
    .script = bs_script,
    .probe = bs_probe,
    .ticks = DY_TICKS,
    .state_size = (u32)sizeof(bs_state),
    .max_bodies = 64u,
};
DE_PHYS_DET_REGISTER(bullet_spray)

/* ================================ character_walk ======================================== */

#if defined(DE_PHYS_DET_HAS_INPUT)

#include "input/input_internal.h"
#include "input/record_format.h"

#include <dengine/de_input.h>

#ifndef DE_PHYS_DET_DATA_DIR
#error "DE_PHYS_DET_DATA_DIR must be defined by tests/determinism/physics/CMakeLists.txt"
#endif

#define CW_ROOT       "detphysdata"
#define CW_DEIR       "character_walk.deir"
#define CW_FIRST_TICK 1000u /* recording tick of scenario tick 0 */
#define CW_PAD        5u
#define CW_TICK_RATE  60u

typedef struct cw_state {
    bool live;
    u32 failures;
    de_input *in;
    de_input_recording *rec;
    de_action move, jump, run;
    de_component ch, xf;
    de_entity character, platform;
    f32 pad_x, pad_y; /* live: the stick values sent so far */
} cw_state;

static de_binding cw_bind(de_binding_source src, u32 code, i8 slot) {
    de_binding b;
    memset(&b, 0, sizeof b);
    b.source = src;
    b.code = code;
    b.composite_slot = slot;
    return b;
}

static de_action cw_action(de_input *in, de_action_map m, const char *name, de_action_type type,
                           const de_binding *b, u32 n) {
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = type;
    d.interaction = DE_INTERACT_PRESS;
    d.bindings = b;
    d.binding_count = n;
    return de_action_add(in, m, &d);
}

/* The schema: map "walk" with move (WASD, left stick), jump (space, pad south), run (left shift,
 * pad west). */
static bool cw_schema(cw_state *st) {
    const de_action_map m = de_action_map_create(st->in, "walk");
    if (!DE_HANDLE_VALID(m))
        return false;
    const i8 none = DE_COMPOSITE_NONE;
    de_binding move[5] = {cw_bind(DE_BIND_KEY, DE_KEY_W, DE_COMPOSITE_UP),
                          cw_bind(DE_BIND_KEY, DE_KEY_S, DE_COMPOSITE_DOWN),
                          cw_bind(DE_BIND_KEY, DE_KEY_A, DE_COMPOSITE_LEFT),
                          cw_bind(DE_BIND_KEY, DE_KEY_D, DE_COMPOSITE_RIGHT),
                          cw_bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, none)};
    move[4].deadzone = 0.15f;
    const de_binding jump[2] = {cw_bind(DE_BIND_KEY, DE_KEY_SPACE, none),
                                cw_bind(DE_BIND_PAD_BUTTON, DE_PAD_SOUTH, none)};
    const de_binding run[2] = {cw_bind(DE_BIND_KEY, DE_KEY_LSHIFT, none),
                               cw_bind(DE_BIND_PAD_BUTTON, DE_PAD_WEST, none)};
    st->move = cw_action(st->in, m, "move", DE_ACTION_AXIS2D, move, 5);
    st->jump = cw_action(st->in, m, "jump", DE_ACTION_BUTTON, jump, 2);
    st->run = cw_action(st->in, m, "run", DE_ACTION_BUTTON, run, 2);
    if (!DE_HANDLE_VALID(st->move) || !DE_HANDLE_VALID(st->jump) || !DE_HANDLE_VALID(st->run))
        return false;
    de_action_map_push(st->in, m);
    return true;
}

/* FixedUpdate system "det.character_walk.input" (before physics.step): the tick view of the
 * actions becomes the character's desired velocity. */
static void cw_input_chunk(const de_query_chunk *chunk, void *user) {
    const cw_state *st = (const cw_state *)user;
    const de_input *in = st->in;
    DeCharacter *ch = (DeCharacter *)chunk->columns[0];
    const de_vec2 mv = de_action_value2(in, st->move);
    const f32 speed = de_action_held(in, st->run) ? 5.0f : 2.5f;
    const f32 jump = de_action_triggered(in, st->jump) ? 5.0f : 0.0f;
    for (u32 i = 0; i < chunk->count; ++i)
        ch[i].desired_velocity = v3(mv.x * speed, mv.y * speed, jump);
}

static void cw_input_system(de_system_ctx *ctx) {
    de_query_each(ctx->world, ctx->queries[0], cw_input_chunk, ctx->user);
}

static de_entity cw_spawn(de_world *w, de_component xf, de_component gx, de_dvec3 pos, de_quat q) {
    const de_component comps[2] = {xf, gx};
    de_entity e = DE_ENTITY_NULL;
    if (de_entity_spawn_with(w, comps, 2, &e) != DE_OK)
        return DE_ENTITY_NULL;
    de_transform *t = (de_transform *)de_entity_get(w, e, xf);
    t->position = pos;
    t->rotation = q;
    return e;
}

static void cw_collider(de_world *w, de_component col, de_entity e, de_shape shape, f32 friction) {
    DeCollider *c = (DeCollider *)de_entity_add(w, e, col);
    c->shape = shape;
    c->offset.rotation.w = 1.0f;
    c->offset.scale = v3(1.0f, 1.0f, 1.0f);
    c->material.friction = friction;
}

static void cw_static_box(de_phys_det_ctx *c, const cw_state *st, de_component col, de_component gx,
                          f32 hx, f32 hy, f32 hz, de_dvec3 pos, de_quat q) {
    const de_entity e = cw_spawn(c->world, st->xf, gx, pos, q);
    cw_collider(c->world, col, e, box_shape(c->physics, hx, hy, hz), 0.6f);
}

static void cw_mount(void) {
    static bool mounted;
    if (!mounted)
        mounted = de_fs_mount(CW_ROOT, DE_PHYS_DET_DATA_DIR, DE_MOUNT_READ_ONLY, 0) == DE_OK;
}

static de_result cw_setup(de_phys_det_ctx *c, bool live) {
    cw_state *st = (cw_state *)c->state;
    de_world *w = c->world;
    st->live = live;
    const de_component col = de_component_find(w, "DeCollider");
    const de_component rb = de_component_find(w, "DeRigidBody");
    st->ch = de_component_find(w, "DeCharacter");
    const de_builtin_component_ids bi = de_builtin_components(w);
    st->xf = bi.transform;
    const de_component gx = bi.global_transform;

    /* Ground; a 20 degree ramp rising toward +X onto a plateau (top 1.82 m, x 7..10); stairs of
     * eight 0.2 m steps rising toward +Y at x 9..12 onto a landing (top 1.6 m); a 55 degree slope
     * facing +Y; a crate to push. */
    cw_static_box(c, st, col, gx, 30.0f, 30.0f, 0.5f, d3(0, 0, -0.5), quat_identity());
    {
        const f32 a = 20.0f * DY_PI / 180.0f;
        f32 s, co;
        de_dsincos(a, &s, &co);
        const f32 half = 2.661f; /* 5 m of run */
        const f64 cx = 2.0 + (f64)(co * half) + (f64)(s * 0.25f);
        const f64 cz = (f64)(s * half) - (f64)(co * 0.25f);
        cw_static_box(c, st, col, gx, half, 1.5f, 0.25f, d3(cx, 0.0, cz),
                      quat_axis_angle(v3(0, 1, 0), -a));
    }
    cw_static_box(c, st, col, gx, 1.5f, 1.5f, 0.91f, d3(8.5, 0.0, 0.91), quat_identity());
    for (u32 k = 0; k < 8u; ++k) {
        const f32 top = 0.2f * (f32)(k + 1u);
        cw_static_box(c, st, col, gx, 1.5f, 0.175f, 0.5f * top,
                      d3(10.5, 2.175 + 0.35 * (f64)k, 0.5 * (f64)top), quat_identity());
    }
    cw_static_box(c, st, col, gx, 1.5f, 1.6f, 0.8f, d3(10.5, 6.2, 0.8), quat_identity());
    cw_static_box(c, st, col, gx, 6.0f, 2.0f, 0.3f, d3(-2.0, 1.5, 0.8),
                  quat_axis_angle(v3(1, 0, 0), -55.0f * DY_PI / 180.0f));
    {
        const de_entity crate = cw_spawn(w, st->xf, gx, d3(6.0, 6.6, 0.4), quat_identity());
        DeRigidBody *r = (DeRigidBody *)de_entity_add(w, crate, rb);
        r->type = DE_BODY_DYNAMIC;
        r->mass = 40.0f;
        cw_collider(w, col, crate, box_shape(c->physics, 0.4f, 0.4f, 0.4f), 0.5f);
    }
    /* The moving platform (kinematic, top 0.3 m), driven by DeTransform targets. */
    st->platform = cw_spawn(w, st->xf, gx, d3(-3.0, 6.6, 0.15), quat_identity());
    {
        DeRigidBody *r = (DeRigidBody *)de_entity_add(w, st->platform, rb);
        r->type = DE_BODY_KINEMATIC;
        cw_collider(w, col, st->platform, box_shape(c->physics, 1.5f, 1.2f, 0.15f), 0.8f);
    }
    /* The character. */
    st->character = cw_spawn(w, st->xf, gx, d3(0.0, 0.0, 0.92), quat_identity());
    DeCharacter *ch = (DeCharacter *)de_entity_add(w, st->character, st->ch);
    ch->radius = 0.3f;
    ch->height = 1.8f;
    ch->step_height = 0.35f;

    /* Input: the schema, attached to the world, and the system that reads it. */
    de_input_desc id;
    memset(&id, 0, sizeof id);
    de_result r = de_input_create(&id, &st->in);
    if (r != DE_OK)
        return r;
    if (!cw_schema(st))
        return DE_FAIL(DE_ERR_INTERNAL, "character_walk: input schema");
    r = de_input_attach(st->in, w);
    if (r != DE_OK)
        return r;
    const de_query_term terms[1] = {{st->ch, DE_WRITE}};
    const de_query_desc q = {terms, 1, NULL, 0};
    de_resource_access acc;
    acc.id = st->in->resource;
    acc.access = DE_READ;
    const char *before[1] = {DE_PHYSICS_STEP_SYSTEM};
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "det.character_walk.input";
    sd.schedule = DE_SCHEDULE_FIXED_UPDATE;
    sd.flags = DE_SYSTEM_DETERMINISTIC;
    sd.fn = cw_input_system;
    sd.user = st;
    sd.queries = &q;
    sd.query_count = 1;
    sd.resources = &acc;
    sd.resource_count = 1;
    sd.before = before;
    sd.before_count = 1;
    if (!DE_HANDLE_VALID(de_system_add(w, &sd)))
        return DE_FAIL_WRAP(DE_ERR_INVALID_ARGUMENT, "character_walk: input system");

    if (live) {
        de_input_record_desc rd;
        memset(&rd, 0, sizeof rd);
        rd.tick_rate = CW_TICK_RATE;
        rd.reserve_ticks = DY_TICKS;
        return de_input_record_begin(st->in, &rd, &st->rec);
    }
    cw_mount();
    return de_input_replay_load(st->in, CW_ROOT ":/" CW_DEIR, &st->rec);
}

static de_result cw_setup_replay(de_phys_det_ctx *c) {
    return cw_setup(c, false);
}

static de_result cw_setup_live(de_phys_det_ctx *c) {
    return cw_setup(c, true);
}

/* ---- The scripted session (live mode): which keys are down and the stick in each tick ---- */

typedef struct cw_frame {
    bool w, a, s, d, space, shift, south, west;
    f32 stick_x, stick_y; /* exact thousandths */
} cw_frame;

static cw_frame cw_frame_at(u32 t) {
    cw_frame f;
    memset(&f, 0, sizeof f);
    if (t < 200u) { /* up the ramp onto the plateau */
        f.d = true;
    } else if (t < 260u) { /* off the plateau's edge with a jump */
        f.d = true;
        f.space = t >= 228u && t < 232u;
    } else if (t < 400u) { /* up the stairs onto the landing */
        f.w = true;
    } else if (t < 420u) {
        /* stand */
    } else if (t < 520u) { /* off the landing toward -X, running (pad), into the crate */
        f.stick_x = -1.0f;
        f.west = true;
    } else if (t < 700u) { /* slowly onto the moving platform, a jump on it */
        f.stick_x = -0.5f;
        f.south = t >= 600u && t < 603u;
    } else if (t < 850u) { /* toward -Y (raw pad y is down), into the steep slope, jumping */
        f.stick_y = 0.9f;
        f.south = t % 50u < 2u;
    } else { /* diagonals on the keyboard, a jump */
        f.a = true;
        f.s = (t / 40u) % 2u == 0u;
        f.space = t == 930u;
    }
    return f;
}

static de__input_event cw_event(de_raw_input_kind kind, u32 frame, u32 *seq) {
    de__input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.time_ns = 1000000000ull + (u64)frame * 16666667ull + (*seq)++;
    return ev;
}

static void cw_key(de_input *in, u32 frame, u32 *seq, de_key k, bool now, bool before) {
    if (now == before)
        return;
    de__input_event ev = cw_event(DE_RAW_KEY, frame, seq);
    ev.key.code = (u16)k;
    ev.key.down = now;
    de__input_inject(in, &ev);
}

static void cw_button(de_input *in, u32 frame, u32 *seq, de_pad_button b, bool now, bool before) {
    if (now == before)
        return;
    de__input_event ev = cw_event(DE_RAW_PAD_BUTTON, frame, seq);
    ev.device = CW_PAD;
    ev.pad_button.button = (u8)b;
    ev.pad_button.down = now;
    de__input_inject(in, &ev);
}

static void cw_axis(cw_state *st, u32 frame, u32 *seq, de_pad_axis a, f32 value, f32 *sent) {
    if (value == *sent)
        return;
    de__input_event ev = cw_event(DE_RAW_PAD_AXIS, frame, seq);
    ev.device = CW_PAD;
    ev.pad_axis.axis = (u8)a;
    ev.pad_axis.value = value;
    de__input_inject(st->in, &ev);
    *sent = value;
}

/* Live mode: queues the events of frame `t` (one frame per tick), then starts the frame. */
static void cw_live_frame(cw_state *st, u32 t) {
    de_input *in = st->in;
    u32 seq = 0;
    if (t == 0u) {
        de_platform_pad_info info;
        memset(&info, 0, sizeof info);
        info.id = CW_PAD;
        info.type = DE_PLATFORM_PAD_XBOX;
        info.player_index = -1;
        memcpy(info.name, "walk pad", 9);
        de__input_inject_pad_info(in, &info);
        de__input_event ev = cw_event(DE_RAW_PAD_ADDED, t, &seq);
        ev.device = CW_PAD;
        de__input_inject(in, &ev);
    }
    const cw_frame f = cw_frame_at(t);
    cw_frame b;
    if (t == 0u)
        memset(&b, 0, sizeof b);
    else
        b = cw_frame_at(t - 1u);
    cw_key(in, t, &seq, DE_KEY_W, f.w, b.w);
    cw_key(in, t, &seq, DE_KEY_A, f.a, b.a);
    cw_key(in, t, &seq, DE_KEY_S, f.s, b.s);
    cw_key(in, t, &seq, DE_KEY_D, f.d, b.d);
    cw_key(in, t, &seq, DE_KEY_SPACE, f.space, b.space);
    cw_key(in, t, &seq, DE_KEY_LSHIFT, f.shift, b.shift);
    cw_button(in, t, &seq, DE_PAD_SOUTH, f.south, b.south);
    cw_button(in, t, &seq, DE_PAD_WEST, f.west, b.west);
    cw_axis(st, t, &seq, DE_AXIS_LEFT_X, f.stick_x, &st->pad_x);
    cw_axis(st, t, &seq, DE_AXIS_LEFT_Y, f.stick_y, &st->pad_y);
    de__input_begin_frame_dt(in, 1.0f / (f32)CW_TICK_RATE);
    de__actions_frame_update(in, 1.0f / (f32)CW_TICK_RATE);
}

static void cw_script(de_phys_det_ctx *c, u64 tick) {
    cw_state *st = (cw_state *)c->state;
    if (st->live)
        cw_live_frame(st, (u32)tick);
    else if (!de_input_replay_tick(st->in, st->rec, CW_FIRST_TICK + tick))
        st->failures++;
    /* The platform shuttles along X (x -6..0) through DeTransform targets. */
    f32 s, co;
    de_dsincos((f32)tick * c->dt * 0.8f, &s, &co);
    de_transform *t = (de_transform *)de_entity_get(c->world, st->platform, st->xf);
    t->position = d3(-3.0 + 3.0 * (f64)s, 6.6, 0.15);
}

/* The replayed (or live, quantized) tick view and the replay failures. */
static u64 cw_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    const cw_state *st = (const cw_state *)c->state;
    return mix(de_input_tick_checksum(st->in), st->failures);
}

static void cw_teardown(de_phys_det_ctx *c) {
    cw_state *st = (cw_state *)c->state;
    de_input_recording_destroy(st->rec);
    st->rec = NULL;
    if (st->in != NULL) {
        de_input_detach(st->in);
        de_input_destroy(st->in);
        st->in = NULL;
    }
}

const de_phys_det_scenario de_phys_det_scenario_character_walk = {
    .name = "character_walk",
    .setup = cw_setup_replay,
    .script = cw_script,
    .probe = cw_probe,
    .teardown = cw_teardown,
    .ticks = DY_TICKS,
    .state_size = (u32)sizeof(cw_state),
    .max_bodies = 64u,
};
DE_PHYS_DET_REGISTER(character_walk)

static const de_phys_det_scenario k_character_walk_live = {
    .name = "character_walk",
    .setup = cw_setup_live,
    .script = cw_script,
    .probe = cw_probe,
    .teardown = cw_teardown,
    .ticks = DY_TICKS,
    .state_size = (u32)sizeof(cw_state),
    .max_bodies = 64u,
};

/* Records the session live through physics.step and checks it against data/character_walk.deir
 * and against the replay (see the file comment). */
TEST(character_walk, live_equals_replay) {
    static u64 live[DY_TICKS], replay[DY_TICKS];
    static de_phys_det_session s;
    memset(&s, 0, sizeof s);
    const de_phys_det_options none = {0};
    ASSERT_EQ(de_phys_det_begin(&s, &k_character_walk_live, &none), DE_OK);
    cw_state *st = (cw_state *)s.ctx.state;
    u32 record_failures = 0;
    for (u32 t = 0; t < DY_TICKS; ++t) {
        ASSERT_EQ(de_phys_det_step(&s, &live[t], NULL), DE_OK);
        if (de_input_record_tick(st->in, st->rec, CW_FIRST_TICK + t) != DE_OK)
            ++record_failures;
    }
    EXPECT_EQ(record_failures, 0u);
    EXPECT_EQ(de_input_recording_tick_count(st->rec), DY_TICKS);
    const u64 size = de__recording_file_size(st->rec);
    u8 *bytes = (u8 *)malloc((size_t)size);
    ASSERT_TRUE(bytes != NULL);
    de__recording_write_file(st->rec, bytes);
    de_phys_det_end(&s);

    const char *path = DE_PHYS_DET_DATA_DIR "/" CW_DEIR;
    if (de_phys_det_update_mode()) {
        FILE *f = fopen(path, "wb");
        ASSERT_TRUE(f != NULL);
        EXPECT_EQ(fwrite(bytes, 1, (size_t)size, f), (size_t)size);
        fclose(f);
        fprintf(stderr, "determinism: rewrote %s (%llu bytes)\n", path, (unsigned long long)size);
    } else {
        FILE *f = fopen(path, "rb");
        ASSERT_TRUE(f != NULL);
        u8 *golden = (u8 *)malloc((size_t)size + 1u);
        ASSERT_TRUE(golden != NULL);
        const size_t n = fread(golden, 1, (size_t)size + 1u, f);
        fclose(f);
        EXPECT_EQ(n, (size_t)size);
        if (n == (size_t)size)
            EXPECT_MEMEQ(golden, bytes, (size_t)size);
        free(golden);
    }
    free(bytes);

    ASSERT_EQ(de_phys_det_run(&de_phys_det_scenario_character_walk, &none, replay, NULL), DE_OK);
    EXPECT_EQ(de_det_report("character_walk live vs replay", live, replay, DY_TICKS),
              DE_DET_NO_DIVERGENCE);
}

#endif /* DE_PHYS_DET_HAS_INPUT */
