/*
 * character2d_internal.h: the 2D kinematic character controller (DENG-258, SPEC §13.3, §13.1),
 * the 2D mirror of the 3D capsule controller (DENG-221, physics/character/). Internal: include as
 * "physics/2d/character2d_internal.h".
 *
 * character2d.c holds the controller state (extension slot DE__P2D_EXT_CHARACTER: which bodies
 * are character proxies, and the stats of the last move), de_character2d_create / destroy /
 * move and the movement core de__character2d_move (see the comment at the top of character2d.c).
 *
 * The controller is built on the 2D scene queries (query2d_internal.h: the per-body casts and
 * rays, with the proxy's filter, the proxy ignored and triggers excluded) and on the 2D
 * narrowphase (de__collide2d) for contact normals and depths. It has no constraint class: the
 * proxy is a kinematic body whose velocity the controller sets, so the step's kinematic
 * integration (DENG-254) moves it and dynamic bodies collide with it there.
 *
 * Determinism (SPEC §5.7): + - * / and de_dmath only; fixed iteration caps; every selection over
 * bodies uses fixed keys (fraction, then body index; contacts in ascending body index). No
 * allocation per move (fixed stack arrays).
 */
#ifndef DE_PHYSICS_CHARACTER2D_INTERNAL_H
#define DE_PHYSICS_CHARACTER2D_INTERNAL_H

#include "physics/2d/query2d_internal.h"

DE_EXTERN_C_BEGIN

/* ================================ Limits and tolerances ================================= */

/** Collide-and-slide iterations per move. */
#define DE__CHAR2D_MAX_SLIDE_ITER 4u
/** Depenetration iterations per move. */
#define DE__CHAR2D_MAX_DEPEN_ITER 4u
/** Bodies considered by one depenetration pass (the lowest body indices when more touch). */
#define DE__CHAR2D_MAX_DEPEN_BODIES 32u
/** Chain segments read per chain body by one contact pass (the lowest segment indices). */
#define DE__CHAR2D_MAX_SEGMENTS 16u
/** A displacement shorter than this (m) is not cast. */
#define DE__CHAR2D_MIN_MOVE 1e-6
/** Depenetration starts when a contact is closer than this fraction of the skin. */
#define DE__CHAR2D_DEPEN_TRIGGER 0.5
/** Smallest cos(angle between the cast and the surface) used to back off by the skin. */
#define DE__CHAR2D_MIN_BACKOFF_COS 0.05
/** A step is taken only when the raised forward cast advances at least this far (m). */
#define DE__CHAR2D_STEP_MIN_FORWARD 1e-3
/** Edge support probe: a ray starts this far inside the contact (m) ... */
#define DE__CHAR2D_EDGE_IN 0.02f
/** ... and this far above it, and is twice as long (m). */
#define DE__CHAR2D_EDGE_UP 0.05f
/** Hits whose normal y is at most this are walls or ceilings: never ground, no edge probe. */
#define DE__CHAR2D_EDGE_MIN_NY 0.05f

/* ================================ State ================================================= */

/** de__character2d_state.kind values. */
enum { DE__CHAR2D_NONE = 0, DE__CHAR2D_CAPSULE = 1, DE__CHAR2D_BOX = 2 };

/** What the last de__character2d_move did (tests and debugging). */
typedef struct de__character2d_stats {
    u32 depen_iterations; /**< depenetration passes that moved the proxy */
    u32 slide_iterations; /**< collide-and-slide casts */
    u32 casts;            /**< every shape cast (slide, step, ground) */
    u32 ground_body;      /**< the body stood on at the end, or DE__Q2_NO_BODY */
    bool stepped;         /**< a step was climbed */
    bool snapped;         /**< the ground probe moved the proxy down */
    bool on_platform;     /**< a moving ground body carried the character */
} de__character2d_stats;

/** Extension slot DE__P2D_EXT_CHARACTER: one allocation (this struct, then `kind`). */
typedef struct de__character2d_state {
    u8 *kind;     /**< [capacity]: DE__CHAR2D_* per body index */
    u32 capacity; /**< the world's body capacity */
    u32 live;     /**< characters created and not destroyed */
    de__character2d_stats last;
    size_t size; /**< bytes of the allocation */
} de__character2d_state;

/** The state, or NULL before the first character. */
static inline de__character2d_state *de__character2d_get(const de_physics2d_world *p) {
    return (de__character2d_state *)de__p2d_ext_get(p, DE__P2D_EXT_CHARACTER);
}

/** DeCharacter2D parameters with defaults resolved. */
typedef struct de__character2d_params {
    f32 step_height;
    f32 cos_max_slope; /**< de_dcos(max_slope_rad) */
    f32 skin;          /**< DE_CHARACTER2D_SKIN_WIDTH */
} de__character2d_params;

/**
 * The movement core. `index` is a live character proxy, `params`, `io` and `dt` are checked by
 * the caller. Writes io's outputs, the proxy's velocity and, when `out` is not NULL, the pose
 * after the next step.
 */
void de__character2d_move(de_physics2d_world *p, u32 index, const de__character2d_params *params,
                          DeCharacter2D *io, f32 dt, de_transform2d *out);

/**
 * The pose kinematic body `index` reaches over one step of `dt` with velocities `v` and `w`, as
 * the controller predicts the step's kinematic integration: `substeps` sub-steps of dt /
 * substeps, an f32 displacement of the center of mass summed per sub-step, the rotation
 * integrated per sub-step (Box2D's b2IntegrateRotation, normalized in f64) about the center of
 * mass. Tests use it to move platforms and proxies without a solver step.
 */
void de__p2d_predict_kinematic(const de_physics2d_world *p, u32 index, de_vec2 v, f32 w, f32 dt,
                               de_transform2d *out);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_CHARACTER2D_INTERNAL_H */
