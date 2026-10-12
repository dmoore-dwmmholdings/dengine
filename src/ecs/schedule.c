/* schedule.c: schedule order, the compiled task graph per schedule and de_world_run_schedule
 * (DENG-350, SPEC §7.3, §8, §10.2, §5.7 rule 5).
 *
 * Order of a schedule (ecs_schedule_build_order), with its systems numbered in registration
 * order:
 *   1. Explicit edges from before / after names. A cycle among them is an error that names the
 *      systems. Unknown names are ignored (with a warning when the graph is built).
 *   2. Conflict edges: for every pair i < j (ascending i, then j) whose access sets conflict and
 *      that the edges so far do not order either way, the edge i -> j (registration order). The
 *      reachability of the edges so far is kept as one bitset row per system; an edge added only
 *      between unordered systems can never close a cycle, so step 2 never fails.
 *   3. The run order: Kahn's algorithm, always taking the ready system registered first.
 * Everything depends only on the systems, their access sets and their registration order, never
 * on the worker count, so ticks, command keys and apply order are the same on every machine.
 *
 * The graph (one node per system, in registration order, and one dependency per edge) is built
 * by the first run after an add or remove and reused until the next one. A run sets each
 * system's tick, change windows and command buffer single-threaded in run order, runs the graph
 * (or the systems serially in run order without a task system), and applies the commands. */
#include "system_internal.h"

#include "change_internal.h"
#include "commands_internal.h"

#include <dengine/de_platform.h>

#include <stdio.h>

#if ECS_DEBUG_CHECKS
static de_log_category sched_log_category(void) {
    return de_log_register_category("ecs", DE_LOG_INFO);
}
#define SCHED_WARN(...) DE_WARN(sched_log_category(), __VA_ARGS__)
#else
#define SCHED_WARN(...) ((void)0)
#endif

static const char *const k_schedule_names[DE_SCHEDULE_COUNT_] = {
    "STARTUP", "FIRST", "PRE_UPDATE", "FIXED_UPDATE", "UPDATE", "POST_UPDATE", "EXTRACT", "LAST"};

static ecs_system *sched_system(const ecs_schedule_state *sc, u32 i) {
    return *(ecs_system *const *)de_array_at(&sc->systems, i);
}

void ecs_schedule_free(ecs_schedule_state *sc) {
    if (sc->graph != NULL)
        de_task_graph_destroy(sc->graph);
    sc->graph = NULL;
    de_array_free(&sc->systems);
    de_array_free(&sc->order);
    de_array_free(&sc->edges);
}

/* ---- Order ---------------------------------------------------------------------------------- */

/* Scratch of one order build: CSR successor lists, in-degrees, an order and reachability. */
typedef struct order_scratch {
    u32 *off;   /* n + 1 */
    u32 *adj;   /* edge capacity */
    u32 *indeg; /* n */
    u32 *order; /* n */
    u32 *pos;   /* n (cycle walk) */
    u32 *path;  /* n (cycle walk) */
    u64 *reach; /* n * words: row a has bit b when a reaches b */
    u32 words;
} order_scratch;

#define ORDER_DONE 0xFFFFFFFFu

/* Topological order of `n` systems with edges `e[0..ne)`, always taking the ready system with the
 * lowest index. Returns how many were placed (n unless there is a cycle). */
static u32 topo_sort(u32 n, const ecs_sched_edge *e, u32 ne, order_scratch *s) {
    memset(s->off, 0, (size_t)(n + 1u) * sizeof(u32));
    memset(s->indeg, 0, (size_t)n * sizeof(u32));
    for (u32 k = 0; k < ne; ++k) {
        ++s->off[e[k].from + 1u];
        ++s->indeg[e[k].to];
    }
    for (u32 v = 0; v < n; ++v)
        s->off[v + 1u] += s->off[v];
    /* Fill with a moving cursor per node (pos is free here), keeping edge order. */
    for (u32 v = 0; v < n; ++v)
        s->pos[v] = s->off[v];
    for (u32 k = 0; k < ne; ++k)
        s->adj[s->pos[e[k].from]++] = e[k].to;

    u32 placed = 0;
    for (;;) {
        u32 v = ORDER_DONE;
        for (u32 i = 0; i < n; ++i) {
            if (s->indeg[i] == 0) {
                v = i;
                break;
            }
        }
        if (v == ORDER_DONE)
            return placed;
        s->indeg[v] = ORDER_DONE;
        s->order[placed++] = v;
        for (u32 k = s->off[v]; k < s->off[v + 1u]; ++k)
            --s->indeg[s->adj[k]];
    }
}

/* Reports a cycle among the systems topo_sort left (indeg not ORDER_DONE): walks predecessors
 * that were left too (each has one) until a system repeats. */
static de_result report_cycle(const ecs_schedule_state *sc, const ecs_sched_edge *e, u32 ne,
                              order_scratch *s, const char *who) {
    const u32 n = sc->systems.count;
    for (u32 i = 0; i < n; ++i)
        s->pos[i] = ORDER_DONE;
    u32 v = 0;
    while (s->indeg[v] == ORDER_DONE)
        ++v;
    u32 len = 0;
    u32 start = 0;
    for (;;) {
        s->pos[v] = len;
        s->path[len++] = v;
        u32 u = ORDER_DONE;
        for (u32 k = 0; k < ne; ++k) {
            if (e[k].to == v && s->indeg[e[k].from] != ORDER_DONE) {
                u = e[k].from;
                break;
            }
        }
        DE_ASSERT(u != ORDER_DONE);
        if (s->pos[u] != ORDER_DONE) {
            start = s->pos[u];
            break;
        }
        v = u;
    }
    /* path[k + 1] -> path[k]; in edge order: path[start], path[len - 1], ..., path[start]. */
    char msg[DE_ERROR_MESSAGE_MAX - 96];
    size_t at = 0;
    msg[0] = '\0';
    for (u32 k = 0; k <= len - start; ++k) {
        const u32 idx = k == 0 ? s->path[start] : s->path[len - k];
        const char *name = ecs_system_name(sched_system(sc, idx));
        const int wrote = snprintf(msg + at, sizeof msg - at, "%s%s", k == 0 ? "" : " -> ", name);
        if (wrote < 0 || (size_t)wrote >= sizeof msg - at)
            break;
        at += (size_t)wrote;
    }
    return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: before/after cycle: %s", who, msg);
}

static bool reach_has(const order_scratch *s, u32 a, u32 b) {
    return (s->reach[(size_t)a * s->words + (b >> 6)] >> (b & 63u)) & 1u;
}

/* Records the edge i -> j in the reachability rows: every system that reaches i (and i itself)
 * now reaches j and everything j reaches. */
static void reach_add_edge(order_scratch *s, u32 n, u32 i, u32 j) {
    const u64 *rj = s->reach + (size_t)j * s->words;
    for (u32 a = 0; a < n; ++a) {
        if (a != i && !reach_has(s, a, i))
            continue;
        u64 *ra = s->reach + (size_t)a * s->words;
        for (u32 k = 0; k < s->words; ++k)
            ra[k] |= rj[k];
        ra[j >> 6] |= (u64)1u << (j & 63u);
    }
}

/* Adds the explicit edges of system i (before / after names) to sc->edges. */
static de_result add_named_edges(ecs_schedule_state *sc, u32 i, bool warn) {
    const ecs_system *s = sched_system(sc, i);
    const u32 n = sc->systems.count;
    for (u32 pass = 0; pass < 2u; ++pass) {
        const char *const *names = pass == 0 ? s->before : s->after;
        const u32 count = pass == 0 ? s->before_count : s->after_count;
        for (u32 b = 0; b < count; ++b) {
            bool found = false;
            for (u32 k = 0; k < n; ++k) {
                const ecs_system *t = sched_system(sc, k);
                if (k == i || t->name == NULL || strcmp(t->name, names[b]) != 0)
                    continue;
                found = true;
                const ecs_sched_edge edge =
                    pass == 0 ? (ecs_sched_edge){i, k} : (ecs_sched_edge){k, i};
                if (de_array_push(&sc->edges, &edge) != DE_OK)
                    return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "schedule order: out of memory");
            }
            if (!found && warn)
                SCHED_WARN("system '%s': %s '%s' names no system of schedule %s; ignored",
                           ecs_system_name(s), pass == 0 ? "before" : "after", names[b],
                           k_schedule_names[s->schedule]);
        }
    }
    return DE_OK;
}

de_result ecs_schedule_build_order(de_world *w, ecs_schedule_state *sc, bool warn,
                                   const char *who) {
    const u32 n = sc->systems.count;
    de_array_clear(&sc->edges);
    if (de_array_resize(&sc->order, n) != DE_OK)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "%s: out of memory", who);
    if (n == 0)
        return DE_OK;

    de_result r = DE_OK;
    for (u32 i = 0; i < n && r == DE_OK; ++i)
        r = add_named_edges(sc, i, warn);
    if (r != DE_OK)
        return r;

    /* Scratch: at most n * (n - 1) / 2 conflict edges are added to the explicit ones. */
    const u32 words = (n + 63u) / 64u;
    const u64 max_edges = (u64)sc->edges.count + (u64)n * (n - 1u) / 2u;
    const size_t u32s = (size_t)(n + 1u) + (size_t)max_edges + 4u * (size_t)n;
    const size_t size = (size_t)n * words * sizeof(u64) + u32s * sizeof(u32);
    u8 *mem = (u8 *)DE_ALLOC(w->a, size, 8u, DE_MEM_ECS);
    if (mem == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "%s: out of memory", who);
    order_scratch s;
    s.words = words;
    s.reach = (u64 *)(void *)mem;
    u32 *u = (u32 *)(void *)(mem + (size_t)n * words * sizeof(u64));
    s.off = u;
    s.adj = s.off + n + 1u;
    s.indeg = s.adj + max_edges;
    s.order = s.indeg + n;
    s.pos = s.order + n;
    s.path = s.pos + n;
    memset(s.reach, 0, (size_t)n * words * sizeof(u64));

    /* 1. Explicit edges: a cycle is an error. */
    const u32 explicit_count = sc->edges.count;
    const ecs_sched_edge *e = DE_ARRAY_DATA(&sc->edges, ecs_sched_edge);
    if (topo_sort(n, e, explicit_count, &s) != n) {
        r = report_cycle(sc, e, explicit_count, &s, who);
        DE_FREE(w->a, mem, size);
        return r;
    }
    /* Reachability of the explicit DAG, in reverse topological order. */
    for (u32 k = n; k-- > 0;) {
        const u32 v = s.order[k];
        for (u32 a = s.off[v]; a < s.off[v + 1u]; ++a) {
            const u32 t = s.adj[a];
            u64 *rv = s.reach + (size_t)v * words;
            const u64 *rt = s.reach + (size_t)t * words;
            for (u32 x = 0; x < words; ++x)
                rv[x] |= rt[x];
            rv[t >> 6] |= (u64)1u << (t & 63u);
        }
    }

    /* 2. Conflict edges between systems the edges so far leave unordered. */
    for (u32 i = 0; i < n && r == DE_OK; ++i) {
        const ecs_system *si = sched_system(sc, i);
        for (u32 j = i + 1u; j < n; ++j) {
            if (reach_has(&s, i, j) || reach_has(&s, j, i))
                continue;
            if (!ecs_access_conflict(&si->access, &sched_system(sc, j)->access))
                continue;
            const ecs_sched_edge edge = {i, j};
            if (de_array_push(&sc->edges, &edge) != DE_OK) {
                r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "%s: out of memory", who);
                break;
            }
            reach_add_edge(&s, n, i, j);
        }
    }

    /* 3. The run order. */
    if (r == DE_OK) {
        e = DE_ARRAY_DATA(&sc->edges, ecs_sched_edge);
        const u32 placed = topo_sort(n, e, sc->edges.count, &s);
        DE_ASSERT(placed == n); /* step 2 never closes a cycle */
        (void)placed;
        u32 *order = DE_ARRAY_DATA(&sc->order, u32);
        for (u32 k = 0; k < n; ++k) {
            order[k] = s.order[k];
            sched_system(sc, s.order[k])->order = k;
        }
    }
    DE_FREE(w->a, mem, size);
    return r;
}

/* ---- Graph ---------------------------------------------------------------------------------- */

/* Runs one system (a graph node, or a step of a serial run). */
static void system_execute(ecs_system *s) {
    if (!s->run_now)
        return;
    de_world *w = s->w;
    DE_ASSERT(!(s->flags & DE_SYSTEM_MAIN_THREAD) || w->tasks == NULL ||
              de_task_worker_index() == 0);
    ecs_system_scope scope;
    ecs_system_scope_enter(s, &scope);
    const bool exclusive = (s->flags & DE_SYSTEM_EXCLUSIVE) != 0;
    /* Nothing else of the schedule runs beside an exclusive system (it conflicts with every
     * system), so it may use the immediate structural API. */
    if (exclusive)
        w->in_schedule = false;
    s->fn(&s->ctx);
    if (exclusive)
        w->in_schedule = true;
    ecs_system_scope_leave(s, &scope);
}

static void system_node_fn(void *user) {
    system_execute((ecs_system *)user);
}

/* Builds the order and (with a task system) the compiled graph of `sc`. */
static de_result schedule_rebuild(de_world *w, ecs_schedule_state *sc) {
    if (sc->graph != NULL) {
        de_task_graph_destroy(sc->graph);
        sc->graph = NULL;
    }
    de_result r = ecs_schedule_build_order(w, sc, true, "de_world_run_schedule");
    if (r != DE_OK)
        return r;
    const u32 n = sc->systems.count;
    for (u32 i = 0; i < n; ++i) {
        ecs_system *s = sched_system(sc, i);
        s->serial_last_ns = s->serial_total_ns = s->serial_runs = 0;
        s->serial_last_worker = DE_TASK_WORKER_NONE;
    }
    if (w->tasks != NULL && n > 0) {
        de_task_graph *g = NULL;
        if ((r = de_task_graph_create(w->tasks, &g)) != DE_OK)
            return DE_FAIL_WRAP(r, "de_world_run_schedule: task graph");
        for (u32 i = 0; i < n; ++i) {
            ecs_system *s = sched_system(sc, i);
            de_task_node_desc nd;
            memset(&nd, 0, sizeof nd);
            nd.name = s->name; /* lives in the system block; the graph goes before it does */
            nd.fn = system_node_fn;
            nd.user = s;
            nd.pool =
                (s->flags & DE_SYSTEM_MAIN_THREAD) ? DE_TASK_POOL_MAIN_THREAD : DE_TASK_POOL_FRAME;
            s->node = de_task_graph_add(g, &nd);
        }
        const ecs_sched_edge *e = DE_ARRAY_DATA(&sc->edges, ecs_sched_edge);
        for (u32 k = 0; k < sc->edges.count; ++k)
            de_task_graph_depend(g, sched_system(sc, e[k].from)->node,
                                 sched_system(sc, e[k].to)->node);
        if ((r = de_task_graph_compile(g)) != DE_OK) {
            de_task_graph_destroy(g);
            return DE_FAIL_WRAP(r, "de_world_run_schedule: task graph");
        }
        sc->graph = g;
    }
    sc->dirty = false;
    ++sc->builds;
    return DE_OK;
}

/* ---- Run ------------------------------------------------------------------------------------ */

de_result de_world_run_schedule(de_world *w, de_schedule s, f32 dt, f32 alpha) {
    if (w == NULL || (u32)s >= (u32)DE_SCHEDULE_COUNT_)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_world_run_schedule: bad world or schedule %d",
                       (int)s);
    ecs_system_registry *reg = ecs_systems_get(w);
    if (reg == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_world_run_schedule");
    if (reg->running || w->in_schedule)
        return DE_FAIL(DE_ERR_BUSY,
                       "de_world_run_schedule: schedule %s started while a schedule "
                       "runs",
                       k_schedule_names[s]);
    ecs_schedule_state *sc = &reg->schedules[s];
    if (sc->dirty) {
        const de_result r = schedule_rebuild(w, sc);
        if (r != DE_OK)
            return r;
    }
    DE_ASSERT(!sc->has_main_thread || w->tasks == NULL || de_task_worker_index() == 0);

    /* The event frame step: an event is readable in the frame it was sent and the next one. */
    if (s == DE_SCHEDULE_FIRST)
        de_world_update_events(w);

    reg->running = true;
    w->in_schedule = true;

    /* Pre-pass, single-threaded in run order: change tick, windows, command buffer, ctx. The
     * ticks follow the run order, never the moment a worker starts a system, so DE_ADDED /
     * DE_CHANGED results do not depend on scheduling. */
    const u32 n = sc->order.count;
    const u32 *order = DE_ARRAY_DATA(&sc->order, u32);
    for (u32 k = 0; k < n; ++k) {
        ecs_system *sys = sched_system(sc, order[k]);
        sys->run_now = sys->enabled;
        if (!sys->run_now)
            continue;
        const u32 this_run = ecs_change_tick_advance(w); /* may clamp last runs */
        const u32 last_run = sys->has_run ? sys->last_run : this_run - ECS_MAX_CHANGE_AGE;
        for (u32 q = 0; q < sys->query_count; ++q)
            ecs_query_set_change_window(w, sys->queries[q], last_run, this_run);
        sys->this_run = this_run;
        DE_ASSERT(sys->order == k);
        de_system_ctx *ctx = &sys->ctx;
        ctx->world = w;
        ctx->commands = ecs_cmd_buffer_acquire(w, k, 0);
        ctx->queries = sys->queries;
        ctx->dt = dt;
        ctx->alpha = alpha;
        ctx->tick = w->tick;
        ctx->user = sys->user;
    }

    if (sc->graph != NULL) {
        de_task_counter done;
        memset(&done, 0, sizeof done);
        de_task_graph_run(sc->graph, &done);
        de_task_wait(w->tasks, &done);
    } else {
        for (u32 k = 0; k < n; ++k) {
            ecs_system *sys = sched_system(sc, order[k]);
#if defined(DE_DEBUG) || defined(DE_DEV)
            if (!sys->run_now)
                continue;
            const u64 t0 = de_time_now_ns();
            system_execute(sys);
            const u64 ns = de_time_now_ns() - t0;
            sys->serial_last_ns = ns;
            sys->serial_total_ns += ns;
            ++sys->serial_runs;
            sys->serial_last_worker = de_task_worker_index();
#else
            system_execute(sys);
#endif
        }
    }

    for (u32 k = 0; k < n; ++k) {
        ecs_system *sys = sched_system(sc, order[k]);
        if (!sys->run_now)
            continue;
        sys->last_run = sys->this_run;
        sys->has_run = true;
    }

    /* The sync point: main-thread buffer, then (system order, range) order. */
    ecs_cmd_apply_all(w);
    w->in_schedule = false;
    reg->running = false;
    if (s == DE_SCHEDULE_FIXED_UPDATE)
        ++w->tick; /* the simulation tick */
    return DE_OK;
}
