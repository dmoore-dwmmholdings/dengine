#include "layout_test_util.h"
#include <stdio.h>
TEST_F(layout_hit, mixed_direction_centers_and_caret_edges, runtime_fx) {
    de_asset_id id = de_asset_id_from("font/hebrew");
    de_font hebrew = de_asset_font(f->assets, id);
    de_load_handle load = de_asset_load(f->assets, id, 0);
    ASSERT_EQ(de_asset_wait(f->assets, load, 30000000000ULL), (int)DE_LOAD_READY);
    de_font chain[] = {hebrew, f->cjk, f->emoji};
    de_text_set_fallbacks(f->t, f->latin, chain, 3);
    const char *mixed = "abc \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d def";
    de_text_layout *l = layout_build(f, mixed, 0, DE_ALIGN_START, DE_DIR_LTR, false);
    u32 previous = UINT32_MAX, rtl = 0;
    for (u32 i = 0; i < l->cluster_count; ++i) {
        const de__layout_cluster *c = &l->clusters[i];
        u32 hit = de_text_layout_hit_test(
            l, (de_vec2){(c->x0 + c->x1) * .5f, c->line * l->line_height + l->line_height * .5f});
        EXPECT_EQ(hit, c->start);
        de_vec4 caret = de_text_layout_caret(l, c->start);
        EXPECT_FLOAT_EQ(caret.x, c->rtl ? c->x1 : c->x0, .001f);
        if (c->rtl) {
            if (rtl)
                EXPECT_LT(c->start, previous);
            previous = c->start;
            ++rtl;
        }
    }
    EXPECT_EQ(rtl, 4u);
    for (u32 byte = 0; byte <= strlen(mixed); ++byte) {
        de_vec4 caret = de_text_layout_caret(l, byte);
        bool edge = false;
        for (u32 i = 0; i < l->cluster_count; ++i)
            if (fabsf(caret.x - l->clusters[i].x0) < .001f ||
                fabsf(caret.x - l->clusters[i].x1) < .001f)
                edge = true;
        EXPECT_TRUE(edge);
    }
    de_text_layout_free(l);
    l = layout_build(f, "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d", 100, DE_ALIGN_START, DE_DIR_AUTO,
                     false);
    f32 right = 0;
    for (u32 i = 0; i < l->cluster_count; ++i)
        if (l->clusters[i].x1 > right)
            right = l->clusters[i].x1;
    EXPECT_FLOAT_EQ(right, 100, .001f);
    de_text_layout_free(l);
    de_asset_release(f->assets, load);
}

TEST_F(layout_hit, newline_empty_and_trailing_lines_have_caret_targets, runtime_fx) {
    const char *separators[] = {"\n", "\r\n", "\xe2\x80\xa8"};
    for (u32 i = 0; i < 3; ++i) {
        char text[32];
        snprintf(text, sizeof text, "abc%s%sx%s", separators[i], separators[i], separators[i]);
        u32 width = (u32)strlen(separators[i]);
        de_text_layout *l = layout_build(f, text, 0, DE_ALIGN_START, DE_DIR_LTR, false);
        ASSERT_EQ(l->line_count, 4u);
        de_vec4 empty = de_text_layout_caret(l, 3 + width);
        EXPECT_FLOAT_EQ(empty.y, l->line_height, .001f);
        EXPECT_EQ(de_text_layout_hit_test(l, (de_vec2){1000, 1.5f * l->line_height}), 3 + width);
        de_vec4 trailing = de_text_layout_caret(l, (u32)strlen(text));
        EXPECT_FLOAT_EQ(trailing.y, 3 * l->line_height, .001f);
        EXPECT_EQ(de_text_layout_hit_test(l, (de_vec2){0, 3.5f * l->line_height}), strlen(text));
        de_text_layout_free(l);
    }
}
