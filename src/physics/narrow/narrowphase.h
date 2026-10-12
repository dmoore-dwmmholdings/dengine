/*
 * narrowphase.h: contact generation between two shapes (DENG-134, SPEC §13.1). Internal: include
 * as "physics/narrow/narrowphase.h".
 *
 * de__collide() turns two shapes at two world poses into contact sets: one set per pair of
 * (sub-)shapes that touch or are closer than the speculative distance. Each set has a world
 * normal from A toward B and up to DE__CONTACT_MAX_POINTS points with anchors on both shapes,
 * a separation and a stable feature id. DENG-135 turns the sets into persistent manifolds;
 * DENG-137 builds shape casts, overlaps and closest points on the GJK/EPA helpers below.
 *
 * Paths (src/physics/narrow/):
 * - dispatch.c: canonical pair order, relative transform, the convex dispatch table, compound
 *   recursion, output conversion.
 * - sphere_capsule.c: sphere and capsule pairs; sphere and capsule against boxes, hulls and
 *   triangles (core GJK, face clipping, SAT when the cores overlap).
 * - sat_box.c: box-box SAT over 15 axes and the box polytope template.
 * - sat_hull.c: polytope-polytope SAT (hull-hull, hull-box, triangle-box, triangle-hull) with
 *   Gauss-map edge pruning.
 * - clip.c: reference / incident faces, Sutherland-Hodgman clipping, edge contacts, reduction.
 * - gjk.c, epa.c: GJK distance (at most 32 iterations) and EPA penetration (at most 64
 *   iterations), used for cylinder pairs and as a fallback.
 * - midphase.c: mesh BVH and heightfield blocks against a convex shape, triangle contacts with
 *   internal-edge correction, compound children.
 *
 * Purity and threads: every function here reads its inputs and writes only `out`. Shapes are
 * immutable, so any number of threads may collide any pairs at once (DENG-135 runs fixed pair
 * ranges in parallel). No function allocates: buffers are fixed-size and on the stack.
 *
 * Determinism (SPEC §5.7): + - * / and de_dsqrt / de_dsqrt64 only, no FP contraction (module
 * flag), bounded loops, ties broken by the lowest index, triangles and compound children visited
 * in ascending index. The relative transform of B in A is computed in f64 and rounded to f32
 * once, so two bodies 100 km from the origin collide like two bodies near it.
 */
#ifndef DE_PHYSICS_NARROWPHASE_H
#define DE_PHYSICS_NARROWPHASE_H

#include "physics/shapes/shape_internal.h"

DE_EXTERN_C_BEGIN

/* ================================ Contact sets ========================================= */

/** Most points in one contact set (DENG-135 reduces to 4). */
#define DE__CONTACT_MAX_POINTS 8u
/** "No sub-shape" in de__contact_set.child_* / sub_*. */
#define DE__SUB_NONE 0xFFFFFFFFu

/*
 * Feature ids. A contact point's `feature_id` is two 16-bit features: bits 0..15 name the
 * feature of shape A, bits 16..31 the feature of shape B. A feature is a 2-bit type in its top
 * bits (14..15) and a 14-bit index:
 *
 *     type DE__FEAT_VERTEX  index = vertex
 *     type DE__FEAT_EDGE    index = edge: for boxes and hulls the lower of the two half-edge
 *                                   indices (e, twin(e)); triangle edge i runs from corner i to
 *                                   corner i + 1; capsule edge 0 is its segment
 *     type DE__FEAT_FACE    index = face (box: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z; triangle:
 *                                   0 front, 1 back; hull: face index)
 *     type DE__FEAT_SHAPE   index = 0: the whole shape (sphere, cylinder, a GJK/EPA point on a
 *                                   curved surface)
 *
 * Box vertex v has the coordinate signs (+ when the bit is set) x: bit 0, y: bit 1, z: bit 2; box
 * half-edges are the static template of sat_box.c. Capsule vertices: 0 = the -Z end, 1 = +Z end.
 *
 * Clipped polygon points: an incident vertex is (reference FACE, incident VERTEX); an incident
 * edge cut by a side plane is (reference EDGE, incident EDGE); a reference corner reached along
 * a cut is (reference VERTEX, incident FACE). Edge-edge contacts are (EDGE, EDGE). The ids are
 * unique within a set and depend only on which features touch, so they stay the same while a
 * resting contact jitters (DENG-135 warm starts on them). A swapped pair swaps the two halves.
 */
enum { DE__FEAT_VERTEX = 0u, DE__FEAT_EDGE = 1u, DE__FEAT_FACE = 2u, DE__FEAT_SHAPE = 3u };
static inline u32 de__feat(u32 type, u32 index) {
    return (type << 14) | (index & 0x3FFFu);
}
static inline u32 de__feat_type(u32 f) {
    return (f >> 14) & 3u;
}
static inline u32 de__feat_index(u32 f) {
    return f & 0x3FFFu;
}
/** Packs feature a (shape A) and b (shape B) into a feature id. */
static inline u32 de__feature_id(u32 a, u32 b) {
    return (a & 0xFFFFu) | ((b & 0xFFFFu) << 16);
}
static inline u32 de__feature_a(u32 id) {
    return id & 0xFFFFu;
}
static inline u32 de__feature_b(u32 id) {
    return id >> 16;
}
static inline u32 de__feature_flip(u32 id) {
    return (id >> 16) | (id << 16);
}

/**
 * One contact point. `local_a` is the point on A's surface in the frame of the top-level shape A
 * (the body frame), `local_b` the point on B's surface in B's top-level frame. `separation` is
 * dot(world(local_b) - world(local_a), normal): negative when the shapes overlap.
 */
typedef struct de__contact_point {
    de_vec3 local_a;
    de_vec3 local_b;
    f32 separation;
    u32 feature_id;
} de__contact_point;

/**
 * Contacts between one sub-shape of A and one of B. `normal` is unit length, in world axes,
 * pointing from A toward B (move B along it to separate). `child_*` is the compound child index,
 * `sub_*` the triangle (mesh: triangle index; heightfield: 2 * (row * (cols - 1) + col) + k);
 * DE__SUB_NONE when the shape has none.
 */
typedef struct de__contact_set {
    de_vec3 normal;
    u32 point_count;
    u32 child_a, child_b;
    u32 sub_a, sub_b;
    de__contact_point points[DE__CONTACT_MAX_POINTS];
} de__contact_set;

/** Caller-owned output. de__collide appends; when `capacity` is reached it sets `overflow` and
 *  drops the rest (deterministically: always the last sets in visiting order). */
typedef struct de__contact_buffer {
    de__contact_set *sets;
    u32 capacity;
    u32 count;
    bool overflow;
} de__contact_buffer;

/** A shape's world pose: f64 position, unit rotation. */
typedef struct de__pose {
    de_dvec3 position;
    de_quat rotation;
} de__pose;

/**
 * Collides shape `a` at pose `xa` with shape `b` at `xb`. Appends one contact set per touching
 * sub-shape pair with at least one point whose separation is <= `speculative` (>= 0, meters).
 * Returns the number of sets appended.
 *
 * - Pairs are put in a canonical order first (higher de_shape_kind first, then the lower shape
 *   id, then the pose), so de__collide(b, a) returns exactly the sets of de__collide(a, b) with
 *   the normal negated, the anchors, features and sub-shapes swapped. (Equal shapes at equal
 *   poses are the one exception: both orders compute the same thing.)
 * - Mesh and heightfield against mesh or heightfield: no contacts (both are static).
 * - Compound children are visited in ascending index (A's children first), triangles in
 *   ascending sub-shape id.
 * - Degenerate cases: coincident sphere centers (and other zero-distance cores) use world +Z as
 *   the normal; a zero-length capsule behaves as a sphere.
 */
u32 de__collide(const de__shape *a, de__pose xa, const de__shape *b, de__pose xb, f32 speculative,
                de__contact_buffer *out);

/* ================================ Internals shared by the narrow/ sources =======================
 */

/* Thresholds (meters, or unitless for cosines). */
/** Cores closer than this count as overlapping: SAT / EPA instead of the GJK closest points. */
#define DE__NP_CORE_EPS 1e-5f
/** A face of B beats A's best face only when its separation is larger by this much. */
#define DE__NP_FACE_TOL 2e-5f
/** An edge pair beats the best face only when its separation is larger by this much. */
#define DE__NP_EDGE_TOL 5e-5f
/** cos of the angle under which a GJK normal counts as a face normal (about 1.15 degrees). */
#define DE__NP_FACE_COS 0.9998f
/** sin^2 of the angle under which two capsule segments are parallel (sin 1.15 deg = 0.02). */
#define DE__NP_PARALLEL_SIN2 4e-4f

/** Pair frame: everything is computed in A's local frame. B's frame maps into it by r, t. */
typedef struct de__np_rel {
    de_mat3 r;    /**< B -> A rotation (columns: B's axes in A) */
    de_vec3 t;    /**< B's origin in A */
    de_vec3 up;   /**< world +Z in A: the fallback normal for zero-distance cases */
    f32 spec;     /**< speculative distance */
    de__dmat3 wa; /**< A's world rotation in f64 (pair-frame normal -> world) */
} de__np_rel;

/** B point -> A frame. */
static inline de_vec3 de__np_to_a(const de__np_rel *rel, de_vec3 p) {
    const f32 *m = rel->r.m;
    return de__v3(m[0] * p.x + m[3] * p.y + m[6] * p.z + rel->t.x,
                  m[1] * p.x + m[4] * p.y + m[7] * p.z + rel->t.y,
                  m[2] * p.x + m[5] * p.y + m[8] * p.z + rel->t.z);
}
/** B direction -> A frame. */
static inline de_vec3 de__np_dir_to_a(const de__np_rel *rel, de_vec3 d) {
    const f32 *m = rel->r.m;
    return de__v3(m[0] * d.x + m[3] * d.y + m[6] * d.z, m[1] * d.x + m[4] * d.y + m[7] * d.z,
                  m[2] * d.x + m[5] * d.y + m[8] * d.z);
}
/** A direction -> B frame. */
static inline de_vec3 de__np_dir_to_b(const de__np_rel *rel, de_vec3 d) {
    const f32 *m = rel->r.m;
    return de__v3(m[0] * d.x + m[1] * d.y + m[2] * d.z, m[3] * d.x + m[4] * d.y + m[5] * d.z,
                  m[6] * d.x + m[7] * d.y + m[8] * d.z);
}
/** A point -> B frame. */
static inline de_vec3 de__np_to_b(const de__np_rel *rel, de_vec3 p) {
    return de__np_dir_to_b(rel, de__v3_sub(p, rel->t));
}

/** Builds the pair frame of B (pose xb) in A (pose xa): f64, rounded to f32 once. */
void de__np_rel_init(de__np_rel *rel, de__pose xa, de__pose xb, f32 speculative);

/** A contact point in the pair frame: `pa` on A, `pb` on B, separation, feature id. */
typedef struct de__np_point {
    de_vec3 pa, pb;
    f32 sep;
    u32 feature;
} de__np_point;

/** The contacts of one pair in the pair frame. `normal` from A toward B. */
typedef struct de__np_manifold {
    de_vec3 normal;
    u32 count;
    de__np_point pts[DE__CONTACT_MAX_POINTS];
} de__np_manifold;

/** Adds a point if there is room and sep <= spec. */
static inline void de__np_add(de__np_manifold *m, de_vec3 pa, de_vec3 pb, f32 sep, u32 feature,
                              f32 spec) {
    if (m->count < DE__CONTACT_MAX_POINTS && sep <= spec) {
        de__np_point *p = &m->pts[m->count++];
        p->pa = pa;
        p->pb = pb;
        p->sep = sep;
        p->feature = feature;
    }
}

/* ---- f32 helpers (deterministic: + - * / and de_dsqrt64) ---- */

static inline f32 de__np_len2(de_vec3 a) {
    return de__v3_dot(a, a);
}
/** |a| computed in f64. */
static inline f32 de__np_len(de_vec3 a) {
    return (f32)de_dsqrt64((f64)a.x * a.x + (f64)a.y * a.y + (f64)a.z * a.z);
}
/** a / |a| in f64; `fallback` when |a| is 0 or not finite. */
de_vec3 de__np_normalize(de_vec3 a, de_vec3 fallback);

/* ---- Convex polytopes (boxes, hulls, triangles) ---- */

/**
 * A polytope in its own frame: the hull layout of shape_internal.h. Boxes use a static template
 * (sat_box.c), triangles a two-faced flat template (front face 0, back face 1; `flat`).
 */
typedef struct de__np_poly {
    const de_vec3 *verts;
    const de_plane *planes;
    const de__hull_face *faces;
    const de__hull_edge *edges;
    u32 vcount, fcount, ecount;
    de_vec3 centroid;
    bool flat;
} de__np_poly;

/** Canonical edge id of half-edge e: the lower of e and its twin. */
static inline u32 de__np_edge_id(const de__np_poly *p, u32 e) {
    const u32 t = p->edges[e].twin;
    return t < e ? t : e;
}

/** A box polytope (owns its vertices and planes; do not copy after init). */
typedef struct de__np_box_poly {
    de_vec3 v[8];
    de_plane p[6];
    de__np_poly poly;
} de__np_box_poly;
void de__np_box_poly_init(de__np_box_poly *bp, de_vec3 half_extents);
/** Canonical edge id of the box edge between template vertices v0 and v1 (adjacent). */
u32 de__np_box_edge(u32 v0, u32 v1);

/** A triangle as a flat polytope plus its active-edge flags (DE__MESH_EDGE*_ACTIVE). */
typedef struct de__np_tri {
    de_vec3 v[3];
    de_plane p[2];
    de__np_poly poly;
    de_vec3 n; /**< unit front normal */
    u8 active; /**< bit i: edge i (corner i -> i + 1) may produce edge or vertex contacts */
} de__np_tri;
/** false for a zero-area triangle. */
bool de__np_tri_init(de__np_tri *t, de_vec3 a, de_vec3 b, de_vec3 c, u8 active_edges);

void de__np_hull_poly(const de__hull *h, de__np_poly *out);

/* ---- Clipping (clip.c) ---- */

/**
 * Face contact: the reference face `face` of A (`ref_is_a`) or of B, the incident face of the
 * other polytope (its face most anti-parallel to the reference normal) clipped against the
 * reference face's side planes (Sutherland-Hodgman with feature tracking). `bverts` are B's
 * vertices in the pair frame. Points with separation <= rel->spec are kept and reduced to
 * DE__CONTACT_MAX_POINTS deterministically (deepest first, then farthest from the chosen ones).
 * The normal is the reference normal, flipped to point from A to B.
 */
void de__np_face_contact(const de__np_poly *a, const de__np_poly *b, const de_vec3 *bverts,
                         const de__np_rel *rel, bool ref_is_a, u32 face, de__np_manifold *out);

/**
 * Edge-edge contact: one point at the closest points of A's half-edge `ea` and B's half-edge
 * `eb` (B's vertices `bverts` in the pair frame), normal `axis` (unit, A toward B).
 */
void de__np_edge_contact(const de__np_poly *a, u32 ea, const de__np_poly *b, const de_vec3 *bverts,
                         u32 eb, de_vec3 axis, const de__np_rel *rel, de__np_manifold *out);

/**
 * Segment p0 -> p1 (pair frame, radius r, features f0 / f1 for the end points and fe for the
 * segment) against face `face` of polytope `poly` (A's frame = pair frame). Writes up to 2
 * points with the face normal; returns the number written.
 */
u32 de__np_clip_segment(const de__np_poly *poly, u32 face, de_vec3 p0, de_vec3 p1, f32 r, u32 f0,
                        u32 f1, u32 fe, f32 spec, de__np_manifold *out);

/** Closest points of segments p0 p1 and q0 q1 (f64 inside); s, t the parameters in [0, 1]. */
void de__np_segments_closest(de_vec3 p0, de_vec3 p1, de_vec3 q0, de_vec3 q1, f32 *s, f32 *t,
                             de_vec3 *cp, de_vec3 *cq);
/** Closest point of segment a b to p, parameter in [0, 1]. */
f32 de__np_segment_closest_t(de_vec3 a, de_vec3 b, de_vec3 p);

/* ---- SAT (sat_box.c, sat_hull.c) ---- */

/**
 * Polytope A (pair frame) against polytope B (B's frame, mapped by rel). `tri` is A's triangle
 * data when A is a mesh or heightfield triangle (active-edge correction), else NULL. Writes the
 * contact (face clipping or one edge-edge point) and returns true when it has points.
 */
bool de__np_poly_poly(const de__np_poly *a, const de__np_tri *tri, const de__np_poly *b,
                      const de__np_rel *rel, de__np_manifold *out);

/* ---- Segment cores (sphere_capsule.c) ---- */

/**
 * Polytope A against a segment core B (p0 -> p1 in the pair frame; p0 == p1 for a sphere) of
 * radius rb. f0 / f1 / fe are B's features of the end points and of the segment interior. `tri`
 * as in de__np_poly_poly.
 */
bool de__np_poly_segment(const de__np_poly *a, const de__np_tri *tri, de_vec3 p0, de_vec3 p1,
                         f32 rb, u32 f0, u32 f1, u32 fe, const de__np_rel *rel,
                         de__np_manifold *out);

/* ---- GJK / EPA (gjk.c, epa.c): also the base of DENG-137's casts and overlaps ---- */

/**
 * A support-mapping proxy in a pair frame. Either a convex shape (`shape` set: sphere, capsule,
 * box, cylinder, hull) or a point list (`shape` NULL: points[0..count): a polytope's vertices,
 * a triangle, a segment, a point). `core` drops the sphere / capsule radius (the shape's margin),
 * so a sphere is its center and a capsule its segment. The proxy's frame maps into the pair frame
 * by rot, pos (identity when `identity`).
 */
typedef struct de__gjk_proxy {
    const de__shape *shape;
    const de_vec3 *points;
    u32 count;
    bool core;
    bool identity;
    de_mat3 rot;
    de_vec3 pos;
} de__gjk_proxy;

/** Proxy of shape s in A's frame (A side: identity) or B's (mapped by rel). */
void de__gjk_proxy_shape(de__gjk_proxy *p, const de__shape *s, bool core, const de__np_rel *rel);
/** Proxy of points[0..count) (count >= 1) in A's frame (rel NULL) or B's (mapped by rel). */
void de__gjk_proxy_points(de__gjk_proxy *p, const de_vec3 *points, u32 count,
                          const de__np_rel *rel);

/**
 * Support point of the proxy along `dir` (pair frame, f64, need not be unit), in the pair frame.
 * *index: the vertex (box: sign bits, hull / points: index, capsule core: 0 = -Z end, 1 = +Z
 * end; 0 for spheres and cylinders). Ties: the shape support's tie rule.
 */
de_dvec3 de__gjk_support(const de__gjk_proxy *p, de_dvec3 dir, u32 *index);

/** Most GJK iterations. */
#define DE__GJK_MAX_ITER 32u

/**
 * GJK result. The simplex is over the difference A - B: w_i = a_i - b_i. When `overlap`,
 * distance is 0 and the simplex holds the origin (EPA starts from it); else point_a / point_b
 * are the closest points (pair frame) and distance their distance.
 */
typedef struct de__gjk_result {
    f64 distance;
    de_dvec3 point_a, point_b;
    bool overlap;
    u32 iterations;
    u32 count;           /**< simplex vertices, 1..4 */
    de_dvec3 a[4], b[4]; /**< support points on A and B */
    u32 ia[4], ib[4];    /**< their support indices */
    f64 lambda[4];       /**< barycentric weights of the closest point */
} de__gjk_result;

/** GJK distance between two proxies (deterministic, at most DE__GJK_MAX_ITER iterations). */
void de__gjk_distance(const de__gjk_proxy *a, const de__gjk_proxy *b, de__gjk_result *out);

/** Most EPA iterations (vertices added). */
#define DE__EPA_MAX_ITER 64u

typedef struct de__epa_result {
    bool ok;          /**< false: degenerate (touching); the fields are a best guess */
    f64 depth;        /**< penetration depth >= 0 */
    de_dvec3 normal;  /**< unit, from A toward B */
    de_dvec3 point_a; /**< deepest point of A along normal (pair frame) */
    de_dvec3 point_b; /**< deepest point of B against normal; point_b = point_a - normal * depth */
    u32 iterations;
} de__epa_result;

/** EPA from an overlapping GJK result (deterministic, at most DE__EPA_MAX_ITER iterations). */
void de__epa_penetration(const de__gjk_proxy *a, const de__gjk_proxy *b, const de__gjk_result *g,
                         const de_vec3 fallback, de__epa_result *out);

/**
 * The GJK/EPA contact of two convex proxies with radii ra, rb (proxies are cores): one point.
 * Features: fa / fb when poly_a / poly_b is NULL (curved shapes: DE__FEAT_SHAPE), else the
 * support feature of the polytope (A's in the pair frame, B's in B's frame). `tri`: A is a
 * triangle (internal-edge correction: an inactive feature falls back to the face normal).
 */
bool de__np_gjk_epa(const de__gjk_proxy *a, const de__np_poly *poly_a, f32 ra, u32 fa,
                    const de__np_tri *tri, const de__gjk_proxy *b, const de__np_poly *poly_b,
                    f32 rb, u32 fb, const de__np_rel *rel, de__np_manifold *out);

/**
 * The feature of polytope p (its own frame) touching along outward direction n_out (p's frame):
 * the vertices within DE__NP_FEATURE_TOL of the support plane give a vertex (1), an edge (2 that
 * share an edge) or the face most aligned with n_out (ties: the lowest index).
 */
u32 de__np_poly_support_feature(const de__np_poly *p, de_vec3 n_out);
/** Support-plane tolerance of de__np_poly_support_feature (meters). */
#define DE__NP_FEATURE_TOL 1e-4f

/**
 * Triangle feature check for internal-edge correction: true when feature `f` of the triangle
 * may give the contact normal n (pair frame, from the triangle toward B): the front face, an
 * active edge, or a vertex with an active edge, and n not pointing behind the triangle.
 */
bool de__np_tri_feature_ok(const de__np_tri *t, u32 f, de_vec3 n);

/* ---- Pair functions (A's kind >= B's kind), all in the pair frame ---- */

typedef bool (*de__np_pair_fn)(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                               de__np_manifold *out);

bool de__np_sphere_sphere(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                          de__np_manifold *out);
bool de__np_capsule_sphere(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                           de__np_manifold *out);
bool de__np_capsule_capsule(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                            de__np_manifold *out);
bool de__np_box_sphere(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                       de__np_manifold *out);
bool de__np_box_capsule(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                        de__np_manifold *out);
bool de__np_box_box(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                    de__np_manifold *out);
bool de__np_hull_sphere(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                        de__np_manifold *out);
bool de__np_hull_capsule(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                         de__np_manifold *out);
bool de__np_hull_box(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                     de__np_manifold *out);
bool de__np_hull_hull(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                      de__np_manifold *out);
/** GJK/EPA for any two convex shapes (the cylinder pairs; tests compare it with SAT). */
bool de__np_convex_gjk(const de__shape *a, const de__shape *b, const de__np_rel *rel,
                       de__np_manifold *out);

/** Triangle (pair frame = the mesh's frame) against convex shape b. */
bool de__np_triangle_convex(const de__np_tri *tri, const de__shape *b, const de__np_rel *rel,
                            de__np_manifold *out);

/** The function for convex kinds ka >= kb. */
de__np_pair_fn de__np_pair_function(de_shape_kind ka, de_shape_kind kb);

/* ---- Recursion and midphase (dispatch.c, midphase.c) ---- */

/** One side of a pair during recursion: a (sub-)shape, its world pose and its place in the
 *  top-level shape. */
typedef struct de__np_side {
    const de__shape *shape;
    de__pose pose;
    bool has_off; /**< a compound child: off_pos / off_rot place it in the compound */
    de_vec3 off_pos;
    de_quat off_rot;
    u32 child; /**< compound child index or DE__SUB_NONE */
} de__np_side;

typedef struct de__np_ctx {
    de__contact_buffer *out;
    f32 spec;
    u32 added;
} de__np_ctx;

/** dispatch.c: converts a pair-frame manifold to a contact set and appends it. `swapped`: the
 *  caller's A is side b. */
void de__np_emit(de__np_ctx *ctx, const de__np_manifold *m, const de__np_side *a,
                 const de__np_side *b, const de__np_rel *rel, u32 sub_a, bool swapped);
/** dispatch.c: collides two sides (recursing into compounds). */
void de__np_collide_sides(de__np_ctx *ctx, const de__np_side *a, const de__np_side *b,
                          bool swapped);

/** midphase.c: mesh or heightfield side a against convex side b (pair frame = a's frame). */
void de__np_midphase(de__np_ctx *ctx, const de__np_side *a, const de__np_side *b,
                     const de__np_rel *rel, bool swapped);
/** midphase.c: calls fn(ctx, child index) for every child of compound side `c` whose box
 *  overlaps `box` (in the compound's frame), in ascending child index. */
void de__np_compound_children(const de__shape *c, de_aabb box, void (*fn)(void *user, u32 child),
                              void *user);
/** midphase.c: edge flags (DE__MESH_EDGE*_ACTIVE) of heightfield triangle k of cell (col, row). */
u8 de__np_hf_edge_flags(const de__heightfield *hf, u32 col, u32 row, u32 k);

/** Box of `local` (B's frame) in A's frame, grown by `grow`. */
de_aabb de__np_box_in_a(const de__np_rel *rel, de_aabb local, f32 grow);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_NARROWPHASE_H */
