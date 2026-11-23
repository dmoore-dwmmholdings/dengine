/* DENG-781: locale listing, string table readback and the editor locale preview. */
#include "loc_test_util.h"
static const de_loc_string *find_string(const de_loc_string *s, u32 count, const char *key) {
    de_name name = de_name_from(key);
    for (u32 i = 0; i < count; ++i)
        if (s[i].key.hash == name.hash)
            return &s[i];
    return NULL;
}
TEST(loc_preview, lists_published_locales_sorted_with_capacity) {
    loc_fixture f;
    loc_setup(&f, "preview_list");
    de_text_system wrapper = {0};
    wrapper.loc = f.loc;
    u32 count = 99;
    ASSERT_EQ(de_loc_list_locales(&wrapper, NULL, 0, &count), DE_OK);
    EXPECT_EQ(count, 0u); /* nothing published yet */
    ASSERT_TRUE(loc_ready(&f, 1));
    ASSERT_EQ(de_loc_list_locales(&wrapper, NULL, 0, &count), DE_OK);
    EXPECT_EQ(count, 3u);
    de_loc_locale_info locales[4];
    memset(locales, 0, sizeof locales);
    ASSERT_EQ(de_loc_list_locales(&wrapper, locales, 2, &count), DE_OK);
    EXPECT_EQ(count, 3u);
    EXPECT_STREQ(locales[0].locale, "en");
    EXPECT_EQ(locales[0].tables, 1u);
    EXPECT_STREQ(locales[1].locale, "pt");
    EXPECT_EQ(locales[2].locale[0], 0);
    ASSERT_EQ(de_loc_list_locales(&wrapper, locales, 4, &count), DE_OK);
    EXPECT_STREQ(locales[2].locale, "pt-BR");
    EXPECT_EQ(de_loc_list_locales(NULL, locales, 4, &count), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_loc_list_locales(&wrapper, locales, 4, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_loc_list_locales(&wrapper, NULL, 1, &count), DE_ERR_INVALID_ARGUMENT);
    loc_destroy(&f);
}
TEST(loc_preview, reads_the_resolved_string_table_with_fallback_locales) {
    loc_fixture f;
    loc_setup(&f, "preview_read");
    de_text_system wrapper = {0};
    wrapper.loc = f.loc;
    ASSERT_TRUE(loc_ready(&f, 1));
    u64 generation = de_loc_generation(&wrapper);
    ASSERT_EQ(de_loc_set_locale(&wrapper, "pt-BR"), DE_OK);
    ASSERT_TRUE(loc_ready(&f, generation + 1));
    (void)de_name_from(
        "menu.play"); /* cooked tables keep hashes; key_name needs the name interned */
    u32 count = 0;
    ASSERT_EQ(de_loc_read_strings(&wrapper, NULL, 0, &count), DE_OK);
    EXPECT_EQ(count, 4u);
    de_loc_string strings[8];
    memset(strings, 0, sizeof strings);
    ASSERT_EQ(de_loc_read_strings(&wrapper, strings, 8, &count), DE_OK);
    ASSERT_EQ(count, 4u);
    EXPECT_STREQ(strings[0].locale, "pt-BR"); /* chain order: the requested locale first */
    const de_loc_string *s = find_string(strings, count, "greeting");
    ASSERT_TRUE(s != NULL);
    EXPECT_STREQ(s->value.ptr, "Olá");
    EXPECT_STREQ(s->locale, "pt-BR");
    s = find_string(strings, count, "menu.play");
    ASSERT_TRUE(s != NULL);
    EXPECT_STREQ(s->value.ptr, "Jogar");
    EXPECT_STREQ(s->locale, "pt");
    EXPECT_EQ(s->table, 0u);
#if DE_NAME_LOOKUP
    EXPECT_STREQ(s->key_name, "menu.play");
#endif
    s = find_string(strings, count, "menu.quit");
    ASSERT_TRUE(s != NULL);
    EXPECT_STREQ(s->value.ptr, "Quit");
    EXPECT_STREQ(s->locale, "en");
    ASSERT_TRUE(find_string(strings, count, "items") != NULL);
    memset(strings, 0, sizeof strings);
    /* DENG-786: the string_table asset of each string. */
    de_asset_id tables[8];
    memset(tables, 0, sizeof tables);
    ASSERT_EQ(de_loc_read_strings_with_tables(&wrapper, strings, tables, 8, &count), DE_OK);
    ASSERT_EQ(count, 4u);
    for (u32 i = 0; i < count; ++i) {
        EXPECT_TRUE(tables[i].hash != 0);
        for (u32 j = 0; j < count; ++j) /* one table per locale: same locale, same asset */
            EXPECT_EQ(tables[i].hash == tables[j].hash, !strcmp(strings[i].locale, strings[j].locale));
    }
    EXPECT_EQ(de_loc_read_strings_with_tables(&wrapper, strings, NULL, 8, &count),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_loc_read_strings_with_tables(NULL, strings, tables, 8, &count),
              DE_ERR_INVALID_ARGUMENT);
    memset(strings, 0, sizeof strings);
    ASSERT_EQ(de_loc_read_strings(&wrapper, strings, 1, &count), DE_OK);
    EXPECT_EQ(count, 4u);
    EXPECT_TRUE(strings[1].value.ptr == NULL);
    EXPECT_EQ(de_loc_read_strings(NULL, strings, 1, &count), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_loc_read_strings(&wrapper, strings, 1, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_loc_read_strings(&wrapper, NULL, 1, &count), DE_ERR_INVALID_ARGUMENT);
    loc_destroy(&f);
}
TEST(loc_preview, preview_keeps_the_selected_locale_and_restores_it) {
    loc_fixture f;
    loc_setup(&f, "preview_switch");
    de_text_system wrapper = {0};
    wrapper.loc = f.loc;
    ASSERT_TRUE(loc_ready(&f, 1));
    u64 generation = de_loc_generation(&wrapper);
    ASSERT_EQ(de_loc_set_locale(&wrapper, "pt"), DE_OK);
    ASSERT_TRUE(loc_ready(&f, ++generation));
    de_loc_locale_state state;
    ASSERT_EQ(de_loc_get_locale_state(&wrapper, &state), DE_OK);
    EXPECT_STREQ(state.active, "pt");
    EXPECT_STREQ(state.selected, "pt");
    EXPECT_STREQ(state.preview, "");
    EXPECT_STREQ(state.fallback, "en");
    EXPECT_EQ(state.generation, generation);
    EXPECT_FALSE(state.previewing);

    ASSERT_EQ(de_loc_preview_locale(&wrapper, "pt-br"), DE_OK);
    ASSERT_TRUE(loc_ready(&f, ++generation));
    EXPECT_STREQ(de_loc(&wrapper, de_name_from("greeting")).ptr, "Olá");
    EXPECT_STREQ(f.assets->locale_buf, "pt-BR");
    ASSERT_EQ(de_loc_get_locale_state(&wrapper, &state), DE_OK);
    EXPECT_STREQ(state.active, "pt-BR");
    EXPECT_STREQ(state.selected, "pt");
    EXPECT_STREQ(state.preview, "pt-BR");
    EXPECT_TRUE(state.previewing);

    /* The game selects a locale during the preview: stored, not published. */
    ASSERT_EQ(de_loc_set_locale(&wrapper, "en"), DE_OK);
    for (u32 i = 0; i < 4; ++i) {
        de_asset_system_update(f.assets, NULL);
        de__loc_update(f.loc);
    }
    EXPECT_EQ(de_loc_generation(&wrapper), generation);
    EXPECT_STREQ(de_loc_locale(&wrapper), "pt-BR");
    EXPECT_EQ(de_loc_preview_locale(&wrapper, "not a locale!"), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_loc_set_locale(&wrapper, "not a locale!"), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_loc_get_locale_state(&wrapper, &state), DE_OK);
    EXPECT_STREQ(state.selected, "en");
    EXPECT_STREQ(state.preview, "pt-BR");

    ASSERT_EQ(de_loc_preview_locale(&wrapper, NULL), DE_OK);
    ASSERT_TRUE(loc_ready(&f, ++generation));
    EXPECT_STREQ(de_loc_locale(&wrapper), "en");
    EXPECT_STREQ(f.assets->locale_buf, "en");
    EXPECT_STREQ(de_loc(&wrapper, de_name_from("menu.play")).ptr, "Play");
    ASSERT_EQ(de_loc_get_locale_state(&wrapper, &state), DE_OK);
    EXPECT_STREQ(state.selected, "en");
    EXPECT_STREQ(state.preview, "");
    EXPECT_FALSE(state.previewing);
    EXPECT_EQ(de_loc_preview_locale(&wrapper, NULL), DE_OK);
    EXPECT_EQ(de_loc_get_locale_state(NULL, &state), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_loc_get_locale_state(&wrapper, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_loc_preview_locale(NULL, "en"), DE_ERR_INVALID_ARGUMENT);
    loc_destroy(&f);
}
/* DENG-585: the locale state reports pseudo-localization, requested and published, so the
 * editor can show the toggle (DIDE-R193). */
TEST(loc_preview, state_reports_pseudo_requested_and_published) {
    loc_fixture f;
    loc_setup(&f, "preview_pseudo");
    de_text_system wrapper = {0};
    wrapper.loc = f.loc;
    ASSERT_TRUE(loc_ready(&f, 1));
    u64 generation = de_loc_generation(&wrapper);
    de_loc_locale_state state;
    ASSERT_EQ(de_loc_get_locale_state(&wrapper, &state), DE_OK);
    EXPECT_FALSE(state.pseudo);
    EXPECT_FALSE(state.pseudo_published);
#if DE_NAME_LOOKUP
    de_loc_pseudo(&wrapper, true);
    ASSERT_EQ(de_loc_get_locale_state(&wrapper, &state), DE_OK);
    EXPECT_TRUE(state.pseudo); /* requested at once, published after the switch */
    EXPECT_FALSE(state.pseudo_published);
    ASSERT_TRUE(loc_ready(&f, ++generation));
    ASSERT_EQ(de_loc_get_locale_state(&wrapper, &state), DE_OK);
    EXPECT_TRUE(state.pseudo);
    EXPECT_TRUE(state.pseudo_published);
    de_str play = de_loc(&wrapper, de_name_from("menu.play"));
    EXPECT_TRUE(play.len > 4 && strcmp(play.ptr, "Play") != 0);
    de_loc_pseudo(&wrapper, false);
    ASSERT_TRUE(loc_ready(&f, ++generation));
    ASSERT_EQ(de_loc_get_locale_state(&wrapper, &state), DE_OK);
    EXPECT_FALSE(state.pseudo);
    EXPECT_FALSE(state.pseudo_published);
    EXPECT_STREQ(de_loc(&wrapper, de_name_from("menu.play")).ptr, "Play");
#endif
    loc_destroy(&f);
}
