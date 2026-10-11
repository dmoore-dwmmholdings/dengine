/* Tests for the SPSC ring (DENG-152): order, capacity edges, a 1e7-element two-thread stress run
 * and a throughput measurement. de_test.h checks run only on the test's thread, so worker
 * threads record what they saw and the test thread checks it after the join. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv (DE_QUEUE_BENCH_N) */
#endif

#include "de_test.h"

#include <dengine/de_platform.h>

#include "core/de_spsc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- a counting malloc-backed allocator (DENG-148 may not be merged yet) ---------- */

typedef struct test_heap {
    u32 allocs;
    u32 frees;
    size_t live;
    bool fail; /* return NULL from alloc */
    size_t last_align;
} test_heap;

/* Over-allocates, aligns by hand and keeps the malloc pointer just before the block, so it works
 * where aligned_alloc does not exist (MSVC) and for any power-of-two alignment. */
static void *th_alloc(void *user, size_t size, size_t align, u32 tag) {
    (void)tag;
    test_heap *h = (test_heap *)user;
    if (h->fail)
        return NULL;
    if (align < sizeof(void *))
        align = sizeof(void *);
    u8 *raw = (u8 *)malloc(size + align + sizeof(void *));
    if (raw == NULL)
        return NULL;
    uintptr_t p = ((uintptr_t)(raw + sizeof(void *)) + align - 1) & ~(uintptr_t)(align - 1);
    memcpy((u8 *)p - sizeof(void *), &raw, sizeof raw);
    h->allocs++;
    h->live += size;
    h->last_align = align;
    return (void *)p;
}

static void th_free(void *user, void *ptr, size_t size) {
    test_heap *h = (test_heap *)user;
    if (ptr == NULL)
        return;
    void *raw;
    memcpy(&raw, (u8 *)ptr - sizeof(void *), sizeof raw);
    free(raw);
    h->frees++;
    h->live -= size;
}

static void *th_realloc(void *user, void *ptr, size_t old_size, size_t new_size, size_t align,
                        u32 tag) {
    void *n = th_alloc(user, new_size, align, tag);
    if (n != NULL && ptr != NULL) {
        memcpy(n, ptr, old_size < new_size ? old_size : new_size);
        th_free(user, ptr, old_size);
    }
    return n;
}

static de_allocator th_allocator(test_heap *h) {
    de_allocator a = {th_alloc, th_realloc, th_free, h};
    return a;
}

static void init_u64_queue(de_spsc *q, test_heap *h, const de_allocator *a, u32 capacity) {
    memset(h, 0, sizeof *h);
    de_spsc_desc d = {0};
    d.capacity = capacity;
    d.elem_size = sizeof(u64);
    d.allocator = a;
    ASSERT_EQ(de_spsc_init(q, &d), DE_OK);
}

/* ---------- single-threaded behaviour ---------- */

typedef struct odd_elem {
    u32 a, b, c; /* 12 bytes: a stride that is not a power of two */
} odd_elem;

TEST(spsc, order) {
    test_heap h = {0};
    de_allocator a = th_allocator(&h);
    de_spsc q;
    de_spsc_desc d = {0};
    d.capacity = 8;
    d.elem_size = sizeof(odd_elem);
    d.allocator = &a;
    ASSERT_EQ(de_spsc_init(&q, &d), DE_OK);
    EXPECT_EQ(h.allocs, 1u);
    EXPECT_EQ(h.last_align, (size_t)DE_CACHE_LINE);
    EXPECT_EQ(de_spsc_capacity(&q), 8u);
    EXPECT_EQ(de_spsc_size_approx(&q), 0u);
    EXPECT_EQ(de_spsc_peek(&q), NULL);

    /* Many laps around the ring with the fill level going up and down. */
    u32 next_in = 0, next_out = 0;
    for (u32 round = 0; round < 500; round++) {
        u32 pushes = (round * 7u) % 9u; /* 0..8 */
        for (u32 i = 0; i < pushes; i++) {
            odd_elem e = {next_in, ~next_in, next_in * 3u};
            if (!de_spsc_push(&q, &e))
                break;
            next_in++;
        }
        EXPECT_EQ(de_spsc_size_approx(&q), next_in - next_out);
        u32 pops = (round * 5u) % 7u;
        for (u32 i = 0; i < pops; i++) {
            odd_elem *front = (odd_elem *)de_spsc_peek(&q);
            odd_elem e;
            if (!de_spsc_pop(&q, &e)) {
                EXPECT_EQ(front, NULL);
                break;
            }
            ASSERT_NE(front, NULL);
            ASSERT_EQ(e.a, next_out);
            ASSERT_EQ(e.b, ~next_out);
            ASSERT_EQ(e.c, next_out * 3u);
            next_out++;
        }
    }
    EXPECT_GT(next_out, 1000u);

    /* Peek leaves the element in place; pop(NULL) drops it. */
    while (de_spsc_pop(&q, NULL)) {
    }
    odd_elem x = {1, 2, 3}, y = {4, 5, 6}, out;
    ASSERT_TRUE(de_spsc_push(&q, &x));
    ASSERT_TRUE(de_spsc_push(&q, &y));
    odd_elem *p = (odd_elem *)de_spsc_peek(&q);
    ASSERT_NE(p, NULL);
    EXPECT_EQ(p->a, 1u);
    EXPECT_EQ(de_spsc_peek(&q), (void *)p);
    EXPECT_TRUE(de_spsc_pop(&q, NULL));
    ASSERT_TRUE(de_spsc_pop(&q, &out));
    EXPECT_EQ(out.a, 4u);
    EXPECT_EQ(out.c, 6u);
    EXPECT_FALSE(de_spsc_pop(&q, &out));

    EXPECT_EQ(h.allocs, 1u); /* nothing allocated after init */
    de_spsc_destroy(&q);
    EXPECT_EQ(h.frees, 1u);
    EXPECT_EQ(h.live, (size_t)0);
    EXPECT_EQ(q.slots, NULL);
}

static void check_edges(u32 capacity) {
    test_heap h;
    de_allocator a = th_allocator(&h);
    de_spsc q;
    init_u64_queue(&q, &h, &a, capacity);
    ASSERT_EQ(de_spsc_capacity(&q), capacity);

    u64 v = 0, out = 0;
    /* Three full laps, so the indices pass the mask several times. */
    for (u32 lap = 0; lap < 3; lap++) {
        EXPECT_EQ(de_spsc_size_approx(&q), 0u);
        EXPECT_FALSE(de_spsc_pop(&q, &out));
        EXPECT_EQ(de_spsc_peek(&q), NULL);
        u64 first = v;
        for (u32 i = 0; i < capacity; i++) {
            ASSERT_TRUE(de_spsc_push(&q, &v));
            v++;
        }
        EXPECT_EQ(de_spsc_size_approx(&q), capacity);
        u64 rejected = 0xdeadbeefull;
        EXPECT_FALSE(de_spsc_push(&q, &rejected)); /* full */
        EXPECT_FALSE(de_spsc_push(&q, &rejected));
        ASSERT_NE(de_spsc_peek(&q), NULL);
        EXPECT_EQ(*(u64 *)de_spsc_peek(&q), first);
        for (u32 i = 0; i < capacity; i++) {
            ASSERT_TRUE(de_spsc_pop(&q, &out));
            ASSERT_EQ(out, first + i);
        }
        EXPECT_FALSE(de_spsc_pop(&q, &out)); /* empty */
    }
    /* One in, one out at the boundary: full -> pop one -> push one fits again. */
    for (u32 i = 0; i < capacity; i++)
        ASSERT_TRUE(de_spsc_push(&q, &v));
    ASSERT_TRUE(de_spsc_pop(&q, NULL));
    EXPECT_TRUE(de_spsc_push(&q, &v));
    EXPECT_FALSE(de_spsc_push(&q, &v));
    de_spsc_destroy(&q);
    EXPECT_EQ(h.live, (size_t)0);
}

TEST(spsc, capacity_edges) {
    check_edges(1);
    check_edges(2);
    check_edges(1024);

    /* Invalid descriptions fail, set the error context and leave the queue zeroed. */
    test_heap h = {0};
    de_allocator a = th_allocator(&h);
    de_spsc q;
    de_spsc_desc d = {0};
    d.allocator = &a;
    d.capacity = 3;
    de_error_clear();
    EXPECT_EQ(de_spsc_init(&q, &d), DE_ERR_INVALID_ARGUMENT);
    ASSERT_NE(de_error_last(), NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(q.slots, NULL);
    d.capacity = 0x80000001u;
    EXPECT_EQ(de_spsc_init(&q, &d), DE_ERR_INVALID_ARGUMENT);
    d.capacity = 4;
    d.elem_align = 3;
    EXPECT_EQ(de_spsc_init(&q, &d), DE_ERR_INVALID_ARGUMENT);
    d.elem_align = DE_CACHE_LINE * 2;
    EXPECT_EQ(de_spsc_init(&q, &d), DE_ERR_INVALID_ARGUMENT);
    d.elem_align = 0;
    d.allocator = NULL;
    EXPECT_EQ(de_spsc_init(&q, &d), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_spsc_init(NULL, &d), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_spsc_init(&q, NULL), DE_ERR_INVALID_ARGUMENT);
    d.allocator = &a;
    h.fail = true;
    EXPECT_EQ(de_spsc_init(&q, &d), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    de_spsc_destroy(&q); /* a failed queue may be destroyed */
    EXPECT_EQ(h.frees, 0u);
    h.fail = false;
    d.allocator = de_allocator_null(); /* the engine's always-failing allocator */
    EXPECT_EQ(de_spsc_init(&q, &d), DE_ERR_OUT_OF_MEMORY);
    d.allocator = &a;

    /* Defaults: a zero desc plus an allocator is a 256-entry pointer queue. */
    de_spsc_desc z = {0};
    z.allocator = &a;
    ASSERT_EQ(de_spsc_init(&q, &z), DE_OK);
    EXPECT_EQ(de_spsc_capacity(&q), DE_SPSC_DEFAULT_CAPACITY);
    EXPECT_EQ(q.elem_size, (u32)sizeof(void *));
    void *ptr = &h;
    ASSERT_TRUE(de_spsc_push(&q, &ptr));
    void *got = NULL;
    ASSERT_TRUE(de_spsc_pop(&q, &got));
    EXPECT_EQ(got, (void *)&h);
    de_spsc_destroy(&q);
    EXPECT_EQ(h.live, (size_t)0);
}

/* ---------- two-thread stress ---------- */

#define STRESS_COUNT 10000000ull

typedef struct stress_ctx {
    de_spsc *q;
    u64 count;
    /* Written by the consumer, read by the test after the join. */
    u64 received;
    u64 first_bad_index;
    u64 first_bad_value;
    u64 peek_mismatches;
} stress_ctx;

static i32 stress_producer(void *user) {
    stress_ctx *c = (stress_ctx *)user;
    for (u64 i = 0; i < c->count; i++) {
        while (!de_spsc_push(c->q, &i))
            de_cpu_pause();
    }
    return 0;
}

static i32 stress_consumer(void *user) {
    stress_ctx *c = (stress_ctx *)user;
    c->first_bad_index = UINT64_MAX;
    for (u64 expect = 0; expect < c->count;) {
        u64 *front = (u64 *)de_spsc_peek(c->q);
        if (front == NULL) {
            de_cpu_pause();
            continue;
        }
        u64 peeked = *front;
        u64 v = 0;
        if (!de_spsc_pop(c->q, &v)) {
            c->first_bad_index = expect; /* peek saw an element that pop did not */
            return 1;
        }
        if (peeked != v)
            c->peek_mismatches++;
        if (v != expect && c->first_bad_index == UINT64_MAX) {
            c->first_bad_index = expect;
            c->first_bad_value = v;
        }
        c->received++;
        expect++;
    }
    return 0;
}

TEST(spsc, stress_1e7) {
    test_heap h;
    de_allocator a = th_allocator(&h);
    de_spsc q;
    /* A small ring: the producer hits "full" and the consumer "empty" constantly, so the
     * cached-index refresh paths run millions of times. */
    init_u64_queue(&q, &h, &a, 64);

    stress_ctx c = {0};
    c.q = &q;
    c.count = STRESS_COUNT;
    de_thread *tp = NULL, *tc = NULL;
    ASSERT_EQ(de_thread_create(stress_consumer, &c, "spsc-consumer", DE_THREAD_NORMAL, 0, &tc),
              DE_OK);
    ASSERT_EQ(de_thread_create(stress_producer, &c, "spsc-producer", DE_THREAD_NORMAL, 0, &tp),
              DE_OK);
    EXPECT_EQ(de_thread_join(tp), 0);
    EXPECT_EQ(de_thread_join(tc), 0);

    EXPECT_EQ(c.received, STRESS_COUNT);
    EXPECT_EQ(c.first_bad_index, UINT64_MAX); /* every value arrived in order */
    EXPECT_EQ(c.first_bad_value, 0ull);
    EXPECT_EQ(c.peek_mismatches, 0ull);
    EXPECT_EQ(de_spsc_size_approx(&q), 0u);
    EXPECT_EQ(h.allocs, 1u);
    de_spsc_destroy(&q);
}

/* ---------- throughput ---------- */

/* Elements per measurement; DE_QUEUE_BENCH_N overrides it (the closing numbers of DENG-152 used
 * a Release build and DE_QUEUE_BENCH_N=50000000). The test never fails on speed. */
static u64 bench_n(void) {
    const char *s = getenv("DE_QUEUE_BENCH_N");
    u64 n = s ? strtoull(s, NULL, 10) : 0;
    return n ? n : 2000000ull;
}

static double mops(u64 items, u64 ns) {
    return ns ? (double)items * 1000.0 / (double)ns : 0.0;
}

TEST(spsc, bench) {
    test_heap h;
    de_allocator a = th_allocator(&h);
    de_spsc q;
    init_u64_queue(&q, &h, &a, 1024);
    u64 n = bench_n();

    /* 1 thread: push and pop in bursts of 512 on the same thread. */
    u64 t0 = de_time_now_ns();
    u64 sum = 0;
    for (u64 i = 0; i < n; i += 512) {
        for (u64 j = 0; j < 512; j++) {
            u64 v = i + j;
            (void)de_spsc_push(&q, &v);
        }
        for (u64 j = 0; j < 512; j++) {
            u64 v = 0;
            (void)de_spsc_pop(&q, &v);
            sum += v;
        }
    }
    u64 t1 = de_time_now_ns();
    EXPECT_NE(sum, 0ull);

    /* 2 threads: one producer, one consumer. */
    stress_ctx c = {0};
    c.q = &q;
    c.count = n;
    de_thread *tp = NULL, *tc = NULL;
    u64 t2 = de_time_now_ns();
    ASSERT_EQ(de_thread_create(stress_consumer, &c, "spsc-consumer", DE_THREAD_NORMAL, 0, &tc),
              DE_OK);
    ASSERT_EQ(de_thread_create(stress_producer, &c, "spsc-producer", DE_THREAD_NORMAL, 0, &tp),
              DE_OK);
    (void)de_thread_join(tp);
    (void)de_thread_join(tc);
    u64 t3 = de_time_now_ns();
    EXPECT_EQ(c.received, n);

    printf("[BENCH] spsc u64, capacity 1024, %llu items: 1 thread %.1f M items/s, "
           "1P/1C %.1f M items/s\n",
           (unsigned long long)n, mops(n, t1 - t0), mops(n, t3 - t2));
    de_spsc_destroy(&q);
}
