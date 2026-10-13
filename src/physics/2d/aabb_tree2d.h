/*
 * aabb_tree2d.h: dynamic AABB tree of the 2D broadphase (DENG-253, SPEC §13.3). Internal: include
 * as "physics/2d/aabb_tree2d.h". The 2D port of physics/broadphase/aabb_tree.h (DENG-133).
 *
 * Nodes live in a pool of `capacity` nodes and refer to each other by index (no pointers), so the
 * layout of a tree depends only on the sequence of operations. Several trees may share one pool
 * (the 2D broadphase keeps its static and dynamic trees in one pool sized for every body). Freed
 * nodes go on a LIFO list and are reused first; a pool never grows after init.
 *
 * Insertion follows Box2D v3: a branch-and-bound descent picks the sibling with the lowest
 * surface-area-heuristic cost (in 2D the half perimeter of a box), then the path back to the root
 * is refitted and each node on it is rotated when a grandchild swap lowers the summed child
 * perimeter. Removal unlinks the leaf and refits its ancestors. A move is a removal and a
 * reinsertion of the same leaf node, so the leaf index (the proxy id) stays the same.
 *
 * Traversals (overlap and ray) visit nodes depth first, child1 before child2, by parent links
 * instead of a stack: no scratch memory, no depth limit, read-only (any number of threads may
 * traverse while nobody modifies the tree).
 *
 * Determinism (SPEC §5.7): float + - * / min max only, no hashing, no pointer order. The tree
 * shape depends on the operations and their order; callers that need an order independent of
 * history (the broadphase pair list) sort their output.
 */
#ifndef DE_PHYSICS_AABB_TREE2D_H
#define DE_PHYSICS_AABB_TREE2D_H

#include "physics/2d/p2d_internal.h"

DE_EXTERN_C_BEGIN

/** "No node": a leaf's children, the root's parent, the end of the free list. */
#define DE__TREE2D_NULL 0xFFFFFFFFu

/**
 * One node. A leaf has `child1 == child2 == DE__TREE2D_NULL`, `height` 0 and the caller's `value`;
 * an internal node has two children, height 1 + the larger child height, `value` DE__TREE2D_NULL
 * and the union of its children's boxes. A free node keeps the next free index in `parent` and
 * has height DE__TREE2D_NULL.
 */
typedef struct de__tree2d_node {
    de_aabb2d box;
    u32 parent;
    u32 child1;
    u32 child2;
    u32 value;  /**< leaf payload (the broadphase stores the body index) */
    u32 height; /**< 0 for a leaf; DE__TREE2D_NULL for a free node */
} de__tree2d_node;

/** Fixed node storage, shared by any number of trees. */
typedef struct de__tree2d_pool {
    de__tree2d_node *nodes; /**< [capacity] */
    u32 capacity;
    u32 used;       /**< allocated nodes (all trees) */
    u32 high_water; /**< nodes ever handed out */
    u32 free_head;  /**< LIFO list of freed nodes through `parent` */
} de__tree2d_pool;

/** Allocates `capacity` nodes from `a` (tag DE_MEM_PHYSICS). */
de_result de__tree2d_pool_init(de__tree2d_pool *pool, const de_allocator *a, u32 capacity);
/** Frees the nodes (safe on a zeroed pool). */
void de__tree2d_pool_free(de__tree2d_pool *pool, const de_allocator *a);

/** A tree in a pool. */
typedef struct de__tree2d {
    de__tree2d_pool *pool;
    u32 root;       /**< DE__TREE2D_NULL when empty */
    u32 leaf_count; /**< leaves (proxies) */
    u32 node_count; /**< leaves + internal nodes: 2 * leaf_count - 1, or 0 */
} de__tree2d;

/** Makes `t` an empty tree that takes its nodes from `pool`. */
void de__tree2d_init(de__tree2d *t, de__tree2d_pool *pool);

/**
 * Inserts a leaf with box `box` and payload `value`. Returns the leaf's node index (stable until
 * de__tree2d_remove), or DE__TREE2D_NULL when the pool has fewer than two free nodes (nothing
 * changes then). Never allocates.
 */
u32 de__tree2d_insert(de__tree2d *t, de_aabb2d box, u32 value);
/** Removes leaf `leaf` and frees its node (and its parent). */
void de__tree2d_remove(de__tree2d *t, u32 leaf);
/** Gives leaf `leaf` the box `box` and reinserts it (removal + insertion); the index stays. */
void de__tree2d_move(de__tree2d *t, u32 leaf, de_aabb2d box);

/** The box of node `n` (a leaf: the box given to insert or move). */
static inline de_aabb2d de__tree2d_box(const de__tree2d *t, u32 n) {
    return t->pool->nodes[n].box;
}
/** The payload of leaf `n`. */
static inline u32 de__tree2d_value(const de__tree2d *t, u32 n) {
    return t->pool->nodes[n].value;
}
/** Height of the root: 0 for an empty tree or a single leaf. */
u32 de__tree2d_height(const de__tree2d *t);
/** Sum of the half perimeters of the internal nodes (the SAH cost; a quality measure). */
f64 de__tree2d_area_cost(const de__tree2d *t);

/**
 * Checks every invariant: parent and child links, heights, internal boxes equal to the union of
 * their children (bit for bit), leaf and node counts, no node on the free list. Returns true when
 * consistent; otherwise logs the first problem (category "physics") and returns false. O(nodes).
 */
bool de__tree2d_validate(const de__tree2d *t);

/* ---- Box helpers ---------------------------------------------------------------------- */

/** true when the closed boxes a and b overlap (touching counts). */
static inline bool de__aabb2d_overlap(const de_aabb2d *a, const de_aabb2d *b) {
    return a->min.x <= b->max.x && b->min.x <= a->max.x && a->min.y <= b->max.y &&
           b->min.y <= a->max.y;
}
/** true when `outer` contains `inner` (closed boxes). */
static inline bool de__aabb2d_contains(const de_aabb2d *outer, const de_aabb2d *inner) {
    return outer->min.x <= inner->min.x && outer->min.y <= inner->min.y &&
           inner->max.x <= outer->max.x && inner->max.y <= outer->max.y;
}
static inline de_aabb2d de__aabb2d_union(const de_aabb2d *a, const de_aabb2d *b) {
    de_aabb2d r;
    r.min = de__v2_min(a->min, b->min);
    r.max = de__v2_max(a->max, b->max);
    return r;
}
/** `b` grown by `d` (>= 0) on every side, rounded outward (computed in f64). */
static inline de_aabb2d de__aabb2d_grow(const de_aabb2d *b, f32 d) {
    const f64 m = (f64)d;
    de_aabb2d r;
    r.min.x = de__f32_round_down((f64)b->min.x - m);
    r.min.y = de__f32_round_down((f64)b->min.y - m);
    r.max.x = de__f32_round_up((f64)b->max.x + m);
    r.max.y = de__f32_round_up((f64)b->max.y + m);
    return r;
}
/** Closed boxes a and b overlap or are within `d` of each other on both axes (f32 sums). */
static inline bool de__aabb2d_within(const de_aabb2d *a, const de_aabb2d *b, f32 d) {
    if (d == 0.0f)
        return de__aabb2d_overlap(a, b);
    return a->min.x <= b->max.x + d && b->min.x <= a->max.x + d && a->min.y <= b->max.y + d &&
           b->min.y <= a->max.y + d;
}

/* ---- Traversal ------------------------------------------------------------------------ */

/** Called for each leaf whose box overlaps the query box. Return false to stop. */
typedef bool (*de__tree2d_query_fn)(void *user, u32 leaf, u32 value);

/**
 * Calls `fn` for every leaf whose box overlaps `box` (closed boxes), depth first, child1 first.
 * Returns false when `fn` stopped the traversal. Read-only.
 */
bool de__tree2d_query(const de__tree2d *t, de_aabb2d box, de__tree2d_query_fn fn, void *user);

/**
 * A ray segment: origin + t * delta for t in [0, max_fraction]. The origin is f64 (plane
 * positions far from the origin); the slab tests against the f32 boxes run in f64.
 */
typedef struct de__tree2d_ray {
    de__d2 origin;
    de_vec2 delta;    /**< segment end - origin */
    f32 max_fraction; /**< the traversal starts with this bound */
} de__tree2d_ray;

/**
 * Called for each leaf whose box the segment enters within the current bound. Returns the new
 * bound: <= 0 stops, a smaller value clips the segment, `max_fraction` keeps going (collect all).
 */
typedef f32 (*de__tree2d_ray_fn)(void *user, const de__tree2d_ray *ray, u32 leaf, u32 value,
                                 f32 max_fraction);

/** Entry fraction of the segment [0, max_fraction] into the closed box: true and *t_enter, or
 *  false when it misses. A zero component of delta is a slab test on the origin alone. */
bool de__ray2d_aabb(const de__tree2d_ray *ray, f32 max_fraction, const de_aabb2d *box,
                    f64 *t_enter);

/**
 * Ray / segment traversal, depth first, child1 first. A node is skipped when the segment misses
 * it or enters it beyond the current bound (entering exactly at the bound still visits it).
 * Returns the final bound (0 when the callback stopped it). Read-only.
 */
f32 de__tree2d_raycast(const de__tree2d *t, const de__tree2d_ray *ray, de__tree2d_ray_fn fn,
                       void *user);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_AABB_TREE2D_H */
