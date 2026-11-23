/* thread_timing.c: opt-in timing-critical threads (de_platform.h, DENG-726).
 *
 * A normal (timeshare) thread under CPU contention wakes from its OS sleep late, or is preempted
 * for a whole quantum while de_sleep_until_ns() spins. The owner chose an opt-in call that
 * moves one thread (the frame loop) into the OS's real-time class instead of boosting every
 * sleeper. One file for every OS: each backend works on the calling thread only.
 * - macOS: Mach THREAD_TIME_CONSTRAINT_POLICY; THREAD_STANDARD_POLICY to undo.
 * - Windows: MMCSS task "Games" through avrt.dll, loaded at run time (no import library).
 * - Linux: SCHED_FIFO | SCHED_RESET_ON_FORK; the previous policy is restored on clear. */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#elif defined(__linux__)
#define _GNU_SOURCE /* SCHED_RESET_ON_FORK */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE /* Mach thread policies under -std=c11 */
#endif

#include <dengine/de_platform.h>

#include "core/de_compiler.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#elif defined(__linux__)
#include <errno.h>
#include <sched.h>
#include <string.h>
#endif

#define DE__TC_MAX_PERIOD_NS   1000000000ull
#define DE__TC_DEFAULT_HZ      60ull
#define DE__TC_LINUX_FIFO_PRIO 10

/* Validates `desc` and fills in the defaults. */
static de_result de__tc_resolve(const de_thread_timing_desc *desc, u64 *period, u64 *comp) {
    if (desc == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_thread_set_timing_critical: desc is NULL");
    if (desc->period_ns > DE__TC_MAX_PERIOD_NS)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_thread_set_timing_critical: period %llu ns is above 1 s",
                       (unsigned long long)desc->period_ns);
    *period = desc->period_ns != 0 ? desc->period_ns : DE__TC_MAX_PERIOD_NS / DE__TC_DEFAULT_HZ;
    if (desc->computation_ns > *period)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_thread_set_timing_critical: computation %llu ns is above the period",
                       (unsigned long long)desc->computation_ns);
    *comp = desc->computation_ns != 0 ? desc->computation_ns : *period / 2;
    return DE_OK;
}

/* ---------- macOS ---------- */
#if defined(__APPLE__)

/* XNU refuses a computation outside [min_rt_quantum, max_rt_quantum]; these are inside it. */
#define DE__TC_MAC_MIN_COMP_NS 50000ull
#define DE__TC_MAC_MAX_COMP_NS 50000000ull

static u32 de__ns_to_abs(u64 ns) {
    static mach_timebase_info_data_t tb; /* racing first calls write the same values */
    if (tb.denom == 0)
        (void)mach_timebase_info(&tb);
    u64 abs = ns * tb.denom / tb.numer; /* ns <= 1e9 and denom is small: no overflow */
    return abs > UINT32_MAX ? UINT32_MAX : (u32)abs;
}

de_result de_thread_set_timing_critical(const de_thread_timing_desc *desc) {
    u64 period, comp;
    DE_TRY(de__tc_resolve(desc, &period, &comp));
    if (comp < DE__TC_MAC_MIN_COMP_NS)
        comp = DE__TC_MAC_MIN_COMP_NS;
    if (comp > DE__TC_MAC_MAX_COMP_NS)
        comp = DE__TC_MAC_MAX_COMP_NS;
    u64 constraint = period > comp ? period : comp;
    thread_time_constraint_policy_data_t pol;
    pol.period = de__ns_to_abs(period);
    pol.computation = de__ns_to_abs(comp);
    pol.constraint = de__ns_to_abs(constraint);
    pol.preemptible = TRUE; /* other real-time threads (audio) may still run first */
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()),
                                         THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol,
                                         THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr == KERN_SUCCESS)
        return DE_OK;
    if (kr == KERN_INVALID_ARGUMENT || kr == KERN_NOT_SUPPORTED || kr == KERN_POLICY_LIMIT)
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "de_thread_set_timing_critical: time-constraint policy refused (%d)",
                       (int)kr);
    return DE_FAIL(DE_ERR_INTERNAL, "de_thread_set_timing_critical: thread_policy_set failed (%d)",
                   (int)kr);
}

void de_thread_clear_timing_critical(void) {
    if (!de_thread_is_timing_critical())
        return;
    thread_standard_policy_data_t pol = {0};
    (void)thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_STANDARD_POLICY,
                            (thread_policy_t)&pol, THREAD_STANDARD_POLICY_COUNT);
}

bool de_thread_is_timing_critical(void) {
    /* Ask the kernel, so a policy the kernel dropped (failsafe demotion) is not reported. */
    thread_time_constraint_policy_data_t pol;
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t get_default = FALSE;
    kern_return_t kr = thread_policy_get(pthread_mach_thread_np(pthread_self()),
                                         THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol,
                                         &count, &get_default);
    return kr == KERN_SUCCESS && !get_default;
}

/* ---------- Windows ---------- */
#elif defined(_WIN32)

typedef HANDLE(WINAPI *de__av_set_fn)(LPCWSTR task, LPDWORD index);
typedef BOOL(WINAPI *de__av_revert_fn)(HANDLE h);

static DE_THREAD_LOCAL HANDLE de__tc_mmcss;

/* avrt.dll from System32, resolved once; racing first calls resolve the same pointers. */
static bool de__avrt(de__av_set_fn *set, de__av_revert_fn *revert) {
    static de__av_set_fn s_set;
    static de__av_revert_fn s_revert;
    if (s_set == NULL || s_revert == NULL) {
        HMODULE m = LoadLibraryExW(L"avrt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m == NULL)
            return false;
        /* Through void (*)(void): a direct FARPROC cast trips -Wcast-function-type. */
        s_revert = (de__av_revert_fn)(void (*)(void))GetProcAddress(
            m, "AvRevertMmThreadCharacteristics");
        s_set = (de__av_set_fn)(void (*)(void))GetProcAddress(m, "AvSetMmThreadCharacteristicsW");
    }
    *set = s_set;
    *revert = s_revert;
    return s_set != NULL && s_revert != NULL;
}

de_result de_thread_set_timing_critical(const de_thread_timing_desc *desc) {
    u64 period, comp;
    DE_TRY(de__tc_resolve(desc, &period, &comp));
    (void)period; /* MMCSS schedules by task class, not by a declared period */
    (void)comp;
    if (de__tc_mmcss != NULL)
        return DE_OK; /* already registered; the task class has no settings to replace */
    de__av_set_fn set;
    de__av_revert_fn revert;
    if (!de__avrt(&set, &revert))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "de_thread_set_timing_critical: avrt.dll unavailable");
    DWORD index = 0;
    HANDLE h = set(L"Games", &index);
    if (h == NULL)
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "de_thread_set_timing_critical: MMCSS refused task \"Games\" (%lu)",
                       (unsigned long)GetLastError());
    de__tc_mmcss = h;
    return DE_OK;
}

void de_thread_clear_timing_critical(void) {
    if (de__tc_mmcss == NULL)
        return;
    de__av_set_fn set;
    de__av_revert_fn revert;
    if (de__avrt(&set, &revert))
        (void)revert(de__tc_mmcss);
    de__tc_mmcss = NULL;
}

bool de_thread_is_timing_critical(void) {
    return de__tc_mmcss != NULL;
}

/* ---------- Linux ---------- */
#elif defined(__linux__)

/* The policy and parameters before the first set, restored by clear. */
static DE_THREAD_LOCAL bool de__tc_saved;
static DE_THREAD_LOCAL int de__tc_old_policy;
static DE_THREAD_LOCAL struct sched_param de__tc_old_param;

de_result de_thread_set_timing_critical(const de_thread_timing_desc *desc) {
    u64 period, comp;
    DE_TRY(de__tc_resolve(desc, &period, &comp));
    (void)period; /* SCHED_FIFO has no period; SCHED_DEADLINE would, but needs root */
    (void)comp;
    if (!de__tc_saved) {
        /* pid 0: the calling thread (Linux schedules threads, not processes). */
        int old = sched_getscheduler(0);
        if (old < 0 || sched_getparam(0, &de__tc_old_param) != 0)
            return DE_FAIL(DE_ERR_INTERNAL, "de_thread_set_timing_critical: sched_getscheduler "
                                            "failed (%d)",
                           errno);
        de__tc_old_policy = old & ~SCHED_RESET_ON_FORK;
    }
    struct sched_param p;
    memset(&p, 0, sizeof p);
    p.sched_priority = DE__TC_LINUX_FIFO_PRIO;
    if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &p) != 0) {
        int err = errno;
        if (err == EPERM)
            return DE_FAIL(DE_ERR_UNSUPPORTED,
                           "de_thread_set_timing_critical: SCHED_FIFO not permitted (needs "
                           "CAP_SYS_NICE or RLIMIT_RTPRIO >= %d)",
                           DE__TC_LINUX_FIFO_PRIO);
        return DE_FAIL(DE_ERR_INTERNAL, "de_thread_set_timing_critical: sched_setscheduler "
                                        "failed (%d: %s)",
                       err, strerror(err));
    }
    de__tc_saved = true;
    return DE_OK;
}

void de_thread_clear_timing_critical(void) {
    if (!de__tc_saved)
        return;
    (void)sched_setscheduler(0, de__tc_old_policy, &de__tc_old_param);
    de__tc_saved = false;
}

bool de_thread_is_timing_critical(void) {
    int p = sched_getscheduler(0);
    return p >= 0 && (p & ~SCHED_RESET_ON_FORK) == SCHED_FIFO;
}

/* ---------- Other OSes ---------- */
#else

de_result de_thread_set_timing_critical(const de_thread_timing_desc *desc) {
    u64 period, comp;
    DE_TRY(de__tc_resolve(desc, &period, &comp));
    (void)period;
    (void)comp;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_thread_set_timing_critical: not supported on this OS");
}

void de_thread_clear_timing_critical(void) {
}

bool de_thread_is_timing_critical(void) {
    return false;
}

#endif
