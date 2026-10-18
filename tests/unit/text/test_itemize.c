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
