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
/* DENG-585: de_ui_find_text_overflow lists the text nodes that do not fit their box, for the
 * editor's localization preview (DIDE-R193). */
#if defined(DE_UI_TEST_FONTS)
static de_entity overflow_label(ui_fx *f, runtime_fx *fonts, de_entity parent, const char *s,
                                f32 w, f32 h) {
    de_entity e = ui_node(f, parent);
    DeUiNode *n = ui_node_edit(f, e);
    if (w > 0)
        n->width = (de_ui_len){DE_UI_PX, w};
    if (h > 0)
        n->height = (de_ui_len){DE_UI_PX, h};
    n->shrink = 0;
    DeUiText *label = de_entity_add(f->world, e, f->ui->ids.text);
    label->style.font = fonts->latin;
    label->style.size_px = 20;
    memcpy(label->text, s, strlen(s));
    return e;
}
#endif
TEST_F(ui_text_metrics, finds_text_that_overflows_its_box, ui_fx) {
#if defined(DE_UI_TEST_FONTS)
    runtime_fx fonts = {0};
    runtime_fx_setup(&fonts);
    f->ui->desc.text = fonts.t;
    de_entity root = ui_node(f, (de_entity){0});
    ui_node_edit(f, root)->direction = DE_UI_COLUMN;
    /* Text wraps and breaks inside words, so only one glyph wider than its 6 px box gives
     * overflow_x. */
    de_entity wide = overflow_label(f, &fonts, root, "W", 6, 40);
    /* Auto-sized: fits. */
    de_entity fits = overflow_label(f, &fonts, root, "Fits", 0, 0);
    /* Wide enough, too low: overflow_y only. */
    de_entity low = overflow_label(f, &fonts, root, "Low", 300, 4);
    /* Hidden, and inside a hidden node: skipped. */
    de_entity hidden = overflow_label(f, &fonts, root, "Hidden overflowing text", 10, 4);
    ui_node_edit(f, hidden)->display = DE_UI_NONE;
    de_entity under = overflow_label(f, &fonts, hidden, "Under a hidden node", 10, 4);
    (void)under;
    /* Padding shrinks the content box: 300 px less 2 x 146 px padding is 8 px. */
    de_entity padded = overflow_label(f, &fonts, root, "W", 300, 40);
    ui_node_edit(f, padded)->padding[1] = (de_ui_len){DE_UI_PX, 146};
    ui_node_edit(f, padded)->padding[3] = (de_ui_len){DE_UI_PX, 146};
    ui_frame(f);

    de_ui_text_overflow found[8];
    memset(found, 0, sizeof found);
    u32 count = 99;
    ASSERT_EQ(de_ui_find_text_overflow(f->ui, (de_entity){0}, found, 8, &count), DE_OK);
    ASSERT_EQ(count, 3u);
    /* Pre-order: wide, low, padded. */
    EXPECT_EQ(found[0].entity.bits, wide.bits);
    EXPECT_TRUE(found[0].overflow_x);
    EXPECT_FLOAT_EQ(found[0].box_size.x, 6, .5f);
    EXPECT_TRUE(found[0].text_size.x > 6);
    EXPECT_EQ(found[1].entity.bits, low.bits);
    EXPECT_FALSE(found[1].overflow_x);
    EXPECT_TRUE(found[1].overflow_y);
    EXPECT_FLOAT_EQ(found[1].box_size.y, 4, .5f);
    EXPECT_TRUE(found[1].text_size.y > 4);
    EXPECT_EQ(found[2].entity.bits, padded.bits);
    EXPECT_TRUE(found[2].overflow_x);
    EXPECT_FLOAT_EQ(found[2].box_size.x, 8, .5f);
    for (u32 i = 0; i < count; ++i)
        EXPECT_TRUE(found[i].entity.bits != fits.bits);

    /* Capacity: the full count, the first entries written. */
    memset(found, 0, sizeof found);
    ASSERT_EQ(de_ui_find_text_overflow(f->ui, (de_entity){0}, found, 1, &count), DE_OK);
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(found[0].entity.bits, wide.bits);
    EXPECT_EQ(found[1].entity.bits, 0u);
    ASSERT_EQ(de_ui_find_text_overflow(f->ui, (de_entity){0}, NULL, 0, &count), DE_OK);
    EXPECT_EQ(count, 3u);
    /* A subtree root. */
    ASSERT_EQ(de_ui_find_text_overflow(f->ui, low, found, 8, &count), DE_OK);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(found[0].entity.bits, low.bits);
    ASSERT_EQ(de_ui_find_text_overflow(f->ui, fits, found, 8, &count), DE_OK);
    EXPECT_EQ(count, 0u);

    /* Longer text after a relayout (as pseudo-localization makes it): it wraps into more lines
     * than its fixed 30 px height holds. */
    DeUiText *label = de_entity_get(f->world, fits, f->ui->ids.text);
    ui_node_edit(f, fits)->width = (de_ui_len){DE_UI_PX, 50};
    ui_node_edit(f, fits)->height = (de_ui_len){DE_UI_PX, 30};
    memcpy(label->text, "[Fiiits !!!]", 13);
    ui_frame(f);
    ASSERT_EQ(de_ui_find_text_overflow(f->ui, fits, found, 8, &count), DE_OK);
    EXPECT_EQ(count, 1u);
    EXPECT_TRUE(found[0].overflow_y);

    de_entity stray = ui_node(f, (de_entity){0}); /* not laid out yet */
    EXPECT_EQ(de_ui_find_text_overflow(f->ui, stray, found, 8, &count), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_ui_find_text_overflow(f->ui, (de_entity){0}, NULL, 1, &count),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_find_text_overflow(f->ui, (de_entity){0}, found, 8, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_find_text_overflow(NULL, (de_entity){0}, found, 8, &count),
              DE_ERR_INVALID_ARGUMENT);
    f->ui->desc.text = NULL;
    EXPECT_EQ(de_ui_find_text_overflow(f->ui, (de_entity){0}, found, 8, &count),
              DE_ERR_INVALID_ARGUMENT);
    runtime_fx_teardown(&fonts);
#else
    SKIP("font cooker disabled");
#endif
}
