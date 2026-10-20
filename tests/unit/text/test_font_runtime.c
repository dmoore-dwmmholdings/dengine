#include "runtime_test_util.h"
#include <hb-ot.h>
TEST_F(font_runtime, loader_metrics_and_dirty, runtime_fx) {
    de_text_font *font = de_text__font_get(f->t, f->latin);
    ASSERT_TRUE(font != NULL);
    EXPECT_EQ(font->mode, 0u);
    EXPECT_EQ(font->dirty_count, font->pages);
    de_font_metrics m;
    ASSERT_EQ(de_text_font_metrics(f->t, f->latin, 48, &m), DE_OK);
    EXPECT_TRUE(m.ascent > 24);
    u32 gid = de_text__codepoint(font, 'A');
    de_text_glyph_entry g;
    ASSERT_TRUE(de_text__glyph(f->t, f->latin, gid, &g));
    EXPECT_FALSE(g.missing);
    EXPECT_TRUE(g.advance > 0);
    EXPECT_TRUE(g.uv.z >= g.uv.x);
    EXPECT_TRUE(g.uv.w >= g.uv.y);
}
TEST(font_runtime, not_ready_safe) {
    de_asset_system *a = NULL;
    de_text_system *t = NULL;
    ASSERT_EQ(de_asset_system_create(NULL, &a), DE_OK);
    ASSERT_EQ(de_text_create(NULL, a, &t), DE_OK);
    u32 id;
    ASSERT_EQ(de__slot_create(a, de_asset_id_from("unready"), de_hash64_str("font"), &id), DE_OK);
    de_text_glyph_entry g;
    EXPECT_FALSE(de_text__glyph(t, (de_font){id}, 12, &g));
    EXPECT_TRUE(g.missing);
    de_font font;
    u32 gid;
    EXPECT_FALSE(de_text__resolve(t, (de_font){id}, 'A', &font, &gid));
    EXPECT_EQ(gid, 0u);
    de_asset_system_destroy(a);
    de_text_destroy(t);
}
TEST_F(font_runtime, reload_generation_and_eviction, runtime_fx) {
    de_font out;
    u32 glyph;
    ASSERT_TRUE(de_text__resolve(f->t, f->latin, 'A', &out, &glyph));
    de_text_font *old = de_text__font_get(f->t, f->latin);
    u32 generation = old->generation;
    de__slot *slot = de__slot_get(f->assets, f->latin.id);
    de__slot_set_object(slot, NULL);
    de_text_update(f->t);
    EXPECT_FALSE(de_text__resolve(f->t, f->latin, 'A', &out, &glyph));
    de__slot_set_object(slot, old);
    de_text_update(f->t);
    EXPECT_TRUE(de_text__resolve(f->t, f->latin, 'A', &out, &glyph));
    EXPECT_GT(old->generation, generation);
}
TEST(font_runtime, decode_corruption) {
    u8 bad[64] = {0};
    memcpy(bad, "DFNT", 4);
    de_text_font *f = NULL;
    EXPECT_EQ(de_text__font_decode(NULL, bad, sizeof bad, &f), DE_ERR_PARSE);
    EXPECT_TRUE(f == NULL);
}
TEST(font_runtime, bitmap_pixel_decode) {
    for (u32 mode = 0; mode < 2; ++mode) {
        de__manifest_kv settings[] = {
            {.key = "mode",
             .value = {.type = DE__MANIFEST_STRING, .u.s = mode ? "pixel" : "bitmap"}},
            {.key = "map", .value = {.type = DE__MANIFEST_STRING, .u.s = "pixel.toml"}}};
        de_cook_result r = {0};
        ASSERT_EQ(font_cook(mode ? "pixel.png" : "pixel.fnt", settings, 2, &r), DE_OK);
        de_text_font *f = NULL;
        ASSERT_EQ(de_text__font_decode(NULL, r.data, r.size, &f), DE_OK);
        EXPECT_EQ(f->mode, mode ? DE_FONT_PIXEL : DE_FONT_BITMAP);
        EXPECT_EQ(de_text__codepoint(f, 'A'), 65u);
        EXPECT_EQ(f->dirty_count, 1u);
        de_text__font_free(f);
        de_cook_result_free(&r);
    }
}
TEST_F(font_runtime, warm_lookup_no_alloc, runtime_fx) {
    de_tracking_allocator track;
    de_allocator a = de_tracking_allocator_init(&track, NULL, DE_MEM_UI);
    de_text_system *t = NULL;
    /* A second text owner must use separate asset registration; reuse the existing immutable
     * runtime font slots through an owner without assets, then attach the same read-only assets. */
    ASSERT_EQ(de_text_create(&a, NULL, &t), DE_OK);
    t->assets = f->assets;
    de_text_set_fallbacks(t, f->latin, &f->cjk, 1);
    de_font out;
    u32 gid;
    de_text_glyph_entry g;
    ASSERT_TRUE(de_text__resolve(t, f->latin, 'A', &out, &gid));
    ASSERT_TRUE(de_text__glyph(t, out, gid, &g));
    de_tracking_stats before = de_tracking_allocator_stats(&track);
    for (u32 i = 0; i < 10000; ++i) {
        ASSERT_TRUE(de_text__resolve(t, f->latin, 'A', &out, &gid));
        ASSERT_TRUE(de_text__glyph(t, out, gid, &g));
    }
    de_tracking_stats after = de_tracking_allocator_stats(&track);
    EXPECT_EQ(before.alloc_count, after.alloc_count);
    EXPECT_EQ(before.realloc_count, after.realloc_count);
    de_text_destroy(t);
    EXPECT_EQ(de_tracking_allocator_stats(&track).blocks_live, 0u);
}

TEST_F(font_runtime, real_reload_pipeline, runtime_fx) {
    de_font out;
    u32 gid;
    ASSERT_TRUE(de_text__resolve(f->t, f->latin, 'A', &out, &gid));
    de_text_font *old = de_text__font_get(f->t, f->latin);
    u32 generation = old->generation;
    de__asset_reload_on_change(f->assets, "fontall:/latin/NotoSans.ttf", DE_WATCH_MODIFIED, false);
    u64 deadline = de_time_now_ns() + 5ULL * 1000000000;
    while (de_text__font_get(f->t, f->latin) == old && de_time_now_ns() < deadline) {
        de_asset_system_update(f->assets, NULL);
        de_text_update(f->t);
    }
    de_text_font *now = de_text__font_get(f->t, f->latin);
    ASSERT_TRUE(now != NULL);
    ASSERT_TRUE(now != old);
    EXPECT_GT(now->generation, generation);
    ASSERT_TRUE(de_text__resolve(f->t, f->latin, 'A', &out, &gid));
    EXPECT_EQ(gid, de_text__codepoint(now, 'A'));
}

TEST(font_runtime, immutable_source_lease_and_decode_allocation_failures) {
    const char *ranges[] = {"65-65"};
    de__manifest_kv settings = {.key = "ranges",
                                .value = {.type = DE__MANIFEST_STRING_ARRAY, .u.arr = {ranges, 1}}};
    de_cook_result cooked = {0};
    ASSERT_EQ(font_cook("NotoSans.ttf", &settings, 1, &cooked), DE_OK);
    de_tracking_allocator tracking;
    de_allocator alloc = de_tracking_allocator_init(&tracking, NULL, DE_MEM_UI);
    for (i64 budget = 0; budget < 2; ++budget) {
        de_tracking_allocator_fail_after(&tracking, budget);
        de_text_font *font = NULL;
        EXPECT_EQ(de_text__font_decode(&alloc, cooked.data, cooked.size, &font),
                  DE_ERR_OUT_OF_MEMORY);
        EXPECT_TRUE(font == NULL);
        EXPECT_EQ(de_tracking_allocator_stats(&tracking).blocks_live, 0u);
        EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
    }
    de_tracking_allocator_fail_after(&tracking, -1);
    de_text_font *font = NULL;
    ASSERT_EQ(de_text__font_decode(&alloc, cooked.data, cooked.size, &font), DE_OK);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).blocks_live, 2u);
    de__font_source_ref(font->source_owner);
    hb_blob_t *blob =
        hb_blob_create((const char *)font->source, font->source_size, HB_MEMORY_MODE_READONLY,
                       font->source_owner, de__font_source_unref);
    unsigned bytes = 0;
    EXPECT_TRUE(hb_blob_get_data(blob, &bytes) == (const char *)font->source);
    EXPECT_EQ(bytes, font->source_size);
    de_text__font_free(font);
    /* The atlas/decoded generation is gone; the immutable source alone lives
     * until the final HarfBuzz reference is released. */
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).blocks_live, 1u);
    hb_face_t *face = hb_face_create(blob, 0);
    hb_font_t *shaping = hb_font_create(face);
    hb_ot_font_set_funcs(shaping);
    hb_buffer_t *buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, "office", 6, 0, 6);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(shaping, buffer, NULL, 0);
    EXPECT_EQ(hb_buffer_get_length(buffer), 4u);
    hb_buffer_destroy(buffer);
    hb_font_destroy(shaping);
    hb_face_destroy(face);
    hb_blob_destroy(blob);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
    de_cook_result_free(&cooked);
}
