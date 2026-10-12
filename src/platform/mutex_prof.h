/*
 * mutex_prof.h: profiler hooks of de_mutex and de_thread_set_name (DENG-94; SPEC §18.1). Internal
 * to thread_posix.c and thread_win32.c; include as "platform/mutex_prof.h".
 *
 * In DE_PROFILE builds a de_mutex carries a de_prof_lock (de_debug.h) after the OS mutex, filled
 * by de_mutex_init when the profiler runs, and every lock, try-lock and unlock reports to it. A
 * condition wait releases and retakes the mutex inside the OS call, so it is reported as an
 * unlock before the wait and a lock after it. Without DE_PROFILE every helper is an empty inline
 * function and de_mutex has no profiler bytes.
 */
#ifndef DE_PLATFORM_MUTEX_PROF_H
#define DE_PLATFORM_MUTEX_PROF_H

#include <dengine/de_platform.h>

#ifdef DE_PROFILE
#include <dengine/de_debug.h>

_Static_assert(sizeof(de_prof_lock) <= sizeof(((de_mutex *)0)->prof_),
               "de_prof_lock does not fit de_mutex");
_Static_assert(_Alignof(de_prof_lock) <= _Alignof(u64), "de_mutex.prof_ alignment too small");

/* Every de_mutex is announced with this location: the profiler lists them as "de_mutex". */
static const de_prof_srcloc de__mutex_prof_loc = {"de_mutex", "de_mutex_init", __FILE__, 0u, 0u};

static inline de_prof_lock *de__mutex_prof(de_mutex *m) {
    return (de_prof_lock *)(void *)m->prof_;
}
static inline void de__mutex_prof_init(de_mutex *m) {
    de_prof_lock_init_(de__mutex_prof(m), &de__mutex_prof_loc);
}
static inline void de__mutex_prof_destroy(de_mutex *m) {
    de_prof_lock_destroy_(de__mutex_prof(m));
}
/* True when de__mutex_prof_after must follow the lock. */
static inline bool de__mutex_prof_before(de_mutex *m) {
    return de_prof_lock_before_(de__mutex_prof(m));
}
static inline void de__mutex_prof_after(de_mutex *m) {
    de_prof_lock_after_(de__mutex_prof(m));
}
static inline void de__mutex_prof_after_try(de_mutex *m, bool acquired) {
    de_prof_lock_after_try_(de__mutex_prof(m), acquired);
}
static inline void de__mutex_prof_after_unlock(de_mutex *m) {
    de_prof_lock_after_unlock_(de__mutex_prof(m));
}
/* After a condition wait: the OS took the mutex again. */
static inline void de__mutex_prof_relocked(de_mutex *m) {
    if (de_prof_lock_before_(de__mutex_prof(m)))
        de_prof_lock_after_(de__mutex_prof(m));
}
static inline void de__thread_prof_name(const char *name) {
    de_prof_thread_name_(name);
}

#else

static inline void de__mutex_prof_init(de_mutex *m) {
    (void)m;
}
static inline void de__mutex_prof_destroy(de_mutex *m) {
    (void)m;
}
static inline bool de__mutex_prof_before(de_mutex *m) {
    (void)m;
    return false;
}
static inline void de__mutex_prof_after(de_mutex *m) {
    (void)m;
}
static inline void de__mutex_prof_after_try(de_mutex *m, bool acquired) {
    (void)m;
    (void)acquired;
}
static inline void de__mutex_prof_after_unlock(de_mutex *m) {
    (void)m;
}
static inline void de__mutex_prof_relocked(de_mutex *m) {
    (void)m;
}
static inline void de__thread_prof_name(const char *name) {
    (void)name;
}

#endif /* DE_PROFILE */

/* ---- Lock hooks (DENG-308): de_lock_set_hooks in de_platform.h ----
 * The slot is defined, with de_lock_set_hooks, in thread_posix.c and thread_win32.c. Release and
 * Shipping builds compile the call out. */
#include "core/de_compiler.h"
#include "platform/de_atomic.h"

extern de_atomic_ptr de__lock_hooks_slot;

#if defined(DE_DEBUG) || defined(DE_DEV)
static inline void de__lock_hook(const void *lock) {
    if (DE_LIKELY(de_atomic_load_ptr(&de__lock_hooks_slot, DE_ATOMIC_RELAXED) == NULL))
        return;
    const de_lock_hooks *h =
        (const de_lock_hooks *)de_atomic_load_ptr(&de__lock_hooks_slot, DE_ATOMIC_ACQUIRE);
    if (h != NULL && h->on_lock != NULL)
        h->on_lock(h->user, lock);
}
#else
static inline void de__lock_hook(const void *lock) {
    (void)lock;
}
#endif

#endif /* DE_PLATFORM_MUTEX_PROF_H */
