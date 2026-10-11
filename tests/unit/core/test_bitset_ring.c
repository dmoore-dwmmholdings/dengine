/* Unit tests: de_bitset and de_ring (DENG-151). */
#include "de_test.h"

#include "core/de_bitset.h"
#include "core/de_ring.h"
#include "test_counting_alloc.h"

#include <string.h>

/* Reference find: plain loop over test(). */
static u32 bs_ref_find(const de_bitset *b, u32 from, bool want) {
    for (u32 i = from; i < b->bit_count; ++i)
        if (de_bitset_test(b, i) == want)
            return i;
    return DE_BITSET_NONE;
}

static void bs_check_all(const de_bitset *b) {
    for (u32 from = 0; from <= b->bit_count + 1u; ++from) {
        ASSERT_EQ(de_bitset_find_first_set(b, from), bs_ref_find(b, from, true));
        ASSERT_EQ(de_bitset_find_first_clear(b, from), bs_ref_find(b, from, false));
    }
    u32 pop = 0;
    for (u32 i = 0; i < b->bit_count; ++i)
        pop += de_bitset_test(b, i) ? 1u : 0u;
    ASSERT_EQ(de_bitset_popcount(b), pop);
}

TEST(bitset, find_first) {
    static const u32 sizes[] = {1, 63, 64, 65, 127, 128, 129, 1000};
    u64 words[DE_BITSET_WORDS(1000)];
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; ++s) {
        const u32 n = sizes[s];
        de_bitset b;
        de_bitset_init_fixed(&b, words, DE_BITSET_WORDS(1000), n);
        EXPECT_EQ(de_bitset_popcount(&b), 0u);
        EXPECT_EQ(de_bitset_find_first_set(&b, 0), DE_BITSET_NONE);
        EXPECT_EQ(de_bitset_find_first_clear(&b, 0), 0u);
        bs_check_all(&b);

        /* All set: no clear bit anywhere, also not in the unused tail of the last word. */
        de_bitset_set_all(&b);
        EXPECT_EQ(de_bitset_popcount(&b), n);
        EXPECT_EQ(de_bitset_find_first_clear(&b, 0), DE_BITSET_NONE);
        EXPECT_EQ(de_bitset_find_first_set(&b, n - 1u), n - 1u);
        EXPECT_EQ(de_bitset_find_first_set(&b, n), DE_BITSET_NONE);
        bs_check_all(&b);

        /* Bits at word boundaries and the ends. */
        de_bitset_clear_all(&b);
        const u32 probes[] = {0, 1, 62, 63, 64, 65, 127, 128, 500, 998, 999};
        for (size_t p = 0; p < sizeof probes / sizeof probes[0]; ++p)
            if (probes[p] < n)
                de_bitset_set(&b, probes[p]);
        de_bitset_set(&b, n - 1u);
        bs_check_all(&b);

        /* The complement pattern. */
        de_bitset_set_all(&b);
        for (size_t p = 0; p < sizeof probes / sizeof probes[0]; ++p)
            if (probes[p] < n)
                de_bitset_clear(&b, probes[p]);
        de_bitset_clear(&b, n - 1u);
        bs_check_all(&b);

        /* A pseudo-random pattern. */
        u64 x = 0x1234567u + n;
        for (u32 i = 0; i < n; ++i) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            de_bitset_assign(&b, i, (x >> 63) != 0);
        }
        bs_check_all(&b);
    }
}

TEST(bitset, growable_and_or) {
    test_counting_alloc ca;
    test_counting_init(&ca);
    de_bitset a, b;
    de_bitset_init(&a, &ca.iface, DE_MEM_GENERAL);
    de_bitset_init(&b, &ca.iface, DE_MEM_GENERAL);
    EXPECT_EQ(test_counting_calls(&ca), 0u);
    ASSERT_EQ(de_bitset_resize(&a, 65), DE_OK);
    ASSERT_EQ(de_bitset_resize(&b, 65), DE_OK);
    for (u32 i = 0; i < 65; i += 2)
        de_bitset_set(&a, i);
    for (u32 i = 0; i < 65; i += 3)
        de_bitset_set(&b, i);
    de_bitset c;
    u64 cw[2];
    de_bitset_init_fixed(&c, cw, 2, 65);
    de_bitset_or(&c, &a);
    de_bitset_and(&c, &b); /* multiples of 6 */
    for (u32 i = 0; i < 65; ++i)
        ASSERT_EQ(de_bitset_test(&c, i), i % 6u == 0);
    de_bitset_or(&c, &b); /* multiples of 3 */
    for (u32 i = 0; i < 65; ++i)
        ASSERT_EQ(de_bitset_test(&c, i), i % 3u == 0);
    de_bitset_and_not(&c, &a); /* odd multiples of 3 */
    for (u32 i = 0; i < 65; ++i)
        ASSERT_EQ(de_bitset_test(&c, i), i % 3u == 0 && (i & 1u));

    /* Growing keeps the bits and zeroes the new ones; shrinking drops the cut-off bits. */
    de_bitset_set(&a, 64);
    ASSERT_EQ(de_bitset_resize(&a, 1000), DE_OK);
    EXPECT_TRUE(de_bitset_test(&a, 64));
    EXPECT_EQ(de_bitset_find_first_set(&a, 65), DE_BITSET_NONE);
    EXPECT_EQ(de_bitset_popcount(&a), 33u);
    ASSERT_EQ(de_bitset_resize(&a, 64), DE_OK);
    EXPECT_EQ(de_bitset_popcount(&a), 32u);
    ASSERT_EQ(de_bitset_resize(&a, 200), DE_OK); /* within capacity: no allocation */
    EXPECT_FALSE(de_bitset_test(&a, 64));
    EXPECT_EQ(de_bitset_popcount(&a), 32u);

    /* A fixed bitset cannot grow past its storage; the null allocator fails cleanly. */
    EXPECT_EQ(de_bitset_resize(&c, 129), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(c.bit_count, 65u);
    EXPECT_EQ(de_bitset_resize(&c, 128), DE_OK);
    de_bitset nb;
    de_bitset_init(&nb, de_allocator_null(), DE_MEM_GENERAL);
    EXPECT_EQ(de_bitset_resize(&nb, 1), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(nb.bit_count, 0u);
    de_bitset_free(&nb);

    de_bitset_free(&a);
    de_bitset_free(&b);
    de_bitset_free(&c);
    EXPECT_EQ(ca.live_blocks, 0u);
    EXPECT_EQ(ca.size_errors, 0u);
    de_error_clear();
}

/* ---- Ring ----------------------------------------------------------------------------- */

typedef struct ring_item {
    u32 seq;
    u32 check;
} ring_item;

TEST(ring, wrap) {
    test_counting_alloc ca;
    test_counting_init(&ca);
    de_ring r;
    ASSERT_EQ(DE_RING_INIT(&r, ring_item, &ca.iface, 8, false), DE_OK);
    EXPECT_EQ(test_counting_calls(&ca), 1u);
    EXPECT_TRUE(de_ring_empty(&r));
    EXPECT_TRUE(de_ring_peek(&r) == NULL);
    EXPECT_TRUE(de_ring_peek_newest(&r) == NULL);

    /* Many rounds of partial fill and drain, so the positions wrap the storage many times. */
    u32 next_in = 0, next_out = 0;
    for (u32 round = 0; round < 10000u; ++round) {
        const u32 push = (round * 5u) % 9u; /* 0..8 */
        for (u32 i = 0; i < push; ++i) {
            const bool ok = DE_RING_PUSH(&r, ring_item, (ring_item){next_in, ~next_in});
            if (de_ring_count(&r) == 8u && !ok)
                break;
            ASSERT_TRUE(ok);
            ++next_in;
        }
        ASSERT_EQ(de_ring_count(&r), next_in - next_out);
        if (!de_ring_empty(&r)) {
            ASSERT_EQ(((ring_item *)de_ring_peek(&r))->seq, next_out);
            ASSERT_EQ(((ring_item *)de_ring_peek_newest(&r))->seq, next_in - 1u);
            for (u32 i = 0; i < de_ring_count(&r); ++i)
                ASSERT_EQ(DE_RING_AT(&r, ring_item, i).seq, next_out + i);
        }
        const u32 pop = (round * 3u) % 7u;
        for (u32 i = 0; i < pop; ++i) {
            ring_item it;
            if (!DE_RING_POP(&r, ring_item, &it)) {
                ASSERT_TRUE(de_ring_empty(&r));
                break;
            }
            ASSERT_EQ(it.seq, next_out);
            ASSERT_EQ(it.check, ~next_out);
            ++next_out;
        }
    }

    /* A full ring without overwrite refuses the push and keeps its contents. */
    de_ring_clear(&r);
    for (u32 i = 0; i < 8; ++i)
        ASSERT_TRUE(DE_RING_PUSH(&r, ring_item, (ring_item){i, 0}));
    EXPECT_TRUE(de_ring_full(&r));
    EXPECT_FALSE(DE_RING_PUSH(&r, ring_item, (ring_item){99, 0}));
    EXPECT_EQ(DE_RING_AT(&r, ring_item, 0).seq, 0u);
    EXPECT_EQ(DE_RING_AT(&r, ring_item, 7).seq, 7u);
    EXPECT_EQ(r.dropped, 0u);
    EXPECT_TRUE(de_ring_pop(&r, NULL));
    EXPECT_TRUE(de_ring_push(&r, NULL)); /* NULL pushes zeroes */
    EXPECT_EQ(DE_RING_AT(&r, ring_item, 7).seq, 0u);
    EXPECT_EQ(test_counting_calls(&ca), 1u); /* push/pop never allocate */

    /* Bad parameters. */
    de_ring bad;
    EXPECT_EQ(de_ring_init(&bad, &ca.iface, 4, 4, 12, false, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ring_init(&bad, &ca.iface, 4, 4, 0, false, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ring_init(&bad, &ca.iface, 0, 4, 8, false, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ring_init(&bad, de_allocator_null(), 4, 4, 8, false, 0), DE_ERR_OUT_OF_MEMORY);
    de_error_clear();

    de_ring_free(&r);
    EXPECT_EQ(ca.live_blocks, 0u);
    EXPECT_EQ(ca.size_errors, 0u);
    EXPECT_EQ(ca.align_errors, 0u);
}

TEST(ring, overwrite) {
    /* Caller storage: never allocates. Capacity 1 is allowed. */
    u32 storage[4];
    de_ring r;
    ASSERT_EQ(de_ring_init_buffer(&r, storage, sizeof(u32), 4, true), DE_OK);
    for (u32 i = 0; i < 1000; ++i) {
        ASSERT_TRUE(DE_RING_PUSH(&r, u32, i));
        const u32 n = i + 1u < 4u ? i + 1u : 4u;
        ASSERT_EQ(de_ring_count(&r), n);
        /* The newest n values, oldest first. */
        for (u32 k = 0; k < n; ++k)
            ASSERT_EQ(DE_RING_AT(&r, u32, k), i + 1u - n + k);
    }
    EXPECT_EQ(r.dropped, 1000u - 4u);
    u32 v = 0;
    ASSERT_TRUE(DE_RING_POP(&r, u32, &v));
    EXPECT_EQ(v, 996u);
    ASSERT_TRUE(DE_RING_PUSH(&r, u32, 1000u)); /* not full: nothing dropped */
    EXPECT_EQ(r.dropped, 1000u - 4u);
    EXPECT_EQ(*(u32 *)de_ring_peek(&r), 997u);
    EXPECT_EQ(*(u32 *)de_ring_peek_newest(&r), 1000u);

    u32 one;
    de_ring r1;
    ASSERT_EQ(de_ring_init_buffer(&r1, &one, sizeof(u32), 1, true), DE_OK);
    ASSERT_TRUE(DE_RING_PUSH(&r1, u32, 5u));
    ASSERT_TRUE(DE_RING_PUSH(&r1, u32, 6u));
    EXPECT_EQ(de_ring_count(&r1), 1u);
    ASSERT_TRUE(DE_RING_POP(&r1, u32, &v));
    EXPECT_EQ(v, 6u);
    EXPECT_FALSE(DE_RING_POP(&r1, u32, &v));
    de_ring_free(&r1); /* caller storage: nothing to free */
    de_ring_free(&r);
}
