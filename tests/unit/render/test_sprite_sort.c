#include "de_test.h"
#include "de_alloc_guard.h"
#include <math.h>
#include <string.h>
#include "render/sprite/sprite_sort.h"
#include "render/passes/forward/forward.h"
#include <dengine/de_platform.h>
#include <stdlib.h>
#include <stdio.h>

TEST(sprite_sort, key_order) {
    ASSERT_LT(de__sprite_sort_key(-1, 300, 1), de__sprite_sort_key(0, -300, 100));
    ASSERT_LT(de__sprite_sort_key(1, -1, 1), de__sprite_sort_key(1, 0, 100));
    ASSERT_LT(de__sprite_sort_key(0, 0, 100), de__sprite_sort_key(0, 0, 1));
    ASSERT_LT(de__sprite_sort_key(0, 0, 1), de__sprite_sort_key(0, 0, -1));
    ASSERT_EQ(de__sprite_sort_key(0, 0, -0.f), de__sprite_sort_key(0, 0, 0.f));
}
TEST(sprite_sort, stable_thousand_frames) {
    enum { N = 128 };
    de__transparent_item items[N], scratch[N];
    for (u32 frame = 0; frame < 1000; ++frame) {
        for (u32 i = 0; i < N; ++i)
            items[i] = (de__transparent_item){.entity = 1 + (i + frame) % N, .depth = 10};
        de__transparent_sort(items, N, scratch);
        for (u32 i = 0; i < N; ++i)
            ASSERT_EQ(items[i].entity, i + 1u);
    }
}
TEST(sprite_sort, mixed_meshes) {
    de__transparent_item a[5] = {{.entity = 1, .depth = 1, .layer = -1},
                                 {.entity = 2, .depth = 100, .layer = 1},
                                 {.entity = 3, .depth = 20}, /* mesh */
                                 {.entity = 4, .depth = 1, .order = 1},
                                 {.entity = 5, .depth = 30}},
                         scratch[5];
    de__transparent_sort(a, 5, scratch);
    const u64 expected[5] = {1, 5, 3, 4, 2};
    for (u32 i = 0; i < 5; ++i)
        ASSERT_EQ(a[i].entity, expected[i]);
}
TEST(sprite_sort, bench_fifty_thousand) {
    const u32 n = 50000;
    de__transparent_item *items = calloc(n, sizeof *items), *scratch = calloc(n, sizeof *scratch);
    ASSERT_TRUE(items != NULL && scratch != NULL);
    u64 elapsed = 0;
    for (u32 trial = 0; trial < 20; ++trial) {
        for (u32 i = 0; i < n; ++i)
            items[i] = (de__transparent_item){.entity = n - i,
                                              .layer = (i16)((i * 71) % 11 - 5),
                                              .order = (i16)((i * 13) % 17 - 8),
                                              .depth = (f32)((i * 1709) % 100000) / 100};
        u64 t = de_time_now_ns();
        de__transparent_sort(items, n, scratch);
        elapsed += de_time_now_ns() - t;
        for (u32 i = 1; i < n; ++i)
            ASSERT_FALSE(de__transparent_before(&items[i], &items[i - 1]));
    }
    double ms = (double)elapsed / 20e6;
    printf("50k mixed layer/order/depth sprites: %.4f ms average (20 trials)\n", ms);
    free(items);
    free(scratch);
    EXPECT_TRUE(ms < 1.0);
}

static int reference_order(const void *pa, const void *pb) {
    const de__transparent_item *a = pa, *b = pb;
    if (de__transparent_before(a, b))
        return -1;
    if (de__transparent_before(b, a))
        return 1;
    /* Unique original positions make qsort an independent stable reference. */
    return a->instance < b->instance ? -1 : a->instance > b->instance;
}
static u32 sort_random(u32 *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}
TEST(sprite_sort, full_comparator_payload_stability_and_scratch_bounds) {
    const u32 sizes[] = {0, 1, 2, 31, 32, 33, 128, 4097, 50000};
    const f32 depths[] = {NAN, INFINITY, -INFINITY, -0.f, 0.f, 1.f, -1.f, 1e30f};
    u32 random = 473;
    for (u32 size = 0; size < sizeof sizes / sizeof sizes[0]; ++size)
        for (u32 mode = 0; mode < 4; ++mode) {
            u32 n = sizes[size];
            de__transparent_item *storage = calloc(n + 2, sizeof *storage);
            de__transparent_item *temporary = calloc(n + 2, sizeof *temporary);
            de__transparent_item *expected = calloc(n ? n : 1, sizeof *expected);
            ASSERT_TRUE(storage && temporary && expected);
            memset(storage, 0xa5, sizeof *storage);
            memset(storage + n + 1, 0x5a, sizeof *storage);
            memset(temporary, 0xa5, sizeof *temporary);
            memset(temporary + n + 1, 0x5a, sizeof *temporary);
            de__transparent_item *items = storage + 1, *scratch = temporary + 1;
            for (u32 i = 0; i < n; ++i) {
                u32 value = sort_random(&random);
                items[i] = (de__transparent_item){
                    .depth = depths[(value >> 16) % 8],
                    .instance = i,
                    .entity = ((u64)(mode == 3 ? 7 : (value >> 8) % 4) << 32) | (value % 16),
                    .index = mode == 3 ? i : (value >> 20) % 8,
                    .flags = value,
                    .participant = i % 3,
                    .participant_item = i * 17,
                    .always_on_top = mode == 1 || (mode == 2 && (i & 1)),
                    .meshlet_first = i * 7,
                    .meshlet_count = i + 1,
                    .layer = (i16)(value >> 8),
                    .order = (i16)(value >> 24),
                };
                /* Dense equal primary keys exercise generation/index ties. */
                if (i & 1)
                    items[i].layer = items[i].order = 0;
            }
            memcpy(expected, items, (size_t)n * sizeof *items);
            qsort(expected, n, sizeof *expected, reference_order);
            de_alloc_guard guard = {0};
            de_alloc_guard_begin(&guard);
            de__transparent_sort(items, n, scratch);
            de_alloc_guard_end(&guard);
            DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
            EXPECT_MEMEQ(items, expected, (size_t)n * sizeof *items);
            for (u32 byte = 0; byte < sizeof *storage; ++byte) {
                EXPECT_EQ(((u8 *)storage)[byte], 0xa5u);
                EXPECT_EQ(((u8 *)(storage + n + 1))[byte], 0x5au);
                EXPECT_EQ(((u8 *)temporary)[byte], 0xa5u);
                EXPECT_EQ(((u8 *)(temporary + n + 1))[byte], 0x5au);
            }
            free(storage);
            free(temporary);
            free(expected);
        }
}

TEST(sprite_sort, separated_tie_runs_and_exact_equal_keys_keep_payload_order) {
    enum { N = 511 };
    const u32 groups[] = {7, 29}; /* Large radix runs and small insertion runs. */
    de__transparent_item *items = calloc(N, sizeof *items);
    de__transparent_item *scratch = calloc(N, sizeof *scratch);
    de__transparent_item *expected = calloc(N, sizeof *expected);
    ASSERT_TRUE(items && scratch && expected);
    for (u32 mode = 0; mode < 2; ++mode) {
        for (u32 i = 0; i < N; ++i)
            items[i] = (de__transparent_item){.depth = (f32)(i % groups[mode]),
                                              .entity = ((u64)(i % 2) << 32) | (i % 3),
                                              .index = (i / 3) % 2,
                                              .instance = i,
                                              .flags = i * 17,
                                              .participant_item = i * 31,
                                              .always_on_top = (i / 2) % 2};
        memcpy(expected, items, N * sizeof *items);
        qsort(expected, N, sizeof *expected, reference_order);
        de_alloc_guard guard = {0};
        de_alloc_guard_begin(&guard);
        de__transparent_sort(items, N, scratch);
        de_alloc_guard_end(&guard);
        DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
        EXPECT_MEMEQ(items, expected, N * sizeof *items);
    }
    free(expected);
    free(scratch);
    free(items);
}
