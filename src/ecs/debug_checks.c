/* debug_checks.c: Debug/Dev checks of running systems (DENG-350, SPEC §7.3, §5.7 rules 4 and 5).
 *
 * Access checks: entity access, query iteration, resource access and event calls compare what
 * they touch with the access set of the calling thread's current system (system.c's scope) and
 * panic on anything undeclared, or on a write that was declared as a read. Exclusive systems and
 * code outside systems are not checked.
 *
 * Determinism checks (DE_SYSTEM_DETERMINISTIC): the thread must have the deterministic FP
 * environment (de_fp_env_check) when the system (or one of its par_each ranges) starts and ends,
 * and the system must not iterate a hash map: the per-thread de_hashmap_iter_count must not move
 * during its own scope. Scopes nest when a waiting thread helps run another system or range, so
 * each scope subtracts what the scopes nested in it counted.
 *
 * Release and Shipping builds compile the access checks out (ECS_CHECK_* expand to nothing) and
 * the scope hooks are empty. */
#include "system_internal.h"

#include "core/de_compiler.h"
#include "query_internal.h"

#include <dengine/de_math.h>

#if DE_HASHMAP_ITER_TRACKING
/* Hash-map iterations of the scopes nested in the current one on this thread. */
static DE_THREAD_LOCAL u64 ecs_tls_nested_iters;
#endif

#if ECS_DEBUG_CHECKS

static const char *access_mode(bool write) {
    return write ? "writes" : "reads";
}

static void check_item(const ecs_system *s, const ecs_access_item *items, u32 count, u32 id,
                       bool write, const char *kind, const char *what) {
    const ecs_access_item *it = ecs_access_find(items, count, id);
    if (it == NULL)
        DE_PANIC("ecs: system '%s' %s %s '%s' (id %u), which it did not declare",
                 ecs_system_name(s), access_mode(write), kind, what, id);
    if (write && it->write == 0)
        DE_PANIC("ecs: system '%s' writes %s '%s' (id %u), which it declared read-only",
                 ecs_system_name(s), kind, what, id);
}

/* The current system of `w` that the checks apply to (not exclusive), or NULL. */
static const ecs_system *checked_system(const de_world *w) {
    const ecs_system *s = ecs_system_current(w);
    return s != NULL && !s->access.exclusive ? s : NULL;
}

void ecs_debug_check_component(const de_world *w, u32 component, bool write) {
    if (w == NULL)
        return;
    const ecs_system *s = checked_system(w);
    if (s == NULL)
        return;
    const ecs_component_info *info = ecs_component_get(w, component);
    check_item(s, s->access.components, s->access.component_count, component, write, "component",
               info != NULL ? info->name : "?");
}

void ecs_debug_check_resource(const de_world *w, u32 resource) {
    if (w == NULL)
        return;
    const ecs_system *s = checked_system(w);
    if (s == NULL)
        return;
    check_item(s, s->access.resources, s->access.resource_count, resource, false, "resource", "");
}

void ecs_debug_check_event(const de_world *w, u32 event, bool write) {
    if (w == NULL)
        return;
    const ecs_system *s = checked_system(w);
    if (s == NULL)
        return;
    check_item(s, s->access.events, s->access.event_count, event, write, "event type", "");
}

void ecs_debug_check_query(const de_world *w, const ecs_query *q) {
    if (w == NULL || q == NULL)
        return;
    const ecs_system *s = checked_system(w);
    if (s == NULL)
        return;
    for (u32 i = 0; i < q->term_count; ++i) {
        const de_access a = q->terms[i].access;
        ecs_debug_check_component(w, q->terms[i].component.id,
                                  a == DE_WRITE || a == DE_OPTIONAL_WRITE);
    }
    for (u32 j = 0; j < q->change_count; ++j)
        ecs_debug_check_component(w, q->filters[q->change_filters[j]].component.id, false);
}

static void check_fp_env(const ecs_system *s, const char *when) {
    if ((s->flags & DE_SYSTEM_DETERMINISTIC) && !de_fp_env_check())
        DE_PANIC("ecs: deterministic system '%s' %s on a thread without the deterministic FP "
                 "environment (call de_fp_env_init_thread; SPEC 5.7 rule 4)",
                 ecs_system_name(s), when);
}

#endif /* ECS_DEBUG_CHECKS */

void ecs_debug_scope_enter(const ecs_system *s, ecs_system_scope *sc) {
#if DE_HASHMAP_ITER_TRACKING
    sc->iter_start = de_hashmap_iter_count();
    sc->prev_nested = ecs_tls_nested_iters;
    ecs_tls_nested_iters = 0;
#else
    sc->iter_start = 0;
    sc->prev_nested = 0;
#endif
#if ECS_DEBUG_CHECKS
    check_fp_env(s, "starts");
#else
    (void)s;
#endif
}

void ecs_debug_scope_leave(const ecs_system *s, ecs_system_scope *sc) {
#if DE_HASHMAP_ITER_TRACKING
    const u64 total = de_hashmap_iter_count() - sc->iter_start;
    const u64 own = total - ecs_tls_nested_iters;
    ecs_tls_nested_iters = sc->prev_nested + total;
    if ((s->flags & DE_SYSTEM_DETERMINISTIC) && own != 0)
        DE_PANIC("ecs: deterministic system '%s' iterated a hash map %llu times (iteration order "
                 "is not deterministic; SPEC 5.7 rule 5)",
                 ecs_system_name(s), (unsigned long long)own);
#else
    (void)sc;
#endif
#if ECS_DEBUG_CHECKS
    check_fp_env(s, "ends");
#else
    (void)s;
#endif
}
