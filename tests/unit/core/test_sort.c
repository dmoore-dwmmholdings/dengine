/* Unit tests: de_sort_stable and the radix sorts (core/de_sort.h). DENG-153. */
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "core/de_sort.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Helpers ---------- */

/* splitmix64: the same sequence on every platform. */
static u64 rng_next(u64 *state) {
    u64 z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

typedef struct test_heap {
    size_t live_bytes;
    size_t allocs;
    int fail;
} test_heap;

static void *th_alloc(void *user, size_t size, size_t align, u32 tag) {
    test_heap *h = (test_heap *)user;
    (void)tag;
    if (h->fail)
        return NULL;
    /* malloc's alignment (16 on the 64-bit targets) covers what the sorts ask for. */
    if (align > 16)
        return NULL;
    void *p = malloc(size == 0 ? 1 : size);
    if (p != NULL) {
        h->live_bytes += size;
        ++h->allocs;
    }
    return p;
}

static void *th_realloc(void *user, void *ptr, size_t old_size, size_t new_size, size_t align,
                        u32 tag) {
    (void)user, (void)ptr, (void)old_size, (void)new_size, (void)align, (void)tag;
    return NULL; /* the sorts never resize */
}

static void th_free(void *user, void *ptr, size_t size) {
    test_heap *h = (test_heap *)user;
    h->live_bytes -= size;
    free(ptr);
}

static de_allocator test_allocator(test_heap *h) {
    memset(h, 0, sizeof *h);
    de_allocator a = {th_alloc, th_realloc, th_free, h};
    return a;
}

/* A record whose key has many duplicates; `seq` is the input position, for the stability check. */
typedef struct rec {
    u32 key;
    u32 seq;
} rec;

static int cmp_rec(const void *a, const void *b, void *user) {
    (void)user;
    const u32 ka = ((const rec *)a)->key, kb = ((const rec *)b)->key;
    return (ka > kb) - (ka < kb);
}

/* The reference: plain insertion sort, stable by construction. */
static void insertion_sort_rec(rec *r, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        const rec cur = r[i];
        size_t j = i;
        for (; j > 0 && r[j - 1].key > cur.key; --j)
            r[j] = r[j - 1];
        r[j] = cur;
    }
}

static void fill_recs(rec *r, size_t n, u32 key_range, u64 seed) {
    u64 s = seed;
    for (size_t i = 0; i < n; ++i) {
        r[i].key = (u32)(rng_next(&s) % key_range);
        r[i].seq = (u32)i;
    }
}

/* ---------- de_sort_stable ---------- */

TEST(sort, stable_dups) {
    enum { N = 100000 };
    static rec r[N];
    test_heap h;
    const de_allocator a = test_allocator(&h);

    /* 1e5 records over 100 keys: about 1000 duplicates per key. Sorted by key, and within a key
     * the input order (seq) is kept: that fixes the output completely, so it equals what any
     * stable sort, the insertion-sort reference included, produces. */
    fill_recs(r, N, 100, 1);
    ASSERT_EQ(de_sort_stable(r, N, sizeof(rec), cmp_rec, NULL, &a), DE_OK);
    for (size_t i = 1; i < N; ++i) {
        ASSERT_LE(r[i - 1].key, r[i].key);
        if (r[i - 1].key == r[i].key)
            ASSERT_LT(r[i - 1].seq, r[i].seq);
    }
    EXPECT_EQ(h.live_bytes, 0u);
    EXPECT_EQ(h.allocs, 1u);

    /* Byte-for-byte equal to the insertion-sort reference, for every size from 0 to 300 (all
     * run and merge boundaries) and for larger arrays with few and with many distinct keys. */
    static rec x[20000], y[20000];
    for (size_t n = 0; n <= 300; ++n) {
        fill_recs(x, n, 7, 100 + n);
        memcpy(y, x, n * sizeof(rec));
        ASSERT_EQ(de_sort_stable(x, n, sizeof(rec), cmp_rec, NULL, &a), DE_OK);
        insertion_sort_rec(y, n);
        ASSERT_MEMEQ(x, y, n * sizeof(rec));
    }
    static const size_t sizes[] = {1000, 4099, 20000};
    static const u32 ranges[] = {2, 50, 1u << 30};
    for (size_t si = 0; si < 3; ++si) {
        for (size_t ri = 0; ri < 3; ++ri) {
            const size_t n = sizes[si];
            fill_recs(x, n, ranges[ri], 7 * si + ri);
            memcpy(y, x, n * sizeof(rec));
            ASSERT_EQ(de_sort_stable(x, n, sizeof(rec), cmp_rec, NULL, &a), DE_OK);
            insertion_sort_rec(y, n);
            ASSERT_MEMEQ(x, y, n * sizeof(rec));
        }
    }

    /* Already sorted and reversed input. */
    for (size_t i = 0; i < 5000; ++i) {
        x[i].key = (u32)(i / 3);
        x[i].seq = (u32)i;
        y[i].key = (u32)((4999 - i) / 3);
        y[i].seq = (u32)i;
    }
    ASSERT_EQ(de_sort_stable(x, 5000, sizeof(rec), cmp_rec, NULL, &a), DE_OK);
    ASSERT_EQ(de_sort_stable(y, 5000, sizeof(rec), cmp_rec, NULL, &a), DE_OK);
    for (size_t i = 0; i < 5000; ++i) {
        ASSERT_EQ(x[i].seq, (u32)i);
        ASSERT_EQ(y[i].key, (u32)(i / 3));
    }
    for (size_t i = 1; i < 5000; ++i)
        if (y[i - 1].key == y[i].key)
            ASSERT_LT(y[i - 1].seq, y[i].seq);
    EXPECT_EQ(h.live_bytes, 0u);
}

/* Elements of odd and large sizes, with the key at the front and the input position behind it. */
typedef struct odd13 {
    u8 key;
    u8 bytes[12];
} odd13;

typedef struct big100 {
    u16 key;
    u32 seq;
    u8 pad[94];
} big100;

static int cmp_odd13(const void *a, const void *b, void *user) {
    (void)user;
    return (int)((const odd13 *)a)->key - (int)((const odd13 *)b)->key;
}

static int cmp_big100(const void *a, const void *b, void *user) {
    /* Uses `user` as a sort direction, to show it reaches the comparison. */
    const int dir = *(const int *)user;
    const int ka = ((const big100 *)a)->key, kb = ((const big100 *)b)->key;
    return dir * ((ka > kb) - (ka < kb));
}

TEST(sort, element_sizes) {
    test_heap h;
    const de_allocator a = test_allocator(&h);
    enum { N = 3000 };
    static odd13 o[N];
    static big100 g[N];
    u64 s = 9;
    for (u32 i = 0; i < N; ++i) {
        o[i].key = (u8)(rng_next(&s) % 10);
        memcpy(o[i].bytes, &i, sizeof i);
        memset(o[i].bytes + sizeof i, (int)(i & 0xFF), sizeof o[i].bytes - sizeof i);
        memset(&g[i], 0, sizeof g[i]);
        g[i].key = (u16)(rng_next(&s) % 40);
        g[i].seq = i;
        g[i].pad[93] = (u8)i;
    }
    ASSERT_EQ(de_sort_stable(o, N, sizeof(odd13), cmp_odd13, NULL, &a), DE_OK);
    for (size_t i = 1; i < N; ++i) {
        ASSERT_LE(o[i - 1].key, o[i].key);
        u32 sp, sc;
        memcpy(&sp, o[i - 1].bytes, sizeof sp);
        memcpy(&sc, o[i].bytes, sizeof sc);
        if (o[i - 1].key == o[i].key)
            ASSERT_LT(sp, sc);
        ASSERT_EQ(o[i].bytes[11], (u8)(sc & 0xFF)); /* whole element moved */
    }
    int descending = -1;
    ASSERT_EQ(de_sort_stable(g, N, sizeof(big100), cmp_big100, &descending, &a), DE_OK);
    for (size_t i = 1; i < N; ++i) {
        ASSERT_GE(g[i - 1].key, g[i].key);
        if (g[i - 1].key == g[i].key)
            ASSERT_LT(g[i - 1].seq, g[i].seq);
        ASSERT_EQ(g[i].pad[93], (u8)g[i].seq);
    }
    EXPECT_EQ(h.live_bytes, 0u);
}

TEST(sort, arguments_and_scratch) {
    test_heap h;
    const de_allocator a = test_allocator(&h);
    rec r[64];

    /* Trivial and small inputs need no scratch at all. */
    EXPECT_EQ(de_sort_stable(NULL, 0, sizeof(rec), cmp_rec, NULL, NULL), DE_OK);
    fill_recs(r, DE_SORT_SMALL, 5, 3);
    EXPECT_EQ(de_sort_stable(r, DE_SORT_SMALL, sizeof(rec), cmp_rec, NULL, NULL), DE_OK);
    for (size_t i = 1; i < DE_SORT_SMALL; ++i)
        EXPECT_LE(r[i - 1].key, r[i].key);

    /* Bad arguments. */
    de_error_clear();
    EXPECT_EQ(de_sort_stable(NULL, 3, sizeof(rec), cmp_rec, NULL, &a), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_sort_stable(r, 3, 0, cmp_rec, NULL, &a), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_sort_stable(r, 3, sizeof(rec), NULL, NULL, &a), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(de_error_last() != NULL);

    /* Larger input without scratch, or with a failing allocator: error, array untouched. */
    fill_recs(r, 64, 1000, 4);
    rec copy[64];
    memcpy(copy, r, sizeof r);
    EXPECT_EQ(de_sort_stable(r, 64, sizeof(rec), cmp_rec, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    h.fail = 1;
    EXPECT_EQ(de_sort_stable(r, 64, sizeof(rec), cmp_rec, NULL, &a), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_MEMEQ(r, copy, sizeof r);
    de_sort_key32 k[64] = {{0, 0}};
    EXPECT_EQ(de_radix_sort_u32(k, 64, &a), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_radix_sort_u32(k, 64, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_radix_sort_u32(NULL, 2, &a), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_radix_sort_u64(NULL, 0, NULL), DE_OK);
    EXPECT_EQ(h.live_bytes, 0u);
}

/* ---------- Radix sort ---------- */

static int cmp_key32(const void *a, const void *b, void *user) {
    (void)user;
    const u32 ka = ((const de_sort_key32 *)a)->key, kb = ((const de_sort_key32 *)b)->key;
    return (ka > kb) - (ka < kb);
}

static int cmp_key64(const void *a, const void *b, void *user) {
    (void)user;
    const u64 ka = ((const de_sort_key64 *)a)->key, kb = ((const de_sort_key64 *)b)->key;
    return (ka > kb) - (ka < kb);
}

/* Key generators: full range, few distinct values, only high bits, only low bits. The last two
 * make most radix passes skippable, which exercises the odd/even pass-count copy-back. */
static u64 make_key(u64 *s, int kind) {
    const u64 r = rng_next(s);
    switch (kind) {
    case 0:
        return r;
    case 1:
        return r % 17;
    case 2:
        return ((r % 255) << 56) | ((r % 3) << 24);
    default:
        return r & 0xFF;
    }
}

TEST(sort, radix_matches) {
    enum { N = 100000 };
    static de_sort_key32 a32[N], b32[N];
    static de_sort_key64 a64[N], b64[N];
    test_heap h;
    const de_allocator a = test_allocator(&h);
    static const size_t sizes[] = {0, 1, 2, 31, 32, 33, 1000, N};
    for (int kind = 0; kind < 4; ++kind) {
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; ++si) {
            const size_t n = sizes[si];
            u64 s = 1000u + (u64)kind * 31u + si;
            for (size_t i = 0; i < n; ++i) {
                const u64 key = make_key(&s, kind);
                a32[i].key = (u32)(key ^ (key >> 32));
                a32[i].index = (u32)i;
                a64[i].key = key;
                a64[i].index = i;
            }
            memcpy(b32, a32, n * sizeof a32[0]);
            memcpy(b64, a64, n * sizeof a64[0]);
            ASSERT_EQ(de_radix_sort_u32(a32, n, &a), DE_OK);
            ASSERT_EQ(de_sort_stable(b32, n, sizeof b32[0], cmp_key32, NULL, &a), DE_OK);
            ASSERT_MEMEQ(a32, b32, n * sizeof a32[0]);
            ASSERT_EQ(de_radix_sort_u64(a64, n, &a), DE_OK);
            ASSERT_EQ(de_sort_stable(b64, n, sizeof b64[0], cmp_key64, NULL, &a), DE_OK);
            ASSERT_MEMEQ(a64, b64, n * sizeof a64[0]);
        }
    }
    EXPECT_EQ(h.live_bytes, 0u);
}
