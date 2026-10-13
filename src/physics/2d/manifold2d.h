/*
 * manifold2d.h: persistent 2D contact manifolds and the collision pipeline of de_physics2d_step
 * (DENG-253, SPEC §13.3, §13.2, §5.7, §8). Internal: include as "physics/2d/manifold2d.h". The 2D
 * mirror of the DENG-135 design (physics/contact/manifold.h).
 *
 * One manifold per touching (body a, body b, chain child) key. Each tick (step2d.c):
 *
 *   1. de__bp2d_update: the body pairs sorted by (a, b), within the speculative margin.
 *   2. de__contacts2d_collide:
 *      a. midphase, serial, in pair order: a pair without a chain is one sub-pair
 *         (a, b, DE__P2D_CHILD_NONE); a pair with a chain is one sub-pair per chain segment whose
 *         bounds overlap the other body's box grown by the margin, in the chain's frame
 *         (de__chain2d_query, ascending segment index). The sub-pair list is therefore sorted by
 *         key. Past capacity the rest is dropped (the lowest keys are kept).
 *      b. narrowphase: de__collide2d per sub-pair through de_task_parallel_for with a fixed grain
 *         (DE__CONTACT2D_PAIR_GRAIN). Sub-pair i writes only slot i (at most one contact set per
 *         sub-pair): no atomics, no shared pool, so the result never depends on the threads.
 *   3. de__contacts2d_merge, serial, in key order: the slots and the previous tick's manifolds
 *      (both sorted by key) give the new sorted manifold array: matched points carry their
 *      impulses (feature id, then nearest within DE__CONTACT2D_CARRY_DIST), keys that appear
 *      begin, keys that vanish end (end records), then the per-body lists are rebuilt.
 *   [DENG-254: the solver runs here, on de__contacts2d_manifolds_mut.]
 *   4. de__contacts2d_end_touch: the ids of the ended manifolds become free (never reused within
 *      the tick that ended them).
 *
 * Determinism (SPEC §5.7): manifolds are kept and iterated in ascending key order (no hash map);
 * parallel work writes only its own slot and is reduced serially in key order, so tasks == NULL
 * and any worker count give the same state. Float math is + - * / and de_dsqrt only.
 *
 * Memory. de__contacts2d_create allocates everything from the world allocator (lazily on the first
 * de_physics2d_step): sub-pairs and their slots (max_subpairs, default the broadphase's max_pairs),
 * two manifold arrays and the end records (max_manifolds, default max(1024, 2 * max_bodies)), the
 * id free list and the per-body lists. The step allocates nothing afterwards. Past a capacity,
 * work is dropped in key order, a warning is logged once per overflow episode and the stats count
 * it.
 *
 * Threading. Create, collide, merge, end_touch and the accessors run on the step's thread; the
 * accessors only read, so solver or query code may call them from many threads while nothing
 * steps.
 */
#ifndef DE_PHYSICS_MANIFOLD2D_H
#define DE_PHYSICS_MANIFOLD2D_H

#include "physics/2d/broadphase2d.h"
#include "physics/2d/narrow2d.h"
#include "physics/2d/p2d_internal.h"

DE_EXTERN_C_BEGIN

/* ================================ Settings ============================================= */

/** Default speculative margin (m), Box2D's 4 * linear slop: points closer than this are kept. */
#define DE__CONTACT2D_DEFAULT_SPECULATIVE 0.02f
/** Smallest default manifold capacity. */
#define DE__CONTACT2D_MIN_MANIFOLDS 1024u
/** A point whose feature id has no match last tick takes the impulses of the nearest unmatched
 *  old point within this distance (m, on A's anchors), as DENG-217 does in 3D. */
#define DE__CONTACT2D_CARRY_DIST 0.01f
/** Sub-pairs per de_task_parallel_for chunk. A constant: chunk boundaries must not depend on the
 *  machine (SPEC §5.7 rule 5). */
#define DE__CONTACT2D_PAIR_GRAIN 64u

/* ================================ Manifolds ============================================= */

/** The key of a manifold: body indices a < b, then the chain segment (DE__P2D_CHILD_NONE when
 *  neither body has a chain). Sorted as unsigned (a, b, child). */
typedef struct de__contact_key2d {
    u32 a, b;
    u32 child;
} de__contact_key2d;

static inline int de__contact_key2d_cmp(const de__contact_key2d *x, const de__contact_key2d *y) {
    if (x->a != y->a)
        return x->a < y->a ? -1 : 1;
    if (x->b != y->b)
        return x->b < y->b ? -1 : 1;
    if (x->child != y->child)
        return x->child < y->child ? -1 : 1;
    return 0;
}

/** One persistent contact point. */
typedef struct de__contact2d {
    de_vec2 local_a;     /**< anchor on A's surface, A's body frame */
    de_vec2 local_b;     /**< anchor on B's surface, B's body frame */
    f32 separation;      /**< along the manifold normal; negative = overlap */
    u32 feature_id;      /**< narrow2d.h; unique within the manifold */
    f32 normal_impulse;  /**< accumulated normal impulse (DENG-254 warm start); 0 when new */
    f32 tangent_impulse; /**< accumulated friction impulse (DENG-254); 0 when new */
} de__contact2d;

/** Manifold flags (de__manifold2d.flags). */
enum {
    /** The key started touching this tick. */
    DE__MANIFOLD2D_BEGIN = 1u << 0,
    /** The key touched last tick too; matched points carry their impulses. */
    DE__MANIFOLD2D_PERSIST = 1u << 1,
    /** The first manifold of a body pair that had none last tick (pair-level begin; one contact
     *  or trigger event per body pair comes from it, DENG-260). */
    DE__MANIFOLD2D_PAIR_BEGIN = 1u << 2,
    /** A body of the pair has DE_BODY_FLAG_TRIGGER: overlap only (separation <= 0), no points. */
    DE__MANIFOLD2D_TRIGGER = 1u << 3
};

/** A manifold: the contacts of one key. It exists while a solid pair has a point within the
 *  speculative margin, or a trigger pair overlaps. */
typedef struct de__manifold2d {
    de__contact_key2d key;
    u32 gen_a, gen_b; /**< body generations when it was made (index reuse ends it) */
    u32 id;           /**< stable while the manifold lives, in [0, 2 * max_manifolds) */
    u32 flags;        /**< DE__MANIFOLD2D_* */
    de_vec2 normal;   /**< unit, plane axes, from A toward B */
    u32 point_count;  /**< 0..2; 0 for triggers */
    de__contact2d points[DE__P2D_MAX_POINTS];
} de__manifold2d;

/** End flags (de__contact_end2d.flags). */
enum {
    /** The body pair has no manifold left (pair-level end). */
    DE__CONTACT2D_END_PAIR = 1u << 0,
    /** A body was destroyed, or its index reused, since the manifold was made. */
    DE__CONTACT2D_END_REMOVED = 1u << 1,
    /** The manifold was a trigger overlap. */
    DE__CONTACT2D_END_TRIGGER = 1u << 2,
    /** Dropped because a capacity was reached (not a real separation). */
    DE__CONTACT2D_END_OVERFLOW = 1u << 3
};

/** A manifold that ended this tick (in key order). The bodies may be dead. */
typedef struct de__contact_end2d {
    de__contact_key2d key;
    u32 gen_a, gen_b;
    u32 id; /**< free again after de__contacts2d_end_touch */
    u32 flags;
} de__contact_end2d;

/**
 * Carries warm-start data from `old` to `cur` (same key, both alive): each point of `cur` whose
 * feature id is in `old` takes that point's impulses; then each other point takes those of the
 * nearest old point not taken yet within DE__CONTACT2D_CARRY_DIST on A (ties: lower old index).
 * The rest start at zero. Sets cur->id from old and the PERSIST flag (BEGIN cleared).
 */
void de__manifold2d_carry(de__manifold2d *cur, const de__manifold2d *old);

/* ================================ Contacts state ======================================== */

typedef struct de__contacts2d de__contacts2d;

/** Settings. `(de__contacts2d_desc){0}` gives the defaults. */
typedef struct de__contacts2d_desc {
    f32 speculative;   /**< m; 0 = DE__CONTACT2D_DEFAULT_SPECULATIVE */
    u32 max_manifolds; /**< 0 = max(1024, 2 * max_bodies) */
    u32 max_subpairs;  /**< 0 = the broadphase's max_pairs */
} de__contacts2d_desc;

/** Counters of the last tick. */
typedef struct de__contact2d_stats {
    u32 pairs;          /**< broadphase pairs */
    u32 subpairs;       /**< sub-pairs collided */
    u32 manifolds;      /**< live manifolds */
    u32 points;         /**< sum of point_count */
    u32 begins;         /**< manifolds with DE__MANIFOLD2D_BEGIN */
    u32 ends;           /**< end records */
    u32 max_manifolds;  /**< capacity */
    u32 max_subpairs;   /**< capacity */
    u32 dropped;        /**< sub-pairs or manifolds dropped by capacity this tick */
    u32 overflow_ticks; /**< total: ticks that dropped something */
    f32 speculative;
} de__contact2d_stats;

/**
 * Creates the contacts state in the DE__P2D_EXT_CONTACTS slot (and the broadphase with its
 * defaults if it does not exist).
 * @return DE_OK; DE_ERR_ALREADY_EXISTS; DE_ERR_INVALID_ARGUMENT (NULL world, bad margin,
 *         capacity too large); DE_ERR_OUT_OF_MEMORY (nothing is left behind).
 */
de_result de__contacts2d_create(de_physics2d_world *p, const de__contacts2d_desc *desc);
static inline de__contacts2d *de__contacts2d_get(const de_physics2d_world *p) {
    return (de__contacts2d *)de__p2d_ext_get(p, DE__P2D_EXT_CONTACTS);
}
/** The contacts state, created with the defaults if needed. */
de_result de__contacts2d_ensure(de_physics2d_world *p, de__contacts2d **out);
/** The speculative margin in use (m); DE__CONTACT2D_DEFAULT_SPECULATIVE before create. */
f32 de__contacts2d_speculative(const de_physics2d_world *p);

/**
 * The box a chain body's segments are tested against for pair (chain body `chain`, other body
 * `other`): the other body's predictive box grown by `margin`, in the chain body's frame (f64,
 * rounded outward). Exposed for tests (the midphase brute-force check).
 */
de_aabb2d de__contacts2d_chain_box(const de_physics2d_world *p, u32 chain, u32 other, f32 margin);

/** Midphase and narrowphase of this tick over the broadphase's last output (see the file
 *  comment). Must be followed by de__contacts2d_merge. */
void de__contacts2d_collide(de_physics2d_world *p);
/** Merges the slots into the manifold array, writes the end records and the per-body lists.
 *  DE_OK, or DE_ERR_OUT_OF_MEMORY when something was dropped this tick. */
de_result de__contacts2d_merge(de_physics2d_world *p);
/** End of the tick's contact work (after the solver): the ended manifolds' ids become free. */
void de__contacts2d_end_touch(de_physics2d_world *p);

/* ---- Iteration API (solver, events, queries, checksum) ---- */

/** The live manifolds of the last step, in ascending key order. Valid until the next step. */
void de__contacts2d_manifolds(const de_physics2d_world *p, const de__manifold2d **out, u32 *count);
/** Mutable view of the same array (the solver writes impulses; tests). */
de__manifold2d *de__contacts2d_manifolds_mut(de_physics2d_world *p, u32 *count);
/** The manifolds that ended in the last step, in ascending key order. */
void de__contacts2d_ended(const de_physics2d_world *p, const de__contact_end2d **out, u32 *count);
/** The indices (into the manifold array, ascending) of the manifolds of body `index`. */
void de__contacts2d_body(const de_physics2d_world *p, u32 index, const u32 **out, u32 *count);
/** The manifold with key `key`, or NULL (binary search). */
const de__manifold2d *de__contacts2d_find(const de_physics2d_world *p,
                                          const de__contact_key2d *key);
/** The sub-pairs of the last collide, in key order (tests). */
void de__contacts2d_subpairs(const de_physics2d_world *p, const de__contact_key2d **out,
                             u32 *count);
void de__contacts2d_stats_get(const de_physics2d_world *p, de__contact2d_stats *out);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_MANIFOLD2D_H */
