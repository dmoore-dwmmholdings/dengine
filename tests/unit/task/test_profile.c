/* Graph node statistics and profiling hooks (src/task/profile.c; DENG-293).
 *
 * Each node sleeps a fixed time and measures its own sleep with de_time_now_ns. The statistics
 * time the node's whole fn, so they must cover that sleep and stay within 10% of it. Comparing
 * with the node's own measurement instead of the requested sleep keeps the test independent of
 * how late the OS wakes the thread on a loaded machine. */
#include "de_test.h"

#include <dengine/de_debug.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include <stdint.h>
#include <stdio.h>

#include "platform/de_atomic.h"

#if defined(DE_DEV) || defined(DE_DEBUG)
#define HAVE_STATS 1
#else
#define HAVE_STATS 0
#endif

#define NODES 6u
#define RUNS  5u

typedef struct sleeper {
    u64 sleep_ns;
    de_atomic_u64 measured_last;
    de_atomic_u64 measured_total;
} sleeper;

static sleeper g_sleepers[NODES];

static void sleep_node(void *user) {
    sleeper *s = (sleeper *)user;
    const u64 t0 = de_time_now_ns();
    de_sleep_ns(s->sleep_ns);
    const u64 dt = de_time_now_ns() - t0;
    de_atomic_store_u64(&s->measured_last, dt, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u64(&s->measured_total, dt, DE_ATOMIC_RELAXED);
}

static de_task_system *make_ts(void) {
    de_fp_env_init_thread();
    de_task_system_desc d = {0};
    d.worker_count = 4;
    d.background_worker_count = 1;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    return ts;
}

#if HAVE_STATS
/* |a - b| <= b / 10 */
static bool within_10pct(u64 a, u64 b) {
    const u64 d = a > b ? a - b : b - a;
    return d * 10u <= b;
}
#endif

TEST(prof, node_stats_sleeping) {
    de_task_system *ts = make_ts();
    de_task_graph *g = NULL;
    ASSERT_EQ(de_task_graph_create(ts, &g), DE_OK);
    /* Two chains of three nodes, 5..30 ms; one main-thread node and one background node. */
    de_task_node h[NODES];
    for (u32 i = 0; i < NODES; ++i) {
        g_sleepers[i].sleep_ns = (u64)(i + 1) * 5000000u;
        de_atomic_store_u64(&g_sleepers[i].measured_last, 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u64(&g_sleepers[i].measured_total, 0, DE_ATOMIC_RELAXED);
        de_task_node_desc d = {0};
        d.name = "prof.sleep";
        d.fn = sleep_node;
        d.user = &g_sleepers[i];
        d.pool = i == 2 ? DE_TASK_POOL_MAIN_THREAD
                        : (i == 5 ? DE_TASK_POOL_BACKGROUND : DE_TASK_POOL_FRAME);
        h[i] = de_task_graph_add(g, &d);
    }
    de_task_graph_depend(g, h[0], h[1]);
    de_task_graph_depend(g, h[1], h[2]);
    de_task_graph_depend(g, h[3], h[4]);
    de_task_graph_depend(g, h[4], h[5]);
    ASSERT_EQ(de_task_graph_compile(g), DE_OK);

    /* Before the first run: zero, never ran. */
    de_task_node_stats st;
    const de_result r0 = de_task_graph_node_stats(g, h[0], &st);
    EXPECT_EQ(r0, HAVE_STATS ? DE_OK : DE_ERR_UNSUPPORTED);
    EXPECT_EQ(st.runs, 0u);
    EXPECT_EQ(st.last_ns, 0u);
    EXPECT_EQ(st.last_worker, DE_TASK_WORKER_NONE);

    de_task_counter done = {0};
    for (u32 r = 0; r < RUNS; ++r) {
        de_task_graph_run(g, &done);
        de_task_wait(ts, &done);
    }
    for (u32 i = 0; i < NODES; ++i) {
        const de_result r = de_task_graph_node_stats(g, h[i], &st);
#if HAVE_STATS
        ASSERT_EQ(r, DE_OK);
        const u64 last = de_atomic_load_u64(&g_sleepers[i].measured_last, DE_ATOMIC_RELAXED);
        const u64 avg = de_atomic_load_u64(&g_sleepers[i].measured_total, DE_ATOMIC_RELAXED) / RUNS;
        printf("prof: node %u sleep %llu ns: last %llu (own %llu), avg %llu (own %llu), "
               "worker %u\n",
               i, (unsigned long long)g_sleepers[i].sleep_ns, (unsigned long long)st.last_ns,
               (unsigned long long)last, (unsigned long long)st.avg_ns, (unsigned long long)avg,
               st.last_worker);
        EXPECT_EQ(st.runs, (u64)RUNS);
        EXPECT_GE(st.last_ns, g_sleepers[i].sleep_ns);
        EXPECT_GE(st.last_ns, last);
        EXPECT_TRUE(within_10pct(st.last_ns, last));
        EXPECT_TRUE(within_10pct(st.avg_ns, avg));
        if (i == 2)
            EXPECT_EQ(st.last_worker, 0u); /* main-thread node */
        else if (i == 5)
            EXPECT_EQ(st.last_worker, DE_TASK_WORKER_NONE); /* background worker */
        else
            EXPECT_LT(st.last_worker, de_task_worker_count(ts));
#else
        EXPECT_EQ(r, DE_ERR_UNSUPPORTED);
        EXPECT_EQ(st.runs, 0u);
#endif
    }

    /* A recompile starts the statistics over. */
    ASSERT_EQ(de_task_graph_compile(g), DE_OK);
    (void)de_task_graph_node_stats(g, h[0], &st);
    EXPECT_EQ(st.runs, 0u);

    de_task_graph_destroy(g);
    de_task_system_destroy(ts);
}

static void noop(void *user) {
    (void)user;
}

TEST(prof, node_stats_errors) {
    de_task_system *ts = make_ts();
    de_task_graph *g = NULL, *other = NULL;
    ASSERT_EQ(de_task_graph_create(ts, &g), DE_OK);
    ASSERT_EQ(de_task_graph_create(ts, &other), DE_OK);
    de_task_node_desc d = {0};
    d.fn = noop; /* no name: the profile zone falls back to "task node" */
    const de_task_node a = de_task_graph_add(g, &d);
    const de_task_node b = de_task_graph_add(other, &d);
    de_task_node_stats st;

    /* Not compiled yet. */
    EXPECT_EQ(de_task_graph_node_stats(g, a, &st), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_task_graph_compile(g), DE_OK);
    ASSERT_EQ(de_task_graph_compile(other), DE_OK);
    EXPECT_EQ(de_task_graph_node_stats(NULL, a, &st), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_task_graph_node_stats(g, a, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_task_graph_node_stats(g, b, &st), DE_ERR_INVALID_ARGUMENT); /* other graph */
    const de_task_node zero = {0};
    EXPECT_EQ(de_task_graph_node_stats(g, zero, &st), DE_ERR_INVALID_ARGUMENT);

    /* Added after the compile: not a compiled node until the next compile. */
    const de_task_node c = de_task_graph_add(g, &d);
    EXPECT_EQ(de_task_graph_node_stats(g, c, &st), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_task_graph_compile(g), DE_OK);
    EXPECT_EQ(de_task_graph_node_stats(g, c, &st), HAVE_STATS ? DE_OK : DE_ERR_UNSUPPORTED);

    de_task_counter done = {0};
    de_task_graph_run(g, &done);
    de_task_wait(ts, &done);
    EXPECT_EQ(de_task_graph_node_stats(g, a, &st), HAVE_STATS ? DE_OK : DE_ERR_UNSUPPORTED);
    EXPECT_EQ(st.runs, HAVE_STATS ? 1u : 0u);

    de_task_graph_destroy(other);
    de_task_graph_destroy(g);
    de_task_system_destroy(ts);
}

#ifdef DE_PROFILE
/* Profile builds link core's de_prof_* functions (src/core/prof/prof.c). This test never calls
 * de_prof_init, so every call must be a harmless no-op. */
TEST(prof, fallbacks_link) {
    static const de_prof_srcloc loc = {"prof.test", "fallbacks_link", __FILE__, 1u, 0u};
    const de_prof_zone z = de_prof_zone_begin_(&loc);
    de_prof_zone_end_(z);
    de_prof_plot_("prof.test", 1.0);
    de_prof_frame_mark_(NULL);
    de_prof_alloc_(&loc, sizeof loc, "prof");
    de_prof_free_(&loc, "prof");
    DE_PROFILE_PLOT("prof.test", 2);
    EXPECT_TRUE(true);
}
#endif
