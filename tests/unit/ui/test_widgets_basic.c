#include "ui_test_util.h"
#include "ui/widgets/widgets_internal.h"
#include "ui/render/draw_list.h"
#include "ui/style/stylesheet_internal.h"
TEST_F(ui_widgets, basic_entry_table_submit_toggle_slider_and_null_scroll, ui_fx) {
    de_ui_widget_table *table = de_ui__widget_table(f->ui);
    ASSERT_TRUE(table);
    EXPECT_TRUE(table->scroll_into_view != NULL);
    de_entity e = ui_node(f, (de_entity){0});
    DeUiButton *button = de_entity_add(f->world, e, f->ui->widgets->button);
    button->on_click = de_name_from("play");
    DeUiText *label = de_entity_add(f->world, e, f->ui->ids.text);
    memcpy(label->text, "Play", 5);
    ui_frame(f);
    const DeUiWidgetType *button_tag = de_entity_get_const(f->world, e, f->ui->style->widget);
    ASSERT_TRUE(button_tag);
    EXPECT_EQ(button_tag->type, DE_UI_WIDGET_BUTTON);
    de_event_reader reader = de_event_reader_create(f->world, f->ui->event);
    table->entries[DE_UI_WIDGET_BUTTON].submit(f->ui, e);
    const de_ui_event *event = de_event_next(f->world, &reader);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, DE_UI_EV_CLICK);
    EXPECT_EQ(event->action.hash, de_name_from("play").hash);
    EXPECT_TRUE(de_event_next(f->world, &reader) == NULL);
    de_entity toggle = ui_node(f, (de_entity){0});
    de_entity_add(f->world, toggle, f->ui->widgets->toggle);
    table->entries[DE_UI_WIDGET_TOGGLE].submit(f->ui, toggle);
    EXPECT_TRUE(
        ((const DeUiToggle *)de_entity_get_const(f->world, toggle, f->ui->widgets->toggle))->on);
    event = de_event_next(f->world, &reader);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, DE_UI_EV_CHANGED);
    EXPECT_FLOAT_EQ(event->value, 1, 0);
    de_entity slider = ui_node(f, (de_entity){0});
    *(DeUiSlider *)de_entity_add(f->world, slider, f->ui->widgets->slider) =
        (DeUiSlider){.value = 2, .min = 0, .max = 10, .step = .5f};
    table->entries[DE_UI_WIDGET_SLIDER].increment(f->ui, slider);
    EXPECT_FLOAT_EQ(
        ((const DeUiSlider *)de_entity_get_const(f->world, slider, f->ui->widgets->slider))->value,
        2.5f, 0);
    table->entries[DE_UI_WIDGET_SLIDER].decrement(f->ui, slider);
    EXPECT_FLOAT_EQ(
        ((const DeUiSlider *)de_entity_get_const(f->world, slider, f->ui->widgets->slider))->value,
        2, 0);
    if (table->scroll_into_view)
        table->scroll_into_view(f->ui, (de_entity){0}, e);
}
TEST_F(ui_widgets, default_sheet_real_fills_progress_and_reattach, ui_fx) {
    EXPECT_EQ(f->ui->style->builtin_count, 1u);
    de_entity root = ui_node(f, (de_entity){0}), e = ui_node(f, root);
    DeUiNode *n = ui_node_edit(f, e);
    n->width = (de_ui_len){DE_UI_PX, 200};
    n->height = (de_ui_len){DE_UI_PX, 20};
    DeUiProgress *p = de_entity_add(f->world, e, f->ui->widgets->progress);
    p->value = .25f;
    ui_frame(f);
    const DeUiWidgetType *tag = de_entity_get_const(f->world, e, f->ui->style->widget);
    ASSERT_TRUE(tag);
    EXPECT_EQ(tag->type, DE_UI_WIDGET_PROGRESS);
    struct de_ui_render *render = f->ui->render;
    ASSERT_TRUE(render->build.count >= 2);
    de_vec4 fill = render->build.commands[render->build.count - 1].gpu.rect;
    EXPECT_FLOAT_EQ(fill.z, 50, .00001f);
    p = de_entity_get(f->world, e, f->ui->widgets->progress);
    p->indeterminate = true;
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, .5f, 0), DE_OK);
    de_vec4 before = render->build.commands[render->build.count - 1].gpu.rect;
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, .5f, 0), DE_OK);
    de_vec4 after = render->build.commands[render->build.count - 1].gpu.rect;
    EXPECT_TRUE(before.x != after.x);
    EXPECT_TRUE(after.z > 0 && after.z <= 50);
    de_ui_attach(f->ui, NULL);
    de_ui_attach(f->ui, f->world);
    ui_frame(f);
    render = f->ui->render;
    ASSERT_TRUE(render);
    EXPECT_TRUE(render->widget_draw != NULL);
    EXPECT_TRUE(render->build.count >= 2);
}
TEST_F(ui_widgets, static500_widgets_zero_heap_allocations, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    for (u32 i = 0; i < 500; ++i) {
        de_entity e = ui_node(f, root);
        de_entity_add(f->world, e, f->ui->widgets->button);
    }
    for (u32 i = 0; i < 4; ++i) {
        ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_PRE_UPDATE, .016f, 0), DE_OK);
        ui_frame(f);
    }
    u64 before = 0, after = 0;
    for (u32 tag = 0; tag < DE_MEM_TAG_BUILTIN_COUNT_; ++tag)
        before += de_mem_stats_for_tag(tag).alloc_count;
    for (u32 i = 0; i < 1000; ++i) {
        ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_PRE_UPDATE, .016f, 0), DE_OK);
        ui_frame(f);
    }
    for (u32 tag = 0; tag < DE_MEM_TAG_BUILTIN_COUNT_; ++tag)
        after += de_mem_stats_for_tag(tag).alloc_count;
    EXPECT_EQ(after, before);
    u32 w = 99, h = 99;
    de_window_size_points((de_window){0}, &w, &h);
    EXPECT_EQ(w, 0u);
    EXPECT_EQ(h, 0u);
}
#if defined(DE_UI_TEST_FONTS) && !defined(_WIN32)
#include "../text/runtime_test_util.h"
#include <stdlib.h>
#include <unistd.h>
static void locale_wait_public(runtime_fx *font, u64 generation) {
    u64 end = de_time_now_ns() + 10000000000ULL;
    do {
        de_asset_system_update(font->assets, NULL);
        de_text_update(font->t);
        if (de_loc_generation(font->t) >= generation)
            return;
        de_sleep_ns(1000000);
    } while (de_time_now_ns() < end);
    ASSERT_TRUE(de_loc_generation(font->t) >= generation);
}
TEST_F(ui_widgets, real_locale_ready_switch_updates_labels_and_only_localized_measurements, ui_fx) {
    runtime_fx fonts = {0};
    runtime_fx_setup(&fonts);
    char directory[] = "/tmp/deng-ui-locale-XXXXXX";
    ASSERT_TRUE(mkdtemp(directory));
    ASSERT_EQ(de_fs_mount("uilocale", directory, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_dir_create("uilocale:/strings/main"), DE_OK);
    const char *en = "[strings]\nplay='Play'\nquit='Quit'\n",
               *pt = "[strings]\nplay='Jogar'\nquit='Sair'\n",
               *manifest = "[assets]\n'strings/main'={type='string_table',source='strings/main'}\n";
    ASSERT_EQ(de_file_write_all_atomic("uilocale:/strings/main/en.toml", en, strlen(en)), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("uilocale:/strings/main/pt.toml", pt, strlen(pt)), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic("uilocale:/assets.toml", manifest, strlen(manifest)), DE_OK);
    u64 initial = de_loc_generation(fonts.t);
    ASSERT_EQ(de_asset_manifest_load(fonts.assets, "uilocale:/assets.toml"), DE_OK);
    locale_wait_public(&fonts, initial + 1);
    f->ui->desc.text = fonts.t;
    de_entity root = ui_node(f, (de_entity){0});
    ui_node_edit(f, root)->direction = DE_UI_COLUMN;
    ui_node_edit(f, root)->align_items = DE_UI_ALIGN_START;
    de_entity labels[3];
    for (u32 i = 0; i < 3; ++i) {
        labels[i] = ui_node(f, root);
        DeUiText *t = de_entity_add(f->world, labels[i], f->ui->ids.text);
        t->style.font = fonts.latin;
        t->style.size_px = 20;
        ui_node_edit(f, labels[i])->shrink = 0;
        if (i < 2)
            t->loc_id = de_name_from(i ? "quit" : "play");
        else
            memcpy(t->text, "Static", 7);
    }
    for (u32 i = 0; i < 4; ++i)
        ui_frame(f);
    const DeUiText *play = de_entity_get_const(f->world, labels[0], f->ui->ids.text);
    EXPECT_STREQ(de_ui__text_content(f->ui, play).ptr, "Play");
    de_vec4 static_rect = ui_rect(f, labels[2]);
    u64 generation = de_loc_generation(fonts.t);
    ASSERT_EQ(de_loc_set_locale(fonts.t, "pt"), DE_OK);
    EXPECT_EQ(de_loc_generation(fonts.t), generation);
    EXPECT_STREQ(de_ui__text_content(f->ui, play).ptr, "Play");
    locale_wait_public(&fonts, generation + 1);
    ui_frame(f);
    play = de_entity_get_const(f->world, labels[0], f->ui->ids.text);
    EXPECT_STREQ(de_ui__text_content(f->ui, play).ptr, "Jogar");
    const DeUiText *quit = de_entity_get_const(f->world, labels[1], f->ui->ids.text);
    EXPECT_STREQ(de_ui__text_content(f->ui, quit).ptr, "Sair");
    EXPECT_TRUE(de_ui_get_layout_stats(f->ui).nodes_laid_out <= 3u);
    expect_rect(ui_rect(f, labels[2]), static_rect.x, static_rect.y, static_rect.z, static_rect.w);
    EXPECT_FALSE(f->ui->records[de_ui__find(f->ui, labels[2])].dirty);
    de_ui_attach(f->ui, NULL);
    f->ui->desc.text = NULL;
    /* Exercise the shared fixture with active localization requests. */
    runtime_fx_teardown(&fonts);
    ASSERT_EQ(de_file_delete("uilocale:/strings/main/en.toml"), DE_OK);
    ASSERT_EQ(de_file_delete("uilocale:/strings/main/pt.toml"), DE_OK);
    ASSERT_EQ(de_file_delete("uilocale:/strings/main"), DE_OK);
    ASSERT_EQ(de_file_delete("uilocale:/strings"), DE_OK);
    ASSERT_EQ(de_file_delete("uilocale:/assets.toml"), DE_OK);
    ASSERT_EQ(de_fs_unmount("uilocale", directory), DE_OK);
    EXPECT_EQ(rmdir(directory), 0);
}
#endif
