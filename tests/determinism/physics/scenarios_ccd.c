/*
 * scenarios_ccd.c: determinism scenarios of continuous collision (DENG-219; SPEC §13.1, §13.2,
 * §19.4). Registered by tests/determinism/physics/CMakeLists.txt.
 *
 *   bullet_wall (600 ticks): bullets (DE_BODY_FLAG_CCD: spheres, boxes, capsules, a compound)
 *                            fired at 150-380 m/s with spin, every 40 ticks, at a 1 cm static
 *                            wall, a two-triangle mesh wall and a heavy dynamic block; the time
 *                            of impact, the start-touching fallback and the speculative stop.
 *                            Two plain pellets at 300 m/s are fired with them (no CCD: they
 *                            tunnel or stop on speculation, the same way on every platform).
 *                            The probe hashes the CCD counters (bullets, hits, fallbacks).
 *   fast_drop (600 ticks):   boxes, spheres, capsules and hulls (no CCD) dropped and thrown at
 *                            30-60 m/s onto a 20 cm slab, a heightfield and a mesh ramp, and two
 *                            boxes meeting head-on at 40 m/s each; re-launched every 120 ticks.
 *                            Predictive broadphase boxes, the per-pair speculative distance and
 *                            the ghost check of far speculative points.
 *
 * SPEC §5.7: de_dmath / de_dvec3 / de_rng only, one random draw per statement, bodies created
 * and scripted in a fixed order. Built with DE_DETERMINISTIC_FLAGS. Goldens:
 * golden/<name>.checksums (regenerate only with test_det_physics --update-golden).
 */
#include "harness.h"

#include "physics/ccd_internal.h"

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
                    f32 friction, f32 restitution, f32 mass, u32 flags) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.shape = s;
    d.transform = t;
    d.mass = mass;
    d.material.friction = friction;
    d.material.restitution = restitution;
    d.gravity_scale = 1.0f;
    d.flags = flags;
    return de_body_create(p, &d);
}

/* A two-triangle quad (counter-clockwise seen from `front`). */
static de_shape quad_mesh(de_physics_world *p, const de_vec3 v[4]) {
    const u32 idx[6] = {0, 1, 2, 0, 2, 3};
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_MESH;
    d.vertices = v;
    d.vertex_count = 4;
    d.indices = idx;
    d.index_count = 6;
    return de_shape_create(p, &d);
}

/* ================================ bullet_wall =========================================== */

#define BW_BULLETS 8u
#define BW_PELLETS 2u

typedef struct bw_state {
    de_shape kinds[4]; /* sphere, box, capsule, compound */
    de_shape pellet;
    de_body bullets[BW_BULLETS];
    de_body pellets[BW_PELLETS];
    de_rng rng;
} bw_state;

static de_result bw_setup(de_phys_det_ctx *c) {
    bw_state *st = (bw_state *)c->state;
    de_physics_world *p = c->physics;
    st->rng = de_rng_seed(c->seed, 219);
    const de_shape ground = box_shape(p, 30.0f, 30.0f, 0.5f);
    const de_shape wall = box_shape(p, 0.005f, 6.0f, 4.0f);
    if (!DE_HANDLE_VALID(make(p, DE_BODY_STATIC, ground, xform(0, 0, -0.5, quat_identity()), 0.4f,
                              0.0f, 0.0f, 0u)) ||
        !DE_HANDLE_VALID(make(p, DE_BODY_STATIC, wall, xform(10.0, -4.0, 4.0, quat_identity()),
                              0.3f, 0.0f, 0.0f, 0u)))
        return DE_ERR_INVALID_ARGUMENT;
    /* A mesh wall in the plane x = 12 (front toward -X), beside the box wall. */
    const de_vec3 quad[4] = {v3(12.0f, 0.0f, 0.0f), v3(12.0f, 0.0f, 8.0f), v3(12.0f, 8.0f, 8.0f),
                             v3(12.0f, 8.0f, 0.0f)};
    const de_shape mesh = quad_mesh(p, quad);
    if (!DE_HANDLE_VALID(
            make(p, DE_BODY_STATIC, mesh, xform(0, 0, 0, quat_identity()), 0.3f, 0.0f, 0.0f, 0u)))
        return DE_ERR_INVALID_ARGUMENT;
    /* A heavy dynamic block in front of the box wall (a non-bullet dynamic target). */
    const de_shape block = box_shape(p, 0.5f, 1.0f, 1.0f);
    if (!DE_HANDLE_VALID(make(p, DE_BODY_DYNAMIC, block, xform(6.0, -6.5, 1.0, quat_identity()),
                              0.6f, 0.0f, 500.0f, 0u)))
        return DE_ERR_INVALID_ARGUMENT;
    st->kinds[0] = round_shape(p, DE_SHAPE_SPHERE, 0.01f, 0.0f);
    st->kinds[1] = box_shape(p, 0.02f, 0.008f, 0.012f);
    st->kinds[2] = round_shape(p, DE_SHAPE_CAPSULE, 0.008f, 0.03f);
    const de_shape pellet = round_shape(p, DE_SHAPE_SPHERE, 0.006f, 0.0f);
    const de_shape kids[2] = {pellet, pellet};
    const de_transform kid_xf[2] = {xform(0.0, 0.015, 0.0, quat_identity()),
                                    xform(0.0, -0.015, 0.0, quat_identity())};
    de_shape_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.kind = DE_SHAPE_COMPOUND;
    cd.children = kids;
    cd.child_xforms = kid_xf;
    cd.child_count = 2;
    st->kinds[3] = de_shape_create(p, &cd);
    st->pellet = round_shape(p, DE_SHAPE_SPHERE, 0.01f, 0.0f);
    for (u32 k = 0; k < 4; ++k)
        if (!DE_HANDLE_VALID(st->kinds[k]))
            return DE_ERR_INVALID_ARGUMENT;
    for (u32 i = 0; i < BW_BULLETS; ++i) {
        st->bullets[i] = make(p, DE_BODY_DYNAMIC, st->kinds[i % 4u],
                              xform(-20.0 - (f64)i, 20.0, 1.0, quat_identity()), 0.3f,
                              0.1f * (f32)(i % 3u), 0.0f, DE_BODY_FLAG_CCD);
        if (!DE_HANDLE_VALID(st->bullets[i]))
            return DE_ERR_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < BW_PELLETS; ++i) {
        st->pellets[i] =
            make(p, DE_BODY_DYNAMIC, st->pellet, xform(-20.0 - (f64)i, 22.0, 1.0, quat_identity()),
                 0.3f, 0.0f, 0.0f, 0u);
        if (!DE_HANDLE_VALID(st->pellets[i]))
            return DE_ERR_INVALID_ARGUMENT;
    }
    return DE_OK;
}

/* Fires body b from about x = -2 toward the walls (or the block) at `speed`, with spin. */
static void bw_fire(de_physics_world *p, de_rng *r, de_body b, f32 speed) {
    const f32 aim = rf(r, 0.0f, 3.0f);
    const f64 y = aim < 1.0f   ? -6.5 + rf(r, -0.7f, 0.7f) /* the block */
                  : aim < 2.0f ? -4.0 + rf(r, -4.0f, 4.0f) /* the box wall */
                               : 4.0 + rf(r, -3.0f, 3.0f); /* the mesh wall */
    const f64 z = 0.6 + rf(r, 0.0f, 2.5f);
    const f32 ax = rf(r, -1.0f, 1.0f);
    const f32 ay = rf(r, -1.0f, 1.0f);
    const f32 angle = rf(r, 0.0f, 6.28f);
    const f64 x = -2.0 - (f64)rf(r, 0.0f, 3.0f);
    const de_transform at = xform(x, y, z, quat_axis_angle(v3(ax, ay, 1.0f), angle));
    (void)de_body_set_transform(p, b, &at);
    const f32 yaw = rf(r, -0.5f, 0.5f);
    const f32 pitch = rf(r, -0.3f, 0.3f);
    f32 sy, cy, sp, cp;
    de_dsincos(yaw, &sy, &cy);
    de_dsincos(pitch, &sp, &cp);
    const de_vec3 v = v3(speed * cy * cp, speed * sy * cp, speed * sp);
    const f32 wx = rf(r, -40.0f, 40.0f);
    const f32 wy = rf(r, -40.0f, 40.0f);
    const f32 wz = rf(r, -40.0f, 40.0f);
    (void)de_body_set_velocity(p, b, v, v3(wx, wy, wz));
}

static void bw_script(de_phys_det_ctx *c, u64 tick) {
    bw_state *st = (bw_state *)c->state;
    if (tick % 40u != 0u)
        return;
    for (u32 i = 0; i < BW_BULLETS; ++i) {
        const f32 speed = rf(&st->rng, 150.0f, 380.0f);
        bw_fire(c->physics, &st->rng, st->bullets[i], speed);
    }
    for (u32 i = 0; i < BW_PELLETS; ++i)
        bw_fire(c->physics, &st->rng, st->pellets[i], 300.0f);
}

/* The CCD counters that do not depend on the broadphase visiting order: bullets swept, bullets
 * moved back to their time of impact, start-touching fallbacks, predicted bodies. */
static u64 ccd_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    de__ccd_stats cs;
    de__ccd_stats_get(c->physics, &cs);
    u64 h = 0x9E3779B97F4A7C15ull;
    const u32 v[4] = {cs.bullets, cs.hits, cs.fallbacks, cs.predicted};
    for (u32 i = 0; i < 4; ++i)
        h = (h ^ (u64)v[i]) * 0x100000001B3ull;
    return h;
}

const de_phys_det_scenario de_phys_det_scenario_bullet_wall = {
    .name = "bullet_wall",
    .setup = bw_setup,
    .script = bw_script,
    .probe = ccd_probe,
    .ticks = 600u,
    .state_size = (u32)sizeof(bw_state),
    .max_bodies = 32u,
};
DE_PHYS_DET_REGISTER(bullet_wall)

/* ================================ fast_drop ============================================= */

#define FD_DROPS 10u

typedef struct fd_state {
    de_body drops[FD_DROPS];
    de_body ram[2];
    de_rng rng;
} fd_state;

static de_result fd_setup(de_phys_det_ctx *c) {
    fd_state *st = (fd_state *)c->state;
    de_physics_world *p = c->physics;
    st->rng = de_rng_seed(c->seed, 2190);
    const de_shape slab = box_shape(p, 6.0f, 6.0f, 0.1f);
    if (!DE_HANDLE_VALID(make(p, DE_BODY_STATIC, slab, xform(0, 0, -0.1, quat_identity()), 0.5f,
                              0.0f, 0.0f, 0u)))
        return DE_ERR_INVALID_ARGUMENT;
    /* A 9 x 9 heightfield of gentle bumps at x in [8, 16]. */
    f32 heights[81];
    for (u32 r = 0; r < 9; ++r)
        for (u32 k = 0; k < 9; ++k)
            heights[r * 9u + k] = 0.15f * (f32)((r * 7u + k * 3u) % 5u);
    de_shape_desc hd;
    memset(&hd, 0, sizeof hd);
    hd.kind = DE_SHAPE_HEIGHTFIELD;
    hd.heights = heights;
    hd.rows = 9;
    hd.cols = 9;
    hd.scale = v3(1.0f, 1.0f, 1.0f);
    const de_shape hf = de_shape_create(p, &hd);
    if (!DE_HANDLE_VALID(make(p, DE_BODY_STATIC, hf, xform(8.0, -4.0, 0.0, quat_identity()), 0.5f,
                              0.0f, 0.0f, 0u)))
        return DE_ERR_INVALID_ARGUMENT;
    /* A mesh ramp at x in [-14, -8], rising toward -X (front +Z). */
    const de_vec3 quad[4] = {v3(-8.0f, -3.0f, 0.0f), v3(-8.0f, 3.0f, 0.0f), v3(-14.0f, 3.0f, 2.5f),
                             v3(-14.0f, -3.0f, 2.5f)};
    const de_shape ramp = quad_mesh(p, quad);
    if (!DE_HANDLE_VALID(
            make(p, DE_BODY_STATIC, ramp, xform(0, 0, 0, quat_identity()), 0.4f, 0.0f, 0.0f, 0u)))
        return DE_ERR_INVALID_ARGUMENT;

    de_shape kinds[4];
    kinds[0] = box_shape(p, 0.4f, 0.3f, 0.25f);
    kinds[1] = round_shape(p, DE_SHAPE_SPHERE, 0.3f, 0.0f);
    kinds[2] = round_shape(p, DE_SHAPE_CAPSULE, 0.2f, 0.35f);
    const de_vec3 pts[6] = {v3(0.4f, 0.0f, 0.0f),  v3(-0.3f, 0.3f, 0.0f), v3(-0.3f, -0.3f, 0.1f),
                            v3(0.0f, 0.0f, 0.45f), v3(0.1f, 0.1f, -0.3f), v3(-0.1f, 0.2f, 0.2f)};
    de_shape_desc hull;
    memset(&hull, 0, sizeof hull);
    hull.kind = DE_SHAPE_CONVEX;
    hull.points = pts;
    hull.point_count = 6;
    kinds[3] = de_shape_create(p, &hull);
    for (u32 k = 0; k < 4; ++k)
        if (!DE_HANDLE_VALID(kinds[k]))
            return DE_ERR_INVALID_ARGUMENT;
    for (u32 i = 0; i < FD_DROPS; ++i) {
        st->drops[i] = make(p, DE_BODY_DYNAMIC, kinds[i % 4u],
                            xform(-30.0 + 2.0 * (f64)i, 25.0, 1.0, quat_identity()), 0.5f,
                            i % 3u == 0u ? 0.4f : 0.0f, 0.0f, 0u);
        if (!DE_HANDLE_VALID(st->drops[i]))
            return DE_ERR_INVALID_ARGUMENT;
    }
    for (u32 i = 0; i < 2; ++i) {
        st->ram[i] =
            make(p, DE_BODY_DYNAMIC, kinds[0],
                 xform(-30.0 + 3.0 * (f64)i, 30.0, 1.0, quat_identity()), 0.3f, 0.2f, 0.0f, 0u);
        if (!DE_HANDLE_VALID(st->ram[i]))
            return DE_ERR_INVALID_ARGUMENT;
    }
    return DE_OK;
}

static void fd_script(de_phys_det_ctx *c, u64 tick) {
    fd_state *st = (fd_state *)c->state;
    de_physics_world *p = c->physics;
    de_rng *r = &st->rng;
    if (tick % 120u != 0u)
        return;
    for (u32 i = 0; i < FD_DROPS; ++i) {
        /* Over the slab, the heightfield or the ramp, 1.5-4 m up, falling at 30-60 m/s. */
        const u32 where = i % 3u;
        const f64 x = where == 0u   ? (f64)rf(r, -4.0f, 4.0f)
                      : where == 1u ? 12.0 + rf(r, -3.0f, 3.0f)
                                    : -11.0 + rf(r, -2.0f, 2.0f);
        const f64 y = (f64)rf(r, -2.5f, 2.5f);
        const f64 z = 3.5 + rf(r, 1.5f, 4.0f);
        const f32 ax = rf(r, -1.0f, 1.0f);
        const f32 ay = rf(r, -1.0f, 1.0f);
        const f32 angle = rf(r, 0.0f, 3.0f);
        const de_transform at = xform(x, y, z, quat_axis_angle(v3(ax, ay, 0.5f), angle));
        (void)de_body_set_transform(p, st->drops[i], &at);
        const f32 vx = rf(r, -8.0f, 8.0f);
        const f32 vy = rf(r, -8.0f, 8.0f);
        const f32 vz = -rf(r, 30.0f, 60.0f);
        const f32 wx = rf(r, -6.0f, 6.0f);
        const f32 wy = rf(r, -6.0f, 6.0f);
        const f32 wz = rf(r, -6.0f, 6.0f);
        (void)de_body_set_velocity(p, st->drops[i], v3(vx, vy, vz), v3(wx, wy, wz));
    }
    /* Two boxes meeting head-on above the slab, 40 m/s each. */
    const f64 h = 2.0 + rf(r, 0.0f, 1.0f);
    const f64 off = (f64)rf(r, -0.2f, 0.2f);
    const de_transform a = xform(-3.0, 4.0, h, quat_identity());
    const de_transform b = xform(3.0, 4.0 + off, h, quat_identity());
    (void)de_body_set_transform(p, st->ram[0], &a);
    (void)de_body_set_transform(p, st->ram[1], &b);
    (void)de_body_set_velocity(p, st->ram[0], v3(40.0f, 0.0f, 0.0f), v3(0.0f, 0.0f, 0.0f));
    (void)de_body_set_velocity(p, st->ram[1], v3(-40.0f, 0.0f, 0.0f), v3(0.0f, 0.0f, 1.0f));
}

const de_phys_det_scenario de_phys_det_scenario_fast_drop = {
    .name = "fast_drop",
    .setup = fd_setup,
    .script = fd_script,
    .probe = ccd_probe,
    .ticks = 600u,
    .state_size = (u32)sizeof(fd_state),
    .max_bodies = 32u,
};
DE_PHYS_DET_REGISTER(fast_drop)
