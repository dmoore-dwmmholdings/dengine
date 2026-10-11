/* thread_win32.c: threads, sync and TLS on Windows (SPEC §6, DENG-147).
 *
 * SRWLOCK for mutexes and rwlocks, CONDITION_VARIABLE for condvars, a Win32 semaphore, and
 * fiber-local storage (FLS) for TLS, because only FLS runs a destructor on thread exit. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <dengine/de_platform.h>

#include <windows.h>
#include <process.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The FLS callback is NTAPI (__stdcall); de_tls_dtor is the default convention. Both are the one
 * x64 convention, so the destructor is passed straight through. 32-bit x86 is not a target. */
#if !defined(_WIN64)
#error "dengine targets 64-bit Windows only (SPEC §3.1)"
#endif

/* ---------- Storage checks ---------- */

_Static_assert(sizeof(SRWLOCK) <= sizeof(((de_mutex *)0)->opaque), "SRWLOCK does not fit de_mutex");
_Static_assert(_Alignof(SRWLOCK) <= _Alignof(de_mutex), "de_mutex alignment too small");
_Static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(((de_cond *)0)->opaque),
               "CONDITION_VARIABLE does not fit de_cond");
_Static_assert(_Alignof(CONDITION_VARIABLE) <= _Alignof(de_cond), "de_cond alignment too small");
_Static_assert(sizeof(SRWLOCK) <= sizeof(((de_rwlock *)0)->opaque),
               "SRWLOCK does not fit de_rwlock");
_Static_assert(sizeof(HANDLE) <= sizeof(((de_sem *)0)->opaque), "HANDLE does not fit de_sem");
_Static_assert(sizeof(DWORD) <= sizeof(u64), "FLS index does not fit de_tls_key");

#define DE__NS_PER_MS 1000000ull

/* An OS sync call failed in a way only misuse or memory corruption can cause: panic. */
#define DE__FATAL(what)                                                                            \
    DE_PANIC("%s failed (GetLastError %lu)", (what), (unsigned long)GetLastError())

static SRWLOCK *de__srw(de_mutex *m) {
    return (SRWLOCK *)(void *)m->opaque;
}
static SRWLOCK *de__srw_rw(de_rwlock *l) {
    return (SRWLOCK *)(void *)l->opaque;
}
static CONDITION_VARIABLE *de__cv(de_cond *c) {
    return (CONDITION_VARIABLE *)(void *)c->opaque;
}
static HANDLE de__sem_handle(de_sem *s) {
    HANDLE h;
    memcpy(&h, s->opaque, sizeof h);
    return h;
}

/* Timeout in ns -> milliseconds for the Win32 wait functions: rounded up, so a wait never ends
 * early, and capped below INFINITE. */
static DWORD de__timeout_ms(u64 timeout_ns) {
    u64 ms = timeout_ns / DE__NS_PER_MS + (timeout_ns % DE__NS_PER_MS != 0);
    return ms >= (u64)INFINITE ? INFINITE - 1u : (DWORD)ms;
}

/* Rounding up is not enough on its own: Win32 wait timeouts run on the system timer tick
 * (15.6 ms by default), so a wait can report a timeout up to about one tick before the requested
 * time has passed on the QueryPerformanceCounter clock that de_time_now_ns() reads (CI saw a
 * 20 ms semaphore wait return after 14.4 ms, DENG-359). The timed waits below therefore check
 * their deadline on that clock and never report a timeout before it. */
static u64 de__deadline_ns(u64 timeout_ns) {
    u64 now = de_time_now_ns();
    return timeout_ns > UINT64_MAX - now ? UINT64_MAX : now + timeout_ns;
}

/* ---------- Threads ---------- */

struct de_thread {
    HANDLE handle;
    de_thread_fn fn;
    void *user;
    de_thread_priority prio;
    i32 result;
    char name[DE_THREAD_NAME_MAX + 1];
};

static void de__copy_name(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (src) {
        while (n + 1 < cap && src[n] != '\0')
            n++;
        /* When truncating, do not cut a UTF-8 sequence in half. */
        while (n > 0 && src[n] != '\0' && ((unsigned char)src[n] & 0xC0u) == 0x80u)
            n--;
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

void de_thread_set_name(const char *name) {
    if (!name)
        return;
    char utf8[DE_THREAD_NAME_MAX + 1];
    wchar_t wide[DE_THREAD_NAME_MAX + 1];
    de__copy_name(utf8, sizeof utf8, name);
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, DE_THREAD_NAME_MAX + 1);
    if (n <= 0)
        return;
    /* Windows 10 1607+. Debuggers and ETW/PIX show it; failure only loses the name. */
    (void)SetThreadDescription(GetCurrentThread(), wide);
}

static unsigned __stdcall de__thread_main(void *arg) {
    de_thread *t = (de_thread *)arg;
    if (t->name[0] != '\0')
        de_thread_set_name(t->name);
    t->result = t->fn(t->user);
    return 0;
}

de_result de_thread_create(de_thread_fn fn, void *user, const char *name, de_thread_priority prio,
                           size_t stack_bytes, de_thread **out) {
    if (out)
        *out = NULL;
    if (!fn || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_thread_create: fn and out are required");

    de_thread *t = (de_thread *)calloc(1, sizeof *t);
    if (!t)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_thread_create: no memory for the thread record");
    t->fn = fn;
    t->user = user;
    t->prio = prio;
    de__copy_name(t->name, sizeof t->name, name);

    size_t stack = stack_bytes ? stack_bytes : DE_THREAD_DEFAULT_STACK_BYTES;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t gran = si.dwPageSize ? si.dwPageSize : 4096u;
    stack = (stack + gran - 1) / gran * gran;
    if (stack > 0xffffffffu)
        stack = 0xffffffffu / gran * gran;

    /* Created suspended so the priority is in place before the entry point runs. */
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack, de__thread_main, t,
                                 CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (h == 0) {
        free(t);
        return DE_FAIL(DE_ERR_INTERNAL, "de_thread_create '%s': _beginthreadex failed (errno %d)",
                       name ? name : "", errno);
    }
    t->handle = (HANDLE)h;

    static const int prio_of[] = {THREAD_PRIORITY_BELOW_NORMAL, THREAD_PRIORITY_NORMAL,
                                  THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_TIME_CRITICAL};
    if ((unsigned)prio < 4u && prio != DE_THREAD_NORMAL)
        (void)SetThreadPriority(t->handle, prio_of[prio]); /* a hint; failure is ignored */

    if (ResumeThread(t->handle) == (DWORD)-1) {
        TerminateThread(t->handle, 1); /* never ran user code */
        CloseHandle(t->handle);
        free(t);
        return DE_FAIL(DE_ERR_INTERNAL, "de_thread_create '%s': ResumeThread failed (%lu)",
                       name ? name : "", (unsigned long)GetLastError());
    }
    *out = t;
    return DE_OK;
}

i32 de_thread_join(de_thread *t) {
    if (!t)
        return 0;
    if (WaitForSingleObject(t->handle, INFINITE) != WAIT_OBJECT_0)
        DE__FATAL("WaitForSingleObject(thread)");
    i32 r = t->result;
    CloseHandle(t->handle);
    free(t);
    return r;
}

de_result de_thread_set_affinity(de_thread *t, u64 core_mask) {
    if (!t || core_mask == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_thread_set_affinity: no thread or empty mask");
    /* Processor group 0 only: machines with more than 64 logical cores are out of scope. */
    if (SetThreadAffinityMask(t->handle, (DWORD_PTR)core_mask) == 0) {
        DWORD e = GetLastError();
        return DE_FAIL(e == ERROR_INVALID_PARAMETER ? DE_ERR_INVALID_ARGUMENT : DE_ERR_INTERNAL,
                       "de_thread_set_affinity: SetThreadAffinityMask failed (%lu)",
                       (unsigned long)e);
    }
    return DE_OK;
}

u32 de_thread_current_id(void) {
    u32 id = (u32)GetCurrentThreadId(); /* already a cheap TEB read; no cache needed */
    return id ? id : 0x80000000u;
}

u32 de_cpu_logical_cores(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (u32)n : 1u;
}

/* ---------- Mutex (exclusive SRWLOCK) ---------- */

void de_mutex_init(de_mutex *m) {
    memset(m, 0, sizeof *m);
    InitializeSRWLock(de__srw(m));
}

void de_mutex_lock(de_mutex *m) {
    AcquireSRWLockExclusive(de__srw(m));
}

bool de_mutex_try_lock(de_mutex *m) {
    return TryAcquireSRWLockExclusive(de__srw(m)) != 0;
}

void de_mutex_unlock(de_mutex *m) {
    ReleaseSRWLockExclusive(de__srw(m));
}

void de_mutex_destroy(de_mutex *m) {
    (void)m; /* SRWLOCK owns no resources */
}

/* ---------- Condition variable ---------- */

void de_cond_init(de_cond *c) {
    memset(c, 0, sizeof *c);
    InitializeConditionVariable(de__cv(c));
}

void de_cond_wait(de_cond *c, de_mutex *m) {
    if (!SleepConditionVariableSRW(de__cv(c), de__srw(m), INFINITE, 0))
        DE__FATAL("SleepConditionVariableSRW");
}

bool de_cond_wait_timeout(de_cond *c, de_mutex *m, u64 timeout_ns) {
    u64 deadline = de__deadline_ns(timeout_ns);
    if (SleepConditionVariableSRW(de__cv(c), de__srw(m), de__timeout_ms(timeout_ns), 0))
        return true;
    if (GetLastError() != ERROR_TIMEOUT)
        DE__FATAL("SleepConditionVariableSRW");
    /* A timeout before the deadline is reported as a wakeup, which the contract allows
     * (spurious wakeups); the caller re-checks its predicate and waits again. Waiting again here
     * instead could miss a signal sent while this thread holds the mutex between the waits. */
    return de_time_now_ns() < deadline;
}

void de_cond_signal(de_cond *c) {
    WakeConditionVariable(de__cv(c));
}

void de_cond_broadcast(de_cond *c) {
    WakeAllConditionVariable(de__cv(c));
}

void de_cond_destroy(de_cond *c) {
    (void)c; /* CONDITION_VARIABLE owns no resources */
}

/* ---------- Reader/writer lock (SRWLOCK) ---------- */

void de_rwlock_init(de_rwlock *l) {
    memset(l, 0, sizeof *l);
    InitializeSRWLock(de__srw_rw(l));
}

void de_rwlock_read_lock(de_rwlock *l) {
    AcquireSRWLockShared(de__srw_rw(l));
}

void de_rwlock_read_unlock(de_rwlock *l) {
    ReleaseSRWLockShared(de__srw_rw(l));
}

void de_rwlock_write_lock(de_rwlock *l) {
    AcquireSRWLockExclusive(de__srw_rw(l));
}

void de_rwlock_write_unlock(de_rwlock *l) {
    ReleaseSRWLockExclusive(de__srw_rw(l));
}

void de_rwlock_destroy(de_rwlock *l) {
    (void)l;
}

/* ---------- Semaphore ---------- */

de_result de_sem_init(de_sem *s, u32 initial) {
    memset(s, 0, sizeof *s);
    if (initial > (u32)LONG_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_sem_init: initial count %u too large", initial);
    HANDLE h = CreateSemaphoreW(NULL, (LONG)initial, LONG_MAX, NULL);
    if (!h)
        return DE_FAIL(DE_ERR_INTERNAL, "de_sem_init: CreateSemaphoreW failed (%lu)",
                       (unsigned long)GetLastError());
    memcpy(s->opaque, &h, sizeof h);
    return DE_OK;
}

void de_sem_post(de_sem *s, u32 count) {
    if (count == 0)
        return;
    if (!ReleaseSemaphore(de__sem_handle(s), (LONG)count, NULL))
        DE__FATAL("ReleaseSemaphore");
}

void de_sem_wait(de_sem *s) {
    if (WaitForSingleObject(de__sem_handle(s), INFINITE) != WAIT_OBJECT_0)
        DE__FATAL("WaitForSingleObject(semaphore)");
}

bool de_sem_try_wait(de_sem *s) {
    return WaitForSingleObject(de__sem_handle(s), 0) == WAIT_OBJECT_0;
}

bool de_sem_wait_timeout(de_sem *s, u64 timeout_ns) {
    u64 deadline = de__deadline_ns(timeout_ns);
    for (;;) {
        DWORD r = WaitForSingleObject(de__sem_handle(s), de__timeout_ms(timeout_ns));
        if (r == WAIT_OBJECT_0)
            return true;
        if (r != WAIT_TIMEOUT)
            DE__FATAL("WaitForSingleObject(semaphore)");
        /* Ended early on the tick clock: wait for the rest (see de__deadline_ns). */
        u64 now = de_time_now_ns();
        if (now >= deadline)
            return false;
        timeout_ns = deadline - now;
    }
}

void de_sem_destroy(de_sem *s) {
    HANDLE h = de__sem_handle(s);
    if (h)
        CloseHandle(h);
    memset(s, 0, sizeof *s);
}

/* ---------- Thread-local storage (FLS) ----------
 * de_tls_key.id is the FLS index + 1, so that the zero key stays invalid. */

de_result de_tls_key_create(de_tls_key *out, de_tls_dtor dtor) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_tls_key_create: out is required");
    out->id = 0;
    /* Cast through a generic function pointer: same convention on x64 (see the top of file). */
    DWORD idx = FlsAlloc((PFLS_CALLBACK_FUNCTION)(void (*)(void))dtor);
    if (idx == FLS_OUT_OF_INDEXES)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_tls_key_create: no free FLS index");
    out->id = (u64)idx + 1u;
    return DE_OK;
}

void de_tls_key_destroy(de_tls_key key) {
    if (key.id != 0)
        (void)FlsFree((DWORD)(key.id - 1u));
}

void *de_tls_get(de_tls_key key) {
    return key.id != 0 ? FlsGetValue((DWORD)(key.id - 1u)) : NULL;
}

de_result de_tls_set(de_tls_key key, void *value) {
    if (key.id == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_tls_set: invalid key");
    if (!FlsSetValue((DWORD)(key.id - 1u), value))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_tls_set: FlsSetValue failed (%lu)",
                       (unsigned long)GetLastError());
    return DE_OK;
}
