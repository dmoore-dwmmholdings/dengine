/*
 * broadphase2d.h: the 2D broadphase (DENG-253, SPEC §13.3, §13.2). Internal: include as
 * "physics/2d/broadphase2d.h". The 2D port of the DENG-133 broadphase (physics/broadphase/).
 *
 * Two dynamic AABB trees (aabb_tree2d.h) in one node pool: the static tree holds static bodies,
 * the dynamic tree dynamic and kinematic bodies. Each body with a shape has one proxy: a leaf whose
 * box is the body's world AABB grown by a margin (the "fat" box, default 0.05 m). A proxy is
 * reinserted ("moved") only when the body's box leaves its fat box. A chain body has one proxy
 * for the whole chain; its segments are found by the midphase (manifold2d.c) through the chain's
 * own segment bounds (de__chain2d_query).
 *
 * State lives in the world's DE__P2D_EXT_BROADPHASE slot, created by de__bp2d_create (explicit
 * settings) or lazily by de__bp2d_update (defaults). It follows bodies through the slot callbacks:
 * body_added inserts a proxy, body_removed removes it.
 *
 * Pairs. de__bp2d_update keeps a persistent sorted set of candidate pairs (proxies whose fat boxes
 * overlap and that may collide), updated from the moved proxies only, and outputs the candidates
 * whose boxes are within the pair distance (de__bp2d_set_pair_distance; 0 = overlap, touching
 * counts). The output is sorted by (lower body index, higher body index), each pair once:
 *
 *     { (a, b) : a < b, both alive with a shape, de__bp2d_should_collide(a, b),
 *       de__aabb2d_within(box[a], box[b], pair_distance) }
 *
 * where box is the body's predictive box (world_aabb when no growth was set; DENG-254 sets growth
 * for fast bodies with de__bp2d_set_predict). It does not depend on the insertion order, the move
 * history, the margin or the tree shape (SPEC §13.2): duplicates are removed by sorting
 * (de_radix_sort_u64), never by iterating a hash map.
 *
 * Memory. Everything is allocated at create from the world allocator: the node pool
 * (2 * max_bodies nodes), per-body arrays and the pair buffers (max_pairs, default
 * max(1024, 4 * max_bodies)). Update and the body callbacks never allocate. Past max_pairs, update
 * keeps a sorted subset (the moved proxies' queries stop at the first one that does not fit, in
 * body order, so the subset is the same on every run), logs a warning once, returns
 * DE_ERR_OUT_OF_MEMORY and requeries every proxy on the next update.
 *
 * Threading. Create, update, sync and the body callbacks run on the step's thread. The queries
 * only read: any number of threads may run them while nothing writes (DENG-258).
 *
 * Determinism (SPEC §5.7): float + - * / min max, bodies in ascending index, traversals depth
 * first and child1 first, output sorted with core's stable radix sort.
 */
#ifndef DE_PHYSICS_BROADPHASE2D_H
#define DE_PHYSICS_BROADPHASE2D_H

#include "physics/2d/aabb_tree2d.h"
#include "physics/2d/p2d_internal.h"

DE_EXTERN_C_BEGIN

/** Default fat-box margin (m). */
#define DE__BP2D_DEFAULT_MARGIN 0.05f
/** Smallest default pair capacity. */
#define DE__BP2D_MIN_PAIRS 1024u

/** Index of the static and the dynamic tree. */
enum { DE__BP2D_TREE_STATIC = 0, DE__BP2D_TREE_DYNAMIC = 1, DE__BP2D_TREE_COUNT = 2 };

typedef struct de__bp2d de__bp2d;

/** Settings. `(de__bp2d_desc){0}` gives the defaults. */
typedef struct de__bp2d_desc {
    f32 margin;    /**< fat-box margin in m; 0 = DE__BP2D_DEFAULT_MARGIN */
    u32 max_pairs; /**< candidate / output pair capacity; 0 = max(1024, 4 * max_bodies) */
} de__bp2d_desc;

/** One output pair: body indices with a < b. */
typedef struct de__bp2d_pair {
    u32 a;
    u32 b;
} de__bp2d_pair;

/** Counters for tests and the debug overlay. */
typedef struct de__bp2d_stats {
    u32 proxies[DE__BP2D_TREE_COUNT];
    u32 nodes[DE__BP2D_TREE_COUNT];
    u32 height[DE__BP2D_TREE_COUNT];
    u32 moved;      /**< proxies (re)inserted since the last pair pass */
    u32 queried;    /**< proxies queried by the last update */
    u32 candidates; /**< candidate pairs after the last update */
    u32 pairs;      /**< output pairs of the last update */
    u32 max_pairs;
    u32 overflows; /**< updates that ran out of pair capacity */
    f32 margin;
} de__bp2d_stats;

/* ---- Rules ---------------------------------------------------------------------------- */

/**
 * The collision filter rule of SPEC §13.1, the same as 3D (de__collision_filter_test): a and b may
 * collide iff (a.layer & b.mask) != 0 and (b.layer & a.mask) != 0, and not when they share a
 * non-zero group. Also the filter of the 2D queries (DENG-258).
 */
static inline bool de__p2d_filter_test(de_collision_filter a, de_collision_filter b) {
    if ((a.layer & b.mask) == 0u || (b.layer & a.mask) == 0u)
        return false;
    return a.group == 0u || a.group != b.group;
}

/** Body-type rule: a pair needs at least one dynamic body. */
static inline bool de__p2d_types_collide(u8 type_a, u8 type_b) {
    return type_a == (u8)DE_BODY_DYNAMIC || type_b == (u8)DE_BODY_DYNAMIC;
}

/**
 * Both rules for live bodies a and b (a != b). DENG-256 adds its joint rule (collide_connected
 * false) here, as DENG-220 did in 3D.
 */
bool de__joints2d_collide(const de_physics2d_world *p, u32 a, u32 b);
static inline bool de__bp2d_should_collide(const de_physics2d_world *p, u32 a, u32 b) {
    return de__p2d_types_collide(p->bodies.type[a], p->bodies.type[b]) &&
           de__p2d_filter_test(p->bodies.filter[a], p->bodies.filter[b]) &&
           de__joints2d_collide(p, a, b);
}

/* ---- Lifetime ------------------------------------------------------------------------- */

/**
 * Creates the broadphase in the DE__P2D_EXT_BROADPHASE slot and inserts a proxy for every live
 * body with a shape (ascending index).
 * @return DE_OK; DE_ERR_ALREADY_EXISTS; DE_ERR_INVALID_ARGUMENT (NULL world, negative or
 *         non-finite margin, max_pairs too large); DE_ERR_OUT_OF_MEMORY.
 */
de_result de__bp2d_create(de_physics2d_world *p, const de__bp2d_desc *desc);
/** The world's broadphase, or NULL before it was created. */
static inline de__bp2d *de__bp2d_get(const de_physics2d_world *p) {
    return (de__bp2d *)de__p2d_ext_get(p, DE__P2D_EXT_BROADPHASE);
}
/** The broadphase, created with the defaults if needed. */
de_result de__bp2d_ensure(de_physics2d_world *p, de__bp2d **out);

/* ---- Per step ------------------------------------------------------------------------- */

/**
 * Moves the proxies whose box left their fat box, finds new candidates from the moved proxies,
 * drops candidates that no longer apply, and builds the sorted output. Creates the broadphase
 * with the defaults on first use; never allocates afterwards.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT; DE_ERR_OUT_OF_MEMORY (create failed, or pair capacity
 *         exceeded: the output then holds a deterministic sorted subset).
 */
de_result de__bp2d_update(de_physics2d_world *p);

/**
 * From the next update on, output the candidates whose boxes are within `distance` (m) of each
 * other on both axes (the speculative margin; manifold2d.c sets it). Clamped to [0, margin];
 * 0 or a bad value = plain overlap. No effect before the broadphase exists.
 */
void de__bp2d_set_pair_distance(de_physics2d_world *p, f32 distance);

/** The output of the last update: `*count` pairs sorted by (a, b), a < b. Valid until the next
 *  update or until a body of a pair is destroyed. */
void de__bp2d_pairs(const de_physics2d_world *p, const de__bp2d_pair **pairs, u32 *count);

/** Moves the proxies whose box left their fat box, without finding pairs (for callers that
 *  teleport bodies and query between steps). No-op before the broadphase exists. */
void de__bp2d_sync(de_physics2d_world *p);
/** Joint connectivity changed; query every proxy next update. */
void de__bp2d_invalidate_pairs(de_physics2d_world *p);

/** Counters; zeroes before the broadphase exists. */
void de__bp2d_stats_get(const de_physics2d_world *p, de__bp2d_stats *out);

/**
 * Predictive growth of body `index`'s box from the next update or sync on (DENG-254, as DENG-219
 * in 3D): `lo` is subtracted from world_aabb.min, `hi` added to world_aabb.max. Negative or
 * non-finite components count as 0. Stays until set again; a new body starts with none.
 */
void de__bp2d_set_predict(de_physics2d_world *p, u32 index, de_vec2 lo, de_vec2 hi);
/** Body `index`'s predictive box (world_aabb when it has no growth or the broadphase does not
 *  exist). */
de_aabb2d de__bp2d_predict_box(const de_physics2d_world *p, u32 index);

/** Checks both trees and the proxy table (every live body with a shape has exactly one leaf in
 *  the right tree, carrying its index, whose fat box holds its box). Logs the first problem. */
bool de__bp2d_validate(const de_physics2d_world *p);

/* ---- Queries (read-only; DENG-258 builds the 2D scene queries on these) ---------------- */

/** The tree DE__BP2D_TREE_*, or NULL before the broadphase exists. */
const de__tree2d *de__bp2d_tree(const de_physics2d_world *p, u32 tree);
/** The leaf of body `index` (DE__TREE2D_NULL when it has none). */
u32 de__bp2d_proxy(const de_physics2d_world *p, u32 index);

/** Called for each body found by a query. Return false to stop. */
typedef bool (*de__bp2d_query_fn)(void *user, u32 body_index);

/**
 * Calls `fn` for every live body whose fat box overlaps `box` and, when `filter` is not NULL,
 * passes de__p2d_filter_test(*filter, body filter). Static tree first, then dynamic; depth first.
 * Candidates only (fat boxes; chains whole). Bodies teleported since the last update or sync are
 * found at their old place. Returns false when `fn` stopped.
 */
bool de__bp2d_query_aabb(const de_physics2d_world *p, de_aabb2d box,
                         const de_collision_filter *filter, de__bp2d_query_fn fn, void *user);

/** Called for each body whose fat box the segment enters within the bound; returns the new bound
 *  (<= 0 stops, smaller clips, `max_fraction` continues). */
typedef f32 (*de__bp2d_ray_fn)(void *user, const de__tree2d_ray *ray, u32 body_index,
                               f32 max_fraction);

/** Ray traversal over the static tree, then the dynamic tree (bound carried over); bodies
 *  filtered as in de__bp2d_query_aabb. Returns the final bound (0 when stopped). */
f32 de__bp2d_raycast(const de_physics2d_world *p, const de__tree2d_ray *ray,
                     const de_collision_filter *filter, de__bp2d_ray_fn fn, void *user);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_BROADPHASE2D_H */
