#include "style_test_util.h"
TEST_F(ui_style_resolve, specificity_sheet_order_state_and_class_removal, ui_fx) {
    ASSERT_EQ(style_install(f->ui, "[style.button]\nopacity=0.1\n[style.\".danger\"]\nopacity=0."
                                   "2\n[style.\"button.danger:hover\"]\nopacity=0.9"),
              DE_OK);
    ASSERT_EQ(style_install(f->ui, "[style.\".danger\"]\nopacity=0.3"), DE_OK);
    de_entity root = ui_node(f, (de_entity){0}), a = style_button(f, root, NULL),
              b = style_button(f, root, "danger");
    style_step(f, 0);
    EXPECT_FLOAT_EQ(style_get(f, a).opacity, .1f, .00001f);
    EXPECT_FLOAT_EQ(style_get(f, b).opacity, .3f, .00001f);
    style_step(f, 0);
    EXPECT_EQ(de_ui_get_style_stats(f->ui).entities_restyled, 0u);
    DeUiInteractionState *interaction = de_entity_add(f->world, b, f->ui->ids.state);
    interaction->hovered = true;
    style_step(f, 0);
    EXPECT_EQ(de_ui_get_style_stats(f->ui).entities_restyled, 1u);
    EXPECT_FLOAT_EQ(style_get(f, b).opacity, .9f, .00001f);
    ui_node_edit(f, b)->style_class = (de_name){0};
    style_step(f, 0);
    EXPECT_FLOAT_EQ(style_get(f, b).opacity, .1f, .00001f);
}
TEST_F(ui_style_resolve, focus_pressed_disabled_and_preserve_unset_fields, ui_fx) {
    ASSERT_EQ(style_install(f->ui,
                            "[style.\"button:focus\"]\nopacity=0.2\n[style.\"button:pressed\"]"
                            "\nopacity=0.4\n[style.\"button:disabled\"]\nopacity=0.6"),
              DE_OK);
    de_entity e = style_button(f, (de_entity){0}, NULL);
    DeUiStyle *own = de_entity_add(f->world, e, f->ui->ids.style);
    own->corner_radius = 7;
    DeUiInteractionState *s = de_entity_add(f->world, e, f->ui->ids.state);
    s->focused = true;
    style_step(f, 0);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, .2f, 0);
    s = de_entity_get(f->world, e, f->ui->ids.state);
    s->pressed = true;
    style_step(f, 0);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, .4f, 0);
    s = de_entity_get(f->world, e, f->ui->ids.state);
    s->disabled = true;
    style_step(f, 0);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, .6f, 0);
    EXPECT_FLOAT_EQ(style_get(f, e).corner_radius, 7, 0);
}
TEST_F(ui_style_resolve, theme_2000_node_budget_and_idle_counter, ui_fx) {
    ASSERT_EQ(style_install(f->ui, "[vars]\nprimary='#000'\n[theme.dark.vars]\nprimary='#fff'\n["
                                   "style.button]\nbackground='$primary'"),
              DE_OK);
    de_entity root = ui_node(f, (de_entity){0});
    for (u32 i = 0; i < 2000; ++i)
        (void)style_button(f, root, NULL);
    style_step(f, 0);
    ASSERT_EQ(de_ui_set_theme(f->ui, "dark"), DE_OK);
    u64 start = de_time_now_ns();
    style_step(f, 0);
    f64 ms = (de_time_now_ns() - start) / 1000000.;
    fprintf(stderr, "theme switch2000nodes %.3fms\n", ms);
    EXPECT_EQ(de_ui_get_style_stats(f->ui).entities_restyled, 2001u);
    EXPECT_TRUE(ms <= 1.0);
    style_step(f, 0);
    EXPECT_EQ(de_ui_get_style_stats(f->ui).entities_restyled, 0u);
}
#if !defined(_WIN32)
#include <stdlib.h>
#include <unistd.h>
TEST_F(ui_style_resolve, registered_asset_blob_and_real_hot_reload_keep_old_on_error, ui_fx) {
    char directory[] = "/tmp/deng-ui-style-XXXXXX";
    ASSERT_TRUE(mkdtemp(directory) != NULL);
    ASSERT_EQ(de_fs_mount("uistyle", directory, DE_MOUNT_WRITABLE, 0), DE_OK);
    const char *manifest = "[project]\nasset_root='.'\nversion=1\n[assets]\n'ui/"
                           "style'={type='ui_style',source='style.toml'}\n'ui/"
                           "blobstyle'={type='blob',source='blob.toml'}\n";
    const char *first = "[style.button]\nopacity=0.2\n", *blob = "[style.\".danger\"]\nradius=9\n";
    ASSERT_EQ(de_file_write_all_atomic("uistyle:/assets.toml", manifest, strlen(manifest)), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("uistyle:/style.toml", first, strlen(first)), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("uistyle:/blob.toml", blob, strlen(blob)), DE_OK);
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
    ASSERT_EQ(de_asset_manifest_load(f->assets, "uistyle:/assets.toml"), DE_OK);
    de_asset_id ids[] = {de_asset_id_from("ui/style"), de_asset_id_from("ui/blobstyle")};
    de_blob sheet = de_ui_stylesheet(f->ui, ids[0]), blobhandle = de_asset_blob(f->assets, ids[1]);
    ASSERT_TRUE(sheet.id);
    ASSERT_TRUE(blobhandle.id);
    f->load = de_asset_load_many(f->assets, ids, 2, 0);
    ASSERT_EQ(de_asset_wait(f->assets, f->load, 30000000000ULL), DE_LOAD_READY);
    ASSERT_EQ(de_ui_load_stylesheet(f->ui, sheet), DE_OK);
    ASSERT_EQ(de_ui_load_stylesheet(f->ui, blobhandle), DE_OK);
    de_entity e = style_button(f, (de_entity){0}, "danger");
    style_step(f, 1.f / 60.f);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, .2f, .00001f);
    EXPECT_FLOAT_EQ(style_get(f, e).corner_radius, 9, 0);
    // The asset system creates source watches on the update following publication.
    de_asset_system_update(f->assets, f->world);
    de_sleep_ns(200000000);
    const char *updated = "[style.button]\nopacity=0.8\n";
    ASSERT_EQ(de_file_write_all_atomic("uistyle:/style.toml", updated, strlen(updated)), DE_OK);
    u64 end = de_time_now_ns() + 5000000000ULL;
    do {
        de_asset_system_update(f->assets, f->world);
        style_step(f, 1.f / 60.f);
        if (fabsf(style_get(f, e).opacity - .8f) < .00001f)
            break;
        de_sleep_ns(1000000);
    } while (de_time_now_ns() < end);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, .8f, .00001f);
    const char *broken = "[style.button]\nopacity='broken'\n";
    ASSERT_EQ(de_file_write_all_atomic("uistyle:/style.toml", broken, strlen(broken)), DE_OK);
    end = de_time_now_ns() + 5000000000ULL;
    char error[256] = {0};
    de_result reload_error = DE_OK;
    do {
        de_asset_system_update(f->assets, f->world);
        style_step(f, 1.f / 60.f);
        reload_error = de_asset_last_error(f->assets, ids[0], error, sizeof error);
        if (reload_error != DE_OK)
            break;
        de_sleep_ns(1000000);
    } while (de_time_now_ns() < end);
    EXPECT_EQ(reload_error, DE_ERR_PARSE);
    EXPECT_TRUE(strstr(error, "line") != NULL);
    EXPECT_FLOAT_EQ(style_get(f, e).opacity, .8f, .00001f);
    de_ui_destroy(f->ui);
    f->ui = NULL;
    de_asset_release(f->assets, f->load);
    f->load = (de_load_handle){0};
    de_asset_system_destroy(f->assets);
    f->assets = NULL;
    ASSERT_EQ(de_file_delete("uistyle:/assets.toml"), DE_OK);
    ASSERT_EQ(de_file_delete("uistyle:/style.toml"), DE_OK);
    ASSERT_EQ(de_file_delete("uistyle:/blob.toml"), DE_OK);
    ASSERT_EQ(de_fs_unmount("uistyle", directory), DE_OK);
    EXPECT_EQ(rmdir(directory), 0);
}
#endif
