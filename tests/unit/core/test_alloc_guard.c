/* Unit tests: the alloc guard of de_test_support (tests/support/de_alloc_guard.h, DENG-155).
 *
 * Its own executable (test_guard), so no thread or hook of another test is around. Everything a
 * region uses for the first time is warmed up before begin. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* posix_memalign */
#endif
#include "de_test.h"

#include "de_alloc_guard.h"

#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdlib.h>
#include <string.h>

/* Keeps the compiler from removing a malloc/free pair. */
static void *volatile g_sink;

/* ---- Arena-only regions ------------------------------------------------------------------- */

TEST(guard, zero_for_arena) {
    de_arena *arena = NULL;
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = (size_t)64 << 20}, &arena), DE_OK);
    de_allocator aa = de_arena_allocator(arena);
    ASSERT_EQ(de_frame_arenas_init(0), DE_OK);
    de_frame_arena_begin(0);
    /* Warm up: the thread's scratch arenas are created on first use. */
    de_scratch warm = de_scratch_begin(NULL, 0);
    de_scratch_end(warm);

    de_alloc_guard g = {0};
    de_alloc_guard_begin(&g);
    for (u32 frame = 1; frame <= 100; ++frame) {
        de_frame_arena_begin(frame);
        u8 *fa = DE_ARENA_PUSH(de_frame_arena(), u8, 4096);
        memset(fa, 1, 4096);
        de_scratch s = de_scratch_begin(NULL, 0);
        de_scratch inner = de_scratch_begin(&s.arena, 1);
        u64 *a = DE_ARENA_PUSH(s.arena, u64, 512);
        u64 *b = DE_ARENA_PUSH(inner.arena, u64, 512);
        a[0] = b[0] = frame;
        de_scratch_end(inner);
        de_scratch_end(s);
        void *p = DE_ALLOC(&aa, 256, 16, DE_MEM_GENERAL);
        p = DE_REALLOC(&aa, p, 256, 512, 16, DE_MEM_GENERAL);
        DE_FREE(&aa, p, 512);
        de_arena_reset(arena);
    }
    de_alloc_guard_end(&g);

    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_EQ(de_alloc_guard_offences(&g), 0u);
    EXPECT_EQ(g.engine_allocs, 0u);
    EXPECT_EQ(g.engine_frees, 0u);
    EXPECT_EQ(g.raw_mallocs, 0u);
    EXPECT_EQ(g.raw_frees, 0u);
    EXPECT_EQ(g.site_count, 0u);
    EXPECT_TRUE(de_alloc_guard_report(&g, __FILE__, __LINE__));

    de_frame_arenas_shutdown();
    de_arena_destroy(arena);
}

/* ---- Raw malloc ------------------------------------------------------------------------------ */

TEST(guard, counts_malloc) {
    if (!de_alloc_guard_counts_raw())
        SKIP("raw malloc calls are not counted in this build (Windows, or macOS ASan/TSan)");
    de_alloc_guard g = {0};
    de_alloc_guard_begin(&g);
    g_sink = malloc(100);
    free(g_sink);
    de_alloc_guard_end(&g);

    EXPECT_EQ(de_alloc_guard_offences(&g), 1u);
    EXPECT_EQ(g.raw_mallocs, 1u);
    EXPECT_EQ(g.raw_bytes, 100u);
    EXPECT_EQ(g.raw_frees, 1u);
    EXPECT_EQ(g.engine_allocs, 0u);
    ASSERT_EQ(g.site_count, 1u);
    EXPECT_EQ(g.sites[0].kind, DE_ALLOC_GUARD_RAW);
    EXPECT_EQ(g.sites[0].size, 100u);
    EXPECT_TRUE(g.sites[0].caller != NULL);
    EXPECT_FALSE(de_alloc_guard_report(&g, __FILE__, __LINE__)); /* prints the site */

    /* Every member of the family counts, once per call. */
    de_alloc_guard_begin(&g);
    void *a = calloc(4, 8);
    g_sink = a;
    void *b = realloc(NULL, 16);
    g_sink = b;
    b = realloc(b, 4096);
    g_sink = b;
    void *c = NULL;
#if defined(_WIN32)
    c = malloc(128); /* not reached: raw calls are not counted on Windows */
#else
    EXPECT_EQ(posix_memalign(&c, 64, 128), 0);
#endif
    g_sink = c;
    free(a);
    free(b);
    free(c);
    free(NULL); /* not a free of a block */
    de_alloc_guard_end(&g);
    EXPECT_EQ(g.raw_mallocs, 4u);
    EXPECT_EQ(g.raw_bytes, 32u + 16u + 4096u + 128u);
    EXPECT_EQ(g.raw_frees, 3u);
}

typedef struct malloc_thread_arg {
    de_sem go, done;
} malloc_thread_arg;

static i32 malloc_on_signal(void *user) {
    malloc_thread_arg *a = (malloc_thread_arg *)user;
    for (int round = 0; round < 2; ++round) {
        de_sem_wait(&a->go);
        g_sink = malloc(24);
        free(g_sink);
        de_sem_post(&a->done, 1);
    }
    return 0;
}

TEST(guard, counts_other_threads) {
    if (!de_alloc_guard_counts_raw())
        SKIP("raw malloc calls are not counted in this build (Windows, or macOS ASan/TSan)");
    malloc_thread_arg arg;
    ASSERT_EQ(de_sem_init(&arg.go, 0), DE_OK);
    ASSERT_EQ(de_sem_init(&arg.done, 0), DE_OK);
    de_thread *t = NULL;
    ASSERT_EQ(de_thread_create(malloc_on_signal, &arg, "guard-malloc", DE_THREAD_NORMAL, 0, &t),
              DE_OK);

    /* Default: every thread counts. The main thread only waits on semaphores meanwhile. */
    de_alloc_guard all = {0};
    de_alloc_guard_begin(&all);
    de_sem_post(&arg.go, 1);
    de_sem_wait(&arg.done);
    de_alloc_guard_end(&all);
    EXPECT_EQ(all.raw_mallocs, 1u);
    EXPECT_EQ(all.raw_frees, 1u);

    /* this_thread_only: the worker's malloc is not counted. */
    de_alloc_guard mine = {0};
    mine.this_thread_only = true;
    de_alloc_guard_begin(&mine);
    de_sem_post(&arg.go, 1);
    de_sem_wait(&arg.done);
    de_alloc_guard_end(&mine);
    EXPECT_EQ(mine.raw_mallocs, 0u);
    EXPECT_EQ(mine.raw_frees, 0u);

    EXPECT_EQ(de_thread_join(t), 0);
    de_sem_destroy(&arg.go);
    de_sem_destroy(&arg.done);
}

/* ---- Engine allocations ---------------------------------------------------------------------- */

TEST(guard, counts_heap) {
    if (!DE_ALLOC_GUARD_SEES_HEAP)
        SKIP("de_heap_allocator() reports its blocks only in Debug and Dev builds");
    const de_allocator *heap = de_heap_allocator();
    DE_FREE(heap, DE_ALLOC(heap, 48, 16, DE_MEM_GENERAL), 48); /* creates the heap */

    de_alloc_guard g = {0};
    de_alloc_guard_begin(&g);
    void *p = DE_ALLOC(heap, 48, 16, DE_MEM_GENERAL);
    DE_FREE(heap, p, 48);
    de_alloc_guard_end(&g);

    EXPECT_EQ(de_alloc_guard_offences(&g), 1u);
    EXPECT_EQ(g.engine_allocs, 1u);
    EXPECT_EQ(g.engine_bytes, 48u);
    EXPECT_EQ(g.engine_frees, 1u);
    EXPECT_EQ(g.raw_mallocs, 0u); /* the heap maps memory from the OS, never through malloc */
    ASSERT_EQ(g.site_count, 1u);
    EXPECT_EQ(g.sites[0].kind, DE_ALLOC_GUARD_ENGINE);
    EXPECT_EQ(g.sites[0].tag, (u32)DE_MEM_GENERAL);
    EXPECT_EQ(g.sites[0].size, 48u);

    /* A realloc is one more allocation (and one free). */
    p = DE_ALLOC(heap, 32, 16, DE_MEM_GENERAL);
    de_alloc_guard_begin(&g);
    p = DE_REALLOC(heap, p, 32, 4000, 16, DE_MEM_GENERAL);
    de_alloc_guard_end(&g);
    DE_FREE(heap, p, 4000);
    EXPECT_EQ(g.engine_allocs, 1u);
    EXPECT_EQ(g.engine_frees, 1u);
}

TEST(guard, counts_tracking_allocator) {
    /* A tracking allocator reports in every build type, even over a non-reporting arena. */
    de_arena *arena = NULL;
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = (size_t)16 << 20}, &arena), DE_OK);
    de_allocator aa = de_arena_allocator(arena);
    de_tracking_allocator t;
    de_allocator ta = de_tracking_allocator_init(&t, &aa, DE_MEM_RENDER);

    de_alloc_guard g = {0};
    de_alloc_guard_begin(&g);
    for (int i = 0; i < 3; ++i)
        DE_FREE(&ta, DE_ALLOC(&ta, 64, 16, DE_MEM_GENERAL), 64);
    de_alloc_guard_end(&g);
    EXPECT_EQ(g.engine_allocs, 3u);
    EXPECT_EQ(g.engine_bytes, 192u);
    EXPECT_EQ(g.engine_frees, 3u);
    ASSERT_EQ(g.site_count, 3u);
    EXPECT_EQ(g.sites[0].tag, (u32)DE_MEM_RENDER); /* the tracker's tag */
    de_arena_destroy(arena);
}

/* ---- Allowlist ---------------------------------------------------------------------------- */

TEST(guard, allowlist) {
    de_arena *arena = NULL;
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = (size_t)16 << 20}, &arena), DE_OK);
    de_allocator aa = de_arena_allocator(arena);
    de_tracking_allocator ts, tr;
    de_allocator scratch = de_tracking_allocator_init(&ts, &aa, DE_MEM_SCRATCH);
    de_allocator render = de_tracking_allocator_init(&tr, &aa, DE_MEM_RENDER);

    /* Nothing is allowed by default. */
    de_alloc_guard g = {0};
    de_alloc_guard_begin(&g);
    (void)DE_ALLOC(&scratch, 32, 16, 0);
    de_alloc_guard_end(&g);
    EXPECT_EQ(g.engine_allocs, 1u);
    EXPECT_EQ(g.allowed_allocs, 0u);

    /* An allowed tag is counted apart; other tags are still offences. */
    de_alloc_guard_allow(&g, DE_MEM_SCRATCH);
    de_alloc_guard_begin(&g);
    (void)DE_ALLOC(&scratch, 32, 16, 0);
    (void)DE_ALLOC(&scratch, 32, 16, 0);
    (void)DE_ALLOC(&render, 32, 16, 0);
    /* Allowing during the region works too. */
    de_alloc_guard_allow(&g, DE_MEM_RENDER);
    (void)DE_ALLOC(&render, 32, 16, 0);
    de_alloc_guard_allow(&g, DE_MEM_TAG_MAX + 5); /* out of range: ignored */
    de_alloc_guard_end(&g);
    EXPECT_EQ(g.allowed_allocs, 3u);
    EXPECT_EQ(g.engine_allocs, 1u);
    ASSERT_EQ(g.site_count, 1u);
    EXPECT_EQ(g.sites[0].tag, (u32)DE_MEM_RENDER);

    /* The allowlist stays with the guard for the next region. */
    de_alloc_guard_begin(&g);
    (void)DE_ALLOC(&render, 32, 16, 0);
    de_alloc_guard_end(&g);
    EXPECT_EQ(g.engine_allocs, 0u);
    EXPECT_EQ(g.allowed_allocs, 1u);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);

    /* Raw calls have their own pseudo-tag. */
    if (de_alloc_guard_counts_raw()) {
        de_alloc_guard r = {0};
        de_alloc_guard_allow(&r, DE_ALLOC_GUARD_TAG_RAW);
        de_alloc_guard_begin(&r);
        g_sink = malloc(10);
        free(g_sink);
        de_alloc_guard_end(&r);
        EXPECT_EQ(r.raw_mallocs, 0u);
        EXPECT_EQ(r.allowed_allocs, 1u);
        EXPECT_EQ(r.raw_frees, 1u);
    }
    de_arena_destroy(arena);
}

/* ---- Hook chaining and the failing assert ---------------------------------------------------- */

typedef struct hook_counts {
    u32 allocs, frees;
} hook_counts;

static void count_alloc(void *user, void *ptr, size_t size, u32 tag) {
    (void)ptr, (void)size, (void)tag;
    ((hook_counts *)user)->allocs++;
}

static void count_free(void *user, void *ptr, size_t size, u32 tag) {
    (void)ptr, (void)size, (void)tag;
    ((hook_counts *)user)->frees++;
}

TEST(guard, chains_hooks) {
    de_arena *arena = NULL;
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = (size_t)16 << 20}, &arena), DE_OK);
    de_allocator aa = de_arena_allocator(arena);
    de_tracking_allocator t;
    de_allocator ta = de_tracking_allocator_init(&t, &aa, DE_MEM_GENERAL);

    hook_counts hc = {0, 0};
    const de_mem_hooks mine = {count_alloc, count_free, &hc};
    const de_mem_hooks *before = de_mem_set_hooks(&mine);

    de_alloc_guard g = {0};
    de_alloc_guard_begin(&g);
    DE_FREE(&ta, DE_ALLOC(&ta, 8, 8, 0), 8);
    de_alloc_guard_end(&g);
    EXPECT_EQ(g.engine_allocs, 1u);
    EXPECT_EQ(hc.allocs, 1u); /* the earlier hooks still saw the block */
    EXPECT_EQ(hc.frees, 1u);

    /* end put the earlier hooks back. */
    EXPECT_TRUE(de_mem_set_hooks(before) == &mine);
    de_arena_destroy(arena);
}

/* Not registered: run by guard.assert_zero_fails, which expects it to fail. */
DE_TEST_CASE(offending_case, guard_inner, offending_region) {
    de_arena *arena = NULL;
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.reserve_bytes = (size_t)1 << 20}, &arena), DE_OK);
    de_allocator aa = de_arena_allocator(arena);
    de_tracking_allocator t;
    de_allocator ta = de_tracking_allocator_init(&t, &aa, DE_MEM_ASSET);
    de_alloc_guard g = {0};
    de_alloc_guard_begin(&g);
    (void)DE_ALLOC(&ta, 100, 16, 0);
    de_alloc_guard_end(&g);
    de_arena_destroy(arena);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
}

TEST(guard, assert_zero_fails) {
    de_test_result res;
    EXPECT_EQ(de_test_run_case(&offending_case, &res, 1), DE_TEST_FAILED);
    EXPECT_TRUE(strstr(res.message, "alloc guard") != NULL);
}
