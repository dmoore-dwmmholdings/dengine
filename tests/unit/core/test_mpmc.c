/* Tests for the Vyukov bounded MPMC queue (DENG-152): exactly-once delivery with 4 producers and
 * 4 consumers, capacity edges (1, 2, 1024) single- and multi-threaded, the semaphore blocking
 * helpers, and a throughput measurement. de_test.h checks run only on the test's thread, so
 * worker threads record what they saw and the test thread checks it after the join. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv (DE_QUEUE_BENCH_N) */
#endif

#include "de_test.h"

#include <dengine/de_platform.h>

#include "core/de_mpmc.h"

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

static void init_u64_queue(de_mpmc *q, test_heap *h, const de_allocator *a, u32 capacity) {
    memset(h, 0, sizeof *h);
    de_mpmc_desc d = {0};
    d.capacity = capacity;
    d.elem_size = sizeof(u64);
    d.allocator = a;
    ASSERT_EQ(de_mpmc_init(q, &d), DE_OK);
}

static void spawn(de_thread **t, de_thread_fn fn, void *user) {
    ASSERT_EQ(de_thread_create(fn, user, "mpmc-test", DE_THREAD_NORMAL, 0, t), DE_OK);
}

/* ---------- exactly-once transfer: P producers x C consumers ---------- */

/* An item is (producer << 32) | sequence. Consumers count, sum, and set the item's bit in a
 * shared bitmap with fetch_or; a bit that was already set is a duplicate. Each consumer also
 * checks that it sees every producer's sequence numbers in increasing order (dequeue positions
 * are claimed in increasing order, so FIFO per producer holds for any one consumer). */
typedef struct xfer {
    de_mpmc *q;
    u32 producers;
    u64 per_producer;
    de_atomic_u64 *bitmap; /* producers * per_producer bits */
    de_atomic_u64 consumed;
    de_atomic_u32 start; /* released together so all threads overlap */
} xfer;

typedef struct producer_arg {
    xfer *x;
    u32 id;
} producer_arg;

typedef struct consumer_arg {
    xfer *x;
    u64 count;
    u64 sum;
    u64 duplicates;
    u64 out_of_range;
    u64 order_violations;
    u64 last_seq[8]; /* last sequence + 1 seen per producer; 0 = none yet */
} consumer_arg;

static void wait_start(xfer *x) {
    while (de_atomic_load_u32(&x->start, DE_ATOMIC_ACQUIRE) == 0)
        de_cpu_pause();
}

static i32 xfer_producer(void *user) {
    producer_arg *p = (producer_arg *)user;
    xfer *x = p->x;
    wait_start(x);
    for (u64 s = 0; s < x->per_producer; s++) {
        u64 item = ((u64)p->id << 32) | s;
        while (!de_mpmc_try_push(x->q, &item))
            de_cpu_pause();
    }
    return 0;
}

static i32 xfer_consumer(void *user) {
    consumer_arg *c = (consumer_arg *)user;
    xfer *x = c->x;
    const u64 total = (u64)x->producers * x->per_producer;
    wait_start(x);
    while (de_atomic_load_u64(&x->consumed, DE_ATOMIC_RELAXED) < total) {
        u64 item;
        if (!de_mpmc_try_pop(x->q, &item)) {
            de_cpu_pause();
            continue;
        }
        de_atomic_fetch_add_u64(&x->consumed, 1, DE_ATOMIC_RELAXED);
        c->count++;
        c->sum += item;
        u32 pid = (u32)(item >> 32);
        u64 seq = item & 0xffffffffull;
        if (pid >= x->producers || seq >= x->per_producer) {
            c->out_of_range++;
            continue;
        }
        if (seq + 1 <= c->last_seq[pid])
            c->order_violations++;
        c->last_seq[pid] = seq + 1;
        u64 bit = (u64)pid * x->per_producer + seq;
        u64 mask = 1ull << (bit & 63);
        if (de_atomic_fetch_or_u64(&x->bitmap[bit >> 6], mask, DE_ATOMIC_RELAXED) & mask)
            c->duplicates++;
    }
    return 0;
}

static void run_transfer(u32 capacity, u32 producers, u32 consumers, u64 per_producer) {
    ASSERT_LE(producers, 8u);
    ASSERT_LE(consumers, 8u);
    test_heap h;
    de_allocator a = th_allocator(&h);
    de_mpmc q;
    init_u64_queue(&q, &h, &a, capacity);

    const u64 total = (u64)producers * per_producer;
    const size_t words = (size_t)((total + 63) / 64);
    xfer x;
    memset(&x, 0, sizeof x);
    x.q = &q;
    x.producers = producers;
    x.per_producer = per_producer;
    x.bitmap = (de_atomic_u64 *)calloc(words, sizeof(de_atomic_u64));
    ASSERT_NE(x.bitmap, NULL);
    de_atomic_init_u64(&x.consumed, 0);
    de_atomic_init_u32(&x.start, 0);

    producer_arg pa[8];
    consumer_arg ca[8];
    memset(ca, 0, sizeof ca);
    de_thread *tp[8] = {0}, *tc[8] = {0};
    for (u32 i = 0; i < consumers; i++) {
        ca[i].x = &x;
        spawn(&tc[i], xfer_consumer, &ca[i]);
    }
    for (u32 i = 0; i < producers; i++) {
        pa[i].x = &x;
        pa[i].id = i;
        spawn(&tp[i], xfer_producer, &pa[i]);
    }
    de_atomic_store_u32(&x.start, 1, DE_ATOMIC_RELEASE);
    for (u32 i = 0; i < producers; i++)
        EXPECT_EQ(de_thread_join(tp[i]), 0);
    for (u32 i = 0; i < consumers; i++)
        EXPECT_EQ(de_thread_join(tc[i]), 0);

    u64 count = 0, sum = 0, dups = 0, oor = 0, order = 0;
    for (u32 i = 0; i < consumers; i++) {
        count += ca[i].count;
        sum += ca[i].sum;
        dups += ca[i].duplicates;
        oor += ca[i].out_of_range;
        order += ca[i].order_violations;
    }
    /* Expected checksum: sum over p, s of (p << 32) | s, in wrapping u64 arithmetic. */
    u64 expect_sum = 0;
    for (u32 p = 0; p < producers; p++)
        expect_sum += ((u64)p << 32) * per_producer + per_producer * (per_producer - 1) / 2;
    u64 missing = 0;
    for (u64 b = 0; b < total; b++) {
        if (!(de_atomic_load_u64(&x.bitmap[b >> 6], DE_ATOMIC_RELAXED) & (1ull << (b & 63))))
            missing++;
    }

    EXPECT_EQ(count, total);
    EXPECT_EQ(sum, expect_sum);
    EXPECT_EQ(dups, 0ull);
    EXPECT_EQ(oor, 0ull);
    EXPECT_EQ(missing, 0ull);
    EXPECT_EQ(order, 0ull);
    EXPECT_EQ(de_mpmc_size_approx(&q), 0u);
    u64 tmp;
    EXPECT_FALSE(de_mpmc_try_pop(&q, &tmp));
    EXPECT_EQ(h.allocs, 1u); /* nothing allocated after init */
    free(x.bitmap);
    de_mpmc_destroy(&q);
    EXPECT_EQ(h.live, (size_t)0);
}

TEST(mpmc, exactly_once) {
    /* The acceptance run: 4 x 4 threads, 1e6 items per producer. */
    run_transfer(1024, 4, 4, 1000000);
    /* A small ring under the same load: constant full/empty contention. */
    run_transfer(8, 4, 4, 100000);
}

/* ---------- capacity edges ---------- */

static void check_edges(u32 capacity) {
    test_heap h;
    de_allocator a = th_allocator(&h);
    de_mpmc q;
    init_u64_queue(&q, &h, &a, capacity);
    ASSERT_EQ(de_mpmc_capacity(&q), capacity);

    u64 v = 0, out = 0;
    /* Three full laps, so positions pass the mask and every cell's sequence moves on. */
    for (u32 lap = 0; lap < 3; lap++) {
        EXPECT_EQ(de_mpmc_size_approx(&q), 0u);
        EXPECT_FALSE(de_mpmc_try_pop(&q, &out));
        u64 first = v;
        for (u32 i = 0; i < capacity; i++) {
            ASSERT_TRUE(de_mpmc_try_push(&q, &v));
            v++;
        }
        EXPECT_EQ(de_mpmc_size_approx(&q), capacity);
        u64 rejected = 0xdeadbeefull;
        EXPECT_FALSE(de_mpmc_try_push(&q, &rejected)); /* full */
        EXPECT_FALSE(de_mpmc_try_push(&q, &rejected));
        for (u32 i = 0; i < capacity; i++) {
            ASSERT_TRUE(de_mpmc_try_pop(&q, &out));
            ASSERT_EQ(out, first + i);
        }
        EXPECT_FALSE(de_mpmc_try_pop(&q, &out)); /* empty */
    }
    /* Full -> pop one -> exactly one push fits again. */
    for (u32 i = 0; i < capacity; i++)
        ASSERT_TRUE(de_mpmc_try_push(&q, &v));
    ASSERT_TRUE(de_mpmc_try_pop(&q, NULL));
    EXPECT_TRUE(de_mpmc_try_push(&q, &v));
    EXPECT_FALSE(de_mpmc_try_push(&q, &v));
    de_mpmc_destroy(&q);
    EXPECT_EQ(h.live, (size_t)0);
}

typedef struct big_elem {
    u8 bytes[40]; /* 40 bytes, natural alignment 8 */
} big_elem;

TEST(mpmc, capacity_edges) {
    check_edges(1);
    check_edges(2);
    check_edges(1024);

    /* Capacity 1 and 2 under contention: the cell sequences go around every single item. */
    run_transfer(1, 1, 1, 100000);
    run_transfer(1, 2, 2, 50000);
    run_transfer(2, 4, 4, 50000);

    /* Element layout: odd sizes and an explicit alignment keep their bytes. */
    test_heap h = {0};
    de_allocator a = th_allocator(&h);
    de_mpmc q;
    de_mpmc_desc d = {0};
    d.capacity = 4;
    d.elem_size = sizeof(big_elem);
    d.elem_align = 32;
    d.allocator = &a;
    ASSERT_EQ(de_mpmc_init(&q, &d), DE_OK);
    EXPECT_EQ(h.last_align, (size_t)DE_CACHE_LINE);
    EXPECT_EQ(q.data_offset % 32u, 0u);
    EXPECT_EQ(q.stride % 32u, 0u);
    for (u32 i = 0; i < 4; i++) {
        big_elem e;
        memset(e.bytes, (int)(0x10 + i), sizeof e.bytes);
        ASSERT_TRUE(de_mpmc_try_push(&q, &e));
    }
    for (u32 i = 0; i < 4; i++) {
        big_elem e, want;
        memset(want.bytes, (int)(0x10 + i), sizeof want.bytes);
        ASSERT_TRUE(de_mpmc_try_pop(&q, &e));
        EXPECT_MEMEQ(e.bytes, want.bytes, sizeof e.bytes);
    }
    de_mpmc_destroy(&q);

    /* Invalid descriptions fail, set the error context and leave the queue zeroed. */
    memset(&d, 0, sizeof d);
    d.allocator = &a;
    d.capacity = 6;
    de_error_clear();
    EXPECT_EQ(de_mpmc_init(&q, &d), DE_ERR_INVALID_ARGUMENT);
    ASSERT_NE(de_error_last(), NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(q.cells, NULL);
    d.capacity = 0x80000001u;
    EXPECT_EQ(de_mpmc_init(&q, &d), DE_ERR_INVALID_ARGUMENT);
    d.capacity = 4;
    d.elem_align = 12;
    EXPECT_EQ(de_mpmc_init(&q, &d), DE_ERR_INVALID_ARGUMENT);
    d.elem_align = 0;
    d.allocator = NULL; /* NULL means the default heap */
    ASSERT_EQ(de_mpmc_init(&q, &d), DE_OK);
    EXPECT_TRUE(q.allocator == de_heap_allocator());
    de_mpmc_destroy(&q);
    EXPECT_EQ(de_mpmc_init(NULL, &d), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_mpmc_init(&q, NULL), DE_ERR_INVALID_ARGUMENT);
    d.allocator = &a;
    h.fail = true;
    EXPECT_EQ(de_mpmc_init(&q, &d), DE_ERR_OUT_OF_MEMORY);
    de_mpmc_destroy(&q); /* a failed queue may be destroyed */
    h.fail = false;
    d.allocator = de_allocator_null(); /* the engine's always-failing allocator */
    EXPECT_EQ(de_mpmc_init(&q, &d), DE_ERR_OUT_OF_MEMORY);
    d.allocator = &a;

    /* Defaults: a zero desc plus an allocator is a 256-entry pointer queue. */
    de_mpmc_desc z = {0};
    z.allocator = &a;
    ASSERT_EQ(de_mpmc_init(&q, &z), DE_OK);
    EXPECT_EQ(de_mpmc_capacity(&q), DE_MPMC_DEFAULT_CAPACITY);
    EXPECT_EQ(q.elem_size, (u32)sizeof(void *));
    void *ptr = &h, *got = NULL;
    ASSERT_TRUE(de_mpmc_try_push(&q, &ptr));
    ASSERT_TRUE(de_mpmc_try_pop(&q, &got));
    EXPECT_EQ(got, (void *)&h);
    de_mpmc_destroy(&q);
    EXPECT_EQ(h.live, (size_t)0);
}

/* ---------- blocking helpers ---------- */

typedef struct blocking {
    de_mpmc *q;
    de_sem *items;
    u64 per_producer;
    de_atomic_u64 *bitmap;
} blocking;

typedef struct blocking_arg {
    blocking *b;
    u32 id;
    u64 quota; /* consumers: how many items to take */
    u64 duplicates;
    u64 out_of_range;
} blocking_arg;

static i32 blocking_producer(void *user) {
    blocking_arg *p = (blocking_arg *)user;
    for (u64 s = 0; s < p->b->per_producer; s++) {
        u64 item = ((u64)p->id << 32) | s;
        while (!de_mpmc_push_post(p->b->q, p->b->items, &item))
            de_cpu_pause();
    }
    return 0;
}

static i32 blocking_consumer(void *user) {
    blocking_arg *c = (blocking_arg *)user;
    for (u64 i = 0; i < c->quota; i++) {
        u64 item;
        de_mpmc_pop_wait(c->b->q, c->b->items, &item);
        u32 pid = (u32)(item >> 32);
        u64 seq = item & 0xffffffffull;
        if (pid >= 2 || seq >= c->b->per_producer) {
            c->out_of_range++;
            continue;
        }
        u64 bit = (u64)pid * c->b->per_producer + seq;
        u64 mask = 1ull << (bit & 63);
        if (de_atomic_fetch_or_u64(&c->b->bitmap[bit >> 6], mask, DE_ATOMIC_RELAXED) & mask)
            c->duplicates++;
    }
    return 0;
}

TEST(mpmc, blocking_helper) {
    test_heap h;
    de_allocator a = th_allocator(&h);
    de_mpmc q;
    init_u64_queue(&q, &h, &a, 16);
    de_sem items;
    ASSERT_EQ(de_sem_init(&items, 0), DE_OK);

    /* Empty: the non-blocking and timed variants give up. */
    u64 out = 0;
    EXPECT_FALSE(de_mpmc_pop_try(&q, &items, &out));
    u64 t0 = de_time_now_ns();
    EXPECT_FALSE(de_mpmc_pop_wait_timeout(&q, &items, &out, 20000000ull)); /* 20 ms */
    EXPECT_GE(de_time_now_ns() - t0, 15000000ull);
    u64 v = 77;
    ASSERT_TRUE(de_mpmc_push_post(&q, &items, &v));
    ASSERT_TRUE(de_mpmc_pop_try(&q, &items, &out));
    EXPECT_EQ(out, 77ull);
    v = 78;
    ASSERT_TRUE(de_mpmc_push_post(&q, &items, &v));
    ASSERT_TRUE(de_mpmc_pop_wait_timeout(&q, &items, &out, 1000000000ull));
    EXPECT_EQ(out, 78ull);
    EXPECT_FALSE(de_mpmc_pop_try(&q, &items, &out));

    /* Full: push_post refuses and does not post. */
    for (u32 i = 0; i < 16; i++)
        ASSERT_TRUE(de_mpmc_push_post(&q, &items, &v));
    EXPECT_FALSE(de_mpmc_push_post(&q, &items, &v));
    for (u32 i = 0; i < 16; i++)
        ASSERT_TRUE(de_mpmc_pop_try(&q, &items, NULL));
    EXPECT_FALSE(de_mpmc_pop_try(&q, &items, &out));

    /* 2 producers x 2 consumers. The consumers start first and sleep on the semaphore. */
    blocking b;
    b.q = &q;
    b.items = &items;
    b.per_producer = 100000;
    const u64 total = 2 * b.per_producer;
    b.bitmap = (de_atomic_u64 *)calloc((size_t)((total + 63) / 64), sizeof(de_atomic_u64));
    ASSERT_NE(b.bitmap, NULL);
    blocking_arg pa[2], ca[2];
    memset(pa, 0, sizeof pa);
    memset(ca, 0, sizeof ca);
    de_thread *tp[2] = {0}, *tc[2] = {0};
    for (u32 i = 0; i < 2; i++) {
        ca[i].b = &b;
        ca[i].quota = total / 2;
        spawn(&tc[i], blocking_consumer, &ca[i]);
    }
    for (u32 i = 0; i < 2; i++) {
        pa[i].b = &b;
        pa[i].id = i;
        spawn(&tp[i], blocking_producer, &pa[i]);
    }
    for (u32 i = 0; i < 2; i++)
        EXPECT_EQ(de_thread_join(tp[i]), 0);
    for (u32 i = 0; i < 2; i++)
        EXPECT_EQ(de_thread_join(tc[i]), 0);

    u64 missing = 0;
    for (u64 bit = 0; bit < total; bit++) {
        if (!(de_atomic_load_u64(&b.bitmap[bit >> 6], DE_ATOMIC_RELAXED) & (1ull << (bit & 63))))
            missing++;
    }
    EXPECT_EQ(missing, 0ull);
    EXPECT_EQ(ca[0].duplicates + ca[1].duplicates, 0ull);
    EXPECT_EQ(ca[0].out_of_range + ca[1].out_of_range, 0ull);
    EXPECT_FALSE(de_mpmc_pop_try(&q, &items, &out)); /* semaphore and queue agree: empty */
    EXPECT_EQ(de_mpmc_size_approx(&q), 0u);

    free(b.bitmap);
    de_sem_destroy(&items);
    de_mpmc_destroy(&q);
    EXPECT_EQ(h.live, (size_t)0);
}

/* ---------- throughput ---------- */

/* Items per measurement; DE_QUEUE_BENCH_N overrides it (the closing numbers of DENG-152 used a
 * Release build and DE_QUEUE_BENCH_N=50000000). The test never fails on speed. */
static u64 bench_n(void) {
    const char *s = getenv("DE_QUEUE_BENCH_N");
    u64 n = s ? strtoull(s, NULL, 10) : 0;
    return n ? n : 2000000ull;
}

static double mops(u64 items, u64 ns) {
    return ns ? (double)items * 1000.0 / (double)ns : 0.0;
}

/* Bench threads do no bookkeeping beyond a running sum: each consumer pops a fixed quota, so no
 * shared counter or bitmap adds contention of its own. */
typedef struct bench_arg {
    de_mpmc *q;
    de_atomic_u32 *start;
    u64 count;
    u64 sum;
} bench_arg;

static i32 bench_producer(void *user) {
    bench_arg *b = (bench_arg *)user;
    while (de_atomic_load_u32(b->start, DE_ATOMIC_ACQUIRE) == 0)
        de_cpu_pause();
    for (u64 i = 1; i <= b->count; i++) {
        while (!de_mpmc_try_push(b->q, &i))
            de_cpu_pause();
    }
    return 0;
}

static i32 bench_consumer(void *user) {
    bench_arg *b = (bench_arg *)user;
    while (de_atomic_load_u32(b->start, DE_ATOMIC_ACQUIRE) == 0)
        de_cpu_pause();
    for (u64 got = 0; got < b->count;) {
        u64 v;
        if (de_mpmc_try_pop(b->q, &v)) {
            b->sum += v;
            got++;
        } else {
            de_cpu_pause();
        }
    }
    return 0;
}

/* Moves `per_thread * threads` items with `threads` producers and as many consumers; returns
 * the elapsed nanoseconds. */
static u64 bench_transfer(de_mpmc *q, u32 threads, u64 per_thread) {
    de_atomic_u32 start;
    de_atomic_init_u32(&start, 0);
    bench_arg pa[4], ca[4];
    de_thread *tp[4] = {0}, *tc[4] = {0};
    for (u32 i = 0; i < threads; i++) {
        pa[i] = (bench_arg){q, &start, per_thread, 0};
        ca[i] = (bench_arg){q, &start, per_thread, 0};
        spawn(&tc[i], bench_consumer, &ca[i]);
        spawn(&tp[i], bench_producer, &pa[i]);
    }
    u64 t0 = de_time_now_ns();
    de_atomic_store_u32(&start, 1, DE_ATOMIC_RELEASE);
    for (u32 i = 0; i < threads; i++) {
        (void)de_thread_join(tp[i]);
        (void)de_thread_join(tc[i]);
    }
    u64 t1 = de_time_now_ns();
    u64 sum = 0;
    for (u32 i = 0; i < threads; i++)
        sum += ca[i].sum;
    EXPECT_EQ(sum, (u64)threads * (per_thread * (per_thread + 1) / 2));
    return t1 - t0;
}

TEST(mpmc, bench) {
    test_heap h;
    de_allocator a = th_allocator(&h);
    de_mpmc q;
    init_u64_queue(&q, &h, &a, 1024);
    u64 n = bench_n();

    /* 1 thread: push and pop in bursts of 512 on the same thread. */
    u64 t0 = de_time_now_ns();
    u64 sum = 0;
    for (u64 i = 0; i < n; i += 512) {
        for (u64 j = 0; j < 512; j++) {
            u64 v = i + j;
            (void)de_mpmc_try_push(&q, &v);
        }
        for (u64 j = 0; j < 512; j++) {
            u64 v = 0;
            (void)de_mpmc_try_pop(&q, &v);
            sum += v;
        }
    }
    u64 t1 = de_time_now_ns();
    EXPECT_NE(sum, 0ull);

    u64 ns_2 = bench_transfer(&q, 1, n);
    u64 ns_8 = bench_transfer(&q, 4, n / 4);
    u64 n_8 = (n / 4) * 4;
    printf("[BENCH] mpmc u64, capacity 1024, %llu items: 1 thread %.1f M items/s, "
           "1P/1C %.1f M items/s, 4P/4C (8 threads) %.1f M items/s\n",
           (unsigned long long)n, mops(n, t1 - t0), mops(n, ns_2), mops(n_8, ns_8));
    de_mpmc_destroy(&q);
}
