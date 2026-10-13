/* Stress tests of the task system (src/task/; DENG-293). The gate for S05: the ECS schedule
 * runs entirely on this scheduler, so these run under DE_SANITIZE=thread and address.
 *
 * stress.mixed_3s: for DE_STRESS_SECONDS (default 3) the main thread picks random operations
 * (nested submits, graph runs with parallel_for inside nodes and main-thread and background
 * nodes, plain parallel_for, frame tasks that submit main-thread tasks, background tasks that
 * wait on frame work, and graph runs overlapping other operations), while two foreign threads
 * submit, wait and run their own graphs the whole time. Every operation checks its counts and
 * invariants; at the end every submitted task has run exactly once.
 *
 * stress.foreign_thread_submits: four threads that are not workers submit frame, background and
 * main-thread tasks (including bursts larger than the injection queue), run parallel_for and
 * graphs, while the main thread only waits for them (running main-thread and frame tasks).
 *
 * Tasks never call ASSERT_* (not thread-safe): they count failures in g_fail, which the main
 * thread checks. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif
#include "de_test.h"

#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/de_atomic.h"
#include "task/task_internal.h"

static de_atomic_u32 g_fail; /* invariant violations seen by tasks */

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            if (de_atomic_fetch_add_u32(&g_fail, 1, DE_ATOMIC_RELAXED) < 8)                        \
                printf("stress: check failed: %s (%s:%d)\n", #c, __FILE__, __LINE__);              \
        }                                                                                          \
    } while (0)

static u64 rng_next(u64 *s) {
    u64 x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static u32 rng_below(u64 *s, u32 n) {
    return (u32)((rng_next(s) >> 32) % n);
}

static u64 stress_seconds_ns(void) {
    const char *v = getenv("DE_STRESS_SECONDS");
    double sec = 3.0;
    if (v != NULL && *v != '\0') {
        const double x = atof(v);
        if (x > 0.0)
            sec = x;
    }
    return (u64)(sec * 1e9);
}

static de_task_system *g_ts;

/* ---------- Nested submits ---------- */

typedef struct nest_op {
    de_task_counter counter;
    de_atomic_u32 ran;
} nest_op;

#define NEST_FAN   6u
#define NEST_DEPTH 3u /* 6 + 36 + 216 tasks */

static void nest_spawn(nest_op *op, u32 depth);

typedef struct nest_slot {
    nest_op *op;
    u32 depth;
} nest_slot;

static void nest_fn(void *user) {
    const nest_slot *s = (const nest_slot *)user;
    de_atomic_fetch_add_u32(&s->op->ran, 1, DE_ATOMIC_RELAXED);
    if (s->depth + 1 < NEST_DEPTH)
        nest_spawn(s->op, s->depth + 1);
}

/* One slot per (op, depth): every task of a depth points to the same immutable slot. */
typedef struct nest_ctx {
    nest_op op;
    nest_slot slots[NEST_DEPTH];
} nest_ctx;

static void nest_spawn(nest_op *op, u32 depth) {
    nest_ctx *ctx = (nest_ctx *)(void *)op; /* op is the first member */
    for (u32 i = 0; i < NEST_FAN; ++i)
        de_task_submit(g_ts, DE_TASK_POOL_FRAME, nest_fn, &ctx->slots[depth], &op->counter);
}

static u32 nest_expected(void) {
    u32 total = 0, level = 1;
    for (u32 d = 0; d < NEST_DEPTH; ++d) {
        level *= NEST_FAN;
        total += level;
    }
    return total;
}

static void nest_init(nest_ctx *ctx) {
    memset(&ctx->op.counter, 0, sizeof ctx->op.counter);
    de_atomic_init_u32(&ctx->op.ran, 0);
    for (u32 d = 0; d < NEST_DEPTH; ++d) {
        ctx->slots[d].op = &ctx->op;
        ctx->slots[d].depth = d;
    }
}

static void op_nested(void) {
    nest_ctx ctx;
    nest_init(&ctx);
    nest_spawn(&ctx.op, 0);
    de_task_wait(g_ts, &ctx.op.counter);
    CHECK(de_atomic_load_u32(&ctx.op.ran, DE_ATOMIC_RELAXED) == nest_expected());
}

/* ---------- parallel_for with coverage and boundary checks ---------- */

typedef struct pfor_check {
    u32 count;
    u32 grain;
    de_atomic_u64 index_sum;
    de_atomic_u32 chunks;
} pfor_check;

static void pfor_check_fn(u32 begin, u32 end, void *user) {
    pfor_check *p = (pfor_check *)user;
    const u64 e = (u64)begin + p->grain < p->count ? (u64)begin + p->grain : p->count;
    CHECK(begin % p->grain == 0);
    CHECK(end == e);
    CHECK(begin < end);
    u64 s = 0;
    for (u32 i = begin; i < end; ++i)
        s += i;
    de_atomic_fetch_add_u64(&p->index_sum, s, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&p->chunks, 1, DE_ATOMIC_RELAXED);
}

static void run_pfor_checked(de_task_system *ts, u32 count, u32 grain) {
    pfor_check p;
    p.count = count;
    p.grain = grain;
    de_atomic_init_u64(&p.index_sum, 0);
    de_atomic_init_u32(&p.chunks, 0);
    de_task_parallel_for(ts, count, grain, pfor_check_fn, &p);
    const u64 expect_sum = count > 0 ? (u64)count * (count - 1) / 2 : 0;
    const u32 expect_chunks = count > 0 ? (count - 1) / grain + 1 : 0;
    CHECK(de_atomic_load_u64(&p.index_sum, DE_ATOMIC_RELAXED) == expect_sum);
    CHECK(de_atomic_load_u32(&p.chunks, DE_ATOMIC_RELAXED) == expect_chunks);
}

static void op_pfor(u64 *rng) {
    const u32 count = rng_below(rng, 4) == 0 ? rng_below(rng, 64) : rng_below(rng, 200000);
    const u32 grain = 1 + rng_below(rng, rng_below(rng, 2) ? 64 : 5000);
    run_pfor_checked(g_ts, count, grain);
}

/* ---------- Graphs ---------- */

#define GRAPH_NODES 40u
#define GRAPH_EDGES 3u /* random predecessors per node, at most */

enum { KIND_PLAIN, KIND_PFOR, KIND_MAIN, KIND_BACKGROUND, KIND_NESTED };

typedef struct sgraph sgraph;

typedef struct snode {
    sgraph *g;
    u32 index;
    u32 kind;
} snode;

struct sgraph {
    de_task_graph *graph;
    de_task_system *ts;
    de_task_node h[GRAPH_NODES];
    snode nodes[GRAPH_NODES];
    u32 pred[GRAPH_NODES][GRAPH_EDGES];
    u32 pred_count[GRAPH_NODES];
    de_atomic_u32 seq;
    de_atomic_u32 start[GRAPH_NODES];
    de_atomic_u32 end[GRAPH_NODES];
    de_atomic_u32 runs[GRAPH_NODES];
    de_task_counter done;
    u64 run_count;
};

static void nested_outer(u32 begin, u32 end, void *user) {
    const sgraph *g = (const sgraph *)user;
    CHECK(end == begin + 1);
    run_pfor_checked(g->ts, 256, 16);
}

static void snode_fn(void *user) {
    snode *n = (snode *)user;
    sgraph *g = n->g;
    const u32 s = de_atomic_fetch_add_u32(&g->seq, 1, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g->start[n->index], s, DE_ATOMIC_RELAXED);
    /* Every predecessor has finished (end stamped) before this node starts. */
    for (u32 k = 0; k < g->pred_count[n->index]; ++k) {
        const u32 p = g->pred[n->index][k];
        CHECK(de_atomic_load_u32(&g->runs[p], DE_ATOMIC_RELAXED) == 1);
        CHECK(de_atomic_load_u32(&g->end[p], DE_ATOMIC_RELAXED) < s);
    }
    switch (n->kind) {
    case KIND_PFOR:
        run_pfor_checked(g->ts, 1000 + n->index * 97u, 1 + n->index);
        break;
    case KIND_MAIN:
        CHECK(de_task_worker_index() == 0);
        break;
    case KIND_BACKGROUND:
        CHECK(de_task_worker_index() == DE_TASK_WORKER_NONE);
        break;
    case KIND_NESTED:
        /* parallel_for chunks that each run a parallel_for, inside a graph node. */
        de_task_parallel_for(g->ts, 4, 1, nested_outer, g);
        break;
    default:
        break;
    }
    de_atomic_fetch_add_u32(&g->runs[n->index], 1, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g->end[n->index], de_atomic_fetch_add_u32(&g->seq, 1, DE_ATOMIC_RELAXED),
                        DE_ATOMIC_RELAXED);
}

/* Builds a random DAG (edges only from lower to higher indices). `with_main`: some nodes run on
 * the main thread (only for graphs whose runner waits on, or keeps pumping, the main thread). */
static void sgraph_build(sgraph *g, de_task_system *ts, u64 seed, bool with_main) {
    memset(g, 0, sizeof *g);
    g->ts = ts;
    u64 rng = seed | 1u;
    CHECK(de_task_graph_create(ts, &g->graph) == DE_OK);
    for (u32 i = 0; i < GRAPH_NODES; ++i) {
        u32 kind = KIND_PLAIN;
        switch (rng_below(&rng, 8)) {
        case 0:
        case 1:
            kind = KIND_PFOR;
            break;
        case 2:
            kind = with_main ? KIND_MAIN : KIND_PLAIN;
            break;
        case 3:
            kind = KIND_BACKGROUND;
            break;
        case 4:
            kind = KIND_NESTED;
            break;
        default:
            break;
        }
        g->nodes[i] = (snode){g, i, kind};
        de_task_node_desc d = {0};
        d.name = "stress.node";
        d.fn = snode_fn;
        d.user = &g->nodes[i];
        d.pool = kind == KIND_MAIN         ? DE_TASK_POOL_MAIN_THREAD
                 : kind == KIND_BACKGROUND ? DE_TASK_POOL_BACKGROUND
                                           : DE_TASK_POOL_FRAME;
        g->h[i] = de_task_graph_add(g->graph, &d);
        const u32 np = i == 0 ? 0 : rng_below(&rng, GRAPH_EDGES + 1);
        for (u32 k = 0; k < np; ++k) {
            const u32 p = rng_below(&rng, i);
            g->pred[i][g->pred_count[i]++] = p;
            de_task_graph_depend(g->graph, g->h[p], g->h[i]);
        }
    }
    CHECK(de_task_graph_compile(g->graph) == DE_OK);
}

static void sgraph_start(sgraph *g) {
    de_atomic_store_u32(&g->seq, 1, DE_ATOMIC_RELAXED);
    for (u32 i = 0; i < GRAPH_NODES; ++i) {
        de_atomic_store_u32(&g->start[i], 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u32(&g->end[i], 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u32(&g->runs[i], 0, DE_ATOMIC_RELAXED);
    }
    de_task_graph_run(g->graph, &g->done);
}

static void sgraph_finish(sgraph *g) {
    de_task_wait(g->ts, &g->done);
    for (u32 i = 0; i < GRAPH_NODES; ++i)
        CHECK(de_atomic_load_u32(&g->runs[i], DE_ATOMIC_RELAXED) == 1);
    ++g->run_count;
}

static void sgraph_destroy(sgraph *g) {
    de_task_graph_destroy(g->graph);
    g->graph = NULL;
}

/* ---------- Main-thread tasks submitted by frame tasks ---------- */

typedef struct main_op {
    de_task_counter counter;
    de_atomic_u32 frame_ran;
    de_atomic_u32 main_ran;
} main_op;

static void main_leaf(void *user) {
    main_op *op = (main_op *)user;
    CHECK(de_task_worker_index() == 0);
    de_atomic_fetch_add_u32(&op->main_ran, 1, DE_ATOMIC_RELAXED);
}

static void frame_to_main(void *user) {
    main_op *op = (main_op *)user;
    de_atomic_fetch_add_u32(&op->frame_ran, 1, DE_ATOMIC_RELAXED);
    de_task_submit(g_ts, DE_TASK_POOL_MAIN_THREAD, main_leaf, op, &op->counter);
}

static void op_main_tasks(u64 *rng) {
    main_op op;
    memset(&op.counter, 0, sizeof op.counter);
    de_atomic_init_u32(&op.frame_ran, 0);
    de_atomic_init_u32(&op.main_ran, 0);
    const u32 n = 1 + rng_below(rng, 32);
    for (u32 i = 0; i < n; ++i)
        de_task_submit(g_ts, DE_TASK_POOL_FRAME, frame_to_main, &op, &op.counter);
    de_task_wait(g_ts, &op.counter);
    CHECK(de_atomic_load_u32(&op.frame_ran, DE_ATOMIC_RELAXED) == n);
    CHECK(de_atomic_load_u32(&op.main_ran, DE_ATOMIC_RELAXED) == n);
}

/* ---------- Background tasks that wait on frame work ---------- */

typedef struct bg_op {
    de_task_counter counter;
    de_atomic_u32 bg_ran;
    de_atomic_u32 leaves;
} bg_op;

static void bg_leaf(void *user) {
    bg_op *op = (bg_op *)user;
    de_atomic_fetch_add_u32(&op->leaves, 1, DE_ATOMIC_RELAXED);
}

#define BG_LEAVES 16u

static void bg_task(void *user) {
    bg_op *op = (bg_op *)user;
    CHECK(de_task_worker_index() == DE_TASK_WORKER_NONE);
    de_task_counter c = {0};
    for (u32 i = 0; i < BG_LEAVES; ++i)
        de_task_submit(g_ts, DE_TASK_POOL_FRAME, bg_leaf, op, &c);
    de_task_wait(g_ts, &c); /* a background worker parks; frame workers run the leaves */
    run_pfor_checked(g_ts, 5000, 100);
    de_atomic_fetch_add_u32(&op->bg_ran, 1, DE_ATOMIC_RELAXED);
}

static void op_background(u64 *rng) {
    bg_op op;
    memset(&op.counter, 0, sizeof op.counter);
    de_atomic_init_u32(&op.bg_ran, 0);
    de_atomic_init_u32(&op.leaves, 0);
    const u32 n = 1 + rng_below(rng, 4);
    for (u32 i = 0; i < n; ++i)
        de_task_submit(g_ts, DE_TASK_POOL_BACKGROUND, bg_task, &op, &op.counter);
    de_task_wait(g_ts, &op.counter);
    CHECK(de_atomic_load_u32(&op.bg_ran, DE_ATOMIC_RELAXED) == n);
    CHECK(de_atomic_load_u32(&op.leaves, DE_ATOMIC_RELAXED) == n * BG_LEAVES);
}

/* ---------- Graph runs from inside frame tasks ---------- */

/* A graph without main-thread nodes, run and waited on inside a frame task (wait-and-help on a
 * worker). */
typedef struct inner_graph_op {
    sgraph *g;
    de_task_counter counter;
} inner_graph_op;

static void run_graph_task(void *user) {
    inner_graph_op *op = (inner_graph_op *)user;
    sgraph_start(op->g);
    sgraph_finish(op->g);
}

/* ---------- Foreign threads ---------- */

typedef struct foreign {
    de_thread *thread;
    u32 id;
    u64 seed;
    u32 iterations; /* 0 = until g_stop */
    sgraph graph;   /* no main-thread nodes */
    de_atomic_u32 frame_ran;
    de_atomic_u32 bg_ran;
    de_atomic_u32 main_ran;
    u64 frame_sent, bg_sent, main_sent;
    bool with_main; /* also submit main-thread tasks (the main thread pumps) */
    bool burst;     /* start with a burst larger than the injection queue */
} foreign;

static de_atomic_u32 g_stop;

/* The foreign threads still running, as a task counter: charged once per thread before it
 * starts, finished by the thread as it ends (internal counter layout, task_internal.h). The
 * main thread waits on it with de_task_wait, which runs main-thread tasks and wakes on each
 * main-thread submit, so it pumps them without spinning: a spinning main thread takes a core
 * from the threads it waits for (DENG-375). */
static de_task_counter g_foreign_alive;

static void foreign_frame(void *user) {
    foreign *f = (foreign *)user;
    CHECK(de_task_worker_index() != DE_TASK_WORKER_NONE);
    de_atomic_fetch_add_u32(&f->frame_ran, 1, DE_ATOMIC_RELAXED);
}

static void foreign_bg(void *user) {
    foreign *f = (foreign *)user;
    CHECK(de_task_worker_index() == DE_TASK_WORKER_NONE);
    de_atomic_fetch_add_u32(&f->bg_ran, 1, DE_ATOMIC_RELAXED);
}

static void foreign_main(void *user) {
    foreign *f = (foreign *)user;
    CHECK(de_task_worker_index() == 0);
    de_atomic_fetch_add_u32(&f->main_ran, 1, DE_ATOMIC_RELAXED);
}

static i32 foreign_main_loop(void *user) {
    foreign *f = (foreign *)user;
    de_fp_env_init_thread();
    CHECK(de_task_worker_index() == DE_TASK_WORKER_NONE);
    u64 rng = f->seed | 1u;
    sgraph_build(&f->graph, g_ts, f->seed * 31u + 7u, false);
    if (f->burst) {
        de_task_counter c = {0};
        const u32 n = DE__TASK_INJECT_CAPACITY + 1000u;
        for (u32 i = 0; i < n; ++i)
            de_task_submit(g_ts, DE_TASK_POOL_FRAME, foreign_frame, f, &c);
        f->frame_sent += n;
        de_task_wait(g_ts, &c);
    }
    for (u32 it = 0; f->iterations == 0 ? de_atomic_load_u32(&g_stop, DE_ATOMIC_ACQUIRE) == 0
                                        : it < f->iterations;
         ++it) {
        de_task_counter c = {0};
        const u32 nf = 1 + rng_below(&rng, 48);
        const u32 nb = rng_below(&rng, 3);
        const u32 nm = f->with_main ? rng_below(&rng, 3) : 0;
        for (u32 i = 0; i < nf; ++i)
            de_task_submit(g_ts, DE_TASK_POOL_FRAME, foreign_frame, f, &c);
        for (u32 i = 0; i < nb; ++i)
            de_task_submit(g_ts, DE_TASK_POOL_BACKGROUND, foreign_bg, f, &c);
        for (u32 i = 0; i < nm; ++i)
            de_task_submit(g_ts, DE_TASK_POOL_MAIN_THREAD, foreign_main, f, &c);
        f->frame_sent += nf;
        f->bg_sent += nb;
        f->main_sent += nm;
        if (it % 8 == 0)
            run_pfor_checked(g_ts, rng_below(&rng, 20000), 1 + rng_below(&rng, 300));
        if (it % 16 == 0) {
            sgraph_start(&f->graph);
            sgraph_finish(&f->graph);
        }
        de_task_wait(g_ts, &c);
        CHECK(de_atomic_load_u32(&f->frame_ran, DE_ATOMIC_RELAXED) == f->frame_sent);
        CHECK(de_atomic_load_u32(&f->bg_ran, DE_ATOMIC_RELAXED) == f->bg_sent);
        CHECK(de_atomic_load_u32(&f->main_ran, DE_ATOMIC_RELAXED) == f->main_sent);
    }
    sgraph_destroy(&f->graph);
    de__task_counter_finish(g_ts, de__counter(&g_foreign_alive));
    return 0;
}

static void foreign_start(foreign *f, u32 id, u32 iterations, bool with_main, bool burst) {
    memset(f, 0, sizeof *f);
    f->id = id;
    f->seed = 0x5EED0000u + id * 7919u;
    f->iterations = iterations;
    f->with_main = with_main;
    f->burst = burst;
    de_atomic_init_u32(&f->frame_ran, 0);
    de_atomic_init_u32(&f->bg_ran, 0);
    de_atomic_init_u32(&f->main_ran, 0);
    de_atomic_fetch_add_u64(&de__counter(&g_foreign_alive)->state, 1, DE_ATOMIC_RELAXED);
    char name[32];
    snprintf(name, sizeof name, "stress-foreign-%u", id);
    ASSERT_EQ(de_thread_create(foreign_main_loop, f, name, DE_THREAD_NORMAL, 0, &f->thread), DE_OK);
}

/* The main thread runs main-thread tasks until every foreign thread has finished, then joins. */
static void foreign_join_all(foreign *fs, u32 n) {
    de_task_wait(g_ts, &g_foreign_alive);
    for (u32 i = 0; i < n; ++i) {
        EXPECT_EQ(de_thread_join(fs[i].thread), 0);
        EXPECT_EQ((u64)de_atomic_load_u32(&fs[i].frame_ran, DE_ATOMIC_RELAXED), fs[i].frame_sent);
        EXPECT_EQ((u64)de_atomic_load_u32(&fs[i].bg_ran, DE_ATOMIC_RELAXED), fs[i].bg_sent);
        EXPECT_EQ((u64)de_atomic_load_u32(&fs[i].main_ran, DE_ATOMIC_RELAXED), fs[i].main_sent);
    }
}

static de_task_system *make_ts(u32 workers, u32 bg) {
    de_fp_env_init_thread();
    de_task_system_desc d = {0};
    d.worker_count = workers;
    d.background_worker_count = bg;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    return ts;
}

/* Every submitted task ran once, and is counted as completed by the time its counter's waiter
 * returns: a task counts its completion before it takes itself off its counter. Graph nodes did
 * not before DENG-388. A node finished its run's `done` counter inside its own function, so a
 * waiter on `done` could get here while the last node's thread was still between the two, and
 * on a loaded Windows runner that thread, a below-normal background worker, waited seconds for
 * CPU time ("one task short", DENG-375 and DENG-388). Node tasks now carry `done` as their task
 * counter, so the check is exact, with no grace period. */
static void expect_quiescent(void) {
    const de__task_stats st = de__task_get_stats(g_ts);
    printf("stress: %llu tasks submitted, %llu completed, %llu record blocks; background boost: "
           "%llu raises, %.1f ms\n",
           (unsigned long long)st.submitted, (unsigned long long)st.completed,
           (unsigned long long)st.record_blocks, (unsigned long long)st.bg_boosts,
           (double)st.bg_boost_ns / 1e6);
    EXPECT_EQ(st.submitted, st.completed);
}

/* ---------- stress.mixed_3s ---------- */

#define MIXED_FOREIGN 2u

TEST(stress, mixed_3s) {
    de_atomic_store_u32(&g_fail, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_stop, 0, DE_ATOMIC_RELAXED);
    g_ts = make_ts(0, 2);

    static sgraph main_graph, side_graph, inner_graph;
    sgraph_build(&main_graph, g_ts, 0xA11CEu, true);
    sgraph_build(&side_graph, g_ts, 0xB0Bu, true);
    sgraph_build(&inner_graph, g_ts, 0xC0DEu, false);

    static foreign fs[MIXED_FOREIGN];
    for (u32 i = 0; i < MIXED_FOREIGN; ++i)
        foreign_start(&fs[i], i, 0, true, false);

    u64 rng = 0x9E3779B97F4A7C15ull;
    u64 ops[8] = {0};
    const u64 t_end = de_time_now_ns() + stress_seconds_ns();
    while (de_time_now_ns() < t_end && de_atomic_load_u32(&g_fail, DE_ATOMIC_RELAXED) == 0) {
        const u32 op = rng_below(&rng, 7);
        ++ops[op];
        switch (op) {
        case 0:
            op_nested();
            break;
        case 1:
            sgraph_start(&main_graph);
            sgraph_finish(&main_graph);
            break;
        case 2:
            op_pfor(&rng);
            break;
        case 3:
            op_main_tasks(&rng);
            break;
        case 4:
            op_background(&rng);
            break;
        case 5: {
            /* Two graphs and other work in flight at once. */
            sgraph_start(&main_graph);
            sgraph_start(&side_graph);
            op_pfor(&rng);
            op_nested();
            sgraph_finish(&side_graph);
            sgraph_finish(&main_graph);
            break;
        }
        default: {
            /* A graph run and waited on inside a frame task. */
            inner_graph_op io;
            io.g = &inner_graph;
            memset(&io.counter, 0, sizeof io.counter);
            de_task_submit(g_ts, DE_TASK_POOL_FRAME, run_graph_task, &io, &io.counter);
            op_pfor(&rng);
            de_task_wait(g_ts, &io.counter);
            break;
        }
        }
    }
    de_atomic_store_u32(&g_stop, 1, DE_ATOMIC_RELEASE);
    foreign_join_all(fs, MIXED_FOREIGN);

    printf("stress: ops nested %llu, graph %llu, pfor %llu, main %llu, background %llu, "
           "overlap %llu, inner-graph %llu; graph runs %llu/%llu/%llu\n",
           (unsigned long long)ops[0], (unsigned long long)ops[1], (unsigned long long)ops[2],
           (unsigned long long)ops[3], (unsigned long long)ops[4], (unsigned long long)ops[5],
           (unsigned long long)ops[6], (unsigned long long)main_graph.run_count,
           (unsigned long long)side_graph.run_count, (unsigned long long)inner_graph.run_count);
    EXPECT_EQ(de_atomic_load_u32(&g_fail, DE_ATOMIC_RELAXED), 0u);
    for (u32 i = 0; i < 7; ++i)
        EXPECT_GT(ops[i], 0u);
    expect_quiescent();

    sgraph_destroy(&inner_graph);
    sgraph_destroy(&side_graph);
    sgraph_destroy(&main_graph);
    de_task_system_destroy(g_ts);
    g_ts = NULL;
}

/* ---------- stress.foreign_thread_submits ---------- */

#define FOREIGN_THREADS    4u
#define FOREIGN_ITERATIONS 1000u

TEST(stress, foreign_thread_submits) {
    de_atomic_store_u32(&g_fail, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_stop, 0, DE_ATOMIC_RELAXED);
    g_ts = make_ts(3, 2);

    static foreign fs[FOREIGN_THREADS];
    for (u32 i = 0; i < FOREIGN_THREADS; ++i)
        foreign_start(&fs[i], 100 + i, FOREIGN_ITERATIONS, true, i < 2);
    foreign_join_all(fs, FOREIGN_THREADS);

    u64 frame = 0, bg = 0, mt = 0;
    for (u32 i = 0; i < FOREIGN_THREADS; ++i) {
        frame += fs[i].frame_sent;
        bg += fs[i].bg_sent;
        mt += fs[i].main_sent;
    }
    printf("stress: foreign threads sent %llu frame, %llu background, %llu main-thread tasks\n",
           (unsigned long long)frame, (unsigned long long)bg, (unsigned long long)mt);
    EXPECT_EQ(de_atomic_load_u32(&g_fail, DE_ATOMIC_RELAXED), 0u);
    EXPECT_GT(mt, 0u);
    expect_quiescent();
    de_task_system_destroy(g_ts);
    g_ts = NULL;
}
