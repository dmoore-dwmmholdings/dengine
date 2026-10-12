/*
 * shape_internal.h: collision shape data and the internal shape API (DENG-132). Include as
 * "physics/shapes/shape_internal.h".
 *
 * Consumed by the narrowphase (DENG-134: support functions, hull half-edges, mesh BVH and edge
 * flags, heightfield blocks, compound child BVH), the broadphase (DENG-133: world AABBs) and the
 * queries (DENG-137). Shapes are immutable after de_shape_create, so any number of threads may
 * read them at once. Every shape lives in its own allocation and never moves, so a de__shape
 * pointer stays valid while the shape is alive (a compound holds its children's pointers).
 *
 * Local frames: Z up. Capsule and cylinder axes are local +Z. All shape data is f32.
 */
#ifndef DE_PHYSICS_SHAPE_INTERNAL_H
#define DE_PHYSICS_SHAPE_INTERNAL_H

#include "physics/physics_internal.h"

DE_EXTERN_C_BEGIN

/* ================================ Mass ================================================= */

/**
 * Mass properties per unit density, in f64: `volume` (m^3), `com` (local), `inertia` about `com`
 * in local axes for density 1 (column-major 3x3). Mass properties at density rho are
 * (rho * volume, com, rho * inertia).
 */
typedef struct de__unit_mass {
    f64 volume;
    de_dvec3 com;
    f64 inertia[9];
} de__unit_mass;

/* ================================ Convex hull ========================================== */

/** "No such element" in u16 hull links and u32 mesh adjacency. */
#define DE__HULL_NONE   0xFFFFu
#define DE__MESH_NO_ADJ 0xFFFFFFFFu

/**
 * Half-edge of a hull. Edge e goes from vertex `origin` to the origin of edge `next`, along the
 * boundary of face `face` (counter-clockwise seen from outside). `twin` is the opposite half-edge,
 * on the neighbouring face. Edges e and e ^ 1 are NOT twins in general: use `twin`.
 */
typedef struct de__hull_edge {
    u16 next;
    u16 twin;
    u16 origin;
    u16 face;
} de__hull_edge;

/** A polygon face: `edge_count` half-edges starting at `first_edge`, linked by `next`. The
 *  face's edges are stored contiguously: [first_edge, first_edge + edge_count). */
typedef struct de__hull_face {
    u16 first_edge;
    u16 edge_count;
} de__hull_face;

/**
 * A convex polyhedron (DE_SHAPE_CONVEX). Vertices are a subset of the input points, in
 * ascending input order. `planes[f]` is face f's outward plane (unit normal n, offset d:
 * dot(n, x) + d = 0); every vertex satisfies dot(n, v) + d <= `tolerance`. Coplanar input
 * triangles are merged into one polygon face, and vertices in the middle of a face or an edge
 * are removed. Limits: 4..DE_SHAPE_MAX_HULL_VERTICES vertices, so u16 indices.
 */
typedef struct de__hull {
    u32 vertex_count, face_count, edge_count;
    de_vec3 *vertices;
    de_plane *planes;     /**< [face_count] */
    de__hull_face *faces; /**< [face_count] */
    de__hull_edge *edges; /**< [edge_count] (twice the number of polytope edges) */
    de_vec3 centroid;     /**< mean of the vertices: an interior point (GJK start) */
    f32 tolerance;        /**< the weld / coplanarity tolerance used to build it */
} de__hull;

/* ================================ BVH ================================================== */

/**
 * Bounding-volume hierarchy over primitives (mesh triangles, compound children). Node 0 is the
 * root. An internal node has `count` 0 and children `first` and `first + 1`; a leaf has
 * `count` > 0 primitives prims[first .. first + count). Traverse left child first (deterministic
 * order). The depth (root = 1) is at most DE__BVH_MAX_DEPTH, so a fixed traversal stack of that
 * size is enough.
 */
#define DE__BVH_MAX_DEPTH 64u

typedef struct de__bvh_node {
    de_aabb bounds;
    u32 first;
    u32 count;
} de__bvh_node;

typedef struct de__bvh {
    de__bvh_node *nodes;
    u32 *prims; /**< primitive indices, leaf order */
    u32 node_count;
    u32 prim_count;
    u32 depth; /**< levels, root = 1; 0 for an empty BVH */
} de__bvh;

/* ================================ Mesh ================================================= */

/** de__mesh.edge_flags bits. Edge i of a triangle runs from corner i to corner (i + 1) % 3. */
enum {
    DE__MESH_EDGE0_ACTIVE = 1u << 0, /**< convex edge, or open boundary: may produce contacts */
    DE__MESH_EDGE1_ACTIVE = 1u << 1,
    DE__MESH_EDGE2_ACTIVE = 1u << 2,
    DE__MESH_TRI_DEGENERATE = 1u << 3 /**< zero area: not in the BVH, never collides */
};

/**
 * Static triangle mesh. Triangle t has corners indices[3t..3t+2]. `adjacent[3t + i]` is the
 * triangle across edge i (DE__MESH_NO_ADJ for a boundary or non-manifold edge). An edge is
 * active when it is a boundary or convex (the neighbour bends away from the triangle's front);
 * flat and concave edges are inactive: the narrowphase uses the face normal instead of an edge
 * normal there, which removes ghost contacts on internal edges (DENG-134).
 */
typedef struct de__mesh {
    de_vec3 *vertices;
    u32 *indices;
    u32 *adjacent;
    u8 *edge_flags;
    u32 vertex_count, tri_count;
    de__bvh bvh; /**< over the non-degenerate triangles; prims are triangle indices */
} de__mesh;

/* ================================ Heightfield ========================================== */

/**
 * Heightfield (layout in de_shape_desc). `heights` is a copy of the input (raw, unscaled).
 * Blocks: DE_HEIGHTFIELD_BLOCK x DE_HEIGHTFIELD_BLOCK cells; block (bx, by) covers cells
 * [bx * B, min((bx + 1) * B, cols - 1)) x [by * B, ...). block_min / block_max are the scaled
 * local z range of the block's non-hole cells (min > max when every cell is a hole), indexed
 * by * blocks_x + bx.
 */
typedef struct de__heightfield {
    u32 rows, cols;
    de_vec3 scale;
    f32 *heights;
    u32 blocks_x, blocks_y;
    f32 *block_min, *block_max;
} de__heightfield;

/* ================================ Compound ============================================= */

typedef struct de__compound_child {
    de__shape *shape; /**< referenced (refcount held) */
    de_shape handle;
    de_vec3 position; /**< child origin in the compound frame */
    de_quat rotation; /**< unit, child -> compound */
    de_aabb bounds;   /**< child's box in the compound frame */
} de__compound_child;

typedef struct de__compound {
    de__compound_child *children;
    u32 child_count;
    de__bvh bvh; /**< prims are child indices */
} de__compound;

/* ================================ Shape ================================================ */

struct de__shape {
    de_shape_kind kind;
    de_shape handle;
    u32 refcount; /**< bodies + compounds that use it */
    de_aabb local_aabb;
    f32 margin;          /**< convex radius: sphere / capsule radius, else 0 */
    f32 bounding_radius; /**< max distance from the local origin to the shape */
    bool has_volume;     /**< false for mesh, heightfield and compounds holding one */
    de__unit_mass unit_mass;
    union {
        struct {
            f32 radius;
        } sphere;
        struct {
            f32 radius, half_height;
        } capsule;
        struct {
            de_vec3 half_extents;
        } box;
        struct {
            f32 radius, half_height;
        } cylinder;
        de__hull hull;
        de__mesh mesh;
        de__heightfield hf;
        de__compound compound;
    } u;
    void *data; /**< the one allocation holding the kind's arrays (BVH arrays are separate) */
    size_t data_size;
};

/* ================================ Shape API ============================================ */

/** The live shape `s` of `p`, or NULL for a stale or invalid handle. */
de__shape *de__shape_get(const de_physics_world *p, de_shape s);

/** true for the kinds with a support function: sphere, capsule, box, cylinder, convex. */
static inline bool de__shape_kind_is_convex(de_shape_kind k) {
    return k == DE_SHAPE_SPHERE || k == DE_SHAPE_CAPSULE || k == DE_SHAPE_BOX ||
           k == DE_SHAPE_CYLINDER || k == DE_SHAPE_CONVEX;
}
/** Convex radius: the shape is its core (de__shape_support_core) swept by this radius. */
static inline f32 de__shape_margin(const de__shape *s) {
    return s->margin;
}

/**
 * Support point of the core of a convex shape in its local frame: a point of the core with the
 * largest dot(point, dir). Sphere core = origin; capsule core = its segment; box, cylinder and
 * hull have no radius (core = shape). Ties: the +side for primitives (dir component >= 0 picks
 * +extent), the lowest vertex index for hulls; a cylinder with dir.xy = 0 gives the cap center.
 * `dir` need not be unit length; the zero vector is allowed. Not for non-convex kinds (returns
 * the origin).
 */
de_vec3 de__shape_support_core(const de__shape *s, de_vec3 dir);
/** Support point of the whole shape: core support + margin * dir / |dir| (+Z for a zero dir). */
de_vec3 de__shape_support(const de__shape *s, de_vec3 dir);

/**
 * The world box of shape `s` placed at (`pos`, `rot`): computed in f64 and rounded outward to
 * f32, so it always contains the shape. Sphere, capsule and cylinder are exact; the other kinds
 * transform their local box (exact for a box).
 */
de_aabb de__shape_world_aabb(const de__shape *s, de_dvec3 pos, de_quat rot);
/** Box of `local` (a box in some frame) after rotation `r` (f64) and translation `pos`, rounded
 *  outward to f32. The empty box stays empty. */
de_aabb de__aabb_transform(de_aabb local, de_dvec3 pos, const de__dmat3 *r);

/* ---- Builders (the shape's data block and BVHs come from p->alloc) ---- */

/**
 * convex_hull.c: builds the hull of points[0..n) with tolerance `tol` (> 0). Writes the hull's
 * arrays into one block from `a` (*out_block / *out_size; the caller frees it); temporary memory
 * also comes from `a`. Deterministic: the same points in the same order give the same bytes.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (fewer than 4 distinct points, or all colinear or
 *         coplanar within tol, or a non-finite point) or DE_ERR_OUT_OF_MEMORY.
 */
de_result de__hull_build(const de_allocator *a, const de_vec3 *points, u32 n, f32 tol,
                         de__hull *out, void **out_block, size_t *out_size);
/** convex_hull.c: the automatic tolerance for points[0..n): max(1e-5, 1e-4 * the longest side of
 *  their box, 4e-7 * their largest |coordinate|). */
f32 de__hull_auto_tolerance(const de_vec3 *points, u32 n);

/**
 * mesh_bvh.c: binned-SAH BVH (12 bins per axis, stable partitions, deterministic ties: lower
 * axis, then lower bin) over `count` primitives prims[i] with boxes bounds[prims[i]]. Leaves hold
 * at most `max_leaf` primitives; below depth 32 every split is a median split, so the depth stays
 * within DE__BVH_MAX_DEPTH. Arrays come from `a`; free with de__bvh_free.
 */
de_result de__bvh_build(const de_allocator *a, const de_aabb *bounds, const u32 *prims, u32 count,
                        u32 max_leaf, de__bvh *out);
void de__bvh_free(const de_allocator *a, de__bvh *bvh);

/** mesh_bvh.c: validates desc and fills s->u.mesh, s->data, local AABB and the BVH. */
de_result de__mesh_build(de_physics_world *p, const de_shape_desc *desc, de__shape *s);
/** heightfield.c: validates desc and fills s->u.hf, s->data, local AABB. */
de_result de__heightfield_build(de_physics_world *p, const de_shape_desc *desc, de__shape *s);
/** compound.c: validates desc (children live, not compound), takes the children's references and
 *  fills s->u.compound, s->data, local AABB, BVH, mass and has_volume. */
de_result de__compound_build(de_physics_world *p, const de_shape_desc *desc, de__shape *s);
/** compound.c: drops the children's references (shape destroy). */
void de__compound_release(de__shape *s);

/* ---- Heightfield helpers ---- */

/** Raw sample (col, row). */
static inline f32 de__hf_sample(const de__heightfield *hf, u32 col, u32 row) {
    return hf->heights[row * hf->cols + col];
}
/** true when cell (col, row) (corners (col, row) .. (col + 1, row + 1)) is a hole. */
static inline bool de__hf_cell_is_hole(const de__heightfield *hf, u32 col, u32 row) {
    return de__hf_sample(hf, col, row) == DE_HEIGHTFIELD_HOLE ||
           de__hf_sample(hf, col + 1, row) == DE_HEIGHTFIELD_HOLE ||
           de__hf_sample(hf, col, row + 1) == DE_HEIGHTFIELD_HOLE ||
           de__hf_sample(hf, col + 1, row + 1) == DE_HEIGHTFIELD_HOLE;
}
/** Local position of sample (col, row) (not a hole). */
static inline de_vec3 de__hf_vertex(const de__heightfield *hf, u32 col, u32 row) {
    return de__v3((f32)col * hf->scale.x, (f32)row * hf->scale.y,
                  de__hf_sample(hf, col, row) * hf->scale.z);
}
/**
 * The two triangles of cell (col, row), counter-clockwise seen from +Z: tri[0] = (c0, c1, c3),
 * tri[1] = (c0, c3, c2) with c0 = (col, row), c1 = (col + 1, row), c2 = (col, row + 1),
 * c3 = (col + 1, row + 1). Sub-shape id of tri[k]: 2 * (row * (cols - 1) + col) + k.
 */
static inline void de__hf_cell_triangles(const de__heightfield *hf, u32 col, u32 row,
                                         de_vec3 tri[2][3]) {
    const de_vec3 c0 = de__hf_vertex(hf, col, row), c1 = de__hf_vertex(hf, col + 1, row);
    const de_vec3 c2 = de__hf_vertex(hf, col, row + 1), c3 = de__hf_vertex(hf, col + 1, row + 1);
    tri[0][0] = c0;
    tri[0][1] = c1;
    tri[0][2] = c3;
    tri[1][0] = c0;
    tri[1][1] = c3;
    tri[1][2] = c2;
}

/* ---- Mass (mass.c) ---- */

void de__mass_sphere(f64 r, de__unit_mass *out);
/** Capsule along local Z: segment half length h, radius r. */
void de__mass_capsule(f64 r, f64 h, de__unit_mass *out);
void de__mass_box(de_dvec3 half_extents, de__unit_mass *out);
/** Cylinder along local Z: radius r, half height h. */
void de__mass_cylinder(f64 r, f64 h, de__unit_mass *out);
/** Hull by tetrahedra from the vertex centroid over each face fan. */
void de__mass_hull(const de__hull *hull, de__unit_mass *out);
/**
 * Adds `child` placed at (pos, rot) into the running sum `acc` (volume-weighted COM, inertia
 * about the origin); call de__mass_compound_finish once all children are in.
 */
void de__mass_compound_add(de__unit_mass *acc, const de__unit_mass *child, de_vec3 pos,
                           de_quat rot);
void de__mass_compound_finish(de__unit_mass *acc);
/** Mass properties at `density` (kg/m^3), rounded to f32. */
void de__unit_mass_scale(const de__unit_mass *u, f64 density, de_mass_props *out);
/** Inverse of a symmetric 3x3 (f64); a singular matrix gives the zero matrix. */
void de__mat3_inverse_d(const f64 m[9], f64 out[9]);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_SHAPE_INTERNAL_H */
