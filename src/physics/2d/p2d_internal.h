/*
 * p2d_internal.h: the 2D physics world, its body pool, its shapes and the deterministic 2D math
 * helpers of src/physics/2d/ (DENG-252). Internal: include as "physics/2d/p2d_internal.h".
 *
 * This is the contract the other S24 tasks build on: collision (DENG-253), solver and islands
 * (DENG-254), joints (DENG-256), queries and the character controller (DENG-258), the plane
 * mapping and ECS systems (DENG-260). The layouts mirror the 3D ones of physics_internal.h and
 * shapes/shape_internal.h; later tasks add their own state through the extension slots
 * (de__p2d_ext_*), not by adding fields to the body pool.
 *
 * Determinism (SPEC §5.7). The files are part of the deterministic physics module (built with
 * -ffp-contract=off). Code here uses only + - * / and de_dsqrt / de_dsqrt64 and the de_d*
 * functions (never <math.h> or the fast de_v2_* functions of de_math.h). Bodies are always
 * visited in ascending body index. Handles use the 3D packing (physics_internal.h).
 */
#ifndef DE_PHYSICS_P2D_INTERNAL_H
#define DE_PHYSICS_P2D_INTERNAL_H

#include "physics/physics_internal.h"

DE_EXTERN_C_BEGIN

/* ================================ Vector helpers ======================================= */

/** An f64 point of the plane (body positions, builders). */
typedef struct de__d2 {
    f64 x, y;
} de__d2;

static inline de_vec2 de__v2(f32 x, f32 y) {
    de_vec2 r = {x, y};
    return r;
}
static inline de_vec2 de__v2_add(de_vec2 a, de_vec2 b) {
    return de__v2(a.x + b.x, a.y + b.y);
}
static inline de_vec2 de__v2_sub(de_vec2 a, de_vec2 b) {
    return de__v2(a.x - b.x, a.y - b.y);
}
static inline de_vec2 de__v2_scale(de_vec2 a, f32 s) {
    return de__v2(a.x * s, a.y * s);
}
static inline f32 de__v2_dot(de_vec2 a, de_vec2 b) {
    return a.x * b.x + a.y * b.y;
}
/** z of the 3D cross product: a.x * b.y - a.y * b.x. */
static inline f32 de__v2_cross(de_vec2 a, de_vec2 b) {
    return a.x * b.y - a.y * b.x;
}
/** (v.y, -v.x): v turned clockwise by 90 degrees (the outward normal side of a CCW edge). */
static inline de_vec2 de__v2_right_perp(de_vec2 v) {
    return de__v2(v.y, -v.x);
}
/** (-v.y, v.x): v turned counter-clockwise by 90 degrees. */
static inline de_vec2 de__v2_left_perp(de_vec2 v) {
    return de__v2(-v.y, v.x);
}
static inline de_vec2 de__v2_min(de_vec2 a, de_vec2 b) {
    return de__v2(de__minf(a.x, b.x), de__minf(a.y, b.y));
}
static inline de_vec2 de__v2_max(de_vec2 a, de_vec2 b) {
    return de__v2(de__maxf(a.x, b.x), de__maxf(a.y, b.y));
}
static inline bool de__v2_finite(de_vec2 a) {
    return de__f32_finite(a.x) && de__f32_finite(a.y);
}

static inline de__d2 de__d2_make(f64 x, f64 y) {
    de__d2 r = {x, y};
    return r;
}
static inline de__d2 de__d2_from(de_vec2 v) {
    return de__d2_make((f64)v.x, (f64)v.y);
}
static inline de__d2 de__d2_add(de__d2 a, de__d2 b) {
    return de__d2_make(a.x + b.x, a.y + b.y);
}
static inline de__d2 de__d2_sub(de__d2 a, de__d2 b) {
    return de__d2_make(a.x - b.x, a.y - b.y);
}
static inline de__d2 de__d2_scale(de__d2 a, f64 s) {
    return de__d2_make(a.x * s, a.y * s);
}
static inline f64 de__d2_dot(de__d2 a, de__d2 b) {
    return a.x * b.x + a.y * b.y;
}
static inline f64 de__d2_cross(de__d2 a, de__d2 b) {
    return a.x * b.y - a.y * b.x;
}
/** (x, y) / |(x, y)| in f64, rounded to f32 once; the zero vector stays zero. */
static inline de_vec2 de__v2_normalize_d(de__d2 v) {
    const f64 l2 = v.x * v.x + v.y * v.y;
    if (!(l2 > 0.0))
        return de__v2(0.0f, 0.0f);
    const f64 inv = 1.0 / de_dsqrt64(l2);
    return de__v2((f32)(v.x * inv), (f32)(v.y * inv));
}

/* ================================ Rotations ============================================ */

static inline de_rot2d de__rot2d_identity(void) {
    de_rot2d r = {1.0f, 0.0f};
    return r;
}
/** r / |r| in f64 (a zero r gives the identity; non-finite gives NaN, like de_dnormalize_quat). */
static inline de_rot2d de__rot2d_normalize(de_rot2d r) {
    const f64 c = r.c, s = r.s;
    const f64 l2 = c * c + s * s;
    if (l2 == 0.0)
        return de__rot2d_identity();
    const f64 inv = 1.0 / de_dsqrt64(l2);
    de_rot2d o = {(f32)(c * inv), (f32)(s * inv)};
    return o;
}
/** v rotated by r in f32: (c x - s y, s x + c y). */
static inline de_vec2 de__rot2d_apply(de_rot2d r, de_vec2 v) {
    return de__v2(r.c * v.x - r.s * v.y, r.s * v.x + r.c * v.y);
}
/** v rotated by the inverse of r in f32. */
static inline de_vec2 de__rot2d_apply_inv(de_rot2d r, de_vec2 v) {
    return de__v2(r.c * v.x + r.s * v.y, -r.s * v.x + r.c * v.y);
}
/** v rotated by r, computed in f64. */
static inline de__d2 de__rot2d_apply_d(de_rot2d r, de__d2 v) {
    const f64 c = r.c, s = r.s;
    return de__d2_make(c * v.x - s * v.y, s * v.x + c * v.y);
}
static inline bool de__rot2d_finite(de_rot2d r) {
    return de__f32_finite(r.c) && de__f32_finite(r.s);
}

/* ================================ AABBs ================================================ */

static inline de_aabb2d de__aabb2d_empty(void) {
    de_aabb2d b;
    b.min = de__v2(FLT_MAX, FLT_MAX);
    b.max = de__v2(-FLT_MAX, -FLT_MAX);
    return b;
}
static inline bool de__aabb2d_is_empty(de_aabb2d b) {
    return b.min.x > b.max.x || b.min.y > b.max.y;
}
/** The f32 box around the f64 box [lo, hi], rounded outward (a few f64 ulps of padding first, so
 *  f64 rounding in a transform can never leave the f32 box inside the exact one). */
de_aabb2d de__aabb2d_from_d(de__d2 lo, de__d2 hi);

/* ================================ Mass ================================================= */

/**
 * Mass properties per unit density, in f64: `area` (m^2), `com` (local), `inertia` about `com`
 * for density 1. At density rho: (rho * area, com, rho * inertia).
 */
typedef struct de__unit_mass2d {
    f64 area;
    de__d2 com;
    f64 inertia;
} de__unit_mass2d;

/* ================================ Shapes =============================================== */

/**
 * A convex polygon (DE_SHAPE2D_POLYGON and DE_SHAPE2D_BOX): `count` (3..8) vertices,
 * counter-clockwise, no three collinear. normals[i] is the outward unit normal of edge
 * vertices[i] -> vertices[(i + 1) % count] (its right perpendicular). `centroid` is the area
 * centroid of the core polygon (an interior point). `radius` rounds it (the shape is the core
 * polygon swept by a disc of that radius). Hull order: the first vertex is the lowest x (then
 * lowest y) of the hull, as the builder found it.
 */
typedef struct de__polygon2d {
    de_vec2 vertices[DE_SHAPE2D_MAX_POLYGON_VERTICES];
    de_vec2 normals[DE_SHAPE2D_MAX_POLYGON_VERTICES];
    de_vec2 centroid;
    f32 radius;
    u32 count;
} de__polygon2d;

/** de__chain_segment.flags. */
enum {
    /** No neighbour before p1 (open chain start): ghost1 is the collinear extension, and the p1
     *  corner is free (all directions of the vertex region are valid). */
    DE__CHAIN_GHOST1_OPEN = 1u << 0,
    /** No neighbour after p2 (open chain end): ghost2 is the collinear extension. */
    DE__CHAIN_GHOST2_OPEN = 1u << 1
};

/**
 * One segment of a chain with its ghost vertices (Box2D's smooth segment): the segment p1 -> p2,
 * ghost1 = the point before p1 on the chain, ghost2 = the point after p2. `normal` is the unit
 * right perpendicular of p2 - p1: the one side that collides. DENG-253 uses the ghosts to drop
 * contacts on internal corners (where the neighbour bends away less than the segment's normal
 * would allow), so a body slides over joints without catching.
 */
typedef struct de__chain_segment {
    de_vec2 ghost1, p1, p2, ghost2;
    de_vec2 normal;
    u32 flags;
} de__chain_segment;

/**
 * A chain (DE_SHAPE2D_CHAIN): `point_count` points copied from the input and `segment_count`
 * segments (point_count - 1 open, point_count for a loop; segment i runs from points[i] to
 * points[(i + 1) % point_count]). Segment i is the sub-shape id i.
 */
typedef struct de__chain2d {
    de_vec2 *points;
    de__chain_segment *segments;
    u32 point_count;
    u32 segment_count;
    bool loop;
} de__chain2d;

typedef struct de__shape2d de__shape2d;

struct de__shape2d {
    de_shape2d_kind kind;
    de_shape handle;
    u32 refcount;         /**< bodies that use it */
    de_aabb2d local_aabb; /**< rounding radius included */
    f32 radius;           /**< convex radius: circle / capsule radius, polygon rounding, else 0 */
    f32 bounding_radius;  /**< max distance from the local origin to the shape */
    bool has_area;        /**< true for circle, capsule, box, polygon */
    de__unit_mass2d unit_mass;
    union {
        struct {
            de_vec2 center;
            f32 radius;
        } circle;
        struct {
            de_vec2 p1, p2;
            f32 radius;
        } capsule;
        struct {
            de_vec2 p1, p2;
            de_vec2 normal; /**< right perpendicular of p2 - p1 (the segment is two-sided) */
        } segment;
        de__polygon2d polygon; /**< DE_SHAPE2D_POLYGON and DE_SHAPE2D_BOX */
        de__chain2d chain;
    } u;
    void *data; /**< the one allocation of the kind's arrays (chain), else NULL */
    size_t data_size;
};

typedef struct de__shape2d_slot {
    de__shape2d *shape; /**< NULL when free */
    u32 generation;
} de__shape2d_slot;

/* ================================ Body pool ============================================ */

/**
 * Bodies live in one preallocated structure of arrays of `capacity` (= max_bodies) slots, like
 * the 3D pool. Iterate in ascending index (SPEC §13.2 stable ID order):
 *
 *     for (u32 i = 0; i < p->bodies.high_water; ++i)
 *         if (p->bodies.alive[i]) ...
 *
 * Fields of a dead slot are unspecified. Static and kinematic bodies have mass 0, inv_mass 0,
 * inertia 0 and inv_inertia 0; dynamic bodies have mass > 0 (inv_inertia 0 with
 * DE_BODY2D_FLAG_FIXED_ROTATION). Velocities of a static body are zero. `world_aabb` is the
 * shape's box at the current transform (de__body2d_refresh_aabb on create and set_transform,
 * de__bodies2d_refresh_aabbs each step); the empty box without a shape.
 */
typedef struct de__body2d_pool {
    u32 capacity;
    u32 high_water;
    u32 live_count;
    u32 free_count;

    u32 *free_list;  /**< LIFO stack of freed indices [capacity] */
    u32 *generation; /**< 1..4095 */
    u8 *alive;
    u8 *type;   /**< de_body_type */
    u32 *flags; /**< de_body_flags */

    de__d2 *position;         /**< plane position of the body origin (f64) */
    de_rot2d *rotation;       /**< unit (cos, sin), body -> plane */
    de_vec2 *linear_velocity; /**< m/s */
    f32 *angular_velocity;    /**< rad/s, counter-clockwise */
    f32 *mass;                /**< kg; 0 when not dynamic */
    f32 *inv_mass;            /**< 1 / mass; 0 when not dynamic */
    f32 *inertia;             /**< about the center of mass; 0 when not dynamic */
    f32 *inv_inertia;         /**< 1 / inertia; 0 when not dynamic or fixed rotation */
    de_vec2 *local_com;       /**< center of mass in the body frame */
    f32 *linear_damping;
    f32 *angular_damping;
    f32 *gravity_scale;
    de_shape *shape; /**< invalid handle = no shape */
    de_collision_filter *filter;
    de_material_phys *material; /**< density resolved (never 0) */
    de_entity *owner;
    de_aabb2d *world_aabb; /**< f32, outward-rounded from f64 */

    void *block; /**< the one allocation behind every array above */
    size_t block_size;
} de__body2d_pool;

/* ================================ Extension slots ====================================== */

/** Same contract as de__phys_ext (physics_internal.h), for the 2D world. */
typedef enum de__p2d_ext_id {
    DE__P2D_EXT_BROADPHASE = 0, /**< DENG-253 */
    DE__P2D_EXT_CONTACTS,       /**< DENG-253 */
    DE__P2D_EXT_SOLVER,         /**< DENG-254 (islands, sleeping, coloring) */
    DE__P2D_EXT_JOINTS,         /**< DENG-256 */
    DE__P2D_EXT_QUERY,          /**< DENG-258 */
    DE__P2D_EXT_CHARACTER,      /**< DENG-258 */
    DE__P2D_EXT_ECS,            /**< DENG-260 */
    DE__P2D_EXT_RESERVED,
    DE__P2D_EXT_COUNT
} de__p2d_ext_id;

typedef struct de__p2d_ext_vtbl {
    void (*destroy)(de_physics2d_world *p, void *state);
    void (*body_added)(de_physics2d_world *p, void *state, u32 body_index);
    void (*body_removed)(de_physics2d_world *p, void *state, u32 body_index);
} de__p2d_ext_vtbl;

typedef struct de__p2d_ext {
    void *state;
    const de__p2d_ext_vtbl *vtbl;
} de__p2d_ext;

/* ================================ World ================================================ */

struct de_physics2d_world {
    /* ---- DENG-252: settings ---- */
    const de_allocator *alloc; /**< never NULL */
    de_task_system *tasks;     /**< may be NULL */
    de_vec2 gravity;
    u32 substeps;
    u32 max_bodies;
    de_physics2d_plane plane;
    u64 tick;
    f32 last_dt;

    /* ---- DENG-252: bodies and shapes ---- */
    de__body2d_pool bodies;
    de__shape2d_slot *shape_slots; /**< [shape_slot_capacity]; [0, shape_slot_count) used */
    u32 *shape_free;
    u32 shape_slot_count;
    u32 shape_slot_capacity;
    u32 shape_free_count;
    u32 shape_live_count;

    /* ---- Extension slots (later tasks) ---- */
    de__p2d_ext ext[DE__P2D_EXT_COUNT];
};

/* ================================ Internal API ========================================= */

static inline void de__p2d_ext_set(de_physics2d_world *p, de__p2d_ext_id id, void *state,
                                   const de__p2d_ext_vtbl *vtbl) {
    p->ext[id].state = state;
    p->ext[id].vtbl = vtbl;
}
static inline void *de__p2d_ext_get(const de_physics2d_world *p, de__p2d_ext_id id) {
    return p->ext[id].state;
}

static inline bool de__body2d_alive(const de_physics2d_world *p, u32 index) {
    return index < p->bodies.high_water && p->bodies.alive[index] != 0;
}
/** The index of live body `b` in *index; false (index untouched) for a stale or invalid id. */
static inline bool de__body2d_resolve(const de_physics2d_world *p, de_body b, u32 *index) {
    if (p == NULL || b.id == 0)
        return false;
    const u32 i = de__handle_index(b.id);
    if (!de__body2d_alive(p, i) || p->bodies.generation[i] != de__handle_gen(b.id))
        return false;
    *index = i;
    return true;
}
static inline de_body de__body2d_handle(const de_physics2d_world *p, u32 index) {
    de_body b;
    b.id = de__handle_make(index, p->bodies.generation[index]);
    return b;
}

/* body2d.c */
de_result de__body2d_pool_init(de__body2d_pool *pool, const de_allocator *a, u32 capacity);
void de__body2d_pool_free(de__body2d_pool *pool, const de_allocator *a);
void de__body2d_refresh_aabb(de_physics2d_world *p, u32 index);
void de__bodies2d_refresh_aabbs(de_physics2d_world *p);

/* shapes2d.c */
/** The live shape `s` of `p`, or NULL for a stale or invalid handle. */
de__shape2d *de__shape2d_get(const de_physics2d_world *p, de_shape s);
/** Frees every shape (world destroy). */
void de__shapes2d_free_all(de_physics2d_world *p);
/** Puts a built shape into a slot (allocating slots if needed). On failure the caller frees it. */
de_result de__shape2d_insert(de_physics2d_world *p, de__shape2d *s);
/** Frees a shape that never got a slot (builder failure). */
void de__shape2d_free(de_physics2d_world *p, de__shape2d *s);

/** true for the kinds with a support function: circle, capsule, box, polygon, segment. */
static inline bool de__shape2d_kind_is_convex(de_shape2d_kind k) {
    return k == DE_SHAPE2D_CIRCLE || k == DE_SHAPE2D_CAPSULE || k == DE_SHAPE2D_BOX ||
           k == DE_SHAPE2D_POLYGON || k == DE_SHAPE2D_SEGMENT;
}
/**
 * Support point of the core of a convex shape in its local frame: the core point with the
 * largest dot(point, dir). Circle core = its center; capsule and segment core = the segment;
 * polygon core = the polygon without rounding. Ties: the lowest vertex index (p1 before p2).
 * `dir` need not be unit length; zero is allowed. Not for chains (returns the origin).
 */
de_vec2 de__shape2d_support_core(const de__shape2d *s, de_vec2 dir);
/** Support of the whole shape: core support + radius * dir / |dir| (+x for a zero dir). */
de_vec2 de__shape2d_support(const de__shape2d *s, de_vec2 dir);
/**
 * The plane box of shape `s` placed at (`pos`, `rot`): computed in f64 and rounded outward to
 * f32, so it always contains the shape. Exact for every kind but the chain (the chain's local
 * box, transformed).
 */
de_aabb2d de__shape2d_world_aabb(const de__shape2d *s, de__d2 pos, de_rot2d rot);
/** Box of a box `local` after rotation `rot` (f64) and translation `pos`, rounded outward. The
 *  empty box stays empty. */
de_aabb2d de__aabb2d_transform(de_aabb2d local, de__d2 pos, de_rot2d rot);

/* polygon2d.c */
/** The automatic hull tolerance of points[0..n): max(1e-5, 1e-4 * the longest side of their box,
 *  4e-7 * their largest |coordinate|). */
f32 de__poly2d_auto_tolerance(const de_vec2 *points, u32 n);
/**
 * Builds the convex hull of points[0..n) (n <= DE_SHAPE2D_MAX_POLYGON_INPUT, finite) with weld /
 * collinearity tolerance `tol` > 0 into *out (radius set to `radius`). Deterministic: the same
 * points in the same order give the same bytes (points are welded in input order, then sorted by
 * (x, y) and wrapped by Andrew's monotone chain in f64).
 * @return DE_OK or DE_ERR_INVALID_ARGUMENT (fewer than 3 hull vertices, all points on one line
 *         within tol, or more than DE_SHAPE2D_MAX_POLYGON_VERTICES hull vertices).
 */
de_result de__poly2d_build(const de_vec2 *points, u32 n, f32 tol, f32 radius, de__polygon2d *out);
/** Fills normals and centroid of a polygon whose CCW vertices and count are set. false when an
 *  edge has zero length or the area is not positive. */
bool de__poly2d_finish(de__polygon2d *poly);

/* chain2d.c */
/** Validates the points and fills s (kind, chain arrays in s->data, local box). */
de_result de__chain2d_build(de_physics2d_world *p, const de_vec2 *points, u32 count, bool loop,
                            de__shape2d *s);

/* mass2d.c */
void de__mass2d_circle(f64 r, de__d2 center, de__unit_mass2d *out);
/** Capsule p1..p2 swept by r. */
void de__mass2d_capsule(de__d2 p1, de__d2 p2, f64 r, de__unit_mass2d *out);
/** Polygon (CCW vertices) rounded by poly->radius: exact (core + edge strips + corner sectors). */
void de__mass2d_polygon(const de__polygon2d *poly, de__unit_mass2d *out);
/** Mass properties at `density` (kg/m^2), rounded to f32. */
void de__unit_mass2d_scale(const de__unit_mass2d *u, f64 density, de_mass_props2d *out);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_P2D_INTERNAL_H */
