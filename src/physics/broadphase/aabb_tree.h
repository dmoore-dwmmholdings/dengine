/*
 * aabb_tree.h: dynamic AABB tree of the broadphase (DENG-133, SPEC §13.1). Internal: include as
 * "physics/broadphase/aabb_tree.h".
 *
 * Nodes live in a pool of `capacity` nodes and refer to each other by index (no pointers), so a
 * tree can be copied, hashed or checked without fixing up addresses, and its layout depends only
 * on the sequence of operations. Several trees may share one pool (the broadphase keeps its
 * static and dynamic trees in one pool sized for every body). Freed nodes go on a LIFO list and
 * are reused first, so a pool never grows after init.
 *
 * Insertion follows Box2D v3: a branch-and-bound descent picks the sibling with the lowest
 * surface-area (SAH) cost, then the path back to the root is refitted and each node on it is
 * rotated when a grandchild swap lowers the summed child area. Removal unlinks the leaf and
 * refits its ancestors. A move is a removal and a reinsertion of the same leaf node, so the leaf
 * index (the proxy id) stays the same.
 *
 * Traversals (overlap and ray) visit nodes depth first, child1 before child2. They walk by parent
 * links instead of a stack, so they need no scratch memory and have no depth limit, and they only
 * read the tree: any number of threads may traverse a tree at once while nobody modifies it. The
 * order of callbacks is that of a stack-based left-first traversal.
 *
 * Determinism (SPEC §5.7): float + - * / min max only, no hashing, no pointer order. The tree
 * shape depends on the operations and their order; callers that need an order independent of
 * history (the broadphase pair list) sort their output.
 */
#ifndef DE_PHYSICS_AABB_TREE_H
#define DE_PHYSICS_AABB_TREE_H

#include "physics/physics_internal.h"

DE_EXTERN_C_BEGIN

/** "No node": a leaf's children, the root's parent, the end of the free list. */
#define DE__TREE_NULL 0xFFFFFFFFu

/**
 * One node. A leaf has `child1 == child2 == DE__TREE_NULL`, `height` 0 and the caller's `value`;
 * an internal node has two children, height 1 + the larger child height, `value` DE__TREE_NULL
 * and the union of its children's boxes. A free node keeps the next free index in `parent`.
 */
typedef struct de__tree_node {
    de_aabb box;
    u32 parent;
    u32 child1;
    u32 child2;
    u32 value;  /**< leaf payload (the broadphase stores the body index) */
    u32 height; /**< 0 for a leaf; DE__TREE_NULL for a free node */
} de__tree_node;

/** Fixed node storage, shared by any number of trees. */
typedef struct de__tree_pool {
    de__tree_node *nodes; /**< [capacity] */
    u32 capacity;
    u32 used;       /**< allocated nodes (all trees) */
    u32 high_water; /**< nodes ever handed out; [high_water, capacity) never used */
    u32 free_head;  /**< LIFO list of freed nodes through `parent` */
} de__tree_pool;

/** Allocates `capacity` nodes from `a` (tag DE_MEM_PHYSICS). */
de_result de__tree_pool_init(de__tree_pool *pool, const de_allocator *a, u32 capacity);
/** Frees the nodes (NULL-safe on a zeroed pool). */
void de__tree_pool_free(de__tree_pool *pool, const de_allocator *a);

/** A tree in a pool. Zero-initialised fields with `pool` set and `root` DE__TREE_NULL is empty. */
typedef struct de__tree {
    de__tree_pool *pool;
    u32 root;       /**< DE__TREE_NULL when empty */
    u32 leaf_count; /**< leaves (proxies) */
    u32 node_count; /**< leaves + internal nodes: 2 * leaf_count - 1, or 0 */
} de__tree;

/** Makes `t` an empty tree that takes its nodes from `pool`. */
void de__tree_init(de__tree *t, de__tree_pool *pool);

/**
 * Inserts a leaf with box `box` and payload `value`. Returns the leaf's node index (stable until
 * de__tree_remove), or DE__TREE_NULL when the pool has fewer than two free nodes (nothing
 * changes then). Never allocates.
 */
u32 de__tree_insert(de__tree *t, de_aabb box, u32 value);
/** Removes leaf `leaf` and frees its node (and its parent). */
void de__tree_remove(de__tree *t, u32 leaf);
/** Gives leaf `leaf` the box `box` and reinserts it (removal + insertion); the index stays. */
void de__tree_move(de__tree *t, u32 leaf, de_aabb box);

/** The box of node `n` (a leaf: the box given to insert or move). */
static inline de_aabb de__tree_box(const de__tree *t, u32 n) {
    return t->pool->nodes[n].box;
}
/** The payload of leaf `n`. */
static inline u32 de__tree_value(const de__tree *t, u32 n) {
    return t->pool->nodes[n].value;
}
/** Height of the root: 0 for an empty tree or a single leaf. */
u32 de__tree_height(const de__tree *t);
/** Sum of the surface areas of the internal nodes (the SAH cost; a quality measure for tests). */
f64 de__tree_area_cost(const de__tree *t);

/**
 * Checks every invariant: parent links, child links, heights, internal boxes equal to the union
 * of their children (bit for bit), leaf and node counts, and that no node is on the free list.
 * Returns true when the tree is consistent; otherwise logs the first problem (category
 * "physics") and returns false. O(nodes); the broadphase calls it after each update in Debug
 * builds.
 */
bool de__tree_validate(const de__tree *t);

/* ---- Traversal ----------------------------------------------------------------------- */

/** true when the closed boxes a and b overlap (touching counts). */
static inline bool de__aabb_overlap(const de_aabb *a, const de_aabb *b) {
    return a->min.x <= b->max.x && b->min.x <= a->max.x && a->min.y <= b->max.y &&
           b->min.y <= a->max.y && a->min.z <= b->max.z && b->min.z <= a->max.z;
}
/** true when `outer` contains `inner` (closed boxes). */
static inline bool de__aabb_contains(const de_aabb *outer, const de_aabb *inner) {
    return outer->min.x <= inner->min.x && outer->min.y <= inner->min.y &&
           outer->min.z <= inner->min.z && inner->max.x <= outer->max.x &&
           inner->max.y <= outer->max.y && inner->max.z <= outer->max.z;
}

/**
 * Called for each leaf whose box overlaps the query box, with the leaf index and its payload.
 * Return true to continue, false to stop the traversal.
 */
typedef bool (*de__tree_query_fn)(void *user, u32 leaf, u32 value);

/**
 * Calls `fn` for every leaf whose box overlaps `box` (closed boxes), depth first, child1 first.
 * Returns false when `fn` stopped the traversal, true otherwise. Read-only.
 */
bool de__tree_query(const de__tree *t, de_aabb box, de__tree_query_fn fn, void *user);

/**
 * A ray segment: the points origin + t * delta for t in [0, max_fraction]. The origin is f64
 * (world positions up to ±100 km, DENG-2 Q10); the slab tests against the f32 boxes run in f64,
 * so they are exact up to the rounding of one division per axis.
 */
typedef struct de__tree_ray {
    de_dvec3 origin;
    de_vec3 delta;    /**< segment end - origin (for a direction d and length L: d * L) */
    f32 max_fraction; /**< in [0, 1] usually; the traversal starts with this bound */
} de__tree_ray;

/**
 * Called for each leaf whose box the segment [0, max_fraction] enters. Returns the new bound: a
 * value <= 0 stops the traversal; a value below `max_fraction` clips the segment (later nodes
 * entered beyond it are skipped); returning `max_fraction` keeps going unchanged (collect all
 * hits). A larger value never extends the segment.
 */
typedef f32 (*de__tree_ray_fn)(void *user, const de__tree_ray *ray, u32 leaf, u32 value,
                               f32 max_fraction);

/**
 * Entry fraction of the segment into `box`: true and *t_enter in [0, max_fraction] when the
 * segment [0, max_fraction] touches the closed box. A zero component of delta is a slab test on
 * the origin alone.
 */
bool de__ray_aabb(const de__tree_ray *ray, f32 max_fraction, const de_aabb *box, f64 *t_enter);

/**
 * Ray / segment traversal, depth first, child1 first. A node is skipped when the segment does
 * not touch it or enters it beyond the current bound (entering exactly at the bound still
 * visits it, so hits with equal fractions all reach the callback and the caller can break ties
 * by body index). Returns the final bound (0 when the callback stopped it). Read-only.
 */
f32 de__tree_raycast(const de__tree *t, const de__tree_ray *ray, de__tree_ray_fn fn, void *user);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_AABB_TREE_H */
