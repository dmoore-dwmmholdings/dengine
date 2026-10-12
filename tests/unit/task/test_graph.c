/* Unit tests of task graphs (src/task/graph.c; DENG-292).
 *
 * Every node records a start and an end sequence number from one global atomic counter. An edge
 * before -> after is satisfied when end[before] < start[after]: the counter's modification order
 * follows happens-before, so a successor that really started after its predecessor finished
 * always draws a larger number. */
#include "de_test.h"

#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "platform/de_atomic.h"

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(disable : 4611) /* setjmp and C++ destructors: this is C */
#endif

#define MAX_NODES 256u

static de_task_system *make_ts(u32 workers, const de_allocator *a) {
    de_fp_env_init_thread();
    de_task_system_desc d = {0};
    d.worker_count = workers;
    d.background_worker_count = 1;
    d.allocator = a;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    ASSERT_TRUE(ts != NULL);
    return ts;
}

/* ---------- Recording nodes ---------- */

static de_atomic_u32 g_seq;
static de_atomic_u32 g_start[MAX_NODES];
static de_atomic_u32 g_end[MAX_NODES];
static de_atomic_u32 g_runs[MAX_NODES];
static de_atomic_u32 g_bad_thread; /* main-thread nodes that ran elsewhere */
static bool g_main_node[MAX_NODES];

static void rec_fn(void *user) {
    const u32 i = (u32)(uintptr_t)user;
    if (g_main_node[i] && de_task_worker_index() != 0)
        de_atomic_fetch_add_u32(&g_bad_thread, 1, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_start[i], de_atomic_fetch_add_u32(&g_seq, 1, DE_ATOMIC_RELAXED),
                        DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&g_runs[i], 1, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_end[i], de_atomic_fetch_add_u32(&g_seq, 1, DE_ATOMIC_RELAXED),
                        DE_ATOMIC_RELAXED);
}

static void rec_reset(u32 n) {
    de_atomic_store_u32(&g_seq, 0, DE_ATOMIC_RELAXED);
    for (u32 i = 0; i < n; ++i) {
        de_atomic_store_u32(&g_start[i], 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u32(&g_end[i], 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u32(&g_runs[i], 0, DE_ATOMIC_RELAXED);
    }
}

typedef struct edge {
    u32 before, after;
} edge;

/* Adds n recording nodes (pool per g_main_node) and the edges; compiles. */
static de_task_graph *build_graph(de_task_system *ts, u32 n, const edge *edges, u32 e,
                                  de_task_node *h) {
    de_task_graph *g = NULL;
    ASSERT_EQ(de_task_graph_create(ts, &g), DE_OK);
    for (u32 i = 0; i < n; ++i) {
        de_task_node_desc d = {0};
        d.name = "rec";
        d.fn = rec_fn;
        d.user = (void *)(uintptr_t)i;
        d.pool = g_main_node[i] ? DE_TASK_POOL_MAIN_THREAD : DE_TASK_POOL_FRAME;
        h[i] = de_task_graph_add(g, &d);
        ASSERT_TRUE(DE_HANDLE_VALID(h[i]));
    }
    for (u32 k = 0; k < e; ++k)
        de_task_graph_depend(g, h[edges[k].before], h[edges[k].after]);
    ASSERT_EQ(de_task_graph_compile(g), DE_OK);
    return g;
}

/* Runs `g` `runs` times from the calling thread; after each run, every node ran once and every
 * edge holds. Returns the number of violations. */
static u32 run_checked(de_task_system *ts, de_task_graph *g, u32 n, const edge *edges, u32 e,
                       u32 runs) {
    u32 bad = 0;
    de_task_counter done = {0};
    for (u32 r = 0; r < runs; ++r) {
        rec_reset(n);
        de_task_graph_run(g, &done);
        de_task_wait(ts, &done);
        for (u32 i = 0; i < n; ++i)
            bad += de_atomic_load_u32(&g_runs[i], DE_ATOMIC_RELAXED) != 1;
        for (u32 k = 0; k < e; ++k)
            bad += de_atomic_load_u32(&g_end[edges[k].before], DE_ATOMIC_RELAXED) >=
                   de_atomic_load_u32(&g_start[edges[k].after], DE_ATOMIC_RELAXED);
        if (bad != 0)
            break;
    }
    return bad;
}

#define RUNS 10000u

/* ---------- diamond, chain, fanout ---------- */

TEST(graph, diamond) {
    de_task_system *ts = make_ts(0, NULL);
    memset(g_main_node, 0, sizeof g_main_node);
    /* A -> B, A -> C, B -> D, C -> D, plus a duplicate A -> B (counts once). */
    static const edge edges[] = {{0, 1}, {0, 2}, {1, 3}, {2, 3}, {0, 1}};
    de_task_node h[4];
    de_task_graph *g = build_graph(ts, 4, edges, 5, h);
    EXPECT_EQ(run_checked(ts, g, 4, edges, 5, RUNS), 0u);
    de_task_graph_destroy(g);
    de_task_system_destroy(ts);
}

TEST(graph, chain) {
    de_task_system *ts = make_ts(0, NULL);
    memset(g_main_node, 0, sizeof g_main_node);
    enum { N = 64 };
    static edge edges[N - 1];
    /* Declared back to front, so node order and edge order do not hide a bug. */
    for (u32 i = 0; i < N - 1; ++i)
        edges[i] = (edge){N - 2 - i, N - 1 - i};
    de_task_node h[N];
    de_task_graph *g = build_graph(ts, N, edges, N - 1, h);
    EXPECT_EQ(run_checked(ts, g, N, edges, N - 1, RUNS), 0u);
    de_task_graph_destroy(g);
    de_task_system_destroy(ts);
}

TEST(graph, fanout) {
    de_task_system *ts = make_ts(0, NULL);
    memset(g_main_node, 0, sizeof g_main_node);
    enum { W = 128, N = W + 2 };
    static edge edges[2 * W];
    for (u32 i = 0; i < W; ++i) {
        edges[2 * i] = (edge){0, 1 + i};
        edges[2 * i + 1] = (edge){1 + i, N - 1};
    }
    de_task_node h[N];
    de_task_graph *g = build_graph(ts, N, edges, 2 * W, h);
    EXPECT_EQ(run_checked(ts, g, N, edges, 2 * W, RUNS), 0u);
    de_task_graph_destroy(g);
    de_task_system_destroy(ts);
}

/* ---------- cycle_detect ---------- */

static void noop_fn(void *user) {
    (void)user;
}

static de_task_node add_named(de_task_graph *g, const char *name) {
    de_task_node_desc d = {0};
    d.name = name;
    d.fn = noop_fn;
    return de_task_graph_add(g, &d);
}

static bool error_has(const char *needle) {
    const de_error *e = de_error_last();
    return e != NULL && strstr(e->message, needle) != NULL;
}

TEST(graph, cycle_detect) {
    de_task_system *ts = make_ts(2, NULL);
    de_task_graph *g = NULL;

    /* a -> b -> c -> a, and c -> d downstream of the cycle. */
    ASSERT_EQ(de_task_graph_create(ts, &g), DE_OK);
    de_task_node a = add_named(g, "a"), b = add_named(g, "b"), c = add_named(g, "c"),
                 d = add_named(g, "d");
    de_task_graph_depend(g, a, b);
    de_task_graph_depend(g, b, c);
    de_task_graph_depend(g, c, a);
    de_task_graph_depend(g, c, d);
    de_error_clear();
    EXPECT_EQ(de_task_graph_compile(g), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_has("cycle: a -> b -> c -> a"));
    EXPECT_TRUE(error_has("4 of 4 nodes"));
    de_task_graph_destroy(g);

    /* A root in front of a two-node cycle: the root is not named. */
    ASSERT_EQ(de_task_graph_create(ts, &g), DE_OK);
    de_task_node r = add_named(g, "root"), x = add_named(g, "x"), y = add_named(g, "y");
    de_task_graph_depend(g, r, x);
    de_task_graph_depend(g, x, y);
    de_task_graph_depend(g, y, x);
    EXPECT_EQ(de_task_graph_compile(g), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_has("cycle: x -> y -> x"));
    EXPECT_FALSE(error_has("root"));
    de_task_graph_destroy(g);

    /* A self edge, on unnamed nodes. */
    ASSERT_EQ(de_task_graph_create(ts, &g), DE_OK);
    de_task_node u0 = add_named(g, NULL), u1 = add_named(g, NULL);
    de_task_graph_depend(g, u0, u1);
    de_task_graph_depend(g, u1, u1);
    EXPECT_EQ(de_task_graph_compile(g), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_has("cycle: #1 -> #1"));
    de_task_graph_destroy(g);

    /* Bad arguments to add and depend are reported by compile. */
    de_task_graph *g2 = NULL;
    ASSERT_EQ(de_task_graph_create(ts, &g), DE_OK);
    ASSERT_EQ(de_task_graph_create(ts, &g2), DE_OK);
    de_task_node ok = add_named(g, "ok");
    de_task_node other = add_named(g2, "other");
    EXPECT_TRUE(DE_HANDLE_VALID(ok));
    de_task_graph_depend(g, ok, other); /* a handle of another graph */
    EXPECT_EQ(de_task_graph_compile(g), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_has("depend"));
    de_task_graph_destroy(g);
    EXPECT_FALSE(DE_HANDLE_VALID(de_task_graph_add(g2, NULL)));
    de_task_node_desc nofn = {0};
    EXPECT_FALSE(DE_HANDLE_VALID(de_task_graph_add(g2, &nofn)));
    EXPECT_EQ(de_task_graph_compile(g2), DE_ERR_INVALID_ARGUMENT);
    de_task_graph_destroy(g2);

    EXPECT_EQ(de_task_graph_create(ts, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_task_graph_create(NULL, &g), DE_ERR_INVALID_ARGUMENT);
    de_task_graph_destroy(NULL);

    /* An empty graph compiles and runs as nothing. */
    ASSERT_EQ(de_task_graph_create(ts, &g), DE_OK);
    EXPECT_EQ(de_task_graph_compile(g), DE_OK);
    de_task_counter done = {0};
    de_task_graph_run(g, &done);
    EXPECT_TRUE(de_task_done(&done));
    de_task_graph_destroy(g);
    de_task_system_destroy(ts);
}

/* ---------- rerun_no_alloc ---------- */

/* Warm-up barrier: one task per frame worker (the main thread runs one inside its wait), each
 * waits until all have started, then submits a child, so every worker's record pool exists
 * before the measured runs. Otherwise a worker that happened to submit no node during the
 * warm-up runs allocates its first record block later. */
typedef struct warm_ctx {
    de_task_system *ts;
    u32 n;
    de_atomic_u32 started;
    de_atomic_u32 timeouts;
    de_task_counter children;
} warm_ctx;

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
    de_task_submit(w->ts, DE_TASK_POOL_FRAME, noop_fn, NULL, &w->children);
}

static void warm_all_workers(de_task_system *ts) {
    static warm_ctx w;
    memset(&w, 0, sizeof w);
    w.ts = ts;
    w.n = de_task_worker_count(ts);
    de_task_counter c = {0};
    for (u32 i = 0; i < w.n; ++i)
        de_task_submit(ts, DE_TASK_POOL_FRAME, warm_fn, &w, &c);
    de_task_wait(ts, &c);
    de_task_wait(ts, &w.children);
    EXPECT_EQ(de_atomic_load_u32(&w.timeouts, DE_ATOMIC_RELAXED), 0u);
}

TEST(graph, rerun_no_alloc) {
    static de_tracking_allocator tr;
    const de_allocator a = de_tracking_allocator_init(&tr, NULL, 0);
    de_task_system *ts = make_ts(0, &a);
    memset(g_main_node, 0, sizeof g_main_node);

    /* 200 nodes, a fixed pseudo-random DAG: every node after the first 8 depends on 1-3 earlier
     * ones. */
    enum { N = 200 };
    static edge edges[3 * N];
    u32 e = 0;
    u64 x = 0x9E3779B97F4A7C15ull;
    for (u32 i = 8; i < N; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const u32 k = 1 + (u32)(x % 3);
        for (u32 j = 0; j < k; ++j)
            edges[e++] = (edge){(u32)((x >> (8 * (j + 1))) % i), i};
    }
    de_task_node h[N];
    de_task_graph *g = build_graph(ts, N, edges, e, h);

    /* Warm-up: every worker's record pool exists, and pools and deques grow to the graph's
     * width. */
    warm_all_workers(ts);
    EXPECT_EQ(run_checked(ts, g, N, edges, e, 50), 0u);
    const de_tracking_stats before = de_tracking_allocator_stats(&tr);
    de_task_counter done = {0};
    for (u32 r = 0; r < 1000; ++r) {
        de_task_graph_run(g, &done);
        de_task_wait(ts, &done);
    }
    const de_tracking_stats after = de_tracking_allocator_stats(&tr);
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    EXPECT_EQ(after.free_count, before.free_count);
    EXPECT_EQ(run_checked(ts, g, N, edges, e, 10), 0u);

    de_task_graph_destroy(g);
    de_task_system_destroy(ts);
    EXPECT_EQ(de_tracking_allocator_stats(&tr).blocks_live, 0u);
}

/* ---------- main_thread_nodes ---------- */

typedef struct outer_run {
    de_task_system *ts;
    de_task_graph *g;
} outer_run;

/* A frame task that runs a graph and waits on it (wait-and-help on a worker). */
static void run_graph_task(void *p) {
    outer_run *o = (outer_run *)p;
    de_task_counter done = {0};
    de_task_graph_run(o->g, &done);
    de_task_wait(o->ts, &done);
}

TEST(graph, main_thread_nodes) {
    de_task_system *ts = make_ts(3, NULL);
    /* F0 -> M1 -> F2 -> M3, F0 -> F4 -> M3, M1 -> F5 (M = main-thread node). */
    memset(g_main_node, 0, sizeof g_main_node);
    g_main_node[1] = g_main_node[3] = true;
    static const edge edges[] = {{0, 1}, {1, 2}, {2, 3}, {0, 4}, {4, 3}, {1, 5}};
    de_task_node h[6];
    de_task_graph *g = build_graph(ts, 6, edges, 6, h);
    de_atomic_store_u32(&g_bad_thread, 0, DE_ATOMIC_RELAXED);

    /* Run from the main thread: its wait drains the main-thread nodes. */
    EXPECT_EQ(run_checked(ts, g, 6, edges, 6, 1000), 0u);

    /* Run from a worker: the main thread's wait on the outer task still runs them. */
    outer_run o = {ts, g};
    for (u32 r = 0; r < 200; ++r) {
        rec_reset(6);
        de_task_counter c = {0};
        de_task_submit(ts, DE_TASK_POOL_FRAME, run_graph_task, &o, &c);
        de_task_wait(ts, &c);
        for (u32 i = 0; i < 6; ++i)
            ASSERT_EQ(de_atomic_load_u32(&g_runs[i], DE_ATOMIC_RELAXED), 1u);
    }
    EXPECT_EQ(de_atomic_load_u32(&g_bad_thread, DE_ATOMIC_RELAXED), 0u);
    de_task_graph_destroy(g);
    de_task_system_destroy(ts);
    memset(g_main_node, 0, sizeof g_main_node);
}

/* ---------- run_from_worker ---------- */

static void count_fn(void *user) {
    de_atomic_fetch_add_u32((de_atomic_u32 *)user, 1, DE_ATOMIC_RELAXED);
}

TEST(graph, run_from_worker) {
    de_task_system *ts = make_ts(0, NULL);
    /* Eight graphs (a root fanning out to 15 nodes), each run and waited on inside its own frame
     * task, all at once, 200 times. */
    enum { G = 8, N = 16, ROUNDS = 200 };
    static de_atomic_u32 hits[G * N];
    static de_task_graph *graphs[G];
    static outer_run runs[G];
    for (u32 k = 0; k < G; ++k) {
        ASSERT_EQ(de_task_graph_create(ts, &graphs[k]), DE_OK);
        de_task_node root = {0};
        for (u32 i = 0; i < N; ++i) {
            de_atomic_store_u32(&hits[k * N + i], 0, DE_ATOMIC_RELAXED);
            de_task_node_desc d = {0};
            d.name = "count";
            d.fn = count_fn;
            d.user = &hits[k * N + i];
            const de_task_node h = de_task_graph_add(graphs[k], &d);
            if (i == 0)
                root = h;
            else
                de_task_graph_depend(graphs[k], root, h);
        }
        ASSERT_EQ(de_task_graph_compile(graphs[k]), DE_OK);
        runs[k] = (outer_run){ts, graphs[k]};
    }
    for (u32 r = 0; r < ROUNDS; ++r) {
        de_task_counter c = {0};
        for (u32 k = 0; k < G; ++k)
            de_task_submit(ts, DE_TASK_POOL_FRAME, run_graph_task, &runs[k], &c);
        de_task_wait(ts, &c);
    }
    for (u32 i = 0; i < G * N; ++i)
        EXPECT_EQ(de_atomic_load_u32(&hits[i], DE_ATOMIC_RELAXED), (u32)ROUNDS);
    for (u32 k = 0; k < G; ++k)
        de_task_graph_destroy(graphs[k]);
    de_task_system_destroy(ts);
}

/* ---------- dirty_after_modify ---------- */

#if defined(DE_DEBUG) || defined(DE_DEV)
#define ASSERTS_ON 1
#else
#define ASSERTS_ON 0
#endif

static jmp_buf g_panic_jmp;
static int g_panic_calls;
static char g_panic_msg[512];

static void longjmp_hook(const de_panic_info *info, void *user) {
    (void)user;
    ++g_panic_calls;
    snprintf(g_panic_msg, sizeof g_panic_msg, "%s", info->message);
    longjmp(g_panic_jmp, 1);
}

static i32 run_expect_panic_thread(void *p) {
    de_task_counter done = {0};
    if (setjmp(g_panic_jmp) == 0)
        de_task_graph_run((de_task_graph *)p, &done);
    return de_task_done(&done) ? 0 : 1; /* nothing may have started */
}

/* Runs `g` expecting the not-compiled assertion (asserting builds). A thread that left a panic by
 * longjmp still counts as panicking (a second DE_PANIC on it aborts at once), so each check runs
 * on a fresh thread; a graph may be run from any thread. */
static void expect_run_asserts(de_task_graph *g) {
    const int before = g_panic_calls;
    g_panic_msg[0] = '\0';
    de_panic_set_hook(longjmp_hook, NULL);
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(run_expect_panic_thread, g, "panic", DE_THREAD_NORMAL, 0, &t),
              DE_OK);
    EXPECT_EQ(de_thread_join(t), 0);
    de_panic_set_hook(NULL, NULL);
    EXPECT_EQ(g_panic_calls, before + 1);
    EXPECT_TRUE(strstr(g_panic_msg, "compile") != NULL);
}

TEST(graph, dirty_after_modify) {
    de_task_system *ts = make_ts(2, NULL);
    memset(g_main_node, 0, sizeof g_main_node);
    de_task_graph *g = NULL;
    ASSERT_EQ(de_task_graph_create(ts, &g), DE_OK);
    de_task_node h[3];
    for (u32 i = 0; i < 2; ++i) {
        de_task_node_desc d = {0};
        d.name = "rec";
        d.fn = rec_fn;
        d.user = (void *)(uintptr_t)i;
        h[i] = de_task_graph_add(g, &d);
    }
    de_task_graph_depend(g, h[0], h[1]);
    if (ASSERTS_ON)
        expect_run_asserts(g); /* never compiled */
    ASSERT_EQ(de_task_graph_compile(g), DE_OK);
    static const edge e01[] = {{0, 1}};
    EXPECT_EQ(run_checked(ts, g, 2, e01, 1, 10), 0u);

    /* Adding a node marks the graph dirty. */
    de_task_node_desc d = {0};
    d.name = "rec";
    d.fn = rec_fn;
    d.user = (void *)(uintptr_t)2;
    h[2] = de_task_graph_add(g, &d);
    if (ASSERTS_ON) {
        expect_run_asserts(g);
        ASSERT_EQ(de_task_graph_compile(g), DE_OK);
    }
    /* (Release builds compile inside run.) */
    static const edge e3[] = {{0, 1}};
    EXPECT_EQ(run_checked(ts, g, 3, e3, 1, 10), 0u);

    /* So does adding an edge: 1 -> 2 must hold after the recompile. */
    de_task_graph_depend(g, h[1], h[2]);
    if (ASSERTS_ON) {
        expect_run_asserts(g);
        ASSERT_EQ(de_task_graph_compile(g), DE_OK);
    }
    static const edge e012[] = {{0, 1}, {1, 2}};
    EXPECT_EQ(run_checked(ts, g, 3, e012, 2, 1000), 0u);

    /* A failed compile leaves the graph not runnable. */
    de_task_graph_depend(g, h[2], h[0]);
    EXPECT_EQ(de_task_graph_compile(g), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_has("cycle: rec -> rec -> rec -> rec"));
    if (ASSERTS_ON)
        expect_run_asserts(g);

    de_task_graph_destroy(g);
    de_task_system_destroy(ts);
}
