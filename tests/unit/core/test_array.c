/* Unit tests: de_array (DENG-151). */
#include "de_test.h"

#include "core/de_array.h"
#include "test_counting_alloc.h"

#include <stdlib.h>
#include <string.h>

static u64 arr_rng_next(u64 *s) {
    /* xorshift64*: deterministic, good enough to pick operations. */
    u64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

typedef struct arr_big {
    u64 a, b, c;
} arr_big;

TEST(array, push_pop) {
    test_counting_alloc ca;
    test_counting_init(&ca);
    DE_ARRAY(u32) arr;
    DE_ARRAY_INIT(&arr, u32, &ca.iface);
    EXPECT_EQ(arr.count, 0u);
    EXPECT_EQ(arr.capacity, 0u);
    EXPECT_EQ(test_counting_calls(&ca), 0u); /* init allocates nothing */

    const u32 n = 1000000u;
    u64 grows = 0;
    for (u32 i = 0; i < n; ++i) {
        const u32 cap_before = arr.capacity;
        const u64 calls_before = test_counting_calls(&ca);
        ASSERT_EQ(DE_ARRAY_PUSH(&arr, u32, i * 7u + 1u), DE_OK);
        /* An allocation happens exactly when the array has to grow. */
        if (cap_before == i) {
            ASSERT_EQ(test_counting_calls(&ca), calls_before + 1u);
            ASSERT_TRUE(arr.capacity >= cap_before + cap_before / 2u);
            ++grows;
        } else {
            ASSERT_EQ(test_counting_calls(&ca), calls_before);
        }
    }
    EXPECT_EQ(arr.count, n);
    EXPECT_LT(grows, 40u); /* x1.5 growth: about log1.5(1e6 / 4) = 31 reallocs */
    for (u32 i = 0; i < n; ++i)
        ASSERT_EQ(DE_ARRAY_AT(&arr, u32, i), i * 7u + 1u);
    EXPECT_EQ(DE_ARRAY_LAST(&arr, u32), (n - 1u) * 7u + 1u);
    EXPECT_EQ(DE_ARRAY_DATA(&arr, u32)[123], 123u * 7u + 1u);

    for (u32 i = n; i-- > 0;) {
        u32 v = 0;
        ASSERT_TRUE(DE_ARRAY_POP(&arr, u32, &v));
        ASSERT_EQ(v, i * 7u + 1u);
    }
    u32 dummy = 0;
    EXPECT_FALSE(DE_ARRAY_POP(&arr, u32, &dummy));
    EXPECT_FALSE(de_array_pop(&arr, NULL));
    EXPECT_GT(arr.capacity, 0u); /* pop never shrinks */

    /* clear keeps the memory; pushing up to the old capacity allocates nothing. */
    const u64 calls = test_counting_calls(&ca);
    const u32 cap = arr.capacity;
    for (u32 i = 0; i < cap; ++i)
        ASSERT_EQ(DE_ARRAY_PUSH(&arr, u32, i), DE_OK);
    de_array_clear(&arr);
    EXPECT_EQ(arr.count, 0u);
    EXPECT_EQ(arr.capacity, cap);
    EXPECT_EQ(test_counting_calls(&ca), calls);

    de_array_free(&arr);
    EXPECT_EQ(arr.data, NULL);
    EXPECT_EQ(arr.capacity, 0u);
    EXPECT_EQ(ca.live_blocks, 0u);
    EXPECT_EQ(ca.live_bytes, 0u);
    EXPECT_EQ(ca.size_errors, 0u);
    EXPECT_EQ(ca.align_errors, 0u);

    /* The array is reusable after free; free of an empty array does nothing. */
    ASSERT_EQ(DE_ARRAY_PUSH(&arr, u32, 5u), DE_OK);
    de_array_free(&arr);
    de_array_free(&arr);
    EXPECT_EQ(ca.live_blocks, 0u);

    de_array zero = {0};
    de_array_free(&zero);

    /* elem_align 0: the largest power of two (up to 16) that divides the size. */
    de_array x;
    de_array_init(&x, &ca.iface, 12, 0, DE_MEM_GENERAL);
    EXPECT_EQ(x.elem_align, 4u);
    de_array_init(&x, &ca.iface, 48, 0, DE_MEM_GENERAL);
    EXPECT_EQ(x.elem_align, 16u);
    de_array_init(&x, &ca.iface, 3, 0, DE_MEM_GENERAL);
    EXPECT_EQ(x.elem_align, 1u);
    ASSERT_EQ(de_array_push(&x, "abc"), DE_OK);
    EXPECT_MEMEQ(de_array_at(&x, 0), "abc", 3);
    de_array_free(&x);
    EXPECT_EQ(ca.live_blocks, 0u);
}

TEST(array, insert_remove) {
    test_counting_alloc ca;
    test_counting_init(&ca);
    de_array arr;
    DE_ARRAY_INIT(&arr, arr_big, &ca.iface);
    EXPECT_EQ(arr.elem_align, (u32)DE_ALIGNOF(arr_big));

    enum { MAX = 4096 };
    arr_big *ref = (arr_big *)malloc(sizeof(arr_big) * MAX);
    ASSERT_TRUE(ref != NULL);
    u32 ref_n = 0;
    u64 rng = 0x9E3779B97F4A7C15ull;

    for (u32 op = 0; op < 200000u; ++op) {
        const u64 r = arr_rng_next(&rng);
        const u32 kind = (u32)(r % 6u);
        const u64 tagv = r >> 8;
        if ((kind <= 1 && ref_n < MAX) || ref_n == 0) {
            const u32 at = ref_n ? (u32)((r >> 32) % (ref_n + 1u)) : 0u;
            const arr_big v = {tagv, ~tagv, op};
            ASSERT_EQ(DE_ARRAY_INSERT(&arr, arr_big, at, v), DE_OK);
            memmove(ref + at + 1, ref + at, sizeof(arr_big) * (ref_n - at));
            ref[at] = v;
            ++ref_n;
        } else if (kind == 2) {
            const u32 at = (u32)((r >> 32) % ref_n);
            de_array_remove_swap(&arr, at);
            ref[at] = ref[ref_n - 1u];
            --ref_n;
        } else if (kind == 3) {
            const u32 at = (u32)((r >> 32) % ref_n);
            de_array_remove_ordered(&arr, at);
            memmove(ref + at, ref + at + 1, sizeof(arr_big) * (ref_n - at - 1u));
            --ref_n;
        } else if (kind == 4 && ref_n < MAX) {
            const arr_big v = {tagv, tagv ^ 1u, op};
            ASSERT_EQ(de_array_push(&arr, &v), DE_OK);
            ref[ref_n++] = v;
        } else {
            arr_big v;
            ASSERT_TRUE(de_array_pop(&arr, &v));
            ASSERT_MEMEQ(&v, &ref[ref_n - 1u], sizeof v);
            --ref_n;
        }
        ASSERT_EQ(arr.count, ref_n);
        if ((op & 1023u) == 0 && ref_n)
            ASSERT_MEMEQ(arr.data, ref, sizeof(arr_big) * ref_n);
    }
    if (ref_n)
        ASSERT_MEMEQ(arr.data, ref, sizeof(arr_big) * ref_n);

    /* Insert at both ends, NULL inserts zeroes, push_n, resize. */
    de_array_clear(&arr);
    ASSERT_EQ(DE_ARRAY_INSERT(&arr, arr_big, 0, (arr_big){2, 2, 2}), DE_OK);
    ASSERT_EQ(DE_ARRAY_INSERT(&arr, arr_big, 0, (arr_big){1, 1, 1}), DE_OK);
    ASSERT_EQ(DE_ARRAY_INSERT(&arr, arr_big, 2, (arr_big){3, 3, 3}), DE_OK);
    ASSERT_EQ(de_array_insert(&arr, 1, NULL), DE_OK);
    EXPECT_EQ(DE_ARRAY_AT(&arr, arr_big, 0).a, 1u);
    EXPECT_EQ(DE_ARRAY_AT(&arr, arr_big, 1).a, 0u);
    EXPECT_EQ(DE_ARRAY_AT(&arr, arr_big, 2).a, 2u);
    EXPECT_EQ(DE_ARRAY_AT(&arr, arr_big, 3).a, 3u);
    const arr_big three[3] = {{7, 0, 0}, {8, 0, 0}, {9, 0, 0}};
    ASSERT_EQ(de_array_push_n(&arr, three, 3), DE_OK);
    EXPECT_EQ(arr.count, 7u);
    EXPECT_EQ(DE_ARRAY_AT(&arr, arr_big, 6).a, 9u);
    ASSERT_EQ(de_array_resize(&arr, 100), DE_OK);
    EXPECT_EQ(DE_ARRAY_AT(&arr, arr_big, 99).a, 0u);
    EXPECT_EQ(DE_ARRAY_AT(&arr, arr_big, 6).a, 9u);
    ASSERT_EQ(de_array_resize(&arr, 2), DE_OK);
    EXPECT_EQ(arr.count, 2u);
    de_array_remove_swap(&arr, 1); /* removing the last element by swap */
    EXPECT_EQ(arr.count, 1u);
    de_array_remove_ordered(&arr, 0);
    EXPECT_EQ(arr.count, 0u);

    /* reserve allocates once and never shrinks. */
    const u64 calls = test_counting_calls(&ca);
    ASSERT_EQ(de_array_reserve(&arr, 10000), DE_OK);
    EXPECT_EQ(arr.capacity, 10000u);
    ASSERT_EQ(de_array_reserve(&arr, 10), DE_OK);
    EXPECT_EQ(arr.capacity, 10000u);
    EXPECT_EQ(test_counting_calls(&ca), calls + 1u);

    free(ref);
    de_array_free(&arr);
    EXPECT_EQ(ca.live_blocks, 0u);
    EXPECT_EQ(ca.size_errors, 0u);
    EXPECT_EQ(ca.align_errors, 0u);
}

TEST(array, oom) {
    /* The null allocator: every growth fails cleanly with DE_ERR_OUT_OF_MEMORY. */
    de_array arr;
    DE_ARRAY_INIT(&arr, u64, de_allocator_null());
    de_error_clear();
    EXPECT_EQ(DE_ARRAY_PUSH(&arr, u64, 1u), DE_ERR_OUT_OF_MEMORY);
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(arr.count, 0u);
    EXPECT_EQ(arr.capacity, 0u);
    EXPECT_EQ(arr.data, NULL);
    EXPECT_EQ(de_array_insert(&arr, 0, NULL), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_array_reserve(&arr, 16), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_array_resize(&arr, 16), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_array_push_n(&arr, NULL, 3), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(arr.count, 0u);
    de_array_free(&arr); /* nothing to free */

    /* A failure mid-way leaves the array as it was, contents included. */
    test_counting_alloc ca;
    test_counting_init(&ca);
    DE_ARRAY_INIT(&arr, u32, &ca.iface);
    ca.fail_after = 3;
    u32 pushed = 0;
    de_result r = DE_OK;
    while ((r = DE_ARRAY_PUSH(&arr, u32, pushed)) == DE_OK)
        ++pushed;
    EXPECT_EQ(r, DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(arr.count, pushed);
    EXPECT_EQ(arr.capacity, pushed); /* full; the grow failed */
    for (u32 i = 0; i < pushed; ++i)
        ASSERT_EQ(DE_ARRAY_AT(&arr, u32, i), i);
    EXPECT_EQ(DE_ARRAY_INSERT(&arr, u32, 0, 99u), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(DE_ARRAY_AT(&arr, u32, 0), 0u);
    EXPECT_EQ(arr.count, pushed);
    ca.fail_after = -1;
    EXPECT_EQ(DE_ARRAY_PUSH(&arr, u32, pushed), DE_OK);
    de_array_free(&arr);
    EXPECT_EQ(ca.live_blocks, 0u);
    EXPECT_EQ(ca.size_errors, 0u);
    de_error_clear();
}

TEST(array, typed_pointer_elems) {
    /* The typed macros work with pointer and qualified element types. */
    test_counting_alloc ca;
    test_counting_init(&ca);
    DE_ARRAY(const char *) names;
    DE_ARRAY_INIT(&names, const char *, &ca.iface);
    ASSERT_EQ(DE_ARRAY_PUSH(&names, const char *, "b"), DE_OK);
    ASSERT_EQ(DE_ARRAY_INSERT(&names, const char *, 0, "a"), DE_OK);
    EXPECT_STREQ(DE_ARRAY_AT(&names, const char *, 0), "a");
    EXPECT_STREQ(DE_ARRAY_LAST(&names, const char *), "b");
    const char *out = NULL;
    ASSERT_TRUE(DE_ARRAY_POP(&names, const char *, &out));
    EXPECT_STREQ(out, "b");
    de_array_free(&names);
    EXPECT_EQ(ca.live_blocks, 0u);
}
