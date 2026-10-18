/*
 * solver2d_internal.h: the sub-stepped soft-step solver of de_physics2d_step, its islands,
 * sleeping, deterministic graph coloring and bullets (DENG-254, SPEC §13.3, §13.1, §13.2, §5.5,
 * §5.7, §8). Internal: include as "physics/2d/solver2d_internal.h". The 2D port of the 3D solver
 * (physics/solver/solver_internal.h, islands.h, graph_color.h, ccd_internal.h); the structure and
 * the float operations follow the current 3D code (DENG-217, DENG-218, DENG-219, DENG-222,
 * DENG-398, DENG-404), in 2D: one angular velocity, a 2D rotation, one friction tangent.
 *
 * One step (step2d.c, after the collision pipeline):
 *
 *   de__solver2d_begin            ensure the state, per-step constants (substep h, softness)
 *   de__islands2d_update          wake requests, then the awake set (ascending body index)
 *   de__solver2d_prepare          solver bodies, gather (again when a constraint woke an
 *                                 island), islands, coloring, prepare
 *   substeps x:
 *     de__solver2d_integrate_velocities   gravity x gravity_scale, damping 1/(1+h c), clamps
 *     de__solver2d_warm_start
 *     de__solver2d_solve                  with position bias (soft step); extra class passes first
 *     de__solver2d_integrate_positions    dp += h v, dq = rot(w h) dq (de_dsincos)
 *     de__solver2d_relax                  without position bias
 *   de__solver2d_restitution
 *   de__solver2d_store            impulses back into the manifolds
 *   de__bullets2d_solve           bullet2d.c: time of impact of bullets against static and
 *                                 kinematic bodies (conservative advancement)
 *   de__solver2d_finalize         velocities and f64 positions to the body pool, AABBs, sync
 *   de__islands2d_finalize        sleep2d.c: timers, islands go to sleep
 *
 * Constraint stages run as graph-colored batches and the per-body kernels in fixed ranges, on
 * p->tasks (graph_color2d.c). Results never depend on the worker count or on tasks == NULL.
 *
 * Memory. The first step creates the state from the world allocator: one block (solver bodies
 * max_bodies + 1, body -> slot map, awake list, the islands state, the coloring bitsets), the
 * record array with the coloring's order, and the contact class's prepared data (contacts
 * max_manifolds). Steps allocate nothing.
 *
 * Settings come from the cvars physics2d.solver.* and physics2d.sleep.*, read when the state is
 * created (tests may change de__solver2d.settings, .islands and .coloring afterwards). They are
 * their own cvars, not the 3D physics.solver.* ones, so tuning one world kind leaves the other.
 */
#ifndef DE_PHYSICS_SOLVER2D_INTERNAL_H
#define DE_PHYSICS_SOLVER2D_INTERNAL_H

#include "physics/2d/constraint2d.h"

DE_EXTERN_C_BEGIN

/* ================================ Settings ============================================= */

#define DE__SOLVER2D_DEFAULT_CONTACT_HERTZ         30.0f
#define DE__SOLVER2D_DEFAULT_CONTACT_DAMPING_RATIO 10.0f
#define DE__SOLVER2D_DEFAULT_CONTACT_PUSH_OUT      3.0f
#define DE__SOLVER2D_DEFAULT_RESTITUTION_THRESHOLD 1.0f
#define DE__SOLVER2D_DEFAULT_MAX_LINEAR_SPEED      400.0f
/** Most rotation per substep (rad): the angular speed clamp is this / h (Box2D v3). */
#define DE__SOLVER2D_MAX_ROTATION_PER_SUBSTEP (0.25f * 3.14159265359f)

#define DE__SOLVER2D_CONTACT_HERTZ_CVAR         "physics2d.solver.contact_hertz"
#define DE__SOLVER2D_CONTACT_DAMPING_RATIO_CVAR "physics2d.solver.contact_damping_ratio"
#define DE__SOLVER2D_CONTACT_PUSH_OUT_CVAR      "physics2d.solver.contact_push_out"
#define DE__SOLVER2D_RESTITUTION_THRESHOLD_CVAR "physics2d.solver.restitution_threshold"
#define DE__SOLVER2D_MAX_LINEAR_SPEED_CVAR      "physics2d.solver.max_linear_speed"
#define DE__SOLVER2D_WARM_START_CVAR            "physics2d.solver.warm_start"
#define DE__SOLVER2D_GRAIN_CVAR                 "physics2d.solver.grain"
#define DE__SOLVER2D_BULLETS_CVAR               "physics2d.solver.bullets"

#define DE__SLEEP2D_DEFAULT_LINEAR  0.05f  /**< m/s */
#define DE__SLEEP2D_DEFAULT_ANGULAR 0.035f /**< rad/s */
#define DE__SLEEP2D_DEFAULT_TIME    0.5f   /**< s */
#define DE__SLEEP2D_LINEAR_CVAR     "physics2d.sleep.linear_threshold"
#define DE__SLEEP2D_ANGULAR_CVAR    "physics2d.sleep.angular_threshold"
#define DE__SLEEP2D_TIME_CVAR       "physics2d.sleep.time"

/** Solver tuning, read from the cvars when the state is created. */
struct de__solver2d_settings {
    /** Contact stiffness (Hz); the step uses min(this, 0.25 * substep rate). Contacts with a
     *  static or kinematic side use twice the result. */
    f32 contact_hertz;
    f32 contact_damping_ratio; /**< 10: heavily damped */
    f32 contact_push_out;      /**< most push-out speed of a penetrating contact (m/s) */
    f32 restitution_threshold; /**< no bounce below this approach speed (m/s) */
    f32 max_linear_speed;      /**< dynamic bodies are clamped to this speed (m/s) */
    bool warm_start;           /**< apply last step's impulses first */
    bool bullets;              /**< run the bullet pass (bullet2d.c) */
};

/* ================================ Islands ============================================== */

/** "No island" (sleep island of an awake body, island of slot 0 and of kinematic bodies). */
#define DE__ISLAND2D_NONE 0xFFFFFFFFu

/** de__island2d.flags. */
enum {
    DE__ISLAND2D_KEEP_AWAKE = 1u << 0, /**< a moving kinematic body touches it */
    DE__ISLAND2D_NO_SLEEP = 1u << 1,   /**< a body lacks DE_BODY_FLAG_ALLOW_SLEEP */
    DE__ISLAND2D_SLEPT = 1u << 2       /**< went to sleep at the end of this step */
};

/** One island of this step (a POD). */
typedef struct de__island2d {
    u32 id;               /**< lowest body index of the island */
    u32 first;            /**< first entry in de__islands2d.island_bodies */
    u32 body_count;       /**< bodies (entries from `first`) */
    u32 constraint_count; /**< records with a writable side in this island */
    u32 flags;            /**< DE__ISLAND2D_* */
    f32 min_sleep_time;   /**< smallest sleep timer of its bodies */
    de__d2 origin;        /**< solve origin: the f64 position of body `id` this step */
} de__island2d;

/** Counters of the last step. */
typedef struct de__islands2d_stats {
    u32 islands;         /**< islands of awake bodies built this step */
    u32 largest;         /**< bodies of the largest island */
    u32 sleeping_bodies; /**< bodies asleep after the step */
    u32 slept_islands;   /**< islands that went to sleep this step */
    u32 woken_islands;   /**< islands woken at the start of this step */
    u32 regathers;       /**< extra gathers because a constraint woke an island */
} de__islands2d_stats;

typedef struct de__islands2d {
    u32 capacity; /**< max_bodies */
    f32 sleep_linear;
    f32 sleep_angular;
    f32 sleep_time;

    /* Persistent, per body index. */
    u8 *sleeping;          /**< 1 = asleep */
    u32 *sleep_island;     /**< id of the island it fell asleep with; NONE when awake */
    f32 *timer;            /**< seconds below the sleep thresholds */
    de__d2 *rest_position; /**< pose when it fell asleep: a change wakes it */
    de_rot2d *rest_rotation;

    /* Wake requests, per island id (= body index). */
    u8 *wake_mark;
    u32 *wake_list;
    u32 wake_count;

    /* Per step, per solver slot ([0] is the static slot). */
    u32 *parent;         /**< union-find */
    u32 *island_of_slot; /**< island index; NONE for slot 0 and kinematic bodies */

    /* Per step. */
    de__island2d *islands; /**< [capacity], ascending id */
    u32 island_count;
    u32 *island_bodies; /**< [capacity] body indices grouped by island, ascending in each */

    de__islands2d_stats stats;
} de__islands2d;

/* ================================ Coloring ============================================= */

/** Colors before overflow (SPEC §13.2 graph-colored batches). */
#define DE__GRAPH2D_MAX_COLORS 24u
/** de__constraint2d.color of a record in the overflow set (solved serially after the colors). */
#define DE__GRAPH2D_OVERFLOW DE__GRAPH2D_MAX_COLORS
/** Records per de_task_parallel_for chunk (cvar physics2d.solver.grain). Never machine-dependent
 *  (SPEC §5.7 rule 5). */
#define DE__SOLVER2D_DEFAULT_GRAIN 64u
/** Solver bodies (and islands) per chunk of the per-body kernels. */
#define DE__SOLVER2D_BODY_GRAIN 256u

/** The stages a constraint class runs. */
typedef enum de__solver2d_stage {
    DE__STAGE2D_PREPARE,
    DE__STAGE2D_WARM_START,
    DE__STAGE2D_SOLVE,
    DE__STAGE2D_RELAX,
    DE__STAGE2D_RESTITUTION,
    DE__STAGE2D_STORE
} de__solver2d_stage;

typedef struct de__coloring2d_stats {
    u32 colors;   /**< colors in use (without the overflow set) */
    u32 overflow; /**< records in the overflow set */
    u32 largest;  /**< records of the largest color */
    u32 chunks;   /**< parallel chunks of one pass over every color (grain-sized) */
} de__coloring2d_stats;

typedef struct de__coloring2d {
    u32 grain;         /**< records per chunk (cvar at create; tests may set) */
    u32 body_grain;    /**< bodies and islands per chunk (DE__SOLVER2D_BODY_GRAIN; tests may set) */
    u32 slot_capacity; /**< solver slots covered by each bitset (max_bodies + 1) */
    u32 words;         /**< u64 words per color bitset */
    u64 *bits;         /**< [DE__GRAPH2D_MAX_COLORS * words] */
    u32 *stamp;        /**< [slot_capacity] scratch of de__graph2d_color_validate */
    u32 *order;        /**< [order_capacity] record indices, grouped by color */
    u32 order_capacity;
    u32 start[DE__GRAPH2D_MAX_COLORS + 2u]; /**< color k: order[start[k], start[k + 1]) */
    u32 color_count;
    de__coloring2d_stats stats;
} de__coloring2d;

/* ================================ Solver state ========================================= */

/** Counters of the last step. */
typedef struct de__solver2d_stats {
    u32 awake_bodies;   /**< solver bodies (without the static slot) */
    u32 constraints;    /**< records solved */
    u32 contact_points; /**< contact points solved */
    u32 dropped;        /**< constraints refused for capacity this step */
    u32 class_count[DE__CONSTRAINT2D_CLASS_COUNT];
} de__solver2d_stats;

/** Counters of the bullet pass of the last step. */
typedef struct de__bullets2d_stats {
    u32 bullets;        /**< bullets swept */
    u32 candidates;     /**< (bullet, body) pairs swept */
    u32 hits;           /**< bullets moved back to their time of impact */
    u32 fallbacks;      /**< pairs swept with the center-of-mass circle */
    u32 iterations;     /**< conservative-advancement steps, all pairs */
    u32 max_iterations; /**< most steps one pair needed */
    u32 capped;         /**< pairs that reached DE__BULLET2D_MAX_ITER */
} de__bullets2d_stats;

typedef struct de__solver2d_class_slot {
    const de__constraint2d_class *cls; /**< NULL = unused */
    void *state;
    u32 capacity;   /**< most records per step */
    u32 count;      /**< records this step */
    u32 iterations; /**< solve and relax passes per substep (1 = one pass) */
} de__solver2d_class_slot;

struct de__solver2d {
    const de_allocator *alloc;
    de_physics2d_world *p;
    de__solver2d_settings settings;
    de__solver2d_ctx ctx;

    u32 body_capacity;         /**< max_bodies */
    de__solver_body2d *bodies; /**< [body_capacity + 1]; [0] = static slot */
    u32 body_count;            /**< slots in use this step, static slot included */
    u32 *slot_of;              /**< [body_capacity] body index -> slot (0 = not awake) */
    u32 *awake;                /**< [body_capacity] awake body indices, ascending */
    u32 awake_count;

    de__solver2d_class_slot classes[DE__CONSTRAINT2D_CLASS_COUNT];
    de__constraint2d *constraints; /**< [constraint_capacity], this step, stable order */
    u32 constraint_count;
    u32 constraint_capacity;

    de__solver2d_stats stats;
    de__bullets2d_stats bullet_stats;
    bool active; /**< between prepare and finalize */

    de__islands2d *islands;   /**< in `block` */
    de__coloring2d *coloring; /**< in `block`; its order shares the record allocation */

    void *block;
    size_t block_size;
};

/* ================================ Lifetime (solver2d.c) ================================ */

static inline de__solver2d *de__solver2d_get(const de_physics2d_world *p) {
    return (de__solver2d *)de__p2d_ext_get(p, DE__P2D_EXT_SOLVER);
}
/**
 * The solver state, created if needed (with the contacts state, whose capacity it needs), with
 * the cvars and the contact class.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL world) or DE_ERR_OUT_OF_MEMORY (nothing is left).
 */
de_result de__solver2d_ensure(de_physics2d_world *p, de__solver2d **out);

/* ================================ Stages (solver2d.c) ================================== */

/** Ensures the state and fills the per-step constants. NULL when the state cannot be created
 *  (the error context says why); bodies then do not move this step. */
de__solver2d *de__solver2d_begin(de_physics2d_world *p, f32 dt);
/** islands2d.c calls this for each awake body, in ascending body index. Returns the slot. */
u32 de__solver2d_add_awake(de__solver2d *s, u32 body_index);
void de__solver2d_prepare(de__solver2d *s);
void de__solver2d_integrate_velocities(de__solver2d *s);
void de__solver2d_warm_start(de__solver2d *s);
void de__solver2d_solve(de__solver2d *s);
void de__solver2d_integrate_positions(de__solver2d *s);
void de__solver2d_relax(de__solver2d *s);
void de__solver2d_restitution(de__solver2d *s);
void de__solver2d_store(de__solver2d *s);
/** Velocities, f64 positions and rotations back to the body pool, world AABBs, broadphase sync;
 *  clears the body -> slot map. */
void de__solver2d_finalize(de__solver2d *s);
void de__solver2d_stats_get(const de_physics2d_world *p, de__solver2d_stats *out);

/* ================================ Contact class (contact_solver2d.c) =================== */

/** Registers the contact class (DE__CONSTRAINT2D_CLASS_CONTACT); called by create. */
de_result de__contact_solver2d_register(de_physics2d_world *p);
/** Contact points prepared in the last step. */
u32 de__contact_solver2d_points(const de_physics2d_world *p);

/* ================================ Islands (islands2d.c, sleep2d.c) ===================== */

size_t de__islands2d_bytes(u32 capacity);
/** Lays the state out in zeroed, 16-byte aligned `mem` and reads the sleep cvars. */
de__islands2d *de__islands2d_init(void *mem, u32 capacity);
/** Wake requests (ended contacts with removed bodies, user changes), then the awake set. */
void de__islands2d_update(de_physics2d_world *p, de__solver2d *s);
/** Processes the pending wake requests, then adds every live non-static body that is not asleep
 *  to the solver, ascending. */
void de__islands2d_collect_awake(de_physics2d_world *p, de__solver2d *s);
static inline bool de__islands2d_wake_pending(const de__islands2d *is) {
    return is->wake_count != 0u;
}
/** Requests the wake of body's island (no effect when it is awake). */
void de__islands2d_request_wake(de__islands2d *is, u32 body);
/** Union-find over the gathered records: the islands of this step and their solve origins. */
void de__islands2d_build(de__solver2d *s);
/** After finalize: sleep timers; islands that rested long enough go to sleep (sleep2d.c). */
void de__islands2d_finalize(de_physics2d_world *p, de__solver2d *s);
void de__sleep2d_read_settings(de__islands2d *is);
void de__islands2d_body_added(de__islands2d *is, u32 index);
void de__islands2d_body_removed(de__islands2d *is, u32 index);
/** true when live body `index` sleeps and no wake request is pending for its island. */
bool de__islands2d_body_sleeping(const de_physics2d_world *p, u32 index);
de__islands2d *de__islands2d_get(const de_physics2d_world *p);
void de__islands2d_stats_get(const de_physics2d_world *p, de__islands2d_stats *out);

/* ================================ Coloring (graph_color2d.c) =========================== */

size_t de__coloring2d_bytes(u32 slot_capacity);
/** Lays the state out in zeroed, 16-byte aligned `mem` and reads the grain cvar. */
de__coloring2d *de__coloring2d_init(void *mem, u32 slot_capacity);
static inline void de__coloring2d_bind_order(de__coloring2d *c, u32 *order, u32 capacity) {
    c->order = order;
    c->order_capacity = capacity;
}
/** Greedy coloring of this step's records in record order (after gather). */
void de__graph2d_color(de__solver2d *s);
/** Every record once, under its color; no writable body twice in one color. */
bool de__graph2d_color_validate(const de__solver2d *s);
/** Runs `stage` over every record: colors in order (chunks of `grain` on p->tasks), then the
 *  overflow set serially. */
void de__solver2d_run_stage(de__solver2d *s, de__solver2d_stage stage);
/** Runs `stage` over the records of the classes whose iterations exceed `pass` only. */
void de__solver2d_run_stage_extra(de__solver2d *s, de__solver2d_stage stage, u32 pass);
/** fn(s, begin, end) over solver slots [1, body_count) in fixed ranges of `body_grain`. */
void de__solver2d_for_bodies(de__solver2d *s, void (*fn)(de__solver2d *s, u32 begin, u32 end));

/* ================================ Bullets (bullet2d.c) ================================= */

/** Gap (m) a bullet keeps from what it hits at its time of impact. */
#define DE__BULLET2D_LINEAR_SLOP 0.005f
/** Most conservative-advancement steps per (bullet, target) pair. */
#define DE__BULLET2D_MAX_ITER 20u
/** Radius of the start-touching fallback circle, as a fraction of the bullet's smallest half
 *  extent (Box2D v3). */
#define DE__BULLET2D_FALLBACK_SCALE 0.25f

/**
 * After de__solver2d_store and before finalize: every awake dynamic body with
 * DE_BODY2D_FLAG_BULLET (not a trigger), in ascending body index, is swept from its start pose to
 * its end pose (center of mass along a line, angle at a constant rate) against static and
 * kinematic bodies (at their end pose). If it would pass through one, its solver body is moved back
 * to the pose at the time of impact (gap DE__BULLET2D_LINEAR_SLOP) and keeps its velocity; the
 * next step's speculative contact stops it. See bullet2d.c.
 */
void de__bullets2d_solve(de_physics2d_world *p, de__solver2d *s);
void de__bullets2d_stats_get(const de_physics2d_world *p, de__bullets2d_stats *out);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_SOLVER2D_INTERNAL_H */
