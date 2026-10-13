/*
 * phys_ecs.h: the ECS systems of 3D physics (DENG-223, SPEC §13.1, §7.1, §7.3, §10.2, §5.7).
 * Internal: include as "physics/ecs/phys_ecs.h".
 *
 * Files (src/physics/ecs/):
 * - attach.c: de_physics_attach (types, components, resource) and de__pecs_attach (this state,
 *   the event types, the two systems).
 * - entity_map.c: the state in extension slot DE__PHYS_EXT_ECS, the entity map, the graveyard,
 *   the growable arrays, and the lookups (de_physics_entity_body, de__phys_ecs_entity_body).
 * - sync.c: ECS -> physics (validation, creation, changes, kinematic targets) and the write-back.
 * - systems.c: physics.step (the order of the passes) and the character moves.
 * - contact_events.c: contact and joint break events.
 * - interpolation.c: the poses of the last two ticks and physics.interpolate.
 *
 * The entity map is an array of entries sorted by entity index (one entry per live entity that
 * has a body), searched by binary search and always walked in entity index order (SPEC §5.7 rule
 * 5: no hash map). New entries of a tick are created in archetype / row order of the change
 * queries, collected in a batch, then sorted by entity index and merged in.
 *
 * Determinism: physics.step only uses + - * /, de_dmath and the physics API; every walk is in
 * entity index, archetype / row or manifold key order. physics.interpolate is presentation only.
 *
 * Memory: every array grows by doubling (from the world allocator) when a tick needs more room
 * than any earlier tick, so steady-state ticks allocate nothing.
 */
#ifndef DE_PHYSICS_ECS_PHYS_ECS_H
#define DE_PHYSICS_ECS_PHYS_ECS_H

#include "physics/physics_internal.h"

DE_EXTERN_C_BEGIN

/** Entry kinds. */
enum {
    DE__PECS_RIGID = 1,    /**< DeRigidBody + DeCollider */
    DE__PECS_STATIC = 2,   /**< DeCollider without DeRigidBody: a static body */
    DE__PECS_CHARACTER = 3 /**< DeCharacter: a character proxy */
};

/** Entry flags. */
enum {
    DE__PECS_NO_INTERP = 1u << 0, /**< DeRigidBody.no_interpolate */
    DE__PECS_FAILED = 1u << 1     /**< re-creating the body failed: drop without a rescan */
};

/** One entity with a body. */
typedef struct de__pecs_entry {
    de_entity entity;
    de_body body;
    de_shape owned_shape; /**< compound made for DeCollider.offset; invalid = none */
    u8 kind;              /**< DE__PECS_* */
    u8 body_type;         /**< de_body_type of the body */
    u8 flags;             /**< DE__PECS_NO_INTERP, DE__PECS_FAILED */
    u8 pad_;
    u64 key; /**< hash of the fields whose change re-creates the body */
} de__pecs_entry;

/** A pose (body origin) of one tick. */
typedef struct de__pecs_pose {
    de_dvec3 position;
    de_quat rotation;
} de__pecs_pose;

/** The poses of one body before and after the last step. */
typedef struct de__pecs_pose_pair {
    de__pecs_pose prev, curr;
} de__pecs_pose_pair;

/** A kinematic target of this tick: the body ends the tick at `pose`. */
typedef struct de__pecs_target {
    de_body body;
    de_entity entity;
    de__pecs_pose pose;
} de__pecs_target;

/** A body destroyed since the last events (for the owners of DE_CONTACT_END). */
typedef struct de__pecs_grave {
    u32 index, generation;
    de_entity owner;
} de__pecs_grave;

/** Counters, for tests and the debug overlay. Totals since attach, except `entries`. */
typedef struct de__pecs_stats {
    u32 entries;        /**< bodies in the map now */
    u64 created;        /**< bodies created */
    u64 destroyed;      /**< bodies destroyed */
    u64 recreated;      /**< bodies re-created after a component change */
    u64 teleports;      /**< poses pushed into bodies (teleports and kinematic targets) */
    u64 velocities;     /**< velocities pushed into bodies */
    u64 writes;         /**< components written back */
    u64 contact_events; /**< contact events sent */
    u64 break_events;   /**< joint break events sent */
    u64 rescans;        /**< full scans after a component was removed */
    u64 failed;         /**< bodies that could not be created */
} de__pecs_stats;

/** Query slots of physics.step (de_system_ctx.queries). */
enum {
    DE__PECS_Q_DECL = 0,     /**< access declaration only; never iterated */
    DE__PECS_Q_RB_CHANGED,   /**< DeRigidBody changed (or added) */
    DE__PECS_Q_COL_CHANGED,  /**< DeCollider changed (or added) */
    DE__PECS_Q_CHAR_CHANGED, /**< DeCharacter changed (or added) */
    DE__PECS_Q_XFORM_COL,    /**< DeTransform changed, with DeCollider */
    DE__PECS_Q_XFORM_CHAR,   /**< DeTransform changed, with DeCharacter */
    DE__PECS_Q_ALL_COL,      /**< every DeCollider (rescans) */
    DE__PECS_Q_ALL_CHAR,     /**< every DeCharacter (moves, rescans) */
    DE__PECS_Q_COUNT
};

typedef struct de__pecs {
    const de_allocator *alloc;
    de_physics_world *p;
    de_world *w;

    de_component c_rb, c_col, c_char, c_xform, c_global, c_child_of, c_children;
    de_event_type ev_contact, ev_break;
    de_system sys_step, sys_interp;

    /* The entity map (sorted by entity index) and this tick's new entries. */
    de__pecs_entry *map;
    u32 count, cap;
    de__pecs_entry *batch;
    u32 batch_count, batch_cap;
    de__pecs_entry *scratch; /**< merge buffer */
    u32 scratch_cap;
    u64 *marks; /**< bit per entity index: a creation was tried this tick */
    u32 mark_words;
    u32 *marked; /**< the entity indices marked this tick (to clear them) */
    u32 marked_count, marked_cap;
    bool rescan; /**< an alive entity lost its entry: scan every candidate this tick */

    /* Interpolation: poses before and after the last step, per body index. */
    de__pecs_pose_pair *poses;
    u32 pose_cap;
    bool stepped; /**< a step ran since attach (the poses are valid) */

    de__pecs_target *targets;
    u32 target_count, target_cap;

    de__pecs_grave *graves;
    u32 grave_count, grave_cap;
    bool grave_overflow_logged;

    de_joint_break_event *breaks;
    u32 break_cap;

    de__pecs_stats stats;
} de__pecs;

static inline de__pecs *de__pecs_get(const de_physics_world *p) {
    return p != NULL ? (de__pecs *)de__phys_ext_get(p, DE__PHYS_EXT_ECS) : NULL;
}

/* ---- entity_map.c ---- */

/** Creates the state in the DE__PHYS_EXT_ECS slot (empty arrays). */
de_result de__pecs_create(de_physics_world *p, de_world *w, de__pecs **out);
/** Grows `*arr` (elements of `elem` bytes, capacity `*cap`) to hold at least `need`. */
bool de__pecs_grow(de__pecs *s, void **arr, u32 *cap, u32 need, size_t elem);
/** The map entry of `e` (same index and generation), or NULL. */
de__pecs_entry *de__pecs_find(de__pecs *s, de_entity e);
/** Marks entity index `e` (a creation was tried this tick); false when it was marked already, or
 *  when there is no memory for the mark (then nothing is tried). */
bool de__pecs_mark(de__pecs *s, de_entity e);
/** true when entity index `e` is marked this tick. */
bool de__pecs_marked(const de__pecs *s, de_entity e);
/** Clears this tick's marks. */
void de__pecs_clear_marks(de__pecs *s);
/** Appends `en` to this tick's batch. */
bool de__pecs_batch_add(de__pecs *s, const de__pecs_entry *en);
/** Sorts the batch by entity index and merges it into the map. */
void de__pecs_batch_merge(de__pecs *s);
/** The owner of body (index, generation): the live body's owner, or the graveyard's. */
de_entity de__pecs_owner(const de__pecs *s, u32 index, u32 generation);
/** Empties the graveyard (after the events of a step). */
void de__pecs_graves_clear(de__pecs *s);
/** Makes the pose arrays cover every body index in use. */
bool de__pecs_poses_ensure(de__pecs *s);

/* ---- sync.c ---- */

/** Pass 1: drops the entries of dead entities, of entities whose kind changed and of stale
 *  bodies, destroying their bodies, in entity index order. */
void de__pecs_validate(de__pecs *s);
/** Pass 2: creation and changes from the change queries (and a full scan when needed). */
void de__pecs_sync(de__pecs *s, const de_system_ctx *ctx, f32 dt);
/** After the step: kinematic bodies onto their targets, velocities back. */
void de__pecs_finish_targets(de__pecs *s);
/** Pass 5: poses and velocities back to the components (only when they differ); the current
 *  interpolation poses. */
void de__pecs_write_back(de__pecs *s);
/** Destroys the body (and owned shape) of an entry. */
void de__pecs_destroy_body(de__pecs *s, de__pecs_entry *en);
/** The kind entity `e` should have now (0: none). */
u8 de__pecs_kind_of(const de__pecs *s, de_entity e);

/* ---- interpolation.c ---- */

/** Stores every mapped body's pose as the pose before the step. */
void de__pecs_capture_prev(de__pecs *s);
/** physics.interpolate. */
void de__pecs_interpolate_system(de_system_ctx *ctx);

/* ---- contact_events.c ---- */

/** Sends the contact events of the last step, then the joint break events. */
void de__pecs_send_events(de__pecs *s);

/* ---- systems.c ---- */

/** physics.step. */
void de__pecs_step_system(de_system_ctx *ctx);
/** Adds physics.step and physics.interpolate (attach.c calls it once per state). */
de_result de__pecs_add_systems(de__pecs *s);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_ECS_PHYS_ECS_H */
