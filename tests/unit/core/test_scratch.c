/* Unit tests: thread scratch arenas (DENG-148).
 *
 * Threads are raw pthreads (Win32 threads on Windows): the engine's thread API (DENG-147) is not
 * a dependency of de_core. */
#include "de_test.h"

#include <dengine/de_memory.h>

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

TEST(scratch, nested_conflicts) {
    de_scratch a = de_scratch_begin(NULL, 0);
    ASSERT_TRUE(a.arena != NULL);
    EXPECT_EQ(de_arena_get_info(a.arena).tag, (u32)DE_MEM_SCRATCH);
    u8 *pa = (u8 *)de_arena_push(a.arena, 256, 16);
    ASSERT_TRUE(pa != NULL);
    memset(pa, 0xA1, 256);
    const de_arena_mark a_after = de_arena_mark_get(a.arena);

    /* A conflicting arena is never handed out. */
    de_scratch b = de_scratch_begin(&a.arena, 1);
    ASSERT_TRUE(b.arena != NULL);
    EXPECT_TRUE(b.arena != a.arena);
    const de_arena_mark b_start = b.mark;
    u8 *pb = (u8 *)de_arena_push(b.arena, 512, 16);
    ASSERT_TRUE(pb != NULL);
    memset(pb, 0xB2, 512);

    /* Nesting inside B with B as the conflict goes back to A, above A's data. */
    de_scratch c = de_scratch_begin(&b.arena, 1);
    EXPECT_TRUE(c.arena == a.arena);
    EXPECT_EQ(c.mark.pos, a_after.pos);
    u8 *pc = (u8 *)de_arena_push(c.arena, 128, 16);
    ASSERT_TRUE(pc != NULL);
    EXPECT_TRUE(pc >= pa + 256);
    memset(pc, 0xC3, 128);
    de_scratch_end(c);
    EXPECT_EQ(de_arena_mark_get(a.arena).pos, a_after.pos);

    /* No conflicts: the first arena, stacked on what is already there. */
    de_scratch d = de_scratch_begin(NULL, 0);
    EXPECT_TRUE(d.arena == a.arena);
    de_scratch_end(d);

    /* Some unrelated arena in conflicts changes nothing. */
    de_arena *other = NULL;
    de_arena_desc od = {0};
    od.reserve_bytes = (size_t)16 << 20;
    ASSERT_EQ(de_arena_create(&od, &other), DE_OK);
    de_arena *const both[2] = {other, b.arena};
    de_scratch e = de_scratch_begin(both, 2);
    EXPECT_TRUE(e.arena == a.arena);
    de_scratch_end(e);
    de_arena_destroy(other);

    /* Ending restores the marks; the outer data is intact. */
    de_scratch_end(b);
    EXPECT_EQ(de_arena_mark_get(b.arena).pos, b_start.pos);
    for (int i = 0; i < 256; ++i)
        ASSERT_EQ(pa[i], 0xA1);
    de_scratch_end(a);
    EXPECT_EQ(de_arena_mark_get(a.arena).pos, a.mark.pos);

    /* The same two arenas come back next time: they live as long as the thread. */
    de_scratch again = de_scratch_begin(NULL, 0);
    EXPECT_TRUE(again.arena == a.arena);
    de_scratch again2 = de_scratch_begin(&again.arena, 1);
    EXPECT_TRUE(again2.arena == b.arena);
    de_scratch_end(again2);
    de_scratch_end(again);
}

/* ---- Threads ------------------------------------------------------------------------------- */

enum { THREADS = 8, ITERATIONS = 2000 };

typedef struct scratch_probe {
    int index;
    de_arena *arena_a, *arena_b;
    int failures;
} scratch_probe;

static atomic_int barrier_count;

/* Spin until all THREADS have arrived at barrier number `round` (1-based). */
static void barrier_wait(int round) {
    atomic_fetch_add(&barrier_count, 1);
    while (atomic_load(&barrier_count) < round * THREADS) {
#if defined(_WIN32)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
}

static int check_fill(const u8 *p, size_t n, u8 v) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != v)
            return 1;
    return 0;
}

static void scratch_worker(scratch_probe *pr) {
    const u8 tag_a = (u8)(0x10 + pr->index);
    const u8 tag_b = (u8)(0x80 + pr->index);
    barrier_wait(1); /* all threads start together */
    for (int it = 0; it < ITERATIONS; ++it) {
        de_scratch a = de_scratch_begin(NULL, 0);
        const size_t na = 64 + (size_t)((it * 37 + pr->index * 11) % 4000);
        u8 *pa = (u8 *)de_arena_push(a.arena, na, 16);
        if (pa == NULL) {
            pr->failures++;
            break;
        }
        memset(pa, tag_a, na);

        de_scratch b = de_scratch_begin(&a.arena, 1);
        if (b.arena == a.arena)
            pr->failures++;
        const size_t nb = 32 + (size_t)((it * 53 + pr->index * 7) % 8000);
        u8 *pb = (u8 *)de_arena_push(b.arena, nb, 16);
        if (pb == NULL) {
            pr->failures++;
            break;
        }
        memset(pb, tag_b, nb);
        pr->arena_a = a.arena;
        pr->arena_b = b.arena;

        /* Other threads write their own scratch meanwhile; ours must be unchanged. */
        pr->failures += check_fill(pa, na, tag_a);
        pr->failures += check_fill(pb, nb, tag_b);
        de_scratch_end(b);
        pr->failures += check_fill(pa, na, tag_a);
        de_scratch_end(a);
        if (de_arena_mark_get(a.arena).pos != a.mark.pos)
            pr->failures++;
    }
    /* Stay alive until every thread is done, so no arena is released and its address reused
     * while the main thread compares them. */
    barrier_wait(2);
}

#if defined(_WIN32)
static unsigned __stdcall scratch_thread(void *p) {
    scratch_worker((scratch_probe *)p);
    return 0;
}
#else
static void *scratch_thread(void *p) {
    scratch_worker((scratch_probe *)p);
    return NULL;
}
#endif

TEST(scratch, thread_isolation) {
    scratch_probe probes[THREADS];
    memset(probes, 0, sizeof probes);
    atomic_store(&barrier_count, 0);
    /* Created before the threads, so it is alive while theirs are and cannot share an address. */
    de_scratch mine = de_scratch_begin(NULL, 0);
#if defined(_WIN32)
    HANDLE th[THREADS];
    for (int i = 0; i < THREADS; ++i) {
        probes[i].index = i;
        th[i] = (HANDLE)_beginthreadex(NULL, 0, scratch_thread, &probes[i], 0, NULL);
        ASSERT_TRUE(th[i] != NULL);
    }
    for (int i = 0; i < THREADS; ++i) {
        WaitForSingleObject(th[i], INFINITE);
        CloseHandle(th[i]);
    }
#else
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; ++i) {
        probes[i].index = i;
        ASSERT_EQ(pthread_create(&th[i], NULL, scratch_thread, &probes[i]), 0);
    }
    for (int i = 0; i < THREADS; ++i)
        pthread_join(th[i], NULL);
#endif

    /* Every thread had two arenas of its own: 16 distinct arenas, none of them ours. */
    de_arena *all[2 * THREADS];
    for (int i = 0; i < THREADS; ++i) {
        EXPECT_EQ(probes[i].failures, 0);
        ASSERT_TRUE(probes[i].arena_a != NULL && probes[i].arena_b != NULL);
        all[2 * i] = probes[i].arena_a;
        all[2 * i + 1] = probes[i].arena_b;
    }
    for (int i = 0; i < 2 * THREADS; ++i) {
        EXPECT_TRUE(all[i] != mine.arena);
        for (int j = i + 1; j < 2 * THREADS; ++j)
            EXPECT_TRUE(all[i] != all[j]);
    }
    de_scratch_end(mine);
}
