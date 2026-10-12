/*
 * profile.h: the task system's profiling hooks and graph node statistics (DENG-293; SPEC §18.1,
 * §18.4). Internal to src/task/; include as "task/profile.h".
 *
 * Two independent features, each compiled in or out by the build type:
 *   - Profile zones and plots (DE_PROFILE: Dev, and Release unless DE_ENABLE_PROFILE is off).
 *     Each graph node runs inside a zone named after the node (a de_prof_srcloc stored per
 *     compiled node, because Tracy-style profilers keep the srcloc pointer). An idle frame worker
 *     that parks runs inside a "task.park" zone, and the number of parked frame workers is
 *     plotted as "task.parked_workers" when one parks or wakes.
 *   - Node statistics (DE_DEV or DE_DEBUG): last and average run time (de_time_now_ns) and the
 *     worker index of the last run, read with de_task_graph_node_stats.
 * With neither (Shipping; Release without DE_ENABLE_PROFILE) every hook below expands to nothing
 * and de__task_node_prof is one unused byte, so the node-run path is the same code as without the
 * hooks.
 *
 * The de_prof_* functions are declared in de_debug.h and implemented in core
 * (src/core/prof/, DENG-94): they send to Tracy after de_prof_init, and return at once before it.
 */
#ifndef DE_TASK_PROFILE_H
#define DE_TASK_PROFILE_H

#include <dengine/de_base.h>
#include <dengine/de_debug.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "platform/de_atomic.h"

#if defined(DE_DEV) || defined(DE_DEBUG)
#define DE__TASK_NODE_STATS 1
#else
#define DE__TASK_NODE_STATS 0
#endif

/** Per compiled graph node: its profile srcloc and its statistics. */
typedef struct de__task_node_prof {
#ifdef DE_PROFILE
    de_prof_srcloc loc;
#endif
#if DE__TASK_NODE_STATS
    /* Written by the thread that runs the node, read by any thread (relaxed: the values are
     * statistics, exact only when the graph is not running). */
    de_atomic_u64 last_ns;
    de_atomic_u64 total_ns;
    de_atomic_u64 runs;
    de_atomic_u32 last_worker;
#endif
#if !defined(DE_PROFILE) && !DE__TASK_NODE_STATS
    u8 unused; /* C11 has no empty structs */
#endif
} de__task_node_prof;

/** Sets up `p` for a node called `name` (may be NULL) and clears its statistics (compile). */
void de__task_node_prof_init(de__task_node_prof *p, const char *name);
/** Copies the statistics of `p` into `out`; false when statistics are compiled out. */
bool de__task_node_prof_read(const de__task_node_prof *p, de_task_node_stats *out);

#if defined(DE_PROFILE) || DE__TASK_NODE_STATS

/** What the begin hook hands to the end hook. */
typedef struct de__task_node_scope {
#ifdef DE_PROFILE
    de_prof_zone zone;
#endif
#if DE__TASK_NODE_STATS
    u64 t0;
#endif
} de__task_node_scope;

static inline de__task_node_scope de__task_node_begin(de__task_node_prof *p) {
    de__task_node_scope s;
#ifdef DE_PROFILE
    s.zone = de_prof_zone_begin_(&p->loc);
#endif
#if DE__TASK_NODE_STATS
    (void)p;
    s.t0 = de_time_now_ns();
#endif
    return s;
}

static inline void de__task_node_end(de__task_node_prof *p, de__task_node_scope s) {
#if DE__TASK_NODE_STATS
    const u64 dt = de_time_now_ns() - s.t0;
    de_atomic_store_u64(&p->last_ns, dt, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u64(&p->total_ns, dt, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u64(&p->runs, 1, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&p->last_worker, de_task_worker_index(), DE_ATOMIC_RELAXED);
#endif
#ifdef DE_PROFILE
    de_prof_zone_end_(s.zone);
#endif
    (void)p;
    (void)s;
}

/** Opens the node scope `var` on `prof` (a de__task_node_prof *). */
#define DE__TASK_NODE_BEGIN(var, prof) const de__task_node_scope var = de__task_node_begin(prof)
/** Closes the node scope `var` opened on `prof`. */
#define DE__TASK_NODE_END(var, prof) de__task_node_end((prof), (var))

#else

#define DE__TASK_NODE_BEGIN(var, prof) ((void)0)
#define DE__TASK_NODE_END(var, prof)   ((void)0)

#endif

#ifdef DE_PROFILE
/** Plots the number of frame workers parked on the idle semaphore (`sleepers`, a de_atomic_u32
 * of the task system). */
#define DE__TASK_PROF_PLOT_PARKED(sleepers)                                                        \
    DE_PROFILE_PLOT("task.parked_workers", de_atomic_load_u32((sleepers), DE_ATOMIC_RELAXED))
#else
#define DE__TASK_PROF_PLOT_PARKED(sleepers) ((void)0)
#endif

#endif /* DE_TASK_PROFILE_H */
