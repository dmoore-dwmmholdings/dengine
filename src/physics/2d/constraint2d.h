/*
 * constraint2d.h: the constraint interface of the 2D soft-step solver (DENG-254, SPEC §13.3,
 * §13.1, §13.2, §5.7). Internal: include as "physics/2d/constraint2d.h". The 2D port of the 3D
 * interface (physics/solver/constraint.h, DENG-217, with the class iterations of DENG-398).
 *
 * The 2D joints (DENG-256) build on this file. Its shape is frozen: append, do not reorder or
 * change a signature.
 *
 * Solver bodies. Each step the solver copies every awake non-static body (ascending body index)
 * into a contiguous array of de__solver_body2d. Slot 0 is the static slot: zero velocity, zero
 * inverse mass, never written. Static bodies, sleeping bodies and "no body" (a joint to the world)
 * map to slot 0. A solver body holds its center-of-mass displacement `dp` (f32) and its rotation
 * `dq` since the step began; the f64 body positions are updated once, at the end of the step
 * (SPEC §5.5, DENG-2 Q10), so no world position is ever formed in f32.
 *
 * Constraint classes. A class (contacts, the joints, ...) registers a vtable and its state with
 * de__solver2d_register_class under a fixed class id. Each step:
 *
 *   gather       serial: the class appends its active constraints in its own stable order with
 *                de__solver2d_add_constraint (contacts: manifold key order; joints: joint index).
 *                A constraint whose two sides are both not writable (static, kinematic or
 *                sleeping) is refused there. Gather may run more than once in a step (a
 *                constraint woke a sleeping island and the solver gathers again), so it must start
 *                from scratch on each call.
 *   prepare      per constraint: anchors, effective masses, softness, warm-start impulses.
 *   per substep  [velocities integrate], warm_start, solve(use_bias = true), [positions
 *                integrate], relax (= solve without position bias).
 *   restitution  per constraint, once after the substeps (may be NULL).
 *   store        per constraint: the accumulated impulses back to persistent storage (the warm
 *                start of the next step).
 *
 * Order (SPEC §13.2). Records are added in a stable order: classes in ascending class id (joints
 * before contacts, so non-penetration has the last word), each class in its own stable order.
 * They are colored greedily in that order (graph_color2d.c), and every stage runs the colors in
 * ascending order, then the overflow set. The records of one color run in parallel: a class
 * function writes only its own record's data and the velocities of the sides it may write
 * (DE__CONSTRAINT2D_WRITE_A / _B), never the static slot.
 *
 * How to add a joint class (DENG-256):
 *   1. Keep the joints in a pool with stable indices (ascending index = solve order).
 *   2. Write the stage functions against de__solver2d_ctx and de__solver_body2d. Read the two
 *      solver bodies with ctx->bodies[c->solver_a] / [c->solver_b], work on local copies, and write
 *      back v and w only for the sides whose DE__CONSTRAINT2D_WRITE_* flag is set.
 *   3. gather: for each active joint in index order, de__solver2d_add_constraint(s,
 *      DE__CONSTRAINT2D_CLASS_JOINT, joint_index, body_a, body_b) (DE__CONSTRAINT2D_NO_BODY for a
 *      world anchor); set the returned record's `data` to the joint index (NULL = not solved).
 *   4. prepare: form world anchors in f64 relative to de__solver2d_constraint_origin(ctx->solver,
 *      c) and round offsets once with de__p2d_relative; use de__soft_make(hertz, zeta, ctx->h) for
 *      soft rows. Warm start from the stored impulses scaled by ctx->warm_start_scale.
 *   5. Register at create time: de__solver2d_register_class(p, DE__CONSTRAINT2D_CLASS_JOINT, &cls,
 *      state, max_joints), and de__solver2d_set_class_iterations for extra passes if needed.
 *   6. On joint create and destroy call de__islands2d_wake_body for both bodies, so a sleeping
 *      island sees the change.
 *
 * Determinism (SPEC §5.7): + - * / only (de_dsqrt for square roots, de_dsincos for rotation
 * increments), no FMA (the module is built with -ffp-contract=off), fixed iteration order.
 */
#ifndef DE_PHYSICS_CONSTRAINT2D_H
#define DE_PHYSICS_CONSTRAINT2D_H

#include "physics/2d/p2d_internal.h"
#include "physics/solver/constraint.h" /* de__soft, de__soft_make (shared with 3D) */

DE_EXTERN_C_BEGIN

/* ================================ Solver bodies ======================================== */

/** The static slot: never written by a constraint. */
#define DE__SOLVER2D_STATIC_SLOT 0u
/** de__constraint2d body index for "no body" (a joint attached to the world). */
#define DE__CONSTRAINT2D_NO_BODY 0xFFFFFFFFu

/** de__solver_body2d.flags. */
enum {
    DE__SOLVER2D_BODY_DYNAMIC = 1u << 0,   /**< dynamic (finite mass); else kinematic or static */
    DE__SOLVER2D_BODY_KINEMATIC = 1u << 1, /**< kinematic: moves by its velocity, infinite mass */
    DE__SOLVER2D_BODY_FIXED_ROTATION = 1u << 2 /**< DE_BODY2D_FLAG_FIXED_ROTATION: w stays 0 */
};

/**
 * Per-step state of one awake body. Velocities are those of the center of mass (plane axes).
 * `dp`, `dq` and `da` start at zero, identity and zero each step: the center of mass is at
 * c0 + dp and the rotation is dq * q0, where c0 and q0 are the values at the start of the step;
 * `da` is the angle turned so far (the sum of w * h, for the bullet sweep). Slot 0 is the static
 * slot.
 */
typedef struct de__solver_body2d {
    de_vec2 v;       /**< linear velocity, m/s */
    f32 w;           /**< angular velocity, rad/s, counter-clockwise */
    de_vec2 dp;      /**< center-of-mass displacement since the step began, m */
    de_rot2d dq;     /**< rotation since the step began */
    f32 da;          /**< angle turned since the step began, rad */
    f32 inv_mass;    /**< 0 when not dynamic */
    f32 inv_inertia; /**< 0 when not dynamic or fixed rotation */
    u32 body;        /**< body pool index; DE__CONSTRAINT2D_NO_BODY for the static slot */
    u32 flags;       /**< DE__SOLVER2D_BODY_* */
    /* Per-step integration constants. */
    f32 linear_damping;  /**< 1 / (1 + h * c) */
    f32 angular_damping; /**< 1 / (1 + h * c) */
    de_vec2 gravity_dv;  /**< h * gravity_scale * gravity (dynamic bodies) */
} de__solver_body2d;

/* ================================ f32 helpers (+ - * / only) ========================== */

/** w x r for a scalar angular velocity w: (-w r.y, w r.x). */
static inline de_vec2 de__v2_cross_sv(f32 w, de_vec2 r) {
    return de__v2(-w * r.y, w * r.x);
}
/** Velocity of the point at offset r from the center of mass of `b`. */
static inline de_vec2 de__solver_body2d_point_velocity(const de__solver_body2d *b, de_vec2 r) {
    return de__v2_add(b->v, de__v2_cross_sv(b->w, r));
}

/**
 * `world - origin` subtracted in f64 and rounded to f32 once per component: the only way the 2D
 * solver turns f64 plane points into f32 (as de__phys_relative in 3D). Debug and Dev builds assert
 * that the offset is below DE__PHYS_MAX_RELATIVE (16,384 m).
 */
static inline de_vec2 de__p2d_relative(de__d2 world, de__d2 origin) {
    const de__d2 d = de__d2_sub(world, origin);
    DE_ASSERT(d.x >= -16384.0 && d.x <= 16384.0 && d.y >= -16384.0 && d.y <= 16384.0);
    return de__v2((f32)d.x, (f32)d.y);
}

/* ================================ Constraint records =================================== */

/** Class ids, in solve order. Joint classes take ids below DE__CONSTRAINT2D_CLASS_CONTACT. */
enum {
    DE__CONSTRAINT2D_CLASS_JOINT = 0,     /**< DENG-256 (one class for every joint kind) */
    DE__CONSTRAINT2D_CLASS_JOINT_2 = 1,   /**< spare (limits / motors if separate) */
    DE__CONSTRAINT2D_CLASS_CHARACTER = 2, /**< a character that pushes bodies, if it needs one */
    DE__CONSTRAINT2D_CLASS_CONTACT = 3,   /**< DENG-254 contacts (last: non-penetration wins) */
    DE__CONSTRAINT2D_CLASS_COUNT = 4
};

/** de__constraint2d.flags. */
enum {
    /** Side A is an awake dynamic body: stage functions may write solver body `solver_a`. */
    DE__CONSTRAINT2D_WRITE_A = 1u << 0,
    /** Side B is an awake dynamic body: stage functions may write solver body `solver_b`. */
    DE__CONSTRAINT2D_WRITE_B = 1u << 1
};

/** de__constraint2d.color before coloring. */
#define DE__CONSTRAINT2D_NO_COLOR 0xFFFFFFFFu

/** One constraint of this step (rebuilt every step by the gather functions, stable order). */
typedef struct de__constraint2d {
    u32 class_id;           /**< DE__CONSTRAINT2D_CLASS_* */
    u32 id;                 /**< stable id within the class: contacts = index in the manifold array
                                 (key order), joints = joint index */
    u32 body_a, body_b;     /**< body pool indices (DE__CONSTRAINT2D_NO_BODY = the world) */
    u32 solver_a, solver_b; /**< solver slots; DE__SOLVER2D_STATIC_SLOT when not awake */
    u32 flags;              /**< DE__CONSTRAINT2D_WRITE_* */
    u32 data;               /**< the class's own index for this constraint */
    u32 color;              /**< graph color, DE__GRAPH2D_OVERFLOW, or DE__CONSTRAINT2D_NO_COLOR */
} de__constraint2d;

typedef struct de__solver2d de__solver2d;
typedef struct de__solver2d_settings de__solver2d_settings; /* solver2d_internal.h */

/** The per-step constants and the solver bodies; read-only for class functions. */
typedef struct de__solver2d_ctx {
    de_physics2d_world *p;
    de__solver2d *solver;
    de__solver_body2d *bodies; /**< [0] is the static slot */
    const de__solver2d_settings *settings;
    f32 dt;                /**< step */
    f32 h;                 /**< substep: dt / substeps */
    f32 inv_h;             /**< 1 / h */
    u32 substeps;          /**< p->substeps */
    u32 substep;           /**< current substep (0-based) while the substeps run */
    de__soft contact_soft; /**< contacts between two bodies of finite mass */
    de__soft static_soft;  /**< contacts with a static or kinematic side (twice the hertz) */
    f32 max_bias_velocity; /**< push-out speed limit of a penetrating contact, m/s */
    f32 restitution_threshold;
    f32 warm_start_scale; /**< 1, or 0 when warm starting is off */
    /** 0 in the solve or relax pass over every class; 1, 2, ... in a class's extra passes
     *  (de__solver2d_set_class_iterations). Other stages always see 0. */
    u32 pass;
} de__solver2d_ctx;

/** A per-constraint stage function (prepare, warm_start, relax, restitution, store). */
typedef void (*de__constraint2d_fn)(const de__solver2d_ctx *ctx, void *state, de__constraint2d *c);

/**
 * A constraint class. Every function but gather and solve may be NULL. `state` is the pointer
 * given at registration.
 */
typedef struct de__constraint2d_class {
    const char *name;
    /** Serial: append this step's constraints with de__solver2d_add_constraint. */
    void (*gather)(de__solver2d *s, const de__solver2d_ctx *ctx, void *state);
    de__constraint2d_fn prepare;
    de__constraint2d_fn warm_start;
    void (*solve)(const de__solver2d_ctx *ctx, void *state, de__constraint2d *c, bool use_bias);
    de__constraint2d_fn relax;       /**< NULL = solve(use_bias = false) */
    de__constraint2d_fn restitution; /**< NULL = none */
    de__constraint2d_fn store;
    /** Called when the solver is destroyed (or the class unregistered) if not NULL. */
    void (*destroy)(de_physics2d_world *p, void *state);
} de__constraint2d_class;

/**
 * Registers class `class_id` with `state` and room for `max_constraints` constraints per step.
 * Creates the solver state if needed. May allocate (the record array grows): call it from create
 * paths, never from a step.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL world or class, id out of range, no gather or
 *         solve); DE_ERR_ALREADY_EXISTS (id taken); DE_ERR_OUT_OF_MEMORY.
 */
de_result de__solver2d_register_class(de_physics2d_world *p, u32 class_id,
                                      const de__constraint2d_class *cls, void *state,
                                      u32 max_constraints);
/** Removes class `class_id` (calls its destroy). Unknown ids are ignored. */
void de__solver2d_unregister_class(de_physics2d_world *p, u32 class_id);

/**
 * From a gather function: appends a constraint between bodies `body_a` and `body_b` (pool indices
 * or DE__CONSTRAINT2D_NO_BODY) with the class's stable `id`. Fills the solver slots and write
 * flags. Returns the record (the class sets `data`), or NULL when neither side is an awake
 * dynamic body or the class's capacity is used up (the overflow is counted). A constraint between
 * a sleeping body and an awake dynamic body or a moving kinematic body requests the wake of the
 * sleeping island (the solver then gathers again).
 */
de__constraint2d *de__solver2d_add_constraint(de__solver2d *s, u32 class_id, u32 id, u32 body_a,
                                              u32 body_b);

/** The solver slot of body `index` this step (DE__SOLVER2D_STATIC_SLOT when not awake). */
u32 de__solver2d_slot(const de__solver2d *s, u32 body_index);

/**
 * The solve origin of record `c` this step: the f64 position of the lowest-index body of the
 * island of its writable side. Valid from the island build (after gather) to the end of the
 * step, so in prepare and the later stages; (0, 0) when the record has no island.
 */
de__d2 de__solver2d_constraint_origin(const de__solver2d *s, const de__constraint2d *c);

/** Most solve and relax passes per substep for one class. */
#define DE__SOLVER2D_MAX_CLASS_ITERATIONS 8u

/**
 * Sets how many solve and relax passes registered class `class_id` gets per substep (1 by
 * default, at most DE__SOLVER2D_MAX_CLASS_ITERATIONS; clamped). Before the pass over every record,
 * the solver runs `iterations - 1` extra passes over this class's records only, with `ctx->pass`
 * set to 1, 2, ..., in the same colors (as DENG-398 in 3D). Call it from create paths.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL world, id out of range, class not registered).
 */
de_result de__solver2d_set_class_iterations(de_physics2d_world *p, u32 class_id, u32 iterations);

/**
 * Wakes body `index` and its island at the start of the next step (de_body2d_is_sleeping is false
 * from now on). For joint create and destroy (DENG-256) and other world changes the solver cannot
 * see. No effect on an awake or dead body, or before the solver exists.
 */
void de__islands2d_wake_body(de_physics2d_world *p, u32 index);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_CONSTRAINT2D_H */
