/* Wake-up latency of the task system (src/task/; DENG-375).
 *
 * DENG-375: on Windows CI, stress.mixed_3s ran 4 operations in 6 s. Threads that are not
 * workers waited on counters by spinning; the background workers run at a lower priority, and
 * Windows schedules strictly by priority, so with more spinners than cores the background
 * workers did not run for seconds. These tests time each way a parked thread is woken:
 *
 *   wake.idle_worker       a frame task submitted while every frame worker is parked: time
 *                          from the submit to the start of the task.
 *   wake.counter_waiters   a counter finished while its waiter is parked (main thread,
 *                          background worker, thread that is not a worker): time from the end
 *                          of the task to the return of de_task_wait.
 *   wake.external_waiters_and_background
 *                          cores + 2 threads that are not workers each wait for background
 *                          tasks, one at a time: the DENG-375 case. Spinning waiters starve the
 *                          background worker; blocking ones do not.
 *
 * Each test prints its numbers and checks the median: under 5 ms when DE_CI or CI is set or in a
 * sanitizer build, under 1 ms on a developer machine. wake.external_waiters_and_background also
 * bounds the total time. The tests run serially (this directory's CMakeLists.txt; Trama
 * "Decision: Timing tests run serially and loosen their bounds on CI"). */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif
#include "de_test.h"

#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/de_atomic.h"
#include "task/task_internal.h"

#define US_NS(us) ((u64)(us) * 1000ull)
#define MS_NS(ms) ((u64)(ms) * 1000ull * 1000ull)

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define DE_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define DE_TEST_SANITIZED 1
#endif

static bool env_flag(const char *name) {
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' && v[0] != '0' && v[0] != 'f' && v[0] != 'F';
}

/* Loose bounds: shared CI runners preempt threads for milliseconds, and sanitizers slow every
 * atomic and system call. */
static bool loose_bounds(void) {
#if defined(DE_TEST_SANITIZED)
    const bool sanitized = true;
#else
    const bool sanitized = false;
#endif
    return sanitized || env_flag("DE_CI") || env_flag("CI");
}

/* Median wake latency allowed. The issue asks for well under 1 ms per wake on an idle machine;
 * Win32 semaphores and the macOS host wake in tens of microseconds. */
static u64 median_bound_ns(void) {
    return loose_bounds() ? MS_NS(5) : MS_NS(1);
}

static de_task_system *make_ts(u32 workers, u32 bg) {
    de_fp_env_init_thread();
    de_task_system_desc d = {0};
    d.worker_count = workers;
    d.background_worker_count = bg;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    ASSERT_TRUE(ts != NULL);
    return ts;
}

static int cmp_u64(const void *a, const void *b) {
    const u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

typedef struct lat_summary {
    u64 median, p90, max;
} lat_summary;

/* Sorts `v` and prints its median, 90th percentile and maximum. */
static lat_summary summarize(const char *what, u64 *v, u32 n) {
    qsort(v, n, sizeof *v, cmp_u64);
    const lat_summary s = {v[n / 2], v[(n * 9) / 10], v[n - 1]};
    printf("wake: %-34s n %3u  median %8.1f us  p90 %8.1f us  max %9.1f us\n", what, n,
           (double)s.median / 1e3, (double)s.p90 / 1e3, (double)s.max / 1e3);
    return s;
}

/* Spins until `*v` reaches `want` or `timeout_ns` passes. Only for short waits in the measuring
 * thread, which owns no work anybody waits for. */
static bool spin_until_u32(const de_atomic_u32 *v, u32 want, u64 timeout_ns) {
    const u64 end = de_time_now_ns() + timeout_ns;
    while (de_atomic_load_u32(v, DE_ATOMIC_ACQUIRE) < want) {
        if (de_time_now_ns() > end)
            return false;
        de_cpu_pause();
    }
    return true;
}

/* Busy work for `ns` that keeps the thread on its core (the timed task must not park). */
static void busy_for(u64 ns) {
    const u64 end = de_time_now_ns() + ns;
    while (de_time_now_ns() < end)
        de_cpu_pause();
}

/* ---------- idle_worker ---------- */

typedef struct start_probe {
    de_atomic_u64 start_ns;
    de_atomic_u32 started;
    de_atomic_u32 worker;
} start_probe;

static void probe_fn(void *user) {
    start_probe *p = (start_probe *)user;
    de_atomic_store_u64(&p->start_ns, de_time_now_ns(), DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&p->worker, de_task_worker_index(), DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&p->started, 1, DE_ATOMIC_RELEASE);
}

#define IDLE_SAMPLES 200u

TEST(wake, idle_worker) {
    de_task_system *ts = make_ts(2, 1);
    u64 lat[IDLE_SAMPLES];
    u32 n = 0, on_main = 0, not_parked = 0;
    for (u32 i = 0; i < IDLE_SAMPLES; ++i) {
        /* Every frame worker parked on the idle semaphore (they park after a short spin). */
        if (!spin_until_u32(&ts->sleepers, ts->frame_count - 1, MS_NS(2000))) {
            ++not_parked;
            continue;
        }
        start_probe p;
        memset(&p, 0, sizeof p);
        de_task_counter c = {0};
        const u64 t0 = de_time_now_ns();
        /* Goes to this thread's deque; the submit wakes one parked worker, which steals it. This
         * thread only watches the flag: it never runs the task itself. */
        de_task_submit(ts, DE_TASK_POOL_FRAME, probe_fn, &p, &c);
        ASSERT_TRUE(spin_until_u32(&p.started, 1, MS_NS(10000)));
        de_task_wait(ts, &c);
        if (de_atomic_load_u32(&p.worker, DE_ATOMIC_RELAXED) == 0) {
            ++on_main;
            continue;
        }
        lat[n++] = de_atomic_load_u64(&p.start_ns, DE_ATOMIC_RELAXED) - t0;
    }
    EXPECT_EQ(not_parked, 0u);
    EXPECT_EQ(on_main, 0u);
    ASSERT_GT(n, IDLE_SAMPLES / 2);
    const lat_summary s = summarize("submit -> parked worker starts", lat, n);
    EXPECT_LT(s.median, median_bound_ns());
    de_task_system_destroy(ts);
}

/* ---------- counter_waiters ---------- */

/* The timed task: it starts on a frame worker, runs long enough for the waiter to park, and
 * stamps its end just before it returns (the counter is finished right after). */
typedef struct timed_task {
    de_atomic_u32 started;
    de_atomic_u64 end_ns;
} timed_task;

#define TIMED_TASK_NS MS_NS(2)

static void timed_fn(void *user) {
    timed_task *t = (timed_task *)user;
    de_atomic_store_u32(&t->started, 1, DE_ATOMIC_RELEASE);
    busy_for(TIMED_TASK_NS);
    de_atomic_store_u64(&t->end_ns, de_time_now_ns(), DE_ATOMIC_RELAXED);
}

static de_task_system *g_ts;

/* One sample, on the calling thread: submit the timed task, let a worker start it, then wait.
 * Returns the time from the task's end to the waiter's return, or UINT64_MAX when the task did
 * not start on another thread in time. */
static u64 one_wait_sample(void) {
    timed_task t;
    memset(&t, 0, sizeof t);
    de_task_counter c = {0};
    de_task_submit(g_ts, DE_TASK_POOL_FRAME, timed_fn, &t, &c);
    /* Only watch until a worker has taken it, so the wait below has nothing to help with. */
    if (!spin_until_u32(&t.started, 1, MS_NS(10000)))
        return UINT64_MAX;
    de_task_wait(g_ts, &c);
    const u64 ret = de_time_now_ns();
    return ret - de_atomic_load_u64(&t.end_ns, DE_ATOMIC_RELAXED);
}

#define WAIT_SAMPLES 40u

typedef struct sampler {
    u64 lat[WAIT_SAMPLES];
    u32 n;
    u32 failed;
} sampler;

static void sample_into(sampler *s) {
    for (u32 i = 0; i < WAIT_SAMPLES; ++i) {
        const u64 v = one_wait_sample();
        if (v == UINT64_MAX)
            ++s->failed;
        else
            s->lat[s->n++] = v;
    }
}

static void bg_sampler_fn(void *user) {
    sample_into((sampler *)user);
}

static i32 ext_sampler_main(void *user) {
    de_fp_env_init_thread();
    sample_into((sampler *)user);
    return 0;
}

static void check_sampler(const char *what, sampler *s) {
    EXPECT_EQ(s->failed, 0u);
    ASSERT_EQ(s->n, WAIT_SAMPLES);
    const lat_summary sum = summarize(what, s->lat, s->n);
    EXPECT_LT(sum.median, median_bound_ns());
}

TEST(wake, counter_waiters) {
    g_ts = make_ts(2, 1);

    static sampler main_s, bg_s, ext_s;
    memset(&main_s, 0, sizeof main_s);
    memset(&bg_s, 0, sizeof bg_s);
    memset(&ext_s, 0, sizeof ext_s);

    /* Main thread (frame worker 0): parks on its slot semaphore, registered in the counter. */
    sample_into(&main_s);

    /* Background worker: parks on its slot semaphore. */
    de_task_counter cb = {0};
    de_task_submit(g_ts, DE_TASK_POOL_BACKGROUND, bg_sampler_fn, &bg_s, &cb);
    de_task_wait(g_ts, &cb);

    /* A thread that is not a worker: parks on an external wait record (DENG-375). */
    de_thread *th = NULL;
    ASSERT_EQ(de_thread_create(ext_sampler_main, &ext_s, "wake-ext", DE_THREAD_NORMAL, 0, &th),
              DE_OK);
    EXPECT_EQ(de_thread_join(th), 0);

    check_sampler("task end -> main thread returns", &main_s);
    check_sampler("task end -> background waiter returns", &bg_s);
    check_sampler("task end -> external waiter returns", &ext_s);
    de_task_system_destroy(g_ts);
    g_ts = NULL;
}

/* ---------- external_waiters_and_background ---------- */

#define EXT_ROUNDS      30u
#define EXT_MAX_THREADS 64u

typedef struct ext_waiter {
    de_thread *thread;
    u64 lat[EXT_ROUNDS];
    u32 n;
} ext_waiter;

static de_atomic_u32 g_bg_ran;

static void bg_noop(void *user) {
    (void)user;
    de_atomic_fetch_add_u32(&g_bg_ran, 1, DE_ATOMIC_RELAXED);
}

/* Each round: one background task, then wait for it. Records submit -> wait returned. */
static i32 ext_waiter_main(void *user) {
    ext_waiter *w = (ext_waiter *)user;
    de_fp_env_init_thread();
    for (u32 r = 0; r < EXT_ROUNDS; ++r) {
        de_task_counter c = {0};
        const u64 t0 = de_time_now_ns();
        de_task_submit(g_ts, DE_TASK_POOL_BACKGROUND, bg_noop, NULL, &c);
        de_task_wait(g_ts, &c);
        w->lat[w->n++] = de_time_now_ns() - t0;
    }
    return 0;
}

TEST(wake, external_waiters_and_background) {
    /* More waiting threads than cores, one low-priority background worker. Before DENG-375 the
     * waiters spun and, on Windows, kept the background worker off every core for seconds. */
    const u32 cores = de_cpu_logical_cores();
    const u32 threads = cores + 2 < EXT_MAX_THREADS ? cores + 2 : EXT_MAX_THREADS;
    g_ts = make_ts(0, 1);
    de_atomic_store_u32(&g_bg_ran, 0, DE_ATOMIC_RELAXED);

    static ext_waiter ws[EXT_MAX_THREADS];
    memset(ws, 0, sizeof ws);
    const u64 t0 = de_time_now_ns();
    for (u32 i = 0; i < threads; ++i) {
        char name[32];
        snprintf(name, sizeof name, "wake-ext-%u", i);
        ASSERT_EQ(
            de_thread_create(ext_waiter_main, &ws[i], name, DE_THREAD_NORMAL, 0, &ws[i].thread),
            DE_OK);
    }
    for (u32 i = 0; i < threads; ++i)
        EXPECT_EQ(de_thread_join(ws[i].thread), 0);
    const u64 total = de_time_now_ns() - t0;

    static u64 all[EXT_MAX_THREADS * EXT_ROUNDS];
    u32 n = 0;
    for (u32 i = 0; i < threads; ++i)
        for (u32 r = 0; r < ws[i].n; ++r)
            all[n++] = ws[i].lat[r];
    ASSERT_EQ(n, threads * EXT_ROUNDS);
    EXPECT_EQ(de_atomic_load_u32(&g_bg_ran, DE_ATOMIC_RELAXED), n);
    char what[64];
    snprintf(what, sizeof what, "%u ext waiters on 1 bg worker", threads);
    const lat_summary s = summarize(what, all, n);
    /* Every task goes through the one background worker, so the total is n wake-ups in a row.
     * The pre-fix stall was seconds; the loose bound leaves room for slow runners. */
    const u64 total_bound = loose_bounds() ? MS_NS(3000) : MS_NS(1000);
    printf("wake: %u background tasks in %.1f ms (bound %.0f ms)\n", n, (double)total / 1e6,
           (double)total_bound / 1e6);
    EXPECT_LT(s.median, median_bound_ns());
    EXPECT_LT(total, total_bound);
    de_task_system_destroy(g_ts);
    g_ts = NULL;
}
