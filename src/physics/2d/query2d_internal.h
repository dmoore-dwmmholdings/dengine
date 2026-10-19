/*
 * query2d_internal.h: the 2D scene queries (DENG-258, SPEC §13.3), the 2D mirror of the 3D
 * queries of DENG-137 (physics/query/). Internal: include as "physics/2d/query2d_internal.h".
 *
 * Files (src/physics/2d/):
 * - query2d.c: filters, the candidate walks (broadphase trees or brute force), per-body point and
 *   overlap tests, de_physics2d_overlap / overlap_aabb / closest_point.
 * - raycast2d.c: analytic rays against every shape kind, de_physics2d_raycast / _ex / _all.
 * - shapecast2d.c: 2D GJK distance, conservative-advancement shape casts, de_physics2d_shape_cast.
 * - character2d.c: the character controller, built on the per-body functions below.
 *
 * Candidates. Once the broadphase exists (DENG-253; the first step, or de__bp2d_ensure), the
 * queries walk its two trees (fat boxes, whole chains) and then test each candidate exactly.
 * Before that they test every live body in ascending index against its world box. Either way the
 * result is selected by fixed keys (fraction or distance, then body index, then sub-shape), never
 * by the visiting order, so the tree shape and the walk do not change any result. Bodies moved by
 * de_body2d_set_transform are found by the tree walk at their old place until the next
 * de__bp2d_sync / update (the same teleport lag as 3D); the character controller syncs first.
 *
 * Frames. Rays and point queries run in the body's local frame in f64 (the query point is
 * brought there with an f64 offset). Casts and overlaps run in the "query frame": world axes
 * with the origin at the query shape's position, the other body's vertices offset in f64 and
 * rounded to f32 once (as the narrowphase pair frame), so 100 km out behaves like the origin.
 *
 * Chains are one-sided for rays and casts: a ray hits a segment only from its front (the right
 * side, de__chain_segment.normal), and a cast is stopped by a segment only when it moves against
 * the normal and the cast shape starts in front of the segment (de__q2_in_front: its deepest point
 * at most DE__Q2_ONE_WAY_SLOP behind the segment's line, or its core clear of the segment on the
 * front side). That makes one-way platforms: shapes pass up through a chain whose normal faces up
 * and land on it. Overlaps and closest points treat segments and chains as two-sided lines.
 *
 * Determinism (SPEC §5.7): + - * /, de_dsqrt / de_dsqrt64 only; fixed iteration caps (GJK 20,
 * casts 32); ties to the lower body index, then the lower sub-shape. Nothing allocates.
 */
#ifndef DE_PHYSICS_QUERY2D_INTERNAL_H
#define DE_PHYSICS_QUERY2D_INTERNAL_H

#include "physics/2d/broadphase2d.h"
#include "physics/2d/narrow2d.h"
#include "physics/2d/p2d_internal.h"

DE_EXTERN_C_BEGIN

/* ================================ Constants ============================================= */

/** "No body" in body-index fields. */
#define DE__Q2_NO_BODY 0xFFFFFFFFu
/** GJK iterations (Box2D's cap). */
#define DE__Q2_GJK_MAX_ITER 20u
/** Conservative-advancement iterations of one cast against one shape. */
#define DE__Q2_CAST_MAX_ITER 32u
/** A cast stops when the surfaces are this close (m): about the 3D cast's 1e-5 m. */
#define DE__Q2_CAST_TOL 1e-5
/** How far a shape may reach behind a chain segment's line and still be stopped by it (m). */
#define DE__Q2_ONE_WAY_SLOP DE__P2D_LINEAR_SLOP
/** Sorted body indices per pass of the overlap collection (stack only). */
#define DE__Q2_CHUNK 64u

/* ================================ Filters =============================================== */

/** A de_physics_query_filter resolved against a world. */
typedef struct de__q2filter {
    de_collision_filter filter; /**< all-zero input resolved to the body default */
    u32 flags;                  /**< de_physics_query_flags */
    u32 ignore;                 /**< body index never reported, or DE__Q2_NO_BODY */
} de__q2filter;

/** Resolves `f` (NULL = defaults). A stale ignore_body ignores nothing. */
void de__q2filter_init(const de_physics2d_world *p, const de_physics_query_filter *f,
                       de__q2filter *out);

/** Body `b` (any index) is a query candidate: alive, with a shape, not ignored, not a trigger
 *  (unless DE_PHYSICS_QUERY_INCLUDE_TRIGGERS), and its filter passes the query's. */
bool de__q2_candidate(const de_physics2d_world *p, u32 b, const de__q2filter *f);

/* ================================ Candidate walks ======================================= */

/** Called for each candidate body; return false to stop. */
typedef bool (*de__q2_body_fn)(void *user, u32 body);

/** Calls `fn` for every candidate whose (fat) box overlaps `box`. Visiting order unspecified.
 *  Returns false when `fn` stopped. */
bool de__q2_each_aabb(const de_physics2d_world *p, de_aabb2d box, const de__q2filter *f,
                      de__q2_body_fn fn, void *user);

/* ================================ Hits ================================================== */

/** One internal hit. */
typedef struct de__q2hit {
    f64 t;          /**< fraction of the ray or cast in [0, 1], or the distance (closest point) */
    de__d2 point;   /**< world, on the body's surface (ray, cast) or the closest point */
    de_vec2 normal; /**< unit, facing the ray / caster / query point (zero when inside a solid) */
    u32 body;
    u32 sub;     /**< chain segment, else DE_SUB_SHAPE_NONE */
    bool inside; /**< the ray or cast started inside / overlapping (fraction 0) */
} de__q2hit;

/** (t, body, sub) order: true when a goes before b. */
static inline bool de__q2hit_before(const de__q2hit *a, const de__q2hit *b) {
    if (a->t != b->t)
        return a->t < b->t;
    if (a->body != b->body)
        return a->body < b->body;
    return a->sub < b->sub;
}

/** The public hit of an internal one. */
de_ray_hit2d de__q2hit_public(const de_physics2d_world *p, const de__q2hit *h);

/* ================================ Rays (raycast2d.c) ==================================== */

/** A ray segment: origin + s * dir for s in [0, length], dir unit (f64). */
typedef struct de__q2ray {
    de__d2 origin;
    de__d2 dir;
    f64 length;
} de__q2ray;

/** Builds a ray from public arguments; false for non-finite input, a zero dir or
 *  max_dist <= 0. */
bool de__q2ray_init(de__q2ray *r, de__d2 origin, de_vec2 dir, f32 max_dist);

/**
 * The first hit of ray `r` on body `b`'s shape (b must be alive with a shape), at most at fraction
 * `max_t`: a ray starting inside a solid hits at fraction 0 (inside, normal -dir, point = origin).
 * Chains: front side only, the first segment by (fraction, segment index).
 */
bool de__q2_ray_body(const de_physics2d_world *p, u32 b, const de__q2ray *r, f64 max_t,
                     de__q2hit *out);

/** The closest hit of `r` over the candidates of `f`, in the order of de_physics2d_raycast_ex
 *  (fraction as f32, then body index, then segment). */
bool de__q2_raycast_closest(const de_physics2d_world *p, const de__q2ray *r, const de__q2filter *f,
                            de__q2hit *out);

/* ================================ Convex proxies and GJK (shapecast2d.c) ================= */

/** A convex core: `count` (1..8) points and a radius. */
typedef struct de__q2proxy {
    de_vec2 v[DE_SHAPE2D_MAX_POLYGON_VERTICES];
    u32 count;
    f32 radius;
} de__q2proxy;

/** The core of a convex shape (circle, capsule, box, polygon, segment) rotated by `rot` and
 *  offset by `offset` (f64, rounded once per point). */
void de__q2proxy_shape(de__q2proxy *out, const de__shape2d *s, de_rot2d rot, de__d2 offset);
/** A two-point proxy of radius 0 (a segment, a chain segment). */
void de__q2proxy_segment(de__q2proxy *out, de_vec2 a, de_vec2 b);

/** GJK result: core distance and closest core points, normal from A toward B. */
typedef struct de__q2dist {
    f32 distance;   /**< between the cores (0 when they overlap) */
    de_vec2 pa, pb; /**< closest points on the cores */
    de_vec2 normal; /**< unit, A toward B; zero when overlapping or coincident */
    bool overlap;   /**< the cores overlap (the simplex enclosed the origin) */
} de__q2dist;

/** Distance between the cores of a and b (same frame), Box2D's GJK with a fixed cap. */
void de__q2_gjk(const de__q2proxy *a, const de__q2proxy *b, de__q2dist *out);

/**
 * The one-way rule of chain segments: shape `s` (same frame as the segment a..b with unit front
 * normal n) is in front of the segment when its deepest point along -n is at most
 * DE__Q2_ONE_WAY_SLOP behind the segment's line, or when its core is clear of the segment and on
 * the front side of it (the direction between the closest core points has a positive n part: a
 * capsule resting on the segment's end with its rounding below the line). A shape that crosses
 * the segment, or comes from behind it, is not.
 */
bool de__q2_in_front(const de__q2proxy *s, de_vec2 a, de_vec2 b, de_vec2 n);

/** a and b overlap or touch (core distance <= summed radii). */
bool de__q2_proxy_overlap(const de__q2proxy *a, const de__q2proxy *b);

/* ================================ Shape casts (shapecast2d.c) =========================== */

/** A cast: a convex shape at a start pose sweeping by `delta` (f64, world). */
typedef struct de__q2cast {
    const de__shape2d *shape;
    de__d2 origin;     /**< start position (the query frame's origin) */
    de_rot2d rotation; /**< normalized */
    de__d2 delta;      /**< the whole motion */
    de_vec2 delta_f;   /**< the same, f32 (query frame) */
    de__q2proxy proxy; /**< the core at the start, in the query frame */
    de_aabb2d swept;   /**< world box of the whole sweep (outward-rounded) */
} de__q2cast;

/** Prepares a cast; false for a shape that is not convex (chain) or non-finite input. */
bool de__q2cast_init(de__q2cast *c, const de__shape2d *s, de__d2 position, de_rot2d rot,
                     de__d2 delta);

/**
 * The first time body `b`'s shape stops cast `c`: t in [0, 1], point on the body's surface,
 * normal facing the cast. A cast that starts overlapping the body (surfaces at or past touching)
 * hits at t 0 with `inside` set, normal -dir and point = the start position; one that starts
 * within DE__Q2_CAST_TOL and moves into the body hits at t 0 with its real normal. Chains:
 * one-sided (see the file comment), first segment by (t, segment).
 */
bool de__q2_cast_body(const de_physics2d_world *p, u32 b, const de__q2cast *c, de__q2hit *out);

/* ================================ Point and overlap (query2d.c) ========================= */

/**
 * Closest point of body `b`'s shape to `q` (solid kinds: inside counts, distance 0). On success
 * out->t is the distance, out->point the closest point, out->normal from it toward q (zero
 * inside a solid; a segment's normal when q lies on it). Returns false only for a chain with no
 * segment within `max_dist`.
 */
bool de__q2_point_body(const de_physics2d_world *p, u32 b, de__d2 q, f64 max_dist, de__q2hit *out);

/** Body `b`'s shape overlaps or touches proxy `q`, given in the query frame with origin `qo`.
 *  `qbox` is q's world box (chain segment culling). */
bool de__q2_overlap_body(const de_physics2d_world *p, u32 b, const de__q2proxy *q, de__d2 qo,
                         de_aabb2d qbox);

/** The box of world box `box` seen in the frame of body pose (pos, rot): computed in f64 and
 *  rounded outward, grown by `grow` (>= 0). */
de_aabb2d de__q2_box_in_frame(de_aabb2d box, de__d2 pos, de_rot2d rot, f32 grow);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_QUERY2D_INTERNAL_H */
