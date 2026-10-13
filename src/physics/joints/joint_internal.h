/*
 * joint_internal.h: joints of the 3D physics world (DENG-220, SPEC §13.1, §13.2, §5.7). Internal:
 * include as "physics/joints/joint_internal.h".
 *
 * State. A joint pool in the world's DE__PHYS_EXT_JOINTS slot, created by the first
 * de_joint_create (or de__joints_create with an explicit capacity): the joints (one de__joint
 * each, index = handle index), a LIFO free list and per-body joint lists (for body destruction and
 * the collide_connected pair exclusion). Handles pack index + 1 and a generation like bodies.
 *
 * Solver. One constraint class, DE__CONSTRAINT_CLASS_JOINT, registered with the pool and capacity
 * max_joints. gather adds every live, unbroken joint in ascending joint index (record id and data
 * = joint index) and starts from scratch on every call. prepare stores the step-start geometry in
 * the joint (anchors from the centers of mass, frame rotations, center offset in f64 -> f32).
 * warm_start / solve / relax rebuild the current geometry from the solver bodies' dp and dq and
 * run the kind's rows (joint_<kind>.c). store checks the break force. Stage functions write only
 * their own joint and the writable sides' velocities (constraint.h).
 *
 * Rows (Box2D v3 soft step, in 3D). A 1-D row has a linear direction n and angular directions
 * a1 (A) and a2 (B): Cdot = n.(vB - vA) + a2.wB - a1.wA, an impulse l adds -mA l n, -IA l a1 to A
 * and +mB l n, +IB l a2 to B. Locked rows and limits use the joint softness in the biased solve and
 * no bias in relax (a joint with hertz > 0 uses its spring in both); limits are speculative while
 * inside the range (bias C / h). Blocks solve 3 (point, angular lock) or 2 (hinge axes) rows at
 * once. Order within a joint: motors, limits, then the locked rows, so the joint has the last word.
 *
 * Determinism (SPEC §5.7): + - * /, de_dsqrt, de_datan2 and de_dnormalize_quat only; fixed order.
 */
#ifndef DE_PHYSICS_JOINT_INTERNAL_H
#define DE_PHYSICS_JOINT_INTERNAL_H

#include "physics/solver/constraint.h"

DE_EXTERN_C_BEGIN

/* ================================ Settings ============================================= */

#define DE__JOINTS_MAX_CVAR           "physics.joints.max_joints"
#define DE__JOINTS_HERTZ_CVAR         "physics.joints.hertz"
#define DE__JOINTS_DAMPING_RATIO_CVAR "physics.joints.damping_ratio"
/** Stiffness of rigid joints: a quarter of the substep rate (60 Hz at 60 Hz x 4 substeps), or
 *  the cvar when it is set and lower. Stiffer joints make long chains unstable. */
#define DE__JOINTS_HERTZ_SUBSTEP_RATIO   0.25f
#define DE__JOINTS_DEFAULT_DAMPING_RATIO 2.0f
#define DE__JOINTS_MIN_CAPACITY          64u
#define DE__JOINTS_MAX_DEFAULT           4096u
/** No joint / end of a body's joint list. */
#define DE__JOINT_NONE 0xFFFFFFFFu

typedef struct de__joints_settings {
    f32 hertz;         /**< rigid joint stiffness (Hz); 0 = a quarter of the substep rate */
    f32 damping_ratio; /**< rigid joint damping ratio */
} de__joints_settings;

/* ================================ Joint ================================================ */

/** Per-step data, written by prepare. */
typedef struct de__joint_step {
    de_quat qa0, qb0;     /**< world rotation of frame A / B at the step start */
    de_vec3 ra0, rb0;     /**< anchors from the centers of mass (world axes, step start) */
    de_vec3 delta_center; /**< center of mass B - center of mass A (f64 difference) */
    de__soft spring;      /**< the joint's spring (hertz > 0) */
    de__soft motor;       /**< position motor spring (motor_hertz > 0) */
    f32 max_motor[6];     /**< motor_max_force * h */
} de__joint_step;

typedef struct de__joint {
    de__joint_step step; /**< first: holds the aligned quaternions */
    /* ---- Definition ---- */
    de_quat frame_qa, frame_qb; /**< frame rotations (body-local, or world for the world side) */
    de_vec3 local_a, local_b;   /**< anchor relative to the body origin (body-local); unused
                                     on the world side */
    de_dvec3 world_a, world_b;  /**< anchor of a world side (f64) */
    u32 body_a, body_b;         /**< body indices; DE__CONSTRAINT_NO_BODY = the world */
    u32 next[2];                /**< next edge (joint * 2 + side) in body_a's / body_b's list */
    u32 generation;             /**< of the current (or next) joint in this slot, 1..4095 */
    u8 alive;
    u8 kind;          /**< de_joint_kind */
    u8 broken;        /**< 1 after the break force was exceeded */
    u8 collide;       /**< collide_connected */
    u8 enable_limit;  /**< hinge, slider */
    u8 linear_block;  /**< 6DOF: every linear axis locked (point block) */
    u8 angular_block; /**< 6DOF: every angular axis locked (angular lock block) */
    u8 axis_mode[6];  /**< de_joint_axis_mode (6DOF) */
    u8 motor_mode[6]; /**< de_joint_motor_mode */
    de_entity entity_a, entity_b;
    f32 lo[6], hi[6];
    f32 motor_target[6];
    f32 motor_max_force[6];
    f32 hertz, damping_ratio;
    f32 motor_hertz, motor_damping_ratio;
    f32 break_force;
    /* ---- Accumulated impulses (per substep; warm start of the next step) ---- */
    de_vec3 lin; /**< point block (world) */
    de_vec3 ang; /**< angular lock block (world) */
    f32 eq[6];   /**< 1-D locked rows per axis; hinge axes in [4], [5] */
    f32 lower[6], upper[6], motor[6];
    /* ---- Reports ---- */
    f32 force;      /**< |linear impulse| / h of the last step (N) */
    u64 break_tick; /**< the tick the joint broke in */
} de__joint;

/* ================================ Pool ================================================= */

typedef struct de__joints {
    const de_allocator *alloc;
    de_physics_world *p;
    de__joints_settings settings;
    de__soft joint_soft; /**< this step: rigid joint softness (gather) */
    u32 capacity;
    u32 high_water; /**< slots ever used */
    u32 live_count;
    u32 free_count;
    u32 exclusions; /**< live joints with collide_connected false between two bodies */
    u32 body_capacity;
    de__joint *joints; /**< [capacity] */
    u32 *free_list;    /**< [capacity] LIFO */
    u32 *heads;        /**< [body_capacity] first edge of each body's joint list */
    u32 gathered;      /**< records added by the last gather */
    void *block;
    size_t block_size;
} de__joints;

/** The joint state, or NULL before the first joint. */
static inline de__joints *de__joints_get(const de_physics_world *p) {
    return (de__joints *)de__phys_ext_get(p, DE__PHYS_EXT_JOINTS);
}
/**
 * Creates the joint pool with room for `capacity` joints (0 = cvar physics.joints.max_joints, then
 * max(64, min(max_bodies, 4096))) and registers the joint class.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_ALREADY_EXISTS (pool exists, or another class has
 *         the joint id) or DE_ERR_OUT_OF_MEMORY.
 */
de_result de__joints_create(de_physics_world *p, u32 capacity);
/** The live joint of handle `j`, or NULL. */
de__joint *de__joint_resolve(const de_physics_world *p, de_joint j);
/** XXH3 of every live joint's state (index, generation, broken, impulses), for test probes. */
u64 de__joints_hash(const de_physics_world *p);

/* ================================ Solve context and rows (joint.c) ======================= */

/** A joint's view of the step in a stage function: velocities and the current geometry. */
typedef struct de__jx {
    const de__solver_ctx *ctx;
    const de__joints *js;
    de_vec3 va, wa, vb, wb; /**< working velocities (written back for writable sides) */
    f32 ma, mb;             /**< inverse masses */
    const de_mat3 *ia, *ib; /**< world inverse inertias */
    de_vec3 ra, rb;         /**< current anchors from the centers of mass */
    de_vec3 d;              /**< anchor B - anchor A (current) */
    de_quat qa, qb;         /**< current world rotations of frame A and B */
    bool use_bias;
    de__soft soft; /**< locked rows and limits for this pass */
} de__jx;

/** A 1-D row (see the file comment). `mass` = 1 / effective mass (0 when it has none). */
typedef struct de__jrow {
    de_vec3 n, a1, a2;
    f32 mass;
} de__jrow;

/* f32 quaternion helpers (+ - * / only). */
static inline de_quat de__jq(f32 x, f32 y, f32 z, f32 w) {
    de_quat q;
    q.x = x;
    q.y = y;
    q.z = z;
    q.w = w;
    return q;
}
static inline de_quat de__jq_mul(de_quat a, de_quat b) {
    return de__jq(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                  a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                  a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                  a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}
static inline de_quat de__jq_conj(de_quat q) {
    return de__jq(-q.x, -q.y, -q.z, q.w);
}
/** Axis i (0 = X, 1 = Y, 2 = Z) of rotation q. */
static inline de_vec3 de__jq_axis(de_quat q, u32 i) {
    const de_vec3 e = de__v3(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f);
    return de__quat_rotate_f(q, e);
}

/** B's frame in A's frame: conj(qa) qb with w >= 0. */
de_quat de__joint_relative(const de__jx *x);
/**
 * Swing-twist split of the relative rotation `q` (w >= 0) about X: q = swing * twist. `twist` in
 * [-pi, pi]; swing as a rotation vector (0, sy, sz) in A's frame; `swing` = its angle in [0, pi].
 */
void de__joint_swing_twist(de_quat q, f32 *twist, f32 *sy, f32 *sz, f32 *swing);

/** Row along n attached to A (prismatic Jacobian: a1 = (ra + d) x n, a2 = rb x n). */
de__jrow de__jrow_linear(const de__jx *x, de_vec3 n);
/** Row along n through the anchors (a1 = ra x n, a2 = rb x n). */
de__jrow de__jrow_point(const de__jx *x, de_vec3 n);
/** Angular row about the unit axis. */
de__jrow de__jrow_angular(const de__jx *x, de_vec3 axis);
/** The same row with the opposite sign (upper limits). */
de__jrow de__jrow_neg(de__jrow r);
f32 de__jrow_cdot(const de__jx *x, const de__jrow *r);
void de__jrow_apply(de__jx *x, const de__jrow *r, f32 impulse);
/** Locked row with error C (x->soft). */
void de__jrow_equality(de__jx *x, const de__jrow *r, f32 C, f32 *acc);
/** One-sided row: keeps C >= 0 (speculative while C > 0); acc >= 0. */
void de__jrow_limit(de__jx *x, const de__jrow *r, f32 C, f32 *acc);
/** Lower and upper limit rows for coordinate `coord` in [lo, hi]. */
void de__jrow_limits(de__jx *x, const de__jrow *r, f32 coord, f32 lo, f32 hi, f32 *acc_lo,
                     f32 *acc_hi);
/** Motor of axis `axis` (de__joint.motor_mode) on row r at coordinate `coord`. */
void de__jrow_motor(de__jx *x, const de__joint *j, u32 axis, const de__jrow *r, f32 coord,
                    f32 *acc);
/** Point block: C = x->d (3 rows through the anchors). */
void de__jpoint_solve(de__jx *x, de_vec3 *acc);
void de__jpoint_apply(de__jx *x, de_vec3 impulse);
/** Angular lock block: B's frame rotation equals A's. */
void de__jlock_solve(de__jx *x, de_vec3 *acc);
void de__jlock_apply(de__jx *x, de_vec3 impulse);
/** Solves the symmetric 3x3 system K x = b (falls back to the diagonal when K is singular). */
de_vec3 de__jm3_solve(const de_mat3 *k, de_vec3 b);

/* ================================ Kinds (joint_<kind>.c) ================================ */

/** Per-kind functions: warm start (apply the accumulated impulses), solve (x->use_bias) and the
 *  linear impulse on the anchors (world vector, for the break force). */
typedef struct de__joint_ops {
    void (*warm_start)(de__jx *x, de__joint *j);
    void (*solve)(de__jx *x, de__joint *j);
    de_vec3 (*linear_impulse)(const de__jx *x, const de__joint *j);
} de__joint_ops;

extern const de__joint_ops de__joint_fixed_ops;
extern const de__joint_ops de__joint_hinge_ops;
extern const de__joint_ops de__joint_ball_ops;
extern const de__joint_ops de__joint_slider_ops;
extern const de__joint_ops de__joint_distance_ops;
extern const de__joint_ops de__joint_cone_twist_ops;
extern const de__joint_ops de__joint_6dof_ops;

/** Shared by fixed, ball, hinge, cone-twist and 6DOF: the point block's impulse. */
de_vec3 de__joint_point_impulse(const de__jx *x, const de__joint *j);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_JOINT_INTERNAL_H */
