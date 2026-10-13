/* system.c: the system registry (add, remove, enable), the per-thread current-system scope, the
 * run tick of immediate changes and per-system stats (DENG-350, SPEC §7.3, §18.4).
 *
 * A system is one allocation: the struct, its query handles, its access arrays and copies of the
 * strings in its desc. Handles are (generation << 20) | (slot + 1), like query handles. Each
 * schedule keeps its systems in registration order; adding or removing one marks the schedule
 * dirty (its order and graph are rebuilt by the next run, schedule.c) and drops the old graph at
 * once, so nothing keeps pointers into a freed system. */
#include "system_internal.h"

#include "in_schedule_internal.h" /* DENG-141 */

#include "change_internal.h" /* ecs_tick_clamp */
#include "commands_internal.h"
#include "core/de_compiler.h"

#include <dengine/de_platform.h>

#if ECS_DEBUG_CHECKS
static de_log_category sys_log_category(void) {
    return de_log_register_category("ecs", DE_LOG_INFO);
}
#define SYS_WARN(...) DE_WARN(sys_log_category(), __VA_ARGS__)
#else
#define SYS_WARN(...) ((void)0)
#endif

/* ---- Registry ------------------------------------------------------------------------------- */

static de_system handle_make(u32 slot, u32 generation) {
    de_system h;
    h.id = (generation << ECS_SYSTEM_INDEX_BITS) | (slot + 1u);
    return h;
}

ecs_system *ecs_system_get(const de_world *w, de_system s) {
    if (w == NULL || w->systems == NULL || s.id == 0)
        return NULL;
    const u32 index = s.id & ECS_SYSTEM_INDEX_MASK;
    if (index == 0 || index > w->systems->slots.count)
        return NULL;
    const ecs_system_slot *sl =
        (const ecs_system_slot *)de_array_at(&w->systems->slots, index - 1u);
    if (sl->sys == NULL || sl->generation != (s.id >> ECS_SYSTEM_INDEX_BITS))
        return NULL;
    return sl->sys;
}

ecs_system_registry *ecs_systems_get(de_world *w) {
    if (w->systems != NULL)
        return w->systems;
    ecs_system_registry *r = DE_NEW(w->a, ecs_system_registry, DE_MEM_ECS);
    if (r == NULL) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "ecs systems: out of memory");
        return NULL;
    }
    memset(r, 0, sizeof *r);
    DE_ARRAY_INIT_TAG(&r->slots, ecs_system_slot, w->a, DE_MEM_ECS);
    DE_ARRAY_INIT_TAG(&r->free_slots, u32, w->a, DE_MEM_ECS);
    for (u32 s = 0; s < DE_SCHEDULE_COUNT_; ++s) {
        ecs_schedule_state *sc = &r->schedules[s];
        DE_ARRAY_INIT_TAG(&sc->systems, ecs_system *, w->a, DE_MEM_ECS);
        DE_ARRAY_INIT_TAG(&sc->order, u32, w->a, DE_MEM_ECS);
        DE_ARRAY_INIT_TAG(&sc->edges, ecs_sched_edge, w->a, DE_MEM_ECS);
        sc->dirty = true;
    }
    w->systems = r;
    return r;
}

static void system_free(de_world *w, ecs_system *s) {
    DE_FREE(w->a, s, s->alloc_size);
}

void ecs_systems_destroy(de_world *w) {
    ecs_system_registry *r = w->systems;
    if (r == NULL)
        return;
    for (u32 s = 0; s < DE_SCHEDULE_COUNT_; ++s)
        ecs_schedule_free(&r->schedules[s]);
    for (u32 i = 0; i < r->slots.count; ++i) {
        ecs_system_slot *sl = &DE_ARRAY_AT(&r->slots, ecs_system_slot, i);
        if (sl->sys != NULL)
            system_free(w, sl->sys); /* its queries go with ecs_queries_destroy */
        sl->sys = NULL;
    }
    de_array_free(&r->slots);
    de_array_free(&r->free_slots);
    DE_FREE(w->a, r, sizeof *r);
    w->systems = NULL;
}

void ecs_systems_clamp_ticks(de_world *w, u32 now) {
    ecs_system_registry *r = w->systems;
    if (r == NULL)
        return;
    for (u32 i = 0; i < r->slots.count; ++i) {
        ecs_system *s = DE_ARRAY_AT(&r->slots, ecs_system_slot, i).sys;
        if (s != NULL && s->has_run)
            s->last_run = ecs_tick_clamp(s->last_run, now);
    }
}

/* ---- Validation ----------------------------------------------------------------------------- */

static bool access_valid(de_access a) {
    return a == DE_READ || a == DE_WRITE || a == DE_OPTIONAL_READ || a == DE_OPTIONAL_WRITE;
}

static bool event_valid(const de_world *w, de_event_type t) {
    return t.id != 0 && t.id <= w->events.count;
}

static de_result names_valid(const char *const *names, u32 n, const char *what) {
    if (n > 0 && names == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_system_add: NULL %s with a count of %u", what,
                       n);
    for (u32 i = 0; i < n; ++i)
        if (names[i] == NULL)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_system_add: %s[%u] is NULL", what, i);
    return DE_OK;
}

/* The live system named `name`, or NULL. Linear in the slots (add time only). */
static const ecs_system *find_by_name(const ecs_system_registry *r, const char *name) {
    for (u32 i = 0; i < r->slots.count; ++i) {
        const ecs_system *s = ((const ecs_system_slot *)de_array_at(&r->slots, i))->sys;
        if (s != NULL && s->name != NULL && strcmp(s->name, name) == 0)
            return s;
    }
    return NULL;
}

static de_result validate(const de_world *w, const de_system_desc *d) {
    const char *nm = d->name != NULL ? d->name : "<unnamed>";
    if (d->fn == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_system_add: system '%s' has no fn", nm);
    if ((u32)d->schedule >= (u32)DE_SCHEDULE_COUNT_)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_system_add: system '%s': unknown schedule %d",
                       nm, (int)d->schedule);
    const u32 known = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_DETERMINISTIC | DE_SYSTEM_EXCLUSIVE;
    if ((d->flags & ~known) != 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_system_add: system '%s': unknown flags 0x%x",
                       nm, d->flags & ~known);
    if ((d->query_count > 0 && d->queries == NULL) ||
        (d->resource_count > 0 && d->resources == NULL) ||
        (d->reads_event_count > 0 && d->reads_events == NULL) ||
        (d->writes_event_count > 0 && d->writes_events == NULL))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_system_add: system '%s': a NULL array with a non-zero count", nm);
    for (u32 i = 0; i < d->resource_count; ++i) {
        const de_resource_access *ra = &d->resources[i];
        if (ra->id.id == 0 || ra->id.id > w->resources.count || !access_valid(ra->access))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_system_add: system '%s': resource %u: invalid id %u or access %d",
                           nm, i, ra->id.id, (int)ra->access);
    }
    for (u32 i = 0; i < d->reads_event_count; ++i)
        if (!event_valid(w, d->reads_events[i]))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_system_add: system '%s': reads_events[%u]: invalid event type %u",
                           nm, i, d->reads_events[i].id);
    for (u32 i = 0; i < d->writes_event_count; ++i)
        if (!event_valid(w, d->writes_events[i]))
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                           "de_system_add: system '%s': writes_events[%u]: invalid event type %u",
                           nm, i, d->writes_events[i].id);
    de_result r;
    if ((r = names_valid(d->before, d->before_count, "before")) != DE_OK ||
        (r = names_valid(d->after, d->after_count, "after")) != DE_OK)
        return r;
    if (d->name != NULL && w->systems != NULL && find_by_name(w->systems, d->name) != NULL)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "de_system_add: a system named '%s' exists", nm);
    return DE_OK;
}

/* ---- Add ------------------------------------------------------------------------------------ */

static size_t align8(size_t v) {
    return (v + 7u) & ~(size_t)7u;
}

/* Copies `s` (n + 1 bytes with the NUL) to `*cursor` and moves the cursor on. */
static const char *copy_str(char **cursor, const char *s) {
    const size_t n = strlen(s) + 1u;
    char *dst = *cursor;
    memcpy(dst, s, n);
    *cursor += n;
    return dst;
}

/* Builds the system block from a validated desc. Queries are not created yet. */
static ecs_system *system_build(de_world *w, const de_system_desc *d) {
    u32 nc = 0, nr = 0, ne = 0;
    ecs_access_bounds(d, &nc, &nr, &ne);
    size_t strings = d->name != NULL ? strlen(d->name) + 1u : 0u;
    for (u32 i = 0; i < d->before_count; ++i)
        strings += strlen(d->before[i]) + 1u;
    for (u32 i = 0; i < d->after_count; ++i)
        strings += strlen(d->after[i]) + 1u;

    /* Layout: struct, queries, access items (components, resources, events), name pointers,
     * then the strings. */
    size_t size = align8(sizeof(ecs_system));
    const size_t queries_off = size;
    size = align8(size + (size_t)d->query_count * sizeof(de_query));
    const size_t items_off = size;
    size = align8(size + ((size_t)nc + nr + ne) * sizeof(ecs_access_item));
    const size_t names_off = size;
    size = align8(size + ((size_t)d->before_count + d->after_count) * sizeof(const char *));
    const size_t strings_off = size;
    size += strings;

    u8 *mem = (u8 *)DE_ALLOC(w->a, size, 8u, DE_MEM_ECS);
    if (mem == NULL)
        return NULL;
    memset(mem, 0, size);
    ecs_system *s = (ecs_system *)(void *)mem;
    s->alloc_size = size;
    s->w = w;
    s->schedule = d->schedule;
    s->fn = d->fn;
    s->user = d->user;
    s->flags = d->flags;
    s->enabled = true;
    s->serial_last_worker = DE_TASK_WORKER_NONE;
    s->owner = w->system_owner; /* DENG-93 */
    s->queries = (de_query *)(void *)(mem + queries_off);
    s->query_count = d->query_count;
    ecs_access_item *items = (ecs_access_item *)(void *)(mem + items_off);
    s->access.components = items;
    s->access.resources = items + nc;
    s->access.events = items + nc + nr;
    ecs_access_build(d, &s->access);
    const char **names = (const char **)(void *)(mem + names_off);
    s->before = names;
    s->before_count = d->before_count;
    s->after = names + d->before_count;
    s->after_count = d->after_count;
    char *cursor = (char *)(mem + strings_off);
    if (d->name != NULL)
        s->name = copy_str(&cursor, d->name);
    for (u32 i = 0; i < d->before_count; ++i)
        s->before[i] = copy_str(&cursor, d->before[i]);
    for (u32 i = 0; i < d->after_count; ++i)
        s->after[i] = copy_str(&cursor, d->after[i]);
    return s;
}

static void destroy_queries(de_world *w, ecs_system *s) {
    for (u32 i = 0; i < s->query_count; ++i) {
        de_query_destroy(w, s->queries[i]);
        s->queries[i].id = 0;
    }
}

/* Takes the schedule's graph down and marks it for a rebuild. */
static void schedule_invalidate(ecs_schedule_state *sc) {
    if (sc->graph != NULL) {
        de_task_graph_destroy(sc->graph);
        sc->graph = NULL;
    }
    sc->dirty = true;
}

static void update_main_thread_flag(ecs_schedule_state *sc) {
    sc->has_main_thread = false;
    for (u32 i = 0; i < sc->systems.count; ++i)
        if (DE_ARRAY_AT(&sc->systems, ecs_system *, i)->flags & DE_SYSTEM_MAIN_THREAD)
            sc->has_main_thread = true;
}

de_system de_system_add(de_world *w, const de_system_desc *desc) {
    const de_system invalid = {0};
    if (w == NULL || desc == NULL) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_system_add: world and desc are required");
        return invalid;
    }
    ECS_ASSERT_STRUCTURAL(w);
    if (validate(w, desc) != DE_OK)
        return invalid;
    ecs_system_registry *r = ecs_systems_get(w);
    if (r == NULL)
        return invalid;
    DE_ASSERT(!r->running);
    ecs_schedule_state *sc = &r->schedules[desc->schedule];

    /* Room first (slot, free list, schedule list), so nothing below fails half-way except the
     * system block, its queries and the order check, which are undone. */
    u32 slot;
    bool new_slot = false;
    if (r->free_slots.count > 0) {
        slot = DE_ARRAY_LAST(&r->free_slots, u32);
    } else {
        if (r->slots.count >= ECS_SYSTEM_MAX_SLOTS) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_system_add: too many systems");
            return invalid;
        }
        if (de_array_reserve(&r->slots, r->slots.count + 1u) != DE_OK ||
            de_array_reserve(&r->free_slots, r->slots.capacity) != DE_OK) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_system_add: out of memory");
            return invalid;
        }
        slot = r->slots.count;
        new_slot = true;
    }
    if (de_array_reserve(&sc->systems, sc->systems.count + 1u) != DE_OK) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_system_add: out of memory");
        return invalid;
    }

    ecs_system *s = system_build(w, desc);
    if (s == NULL) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_system_add: out of memory");
        return invalid;
    }
    for (u32 i = 0; i < desc->query_count; ++i) {
        s->queries[i] = de_query_create(w, &desc->queries[i]);
        if (!DE_HANDLE_VALID(s->queries[i])) {
            const de_error *e = de_error_last();
            const de_result code = e != NULL ? e->result : DE_ERR_INVALID_ARGUMENT;
            DE_FAIL_WRAP(code, "de_system_add: system '%s': query %u", ecs_system_name(s), i);
            destroy_queries(w, s);
            system_free(w, s);
            return invalid;
        }
    }

    s->seq = r->next_seq;
    de_result pr = de_array_push(&sc->systems, &s); /* reserved above */
    DE_ASSERT(pr == DE_OK);
    (void)pr;
    /* The order check now, so a cycle is reported by the add that closes it. */
    const de_result order = ecs_schedule_build_order(w, sc, false, "de_system_add");
    if (order != DE_OK) {
        de_array_pop(&sc->systems, NULL);
        schedule_invalidate(sc);
        destroy_queries(w, s);
        system_free(w, s);
        return invalid;
    }

    if (new_slot) {
        const ecs_system_slot sl = {s, 1u, 0u};
        pr = de_array_push(&r->slots, &sl); /* reserved above */
        DE_ASSERT(pr == DE_OK);
    } else {
        de_array_pop(&r->free_slots, NULL);
        DE_ARRAY_AT(&r->slots, ecs_system_slot, slot).sys = s;
    }
    ++r->next_seq;
    s->handle = handle_make(slot, DE_ARRAY_AT(&r->slots, ecs_system_slot, slot).generation);
    schedule_invalidate(sc);
    update_main_thread_flag(sc);

    if ((desc->flags & DE_SYSTEM_DETERMINISTIC) && desc->schedule != DE_SCHEDULE_FIXED_UPDATE &&
        desc->schedule != DE_SCHEDULE_STARTUP)
        SYS_WARN("de_system_add: deterministic system '%s' is not in FIXED_UPDATE or STARTUP "
                 "(SPEC §10.2: deterministic simulation runs in FixedUpdate)",
                 ecs_system_name(s));
    return s->handle;
}

/* ---- Remove and enable ---------------------------------------------------------------------- */

void de_system_remove(de_world *w, de_system h) {
    ecs_system *s = ecs_system_get(w, h);
    if (s == NULL)
        return;
    ECS_ASSERT_STRUCTURAL(w);
    ecs_system_registry *r = w->systems;
    DE_ASSERT(!r->running);
    ecs_schedule_state *sc = &r->schedules[s->schedule];
    for (u32 i = 0; i < sc->systems.count; ++i) {
        if (DE_ARRAY_AT(&sc->systems, ecs_system *, i) == s) {
            de_array_remove_ordered(&sc->systems, i); /* keeps registration order */
            break;
        }
    }
    schedule_invalidate(sc);
    update_main_thread_flag(sc);

    const u32 slot = (h.id & ECS_SYSTEM_INDEX_MASK) - 1u;
    ecs_system_slot *sl = &DE_ARRAY_AT(&r->slots, ecs_system_slot, slot);
    destroy_queries(w, s);
    system_free(w, s);
    sl->sys = NULL;
    if (sl->generation == ECS_SYSTEM_GEN_MAX)
        return; /* retired */
    ++sl->generation;
    const de_result pr = de_array_push(&r->free_slots, &slot); /* capacity >= slots */
    DE_ASSERT(pr == DE_OK);
    (void)pr;
}

void de_system_set_enabled(de_world *w, de_system h, bool enabled) {
    ecs_system *s = ecs_system_get(w, h);
    if (s != NULL)
        s->enabled = enabled; /* read by the next run's pre-pass only */
}

bool de_system_enabled(const de_world *w, de_system h) {
    const ecs_system *s = ecs_system_get(w, h);
    return s != NULL && s->enabled;
}

/* ---- Current system (per thread) ------------------------------------------------------------ */

static DE_THREAD_LOCAL const ecs_system *ecs_tls_system;

void ecs_system_scope_enter(const ecs_system *s, ecs_system_scope *sc) {
    sc->prev = ecs_tls_system;
    ecs_tls_system = s;
    ecs_debug_scope_enter(s, sc);
}

void ecs_system_scope_leave(const ecs_system *s, ecs_system_scope *sc) {
    ecs_debug_scope_leave(s, sc);
    ecs_tls_system = sc->prev;
}

const ecs_system *ecs_system_current(const de_world *w) {
    const ecs_system *s = ecs_tls_system;
    return s != NULL && s->w == w ? s : NULL;
}

/* ---- DENG-141: world-free "inside a system" query (in_schedule_internal.h) ----------------- */
bool de__ecs_in_schedule(void) {
    return ecs_tls_system != NULL;
}
/* ---- end DENG-141 --------------------------------------------------------------------------- */

/* ---- DENG-84: the current system's schedule (in_schedule_internal.h) ---------------------- */
int de__ecs_current_schedule(void) {
    const ecs_system *s = ecs_tls_system;
    return s != NULL ? (int)s->schedule : -1;
}
/* ---- end DENG-84 ---------------------------------------------------------------------------- */

u32 ecs_system_stamp_tick(const de_world *w) {
    const ecs_system *s = ecs_system_current(w);
    return s != NULL ? s->this_run : w->change_tick;
}

de_commands *ecs_system_range_commands(const ecs_system *s, u32 range) {
    DE_ASSERT(range < 0xFFFFFFFFu);
    de_commands *c = ecs_cmd_buffer_acquire(s->w, s->order, range + 1u);
    if (c == NULL) {
        /* Out of memory, or more than ECS_CMD_MAX_SLOTS buffers before one apply (a par_each
         * with that many ranges): which ranges lose their buffer depends on timing, so this is
         * an error, not a quiet drop. */
        DE_ERROR(de_log_register_category("ecs", DE_LOG_INFO),
                 "system '%s': no command buffer for par_each range %u (%u buffers at most "
                 "between applies; use a larger grain); its commands are lost",
                 ecs_system_name(s), range, ECS_CMD_MAX_SLOTS);
    }
    return c;
}

/* ---- Stats ---------------------------------------------------------------------------------- */

de_result de_system_get_stats(const de_world *w, de_system h, de_task_node_stats *out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_system_get_stats: out is NULL");
    memset(out, 0, sizeof *out);
    out->last_worker = DE_TASK_WORKER_NONE;
    const ecs_system *s = ecs_system_get(w, h);
    if (s == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_system_get_stats: not a system of the world");
    const ecs_schedule_state *sc = &w->systems->schedules[s->schedule];
    if (sc->graph != NULL && !sc->dirty)
        return de_task_graph_node_stats(sc->graph, s->node, out); /* UNSUPPORTED in Release */
#if defined(DE_DEBUG) || defined(DE_DEV)
    /* No graph: never run yet, or no task system (schedule.c measures the serial runs). */
    out->last_ns = s->serial_last_ns;
    out->runs = s->serial_runs;
    out->avg_ns = s->serial_runs > 0 ? s->serial_total_ns / s->serial_runs : 0;
    out->last_worker = s->serial_last_worker;
    return DE_OK;
#else
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_system_get_stats: not measured in this build type");
#endif
}
