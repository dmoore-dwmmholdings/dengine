/* Tests for the TLSF allocator and the default heap (DENG-149): a 1e6-operation random test
 * against a shadow table, coalescing, alignment, pool growth, in-place realloc, the heap under
 * 8 threads, large heap blocks, and a microbenchmark. de_test.h checks run only on the test's
 * thread, so worker threads record failures and the test thread checks them after the join. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif

#include "de_test.h"

#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ----------------------------------------------------------------------------- */

static u64 rng_next(u64 *s) {
    /* splitmix64 */
    u64 z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Mostly small sizes, some medium, a few large: the mix the engine heap sees. */
static size_t rng_size(u64 *s) {
    const u64 r = rng_next(s);
    const u32 pick = (u32)(r % 100);
    const u64 v = r >> 8;
    if (pick < 70)
        return 1 + (size_t)(v % 256);
    if (pick < 97)
        return 257 + (size_t)(v % 8000);
    return 8192 + (size_t)(v % 56000);
}

static size_t rng_align(u64 *s) {
    const u64 r = rng_next(s);
    if ((r & 7) != 0)
        return (r & 8) ? 0 : 16;         /* default alignment */
    return (size_t)16 << ((r >> 4) % 9); /* 16 .. 4096 */
}

/* A buffer of `n` bytes all equal to `b`, for memcmp checks of block contents. */
static u8 g_pattern[256][16 * 1024];
static bool g_pattern_ready;

static void pattern_init(void) {
    if (g_pattern_ready)
        return;
    for (int i = 0; i < 256; ++i)
        memset(g_pattern[i], i, sizeof g_pattern[i]);
    g_pattern_ready = true;
}

static bool block_has(const u8 *p, size_t n, u8 byte) {
    while (n > 0) {
        const size_t c = n < sizeof g_pattern[0] ? n : sizeof g_pattern[0];
        if (memcmp(p, g_pattern[byte], c) != 0)
            return false;
        p += c;
        n -= c;
    }
    return true;
}

typedef struct live_block {
    u8 *p;
    size_t size;
    size_t align;
    u8 byte;
} live_block;

static int cmp_live(const void *a, const void *b) {
    const uintptr_t x = (uintptr_t)((const live_block *)a)->p;
    const uintptr_t y = (uintptr_t)((const live_block *)b)->p;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Sorts the live blocks by address and checks that no two overlap. */
static bool no_overlap(live_block *blocks, size_t n, live_block *scratch) {
    size_t m = 0;
    for (size_t i = 0; i < n; ++i)
        if (blocks[i].p != NULL)
            scratch[m++] = blocks[i];
    qsort(scratch, m, sizeof *scratch, cmp_live);
    for (size_t i = 1; i < m; ++i)
        if (scratch[i - 1].p + scratch[i - 1].size > scratch[i].p)
            return false;
    return true;
}

static u64 env_u64(const char *name, u64 def) {
    const char *v = getenv(name);
    if (v == NULL || *v == '\0')
        return def;
    return (u64)strtoull(v, NULL, 10);
}

/* ---- random ops against a shadow table --------------------------------------------------- */

#define SHADOW_SLOTS 4096

TEST(tlsf, random_ops_shadow) {
    pattern_init();
    const size_t bytes = (size_t)96 << 20;
    void *mem = malloc(bytes);
    ASSERT_TRUE(mem != NULL);
    de_tlsf *t = NULL;
    ASSERT_EQ(de_tlsf_create(mem, bytes, &t), DE_OK);
    de_allocator a = de_tlsf_allocator(t);

    de_tlsf_info empty;
    ASSERT_EQ(de_tlsf_check(t, &empty), DE_OK);
    EXPECT_EQ(empty.free_bytes, empty.capacity);
    EXPECT_EQ(empty.free_blocks, 1u);
    EXPECT_EQ(empty.pool_count, 1u);
    EXPECT_GT(empty.capacity, bytes - de_tlsf_control_size() - 128);

    live_block *live = (live_block *)calloc(SHADOW_SLOTS, sizeof *live);
    live_block *scratch = (live_block *)calloc(SHADOW_SLOTS, sizeof *scratch);
    ASSERT_TRUE(live != NULL && scratch != NULL);

    u64 seed = 0xDE149u;
    const u64 ops = env_u64("DE_TLSF_OPS", 1000000);
    u64 allocs = 0, frees = 0, reallocs = 0;
    for (u64 op = 0; op < ops; ++op) {
        const u64 r = rng_next(&seed);
        live_block *s = &live[r % SHADOW_SLOTS];
        if (s->p != NULL && ((r >> 40) & 7) == 0) {
            /* realloc: contents up to the smaller size must survive */
            ASSERT_TRUE(block_has(s->p, s->size, s->byte));
            const size_t ns = rng_size(&seed);
            u8 *q = (u8 *)DE_REALLOC(&a, s->p, s->size, ns, s->align, DE_MEM_GENERAL);
            ASSERT_TRUE(q != NULL);
            const size_t keep = s->size < ns ? s->size : ns;
            ASSERT_TRUE(block_has(q, keep, s->byte));
            ASSERT_EQ((uintptr_t)q & ((s->align ? s->align : 16) - 1), 0u);
            memset(q, s->byte, ns);
            s->p = q;
            s->size = ns;
            reallocs++;
        } else if (s->p != NULL) {
            ASSERT_TRUE(block_has(s->p, s->size, s->byte));
            DE_FREE(&a, s->p, s->size);
            s->p = NULL;
            frees++;
        } else {
            const size_t size = rng_size(&seed);
            const size_t align = rng_align(&seed);
            u8 *p = (u8 *)DE_ALLOC(&a, size, align, DE_MEM_GENERAL);
            ASSERT_TRUE(p != NULL);
            ASSERT_EQ((uintptr_t)p & ((align ? align : 16) - 1), 0u);
            ASSERT_TRUE(p >= (u8 *)mem && p + size <= (u8 *)mem + bytes);
            s->p = p;
            s->size = size;
            s->align = align;
            s->byte = (u8)(r >> 32);
            memset(p, s->byte, size);
            allocs++;
        }
        if ((op + 1) % 100000 == 0) {
            ASSERT_TRUE(no_overlap(live, SHADOW_SLOTS, scratch));
            de_tlsf_info info;
            ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
            size_t used = 0;
            u32 count = 0;
            for (size_t i = 0; i < SHADOW_SLOTS; ++i) {
                if (live[i].p != NULL) {
                    used += live[i].size;
                    count++;
                }
            }
            EXPECT_EQ(info.used_blocks, count);
            EXPECT_GE(info.used_bytes, used);
        }
    }
    EXPECT_GT(allocs, ops / 3);
    EXPECT_GT(frees, ops / 4);
    EXPECT_GT(reallocs, 0u);

    for (size_t i = 0; i < SHADOW_SLOTS; ++i) {
        if (live[i].p != NULL) {
            ASSERT_TRUE(block_has(live[i].p, live[i].size, live[i].byte));
            DE_FREE(&a, live[i].p, live[i].size);
        }
    }
    de_tlsf_info end;
    ASSERT_EQ(de_tlsf_check(t, &end), DE_OK);
    EXPECT_EQ(end.free_bytes, end.capacity); /* free-list total == pool size */
    EXPECT_EQ(end.capacity, empty.capacity);
    EXPECT_EQ(end.free_blocks, 1u);
    EXPECT_EQ(end.used_blocks, 0u);
    EXPECT_EQ(end.used_bytes, 0u);
    free(scratch);
    free(live);
    free(mem);
}

/* ---- coalescing -------------------------------------------------------------------------- */

TEST(tlsf, coalesce) {
    static DE_ALIGN(16) u8 mem[1 << 16];
    de_tlsf *t = NULL;
    ASSERT_EQ(de_tlsf_create(mem, sizeof mem, &t), DE_OK);
    de_allocator a = de_tlsf_allocator(t);
    de_tlsf_info info;
    ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
    const size_t cap = info.capacity;

    /* Three neighbours a | b | c | rest. */
    void *pa = DE_ALLOC(&a, 1000, 16, 0);
    void *pb = DE_ALLOC(&a, 2000, 16, 0);
    void *pc = DE_ALLOC(&a, 3000, 16, 0);
    ASSERT_TRUE(pa && pb && pc);
    ASSERT_TRUE((u8 *)pa < (u8 *)pb && (u8 *)pb < (u8 *)pc);
    ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
    EXPECT_EQ(info.used_blocks, 3u);
    EXPECT_EQ(info.free_blocks, 1u);

    DE_FREE(&a, pa, 1000); /* a alone: free blocks a, rest */
    ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
    EXPECT_EQ(info.free_blocks, 2u);
    DE_FREE(&a, pc, 3000); /* c merges with rest */
    ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
    EXPECT_EQ(info.free_blocks, 2u);
    DE_FREE(&a, pb, 2000); /* b merges with both neighbours: one block again */
    ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
    EXPECT_EQ(info.free_blocks, 1u);
    EXPECT_EQ(info.free_bytes, cap);
    EXPECT_EQ(info.largest_free, cap);

    /* The merged block serves a request of nearly the whole pool. A request is rounded up to
     * the next size-class boundary (good fit), so exactly `cap` is not guaranteed to fit. */
    const size_t most = cap - cap / 16;
    void *all = DE_ALLOC(&a, most, 16, 0);
    ASSERT_TRUE(all != NULL);
    EXPECT_EQ(all, pa);
    de_error_clear();
    EXPECT_TRUE(DE_ALLOC(&a, cap, 16, 0) == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    DE_FREE(&a, all, most);

    /* Many small blocks freed in a scattered order come back as one block. */
    void *ptrs[200];
    for (int i = 0; i < 200; ++i) {
        ptrs[i] = DE_ALLOC(&a, (size_t)(16 + (i % 32) * 8), 16, 0);
        ASSERT_TRUE(ptrs[i] != NULL);
    }
    for (int i = 0; i < 200; i += 2)
        DE_FREE(&a, ptrs[i], 0);
    ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
    EXPECT_EQ(info.free_blocks, 101u); /* 100 holes and the rest of the pool */
    for (int i = 1; i < 200; i += 2)
        DE_FREE(&a, ptrs[i], 0);
    ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
    EXPECT_EQ(info.free_blocks, 1u);
    EXPECT_EQ(info.free_bytes, cap);
}

/* ---- alignment --------------------------------------------------------------------------- */

TEST(tlsf, alignment) {
    const size_t bytes = (size_t)8 << 20;
    u8 *mem = (u8 *)malloc(bytes + 7);
    ASSERT_TRUE(mem != NULL);
    de_tlsf *t = NULL;
    /* An odd start address: create aligns inside. */
    ASSERT_EQ(de_tlsf_create(mem + 7, bytes, &t), DE_OK);
    de_allocator a = de_tlsf_allocator(t);
    void *keep[64];
    int nkeep = 0;
    for (size_t align = 1; align <= 4096; align <<= 1) {
        static const size_t sizes[] = {0, 1, 15, 16, 17, 100, 511, 512, 513, 4095, 4096, 70000};
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i) {
            u8 *p = (u8 *)DE_ALLOC(&a, sizes[i], align, 0);
            ASSERT_TRUE(p != NULL);
            const size_t eff = align < 16 ? 16 : align;
            ASSERT_EQ((uintptr_t)p & (eff - 1), 0u);
            memset(p, 0xAB, sizes[i]);
            if ((i & 3) == 0 && nkeep < 64)
                keep[nkeep++] = p; /* keep some, so later blocks land between them */
            else
                DE_FREE(&a, p, sizes[i]);
            ASSERT_EQ(de_tlsf_check(t, NULL), DE_OK);
        }
    }
    /* Alignment larger than 4 KiB works too. */
    void *big = DE_ALLOC(&a, 100, (size_t)1 << 16, 0);
    ASSERT_TRUE(big != NULL);
    EXPECT_EQ((uintptr_t)big & 0xFFFFu, 0u);
    DE_FREE(&a, big, 100);

    /* A non-power-of-two alignment fails. */
    de_error_clear();
    EXPECT_TRUE(DE_ALLOC(&a, 16, 24, 0) == NULL);
    for (int i = 0; i < nkeep; ++i)
        DE_FREE(&a, keep[i], 0);
    de_tlsf_info info;
    ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
    EXPECT_EQ(info.free_bytes, info.capacity);
    EXPECT_EQ(info.free_blocks, 1u);
    free(mem);
}

/* ---- create arguments, add_pool ---------------------------------------------------------- */

TEST(tlsf, add_pool) {
    de_tlsf *t = NULL;
    static DE_ALIGN(16) u8 tiny[64];
    EXPECT_EQ(de_tlsf_create(tiny, sizeof tiny, &t), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_tlsf_create(NULL, 1 << 20, &t), DE_ERR_INVALID_ARGUMENT);

    const size_t first = de_tlsf_control_size() + 4096;
    u8 *mem0 = (u8 *)malloc(first);
    u8 *mem1 = (u8 *)malloc((size_t)1 << 20);
    u8 *mem2 = (u8 *)malloc(4096);
    ASSERT_TRUE(mem0 && mem1 && mem2);
    ASSERT_EQ(de_tlsf_create(mem0, first, &t), DE_OK);
    de_allocator a = de_tlsf_allocator(t);

    /* Fill the first pool. */
    void *small[512];
    int n = 0;
    while (n < 512 && (small[n] = DE_ALLOC(&a, 64, 16, 0)) != NULL)
        n++;
    ASSERT_GT(n, 10);
    ASSERT_LT(n, 512);
    EXPECT_TRUE(DE_ALLOC(&a, 100000, 16, 0) == NULL);

    EXPECT_EQ(de_tlsf_add_pool(t, mem1, 32), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_tlsf_add_pool(t, mem1, (size_t)1 << 20), DE_OK);
    u8 *p = (u8 *)DE_ALLOC(&a, 100000, 16, 0);
    ASSERT_TRUE(p != NULL);
    EXPECT_TRUE(p >= mem1 && p + 100000 <= mem1 + ((size_t)1 << 20));
    /* A block never spans pools: larger than any pool fails. */
    EXPECT_TRUE(DE_ALLOC(&a, (size_t)2 << 20, 16, 0) == NULL);
    ASSERT_EQ(de_tlsf_add_pool(t, mem2 + 3, 4093), DE_OK); /* unaligned start */

    de_tlsf_info info;
    ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
    EXPECT_EQ(info.pool_count, 3u);
    DE_FREE(&a, p, 100000);
    for (int i = 0; i < n; ++i)
        DE_FREE(&a, small[i], 64);
    ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
    EXPECT_EQ(info.free_blocks, 3u); /* one per pool; pools never merge */
    EXPECT_EQ(info.free_bytes, info.capacity);
    free(mem2);
    free(mem1);
    free(mem0);
}

/* ---- realloc ----------------------------------------------------------------------------- */

TEST(tlsf, realloc_in_place) {
    static DE_ALIGN(16) u8 mem[1 << 18];
    de_tlsf *t = NULL;
    ASSERT_EQ(de_tlsf_create(mem, sizeof mem, &t), DE_OK);
    de_allocator a = de_tlsf_allocator(t);
    u8 *p = (u8 *)DE_ALLOC(&a, 100, 16, 0);
    ASSERT_TRUE(p != NULL);
    for (int i = 0; i < 100; ++i)
        p[i] = (u8)i;
    /* The block is followed by the free rest of the pool: growth stays in place. */
    u8 *q = (u8 *)DE_REALLOC(&a, p, 100, 5000, 16, 0);
    EXPECT_EQ(q, p);
    q = (u8 *)DE_REALLOC(&a, q, 5000, 40, 16, 0);
    EXPECT_EQ(q, p);
    for (int i = 0; i < 40; ++i)
        ASSERT_EQ(q[i], (u8)i);
    /* Blocked by a neighbour: it moves and keeps the contents. */
    void *blocker = DE_ALLOC(&a, 16, 16, 0);
    ASSERT_TRUE(blocker != NULL);
    u8 *r = (u8 *)DE_REALLOC(&a, q, 40, 2000, 16, 0);
    ASSERT_TRUE(r != NULL);
    EXPECT_NE(r, q);
    for (int i = 0; i < 40; ++i)
        ASSERT_EQ(r[i], (u8)i);
    /* A stricter alignment the block does not have moves it too. */
    u8 *s = (u8 *)DE_REALLOC(&a, r, 2000, 2000, 4096, 0);
    ASSERT_TRUE(s != NULL);
    EXPECT_EQ((uintptr_t)s & 4095u, 0u);
    /* realloc of NULL allocates. */
    void *n = DE_REALLOC(&a, NULL, 0, 64, 16, 0);
    ASSERT_TRUE(n != NULL);
    DE_FREE(&a, n, 64);
    DE_FREE(&a, s, 2000);
    DE_FREE(&a, blocker, 16);
    de_tlsf_info info;
    ASSERT_EQ(de_tlsf_check(t, &info), DE_OK);
    EXPECT_EQ(info.free_blocks, 1u);
    EXPECT_EQ(info.free_bytes, info.capacity);
}

/* ---- the default heap -------------------------------------------------------------------- */

#define HEAP_THREADS 8
#define HEAP_SLOTS   512

typedef struct heap_worker {
    u64 seed;
    u64 ops;
    u32 id;
    u64 failures; /* checked by the test thread */
    u64 larges;
} heap_worker;

/* Bytes of a block that the heap workers fill and check: large blocks only up to the size of
 * the pattern buffer, so the test stays fast. */
static size_t fill_len(size_t size) {
    return size < sizeof g_pattern[0] ? size : sizeof g_pattern[0];
}

static i32 heap_worker_fn(void *user) {
    heap_worker *w = (heap_worker *)user;
    const de_allocator *h = de_heap_allocator();
    live_block *live = (live_block *)calloc(HEAP_SLOTS, sizeof *live);
    if (live == NULL) {
        w->failures++;
        return 1;
    }
    for (u64 op = 0; op < w->ops; ++op) {
        const u64 r = rng_next(&w->seed);
        live_block *s = &live[r % HEAP_SLOTS];
        if (s->p != NULL) {
            if (!block_has(s->p, fill_len(s->size), s->byte))
                w->failures++;
            if (((r >> 40) & 15) == 0) {
                const size_t ns = rng_size(&w->seed);
                u8 *q = (u8 *)DE_REALLOC(h, s->p, s->size, ns, 16, DE_MEM_GENERAL);
                if (q == NULL) {
                    w->failures++;
                    continue;
                }
                const size_t keep = s->size < ns ? s->size : ns;
                if (!block_has(q, fill_len(keep), s->byte))
                    w->failures++;
                memset(q, s->byte, fill_len(ns));
                s->p = q;
                s->size = ns;
            } else {
                DE_FREE(h, s->p, s->size);
                s->p = NULL;
            }
        } else {
            size_t size = rng_size(&w->seed);
            if (((r >> 20) & 1023) == 0) {
                size = DE_HEAP_LARGE_THRESHOLD + (size_t)(r % 300000); /* the large path */
                w->larges++;
            }
            const size_t align = rng_align(&w->seed);
            u8 *p = (u8 *)DE_ALLOC(h, size, align, DE_MEM_GENERAL);
            if (p == NULL || ((uintptr_t)p & ((align ? align : 16) - 1)) != 0) {
                w->failures++;
                continue;
            }
            s->p = p;
            s->size = size;
            s->byte = (u8)(w->id * 31 + (r >> 32));
            memset(p, s->byte, fill_len(size));
        }
    }
    for (size_t i = 0; i < HEAP_SLOTS; ++i) {
        if (live[i].p != NULL) {
            if (!block_has(live[i].p, fill_len(live[i].size), live[i].byte))
                w->failures++;
            DE_FREE(h, live[i].p, live[i].size);
        }
    }
    free(live);
    return 0;
}

TEST(tlsf, heap_threads) {
    pattern_init();
    const de_heap_info before = de_heap_get_info();
    heap_worker w[HEAP_THREADS];
    de_thread *th[HEAP_THREADS];
    const u64 ops = env_u64("DE_HEAP_OPS", 200000);
    for (u32 i = 0; i < HEAP_THREADS; ++i) {
        memset(&w[i], 0, sizeof w[i]);
        w[i].seed = 0x1234567ull * (i + 1);
        w[i].ops = ops;
        w[i].id = i;
        ASSERT_EQ(de_thread_create(heap_worker_fn, &w[i], "heap_test", DE_THREAD_NORMAL, 0, &th[i]),
                  DE_OK);
    }
    u64 larges = 0;
    for (u32 i = 0; i < HEAP_THREADS; ++i) {
        EXPECT_EQ(de_thread_join(th[i]), 0);
        EXPECT_EQ(w[i].failures, 0u);
        larges += w[i].larges;
    }
    EXPECT_GT(larges, 0u);
    const de_heap_info after = de_heap_get_info();
    /* 8 threads x 512 slots x ~1.8 KiB average outgrow the first 4 MiB pool. */
    EXPECT_GT(after.pool_count, 1u);
    EXPECT_GE(after.pool_count, before.pool_count);
    EXPECT_GT(after.committed, (size_t)4 << 20);
    EXPECT_EQ(after.large_count, before.large_count);
    de_tlsf_info info;
    ASSERT_EQ(de_heap_check(&info), DE_OK);
    EXPECT_EQ(info.pool_count, after.pool_count);
}

TEST(tlsf, heap_large_blocks) {
    const de_allocator *h = de_heap_allocator();
    const de_heap_info before = de_heap_get_info();
    u8 *p = (u8 *)DE_ALLOC(h, (size_t)3 << 20, 4096, DE_MEM_ASSET);
    ASSERT_TRUE(p != NULL);
    EXPECT_EQ((uintptr_t)p & 4095u, 0u);
    memset(p, 0x5A, (size_t)3 << 20);
    de_heap_info mid = de_heap_get_info();
    EXPECT_EQ(mid.large_count, before.large_count + 1);
    EXPECT_GE(mid.large_bytes, before.large_bytes + ((size_t)3 << 20));

    /* Shrinking a large block that stays large is in place. */
    u8 *q = (u8 *)DE_REALLOC(h, p, (size_t)3 << 20, (size_t)2 << 20, 16, DE_MEM_ASSET);
    EXPECT_EQ(q, p);
    /* Shrinking to a small size moves it into the TLSF. */
    u8 *s = (u8 *)DE_REALLOC(h, q, (size_t)2 << 20, 1000, 16, DE_MEM_ASSET);
    ASSERT_TRUE(s != NULL);
    for (int i = 0; i < 1000; ++i)
        ASSERT_EQ(s[i], 0x5A);
    EXPECT_EQ(de_heap_get_info().large_count, before.large_count);
    /* Growing a small block past the threshold moves it out again. */
    u8 *g = (u8 *)DE_REALLOC(h, s, 1000, (size_t)5 << 20, 16, DE_MEM_ASSET);
    ASSERT_TRUE(g != NULL);
    for (int i = 0; i < 1000; ++i)
        ASSERT_EQ(g[i], 0x5A);
    EXPECT_EQ(de_heap_get_info().large_count, before.large_count + 1);
    DE_FREE(h, g, (size_t)5 << 20);

    /* A big alignment takes the large path whatever the size. */
    void *al = DE_ALLOC(h, 64, (size_t)1 << 20, DE_MEM_GENERAL);
    ASSERT_TRUE(al != NULL);
    EXPECT_EQ((uintptr_t)al & (((size_t)1 << 20) - 1), 0u);
    DE_FREE(h, al, 64);

    const de_heap_info end = de_heap_get_info();
    EXPECT_EQ(end.large_count, before.large_count);
    EXPECT_EQ(end.large_bytes, before.large_bytes);

    de_error_clear();
    EXPECT_TRUE(DE_ALLOC(h, 16, 3, 0) == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    DE_FREE(h, NULL, 0);
}

/* ---- microbenchmark ---------------------------------------------------------------------- */

/* Times `ops` alloc/free operations with random sizes on a TLSF of `bytes` and reports the mean
 * and the slowest single operation. Never fails on speed. */
static void bench_one(size_t bytes, u64 ops, const char *label) {
    u8 *mem = (u8 *)malloc(bytes);
    if (mem == NULL)
        return;
    de_tlsf *t = NULL;
    if (de_tlsf_create(mem, bytes, &t) != DE_OK) {
        free(mem);
        return;
    }
    de_allocator a = de_tlsf_allocator(t);
    enum { SLOTS = 8192 };
    void **slot = (void **)calloc(SLOTS, sizeof *slot);
    size_t *size = (size_t *)calloc(SLOTS, sizeof *size);
    if (slot == NULL || size == NULL) {
        free(size);
        free(slot);
        free(mem);
        return;
    }
    u64 seed = 42, worst = 0;
    /* One untimed pass first, so page faults stay out of the numbers. */
    for (u64 i = 0; i < ops; ++i) {
        const u64 r = rng_next(&seed);
        const u32 k = (u32)(r % SLOTS);
        if (slot[k] != NULL) {
            DE_FREE(&a, slot[k], size[k]);
            slot[k] = NULL;
        } else {
            size[k] = 16 + (size_t)((r >> 16) % 2048);
            slot[k] = DE_ALLOC(&a, size[k], 16, 0);
        }
    }
    const u64 t0 = de_time_now_ns();
    for (u64 i = 0; i < ops; ++i) {
        const u64 r = rng_next(&seed);
        const u32 k = (u32)(r % SLOTS);
        const u64 s0 = (i & 63) == 0 ? de_time_now_ns() : 0;
        if (slot[k] != NULL) {
            DE_FREE(&a, slot[k], size[k]);
            slot[k] = NULL;
        } else {
            size[k] = 16 + (size_t)((r >> 16) % 2048);
            slot[k] = DE_ALLOC(&a, size[k], 16, 0);
        }
        if ((i & 63) == 0) {
            const u64 d = de_time_now_ns() - s0;
            if (d > worst)
                worst = d;
        }
    }
    const u64 t1 = de_time_now_ns();
    printf("[BENCH] tlsf %s: %llu ops in %.2f ms, %.1f ns/op mean, slowest sampled op %llu ns\n",
           label, (unsigned long long)ops, (double)(t1 - t0) / 1e6, (double)(t1 - t0) / (double)ops,
           (unsigned long long)worst);
    free(size);
    free(slot);
    free(mem);
}

TEST(tlsf, bench) {
    const u64 ops = env_u64("DE_TLSF_BENCH_N", 1000000);
    /* The same workload on a small and a 32x larger heap: the cost per op must not scale with
     * the heap size (no loop over blocks). */
    bench_one((size_t)32 << 20, ops, "32 MiB heap");
    bench_one((size_t)1 << 30, ops, "1 GiB heap");

    const de_allocator *h = de_heap_allocator();
    void *warm = DE_ALLOC(h, 64, 16, 0);
    DE_FREE(h, warm, 64);
    u64 seed = 7;
    enum { SLOTS = 8192 };
    void **slot = (void **)calloc(SLOTS, sizeof *slot);
    size_t *size = (size_t *)calloc(SLOTS, sizeof *size);
    ASSERT_TRUE(slot && size);
    const u64 t0 = de_time_now_ns();
    for (u64 i = 0; i < ops; ++i) {
        const u64 r = rng_next(&seed);
        const u32 k = (u32)(r % SLOTS);
        if (slot[k] != NULL) {
            DE_FREE(h, slot[k], size[k]);
            slot[k] = NULL;
        } else {
            size[k] = 16 + (size_t)((r >> 16) % 2048);
            slot[k] = DE_ALLOC(h, size[k], 16, 0);
        }
    }
    const u64 t1 = de_time_now_ns();
    for (u32 k = 0; k < SLOTS; ++k)
        DE_FREE(h, slot[k], size[k]);
    printf("[BENCH] de_heap_allocator (mutex%s): %llu ops, %.1f ns/op mean\n",
           DE_MEM_TRACKING ? " + tracking" : "", (unsigned long long)ops,
           (double)(t1 - t0) / (double)ops);
    free(size);
    free(slot);
}
