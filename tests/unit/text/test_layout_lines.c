/* test_layout_lines.c: de_text_layout_lines, the line breaks of a laid-out paragraph (DENG-782). */
#include "layout_test_util.h"
TEST_F(layout_lines, wrapped_lines_cover_the_text_in_order, runtime_fx) {
    const char *text = "The quick brown fox";
    de_text_layout *a = layout_build(f, "The quick", 0, DE_ALIGN_START, DE_DIR_LTR, false);
    de_text_layout *b = layout_build(f, "brown fox", 0, DE_ALIGN_START, DE_DIR_LTR, false);
    f32 wrap = (de_text_layout_size(a).x + de_text_layout_size(b).x) * .5f;
    de_text_layout_free(a);
    de_text_layout_free(b);
    de_text_layout *l = layout_build(f, text, wrap, DE_ALIGN_START, DE_DIR_LTR, false);
    EXPECT_EQ(de_text_layout_lines(l, NULL, 0), 3u);
    de_text_line lines[4] = {0};
    ASSERT_EQ(de_text_layout_lines(l, lines, 4), 3u);
    EXPECT_EQ(lines[0].byte_start, 0u);
    EXPECT_EQ(lines[2].byte_end, (u32)strlen(text));
    f32 widest = 0;
    for (u32 i = 0; i < 3; ++i) {
        if (i)
            EXPECT_EQ(lines[i].byte_start, lines[i - 1].byte_end);
        EXPECT_TRUE(lines[i].byte_end > lines[i].byte_start);
        EXPECT_TRUE(lines[i].width > 0);
        EXPECT_TRUE(lines[i].width <= wrap + .001f);
        EXPECT_FLOAT_EQ(lines[i].x, 0, .001f);
        EXPECT_FLOAT_EQ(lines[i].height, de_text_layout_size(l).y / 3, .001f);
        EXPECT_FLOAT_EQ(lines[i].y, lines[i].height * i, .001f);
        widest = fmaxf(widest, lines[i].x + lines[i].width);
    }
    /* A break after "The " keeps the space in the range but not in the width. */
    EXPECT_EQ(text[lines[1].byte_start - 1], ' ');
    EXPECT_FLOAT_EQ(widest, de_text_layout_size(l).x, .001f);
    /* A short buffer gets the first lines and the full count. */
    de_text_line one = {0};
    EXPECT_EQ(de_text_layout_lines(l, &one, 1), 3u);
    EXPECT_EQ(one.byte_end, lines[0].byte_end);
    de_text_layout_free(l);
}
TEST_F(layout_lines, hard_breaks_blank_lines_and_alignment, runtime_fx) {
    de_text_layout *l = layout_build(f, "ab\n\ncd", 0, DE_ALIGN_START, DE_DIR_LTR, false);
    de_text_line lines[3] = {0};
    ASSERT_EQ(de_text_layout_lines(l, lines, 3), 3u);
    EXPECT_EQ(lines[0].byte_start, 0u);
    EXPECT_EQ(lines[0].byte_end, 3u);
    EXPECT_EQ(lines[1].byte_start, 3u);
    EXPECT_EQ(lines[1].byte_end, 4u);
    EXPECT_FLOAT_EQ(lines[1].width, 0, 0);
    EXPECT_EQ(lines[2].byte_start, 4u);
    EXPECT_EQ(lines[2].byte_end, 6u);
    EXPECT_TRUE(lines[2].width > 0);
    de_text_layout_free(l);
    l = layout_build(f, "abc", 100, DE_ALIGN_END, DE_DIR_LTR, false);
    ASSERT_EQ(de_text_layout_lines(l, lines, 3), 1u);
    EXPECT_FLOAT_EQ(lines[0].x + lines[0].width, 100, .001f);
    de_text_layout_free(l);
    l = layout_build(f, "", 0, DE_ALIGN_START, DE_DIR_LTR, false);
    EXPECT_EQ(de_text_layout_lines(l, lines, 3), de_text_layout_size(l).y > 0 ? 1u : 0u);
    de_text_layout_free(l);
    EXPECT_EQ(de_text_layout_lines(NULL, lines, 3), 0u);
}
