/*
 * query_internal.h: scene queries (DENG-137, SPEC §13.1). Internal: include as
 * "physics/query/query_internal.h".
 *
 * Files (src/physics/query/):
 * - raycast.c: filters, candidate enumeration (broadphase trees, or every body when the
 *   broadphase does not exist yet), de_physics_raycast / raycast_all and the per-body ray test.
 * - ray_shapes.c: ray against one shape in its own frame, in f64: sphere, capsule, box (slab),
 *   cylinder, hull (plane clipping), mesh (BVH + watertight ray-triangle), heightfield (2D DDA
 *   over cells), compound (child BVH).
 * - shape_cast.c: conservative advancement on GJK distance (narrow/narrowphase.h) for convex
 *   against convex, and against the triangles of meshes and heightfields; compounds on either
 *   side.
 * - overlap.c: GJK boolean overlap. closest_point.c: GJK distance and point-triangle distance.
 *
 * Every public query is "best per body, then a fixed order over bodies". The per-body functions
 * below are pure (they depend only on the world, the body and the query, never on the order in
 * which bodies are visited or on the broadphase tree), and the selection over bodies uses fixed
 * keys: (fraction, body index) for rays and casts, body index for overlaps. So results equal a
 * brute-force loop over all bodies (the unit tests check exactly that).
 *
 * Determinism (SPEC §5.7): + - * / and de_dsqrt64 in f64 (shape data is f32, converted exactly),
 * no FP contraction (module flag), fixed traversal stacks, ties to the lower index. Large worlds:
 * a body's offset from the query origin is formed in f64 before anything is rounded.
 *
 * Threading: read-only. Nothing here writes to the world, the broadphase or any static state.
 */
#ifndef DE_PHYSICS_QUERY_INTERNAL_H
#define DE_PHYSICS_QUERY_INTERNAL_H

#include "physics/broadphase/broadphase.h"
#include "physics/narrow/narrowphase.h"

DE_EXTERN_C_BEGIN

/** "No body" in de__qfilter.ignore_index. */
#define DE__Q_NO_BODY 0xFFFFFFFFu

/**
 * Shape cast tolerance (m): conservative advancement stops when the shapes are closer than this
 * (plus two f32 steps of their relative offset, see shape_cast.c).
 */
#define DE__Q_CAST_TOL 1e-5
/** Most conservative-advancement iterations per (sub-)shape pair. */
#define DE__Q_CAST_MAX_ITER 64u

/* ================================ Filters ============================================== */

/** A resolved query filter. */
typedef struct de__qfilter {
    de_collision_filter filter; /**< all-zero input replaced by the body default */
    u32 flags;                  /**< de_physics_query_flags */
    u32 ignore_index;           /**< body index never reported, or DE__Q_NO_BODY */
} de__qfilter;

/** Resolves `in` (NULL = defaults). A stale `ignore_body` ignores nothing. */
void de__qfilter_init(const de_physics_world *p, const de_physics_query_filter *in,
                      de__qfilter *out);
/** The filter of the plain API: `filter` with no flags. */
void de__qfilter_plain(de_collision_filter filter, de__qfilter *out);

/** true when body `i` may be reported: alive, has a shape, not the ignored body, not a trigger
 *  (unless DE_PHYSICS_QUERY_INCLUDE_TRIGGERS) and de__collision_filter_test(query, body) passes. */
static inline bool de__query_accept(const de_physics_world *p, u32 i, const de__qfilter *f) {
    if (!de__body_alive(p, i) || p->bodies.shape[i].id == 0u || i == f->ignore_index)
        return false;
    if ((p->bodies.flags[i] & (u32)DE_BODY_FLAG_TRIGGER) != 0u &&
        (f->flags & (u32)DE_PHYSICS_QUERY_INCLUDE_TRIGGERS) == 0u)
        return false;
    return de__collision_filter_test(f->filter, p->bodies.filter[i]);
}

/* ================================ Small f64 helpers ===================================== */

/** v / |v| (f64); v unchanged when its length is 0 or not finite. */
static inline de_dvec3 de__q_dnorm(de_dvec3 v) {
    const f64 l2 = de__d3_dot(v, v);
    if (!(l2 > 0.0) || !de__f64_finite(l2))
        return v;
    return de__d3_scale(v, 1.0 / de_dsqrt64(l2));
}
static inline de_vec3 de__q_to_f32(de_dvec3 v) {
    return de__v3((f32)v.x, (f32)v.y, (f32)v.z);
}

/**
 * Slab test of the segment o + t * d, t in [0, max_t], against the closed box [lo, hi] (f64).
 * true with the entry parameter (0 when o is inside) when they touch.
 */
bool de__q_slab(de_dvec3 o, de_dvec3 d, de_dvec3 lo, de_dvec3 hi, f64 max_t, f64 *t_enter);

/** Next f32 bound above `fraction` (at most `limit`): the traversal bound that still visits
 *  every node a hit rounding to `fraction` could be in. */
static inline f32 de__q_bound(f32 fraction, f32 limit) {
    const f32 b = de__f32_next_up(fraction);
    return b < limit ? b : limit;
}

/** f64 product of two unit quaternions (a then b: a * b), renormalized (zero = identity). */
de_quat de__q_quat_mul(de_quat a, de_quat b);
/** q normalized in f64 (a zero quaternion gives the identity). */
static inline de_quat de__q_quat_normalize(de_quat q) {
    const de_quat id = {0.0f, 0.0f, 0.0f, 1.0f};
    return de__q_quat_mul(id, q);
}

/* ================================ Candidate enumeration ================================= */

/** Called for each accepted body; return false to stop. */
typedef bool (*de__q_body_fn)(void *user, u32 body);

/**
 * Calls fn for every accepted body whose world AABB overlaps `box` (closed boxes). Uses the
 * broadphase trees when the broadphase exists (static tree, then dynamic tree), else every body
 * in ascending index. Callers must not depend on the visiting order.
 */
void de__query_each_aabb(const de_physics_world *p, de_aabb box, const de__qfilter *f,
                         de__q_body_fn fn, void *user);

/* ================================ Rays ================================================== */

/**
 * A query segment: origin + t * delta, t in [0, 1]. `delta32` is normalize(dir) * max_dist
 * computed in f64 and rounded to f32 once (the broadphase ray uses it); `delta` is the same
 * value in f64, used by every shape test, so the trees and the shapes see the same segment.
 */
typedef struct de__qray {
    de_dvec3 origin;
    de_vec3 delta32;
    de_dvec3 delta;
    de_vec3 dir; /**< unit direction (f32), for the start-inside normal -dir */
} de__qray;

/** false for a non-finite origin, a zero or non-finite dir, or max_dist <= 0 / not finite. */
bool de__qray_init(de__qray *r, de_dvec3 origin, de_vec3 dir, f32 max_dist);

/**
 * A ray hit in a shape's frame: parameter t, unit normal facing the ray (local axes), sub-shape,
 * and `inside` (the ray starts inside a solid: t = 0, normal unused).
 */
typedef struct de__qlocal_hit {
    f64 t;
    de_dvec3 normal;
    u32 sub;
    bool inside;
} de__qlocal_hit;

/**
 * ray_shapes.c: the first hit of o + t * d, t in [0, max_t] (inclusive), with shape `s` in its
 * frame. Ties inside a shape go to the lower sub-shape. `flags`:
 * DE_PHYSICS_QUERY_IGNORE_BACK_FACES. The result for a hit with t <= max_t does not depend on
 * max_t.
 */
bool de__ray_shape(const de__shape *s, de_dvec3 o, de_dvec3 d, f64 max_t, u32 flags,
                   de__qlocal_hit *out);

/**
 * Ray against live body `body` (its shape at its pose), t in [0, max_t]. Fills every field of
 * *out (world point, world normal, fraction = (f32)t, sub-shape, body handle, owner entity).
 * Filters are not checked here. This is the brute-force reference of the tests.
 */
bool de__query_ray_body(const de_physics_world *p, u32 body, const de__qray *ray, f64 max_t,
                        u32 flags, de_ray_hit *out);

/** (fraction, body index) order of hits: true when a sorts before b. */
static inline bool de__q_hit_less(const de_ray_hit *a, const de_ray_hit *b) {
    if (a->fraction != b->fraction)
        return a->fraction < b->fraction;
    return de__handle_index(a->body.id) < de__handle_index(b->body.id);
}

/* ================================ Shape casts =========================================== */

/** A cast: `shape` (convex or compound of convex) from (position, rotation) along delta. */
typedef struct de__qcast {
    const de__shape *shape;
    de_dvec3 position;
    de_quat rotation;
    de_dvec3 delta; /**< normalize(dir) * max_dist (f64) */
    de_vec3 dir;    /**< unit (f32), for the start-overlap normal */
    de_aabb start;  /**< world box at the start */
    de_aabb swept;  /**< union of the world boxes at t = 0 and t = 1 */
} de__qcast;

/** false for an unsupported shape (mesh, heightfield, compound holding one) or bad input. */
bool de__qcast_init(de__qcast *c, const de__shape *shape, const de_transform *from, de_vec3 dir,
                    f32 max_dist);

/**
 * Cast against live body `body`, t in [0, max_t]. Fills *out like de__query_ray_body (point on
 * the body, normal facing the cast). Pure: the brute-force reference of the tests.
 */
bool de__query_cast_body(const de_physics_world *p, u32 body, const de__qcast *c, f64 max_t,
                         u32 flags, de_ray_hit *out);

/* ================================ Overlap and closest point ============================= */

/** A query shape placed in the world. */
typedef struct de__qshape {
    const de__shape *shape;
    de__pose pose;
    de_aabb box; /**< world box */
} de__qshape;

/** false for an unsupported shape (as de__qcast_init) or bad input. */
bool de__qshape_init(de__qshape *q, const de__shape *shape, const de_transform *at);

/** true when `q` overlaps or touches body `body`'s shape (triangles two-sided). */
bool de__query_overlap_body(const de_physics_world *p, u32 body, const de__qshape *q);

/**
 * Closest point of body `body`'s shape (solid) to `point`, if within max_dist. Fills *out as
 * de_physics_closest_point documents (fraction = distance / max_dist, 0 when max_dist is 0).
 */
bool de__query_closest_body(const de_physics_world *p, u32 body, de_dvec3 point, f64 max_dist,
                            de_ray_hit *out);

/* ================================ Shared by shape_cast.c / overlap.c / closest_point.c == */

/** A (sub-)shape at a world pose; `sub` is the sub-shape id reported for it (compound child) or
 *  DE_SUB_SHAPE_NONE. */
typedef struct de__qside {
    const de__shape *shape;
    de__pose pose;
    u32 sub;
} de__qside;

/** World pose of child `i` of compound side `c`. */
de__qside de__q_child_side(const de__qside *c, u32 i);

/**
 * Calls fn(user, triangle sub-shape id, a, b, c) for every non-degenerate triangle of mesh or
 * heightfield `s` whose box overlaps `box` (s's frame); stops when fn returns false. Heightfield
 * holes are skipped. The order is fixed (mesh: BVH order, left child first, fixed stack;
 * heightfield: rows, then columns) but not by sub-shape id: callers that pick a best triangle
 * compare full keys, so their result does not depend on it.
 */
typedef bool (*de__q_tri_fn)(void *user, u32 sub, de_vec3 a, de_vec3 b, de_vec3 c);
void de__q_triangles(const de__shape *s, de_aabb box, de__q_tri_fn fn, void *user);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_QUERY_INTERNAL_H */
