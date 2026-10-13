/*
 * scenarios_contacts.c: determinism scenario of the collision pipeline (DENG-135; SPEC §13.2,
 * §19.4). Registered by tests/determinism/physics/CMakeLists.txt.
 *
 *   contact_pile (600 ticks): 96 dynamic and kinematic bodies of every convex kind (and a
 *                    compound) on scripted de_dmath paths that dip into a static ground box, a
 *                    heightfield with a hole, a mesh ramp, a static compound, a trigger zone and
 *                    each other; a capsule rests on a box 30 km from the origin. Bodies are
 *                    destroyed and recreated (index reuse) every 50 ticks. Before each step a
 *                    stand-in solver reads the last step's manifolds in key order: it writes
 *                    impulses (so warm-start carry-over is in the checksum) and pushes dynamic
 *                    bodies out along the normals (so a contact difference reaches the
 *                    positions). The physics checksum covers the manifolds and end records.
 *
 * SPEC §5.7: de_dmath / de_dvec3 / de_rng only, one random draw per statement, bodies and
 * manifolds visited in ascending order. Built with DE_DETERMINISTIC_FLAGS. Golden:
 * golden/contact_pile.checksums.
 */
#include "harness.h"

#include "physics/contact/manifold.h"
#include "physics/physics_internal.h"

#include <string.h>

#define CP_TICKS  600u
#define CP_MOVERS 96u
#define CP_KINDS  8u
#define CP_BODIES 128u
#define CP_FAR_X  30000.0
#define CP_FAR_Y  -12000.0

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

/* a * b (apply b, then a): + - * only. */
static de_quat quat_mul(de_quat a, de_quat b) {
    de_quat q;
    q.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    q.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    q.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    q.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    return q;
}

static de_transform xform(de_dvec3 p, de_quat q) {
    de_transform t;
    memset(&t, 0, sizeof t);
    t.position = p;
    t.rotation = q;
    t.scale = v3(1.0f, 1.0f, 1.0f);
    return t;
}

static de_body make_body(de_physics_world *p, de_body_type type, de_shape shape, de_transform t,
                         u32 flags) {
    de_body_desc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.transform = t;
    d.shape = shape;
    d.flags = flags;
    d.material.friction = 0.6f;
    return de_body_create(p, &d);
}

typedef struct cp_state {
    de_shape kinds[CP_KINDS];
    de_body movers[CP_MOVERS];
    u32 kind_of[CP_MOVERS];
    de_dvec3 center[CP_MOVERS];
    f32 radius[CP_MOVERS], omega[CP_MOVERS], phase[CP_MOVERS], bob[CP_MOVERS];
    de_body far_capsule;
    de_dvec3 push[CP_BODIES]; /* stand-in solver displacement per body index */
} cp_state;

static de_result cp_setup(de_phys_det_ctx *c) {
    cp_state *st = (cp_state *)c->state;
    de_physics_world *p = c->physics;
    de_rng r = de_rng_seed(c->seed, 135);
    de_shape_desc d;

    /* Statics: ground box, heightfield with a hole, mesh ramp, compound, trigger zone. */
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = v3(6.0f, 6.0f, 0.5f);
    const de_shape ground = de_shape_create(p, &d);
    (void)make_body(p, DE_BODY_STATIC, ground, xform(dv3(0.0, 0.0, -0.5), quat_identity()), 0);
    static f32 heights[10 * 10];
    for (u32 j = 0; j < 10; ++j)
        for (u32 i = 0; i < 10; ++i)
            heights[j * 10 + i] = 0.15f * de_dsin(0.8f * (f32)i) * de_dcos(0.6f * (f32)j);
    heights[5 * 10 + 4] = DE_HEIGHTFIELD_HOLE;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_HEIGHTFIELD;
    d.heights = heights;
    d.rows = 10;
    d.cols = 10;
    d.scale = v3(1.0f, 1.0f, 1.0f);
    (void)make_body(p, DE_BODY_STATIC, de_shape_create(p, &d),
                    xform(dv3(6.0, -4.0, 0.0), quat_identity()), 0);
    static const de_vec3 ramp_v[6] = {{-2, -2, 0},   {2, -2, 0}, {2, 0, 0.4f},
                                      {-2, 0, 0.4f}, {2, 2, 0},  {-2, 2, 0}};
    static const u32 ramp_i[12] = {0, 1, 2, 0, 2, 3, 3, 2, 4, 3, 4, 5};
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_MESH;
    d.vertices = ramp_v;
    d.vertex_count = 6;
    d.indices = ramp_i;
    d.index_count = 12;
    (void)make_body(p, DE_BODY_STATIC, de_shape_create(p, &d),
                    xform(dv3(-8.0, 0.0, -0.2), quat_identity()), 0);

    /* Mover shapes: sphere, capsule, box, cylinder, hull, compound, small box, thin box. */
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_SPHERE;
    d.radius = 0.35f;
    st->kinds[0] = de_shape_create(p, &d);
    d.kind = DE_SHAPE_CAPSULE;
    d.radius = 0.2f;
    d.half_height = 0.35f;
    st->kinds[1] = de_shape_create(p, &d);
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = v3(0.35f, 0.3f, 0.25f);
    st->kinds[2] = de_shape_create(p, &d);
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CYLINDER;
    d.radius = 0.3f;
    d.half_height = 0.25f;
    st->kinds[3] = de_shape_create(p, &d);
    de_vec3 cloud[12];
    for (u32 i = 0; i < 12; ++i) {
        const f32 x = rf(&r, -0.35f, 0.35f);
        const f32 y = rf(&r, -0.35f, 0.35f);
        const f32 z = rf(&r, -0.3f, 0.3f);
        cloud[i] = v3(x, y, z);
    }
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CONVEX;
    d.points = cloud;
    d.point_count = 12;
    st->kinds[4] = de_shape_create(p, &d);
    const de_shape children[2] = {st->kinds[0], st->kinds[2]};
    const de_transform child_x[2] = {xform(dv3(0.0, 0.0, 0.35), quat_identity()),
                                     xform(dv3(0.0, 0.0, -0.2), quat_identity())};
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_COMPOUND;
    d.children = children;
    d.child_xforms = child_x;
    d.child_count = 2;
    st->kinds[5] = de_shape_create(p, &d);
    (void)make_body(p, DE_BODY_STATIC, st->kinds[5], xform(dv3(3.0, 4.0, 0.2), quat_identity()), 0);
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = v3(0.18f, 0.18f, 0.18f);
    st->kinds[6] = de_shape_create(p, &d);
    d.half_extents = v3(0.7f, 0.45f, 0.06f);
    st->kinds[7] = de_shape_create(p, &d);
    for (u32 k = 0; k < CP_KINDS; ++k)
        if (!DE_HANDLE_VALID(st->kinds[k]))
            return DE_ERR_INVALID_ARGUMENT;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_BOX;
    d.half_extents = v3(1.5f, 1.5f, 1.0f);
    (void)make_body(p, DE_BODY_STATIC, de_shape_create(p, &d),
                    xform(dv3(-3.0, -4.0, 0.5), quat_identity()), (u32)DE_BODY_FLAG_TRIGGER);

    /* Far from the origin: a dynamic capsule lying on a static box. */
    (void)make_body(p, DE_BODY_STATIC, ground,
                    xform(dv3(CP_FAR_X, CP_FAR_Y, -0.5), quat_identity()), 0);
    st->far_capsule = make_body(
        p, DE_BODY_DYNAMIC, st->kinds[1],
        xform(dv3(CP_FAR_X, CP_FAR_Y, 0.199), quat_axis_angle(v3(1.0f, 0.0f, 0.0f), 1.5707963f)),
        0);

    for (u32 k = 0; k < CP_MOVERS; ++k) {
        const f32 cx = rf(&r, -5.0f, 5.0f);
        const f32 cy = rf(&r, -5.0f, 5.0f);
        st->center[k] = dv3((f64)cx, (f64)cy, 0.35);
        st->radius[k] = rf(&r, 0.3f, 1.2f);
        st->omega[k] = rf(&r, 0.4f, 1.6f);
        st->phase[k] = rf(&r, 0.0f, 6.2831853f);
        st->bob[k] = rf(&r, 0.05f, 0.3f);
        st->kind_of[k] = k % CP_KINDS;
        const f32 th = st->phase[k];
        const de_dvec3 pos =
            dv3(st->center[k].x + (f64)(st->radius[k] * de_dcos(th)),
                st->center[k].y + (f64)(st->radius[k] * de_dsin(th)), st->center[k].z);
        const de_body_type type = (k % 8u == 7u) ? DE_BODY_KINEMATIC : DE_BODY_DYNAMIC;
        st->movers[k] =
            make_body(p, type, st->kinds[st->kind_of[k]], xform(pos, quat_identity()), 0);
        if (!DE_HANDLE_VALID(st->movers[k]))
            return DE_ERR_INVALID_ARGUMENT;
    }
    return DE_OK;
}

/* The stand-in solver: impulses from the depth (warm-started by the carried value), and a push of
 * half the deepest overlap along the normal, split between the dynamic bodies. */
static void cp_fake_solver(cp_state *st, de_physics_world *p) {
    u32 n = 0;
    de__manifold *m = de__contacts_manifolds_mut(p, &n);
    for (u32 i = 0; i < CP_BODIES; ++i)
        st->push[i] = dv3(0.0, 0.0, 0.0);
    for (u32 i = 0; i < n; ++i) {
        de__manifold *x = &m[i];
        if (x->point_count == 0)
            continue;
        f32 deepest = 0.0f;
        for (u32 k = 0; k < x->point_count; ++k) {
            de__contact *c = &x->points[k];
            const f32 pen = c->separation < 0.0f ? -c->separation : 0.0f;
            c->normal_impulse = 0.75f * c->normal_impulse + pen;
            c->tangent_impulse[0] = 0.5f * c->tangent_impulse[0] + 0.1f * pen;
            c->tangent_impulse[1] = -0.5f * c->tangent_impulse[1] + 0.05f * (f32)k;
            deepest = c->separation < deepest ? c->separation : deepest;
        }
        if (deepest >= 0.0f)
            continue;
        const bool da = p->bodies.type[x->key.a] == (u8)DE_BODY_DYNAMIC;
        const bool db = p->bodies.type[x->key.b] == (u8)DE_BODY_DYNAMIC;
        if (!da && !db)
            continue;
        const f64 share = (f64)(-deepest) * ((da && db) ? 0.25 : 0.5);
        const de_dvec3 dn =
            dv3((f64)x->normal.x * share, (f64)x->normal.y * share, (f64)x->normal.z * share);
        if (da && x->key.a < CP_BODIES)
            st->push[x->key.a] = de_dv3_sub(st->push[x->key.a], dn);
        if (db && x->key.b < CP_BODIES)
            st->push[x->key.b] = de_dv3_add(st->push[x->key.b], dn);
    }
    for (u32 i = 0; i < p->bodies.high_water && i < CP_BODIES; ++i) {
        if (!p->bodies.alive[i])
            continue;
        p->bodies.position[i] = de_dv3_add(p->bodies.position[i], st->push[i]);
    }
}

static void cp_script(de_phys_det_ctx *c, u64 tick) {
    cp_state *st = (cp_state *)c->state;
    de_physics_world *p = c->physics;
    const f32 dt = c->dt;
    const f32 time = (f32)tick * dt;

    cp_fake_solver(st, p);

    /* Churn: every 50 ticks one mover is destroyed and recreated with the next shape kind (it
     * takes the freed index at once). */
    if (tick > 0 && tick % 50u == 0u) {
        const u32 k = (u32)(tick / 50u * 11u) % CP_MOVERS;
        de_transform t;
        if (de_body_get_transform(p, st->movers[k], &t)) {
            const de_body_type type =
                (de_body_type)p->bodies.type[de__handle_index(st->movers[k].id)];
            de_body_destroy(p, st->movers[k]);
            st->kind_of[k] = (st->kind_of[k] + 1u) % CP_KINDS;
            st->movers[k] = make_body(p, type, st->kinds[st->kind_of[k]], t, 0);
        }
    }

    /* Scripted paths, integrated from each body's own state (a difference carries on). */
    for (u32 k = 0; k < CP_MOVERS; ++k) {
        de_transform t;
        if (!de_body_get_transform(p, st->movers[k], &t))
            continue;
        const f32 w = st->omega[k];
        const f32 theta = st->phase[k] + w * time;
        f32 s, co, s2, c2;
        de_dsincos(theta, &s, &co);
        de_dsincos(2.0f * theta, &s2, &c2);
        const f32 rw = st->radius[k] * w;
        /* The bob pulls back toward the path height, so pushes do not accumulate forever. */
        const f32 back = (f32)(st->center[k].z - t.position.z) * 2.0f;
        const de_vec3 v = v3(-rw * s, rw * co, 2.0f * st->bob[k] * w * c2 + back);
        const de_vec3 av = v3(0.6f * de_dsin(0.7f * theta + (f32)k), 0.2f + 0.05f * (f32)k,
                              0.7f * de_dcos(0.5f * theta));
        t.position = de_dv3_add(t.position, de_dv3_scale(de_dv3_from_v3(v), (f64)dt));
        t.rotation = de_dintegrate_quat(t.rotation, av, dt);
        de_body_set_transform(p, st->movers[k], &t);
    }
    /* The far capsule rocks a little on its box. */
    const de_quat lie = quat_axis_angle(v3(1.0f, 0.0f, 0.0f), 1.5707963f);
    const de_quat rock = quat_axis_angle(v3(0.0f, 1.0f, 0.0f), 0.01f * de_dsin(time * 3.0f));
    de_transform ft;
    if (de_body_get_transform(p, st->far_capsule, &ft)) {
        ft.rotation = quat_mul(rock, lie);
        de_body_set_transform(p, st->far_capsule, &ft);
    }
}

const de_phys_det_scenario de_phys_det_scenario_contact_pile = {
    .name = "contact_pile",
    .setup = cp_setup,
    .script = cp_script,
    .ticks = CP_TICKS,
    .state_size = (u32)sizeof(cp_state),
    .max_bodies = CP_BODIES,
};
DE_PHYS_DET_REGISTER(contact_pile)
