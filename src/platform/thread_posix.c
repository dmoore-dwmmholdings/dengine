/* thread_posix.c: threads, sync and TLS on macOS and Linux (SPEC §6, DENG-147).
 *
 * pthreads for threads, mutexes, condvars, rwlocks and TLS. Semaphores: dispatch_semaphore on
 * macOS (unnamed POSIX semaphores are not implemented there), sem_t on Linux. */
#if defined(__linux__)
#define _GNU_SOURCE /* pthread_setname_np, pthread_setaffinity_np, sem_clockwait */
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE /* pthread_setname_np, QoS, mach thread policies under -std=c11 */
#endif

#include <dengine/de_platform.h>

#include "core/de_compiler.h"
#include "platform/mutex_prof.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sched.h>
#include <semaphore.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

/* ---------- Storage checks: the OS objects must fit the public opaque storage ---------- */

_Static_assert(sizeof(pthread_mutex_t) <= sizeof(((de_mutex *)0)->opaque),
               "pthread_mutex_t does not fit de_mutex");
_Static_assert(_Alignof(pthread_mutex_t) <= _Alignof(de_mutex), "de_mutex alignment too small");
_Static_assert(sizeof(pthread_cond_t) <= sizeof(((de_cond *)0)->opaque),
               "pthread_cond_t does not fit de_cond");
_Static_assert(_Alignof(pthread_cond_t) <= _Alignof(de_cond), "de_cond alignment too small");
_Static_assert(sizeof(pthread_rwlock_t) <= sizeof(((de_rwlock *)0)->opaque),
               "pthread_rwlock_t does not fit de_rwlock");
_Static_assert(_Alignof(pthread_rwlock_t) <= _Alignof(de_rwlock), "de_rwlock alignment too small");
#if defined(__APPLE__)
_Static_assert(sizeof(dispatch_semaphore_t) <= sizeof(((de_sem *)0)->opaque),
               "dispatch_semaphore_t does not fit de_sem");
#else
_Static_assert(sizeof(sem_t) <= sizeof(((de_sem *)0)->opaque), "sem_t does not fit de_sem");
_Static_assert(_Alignof(sem_t) <= _Alignof(de_sem), "de_sem alignment too small");
#endif
_Static_assert(sizeof(pthread_key_t) <= sizeof(u64), "pthread_key_t does not fit de_tls_key");

#define DE__NS_PER_S 1000000000ull

/* An OS sync primitive failed in a way that only memory corruption or misuse (unlocking a
 * mutex the thread does not hold, destroying a locked one) can cause: panic. */
#define DE__FATAL(what, err) DE_PANIC("%s failed (%d: %s)", (what), (err), strerror(err))

#define DE__CHECK(call)                                                                            \
    do {                                                                                           \
        int de__err = (call);                                                                      \
        if (de__err != 0)                                                                          \
            DE__FATAL(#call, de__err);                                                             \
    } while (0)

static pthread_mutex_t *de__mtx(de_mutex *m) {
    return (pthread_mutex_t *)(void *)m->opaque;
}
static pthread_cond_t *de__cnd(de_cond *c) {
    return (pthread_cond_t *)(void *)c->opaque;
}
static pthread_rwlock_t *de__rwl(de_rwlock *l) {
    return (pthread_rwlock_t *)(void *)l->opaque;
}

/* ---------- Threads ---------- */

struct de_thread {
    pthread_t handle;
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
    de__thread_prof_name(name);
#if defined(__APPLE__)
    char buf[DE_THREAD_NAME_MAX + 1];
    de__copy_name(buf, sizeof buf, name);
    pthread_setname_np(buf); /* macOS can only name the calling thread */
#elif defined(__linux__)
    char buf[16]; /* TASK_COMM_LEN: longer names make pthread_setname_np fail with ERANGE */
    de__copy_name(buf, sizeof buf, name);
    pthread_setname_np(pthread_self(), buf);
#else
    (void)name;
#endif
}

#if defined(__linux__)
static void de__apply_priority(de_thread_priority prio) {
    /* Linux applies setpriority(PRIO_PROCESS, tid) to one thread. Raising (negative nice) needs
     * CAP_SYS_NICE or RLIMIT_NICE, so a failure is ignored: priorities are a hint. */
    static const int nice_of[] = {5, 0, -5, -10};
    if ((unsigned)prio >= sizeof nice_of / sizeof nice_of[0] || nice_of[prio] == 0)
        return;
    pid_t tid = (pid_t)syscall(SYS_gettid);
    (void)setpriority(PRIO_PROCESS, (id_t)tid, nice_of[prio]);
}
#endif

static const char *de__name_or(const char *name) {
    return name ? name : "";
}

static void *de__thread_main(void *arg) {
    de_thread *t = (de_thread *)arg;
    if (t->name[0] != '\0')
        de_thread_set_name(t->name);
#if defined(__linux__)
    de__apply_priority(t->prio);
#endif
    t->result = t->fn(t->user);
    return NULL;
}

static size_t de__round_stack(size_t bytes) {
    long page = sysconf(_SC_PAGESIZE);
    size_t p = page > 0 ? (size_t)page : 4096u;
    if (bytes == 0)
        bytes = DE_THREAD_DEFAULT_STACK_BYTES;
    if (bytes < (size_t)PTHREAD_STACK_MIN)
        bytes = (size_t)PTHREAD_STACK_MIN;
    return (bytes + p - 1) / p * p;
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

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        free(t);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_thread_create: pthread_attr_init failed");
    }
    int err = pthread_attr_setstacksize(&attr, de__round_stack(stack_bytes));
#if defined(__APPLE__)
    if (err == 0) {
        static const qos_class_t qos_of[] = {QOS_CLASS_UTILITY, QOS_CLASS_DEFAULT,
                                             QOS_CLASS_USER_INITIATED, QOS_CLASS_USER_INTERACTIVE};
        qos_class_t qos = (unsigned)prio < 4u ? qos_of[prio] : QOS_CLASS_DEFAULT;
        err = pthread_attr_set_qos_class_np(&attr, qos, 0);
    }
#endif
    if (err == 0)
        err = pthread_create(&t->handle, &attr, de__thread_main, t);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        free(t);
        return DE_FAIL(err == EAGAIN || err == ENOMEM ? DE_ERR_OUT_OF_MEMORY : DE_ERR_INTERNAL,
                       "de_thread_create '%s': pthread_create failed (%d: %s)", de__name_or(name),
                       err, strerror(err));
    }
    *out = t;
    return DE_OK;
}

i32 de_thread_join(de_thread *t) {
    if (!t)
        return 0;
    DE__CHECK(pthread_join(t->handle, NULL));
    i32 r = t->result; /* pthread_join orders the thread's writes before this read */
    free(t);
    return r;
}

de_result de_thread_set_affinity(de_thread *t, u64 core_mask) {
    if (!t || core_mask == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_thread_set_affinity: no thread or empty mask");
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    bool any = false;
    for (unsigned i = 0; i < 64 && i < (unsigned)CPU_SETSIZE; i++) {
        if (core_mask & (1ull << i)) {
            CPU_SET(i, &set);
            any = true;
        }
    }
    if (!any)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_thread_set_affinity: no core in the mask");
    int err = pthread_setaffinity_np(t->handle, sizeof set, &set);
    if (err == 0)
        return DE_OK;
    return DE_FAIL(err == EINVAL ? DE_ERR_INVALID_ARGUMENT : DE_ERR_INTERNAL,
                   "de_thread_set_affinity: pthread_setaffinity_np failed (%d: %s)", err,
                   strerror(err));
#elif defined(__APPLE__)
    /* No hard affinity on macOS. Threads with the same tag are hinted to share an L2; Apple
     * Silicon rejects the policy (KERN_NOT_SUPPORTED). */
    int lowest = 0;
    while (!(core_mask & (1ull << lowest)))
        lowest++;
    thread_affinity_policy_data_t policy = {lowest + 1};
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(t->handle), THREAD_AFFINITY_POLICY,
                                         (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT);
    if (kr == KERN_SUCCESS)
        return DE_OK;
    if (kr == KERN_NOT_SUPPORTED)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "de_thread_set_affinity: not supported on this Mac");
    return DE_FAIL(DE_ERR_INTERNAL, "de_thread_set_affinity: thread_policy_set failed (%d)",
                   (int)kr);
#else
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_thread_set_affinity: not supported on this OS");
#endif
}

de_result de_thread_set_priority(de_thread *t, de_thread_priority prio) {
    if (!t || (unsigned)prio >= 4u)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_thread_set_priority: no thread or bad priority");
    /* See the header: macOS sets QoS only on the calling thread, and Linux needs privileges to
     * raise one; both schedulers give lower priorities a smaller share rather than none. */
    return DE_FAIL(DE_ERR_UNSUPPORTED, "de_thread_set_priority: not supported on this OS");
}

static DE_THREAD_LOCAL u32 de__tid_cache;

u32 de_thread_current_id(void) {
    u32 id = de__tid_cache;
    if (id == 0) {
#if defined(__APPLE__)
        u64 tid = 0;
        pthread_threadid_np(NULL, &tid);
        id = (u32)tid;
#elif defined(__linux__)
        id = (u32)syscall(SYS_gettid);
#else
        id = (u32)(uintptr_t)pthread_self();
#endif
        if (id == 0)
            id = 0x80000000u; /* 0 is reserved for "no thread"; never a real id in practice */
        de__tid_cache = id;
    }
    return id;
}

u32 de_cpu_logical_cores(void) {
#if defined(__linux__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0)
            return (u32)n;
    }
#elif defined(__APPLE__)
    int n = 0;
    size_t len = sizeof n;
    if (sysctlbyname("hw.logicalcpu", &n, &len, NULL, 0) == 0 && n > 0)
        return (u32)n;
#endif
    long n2 = sysconf(_SC_NPROCESSORS_ONLN);
    return n2 > 0 ? (u32)n2 : 1u;
}

/* ---------- Mutex ---------- */

/* ---- Lock hooks (DENG-308; de_lock_set_hooks, platform/mutex_prof.h) ---- */

de_atomic_ptr de__lock_hooks_slot = DE_ATOMIC_INIT(NULL);

const de_lock_hooks *de_lock_set_hooks(const de_lock_hooks *hooks) {
    /* acq_rel: publishes *hooks to lockers, and lets the caller read the old hooks safely. */
    return (const de_lock_hooks *)de_atomic_exchange_ptr(
        &de__lock_hooks_slot, (void *)(uintptr_t)hooks, DE_ATOMIC_ACQ_REL);
}

void de_mutex_init(de_mutex *m) {
    memset(m, 0, sizeof *m);
    pthread_mutexattr_t attr;
    DE__CHECK(pthread_mutexattr_init(&attr));
#if defined(__APPLE__)
    /* The default macOS policy hands the lock over in FIFO order, which collapses throughput
     * under contention. First-fit lets a running thread take it again (Linux-like behaviour). */
    DE__CHECK(pthread_mutexattr_setpolicy_np(&attr, PTHREAD_MUTEX_POLICY_FIRSTFIT_NP));
#endif
    DE__CHECK(pthread_mutex_init(de__mtx(m), &attr));
    pthread_mutexattr_destroy(&attr);
    de__mutex_prof_init(m);
}

void de_mutex_lock(de_mutex *m) {
    de__lock_hook(m);
    const bool prof_after = de__mutex_prof_before(m);
    DE__CHECK(pthread_mutex_lock(de__mtx(m)));
    if (prof_after)
        de__mutex_prof_after(m);
}

bool de_mutex_try_lock(de_mutex *m) {
    de__lock_hook(m);
    int err = pthread_mutex_trylock(de__mtx(m));
    if (err == EBUSY) {
        de__mutex_prof_after_try(m, false);
        return false;
    }
    if (err != 0)
        DE__FATAL("pthread_mutex_trylock", err);
    de__mutex_prof_after_try(m, true);
    return true;
}

void de_mutex_unlock(de_mutex *m) {
    DE__CHECK(pthread_mutex_unlock(de__mtx(m)));
    de__mutex_prof_after_unlock(m);
}

void de_mutex_destroy(de_mutex *m) {
    de__mutex_prof_destroy(m);
    DE__CHECK(pthread_mutex_destroy(de__mtx(m)));
}

/* ---------- Condition variable ---------- */

void de_cond_init(de_cond *c) {
    memset(c, 0, sizeof *c);
#if defined(__APPLE__)
    DE__CHECK(pthread_cond_init(de__cnd(c), NULL));
#else
    /* Timed waits measure against the monotonic clock, so wall-clock jumps do not affect them. */
    pthread_condattr_t attr;
    DE__CHECK(pthread_condattr_init(&attr));
    DE__CHECK(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    DE__CHECK(pthread_cond_init(de__cnd(c), &attr));
    pthread_condattr_destroy(&attr);
#endif
}

void de_cond_wait(de_cond *c, de_mutex *m) {
    de__mutex_prof_after_unlock(m);
    DE__CHECK(pthread_cond_wait(de__cnd(c), de__mtx(m)));
    de__mutex_prof_relocked(m);
}

#if !defined(__APPLE__)
/* now(CLOCK_MONOTONIC) + timeout, saturated so a huge timeout does not wrap. */
static struct timespec de__abs_monotonic(u64 timeout_ns) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 sec = timeout_ns / DE__NS_PER_S;
    u64 nsec = (u64)ts.tv_nsec + timeout_ns % DE__NS_PER_S;
    if (nsec >= DE__NS_PER_S) {
        nsec -= DE__NS_PER_S;
        sec++;
    }
    const u64 max_add = (u64)INT32_MAX * 64u; /* about 4000 years: far beyond any real wait */
    if (sec > max_add)
        sec = max_add;
    ts.tv_sec += (time_t)sec;
    ts.tv_nsec = (long)nsec;
    return ts;
}
#endif

bool de_cond_wait_timeout(de_cond *c, de_mutex *m, u64 timeout_ns) {
    int err;
    de__mutex_prof_after_unlock(m);
#if defined(__APPLE__)
    struct timespec rel;
    u64 sec = timeout_ns / DE__NS_PER_S;
    rel.tv_sec = sec > (u64)INT32_MAX * 64u ? (time_t)INT32_MAX * 64 : (time_t)sec;
    rel.tv_nsec = (long)(timeout_ns % DE__NS_PER_S);
    err = pthread_cond_timedwait_relative_np(de__cnd(c), de__mtx(m), &rel);
#else
    struct timespec abs = de__abs_monotonic(timeout_ns);
    err = pthread_cond_timedwait(de__cnd(c), de__mtx(m), &abs);
#endif
    de__mutex_prof_relocked(m);
    if (err == ETIMEDOUT)
        return false;
    if (err != 0)
        DE__FATAL("pthread_cond_timedwait", err);
    return true;
}

void de_cond_signal(de_cond *c) {
    DE__CHECK(pthread_cond_signal(de__cnd(c)));
}

void de_cond_broadcast(de_cond *c) {
    DE__CHECK(pthread_cond_broadcast(de__cnd(c)));
}

void de_cond_destroy(de_cond *c) {
    DE__CHECK(pthread_cond_destroy(de__cnd(c)));
}

/* ---------- Reader/writer lock ---------- */

void de_rwlock_init(de_rwlock *l) {
    memset(l, 0, sizeof *l);
#if defined(__GLIBC__)
    /* glibc prefers readers by default, so a steady stream of readers starves a writer forever.
     * Prefer writers, like macOS and Windows SRWLOCK in practice. */
    pthread_rwlockattr_t attr;
    DE__CHECK(pthread_rwlockattr_init(&attr));
    DE__CHECK(pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP));
    DE__CHECK(pthread_rwlock_init(de__rwl(l), &attr));
    pthread_rwlockattr_destroy(&attr);
#else
    DE__CHECK(pthread_rwlock_init(de__rwl(l), NULL));
#endif
}

void de_rwlock_read_lock(de_rwlock *l) {
    de__lock_hook(l);
    DE__CHECK(pthread_rwlock_rdlock(de__rwl(l)));
}

void de_rwlock_read_unlock(de_rwlock *l) {
    DE__CHECK(pthread_rwlock_unlock(de__rwl(l)));
}

void de_rwlock_write_lock(de_rwlock *l) {
    de__lock_hook(l);
    DE__CHECK(pthread_rwlock_wrlock(de__rwl(l)));
}

void de_rwlock_write_unlock(de_rwlock *l) {
    DE__CHECK(pthread_rwlock_unlock(de__rwl(l)));
}

void de_rwlock_destroy(de_rwlock *l) {
    DE__CHECK(pthread_rwlock_destroy(de__rwl(l)));
}

/* ---------- Semaphore ---------- */

#if defined(__APPLE__)

static dispatch_semaphore_t de__dsem(de_sem *s) {
    dispatch_semaphore_t d;
    memcpy(&d, s->opaque, sizeof d);
    return d;
}

de_result de_sem_init(de_sem *s, u32 initial) {
    memset(s, 0, sizeof *s);
    /* libdispatch traps when a semaphore is released with a count below its creation value, so
     * create it at 0 and post the initial count. */
    dispatch_semaphore_t d = dispatch_semaphore_create(0);
    if (!d)
        return DE_FAIL(DE_ERR_INTERNAL, "de_sem_init: dispatch_semaphore_create failed");
    memcpy(s->opaque, &d, sizeof d);
    for (u32 i = 0; i < initial; i++)
        dispatch_semaphore_signal(d);
    return DE_OK;
}

void de_sem_post(de_sem *s, u32 count) {
    dispatch_semaphore_t d = de__dsem(s);
    for (u32 i = 0; i < count; i++)
        dispatch_semaphore_signal(d);
}

void de_sem_wait(de_sem *s) {
    dispatch_semaphore_wait(de__dsem(s), DISPATCH_TIME_FOREVER);
}

bool de_sem_try_wait(de_sem *s) {
    return dispatch_semaphore_wait(de__dsem(s), DISPATCH_TIME_NOW) == 0;
}

bool de_sem_wait_timeout(de_sem *s, u64 timeout_ns) {
    int64_t delta = timeout_ns > (u64)INT64_MAX ? INT64_MAX : (int64_t)timeout_ns;
    return dispatch_semaphore_wait(de__dsem(s), dispatch_time(DISPATCH_TIME_NOW, delta)) == 0;
}

void de_sem_destroy(de_sem *s) {
    dispatch_semaphore_t d = de__dsem(s);
    if (d)
        dispatch_release(d);
    memset(s, 0, sizeof *s);
}

#else /* Linux and other POSIX: unnamed sem_t */

/* ThreadSanitizer intercepts sem_post and sem_timedwait but not sem_clockwait (checked with the
 * GCC 14 and Clang 18 runtimes). Without help it sees no happens-before edge from a post to a
 * wake through sem_clockwait, and reports a race on all data written before de_sem_post and
 * read after de_sem_wait_timeout (DENG-360). glibc's sem_clockwait does acquire; this annotation
 * tells TSan so. The interceptor releases on the same address in sem_post. */
#if defined(__SANITIZE_THREAD__)
#define DE__TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DE__TSAN 1
#endif
#endif
#if defined(DE__TSAN)
#include <sanitizer/tsan_interface.h>
#define DE__TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#else
#define DE__TSAN_ACQUIRE(addr) ((void)(addr))
#endif

static sem_t *de__psem(de_sem *s) {
    return (sem_t *)(void *)s->opaque;
}

de_result de_sem_init(de_sem *s, u32 initial) {
    memset(s, 0, sizeof *s);
    if (initial > (u32)SEM_VALUE_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_sem_init: initial count %u too large", initial);
    if (sem_init(de__psem(s), 0, initial) != 0)
        return DE_FAIL(DE_ERR_INTERNAL, "de_sem_init: sem_init failed (%d: %s)", errno,
                       strerror(errno));
    return DE_OK;
}

void de_sem_post(de_sem *s, u32 count) {
    for (u32 i = 0; i < count; i++) {
        if (sem_post(de__psem(s)) != 0)
            DE__FATAL("sem_post", errno);
    }
}

void de_sem_wait(de_sem *s) {
    while (sem_wait(de__psem(s)) != 0) {
        if (errno != EINTR)
            DE__FATAL("sem_wait", errno);
    }
}

bool de_sem_try_wait(de_sem *s) {
    while (sem_trywait(de__psem(s)) != 0) {
        if (errno == EAGAIN)
            return false;
        if (errno != EINTR)
            DE__FATAL("sem_trywait", errno);
    }
    return true;
}

bool de_sem_wait_timeout(de_sem *s, u64 timeout_ns) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
    struct timespec abs = de__abs_monotonic(timeout_ns);
    while (sem_clockwait(de__psem(s), CLOCK_MONOTONIC, &abs) != 0) {
#else
    /* sem_timedwait only knows CLOCK_REALTIME; a wall-clock jump can stretch the wait. */
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    u64 nsec = (u64)abs.tv_nsec + timeout_ns % DE__NS_PER_S;
    abs.tv_sec += (time_t)(timeout_ns / DE__NS_PER_S + nsec / DE__NS_PER_S);
    abs.tv_nsec = (long)(nsec % DE__NS_PER_S);
    while (sem_timedwait(de__psem(s), &abs) != 0) {
#endif
        if (errno == ETIMEDOUT)
            return false;
        if (errno != EINTR)
            DE__FATAL("sem_clockwait", errno);
    }
    DE__TSAN_ACQUIRE(de__psem(s));
    return true;
}

void de_sem_destroy(de_sem *s) {
    if (sem_destroy(de__psem(s)) != 0)
        DE__FATAL("sem_destroy", errno);
}

#endif

/* ---------- Thread-local storage ----------
 * de_tls_key.id is pthread_key_t + 1, so that the zero key stays invalid. */

de_result de_tls_key_create(de_tls_key *out, de_tls_dtor dtor) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_tls_key_create: out is required");
    out->id = 0;
    pthread_key_t k;
    int err = pthread_key_create(&k, dtor);
    if (err != 0)
        return DE_FAIL(err == EAGAIN || err == ENOMEM ? DE_ERR_OUT_OF_MEMORY : DE_ERR_INTERNAL,
                       "de_tls_key_create: pthread_key_create failed (%d: %s)", err, strerror(err));
    out->id = (u64)k + 1u;
    return DE_OK;
}

void de_tls_key_destroy(de_tls_key key) {
    if (key.id != 0)
        DE__CHECK(pthread_key_delete((pthread_key_t)(key.id - 1u)));
}

void *de_tls_get(de_tls_key key) {
    return key.id != 0 ? pthread_getspecific((pthread_key_t)(key.id - 1u)) : NULL;
}

de_result de_tls_set(de_tls_key key, void *value) {
    if (key.id == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_tls_set: invalid key");
    int err = pthread_setspecific((pthread_key_t)(key.id - 1u), value);
    if (err == 0)
        return DE_OK;
    return DE_FAIL(err == ENOMEM ? DE_ERR_OUT_OF_MEMORY : DE_ERR_INVALID_ARGUMENT,
                   "de_tls_set: pthread_setspecific failed (%d: %s)", err, strerror(err));
}
