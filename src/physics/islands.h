/*
 * islands.h: islands, sleeping and wake-up of the 3D physics world (DENG-218, SPEC §13.1, §13.2,
 * §5.7). Internal: include as "physics/islands.h".
 *
 * Islands. Rebuilt every step after the constraints are gathered (de__islands_build): a
 * union-find over the solver slots of the awake bodies, visiting the constraint records in their
 * stable order. Only records whose two sides are writable (awake dynamic bodies) link: static,
 * kinematic and sleeping bodies and the world never join islands. The root of a set is always
 * its lowest slot, and slots follow body indices, so the island id is the lowest body index of
 * the island whatever the union order. Islands are numbered in ascending id; each lists its
 * bodies in ascending index.
 *
 * Sleeping (sleep.c). After each step every awake dynamic body updates its sleep timer: it grows
 * by dt while the body has DE_BODY_FLAG_ALLOW_SLEEP and moves slower than the thresholds
 * (|v| < physics.sleep.linear_threshold, |w| < physics.sleep.angular_threshold), else it is reset.
 * An island goes to sleep when all of its bodies allow sleep, the smallest timer reaches
 * physics.sleep.time, and no moving kinematic body touches it. Its bodies keep their pose, get
 * zero velocities and leave the solver: no solver body, no constraint, no integration, so their
 * world AABBs and broadphase proxies stay where they are. Manifolds between sleeping bodies are
 * kept (the collision pipeline still finds the pairs) with their impulses, for the warm start
 * after waking.
 *
 * Waking. A sleeping body wakes with every body of its island (same sleep island id) at the start
 * of the next step (de__islands_update), when:
 *   - a gathered constraint links it to an awake dynamic body or to a moving kinematic body
 *     (a contact that begins, a joint; the solver wakes the island and gathers again),
 *   - the user changes it: de_body_set_velocity (non-zero), de_body_set_transform (another pose),
 *   - de_body_wake, or de__islands_wake_body (joint create and destroy, DENG-220),
 *   - a body it touched is destroyed (contact end with DE__CONTACT_END_REMOVED), or a body of its
 *     island is destroyed.
 * de_body_is_sleeping already answers false after a wake request.
 *
 * Determinism (SPEC §5.7): every pass visits bodies in ascending index and records in record
 * order; the parallel passes (timers, island evaluation) use fixed ranges (DE__SOLVER_BODY_GRAIN
 * bodies or islands) and write only their own entries; islands go to sleep in ascending id.
 *
 * Memory: the state lives in the solver's one block (solver.c); steps allocate nothing.
 */
#ifndef DE_PHYSICS_ISLANDS_H
#define DE_PHYSICS_ISLANDS_H

#include "physics/physics_internal.h"

DE_EXTERN_C_BEGIN

typedef struct de__solver de__solver;

/* ================================ Settings ============================================= */

#define DE__SLEEP_DEFAULT_LINEAR  0.05f  /**< m/s */
#define DE__SLEEP_DEFAULT_ANGULAR 0.035f /**< rad/s */
#define DE__SLEEP_DEFAULT_TIME    0.5f   /**< s */

#define DE__SLEEP_LINEAR_CVAR  "physics.sleep.linear_threshold"
#define DE__SLEEP_ANGULAR_CVAR "physics.sleep.angular_threshold"
#define DE__SLEEP_TIME_CVAR    "physics.sleep.time"

/** "No island" (de__islands.sleep_island of an awake body, de__islands.island_of_slot[0]). */
#define DE__ISLAND_NONE 0xFFFFFFFFu

/* ================================ Island record ======================================== */

/** de__island.flags. */
enum {
    /** A moving kinematic body touches the island: it cannot sleep this step. */
    DE__ISLAND_KEEP_AWAKE = 1u << 0,
    /** A body of the island lacks DE_BODY_FLAG_ALLOW_SLEEP (set by the sleep evaluation). */
    DE__ISLAND_NO_SLEEP = 1u << 1,
    /** The island went to sleep at the end of this step. */
    DE__ISLAND_SLEPT = 1u << 2
};

/**
 * One island of this step. A POD. `origin` is the island's solve origin for large worlds
 * (DENG-222, physics/large_world.h): the f64 position of body `id`, set by de__islands_build.
 */
typedef struct de__island {
    u32 id;               /**< lowest body index of the island */
    u32 first;            /**< first entry of the island in de__islands.island_bodies */
    u32 body_count;       /**< bodies (entries from `first`) */
    u32 constraint_count; /**< records whose sides are both in this island */
    u32 flags;            /**< DE__ISLAND_* */
    f32 min_sleep_time;   /**< smallest sleep timer of its bodies (sleep evaluation) */
    de_dvec3 origin;      /**< DENG-222: solve origin = position of body `id` this step */
} de__island;

/** Counters of the last step. */
typedef struct de__islands_stats {
    u32 islands;         /**< islands of awake bodies built this step */
    u32 largest;         /**< bodies of the largest island */
    u32 sleeping_bodies; /**< bodies asleep after the step */
    u32 slept_islands;   /**< islands that went to sleep this step */
    u32 woken_islands;   /**< islands woken at the start of this step */
    u32 regathers;       /**< extra gathers because a constraint woke an island */
} de__islands_stats;

/* ================================ State ================================================ */

typedef struct de__islands {
    u32 capacity; /**< max_bodies */
    f32 sleep_linear;
    f32 sleep_angular;
    f32 sleep_time;

    /* Persistent, per body index. */
    u8 *sleeping;      /**< 1 = asleep */
    u32 *sleep_island; /**< id of the island it fell asleep with; DE__ISLAND_NONE when awake */
    f32 *timer;        /**< seconds below the sleep thresholds */
    de_dvec3 *rest_position; /**< pose when it fell asleep: a change wakes it */
    de_quat *rest_rotation;

    /* Wake requests, per island id (= body index). */
    u8 *wake_mark;
    u32 *wake_list; /**< marked ids, in request order (processed as a set) */
    u32 wake_count;

    /* Per step, per solver slot ([0] is the static slot). */
    u32 *parent;         /**< union-find */
    u32 *island_of_slot; /**< island index; DE__ISLAND_NONE for slot 0 and kinematic bodies */

    /* Per step. */
    de__island *islands; /**< [capacity], ascending id */
    u32 island_count;
    u32 *island_bodies; /**< [capacity] body indices, grouped by island, ascending in each */

    de__islands_stats stats;
} de__islands;

/** Bytes of the state and its arrays for `capacity` bodies (solver.c carves them from its block).
 */
size_t de__islands_bytes(u32 capacity);
/** Lays the state out in `mem` (de__islands_bytes(capacity) bytes, 16-byte aligned, zeroed) and
 *  reads the cvars. Returns the state (at the start of `mem`). */
de__islands *de__islands_init(void *mem, u32 capacity);

/* ================================ Step (islands.c, sleep.c) ============================ */

/**
 * The solver's view of the awake set: processes the pending wake requests, then calls
 * de__solver_add_awake for every live non-static body that is not asleep, ascending. Called by
 * de__islands_update and again by de__solver_prepare after a constraint woke an island.
 */
void de__islands_collect_awake(de_physics_world *p, de__solver *s);
/** true when a wake request is pending (the solver then gathers again). */
static inline bool de__islands_wake_pending(const de__islands *is) {
    return is->wake_count != 0u;
}
/**
 * From de__solver_add_constraint: a constraint between `body` (asleep) and an awake dynamic body
 * or a moving kinematic body. Requests the wake of body's island.
 */
void de__islands_request_wake(de__islands *is, u32 body);
/** Union-find over the gathered records: fills the island records of this step. */
void de__islands_build(de__solver *s);
/** sleep.c: sleep timers and island evaluation after the step; islands go to sleep. */
void de__sleep_finalize(de_physics_world *p, de__solver *s);
/** sleep.c: reads the physics.sleep.* cvars (registers them) into the state; at create. */
void de__sleep_read_settings(de__islands *is);

/* ================================ Body callbacks and wake (sleep.c) ===================== */

/** A body was created at `index` (index reuse): it starts awake with a zero timer. */
void de__islands_body_added(de__islands *is, u32 index);
/** A body is being destroyed: when it sleeps, its island is woken. */
void de__islands_body_removed(de__islands *is, u32 index);
/**
 * Wakes body `index` and its island at the start of the next step (de_body_is_sleeping is false
 * from now on). For joint create and destroy (DENG-220) and other world changes that the
 * solver cannot see. No effect on an awake or dead body, or before the first step.
 */
void de__islands_wake_body(de_physics_world *p, u32 index);
/** true when live body `index` is asleep and no wake request is pending for its island. */
bool de__islands_body_sleeping(const de_physics_world *p, u32 index);
/** The islands state, or NULL before the solver exists. */
de__islands *de__islands_get(const de_physics_world *p);
void de__islands_stats_get(const de_physics_world *p, de__islands_stats *out);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_ISLANDS_H */
