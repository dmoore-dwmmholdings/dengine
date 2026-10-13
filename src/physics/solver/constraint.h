/*
 * constraint.h: the generic constraint interface of the physics solver (DENG-217, SPEC §13.1,
 * §13.2, §5.7). Internal: include as "physics/solver/constraint.h".
 *
 * This is the contract that the S23 wave-2 tasks build on in parallel: joints (DENG-220) add
 * constraint classes, graph coloring and parallel batches (DENG-218) schedule any constraint, CCD
 * (DENG-219) and the character controller (DENG-221) read the solver bodies. Its shape is frozen:
 * append, do not reorder or change a signature.
 *
 * Solver bodies. Each step the solver copies every awake non-static body (de__islands_update
 * picks them; ascending body index) into a contiguous array of de__solver_body. Slot 0 is the
 * static slot: zero velocity, zero inverse mass, no motion. Static bodies, sleeping bodies and
 * "no body" (a joint to the world) all map to slot 0. Positions are never formed as floats in
 * world space: a solver body holds its center-of-mass displacement `dp` (f32) and rotation `dq`
 * since the step began, and the double positions are updated once at the end of the step
 * (SPEC §5.5, DENG-2 Q10).
 *
 * Constraint classes. A class (contacts, each joint kind family, ...) registers a vtable and its
 * state with de__solver_register_class under a fixed class id. Each step:
 *
 *   gather       serial, once per step, after the islands: the class appends its active
 *                constraints in its own stable order with de__solver_add_constraint (contacts:
 *                manifold key order; joints: joint index). A constraint whose two sides are both
 *                not writable (static, kinematic or sleeping) is refused there and not solved.
 *   prepare      per constraint: anchors, effective masses, softness, warm-start impulses.
 *   per substep  warm_start, solve(use_bias = true), [positions integrate], relax
 *                (= solve without position bias). Velocities are integrated before warm_start.
 *   restitution  per constraint, once after the substeps (may be NULL).
 *   store        per constraint: accumulated impulses back to persistent storage (warm start of
 *                the next step).
 *
 * Order (SPEC §13.2). Records are solved in the order they were added: classes in ascending class
 * id (joints before contacts, so non-penetration has the last word), each class in its own stable
 * order. DENG-218 replaces the serial loops of solver.c by graph-colored batches; a class function
 * must then only write its own record's data and the velocities of the sides it may write
 * (DE__CONSTRAINT_WRITE_A / _B), never the static slot.
 *
 * Determinism (SPEC §5.7): + - * / only (de_dsqrt for square roots), no FMA (the module is built
 * with -ffp-contract=off), fixed iteration order.
 */
#ifndef DE_PHYSICS_SOLVER_CONSTRAINT_H
#define DE_PHYSICS_SOLVER_CONSTRAINT_H

#include "physics/physics_internal.h"

DE_EXTERN_C_BEGIN

/* ================================ Solver bodies ======================================== */

/** The static slot: never written by a constraint. */
#define DE__SOLVER_STATIC_SLOT 0u
/** de__constraint body index for "no body" (a joint attached to the world). */
#define DE__CONSTRAINT_NO_BODY 0xFFFFFFFFu

/** de__solver_body.flags. */
enum {
    DE__SOLVER_BODY_DYNAMIC = 1u << 0,   /**< dynamic (finite mass); else kinematic or static */
    DE__SOLVER_BODY_KINEMATIC = 1u << 1, /**< kinematic: moves by its velocity, infinite mass */
    DE__SOLVER_BODY_LOCK_X = 1u << 2,    /**< no rotation about world X (DE_BODY_FLAG_LOCK_ROT_X) */
    DE__SOLVER_BODY_LOCK_Y = 1u << 3,
    DE__SOLVER_BODY_LOCK_Z = 1u << 4
};

/**
 * Per-step state of one awake body. Velocities are those of the center of mass (world axes).
 * `dp` and `dq` start at zero and identity each step: the body's center of mass is at
 * c0 + dp and its rotation is dq * q0, where c0 and q0 are the values at the start of the step.
 * `inv_inertia` is the world inverse inertia at the start of the step (R I^-1 R^T, rows and
 * columns of locked axes zeroed); it stays fixed for the step. Slot 0 is the static slot.
 */
typedef struct de__solver_body {
    de_vec3 v;  /**< linear velocity, m/s */
    de_vec3 w;  /**< angular velocity, rad/s */
    de_vec3 dp; /**< center-of-mass displacement since the step began, m */
    de_quat dq; /**< rotation since the step began (world frame) */
    f32 inv_mass;
    de_mat3 inv_inertia; /**< world inverse inertia (symmetric) */
    u32 body;            /**< body pool index; DE__CONSTRAINT_NO_BODY for the static slot */
    u32 flags;           /**< DE__SOLVER_BODY_* */
    /* Per-step integration constants (integrate.c). */
    f32 linear_damping;  /**< 1 / (1 + h * c) */
    f32 angular_damping; /**< 1 / (1 + h * c) */
    de_vec3 gravity_dv;  /**< h * gravity_scale * gravity (dynamic bodies) */
} de__solver_body;

/* ================================ Soft constraints ===================================== */

/**
 * Soft-step coefficients (Box2D v3 "soft step"): a constraint of stiffness `hertz` and damping
 * ratio `zeta` solved with substep h. With omega = 2 pi hertz, a1 = 2 zeta + h omega,
 * a2 = h omega a1, a3 = 1 / (1 + a2): bias_rate = omega / a1, mass_scale = a2 a3,
 * impulse_scale = a3. hertz = 0 is a rigid constraint (bias 0, mass scale 1, impulse scale 0).
 * The solve is: impulse = -mass * mass_scale * (vn + bias_rate * C) - impulse_scale * accumulated.
 */
typedef struct de__soft {
    f32 bias_rate;
    f32 mass_scale;
    f32 impulse_scale;
} de__soft;

static inline de__soft de__soft_make(f32 hertz, f32 zeta, f32 h) {
    de__soft s;
    if (!(hertz > 0.0f) || !(h > 0.0f)) {
        s.bias_rate = 0.0f;
        s.mass_scale = 1.0f;
        s.impulse_scale = 0.0f;
        return s;
    }
    const f32 omega = 6.28318530718f * hertz;
    const f32 a1 = 2.0f * zeta + h * omega;
    const f32 a2 = h * omega * a1;
    const f32 a3 = 1.0f / (1.0f + a2);
    s.bias_rate = omega / a1;
    s.mass_scale = a2 * a3;
    s.impulse_scale = a3;
    return s;
}

/* ================================ f32 helpers (+ - * / only) ========================== */

/** m * v for a column-major de_mat3 (element (r, c) is m[c * 3 + r]). */
static inline de_vec3 de__m3_mul(const de_mat3 *m, de_vec3 v) {
    return de__v3(m->m[0] * v.x + m->m[3] * v.y + m->m[6] * v.z,
                  m->m[1] * v.x + m->m[4] * v.y + m->m[7] * v.z,
                  m->m[2] * v.x + m->m[5] * v.y + m->m[8] * v.z);
}
/** v rotated by the unit quaternion q, in f32: v + 2w (u x v) + 2 u x (u x v). */
static inline de_vec3 de__quat_rotate_f(de_quat q, de_vec3 v) {
    const de_vec3 u = de__v3(q.x, q.y, q.z);
    const de_vec3 t = de__v3_scale(de__v3_cross(u, v), 2.0f);
    return de__v3_add(de__v3_add(v, de__v3_scale(t, q.w)), de__v3_cross(u, t));
}
/** dot(r x n, I (r x n)): the angular part of an effective mass. */
static inline f32 de__angular_mass(const de_mat3 *inv_inertia, de_vec3 r, de_vec3 n) {
    const de_vec3 rn = de__v3_cross(r, n);
    return de__v3_dot(rn, de__m3_mul(inv_inertia, rn));
}

/* ================================ Constraint records =================================== */

/** Class ids, in solve order. Joint classes take ids below DE__CONSTRAINT_CLASS_CONTACT. */
enum {
    DE__CONSTRAINT_CLASS_JOINT = 0,     /**< DENG-220 (one class for every joint kind) */
    DE__CONSTRAINT_CLASS_JOINT_2 = 1,   /**< DENG-220 (spare: limits / motors if separate) */
    DE__CONSTRAINT_CLASS_CHARACTER = 2, /**< DENG-221 (pushing bodies), if it needs one */
    DE__CONSTRAINT_CLASS_CONTACT = 3,   /**< DENG-217 contacts (last: non-penetration wins) */
    DE__CONSTRAINT_CLASS_COUNT = 4
};

/** de__constraint.flags. */
enum {
    /** Side A is an awake dynamic body: solve may write solver body `solver_a`. */
    DE__CONSTRAINT_WRITE_A = 1u << 0,
    /** Side B is an awake dynamic body: solve may write solver body `solver_b`. */
    DE__CONSTRAINT_WRITE_B = 1u << 1
};

/** de__constraint.color before graph coloring (DENG-218). */
#define DE__CONSTRAINT_NO_COLOR 0xFFFFFFFFu

/**
 * One constraint of this step, in the solver's record array (stable order, see the file
 * comment). Records are rebuilt every step by the classes' gather functions.
 */
typedef struct de__constraint {
    u32 class_id;       /**< DE__CONSTRAINT_CLASS_* */
    u32 id;             /**< stable id within the class: contacts = index in the manifold array (key
                             order), joints = joint index */
    u32 body_a, body_b; /**< body pool indices (DE__CONSTRAINT_NO_BODY = the world) */
    u32 solver_a, solver_b; /**< solver body slots; DE__SOLVER_STATIC_SLOT when not awake */
    u32 flags;              /**< DE__CONSTRAINT_WRITE_* */
    u32 data;               /**< the class's own index for this constraint (its prepared data) */
    u32 color;              /**< DENG-218; DE__CONSTRAINT_NO_COLOR until then */
} de__constraint;

typedef struct de__solver de__solver;
typedef struct de__solver_settings de__solver_settings; /* solver_internal.h */

/**
 * What a class function gets: the per-step constants and the solver bodies. One per solver,
 * rebuilt at the start of each step; read-only for class functions.
 */
typedef struct de__solver_ctx {
    de_physics_world *p;
    de__solver *solver;
    de__solver_body *bodies; /**< [0] is the static slot */
    const de__solver_settings *settings;
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
    /** DENG-398 (appended): 0 in the solve or relax pass over every class; 1, 2, ... in a class's
     *  extra passes (de__solver_set_class_iterations). Other stages always see 0. */
    u32 pass;
} de__solver_ctx;

/** A per-constraint stage function (prepare, warm_start, relax, restitution, store). */
typedef void (*de__constraint_fn)(const de__solver_ctx *ctx, void *state, de__constraint *c);

/**
 * A constraint class. Every function but gather and solve may be NULL. `state` is the pointer
 * given at registration. Per-constraint functions may run in parallel for constraints of
 * different colors (DENG-218): they write only their record's data and the writable sides'
 * solver bodies.
 */
typedef struct de__constraint_class {
    const char *name;
    /** Serial, once per step: append this step's constraints with de__solver_add_constraint. */
    void (*gather)(de__solver *s, const de__solver_ctx *ctx, void *state);
    de__constraint_fn prepare;
    de__constraint_fn warm_start;
    void (*solve)(const de__solver_ctx *ctx, void *state, de__constraint *c, bool use_bias);
    de__constraint_fn relax;       /**< NULL = solve(use_bias = false) */
    de__constraint_fn restitution; /**< NULL = none */
    de__constraint_fn store;
    /** Called when the solver is destroyed (or the class unregistered) if not NULL. */
    void (*destroy)(de_physics_world *p, void *state);
} de__constraint_class;

/**
 * Registers class `class_id` with `state` and room for `max_constraints` constraints per step.
 * Creates the solver state if needed. May allocate (the record array grows): call it from create
 * paths, never from a step.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL world or class, id out of range, no gather or
 *         solve); DE_ERR_ALREADY_EXISTS (id taken); DE_ERR_OUT_OF_MEMORY.
 */
de_result de__solver_register_class(de_physics_world *p, u32 class_id,
                                    const de__constraint_class *cls, void *state,
                                    u32 max_constraints);
/** Removes class `class_id` (calls its destroy). Unknown ids are ignored. */
void de__solver_unregister_class(de_physics_world *p, u32 class_id);

/**
 * From a gather function: appends a constraint between bodies `body_a` and `body_b` (pool indices
 * or DE__CONSTRAINT_NO_BODY) with the class's stable `id`. Fills the solver slots and write
 * flags. Returns the record (the class sets `data`), or NULL when neither side is an awake
 * dynamic body (nothing to solve) or the class's capacity is used up (the overflow is counted).
 */
de__constraint *de__solver_add_constraint(de__solver *s, u32 class_id, u32 id, u32 body_a,
                                          u32 body_b);

/** The solver slot of body `index` this step (DE__SOLVER_STATIC_SLOT when not awake). */
u32 de__solver_slot(const de__solver *s, u32 body_index);

/* ================================ Class iterations (DENG-398, appended) ================= */

/** Most solve and relax passes per substep for one class. */
#define DE__SOLVER_MAX_CLASS_ITERATIONS 8u

/**
 * Sets how many solve and relax passes registered class `class_id` gets per substep (1, the
 * default after registration, to DE__SOLVER_MAX_CLASS_ITERATIONS; out-of-range values are
 * clamped). Before the full pass over every record, the solver runs `iterations - 1` extra passes
 * over this class's records only, with `ctx->pass` set to the pass number (1, 2, ...). These
 * passes use the same colors and the same rules as every other stage, so classes solved later
 * (contacts) still have the last word, and the worker count changes no bit. Joints use it
 * (DENG-398): a long chain converges in a few passes, which lets the locked rows be twice as
 * stiff and sag a quarter as much. Call it from create paths.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL world, id out of range, class not registered).
 */
de_result de__solver_set_class_iterations(de_physics_world *p, u32 class_id, u32 iterations);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_SOLVER_CONSTRAINT_H */
