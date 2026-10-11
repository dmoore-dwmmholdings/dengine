/* sleep_pacing.c: precise sleep and the frame pacer (SPEC §6, §10.2 step 6; DENG-249).
 *
 * One file for every OS: only the coarse OS sleep differs. de_sleep_until_ns() lets the OS sleep
 * until DE_SLEEP_SPIN_NS before the deadline, then spins on de_time_now_ns(), so it never returns
 * early and is late only when the thread is preempted. */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#elif defined(__linux__)
#define _GNU_SOURCE
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include <dengine/de_platform.h>

#include "platform/de_atomic.h"

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#else
#include <errno.h>
#include <time.h>
#endif

#define DE__NS_PER_S 1000000000ull

/* ---------- Coarse OS sleep ---------- */

#if defined(_WIN32)

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* 0 = not tried yet, 1 = high-resolution timers work, 2 = fallback (timeBeginPeriod(1) is set).
 * Racing first callers do the same probe; timeBeginPeriod may then run twice, which only adds
 * a reference the process never ends anyway. */
static de_atomic_u32 de__win_sleep_kind = DE_ATOMIC_INIT(0);

/* Sleeps about `ns` (> 0). The caller re-checks the clock, so early or late wakeups are fine. */
static void de__os_sleep(u64 ns) {
    u32 kind = de_atomic_load_u32(&de__win_sleep_kind, DE_ATOMIC_ACQUIRE);
    if (kind != 2) {
        /* A timer per call: one syscall pair per frame costs microseconds, and nothing leaks
         * when threads come and go. The high-resolution flag exists since Windows 10 1803. */
        HANDLE t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
        if (t != NULL) {
            if (kind == 0)
                de_atomic_store_u32(&de__win_sleep_kind, 1, DE_ATOMIC_RELEASE);
            LARGE_INTEGER due;
            /* Negative = relative, in 100 ns units. Round up so the timer never fires early by
             * a fraction of a unit; the spin absorbs the rest. */
            due.QuadPart = -(LONGLONG)((ns + 99) / 100);
            if (SetWaitableTimerEx(t, &due, 0, NULL, NULL, NULL, 0))
                WaitForSingleObject(t, INFINITE);
            CloseHandle(t);
            return;
        }
        if (kind == 1) {
            /* The timer worked before, so this is a transient failure (out of handles): yield
             * and let the caller spin. */
            Sleep(0);
            return;
        }
        timeBeginPeriod(1); /* 1 ms scheduler ticks for Sleep(); kept for the process life */
        de_atomic_store_u32(&de__win_sleep_kind, 2, DE_ATOMIC_RELEASE);
    }
    /* Sleep(ms) can wake up to a tick late, and the caller stopped DE_SLEEP_SPIN_NS (1 ms)
     * short of the deadline, so round down: Sleep(0) just yields. */
    Sleep((DWORD)(ns / 1000000u));
}

#else

static void de__os_sleep(u64 ns) {
    struct timespec req;
    req.tv_sec = (time_t)(ns / DE__NS_PER_S);
    req.tv_nsec = (long)(ns % DE__NS_PER_S);
    /* An interrupted sleep returns early; the caller loops on the clock, so just return. */
    (void)nanosleep(&req, NULL);
}

#endif

/* ---------- Precise sleep ---------- */

/* OS sleeps shorter than this are not worth a system call: spin instead. */
#define DE__MIN_OS_SLEEP_NS 100000ull

void de_sleep_until_ns(u64 deadline_ns) {
    for (;;) {
        u64 now = de_time_now_ns();
        if (now >= deadline_ns)
            return;
        u64 left = deadline_ns - now;
        if (left <= DE_SLEEP_SPIN_NS + DE__MIN_OS_SLEEP_NS)
            break;
        u64 os_ns = left - DE_SLEEP_SPIN_NS;
        /* Ask for half the time left. Timer leeway can grow with the requested time: macOS
         * measured 50 % of it (8 ms late on a 16 ms nanosleep or mach_wait_until) for a
         * throttled process, and a Linux guest in a VM on such a host inherits it (5 ms late on
         * 11 ms). One long sleep would blow through the spin margin; halving stays early for any
         * leeway below 100 %, at the cost of a few more wakeups (about 7 for a 16 ms wait). */
        os_ns /= 2;
        de__os_sleep(os_ns);
    }
    while (de_time_now_ns() < deadline_ns)
        de_cpu_pause();
}

void de_sleep_ns(u64 ns) {
    if (ns == 0)
        return;
    u64 now = de_time_now_ns();
    u64 deadline = now + ns;
    if (deadline < now) /* saturate: "forever" stays forever */
        deadline = UINT64_MAX;
    de_sleep_until_ns(deadline);
}

/* ---------- Frame pacer ---------- */

static bool de__pacer_capped(const de_frame_pacer *p) {
    return p->desc.target_hz != 0 && p->desc.mode != DE_FRAME_PACER_NONE;
}

static void de__pacer_apply(de_frame_pacer *p, const de_frame_pacer_desc *desc) {
    if (desc != NULL)
        p->desc = *desc;
    else
        p->desc = (de_frame_pacer_desc){0};
    if (p->desc.vsync_lead_ns == 0)
        p->desc.vsync_lead_ns = DE_FRAME_PACER_DEFAULT_VSYNC_LEAD_NS;
    p->started = false; /* the next begin starts a new schedule */
}

void de_frame_pacer_init(de_frame_pacer *p, const de_frame_pacer_desc *desc) {
    if (p == NULL)
        return;
    *p = (de_frame_pacer){0};
    de__pacer_apply(p, desc);
}

void de_frame_pacer_set(de_frame_pacer *p, const de_frame_pacer_desc *desc) {
    if (p == NULL)
        return;
    de__pacer_apply(p, desc);
}

void de_frame_pacer_begin(de_frame_pacer *p) {
    if (p == NULL)
        return;
    u64 now = de_time_now_ns();
    if (p->begin_ns != 0)
        p->stats.last_frame_ns = now - p->begin_ns;
    p->begin_ns = now;
    p->stats.frames++;
    if (!p->started) {
        p->epoch_ns = now;
        p->index = 0;
        p->started = true;
    }
}

void de_frame_pacer_wait(de_frame_pacer *p) {
    if (p == NULL || p->begin_ns == 0)
        return;
    u64 now = de_time_now_ns();
    p->stats.last_work_ns = now - p->begin_ns;
    if (!de__pacer_capped(p) || !p->started)
        return;

    u64 hz = p->desc.target_hz;
    p->index++;
    if (p->index >= hz) {
        /* hz frames are exactly one second: move the epoch by whole seconds so index * 1e9
         * never overflows, without any rounding. */
        p->epoch_ns += DE__NS_PER_S;
        p->index -= hz;
    }
    u64 deadline = p->epoch_ns + p->index * DE__NS_PER_S / hz;

    if (now > deadline) {
        p->stats.missed_deadlines++;
        u64 period = DE__NS_PER_S / hz;
        if (now - deadline > 2 * period) {
            /* A stall. Catching up would run a burst of near-zero frames; restart instead, so
             * the next deadline is one period after this frame's end. */
            p->stats.resyncs++;
            p->epoch_ns = now;
            p->index = 0;
        }
        return; /* late but close: no sleep, the next frames catch up */
    }

    u64 target = deadline;
    if (p->desc.mode == DE_FRAME_PACER_VSYNC_ASSIST) {
        /* Present blocks on the vblank and finishes the wait. */
        u64 lead = p->desc.vsync_lead_ns;
        target = deadline - now > lead ? deadline - lead : now;
    }
    de_sleep_until_ns(target);
}

de_frame_pacer_stats de_frame_pacer_stats_get(const de_frame_pacer *p) {
    if (p == NULL)
        return (de_frame_pacer_stats){0};
    return p->stats;
}
