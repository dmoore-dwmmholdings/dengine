/* time_posix.c: monotonic high-resolution time on macOS and Linux (SPEC §6, DENG-147). */
#if defined(__linux__)
#define _GNU_SOURCE
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include <dengine/de_platform.h>

#include "platform/de_atomic.h"
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

de_result de_time_thread_cpu_ns(u64 *out) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "thread CPU time output is NULL");
    *out = 0;
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return DE_FAIL(DE_ERR_IO, "thread CPU time query failed");
    *out = (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
    return DE_OK;
}

#if defined(__APPLE__)

/* mach timebase as (numer << 32 | denom); 0 = not read yet. Every thread computes the same
 * value, so a race between first callers is harmless. */
static de_atomic_u64 de__timebase = DE_ATOMIC_INIT(0);

u64 de_time_now_ns(void) {
    u64 tb = de_atomic_load_u64(&de__timebase, DE_ATOMIC_RELAXED);
    if (tb == 0) {
        mach_timebase_info_data_t info;
        if (mach_timebase_info(&info) != KERN_SUCCESS || info.denom == 0)
            info.numer = info.denom = 1;
        tb = ((u64)info.numer << 32) | (u64)info.denom;
        de_atomic_store_u64(&de__timebase, tb, DE_ATOMIC_RELAXED);
    }
    u64 numer = tb >> 32, denom = tb & 0xffffffffu;
    u64 t = mach_absolute_time();
    if (numer == denom)
        return t; /* Intel-style 1/1 timebase */
    /* Split the multiply so ticks * numer cannot overflow (Apple Silicon: 125/3 at 24 MHz). */
    return t / denom * numer + t % denom * numer / denom;
}

#else

u64 de_time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

#endif
