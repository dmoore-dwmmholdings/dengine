/* ecs_test_util.h: an ECS world with an attached physics world, and entity helpers, shared by the
 * physics ECS tests (DENG-223). Test-only. */
#ifndef DE_ECS_TEST_UTIL_H
#define DE_ECS_TEST_UTIL_H

#include "solver_test_util.h"

#include "physics/ecs/phys_ecs.h"

/** An ECS world with physics attached. Allocations of both go through `track`. */
typedef struct pe_env {
    de_tracking_allocator track;
    de_allocator alloc;
    de_type_registry *types;
    de_world *w;
    de_physics_world *p;
    de_component rb, col, ch, xf, gx;
    de_event_reader contacts, breaks;
    u32 frame;
} pe_env;

/** Creates the worlds and attaches; `max_bodies` 0 = 1024. */
static inline void pe_init(pe_env *e, u32 max_bodies) {
    memset(e, 0, sizeof *e);
    de_fp_env_init_thread(); /* physics.step is a deterministic system (SPEC §5.7 rule 4) */
    e->alloc = de_tracking_allocator_init(&e->track, NULL, DE_MEM_PHYSICS);
    EXPECT_TRUE(de_type_registry_create(NULL, &e->types) == DE_OK);
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.allocator = &e->alloc;
    wd.types = e->types;
    EXPECT_TRUE(de_world_create(&wd, &e->w) == DE_OK);
    de_physics_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.allocator = &e->alloc;
    pd.max_bodies = max_bodies != 0 ? max_bodies : 1024u;
    pd.deterministic = true;
    EXPECT_TRUE(de_physics_create(&pd, &e->p) == DE_OK);
    EXPECT_TRUE(de_physics_attach(e->p, e->w, e->types) == DE_OK);
    e->rb = e->p->comp_rigid_body;
    e->col = e->p->comp_collider;
    e->ch = e->p->comp_character;
    const de_builtin_component_ids bi = de_builtin_components(e->w);
    e->xf = bi.transform;
    e->gx = bi.global_transform;
    e->contacts = de_event_reader_create(e->w, de_physics_contact_event_type(e->p));
    e->breaks = de_event_reader_create(e->w, de_physics_joint_break_event_type(e->p));
}

static inline void pe_free(pe_env *e) {
    de_world_destroy(e->w); /* first: its systems point to the physics world */
    de_physics_destroy(e->p);
    de_type_registry_destroy(e->types);
}

/** One fixed tick: a new event frame, then FixedUpdate with dt. */
static inline de_result pe_tick_dt(pe_env *e, f32 dt) {
    de_world_update_events(e->w);
    ++e->frame;
    return de_world_run_schedule(e->w, DE_SCHEDULE_FIXED_UPDATE, dt, 0.0f);
}
static inline de_result pe_tick(pe_env *e) {
    return pe_tick_dt(e, ST_DT);
}

/** Spawns an entity with DeTransform at (x, y, z) and DeGlobalTransform. */
static inline de_entity pe_spawn_xf(pe_env *e, f64 x, f64 y, f64 z) {
    const de_component comps[2] = {e->xf, e->gx};
    de_entity ent;
    EXPECT_TRUE(de_entity_spawn_with(e->w, comps, 2, &ent) == DE_OK);
    de_transform *t = (de_transform *)de_entity_get(e->w, ent, e->xf);
    t->position = de__d3(x, y, z);
    return ent;
}

/** Adds DeCollider with `shape` (material friction 0.5) to `ent`. */
static inline DeCollider *pe_add_collider(pe_env *e, de_entity ent, de_shape shape) {
    DeCollider *c = (DeCollider *)de_entity_add(e->w, ent, e->col);
    c->shape = shape;
    c->offset.rotation.w = 1.0f;
    c->offset.scale = de__v3(1.0f, 1.0f, 1.0f);
    c->material.friction = 0.5f;
    return c;
}

/** A rigid body entity (DeTransform, DeGlobalTransform, DeRigidBody of `type`, DeCollider). */
static inline de_entity pe_spawn_body(pe_env *e, de_body_type type, de_shape shape, f64 x, f64 y,
                                      f64 z) {
    const de_entity ent = pe_spawn_xf(e, x, y, z);
    DeRigidBody *rb = (DeRigidBody *)de_entity_add(e->w, ent, e->rb);
    rb->type = type;
    pe_add_collider(e, ent, shape);
    return ent;
}

/** A static collider entity (no DeRigidBody). */
static inline de_entity pe_spawn_static(pe_env *e, de_shape shape, f64 x, f64 y, f64 z) {
    const de_entity ent = pe_spawn_xf(e, x, y, z);
    pe_add_collider(e, ent, shape);
    return ent;
}

/** Flat ground: a static box whose top is z = 0. */
static inline de_entity pe_ground(pe_env *e) {
    return pe_spawn_static(e, st_box(e->p, 50.0f, 50.0f, 0.5f), 0.0, 0.0, -0.5);
}

static inline const de_transform *pe_xf(const pe_env *e, de_entity ent) {
    return (const de_transform *)de_entity_get_const(e->w, ent, e->xf);
}
static inline const DeRigidBody *pe_rb(const pe_env *e, de_entity ent) {
    return (const DeRigidBody *)de_entity_get_const(e->w, ent, e->rb);
}

/** The body index of an entity (UINT32_MAX when it has no body). */
static inline u32 pe_index(const pe_env *e, de_entity ent) {
    u32 i;
    return de__phys_ecs_entity_body(e->p, ent, &i) ? i : 0xFFFFFFFFu;
}

/** Reads every unread contact event into out[0..max); returns how many were read. */
static inline u32 pe_read_contacts(pe_env *e, de_contact_event *out, u32 max) {
    u32 n = 0;
    const de_contact_event *ev;
    while ((ev = (const de_contact_event *)de_event_next(e->w, &e->contacts)) != NULL) {
        if (n < max)
            out[n] = *ev;
        ++n;
    }
    return n;
}

#endif /* DE_ECS_TEST_UTIL_H */
