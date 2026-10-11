/* de_atomic.h: internal atomics for the engine (SPEC §3.2, §6). Not a public header.
 *
 * Thin static inline wrappers over C11 <stdatomic.h>. The values are wrapped in structs, so a
 * plain read or write of a shared variable does not compile: every access names its memory
 * order. Include as "platform/de_atomic.h" (core and platform may include each other).
 *
 * MSVC (cl) needs /experimental:c11atomics (VS 17.5+), added in cmake/DeCompiler.cmake.
 * clang-cl, Apple Clang, Clang and GCC need nothing.
 */
#ifndef DE_ATOMIC_H
#define DE_ATOMIC_H

#include <dengine/de_base.h>
#include <stdatomic.h>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

typedef memory_order de_memory_order;
#define DE_ATOMIC_RELAXED memory_order_relaxed
#define DE_ATOMIC_ACQUIRE memory_order_acquire
#define DE_ATOMIC_RELEASE memory_order_release
#define DE_ATOMIC_ACQ_REL memory_order_acq_rel
#define DE_ATOMIC_SEQ_CST memory_order_seq_cst

typedef struct de_atomic_u32 {
    _Atomic(u32) v;
} de_atomic_u32;

typedef struct de_atomic_u64 {
    _Atomic(u64) v;
} de_atomic_u64;

typedef struct de_atomic_ptr {
    _Atomic(void *) v;
} de_atomic_ptr;

/* Lock-free is a requirement, not a hope: a lock-based fallback would break signal and
 * lock-free queue users (DENG-152). DE_ATOMIC_LOCK_FREE_{U32,U64,PTR} are integer constant
 * expressions (1 = always lock-free) for other headers that want to restate the requirement in a
 * _Static_assert (core/de_spsc.h, core/de_mpmc.h); use them instead of ATOMIC_*_LOCK_FREE. */
#if defined(_MSC_VER) && !defined(__clang__)
/* MSVC cl's <stdatomic.h> (vcruntime_c11_stdatomic.h) defines every ATOMIC_*_LOCK_FREE as 1
 * ("sometimes"), because it also accepts _Atomic types of any size. Its rule is fixed and
 * written in the CRT header itself: _Atomic_is_lock_free(size) is `size <= 8 && size is a power
 * of two`; any other size gets an embedded spinlock, which makes the _Atomic type larger than its
 * value type. So check that exact rule, and that no lock was added, at compile time. (clang-cl
 * uses Clang's own <stdatomic.h> and takes the #else branch.) */
#define DE__MSVC_ATOMIC_LOCK_FREE(T)                                                               \
    (sizeof(T) <= 8 && (sizeof(T) & (sizeof(T) - 1)) == 0 && sizeof(_Atomic(T)) == sizeof(T))
#define DE_ATOMIC_LOCK_FREE_U32 DE__MSVC_ATOMIC_LOCK_FREE(u32)
#define DE_ATOMIC_LOCK_FREE_U64 DE__MSVC_ATOMIC_LOCK_FREE(u64)
#define DE_ATOMIC_LOCK_FREE_PTR DE__MSVC_ATOMIC_LOCK_FREE(void *)
#else
#define DE_ATOMIC_LOCK_FREE_U32 (ATOMIC_INT_LOCK_FREE == 2)
#define DE_ATOMIC_LOCK_FREE_U64 (ATOMIC_LLONG_LOCK_FREE == 2)
#define DE_ATOMIC_LOCK_FREE_PTR (ATOMIC_POINTER_LOCK_FREE == 2)
#endif
_Static_assert(DE_ATOMIC_LOCK_FREE_U32, "32-bit atomics must be lock-free");
_Static_assert(DE_ATOMIC_LOCK_FREE_U64, "64-bit atomics must be lock-free");
_Static_assert(DE_ATOMIC_LOCK_FREE_PTR, "pointer atomics must be lock-free");

/* Initialization of an object no other thread can see yet (not an atomic operation). */
#define DE_ATOMIC_INIT(value) {(value)}

static inline void de_atomic_init_u32(de_atomic_u32 *a, u32 v) {
    atomic_init(&a->v, v);
}
static inline void de_atomic_init_u64(de_atomic_u64 *a, u64 v) {
    atomic_init(&a->v, v);
}
static inline void de_atomic_init_ptr(de_atomic_ptr *a, void *v) {
    atomic_init(&a->v, v);
}

/* ---------- u32 ---------- */

static inline u32 de_atomic_load_u32(const de_atomic_u32 *a, de_memory_order mo) {
    /* C11 atomic_load takes a non-const pointer in some libraries (MSVC); the cast is safe. */
    return atomic_load_explicit((_Atomic(u32) *)&a->v, mo);
}
static inline void de_atomic_store_u32(de_atomic_u32 *a, u32 v, de_memory_order mo) {
    atomic_store_explicit(&a->v, v, mo);
}
static inline u32 de_atomic_exchange_u32(de_atomic_u32 *a, u32 v, de_memory_order mo) {
    return atomic_exchange_explicit(&a->v, v, mo);
}
/* Strong CAS. On failure *expected receives the current value. */
static inline bool de_atomic_cas_u32(de_atomic_u32 *a, u32 *expected, u32 desired,
                                     de_memory_order success, de_memory_order failure) {
    /* MSVC cl's __c11_atomic_compare_exchange_* intrinsics do not count the failure order as
     * a use (C4100 under /W4 /WX); it is still passed, so the semantics are unchanged. */
    (void)failure;
    return atomic_compare_exchange_strong_explicit(&a->v, expected, desired, success, failure);
}
/* Weak CAS: may fail spuriously; use it in retry loops. */
static inline bool de_atomic_cas_weak_u32(de_atomic_u32 *a, u32 *expected, u32 desired,
                                          de_memory_order success, de_memory_order failure) {
    (void)failure;
    return atomic_compare_exchange_weak_explicit(&a->v, expected, desired, success, failure);
}
/* The fetch_* functions return the previous value. */
static inline u32 de_atomic_fetch_add_u32(de_atomic_u32 *a, u32 v, de_memory_order mo) {
    return atomic_fetch_add_explicit(&a->v, v, mo);
}
static inline u32 de_atomic_fetch_sub_u32(de_atomic_u32 *a, u32 v, de_memory_order mo) {
    return atomic_fetch_sub_explicit(&a->v, v, mo);
}
static inline u32 de_atomic_fetch_and_u32(de_atomic_u32 *a, u32 v, de_memory_order mo) {
    return atomic_fetch_and_explicit(&a->v, v, mo);
}
static inline u32 de_atomic_fetch_or_u32(de_atomic_u32 *a, u32 v, de_memory_order mo) {
    return atomic_fetch_or_explicit(&a->v, v, mo);
}

/* ---------- u64 ---------- */

static inline u64 de_atomic_load_u64(const de_atomic_u64 *a, de_memory_order mo) {
    return atomic_load_explicit((_Atomic(u64) *)&a->v, mo);
}
static inline void de_atomic_store_u64(de_atomic_u64 *a, u64 v, de_memory_order mo) {
    atomic_store_explicit(&a->v, v, mo);
}
static inline u64 de_atomic_exchange_u64(de_atomic_u64 *a, u64 v, de_memory_order mo) {
    return atomic_exchange_explicit(&a->v, v, mo);
}
static inline bool de_atomic_cas_u64(de_atomic_u64 *a, u64 *expected, u64 desired,
                                     de_memory_order success, de_memory_order failure) {
    (void)failure;
    return atomic_compare_exchange_strong_explicit(&a->v, expected, desired, success, failure);
}
static inline bool de_atomic_cas_weak_u64(de_atomic_u64 *a, u64 *expected, u64 desired,
                                          de_memory_order success, de_memory_order failure) {
    (void)failure;
    return atomic_compare_exchange_weak_explicit(&a->v, expected, desired, success, failure);
}
static inline u64 de_atomic_fetch_add_u64(de_atomic_u64 *a, u64 v, de_memory_order mo) {
    return atomic_fetch_add_explicit(&a->v, v, mo);
}
static inline u64 de_atomic_fetch_sub_u64(de_atomic_u64 *a, u64 v, de_memory_order mo) {
    return atomic_fetch_sub_explicit(&a->v, v, mo);
}
static inline u64 de_atomic_fetch_and_u64(de_atomic_u64 *a, u64 v, de_memory_order mo) {
    return atomic_fetch_and_explicit(&a->v, v, mo);
}
static inline u64 de_atomic_fetch_or_u64(de_atomic_u64 *a, u64 v, de_memory_order mo) {
    return atomic_fetch_or_explicit(&a->v, v, mo);
}

/* ---------- pointers ---------- */

static inline void *de_atomic_load_ptr(const de_atomic_ptr *a, de_memory_order mo) {
    return atomic_load_explicit((_Atomic(void *) *)&a->v, mo);
}
static inline void de_atomic_store_ptr(de_atomic_ptr *a, void *v, de_memory_order mo) {
    atomic_store_explicit(&a->v, v, mo);
}
static inline void *de_atomic_exchange_ptr(de_atomic_ptr *a, void *v, de_memory_order mo) {
    return atomic_exchange_explicit(&a->v, v, mo);
}
static inline bool de_atomic_cas_ptr(de_atomic_ptr *a, void **expected, void *desired,
                                     de_memory_order success, de_memory_order failure) {
    (void)failure;
    return atomic_compare_exchange_strong_explicit(&a->v, expected, desired, success, failure);
}
static inline bool de_atomic_cas_weak_ptr(de_atomic_ptr *a, void **expected, void *desired,
                                          de_memory_order success, de_memory_order failure) {
    (void)failure;
    return atomic_compare_exchange_weak_explicit(&a->v, expected, desired, success, failure);
}

/* ---------- fences and spinning ---------- */

/* GCC 11+ with -fsanitize=thread warns that TSan does not support atomic_thread_fence (-Wtsan),
 * and -Werror makes that a build error. The fence is still emitted and still orders memory on
 * the hardware, so the wrapper keeps it and silences only that warning: the memory-order
 * semantics are the same as in every other build. TSan does not model a standalone fence as a
 * happens-before edge (Clang TSan ignores it silently, GCC says so), so engine code should
 * synchronise through acquire/release operations on atomics, which TSan checks. Newer Clang also
 * defines __SANITIZE_THREAD__, hence the !__clang__ test. */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11 && defined(__SANITIZE_THREAD__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtsan"
static inline void de_atomic_thread_fence(de_memory_order mo) {
    atomic_thread_fence(mo);
}
#pragma GCC diagnostic pop
#else
static inline void de_atomic_thread_fence(de_memory_order mo) {
    atomic_thread_fence(mo);
}
#endif

/* Compiler-only barrier: orders memory accesses against a signal handler on the same thread. */
static inline void de_atomic_signal_fence(de_memory_order mo) {
    atomic_signal_fence(mo);
}

/* Spin-wait hint: lowers power and frees the core's pipeline for the other hyperthread. */
static inline void de_cpu_pause(void) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
    __yield();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    /* Unknown CPU: no hint, the caller's loop still works. */
#endif
}

#endif /* DE_ATOMIC_H */
