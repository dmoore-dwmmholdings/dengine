/* Tests for precise sleep and the frame pacer (DENG-249). Label `timing`.
 *
 * The bounds are tight on a developer machine. Shared CI runners preempt threads for
 * milliseconds at a time, so when DE_CI or CI is set (GitHub Actions sets CI=true) the
 * statistical bounds are loose; the "never early" checks stay exact everywhere. */
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#if defined(__linux__)
#define _GNU_SOURCE /* nanosleep under -std=c11 */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include "de_test.h"

#include <dengine/de_platform.h>

#include "platform/de_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#if !defined(_WIN32)
#include <time.h>
#endif

#define MS 1000000ull
#define US 1000ull

/* An environment flag is set unless it is missing, empty, "0" or "false". */
static bool env_flag(const char *name) {
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' && v[0] != '0' && v[0] != 'f' && v[0] != 'F';
}

static bool on_ci(void) {
    return env_flag("DE_CI") || env_flag("CI");
}

static u64 abs_diff(u64 a, u64 b) {
    return a > b ? a - b : b - a;
}

/* Bound for "returns at once": a CI runner can preempt any call for tens of milliseconds. */
static u64 quick_ns(void) {
    return on_ci() ? 100 * MS : 2 * MS;
}

TEST(pacing, sleep_edge_cases) {
    u64 t0 = de_time_now_ns();
    de_sleep_ns(0);
    de_sleep_until_ns(0);
    de_sleep_until_ns(t0); /* already past */
    EXPECT_LT(de_time_now_ns() - t0, quick_ns());

    /* Below the spin threshold: pure spin, still never early. */
    for (int i = 0; i < 20; i++) {
        u64 s = de_time_now_ns();
        de_sleep_ns(100 * US);
        EXPECT_GE(de_time_now_ns() - s, 100 * US);
    }
    /* Longer than the spin threshold, with a deadline. */
    u64 deadline = de_time_now_ns() + 7 * MS;
    de_sleep_until_ns(deadline);
    EXPECT_GE(de_time_now_ns(), deadline);
}

/* Prints how late a bare 0.5 ms OS sleep wakes on this host (POSIX nanosleep), the number that
 * decides de_sleep_until_ns()'s spin window (DENG-361). A record for CI logs, not a check. */
static void print_os_sleep_lateness(void) {
#if !defined(_WIN32)
    enum { N = 21 };
    u64 late[N];
    for (int i = 0; i < N; i++) {
        struct timespec req = {0, 500000};
        u64 t0 = de_time_now_ns();
        (void)nanosleep(&req, NULL);
        u64 dt = de_time_now_ns() - t0;
        late[i] = dt > 500 * US ? dt - 500 * US : 0;
    }
    for (int i = 1; i < N; i++) { /* insertion sort, for the median */
        u64 v = late[i];
        int j = i;
        for (; j > 0 && late[j - 1] > v; j--)
            late[j] = late[j - 1];
        late[j] = v;
    }
    printf("nanosleep(0.5 ms) wakes late by: min %.1f us, median %.1f us, max %.1f us (%d runs)\n",
           (double)late[0] / 1000.0, (double)late[N / 2] / 1000.0, (double)late[N - 1] / 1000.0, N);
#endif
}

/* Host load when the samples ran, a record for failure logs, not a check (DENG-726). A full
 * audit missed the host gate (188/200) while a run on an idle host made 200/200. Under CPU
 * contention a timeshare thread can wake from its OS sleep past the deadline, or, once the
 * learned spin window covers the whole 2 ms, spin without sleeping and lose the CPU for a
 * scheduler quantum. The load average tells such runs apart from a regression in
 * de_sleep_ns(). (getrusage() is no help here: Darwin counts blocking sleeps as involuntary
 * context switches.) */
static void print_host_load(void) {
#if !defined(_WIN32)
    double load[3] = {0, 0, 0};
    if (getloadavg(load, 3) == 3)
        printf("host load average: %.2f %.2f %.2f\n", load[0], load[1], load[2]);
#endif
}

/* The thread that measures opts in as timing-critical (DENG-726 decision: opt-in API), with a
 * period of one 2 ms sample. Where the OS refuses (Linux without CAP_SYS_NICE/RLIMIT_RTPRIO) the
 * test measures a normal thread, against the same gate, and says so. */
static bool opt_in_timing_critical(void) {
    de_thread_timing_desc td = {.period_ns = 2 * MS + 500 * US, .computation_ns = 2 * MS};
    de_result r = de_thread_set_timing_critical(&td);
    if (r != DE_OK)
        de_error_clear();
    printf("timing-critical thread: %s\n", r == DE_OK ? "yes" : "no (OS refused; normal thread)");
    return r == DE_OK;
}

/* 200 de_sleep_ns(2 ms) samples, histogram to stdout, then the never-early check and the
 * +0.3 ms gate: >= 95 % on a host, 80 % on CI (see sleep_accuracy). */
static void measure_sleep_accuracy(const char *what) {
    enum { SAMPLES = 200, BUCKETS = 8 };
    static const u64 edges[BUCKETS] = {10 * US,  25 * US,  50 * US,   100 * US,
                                       200 * US, 300 * US, 1000 * US, UINT64_MAX};
    static const char *labels[BUCKETS] = {"<10us",  "<25us",  "<50us", "<100us",
                                          "<200us", "<300us", "<1ms",  ">=1ms"};
    u32 hist[BUCKETS] = {0};
    u32 early = 0, within = 0;
    u64 worst = 0, sum = 0;
    const u64 target = 2 * MS;

    for (int i = 0; i < SAMPLES; i++) {
        u64 t0 = de_time_now_ns();
        de_sleep_ns(target);
        u64 dt = de_time_now_ns() - t0;
        if (dt < target) {
            early++;
            continue;
        }
        u64 over = dt - target;
        sum += over;
        if (over > worst)
            worst = over;
        if (over <= 300 * US)
            within++;
        for (int b = 0; b < BUCKETS; b++) {
            if (over < edges[b]) {
                hist[b]++;
                break;
            }
        }
    }

    printf("%s: de_sleep_ns(2 ms) overshoot over %d samples (mean %.1f us, worst %.1f us):\n",
           what, SAMPLES, (double)sum / SAMPLES / 1000.0, (double)worst / 1000.0);
    for (int b = 0; b < BUCKETS; b++)
        printf("  %-7s %3u\n", labels[b], hist[b]);
    printf("  within +0.3 ms: %u / %d\n", within, SAMPLES);
    print_host_load();

    EXPECT_EQ(early, 0u);
    if (on_ci()) {
        /* Shared runners preempt the spin now and then. GitHub's macOS VMs, whose nanosleep
         * wakes 1.5 to 6.3 ms late, measured 194 to 199 of 200 with the adaptive spin window
         * (13 before it, DENG-361); Linux and Windows runners 200. 80 % leaves room for a noisy
         * VM and still fails on a spin window that does not adapt. */
        EXPECT_GE(within * 100, (u32)SAMPLES * 80u);
    } else {
        EXPECT_GE(within * 100, (u32)SAMPLES * 95u);
    }
}

TEST(pacing, sleep_accuracy) {
    /* Acceptance: de_sleep_ns(2 ms) lands within +0.3 ms / -0 of the target in >= 95 % of 200
     * samples, on a timing-critical thread (DENG-726). The histogram goes to stdout. */
    print_os_sleep_lateness();
    bool rt = opt_in_timing_critical();
    measure_sleep_accuracy("idle host");
    if (rt) {
        de_thread_clear_timing_critical();
        EXPECT_FALSE(de_thread_is_timing_critical());
    }
}

/* ---------- Under CPU contention (DENG-726) ---------- */

typedef struct busy_arg {
    de_atomic_u32 *stop;
} busy_arg;

static i32 busy_main(void *user) {
    busy_arg *a = (busy_arg *)user;
    volatile u64 n = 0;
    while (de_atomic_load_u32(a->stop, DE_ATOMIC_RELAXED) == 0)
        n++;
    return 0;
}

TEST(pacing, sleep_accuracy_contended) {
    /* The full native audit missed the gate (188/200) while a compile kept every core busy; a
     * normal thread then wakes late from its OS sleep or is preempted in its spin. Two busy
     * threads per core reproduce that inside the test (main: 168-191/200 with 30 busy
     * processes on 15 cores). The opted-in thread must still meet the unchanged gate. */
    if (!opt_in_timing_critical())
        SKIP("the OS refused a timing-critical thread; nothing to check under contention");
    enum { MAX_BUSY = 128 };
    u32 n = 2 * de_cpu_logical_cores();
    if (n > MAX_BUSY)
        n = MAX_BUSY;
    de_atomic_u32 stop = DE_ATOMIC_INIT(0);
    busy_arg arg = {&stop};
    de_thread *busy[MAX_BUSY];
    u32 started = 0;
    for (; started < n; started++) {
        if (de_thread_create(busy_main, &arg, "busy", DE_THREAD_NORMAL, 0, &busy[started]) != DE_OK)
            break;
    }
    if (started == n) {
        de_sleep_ns(50 * MS); /* let the busy threads take the cores */
        printf("busy threads: %u on %u cores\n", n, de_cpu_logical_cores());
        measure_sleep_accuracy("contended");
    }
    de_atomic_store_u32(&stop, 1, DE_ATOMIC_RELAXED);
    for (u32 i = 0; i < started; i++)
        (void)de_thread_join(busy[i]);
    de_thread_clear_timing_critical();
    EXPECT_EQ(started, n);
}

/* ---------- The timing-critical API (DENG-726) ---------- */

typedef struct tc_probe {
    bool before;
} tc_probe;

static i32 tc_other_thread(void *user) {
    tc_probe *p = (tc_probe *)user;
    p->before = de_thread_is_timing_critical();
    return 0;
}

TEST(pacing, timing_critical_api) {
    EXPECT_FALSE(de_thread_is_timing_critical()); /* default: normal scheduling */
    de_thread_clear_timing_critical();            /* no-op on a normal thread */
    EXPECT_FALSE(de_thread_is_timing_critical());

    EXPECT_EQ(de_thread_set_timing_critical(NULL), DE_ERR_INVALID_ARGUMENT);
    de_thread_timing_desc bad = {.period_ns = 2000000000ull};
    EXPECT_EQ(de_thread_set_timing_critical(&bad), DE_ERR_INVALID_ARGUMENT);
    bad = (de_thread_timing_desc){.period_ns = 10 * MS, .computation_ns = 11 * MS};
    EXPECT_EQ(de_thread_set_timing_critical(&bad), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    EXPECT_FALSE(de_thread_is_timing_critical());

    de_result r = de_thread_set_timing_critical(&(de_thread_timing_desc){0});
#if defined(__APPLE__) || defined(_WIN32)
    ASSERT_EQ(r, DE_OK); /* no privileges needed on these OSes */
#else
    if (r == DE_ERR_UNSUPPORTED) {
        de_error_clear();
        EXPECT_FALSE(de_thread_is_timing_critical());
        SKIP("SCHED_FIFO not allowed for this user (no CAP_SYS_NICE / RLIMIT_RTPRIO)");
    }
    ASSERT_EQ(r, DE_OK);
#endif
    EXPECT_TRUE(de_thread_is_timing_critical());

    /* Per thread: a thread this one creates is not timing-critical. */
    tc_probe probe = {true};
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(tc_other_thread, &probe, "tc-probe", DE_THREAD_NORMAL, 0, &t), DE_OK);
    (void)de_thread_join(t);
    EXPECT_FALSE(probe.before);

    /* Setting again replaces the settings; a 1 s period with all of it as computation is the
     * largest valid desc. */
    EXPECT_EQ(de_thread_set_timing_critical(
                  &(de_thread_timing_desc){.period_ns = 1000000000ull,
                                           .computation_ns = 1000000000ull}),
              DE_OK);
    EXPECT_TRUE(de_thread_is_timing_critical());
    /* Sleeping stays exact and never early while timing-critical. */
    for (int i = 0; i < 20; i++) {
        u64 s = de_time_now_ns();
        de_sleep_ns(3 * MS);
        EXPECT_GE(de_time_now_ns() - s, 3 * MS);
    }

    de_thread_clear_timing_critical();
    EXPECT_FALSE(de_thread_is_timing_critical());
    de_thread_clear_timing_critical(); /* twice is fine */
    EXPECT_FALSE(de_thread_is_timing_critical());
}

TEST(pacing, pacer_60hz) {
    /* Acceptance: 60 Hz over 300 frames with a 5 ms workload: mean frame time 16.67 ms
     * +/- 0.2 ms, and no cumulative drift (frame 300 starts 300 periods after frame 0). */
    enum { FRAMES = 300 };
    de_frame_pacer p;
    de_frame_pacer_init(&p, &(de_frame_pacer_desc){.target_hz = 60, .mode = DE_FRAME_PACER_CAP});

    u64 first = 0, sum = 0, min_frame = UINT64_MAX, max_frame = 0;
    for (int i = 0; i <= FRAMES; i++) {
        de_frame_pacer_begin(&p);
        if (i == 0) {
            first = p.begin_ns;
        } else {
            u64 ft = de_frame_pacer_stats_get(&p).last_frame_ns;
            sum += ft;
            if (ft < min_frame)
                min_frame = ft;
            if (ft > max_frame)
                max_frame = ft;
        }
        if (i == FRAMES)
            break;
        de_sleep_ns(5 * MS); /* the frame's work */
        de_frame_pacer_wait(&p);
    }
    u64 total = p.begin_ns - first;
    u64 expected = (u64)FRAMES * 1000000000ull / 60; /* exactly 5 s */
    de_frame_pacer_stats st = de_frame_pacer_stats_get(&p);
    double mean_ms = (double)sum / FRAMES / 1e6;
    printf("60 Hz x %d frames: mean %.4f ms, min %.3f ms, max %.3f ms, total %.3f ms "
           "(expected %.3f), missed %llu, resyncs %llu, last work %.3f ms\n",
           FRAMES, mean_ms, (double)min_frame / 1e6, (double)max_frame / 1e6, (double)total / 1e6,
           (double)expected / 1e6, (unsigned long long)st.missed_deadlines,
           (unsigned long long)st.resyncs, (double)st.last_work_ns / 1e6);

    EXPECT_EQ(st.frames, (u64)FRAMES + 1);
    EXPECT_GE(st.last_work_ns, 5 * MS);
    if (on_ci()) {
        EXPECT_FLOAT_EQ(mean_ms, 1000.0 / 60.0, 2.0);
    } else {
        EXPECT_FLOAT_EQ(mean_ms, 1000.0 / 60.0, 0.2);
        /* No drift: the schedule is epoch + n * period, so after 300 frames the error is one
         * frame's wake-up jitter, not 300 of them. */
        EXPECT_LE(abs_diff(total, expected), 1 * MS);
        EXPECT_EQ(st.resyncs, 0u);
    }
}

TEST(pacing, pacer_resync_after_stall) {
    const u64 period = 10 * MS; /* 100 Hz */
    const bool ci = on_ci();
    de_frame_pacer p;
    de_frame_pacer_init(&p, &(de_frame_pacer_desc){.target_hz = 100, .mode = DE_FRAME_PACER_CAP});

    for (int i = 0; i < 5; i++) {
        de_frame_pacer_begin(&p);
        de_sleep_ns(1 * MS);
        de_frame_pacer_wait(&p);
    }
    de_frame_pacer_stats before = de_frame_pacer_stats_get(&p);

    /* A late frame, 1.5 periods of work: a miss, but no resync. The next frames run short and
     * catch up, so frame starts stay on the original schedule. */
    de_frame_pacer_begin(&p);
    u64 epoch = p.epoch_ns, late_begin = p.begin_ns;
    de_sleep_ns(15 * MS);
    de_frame_pacer_wait(&p);
    de_frame_pacer_stats st = de_frame_pacer_stats_get(&p);
    EXPECT_GE(st.missed_deadlines, before.missed_deadlines + 1);
    if (!ci) { /* a preempted runner can turn the late frame into a stall */
        EXPECT_EQ(st.missed_deadlines, before.missed_deadlines + 1);
        EXPECT_EQ(st.resyncs, before.resyncs);
        EXPECT_EQ(p.epoch_ns, epoch);
    }
    for (int i = 0; i < 3; i++) {
        de_frame_pacer_begin(&p);
        de_sleep_ns(1 * MS);
        de_frame_pacer_wait(&p);
    }
    /* Frame k starts at deadline k = epoch + k * period. The late frame was frame 5; frame 6
     * started 5 ms late with no sleep before it, and frame 9 is back on the schedule. */
    de_frame_pacer_begin(&p);
    u64 on_schedule = epoch + 9 * period;
    printf("catch-up: late frame began %+.3f ms vs schedule, frame 9 began %+.3f ms\n",
           ((double)late_begin - (double)(epoch + 5 * period)) / 1e6,
           ((double)p.begin_ns - (double)on_schedule) / 1e6);
    if (!ci)
        EXPECT_LE(abs_diff(p.begin_ns, on_schedule), 1 * MS);
    de_sleep_ns(1 * MS);
    de_frame_pacer_wait(&p);

    /* A stall of 5 periods: the schedule restarts instead of bursting through 5 deadlines. */
    before = de_frame_pacer_stats_get(&p);
    de_frame_pacer_begin(&p);
    de_sleep_ns(50 * MS);
    u64 w0 = de_time_now_ns();
    de_frame_pacer_wait(&p);
    u64 w1 = de_time_now_ns();
    st = de_frame_pacer_stats_get(&p);
    EXPECT_EQ(st.missed_deadlines, before.missed_deadlines + 1);
    EXPECT_EQ(st.resyncs, before.resyncs + 1);
    EXPECT_LT(w1 - w0, quick_ns()); /* a late wait does not sleep */

    u64 min_frame = UINT64_MAX;
    for (int i = 0; i < 6; i++) {
        de_frame_pacer_begin(&p);
        if (i > 0) {
            u64 ft = de_frame_pacer_stats_get(&p).last_frame_ns;
            if (ft < min_frame)
                min_frame = ft;
        }
        de_sleep_ns(1 * MS);
        de_frame_pacer_wait(&p);
    }
    st = de_frame_pacer_stats_get(&p);
    printf("after the stall: shortest frame %.3f ms, resyncs %llu, missed %llu\n",
           (double)min_frame / 1e6, (unsigned long long)st.resyncs,
           (unsigned long long)st.missed_deadlines);
    if (!ci)
        EXPECT_EQ(st.resyncs, before.resyncs + 1);
    /* No burst: a burst through the missed deadlines would give frames of about 1 ms (the
     * work alone). Wake-up jitter (preemption, VMs) can shorten one frame a little, so the
     * bound is half a period. */
    if (!ci)
        EXPECT_GE(min_frame, period / 2);
}

TEST(pacing, pacer_modes) {
    de_frame_pacer p;

    /* (desc){0} and NULL: uncapped. Waits return at once; stats still count. */
    de_frame_pacer_init(&p, NULL);
    for (int i = 0; i < 3; i++) {
        de_frame_pacer_begin(&p);
        u64 t0 = de_time_now_ns();
        de_frame_pacer_wait(&p);
        EXPECT_LT(de_time_now_ns() - t0, quick_ns());
    }
    de_frame_pacer_stats st = de_frame_pacer_stats_get(&p);
    EXPECT_EQ(st.frames, 3u);
    EXPECT_EQ(st.missed_deadlines, 0u);
    EXPECT_GT(st.last_frame_ns, 0u);

    /* CAP with target 0 is uncapped too. */
    de_frame_pacer_init(&p, &(de_frame_pacer_desc){.mode = DE_FRAME_PACER_CAP});
    de_frame_pacer_begin(&p);
    u64 t0 = de_time_now_ns();
    de_frame_pacer_wait(&p);
    EXPECT_LT(de_time_now_ns() - t0, quick_ns());

    /* A wait without a begin is ignored. */
    de_frame_pacer_init(&p, &(de_frame_pacer_desc){.target_hz = 10, .mode = DE_FRAME_PACER_CAP});
    t0 = de_time_now_ns();
    de_frame_pacer_wait(&p);
    EXPECT_LT(de_time_now_ns() - t0, quick_ns());
    EXPECT_EQ(de_frame_pacer_stats_get(&p).frames, 0u);

    /* VSYNC_ASSIST wakes vsync_lead_ns before the deadline (here 3 ms before 20 ms). */
    de_frame_pacer_init(&p, &(de_frame_pacer_desc){.target_hz = 50,
                                                   .mode = DE_FRAME_PACER_VSYNC_ASSIST,
                                                   .vsync_lead_ns = 3 * MS});
    de_frame_pacer_begin(&p);
    u64 begin = p.begin_ns;
    de_frame_pacer_wait(&p);
    u64 woke = de_time_now_ns() - begin;
    EXPECT_GE(woke, 17 * MS);
    if (!on_ci()) {
        EXPECT_LT(woke, 18 * MS);
        EXPECT_EQ(de_frame_pacer_stats_get(&p).missed_deadlines, 0u);
    }

    /* set() switches to a 25 Hz cap; the schedule restarts at the next begin, stats stay. */
    de_frame_pacer_set(&p, &(de_frame_pacer_desc){.target_hz = 25, .mode = DE_FRAME_PACER_CAP});
    de_frame_pacer_begin(&p);
    begin = p.begin_ns;
    de_frame_pacer_wait(&p);
    EXPECT_GE(de_time_now_ns() - begin, 40 * MS);
    EXPECT_EQ(de_frame_pacer_stats_get(&p).frames, 2u);
    EXPECT_EQ(p.desc.vsync_lead_ns, DE_FRAME_PACER_DEFAULT_VSYNC_LEAD_NS);

    /* NULL pacers are ignored. */
    de_frame_pacer_init(NULL, NULL);
    de_frame_pacer_begin(NULL);
    de_frame_pacer_wait(NULL);
    EXPECT_EQ(de_frame_pacer_stats_get(NULL).frames, 0u);
}
