#include "style_test_util.h"
TEST_F(ui_stylesheet, variables_themes_colors_lengths_and_authored_defaults, ui_fx) {
    ASSERT_EQ(style_install(
                  f->ui, "[vars]\nprimary='#f80'\n[theme.dark.vars]\nprimary='#4080ff80'\n[style."
                         "button]\nbackground='$primary'\nradius=6\npadding=[8,16]\nmargin=[1,2,3,"
                         "4]\nwidth='50%'\nheight='auto'\nborder_width=2\ngap=3\nfont_size=22\n"),
              DE_OK);
    de_entity root = ui_node(f, (de_entity){0}), e = style_button(f, root, NULL);
    DeUiStyle *authored = de_entity_add(f->world, e, f->ui->ids.style);
    authored->opacity = .6f;
    DeUiText *text = de_entity_add(f->world, e, f->ui->ids.text);
    text->style.size_px = 15;
    style_step(f, 0);
    DeUiStyle style = style_get(f, e);
    EXPECT_FLOAT_EQ(style.background.x, 1, .00001f);
    EXPECT_FLOAT_EQ(style.background.y, .2462013f, .00001f);
    EXPECT_FLOAT_EQ(style.background.w, 1, .00001f);
    EXPECT_FLOAT_EQ(style.opacity, .6f, .00001f);
    EXPECT_FLOAT_EQ(style.corner_radius, 6, 0);
    const DeUiNode *node = de_entity_get_const(f->world, e, f->ui->ids.node);
    EXPECT_EQ(node->width.unit, DE_UI_PERCENT);
    EXPECT_FLOAT_EQ(node->width.value, 50, 0);
    EXPECT_FLOAT_EQ(node->padding[0].value, 8, 0);
    EXPECT_FLOAT_EQ(node->padding[1].value, 16, 0);
    EXPECT_FLOAT_EQ(node->margin[3].value, 4, 0);
    text = de_entity_get(f->world, e, f->ui->ids.text);
    EXPECT_FLOAT_EQ(text->style.size_px, 22, 0);
    ASSERT_EQ(de_ui_set_theme(f->ui, "dark"), DE_OK);
    style_step(f, 0);
    style = style_get(f, e);
    EXPECT_FLOAT_EQ(style.background.x, .05126946f, .00001f);
    EXPECT_FLOAT_EQ(style.background.w, 128.f / 255, .00001f);
}
TEST_F(ui_stylesheet, semantic_errors_have_line_numbers, ui_fx) {
    const char *invalid[] = {"[style.button]\nbackground='red'",
                             "[style.button]\nopacity='wrong'",
                             "[style.\"button:unknown\"]\nopacity=1",
                             "[style.button]\nunknown=3",
                             "[vars]\na='$b'\nb='$a'\n[style.button]\nbackground='$a'",
                             "[style.button]\npadding=[1,2,3]",
                             "[style.button]\ntransition=-1",
                             "[style.button]\neasing='bounce'",
                             "[style.button]\nopacity=nan",
                             "[style.button]\nwidth='4fr'"};
    for (u32 i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        de_ui_sheet_document *sheet = NULL;
        EXPECT_EQ(de_ui__sheet_parse(NULL, invalid[i], strlen(invalid[i]), &sheet), DE_ERR_PARSE);
        EXPECT_TRUE(sheet == NULL);
        EXPECT_TRUE(strstr(de_error_last()->message, "line") != NULL);
    }
    de_ui_sheet_document *sheet = NULL;
    EXPECT_EQ(de_ui__sheet_parse(NULL, "[style", 6, &sheet), DE_ERR_PARSE);
    EXPECT_TRUE(de_error_last()->message[0]);
}
