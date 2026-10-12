/*
 * system_internal.h: systems, access sets, schedules and the Debug/Dev system checks (DENG-350,
 * SPEC §7.3, §8, §5.7).
 *
 * Include as "ecs/system_internal.h" (or "system_internal.h" inside src/ecs/). Not public API.
 *
 * Files:
 *   - system.c: the system registry (slots, handles), add / remove / enable, the per-thread
 *     "current system" scope, the run tick for immediate change stamping, stats.
 *   - access.c: access sets (sorted component, resource and event items) and the conflict test.
 *   - schedule.c: the per-schedule order (explicit edges, conflict edges, topological order,
 *     cycle report), the compiled task graph and de_world_run_schedule.
 *   - debug_checks.c: the Debug/Dev access checks and the deterministic-system checks.
 *
 * Hooks in other files (one call each): entity.c, resource.c and event.c call the
 * ECS_CHECK_* macros; query_iter.c gives slices their command buffer and runs par_each ranges in
 * the system's scope; change.c stamps immediate changes with ecs_system_stamp_tick and clamps the
 * systems' last runs (ecs_systems_clamp_ticks); world.c calls ecs_systems_destroy.
 */
#ifndef DE_ECS_SYSTEM_INTERNAL_H
#define DE_ECS_SYSTEM_INTERNAL_H

#include "ecs_internal.h"

DE_EXTERN_C_BEGIN

/** 1 in Debug and Dev builds (where DE_ASSERT is on): the access and determinism checks of
 * debug_checks.c run. */
#if defined(DE_DEBUG) || defined(DE_DEV)
#define ECS_DEBUG_CHECKS 1
#else
#define ECS_DEBUG_CHECKS 0
#endif

/* System handles: the low ECS_SYSTEM_INDEX_BITS hold slot + 1, the bits above the generation
 * (1 .. ECS_SYSTEM_GEN_MAX). A slot whose generation would wrap is retired, never reused. */
#define ECS_SYSTEM_INDEX_BITS 20u
#define ECS_SYSTEM_INDEX_MASK ((1u << ECS_SYSTEM_INDEX_BITS) - 1u)
#define ECS_SYSTEM_GEN_MAX    ((1u << (32u - ECS_SYSTEM_INDEX_BITS)) - 1u)
#define ECS_SYSTEM_MAX_SLOTS  (ECS_SYSTEM_INDEX_MASK - 1u)

struct ecs_query;

/* ---- Access sets (access.c) ------------------------------------------------------------------ */

/** One accessed component, resource or event type. */
typedef struct ecs_access_item {
    u32 id;    /**< component id, resource id or event type id */
    u32 write; /**< 1: written (implies read), 0: read only */
} ecs_access_item;

/** What a system may touch. Each array is sorted by id, one item per id. */
typedef struct ecs_access_set {
    ecs_access_item *components;
    ecs_access_item *resources;
    ecs_access_item *events;
    u32 component_count;
    u32 resource_count;
    u32 event_count;
    bool exclusive; /**< DE_SYSTEM_EXCLUSIVE: everything, conflicts with every system */
} ecs_access_set;

/** Upper bounds of the item counts of `d` (before duplicates are merged). */
void ecs_access_bounds(const de_system_desc *d, u32 *components, u32 *resources, u32 *events);

/**
 * Fills `out` from `d` into the arrays `out` already points at (sized by ecs_access_bounds):
 * terms read or write (optional ones too), DE_ADDED / DE_CHANGED filters read (DENG-347: the row
 * test reads the tick arrays), DE_WITH / DE_WITHOUT are not access; resources and events as
 * declared. Sorts and merges each array (a write wins). `d` must be validated already.
 */
void ecs_access_build(const de_system_desc *d, ecs_access_set *out);

/** true when two systems with these sets must not run at the same time. */
bool ecs_access_conflict(const ecs_access_set *a, const ecs_access_set *b);

/** The item of `id` in a sorted array, or NULL. Binary search. */
const ecs_access_item *ecs_access_find(const ecs_access_item *items, u32 count, u32 id);

/* ---- Systems (system.c) ---------------------------------------------------------------------- */

/**
 * A system. One allocation (`alloc_size` bytes) holds the struct, its query handles, its access
 * arrays and copies of its name and before / after names.
 */
typedef struct ecs_system {
    de_world *w;
    de_system handle;
    u64 seq;          /**< registration number in the world (registration order) */
    const char *name; /**< copy, or NULL */
    de_schedule schedule;
    de_system_fn fn;
    void *user;
    u32 flags; /**< de_system_flags */
    bool enabled;
    bool run_now; /**< `enabled` as the current run's pre-pass saw it */
    bool has_run; /**< `last_run` is valid */
    u32 last_run; /**< change tick of the last run (clamped by ecs_systems_clamp_ticks) */
    u32 this_run; /**< change tick of the current run */
    u32 order;    /**< position in the schedule's order: the command-buffer key */
    de_query *queries;
    u32 query_count;
    ecs_access_set access;
    const char **before;
    const char **after;
    u32 before_count;
    u32 after_count;
    de_task_node node; /**< in the schedule's compiled graph */
    de_system_ctx ctx; /**< filled by the pre-pass of each run */
    /* Timings when the world has no task system (Debug and Dev). */
    u64 serial_last_ns;
    u64 serial_total_ns;
    u64 serial_runs;
    u32 serial_last_worker;
    size_t alloc_size;
} ecs_system;

/** One registry slot. `sys` is NULL while the slot is free or retired. */
typedef struct ecs_system_slot {
    ecs_system *sys;
    u32 generation;
    u32 pad_;
} ecs_system_slot;

/** An edge of a schedule's order: `from` runs before `to` (indices into the schedule's systems). */
typedef struct ecs_sched_edge {
    u32 from;
    u32 to;
} ecs_sched_edge;

/** One schedule of a world. */
typedef struct ecs_schedule_state {
    DE_ARRAY(ecs_system *) systems; /**< registration order */
    DE_ARRAY(u32) order;            /**< the run order: indices into `systems` */
    DE_ARRAY(ecs_sched_edge) edges; /**< the DAG of the last build */
    de_task_graph *graph;           /**< NULL without a task system, or while dirty */
    bool dirty;                     /**< order / graph must be rebuilt before the next run */
    bool has_main_thread;           /**< a DE_SYSTEM_MAIN_THREAD system is in the schedule */
    u64 builds;                     /**< rebuilds so far (tests) */
} ecs_schedule_state;

/** The systems of a world (de_world.systems; created on first use). */
typedef struct ecs_system_registry {
    DE_ARRAY(ecs_system_slot) slots;
    DE_ARRAY(u32) free_slots; /**< LIFO; capacity >= slots.count */
    ecs_schedule_state schedules[DE_SCHEDULE_COUNT_];
    u64 next_seq;
    bool running; /**< a schedule runs */
} ecs_system_registry;

/** The registry of `w`, created on first use. NULL (error context set) on out of memory. */
ecs_system_registry *ecs_systems_get(de_world *w);

/** The system of handle `s`, or NULL (invalid, stale or removed). */
ecs_system *ecs_system_get(const de_world *w, de_system s);

/** Frees every system and schedule graph of the world (de_world_destroy). The queries are left
 * to ecs_queries_destroy. */
void ecs_systems_destroy(de_world *w);

/** Clamps every system's last run with ecs_tick_clamp (ecs_check_change_ticks). */
void ecs_systems_clamp_ticks(de_world *w, u32 now);

/* Per-thread scope of a running system (or of a par_each range of one). Scopes nest: a thread
 * that waits inside a system may help run another system's node or range. */
typedef struct ecs_system_scope {
    const ecs_system *prev;
    u64 iter_start;  /**< de_hashmap_iter_count() at enter (Debug/Dev) */
    u64 prev_nested; /**< the enclosing scope's nested-iteration count (Debug/Dev) */
} ecs_system_scope;

/** Makes `s` the calling thread's current system (and runs the enter checks). */
void ecs_system_scope_enter(const ecs_system *s, ecs_system_scope *sc);
/** Runs the leave checks and restores the previous current system. */
void ecs_system_scope_leave(const ecs_system *s, ecs_system_scope *sc);

/** The system running on the calling thread if it belongs to `w`, else NULL. */
const ecs_system *ecs_system_current(const de_world *w);

/** The tick an immediate change of `w` gets on the calling thread: the current system's run
 * tick inside a system of `w`, else the world's change tick. */
u32 ecs_system_stamp_tick(const de_world *w);

/** The command buffer for par_each range `range` of system `s` (key (s->order, range + 1)). */
de_commands *ecs_system_range_commands(const ecs_system *s, u32 range);

/** The name of `s` for messages ("<unnamed>" when it has none). */
static inline const char *ecs_system_name(const ecs_system *s) {
    return s->name != NULL ? s->name : "<unnamed>";
}

/* ---- Schedules (schedule.c) ------------------------------------------------------------------ */

/**
 * Builds the order of schedule `sc` (edges and topological order; sets each system's `order`).
 * With `warn`, before / after names that match no system are logged. @return DE_OK,
 * DE_ERR_INVALID_ARGUMENT (a cycle of before / after edges; the message names it, prefixed by
 * `who`) or DE_ERR_OUT_OF_MEMORY.
 */
de_result ecs_schedule_build_order(de_world *w, ecs_schedule_state *sc, bool warn, const char *who);

/** Frees a schedule's arrays and graph. */
void ecs_schedule_free(ecs_schedule_state *sc);

/* ---- Debug checks (debug_checks.c) ----------------------------------------------------------- */

/** Panics if the current system of `w` on this thread did not declare `component` (as a write
 * when `write`). No-op outside systems of `w` and in exclusive systems. */
void ecs_debug_check_component(const de_world *w, u32 component, bool write);
/** The same for resource `resource` (any declared mode: de_resource_get has no read-only form). */
void ecs_debug_check_resource(const de_world *w, u32 resource);
/** The same for event type `event` (`write`: sending). */
void ecs_debug_check_event(const de_world *w, u32 event, bool write);
/** The same for every term (in its mode) and change filter (read) of query `q`. */
void ecs_debug_check_query(const de_world *w, const struct ecs_query *q);

/** Enter / leave checks of a scope: DE_SYSTEM_DETERMINISTIC systems need de_fp_env_check() and
 * must not iterate a hash map (de_hashmap_iter_count, nested scopes excluded). */
void ecs_debug_scope_enter(const ecs_system *s, ecs_system_scope *sc);
void ecs_debug_scope_leave(const ecs_system *s, ecs_system_scope *sc);

#if ECS_DEBUG_CHECKS
#define ECS_CHECK_COMPONENT(w, c, write) ecs_debug_check_component((w), (c), (write))
#define ECS_CHECK_RESOURCE(w, r)         ecs_debug_check_resource((w), (r))
#define ECS_CHECK_EVENT(w, t, write)     ecs_debug_check_event((w), (t), (write))
#define ECS_CHECK_QUERY(w, q)            ecs_debug_check_query((w), (q))
#else
#define ECS_CHECK_COMPONENT(w, c, write) ((void)0)
#define ECS_CHECK_RESOURCE(w, r)         ((void)0)
#define ECS_CHECK_EVENT(w, t, write)     ((void)0)
#define ECS_CHECK_QUERY(w, q)            ((void)0)
#endif

DE_EXTERN_C_END

#endif /* DE_ECS_SYSTEM_INTERNAL_H */
