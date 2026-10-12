/*
 * profile.c: graph node statistics and the no-op profiler fallbacks (DENG-293). The hooks
 * themselves are inline, in profile.h.
 *
 * Profiler fallbacks. de_debug.h declares de_prof_zone_begin_ and the other de_prof_* functions,
 * and DE_PROFILE builds (Dev, and Release by default) call them, but the profiler backend (Tracy,
 * S06 DENG-94) does not exist yet. So this file defines every de_prof_* function as a no-op:
 *   - GCC and Clang outside Windows: weak definitions. A strong definition anywhere in the link
 *     (the backend) replaces them without any build change, as long as the object that holds it
 *     is linked in (a static archive member is pulled only for an undefined symbol, and these
 *     weak ones already define it: the backend must be linked as an object or object library,
 *     or define DE_PROFILE_BACKEND, below).
 *   - MSVC and Windows: no portable weak symbols, so plain definitions.
 * When the backend lands it defines DE_PROFILE_BACKEND for every module (on de_config), and the
 * fallbacks below compile out. They are only compiled with DE_PROFILE, the only builds that call
 * them.
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

/* ---------- Profiler fallbacks (until DENG-94) ---------- */

#if defined(DE_PROFILE) && !defined(DE_PROFILE_BACKEND)

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
#define DE__PROF_FALLBACK __attribute__((weak))
#else
#define DE__PROF_FALLBACK
#endif

DE__PROF_FALLBACK DE_API de_prof_zone de_prof_zone_begin_(const de_prof_srcloc *loc) {
    (void)loc;
    de_prof_zone z = {{0, 0}};
    return z;
}

DE__PROF_FALLBACK DE_API void de_prof_zone_end_(de_prof_zone z) {
    (void)z;
}

DE__PROF_FALLBACK DE_API void de_prof_frame_mark_(const char *name) {
    (void)name;
}

DE__PROF_FALLBACK DE_API void de_prof_plot_(const char *name, f64 value) {
    (void)name;
    (void)value;
}

DE__PROF_FALLBACK DE_API void de_prof_alloc_(const void *ptr, size_t size, const char *pool) {
    (void)ptr;
    (void)size;
    (void)pool;
}

DE__PROF_FALLBACK DE_API void de_prof_free_(const void *ptr, const char *pool) {
    (void)ptr;
    (void)pool;
}

#endif
