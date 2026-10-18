#include "runtime_test_util.h"
#include "text/shape/shape_state.h"
#include <string.h>
TEST_F(shape_cache, hit_eviction_and_generation, runtime_fx) {
    de_arena *arena;
    ASSERT_EQ(de_arena_create(NULL, &arena), DE_OK);
    de_allocator a = de_arena_allocator(arena);
    de_text_style s = {.font = f->latin, .size_px = 48};
    de_shaped_run one, two;
    ASSERT_EQ(de_text__shape(f->t, DE_STR("office"), &s, DE_DIR_LTR, &a, &one), DE_OK);
    ASSERT_EQ(de_text__shape(f->t, DE_STR("office"), &s, DE_DIR_LTR, &a, &two), DE_OK);
    ASSERT_EQ(one.count, two.count);
    ASSERT_MEMEQ(one.glyphs, two.glyphs, one.count * sizeof *one.glyphs);
    de__shape_key key;
    u64 before = de__shape_hash(f->t, DE_STR("office"), &s, DE_DIR_LTR, &key);
    de_text__font_get(f->t, f->latin)->generation++;
    u64 after = de__shape_hash(f->t, DE_STR("office"), &s, DE_DIR_LTR, &key);
    EXPECT_NE(before, after);
    ASSERT_EQ(de_cvar_set(f->t->shape->capacity, "16"), DE_OK);
    for (u32 i = 0; i < 100; ++i) {
        char str[32];
        snprintf(str, sizeof str, "item %u", i);
        ASSERT_EQ(de_text__shape(f->t, (de_str){str, strlen(str)}, &s, DE_DIR_LTR, &a, &two),
                  DE_OK);
    }
    ASSERT_EQ(de_cvar_set(f->t->shape->capacity, "4096"), DE_OK);
    de_arena_destroy(arena);
}
TEST_F(shape_cache, performance_budget, runtime_fx) {
    de_arena *arena;
    ASSERT_EQ(de_arena_create(NULL, &arena), DE_OK);
    de_allocator a = de_arena_allocator(arena);
    char text[1001];
    for (u32 i = 0; i < 1000; ++i)
        text[i] = "A quick brown fox jumps over the lazy dog. "[i % 42];
    text[1000] = 0;
    de_text_style s = {.font = f->latin, .size_px = 48};
    de_shaped_run run;
    u64 start = de_time_now_ns();
    ASSERT_EQ(de_text__shape(f->t, (de_str){text, 1000}, &s, DE_DIR_LTR, &a, &run), DE_OK);
    u64 cold = de_time_now_ns() - start;
    de_arena_reset(arena);
    ASSERT_EQ(de_text__shape(f->t, (de_str){text, 100}, &s, DE_DIR_LTR, &a, &run), DE_OK);
    de_arena_reset(arena);
    start = de_time_now_ns();
    for (u32 i = 0; i < 1000; ++i) {
        ASSERT_EQ(de_text__shape(f->t, (de_str){text, 100}, &s, DE_DIR_LTR, &a, &run), DE_OK);
        de_arena_reset(arena);
    }
    u64 hit = (de_time_now_ns() - start) / 1000;
    printf("Shape1000 cold %.3fms cache100 hit %.3fus\n", (double)cold / 1e6, (double)hit / 1e3);
#if defined(DE_DEV)
    EXPECT_LT(cold, 300000u);
    EXPECT_LT(hit, 2000u);
#endif
    de_arena_destroy(arena);
}
typedef struct concurrent_shape {
    de_text_system *t;
    de_font font;
} concurrent_shape;
static i32 shape_thread(void *user) {
    concurrent_shape *c = user;
    de_arena *ar;
    if (de_arena_create(NULL, &ar) != DE_OK)
        return 1;
    de_allocator a = de_arena_allocator(ar);
    de_text_style s = {.font = c->font, .size_px = 48};
    for (u32 i = 0; i < 1000; ++i) {
        de_shaped_run r;
        if (de_text__shape(c->t, DE_STR("office AV"), &s, DE_DIR_LTR, &a, &r) != DE_OK ||
            r.count != 7) {
            de_arena_destroy(ar);
            return 2;
        }
        de_arena_reset(ar);
    }
    de_arena_destroy(ar);
    return 0;
}
TEST_F(shape_cache, concurrent_workers, runtime_fx) {
    concurrent_shape c = {f->t, f->latin};
    de_thread *threads[4];
    for (u32 i = 0; i < 4; ++i)
        ASSERT_EQ(de_thread_create(shape_thread, &c, "shape test", DE_THREAD_NORMAL, 0,
                                   &threads[i]),
                  DE_OK);
    for (u32 i = 0; i < 4; ++i)
        EXPECT_EQ(de_thread_join(threads[i]), 0);
}
