#include "de_test.h"
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
