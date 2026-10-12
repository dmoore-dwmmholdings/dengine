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
 *   wake.background_frame_saturated
 *                          background tasks submitted while every frame worker spins: time from
 *                          the submit to the task's end. On Windows the below-normal background
 *                          worker starved for about 4 s per task before the boost (DENG-388).
 *   wake.background_boost_duty_cycle
 *                          the boost policy (forced on, every OS): no raise while idle or while
 *                          background tasks make progress; a task without visible progress is
 *                          raised only within the budget (DENG-388).
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

/* ---------- Background boost (DENG-388) ---------- */

static de_task_system *make_ts_boost(u32 workers, u32 bg, de_task_bg_boost boost) {
    de_fp_env_init_thread();
    de_task_system_desc d = {0};
    d.worker_count = workers;
    d.background_worker_count = bg;
    d.background_boost = boost;
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&d, &ts), DE_OK);
    ASSERT_TRUE(ts != NULL);
    return ts;
}

/* A frame task that keeps its worker busy until `g_spin_stop` is set. */
static de_atomic_u32 g_spin_stop;
static de_atomic_u32 g_spinning;
static de_sem g_nap; /* never posted: a timed wait on it is an OS sleep */

static void spin_fn(void *user) {
    (void)user;
    de_atomic_fetch_add_u32(&g_spinning, 1, DE_ATOMIC_RELAXED);
    while (de_atomic_load_u32(&g_spin_stop, DE_ATOMIC_ACQUIRE) == 0)
        de_cpu_pause();
}

#define SAT_SAMPLES 20u
#define SAT_TASK_NS MS_NS(1)

typedef struct sat_driver {
    u64 lat[SAT_SAMPLES];
    u32 n;
    u32 stalled; /* samples during which the machine itself stalled (see sat_watch_main) */
} sat_driver;

typedef struct sat_task {
    de_atomic_u64 end_ns;
} sat_task;

static void sat_bg_fn(void *user) {
    sat_task *t = (sat_task *)user;
    busy_for(SAT_TASK_NS);
    de_atomic_store_u64(&t->end_ns, de_time_now_ns(), DE_ATOMIC_RELAXED);
}

/* A time-critical thread that only sleeps (1 ms timed waits, a timer tick on Windows) and
 * records the longest gap between two of its wake-ups. Nothing in the test can keep it from
 * running, so a gap far beyond a tick means the machine stalled every thread (a CI virtual
 * machine that the host did not run): on one Windows runner the monitor, the driver and this
 * kind of thread all stood still for 100-200 ms at a time. Such samples measure the host, not
 * the scheduler, and are left out of the bound. */
static de_atomic_u32 g_watch_stop;
static de_atomic_u64 g_watch_gap;

#define SAT_STALL_GAP_NS MS_NS(50)

static i32 sat_watch_main(void *user) {
    (void)user;
    u64 last = de_time_now_ns();
    while (de_atomic_load_u32(&g_watch_stop, DE_ATOMIC_ACQUIRE) == 0) {
        (void)de_sem_wait_timeout(&g_nap, MS_NS(1));
        const u64 now = de_time_now_ns();
        const u64 gap = now - last;
        last = now;
        u64 cur = de_atomic_load_u64(&g_watch_gap, DE_ATOMIC_RELAXED);
        while (gap > cur && !de_atomic_cas_weak_u64(&g_watch_gap, &cur, gap, DE_ATOMIC_RELAXED,
                                                    DE_ATOMIC_RELAXED)) {
        }
    }
    return 0;
}

/* Not a worker: one background task at a time, timed from the submit to the task's own end (so
 * the driver's own wake-up is not part of it). Then it stops the spinning frame workers. It runs
 * time-critical, so that only the background worker is measured: at normal priority it waited
 * behind the spinners for a whole quantum (about 190 ms on Windows Server) before it even
 * submitted, and above normal it still lost a quantum to spinners that had just woken (Windows
 * runs a thread woken from a wait one level higher for a quantum). */
static i32 sat_driver_main(void *user) {
    sat_driver *d = (sat_driver *)user;
    de_fp_env_init_thread();
    /* Every frame worker (the main thread included) spins first. */
    while (de_atomic_load_u32(&g_spinning, DE_ATOMIC_ACQUIRE) < de_task_worker_count(g_ts))
        (void)de_sem_wait_timeout(&g_nap, MS_NS(1));
    for (u32 i = 0; i < SAT_SAMPLES; ++i) {
        sat_task t;
        de_atomic_init_u64(&t.end_ns, 0);
        de_task_counter c = {0};
        de_atomic_store_u64(&g_watch_gap, 0, DE_ATOMIC_RELAXED);
        const u64 t0 = de_time_now_ns();
        de_task_submit(g_ts, DE_TASK_POOL_BACKGROUND, sat_bg_fn, &t, &c);
        de_task_wait(g_ts, &c);
        const u64 lat = de_atomic_load_u64(&t.end_ns, DE_ATOMIC_RELAXED) - t0;
        const u64 gap = de_atomic_load_u64(&g_watch_gap, DE_ATOMIC_RELAXED);
        if (gap > SAT_STALL_GAP_NS) {
            printf("wake: sample %u (%.1f ms) left out: the watch thread stood still %.1f ms\n", i,
                   (double)lat / 1e6, (double)gap / 1e6);
            ++d->stalled;
            continue;
        }
        d->lat[d->n++] = lat;
    }
    de_atomic_store_u32(&g_spin_stop, 1, DE_ATOMIC_RELEASE);
    return 0;
}

static void bg_warm_fn(void *user) {
    (void)user;
}

/* Background tasks submitted while every frame worker is busy (normal priority, every core):
 * on Windows the below-normal background worker got no CPU until a frame worker's quantum ended
 * (about 190 ms on Windows Server) or, often, until the OS's own anti-starvation boost (about
 * 5 s; CI control runs with the boost off, DENG-388). With it, a stalled worker is raised at the
 * next 10 ms check (on Windows 16 to 32 ms: timed waits end on 15.6 ms timer ticks) and runs its
 * 1 ms task at once. In 400 CI runs (4 Windows jobs) the slowest task took 47 ms. */
TEST(wake, background_frame_saturated) {
    g_ts = make_ts_boost(0, 1, DE_TASK_BG_BOOST_AUTO);
    de_atomic_store_u32(&g_spin_stop, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&g_spinning, 0, DE_ATOMIC_RELAXED);
    ASSERT_EQ(de_sem_init(&g_nap, 0), DE_OK);

    /* Steady state first: every thread started (a starting thread holds the OS loader lock on
     * Windows, and a starved one there delays all others), the background worker has run a
     * task, and the frame workers are parked. */
    de_task_counter warm = {0};
    de_task_submit(g_ts, DE_TASK_POOL_BACKGROUND, bg_warm_fn, NULL, &warm);
    de_task_wait(g_ts, &warm);
    ASSERT_TRUE(spin_until_u32(&g_ts->sleepers, g_ts->frame_count - 1, MS_NS(5000)));

    static sat_driver drv;
    memset(&drv, 0, sizeof drv);
    de_atomic_store_u32(&g_watch_stop, 0, DE_ATOMIC_RELAXED);
    de_thread *watch = NULL;
    ASSERT_EQ(de_thread_create(sat_watch_main, NULL, "wake-watch", DE_THREAD_REALTIME, 0, &watch),
              DE_OK);
    de_thread *th = NULL;
    ASSERT_EQ(de_thread_create(sat_driver_main, &drv, "wake-sat", DE_THREAD_REALTIME, 0, &th),
              DE_OK);
    /* One spinner per frame worker; the main thread runs one inside the wait. */
    de_task_counter spinners = {0};
    for (u32 i = 0; i < de_task_worker_count(g_ts); ++i)
        de_task_submit(g_ts, DE_TASK_POOL_FRAME, spin_fn, NULL, &spinners);
    de_task_wait(g_ts, &spinners);
    EXPECT_EQ(de_thread_join(th), 0);
    de_atomic_store_u32(&g_watch_stop, 1, DE_ATOMIC_RELEASE);
    EXPECT_EQ(de_thread_join(watch), 0);
    de_sem_destroy(&g_nap);

    /* Most samples must count: a machine that stalls all the time is not measured at all. */
    ASSERT_EQ(drv.n + drv.stalled, SAT_SAMPLES);
    ASSERT_GE(drv.n, SAT_SAMPLES / 2);
    const lat_summary s = summarize("bg task, frame workers saturated", drv.lat, drv.n);
    const de__task_stats st = de__task_get_stats(g_ts);
    printf("wake: %u frame workers spinning, %llu checks, %llu raises, %.1f ms raised, %u "
           "priority failures\n",
           de_task_worker_count(g_ts), (unsigned long long)st.bg_checks,
           (unsigned long long)st.bg_boosts, (double)st.bg_boost_ns / 1e6, st.bg_prio_failures);
    EXPECT_EQ(st.bg_prio_failures, 0u);
    /* Windows (boost on): the documented bound is one or two checks, about 40 ms; a quantum
     * (about 190 ms) means the boost did not work. Other OSes do not boost: their schedulers
     * give the low priority a share (up to 95 ms per task on the macOS host with 15 spinners).
     * CI and sanitizers get room. */
#if defined(_WIN32)
    EXPECT_LT(s.max, loose_bounds() ? MS_NS(150) : MS_NS(100));
#else
    EXPECT_LT(s.max, loose_bounds() ? MS_NS(500) : MS_NS(250));
#endif
    de_task_system_destroy(g_ts);
    g_ts = NULL;
}

/* The boost's policy, on every OS (DE_TASK_BG_BOOST_ON; where the OS cannot change a priority the
 * monitor still runs and counts): no raise while nothing is pending or while background tasks
 * keep starting and ending, and a task that does nothing the monitor can see (it blocks in user
 * code, as a starved or long-computing one would look) is raised again and again, but only for as
 * much time as the budget allows: 1/8 of wall time after a burst of at most 32 ms. */
static void bg_block_fn(void *user) {
    (void)de_sem_wait_timeout((de_sem *)user, MS_NS(1000));
}

static void bg_short_fn(void *user) {
    (void)user;
    busy_for(US_NS(500));
}

TEST(wake, background_boost_duty_cycle) {
    g_ts = make_ts_boost(1, 1, DE_TASK_BG_BOOST_ON);
    ASSERT_EQ(de_sem_init(&g_nap, 0), DE_OK);

    /* Idle: the monitor blocks; it neither checks nor raises. */
    (void)de_sem_wait_timeout(&g_nap, MS_NS(300));
    const de__task_stats s0 = de__task_get_stats(g_ts);
    const u64 idle = s0.bg_boosts + s0.bg_checks;

    /* Progress: 1000 short tasks back to back (about 0.5 s), each one waited for. A raise is
     * legitimate only while one task was pending for a whole check (the low-priority worker did
     * not run it at once): each task allows one raise per check interval it was pending. */
    const u64 b0 = de__task_get_stats(g_ts).bg_boosts;
    u64 allowed = 1; /* a scheduling hiccup on a shared runner */
    for (u32 i = 0; i < 1000; ++i) {
        de_task_counter c = {0};
        const u64 t0 = de_time_now_ns();
        de_task_submit(g_ts, DE_TASK_POOL_BACKGROUND, bg_short_fn, NULL, &c);
        de_task_wait(g_ts, &c);
        allowed += (de_time_now_ns() - t0) / DE__TASK_BG_CHECK_NS;
    }
    const u64 progressing = de__task_get_stats(g_ts).bg_boosts - b0;

    /* No visible progress: one task blocked for 1 s. Wait until the burst allowance is back. */
    (void)de_sem_wait_timeout(&g_nap, MS_NS(300));
    de_sem block;
    ASSERT_EQ(de_sem_init(&block, 0), DE_OK);
    const de__task_stats s1 = de__task_get_stats(g_ts);
    const u64 t0 = de_time_now_ns();
    de_task_counter c = {0};
    de_task_submit(g_ts, DE_TASK_POOL_BACKGROUND, bg_block_fn, &block, &c);
    de_task_wait(g_ts, &c);
    const u64 elapsed = de_time_now_ns() - t0;
    const de__task_stats s2 = de__task_get_stats(g_ts);
    const u64 raises = s2.bg_boosts - s1.bg_boosts;
    const u64 raised_ns = s2.bg_boost_ns - s1.bg_boost_ns;
    de_sem_destroy(&block);
    de_sem_destroy(&g_nap);

    /* Budget: the cap, 1/8 of the time, and the raise that overdraws it (one slice, which ends
     * on a timer tick on Windows) plus the refill of one check interval (also tick-rounded). */
    const u64 budget = DE__TASK_BG_CREDIT_CAP_NS + elapsed / DE__TASK_BG_SHARE_DIV + MS_NS(40);
    printf(
        "wake: boost: idle %llu checks+raises, progressing %llu (allowed %llu), blocked task %llu "
        "raises for %.1f ms in %.0f ms (budget %.1f ms)\n",
        (unsigned long long)idle, (unsigned long long)progressing, (unsigned long long)allowed,
        (unsigned long long)raises, (double)raised_ns / 1e6, (double)elapsed / 1e6,
        (double)budget / 1e6);
    EXPECT_EQ(idle, 0u);
    EXPECT_LE(progressing, allowed);
    EXPECT_GE(raises, 3u);
    EXPECT_LE(raised_ns, budget);
    de_task_system_destroy(g_ts);
    g_ts = NULL;
}
