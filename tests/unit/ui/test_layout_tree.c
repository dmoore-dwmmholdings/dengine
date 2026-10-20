#include "ui_test_util.h"
#if defined(DE_UI_TEST_FONTS)
#include "../text/runtime_test_util.h"
#endif
TEST_F(ui_tree, hierarchy_reparent_remove_display_and_reflection, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0}), a = ui_node(f, root), b = ui_node(f, root),
              leaf = ui_node(f, a);
    for (u32 i = 0; i < 2; ++i) {
        DeUiNode *n = ui_node_edit(f, i ? b : a);
        n->width = (de_ui_len){DE_UI_PX, 200};
        n->height = (de_ui_len){DE_UI_PX, 100};
    }
    ui_node_edit(f, leaf)->grow = 1;
    ui_frame(f);
    expect_rect(ui_rect(f, leaf), 0, 0, 200, 100);
    ASSERT_EQ(de_entity_set_parent(f->world, leaf, b), DE_OK);
    ui_frame(f);
    expect_rect(ui_rect(f, leaf), 200, 0, 200, 100);
    ui_node_edit(f, b)->display = DE_UI_NONE;
    ui_frame(f);
    expect_rect(ui_rect(f, leaf), 0, 0, 0, 0);
    ui_node_edit(f, b)->display = DE_UI_FLEX;
    ui_frame(f);
    EXPECT_TRUE(ui_rect(f, leaf).z > 0);
    de_entity_despawn_recursive(f->world, a);
    ui_frame(f);
    EXPECT_EQ(de_ui_get_layout_stats(f->ui).total_nodes, 3u);
    const de_type_desc *type = de_type_get(f->types, de_type_find(f->types, "DeUiNode"));
    ASSERT_TRUE(type);
    bool rect = false;
    for (u32 i = 0; i < type->field_count; ++i)
        if (!strcmp(type->fields[i].name, "computed_rect")) {
            rect = true;
            EXPECT_TRUE(type->fields[i].flags & DE_FIELD_READONLY);
            EXPECT_FALSE(type->fields[i].flags & DE_FIELD_SERIALIZE);
            EXPECT_TRUE(!strcmp(type->fields[i].attrs.units, "px"));
        }
    EXPECT_TRUE(rect);
    EXPECT_TRUE(f->ui->resource.id);
    EXPECT_TRUE(f->ui->event.id);
}
TEST_F(ui_tree, incremental_leaf_and_5000_node_budget, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0}), groups[10], leaf = {0};
    ui_node_edit(f, root)->direction = DE_UI_COLUMN;
    for (u32 i = 0; i < 10; ++i) {
        groups[i] = ui_node(f, root);
        DeUiNode *n = ui_node_edit(f, groups[i]);
        n->height = (de_ui_len){DE_UI_PX, 10};
    }
    for (u32 i = 0; i < 4989; ++i) {
        de_entity e = ui_node(f, groups[i % 10]);
        DeUiNode *n = ui_node_edit(f, e);
        n->width = (de_ui_len){DE_UI_PX, 1};
        n->height = (de_ui_len){DE_UI_PX, 5};
        if (i == 0)
            leaf = e;
    }
    u64 start = de_time_now_ns();
    ui_frame(f);
    u64 full = de_time_now_ns() - start;
    de_ui_layout_stats stats = de_ui_get_layout_stats(f->ui);
    EXPECT_EQ(stats.total_nodes, 5000u);
    EXPECT_EQ(stats.nodes_laid_out, 5000u);
    printf("5000 UI nodes full layout %.3fms\n", (double)full / 1e6);
#if defined(DE_DEV)
    EXPECT_LE(full, 2000000u);
#endif
    ui_frame(f);
    EXPECT_EQ(de_ui_get_layout_stats(f->ui).nodes_laid_out, 0u);
    ui_node_edit(f, leaf)->width.value = 2;
    ui_frame(f);
    stats = de_ui_get_layout_stats(f->ui);
    EXPECT_GT(stats.nodes_laid_out, 1u);
    EXPECT_LT(stats.nodes_laid_out, 600u);
}
TEST_F(ui_tree, static_2000_nodes_1000_frames_zero_heap, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    for (u32 i = 0; i < 1999; ++i) {
        de_entity e = ui_node(f, root);
        DeUiNode *n = ui_node_edit(f, e);
        n->width = (de_ui_len){DE_UI_PX, 1};
    }
    for (u32 i = 0; i < 4; ++i)
        ui_frame(f);
    u64 before = 0;
    for (u32 tag = 0; tag < DE_MEM_TAG_BUILTIN_COUNT_; ++tag)
        before += de_mem_stats_for_tag(tag).alloc_count;
    for (u32 i = 0; i < 1000; ++i)
        ui_frame(f);
    u64 after = 0;
    for (u32 tag = 0; tag < DE_MEM_TAG_BUILTIN_COUNT_; ++tag)
        after += de_mem_stats_for_tag(tag).alloc_count;
    EXPECT_EQ(after, before);
    EXPECT_EQ(de_ui_get_layout_stats(f->ui).nodes_laid_out, 0u);
}
TEST_F(ui_tree, text_measure_and_bounded_authored_string, ui_fx) {
#if defined(DE_UI_TEST_FONTS)
    runtime_fx fonts = {0};
    runtime_fx_setup(&fonts);
    f->ui->desc.text = fonts.t;
    de_entity root = ui_node(f, (de_entity){0});
    ui_node_edit(f, root)->direction = DE_UI_COLUMN;
    de_entity text = ui_node(f, root);
    DeUiText *label = de_entity_add(f->world, text, f->ui->ids.text);
    label->style.font = fonts.latin;
    label->style.size_px = 20;
    memcpy(label->text, "The quick brown fox wraps over multiple lines", 45);
    de_ui_set_viewport(f->ui, 100, 100, (de_window_insets){0});
    ui_node_edit(f, text)->shrink = 0;
    ui_frame(f);
    /* Widget/style publication can move the entity between archetypes. */
    label = de_entity_get(f->world, text, f->ui->ids.text);
    de_text_layout *layout;
    de_text_layout_desc desc = {
        .utf8 = de_ui__text_content(f->ui, label), .style = &label->style, .max_width = 100};
    ASSERT_EQ(de_text_layout_build(fonts.t, &desc, NULL, &layout), DE_OK);
    EXPECT_FLOAT_EQ(ui_rect(f, text).w, de_text_layout_size(layout).y, .5f);
    EXPECT_TRUE(ui_rect(f, text).z <= 100);
    EXPECT_TRUE(ui_rect(f, text).w > 20);
    de_text_layout_free(layout);
    memset(label->text, 'X', sizeof label->text);
    de_entity_mark_changed(f->world, text, f->ui->ids.text);
    de_str invalid = de_ui__text_content(f->ui, label);
    EXPECT_EQ(invalid.len, 0u);
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    f->ui->desc.text = NULL;
    runtime_fx_teardown(&fonts);
#else
    SKIP("font cooker disabled");
#endif
}
#if defined(DE_UI_TEST_FONTS)
static void distributed_text_height(ui_fx *f, bool percent) {
    runtime_fx fonts = {0};
    runtime_fx_setup(&fonts);
    f->ui->desc.text = fonts.t;
    de_entity root = ui_node(f, (de_entity){0}), text = ui_node(f, root), other = ui_node(f, root);
    ui_node_edit(f, root)->align_items = DE_UI_ALIGN_START;
    DeUiNode *node = ui_node_edit(f, text);
    node->width = (de_ui_len){percent ? DE_UI_PERCENT : DE_UI_PX, percent ? 50.f : 300.f};
    for (u32 i = 0; i < 4; ++i)
        node->padding[i] = (de_ui_len){DE_UI_PX, 5};
    ui_node_edit(f, other)->width = (de_ui_len){DE_UI_PX, percent ? 200.f : 300.f};
    DeUiText *label = de_entity_add(f->world, text, f->ui->ids.text);
    label->style.font = fonts.latin;
    label->style.size_px = 20;
    memcpy(label->text,
           "The quick brown fox jumps over the lazy dog. The quick brown fox "
           "jumps over the lazy dog.",
           sizeof("The quick brown fox jumps over the lazy dog. The quick brown fox "
                  "jumps over the lazy dog."));
    ui_frame(f);
    label = de_entity_get(f->world, text, f->ui->ids.text);
    de_text_layout *layout = NULL;
    de_text_layout_desc desc = {
        .utf8 = de_ui__text_content(f->ui, label), .style = &label->style, .max_width = 190};
    ASSERT_EQ(de_text_layout_build(fonts.t, &desc, NULL, &layout), DE_OK);
    f32 height = roundf(de_text_layout_size(layout).y + 10);
    de_text_layout_free(layout);
    EXPECT_FLOAT_EQ(ui_rect(f, text).z, 200, .5f);
    EXPECT_FLOAT_EQ(ui_rect(f, text).w, height, .5f);
    ui_frame(f);
    EXPECT_FLOAT_EQ(ui_rect(f, text).w, height, .5f);
    de_ui_set_viewport(f->ui, 600, 100, (de_window_insets){0});
    ui_frame(f);
    label = de_entity_get(f->world, text, f->ui->ids.text);
    desc = (de_text_layout_desc){
        .utf8 = de_ui__text_content(f->ui, label), .style = &label->style, .max_width = 290};
    ASSERT_EQ(de_text_layout_build(fonts.t, &desc, NULL, &layout), DE_OK);
    EXPECT_FLOAT_EQ(ui_rect(f, text).z, 300, .5f);
    EXPECT_FLOAT_EQ(ui_rect(f, text).w, roundf(de_text_layout_size(layout).y + 10), .5f);
    de_text_layout_free(layout);
    f->ui->desc.text = NULL;
    runtime_fx_teardown(&fonts);
}
#endif
TEST_F(ui_tree, percent_basis_text_wraps_at_distributed_width, ui_fx) {
#if defined(DE_UI_TEST_FONTS)
    distributed_text_height(f, true);
#else
    SKIP("font cooker disabled");
#endif
}
TEST_F(ui_tree, shrunk_fixed_basis_text_wraps_at_distributed_width, ui_fx) {
#if defined(DE_UI_TEST_FONTS)
    distributed_text_height(f, false);
#else
    SKIP("font cooker disabled");
#endif
}
#if defined(DE_UI_ASSET_ROOT) && DE_HAS_RENDER
#include "render/renderer_internal.h"
#include "render/loaders/render_assets.h"
#endif
TEST_F(ui_tree, actual_image_intrinsic_texture_uv_size, ui_fx) {
#if defined(DE_UI_ASSET_ROOT) && DE_HAS_RENDER
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &f->device),
              DE_OK);
    ASSERT_EQ(de_asset_system_create(NULL, &f->assets), DE_OK);
    ASSERT_EQ(de_renderer_create(
                  &(de_renderer_desc){.device = f->device, .assets = f->assets, .types = f->types},
                  &f->renderer),
              DE_OK);
    f->ui->desc.renderer = f->renderer;
    ASSERT_EQ(de_fs_mount("uiassets", DE_UI_ASSET_ROOT, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "uiassets:/tests/unit/ui/assets.toml"), DE_OK);
    de_asset_id id = de_asset_id_from("ui/checker");
    de_texture texture = de_asset_texture(f->assets, id);
    f->load = de_asset_load(f->assets, id, 0);
    u64 end = de_time_now_ns() + 30000000000ULL;
    while ((de_asset_status(f->assets, f->load, NULL) == DE_LOAD_PENDING ||
            de_asset_status(f->assets, f->load, NULL) == DE_LOAD_LOADING) &&
           de_time_now_ns() < end) {
        de_render_uploader_flush(f->renderer->uploader);
        de_render_uploader_acquire_submit(f->renderer->uploader);
        de_asset_system_update(f->assets, NULL);
        de_sleep_ns(100000);
    }
    ASSERT_EQ(de_asset_status(f->assets, f->load, NULL), (int)DE_LOAD_READY);
    u32 width = 99, height = 99;
    EXPECT_EQ(de_render_texture_size(NULL, texture, &width, &height), false);
    EXPECT_EQ(width, 0u);
    EXPECT_EQ(height, 0u);
    EXPECT_EQ(de_render_texture_size(f->renderer, texture, &width, &height), true);
    EXPECT_EQ(width, 16u);
    EXPECT_EQ(height, 8u);
    EXPECT_EQ(de_render_texture_size(f->renderer, (de_texture){0}, &width, &height), false);
    EXPECT_EQ(width, 0u);
    EXPECT_EQ(height, 0u);
    de_renderer_output_size(f->renderer, &width, &height);
    EXPECT_EQ(width, 1280u);
    EXPECT_EQ(height, 720u);
    de_renderer_output_size(NULL, &width, &height);
    EXPECT_EQ(width, 0u);
    EXPECT_EQ(height, 0u);
    de_renderer_output_size(f->renderer, NULL, NULL);

    de_entity root = ui_node(f, (de_entity){0});
    ui_node_edit(f, root)->align_items = DE_UI_ALIGN_START;
    de_entity image = ui_node(f, root);
    DeUiImage *component = de_entity_add(f->world, image, f->ui->ids.image);
    component->texture = texture;
    component->uv_rect = (de_vec4){0, 0, .5f, 1};
    ui_frame(f);
    expect_rect(ui_rect(f, image), 0, 0, 8, 8);
#else
    SKIP("source texture importer or renderer disabled");
#endif
}
typedef struct ui_serial_buffer {
    char bytes[8192];
    size_t length, offset;
} ui_serial_buffer;
static de_result ui_write(void *arg, const void *data, size_t count) {
    ui_serial_buffer *b = arg;
    if (count >= sizeof b->bytes - b->length)
        return DE_ERR_OUT_OF_MEMORY;
    memcpy(b->bytes + b->length, data, count);
    b->length += count;
    b->bytes[b->length] = 0;
    return DE_OK;
}
static de_result ui_read(void *arg, void *data, size_t count, size_t *read) {
    ui_serial_buffer *b = arg;
    if (count > b->length - b->offset)
        count = b->length - b->offset;
    memcpy(data, b->bytes + b->offset, count);
    b->offset += count;
    *read = count;
    return DE_OK;
}
TEST_F(ui_tree, authored_reflection_roundtrip_excludes_computed_and_runtime_pointer, ui_fx) {
    DeUiNode node = {.width = {DE_UI_PERCENT, 50}, .gap = 7, .computed_rect = {1, 2, 3, 4}};
    ui_serial_buffer buffer = {0};
    de_writer writer = {ui_write, &buffer};
    de_ser_options options = {.format = DE_SER_TEXT, .required_flags = DE_FIELD_SERIALIZE};
    ASSERT_EQ(
        de_serialize_object(f->types, de_type_find(f->types, "DeUiNode"), &node, &options, &writer),
        DE_OK);
    EXPECT_TRUE(strstr(buffer.bytes, "computed_rect") == NULL);
    DeUiNode restored = {0};
    de_reader reader = {ui_read, &buffer};
    ASSERT_EQ(de_deserialize_object(f->types, de_type_find(f->types, "DeUiNode"), &restored,
                                    &options, &reader),
              DE_OK);
    EXPECT_EQ(restored.width.unit, DE_UI_PERCENT);
    EXPECT_FLOAT_EQ(restored.width.value, 50, 0);
    EXPECT_FLOAT_EQ(restored.gap, 7, 0);
    EXPECT_FLOAT_EQ(restored.computed_rect.z, 0, 0);
    DeUiText text = {
        .literal = DE_STR("transient runtime"), .style = {.size_px = 20}, .text = "authored label"};
    memset(&buffer, 0, sizeof buffer);
    ASSERT_EQ(
        de_serialize_object(f->types, de_type_find(f->types, "DeUiText"), &text, &options, &writer),
        DE_OK);
    EXPECT_TRUE(strstr(buffer.bytes, "authored label") != NULL);
    EXPECT_TRUE(strstr(buffer.bytes, "transient runtime") == NULL);
}
