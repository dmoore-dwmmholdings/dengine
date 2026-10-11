/* Stress test of the S02 core pieces together (DENG-155): 8 threads for about 2 s mix logging
 * (DENG-150), the heap and a shared thread-safe pool (DENG-149), thread scratch (DENG-148), an
 * MPMC queue that moves pool blocks between threads (DENG-152), name interning (DENG-153) and the
 * containers (DENG-151). Every structure built on an allocator takes the default (NULL), so this
 * also covers NULL = de_heap_allocator().
 *
 * Its value is in the sanitizer runs: it must be clean under DE_SANITIZE=thread and
 * DE_SANITIZE=address. DE_STRESS_MS overrides the run time in milliseconds. */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#endif
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_array.h"
#include "core/de_hashmap.h"
#include "core/de_mpmc.h"
#include "core/log_internal.h"
#include "platform/de_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRESS_THREADS     8
#define STRESS_DEFAULT_MS  2000
#define STRESS_LIVE_BLOCKS 64
#define STRESS_NAMES       512

/* A pool block passed between threads: who made it, and a stamp over the rest. */
typedef struct stress_block {
    u32 owner;
    u32 seq;
    u64 check;
    u8 payload[48];
} stress_block;

typedef struct stress_shared {
    de_pool *pool;
    de_mpmc queue; /* stress_block * */
    de_log_category cat;
    u64 deadline_ns;
    de_atomic_u64 logged;    /* messages the workers logged in `cat` */
    de_atomic_u64 delivered; /* records of `cat` the sink saw */
    de_atomic_u64 blocks_moved;
    de_atomic_u32 failures;
} stress_shared;

typedef struct stress_worker {
    stress_shared *sh;
    u32 index;
    u64 iterations;
} stress_worker;

static u64 stress_rng(u64 *s) {
    u64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static u64 block_check(const stress_block *b) {
    return de_hash64(b->payload, sizeof b->payload, ((u64)b->owner << 32) | b->seq);
}

/* Records a failure from a worker thread; de_test.h checks run on the main thread only. */
static void stress_fail(stress_shared *sh, const char *what, u32 worker) {
    fprintf(stderr, "stress: worker %u: %s\n", worker, what);
    de_atomic_fetch_add_u32(&sh->failures, 1, DE_ATOMIC_RELAXED);
}

static void stress_sink(const de_log_record *rec, void *user) {
    stress_shared *sh = (stress_shared *)user;
    if (rec->category.id == sh->cat.id)
        de_atomic_fetch_add_u64(&sh->delivered, 1, DE_ATOMIC_RELAXED);
}

typedef struct heap_slot {
    u8 *ptr;
    size_t size;
    u8 fill;
} heap_slot;

static bool heap_slot_ok(const heap_slot *s) {
    for (size_t i = 0; i < s->size; i += 61)
        if (s->ptr[i] != s->fill)
            return false;
    return s->ptr[s->size - 1] == s->fill;
}

static i32 stress_main(void *user) {
    stress_worker *w = (stress_worker *)user;
    stress_shared *sh = w->sh;
    const de_allocator *heap = de_heap_allocator();
    u64 rng = 0x9E3779B97F4A7C15ull * (w->index + 1);
    heap_slot live[STRESS_LIVE_BLOCKS];
    memset(live, 0, sizeof live);
    u32 seq = 0;

    DE_ARRAY(u64) arr;
    DE_ARRAY_INIT(&arr, u64, NULL);
    de_hashmap map;
    if (DE_HASHMAP_INIT(&map, u64, u64, NULL) != DE_OK) {
        stress_fail(sh, "hash map init", w->index);
        return 1;
    }

    while (de_time_now_ns() < sh->deadline_ns) {
        for (int burst = 0; burst < 64; ++burst) {
            const u64 r = stress_rng(&rng);
            w->iterations++;

            /* Heap: replace a random slot (sometimes by realloc), checking its old contents. */
            heap_slot *s = &live[r % STRESS_LIVE_BLOCKS];
            const size_t size = 1 + (size_t)((r >> 8) % 4096);
            if (s->ptr != NULL && !heap_slot_ok(s))
                stress_fail(sh, "heap block contents changed", w->index);
            if (s->ptr != NULL && (r & 0x10000) != 0) {
                u8 *p = (u8 *)DE_REALLOC(heap, s->ptr, s->size, size, 16, DE_MEM_GENERAL);
                if (p == NULL) {
                    stress_fail(sh, "heap realloc failed", w->index);
                    continue;
                }
                s->ptr = p;
            } else {
                if (s->ptr != NULL)
                    DE_FREE(heap, s->ptr, s->size);
                s->ptr = (u8 *)DE_ALLOC(heap, size, 16, DE_MEM_GENERAL);
                if (s->ptr == NULL) {
                    stress_fail(sh, "heap alloc failed", w->index);
                    continue;
                }
            }
            s->size = size;
            s->fill = (u8)(r >> 24);
            memset(s->ptr, s->fill, size);

            /* Pool + MPMC: make a block and queue it; take one (maybe another thread's) back. */
            stress_block *b = (stress_block *)de_pool_alloc(sh->pool);
            if (b == NULL) {
                stress_fail(sh, "pool alloc failed", w->index);
                continue;
            }
            b->owner = w->index;
            b->seq = seq++;
            memset(b->payload, (int)(r >> 32), sizeof b->payload);
            b->check = block_check(b);
            if (!de_mpmc_try_push(&sh->queue, &b))
                de_pool_free(sh->pool, b);
            stress_block *got = NULL;
            if (de_mpmc_try_pop(&sh->queue, &got)) {
                if (got->check != block_check(got))
                    stress_fail(sh, "queued pool block corrupted", w->index);
                if (got->owner != w->index)
                    de_atomic_fetch_add_u64(&sh->blocks_moved, 1, DE_ATOMIC_RELAXED);
                de_pool_free(sh->pool, got);
            }

            /* Scratch: nested scopes, the inner one must not alias the outer. */
            de_scratch outer = de_scratch_begin(NULL, 0);
            u64 *xs = DE_ARENA_PUSH(outer.arena, u64, 64);
            de_scratch inner = de_scratch_begin(&outer.arena, 1);
            u64 *ys = DE_ARENA_PUSH(inner.arena, u64, 64);
            if (xs == NULL || ys == NULL || inner.arena == outer.arena) {
                stress_fail(sh, "scratch", w->index);
            } else {
                for (u32 i = 0; i < 64; ++i)
                    xs[i] = r + i;
                memset(ys, 0xAB, 64 * sizeof *ys);
                if (xs[63] != r + 63)
                    stress_fail(sh, "scratch scopes overlap", w->index);
            }
            de_scratch_end(inner);
            de_scratch_end(outer);

            /* Names: a shared set of strings, interned from every thread at once. */
            char text[32];
            snprintf(text, sizeof text, "stress.name.%u", (unsigned)(r % STRESS_NAMES));
            const de_name n = de_name_from(text);
            if (n.hash != de_hash64_str(text))
                stress_fail(sh, "name hash", w->index);
#if DE_NAME_LOOKUP
            const char *back = de_name_str(n);
            if (back == NULL || strcmp(back, text) != 0)
                stress_fail(sh, "name reverse lookup", w->index);
#endif

            /* Containers: a per-thread array and map on the default heap. */
            if (DE_ARRAY_PUSH(&arr, u64, r) != DE_OK)
                stress_fail(sh, "array push", w->index);
            if (arr.count >= 1024)
                de_array_clear(&arr);
            const u64 key = r % 2048;
            if (de_hashmap_put(&map, key * 0x9E3779B97F4A7C15ull, &key, &r) != DE_OK)
                stress_fail(sh, "hash map put", w->index);
            if (de_hashmap_count(&map) >= 1024)
                de_hashmap_clear(&map);

            /* Log: every 16th iteration. */
            if ((w->iterations & 15) == 0) {
                DE_INFO(sh->cat, "worker %u iteration %llu name %s", w->index,
                        (unsigned long long)w->iterations, text);
                de_atomic_fetch_add_u64(&sh->logged, 1, DE_ATOMIC_RELAXED);
            }
        }
    }

    for (u32 i = 0; i < STRESS_LIVE_BLOCKS; ++i) {
        if (live[i].ptr != NULL) {
            if (!heap_slot_ok(&live[i]))
                stress_fail(sh, "heap block contents changed", w->index);
            DE_FREE(heap, live[i].ptr, live[i].size);
        }
    }
    de_array_free(&arr);
    de_hashmap_free(&map);
    return 0;
}

TEST(core, stress_mixed) {
    u64 ms = STRESS_DEFAULT_MS;
    const char *env = getenv("DE_STRESS_MS");
    if (env != NULL && env[0] != '\0')
        ms = strtoull(env, NULL, 10);

    static stress_shared sh; /* big: the queue is cache-line aligned */
    memset(&sh, 0, sizeof sh);
    de_atomic_init_u64(&sh.logged, 0);
    de_atomic_init_u64(&sh.delivered, 0);
    de_atomic_init_u64(&sh.blocks_moved, 0);
    de_atomic_init_u32(&sh.failures, 0);

    de_log_shutdown();
    ASSERT_EQ(de_log_init(&(de_log_desc){.no_console_sink = true}), DE_OK);
    de_log_add_sink(stress_sink, &sh);
    sh.cat = de_log_register_category("stress", DE_LOG_INFO);
    const u64 dropped0 = de__log_get_counters().dropped;

    ASSERT_EQ(de_pool_create(&(de_pool_desc){.block_size = sizeof(stress_block),
                                             .blocks_per_page = 64,
                                             .thread_safe = true},
                             &sh.pool),
              DE_OK);
    ASSERT_EQ(de_mpmc_init(&sh.queue,
                           &(de_mpmc_desc){.capacity = 1024, .elem_size = sizeof(stress_block *)}),
              DE_OK);

    sh.deadline_ns = de_time_now_ns() + ms * 1000000ull;
    stress_worker workers[STRESS_THREADS];
    de_thread *threads[STRESS_THREADS];
    u32 started = 0;
    for (u32 i = 0; i < STRESS_THREADS; ++i) {
        workers[i] = (stress_worker){&sh, i, 0};
        char name[16];
        snprintf(name, sizeof name, "stress-%u", i);
        if (de_thread_create(stress_main, &workers[i], name, DE_THREAD_NORMAL, 0, &threads[i]) !=
            DE_OK)
            break;
        started++;
    }
    u64 iterations = 0;
    i32 rc = 0;
    for (u32 i = 0; i < started; ++i) {
        rc |= de_thread_join(threads[i]);
        iterations += workers[i].iterations;
    }
    ASSERT_EQ(started, (u32)STRESS_THREADS);
    EXPECT_EQ(rc, 0);

    /* Blocks still queued go back to the pool; then every block is home. */
    stress_block *b = NULL;
    while (de_mpmc_try_pop(&sh.queue, &b)) {
        EXPECT_EQ(b->check, block_check(b));
        de_pool_free(sh.pool, b);
    }
    EXPECT_EQ(de_pool_get_info(sh.pool).blocks_live, 0u);
    de_mpmc_destroy(&sh.queue);
    de_pool_destroy(sh.pool);

    /* Every message was delivered or counted as dropped. */
    de_log_flush();
    const u64 dropped = de__log_get_counters().dropped - dropped0;
    de_log_shutdown();
    const u64 logged = de_atomic_load_u64(&sh.logged, DE_ATOMIC_RELAXED);
    EXPECT_EQ(de_atomic_load_u64(&sh.delivered, DE_ATOMIC_RELAXED) + dropped, logged);

    EXPECT_EQ(de_atomic_load_u32(&sh.failures, DE_ATOMIC_RELAXED), 0u);
    EXPECT_GT(logged, 0u);
    EXPECT_GT(de_atomic_load_u64(&sh.blocks_moved, DE_ATOMIC_RELAXED), 0u);
    de_tlsf_info info;
    EXPECT_EQ(de_heap_check(&info), DE_OK);
    printf("[STRESS] %u threads, %llu ms: %llu iterations, %llu logged (%llu dropped), "
           "%llu pool blocks changed threads\n",
           (unsigned)STRESS_THREADS, (unsigned long long)ms, (unsigned long long)iterations,
           (unsigned long long)logged, (unsigned long long)dropped,
           (unsigned long long)de_atomic_load_u64(&sh.blocks_moved, DE_ATOMIC_RELAXED));
}
