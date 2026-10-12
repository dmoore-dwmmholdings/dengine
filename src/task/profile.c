/*
 * profile.c: graph node statistics (DENG-293). The hooks themselves are inline, in profile.h.
 *
 * The de_prof_* functions the hooks call are defined in core (src/core/prof/prof.c, DENG-94) in
 * every DE_PROFILE build. Until S06 this file held weak no-op fallbacks for them.
 */
#include "task/profile.h"

#include <string.h>

/* ---------- Node statistics ---------- */

void de__task_node_prof_init(de__task_node_prof *p, const char *name) {
    memset(p, 0, sizeof *p);
#ifdef DE_PROFILE
    p->loc.name = name != NULL ? name : "task node";
    p->loc.function = "graph_run_node";
    p->loc.file = __FILE__;
    p->loc.line = 0;
    p->loc.color = 0;
#else
    (void)name;
#endif
#if DE__TASK_NODE_STATS
    de_atomic_init_u64(&p->last_ns, 0);
    de_atomic_init_u64(&p->total_ns, 0);
    de_atomic_init_u64(&p->runs, 0);
    de_atomic_init_u32(&p->last_worker, DE_TASK_WORKER_NONE);
#endif
}

bool de__task_node_prof_read(const de__task_node_prof *p, de_task_node_stats *out) {
    memset(out, 0, sizeof *out);
#if DE__TASK_NODE_STATS
    /* Runs first: a total read afterwards includes at least those runs, so avg is never 0 for a
     * node that ran (it may include one run more while the graph is running). */
    const u64 runs = de_atomic_load_u64(&p->runs, DE_ATOMIC_RELAXED);
    const u64 total = de_atomic_load_u64(&p->total_ns, DE_ATOMIC_RELAXED);
    out->runs = runs;
    out->last_ns = de_atomic_load_u64(&p->last_ns, DE_ATOMIC_RELAXED);
    out->avg_ns = runs > 0 ? total / runs : 0;
    out->last_worker = de_atomic_load_u32(&p->last_worker, DE_ATOMIC_RELAXED);
    return true;
#else
    (void)p;
    out->last_worker = DE_TASK_WORKER_NONE;
    return false;
#endif
}
