/*
 * scenarios_ecs.c: determinism scenario of the physics ECS systems (DENG-223; SPEC §13.1, §5.7,
 * §19.4). Registered by tests/determinism/physics/CMakeLists.txt.
 *
 *   ecs_scene (240 ticks): everything is an entity, stepped by physics.step through
 *     de_world_run_schedule(FIXED_UPDATE):
 *     - static colliders (DeCollider only): ground, a ramp, a trigger zone, a wall with a collider
 *       offset;
 *     - 100 dynamic bodies (boxes, spheres, capsules, mixed materials) spawned two per tick over
 *       the first 50 ticks, 20 of them despawned at ticks 60..79;
 *     - a kinematic platform driven by DeTransform targets every tick;
 *     - a character walking a circle (DeCharacter.desired_velocity);
 *     - two pendulums (ball, hinge) joined by entity joints at tick 1; the hinge breaks in its
 *       first swing;
 *     - a velocity kick (tick 100), a teleport (tick 140) and a material change that re-creates a
 *       body (tick 150).
 *   The probe hashes every contact and joint break event of the tick, so the events are part of
 *   the cross-platform golden too; the tick checksum already holds de_physics_checksum and
 *   de_world_checksum (DeRigidBody, DeCollider, DeCharacter, DeTransform).
 *
 * SPEC §5.7: de_dmath / de_dvec3 / de_rng only, one random draw per statement, entities spawned,
 * despawned and scripted in a fixed order. Built with DE_DETERMINISTIC_FLAGS. Golden:
 * golden/ecs_scene.checksums (regenerate only with test_det_physics --update-golden).
 */
#include "harness.h"

#include <string.h>

#define ES_SPAWN_TICKS 50u
#define ES_PER_TICK    2u
#define ES_BODIES      (ES_SPAWN_TICKS * ES_PER_TICK)
#define ES_DESPAWN_AT  60u
#define ES_DESPAWNS    20u

typedef struct es_state {
    de_rng rng;
    de_component rb, col, ch, xf, gx;
    de_shape shapes[3]; /* box, sphere, capsule */
    de_shape ground_box, ramp_box, zone_box, wall_box;
    de_entity bodies[ES_BODIES];
    u32 spawned;
    de_entity platform, character;
    de_entity anchor[2], bob[2];
    de_event_reader contacts, breaks;
} es_state;

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

static de_quat quat_z(f32 angle) {
    f32 s, c;
    de_dsincos(angle * 0.5f, &s, &c);
    de_quat q;
    q.x = 0.0f;
    q.y = 0.0f;
    q.z = s;
    q.w = c;
    return q;
}

static de_quat quat_y(f32 angle) {
    f32 s, c;
    de_dsincos(angle * 0.5f, &s, &c);
    de_quat q;
    q.x = 0.0f;
    q.y = s;
    q.z = 0.0f;
    q.w = c;
    return q;
}

static de_shape make_shape(de_physics_world *p, de_shape_kind kind, de_vec3 he, f32 r, f32 hh) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = kind;
    d.half_extents = he;
    d.radius = r;
    d.half_height = hh;
    return de_shape_create(p, &d);
}

/* An entity with DeTransform (and DeGlobalTransform) at `pos`, rotated by `q`. */
static de_entity spawn_xf(es_state *s, de_world *w, de_dvec3 pos, de_quat q) {
    const de_component comps[2] = {s->xf, s->gx};
    de_entity e = DE_ENTITY_NULL;
    if (de_entity_spawn_with(w, comps, 2, &e) != DE_OK)
        return DE_ENTITY_NULL;
    de_transform *t = (de_transform *)de_entity_get(w, e, s->xf);
    t->position = pos;
    t->rotation = q;
    return e;
}

static DeCollider *add_collider(es_state *s, de_world *w, de_entity e, de_shape shape, f32 friction,
                                f32 restitution) {
    DeCollider *c = (DeCollider *)de_entity_add(w, e, s->col);
    c->shape = shape;
    c->offset.rotation.w = 1.0f;
    c->offset.scale = v3(1.0f, 1.0f, 1.0f);
    c->material.friction = friction;
    c->material.restitution = restitution;
    return c;
}

static DeRigidBody *add_rigid(es_state *s, de_world *w, de_entity e, de_body_type type) {
    DeRigidBody *rb = (DeRigidBody *)de_entity_add(w, e, s->rb);
    rb->type = type;
    return rb;
}

static de_result es_setup(de_phys_det_ctx *c) {
    es_state *s = (es_state *)c->state;
    de_world *w = c->world;
    de_physics_world *p = c->physics;
    s->rng = de_rng_seed(c->seed, 223u);
    s->rb = de_component_find(w, "DeRigidBody");
    s->col = de_component_find(w, "DeCollider");
    s->ch = de_component_find(w, "DeCharacter");
    const de_builtin_component_ids bi = de_builtin_components(w);
    s->xf = bi.transform;
    s->gx = bi.global_transform;
    s->shapes[0] = make_shape(p, DE_SHAPE_BOX, v3(0.25f, 0.2f, 0.15f), 0.0f, 0.0f);
    s->shapes[1] = make_shape(p, DE_SHAPE_SPHERE, v3(0, 0, 0), 0.2f, 0.0f);
    s->shapes[2] = make_shape(p, DE_SHAPE_CAPSULE, v3(0, 0, 0), 0.15f, 0.2f);
    s->ground_box = make_shape(p, DE_SHAPE_BOX, v3(20.0f, 20.0f, 0.5f), 0.0f, 0.0f);
    s->ramp_box = make_shape(p, DE_SHAPE_BOX, v3(2.0f, 2.0f, 0.1f), 0.0f, 0.0f);
    s->zone_box = make_shape(p, DE_SHAPE_BOX, v3(1.5f, 1.5f, 1.0f), 0.0f, 0.0f);
    s->wall_box = make_shape(p, DE_SHAPE_BOX, v3(0.2f, 3.0f, 1.0f), 0.0f, 0.0f);

    /* Static colliders. */
    de_entity e = spawn_xf(s, w, dv3(0.0, 0.0, -0.5), quat_z(0.0f));
    add_collider(s, w, e, s->ground_box, 0.6f, 0.0f);
    e = spawn_xf(s, w, dv3(-3.0, 2.0, 0.6), quat_y(0.3f));
    add_collider(s, w, e, s->ramp_box, 0.4f, 0.0f);
    e = spawn_xf(s, w, dv3(3.0, -3.0, 1.0), quat_z(0.0f));
    add_collider(s, w, e, s->zone_box, 0.0f, 0.0f)->is_trigger = true;
    e = spawn_xf(s, w, dv3(6.0, -6.0, 0.0), quat_z(0.2f));
    add_collider(s, w, e, s->wall_box, 0.5f, 0.0f)->offset.position = dv3(0.0, 0.0, 1.0);

    /* Kinematic platform. */
    s->platform = spawn_xf(s, w, dv3(6.0, 2.0, 0.3), quat_z(0.0f));
    add_rigid(s, w, s->platform, DE_BODY_KINEMATIC);
    add_collider(s, w, s->platform, s->ramp_box, 0.8f, 0.0f);

    /* Character. */
    s->character = spawn_xf(s, w, dv3(-6.0, -4.0, 1.0), quat_z(0.0f));
    DeCharacter *ch = (DeCharacter *)de_entity_add(w, s->character, s->ch);
    ch->radius = 0.3f;
    ch->height = 1.6f;

    /* Pendulum anchors and bobs (joined at tick 1, once they have bodies). */
    for (u32 i = 0; i < 2; ++i) {
        const f64 x = -8.0 + 3.0 * (f64)i;
        s->anchor[i] = spawn_xf(s, w, dv3(x, 6.0, 5.0), quat_z(0.0f));
        add_rigid(s, w, s->anchor[i], DE_BODY_STATIC);
        add_collider(s, w, s->anchor[i], s->shapes[1], 0.5f, 0.0f);
        s->bob[i] = spawn_xf(s, w, dv3(x + 1.0, 6.0, 4.0), quat_z(0.0f));
        add_rigid(s, w, s->bob[i], DE_BODY_DYNAMIC);
        add_collider(s, w, s->bob[i], s->shapes[0], 0.5f, 0.1f);
    }

    s->contacts = de_event_reader_create(w, de_physics_contact_event_type(p));
    s->breaks = de_event_reader_create(w, de_physics_joint_break_event_type(p));
    return DE_OK;
}

static void spawn_dynamic(es_state *s, de_world *w) {
    const f32 x = rf(&s->rng, -4.0f, 4.0f);
    const f32 y = rf(&s->rng, -4.0f, 4.0f);
    const f32 z = rf(&s->rng, 1.0f, 4.0f);
    const f32 yaw = rf(&s->rng, 0.0f, 3.0f);
    const f32 vx = rf(&s->rng, -1.0f, 1.0f);
    const f32 vy = rf(&s->rng, -1.0f, 1.0f);
    const f32 friction = rf(&s->rng, 0.1f, 0.9f);
    const f32 restitution = rf(&s->rng, 0.0f, 0.5f);
    const u32 n = s->spawned;
    const de_entity e = spawn_xf(s, w, dv3((f64)x, (f64)y, (f64)z), quat_z(yaw));
    DeRigidBody *rb = add_rigid(s, w, e, DE_BODY_DYNAMIC);
    rb->linear_velocity = v3(vx, vy, 0.0f);
    rb->angular_velocity = v3(0.0f, 0.0f, (f32)(n % 5u) - 2.0f);
    rb->linear_damping = (n % 7u == 0u) ? 0.5f : 0.0f;
    add_collider(s, w, e, s->shapes[n % 3u], friction, restitution);
    s->bodies[s->spawned++] = e;
}

static void es_script(de_phys_det_ctx *c, u64 tick) {
    es_state *s = (es_state *)c->state;
    de_world *w = c->world;
    const f32 t = (f32)tick * c->dt;
    if (tick < ES_SPAWN_TICKS)
        for (u32 i = 0; i < ES_PER_TICK; ++i)
            spawn_dynamic(s, w);
    if (tick >= ES_DESPAWN_AT && tick < ES_DESPAWN_AT + ES_DESPAWNS)
        de_entity_despawn(w, s->bodies[(u32)(tick - ES_DESPAWN_AT) * 5u]);

    if (tick == 1) {
        for (u32 i = 0; i < 2; ++i) {
            de_joint_desc jd;
            memset(&jd, 0, sizeof jd);
            jd.kind = i == 0 ? DE_JOINT_BALL : DE_JOINT_HINGE;
            jd.a = s->anchor[i];
            jd.b = s->bob[i];
            /* Pivot at the anchor's origin; the bob hangs 45 degrees out, 1.41 m away, and swings
             * in the x-z plane (hinge axis X turned onto world Y). The 60 kg bob's tension
             * reaches about 930 N at the bottom of the swing: the hinge (800 N) breaks. */
            jd.frame_b.position = dv3(-1.0, 0.0, 1.0);
            jd.frame_a.rotation = quat_z(1.5707963f);
            jd.frame_b.rotation = jd.frame_a.rotation;
            jd.break_force = i == 1 ? 800.0f : 0.0f;
            (void)de_joint_create(c->physics, &jd);
        }
    }

    /* The platform follows a figure through DeTransform targets. */
    f32 sn, cs;
    de_dsincos(t * 1.3f, &sn, &cs);
    de_transform *px = (de_transform *)de_entity_get(w, s->platform, s->xf);
    px->position = dv3(6.0 + 2.0 * (f64)sn, 2.0 + 1.0 * (f64)cs, 0.3 + 0.2 * (f64)(sn * cs));
    px->rotation = quat_z(0.4f * sn);

    /* The character walks a circle and jumps now and then. */
    de_dsincos(t * 0.8f, &sn, &cs);
    DeCharacter *ch = (DeCharacter *)de_entity_get(w, s->character, s->ch);
    ch->desired_velocity = v3(2.0f * cs, 2.0f * sn, (tick % 90u == 45u) ? 4.0f : 0.0f);

    if (tick == 100) {
        DeRigidBody *rb = (DeRigidBody *)de_entity_get(w, s->bodies[3], s->rb);
        rb->linear_velocity = v3(0.0f, 0.0f, 6.0f);
    }
    if (tick == 140) {
        de_transform *x = (de_transform *)de_entity_get(w, s->bodies[7], s->xf);
        x->position = dv3(0.0, 0.0, 6.0);
    }
    if (tick == 150) {
        DeCollider *col = (DeCollider *)de_entity_get(w, s->bodies[11], s->col);
        col->material.restitution = 0.9f;
    }
}

typedef struct es_buf {
    u8 b[64];
    u32 n;
} es_buf;

static void put(es_buf *b, const void *v, u32 n) {
    memcpy(b->b + b->n, v, n);
    b->n += n;
}

static u64 es_probe(de_phys_det_ctx *c, u64 tick) {
    (void)tick;
    es_state *s = (es_state *)c->state;
    u64 h = 0x5ECDE223ull;
    const de_contact_event *ev;
    while ((ev = (const de_contact_event *)de_event_next(c->world, &s->contacts)) != NULL) {
        es_buf b;
        b.n = 0;
        const u32 kind = (u32)ev->kind;
        put(&b, &kind, 4);
        put(&b, &ev->a.bits, 8);
        put(&b, &ev->b.bits, 8);
        put(&b, &ev->point, sizeof ev->point);
        put(&b, &ev->normal, sizeof ev->normal);
        put(&b, &ev->impulse, 4);
        h = de_hash64(b.b, b.n, h);
    }
    const de_joint_break_event *br;
    while ((br = (const de_joint_break_event *)de_event_next(c->world, &s->breaks)) != NULL) {
        es_buf b;
        b.n = 0;
        put(&b, &br->joint.id, 4);
        put(&b, &br->a.bits, 8);
        put(&b, &br->b.bits, 8);
        put(&b, &br->force, 4);
        h = de_hash64(b.b, b.n, h);
    }
    /* The harness runs no First schedule: start the next event frame here (read everything
     * first), so the buffers stay small. */
    de_world_update_events(c->world);
    return h;
}

const de_phys_det_scenario de_phys_det_scenario_ecs_scene = {
    "ecs_scene", es_setup, es_script, es_probe, NULL, 240, (u32)sizeof(es_state), 1024, 0.0f, 0, 0};
DE_PHYS_DET_REGISTER(ecs_scene)
