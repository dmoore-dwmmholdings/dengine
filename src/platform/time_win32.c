/* time_win32.c: monotonic high-resolution time on Windows (SPEC §6, DENG-147). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <dengine/de_platform.h>

#include <windows.h>

#include "platform/de_atomic.h"

/* QueryPerformanceFrequency is fixed at boot; 0 = not read yet. A race between first callers
 * stores the same value twice, which is harmless. */
static de_atomic_u64 de__qpc_freq = DE_ATOMIC_INIT(0);

u64 de_time_now_ns(void) {
    u64 freq = de_atomic_load_u64(&de__qpc_freq, DE_ATOMIC_RELAXED);
    if (freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f); /* cannot fail on Windows XP and later */
        freq = f.QuadPart > 0 ? (u64)f.QuadPart : 1u;
        de_atomic_store_u64(&de__qpc_freq, freq, DE_ATOMIC_RELAXED);
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    u64 ticks = (u64)c.QuadPart;
    /* Split the multiply so ticks * 1e9 cannot overflow (10 MHz QPC overflows after ~30 min). */
    return ticks / freq * 1000000000ull + ticks % freq * 1000000000ull / freq;
}
