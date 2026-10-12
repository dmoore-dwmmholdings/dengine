/* test_gpu_tlsf.c — the offset-based device-memory TLSF (src/gpu/memory/gpu_tlsf.c, DENG-282):
 * random alloc/free checked against an interval tree, alignment, coalescing, and an O(1) timing
 * bound. CTest names: gpu.gpu_tlsf.<test>. */
#include "de_test.h"

#include <dengine/de_platform.h>

#include "gpu/memory/gpu_tlsf.h"

#include <stdio.h>
#include <string.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) ||                         \
    __has_feature(memory_sanitizer)
#define DE_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define DE_TEST_SANITIZED 1
#endif

#define KIB ((u64)1 << 10)
#define MIB ((u64)1 << 20)
#define GIB ((u64)1 << 30)

/* ---------- Helpers ---------- */

typedef struct rng {
    u64 s;
} rng;

static u64 rng_next(rng *r) {
    /* xorshift64*: deterministic, so a failure reproduces. */
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return r->s * 0x2545F4914F6CDD1Dull;
}

static u64 rng_range(rng *r, u64 n) {
    return rng_next(r) % n;
}

/* Log-uniform size in [256 B, 32 MiB], plus an odd remainder so rounding is exercised. */
static u64 rand_size(rng *r) {
    const u32 lg = 8 + (u32)rng_range(r, 18); /* 2^8 .. 2^25 */
    u64 s = ((u64)1 << lg) + rng_range(r, (u64)1 << lg);
    if (s > 32 * MIB)
        s = 32 * MIB;
    return s;
}

/* 0 (default), or a power of two up to 64 KiB, some below the granularity. */
static u64 rand_align(rng *r) {
    const u32 k = (u32)rng_range(r, 18);
    return k == 17 ? 0 : (u64)1 << k; /* 1 .. 64 KiB */
}

static u64 round_gran(u64 size) {
    return size ? (size + 255) & ~(u64)255 : 256;
}

/* ---------- Interval tree ----------
 * A treap of the live [lo, hi) ranges keyed by lo. The ranges never overlap (that is what the
 * test checks), so a new range overlaps some live range iff it overlaps the live range with the
 * largest lo below the new hi. */

#define IT_MAX 4096

typedef struct it_node {
    u64 lo, hi;
    u32 prio;
    i32 l, r;
} it_node;

typedef struct itree {
    it_node n[IT_MAX];
    i32 free_list[IT_MAX];
    i32 free_count;
    i32 root;
    u32 count;
    rng prio;
} itree;

static void it_init(itree *t) {
    t->root = -1;
    t->count = 0;
    t->free_count = IT_MAX;
    for (i32 i = 0; i < IT_MAX; ++i)
        t->free_list[i] = IT_MAX - 1 - i;
    t->prio.s = 0x9E3779B97F4A7C15ull;
}

static i32 it_rot_right(itree *t, i32 x) {
    i32 y = t->n[x].l;
    t->n[x].l = t->n[y].r;
    t->n[y].r = x;
    return y;
}

static i32 it_rot_left(itree *t, i32 x) {
    i32 y = t->n[x].r;
    t->n[x].r = t->n[y].l;
    t->n[y].l = x;
    return y;
}

static i32 it_insert_at(itree *t, i32 root, i32 node) {
    if (root < 0)
        return node;
    if (t->n[node].lo < t->n[root].lo) {
        t->n[root].l = it_insert_at(t, t->n[root].l, node);
        if (t->n[t->n[root].l].prio > t->n[root].prio)
            root = it_rot_right(t, root);
    } else {
        t->n[root].r = it_insert_at(t, t->n[root].r, node);
        if (t->n[t->n[root].r].prio > t->n[root].prio)
            root = it_rot_left(t, root);
    }
    return root;
}

static i32 it_erase_at(itree *t, i32 root, u64 lo, bool *found) {
    if (root < 0)
        return -1;
    if (lo < t->n[root].lo) {
        t->n[root].l = it_erase_at(t, t->n[root].l, lo, found);
    } else if (lo > t->n[root].lo) {
        t->n[root].r = it_erase_at(t, t->n[root].r, lo, found);
    } else {
        const i32 l = t->n[root].l, r = t->n[root].r;
        if (l < 0 || r < 0) {
            *found = true;
            t->free_list[t->free_count++] = root;
            return l < 0 ? r : l;
        }
        if (t->n[l].prio > t->n[r].prio) {
            root = it_rot_right(t, root);
            t->n[root].r = it_erase_at(t, t->n[root].r, lo, found);
        } else {
            root = it_rot_left(t, root);
            t->n[root].l = it_erase_at(t, t->n[root].l, lo, found);
        }
    }
    return root;
}

/* True when [lo, hi) overlaps a live range. */
static bool it_overlaps(const itree *t, u64 lo, u64 hi) {
    i32 cur = t->root, best = -1;
    while (cur >= 0) {
        if (t->n[cur].lo < hi) {
            best = cur;
            cur = t->n[cur].r;
        } else {
            cur = t->n[cur].l;
        }
    }
    return best >= 0 && t->n[best].hi > lo;
}

/* Inserts [lo, hi); false (and nothing inserted) if it overlaps a live range. */
static bool it_insert(itree *t, u64 lo, u64 hi) {
    if (it_overlaps(t, lo, hi) || t->free_count == 0)
        return false;
    const i32 node = t->free_list[--t->free_count];
    t->n[node] = (it_node){.lo = lo, .hi = hi, .prio = (u32)rng_next(&t->prio), .l = -1, .r = -1};
    t->root = it_insert_at(t, t->root, node);
    ++t->count;
    return true;
}

static bool it_erase(itree *t, u64 lo) {
    bool found = false;
    t->root = it_erase_at(t, t->root, lo, &found);
    if (found)
        --t->count;
    return found;
}

/* ---------- Tests ---------- */

typedef struct live_slot {
    de__gpu_tlsf_block *b;
    u64 off, size;
} live_slot;

TEST(gpu_tlsf, interval_tree_self_check) {
    static itree t;
    it_init(&t);
    EXPECT_TRUE(it_insert(&t, 100, 200));
    EXPECT_TRUE(it_insert(&t, 300, 400));
    EXPECT_TRUE(it_insert(&t, 200, 300)); /* touching is not overlapping */
    EXPECT_FALSE(it_insert(&t, 150, 160));
    EXPECT_FALSE(it_insert(&t, 50, 101));
    EXPECT_FALSE(it_insert(&t, 399, 500));
    EXPECT_FALSE(it_insert(&t, 0, 1000));
    EXPECT_TRUE(it_insert(&t, 400, 401));
    EXPECT_TRUE(it_erase(&t, 200));
    EXPECT_FALSE(it_erase(&t, 200));
    EXPECT_TRUE(it_insert(&t, 250, 260));
    EXPECT_EQ(t.count, 4u);
}

TEST(gpu_tlsf, random_no_overlap) {
    enum { SLOTS = 2048 };
    const u64 cap = 8 * GIB;
    const u64 ops = 1000000;
    static itree tree;
    static live_slot slot[SLOTS];
    memset(slot, 0, sizeof slot);
    it_init(&tree);
    de__gpu_tlsf *t;
    ASSERT_EQ(de__gpu_tlsf_create(NULL, cap, &t), DE_OK);

    rng r = {0x1234567887654321ull};
    u64 used = 0, allocs = 0, frees = 0, misses = 0;
    u32 live = 0;
    for (u64 i = 0; i < ops; ++i) {
        live_slot *s = &slot[rng_range(&r, SLOTS)];
        if (s->b) {
            ASSERT_TRUE(it_erase(&tree, s->off));
            de__gpu_tlsf_free(t, s->b);
            used -= s->size;
            --live;
            s->b = NULL;
            ++frees;
        } else {
            const u64 size = rand_size(&r), align = rand_align(&r);
            u64 off = ~(u64)0;
            de__gpu_tlsf_block *b = de__gpu_tlsf_alloc(t, size, align, &off);
            if (!b) {
                ++misses;
            } else {
                const u64 a = align < 256 ? 256 : align;
                ASSERT_EQ(off % a, (u64)0);
                ASSERT_EQ(de__gpu_tlsf_block_offset(b), off);
                ASSERT_EQ(de__gpu_tlsf_block_size(b), round_gran(size));
                ASSERT_LE(off + round_gran(size), cap);
                /* The interval tree rejects any overlap with a live block. */
                ASSERT_TRUE(it_insert(&tree, off, off + round_gran(size)));
                s->b = b;
                s->off = off;
                s->size = round_gran(size);
                used += s->size;
                ++live;
                ++allocs;
            }
        }
        /* Stats after every operation. */
        de__gpu_tlsf_stats st;
        de__gpu_tlsf_get_stats(t, &st);
        ASSERT_EQ(st.used, used);
        ASSERT_EQ(st.free, cap - used);
        ASSERT_EQ(st.used_blocks, live);
        ASSERT_LE(st.largest_free, st.free);
        if ((i + 1) % 100000 == 0)
            ASSERT_EQ(de__gpu_tlsf_check(t), DE_OK);
    }
    printf("gpu_tlsf.random_no_overlap: %llu allocs, %llu frees, %llu misses (full)\n",
           (unsigned long long)allocs, (unsigned long long)frees, (unsigned long long)misses);
    EXPECT_GT(allocs, ops / 3);

    for (u32 i = 0; i < SLOTS; ++i)
        if (slot[i].b)
            de__gpu_tlsf_free(t, slot[i].b);
    de__gpu_tlsf_stats st;
    de__gpu_tlsf_get_stats(t, &st);
    EXPECT_EQ(st.used, (u64)0);
    EXPECT_EQ(st.free_blocks, 1u);
    EXPECT_EQ(st.largest_free, cap);
    EXPECT_EQ(de__gpu_tlsf_check(t), DE_OK);
    de__gpu_tlsf_destroy(t);
}

TEST(gpu_tlsf, alignment) {
    de__gpu_tlsf *t;
    ASSERT_EQ(de__gpu_tlsf_create(NULL, 256 * MIB, &t), DE_OK);
    /* Misalign the free space with small blocks, then ask for every alignment. */
    de__gpu_tlsf_block *pad[64];
    u64 off;
    for (u32 i = 0; i < 64; ++i) {
        pad[i] = de__gpu_tlsf_alloc(t, 256 * (1 + i % 3), 0, &off);
        ASSERT_TRUE(pad[i] != NULL);
        EXPECT_EQ(off % 256, (u64)0);
    }
    de__gpu_tlsf_block *blk[24];
    u32 n = 0;
    for (u32 k = 0; k <= 22; ++k) { /* 1 B .. 4 MiB */
        const u64 align = (u64)1 << k;
        de__gpu_tlsf_block *b = de__gpu_tlsf_alloc(t, 1000 + k, align, &off);
        ASSERT_TRUE(b != NULL);
        EXPECT_EQ(off % (align < 256 ? 256 : align), (u64)0);
        EXPECT_EQ(de__gpu_tlsf_block_size(b), (u64)1024);
        blk[n++] = b;
    }
    ASSERT_EQ(de__gpu_tlsf_check(t), DE_OK);
    /* The leading gaps went back to the free lists: freeing all coalesces to one block. */
    for (u32 i = 0; i < n; ++i)
        de__gpu_tlsf_free(t, blk[i]);
    for (u32 i = 0; i < 64; ++i)
        de__gpu_tlsf_free(t, pad[i]);
    de__gpu_tlsf_stats st;
    de__gpu_tlsf_get_stats(t, &st);
    EXPECT_EQ(st.free_blocks, 1u);
    EXPECT_EQ(st.largest_free, 256 * MIB);

    /* Bad requests fail without touching the heap. */
    EXPECT_TRUE(de__gpu_tlsf_alloc(t, 256, 3 * 256, &off) == NULL);   /* not a power of two */
    EXPECT_TRUE(de__gpu_tlsf_alloc(t, 256, 512 * MIB, &off) == NULL); /* above the capacity */
    EXPECT_TRUE(de__gpu_tlsf_alloc(t, 257 * MIB, 0, &off) == NULL);   /* too big */
    /* The whole range, then nothing more. */
    de__gpu_tlsf_block *all = de__gpu_tlsf_alloc(t, 256 * MIB, 64 * KIB, &off);
    ASSERT_TRUE(all != NULL);
    EXPECT_EQ(off, (u64)0);
    EXPECT_TRUE(de__gpu_tlsf_alloc(t, 1, 0, &off) == NULL);
    de__gpu_tlsf_free(t, all);
    EXPECT_TRUE(de__gpu_tlsf_is_empty(t));
    /* Size 0 takes one granule. */
    de__gpu_tlsf_block *z = de__gpu_tlsf_alloc(t, 0, 0, &off);
    ASSERT_TRUE(z != NULL);
    EXPECT_EQ(de__gpu_tlsf_block_size(z), (u64)256);
    de__gpu_tlsf_free(t, z);
    EXPECT_EQ(de__gpu_tlsf_check(t), DE_OK);
    de__gpu_tlsf_destroy(t);

    /* Capacity limits. */
    de__gpu_tlsf *bad;
    EXPECT_EQ(de__gpu_tlsf_create(NULL, 255, &bad), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__gpu_tlsf_create(NULL, DE__GPU_TLSF_MAX_CAPACITY + 256, &bad),
              DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de__gpu_tlsf_create(NULL, DE__GPU_TLSF_MAX_CAPACITY, &bad), DE_OK);
    de__gpu_tlsf_block *huge = de__gpu_tlsf_alloc(bad, DE__GPU_TLSF_MAX_CAPACITY / 2, 0, &off);
    EXPECT_TRUE(huge != NULL);
    de__gpu_tlsf_free(bad, huge);
    de__gpu_tlsf_destroy(bad);
}

TEST(gpu_tlsf, coalesce) {
    const u64 cap = 64 * MIB;
    de__gpu_tlsf *t;
    ASSERT_EQ(de__gpu_tlsf_create(NULL, cap, &t), DE_OK);
    u64 off;
    /* Neighbour cases: A B C D, then the free tail. Free B (no free neighbour), A (merges with
     * B on its right), D (merges with the tail), C (merges on both sides) -> one block. */
    de__gpu_tlsf_block *a = de__gpu_tlsf_alloc(t, MIB, 0, &off);
    de__gpu_tlsf_block *b = de__gpu_tlsf_alloc(t, MIB, 0, &off);
    de__gpu_tlsf_block *c = de__gpu_tlsf_alloc(t, MIB, 0, &off);
    de__gpu_tlsf_block *d = de__gpu_tlsf_alloc(t, MIB, 0, &off);
    ASSERT_TRUE(a && b && c && d);
    de__gpu_tlsf_stats st;
    de__gpu_tlsf_free(t, b);
    de__gpu_tlsf_get_stats(t, &st);
    EXPECT_EQ(st.free_blocks, 2u);
    EXPECT_TRUE(st.fragmentation > 0.0f);
    de__gpu_tlsf_free(t, a);
    de__gpu_tlsf_get_stats(t, &st);
    EXPECT_EQ(st.free_blocks, 2u);
    de__gpu_tlsf_free(t, d);
    de__gpu_tlsf_get_stats(t, &st);
    EXPECT_EQ(st.free_blocks, 2u);
    de__gpu_tlsf_free(t, c);
    de__gpu_tlsf_get_stats(t, &st);
    EXPECT_EQ(st.free_blocks, 1u);
    EXPECT_EQ(st.largest_free, cap);
    EXPECT_TRUE(st.fragmentation == 0.0f);
    ASSERT_EQ(de__gpu_tlsf_check(t), DE_OK);

    /* Fill the range exactly with mixed sizes, free in random order: one block again. */
    enum { N = 4096 };
    static de__gpu_tlsf_block *blk[N];
    u32 n = 0;
    rng r = {42};
    for (; n < N; ++n) {
        blk[n] = de__gpu_tlsf_alloc(t, 256 * (1 + rng_range(&r, 16)), 0, &off);
        if (!blk[n])
            break;
    }
    ASSERT_GT(n, 100u);
    for (u32 i = n - 1; i > 0; --i) {
        const u32 j = (u32)rng_range(&r, i + 1);
        de__gpu_tlsf_block *tmp = blk[i];
        blk[i] = blk[j];
        blk[j] = tmp;
    }
    for (u32 i = 0; i < n; ++i) {
        de__gpu_tlsf_free(t, blk[i]);
        if (i % 512 == 0)
            ASSERT_EQ(de__gpu_tlsf_check(t), DE_OK);
    }
    de__gpu_tlsf_get_stats(t, &st);
    EXPECT_EQ(st.used, (u64)0);
    EXPECT_EQ(st.used_blocks, 0u);
    EXPECT_EQ(st.free_blocks, 1u);
    EXPECT_EQ(st.largest_free, cap);
    ASSERT_EQ(de__gpu_tlsf_check(t), DE_OK);
    de__gpu_tlsf_destroy(t);
}

TEST(gpu_tlsf, perf) {
    /* 1M random alloc/free operations (256 B .. 32 MiB, alignments up to 64 KiB) on a 16 GiB
     * range. TLSF alloc and free are O(1), so the time does not depend on how many blocks
     * exist. The bound is asserted in Release and Shipping builds without sanitizers. */
    enum { SLOTS = 4096 };
    const u64 ops = 1000000;
    static de__gpu_tlsf_block *slot[SLOTS];
    static u64 sizes[1 << 16], aligns[1 << 16];
    static u32 picks[1 << 16];
    memset(slot, 0, sizeof slot);
    rng r = {7};
    for (u32 i = 0; i < (1u << 16); ++i) {
        sizes[i] = rand_size(&r);
        aligns[i] = rand_align(&r);
        picks[i] = (u32)rng_range(&r, SLOTS);
    }
    de__gpu_tlsf *t;
    ASSERT_EQ(de__gpu_tlsf_create(NULL, 16 * GIB, &t), DE_OK);
    u64 off, misses = 0;
    const u64 t0 = de_time_now_ns();
    for (u64 i = 0; i < ops; ++i) {
        const u32 k = (u32)(i & 0xFFFF);
        const u32 s = (picks[k] + (u32)(i >> 16) * 977u) % SLOTS;
        if (slot[s]) {
            de__gpu_tlsf_free(t, slot[s]);
            slot[s] = NULL;
        } else {
            slot[s] = de__gpu_tlsf_alloc(t, sizes[k], aligns[k], &off);
            misses += slot[s] == NULL;
        }
    }
    const u64 ns = de_time_now_ns() - t0;
    printf("[BENCH] gpu_tlsf: %llu ops in %.2f ms, %.1f ns/op (%llu misses)\n",
           (unsigned long long)ops, (double)ns / 1e6, (double)ns / (double)ops,
           (unsigned long long)misses);
    EXPECT_EQ(de__gpu_tlsf_check(t), DE_OK);
    for (u32 i = 0; i < SLOTS; ++i)
        de__gpu_tlsf_free(t, slot[i]);
    EXPECT_TRUE(de__gpu_tlsf_is_empty(t));
    de__gpu_tlsf_destroy(t);
#if !defined(DE_DEBUG) && !defined(DE_DEV) && !defined(DE_TEST_SANITIZED)
    EXPECT_LT(ns, (u64)200000000); /* < 200 ms */
#endif
}
