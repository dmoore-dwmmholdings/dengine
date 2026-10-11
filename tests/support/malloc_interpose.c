/*
 * malloc_interpose.c: counts malloc-family calls for the alloc guard (de_alloc_guard.h).
 *
 * macOS: built as the shared library de_malloc_interpose. Its __DATA,__interpose section
 * (DYLD_INTERPOSE) makes dyld bind every other image's malloc, calloc, ... to the replacements
 * below when the library is loaded at launch, i.e. when the test executable links it. Calls made
 * from inside this library are not redirected, so the replacements call the real functions by
 * name.
 *
 * Linux: compiled into de_test_support, and the executable is linked with
 * `ld --wrap=malloc,...` (tests/support/CMakeLists.txt): references to malloc in the executable's
 * objects go to __wrap_malloc, which calls __real_malloc. Calls inside libc are not counted.
 *
 * The callback runs at most once at a time per thread: a thread flag stops a callback that
 * allocates from being called again for its own allocation. On macOS the flag is a pthread
 * key, because dyld allocates a thread's block of _Thread_local variables (with malloc) on first
 * use, and that must not happen inside malloc.
 */
#include "de_alloc_guard.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <pthread.h>
#define DE__RAW_EXPORT __attribute__((visibility("default")))
#else
#define DE__RAW_EXPORT
#endif

/* No initializer: static zero-initialisation is a valid NULL state for an atomic (C11
 * 7.17.2.1). `= NULL` is a void * converted to a function pointer, which Apple Clang 15 rejects
 * under -Wpedantic (DENG-359). */
static _Atomic(de__raw_alloc_fn) de__raw_fn;

DE__RAW_EXPORT void de__raw_alloc_set_callback(de__raw_alloc_fn fn) {
    atomic_store_explicit(&de__raw_fn, fn, memory_order_release);
}

/* ---- Per-thread re-entrancy flag ---------------------------------------------------------- */

#if defined(__APPLE__)
static pthread_key_t de__raw_key;
static atomic_int de__raw_key_ok = 0;

/* pthread_key_create allocates nothing; it runs when the library is loaded. Until then the
 * callback is never called. */
__attribute__((constructor)) static void de__raw_init(void) {
    if (pthread_key_create(&de__raw_key, NULL) == 0)
        atomic_store_explicit(&de__raw_key_ok, 1, memory_order_release);
}

static int de__raw_enter(void) {
    if (!atomic_load_explicit(&de__raw_key_ok, memory_order_acquire))
        return 0;
    if (pthread_getspecific(de__raw_key) != NULL)
        return 0;
    pthread_setspecific(de__raw_key, (void *)1);
    return 1;
}

static void de__raw_leave(void) {
    pthread_setspecific(de__raw_key, NULL);
}
#else
/* In the executable itself: initial-exec TLS, which never allocates. */
static _Thread_local int de__raw_busy;

static int de__raw_enter(void) {
    if (de__raw_busy)
        return 0;
    de__raw_busy = 1;
    return 1;
}

static void de__raw_leave(void) {
    de__raw_busy = 0;
}
#endif

static void de__raw_note(int is_free, size_t size, void *caller) {
    de__raw_alloc_fn fn = atomic_load_explicit(&de__raw_fn, memory_order_acquire);
    if (fn == NULL || !de__raw_enter())
        return;
    fn(is_free, size, caller);
    de__raw_leave();
}

#define DE__CALLER() __builtin_return_address(0)

/* ---- macOS: DYLD_INTERPOSE ---------------------------------------------------------------- */

#if defined(__APPLE__)

static void *de__i_malloc(size_t n) {
    void *p = malloc(n);
    if (p != NULL)
        de__raw_note(0, n, DE__CALLER());
    return p;
}

static void *de__i_calloc(size_t count, size_t n) {
    void *p = calloc(count, n);
    if (p != NULL)
        de__raw_note(0, count * n, DE__CALLER());
    return p;
}

static void *de__i_realloc(void *old, size_t n) {
    void *p = realloc(old, n);
    if (p != NULL)
        de__raw_note(0, n, DE__CALLER());
    return p;
}

static void *de__i_reallocf(void *old, size_t n) {
    void *p = reallocf(old, n);
    if (p != NULL)
        de__raw_note(0, n, DE__CALLER());
    return p;
}

static void *de__i_valloc(size_t n) {
    void *p = valloc(n);
    if (p != NULL)
        de__raw_note(0, n, DE__CALLER());
    return p;
}

static void *de__i_aligned_alloc(size_t align, size_t n) {
    void *p = aligned_alloc(align, n);
    if (p != NULL)
        de__raw_note(0, n, DE__CALLER());
    return p;
}

static int de__i_posix_memalign(void **out, size_t align, size_t n) {
    int r = posix_memalign(out, align, n);
    if (r == 0)
        de__raw_note(0, n, DE__CALLER());
    return r;
}

static void de__i_free(void *p) {
    if (p != NULL)
        de__raw_note(1, 0, DE__CALLER());
    free(p);
}

/* dyld reads pairs of (replacement, replacee). `void (*)(void)` is the generic function pointer
 * type, so the casts draw no cast-function-type warning. */
typedef struct de__interpose {
    void (*replacement)(void);
    void (*replacee)(void);
} de__interpose;

#define DE__INTERPOSE(repl, orig) {(void (*)(void))(repl), (void (*)(void))(orig)}

__attribute__((used,
               section("__DATA,__interpose"))) static const de__interpose de__interposers[] = {
    DE__INTERPOSE(de__i_malloc, malloc),
    DE__INTERPOSE(de__i_calloc, calloc),
    DE__INTERPOSE(de__i_realloc, realloc),
    DE__INTERPOSE(de__i_reallocf, reallocf),
    DE__INTERPOSE(de__i_valloc, valloc),
    DE__INTERPOSE(de__i_aligned_alloc, aligned_alloc),
    DE__INTERPOSE(de__i_posix_memalign, posix_memalign),
    DE__INTERPOSE(de__i_free, free),
};

/* ---- Linux: ld --wrap --------------------------------------------------------------------- */

#else

void *__real_malloc(size_t n);
void *__real_calloc(size_t count, size_t n);
void *__real_realloc(void *old, size_t n);
int __real_posix_memalign(void **out, size_t align, size_t n);
void *__real_aligned_alloc(size_t align, size_t n);
void __real_free(void *p);

void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t count, size_t n);
void *__wrap_realloc(void *old, size_t n);
int __wrap_posix_memalign(void **out, size_t align, size_t n);
void *__wrap_aligned_alloc(size_t align, size_t n);
void __wrap_free(void *p);

void *__wrap_malloc(size_t n) {
    void *p = __real_malloc(n);
    if (p != NULL)
        de__raw_note(0, n, DE__CALLER());
    return p;
}

void *__wrap_calloc(size_t count, size_t n) {
    void *p = __real_calloc(count, n);
    if (p != NULL)
        de__raw_note(0, count * n, DE__CALLER());
    return p;
}

void *__wrap_realloc(void *old, size_t n) {
    void *p = __real_realloc(old, n);
    if (p != NULL)
        de__raw_note(0, n, DE__CALLER());
    return p;
}

int __wrap_posix_memalign(void **out, size_t align, size_t n) {
    int r = __real_posix_memalign(out, align, n);
    if (r == 0)
        de__raw_note(0, n, DE__CALLER());
    return r;
}

void *__wrap_aligned_alloc(size_t align, size_t n) {
    void *p = __real_aligned_alloc(align, n);
    if (p != NULL)
        de__raw_note(0, n, DE__CALLER());
    return p;
}

void __wrap_free(void *p) {
    if (p != NULL)
        de__raw_note(1, 0, DE__CALLER());
    __real_free(p);
}

#endif
