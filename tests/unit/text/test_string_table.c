#include "loc_test_util.h"
#include "de_alloc_guard.h"
#include "cook/lib/cooker_internal.h"
#include "cook/lib/decache.h"
#include "core/de_toml_internal.h"
TEST(loc_tables, deterministic_directory_cook_and_locale_subassets) {
    loc_fixture f;
    loc_setup(&f, "deterministic");
    de_cooker *c = NULL;
    ASSERT_EQ(de_cooker_create(NULL, &c), DE_OK);
    de_asset_type_desc type = de_string_table_import_type_desc();
    ASSERT_EQ(de_cooker_register_type(c, &type), DE_OK);
    de_cook_result first = loc_cook(&f, c), second = loc_cook(&f, c);
    EXPECT_EQ(first.size, second.size);
    EXPECT_MEMEQ(first.data, second.data, (size_t)first.size);
    EXPECT_EQ(first.subasset_count, 3u);
    EXPECT_EQ(first.extra_source_count, 3u);
    const de_cook_subasset *en = de_cook_result_subasset(&first, "en");
    ASSERT_TRUE(en != NULL);
    de__loc_table table;
    ASSERT_EQ(de__loc_table_parse(en->data, en->size, &table), DE_OK);
    EXPECT_STREQ(de__loc_table_lookup(&table, de_name_from("menu.play")).ptr, "Play");
    de_cook_result_free(&first);
    de_cook_result_free(&second);
    de_cooker_destroy(c);
    loc_destroy(&f);
}
TEST(loc_tables, async_fallback_switch_generation_asset_overrides_and_two_frame_lifetime) {
    loc_fixture f;
    loc_setup(&f, "switch");
    ASSERT_TRUE(loc_ready(&f, 1));
    de_text_system wrapper = {0};
    wrapper.loc = f.loc;
    de_str old = de_loc(&wrapper, de_name_from("menu.play"));
    EXPECT_STREQ(old.ptr, "Play");
    u64 generation = de_loc_generation(&wrapper);
    ASSERT_EQ(de_loc_set_locale(&wrapper, "pt-BR"), DE_OK);
    EXPECT_STREQ(de_loc_locale(&wrapper), "en");
    EXPECT_EQ(de_loc_generation(&wrapper), generation);
    ASSERT_TRUE(loc_ready(&f, generation + 1));
    EXPECT_EQ(de_loc_generation(&wrapper), generation + 1);
    EXPECT_STREQ(de_loc_locale(&wrapper), "pt-BR");
    EXPECT_STREQ(f.assets->locale_buf, "pt-BR");
    EXPECT_STREQ(de_loc(&wrapper, de_name_from("greeting")).ptr, "Olá");
    EXPECT_STREQ(de_loc(&wrapper, de_name_from("menu.play")).ptr, "Jogar");
    EXPECT_STREQ(de_loc(&wrapper, de_name_from("menu.quit")).ptr, "Quit");
    EXPECT_STREQ(old.ptr, "Play");
    de__loc_update(f.loc);
    EXPECT_STREQ(old.ptr, "Play");
    de__loc_update(f.loc);
    EXPECT_STREQ(old.ptr, "Play");
    ASSERT_EQ(de_loc_set_locale(&wrapper, "pt-BR"), DE_OK);
    de__loc_update(f.loc);
    EXPECT_EQ(de_loc_generation(&wrapper), generation + 1);
    de_string_table handle =
        de_asset_string_table(f.assets, de_asset_id_from("strings/main#pt-BR"));
    EXPECT_TRUE(handle.id != 0);
    loc_destroy(&f);
}
TEST(loc_tables, lookup_format_and_missing_id_are_allocation_free) {
    loc_fixture f;
    loc_setup(&f, "alloc");
    ASSERT_TRUE(loc_ready(&f, 1));
    de_name items = de_name_from("items"), missing = de_name_from("missing.key");
    de_loc_arg arg = {"count", DE_LOC_ARG_INT, NULL, 2, 0};
    char out[128];
    de__loc_format(f.loc, items, &arg, 1, out, sizeof out);
    de__loc_lookup(f.loc, missing);
    de_alloc_guard guard = {0};
    guard.this_thread_only = true;
    de_alloc_guard_begin(&guard);
    for (u32 i = 0; i < 1000; ++i) {
        de__loc_lookup(f.loc, items);
        de__loc_lookup(f.loc, missing);
        de__loc_format(f.loc, items, &arg, 1, out, sizeof out);
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    EXPECT_STREQ(out, "2 items");
#if DE_NAME_LOOKUP
    EXPECT_STREQ(de__loc_lookup(f.loc, missing).ptr, "[[missing.key]]");
#else
    EXPECT_STREQ(de__loc_lookup(f.loc, missing).ptr, "");
#endif
    loc_destroy(&f);
}
TEST(loc_tables, pseudo_publication_and_invalid_locale_validation) {
    loc_fixture f;
    loc_setup(&f, "pseudo");
    ASSERT_TRUE(loc_ready(&f, 1));
    u64 generation = de__loc_generation(f.loc);
    EXPECT_EQ(de__loc_set_locale(f.loc, "../../en"), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__loc_set_default_locale(f.loc, ""), DE_ERR_INVALID_ARGUMENT);
    de__loc_pseudo(f.loc, true);
#if DE_NAME_LOOKUP
    ASSERT_TRUE(loc_ready(&f, generation + 1));
    de_str s = de__loc_lookup(f.loc, de_name_from("menu.play"));
    EXPECT_TRUE(s.len > 4);
    EXPECT_TRUE(strstr(s.ptr, "[!! ") != NULL);
    de_loc_arg arg = {"count", DE_LOC_ARG_INT, NULL, 2, 0};
    char out[128];
    de__loc_format(f.loc, de_name_from("items"), &arg, 1, out, sizeof out);
    EXPECT_TRUE(strstr(out, "2 items") != NULL);
#else
    EXPECT_EQ(de__loc_generation(f.loc), generation);
#endif
    loc_destroy(&f);
}
TEST(loc_tables, corrupt_offsets_duplicate_hashes_invalid_utf8_and_messages_rejected) {
    de__loc_pair pairs[] = {{"a", "A", 1, 0}, {"b", "{n}", 3, 0}};
    void *data = NULL;
    u64 bytes = 0;
    ASSERT_EQ(de__loc_table_build(de_heap_allocator(), pairs, 2, &data, &bytes), DE_OK);
    de__loc_table table;
    ASSERT_EQ(de__loc_table_parse(data, bytes, &table), DE_OK);
    u8 *p = data;
    de__loc_w32(p + 40, UINT32_MAX);
    EXPECT_EQ(de__loc_table_parse(data, bytes, &table), DE_ERR_PARSE);
    DE_FREE(de_heap_allocator(), data, (size_t)bytes);
    de__loc_pair duplicate[] = {{"a", "A", 1, 0}, {"a", "B", 1, 0}};
    EXPECT_EQ(de__loc_table_build(de_heap_allocator(), duplicate, 2, &data, &bytes),
              DE_ERR_ALREADY_EXISTS);
    de__loc_pair bad = {"a", "{n, plural, one{x}}", 19, 0};
    EXPECT_EQ(de__loc_table_build(de_heap_allocator(), &bad, 1, &data, &bytes), DE_ERR_PARSE);
}
TEST(loc_tables, directory_cache_tracks_add_remove_and_content_while_file_source_is_unchanged) {
    loc_fixture f;
    loc_setup(&f, "cache");
    char path[512];
    snprintf(path, sizeof path, "%s/cookcache", f.root);
    de_cooker_desc d = {0};
    d.cache_dir = path;
    de_cooker *c = NULL;
    ASSERT_EQ(de_cooker_create(&d, &c), DE_OK);
    de_asset_type_desc type = de_string_table_import_type_desc();
    ASSERT_EQ(de_cooker_register_type(c, &type), DE_OK);
    de_cook_result first = loc_cook(&f, c);
    EXPECT_EQ(first.subasset_count, 3u);
    de_cook_result_free(&first);
    loc_file(&f, "de", "[strings]\n\"menu.play\"=\"Spielen\"\n");
    de_cook_result added = loc_cook(&f, c);
    EXPECT_EQ(added.subasset_count, 4u);
    de_cook_result_free(&added);
    loc_file(&f, "de", "[strings]\n\"menu.play\"=\"Starten\"\n");
    de_cook_result changed = loc_cook(&f, c);
    const de_cook_subasset *german = de_cook_result_subasset(&changed, "de");
    ASSERT_TRUE(german != NULL);
    de__loc_table table;
    ASSERT_EQ(de__loc_table_parse(german->data, german->size, &table), DE_OK);
    EXPECT_STREQ(de__loc_table_lookup(&table, de_name_from("menu.play")).ptr, "Starten");
    de_cook_result_free(&changed);
    snprintf(path, sizeof path, "%s/strings/main/de.toml", f.root);
    ASSERT_EQ(de_file_delete(path), DE_OK);
    de_cook_result removed = loc_cook(&f, c);
    EXPECT_EQ(removed.subasset_count, 3u);
    de_cook_result_free(&removed);
    snprintf(path, sizeof path, "%s/plain.bin", f.root);
    loc_write(path, "ordinary file source");
    void *source = NULL;
    u64 size = 0;
    ASSERT_EQ(de__cook_read_source(path, de_heap_allocator(), &source, &size), DE_OK);
    EXPECT_EQ(size, 20u);
    EXPECT_MEMEQ(source, "ordinary file source", 20);
    DE_FREE(de_heap_allocator(), source, (size_t)size + 1);
    de_asset_type_desc blob = {0};
    blob.name = "blob";
    ASSERT_EQ(de_cooker_register_type(c, &blob), DE_OK);
    de__manifest_asset a = {0};
    a.id = "plain";
    a.type = "blob";
    a.source = "plain.bin";
    de_cook_input in = {&a, f.root, NULL};
    de_cook_result one = {0}, two = {0};
    ASSERT_EQ(de_cooker_cook(c, &in, &one), DE_OK);
    de_cooker_cache_stats before_cache = {0}, after_cache = {0};
    de_cooker_get_cache_stats(c, &before_cache);
    ASSERT_EQ(de_cooker_cook(c, &in, &two), DE_OK);
    de_cooker_get_cache_stats(c, &after_cache);
    EXPECT_EQ(after_cache.hits, before_cache.hits + 1);
    EXPECT_MEMEQ(one.data, two.data, (size_t)one.size);
    de_cook_result_free(&one);
    de_cook_result_free(&two);
    de_cooker_destroy(c);
    loc_destroy(&f);
}
typedef struct locale_warnings {
    u32 count;
    char message[512];
} locale_warnings;
static void capture_locale_warning(const de_log_record *record, void *user) {
    locale_warnings *warnings = user;
    if (record->level != DE_LOG_WARN)
        return;
    char message[512];
    snprintf(message, sizeof message, "%.*s", (int)record->message.len, record->message.ptr);
    if (strstr(message, "duplicate string key")) {
        ++warnings->count;
        memcpy(warnings->message, message, strlen(message) + 1);
    }
}
TEST(loc_tables, cook_errors_include_source_line_and_duplicates_are_reported) {
    loc_fixture f;
    loc_setup(&f, "errors");
    de_cooker *c = NULL;
    ASSERT_EQ(de_cooker_create(NULL, &c), DE_OK);
    de_asset_type_desc type = de_string_table_import_type_desc();
    ASSERT_EQ(de_cooker_register_type(c, &type), DE_OK);
    loc_file(&f, "en", "[strings]\n\"broken\"=\"{n, plural, one{x}}\"\n");
    de_cook_input in = {&f.assets->manifest->assets[0], f.root, NULL};
    de_cook_result r = {0};
    EXPECT_EQ(de_cooker_cook(c, &in, &r), DE_ERR_PARSE);
    EXPECT_TRUE(strstr(de_error_last()->message, "en.toml:2") != NULL);
    const char *duplicate = "[strings]\n\"a\"=\"A\"\n\"a\"=\"B\"\n";
    /* The shared parser still rejects the exact same document. */
    toml_result_t strict = de__toml_parse(NULL, duplicate, (int)strlen(duplicate));
    EXPECT_FALSE(strict.ok);
    de__toml_free(strict);
    locale_warnings warnings = {0};
    de_log_desc log = {.no_console_sink = true};
    ASSERT_EQ(de_log_init(&log), DE_OK);
    de_log_add_sink(capture_locale_warning, &warnings);
    loc_file(&f, "en", duplicate);
    ASSERT_EQ(de_cooker_cook(c, &in, &r), DE_OK);
    const de_cook_subasset *en = de_cook_result_subasset(&r, "en");
    ASSERT_TRUE(en != NULL);
    de__loc_table table;
    ASSERT_EQ(de__loc_table_parse(en->data, en->size, &table), DE_OK);
    EXPECT_STREQ(de__loc_table_lookup(&table, de_name_from("a")).ptr, "A");
    de_cook_result_free(&r);
    de_log_flush();
    EXPECT_EQ(warnings.count, 1u);
    EXPECT_TRUE(strstr(warnings.message, "en.toml:3") != NULL);
    EXPECT_TRUE(strstr(warnings.message, "key 'a'") != NULL);
    EXPECT_TRUE(strstr(warnings.message, "first value from line 2") != NULL);
    de_log_remove_sink(capture_locale_warning, &warnings);
    de_log_shutdown();
    de_cooker_destroy(c);
    loc_destroy(&f);
}

TEST(loc_tables, duplicate_policy_handles_escaped_multiline_keys_and_keeps_other_errors) {
    loc_fixture f;
    loc_setup(&f, "duplicate_policy");
    de_cooker *c = NULL;
    ASSERT_EQ(de_cooker_create(NULL, &c), DE_OK);
    de_asset_type_desc type = de_string_table_import_type_desc();
    ASSERT_EQ(de_cooker_register_type(c, &type), DE_OK);
    loc_file(&f, "en",
             "[strings]\n\"caf\\u00e9\"=\"\"\"First\nline\"\"\"\n"
             "'café'='Second'\n\"café\"='Third'\n'next'='After duplicates'\n");
    de_cook_result first = loc_cook(&f, c), second = loc_cook(&f, c);
    const de_cook_subasset *en = de_cook_result_subasset(&first, "en");
    ASSERT_TRUE(en != NULL);
    de__loc_table table;
    ASSERT_EQ(de__loc_table_parse(en->data, en->size, &table), DE_OK);
    EXPECT_EQ(table.count, 2u);
    EXPECT_STREQ(de__loc_table_lookup(&table, de_name_from("café")).ptr, "First\nline");
    EXPECT_STREQ(de__loc_table_lookup(&table, de_name_from("next")).ptr, "After duplicates");
    const de_cook_subasset *again = de_cook_result_subasset(&second, "en");
    ASSERT_TRUE(again != NULL);
    EXPECT_EQ(en->size, again->size);
    EXPECT_MEMEQ(en->data, again->data, (size_t)en->size);
    de_cook_result_free(&first);
    de_cook_result_free(&second);
    const char *invalid[] = {
        "[strings]\na='A'\na=7\n",
        "[strings]\na=7\na='A'\n",
        "[strings]\na='A'\na=\"unterminated\n",
        "[strings]\na='A'\n[other]\nx='X'\nx='Y'\n",
        "[strings]\na='A'\n[strings]\nb='B'\n",
        "strings={a='A',a='B'}\n",
    };
    de_cook_input in = {&f.assets->manifest->assets[0], f.root, NULL};
    for (u32 i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        loc_file(&f, "en", invalid[i]);
        de_cook_result r = {0};
        EXPECT_EQ(de_cooker_cook(c, &in, &r), DE_ERR_PARSE);
        EXPECT_TRUE(strstr(de_error_last()->message, "en.toml") != NULL);
        de_cook_result_free(&r);
    }
    de_cooker_destroy(c);
    loc_destroy(&f);
}
