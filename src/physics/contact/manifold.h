/*
 * manifold.h: persistent contact manifolds and the collision pipeline of de_physics_step
 * (DENG-135, SPEC §13.1, §13.2, §5.7, §8). Internal: include as "physics/contact/manifold.h".
 *
 * One manifold per touching sub-shape pair of each broadphase pair. Each tick (step.c):
 *
 *   1. de__broadphase_update: the sorted pair list (pairs whose world AABBs are within the
 *      speculative margin, see de__broadphase_set_pair_distance).
 *   2. de__contacts_collide (pair_cache.c): de__collide per pair through de_task_parallel_for
 *      with a fixed grain (DE__CONTACT_PAIR_GRAIN). Each pair writes its candidate manifolds
 *      (reduced to <= 4 points, sorted by sub-shape key) into its own output slot. Where in the
 *      scratch pool a slot's entries land is not deterministic, but it is never observed: the
 *      merge reads the slots in pair order. When the pool overflows, the whole pass is redone on
 *      the calling thread in pair order, so what is dropped is deterministic too.
 *   3. de__contacts_merge (pair_cache.c): one serial pass in pair order over the slots and the
 *      previous tick's manifolds (both sorted by key) builds the new sorted manifold array:
 *      matched points carry their impulses, GJK single-point pairs accumulate points, keys that
 *      appear begin, keys that vanish end (end records), then the per-body lists are rebuilt.
 *
 * Determinism (SPEC §5.7). Manifolds are always kept and iterated in ascending key order (no
 * hash map exists). Parallel work writes only per-pair data and is reduced serially in pair order,
 * so the result is the same for tasks == NULL and any worker count. Float math is + - * / only.
 *
 * Memory. Everything is allocated by de__contacts_create from the world allocator (lazily on the
 * first de_physics_step, or explicitly with settings): three manifold arrays (previous, current,
 * scratch) of max_manifolds entries, the end records, the id free list, the per-pair slots
 * (broadphase max_pairs) and the per-body lists. The step allocates nothing afterwards. Past
 * max_manifolds, manifolds are dropped in key order (the lowest keys are kept), a warning is
 * logged once per overflow episode and the stats count it.
 *
 * Threading. Create, collide, merge and the accessors run on the step's thread; the accessors
 * only read, so solver code may call them from many threads while nothing steps.
 */
#ifndef DE_PHYSICS_CONTACT_MANIFOLD_H
#define DE_PHYSICS_CONTACT_MANIFOLD_H

#include "physics/broadphase/broadphase.h"
#include "physics/narrow/narrowphase.h"
#include "physics/physics_internal.h"
#include "platform/de_atomic.h"

DE_EXTERN_C_BEGIN

/* ================================ Settings ============================================= */

/** Most points a manifold keeps (the narrowphase gives up to DE__CONTACT_MAX_POINTS). */
#define DE__MANIFOLD_MAX_POINTS 4u
/** Default speculative margin (m): points closer than this are kept (S23 CCD enlarges it). */
#define DE__CONTACT_DEFAULT_SPECULATIVE 0.02f
#define DE__CONTACT_SPECULATIVE_CVAR    "physics.contact.speculative"
/** Manifold capacity cvar (int; 0 = max(1024, 2 * max_bodies)). Read at create. */
#define DE__CONTACT_MAX_MANIFOLDS_CVAR "physics.contact.max_manifolds"
#define DE__CONTACT_MIN_MANIFOLDS      1024u
/** An accumulated point is dropped once its two anchors drift apart along the contact plane by
 *  more than this (m). */
#define DE__CONTACT_DRIFT 0.02f
/** A new GJK point within this distance (m, on body A) of an accumulated one replaces it and takes
 *  its feature id and impulses. */
#define DE__CONTACT_MERGE_DIST 0.01f
/** A point whose feature id has no match in the last tick's manifold takes the impulses of the
 *  nearest unmatched old point within this distance (m, on A's anchors; DENG-217). */
#define DE__CONTACT_CARRY_DIST 0.01f
/** Pairs per de_task_parallel_for chunk. A constant: chunk boundaries must not depend on the
 *  machine (SPEC §5.7 rule 5). */
#define DE__CONTACT_PAIR_GRAIN 64u
/** Contact sets one pair may produce in one tick (a convex shape on many mesh triangles); the
 *  rest are dropped (the last ones in the narrowphase's visiting order). */
#define DE__CONTACT_PAIR_MAX_SETS 64u

/* ================================ Manifolds ============================================ */

/**
 * The key of a manifold: body indices a < b (the broadphase pair), then compound child and
 * triangle of each side (DE__SUB_NONE when the shape has none; see de__contact_set). Manifolds are
 * sorted by (a, b, child_a, sub_a, child_b, sub_b), compared as unsigned integers.
 */
typedef struct de__contact_key {
    u32 a, b;
    u32 child_a, sub_a;
    u32 child_b, sub_b;
} de__contact_key;

/** One persistent contact point. */
typedef struct de__contact {
    de_vec3 local_a;    /**< anchor on A, in A's body frame */
    de_vec3 local_b;    /**< anchor on B, in B's body frame */
    f32 separation;     /**< along the manifold normal; negative = overlap */
    u32 feature_id;     /**< narrowphase feature id (narrowphase.h); unique within the manifold */
    f32 normal_impulse; /**< accumulated normal impulse (S23 warm start); 0 when new */
    f32 tangent_impulse[2]; /**< accumulated friction impulses (S23 warm start); 0 when new */
} de__contact;

/** Manifold flags (de__manifold.flags). */
enum {
    /** The key started touching this tick (no manifold with this key last tick). */
    DE__MANIFOLD_BEGIN = 1u << 0,
    /** The key touched last tick too; matched points carry their impulses. */
    DE__MANIFOLD_PERSIST = 1u << 1,
    /** The first manifold of a body pair that had none last tick (pair-level begin; S23 sends
     *  one contact or trigger event per body pair from it). */
    DE__MANIFOLD_PAIR_BEGIN = 1u << 2,
    /** A body of the pair has DE_BODY_FLAG_TRIGGER: overlap only (separation <= 0), no points. */
    DE__MANIFOLD_TRIGGER = 1u << 3,
    /** GJK single-point pair (a cylinder against anything but a sphere): points are gathered over
     *  ticks (see de__manifold_accumulate). */
    DE__MANIFOLD_ACCUMULATE = 1u << 4
};

/**
 * A manifold: the contacts of one key. Touching means it exists: a solid pair has at least one
 * point within the speculative margin, a trigger pair overlaps. Layout: one contiguous record per
 * manifold, so S23 can color and solve them in place.
 */
typedef struct de__manifold {
    de__contact_key key;
    u32 gen_a, gen_b; /**< body generations when the manifold was made (index reuse ends it) */
    u32 id;           /**< stable while the manifold lives, in [0, 2 * max_manifolds) */
    u32 flags;        /**< DE__MANIFOLD_* */
    de_vec3 normal;   /**< unit, world axes, from A toward B */
    u32 point_count;  /**< 0..DE__MANIFOLD_MAX_POINTS; 0 for triggers */
    u32 serial;       /**< next accumulated-point serial (feature ids of accumulated points) */
    de__contact points[DE__MANIFOLD_MAX_POINTS];
} de__manifold;

/** End flags (de__contact_end.flags). */
enum {
    /** The body pair has no manifold left (pair-level end; S23 sends the end event from it). */
    DE__CONTACT_END_PAIR = 1u << 0,
    /** A body was destroyed, or its index reused by a new body, since the manifold was made. */
    DE__CONTACT_END_REMOVED = 1u << 1,
    /** The manifold was a trigger overlap. */
    DE__CONTACT_END_TRIGGER = 1u << 2,
    /** Dropped because max_manifolds was reached (not a real separation). */
    DE__CONTACT_END_OVERFLOW = 1u << 3
};

/** A manifold that ended this tick (in key order). The bodies may be dead. */
typedef struct de__contact_end {
    de__contact_key key;
    u32 gen_a, gen_b;
    u32 id; /**< the manifold's id; free again from the next tick */
    u32 flags;
} de__contact_end;

/** Compares keys (a, b, child_a, sub_a, child_b, sub_b): < 0, 0, > 0. */
static inline int de__contact_key_cmp(const de__contact_key *x, const de__contact_key *y) {
    const u32 xs[6] = {x->a, x->b, x->child_a, x->sub_a, x->child_b, x->sub_b};
    const u32 ys[6] = {y->a, y->b, y->child_a, y->sub_a, y->child_b, y->sub_b};
    for (u32 i = 0; i < 6; ++i)
        if (xs[i] != ys[i])
            return xs[i] < ys[i] ? -1 : 1;
    return 0;
}

/* ================================ Pure helpers (reduce.c, manifold.c) =================== */

/**
 * reduce.c: chooses at most DE__MANIFOLD_MAX_POINTS of n (<= DE__CONTACT_MAX_POINTS) points.
 * `pos` are the points in one frame, `normal` the unit contact normal in that frame. n <= 4 keeps
 * every point. Otherwise: the deepest point (smallest separation; ties: lowest feature id), then
 * the three others whose convex hull with it has the largest area in the contact plane, over every
 * candidate triple (at most 35). Ties: the larger spread (largest squared distance between two
 * chosen points), then the lower sorted feature ids. Writes the chosen indices to `out` in
 * ascending order and returns how many.
 */
u32 de__contact_reduce(const de_vec3 *pos, const f32 *sep, const u32 *feature, u32 n,
                       de_vec3 normal, u32 out[DE__MANIFOLD_MAX_POINTS]);

/** reduce.c: area in the plane with unit normal n of the convex hull of four points. */
f32 de__contact_quad_area(de_vec3 p0, de_vec3 p1, de_vec3 p2, de_vec3 p3, de_vec3 n);

/**
 * manifold.c: carries warm-start data from `old` to `cur` (the same key, both alive): every point
 * of `cur` whose feature id is in `old` takes that point's impulses; then (DENG-217) every other
 * point takes those of the nearest old point not taken yet within DE__CONTACT_CARRY_DIST (A's
 * anchors; ties: lower old index). The rest start at zero. Sets `cur->id`, `serial` and the
 * PERSIST flag from `old`.
 */
void de__manifold_carry(de__manifold *cur, const de__manifold *old);

/**
 * manifold.c: GJK single-point accumulation. `cur` holds the new point (point_count 1); `old`
 * (may be NULL) the points of the last tick. Old points are re-measured at the current poses
 * (`xa`, `xb`; separation along cur->normal); a point whose anchors drifted apart along the plane
 * by more than DE__CONTACT_DRIFT, or whose separation is above `speculative`, is dropped. An old
 * point within DE__CONTACT_MERGE_DIST of the new one (on A) is replaced by it (the new point takes
 * its feature id and impulses); otherwise the new point gets a fresh feature id (its curved-shape
 * half carries cur->serial, which then advances) and zero impulses. If more than
 * DE__MANIFOLD_MAX_POINTS remain, de__contact_reduce chooses. Points are kept in the order:
 * surviving old points, then the new one. Call de__manifold_carry first (it copies old's serial).
 */
void de__manifold_accumulate(de__manifold *cur, const de__manifold *old, de__pose xa, de__pose xb,
                             f32 speculative);

/* ================================ Contacts state ======================================== */

typedef struct de__contacts de__contacts;

/** Settings. `(de__contacts_desc){0}` gives the defaults (cvars). */
typedef struct de__contacts_desc {
    f32 speculative;   /**< m; 0 = cvar physics.contact.speculative (0.02) */
    u32 max_manifolds; /**< 0 = cvar or max(1024, 2 * max_bodies) */
} de__contacts_desc;

/** Counters of the last tick, for tests, the debug overlay and benchmarks. */
typedef struct de__contact_stats {
    u32 pairs;          /**< broadphase pairs collided */
    u32 manifolds;      /**< live manifolds */
    u32 points;         /**< solver points (sum of point_count) */
    u32 begins;         /**< manifolds with DE__MANIFOLD_BEGIN */
    u32 ends;           /**< end records */
    u32 max_manifolds;  /**< capacity */
    u32 dropped;        /**< manifolds or sets dropped by capacity limits this tick */
    u32 serial_reruns;  /**< total: narrowphase passes redone serially after a pool overflow */
    u32 overflow_ticks; /**< total: ticks that dropped something */
    f32 speculative;
} de__contact_stats;

/**
 * Creates the contacts state in the DE__PHYS_EXT_CONTACTS slot (and the broadphase with its
 * defaults if it does not exist). Registers the two cvars.
 * @return DE_OK; DE_ERR_ALREADY_EXISTS; DE_ERR_INVALID_ARGUMENT (NULL world, bad speculative
 *         margin, capacity too large); DE_ERR_OUT_OF_MEMORY (nothing is left behind).
 */
de_result de__contacts_create(de_physics_world *p, const de__contacts_desc *desc);
static inline de__contacts *de__contacts_get(const de_physics_world *p) {
    return (de__contacts *)de__phys_ext_get(p, DE__PHYS_EXT_CONTACTS);
}
/** The contacts state, created with the defaults if needed. */
de_result de__contacts_ensure(de_physics_world *p, de__contacts **out);

/** The speculative margin in use (m); DE__CONTACT_DEFAULT_SPECULATIVE before create. */
f32 de__contacts_speculative(const de_physics_world *p);

/**
 * pair_cache.c: narrowphase over pairs[0..count) (sorted, a < b, bodies alive with shapes) into
 * the per-pair slots: de_task_parallel_for on p->tasks with DE__CONTACT_PAIR_GRAIN, or inline when
 * p->tasks is NULL. Must be followed by de__contacts_merge with the same pairs.
 */
void de__contacts_collide(de_physics_world *p, const de__bp_pair *pairs, u32 count);
/**
 * pair_cache.c: merges the slots into the manifold array (see the file comment), writes the end
 * records and the per-body lists. Returns DE_OK or DE_ERR_OUT_OF_MEMORY when something was
 * dropped (logged once per overflow episode).
 */
de_result de__contacts_merge(de_physics_world *p, const de__bp_pair *pairs, u32 count);

/* ---- Iteration API (the S23 solver, contact events, debug draw, checksum) ---- */

/** The live manifolds of the last step, in ascending key order (empty before the first). Valid
 *  until the next step. */
void de__contacts_manifolds(const de_physics_world *p, const de__manifold **out, u32 *count);
/** Mutable view of the same array (S23 writes impulses; tests). */
de__manifold *de__contacts_manifolds_mut(de_physics_world *p, u32 *count);
/** The manifolds that ended in the last step, in ascending key order. */
void de__contacts_ended(const de_physics_world *p, const de__contact_end **out, u32 *count);
/** The indices (into the manifold array, ascending) of the manifolds of body `index`. */
void de__contacts_body(const de_physics_world *p, u32 index, const u32 **out, u32 *count);
/** Finds the manifold with key `key`; NULL when there is none (binary search). */
const de__manifold *de__contacts_find(const de_physics_world *p, const de__contact_key *key);
void de__contacts_stats(const de_physics_world *p, de__contact_stats *out);

/* ================================ Private to src/physics/contact/ ======================= */

/** The output of one pair in the scratch pool: entries [first, first + count). */
typedef struct de__contact_slot {
    u32 first;
    u32 count;
} de__contact_slot;

struct de__contacts {
    const de_allocator *alloc;
    f32 speculative;
    u32 max_manifolds; /**< capacity of each manifold array and of the end records */
    u32 max_pairs;     /**< slots (the broadphase's max_pairs at create) */
    u32 body_capacity; /**< max_bodies */

    de__manifold *live[2]; /**< live[cur]: the manifolds of the last step, sorted by key */
    u32 cur;
    u32 count;

    de__manifold *pool;         /**< [max_manifolds] per-pair candidates of this step */
    de_atomic_u64 pool_next;    /**< next free pool entry (parallel reservations) */
    de_atomic_u32 pool_full;    /**< 1 when a reservation did not fit */
    de_atomic_u32 sets_dropped; /**< sets past DE__CONTACT_PAIR_MAX_SETS in this pass */
    de__contact_slot *slots;    /**< [max_pairs] */

    de__contact_end *ended; /**< [max_manifolds] */
    u32 ended_count;

    u32 *free_ids; /**< LIFO stack of free manifold ids [2 * max_manifolds] */
    u32 free_count;

    u32 *body_start; /**< [body_capacity + 1]: body i's list is items[start[i], start[i + 1]) */
    u32 *body_items; /**< [2 * max_manifolds] manifold indices */
    u32 body_count;  /**< bodies covered by body_start (high_water at the last merge) */

    /* The pass in progress (de__contacts_collide). */
    const de_physics_world *pass_world;
    const de__bp_pair *pass_pairs;
    bool pass_serial; /**< the serial rerun: reservations in pair order, never parallel */
    u32 pass_dropped; /**< serial pass: candidates that did not fit the pool */

    de__contact_stats stats;
    bool overflow_logged;

    void *block; /**< the one allocation behind every array above */
    size_t block_size;
};

DE_EXTERN_C_END

#endif /* DE_PHYSICS_CONTACT_MANIFOLD_H */
