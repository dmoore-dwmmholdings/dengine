/*
 * ccd_internal.h: continuous collision of de_physics_step (DENG-219, SPEC §13.1, §13.2, §5.7).
 * Internal: include as "physics/ccd_internal.h".
 *
 * Two parts, both called from step.c through the hooks of solver_internal.h:
 *
 *   1. Speculative contacts for every body (de__ccd_prepare, before the collision pipeline).
 *      Each live body gets a predicted velocity for the step (its velocity plus one step of
 *      gravity when dynamic) and an angular reach (|w| times the largest distance of its shape
 *      from its center of mass). Then:
 *      - the broadphase grows the body's box by velocity * dt toward the motion and by the
 *        angular reach * dt on every side, both clamped to `predict_max`
 *        (de__broadphase_set_predict), so pairs that may meet during the step are found before
 *        they touch. Growth only adds pairs; the output stays sorted by (a, b);
 *      - the narrowphase of a pair runs with the speculative distance
 *        clamp(approach bound * dt, base, speculative_max), where the bound is
 *        |v_b - v_a| + reach_a + reach_b and `base` is the contacts' margin
 *        (physics.contact.speculative, 2 cm). Then each point beyond `base` is kept only when
 *        its separation is <= the relative normal approach speed at the point * dt, and only
 *        when the two bodies may really come within the slop during the step
 *        (de__ccd_pair_may_touch). The solver closes a speculative gap along the contact's
 *        fixed normal; without the second test a sphere rolling fast past a box corner would
 *        be stopped by a corner it never reaches (a "ghost" stop).
 *        (de__contacts_pair_speculative, de__contacts_filter_speculative in contact/manifold.c.)
 *      A resting or slow pair (approach * dt <= base) is exactly as before DENG-219.
 *
 *   2. Swept time of impact for bullets (de__ccd_solve, after the solver stored its impulses and
 *      before finalize). Every awake dynamic body with DE_BODY_FLAG_CCD, in ascending body index,
 *      is swept from its start pose to its end pose (center of mass along a line, rotation by
 *      normalized linear interpolation of the step's rotation) against static and kinematic
 *      bodies and dynamic bodies without the flag, each held at its end pose. Conservative
 *      advancement on GJK distance (narrow/gjk.c) finds the first time the gap falls to
 *      DE__CCD_LINEAR_SLOP, in at most DE__CCD_MAX_ITER steps per (sub-)shape pair. A hit
 *      counts unless the solver already held the bullet: its end pose is on the same side of the
 *      target (same separating direction), at most the slop deep (EPA), and not closing on it
 *      by more than the slop per step. If the earliest counted time t < 1, the bullet's solver
 *      body is moved back to its pose at t (dp and dq scaled to t) and keeps its velocity; the
 *      next step's speculative contact (a positive gap, solved rigidly) stops it.
 *      A target that is already within the slop at t = 0 is swept against a small sphere at the
 *      bullet's center of mass instead (Box2D v3; radius DE__CCD_FALLBACK_SCALE times the
 *      smallest half extent), which may close half its starting gap and may not enter the
 *      target. So bullets that rest or slide on something do not stop, and a bullet pressed
 *      into a thin wall still cannot get its center through. Bullets are not swept against
 *      each other.
 *
 * State: the DE__PHYS_EXT_CCD slot, created by the first step (de__ccd_ensure) from the world
 * allocator: the settings and two per-body arrays (predicted velocity, angular reach). Steps
 * allocate nothing afterwards.
 *
 * Settings come from cvars, read when the state is created (tests may change
 * de__ccd.settings directly afterwards):
 *   physics.ccd.enabled          (bool, default on)   the bullet pass
 *   physics.ccd.speculative_max  (float, m, default 1) largest speculative distance of a pair
 *   physics.ccd.predict_max      (float, m, default 1) largest predictive growth of a box side
 * speculative_max = predict_max = 0 restore the fixed-margin behaviour of DENG-135.
 *
 * Determinism (SPEC §5.7): + - * / and de_dsqrt / de_dsqrt64 only, bodies in ascending index,
 * bounded loops. The bullet pass takes the minimum time over its candidates, so the broadphase
 * visiting order does not matter. Threading: prepare and the bullet pass run on the step's
 * thread; the narrowphase reads the per-body arrays from many threads.
 */
#ifndef DE_PHYSICS_CCD_INTERNAL_H
#define DE_PHYSICS_CCD_INTERNAL_H

#include "physics/physics_internal.h"

DE_EXTERN_C_BEGIN

/* ================================ Settings ============================================= */

#define DE__CCD_DEFAULT_SPECULATIVE_MAX 1.0f
#define DE__CCD_DEFAULT_PREDICT_MAX     1.0f
#define DE__CCD_ENABLED_CVAR            "physics.ccd.enabled"
#define DE__CCD_SPECULATIVE_MAX_CVAR    "physics.ccd.speculative_max"
#define DE__CCD_PREDICT_MAX_CVAR        "physics.ccd.predict_max"

/** Gap (m) a bullet keeps from what it hits: its pose at the time of impact is this far away. */
#define DE__CCD_LINEAR_SLOP 0.005f
/** Most conservative-advancement steps per (sub-)shape pair of the bullet pass. */
#define DE__CCD_MAX_ITER 20u
/** Radius of the start-touching fallback sphere, as a fraction of the bullet's smallest half
 *  extent (Box2D v3). */
#define DE__CCD_FALLBACK_SCALE 0.25f

typedef struct de__ccd_settings {
    bool enabled;        /**< run the bullet pass */
    f32 speculative_max; /**< m; the per-pair speculative distance is at most this */
    f32 predict_max;     /**< m; the predictive growth of a box side is at most this */
} de__ccd_settings;

/** Counters of the last step (tests, the debug overlay). */
typedef struct de__ccd_stats {
    u32 predicted;      /**< bodies whose broadphase box grew */
    u32 bullets;        /**< bullets swept */
    u32 candidates;     /**< (bullet, body) pairs swept */
    u32 hits;           /**< bullets moved back to their time of impact */
    u32 fallbacks;      /**< (bullet, body) pairs that used the center-of-mass sphere */
    u32 iterations;     /**< conservative-advancement steps, all pairs */
    u32 max_iterations; /**< most steps one (sub-)shape pair needed */
    u32 capped;         /**< pairs that reached DE__CCD_MAX_ITER (counted as a hit there) */
} de__ccd_stats;

/* ================================ State ================================================ */

typedef struct de__ccd {
    const de_allocator *alloc;
    de__ccd_settings settings;
    u32 capacity; /**< max_bodies */

    /* Per body, written by de__ccd_prepare for every live body (ascending index). */
    de_vec3 *velocity; /**< predicted linear velocity of the center of mass for the step */
    f32 *reach;        /**< |w| * largest distance of the shape from the center of mass (m/s) */

    f32 dt;        /**< dt of the prepared step */
    u64 tick;      /**< p->tick when prepared (the arrays apply only to that step) */
    bool prepared; /**< de__ccd_prepare ran at least once */
    de__ccd_stats stats;

    void *block; /**< velocity, reach */
    size_t block_size;
} de__ccd;

/** The CCD state, or NULL before the first step. */
static inline de__ccd *de__ccd_get(const de_physics_world *p) {
    return (de__ccd *)de__phys_ext_get(p, DE__PHYS_EXT_CCD);
}
/**
 * The CCD state, created if needed (registers the cvars).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL world) or DE_ERR_OUT_OF_MEMORY (nothing is left).
 */
de_result de__ccd_ensure(de_physics_world *p, de__ccd **out);

/**
 * The CCD state when de__ccd_prepare ran for the step in progress (p->tick has not advanced
 * since), else NULL. The narrowphase uses the per-body arrays only then, so tests that run the
 * collision pipeline by hand get the fixed margin.
 */
static inline const de__ccd *de__ccd_current(const de_physics_world *p) {
    const de__ccd *c = de__ccd_get(p);
    return c != NULL && c->prepared && c->tick == p->tick ? c : NULL;
}

/**
 * Whether bodies a and b (a live pair with shapes) may come within DE__CCD_LINEAR_SLOP of each
 * other during the prepared step, both moving by their predicted velocities (linear) and angular
 * velocities: conservative advancement on GJK distance over the step, as in the bullet sweep.
 * true when CCD did not prepare this step, or when they are that close at its start. Pure: the
 * narrowphase calls it from many threads (de__contacts_filter_speculative), for pairs with
 * speculative points beyond the base margin.
 */
bool de__ccd_pair_may_touch(const de_physics_world *p, u32 a, u32 b);

/** Counters of the last step; zeroes before the state exists. */
void de__ccd_stats_get(const de_physics_world *p, de__ccd_stats *out);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_CCD_INTERNAL_H */
