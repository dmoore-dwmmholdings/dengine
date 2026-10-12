/*
 * physics_internal.h: the physics world, the body pool and the deterministic math helpers of
 * src/physics/ (DENG-132). Internal: include as "physics/physics_internal.h".
 *
 * This is the contract the other S22 tasks build on: broadphase (DENG-133), narrowphase
 * (DENG-134), contacts and step wiring (DENG-135), determinism CI (DENG-136) and queries
 * (DENG-137). The layouts below are settled; later tasks add their own state through the
 * extension slots (de__phys_ext_*), not by adding fields to the body pool.
 *
 * Determinism (SPEC §5.7). Every file of the module is built with -ffp-contract=off. Code here
 * uses only + - * / and de_dsqrt / de_dsqrt64 (never <math.h> and never the fast de_v3_* /
 * de_quat_* functions of de_math.h sections 2 and 3, which may use FMA or SIMD). Bodies are
 * always visited in ascending body index.
 */
#ifndef DE_PHYSICS_INTERNAL_H
#define DE_PHYSICS_INTERNAL_H

#include <dengine/de_physics.h>

#include <float.h>
#include <string.h>

DE_EXTERN_C_BEGIN

/* ================================ Handles ============================================== */

/*
 * de_body and de_shape ids: low 20 bits index + 1, high 12 bits generation (1..4095). Each free
 * of an index bumps its generation (4095 wraps to 1), so a stale handle is detected until the
 * same index has been reused 4095 times. The id is never 0.
 */
#define DE__HANDLE_INDEX_BITS 20u
#define DE__HANDLE_INDEX_MASK ((1u << DE__HANDLE_INDEX_BITS) - 1u)
#define DE__HANDLE_GEN_MAX    0xFFFu

static inline u32 de__handle_make(u32 index, u32 generation) {
    return (generation << DE__HANDLE_INDEX_BITS) | (index + 1u);
}
/** Index of a handle id, or UINT32_MAX for 0. */
static inline u32 de__handle_index(u32 id) {
    return (id & DE__HANDLE_INDEX_MASK) - 1u;
}
static inline u32 de__handle_gen(u32 id) {
    return id >> DE__HANDLE_INDEX_BITS;
}
static inline u32 de__gen_next(u32 g) {
    return g >= DE__HANDLE_GEN_MAX ? 1u : g + 1u;
}

/* ================================ Deterministic scalar / vector helpers ================= */

static inline bool de__f32_finite(f32 x) {
    u32 u;
    memcpy(&u, &x, sizeof u);
    return (u & 0x7F800000u) != 0x7F800000u;
}
static inline bool de__f64_finite(f64 x) {
    u64 u;
    memcpy(&u, &x, sizeof u);
    return (u & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}
static inline f32 de__absf(f32 x) {
    return x < 0.0f ? -x : x;
}
static inline f64 de__absd(f64 x) {
    return x < 0.0 ? -x : x;
}
static inline f32 de__minf(f32 a, f32 b) {
    return a < b ? a : b;
}
static inline f32 de__maxf(f32 a, f32 b) {
    return a > b ? a : b;
}
static inline f64 de__mind(f64 a, f64 b) {
    return a < b ? a : b;
}
static inline f64 de__maxd(f64 a, f64 b) {
    return a > b ? a : b;
}

/** The next f32 above `f` (+0 and -0 give FLT_MIN, never a denormal; +inf and NaN stay). */
static inline f32 de__f32_next_up(f32 f) {
    if (f != f)
        return f;
    if (f == 0.0f)
        return FLT_MIN;
    u32 u;
    memcpy(&u, &f, sizeof u);
    if (f > 0.0f) {
        if (u == 0x7F800000u)
            return f;
        ++u;
    } else {
        --u;
    }
    memcpy(&f, &u, sizeof u);
    return f;
}
/** The next f32 below `f` (+0 and -0 give -FLT_MIN; -inf and NaN stay). */
static inline f32 de__f32_next_down(f32 f) {
    return -de__f32_next_up(-f);
}
/** The largest f32 <= x (outward rounding of a lower bound). */
static inline f32 de__f32_round_down(f64 x) {
    f32 f = (f32)x;
    if ((f64)f > x)
        f = de__f32_next_down(f);
    return f;
}
/** The smallest f32 >= x (outward rounding of an upper bound). */
static inline f32 de__f32_round_up(f64 x) {
    f32 f = (f32)x;
    if ((f64)f < x)
        f = de__f32_next_up(f);
    return f;
}

static inline de_vec3 de__v3(f32 x, f32 y, f32 z) {
    de_vec3 r = {x, y, z};
    return r;
}
static inline de_vec3 de__v3_add(de_vec3 a, de_vec3 b) {
    return de__v3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline de_vec3 de__v3_sub(de_vec3 a, de_vec3 b) {
    return de__v3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline de_vec3 de__v3_scale(de_vec3 a, f32 s) {
    return de__v3(a.x * s, a.y * s, a.z * s);
}
static inline f32 de__v3_dot(de_vec3 a, de_vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline de_vec3 de__v3_cross(de_vec3 a, de_vec3 b) {
    return de__v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline de_vec3 de__v3_min(de_vec3 a, de_vec3 b) {
    return de__v3(de__minf(a.x, b.x), de__minf(a.y, b.y), de__minf(a.z, b.z));
}
static inline de_vec3 de__v3_max(de_vec3 a, de_vec3 b) {
    return de__v3(de__maxf(a.x, b.x), de__maxf(a.y, b.y), de__maxf(a.z, b.z));
}
static inline bool de__v3_finite(de_vec3 a) {
    return de__f32_finite(a.x) && de__f32_finite(a.y) && de__f32_finite(a.z);
}

static inline de_dvec3 de__d3(f64 x, f64 y, f64 z) {
    de_dvec3 r = {x, y, z};
    return r;
}
static inline de_dvec3 de__d3_from(de_vec3 v) {
    return de__d3((f64)v.x, (f64)v.y, (f64)v.z);
}
static inline de_dvec3 de__d3_add(de_dvec3 a, de_dvec3 b) {
    return de__d3(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline de_dvec3 de__d3_sub(de_dvec3 a, de_dvec3 b) {
    return de__d3(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline de_dvec3 de__d3_scale(de_dvec3 a, f64 s) {
    return de__d3(a.x * s, a.y * s, a.z * s);
}
static inline f64 de__d3_dot(de_dvec3 a, de_dvec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline de_dvec3 de__d3_cross(de_dvec3 a, de_dvec3 b) {
    return de__d3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline bool de__d3_finite(de_dvec3 a) {
    return de__f64_finite(a.x) && de__f64_finite(a.y) && de__f64_finite(a.z);
}

/** 3x3 matrix in f64, column-major like de_mat3: element (row r, column c) is m[c * 3 + r]. */
typedef struct de__dmat3 {
    f64 m[9];
} de__dmat3;

/** Rotation matrix of q, computed in f64 with q / |q| (a zero q gives the zero matrix). */
static inline de__dmat3 de__dmat3_from_quat(de_quat q) {
    const f64 x = q.x, y = q.y, z = q.z, w = q.w;
    const f64 n = x * x + y * y + z * z + w * w;
    const f64 s = n > 0.0 ? 2.0 / n : 0.0;
    de__dmat3 r;
    r.m[0] = 1.0 - s * (y * y + z * z);
    r.m[1] = s * (x * y + w * z);
    r.m[2] = s * (x * z - w * y);
    r.m[3] = s * (x * y - w * z);
    r.m[4] = 1.0 - s * (x * x + z * z);
    r.m[5] = s * (y * z + w * x);
    r.m[6] = s * (x * z + w * y);
    r.m[7] = s * (y * z - w * x);
    r.m[8] = 1.0 - s * (x * x + y * y);
    if (n <= 0.0)
        memset(&r, 0, sizeof r);
    return r;
}
/** m * v. */
static inline de_dvec3 de__dmat3_mul(const de__dmat3 *m, de_dvec3 v) {
    return de__d3(m->m[0] * v.x + m->m[3] * v.y + m->m[6] * v.z,
                  m->m[1] * v.x + m->m[4] * v.y + m->m[7] * v.z,
                  m->m[2] * v.x + m->m[5] * v.y + m->m[8] * v.z);
}
/** transpose(m) * v (the inverse rotation for a rotation matrix). */
static inline de_dvec3 de__dmat3_tmul(const de__dmat3 *m, de_dvec3 v) {
    return de__d3(m->m[0] * v.x + m->m[1] * v.y + m->m[2] * v.z,
                  m->m[3] * v.x + m->m[4] * v.y + m->m[5] * v.z,
                  m->m[6] * v.x + m->m[7] * v.y + m->m[8] * v.z);
}
/** v rotated by q, in f64 (q normalized in f64), rounded to f32 once. */
static inline de_vec3 de__quat_rotate(de_quat q, de_vec3 v) {
    const de__dmat3 m = de__dmat3_from_quat(q);
    const de_dvec3 r = de__dmat3_mul(&m, de__d3_from(v));
    return de__v3((f32)r.x, (f32)r.y, (f32)r.z);
}

/* ================================ Body pool ============================================ */

/**
 * Bodies live in one preallocated structure of arrays of `capacity` (= max_bodies) slots. Index
 * i of every array is body i. Slots [0, high_water) have been used at least once; `alive[i]`
 * says whether slot i holds a body now. Freed indices go on a LIFO stack and are reused first.
 *
 * Iterate bodies in ascending index (SPEC §13.2 stable ID order):
 *
 *     for (u32 i = 0; i < p->bodies.high_water; ++i)
 *         if (p->bodies.alive[i]) ...
 *
 * Fields of a dead slot are unspecified. Static and kinematic bodies have mass 0, inv_mass 0 and
 * zero inertia; dynamic bodies have mass > 0. Velocities of a static body are zero.
 * `world_aabb` is the shape's world box at the body's current transform, refreshed by
 * de__body_refresh_aabb (on create and set_transform) and de__bodies_refresh_aabbs (each step);
 * the empty box for a body without a shape.
 */
typedef struct de__body_pool {
    u32 capacity;   /**< slots (max_bodies) */
    u32 high_water; /**< slots ever used; iterate [0, high_water) */
    u32 live_count; /**< alive bodies */
    u32 free_count; /**< entries of free_list in use */

    u32 *free_list;  /**< LIFO stack of freed indices [capacity] */
    u32 *generation; /**< generation of the current (or next) body in each slot, 1..4095 */
    u8 *alive;       /**< 1 when the slot holds a body */
    u8 *type;        /**< de_body_type */
    u32 *flags;      /**< de_body_flags */

    de_dvec3 *position;         /**< world position of the body origin (f64) */
    de_quat *rotation;          /**< unit quaternion, body -> world */
    de_vec3 *linear_velocity;   /**< m/s, world */
    de_vec3 *angular_velocity;  /**< rad/s, world axes */
    f32 *mass;                  /**< kg; 0 when not dynamic */
    f32 *inv_mass;              /**< 1 / mass; 0 when not dynamic */
    de_vec3 *local_com;         /**< center of mass in the body frame */
    de_mat3 *local_inertia;     /**< about the center of mass, body axes */
    de_mat3 *local_inv_inertia; /**< inverse of local_inertia; zero when not dynamic */
    f32 *linear_damping;
    f32 *angular_damping;
    f32 *gravity_scale;
    de_shape *shape; /**< invalid handle = no shape */
    de_collision_filter *filter;
    de_material_phys *material; /**< density resolved (never 0) */
    de_entity *owner;
    de_aabb *world_aabb; /**< f32, outward-rounded from f64 */

    void *block; /**< the one allocation behind every array above */
    size_t block_size;
} de__body_pool;

/* ================================ Shape slots ========================================== */

typedef struct de__shape de__shape; /* shapes/shape_internal.h */

typedef struct de__shape_slot {
    de__shape *shape; /**< NULL when free */
    u32 generation;   /**< generation of the current (or next) shape in the slot */
} de__shape_slot;

/* ================================ Extension slots ====================================== */

/**
 * State that later tasks hang off the world without editing world.c: a task stores its state
 * pointer in its slot (lazily, the first time it runs, or from its own init function) with a
 * vtable. The world calls `body_added` after a body is created and `body_removed` before a body
 * is freed (slots in ascending order; either may be NULL), and `destroy` at world destroy (slots
 * in descending order, before bodies and shapes are freed). A slot's state is owned by its task.
 */
typedef enum de__phys_ext_id {
    DE__PHYS_EXT_BROADPHASE = 0, /**< DENG-133 */
    DE__PHYS_EXT_NARROWPHASE,    /**< DENG-134 (optional caches) */
    DE__PHYS_EXT_CONTACTS,       /**< DENG-135 */
    DE__PHYS_EXT_QUERY,          /**< DENG-137 */
    DE__PHYS_EXT_SOLVER,         /**< S23 */
    DE__PHYS_EXT_JOINTS,         /**< S23 */
    DE__PHYS_EXT_CHARACTER,      /**< S23 */
    DE__PHYS_EXT_RESERVED,
    DE__PHYS_EXT_COUNT
} de__phys_ext_id;

typedef struct de__phys_ext_vtbl {
    void (*destroy)(de_physics_world *p, void *state);
    void (*body_added)(de_physics_world *p, void *state, u32 body_index);
    void (*body_removed)(de_physics_world *p, void *state, u32 body_index);
} de__phys_ext_vtbl;

typedef struct de__phys_ext {
    void *state;
    const de__phys_ext_vtbl *vtbl;
} de__phys_ext;

/* ================================ World ================================================ */

struct de_physics_world {
    /* ---- DENG-132: settings ---- */
    const de_allocator *alloc; /**< never NULL */
    de_task_system *tasks;     /**< may be NULL */
    de_vec3 gravity;
    u32 substeps;
    u32 max_bodies;
    u64 tick;    /**< steps since create */
    f32 last_dt; /**< dt of the last step (0 before the first) */

    /* ---- DENG-132: bodies and shapes ---- */
    de__body_pool bodies;
    de__shape_slot *shape_slots; /**< [shape_slot_capacity]; [0, shape_slot_count) used */
    u32 *shape_free;             /**< LIFO stack of free shape slot indices */
    u32 shape_slot_count;
    u32 shape_slot_capacity;
    u32 shape_free_count;
    u32 shape_live_count;

    /* ---- DENG-132: ECS link (ecs/attach.c); invalid until de_physics_attach ---- */
    de_world *ecs_world;
    de_component comp_rigid_body;
    de_component comp_collider;
    de_component comp_character;
    de_resource_id res_physics_world;

    /* ---- Extension slots (later tasks; see de__phys_ext_id) ---- */
    de__phys_ext ext[DE__PHYS_EXT_COUNT];
};

/* ================================ Internal API ========================================= */

/** Log category "physics". */
de_log_category de__physics_log(void);

/** Sets extension slot `id` (state may be NULL to clear it). */
static inline void de__phys_ext_set(de_physics_world *p, de__phys_ext_id id, void *state,
                                    const de__phys_ext_vtbl *vtbl) {
    p->ext[id].state = state;
    p->ext[id].vtbl = vtbl;
}
static inline void *de__phys_ext_get(const de_physics_world *p, de__phys_ext_id id) {
    return p->ext[id].state;
}

/** true when `index` holds a live body. */
static inline bool de__body_alive(const de_physics_world *p, u32 index) {
    return index < p->bodies.high_water && p->bodies.alive[index] != 0;
}
/** The index of live body `b` in *index; false (index untouched) for a stale or invalid id. */
static inline bool de__body_resolve(const de_physics_world *p, de_body b, u32 *index) {
    if (p == NULL || b.id == 0)
        return false;
    const u32 i = de__handle_index(b.id);
    if (!de__body_alive(p, i) || p->bodies.generation[i] != de__handle_gen(b.id))
        return false;
    *index = i;
    return true;
}
/** The handle of live body `index`. */
static inline de_body de__body_handle(const de_physics_world *p, u32 index) {
    de_body b;
    b.id = de__handle_make(index, p->bodies.generation[index]);
    return b;
}

/** body.c: recomputes bodies.world_aabb[index] from the shape and the transform. */
void de__body_refresh_aabb(de_physics_world *p, u32 index);
/** body.c: de__body_refresh_aabb for every live body, in index order. */
void de__bodies_refresh_aabbs(de_physics_world *p);
/** body.c: allocates the body pool (world.c). */
de_result de__body_pool_init(de__body_pool *pool, const de_allocator *a, u32 capacity);
/** body.c: frees the body pool. */
void de__body_pool_free(de__body_pool *pool, const de_allocator *a);
/** shapes/shape.c: frees every shape (world destroy). */
void de__shapes_free_all(de_physics_world *p);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_INTERNAL_H */
