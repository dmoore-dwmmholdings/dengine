/* DENG-781: theme listing, theme variables and the editor theme preview. */
#include "style_test_util.h"
static const char *find_var(const de_ui_theme_var *vars, u32 count, const char *name,
                            bool *themed) {
    for (u32 i = 0; i < count; ++i)
        if (!strcmp(vars[i].name, name)) {
            if (themed)
                *themed = vars[i].themed;
            return vars[i].value;
        }
    return NULL;
}
TEST_F(ui_theme_preview, lists_themes_across_sheets_sorted_with_capacity, ui_fx) {
    ASSERT_EQ(style_install(f->ui, "[vars]\na='#fff'\n[theme.dark.vars]\na='#000'\n"
                                   "[theme.contrast.vars]\na='#ff0'\n"),
              DE_OK);
    ASSERT_EQ(
        style_install(f->ui, "[vars]\nb=4\n[theme.dark.vars]\nb=6\n[theme.sepia.vars]\nb=5\n"),
        DE_OK);
    u32 count = 99;
    ASSERT_EQ(de_ui_list_themes(f->ui, NULL, 0, &count), DE_OK);
    EXPECT_EQ(count, 3u);
    de_ui_theme_info some[2];
    memset(some, 0, sizeof some);
    ASSERT_EQ(de_ui_list_themes(f->ui, some, 2, &count), DE_OK);
    EXPECT_EQ(count, 3u);
    EXPECT_STREQ(some[0].name, "contrast");
    EXPECT_EQ(some[0].sheets, 1u);
    EXPECT_STREQ(some[1].name, "dark");
    EXPECT_EQ(some[1].sheets, 2u);
    de_ui_theme_info all[4];
    memset(all, 0, sizeof all);
    ASSERT_EQ(de_ui_list_themes(f->ui, all, 4, &count), DE_OK);
    EXPECT_EQ(count, 3u);
    EXPECT_STREQ(all[2].name, "sepia");
    EXPECT_EQ(all[3].name[0], 0);
    EXPECT_EQ(de_ui_list_themes(NULL, all, 4, &count), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_list_themes(f->ui, all, 4, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_list_themes(f->ui, NULL, 1, &count), DE_ERR_INVALID_ARGUMENT);
}
TEST_F(ui_theme_preview, sheet_info_describes_where_a_sheet_came_from, ui_fx) {
    ASSERT_EQ(style_install(f->ui, "[vars]\na=1\n"), DE_OK);
    de_ui_sheet_info info;
    memset(&info, 0xAB, sizeof info);
    ASSERT_EQ(de_ui_get_sheet_info(f->ui, 0, &info), DE_OK);
    EXPECT_EQ(info.asset.hash, 0u); /* loaded from source text */
    EXPECT_STREQ(info.path, "");
    EXPECT_FALSE(info.builtin);
    EXPECT_EQ(de_ui_get_sheet_info(f->ui, 1, &info), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_ui_get_sheet_info(NULL, 0, &info), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_get_sheet_info(f->ui, 0, NULL), DE_ERR_INVALID_ARGUMENT);
}
TEST_F(ui_theme_preview, theme_vars_merge_overlay_and_format_values, ui_fx) {
    char long_value[300];
    memset(long_value, 'x', sizeof long_value);
    long_value[sizeof long_value - 1] = 0;
    char source[1024];
    snprintf(source, sizeof source,
             "[vars]\nprimary='#f80'\npad=[1,2,3,4]\nsize=12\nratio=0.5\nwhole=2.0\nq='a\"b'\n"
             "flag=true\nlong='%s'\n[theme.dark.vars]\nprimary='#202020'\nextra='50%%'\n",
             long_value);
    ASSERT_EQ(style_install(f->ui, source), DE_OK);
    de_ui_theme_var vars[16];
    memset(vars, 0, sizeof vars);
    u32 count = 0;
    ASSERT_EQ(de_ui_theme_vars(f->ui, NULL, vars, 16, &count), DE_OK);
    EXPECT_EQ(count, 8u);
    bool themed = true;
    EXPECT_STREQ(find_var(vars, count, "primary", &themed), "\"#f80\"");
    EXPECT_FALSE(themed);
    EXPECT_STREQ(find_var(vars, count, "pad", NULL), "[1, 2, 3, 4]");
    EXPECT_STREQ(find_var(vars, count, "size", NULL), "12");
    EXPECT_STREQ(find_var(vars, count, "ratio", NULL), "0.5");
    EXPECT_STREQ(find_var(vars, count, "whole", NULL), "2.0");
    EXPECT_STREQ(find_var(vars, count, "q", NULL), "\"a\\\"b\"");
    EXPECT_STREQ(find_var(vars, count, "flag", NULL), "true");
    EXPECT_TRUE(find_var(vars, count, "extra", NULL) == NULL);
    EXPECT_STREQ(vars[0].name, "primary");
    EXPECT_EQ(vars[0].sheet, 0u);
    EXPECT_TRUE(vars[7].truncated);
    EXPECT_EQ(strlen(vars[7].value), DE_UI_THEME_VALUE_MAX - 1);
    EXPECT_FALSE(vars[0].truncated);

    ASSERT_EQ(de_ui_theme_vars(f->ui, "dark", vars, 16, &count), DE_OK);
    EXPECT_EQ(count, 9u);
    EXPECT_STREQ(find_var(vars, count, "primary", &themed), "\"#202020\"");
    EXPECT_TRUE(themed);
    EXPECT_STREQ(find_var(vars, count, "size", &themed), "12");
    EXPECT_FALSE(themed);
    EXPECT_STREQ(vars[8].name, "extra");
    EXPECT_STREQ(vars[8].value, "\"50%\"");
    EXPECT_TRUE(vars[8].themed);

    /* The active theme is the default; a capacity below the count still reports it. */
    ASSERT_EQ(de_ui_set_theme(f->ui, "dark"), DE_OK);
    ASSERT_EQ(de_ui_theme_vars(f->ui, NULL, vars, 1, &count), DE_OK);
    EXPECT_EQ(count, 9u);
    EXPECT_STREQ(vars[0].value, "\"#202020\"");
    ASSERT_EQ(de_ui_theme_vars(f->ui, "", NULL, 0, &count), DE_OK);
    EXPECT_EQ(count, 8u);
    EXPECT_EQ(de_ui_theme_vars(f->ui, "nope", vars, 16, &count), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_ui_theme_vars(NULL, NULL, vars, 16, &count), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_theme_vars(f->ui, NULL, NULL, 2, &count), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_theme_vars(f->ui, NULL, vars, 16, NULL), DE_ERR_INVALID_ARGUMENT);
}
TEST_F(ui_theme_preview, preview_keeps_the_selected_theme_and_restores_it, ui_fx) {
    ASSERT_EQ(style_install(f->ui, "[vars]\nprimary='#ff0000'\n[theme.dark.vars]\nprimary='#00ff00'"
                                   "\n[theme.light.vars]\nprimary='#0000ff'\n[style.button]\n"
                                   "background='$primary'\n"),
              DE_OK);
    de_entity root = ui_node(f, (de_entity){0}), e = style_button(f, root, NULL);
    ASSERT_EQ(de_ui_set_theme(f->ui, "dark"), DE_OK);
    style_step(f, 0);
    EXPECT_FLOAT_EQ(style_get(f, e).background.y, 1, 0);
    de_ui_theme_state state;
    ASSERT_EQ(de_ui_get_theme_state(f->ui, &state), DE_OK);
    EXPECT_STREQ(state.active, "dark");
    EXPECT_STREQ(state.selected, "dark");
    EXPECT_FALSE(state.previewing);

    ASSERT_EQ(de_ui_preview_theme(f->ui, "light"), DE_OK);
    style_step(f, 0);
    EXPECT_FLOAT_EQ(style_get(f, e).background.z, 1, 0);
    ASSERT_EQ(de_ui_get_theme_state(f->ui, &state), DE_OK);
    EXPECT_STREQ(state.active, "light");
    EXPECT_STREQ(state.selected, "dark");
    EXPECT_TRUE(state.previewing);

    /* The game changes its theme during the preview: stored, not shown. */
    ASSERT_EQ(de_ui_set_theme(f->ui, NULL), DE_OK);
    style_step(f, 0);
    EXPECT_FLOAT_EQ(style_get(f, e).background.z, 1, 0);
    ASSERT_EQ(de_ui_get_theme_state(f->ui, &state), DE_OK);
    EXPECT_STREQ(state.active, "light");
    EXPECT_STREQ(state.selected, "");

    /* Errors keep the state. */
    EXPECT_EQ(de_ui_preview_theme(f->ui, "nope"), DE_ERR_NOT_FOUND);
    char long_name[DE_UI_THEME_NAME_MAX + 1];
    memset(long_name, 'a', sizeof long_name);
    long_name[DE_UI_THEME_NAME_MAX] = 0;
    EXPECT_EQ(de_ui_preview_theme(f->ui, long_name), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_set_theme(f->ui, long_name), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_ui_get_theme_state(f->ui, &state), DE_OK);
    EXPECT_STREQ(state.active, "light");
    EXPECT_STREQ(state.selected, "");
    EXPECT_TRUE(state.previewing);
    style_step(f, 0);
    EXPECT_FLOAT_EQ(style_get(f, e).background.z, 1, 0);

    /* "" previews the base variables; NULL ends the preview with the game's choice. */
    ASSERT_EQ(de_ui_preview_theme(f->ui, "dark"), DE_OK);
    ASSERT_EQ(de_ui_preview_theme(f->ui, NULL), DE_OK);
    style_step(f, 0);
    EXPECT_FLOAT_EQ(style_get(f, e).background.x, 1, 0);
    EXPECT_FLOAT_EQ(style_get(f, e).background.y, 0, 0);
    ASSERT_EQ(de_ui_get_theme_state(f->ui, &state), DE_OK);
    EXPECT_STREQ(state.active, "");
    EXPECT_STREQ(state.selected, "");
    EXPECT_FALSE(state.previewing);
    EXPECT_EQ(de_ui_preview_theme(f->ui, NULL), DE_OK);
    ASSERT_EQ(de_ui_set_theme(f->ui, "light"), DE_OK);
    style_step(f, 0);
    EXPECT_FLOAT_EQ(style_get(f, e).background.z, 1, 0);
    EXPECT_EQ(de_ui_get_theme_state(NULL, &state), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_get_theme_state(f->ui, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_preview_theme(NULL, "dark"), DE_ERR_INVALID_ARGUMENT);
}
