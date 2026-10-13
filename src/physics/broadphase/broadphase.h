/*
 * broadphase.h: the physics broadphase (DENG-133, SPEC §13.1, §13.2). Internal: include as
 * "physics/broadphase/broadphase.h".
 *
 * Two dynamic AABB trees (aabb_tree.h) in one node pool: the static tree holds static bodies, the
 * dynamic tree dynamic and kinematic bodies. Each body with a shape has one proxy: a leaf whose
 * box is the body's world AABB grown by a margin on every side (the "fat" box, default 0.05 m,
 * cvar `physics.broadphase.margin`). A proxy is reinserted ("moved") only when the body's world
 * AABB leaves its fat box.
 *
 * State lives in the world's DE__PHYS_EXT_BROADPHASE slot. It is created by
 * de__broadphase_create (explicit settings) or lazily by de__broadphase_ensure /
 * de__broadphase_update (defaults), and then follows bodies through the slot callbacks:
 * body_added inserts a proxy, body_removed removes it. world.c and body.c know nothing of it.
 *
 * Pairs. de__broadphase_update keeps a persistent set of candidate pairs (proxies whose fat boxes
 * overlap and that may collide), updated from the moved proxies only, and outputs the candidates
 * whose world AABBs overlap (closed boxes: touching counts; or, once DENG-135's step sets a pair
 * distance, are within it: de__broadphase_set_pair_distance). The output is sorted by
 * (lower body index, higher body index) and contains each pair once, so it is exactly
 *
 *     { (a, b) : a < b, both bodies alive with a shape, de__bp_should_collide(a, b),
 *       world_aabb[a] and world_aabb[b] overlap }
 *
 * whatever the insertion order, the move history, the fat margin or the tree shape (SPEC §13.2).
 * Duplicates are removed by sorting (de_radix_sort_u64), never by iterating a hash map.
 *
 * Memory. Everything is allocated at create from the world allocator: the node pool
 * (2 * max_bodies nodes), per-body proxy arrays, and the pair buffers (max_pairs entries,
 * `physics.broadphase.max_pairs`, 0 = 4 * max_bodies, at least 1024). Update, pair finding and the
 * body callbacks never allocate. When more pairs than max_pairs are found, update keeps the
 * lowest keys, logs a warning once, returns DE_ERR_OUT_OF_MEMORY and requeries every proxy on
 * the next update (so it recovers when the pair count drops).
 *
 * Threading. Create, update, sync and the body callbacks are single-threaded (the step's thread).
 * The queries (de__broadphase_query_aabb, de__broadphase_raycast, the tree traversals) only read,
 * so any number of threads may run them at once while nothing writes (DENG-137).
 *
 * Determinism (SPEC §5.7): float + - * / min max only, bodies visited in ascending index,
 * traversals depth first and child1 first, output sorted with the stable radix sort of core.
 */
#ifndef DE_PHYSICS_BROADPHASE_H
#define DE_PHYSICS_BROADPHASE_H

#include "physics/broadphase/aabb_tree.h"
#include "physics/physics_internal.h"

DE_EXTERN_C_BEGIN

/** Default fat-box margin (m) and its cvar. */
#define DE__BP_DEFAULT_MARGIN 0.05f
#define DE__BP_MARGIN_CVAR    "physics.broadphase.margin"
/** Pair capacity cvar (int; 0 = max(1024, 4 * max_bodies)). Read at create. */
#define DE__BP_MAX_PAIRS_CVAR "physics.broadphase.max_pairs"
#define DE__BP_MIN_PAIRS      1024u

/** Index of the static and the dynamic tree. */
enum { DE__BP_TREE_STATIC = 0, DE__BP_TREE_DYNAMIC = 1, DE__BP_TREE_COUNT = 2 };

typedef struct de__broadphase de__broadphase;

/** Broadphase settings. `(de__broadphase_desc){0}` gives the defaults (cvars). */
typedef struct de__broadphase_desc {
    f32 margin;    /**< fat-box margin in m; 0 = cvar physics.broadphase.margin (0.05) */
    u32 max_pairs; /**< candidate / output pair capacity; 0 = cvar or max(1024, 4 * max_bodies) */
} de__broadphase_desc;

/** One output pair: body indices with a < b. */
typedef struct de__bp_pair {
    u32 a;
    u32 b;
} de__bp_pair;

/** Counters for tests, the debug overlay and the benchmark. */
typedef struct de__bp_stats {
    u32 proxies[DE__BP_TREE_COUNT]; /**< leaves per tree */
    u32 nodes[DE__BP_TREE_COUNT];   /**< nodes per tree */
    u32 height[DE__BP_TREE_COUNT];  /**< root height per tree */
    u32 moved;      /**< proxies reinserted (or inserted) since the last pair pass */
    u32 queried;    /**< proxies queried by the last update */
    u32 candidates; /**< candidate pairs after the last update */
    u32 pairs;      /**< output pairs of the last update */
    u32 max_pairs;  /**< capacity */
    u32 overflows;  /**< updates that ran out of pair capacity */
    f32 margin;
} de__bp_stats;

/* ---- Rules (the contract DENG-137 shares) --------------------------------------------- */

/**
 * The collision filter rule of SPEC §13.1, also used by scene queries (DENG-137): a and b may
 * collide iff (a.layer & b.mask) != 0 and (b.layer & a.mask) != 0, and not when they share a
 * non-zero group (a.group == b.group != 0, e.g. the bones of one ragdoll).
 */
static inline bool de__collision_filter_test(de_collision_filter a, de_collision_filter b) {
    if ((a.layer & b.mask) == 0u || (b.layer & a.mask) == 0u)
        return false;
    return a.group == 0u || a.group != b.group;
}

/**
 * Body-type rule: a pair needs at least one dynamic body. Static–static, kinematic–kinematic and
 * kinematic–static pairs are never generated.
 */
static inline bool de__bp_types_collide(u8 type_a, u8 type_b) {
    return type_a == (u8)DE_BODY_DYNAMIC || type_b == (u8)DE_BODY_DYNAMIC;
}

/** Both rules for live bodies a and b (a != b). */
static inline bool de__bp_should_collide(const de_physics_world *p, u32 a, u32 b) {
    return de__bp_types_collide(p->bodies.type[a], p->bodies.type[b]) &&
           de__collision_filter_test(p->bodies.filter[a], p->bodies.filter[b]);
}

/* ---- Lifetime ----------------------------------------------------------------------------- */

/**
 * Creates the broadphase in the world's DE__PHYS_EXT_BROADPHASE slot and inserts a proxy for
 * every live body with a shape (ascending index). Registers the two cvars.
 * @return DE_OK; DE_ERR_ALREADY_EXISTS (already created); DE_ERR_INVALID_ARGUMENT (NULL world,
 *         negative or non-finite margin); DE_ERR_OUT_OF_MEMORY.
 */
de_result de__broadphase_create(de_physics_world *p, const de__broadphase_desc *desc);
/** The world's broadphase, or NULL before it was created. */
static inline de__broadphase *de__broadphase_get(const de_physics_world *p) {
    return (de__broadphase *)de__phys_ext_get(p, DE__PHYS_EXT_BROADPHASE);
}
/** The broadphase, created with the defaults if needed (the only allocation it ever does). */
de_result de__broadphase_ensure(de_physics_world *p, de__broadphase **out);

/* ---- Per step (DENG-135 calls these from step.c, after the world AABBs are refreshed) --- */

/**
 * Moves the proxies whose world AABB left their fat box, finds the new candidate pairs from the
 * moved proxies, drops the candidates that no longer apply, and builds the sorted output pair
 * list. Creates the broadphase with the defaults on first use. Never allocates afterwards.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL world); DE_ERR_OUT_OF_MEMORY (create failed, or
 *         pair capacity exceeded: the output then holds the lowest max_pairs candidates that
 *         overlap, and a warning is logged on the first such update).
 */
de_result de__broadphase_update(de_physics_world *p);

/**
 * DENG-135: from the next update on, output the candidates whose world AABBs are within
 * `distance` (m) of each other on every axis instead of overlapping, so contacts within the
 * speculative margin are found. Clamped to [0, margin] (a pair that close is always a candidate);
 * 0 or a bad value restores plain overlap (the default). No effect before the broadphase exists.
 */
void de__broadphase_set_pair_distance(de_physics_world *p, f32 distance);

/**
 * The output of the last de__broadphase_update: `*count` pairs sorted by (a, b), a < b. Valid
 * until the next update or until a body of a pair is destroyed (check de__body_alive if bodies
 * may have been destroyed in between). Empty before the first update.
 */
void de__broadphase_pairs(const de_physics_world *p, const de__bp_pair **pairs, u32 *count);

/**
 * Moves the proxies whose world AABB left their fat box, without finding pairs (the moved
 * proxies are queried by the next update). For callers that teleport bodies and query between
 * steps. No-op when the broadphase was not created.
 */
void de__broadphase_sync(de_physics_world *p);

/** Counters; zeroes when the broadphase was not created. */
void de__broadphase_stats(const de_physics_world *p, de__bp_stats *out);

/**
 * Checks both trees (de__tree_validate) and the proxy table: every live body with a shape has
 * exactly one leaf, in the right tree, carrying its index, and its fat box contains its world
 * AABB unless it is waiting for a sync; no other leaves. Logs the first problem.
 */
bool de__broadphase_validate(const de_physics_world *p);

/* ---- Queries (read-only; DENG-137 builds the scene queries on these) --------------------- */

/** The tree of type `tree` (DE__BP_TREE_*), or NULL before the broadphase exists. */
const de__tree *de__broadphase_tree(const de_physics_world *p, u32 tree);
/** The leaf of body `index` (DE__TREE_NULL when it has none). */
u32 de__broadphase_proxy(const de_physics_world *p, u32 index);

/**
 * Called for each body found by a broadphase query. Return true to continue, false to stop.
 */
typedef bool (*de__bp_query_fn)(void *user, u32 body_index);

/**
 * Calls `fn` for every live body whose fat box overlaps `box` and, when `filter` is not NULL,
 * that passes de__collision_filter_test(*filter, body filter). Static tree first, then the
 * dynamic tree; each depth first, child1 first. Candidates only: the fat box is larger than the
 * body, so test `p->bodies.world_aabb` or the shape. Proxies follow bodies at
 * de__broadphase_update / sync: a body teleported since then (de_body_set_transform) is found
 * at its old place until the next update. Returns false when `fn` stopped the query.
 */
bool de__broadphase_query_aabb(const de_physics_world *p, de_aabb box,
                               const de_collision_filter *filter, de__bp_query_fn fn, void *user);

/**
 * Called for each body whose fat box the segment enters within the current bound: return the new
 * bound (see de__tree_ray_fn: <= 0 stops, smaller clips, `max_fraction` continues).
 */
typedef f32 (*de__bp_ray_fn)(void *user, const de__tree_ray *ray, u32 body_index, f32 max_fraction);

/**
 * Ray / segment traversal over the static tree, then the dynamic tree, with the bound carried
 * from one to the other; bodies filtered as in de__broadphase_query_aabb. Returns the final
 * bound (0 when the callback stopped it).
 */
f32 de__broadphase_raycast(const de_physics_world *p, const de__tree_ray *ray,
                           const de_collision_filter *filter, de__bp_ray_fn fn, void *user);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_BROADPHASE_H */
