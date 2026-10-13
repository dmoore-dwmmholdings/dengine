/*
 * solver_internal.h: the sub-stepped soft-step (TGS-style) solver of de_physics_step (DENG-217,
 * SPEC §13.1, §13.2, §5.5, §5.7). Internal: include as "physics/solver/solver_internal.h".
 *
 * One step (step.c calls the stages in this order):
 *
 *   de__solver_begin          ensure the state, per-step constants (substep h, softness)
 *   de__islands_update        islands.c: which bodies are awake (stub: every non-static body)
 *   de__solver_prepare        solver bodies of the awake bodies, gather + prepare constraints
 *   substeps x:
 *     de__solver_integrate_velocities   gravity x gravity_scale, damping v *= 1/(1+h c), clamps
 *     de__solver_warm_start
 *     de__solver_solve                  with position bias (soft step)
 *     de__solver_integrate_positions    dp += h v, dq = integrate(dq, w, h)
 *     de__solver_relax                  without position bias
 *   de__solver_restitution
 *   de__solver_store          impulses back into the manifolds (warm start of the next step)
 *   de__ccd_solve             ccd.c (stub; DENG-219)
 *   de__solver_finalize       velocities and f64 positions back to the body pool, AABBs, sync
 *   de__islands_finalize      islands.c (stub; DENG-218 sleep timers)
 *
 * The stage functions of solver.c run serially in stable constraint order; DENG-218 owns
 * solver.c in wave 2 and replaces them with graph-colored parallel batches without touching
 * step.c. integrate.c provides the per-body kernels as ranges, so they can be split too.
 *
 * Memory. The state is created by the first step (or de__solver_ensure / a class registration)
 * from the world allocator: the solver bodies (max_bodies + 1), the body -> slot map, the awake
 * list, the constraint records (sum of the classes' capacities) and the contact class's prepared
 * data (contacts max_manifolds). Steps allocate nothing.
 *
 * Settings come from cvars `physics.solver.*`, read when the state is created (tests may change
 * de__solver.settings directly afterwards).
 */
#ifndef DE_PHYSICS_SOLVER_INTERNAL_H
#define DE_PHYSICS_SOLVER_INTERNAL_H

#include "physics/solver/constraint.h"

DE_EXTERN_C_BEGIN

/* ================================ Settings ============================================= */

#define DE__SOLVER_DEFAULT_CONTACT_HERTZ         30.0f
#define DE__SOLVER_DEFAULT_CONTACT_DAMPING_RATIO 10.0f
#define DE__SOLVER_DEFAULT_CONTACT_PUSH_OUT      3.0f
#define DE__SOLVER_DEFAULT_RESTITUTION_THRESHOLD 1.0f
#define DE__SOLVER_DEFAULT_MAX_LINEAR_SPEED      400.0f
/** Most rotation per substep (rad): the angular speed clamp is this / h (Box2D v3). */
#define DE__SOLVER_MAX_ROTATION_PER_SUBSTEP (0.25f * 3.14159265359f)

#define DE__SOLVER_CONTACT_HERTZ_CVAR         "physics.solver.contact_hertz"
#define DE__SOLVER_CONTACT_DAMPING_RATIO_CVAR "physics.solver.contact_damping_ratio"
#define DE__SOLVER_CONTACT_PUSH_OUT_CVAR      "physics.solver.contact_push_out"
#define DE__SOLVER_RESTITUTION_THRESHOLD_CVAR "physics.solver.restitution_threshold"
#define DE__SOLVER_MAX_LINEAR_SPEED_CVAR      "physics.solver.max_linear_speed"
#define DE__SOLVER_WARM_START_CVAR            "physics.solver.warm_start"

/** Solver tuning. Read from the cvars when the state is created. */
struct de__solver_settings {
    /** Contact stiffness (Hz); the step uses min(this, 0.25 * substep rate). Contacts with a
     *  static or kinematic side use twice the result. */
    f32 contact_hertz;
    f32 contact_damping_ratio; /**< contact damping ratio (10: heavily damped) */
    f32 contact_push_out;      /**< most push-out speed of a penetrating contact (m/s) */
    f32 restitution_threshold; /**< no bounce below this approach speed (m/s) */
    f32 max_linear_speed;      /**< dynamic bodies are clamped to this speed (m/s) */
    bool warm_start;           /**< apply last step's impulses first */
};

/* ================================ State ================================================ */

/** Counters of the last step, for tests, the debug overlay and benchmarks. */
typedef struct de__solver_stats {
    u32 awake_bodies;                            /**< solver bodies (without the static slot) */
    u32 constraints;                             /**< records solved */
    u32 contact_points;                          /**< contact points solved */
    u32 dropped;                                 /**< constraints refused for capacity this step */
    u32 class_count[DE__CONSTRAINT_CLASS_COUNT]; /**< records per class */
} de__solver_stats;

typedef struct de__solver_class_slot {
    const de__constraint_class *cls; /**< NULL = unused */
    void *state;
    u32 capacity; /**< most records per step */
    u32 count;    /**< records this step */
} de__solver_class_slot;

struct de__solver {
    const de_allocator *alloc;
    de_physics_world *p;
    de__solver_settings settings;
    de__solver_ctx ctx; /**< this step's constants */

    u32 body_capacity;       /**< max_bodies */
    de__solver_body *bodies; /**< [body_capacity + 1]; [0] = static slot */
    u32 body_count;          /**< slots in use this step, static slot included */
    u32 *slot_of;            /**< [body_capacity] body index -> slot (0 = not awake) */
    u32 *awake;              /**< [body_capacity] awake body indices, ascending */
    u32 awake_count;

    de__solver_class_slot classes[DE__CONSTRAINT_CLASS_COUNT];
    de__constraint *constraints; /**< [constraint_capacity], this step, stable order */
    u32 constraint_count;
    u32 constraint_capacity;

    de__solver_stats stats;
    bool active; /**< between de__solver_prepare and de__solver_finalize */

    void *block; /**< bodies, slot_of, awake */
    size_t block_size;
};

/* ================================ Lifetime ============================================= */

/** The solver state, or NULL before it exists. */
static inline de__solver *de__solver_get(const de_physics_world *p) {
    return (de__solver *)de__phys_ext_get(p, DE__PHYS_EXT_SOLVER);
}
/**
 * The solver state, created if needed (with the contacts state, which it needs for the contact
 * class's capacity). Registers the cvars and the contact class.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL world) or DE_ERR_OUT_OF_MEMORY (nothing is left).
 */
de_result de__solver_ensure(de_physics_world *p, de__solver **out);

/* ================================ Stages (step.c) ====================================== */

/** Ensures the state and fills the per-step constants. NULL when the state cannot be created
 *  (the error context says why); the step then only advances the tick. */
de__solver *de__solver_begin(de_physics_world *p, f32 dt);
/** islands.c calls this for each awake body, in ascending body index. Returns the slot. */
u32 de__solver_add_awake(de__solver *s, u32 body_index);
/** Builds the solver bodies of the awake bodies, gathers and prepares every constraint. */
void de__solver_prepare(de__solver *s);
void de__solver_integrate_velocities(de__solver *s);
void de__solver_warm_start(de__solver *s);
void de__solver_solve(de__solver *s);
void de__solver_integrate_positions(de__solver *s);
void de__solver_relax(de__solver *s);
void de__solver_restitution(de__solver *s);
void de__solver_store(de__solver *s);
/** Velocities, f64 positions and rotations back to the body pool, world AABBs, broadphase
 *  sync; clears the body -> slot map. */
void de__solver_finalize(de__solver *s);
void de__solver_stats_get(const de_physics_world *p, de__solver_stats *out);

/* ================================ Kernels (integrate.c) ================================= */

/** Solver body of awake body `index` at the start of a step (velocities, inverse inertia,
 *  per-step integration constants). */
void de__integrate_init_body(const de__solver *s, u32 body_index, de__solver_body *out);
/** Velocity integration of slots [begin, end) (slot 0 is never touched). */
void de__integrate_velocities(de__solver *s, u32 begin, u32 end);
/** Position integration of slots [begin, end). */
void de__integrate_positions(de__solver *s, u32 begin, u32 end);
/** Writes slots [begin, end) back to the body pool (velocities, f64 position, rotation, AABB). */
void de__integrate_finalize(de__solver *s, u32 begin, u32 end);

/* ================================ Contact class (contact_solver.c) ====================== */

/** Registers the contact constraint class (DE__CONSTRAINT_CLASS_CONTACT); called by create. */
de_result de__contact_solver_register(de_physics_world *p);
/** Contact points prepared in the last step. */
u32 de__contact_solver_points(const de_physics_world *p);

/* ================================ Hooks (islands.c, ccd.c) ============================== */

/**
 * islands.c (DENG-218 replaces it): once per step after the collision pipeline and before
 * de__solver_prepare. Calls de__solver_add_awake for every body to simulate this step, in
 * ascending body index. The stub adds every live dynamic and kinematic body (no sleeping).
 */
void de__islands_update(de_physics_world *p, de__solver *s);
/**
 * islands.c (DENG-218): once per step after de__solver_finalize (the body pool holds the new
 * velocities and positions): sleep timers, island splitting. The stub does nothing.
 */
void de__islands_finalize(de_physics_world *p, de__solver *s);
/**
 * ccd.c (DENG-219 replaces it): once per step after the world AABBs are refreshed and before the
 * collision pipeline (per-body speculative margins, swept boxes). The stub does nothing.
 */
void de__ccd_prepare(de_physics_world *p, f32 dt);
/**
 * ccd.c (DENG-219): once per step after de__solver_store and before de__solver_finalize. May
 * change the solver bodies' dp / dq / v / w of bullets (time-of-impact clamps). `s` may be NULL
 * when the solver could not run. The stub does nothing.
 */
void de__ccd_solve(de_physics_world *p, de__solver *s);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_SOLVER_INTERNAL_H */
