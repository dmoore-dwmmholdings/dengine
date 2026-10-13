/*
 * character_internal.h: the kinematic capsule character controller (DENG-221, SPEC §13.1).
 * Internal: include as "physics/character/character_internal.h".
 *
 * Files (src/physics/character/):
 * - character.c: the controller state in extension slot DE__PHYS_EXT_CHARACTER (settings from
 *   cvars, stats of the last move), de_character_create / destroy, and de_character_move's
 *   argument checks.
 * - character_move.c: de__character_move, the movement core (see the comment at its top).
 *
 * The controller is built on the scene queries (query/query_internal.h: the same candidate
 * enumeration, per-body casts and rays as de_physics_shape_cast_ex / raycast_ex, with the proxy
 * as the ignored body and triggers excluded) and on the narrowphase (de__collide) for contact
 * normals and depths. It has no constraint class: the proxy is a kinematic body whose velocity
 * the controller sets, so the solver moves it during de_physics_step and dynamic bodies collide
 * with it there.
 *
 * Determinism (SPEC §5.7): + - * / and de_dmath only; fixed iteration caps; every selection over
 * bodies uses fixed keys (fraction, then body index; contacts in ascending body index), never the
 * order in which the broadphase visits bodies. No allocation per move (fixed stack arrays).
 */
#ifndef DE_PHYSICS_CHARACTER_INTERNAL_H
#define DE_PHYSICS_CHARACTER_INTERNAL_H

#include "physics/physics_internal.h"

DE_EXTERN_C_BEGIN

/* ================================ Limits and tolerances ================================= */

/** Collide-and-slide iterations per move (shape casts along the remaining displacement). */
#define DE__CHAR_MAX_SLIDE_ITER 4u
/** Depenetration iterations per move. */
#define DE__CHAR_MAX_DEPEN_ITER 4u
/** Bodies considered by one depenetration pass (the lowest body indices when more touch). */
#define DE__CHAR_MAX_DEPEN_BODIES 32u
/** Contact sets read per body by de__collide (the rest are dropped deterministically). */
#define DE__CHAR_MAX_CONTACT_SETS 16u
/** Bodies pushed per move (one push per body). */
#define DE__CHAR_MAX_PUSHES 8u
/** A displacement shorter than this (m) is not cast. */
#define DE__CHAR_MIN_MOVE 1e-6
/** Depenetration starts when a contact is closer than this fraction of skin_width. */
#define DE__CHAR_DEPEN_TRIGGER 0.5
/** Smallest cos(angle between the cast and the surface) used to back off by skin_width. */
#define DE__CHAR_MIN_BACKOFF_COS 0.05
/** A step is taken only when the raised forward cast advances at least this far (m). */
#define DE__CHAR_STEP_MIN_FORWARD 1e-3
/** Edge support probe: a ray starts this far inside the contact (m) ... */
#define DE__CHAR_EDGE_IN 0.02f
/** ... and this far above it, and is twice as long (m). */
#define DE__CHAR_EDGE_UP 0.05f
/** Hits whose normal z is at most this are walls or ceilings: never ground, no edge probe. */
#define DE__CHAR_EDGE_MIN_NZ 0.05f

/* ================================ Settings and state ==================================== */

#define DE__CHAR_MASS_CVAR          "physics.character.mass"
#define DE__CHAR_PUSH_FORCE_CVAR    "physics.character.push_force"
#define DE__CHAR_DEFAULT_MASS       80.0f
#define DE__CHAR_DEFAULT_PUSH_FORCE 1000.0f

/** World-wide controller settings (cvars, read when the state is created; tests may change
 *  them directly). */
typedef struct de__character_settings {
    f32 mass;       /**< kg: the character's side of the push mass ratio (> 0) */
    f32 push_force; /**< N: the largest push impulse per second (>= 0; 0 = never push) */
} de__character_settings;

/** What the last de__character_move did (tests and debugging). */
typedef struct de__character_stats {
    u32 depen_iterations; /**< depenetration passes that moved the capsule */
    u32 slide_iterations; /**< collide-and-slide casts */
    u32 casts;            /**< every shape cast (slide, step, ground) */
    u32 pushes;           /**< dynamic bodies pushed */
    bool stepped;         /**< a step was climbed */
    bool snapped;         /**< the ground probe moved the capsule down */
    bool on_platform;     /**< a moving ground body carried the character */
} de__character_stats;

/** Extension slot DE__PHYS_EXT_CHARACTER. */
typedef struct de__character_state {
    de__character_settings settings;
    de__character_stats last;
    u32 live; /**< characters created and not destroyed */
} de__character_state;

/** The state, created on first use (create-time: may allocate). */
de_result de__character_ensure(de_physics_world *p, de__character_state **out);
/** The state, or NULL before the first character. */
static inline de__character_state *de__character_get(const de_physics_world *p) {
    return (de__character_state *)de__phys_ext_get(p, DE__PHYS_EXT_CHARACTER);
}

/** DeCharacter parameters with defaults resolved. */
typedef struct de__character_params {
    f32 step_height;
    f32 cos_max_slope; /**< de_dcos(max_slope_rad) */
    f32 skin;
} de__character_params;

/**
 * The movement core. `index` is the proxy (a live kinematic capsule body), `io` and `dt` are
 * checked by the caller. Writes io's outputs, the proxy's velocities and, when `out` is not NULL,
 * the pose after the next step.
 */
void de__character_move(de_physics_world *p, u32 index, const de__character_params *params,
                        DeCharacter *io, f32 dt, de_transform *out);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_CHARACTER_INTERNAL_H */
