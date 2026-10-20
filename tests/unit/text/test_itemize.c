#include "runtime_test_util.h"
#include "text/shape/shaper_internal.h"
TEST_F(itemize, font_script_and_invalid_utf8, runtime_fx) {
    de_text_item *items;
    u32 count;
    const de_allocator *a = de_heap_allocator();
    de_str text = DE_STR("Hello 世界");
    ASSERT_EQ(de_text__itemize(f->t, f->latin, text, a, &items, &count), DE_OK);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(items[0].font.id, f->latin.id);
    EXPECT_EQ(items[0].end, 6u);
    EXPECT_EQ(items[1].font.id, f->cjk.id);
    EXPECT_EQ(items[1].script, HB_SCRIPT_HAN);
    DE_FREE(a, items, text.len * sizeof *items);
    u32 at = 0;
    EXPECT_EQ(de__text_utf8("\xff", 1, &at), 0xfffdu);
    EXPECT_EQ(at, 1u);
    at = 0;
    EXPECT_EQ(de__text_utf8("\xf0\x80\x80\x80", 4, &at), 0xfffdu);
    EXPECT_EQ(at, 1u);
}
TEST_F(itemize, common_inherited_merge, runtime_fx) {
    de_text_item *items;
    u32 count;
    de_str text = DE_STR("(Hello!)");
    ASSERT_EQ(de_text__itemize(f->t, f->latin, text, de_heap_allocator(), &items, &count), DE_OK);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(items[0].script, HB_SCRIPT_LATIN);
    DE_FREE(de_heap_allocator(), items, text.len * sizeof *items);
}

TEST_F(itemize, inherited_marks_follow_fallback_base_when_covered, runtime_fx) {
    de_asset_id id = de_asset_id_from("font/mark-primary");
    de_font primary = de_asset_font(f->assets, id);
    de_load_handle load = de_asset_load(f->assets, id, 0);
    ASSERT_EQ(de_asset_wait(f->assets, load, 60000000000ull), DE_LOAD_READY);
    de_text_set_fallbacks(f->t, primary, &f->latin, 1);
    de_font resolved;
    u32 glyph;
    ASSERT_TRUE(de_text__resolve(f->t, primary, 'A', &resolved, &glyph));
    ASSERT_EQ(resolved.id, f->latin.id);
    ASSERT_TRUE(de_text__resolve(f->t, primary, 0x301, &resolved, &glyph));
    ASSERT_EQ(resolved.id, primary.id);
    ASSERT_TRUE(de_text__codepoint(de_text__font_get(f->t, f->latin), 0x301) != 0);
    de_str text = DE_STR("A\xcc\x81\xcc\x80");
    de_text_item *items;
    u32 count;
    ASSERT_EQ(de_text__itemize(f->t, primary, text, de_heap_allocator(), &items, &count), DE_OK);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(items[0].font.id, f->latin.id);
    EXPECT_EQ(items[0].script, HB_SCRIPT_LATIN);
    EXPECT_EQ(items[0].end, text.len);
    DE_FREE(de_heap_allocator(), items, text.len * sizeof *items);
    de_text_style style = {.font = primary, .size_px = 48};
    de_shaped_run actual, reference;
    ASSERT_EQ(de_text__shape(f->t, text, &style, DE_DIR_LTR, NULL, &actual), DE_OK);
    style.font = f->latin;
    ASSERT_EQ(de_text__shape(f->t, text, &style, DE_DIR_LTR, NULL, &reference), DE_OK);
    ASSERT_EQ(actual.count, reference.count);
    for (u32 i = 0; i < actual.count; ++i) {
        EXPECT_EQ(actual.glyphs[i].font.id, f->latin.id);
        EXPECT_EQ(actual.glyphs[i].glyph, reference.glyphs[i].glyph);
        EXPECT_EQ(actual.glyphs[i].cluster, reference.glyphs[i].cluster);
        EXPECT_FLOAT_EQ(actual.glyphs[i].x_advance, reference.glyphs[i].x_advance, 0);
        EXPECT_FLOAT_EQ(actual.glyphs[i].x_offset, reference.glyphs[i].x_offset, 0);
        EXPECT_FLOAT_EQ(actual.glyphs[i].y_offset, reference.glyphs[i].y_offset, 0);
    }
    de_text__shape_free(&actual);
    de_text__shape_free(&reference);
    /* A fallback lacking the mark must not steal it from a font that covers it. */
    de_text_set_fallbacks(f->t, primary, &f->cjk, 1);
    text = DE_STR("漢\xcc\x81");
    ASSERT_EQ(de_text__itemize(f->t, primary, text, de_heap_allocator(), &items, &count), DE_OK);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(items[0].font.id, f->cjk.id);
    EXPECT_EQ(items[1].font.id, primary.id);
    DE_FREE(de_heap_allocator(), items, text.len * sizeof *items);
    de_asset_release(f->assets, load);
}
