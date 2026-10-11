/* Tests for the fixed-size pool (DENG-149): basics and defaults, alignment, a 1e6-operation
 * random alloc/free sequence with overlap checks, backing-allocator failures, and a thread-safe
 * stress with 8 threads. Worker threads record failures; the test thread checks them. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif

#include "de_test.h"

#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static u64 rng_next(u64 *s) {
    u64 z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static u64 env_u64(const char *name, u64 def) {
    const char *v = getenv(name);
    if (v == NULL || *v == '\0')
        return def;
    return (u64)strtoull(v, NULL, 10);
}

/* Writes `tag` over every u32 of a block / checks that it is still there. */
static void stamp(void *b, size_t size, u32 tag) {
    u8 *p = (u8 *)b;
    for (size_t i = 0; i + sizeof tag <= size; i += sizeof tag)
        memcpy(p + i, &tag, sizeof tag);
}

static bool stamped(const void *b, size_t size, u32 tag) {
    const u8 *p = (const u8 *)b;
    for (size_t i = 0; i + sizeof tag <= size; i += sizeof tag) {
        u32 v;
        memcpy(&v, p + i, sizeof v);
        if (v != tag)
            return false;
    }
    return true;
}

TEST(pool, basic) {
    de_pool *p = NULL;
    de_pool_desc d = {0};
    EXPECT_EQ(de_pool_create(&d, &p), DE_ERR_INVALID_ARGUMENT); /* block_size 0 */
    d.block_size = 24;
    d.block_align = 12;
    EXPECT_EQ(de_pool_create(&d, &p), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_pool_create(NULL, &p), DE_ERR_INVALID_ARGUMENT);

    d.block_align = 0;
    d.blocks_per_page = 64;
    ASSERT_EQ(de_pool_create(&d, &p), DE_OK);
    de_pool_info info = de_pool_get_info(p);
    EXPECT_EQ(info.block_stride, 32u); /* 24 rounded up to the default 16 */
    EXPECT_EQ(info.block_align, 16u);
    EXPECT_EQ(info.page_count, 0u);

    enum { N = 1000 };
    void *b[N];
    for (int i = 0; i < N; ++i) {
        b[i] = de_pool_alloc(p);
        ASSERT_TRUE(b[i] != NULL);
        ASSERT_EQ((uintptr_t)b[i] & 15u, 0u);
        stamp(b[i], 24, (u32)i);
    }
    info = de_pool_get_info(p);
    EXPECT_EQ(info.blocks_live, (u64)N);
    EXPECT_EQ(info.page_count, (N + 63) / 64u);
    for (int i = 0; i < N; ++i)
        ASSERT_TRUE(stamped(b[i], 24, (u32)i));

    /* Freed blocks are reused, last freed first. */
    de_pool_free(p, b[10]);
    de_pool_free(p, b[20]);
    EXPECT_EQ(de_pool_alloc(p), b[20]);
    EXPECT_EQ(de_pool_alloc(p), b[10]);
    for (int i = 0; i < N; ++i)
        de_pool_free(p, b[i]);
    de_pool_free(p, NULL);
    info = de_pool_get_info(p);
    EXPECT_EQ(info.blocks_live, 0u);
    EXPECT_EQ(info.page_count, (N + 63) / 64u); /* pages stay until destroy */
    /* Reallocating N blocks needs no new page. */
    for (int i = 0; i < N; ++i)
        b[i] = de_pool_alloc(p);
    EXPECT_EQ(de_pool_get_info(p).page_count, (N + 63) / 64u);
    de_pool_destroy(p); /* with live blocks */
    de_pool_destroy(NULL);

    /* Defaults: pages of about 64 KiB; tiny blocks hold at least a pointer. */
    de_pool_desc d2 = {0};
    d2.block_size = 1;
    d2.block_align = 1;
    ASSERT_EQ(de_pool_create(&d2, &p), DE_OK);
    info = de_pool_get_info(p);
    EXPECT_EQ(info.block_stride, sizeof(void *));
    EXPECT_EQ(info.blocks_per_page, (u32)(65536 / sizeof(void *)));
    de_pool_destroy(p);

    de_pool_desc d3 = {0};
    d3.block_size = (size_t)1 << 20; /* bigger than the default page */
    ASSERT_EQ(de_pool_create(&d3, &p), DE_OK);
    EXPECT_EQ(de_pool_get_info(p).blocks_per_page, 1u);
    void *big = de_pool_alloc(p);
    ASSERT_TRUE(big != NULL);
    memset(big, 1, (size_t)1 << 20);
    de_pool_destroy(p);
}

TEST(pool, alignment) {
    for (size_t align = 1; align <= 4096; align <<= 1) {
        static const size_t sizes[] = {1, 7, 8, 24, 100, 4096, 5000};
        for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
            de_pool_desc d = {0};
            d.block_size = sizes[s];
            d.block_align = align;
            d.blocks_per_page = 5;
            de_pool *p = NULL;
            ASSERT_EQ(de_pool_create(&d, &p), DE_OK);
            const size_t eff = align < sizeof(void *) ? sizeof(void *) : align;
            void *b[23];
            for (int i = 0; i < 23; ++i) {
                b[i] = de_pool_alloc(p);
                ASSERT_TRUE(b[i] != NULL);
                ASSERT_EQ((uintptr_t)b[i] & (eff - 1), 0u);
                memset(b[i], i, sizes[s]);
            }
            for (int i = 0; i < 23; ++i) {
                const u8 *q = (const u8 *)b[i];
                ASSERT_EQ(q[0], (u8)i);
                ASSERT_EQ(q[sizes[s] - 1], (u8)i);
            }
            de_pool_destroy(p);
        }
    }
}

/* 1e6 random allocs and frees against a shadow table: every block aligned, none overlapping
 * (checked by stamping every live block's whole contents and re-reading it before its free),
 * and a live block never handed out twice. */
TEST(pool, random_ops) {
    de_pool_desc d = {0};
    d.block_size = 40;
    d.block_align = 32;
    d.blocks_per_page = 100;
    de_pool *p = NULL;
    ASSERT_EQ(de_pool_create(&d, &p), DE_OK);
    const size_t stride = de_pool_get_info(p).block_stride;
    EXPECT_EQ(stride, 64u);

    enum { SLOTS = 3000 };
    void **slot = (void **)calloc(SLOTS, sizeof *slot);
    ASSERT_TRUE(slot != NULL);
    u64 seed = 99;
    const u64 ops = env_u64("DE_POOL_OPS", 1000000);
    u32 live = 0, max_live = 0;
    for (u64 op = 0; op < ops; ++op) {
        const u64 r = rng_next(&seed);
        const u32 k = (u32)(r % SLOTS);
        if (slot[k] != NULL) {
            ASSERT_TRUE(stamped(slot[k], stride, k));
            de_pool_free(p, slot[k]);
            slot[k] = NULL;
            live--;
        } else {
            void *b = de_pool_alloc(p);
            ASSERT_TRUE(b != NULL);
            ASSERT_EQ((uintptr_t)b & 31u, 0u);
            stamp(b, stride, k); /* the whole stride: an overlap would break a neighbour */
            slot[k] = b;
            if (++live > max_live)
                max_live = live;
        }
    }
    for (u32 k = 0; k < SLOTS; ++k) {
        if (slot[k] != NULL)
            ASSERT_TRUE(stamped(slot[k], stride, k));
    }
    const de_pool_info info = de_pool_get_info(p);
    EXPECT_EQ(info.blocks_live, (u64)live);
    /* Pages only grow to the peak number of live blocks. */
    EXPECT_EQ(info.page_count, (max_live + 99) / 100u);
    free(slot);
    de_pool_destroy(p);
}

TEST(pool, backing_failure) {
    de_pool_desc d = {0};
    d.block_size = 16;
    d.backing = de_allocator_null();
    de_pool *p = NULL;
    de_error_clear();
    EXPECT_EQ(de_pool_create(&d, &p), DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(p == NULL);

    /* The pool itself and one page succeed, the second page fails. */
    de_tracking_allocator tr;
    de_allocator ta = de_tracking_allocator_init(&tr, NULL, DE_MEM_GENERAL);
    de_tracking_allocator_fail_after(&tr, 2);
    d.backing = &ta;
    d.blocks_per_page = 4;
    ASSERT_EQ(de_pool_create(&d, &p), DE_OK);
    for (int i = 0; i < 4; ++i)
        ASSERT_TRUE(de_pool_alloc(p) != NULL);
    de_error_clear();
    EXPECT_TRUE(de_pool_alloc(p) == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    de_tracking_allocator_fail_after(&tr, -1);
    EXPECT_TRUE(de_pool_alloc(p) != NULL);
    de_pool_destroy(p);
    /* Everything went back to the backing allocator. */
    const de_tracking_stats st = de_tracking_allocator_stats(&tr);
    EXPECT_EQ(st.alloc_count, 3u);
    EXPECT_EQ(st.free_count, 3u);
    EXPECT_EQ(st.bytes_live, 0u);
    EXPECT_EQ(st.fail_count, 1u);
}

/* ---- thread-safe stress ------------------------------------------------------------------ */

#define POOL_THREADS 8

typedef struct pool_worker {
    de_pool *pool;
    u32 id;
    u64 ops;
    u64 failures;
} pool_worker;

static i32 pool_worker_fn(void *user) {
    pool_worker *w = (pool_worker *)user;
    enum { SLOTS = 256 };
    void *slot[SLOTS];
    memset(slot, 0, sizeof slot);
    u64 seed = 0xABCDu + w->id;
    for (u64 op = 0; op < w->ops; ++op) {
        const u64 r = rng_next(&seed);
        const u32 k = (u32)(r % SLOTS);
        const u32 tag = (w->id << 16) | k;
        if (slot[k] != NULL) {
            if (!stamped(slot[k], 48, tag))
                w->failures++;
            de_pool_free(w->pool, slot[k]);
            slot[k] = NULL;
        } else {
            void *b = de_pool_alloc(w->pool);
            if (b == NULL || ((uintptr_t)b & 15u) != 0) {
                w->failures++;
                continue;
            }
            stamp(b, 48, tag);
            slot[k] = b;
        }
    }
    for (u32 k = 0; k < SLOTS; ++k) {
        if (slot[k] != NULL) {
            if (!stamped(slot[k], 48, (w->id << 16) | k))
                w->failures++;
            de_pool_free(w->pool, slot[k]);
        }
    }
    return 0;
}

TEST(pool, thread_safe_stress) {
    de_pool_desc d = {0};
    d.block_size = 48;
    d.blocks_per_page = 32; /* small pages: growth races with alloc and free */
    d.thread_safe = true;
    de_pool *p = NULL;
    ASSERT_EQ(de_pool_create(&d, &p), DE_OK);
    pool_worker w[POOL_THREADS];
    de_thread *th[POOL_THREADS];
    const u64 ops = env_u64("DE_POOL_THREAD_OPS", 200000);
    for (u32 i = 0; i < POOL_THREADS; ++i) {
        w[i].pool = p;
        w[i].id = i;
        w[i].ops = ops;
        w[i].failures = 0;
        ASSERT_EQ(de_thread_create(pool_worker_fn, &w[i], "pool_test", DE_THREAD_NORMAL, 0, &th[i]),
                  DE_OK);
    }
    for (u32 i = 0; i < POOL_THREADS; ++i) {
        EXPECT_EQ(de_thread_join(th[i]), 0);
        EXPECT_EQ(w[i].failures, 0u);
    }
    const de_pool_info info = de_pool_get_info(p);
    EXPECT_EQ(info.blocks_live, 0u);
    EXPECT_LE(info.page_count, (POOL_THREADS * 256 + 31) / 32u);
    de_pool_destroy(p);
}
