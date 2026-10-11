/* Unit tests: virtual arena, arena allocator, null allocator, frame arenas (DENG-148). */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#endif

#include "de_test.h"

#include <dengine/de_memory.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define PSAPI_VERSION 2 /* GetProcessMemoryInfo -> K32GetProcessMemoryInfo in kernel32 */
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#define MIB ((size_t)1 << 20)
#define GIB ((size_t)1 << 30)

/* ---- Helpers ------------------------------------------------------------------------------- */

static de_arena *make_arena(size_t reserve) {
    de_arena_desc d = {0};
    d.reserve_bytes = reserve;
    d.name = "test";
    de_arena *a = NULL;
    de_result r = de_arena_create(&d, &a);
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(a != NULL);
    return a;
}

static u64 now_ns(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (u64)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#elif defined(__APPLE__)
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0)
        mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000u + (u64)ts.tv_nsec;
#endif
}

/* Resident set size of this process in bytes, or 0 if unknown. */
static size_t rss_bytes(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return (size_t)pmc.WorkingSetSize;
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) !=
        KERN_SUCCESS)
        return 0;
    return (size_t)info.resident_size;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (f == NULL)
        return 0;
    unsigned long size = 0, resident = 0;
    int n = fscanf(f, "%lu %lu", &size, &resident);
    fclose(f);
    return n == 2 ? (size_t)resident * (size_t)sysconf(_SC_PAGESIZE) : 0;
#endif
}

/* ---- Heap-call counter ----------------------------------------------------------------------
 * macOS: libmalloc calls `malloc_logger` (the hook MallocStackLogging uses) for every allocation
 * through the system zones. Linux/glibc: this executable defines malloc and friends, which
 * forward to glibc's __libc_* entry points. Neither works under ASan/TSan, whose runtimes replace
 * malloc: the test's self-check notices and skips. Windows: not counted (skips).
 * DE_TEST_SANITIZED is also used by arena.large_reserve_commit, whose RSS bounds do not hold when
 * the sanitizer's shadow memory is resident, and by arena.reset_is_constant_time for its timing
 * bound. */

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define DE_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define DE_TEST_SANITIZED 1
#endif

#if defined(__APPLE__) || (defined(__GLIBC__) && !defined(DE_TEST_SANITIZED))
static volatile long heap_calls;
static volatile int heap_counting;
#endif

#if defined(__APPLE__)
typedef void(de_test_malloc_logger_t)(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                                      uintptr_t result, uint32_t skip);
extern de_test_malloc_logger_t *malloc_logger;
static void count_logger(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                         uintptr_t result, uint32_t skip) {
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)result;
    (void)skip;
    if (heap_counting && (type & 2u) != 0) /* MALLOC_LOG_TYPE_ALLOCATE */
        ++heap_calls;
}
#define HEAP_COUNTER_AVAILABLE 1
static void heap_count_start(void) {
    heap_calls = 0;
    heap_counting = 1;
    malloc_logger = count_logger;
}
static void heap_count_stop(void) {
    malloc_logger = NULL;
    heap_counting = 0;
}
#elif defined(__GLIBC__) && !defined(DE_TEST_SANITIZED)
extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t n, size_t size);
extern void *__libc_realloc(void *p, size_t size);
extern void __libc_free(void *p);
void *malloc(size_t size) {
    if (heap_counting)
        ++heap_calls;
    return __libc_malloc(size);
}
void *calloc(size_t n, size_t size) {
    if (heap_counting)
        ++heap_calls;
    return __libc_calloc(n, size);
}
void *realloc(void *p, size_t size) {
    if (heap_counting)
        ++heap_calls;
    return __libc_realloc(p, size);
}
void free(void *p) {
    __libc_free(p);
}
#define HEAP_COUNTER_AVAILABLE 1
static void heap_count_start(void) {
    heap_calls = 0;
    heap_counting = 1;
}
static void heap_count_stop(void) {
    heap_counting = 0;
}
#endif

/* ---- Tests --------------------------------------------------------------------------------- */

TEST(arena, push_align) {
    de_arena *a = make_arena(64 * MIB);
    for (size_t align = 1; align <= 4096; align <<= 1) {
        for (int i = 0; i < 4; ++i) {
            /* An odd-sized push first, so the next one starts misaligned. */
            u8 *pad = (u8 *)de_arena_push(a, 3, 1);
            ASSERT_TRUE(pad != NULL);
            u8 *p = (u8 *)de_arena_push(a, 40, align);
            ASSERT_TRUE(p != NULL);
            EXPECT_EQ((uintptr_t)p % align, 0u);
            EXPECT_TRUE(p >= pad + 3);
            memset(p, 0x5A, 40);
        }
    }
    /* align 0 means the default (16). */
    (void)de_arena_push(a, 1, 1);
    u8 *p = (u8 *)de_arena_push(a, 8, 0);
    ASSERT_TRUE(p != NULL);
    EXPECT_EQ((uintptr_t)p % 16u, 0u);

    /* A non-power-of-two alignment fails without moving the position. */
    de_arena_mark before = de_arena_mark_get(a);
    de_error_clear();
    EXPECT_TRUE(de_arena_push(a, 8, 24) == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_arena_mark_get(a).pos, before.pos);

    /* DE_ARENA_PUSH aligns to the type. */
    double *d = DE_ARENA_PUSH(a, double, 10);
    ASSERT_TRUE(d != NULL);
    EXPECT_EQ((uintptr_t)d % DE_ALIGNOF(double), 0u);
    de_arena_destroy(a);
}

TEST(arena, mark_reset) {
    de_arena *a = make_arena(64 * MIB);
    const de_arena_mark m0 = de_arena_mark_get(a);
    u8 *p1 = (u8 *)de_arena_push(a, 100, 8);
    ASSERT_TRUE(p1 != NULL);
    const de_arena_mark m1 = de_arena_mark_get(a);
    u8 *p2 = (u8 *)de_arena_push(a, 200, 8);
    ASSERT_TRUE(p2 != NULL);
    memset(p1, 0xAB, 100);
    memset(p2, 0xEE, 200);

    /* Reset to m1: the next push reuses p2's memory; p1 is untouched. */
    de_arena_reset_to(a, m1);
    EXPECT_EQ(de_arena_mark_get(a).pos, m1.pos);
    u8 *p3 = (u8 *)de_arena_push(a, 200, 8);
    EXPECT_TRUE(p3 == p2);
    for (int i = 0; i < 100; ++i)
        ASSERT_EQ(p1[i], 0xAB);

    /* push_zero after a reset of dirty memory returns zeros. */
    de_arena_reset_to(a, m1);
    u8 *z = (u8 *)de_arena_push_zero(a, 200, 8);
    ASSERT_TRUE(z == p2);
    for (int i = 0; i < 200; ++i)
        ASSERT_EQ(z[i], 0);

    /* Full reset goes back to the first position. */
    de_arena_reset(a);
    EXPECT_EQ(de_arena_mark_get(a).pos, m0.pos);
    EXPECT_TRUE(de_arena_push(a, 100, 8) == p1);

    /* push_zero over a large dirty range, partly above the previous high-water mark. */
    de_arena_reset(a);
    u8 *big = (u8 *)de_arena_push(a, 3 * MIB, 64);
    ASSERT_TRUE(big != NULL);
    memset(big, 0xFF, 3 * MIB);
    de_arena_reset(a);
    (void)de_arena_push(a, 1 * MIB, 64);
    u8 *zz = (u8 *)de_arena_push_zero(a, 4 * MIB, 64);
    ASSERT_TRUE(zz != NULL);
    size_t nonzero = 0;
    for (size_t i = 0; i < 4 * MIB; ++i)
        nonzero += zz[i] != 0;
    EXPECT_EQ(nonzero, 0u);

    de_arena_info info = de_arena_get_info(a);
    EXPECT_GE(info.peak, 5 * MIB);
    EXPECT_GE(info.committed, info.used);
    EXPECT_STREQ(info.name, "test");
    de_arena_destroy(a);
}

TEST(arena, reset_is_constant_time) {
    de_arena *a = make_arena(1 * GIB);
    enum { ROUNDS = 7, PUSHES = 1000000 };
    u64 times[ROUNDS];
    for (int r = 0; r < ROUNDS; ++r) {
        for (int i = 0; i < PUSHES; ++i)
            ASSERT_TRUE(de_arena_push(a, 16, 8) != NULL);
        const u64 t0 = now_ns();
        de_arena_reset(a);
        times[r] = now_ns() - t0;
        EXPECT_EQ(de_arena_get_info(a).used, de_arena_mark_get(a).pos);
    }
    /* Median of the rounds, so one preemption does not decide the result. */
    for (int i = 1; i < ROUNDS; ++i)
        for (int j = i; j > 0 && times[j - 1] > times[j]; --j) {
            u64 t = times[j];
            times[j] = times[j - 1];
            times[j - 1] = t;
        }
    printf("    arena.reset after %d pushes: median %llu ns\n", PUSHES,
           (unsigned long long)times[ROUNDS / 2]);
#if defined(DE_DEBUG) || defined(DE_TEST_SANITIZED)
    /* Debug builds fill the popped 16 MB with 0xDD on reset (by design O(bytes)); the O(1) bound
     * is checked in the other build types. Under a sanitizer the timing is too noisy for a 1 us
     * bound when other tests (core.stress_mixed) run in parallel: it failed once under Linux TSan
     * and once under a parallel ASan+UBSan run (DENG-155). The median is still printed. */
#else
    EXPECT_LT(times[ROUNDS / 2], 1000u);
#endif
    de_arena_destroy(a);
}

TEST(arena, large_reserve_commit) {
    de_arena *a = make_arena(0); /* default: 64 GiB, or a fallback reservation */
    de_arena_info info = de_arena_get_info(a);
    EXPECT_GE(info.reserved, 1 * GIB + MIB);
    EXPECT_LT(info.committed, 1 * MIB);
    printf("    reserved %zu MiB, committed %zu KiB after create\n", info.reserved >> 20,
           info.committed >> 10);

    const size_t rss0 = rss_bytes();
    for (size_t i = 0; i < 1024; ++i) {
        u8 *p = (u8 *)de_arena_push(a, MIB, 16);
        ASSERT_TRUE(p != NULL);
        p[0] = (u8)i; /* touch one page per MiB */
    }
    const size_t rss1 = rss_bytes();
    info = de_arena_get_info(a);
    EXPECT_GE(info.used, 1 * GIB);
    EXPECT_GE(info.committed, info.used);
    EXPECT_LT(info.committed, info.used + 64 * 1024 * 2);
    printf("    pushed 1 GiB: committed %zu MiB, RSS %zu -> %zu MiB\n", info.committed >> 20,
           rss0 >> 20, rss1 >> 20);
#if defined(DE_TEST_SANITIZED)
    /* The RSS bounds below do not hold under ASan/TSan/MSan: the runtime's shadow memory for the
     * touched pages is resident too (ASan adds 1/8 of the touched bytes, TSan more), so RSS grows
     * by more than the arena committed. The reserve/commit/used checks above still run; the RSS
     * bounds run in every unsanitized build. */
    printf("    RSS bounds not checked: sanitizer shadow memory counts toward RSS\n");
#else
    if (rss0 != 0 && rss1 != 0) {
        const size_t grew = rss1 > rss0 ? rss1 - rss0 : 0;
        /* RSS can never grow by more than what was committed (plus slack for the runtime). */
        EXPECT_LE(grew, info.committed + 64 * MIB);
#if !defined(DE_DEBUG)
        /* Only touched pages are resident: 1024 pages of at most 64 KiB, far below 1 GiB. Debug
         * builds fill every pushed byte with 0xCD, so there the whole GiB is resident. */
        EXPECT_LT(grew, 256 * MIB);
#endif
    }
#endif
    de_arena_destroy(a);
}

TEST(arena, decommit_threshold) {
    de_arena_desc d = {0};
    d.reserve_bytes = 256 * MIB;
    d.decommit_threshold = 1 * MIB;
    de_arena *a = NULL;
    ASSERT_EQ(de_arena_create(&d, &a), DE_OK);
    u8 *p = (u8 *)de_arena_push(a, 32 * MIB, 16);
    ASSERT_TRUE(p != NULL);
    memset(p, 0x77, 32 * MIB);
    EXPECT_GE(de_arena_get_info(a).committed, 32 * MIB);
    de_arena_reset(a);
    de_arena_info info = de_arena_get_info(a);
    EXPECT_LE(info.committed, 1 * MIB + 64 * 1024);
    /* Memory committed again after a decommit reads as zero. */
    u8 *q = (u8 *)de_arena_push_zero(a, 32 * MIB, 16);
    ASSERT_TRUE(q == p);
    size_t nonzero = 0;
    for (size_t i = 0; i < 32 * MIB; ++i)
        nonzero += q[i] != 0;
    EXPECT_EQ(nonzero, 0u);
    de_arena_destroy(a);
}

TEST(arena, allocator_realloc_inplace) {
    de_arena *a = make_arena(64 * MIB);
    de_allocator al = de_arena_allocator(a);

    u8 *p = (u8 *)DE_ALLOC(&al, 64, 16, DE_MEM_GENERAL);
    ASSERT_TRUE(p != NULL);
    for (int i = 0; i < 64; ++i)
        p[i] = (u8)i;

    /* The last block grows in place, also across commit steps. */
    u8 *q = (u8 *)DE_REALLOC(&al, p, 64, 4096, 16, DE_MEM_GENERAL);
    EXPECT_TRUE(q == p);
    q = (u8 *)DE_REALLOC(&al, q, 4096, 8 * MIB, 16, DE_MEM_GENERAL);
    EXPECT_TRUE(q == p);
    for (int i = 0; i < 64; ++i)
        ASSERT_EQ(q[i], (u8)i);
    /* ... and shrinks in place, giving the tail back. */
    q = (u8 *)DE_REALLOC(&al, q, 8 * MIB, 128, 16, DE_MEM_GENERAL);
    EXPECT_TRUE(q == p);
    u8 *next = (u8 *)DE_ALLOC(&al, 16, 16, DE_MEM_GENERAL);
    EXPECT_TRUE(next == p + 128);

    /* A block that is no longer last is copied. */
    u8 *r = (u8 *)DE_REALLOC(&al, p, 128, 256, 16, DE_MEM_GENERAL);
    ASSERT_TRUE(r != NULL);
    EXPECT_TRUE(r != p);
    for (int i = 0; i < 64; ++i)
        ASSERT_EQ(r[i], (u8)i);

    /* realloc(NULL) allocates; free is a no-op. */
    u8 *s = (u8 *)DE_REALLOC(&al, NULL, 0, 32, 8, DE_MEM_GENERAL);
    ASSERT_TRUE(s != NULL);
    const de_arena_mark before = de_arena_mark_get(a);
    DE_FREE(&al, s, 32);
    EXPECT_EQ(de_arena_mark_get(a).pos, before.pos);

    /* DE_NEW goes through alloc. */
    double *d = DE_NEW(&al, double, DE_MEM_GENERAL);
    ASSERT_TRUE(d != NULL);
    EXPECT_EQ((uintptr_t)d % DE_ALIGNOF(double), 0u);
    de_arena_destroy(a);
}

TEST(arena, out_of_reserve_fails) {
    de_arena_desc d = {0};
    d.reserve_bytes = 1 * MIB;
    d.name = "small";
    de_arena *a = NULL;
    ASSERT_EQ(de_arena_create(&d, &a), DE_OK);
    EXPECT_GE(de_arena_get_info(a).reserved, 1 * MIB);
    const size_t reserved = de_arena_get_info(a).reserved;

    const de_arena_mark m = de_arena_mark_get(a);
    de_error_clear();
    EXPECT_TRUE(de_arena_push(a, reserved, 16) == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_arena_mark_get(a).pos, m.pos);
    /* A size that would overflow the address arithmetic fails the same way. */
    de_error_clear();
    EXPECT_TRUE(de_arena_push(a, SIZE_MAX - 8, 16) == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);

    /* The arena still works, and fills up exactly to the reservation. */
    EXPECT_TRUE(de_arena_push(a, 1024, 16) != NULL);
    const size_t left = reserved - de_arena_mark_get(a).pos;
    EXPECT_TRUE(de_arena_push(a, left, 1) != NULL);
    EXPECT_TRUE(de_arena_push(a, 1, 1) == NULL);

    /* The arena allocator fails the same way, and realloc leaves the old block alone. */
    de_arena_reset(a);
    de_allocator al = de_arena_allocator(a);
    u8 *p = (u8 *)DE_ALLOC(&al, 16, 16, 0);
    ASSERT_TRUE(p != NULL);
    p[0] = 42;
    de_error_clear();
    EXPECT_TRUE(DE_REALLOC(&al, p, 16, 2 * reserved, 16, 0) == NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(p[0], 42);
    de_arena_destroy(a);

    /* Invalid arguments. */
    EXPECT_EQ(de_arena_create(&d, NULL), DE_ERR_INVALID_ARGUMENT);
    de_arena_destroy(NULL);
}

TEST(arena, null_allocator) {
    const de_allocator *n = de_allocator_null();
    ASSERT_TRUE(n != NULL);
    de_error_clear();
    EXPECT_TRUE(DE_ALLOC(n, 16, 8, 0) == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    de_error_clear();
    EXPECT_TRUE(DE_REALLOC(n, NULL, 0, 16, 8, 0) == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    DE_FREE(n, NULL, 0);
}

TEST(arena, frame_arena_cycle) {
    EXPECT_TRUE(de_frame_arena() == NULL);
    EXPECT_EQ(de_frame_arenas_init(DE_FRAME_ARENA_MAX_COUNT + 1), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_frame_arenas_init(0), DE_OK); /* default: 2 */
    EXPECT_EQ(de_frame_arenas_init(2), DE_ERR_ALREADY_EXISTS);
    EXPECT_TRUE(de_frame_arena() == NULL);

    de_frame_arena_begin(0);
    de_arena *f0 = de_frame_arena();
    ASSERT_TRUE(f0 != NULL);
    const de_arena_mark start = de_arena_mark_get(f0);
    u8 *p0 = (u8 *)de_arena_push(f0, 1000, 16);
    ASSERT_TRUE(p0 != NULL);
    memset(p0, 0x11, 1000);

    de_frame_arena_begin(1);
    de_arena *f1 = de_frame_arena();
    ASSERT_TRUE(f1 != NULL);
    EXPECT_TRUE(f1 != f0);
    /* Frame 0's data survives while frame 1 is current (it may still be in flight). */
    EXPECT_EQ(p0[999], 0x11);
    EXPECT_EQ(de_arena_mark_get(f0).pos, start.pos + 1000);
    EXPECT_EQ(de_arena_get_info(f1).tag, (u32)DE_MEM_SCRATCH);

    /* Frame 2 reuses frame 0's arena, reset. */
    de_frame_arena_begin(2);
    EXPECT_TRUE(de_frame_arena() == f0);
    EXPECT_EQ(de_arena_mark_get(f0).pos, start.pos);
    EXPECT_TRUE(de_arena_push(f0, 1000, 16) == p0);

    de_frame_arenas_shutdown();
    EXPECT_TRUE(de_frame_arena() == NULL);

    /* Three in flight. */
    ASSERT_EQ(de_frame_arenas_init(3), DE_OK);
    de_arena *seen[3];
    for (u64 i = 0; i < 3; ++i) {
        de_frame_arena_begin(i);
        seen[i] = de_frame_arena();
    }
    EXPECT_TRUE(seen[0] != seen[1] && seen[1] != seen[2] && seen[0] != seen[2]);
    de_frame_arena_begin(4);
    EXPECT_TRUE(de_frame_arena() == seen[1]);
    de_frame_arenas_shutdown();
}

TEST(arena, no_heap_after_create) {
#if !defined(HEAP_COUNTER_AVAILABLE)
    SKIP("no malloc counter on this platform (see the DENG-148 closing comment)");
#else
    /* Setup outside the counted window: the arena, the frame arenas, and this thread's scratch
     * arenas (their first use creates the thread-local block and the pthread keys). */
    de_arena *a = make_arena(256 * MIB);
    ASSERT_EQ(de_frame_arenas_init(2), DE_OK);
    de_scratch s0 = de_scratch_begin(NULL, 0);
    de_scratch s1 = de_scratch_begin(&s0.arena, 1);
    de_scratch_end(s1);
    de_scratch_end(s0);

    /* Self-check: the counter must see a real malloc, or the result below means nothing. */
    void *(*volatile real_malloc)(size_t) = malloc;
    heap_count_start();
    void *probe = real_malloc(48);
    heap_count_stop();
    free(probe);
    if (heap_calls == 0) {
        de_frame_arenas_shutdown();
        de_arena_destroy(a);
        SKIP("the malloc counter sees nothing (sanitizer runtime replaces malloc)");
    }

    heap_count_start();
    for (int frame = 0; frame < 4; ++frame) {
        de_frame_arena_begin((u64)frame);
        (void)de_arena_push(de_frame_arena(), 4096, 64);
        for (int i = 0; i < 1000; ++i)
            (void)de_arena_push(a, 1000, 16); /* crosses many commit steps */
        (void)de_arena_push_zero(a, 3 * MIB, 4096);
        de_allocator al = de_arena_allocator(a);
        void *p = DE_ALLOC(&al, 64, 16, 0);
        p = DE_REALLOC(&al, p, 64, 2 * MIB, 16, 0);
        DE_FREE(&al, p, 2 * MIB);
        de_scratch outer = de_scratch_begin(NULL, 0);
        (void)de_arena_push(outer.arena, 100000, 16);
        de_scratch inner = de_scratch_begin(&outer.arena, 1);
        (void)de_arena_push_zero(inner.arena, 100000, 16);
        de_scratch_end(inner);
        de_scratch_end(outer);
        de_arena_reset(a);
    }
    de_arena *b = NULL;
    de_arena_desc d = {0};
    d.reserve_bytes = 64 * MIB;
    de_result r = de_arena_create(&d, &b);
    if (r == DE_OK) {
        (void)de_arena_push(b, 10, 1);
        de_arena_destroy(b);
    }
    heap_count_stop();

    EXPECT_EQ(r, DE_OK);
    EXPECT_EQ(heap_calls, 0);
    de_frame_arenas_shutdown();
    de_arena_destroy(a);
#endif
}
