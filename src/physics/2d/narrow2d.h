/*
 * narrow2d.h: the 2D narrowphase (DENG-253, SPEC §13.3): contact sets of two shapes at two poses,
 * Box2D v3-style manifold functions, chain segments with ghost vertices, and the chain segment
 * bounds. Internal: include as "physics/2d/narrow2d.h". Mirrors the 3D design of DENG-134
 * (narrow/narrowphase.h): canonical pair order, 16+16-bit feature ids, computation in A's frame.
 *
 * Contact set. de__collide2d gives at most one set per call: a unit normal (plane axes, from A
 * toward B) and up to 2 points. A point has an anchor on A's surface in A's body frame, an anchor
 * on B's surface in B's body frame, and a separation along the normal (negative = overlap); the
 * separation is dot(world_b - world_a, normal) up to f32 rounding. Points whose separation is
 * above the speculative distance are not reported, so a set with points means "touching" in the
 * sense of DENG-135 (within the speculative margin).
 *
 * Pair functions (A first; the canonical order below decides which shape is A):
 *   circle-circle, capsule-circle, capsule-capsule, polygon-circle, polygon-capsule,
 *   polygon-polygon (SAT on rounded polygons + reference-edge clipping; a vertex-vertex path with
 *   the true normal when the cores are apart), segment-* (a segment is a capsule of radius 0, and a
 *   two-vertex polygon against polygons), chain segment-circle and chain segment-polygon/capsule/
 *   segment (one-sided, ghost vertices: Box2D v3's smooth segments). Chain against chain gives
 *   nothing. A capsule whose two centers coincide is a circle.
 *
 * Canonical order. Shapes are ranked circle < capsule < box = polygon < segment < chain; the
 * higher rank is A. On equal ranks the lower shape handle is A, then the pose that compares lower
 * as (x, y, rotation c, rotation s). collide(B, A) computes the same thing as collide(A, B) and
 * swaps it (negated normal, swapped anchors, swapped feature halves): bit-identical results.
 *
 * Feature ids: A's half in bits 0..15, B's in bits 16..31; a half is a 2-bit type
 * (DE__F2_VERTEX, DE__F2_EDGE, DE__F2_SHAPE) and a 14-bit index. Clipped points name the two
 * vertices they come from (reference-edge end and incident vertex, as Box2D's ids), so a resting
 * pair keeps its ids whether or not the incident vertex is clipped, and whichever polygon gives
 * the reference face. Two points are ordered by min(id, swapped id), which does not depend on
 * which shape is A either, so the order is stable too. Points that involve a circle
 * (and the single point of a capsule pair) are (SHAPE 0, SHAPE 0): one point per manifold, always
 * the same contact.
 *
 * Pair frame. Each pair is computed in A's frame shifted to a point of A (its first vertex or
 * center), so far-away chain segments and large worlds keep f32 precision: B's pose relative to
 * it is computed in f64 and rounded once; anchors and normals go back in f64.
 *
 * Purity: reads shapes and poses, writes only the output; no allocation, no globals. Any number
 * of threads may call it at once (the parallel narrowphase of manifold2d.c).
 *
 * Determinism (SPEC §5.7): f32 + - * /, de_dsqrt / de_dsqrt64, f64 for poses; no transcendental
 * functions; fixed iteration orders; ties to the lowest index.
 */
#ifndef DE_PHYSICS_NARROW2D_H
#define DE_PHYSICS_NARROW2D_H

#include "physics/2d/aabb_tree2d.h"
#include "physics/2d/p2d_internal.h"

DE_EXTERN_C_BEGIN

/* ================================ Constants ============================================= */

/** Linear slop (m): Box2D's 0.005. The polygon pair uses 0.1 * slop as its reference-face
 *  hysteresis and as the "cores apart" threshold of the vertex-vertex path. */
#define DE__P2D_LINEAR_SLOP 0.005f
/** No chain child: the key child of a pair without a chain. */
#define DE__P2D_CHILD_NONE 0xFFFFFFFFu
/** Most points of a 2D contact set. */
#define DE__P2D_MAX_POINTS 2u

/* ================================ Feature ids =========================================== */

enum { DE__F2_VERTEX = 0, DE__F2_EDGE = 1, DE__F2_SHAPE = 2 };

/** A feature half: 2-bit type, 14-bit index. */
static inline u32 de__f2(u32 type, u32 index) {
    return (type << 14) | (index & 0x3FFFu);
}
/** A feature id from A's and B's halves. */
static inline u32 de__f2_id(u32 half_a, u32 half_b) {
    return (half_a & 0xFFFFu) | ((half_b & 0xFFFFu) << 16);
}
static inline u32 de__f2_a(u32 id) {
    return id & 0xFFFFu;
}
static inline u32 de__f2_b(u32 id) {
    return id >> 16;
}
static inline u32 de__f2_type(u32 half) {
    return (half >> 14) & 3u;
}
static inline u32 de__f2_index(u32 half) {
    return half & 0x3FFFu;
}
/** The id with its halves swapped (the pair seen from B). */
static inline u32 de__f2_swap(u32 id) {
    return de__f2_id(de__f2_b(id), de__f2_a(id));
}

/* ================================ Contact sets ========================================== */

/** A body pose: f64 plane position of the body origin and its rotation. */
typedef struct de__xf2d {
    de__d2 p;
    de_rot2d q;
} de__xf2d;

/** One contact point (see the file comment). */
typedef struct de__contact_point2d {
    de_vec2 local_a; /**< on A's surface, A's body frame */
    de_vec2 local_b; /**< on B's surface, B's body frame */
    f32 separation;  /**< along the normal; negative = overlap */
    u32 feature_id;
} de__contact_point2d;

/** The contacts of one shape pair (or of one chain segment and a shape). */
typedef struct de__contact_set2d {
    de_vec2 normal;  /**< unit, plane axes, from A toward B (zero when point_count is 0) */
    u32 point_count; /**< 0..DE__P2D_MAX_POINTS */
    de__contact_point2d points[DE__P2D_MAX_POINTS];
} de__contact_set2d;

/**
 * Collides shape `sa` at pose `xa` with shape `sb` at pose `xb`. `child` is the segment index
 * when one of the shapes is a chain (must be below its segment_count), else ignored.
 * `speculative` (m, >= 0): points farther apart than this are dropped. Fills *out (point_count 0
 * when nothing touches) and returns out->point_count. Both shapes must be live 2D shapes.
 */
u32 de__collide2d(const de__shape2d *sa, de__xf2d xa, const de__shape2d *sb, de__xf2d xb, u32 child,
                  f32 speculative, de__contact_set2d *out);

/** The canonical rank of a kind: circle 0, capsule 1, box and polygon 2, segment 3, chain 4. */
static inline u32 de__shape2d_rank(de_shape2d_kind k) {
    switch (k) {
    case DE_SHAPE2D_CIRCLE:
        return 0;
    case DE_SHAPE2D_CAPSULE:
        return 1;
    case DE_SHAPE2D_BOX:
    case DE_SHAPE2D_POLYGON:
        return 2;
    case DE_SHAPE2D_SEGMENT:
        return 3;
    default:
        return 4;
    }
}

/* ================================ Shared helpers (narrow2d.c) ============================ */
/* Used by collide_chain2d.c; everything below works in one f32 frame (the pair frame). */

/** A convex core in the pair frame: 2..8 CCW vertices with outward unit normals (2 = a segment
 *  as Box2D's two-sided capsule polygon), a rounding radius and an interior point. */
typedef struct de__n2_poly {
    de_vec2 v[DE_SHAPE2D_MAX_POLYGON_VERTICES];
    de_vec2 n[DE_SHAPE2D_MAX_POLYGON_VERTICES];
    de_vec2 centroid;
    f32 radius;
    u32 count;
} de__n2_poly;

/** A point of a local manifold: anchors on A's and B's surfaces in the pair frame. */
typedef struct de__n2_point {
    de_vec2 pa, pb;
    f32 separation;
    u32 id;
} de__n2_point;

/** A local manifold in the pair frame; normal from A toward B. */
typedef struct de__n2_manifold {
    de_vec2 normal;
    u32 count;
    de__n2_point pt[DE__P2D_MAX_POINTS];
} de__n2_manifold;

/** |v| and v / |v| (zero vector and 0 when |v| < FLT_EPSILON). de_dsqrt. */
f32 de__n2_normalize(de_vec2 v, de_vec2 *out);

/** Closest points of segments p1-q1 and p2-q2 (Box2D's b2SegmentDistance). Fractions are
 *  exactly 0 or 1 at the end points. */
typedef struct de__n2_segdist {
    de_vec2 c1, c2;
    f32 f1, f2;
    f32 d2; /**< squared distance */
} de__n2_segdist;
de__n2_segdist de__n2_segment_distance(de_vec2 p1, de_vec2 q1, de_vec2 p2, de_vec2 q2);

/**
 * Clips incident segment b1-b2 against reference segment a1-a2 whose unit `normal` points from
 * the reference toward the incident side (Box2D's b2ClipSegments): up to two points, the "lower"
 * one (from b2, clipped at a1) first. Per point: `ref` = the point on the reference surface
 * (radius ra), `inc` = the point on the incident surface (radius rb), `separation` along normal.
 * Returns 0 when the projections do not overlap.
 */
typedef struct de__n2_clip {
    de_vec2 ref[2], inc[2];
    f32 separation[2];
} de__n2_clip;
u32 de__n2_clip_segments(de_vec2 a1, de_vec2 a2, de_vec2 b1, de_vec2 b2, de_vec2 normal, f32 ra,
                         f32 rb, de__n2_clip *out);

/** Appends a point to m when separation <= speculative. */
void de__n2_add(de__n2_manifold *m, de_vec2 pa, de_vec2 pb, f32 separation, u32 id,
                f32 speculative);

/* ================================ Chains (collide_chain2d.c) ============================= */

/**
 * Chain segment (A, in the pair frame: the segment's points shifted by `shift`) against a circle
 * of radius `rb` at `c` (B). One-sided: nothing when the center is behind the segment. A shared
 * vertex region belongs to exactly one segment: the one that starts at the vertex (the vertex
 * region of a segment's end belongs to the next segment), and an open chain end (DE__CHAIN_GHOST*
 * _OPEN) belongs to its segment. So a circle rolling across joints gets one contact, with the
 * normal of the closest chain point.
 */
void de__n2_chain_circle(const de__chain_segment *seg, de_vec2 shift, de_vec2 c, f32 rb,
                         f32 speculative, de__n2_manifold *m);

/**
 * Chain segment (A) against a polygon core `b` (B: a polygon, a capsule or a segment as two-
 * vertex polygon, with its radius) in the pair frame. Box2D v3's smooth segment: one-sided by the
 * segment and its convex neighbours' normals; normals classified on the ghost vertices' Gauss map
 * (skip, admit, snap to the segment normal); the neighbour that is the better incident feature
 * gets the contact; an open chain end is a free convex corner (all normals between the segment
 * normal and the outward segment direction). Closest features of separated cores come from exact
 * segment distances (no GJK).
 */
void de__n2_chain_polygon(const de__chain_segment *seg, de_vec2 shift, const de__n2_poly *b,
                          f32 speculative, de__n2_manifold *m);

/** Called for each chain segment whose bounds overlap the query box; return false to stop. */
typedef bool (*de__chain2d_query_fn)(void *user, u32 segment);

/**
 * Calls `fn` for every segment of `c` whose box overlaps `box` (closed boxes, chain frame), in
 * ascending segment index. Returns false when `fn` stopped. Read-only; no scratch memory beyond
 * a fixed stack.
 */
bool de__chain2d_query(const de__chain2d *c, de_aabb2d box, de__chain2d_query_fn fn, void *user);

/** Called for each segment whose box the ray enters within the bound; returns the new bound as
 *  de__tree2d_ray_fn does. */
typedef f32 (*de__chain2d_ray_fn)(void *user, const de__tree2d_ray *ray, u32 segment,
                                  f32 max_fraction);

/**
 * Ray traversal of the segment bounds (ray in the chain frame), in ascending segment index among
 * the boxes entered; bound handling as de__tree2d_raycast. Returns the final bound.
 */
f32 de__chain2d_raycast(const de__chain2d *c, const de__tree2d_ray *ray, de__chain2d_ray_fn fn,
                        void *user);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_NARROW2D_H */
