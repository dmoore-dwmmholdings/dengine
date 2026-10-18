#include "font_test_util.h"
static const char text[] =
    "info face=\"Test\" size=8\ncommon lineHeight=8 base=7 scaleW=16 scaleH=8 pages=1\npage id=0 "
    "file=\"pixel.png\"\nchars count=2\nchar id=66 x=8 y=0 width=8 height=8 xoffset=0 yoffset=0 "
    "xadvance=7 page=0 chnl=15\nchar id=65 x=0 y=0 width=8 height=8 xoffset=0 yoffset=0 xadvance=8 "
    "page=0 chnl=15\nkernings count=1\nkerning first=65 second=66 amount=-1\n";
TEST(bmfont, text_metrics_and_errors) {
    de_arena *ar = NULL;
    ASSERT_EQ(de_arena_create(NULL, &ar), DE_OK);
    de_allocator a = de_arena_allocator(ar);
    de__bmfont b;
    ASSERT_EQ(de__bmfont_parse(text, sizeof text - 1, &a, &b), DE_OK);
    ASSERT_EQ(b.count, 2u);
    EXPECT_EQ(b.glyphs[0].index, 65u);
    EXPECT_EQ(b.glyphs[1].x, 8u);
    EXPECT_FLOAT_EQ(b.kerns[0].amount, -1, .001);
    EXPECT_EQ(de__bmfont_parse("BMF\3x", 5, &a, &b), DE_ERR_PARSE);
    EXPECT_EQ(de__bmfont_parse("info size=hi\n", 13, &a, &b), DE_ERR_PARSE);
    de_arena_destroy(ar);
}
TEST(bmfont, text_binary_identical_cooks) {
    de__manifest_kv s = {.key = "mode", .value = {.type = DE__MANIFEST_STRING, .u.s = "bitmap"}};
    de_cook_result a = {0}, b = {0};
    ASSERT_EQ(font_cook("pixel.fnt", &s, 1, &a), DE_OK);
    ASSERT_EQ(font_cook("pixel_binary.fnt", &s, 1, &b), DE_OK);
    ASSERT_EQ(a.size, b.size);
    ASSERT_MEMEQ(a.data, b.data, (size_t)a.size);
    de_cook_result_free(&a);
    de_cook_result_free(&b);
}
