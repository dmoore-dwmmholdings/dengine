#include "runtime_test_util.h"
#include "text/shape/shape_state.h"
#include "text/glyph_cache_internal.h"
#include <string.h>
static de__hb_font *cached_font(de_text_system *text, de_font handle) {
    for (u32 i = 0; i < text->shape->font_count; ++i)
        if (text->shape->fonts[i].handle.id == handle.id)
            return &text->shape->fonts[i];
    return NULL;
}
TEST_F(shape_cache, atlas_publication_reuses_immutable_harfbuzz_font, runtime_fx) {
    de_text_font *decoded = de_text__font_get(f->t, f->cjk);
    ASSERT_TRUE(decoded != NULL && decoded->source_size != 0);
    u32 glyph = 0;
    for (u32 cp = 0x4e00; cp < 0x4e00 + 550 && !glyph; ++cp) {
        u32 candidate = de_text__codepoint(decoded, cp);
        bool baked = false;
        for (u32 i = 0; i < decoded->count; ++i)
            if (decoded->glyphs[i].index == candidate)
                baked = true;
        if (candidate && !baked)
            glyph = candidate;
    }
    ASSERT_TRUE(glyph != 0);
    de_text_style style = {.font = f->cjk, .size_px = 48};
    de_shaped_run first = {0}, second = {0};
    ASSERT_EQ(de_text__shape(f->t, DE_STR("漢"), &style, DE_DIR_LTR, NULL, &first), DE_OK);
    de__hb_font *slot = cached_font(f->t, f->cjk);
    if (!slot)
        de_text__shape_free(&first);
    ASSERT_TRUE(slot != NULL);
    // Keep the old object alive so allocator address reuse cannot fake a cache hit.
    hb_font_t *retained = hb_font_reference(slot->font);
    u32 generation = decoded->generation;
    u64 revision = decoded->identity;
    u64 generated = de_text_get_stats(f->t).runtime_generated;
    de_text__glyph_miss(f->t, f->cjk, glyph);
    de_text__glyph_jobs_wait(f->t);
    de_text_update(f->t);
    bool published = de_text_get_stats(f->t).runtime_generated > generated &&
                     decoded->generation != generation && decoded->identity == revision;
    de_result result = de_text__shape(f->t, DE_STR("漢"), &style, DE_DIR_LTR, NULL, &second);
    bool reused = slot->font == retained;
    bool same =
        result == DE_OK && first.count == second.count &&
        (!first.count || !memcmp(first.glyphs, second.glyphs, first.count * sizeof *first.glyphs));
    hb_font_destroy(retained);
    de_text__shape_free(&first);
    de_text__shape_free(&second);
    EXPECT_TRUE(published);
    EXPECT_EQ(result, DE_OK);
    EXPECT_TRUE(reused);
    EXPECT_TRUE(same);
}
TEST_F(shape_cache, changed_decoded_revision_rebuilds_at_same_address, runtime_fx) {
    de_text_style style = {.font = f->latin, .size_px = 48};
    de_shaped_run first = {0}, second = {0};
    ASSERT_EQ(de_text__shape(f->t, DE_STR("office"), &style, DE_DIR_LTR, NULL, &first), DE_OK);
    de__hb_font *slot = cached_font(f->t, f->latin);
    de_text_font *decoded = de_text__font_get(f->t, f->latin);
    if (!slot || !decoded)
        de_text__shape_free(&first);
    ASSERT_TRUE(slot != NULL && decoded != NULL);
    hb_font_t *retained = hb_font_reference(slot->font);
    u64 revision = decoded->identity;
    // Model a new decoded revision at the same address; no atlas generation is edited.
    de__text_lock(f->t->runtime);
    decoded->identity = revision ^ (1ULL << 63);
    de__text_unlock(f->t->runtime);
    // Different text forces a shaping miss independently of cache-key epoch behavior.
    de_result result = de_text__shape(f->t, DE_STR("office!"), &style, DE_DIR_LTR, NULL, &second);
    bool rebuilt = slot->font != retained;
    // Restore the fixture's decoded revision before assertions or teardown.
    de__text_lock(f->t->runtime);
    decoded->identity = revision;
    de__text_unlock(f->t->runtime);
    hb_font_destroy(retained);
    de_text__shape_free(&first);
    de_text__shape_free(&second);
    EXPECT_EQ(result, DE_OK);
    EXPECT_TRUE(rebuilt);
}
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
        ASSERT_EQ(
            de_thread_create(shape_thread, &c, "shape test", DE_THREAD_NORMAL, 0, &threads[i]),
            DE_OK);
    for (u32 i = 0; i < 4; ++i)
        EXPECT_EQ(de_thread_join(threads[i]), 0);
}

TEST_F(shape_cache, immutable_source_is_shared_without_copying, runtime_fx) {
    de_text_style style = {.font = f->latin, .size_px = 48};
    de_shaped_run run;
    ASSERT_EQ(de_text__shape(f->t, DE_STR("office"), &style, DE_DIR_LTR, NULL, &run), DE_OK);
    de_text__shape_free(&run);
    ASSERT_EQ(f->t->shape->font_count, 1u);
    hb_font_t *font = f->t->shape->fonts[0].font;
    hb_blob_t *blob = hb_face_reference_blob(hb_font_get_face(font));
    unsigned size = 0;
    de_text_font *decoded = de_text__font_get(f->t, f->latin);
    EXPECT_TRUE(hb_blob_get_data(blob, &size) == (const char *)decoded->source);
    EXPECT_EQ(size, decoded->source_size);
    hb_blob_destroy(blob);
}
