/* test_text_metrics.c: de_ui_measure_text, the editor's text metrics with the game's fonts
 * (DENG-782). The fixture's live viewport is 400x100 with reference height 100 (scale 1). */
#include "ui_test_util.h"
#if defined(DE_UI_TEST_FONTS)
#include "../text/runtime_test_util.h"
static de_vec2 direct_size(runtime_fx *fonts, const char *s, f32 size_px, f32 wrap) {
    de_text_style style = {.font = fonts->latin, .size_px = size_px};
    de_text_layout_desc d = {.utf8 = {s, strlen(s)}, .style = &style, .max_width = wrap};
    de_text_layout *l = NULL;
    if (de_text_layout_build(fonts->t, &d, NULL, &l) != DE_OK)
        return (de_vec2){-1, -1};
    de_vec2 size = de_text_layout_size(l);
    de_text_layout_free(l);
    return size;
}
#endif
TEST_F(ui_text_metrics, matches_the_text_widget_and_reports_lines, ui_fx) {
#if defined(DE_UI_TEST_FONTS)
    runtime_fx fonts = {0};
    runtime_fx_setup(&fonts);
    f->ui->desc.text = fonts.t;
    const char *s = "The quick brown fox wraps over multiple lines";
    de_entity root = ui_node(f, (de_entity){0});
    ui_node_edit(f, root)->direction = DE_UI_COLUMN;
    de_entity text = ui_node(f, root);
    DeUiText *label = de_entity_add(f->world, text, f->ui->ids.text);
    label->style.font = fonts.latin;
    label->style.size_px = 20;
    memcpy(label->text, s, strlen(s));
    de_ui_set_viewport(f->ui, 100, 100, (de_window_insets){0});
    ui_node_edit(f, text)->shrink = 0;
    ui_frame(f);
    de_text_style style = {.font = fonts.latin, .size_px = 20};
    de_ui_text_metrics m = {0};
    de_text_line lines[8] = {0};
    ASSERT_EQ(de_ui_measure_text(f->ui,
                                 &(de_ui_text_measure_desc){.utf8 = {s, strlen(s)},
                                                            .style = &style,
                                                            .wrap_width = 100},
                                 lines, 8, &m),
              DE_OK);
    /* Node rects are rounded to whole pixels. */
    EXPECT_FLOAT_EQ(m.size.y, ui_rect(f, text).w, .5f);
    EXPECT_TRUE(m.size.x <= 100);
    EXPECT_FLOAT_EQ(m.scale, 1, 0);
    EXPECT_TRUE(m.line_count > 1);
    EXPECT_FLOAT_EQ(m.line_height * m.line_count, m.size.y, .001f);
    EXPECT_FALSE(m.overflow_x);
    EXPECT_FALSE(m.overflow_y);
    EXPECT_EQ(lines[0].byte_start, 0u);
    EXPECT_EQ(lines[m.line_count - 1].byte_end, (u32)strlen(s));
    for (u32 i = 1; i < m.line_count && i < 8; ++i)
        EXPECT_EQ(lines[i].byte_start, lines[i - 1].byte_end);
    /* No wrap: one line, the natural width. */
    ASSERT_EQ(de_ui_measure_text(
                  f->ui, &(de_ui_text_measure_desc){.utf8 = {s, strlen(s)}, .style = &style},
                  NULL, 0, &m),
              DE_OK);
    EXPECT_EQ(m.line_count, 1u);
    EXPECT_FLOAT_EQ(m.size.x, direct_size(&fonts, s, 20, 0).x, .001f);
    f->ui->desc.text = NULL;
    runtime_fx_teardown(&fonts);
#else
    SKIP("font cooker disabled");
#endif
}
TEST_F(ui_text_metrics, scale_default_size_and_overflow, ui_fx) {
#if defined(DE_UI_TEST_FONTS)
    runtime_fx fonts = {0};
    runtime_fx_setup(&fonts);
    f->ui->desc.text = fonts.t;
    const char *s = "Overflowing";
    de_text_style style = {.font = fonts.latin, .size_px = 20};
    de_ui_text_metrics m = {0};
    /* An explicit layout scale multiplies the size, as a preview at that scale lays it out. */
    ASSERT_EQ(de_ui_measure_text(f->ui,
                                 &(de_ui_text_measure_desc){
                                     .utf8 = {s, strlen(s)}, .style = &style, .scale = 2},
                                 NULL, 0, &m),
              DE_OK);
    EXPECT_FLOAT_EQ(m.scale, 2, 0);
    de_vec2 big = direct_size(&fonts, s, 40, 0);
    EXPECT_FLOAT_EQ(m.size.x, big.x, .001f);
    EXPECT_FLOAT_EQ(m.size.y, big.y, .001f);
    /* size_px 0 is 16 px, as a DeUiText with no size. */
    de_text_style unsized = {.font = fonts.latin};
    ASSERT_EQ(de_ui_measure_text(
                  f->ui, &(de_ui_text_measure_desc){.utf8 = {s, strlen(s)}, .style = &unsized},
                  NULL, 0, &m),
              DE_OK);
    EXPECT_FLOAT_EQ(m.size.x, direct_size(&fonts, s, 16, 0).x, .001f);
    /* One word wider than the wrap width cannot break: overflow_x. A box lower than the
     * text: overflow_y. */
    ASSERT_EQ(de_ui_measure_text(f->ui,
                                 &(de_ui_text_measure_desc){.utf8 = {s, strlen(s)},
                                                            .style = &style,
                                                            .wrap_width = 10,
                                                            .box_height = 5},
                                 NULL, 0, &m),
              DE_OK);
    EXPECT_TRUE(m.overflow_x);
    EXPECT_TRUE(m.overflow_y);
    EXPECT_TRUE(m.size.x > 10);
    f->ui->desc.text = NULL;
    runtime_fx_teardown(&fonts);
#else
    SKIP("font cooker disabled");
#endif
}
TEST_F(ui_text_metrics, invalid_arguments, ui_fx) {
    de_text_style style = {.size_px = 20};
    de_ui_text_metrics m = {0};
    de_ui_text_measure_desc d = {.utf8 = DE_STR("x"), .style = &style};
    EXPECT_EQ(de_ui_measure_text(NULL, &d, NULL, 0, &m), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_measure_text(f->ui, NULL, NULL, 0, &m), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_measure_text(f->ui, &d, NULL, 0, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_measure_text(f->ui, &d, NULL, 1, &m), DE_ERR_INVALID_ARGUMENT);
    /* No text system attached to the UI. */
    EXPECT_EQ(de_ui_measure_text(f->ui, &d, NULL, 0, &m), DE_ERR_INVALID_ARGUMENT);
    d.wrap_width = NAN;
    EXPECT_EQ(de_ui_measure_text(f->ui, &d, NULL, 0, &m), DE_ERR_INVALID_ARGUMENT);
    d.wrap_width = 0;
    d.style = NULL;
    EXPECT_EQ(de_ui_measure_text(f->ui, &d, NULL, 0, &m), DE_ERR_INVALID_ARGUMENT);
}
