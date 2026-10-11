/* Tests for de_time_now_ns (DENG-147). */
#if defined(__linux__)
#define _GNU_SOURCE
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include "de_test.h"

#include <dengine/de_platform.h>

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

TEST(time, rate) {
    /* de_time_now_ns runs at the rate of the wall clock: over a 100 ms sleep the two deltas
     * agree within 5 %. The sleep itself may overshoot (scheduling); both clocks see that. */
    u64 w0 = wall_ns();
    u64 t0 = de_time_now_ns();
    sleep_100ms();
    u64 t1 = de_time_now_ns();
    u64 w1 = wall_ns();

    u64 dt = t1 - t0;
    u64 dw = w1 - w0;
    EXPECT_GE(dt, 95000000u); /* slept at least ~100 ms */
    EXPECT_GE(dw, 95000000u);
    EXPECT_GE(dt * 100, dw * 95);
    EXPECT_LE(dt * 100, dw * 105);
}
