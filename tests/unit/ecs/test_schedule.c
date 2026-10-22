/* test_schedule.c: schedules: the conflict DAG, before / after, cycles, graph reuse, the command
 * sync point, deterministic spawning for every worker count, the event frame step in FIRST and
 * per-system change windows (DENG-350; SPEC §7.3, §8, §10.2, §5.7). Range buffers allocate
 * nothing once warm, in any acquire order (DENG-379).
 *
 * SCHED_SPAWN_TRACE is the test vector of sched.deterministic_spawn_ids_workers: an integer-only
 * hash of the world after each of 60 FixedUpdate ticks of three parallel spawning systems. It
 * must hold on every machine and for every worker count; a change is a determinism bug, never a
 * reason to update it. */
#include "ecs_test_util.h"

#include "ecs/commands_internal.h"
#include "ecs/query_internal.h"
#include "ecs/system_internal.h"
#include "platform/de_atomic.h"

#include <dengine/de_platform.h>

#include <stdio.h>

/** Hash of sched.deterministic_spawn_ids_workers (see the file comment). */
#define SCHED_SPAWN_TRACE 0x1EAA06802F2C4340ull

static de_task_system *make_ts(u32 workers) {
    de_fp_env_init_thread();
    de_task_system_desc d;
    memset(&d, 0, sizeof d);
    d.worker_count = workers;
    d.background_worker_count = 1;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    return ts;
}

static u32 parallel_workers(void) {
    const u32 cores = de_cpu_logical_cores();
    return cores > 3u ? cores - 1u : 2u;
}

static void rebuild_world(ecs_fx *f, de_task_system *ts, u32 chunk_bytes) {
    ecs_fx_destroy_world(f);
    de_world_desc d;
    memset(&d, 0, sizeof d);
    d.tasks = ts;
    d.chunk_bytes = chunk_bytes;
    ecs_fx_create_world(f, &d);
}

static bool error_has(const char *needle) {
    const de_error *e = de_error_last();
    return e != NULL && strstr(e->message, needle) != NULL;
}

static void spin_ns(u64 ns) {
    const u64 t0 = de_time_now_ns();
    while (de_time_now_ns() - t0 < ns) {
    }
}

static de_system add_system(de_world *w, const char *name, de_schedule s, de_system_fn fn,
                            void *user, const de_query_desc *q, u32 nq) {
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.schedule = s;
    d.fn = fn;
    d.user = user;
    d.queries = q;
    d.query_count = nq;
    return de_system_add(w, &d);
}

/* ---- Instrumented systems ------------------------------------------------------------------- */

/* Shared by the systems of one test: an in-flight flag per conflict group, a sequence counter,
 * and per system the sequence numbers of its last start and end. */
typedef struct probe_shared {
    de_atomic_u32 in_flight;
    de_atomic_u32 overlaps;
    de_atomic_u32 seq;
    de_atomic_u32 arrived; /* readers_parallel */
    de_atomic_u32 met;     /* readers_parallel: runs where both readers were in at once */
} probe_shared;

typedef struct probe {
    probe_shared *sh;
    u32 start, end;
    u64 spin;
    bool exclusive_group; /* uses in_flight */
} probe;

static void probe_fn(de_system_ctx *ctx) {
    probe *p = (probe *)ctx->user;
    p->start = de_atomic_fetch_add_u32(&p->sh->seq, 1u, DE_ATOMIC_RELAXED);
    if (p->exclusive_group && de_atomic_exchange_u32(&p->sh->in_flight, 1u, DE_ATOMIC_ACQ_REL))
        de_atomic_fetch_add_u32(&p->sh->overlaps, 1u, DE_ATOMIC_RELAXED);
    spin_ns(p->spin);
    if (p->exclusive_group)
        de_atomic_store_u32(&p->sh->in_flight, 0u, DE_ATOMIC_RELEASE);
    p->end = de_atomic_fetch_add_u32(&p->sh->seq, 1u, DE_ATOMIC_RELAXED);
}

/* Arrives, then waits up to 2 ms for the other reader. */
static void meet_fn(de_system_ctx *ctx) {
    probe *p = (probe *)ctx->user;
    const u32 n = de_atomic_fetch_add_u32(&p->sh->arrived, 1u, DE_ATOMIC_ACQ_REL) + 1u;
    if (n == 2u) {
        de_atomic_fetch_add_u32(&p->sh->met, 1u, DE_ATOMIC_RELAXED);
        return;
    }
    const u64 t0 = de_time_now_ns();
    while (de_atomic_load_u32(&p->sh->arrived, DE_ATOMIC_ACQUIRE) < 2u &&
           de_time_now_ns() - t0 < 2000000u) {
    }
}

/* ---- sched.conflict_serialized -------------------------------------------------------------- */

TEST_F(sched, conflict_serialized, ecs_fx) {
    de_task_system *ts = make_ts(parallel_workers());
    rebuild_world(f, ts, 0);
    probe_shared sh;
    memset(&sh, 0, sizeof sh);
    probe pa = {&sh, 0, 0, 30000u, true}, pb = {&sh, 0, 0, 30000u, true};
    probe pc = {&sh, 0, 0, 30000u, false};
    const de_query_term wpos = {f->pos, DE_WRITE};
    const de_query_term rvel = {f->vel, DE_READ};
    const de_query_desc qw = {&wpos, 1, NULL, 0}, qr = {&rvel, 1, NULL, 0};
    ASSERT_TRUE(DE_HANDLE_VALID(add_system(f->w, "a", DE_SCHEDULE_UPDATE, probe_fn, &pa, &qw, 1)));
    ASSERT_TRUE(DE_HANDLE_VALID(add_system(f->w, "c", DE_SCHEDULE_UPDATE, probe_fn, &pc, &qr, 1)));
    ASSERT_TRUE(DE_HANDLE_VALID(add_system(f->w, "b", DE_SCHEDULE_UPDATE, probe_fn, &pb, &qw, 1)));
    u32 order_ok = 0;
    for (u32 run = 0; run < 100u; ++run) {
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
        if (pa.end < pb.start)
            ++order_ok; /* registration order: a before b */
    }
    EXPECT_EQ(de_atomic_load_u32(&sh.overlaps, DE_ATOMIC_RELAXED), 0u);
    EXPECT_EQ(order_ok, 100u);
    /* The DAG: one conflict edge a -> b (c conflicts with neither). */
    const ecs_schedule_state *sc = &f->w->systems->schedules[DE_SCHEDULE_UPDATE];
    ASSERT_EQ(sc->edges.count, 1u);
    const ecs_sched_edge *e0 = (const ecs_sched_edge *)de_array_at(&sc->edges, 0);
    EXPECT_EQ(e0->from, 0u);
    EXPECT_EQ(e0->to, 2u);
    ecs_fx_destroy_world(f);
    de_task_system_destroy(ts);
}

/* ---- sched.readers_parallel ----------------------------------------------------------------- */

TEST_F(sched, readers_parallel, ecs_fx) {
    de_task_system *ts = make_ts(parallel_workers());
    ASSERT_GE(de_task_worker_count(ts), 2u);
    rebuild_world(f, ts, 0);
    probe_shared sh;
    memset(&sh, 0, sizeof sh);
    probe pa = {&sh, 0, 0, 0, false}, pb = {&sh, 0, 0, 0, false};
    const de_query_term rpos = {f->pos, DE_READ};
    const de_query_filter ch = {f->vel, DE_CHANGED}; /* a change filter is a read too */
    const de_query_desc q1 = {&rpos, 1, NULL, 0}, q2 = {&rpos, 1, &ch, 1};
    ASSERT_TRUE(DE_HANDLE_VALID(add_system(f->w, "r1", DE_SCHEDULE_UPDATE, meet_fn, &pa, &q1, 1)));
    ASSERT_TRUE(DE_HANDLE_VALID(add_system(f->w, "r2", DE_SCHEDULE_UPDATE, meet_fn, &pb, &q2, 1)));
    for (u32 run = 0; run < 100u; ++run) {
        de_atomic_store_u32(&sh.arrived, 0u, DE_ATOMIC_RELAXED);
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    }
    EXPECT_EQ(f->w->systems->schedules[DE_SCHEDULE_UPDATE].edges.count, 0u);
    EXPECT_GE(de_atomic_load_u32(&sh.met, DE_ATOMIC_RELAXED), 1u);
    printf("readers met in %u of 100 runs\n", de_atomic_load_u32(&sh.met, DE_ATOMIC_RELAXED));
    ecs_fx_destroy_world(f);
    de_task_system_destroy(ts);
}

/* ---- sched.before_after --------------------------------------------------------------------- */

typedef struct order_log {
    char seq[16];
    u32 n;
} order_log;

typedef struct order_sys {
    order_log *log;
    char tag;
} order_sys;

static void log_fn(de_system_ctx *ctx) {
    order_sys *o = (order_sys *)ctx->user;
    if (o->log->n < sizeof o->log->seq - 1u)
        o->log->seq[o->log->n++] = o->tag;
}

static de_system add_ordered(de_world *w, const char *name, order_sys *o, const de_query_desc *q,
                             const char *const *before, u32 nb, const char *const *after, u32 na) {
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.schedule = DE_SCHEDULE_UPDATE;
    d.fn = log_fn;
    d.user = o;
    d.queries = q;
    d.query_count = q != NULL ? 1u : 0u;
    d.before = before;
    d.before_count = nb;
    d.after = after;
    d.after_count = na;
    return de_system_add(w, &d);
}

/* Runs UPDATE `runs` times and checks that every run logged `expect`. */
static void expect_order(ecs_fx *f, order_log *log, const char *expect, u32 runs) {
    for (u32 run = 0; run < runs; ++run) {
        memset(log, 0, sizeof *log);
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
        EXPECT_STREQ(log->seq, expect);
    }
}

TEST_F(sched, before_after, ecs_fx) {
    order_log log;
    order_sys sa = {&log, 'A'}, sb = {&log, 'B'}, sc = {&log, 'C'}, sd = {&log, 'D'};
    const de_query_term wpos = {f->pos, DE_WRITE};
    const de_query_desc qw = {&wpos, 1, NULL, 0};
    for (u32 pass = 0; pass < 2u; ++pass) {
        de_task_system *ts = pass == 0 ? NULL : make_ts(parallel_workers());
        rebuild_world(f, ts, 0);
        /* All write pos: by default registration order A, B, C. */
        ASSERT_TRUE(DE_HANDLE_VALID(add_ordered(f->w, "A", &sa, &qw, NULL, 0, NULL, 0)));
        ASSERT_TRUE(DE_HANDLE_VALID(add_ordered(f->w, "B", &sb, &qw, NULL, 0, NULL, 0)));
        expect_order(f, &log, "AB", 5);
        /* C before A: overrides registration order; B still after A. */
        const char *const before_a[] = {"A"};
        ASSERT_TRUE(DE_HANDLE_VALID(add_ordered(f->w, "C", &sc, &qw, before_a, 1, NULL, 0)));
        expect_order(f, &log, "CAB", 20);
        /* D (no access) after B and before C; an unknown name is ignored (a warning). The
         * explicit edges B -> D -> C -> A now order B before A, against registration order. */
        const char *const after_b[] = {"B", "nobody"};
        const char *const before_c[] = {"C"};
        ASSERT_TRUE(DE_HANDLE_VALID(add_ordered(f->w, "D", &sd, NULL, before_c, 1, after_b, 2)));
        expect_order(f, &log, "BDCA", 20);
        ecs_fx_destroy_world(f);
        if (ts != NULL)
            de_task_system_destroy(ts);
    }
    ecs_fx_create_world(f, NULL);
}

/* ---- sched.cycle_error ---------------------------------------------------------------------- */

TEST_F(sched, cycle_error, ecs_fx) {
    order_log log;
    memset(&log, 0, sizeof log);
    order_sys o = {&log, 'x'};
    const char *const before_b[] = {"B"};
    const char *const before_a[] = {"A"};
    ASSERT_TRUE(DE_HANDLE_VALID(add_ordered(f->w, "A", &o, NULL, before_b, 1, NULL, 0)));
    /* B before A closes A -> B -> A: refused, naming both. */
    EXPECT_FALSE(DE_HANDLE_VALID(add_ordered(f->w, "B", &o, NULL, before_a, 1, NULL, 0)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_has("A -> B -> A") || error_has("B -> A -> B"));
    printf("%s\n", de_error_last()->message);
    /* B after A agrees with A before B: accepted. */
    const char *const after_a[] = {"A"};
    ASSERT_TRUE(DE_HANDLE_VALID(add_ordered(f->w, "B", &o, NULL, NULL, 0, after_a, 1)));

    /* A three-system cycle P -> Q -> R -> P is reported by the add that closes it. */
    const char *const before_q[] = {"Q"};
    const char *const before_r[] = {"R"};
    const char *const before_p[] = {"P"};
    ASSERT_TRUE(DE_HANDLE_VALID(add_ordered(f->w, "P", &o, NULL, before_q, 1, NULL, 0)));
    ASSERT_TRUE(DE_HANDLE_VALID(add_ordered(f->w, "Q", &o, NULL, before_r, 1, NULL, 0)));
    EXPECT_FALSE(DE_HANDLE_VALID(add_ordered(f->w, "R", &o, NULL, before_p, 1, NULL, 0)));
    EXPECT_TRUE(error_has("P -> Q -> R -> P") || error_has("Q -> R -> P -> Q") ||
                error_has("R -> P -> Q -> R"));
    printf("%s\n", de_error_last()->message);

    /* The refused systems left nothing behind: their names are free, the schedule runs. */
    ASSERT_TRUE(DE_HANDLE_VALID(add_ordered(f->w, "R", &o, NULL, NULL, 0, NULL, 0)));
    /* A system naming itself is ignored. */
    const char *const before_self[] = {"S"};
    ASSERT_TRUE(DE_HANDLE_VALID(add_ordered(f->w, "S", &o, NULL, before_self, 1, NULL, 0)));
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(log.n, 6u); /* A, B, P, Q, R, S */
}

/* ---- sched.graph_reuse_no_alloc ------------------------------------------------------------- */

typedef struct churn_ctx {
    de_component tag;
    de_component pos;
} churn_ctx;

/* Per row: write pos, and toggle the enemy tag through the range's command buffer. */
static void churn_chunk(const de_query_chunk *ch, void *user) {
    const churn_ctx *c = (const churn_ctx *)user;
    TPos *p = (TPos *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i) {
        p[i].v.x += 1.0f;
        if ((de_entity_index(ch->entities[i]) & 3u) == 0)
            (void)de_cmd_add(ch->commands, ch->entities[i], c->tag);
    }
}

static void unchurn_chunk(const de_query_chunk *ch, void *user) {
    const churn_ctx *c = (const churn_ctx *)user;
    for (u32 i = 0; i < ch->count; ++i)
        de_cmd_remove(ch->commands, ch->entities[i], c->tag);
}

static void churn_fn(de_system_ctx *ctx) {
    de_query_par_each(ctx->world, ctx->queries[0], 64, churn_chunk, ctx->user);
}

static void unchurn_fn(de_system_ctx *ctx) {
    de_query_par_each(ctx->world, ctx->queries[0], 64, unchurn_chunk, ctx->user);
    const de_entity e = de_cmd_spawn(ctx->commands);
    de_cmd_despawn(ctx->commands, e); /* a cancelled spawn: no structural change */
}

static void light_fn(de_system_ctx *ctx) {
    (void)ctx;
}

/* Warm-up barrier (as in graph.rerun_no_alloc): one task per frame worker, each waits until all
 * have started, then submits a child, so every worker's task record pool exists before the
 * measured runs. Otherwise a worker that happened to submit nothing during the warm-up runs
 * allocates its first record block later (more likely on a loaded machine). */
typedef struct warm_ctx {
    de_task_system *ts;
    u32 n;
    de_atomic_u32 started;
    de_atomic_u32 timeouts;
    de_task_counter children;
} warm_ctx;

static void warm_noop(void *user) {
    (void)user;
}

static void warm_fn(void *user) {
    warm_ctx *w = (warm_ctx *)user;
    de_atomic_fetch_add_u32(&w->started, 1, DE_ATOMIC_ACQ_REL);
    const u64 end = de_time_now_ns() + 10ull * 1000 * 1000 * 1000;
    while (de_atomic_load_u32(&w->started, DE_ATOMIC_ACQUIRE) < w->n) {
        if (de_time_now_ns() > end) {
            de_atomic_fetch_add_u32(&w->timeouts, 1, DE_ATOMIC_RELAXED);
            break;
        }
        de_cpu_pause();
    }
    de_task_submit(w->ts, DE_TASK_POOL_FRAME, warm_noop, NULL, &w->children);
}

static void warm_all_workers(de_task_system *ts) {
    static warm_ctx w;
    memset(&w, 0, sizeof w);
    w.ts = ts;
    w.n = de_task_worker_count(ts);
    de_task_counter c;
    memset(&c, 0, sizeof c);
    for (u32 i = 0; i < w.n; ++i)
        de_task_submit(ts, DE_TASK_POOL_FRAME, warm_fn, &w, &c);
    de_task_wait(ts, &c);
    de_task_wait(ts, &w.children);
    EXPECT_EQ(de_atomic_load_u32(&w.timeouts, DE_ATOMIC_RELAXED), 0u);
}

TEST_F(sched, graph_reuse_no_alloc, ecs_fx) {
    de_tracking_allocator task_track;
    const de_allocator task_alloc = de_tracking_allocator_init(&task_track, NULL, DE_MEM_GENERAL);
    de_fp_env_init_thread();
    de_task_system_desc td;
    memset(&td, 0, sizeof td);
    td.worker_count = parallel_workers();
    td.background_worker_count = 1;
    td.allocator = &task_alloc;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&td, &ts), DE_OK);
    rebuild_world(f, ts, 2048);

    for (u32 i = 0; i < 2000u; ++i) {
        const de_entity e = de_entity_spawn(f->w);
        de_entity_add(f->w, e, f->pos);
        if (i % 3u == 0)
            de_entity_add(f->w, e, f->vel);
    }
    churn_ctx cc = {f->enemy, f->pos};
    const de_query_term wpos = {f->pos, DE_WRITE};
    const de_query_filter with_enemy = {f->enemy, DE_WITH};
    const de_query_term rvel = {f->vel, DE_READ};
    const de_query_term rhealth = {f->health, DE_READ};
    const de_query_desc q_churn = {&wpos, 1, NULL, 0};
    const de_query_desc q_unchurn = {NULL, 0, &with_enemy, 1};
    const de_query_desc q_vel = {&rvel, 1, NULL, 0};
    const de_query_desc q_health = {&rhealth, 1, NULL, 0};
    ASSERT_TRUE(DE_HANDLE_VALID(
        add_system(f->w, "unchurn", DE_SCHEDULE_UPDATE, unchurn_fn, &cc, &q_unchurn, 1)));
    ASSERT_TRUE(
        DE_HANDLE_VALID(add_system(f->w, "churn", DE_SCHEDULE_UPDATE, churn_fn, &cc, &q_churn, 1)));
    ASSERT_TRUE(
        DE_HANDLE_VALID(add_system(f->w, "light1", DE_SCHEDULE_UPDATE, light_fn, NULL, &q_vel, 1)));
    ASSERT_TRUE(DE_HANDLE_VALID(
        add_system(f->w, "light2", DE_SCHEDULE_UPDATE, light_fn, NULL, &q_health, 1)));
    const ecs_schedule_state *sc = &f->w->systems->schedules[DE_SCHEDULE_UPDATE];

    warm_all_workers(ts);
    for (u32 run = 0; run < 30u; ++run) /* warm-up: graph, buffers, archetypes, task pools */
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    const u64 builds = sc->builds;
    const de_task_graph *graph = sc->graph;
    ASSERT_TRUE(graph != NULL);
    const de_tracking_stats w0 = de_tracking_allocator_stats(&f->track);
    const de_tracking_stats t0 = de_tracking_allocator_stats(&task_track);
    for (u32 run = 0; run < 200u; ++run)
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    const de_tracking_stats w1 = de_tracking_allocator_stats(&f->track);
    const de_tracking_stats t1 = de_tracking_allocator_stats(&task_track);
    EXPECT_EQ(sc->builds, builds);
    EXPECT_TRUE(sc->graph == graph);
    EXPECT_EQ(w1.alloc_count, w0.alloc_count);
    EXPECT_EQ(w1.realloc_count, w0.realloc_count);
    EXPECT_EQ(w1.free_count, w0.free_count);
    EXPECT_EQ(t1.alloc_count, t0.alloc_count);
    EXPECT_EQ(t1.realloc_count, t0.realloc_count);
    EXPECT_EQ(t1.free_count, t0.free_count);
    /* The churn really happened: every 4th index toggled on and off each run. */
    const TPos *p =
        (const TPos *)de_entity_get_const(f->w, (de_entity){((u64)1u << 32) | 7u}, f->pos);
    ASSERT_TRUE(p != NULL);
    EXPECT_FLOAT_EQ(p->v.x, 230.0f, 0.0);

    /* Adding a system rebuilds the graph once. */
    ASSERT_TRUE(
        DE_HANDLE_VALID(add_system(f->w, "light3", DE_SCHEDULE_UPDATE, light_fn, NULL, &q_vel, 1)));
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(sc->builds, builds + 1u);

    ecs_fx_destroy_world(f);
    de_task_system_destroy(ts);
    const de_tracking_stats tend = de_tracking_allocator_stats(&task_track);
    EXPECT_EQ(tend.blocks_live, 0u);
}

/* ---- sched.range_buffers_any_order_no_alloc ------------------------------------------------- */

/* DENG-379. par_each ranges acquire their buffers in whatever order the worker threads reach
 * them, and only some ranges record. graph_reuse_no_alloc failed once on a loaded CI runner: a
 * recording range got a buffer that had no page yet. This test sets the acquire order itself.
 * It acquires the same keys in forward, reverse and shuffled order, with uneven loads: every
 * third key records nothing, the rest record one or two pages. After one apply in each order,
 * no order may allocate. */
#define RB_KEYS 48u

static u32 rb_commands(u32 range) {
    if (range % 3u == 0)
        return 0;                        /* records nothing, like a range without matching rows */
    return range % 3u == 1 ? 40u : 900u; /* one 16 KiB page, or two */
}

/* One cycle: acquire RB_KEYS keys of system order 5 in the order of `perm`, record, apply. */
static void rb_cycle(de_world *w, const u32 *perm, de_entity target, de_component comp) {
    for (u32 i = 0; i < RB_KEYS; ++i) {
        const u32 range = perm[i] + 1u;
        de_commands *c = ecs_cmd_buffer_acquire(w, 5u, range);
        ASSERT_TRUE(c != NULL);
        for (u32 k = 0; k < rb_commands(range); ++k)
            de_cmd_remove(c, target, comp); /* the target lacks it: applies as a no-op */
    }
    ecs_cmd_apply_all(w);
}

TEST_F(sched, range_buffers_any_order_no_alloc, ecs_fx) {
    const de_entity target = de_entity_spawn(f->w);
    ASSERT_TRUE(de_entity_add(f->w, target, f->pos) != NULL);
    u32 fwd[RB_KEYS], rev[RB_KEYS], mix[RB_KEYS];
    for (u32 i = 0; i < RB_KEYS; ++i) {
        fwd[i] = i;
        rev[i] = RB_KEYS - 1u - i;
        mix[i] = i;
    }
    /* Warm-up: one cycle in each of two orders. */
    rb_cycle(f->w, fwd, target, f->vel);
    rb_cycle(f->w, rev, target, f->vel);
    const de_tracking_stats s0 = de_tracking_allocator_stats(&f->track);
    u32 seed = 0x379u;
    for (u32 cycle = 0; cycle < 60u; ++cycle) {
        const u32 *perm = cycle % 3u == 0 ? fwd : cycle % 3u == 1 ? rev : mix;
        if (cycle % 3u == 2) /* a fresh Fisher-Yates shuffle (LCG) every third cycle */
            for (u32 i = RB_KEYS - 1u; i > 0; --i) {
                seed = seed * 1664525u + 1013904223u;
                const u32 j = (seed >> 8) % (i + 1u);
                const u32 t = mix[i];
                mix[i] = mix[j];
                mix[j] = t;
            }
        rb_cycle(f->w, perm, target, f->vel);
    }
    const de_tracking_stats s1 = de_tracking_allocator_stats(&f->track);
    EXPECT_EQ(s1.alloc_count, s0.alloc_count);
    EXPECT_EQ(s1.realloc_count, s0.realloc_count);
    EXPECT_EQ(s1.free_count, s0.free_count);

    /* New keys of system order 6, then 7: order 7 takes over the buffers that order 5 left
     * unused in the apply before. A key whose buffer was taken gets another one, and the apply
     * order stays by key: (4, 1) before (5, 2), whatever the acquire order. */
    for (u32 order = 6u; order <= 7u; ++order) {
        for (u32 i = 0; i < RB_KEYS; ++i)
            ASSERT_TRUE(ecs_cmd_buffer_acquire(f->w, order, i + 1u) != NULL);
        ecs_cmd_apply_all(f->w);
    }
    de_commands *late = ecs_cmd_buffer_acquire(f->w, 5u, 2u);
    de_commands *early = ecs_cmd_buffer_acquire(f->w, 4u, 1u);
    ASSERT_TRUE(late != NULL && early != NULL && late != early);
    EXPECT_TRUE(ecs_cmd_buffer_acquire(f->w, 5u, 2u) == late); /* same key, same buffer */
    const de_entity a = de_cmd_spawn(late);
    const de_entity b = de_cmd_spawn(early);
    ecs_cmd_apply_all(f->w);
    const de_entity ra = de_commands_resolve(f->w, a), rb = de_commands_resolve(f->w, b);
    ASSERT_TRUE(de_entity_alive(f->w, ra) && de_entity_alive(f->w, rb));
    EXPECT_LT(de_entity_index(rb), de_entity_index(ra)); /* (4, 1) spawned first */
}

/* At the slot limit, a new key set takes over the slots of the last apply's keys (DENG-379):
 * two disjoint sets of ECS_CMD_MAX_SLOTS - 1 keys, one after the other, then the first again. */
TEST_F(sched, range_buffers_slot_limit, ecs_fx) {
    const u32 n = ECS_CMD_MAX_SLOTS - 1u; /* every slot but the main buffer's */
    for (u32 pass = 0; pass < 3u; ++pass) {
        const u32 order = pass == 1u ? 2u : 1u;
        for (u32 i = 0; i < n; ++i)
            ASSERT_TRUE(ecs_cmd_buffer_acquire(f->w, order, i + 1u) != NULL);
        ecs_cmd_apply_all(f->w);
        EXPECT_EQ(f->w->cmd->buffers.count, ECS_CMD_MAX_SLOTS);
    }
    /* One more key than slots is still refused. */
    for (u32 i = 0; i < n; ++i)
        ASSERT_TRUE(ecs_cmd_buffer_acquire(f->w, 3u, i + 1u) != NULL);
    EXPECT_TRUE(ecs_cmd_buffer_acquire(f->w, 3u, n + 1u) == NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    ecs_cmd_apply_all(f->w);
}

/* ---- sched.commands_sync_point -------------------------------------------------------------- */

typedef struct sync_ctx {
    de_component pos, vel;
    u32 seen_by_counter; /* rows the counter system saw in its last run */
    de_entity pending[4];
    de_commands *range_buffers[64];
    u32 ranges;
} sync_ctx;

static void spawn4_fn(de_system_ctx *ctx) {
    sync_ctx *s = (sync_ctx *)ctx->user;
    for (u32 i = 0; i < 4; ++i) {
        s->pending[i] = de_cmd_spawn(ctx->commands);
        TPos *p = (TPos *)de_cmd_add(ctx->commands, s->pending[i], s->pos);
        if (p != NULL)
            p->v.x = (f32)(100 + i);
    }
}

static void count_chunk(const de_query_chunk *ch, void *user) {
    ((sync_ctx *)user)->seen_by_counter += ch->count;
}

static void counter_fn(de_system_ctx *ctx) {
    sync_ctx *s = (sync_ctx *)ctx->user;
    s->seen_by_counter = 0;
    de_query_each(ctx->world, ctx->queries[0], count_chunk, s);
}

static void add_vel_chunk(const de_query_chunk *ch, void *user) {
    sync_ctx *s = (sync_ctx *)user;
    if (ch->range < 64u)
        s->range_buffers[ch->range] = ch->commands; /* one thread per range */
    for (u32 i = 0; i < ch->count; ++i) {
        TVel *v = (TVel *)de_cmd_add(ch->commands, ch->entities[i], s->vel);
        if (v != NULL)
            v->v.y = (f32)ch->range;
    }
}

static void add_vel_fn(de_system_ctx *ctx) {
    sync_ctx *s = (sync_ctx *)ctx->user;
    s->ranges = (de_query_count(ctx->world, ctx->queries[0]) + 2u) / 3u;
    de_query_par_each(ctx->world, ctx->queries[0], 3, add_vel_chunk, s);
}

static void add_vel_without_range_scratch(de_system_ctx *ctx) {
    ecs_query *q = ecs_query_get(ctx->world, ctx->queries[0]);
    ASSERT_EQ(q->range_commands.capacity, 0u);
    const de_allocator *saved = q->range_commands.a;
    q->range_commands.a = de_allocator_null();
    add_vel_fn(ctx);
    q->range_commands.a = saved;
    EXPECT_EQ(q->range_commands.capacity, 0u);
}

TEST_F(sched, range_scratch_oom_preserves_commands, ecs_fx) {
    de_task_system *ts = make_ts(4);
    rebuild_world(f, ts, 0);
    sync_ctx s = {.pos = f->pos, .vel = f->vel};
    de_entity entities[8];
    for (u32 i = 0; i < 8; ++i) {
        entities[i] = de_entity_spawn(f->w);
        ASSERT_TRUE(de_entity_add(f->w, entities[i], f->pos) != NULL);
    }
    const de_query_term term = {f->pos, DE_READ};
    const de_query_desc query = {.terms = &term, .term_count = 1};
    ASSERT_TRUE(DE_HANDLE_VALID(add_system(f->w, "scratch-oom", DE_SCHEDULE_UPDATE,
                                           add_vel_without_range_scratch, &s, &query, 1)));
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0, 0), DE_OK);
    EXPECT_EQ(s.ranges, 3u);
    for (u32 i = 0; i < 8; ++i) {
        const TVel *v = de_entity_get_const(f->w, entities[i], f->vel);
        ASSERT_TRUE(v != NULL);
        EXPECT_FLOAT_EQ(v->v.y, (f32)(i / 3u), 0.0);
    }
    ecs_fx_destroy_world(f);
    de_task_system_destroy(ts);
}

TEST_F(sched, commands_sync_point, ecs_fx) {
    de_task_system *ts = make_ts(parallel_workers());
    rebuild_world(f, ts, 0);
    sync_ctx s;
    memset(&s, 0, sizeof s);
    s.pos = f->pos;
    s.vel = f->vel;
    const de_query_term rpos = {f->pos, DE_READ};
    const de_query_filter no_vel = {f->vel, DE_WITHOUT};
    const de_query_desc q_count = {&rpos, 1, NULL, 0};
    const de_query_desc q_novel = {&rpos, 1, &no_vel, 1};
    ASSERT_TRUE(
        DE_HANDLE_VALID(add_system(f->w, "spawn", DE_SCHEDULE_UPDATE, spawn4_fn, &s, NULL, 0)));
    ASSERT_TRUE(DE_HANDLE_VALID(
        add_system(f->w, "count", DE_SCHEDULE_UPDATE, counter_fn, &s, &q_count, 1)));
    ASSERT_TRUE(DE_HANDLE_VALID(
        add_system(f->w, "add_vel", DE_SCHEDULE_POST_UPDATE, add_vel_fn, &s, &q_novel, 1)));

    /* A main-thread command recorded outside the schedule applies first at its sync point. */
    const de_entity pm = de_cmd_spawn(de_world_commands(f->w));
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(s.seen_by_counter, 0u); /* commands are not applied during the schedule */
    const de_entity main_e = de_commands_resolve(f->w, pm);
    EXPECT_EQ(main_e.bits, ((u64)1u << 32) | 0u); /* index 0: the main buffer applied first */
    for (u32 i = 0; i < 4; ++i) {
        const de_entity e = de_commands_resolve(f->w, s.pending[i]);
        EXPECT_EQ(e.bits, ((u64)1u << 32) | (u64)(1u + i)); /* then the system's, in order */
        const TPos *p = (const TPos *)de_entity_get_const(f->w, e, f->pos);
        ASSERT_TRUE(p != NULL);
        EXPECT_FLOAT_EQ(p->v.x, (f32)(100 + i), 0.0);
    }
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(s.seen_by_counter, 4u); /* the first run's spawns, not this run's */

    /* par_each ranges get one buffer each; all of them apply at the end of POST_UPDATE. */
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 0.0f, 0.0f), DE_OK);
    ASSERT_EQ(s.ranges, 3u); /* 8 entities with pos, grain 3 */
    for (u32 r = 0; r < s.ranges; ++r) {
        EXPECT_TRUE(s.range_buffers[r] != NULL);
        for (u32 k = 0; k < r; ++k)
            EXPECT_TRUE(s.range_buffers[r] != s.range_buffers[k]);
    }
    u32 with_vel = 0;
    for (u32 i = 1; i <= 8u; ++i) {
        const de_entity e = {((u64)1u << 32) | i};
        const TVel *v = (const TVel *)de_entity_get_const(f->w, e, f->vel);
        if (v != NULL) {
            ++with_vel;
            EXPECT_FLOAT_EQ(v->v.y, (f32)((i - 1u) / 3u), 0.0); /* row i - 1 of the query */
        }
    }
    EXPECT_EQ(with_vel, 8u);
    ecs_fx_destroy_world(f);
    de_task_system_destroy(ts);
}

/* ---- sched.deterministic_spawn_ids_workers -------------------------------------------------- */

typedef struct det_world {
    de_component pos, vel, health;
} det_world;

/* SA: par_each over pos (grain 7): move, spawn children with pos, despawn the far ones. */
static void sa_chunk(const de_query_chunk *ch, void *user) {
    const det_world *dw = (const det_world *)user;
    TPos *p = (TPos *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i) {
        p[i].v.x += 1.0f;
        const u32 x = (u32)p[i].v.x, y = (u32)p[i].v.y;
        if ((x + y) % 13u == 0) {
            const de_entity child = de_cmd_spawn(ch->commands);
            TPos *np = (TPos *)de_cmd_add(ch->commands, child, dw->pos);
            if (np != NULL) {
                np->v.x = 0.0f;
                np->v.y = (f32)((y * 3u + 1u) % 1000u);
            }
        }
        if (x > 12u)
            de_cmd_despawn(ch->commands, ch->entities[i]);
    }
}

static void sa_fn(de_system_ctx *ctx) {
    de_query_par_each(ctx->world, ctx->queries[0], 7, sa_chunk, ctx->user);
}

/* SB: serial over vel: grow, spawn three per tick with the system buffer, despawn the old. */
typedef struct sb_state {
    de_commands *cmds;
} sb_state;

static void sb_chunk(const de_query_chunk *ch, void *user) {
    (void)user;
    TVel *v = (TVel *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i) {
        v[i].v.y += 2.0f;
        if (v[i].v.y >= 30.0f)
            de_cmd_despawn(ch->commands, ch->entities[i]);
    }
}

static void sb_fn(de_system_ctx *ctx) {
    const det_world *dw = (const det_world *)ctx->user;
    de_query_each(ctx->world, ctx->queries[0], sb_chunk, NULL);
    for (u32 i = 0; i < 3u; ++i) {
        const de_entity e = de_cmd_spawn(ctx->commands);
        TVel *v = (TVel *)de_cmd_add(ctx->commands, e, dw->vel);
        if (v != NULL) {
            v->v.x = (f32)ctx->tick;
            v->v.z = (f32)i;
        }
    }
}

/* SC: par_each over health (grain 5): count down; at 3 spawn an entity with pos and health, at 5
 * add vel to the row's entity, at 0 despawn it. */
static void sc_chunk(const de_query_chunk *ch, void *user) {
    const det_world *dw = (const det_world *)user;
    THealth *h = (THealth *)ch->columns[0];
    for (u32 i = 0; i < ch->count; ++i) {
        h[i].current -= 1.0f;
        const i32 cur = (i32)h[i].current;
        if (cur == 3) {
            const de_entity e = de_cmd_spawn(ch->commands);
            THealth *nh = (THealth *)de_cmd_add(ch->commands, e, dw->health);
            TPos *np = (TPos *)de_cmd_add(ch->commands, e, dw->pos);
            if (nh != NULL && np != NULL) {
                nh->current = h[i].max;
                nh->max = h[i].max;
                np->v.y = h[i].max;
            }
        } else if (cur == 5) {
            TVel *v = (TVel *)de_cmd_add(ch->commands, ch->entities[i], dw->vel);
            if (v != NULL)
                v->v.y = 20.0f;
        } else if (cur <= 0) {
            de_cmd_despawn(ch->commands, ch->entities[i]);
        }
    }
}

static void sc_fn(de_system_ctx *ctx) {
    de_query_par_each(ctx->world, ctx->queries[0], 5, sc_chunk, ctx->user);
}

typedef struct world_hash {
    u64 h;
    u32 n;
} world_hash;

static void hash_bytes(u64 *h, const void *p, size_t n) {
    const u8 *b = (const u8 *)p;
    for (size_t i = 0; i < n; ++i) {
        *h ^= b[i];
        *h *= 0x100000001B3ull;
    }
}

static void hash_chunk(const de_query_chunk *ch, void *user) {
    world_hash *wh = (world_hash *)user;
    for (u32 i = 0; i < ch->count; ++i) {
        hash_bytes(&wh->h, &ch->entities[i].bits, sizeof(u64));
        for (u32 c = 0; c < 3u; ++c) {
            static const size_t sizes[3] = {sizeof(TPos), sizeof(TVel), sizeof(THealth)};
            const u8 present = ch->columns[c] != NULL;
            hash_bytes(&wh->h, &present, 1);
            if (present)
                hash_bytes(&wh->h, (const u8 *)ch->columns[c] + (size_t)i * sizes[c], sizes[c]);
        }
        ++wh->n;
    }
}

/* 60 FixedUpdate ticks; the hash of the world (every entity id and component byte, in query
 * order) after each tick. */
static u64 det_spawn_run(ecs_fx *f, de_task_system *ts, u32 *out_alive) {
    rebuild_world(f, ts, 2048);
    det_world dw = {f->pos, f->vel, f->health};
    for (u32 i = 0; i < 50u; ++i) {
        const de_entity e = de_entity_spawn(f->w);
        if (i % 2u == 0) {
            TPos *p = (TPos *)de_entity_add(f->w, e, f->pos);
            p->v.y = (f32)i;
        }
        if (i % 3u == 0) {
            THealth *h = (THealth *)de_entity_add(f->w, e, f->health);
            h->current = (f32)(4u + i % 7u);
            h->max = (f32)(6u + i % 5u);
        }
        if (i % 5u == 0)
            de_entity_add(f->w, e, f->vel);
    }
    const de_query_term wpos = {f->pos, DE_WRITE}, wvel = {f->vel, DE_WRITE},
                        whealth = {f->health, DE_WRITE};
    /* Each entity has one owner that may despawn it (WITHOUT is not access, so the three
     * systems still do not conflict and run in parallel). */
    const de_query_filter no_health = {f->health, DE_WITHOUT}, no_pos = {f->pos, DE_WITHOUT};
    const de_query_filter fb[2] = {no_health, no_pos};
    const de_query_desc qa = {&wpos, 1, &no_health, 1}, qb = {&wvel, 1, fb, 2},
                        qc = {&whealth, 1, NULL, 0};
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.schedule = DE_SCHEDULE_FIXED_UPDATE;
    d.flags = DE_SYSTEM_DETERMINISTIC;
    d.user = &dw;
    d.query_count = 1;
    d.name = "sa";
    d.fn = sa_fn;
    d.queries = &qa;
    EXPECT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &d)));
    d.name = "sb";
    d.fn = sb_fn;
    d.queries = &qb;
    EXPECT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &d)));
    d.name = "sc";
    d.fn = sc_fn;
    d.queries = &qc;
    EXPECT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &d)));
    EXPECT_EQ(f->w->systems->schedules[DE_SCHEDULE_FIXED_UPDATE].edges.count, 0u);

    const de_query_term all[3] = {
        {f->pos, DE_OPTIONAL_READ}, {f->vel, DE_OPTIONAL_READ}, {f->health, DE_OPTIONAL_READ}};
    const de_query_desc qall = {all, 3, NULL, 0};
    const de_query everything = de_query_create(f->w, &qall);
    u64 trace = 0xCBF29CE484222325ull;
    world_hash wh = {0, 0};
    for (u32 tick = 0; tick < 60u; ++tick) {
        EXPECT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_FIXED_UPDATE, 1.0f / 60.0f, 0.0f), DE_OK);
        wh.h = 0xCBF29CE484222325ull;
        wh.n = 0;
        de_query_each(f->w, everything, hash_chunk, &wh);
        hash_bytes(&trace, &wh.h, sizeof wh.h);
        hash_bytes(&trace, &wh.n, sizeof wh.n);
    }
    EXPECT_EQ(de_world_tick(f->w), 60u);
    *out_alive = wh.n;
    ecs_fx_destroy_world(f);
    return trace;
}

TEST_F(sched, deterministic_spawn_ids_workers, ecs_fx) {
    de_fp_env_init_thread();
    u32 alive0 = 0;
    const u64 base = det_spawn_run(f, NULL, &alive0);
    printf("no task system: trace 0x%016llX, %u entities alive\n", (unsigned long long)base,
           alive0);
    EXPECT_GT(alive0, 50u);
    static const u32 workers[] = {1u, 2u, 8u};
    for (u32 wi = 0; wi < 3u; ++wi) {
        de_task_system *ts = make_ts(workers[wi]);
        for (u32 run = 0; run < 5u; ++run) {
            u32 alive = 0;
            const u64 t = det_spawn_run(f, ts, &alive);
            EXPECT_EQ(t, base);
            EXPECT_EQ(alive, alive0);
        }
        de_task_system_destroy(ts);
    }
    /* Test vector: integer-valued data and integer-only hashing, the same on every platform. */
    EXPECT_EQ(base, SCHED_SPAWN_TRACE);
    ecs_fx_create_world(f, NULL);
}

/* ---- sched.events_updated_in_first ---------------------------------------------------------- */

typedef struct event_state {
    de_event_type ev;
    u32 frame;
    de_event_reader reader;
    bool reader_made;
    u32 got[16];
    u32 got_n;
} event_state;

static void send_fn(de_system_ctx *ctx) {
    event_state *s = (event_state *)ctx->user;
    const THealth h = {(f32)s->frame, 0.0f};
    de_event_send(ctx->world, s->ev, &h);
}

static void read_fn(de_system_ctx *ctx) {
    event_state *s = (event_state *)ctx->user;
    if (!s->reader_made) {
        s->reader = de_event_reader_create(ctx->world, s->ev);
        s->reader_made = true;
    }
    const THealth *h;
    while ((h = (const THealth *)de_event_next(ctx->world, &s->reader)) != NULL)
        if (s->got_n < 16u)
            s->got[s->got_n++] = (u32)h->current;
}

/* The events a fresh reader sees now, as frame numbers. */
static u32 alive_events(de_world *w, de_event_type ev, u32 *out, u32 max) {
    de_event_reader r = de_event_reader_create(w, ev);
    u32 n = 0;
    const THealth *h;
    while ((h = (const THealth *)de_event_next(w, &r)) != NULL)
        if (n < max)
            out[n++] = (u32)h->current;
    return n;
}

TEST_F(sched, events_updated_in_first, ecs_fx) {
    event_state s;
    memset(&s, 0, sizeof s);
    s.ev = de_event_register(f->w, f->t_health);
    de_system_desc d;
    memset(&d, 0, sizeof d);
    d.schedule = DE_SCHEDULE_UPDATE;
    d.user = &s;
    d.name = "send";
    d.fn = send_fn;
    d.writes_events = &s.ev;
    d.writes_event_count = 1;
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &d)));
    memset(&d, 0, sizeof d);
    d.schedule = DE_SCHEDULE_UPDATE;
    d.user = &s;
    d.name = "read";
    d.fn = read_fn;
    d.reads_events = &s.ev;
    d.reads_event_count = 1;
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &d))); /* after "send": a conflict */

    u32 alive[8];
    for (s.frame = 0; s.frame < 6u; ++s.frame) {
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_FIRST, 0.0f, 0.0f), DE_OK);
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
        /* The reader system saw this frame's event in this frame. */
        ASSERT_EQ(s.got_n, s.frame + 1u);
        EXPECT_EQ(s.got[s.frame], s.frame);
        /* Alive: this frame's and last frame's; FIRST dropped the one before. */
        const u32 n = alive_events(f->w, s.ev, alive, 8);
        if (s.frame == 0) {
            ASSERT_EQ(n, 1u);
            EXPECT_EQ(alive[0], 0u);
        } else {
            ASSERT_EQ(n, 2u);
            EXPECT_EQ(alive[0], s.frame - 1u);
            EXPECT_EQ(alive[1], s.frame);
        }
    }
    EXPECT_EQ(de_event_reader_missed(&s.reader), 0u);
    /* Without FIRST nothing is dropped. */
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(alive_events(f->w, s.ev, alive, 8), 4u);
}

/* ---- sched.change_window_per_system --------------------------------------------------------- */

typedef struct change_probe {
    u32 rows;
    de_entity target;  /* mutable get target (system "m") */
    de_component comp; /* its component */
} change_probe;

static void rows_chunk(const de_query_chunk *ch, void *user) {
    ((change_probe *)user)->rows += ch->count;
}

static void rows_fn(de_system_ctx *ctx) {
    change_probe *p = (change_probe *)ctx->user;
    p->rows = 0;
    de_query_each(ctx->world, ctx->queries[0], rows_chunk, p);
}

static void write_fn(de_system_ctx *ctx) {
    change_probe *p = (change_probe *)ctx->user;
    p->rows = 0;
    de_query_each(ctx->world, ctx->queries[0], rows_chunk, p); /* marks every yielded row */
}

static void rows_then_get_fn(de_system_ctx *ctx) {
    rows_fn(ctx);
    change_probe *p = (change_probe *)ctx->user;
    (void)de_entity_get(ctx->world, p->target, p->comp); /* stamps the system's run tick */
}

TEST_F(sched, change_window_per_system, ecs_fx) {
    de_task_system *ts = make_ts(parallel_workers());
    rebuild_world(f, ts, 0);
    de_entity e[10];
    for (u32 i = 0; i < 10u; ++i) {
        e[i] = de_entity_spawn(f->w);
        de_entity_add(f->w, e[i], f->pos);
        if (i < 4u)
            de_entity_add(f->w, e[i], f->enemy);
        if (i < 3u)
            de_entity_add(f->w, e[i], f->health);
    }
    change_probe r0, w, r1, m, o;
    memset(&r0, 0, sizeof r0);
    memset(&w, 0, sizeof w);
    memset(&r1, 0, sizeof r1);
    memset(&m, 0, sizeof m);
    memset(&o, 0, sizeof o);
    const de_query_term rpos = {f->pos, DE_READ}, wpos = {f->pos, DE_WRITE};
    const de_query_filter changed_pos = {f->pos, DE_CHANGED}, with_enemy = {f->enemy, DE_WITH};
    const de_query_desc q_changed = {&rpos, 1, &changed_pos, 1};
    const de_query_desc q_write = {&wpos, 1, &with_enemy, 1};
    ASSERT_TRUE(
        DE_HANDLE_VALID(add_system(f->w, "r0", DE_SCHEDULE_UPDATE, rows_fn, &r0, &q_changed, 1)));
    const de_system wh = add_system(f->w, "w", DE_SCHEDULE_UPDATE, write_fn, &w, &q_write, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(wh));
    ASSERT_TRUE(
        DE_HANDLE_VALID(add_system(f->w, "r1", DE_SCHEDULE_UPDATE, rows_fn, &r1, &q_changed, 1)));

    static const u32 expect_r0[5] = {10, 4, 4, 4, 0};
    static const u32 expect_r1[5] = {10, 4, 4, 0, 0};
    for (u32 run = 0; run < 5u; ++run) {
        if (run == 3)
            de_system_set_enabled(f->w, wh, false);
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_UPDATE, 0.0f, 0.0f), DE_OK);
        /* r0 runs before w: it sees w's writes of the previous run; r1 sees this run's. */
        EXPECT_EQ(r0.rows, expect_r0[run]);
        EXPECT_EQ(r1.rows, expect_r1[run]);
    }

    /* A mutable get inside a system stamps that system's run tick: "m" does not see its own
     * change on its next run, "o" (after it) sees it every run. */
    const de_query_term rh = {f->health, DE_READ}, wh_term = {f->health, DE_WRITE};
    const de_query_filter changed_h = {f->health, DE_CHANGED};
    const de_query_desc qm[2] = {{&rh, 1, &changed_h, 1}, {&wh_term, 1, NULL, 0}};
    m.target = e[0];
    m.comp = f->health;
    ASSERT_TRUE(DE_HANDLE_VALID(
        add_system(f->w, "m", DE_SCHEDULE_POST_UPDATE, rows_then_get_fn, &m, qm, 2)));
    ASSERT_TRUE(
        DE_HANDLE_VALID(add_system(f->w, "o", DE_SCHEDULE_POST_UPDATE, rows_fn, &o, qm, 1)));
    for (u32 run = 0; run < 4u; ++run) {
        ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 0.0f, 0.0f), DE_OK);
        EXPECT_EQ(m.rows, run == 0 ? 3u : 0u);
        EXPECT_EQ(o.rows, run == 0 ? 3u : 1u);
    }
    /* Outside systems the world's change tick is stamped: both see it once. */
    de_entity_mark_changed(f->w, e[1], f->health);
    ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_POST_UPDATE, 0.0f, 0.0f), DE_OK);
    EXPECT_EQ(m.rows, 1u);
    EXPECT_EQ(o.rows, 2u);
    ecs_fx_destroy_world(f);
    de_task_system_destroy(ts);
}

/* ---- sched.minimal_game_systems ------------------------------------------------------------- */
/* The two systems of examples/minimal_game.c without the asset system: a main-thread "loading"
 * system in Update that switches to playing (here after three frames instead of a bundle load),
 * and the deterministic FixedUpdate "spin" system that par_each-es over DeSpin with de_dsin. */

typedef struct DeSpin {
    f32 speed_rad;
} DeSpin;

typedef enum { STATE_LOADING, STATE_PLAYING } game_phase;

typedef struct game_state {
    game_phase phase;
    u32 loading_runs;
    u32 off_main;
    de_component c_spin;
} game_state;

static void loading_system(de_system_ctx *ctx) {
    game_state *gs = (game_state *)ctx->user;
    if (de_task_worker_index() != 0)
        ++gs->off_main;
    ++gs->loading_runs;
    if (gs->phase == STATE_LOADING && gs->loading_runs >= 3u)
        gs->phase = STATE_PLAYING;
}

typedef struct spin_job {
    f32 dt;
} spin_job;

static void spin_chunk(const de_query_chunk *chunk, void *user) {
    const spin_job *job = (const spin_job *)user;
    DeSpin *spins = (DeSpin *)chunk->columns[0];
    for (u32 i = 0; i < chunk->count; ++i)
        spins[i].speed_rad = de_dsin(spins[i].speed_rad + job->dt); /* deterministic math */
}

static void spin_system(de_system_ctx *ctx) {
    spin_job job = {ctx->dt};
    de_query_par_each(ctx->world, ctx->queries[0], 1024, spin_chunk, &job);
}

TEST_F(sched, minimal_game_systems, ecs_fx) {
    de_task_system *ts = make_ts(4);
    rebuild_world(f, ts, 0);
    game_state gs;
    memset(&gs, 0, sizeof gs);
    static const de_field_info spin_fields[] = {
        DE_FIELD(DeSpin, speed_rad, DE_FK_F32, DE_FIELD_DEFAULT | DE_FIELD_REPLICATE),
    };
    const de_type_id t = de_type_register(f->types, &DE_TYPE_DESC(DeSpin, 1, spin_fields));
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    gs.c_spin = de_component_register(f->w, t, DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE);
    ASSERT_TRUE(DE_HANDLE_VALID(gs.c_spin));

    de_system_desc loading = {
        .name = "game.loading",
        .schedule = DE_SCHEDULE_UPDATE,
        .fn = loading_system,
        .user = &gs,
        .flags = DE_SYSTEM_MAIN_THREAD,
    };
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &loading)));
    de_query_term terms[1];
    terms[0] = (de_query_term){gs.c_spin, DE_WRITE};
    const de_query_desc q = {.terms = terms, .term_count = 1};
    de_system_desc spin = {
        .name = "game.spin",
        .schedule = DE_SCHEDULE_FIXED_UPDATE,
        .fn = spin_system,
        .user = &gs,
        .flags = DE_SYSTEM_DETERMINISTIC,
        .queries = &q,
        .query_count = 1,
    };
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(f->w, &spin)));

    enum { N = 3000 };
    static f32 expect[N];
    for (u32 i = 0; i < N; ++i) {
        const de_entity e = de_entity_spawn(f->w);
        DeSpin *s = (DeSpin *)de_entity_add(f->w, e, gs.c_spin);
        s->speed_rad = (f32)i * 0.001f;
        expect[i] = s->speed_rad;
    }
    const f32 dt = 1.0f / 60.0f;
    for (u32 frame = 0; frame < 10u; ++frame) {
        static const de_schedule pre[] = {DE_SCHEDULE_FIRST, DE_SCHEDULE_PRE_UPDATE};
        static const de_schedule post[] = {DE_SCHEDULE_UPDATE, DE_SCHEDULE_POST_UPDATE,
                                           DE_SCHEDULE_EXTRACT, DE_SCHEDULE_LAST};
        for (u32 k = 0; k < 2u; ++k)
            ASSERT_EQ(de_world_run_schedule(f->w, pre[k], dt, 0.0f), DE_OK);
        for (u32 step = 0; step < 2u; ++step)
            ASSERT_EQ(de_world_run_schedule(f->w, DE_SCHEDULE_FIXED_UPDATE, dt, 0.0f), DE_OK);
        for (u32 k = 0; k < 4u; ++k)
            ASSERT_EQ(de_world_run_schedule(f->w, post[k], dt, 0.5f), DE_OK);
    }
    EXPECT_EQ(gs.loading_runs, 10u);
    EXPECT_EQ(gs.off_main, 0u);
    EXPECT_EQ(gs.phase, STATE_PLAYING);
    EXPECT_EQ(de_world_tick(f->w), 20u);
    u32 mismatches = 0;
    for (u32 i = 0; i < N; ++i) {
        f32 v = expect[i];
        for (u32 step = 0; step < 20u; ++step)
            v = de_dsin(v + dt);
        const de_entity e = {((u64)1u << 32) | i};
        const DeSpin *s = (const DeSpin *)de_entity_get_const(f->w, e, gs.c_spin);
        if (s == NULL || memcmp(&s->speed_rad, &v, sizeof v) != 0)
            ++mismatches;
    }
    EXPECT_EQ(mismatches, 0u);
    ecs_fx_destroy_world(f);
    de_task_system_destroy(ts);
}
