/*
 * query_internal.h: the private structures and internal API of ECS queries (DENG-346, SPEC §7.2).
 *
 * Include as "ecs/query_internal.h" (or "query_internal.h" inside src/ecs/). Not public API.
 *
 * Files:
 *   - query.c: the per-world query registry (slots, handles), create, destroy, the change window.
 *   - query_cache.c: archetype matching, the matched-archetype cache and its update when an
 *     archetype is created, and the iteration-order iterator.
 *   - query_iter.c: slices (de_query_chunk), de_query_each, de_query_count, de_query_par_each.
 *
 * Hook points for later S05 tasks:
 *   - DENG-347 (change detection): ecs_change_filter_default (query_iter.c) is the row predicate
 *     of DE_ADDED / DE_CHANGED filters; ecs_query_set_change_window stores the tick window it
 *     compares against; query_slice_done (query_iter.c) is where changed ticks of write terms
 *     are marked (ecs_query.terms[i].access).
 *   - DENG-350 (systems): per-range command buffers go into emit_slice / run_range
 *     (query_iter.c), the one place that builds a de_query_chunk.
 */
#ifndef DE_ECS_QUERY_INTERNAL_H
#define DE_ECS_QUERY_INTERNAL_H

#include "ecs_internal.h"

DE_EXTERN_C_BEGIN

/** Entry of an ecs_query cols array: the term is an absent optional component. */
#define ECS_QUERY_COL_ABSENT ECS_INVALID_INDEX
/** Entry of an ecs_query cols array: the term is a tag (no column; the placeholder is used). */
#define ECS_QUERY_COL_TAG (ECS_INVALID_INDEX - 1u)
/** Most column entries per matched archetype (terms + change filters). */
#define ECS_QUERY_MAX_COLS (DE_QUERY_MAX_TERMS + DE_QUERY_MAX_FILTERS)

/* Query handles: the low ECS_QUERY_INDEX_BITS hold slot + 1, the bits above the generation
 * (1 .. ECS_QUERY_GEN_MAX). A slot whose generation would wrap is retired, never reused. */
#define ECS_QUERY_INDEX_BITS 20u
#define ECS_QUERY_INDEX_MASK ((1u << ECS_QUERY_INDEX_BITS) - 1u)
#define ECS_QUERY_GEN_MAX    ((1u << (32u - ECS_QUERY_INDEX_BITS)) - 1u)
#define ECS_QUERY_MAX_SLOTS  (ECS_QUERY_INDEX_MASK - 1u)

typedef struct ecs_query ecs_query;

/**
 * The row predicate of a query's DE_ADDED / DE_CHANGED filters (DENG-347 implements it).
 * `filter_cols[j]` is the column in `a` of the query's j-th change filter
 * (q->filters[q->change_filters[j]]). Returns true when row `row` of chunk `ch` passes every
 * change filter in the query's window [q->last_run_tick, q->this_run_tick]. It must only read:
 * de_query_par_each calls it from many threads at once.
 */
typedef bool (*ecs_change_filter_fn)(const ecs_query *q, const ecs_archetype *a,
                                     const ecs_chunk *ch, const u32 *filter_cols, u32 row);

/**
 * A query. One allocation holds the struct and its arrays (`alloc_size` bytes).
 *
 * Matched archetypes are kept in `entries`, in creation order. Each entry is `entry_stride` u32:
 *   [0] archetype index,
 *   [1] rows of the matched archetypes before this one (written by de_query_par_each),
 *   [2 .. 2 + term_count) the column of each term (or ECS_QUERY_COL_ABSENT / _TAG),
 *   then the column of each change filter.
 * `scanned` world archetypes have been tested. Normally it equals the world's archetype count;
 * when an entry could not be stored (out of memory in the creation listener), it stays behind and
 * the iterator matches archetypes [scanned, count) on the fly, read-only, until the next
 * archetype creation retries.
 */
struct ecs_query {
    u32 term_count;
    u32 filter_count;
    de_query_term *terms;     /**< copy of desc->terms */
    de_query_filter *filters; /**< copy of desc->filters */
    u32 *required;            /**< sorted unique: READ / WRITE terms, WITH / ADDED / CHANGED */
    u32 required_count;
    u32 excluded_count;
    u32 *excluded;       /**< sorted unique: WITHOUT */
    u32 *change_filters; /**< indices into `filters` of the ADDED / CHANGED filters, in order */
    u32 change_count;
    u32 col_count;           /**< term_count + change_count */
    u32 entry_stride;        /**< 2 + col_count */
    u32 scanned;             /**< world archetypes tested so far (creation order) */
    de_array entries;        /**< entry_stride u32 per matched archetype */
    de_array range_commands; /**< reusable de_commands* preparation scratch for par_each */
    u64 match_tests;         /**< archetype match tests run by the cache (create + listener) */
    /* DENG-347: change window and row predicate. */
    u32 last_run_tick;
    u32 this_run_tick;
    ecs_change_filter_fn change_filter;
    /* DENG-347: false until ecs_query_set_change_window; without a window the change filters
     * pass every row and write marking stamps the world's current change tick
     * (change_internal.h). */
    bool change_window_set;
    size_t alloc_size;
};

/** One registry slot. `q` is NULL while the slot is free or retired. */
typedef struct ecs_query_slot {
    ecs_query *q;
    u32 generation; /**< 1 .. ECS_QUERY_GEN_MAX */
    u32 pad_;
} ecs_query_slot;

/** The queries of a world (de_world.queries). */
typedef struct ecs_query_registry {
    DE_ARRAY(ecs_query_slot) slots;
    DE_ARRAY(u32) free_slots; /**< LIFO; capacity >= slots.count, so destroy never allocates */
} ecs_query_registry;

/* ---- query.c -------------------------------------------------------------------------------- */

/** The query of handle `q`, or NULL (invalid, stale or destroyed). */
ecs_query *ecs_query_get(const de_world *w, de_query q);

/**
 * Sets the tick window the change filters of `q` compare against (DENG-347; DENG-350 calls it
 * before a system runs). Ignores an invalid handle.
 */
void ecs_query_set_change_window(de_world *w, de_query q, u32 last_run_tick, u32 this_run_tick);

/* ---- query_cache.c -------------------------------------------------------------------------- */

/** true if archetype `a` has every required component of `q` and no excluded one. */
bool ecs_query_matches(const ecs_query *q, const ecs_archetype *a);

/** Writes the `q->col_count` column entries of `q` for matching archetype `a` to `cols`. */
void ecs_query_columns(const ecs_query *q, const ecs_archetype *a, u32 *cols);

/**
 * Tests world archetypes [q->scanned, count) in creation order and appends the matches. On out of
 * memory stops at the archetype it could not store (q->scanned stays there) and returns
 * DE_ERR_OUT_OF_MEMORY; nothing is lost, a later call continues.
 */
de_result ecs_query_scan(de_world *w, ecs_query *q);

/** The ecs_on_archetype_created listener of the registry: scans every live query (slot order). */
void ecs_query_on_archetype_created(de_world *w, ecs_archetype *a, void *user);

static inline u32 ecs_query_entry_count(const ecs_query *q) {
    return q->entries.count;
}

static inline u32 *ecs_query_entry(const ecs_query *q, u32 k) {
    return (u32 *)q->entries.data + (size_t)k * q->entry_stride;
}

/**
 * Iteration-order cursor: the matched entries, then (only after an out-of-memory in the cache)
 * the unscanned world archetypes that match. Read-only on the query and the world.
 */
typedef struct ecs_query_iter {
    u32 pos;                      /**< next entry */
    u32 next;                     /**< next unscanned world archetype, once the entries are done */
    u32 cols[ECS_QUERY_MAX_COLS]; /**< columns of an on-the-fly match */
} ecs_query_iter;

/** Positions `it` at entry `pos` (0 = the start). */
static inline void ecs_query_iter_init(const ecs_query *q, ecs_query_iter *it, u32 pos) {
    it->pos = pos;
    it->next = q->scanned;
}

/**
 * The next matched archetype in iteration order (empty ones included) and its column entries
 * (`*cols`, q->col_count values, valid until the next call). false at the end.
 */
bool ecs_query_iter_next(const de_world *w, const ecs_query *q, ecs_query_iter *it,
                         ecs_archetype **a, const u32 **cols);

/* ---- query_iter.c --------------------------------------------------------------------------- */

/** The change-filter predicate of every query: the DE_ADDED / DE_CHANGED tick test of DENG-347
 * (ecs_change_filter_ticks, change.c). */
bool ecs_change_filter_default(const ecs_query *q, const ecs_archetype *a, const ecs_chunk *ch,
                               const u32 *filter_cols, u32 row);

DE_EXTERN_C_END

#endif /* DE_ECS_QUERY_INTERNAL_H */
