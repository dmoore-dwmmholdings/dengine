/* Tests for de_time_now_ns (DENG-147). */
#if defined(__linux__)
#define _GNU_SOURCE
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include "de_test.h"

#include <dengine/de_platform.h>

#include <stdio.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

TEST(time, monotonic) {
    enum { CALLS = 1000000 };
    u64 first = de_time_now_ns();
    u64 prev = first;
    u32 backwards = 0;
    for (u32 i = 0; i < CALLS; i++) {
        u64 now = de_time_now_ns();
        if (now < prev)
            backwards++;
        prev = now;
    }
    EXPECT_EQ(backwards, 0u);
    EXPECT_GT(prev, first); /* a million calls take longer than one tick */
    EXPECT_GT(first, 0u);
}

/* Wall clock independent of the code under test: C11 timespec_get. */
static u64 wall_ns(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static void sleep_100ms(void) {
#if defined(_WIN32)
    Sleep(100);
#else
    struct timespec req = {0, 100000000L}, rem;
    while (nanosleep(&req, &rem) != 0)
        req = rem;
#endif
}

/* The rate check. A clock sample is a bracket: a wall read, a read of the clock under test, a
 * wall read. The clock read happened at a wall time inside [lo, hi]. Over a window between two
 * samples a and b, the wall time between the two clock reads therefore lies in
 * [b.lo - a.hi, b.hi - a.lo], however long the thread was preempted between the reads. The first
 * version read wall, clock, sleep, clock, wall and compared the outer wall pair with the inner
 * clock pair: a preemption between a wall read and a clock read added to the wall delta only, and
 * Linux Clang TSan on a loaded runner failed it with 100.09 ms against 107.05 ms (DENG-377). */
typedef u64 (*clock_fn)(void);

typedef struct clock_sample {
    u64 t;      /* the clock under test */
    u64 lo, hi; /* wall clock just before and just after */
} clock_sample;

enum { SAMPLE_TRIES = 16, RATE_WINDOWS = 5 };

/* The tightest of SAMPLE_TRIES brackets. One preemption widens only the bracket it lands in. */
static clock_sample sample_clock(clock_fn clock) {
    clock_sample best = {0, 0, 0};
    u64 best_width = ~(u64)0;
    for (u32 i = 0; i < SAMPLE_TRIES; i++) {
        u64 lo = wall_ns();
        u64 t = clock();
        u64 hi = wall_ns();
        if (hi >= lo && hi - lo < best_width) { /* hi < lo: the wall clock was stepped back */
            best_width = hi - lo;
            best.t = t;
            best.lo = lo;
            best.hi = hi;
        }
    }
    return best;
}

typedef enum rate_verdict {
    RATE_OK,     /* within 5 % of the wall clock, in a window measured to 1 % */
    RATE_WRONG,  /* outside 5 % even with the whole bracket uncertainty in its favour */
    RATE_UNSURE, /* no window was measured to 1 % */
} rate_verdict;

typedef struct rate_window {
    u64 dt;             /* delta of the clock under test */
    u64 dw_min, dw_max; /* bounds of the wall time between the two clock reads */
} rate_window;

/* Measures `clock` against the wall clock over 100 ms windows, at most RATE_WINDOWS of them.
 * `clock` passes when dt is within 5 % of every wall delta the brackets allow, in a window whose
 * slack (dw_max - dw_min) is at most 1 % of it. It fails at once in a window that rules out every
 * such wall delta. A clock running more than about 6 % off (5 % plus the 1 % slack) is rejected
 * by every 1 % window, so it never passes. */
static rate_verdict measure_rate(clock_fn clock, rate_window *out) {
    for (u32 w = 0; w < RATE_WINDOWS; w++) {
        clock_sample a = sample_clock(clock);
        sleep_100ms();
        clock_sample b = sample_clock(clock);
        if (a.hi == 0 || b.lo < a.hi) /* no valid bracket, or the wall clock stepped back */
            continue;
        if (b.t < a.t) {
            *out = (rate_window){0, b.lo - a.hi, b.hi - a.lo};
            return RATE_WRONG; /* the clock under test went backwards */
        }
        *out = (rate_window){b.t - a.t, b.lo - a.hi, b.hi - a.lo};
        if (out->dt * 100 < out->dw_min * 95 || out->dt * 100 > out->dw_max * 105)
            return RATE_WRONG;
        if ((out->dw_max - out->dw_min) * 100 <= out->dw_min)
            return RATE_OK;
    }
    return RATE_UNSURE;
}

static void print_window(const char *what, rate_verdict v, const rate_window *w) {
    fprintf(stderr, "%s: verdict %d, dt %llu ns, wall %llu..%llu ns\n", what, (int)v,
            (unsigned long long)w->dt, (unsigned long long)w->dw_min,
            (unsigned long long)w->dw_max);
}

TEST(time, rate) {
    /* de_time_now_ns runs at the rate of the wall clock: over a 100 ms sleep the two deltas
     * agree within 5 %. The sleep itself may overshoot (scheduling); both clocks see that. */
    rate_window w = {0, 0, 0};
    rate_verdict v = measure_rate(de_time_now_ns, &w);
    if (v != RATE_OK)
        print_window("de_time_now_ns", v, &w);
    EXPECT_EQ(v, RATE_OK);
    EXPECT_GE(w.dt, 95000000u); /* slept at least ~100 ms */
    EXPECT_GE(w.dw_max, 95000000u);
}

/* Test-only clocks that run at 90 % and 110 % of de_time_now_ns from `scaled_base` on. */
static u64 scaled_base;
static u64 clock_slow(void) {
    return scaled_base + (de_time_now_ns() - scaled_base) * 90 / 100;
}
static u64 clock_fast(void) {
    return scaled_base + (de_time_now_ns() - scaled_base) * 110 / 100;
}

TEST(time, rate_detects_wrong_rate) {
    /* The check of time.rate still rejects a clock at the wrong rate. */
    rate_window w = {0, 0, 0};
    scaled_base = de_time_now_ns();
    rate_verdict v = measure_rate(clock_slow, &w);
    if (v != RATE_WRONG)
        print_window("90 % clock", v, &w);
    EXPECT_EQ(v, RATE_WRONG);

    scaled_base = de_time_now_ns();
    v = measure_rate(clock_fast, &w);
    if (v != RATE_WRONG)
        print_window("110 % clock", v, &w);
    EXPECT_EQ(v, RATE_WRONG);
}
