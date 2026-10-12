/* test_handle_pool.c — generational slot pools behind the de_gpu handles (DENG-281). */
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "gpu/handle_pool.h"

#include <stdint.h>
#include <string.h>

static const char *errmsg(void) {
    const de_error *e = de_error_last();
    return e ? e->message : "";
}

typedef struct pool_fx {
    de__gpu_pool pool;
    de_tracking_allocator tracker;
    de_allocator alloc;
} pool_fx;

static void pool_fx_setup(pool_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
}
static void pool_fx_teardown(pool_fx *f) {
    de__gpu_pool_destroy(&f->pool);
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracker).blocks_live, 0u);
}

TEST_F(handle_pool, generation, pool_fx) {
    ASSERT_EQ(de__gpu_pool_init(&f->pool, &f->alloc, "thing", 40, 1), DE_OK);
    void *e = NULL;
    u32 id = de__gpu_pool_alloc(&f->pool, &e);
    ASSERT_NE(id, 0u);
    ASSERT_TRUE(e != NULL);
    EXPECT_EQ(de__gpu_handle_index(id), 0u);
    EXPECT_EQ(de__gpu_handle_gen(id), 1u);
    EXPECT_TRUE(de__gpu_pool_get(&f->pool, id) == e);

    /* Retire: the id is stale at once, the element stays readable by index. */
    u32 index = de__gpu_pool_retire(&f->pool, id);
    ASSERT_EQ(index, 0u);
    EXPECT_TRUE(de__gpu_pool_get(&f->pool, id) == NULL);
    EXPECT_TRUE(de__gpu_pool_is_stale(&f->pool, id));
    EXPECT_TRUE(de__gpu_pool_at(&f->pool, index) == e);
    /* A second retire of the same id is refused. */
    EXPECT_EQ(de__gpu_pool_retire(&f->pool, id), DE__GPU_POOL_MAX_SLOTS);
    de__gpu_pool_release(&f->pool, index);

    /* 10000 reuses of the one slot: the generation cycles 1..4095, never 0; old ids go stale. */
    u32 prev = id;
    u32 wraps = 0;
    for (u32 i = 0; i < 10000; ++i) {
        u32 next = de__gpu_pool_alloc(&f->pool, &e);
        ASSERT_NE(next, 0u);
        ASSERT_EQ(de__gpu_handle_index(next), 0u);
        ASSERT_NE(de__gpu_handle_gen(next), 0u);
        ASSERT_NE(next, prev);
        ASSERT_TRUE(de__gpu_pool_get(&f->pool, prev) == NULL);
        ASSERT_TRUE(de__gpu_pool_get(&f->pool, next) == e);
        if (de__gpu_handle_gen(next) < de__gpu_handle_gen(prev))
            ++wraps;
        if (de__gpu_handle_gen(prev) == DE__GPU_HANDLE_GEN_MAX)
            ASSERT_EQ(de__gpu_handle_gen(next), 1u);
        ASSERT_EQ(de__gpu_pool_retire(&f->pool, next), 0u);
        de__gpu_pool_release(&f->pool, 0);
        prev = next;
    }
    EXPECT_EQ(wraps, 2u); /* 10001 lifetimes / 4095 generations */
    de__gpu_pool_stats st = de__gpu_pool_get_stats(&f->pool);
    EXPECT_EQ(st.live, 0u);
    EXPECT_EQ(st.free, 1u);
    EXPECT_EQ(st.used, 1u);
}

TEST_F(handle_pool, capacity, pool_fx) {
    ASSERT_EQ(de__gpu_pool_init(&f->pool, &f->alloc, "thing", 8, 4), DE_OK);
    u32 ids[4];
    void *e;
    for (u32 i = 0; i < 4; ++i) {
        ids[i] = de__gpu_pool_alloc(&f->pool, &e);
        ASSERT_NE(ids[i], 0u);
        EXPECT_EQ(de__gpu_handle_index(ids[i]), i);
    }
    de_error_clear();
    EXPECT_EQ(de__gpu_pool_alloc(&f->pool, &e), 0u);
    EXPECT_TRUE(e == NULL);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(strstr(errmsg(), "thing pool is full") != NULL);

    /* A retired slot still counts until it is released (the GPU may still use it). */
    u32 index = de__gpu_pool_retire(&f->pool, ids[2]);
    EXPECT_EQ(de__gpu_pool_alloc(&f->pool, &e), 0u);
    EXPECT_TRUE(strstr(errmsg(), "1 awaiting the GPU") != NULL);
    de__gpu_pool_release(&f->pool, index);
    u32 again = de__gpu_pool_alloc(&f->pool, &e);
    ASSERT_NE(again, 0u);
    EXPECT_EQ(de__gpu_handle_index(again), 2u);
    EXPECT_NE(again, ids[2]);

    /* Too many slots for a 20-bit index. */
    de__gpu_pool big;
    EXPECT_EQ(de__gpu_pool_init(&big, &f->alloc, "big", 8, DE__GPU_POOL_MAX_SLOTS + 1u),
              DE_ERR_INVALID_ARGUMENT);
}

TEST_F(handle_pool, fifo_reuse_and_forged_ids, pool_fx) {
    ASSERT_EQ(de__gpu_pool_init(&f->pool, &f->alloc, "thing", 16, 0), DE_OK);
    EXPECT_EQ(f->pool.max_slots, DE__GPU_POOL_MAX_SLOTS);
    void *e;
    u32 a = de__gpu_pool_alloc(&f->pool, &e);
    u32 b = de__gpu_pool_alloc(&f->pool, &e);
    u32 c = de__gpu_pool_alloc(&f->pool, &e);
    ASSERT_TRUE(a && b && c);
    /* Release b, then a: they come back in that order (FIFO), before any new slot. */
    de__gpu_pool_release(&f->pool, de__gpu_pool_retire(&f->pool, b));
    de__gpu_pool_release(&f->pool, de__gpu_pool_retire(&f->pool, a));
    u32 x = de__gpu_pool_alloc(&f->pool, &e);
    u32 y = de__gpu_pool_alloc(&f->pool, &e);
    u32 z = de__gpu_pool_alloc(&f->pool, &e);
    EXPECT_EQ(de__gpu_handle_index(x), de__gpu_handle_index(b));
    EXPECT_EQ(de__gpu_handle_index(y), de__gpu_handle_index(a));
    EXPECT_EQ(de__gpu_handle_index(z), 3u);

    /* 0, a generation-0 id, and an index never handed out are invalid but not "stale". */
    EXPECT_TRUE(de__gpu_pool_get(&f->pool, 0) == NULL);
    EXPECT_FALSE(de__gpu_pool_is_stale(&f->pool, 0));
    EXPECT_TRUE(de__gpu_pool_get(&f->pool, de__gpu_handle_make(1, 0)) == NULL);
    u32 forged = de__gpu_handle_make(500, 1);
    EXPECT_TRUE(de__gpu_pool_get(&f->pool, forged) == NULL);
    EXPECT_FALSE(de__gpu_pool_is_stale(&f->pool, forged));
    EXPECT_EQ(de__gpu_pool_retire(&f->pool, forged), DE__GPU_POOL_MAX_SLOTS);
    /* An old id of a reused slot is stale. */
    EXPECT_TRUE(de__gpu_pool_is_stale(&f->pool, a));
    (void)c;
}

TEST_F(handle_pool, chunks_keep_elements_in_place, pool_fx) {
    ASSERT_EQ(de__gpu_pool_init(&f->pool, &f->alloc, "thing", 24, 0), DE_OK);
    enum { N = 1000 };
    static u32 ids[N];
    static void *elems[N];
    for (u32 i = 0; i < N; ++i) {
        ids[i] = de__gpu_pool_alloc(&f->pool, &elems[i]);
        ASSERT_NE(ids[i], 0u);
        ASSERT_EQ((uintptr_t)elems[i] % 16u, 0u);
        const u8 *bytes = (const u8 *)elems[i];
        for (u32 j = 0; j < 32; ++j) /* element rounded up to 32 bytes, zeroed */
            ASSERT_EQ(bytes[j], 0u);
        memset(elems[i], 0xAB, 24);
    }
    for (u32 i = 0; i < N; ++i)
        ASSERT_TRUE(de__gpu_pool_get(&f->pool, ids[i]) == elems[i]);
    de__gpu_pool_stats st = de__gpu_pool_get_stats(&f->pool);
    EXPECT_EQ(st.live, (u32)N);
    EXPECT_EQ(st.chunks, (N + DE__GPU_POOL_CHUNK_SLOTS - 1u) / DE__GPU_POOL_CHUNK_SLOTS);
    /* One chunk table plus one block per chunk. */
    EXPECT_EQ(de_tracking_allocator_stats(&f->tracker).blocks_live, 1u + st.chunks);
}
