/* panic.c: DE_PANIC and the panic hook (SPEC §5.2, §18.5).
 *
 * The hook is one (fn, user) slot so the S03 crash handler (DENG-251) can attach. The pair is
 * guarded by a spinlock so a reader never sees fn from one de_panic_set_hook call and user from
 * another; nothing is called while the lock is held. No heap allocation: the message is
 * formatted on the stack and written with fputs to unbuffered stderr.
 */
#include <dengine/de_base.h>

#include "core/de_compiler.h"
#include "core/log_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* MSVC's C mode needs /experimental:c11atomics for <stdatomic.h>; use its intrinsics instead. */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static volatile long de__hook_lock;
static void de__hook_lock_acquire(void) {
    while (_InterlockedExchange(&de__hook_lock, 1) != 0) {
    }
}
static void de__hook_lock_release(void) {
    _InterlockedExchange(&de__hook_lock, 0);
}
#else
#include <stdatomic.h>
static atomic_flag de__hook_lock = ATOMIC_FLAG_INIT;
static void de__hook_lock_acquire(void) {
    while (atomic_flag_test_and_set_explicit(&de__hook_lock, memory_order_acquire)) {
    }
}
static void de__hook_lock_release(void) {
    atomic_flag_clear_explicit(&de__hook_lock, memory_order_release);
}
#endif

static de_panic_hook_fn de__hook_fn;
static void *de__hook_user;

/* Set while this thread is inside de_panic_. A second panic on the same thread (from the hook,
 * or from code the hook calls) aborts at once. It stays set if the hook leaves by longjmp: the
 * thread is still in a panic, and any later panic on it aborts without the hook. */
static DE_THREAD_LOCAL int de__in_panic;

/* Big enough for a de_error message plus context; longer messages are cut. */
#define DE__PANIC_MESSAGE_MAX 1024

void de_panic_set_hook(de_panic_hook_fn fn, void *user) {
    de__hook_lock_acquire();
    de__hook_fn = fn;
    de__hook_user = user;
    de__hook_lock_release();
}

static void de__panic_write(const char *s) {
    if (s != NULL)
        fputs(s, stderr);
}

void de_panic_(const char *file, u32 line, const char *func, const char *fmt, ...) {
    if (de__in_panic) {
        de__panic_write("DE_PANIC: panic while panicking; aborting\n");
        abort();
    }
    de__in_panic = 1;

    char msg[DE__PANIC_MESSAGE_MAX];
    msg[0] = '\0';
    if (fmt != NULL) {
        va_list ap;
        va_start(ap, fmt);
        if (vsnprintf(msg, sizeof msg, fmt, ap) < 0)
            msg[0] = '\0';
        va_end(ap);
    }

    /* Get buffered log records out before the crash handler runs (DENG-150). Best effort: it
     * gives up instead of waiting on a stuck sink. */
    de__log_flush_for_panic();

    de__hook_lock_acquire();
    de_panic_hook_fn fn = de__hook_fn;
    void *user = de__hook_user;
    de__hook_lock_release();

    if (fn != NULL) {
        de_panic_info info = {msg, file, func, line};
        fn(&info, user);
    }

    char where[64];
    snprintf(where, sizeof where, ":%lu", (unsigned long)line);
    de__panic_write("DE_PANIC at ");
    de__panic_write(file != NULL ? file : "?");
    de__panic_write(where);
    de__panic_write(" (");
    de__panic_write(func != NULL ? func : "?");
    de__panic_write("): ");
    de__panic_write(msg);
    de__panic_write("\n");
    fflush(stderr);
    abort();
}
