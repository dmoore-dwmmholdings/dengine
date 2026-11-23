/* DENG-780: the stylesheet file convention, the loaded-sheet list and structured parse errors
 * (line and column) that the editor shows next to the sheet's file. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "style_test_util.h"
#include "asset/hot_reload_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

TEST(ui_stylesheet_files, suffix_is_one_constant_and_matches_whole_names) {
    EXPECT_STREQ(DE_UI_STYLESHEET_SUFFIX, DE_ASSET_STYLESHEET_SUFFIX);
    EXPECT_STREQ(DE_UI_STYLESHEET_SUFFIX, ".uistyle.toml");
    EXPECT_TRUE(de_ui_is_stylesheet_path("ui/hud" DE_UI_STYLESHEET_SUFFIX));
    EXPECT_TRUE(de_ui_is_stylesheet_path("C:\\game\\menu" DE_UI_STYLESHEET_SUFFIX));
    EXPECT_FALSE(de_ui_is_stylesheet_path(DE_UI_STYLESHEET_SUFFIX));       /* no name */
    EXPECT_FALSE(de_ui_is_stylesheet_path("ui/" DE_UI_STYLESHEET_SUFFIX)); /* no name */
    EXPECT_FALSE(de_ui_is_stylesheet_path("ui/hud.toml"));
    EXPECT_FALSE(de_ui_is_stylesheet_path("ui/hud.UISTYLE.TOML")); /* byte match */
    EXPECT_FALSE(de_ui_is_stylesheet_path("ui/hud.uistyle.toml.bak"));
    EXPECT_FALSE(de_ui_is_stylesheet_path(""));
    EXPECT_FALSE(de_ui_is_stylesheet_path(NULL));
}

TEST(ui_stylesheet_files, syntax_errors_have_line_and_column) {
    de_ui_sheet_error error;
    memset(&error, 0xAB, sizeof error);
    const char *ok = "[vars]\na=1\n[style.button]\nopacity=0.5\n";
    EXPECT_EQ(de_ui_check_stylesheet(ok, strlen(ok), &error), DE_OK);
    EXPECT_EQ(error.line, 0u);
    EXPECT_EQ(error.column, 0u);
    EXPECT_STREQ(error.message, "");

    /* An unterminated string on line 3: the column is where the bad value starts. */
    const char *bad = "[style.button]\nopacity = 0.5\nradius = 'open\n";
    EXPECT_EQ(de_ui_check_stylesheet(bad, strlen(bad), &error), DE_ERR_PARSE);
    EXPECT_EQ(error.line, 3u);
    EXPECT_EQ(error.column, 10u);
    EXPECT_TRUE(error.message[0] != 0);
    EXPECT_TRUE(strstr(error.message, "(line") == NULL); /* the position is in the fields */
    EXPECT_TRUE(strstr(error.message, "UI stylesheet") == NULL);
    /* The error context message carries the same position for logs. */
    EXPECT_TRUE(strstr(de_error_last()->message, "line 3, column 10") != NULL);

    /* A missing key on line 2, column 3. */
    const char *nokey = "[vars]\n  = 4\n";
    EXPECT_EQ(de_ui_check_stylesheet(nokey, strlen(nokey), &error), DE_ERR_PARSE);
    EXPECT_EQ(error.line, 2u);
    EXPECT_EQ(error.column, 3u);

    EXPECT_EQ(de_ui_check_stylesheet(NULL, 1, &error), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_check_stylesheet(ok, strlen(ok), NULL), DE_ERR_INVALID_ARGUMENT);
}

TEST(ui_stylesheet_files, semantic_errors_have_line_and_column) {
    static const struct {
        const char *source;
        u32 line, column;
        const char *message;
    } cases[] = {
        {"[style.button]\nopacity='wrong'", 2, 9, NULL},
        {"[style.button]\nbackground='red'", 2, 12, NULL},
        {"\n[style.\"button:unknown\"]\nopacity=1", 2, 2, "unknown state selector"},
        {"[style.button]\nopacity = 1\n  unknown=3", 3, 11, "unknown style property"},
        {"[style.button]\npadding=[1,2,3]", 2, 9, NULL},
        {"[style.button]\neasing='bounce'", 2, 8, NULL},
        {"vars = 4\n", 1, 8, "vars must be a table"},
    };
    for (u32 i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        de_ui_sheet_error error;
        memset(&error, 0, sizeof error);
        EXPECT_EQ(de_ui_check_stylesheet(cases[i].source, strlen(cases[i].source), &error),
                  DE_ERR_PARSE);
        EXPECT_EQ(error.line, cases[i].line);
        EXPECT_EQ(error.column, cases[i].column);
        EXPECT_TRUE(error.message[0] != 0);
        if (cases[i].message)
            EXPECT_STREQ(error.message, cases[i].message);
    }
}

TEST(ui_stylesheet_files, error_from_message_reads_engine_and_wrapped_messages) {
    de_ui_sheet_error e;
    de_ui__sheet_error_from_message("UI stylesheet line 4, column 7: unknown widget type", &e);
    EXPECT_EQ(e.line, 4u);
    EXPECT_EQ(e.column, 7u);
    EXPECT_STREQ(e.message, "unknown widget type");
    /* The asset system prefixes its own context; old messages have no column. */
    de_ui__sheet_error_from_message("ui/hud: load failed: UI stylesheet line 2: bad", &e);
    EXPECT_EQ(e.line, 2u);
    EXPECT_EQ(e.column, 0u);
    EXPECT_STREQ(e.message, "bad");
    de_ui__sheet_error_from_message("file not found", &e);
    EXPECT_EQ(e.line, 0u);
    EXPECT_EQ(e.column, 0u);
    EXPECT_STREQ(e.message, "file not found");
    de_ui__sheet_error_from_message(NULL, &e);
    EXPECT_STREQ(e.message, "");
}

TEST_F(ui_stylesheet_files, list_sheets_in_load_order_with_builtin_first, ui_fx) {
    u32 count = 99;
    ASSERT_EQ(de_ui_list_sheets(f->ui, NULL, 0, &count), DE_OK);
    const u32 builtin = count;
    EXPECT_TRUE(builtin >= 1u); /* the embedded widget theme */
    ASSERT_EQ(de_ui__load_stylesheet_source(f->ui, "[vars]\na=1\n", 11, false), DE_OK);
    ASSERT_EQ(de_ui_list_sheets(f->ui, NULL, 0, &count), DE_OK);
    EXPECT_EQ(count, builtin + 1u);
    de_ui_sheet_status sheets[40];
    memset(sheets, 0xAB, sizeof sheets);
    ASSERT_EQ(de_ui_list_sheets(f->ui, sheets, 40, &count), DE_OK);
    EXPECT_EQ(count, builtin + 1u);
    for (u32 i = 0; i < count; ++i) {
        EXPECT_EQ(sheets[i].index, i); /* the index of de_ui_theme_var.sheet */
        EXPECT_TRUE(sheets[i].loaded);
        EXPECT_FALSE(sheets[i].has_error);
        EXPECT_EQ(sheets[i].info.builtin, i < builtin);
        EXPECT_STREQ(sheets[i].info.path, "");
        EXPECT_STREQ(sheets[i].source, "");
    }
    EXPECT_EQ(de_ui_list_sheets(NULL, sheets, 1, &count), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_list_sheets(f->ui, sheets, 1, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_list_sheets(f->ui, NULL, 1, &count), DE_ERR_INVALID_ARGUMENT);
}

#if !defined(_WIN32)
static void write_file(const char *path, const char *text) {
    ASSERT_EQ(de_file_write_all_atomic(path, text, strlen(text)), DE_OK);
}
static const de_ui_sheet_status *find_sheet(const de_ui_sheet_status *sheets, u32 count,
                                            const char *path) {
    for (u32 i = 0; i < count; ++i)
        if (!strcmp(sheets[i].info.path, path))
            return &sheets[i];
    return NULL;
}

/* Asset sheets carry their manifest path and source file; a first load that fails is listed
 * after the loaded sheets with its error; a rejected reload keeps the old rules and reports the
 * error until a good reload clears it. */
TEST_F(ui_stylesheet_files, asset_sheets_report_source_and_errors, ui_fx) {
    char directory[] = "/tmp/deng-ui-sheets-XXXXXX";
    ASSERT_TRUE(mkdtemp(directory) != NULL);
    ASSERT_EQ(de_fs_mount("uisheets", directory, DE_MOUNT_WRITABLE, 0), DE_OK);
    write_file("uisheets:/assets.toml",
               "[project]\nasset_root='.'\nversion=1\n[assets]\n"
               "'ui/hud'={type='ui_style',source='hud.uistyle.toml'}\n"
               "'ui/broken'={type='ui_style',source='broken.uistyle.toml'}\n"
               "'ui/blob'={type='blob',source='table.uistyle.toml'}\n");
    write_file("uisheets:/hud.uistyle.toml", "[style.button]\nopacity=0.2\n");
    write_file("uisheets:/broken.uistyle.toml", "[style.button]\n\nopacity = 'x'\n");
    write_file("uisheets:/table.uistyle.toml", "[style.text]\nopacity=0.4\n");
    de_ui_destroy(f->ui);
    f->ui = NULL;
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.hot_reload = true}, &f->assets),
              DE_OK);
    ASSERT_EQ(
        de_ui_create(&(de_ui_desc){.assets = f->assets, .max_nodes = 6000, .reference_height = 100},
                     &f->ui),
        DE_OK);
    de_ui_attach(f->ui, f->world);
    de_ui_set_viewport(f->ui, 400, 100, (de_window_insets){0});
    ASSERT_EQ(de_asset_manifest_load(f->assets, "uisheets:/assets.toml"), DE_OK);
    de_asset_id ids[] = {de_asset_id_from("ui/hud"), de_asset_id_from("ui/broken"),
                         de_asset_id_from("ui/blob")};
    f->load = de_asset_load_many(f->assets, ids, 3, 0);
    de_asset_wait(f->assets, f->load, 30000000000ULL);
    de_blob hud = de_ui_stylesheet(f->ui, ids[0]), broken = de_ui_stylesheet(f->ui, ids[1]),
            table = de_asset_blob(f->assets, ids[2]);
    ASSERT_EQ(de_ui_load_stylesheet(f->ui, hud), DE_OK);
    EXPECT_TRUE(de_ui_load_stylesheet(f->ui, broken) != DE_OK);
    EXPECT_TRUE(de_ui_load_stylesheet(f->ui, broken) != DE_OK); /* listed once */
    ASSERT_EQ(de_ui_load_stylesheet(f->ui, table), DE_OK);

    u32 builtin = 0, count = 0;
    de_ui_sheet_status sheets[40];
    ASSERT_EQ(de_ui_list_sheets(f->ui, sheets, 40, &count), DE_OK);
    for (u32 i = 0; i < count; ++i)
        builtin += sheets[i].info.builtin;
    EXPECT_EQ(count, builtin + 3u);
    const de_ui_sheet_status *h = find_sheet(sheets, count, "ui/hud");
    const de_ui_sheet_status *b = find_sheet(sheets, count, "ui/broken");
    const de_ui_sheet_status *t = find_sheet(sheets, count, "ui/blob");
    ASSERT_TRUE(h && b && t);
    EXPECT_TRUE(h->loaded);
    EXPECT_FALSE(h->has_error);
    EXPECT_STREQ(h->source, "hud.uistyle.toml");
    EXPECT_EQ(h->info.asset.hash, ids[0].hash);
    EXPECT_EQ(h->index, builtin);
    EXPECT_TRUE(t->loaded);
    EXPECT_STREQ(t->source, "table.uistyle.toml");
    EXPECT_EQ(t->index, builtin + 1u); /* load order among loaded sheets */
    EXPECT_FALSE(b->loaded);
    EXPECT_EQ(b->index, builtin + 2u); /* failed sheets come after the loaded ones */
    EXPECT_TRUE(b->has_error);
    EXPECT_STREQ(b->source, "broken.uistyle.toml");
    EXPECT_EQ(b->error.line, 3u);
    EXPECT_EQ(b->error.column, 11u);
    EXPECT_TRUE(b->error.message[0] != 0);

    /* Fixing the file and loading it again moves it to the loaded sheets. */
#if DE__ASSET_RELOAD_ENABLED
    de_asset_system_update(f->assets, f->world);
    de_sleep_ns(200000000);
    write_file("uisheets:/table.uistyle.toml", "[style.text]\n\n\nopacity=");
    u64 end = de_time_now_ns() + 5000000000ULL;
    const de_ui_sheet_status *r = NULL;
    do {
        de_asset_system_update(f->assets, f->world);
        de_ui__sheets_refresh(f->ui->style);
        ASSERT_EQ(de_ui_list_sheets(f->ui, sheets, 40, &count), DE_OK);
        r = find_sheet(sheets, count, "ui/blob");
        if (r && r->has_error)
            break;
        de_sleep_ns(1000000);
    } while (de_time_now_ns() < end);
    ASSERT_TRUE(r != NULL);
    EXPECT_TRUE(r->loaded); /* the old rules stay */
    EXPECT_TRUE(r->has_error);
    EXPECT_EQ(r->error.line, 4u);
    EXPECT_EQ(r->error.column, 9u);
    write_file("uisheets:/table.uistyle.toml", "[style.text]\nopacity=0.6\n");
    end = de_time_now_ns() + 5000000000ULL;
    do {
        de_asset_system_update(f->assets, f->world);
        de_ui__sheets_refresh(f->ui->style);
        ASSERT_EQ(de_ui_list_sheets(f->ui, sheets, 40, &count), DE_OK);
        r = find_sheet(sheets, count, "ui/blob");
        if (r && !r->has_error)
            break;
        de_sleep_ns(1000000);
    } while (de_time_now_ns() < end);
    ASSERT_TRUE(r != NULL);
    EXPECT_FALSE(r->has_error);
#endif
    de_ui_destroy(f->ui);
    f->ui = NULL;
    de_asset_release(f->assets, f->load);
    f->load = (de_load_handle){0};
    de_asset_system_destroy(f->assets);
    f->assets = NULL;
    static const char *const files[] = {"assets.toml", "hud.uistyle.toml", "broken.uistyle.toml",
                                        "table.uistyle.toml"};
    for (u32 i = 0; i < 4; ++i) {
        char path[64];
        snprintf(path, sizeof path, "uisheets:/%s", files[i]);
        EXPECT_EQ(de_file_delete(path), DE_OK);
    }
    EXPECT_EQ(de_fs_unmount("uisheets", directory), DE_OK);
    EXPECT_EQ(rmdir(directory), 0);
}
#endif
