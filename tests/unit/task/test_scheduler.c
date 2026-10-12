/* Unit tests of the task scheduler (src/task/scheduler.c, worker.c; DENG-291).
 *
 * sched.background_isolation asserts a wall-clock bound: it runs serially (see this directory's
 * CMakeLists.txt) and loosens the bound when DE_CI or CI is set (Trama "Decision: Timing tests
 * run serially and loosen their bounds on CI"). */
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#include "de_test.h"

#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform/de_atomic.h"
#include "task/task_internal.h"

#define MS_NS(ms) ((u64)(ms) * 1000ull * 1000ull)

static bool env_flag(const char *name) {
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' && v[0] != '0' && v[0] != 'f' && v[0] != 'F';
}

static bool on_ci(void) {
    return env_flag("DE_CI") || env_flag("CI");
}

static de_task_system *make_ts(u32 workers, u32 bg, const de_allocator *a) {
    de_fp_env_init_thread();
    de_task_system_desc d = {0};
    d.worker_count = workers;
    d.background_worker_count = bg;
    d.allocator = a;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    ASSERT_TRUE(ts != NULL);
    return ts;
}

/* Spins until `*flag` reaches `want` or `timeout_ns` passes. */
static bool spin_until_u32(de_atomic_u32 *flag, u32 want, u64 timeout_ns) {
    const u64 end = de_time_now_ns() + timeout_ns;
    while (de_atomic_load_u32(flag, DE_ATOMIC_ACQUIRE) < want) {
        if (de_time_now_ns() > end)
            return false;
        de_cpu_pause();
    }
    return true;
}

/* ---------- create_options ---------- */

static de_atomic_u32 g_ext_index;

static i32 ext_read_index(void *p) {
    (void)p;
    de_atomic_store_u32(&g_ext_index, de_task_worker_index(), DE_ATOMIC_RELEASE);
    return 0;
}

TEST(sched, create_options) {
    de_fp_env_init_thread();
    EXPECT_EQ(de_task_worker_index(), DE_TASK_WORKER_NONE);
    EXPECT_EQ(de_task_system_create(NULL, NULL), DE_ERR_INVALID_ARGUMENT);

    de_task_system_desc d = {0};
    d.worker_count = 2;
    d.background_worker_count = 1;
    d.pin_workers = true; /* macOS cannot pin: logs once and runs unpinned */
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    EXPECT_EQ(de_task_worker_count(ts), 3u);
    EXPECT_EQ(de_task_worker_index(), 0u);

    /* One thread, one task system. */
    de_task_system *ts2 = NULL;
    EXPECT_EQ(de_task_system_create(&d, &ts2), DE_ERR_BUSY);
    EXPECT_TRUE(ts2 == NULL);

    de_thread *t = NULL;
    de_atomic_store_u32(&g_ext_index, 0, DE_ATOMIC_RELAXED);
    ASSERT_EQ(de_thread_create(ext_read_index, NULL, "ext", DE_THREAD_NORMAL, 0, &t), DE_OK);
    de_thread_join(t);
    EXPECT_EQ(de_atomic_load_u32(&g_ext_index, DE_ATOMIC_ACQUIRE), DE_TASK_WORKER_NONE);

    de_task_system_destroy(ts);
    EXPECT_EQ(de_task_worker_index(), DE_TASK_WORKER_NONE);
    de_task_system_destroy(NULL);

    /* Defaults: cores - 1 workers (at least 1). */
    ts = make_ts(0, 0, NULL);
    const u32 cores = de_cpu_logical_cores();
    EXPECT_EQ(de_task_worker_count(ts), cores > 1 ? cores : 2u);
    de_task_system_destroy(ts);
}

/* ---------- million_tasks ---------- */

#define MILLION 1000000u
static de_atomic_u32 *g_hits;

static void hit_fn(void *user) {
    de_atomic_fetch_add_u32(&g_hits[(uintptr_t)user], 1, DE_ATOMIC_RELAXED);
}

TEST(sched, million_tasks) {
    de_task_system *ts = make_ts(0, 1, NULL);
    g_hits = (de_atomic_u32 *)calloc(MILLION, sizeof(de_atomic_u32));
    ASSERT_TRUE(g_hits != NULL);
    de_task_counter c = {0};
    /* Hold the counter open while submitting (the same internal path a task's completion
     * takes), so its only legitimate transition to zero is the last one. Without the hold, fast
     * workers could legitimately empty it between two submits. */
    de__task_counter *ic = (de__task_counter *)(void *)c.opaque;
    de_atomic_fetch_add_u64(&ic->state, 1, DE_ATOMIC_RELAXED);
    for (u32 i = 0; i < MILLION; ++i)
        de_task_submit(ts, DE_TASK_POOL_FRAME, hit_fn, (void *)(uintptr_t)i, &c);
    EXPECT_FALSE(de_task_done(&c));
    de__task_counter_finish(ts, ic);
    de_task_wait(ts, &c);
    ASSERT_TRUE(de_task_done(&c));
    EXPECT_EQ(de_atomic_load_u64(&ic->state, DE_ATOMIC_RELAXED) & 0xFFFFFFFFull, 0ull);

    u32 lost = 0, duplicated = 0;
    for (u32 i = 0; i < MILLION; ++i) {
        const u32 h = de_atomic_load_u32(&g_hits[i], DE_ATOMIC_RELAXED);
        lost += h == 0;
        duplicated += h > 1;
    }
    EXPECT_EQ(lost, 0u);
    EXPECT_EQ(duplicated, 0u);
    const de__task_stats st = de__task_get_stats(ts);
    EXPECT_EQ(st.counter_zero_events, 1ull);
    EXPECT_EQ(st.submitted, (u64)MILLION);
    EXPECT_EQ(st.completed, (u64)MILLION);
    printf("million_tasks: %u workers, %llu record blocks\n", de_task_worker_count(ts),
           (unsigned long long)st.record_blocks);
    de_task_system_destroy(ts);
    free(g_hits);
    g_hits = NULL;
}

/* ---------- wait_helps ---------- */

static de_atomic_u32 g_blocker_started;
static de_atomic_u32 g_blocker_release;
static de_atomic_u32 g_ran_on[128];

static void blocker_fn(void *user) {
    (void)user;
    de_atomic_store_u32(&g_blocker_started, 1, DE_ATOMIC_RELEASE);
    spin_until_u32(&g_blocker_release, 1, MS_NS(10000));
}

static void record_index_fn(void *user) {
    de_atomic_store_u32(&g_ran_on[(uintptr_t)user], de_task_worker_index(), DE_ATOMIC_RELAXED);
}

TEST(sched, wait_helps) {
    de_task_system *ts = make_ts(1, 1, NULL);
    de_atomic_store_u32(&g_blocker_started, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_blocker_release, 0, DE_ATOMIC_RELAXED);
    de_task_counter cb = {0};
    de_task_submit(ts, DE_TASK_POOL_FRAME, blocker_fn, NULL, &cb);
    /* The main thread does not run tasks outside a wait, so worker 1 steals the blocker. */
    ASSERT_TRUE(spin_until_u32(&g_blocker_started, 1, MS_NS(10000)));

    de_task_counter c = {0};
    for (u32 i = 0; i < 128; ++i) {
        de_atomic_store_u32(&g_ran_on[i], DE_TASK_WORKER_NONE - 1, DE_ATOMIC_RELAXED);
        de_task_submit(ts, DE_TASK_POOL_FRAME, record_index_fn, (void *)(uintptr_t)i, &c);
    }
    de_task_wait(ts, &c); /* worker 1 is blocked: only the main thread can run them */
    u32 on_main = 0;
    for (u32 i = 0; i < 128; ++i)
        on_main += de_atomic_load_u32(&g_ran_on[i], DE_ATOMIC_RELAXED) == 0u;
    EXPECT_EQ(on_main, 128u);
    EXPECT_FALSE(de_task_done(&cb));
    de_atomic_store_u32(&g_blocker_release, 1, DE_ATOMIC_RELEASE);
    de_task_wait(ts, &cb);
    EXPECT_TRUE(de_task_done(&cb));
    de_task_system_destroy(ts);
}

/* ---------- nested_submit_wait ---------- */

#define NEST_DEPTH  5u
#define NEST_FANOUT 6u

typedef struct nest_ctx {
    de_task_system *ts;
    de_atomic_u32 nodes;
} nest_ctx;

typedef struct nest_arg {
    nest_ctx *ctx;
    u32 depth;
} nest_arg;

static void nest_fn(void *user) {
    const nest_arg *a = (const nest_arg *)user;
    de_atomic_fetch_add_u32(&a->ctx->nodes, 1, DE_ATOMIC_RELAXED);
    if (a->depth == NEST_DEPTH)
        return;
    nest_arg child = {a->ctx, a->depth + 1}; /* on this stack: we wait below */
    de_task_counter c = {0};
    for (u32 i = 0; i < NEST_FANOUT; ++i)
        de_task_submit(a->ctx->ts, DE_TASK_POOL_FRAME, nest_fn, &child, &c);
    de_task_wait(a->ctx->ts, &c);
}

TEST(sched, nested_submit_wait) {
    nest_ctx ctx;
    ctx.ts = make_ts(0, 1, NULL);
    de_atomic_init_u32(&ctx.nodes, 0);
    u32 expected = 0, level = 1;
    for (u32 d = 0; d <= NEST_DEPTH; ++d) {
        expected += level;
        level *= NEST_FANOUT;
    }
    for (u32 round = 0; round < 3; ++round) {
        de_atomic_store_u32(&ctx.nodes, 0, DE_ATOMIC_RELAXED);
        nest_arg root = {&ctx, 0};
        de_task_counter c = {0};
        de_task_submit(ctx.ts, DE_TASK_POOL_FRAME, nest_fn, &root, &c);
        de_task_wait(ctx.ts, &c);
        EXPECT_EQ(de_atomic_load_u32(&ctx.nodes, DE_ATOMIC_RELAXED), expected);
    }
    de_task_system_destroy(ctx.ts);
}

/* ---------- main_thread_queue ---------- */

typedef struct main_ctx {
    de_task_system *ts;
    u32 main_thread_id;
    de_atomic_u32 inside;     /* 1 while the test thread is in de_task_wait/pump */
    de_atomic_u32 ran;        /* main-thread tasks run */
    de_atomic_u32 violations; /* main-thread tasks run elsewhere or outside wait/pump */
    de_task_counter cm;
} main_ctx;

static void main_task_fn(void *user) {
    main_ctx *m = (main_ctx *)user;
    if (de_thread_current_id() != m->main_thread_id || de_task_worker_index() != 0 ||
        de_atomic_load_u32(&m->inside, DE_ATOMIC_RELAXED) == 0)
        de_atomic_fetch_add_u32(&m->violations, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&m->ran, 1, DE_ATOMIC_RELAXED);
}

static void frame_submits_main_fn(void *user) {
    main_ctx *m = (main_ctx *)user;
    for (u32 i = 0; i < 16; ++i)
        de_task_submit(m->ts, DE_TASK_POOL_MAIN_THREAD, main_task_fn, m, &m->cm);
}

static i32 ext_submits_main(void *user) {
    main_ctx *m = (main_ctx *)user;
    for (u32 i = 0; i < 16; ++i)
        de_task_submit(m->ts, DE_TASK_POOL_MAIN_THREAD, main_task_fn, m, &m->cm);
    return 0;
}

static void set_inside(main_ctx *m, u32 v) {
    de_atomic_store_u32(&m->inside, v, DE_ATOMIC_RELAXED);
}

TEST(sched, main_thread_queue) {
    static main_ctx m; /* static: the counter must not move while tasks use it */
    memset(&m, 0, sizeof m);
    m.ts = make_ts(3, 1, NULL);
    m.main_thread_id = de_thread_current_id();

    /* 1. Workers (and a non-worker thread) submit main-thread tasks. */
    de_task_counter cf = {0};
    for (u32 i = 0; i < 8; ++i)
        de_task_submit(m.ts, DE_TASK_POOL_FRAME, frame_submits_main_fn, &m, &cf);
    de_thread *ext = NULL;
    ASSERT_EQ(de_thread_create(ext_submits_main, &m, "ext", DE_THREAD_NORMAL, 0, &ext), DE_OK);
    de_thread_join(ext);
    set_inside(&m, 1);
    de_task_wait(m.ts, &cf); /* may run some main-thread tasks: allowed */
    set_inside(&m, 0);

    /* 2. Outside wait/pump nothing runs them, however long we look. */
    const u32 before = de_atomic_load_u32(&m.ran, DE_ATOMIC_RELAXED);
    de_sleep_ns(MS_NS(20));
    EXPECT_EQ(de_atomic_load_u32(&m.ran, DE_ATOMIC_RELAXED), before);
    EXPECT_TRUE((before == 9u * 16u) == de_task_done(&m.cm));

    /* 3. pump runs the rest on this thread. */
    set_inside(&m, 1);
    const u32 pumped = de_task_pump_main(m.ts, 0);
    set_inside(&m, 0);
    EXPECT_EQ(before + pumped, 9u * 16u);
    EXPECT_TRUE(de_task_done(&m.cm));

    /* 4. pump honours max; wait drains main-thread tasks too. */
    for (u32 i = 0; i < 10; ++i)
        de_task_submit(m.ts, DE_TASK_POOL_MAIN_THREAD, main_task_fn, &m, &m.cm);
    set_inside(&m, 1);
    EXPECT_EQ(de_task_pump_main(m.ts, 3), 3u);
    EXPECT_EQ(de_task_pump_main(m.ts, 0), 7u);
    EXPECT_EQ(de_task_pump_main(m.ts, 0), 0u);
    set_inside(&m, 0);
    for (u32 i = 0; i < 5; ++i)
        de_task_submit(m.ts, DE_TASK_POOL_MAIN_THREAD, main_task_fn, &m, &m.cm);
    set_inside(&m, 1);
    de_task_wait(m.ts, &m.cm);
    set_inside(&m, 0);

    EXPECT_EQ(de_atomic_load_u32(&m.ran, DE_ATOMIC_RELAXED), 9u * 16u + 15u);
    EXPECT_EQ(de_atomic_load_u32(&m.violations, DE_ATOMIC_RELAXED), 0u);
    de_task_system_destroy(m.ts);
}

/* ---------- background_isolation ---------- */

static de_atomic_u32 g_bg_started;
static de_atomic_u32 g_bg_finished;
static de_atomic_u32 g_bg_index;
static de_atomic_u32 g_frame_on_bg;
static de_atomic_u32 g_frame_ran;

static void long_bg_fn(void *user) {
    (void)user;
    de_atomic_store_u32(&g_bg_index, de_task_worker_index(), DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_bg_started, 1, DE_ATOMIC_RELEASE);
    de_sleep_ns(MS_NS(500));
    de_atomic_store_u32(&g_bg_finished, 1, DE_ATOMIC_RELEASE);
}

static void small_frame_fn(void *user) {
    (void)user;
    if (de_task_worker_index() == DE_TASK_WORKER_NONE)
        de_atomic_fetch_add_u32(&g_frame_on_bg, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&g_frame_ran, 1, DE_ATOMIC_RELAXED);
}

TEST(sched, background_isolation) {
    de_task_system *ts = make_ts(0, 1, NULL);
    de_atomic_store_u32(&g_bg_started, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_bg_finished, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_frame_on_bg, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_frame_ran, 0, DE_ATOMIC_RELAXED);

    de_task_counter cb = {0};
    de_task_submit(ts, DE_TASK_POOL_BACKGROUND, long_bg_fn, NULL, &cb);
    ASSERT_TRUE(spin_until_u32(&g_bg_started, 1, MS_NS(5000)));

    const u64 t0 = de_time_now_ns();
    de_task_counter cf = {0};
    for (u32 i = 0; i < 1000; ++i)
        de_task_submit(ts, DE_TASK_POOL_FRAME, small_frame_fn, NULL, &cf);
    de_task_wait(ts, &cf);
    const u64 dt = de_time_now_ns() - t0;
    const bool bg_done_early = de_atomic_load_u32(&g_bg_finished, DE_ATOMIC_ACQUIRE) != 0;

    /* The defect this guards against (frame tasks queued behind the background task) costs at
     * least the remaining ~500 ms; CI runners preempt for tens of ms, so CI allows 400 ms. */
    const u64 bound = on_ci() ? MS_NS(400) : MS_NS(50);
    printf("background_isolation: 1000 frame tasks in %.3f ms (bound %.0f ms)\n", (double)dt / 1e6,
           (double)bound / 1e6);
    EXPECT_LT(dt, bound);
    EXPECT_FALSE(bg_done_early);
    EXPECT_EQ(de_atomic_load_u32(&g_frame_ran, DE_ATOMIC_RELAXED), 1000u);
    EXPECT_EQ(de_atomic_load_u32(&g_frame_on_bg, DE_ATOMIC_RELAXED), 0u);
    EXPECT_EQ(de_atomic_load_u32(&g_bg_index, DE_ATOMIC_RELAXED), DE_TASK_WORKER_NONE);

    de_task_wait(ts, &cb); /* the main thread parks; the background worker wakes it */
    EXPECT_TRUE(de_atomic_load_u32(&g_bg_finished, DE_ATOMIC_ACQUIRE) != 0);
    de_task_system_destroy(ts);
}

/* ---------- fpenv_on_workers ---------- */

typedef struct barrier_ctx {
    de_atomic_u32 arrived;
    u32 count;
    de_atomic_u32 fp_ok;
    de_atomic_u32 timeouts;
    de_atomic_u32 seen[256]; /* frame worker index -> tasks run there */
} barrier_ctx;

/* Holds its thread until `count` tasks hold theirs, so each runs on a different thread. */
static void barrier_fn(void *user) {
    barrier_ctx *b = (barrier_ctx *)user;
    if (de_fp_env_check())
        de_atomic_fetch_add_u32(&b->fp_ok, 1, DE_ATOMIC_RELAXED);
    const u32 idx = de_task_worker_index();
    if (idx < 256)
        de_atomic_fetch_add_u32(&b->seen[idx], 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&b->arrived, 1, DE_ATOMIC_ACQ_REL);
    if (!spin_until_u32(&b->arrived, b->count, MS_NS(10000)))
        de_atomic_fetch_add_u32(&b->timeouts, 1, DE_ATOMIC_RELAXED);
}

static void barrier_init(barrier_ctx *b, u32 count) {
    memset(b, 0, sizeof *b);
    b->count = count;
}

TEST(sched, fpenv_on_workers) {
    de_task_system *ts = make_ts(0, 2, NULL);
    const u32 n = de_task_worker_count(ts);
    ASSERT_LE(n, 256u);
    static barrier_ctx b;
    barrier_init(&b, n);
    de_task_counter c = {0};
    for (u32 i = 0; i < n; ++i)
        de_task_submit(ts, DE_TASK_POOL_FRAME, barrier_fn, &b, &c);
    de_task_wait(ts, &c);
    EXPECT_EQ(de_atomic_load_u32(&b.timeouts, DE_ATOMIC_RELAXED), 0u);
    EXPECT_EQ(de_atomic_load_u32(&b.fp_ok, DE_ATOMIC_RELAXED), n);
    for (u32 i = 0; i < n; ++i)
        EXPECT_EQ(de_atomic_load_u32(&b.seen[i], DE_ATOMIC_RELAXED), 1u);

    /* Background workers too (they report DE_TASK_WORKER_NONE). */
    barrier_init(&b, 2);
    de_task_counter cb = {0};
    for (u32 i = 0; i < 2; ++i)
        de_task_submit(ts, DE_TASK_POOL_BACKGROUND, barrier_fn, &b, &cb);
    de_task_wait(ts, &cb);
    EXPECT_EQ(de_atomic_load_u32(&b.timeouts, DE_ATOMIC_RELAXED), 0u);
    EXPECT_EQ(de_atomic_load_u32(&b.fp_ok, DE_ATOMIC_RELAXED), 2u);
    de_task_system_destroy(ts);
}

/* ---------- no_alloc_after_warmup ---------- */

#define ROUND_FRAME   768u /* per round, from the main thread */
#define ROUND_MAIN    20u
#define ROUND_BG      4u
#define SUBMITTER_GAP 48u /* every 48th frame task submits NEST_CHILDREN children and waits */
#define NEST_CHILDREN 10u

typedef struct alloc_ctx {
    de_task_system *ts;
    de_atomic_u32 ran;
    de_atomic_u32 gate; /* warm-up: blocks the workers while the main thread submits */
    de_atomic_u32 gate_started;
} alloc_ctx;

static void count_fn(void *user) {
    de_atomic_fetch_add_u32(&((alloc_ctx *)user)->ran, 1, DE_ATOMIC_RELAXED);
}

static void submitter_fn(void *user) {
    alloc_ctx *a = (alloc_ctx *)user;
    de_task_counter c = {0};
    for (u32 i = 0; i < NEST_CHILDREN; ++i)
        de_task_submit(a->ts, DE_TASK_POOL_FRAME, count_fn, a, &c);
    de_task_wait(a->ts, &c);
    count_fn(a);
}

static void gate_fn(void *user) {
    alloc_ctx *a = (alloc_ctx *)user;
    de_atomic_fetch_add_u32(&a->gate_started, 1, DE_ATOMIC_ACQ_REL);
    spin_until_u32(&a->gate, 1, MS_NS(10000));
}

/* Barrier task that also submits children, so every frame worker's record pool and deque are
 * warm (each worker runs exactly one of these). */
typedef struct warm_ctx {
    barrier_ctx b;
    alloc_ctx *a;
} warm_ctx;

static void warm_fn(void *user) {
    warm_ctx *w = (warm_ctx *)user;
    barrier_fn(&w->b);
    submitter_fn(w->a);
}

static void alloc_round(alloc_ctx *a, bool gated) {
    const u32 workers = de_task_worker_count(a->ts) - 1;
    de_task_counter cg = {0};
    if (gated) {
        /* Hold every worker so all the round's records are live at once: the largest pool the
         * main thread ever needs for a round. */
        de_atomic_store_u32(&a->gate, 0, DE_ATOMIC_RELAXED);
        de_atomic_store_u32(&a->gate_started, 0, DE_ATOMIC_RELAXED);
        for (u32 i = 0; i < workers; ++i)
            de_task_submit(a->ts, DE_TASK_POOL_FRAME, gate_fn, a, &cg);
        spin_until_u32(&a->gate_started, workers, MS_NS(10000));
    }
    de_task_counter cf = {0}, cm = {0}, cb = {0};
    for (u32 i = 0; i < ROUND_FRAME; ++i)
        de_task_submit(a->ts, DE_TASK_POOL_FRAME, i % SUBMITTER_GAP == 0 ? submitter_fn : count_fn,
                       a, &cf);
    for (u32 i = 0; i < ROUND_MAIN; ++i)
        de_task_submit(a->ts, DE_TASK_POOL_MAIN_THREAD, count_fn, a, &cm);
    for (u32 i = 0; i < ROUND_BG; ++i)
        de_task_submit(a->ts, DE_TASK_POOL_BACKGROUND, count_fn, a, &cb);
    if (gated) {
        de_atomic_store_u32(&a->gate, 1, DE_ATOMIC_RELEASE);
        de_task_wait(a->ts, &cg);
    }
    de_task_wait(a->ts, &cf);
    de_task_wait(a->ts, &cm);
    de_task_wait(a->ts, &cb);
}

TEST(sched, no_alloc_after_warmup) {
    de_tracking_allocator tracker;
    const de_allocator alloc = de_tracking_allocator_init(&tracker, NULL, DE_MEM_GENERAL);
    static alloc_ctx a;
    memset(&a, 0, sizeof a);
    a.ts = make_ts(0, 1, &alloc);
    const u32 n = de_task_worker_count(a.ts);

    /* Warm-up. */
    static warm_ctx w;
    barrier_init(&w.b, n);
    w.a = &a;
    de_task_counter cw = {0};
    for (u32 i = 0; i < n; ++i)
        de_task_submit(a.ts, DE_TASK_POOL_FRAME, warm_fn, &w, &cw);
    de_task_wait(a.ts, &cw);
    EXPECT_EQ(de_atomic_load_u32(&w.b.timeouts, DE_ATOMIC_RELAXED), 0u);
    alloc_round(&a, true);
    for (u32 i = 0; i < 2; ++i)
        alloc_round(&a, false);

    const de_tracking_stats before = de_tracking_allocator_stats(&tracker);
    de_atomic_store_u32(&a.ran, 0, DE_ATOMIC_RELAXED);
    const u32 rounds = 20;
    for (u32 i = 0; i < rounds; ++i)
        alloc_round(&a, false);
    const de_tracking_stats after = de_tracking_allocator_stats(&tracker);

    const u32 submitters = (ROUND_FRAME + SUBMITTER_GAP - 1) / SUBMITTER_GAP;
    EXPECT_EQ(de_atomic_load_u32(&a.ran, DE_ATOMIC_RELAXED),
              rounds * (ROUND_FRAME + ROUND_MAIN + ROUND_BG + submitters * NEST_CHILDREN));
    EXPECT_EQ(after.alloc_count, before.alloc_count);
    EXPECT_EQ(after.realloc_count, before.realloc_count);
    printf("no_alloc_after_warmup: %llu blocks live, %llu bytes live\n",
           (unsigned long long)after.blocks_live, (unsigned long long)after.bytes_live);
    de_task_system_destroy(a.ts);
    const de_tracking_stats end = de_tracking_allocator_stats(&tracker);
    EXPECT_EQ(end.blocks_live, 0ull); /* destroy frees everything */
}

/* ---------- external_thread_submit ---------- */

typedef struct ext_ctx {
    de_task_system *ts;
    de_atomic_u32 ran;
    de_atomic_u32 on_worker;
} ext_ctx;

static void ext_task_fn(void *user) {
    ext_ctx *e = (ext_ctx *)user;
    if (de_task_worker_index() != DE_TASK_WORKER_NONE)
        de_atomic_fetch_add_u32(&e->on_worker, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&e->ran, 1, DE_ATOMIC_RELAXED);
}

static i32 ext_submitter(void *user) {
    ext_ctx *e = (ext_ctx *)user;
    de_task_counter c = {0};
    /* More than the injection queue holds, so a full queue is exercised. */
    for (u32 i = 0; i < 3 * DE__TASK_INJECT_CAPACITY; ++i)
        de_task_submit(e->ts, DE_TASK_POOL_FRAME, ext_task_fn, e, &c);
    de_task_wait(e->ts, &c); /* a non-worker thread polls */
    return de_task_done(&c) ? 0 : 1;
}

TEST(sched, external_thread_submit) {
    static ext_ctx e;
    memset(&e, 0, sizeof e);
    e.ts = make_ts(2, 1, NULL);
    de_thread *t[2];
    for (u32 i = 0; i < 2; ++i)
        ASSERT_EQ(de_thread_create(ext_submitter, &e, "ext", DE_THREAD_NORMAL, 0, &t[i]), DE_OK);
    i32 rc = 0;
    for (u32 i = 0; i < 2; ++i)
        rc |= de_thread_join(t[i]);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(de_atomic_load_u32(&e.ran, DE_ATOMIC_RELAXED), 6u * DE__TASK_INJECT_CAPACITY);
    EXPECT_EQ(de_atomic_load_u32(&e.on_worker, DE_ATOMIC_RELAXED), 6u * DE__TASK_INJECT_CAPACITY);
    de_task_system_destroy(e.ts);
}

/* ---------- shutdown_with_pending ---------- */

typedef struct shut_ctx {
    de_task_system *ts;
    de_atomic_u32 frame;
    de_atomic_u32 main;
    de_atomic_u32 bg;
    de_atomic_u32 main_elsewhere;
} shut_ctx;

static void shut_bg_fn(void *user) {
    de_sleep_ns(MS_NS(5));
    de_atomic_fetch_add_u32(&((shut_ctx *)user)->bg, 1, DE_ATOMIC_RELAXED);
}

static void shut_main_fn(void *user) {
    shut_ctx *s = (shut_ctx *)user;
    if (de_task_worker_index() != 0)
        de_atomic_fetch_add_u32(&s->main_elsewhere, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u32(&s->main, 1, DE_ATOMIC_RELAXED);
}

static void shut_leaf_fn(void *user) {
    de_atomic_fetch_add_u32(&((shut_ctx *)user)->frame, 1, DE_ATOMIC_RELAXED);
}

/* Submits more work of every kind while the system drains. */
static void shut_frame_fn(void *user) {
    shut_ctx *s = (shut_ctx *)user;
    de_task_submit(s->ts, DE_TASK_POOL_FRAME, shut_leaf_fn, s, NULL);
    de_task_submit(s->ts, DE_TASK_POOL_MAIN_THREAD, shut_main_fn, s, NULL);
    de_task_submit(s->ts, DE_TASK_POOL_BACKGROUND, shut_bg_fn, s, NULL);
    de_atomic_fetch_add_u32(&s->frame, 1, DE_ATOMIC_RELAXED);
}

TEST(sched, shutdown_with_pending) {
    static shut_ctx s;
    memset(&s, 0, sizeof s);
    s.ts = make_ts(0, 2, NULL);
    for (u32 i = 0; i < 5000; ++i)
        de_task_submit(s.ts, DE_TASK_POOL_FRAME, i % 100 == 0 ? shut_frame_fn : shut_leaf_fn, &s,
                       NULL);
    for (u32 i = 0; i < 100; ++i)
        de_task_submit(s.ts, DE_TASK_POOL_MAIN_THREAD, shut_main_fn, &s, NULL);
    for (u32 i = 0; i < 8; ++i)
        de_task_submit(s.ts, DE_TASK_POOL_BACKGROUND, shut_bg_fn, &s, NULL);
    de_task_system_destroy(s.ts); /* no wait: destroy drains */
    EXPECT_EQ(de_atomic_load_u32(&s.frame, DE_ATOMIC_RELAXED), 5000u + 50u);
    EXPECT_EQ(de_atomic_load_u32(&s.main, DE_ATOMIC_RELAXED), 100u + 50u);
    EXPECT_EQ(de_atomic_load_u32(&s.bg, DE_ATOMIC_RELAXED), 8u + 50u);
    EXPECT_EQ(de_atomic_load_u32(&s.main_elsewhere, DE_ATOMIC_RELAXED), 0u);
    EXPECT_EQ(de_task_worker_index(), DE_TASK_WORKER_NONE);
}
