#include "ui_test_util.h"
#include "ui/world/world_internal.h"
#include "ui/render/draw_list.h"
TEST_F(ui_pixel, integer_scale_and_all_edges_on_internal_grid, ui_fx) {
    f->ui->desc.pixel_art = true;
    ASSERT_EQ(de_cvar_set(f->ui->scale_cvar, "1.7"), DE_OK);
    de_entity root = ui_node(f, (de_entity){0}), child = ui_node(f, root);
    DeUiNode *node = ui_node_edit(f, child);
    node->width = (de_ui_len){DE_UI_PX, 10.25f};
    node->height = (de_ui_len){DE_UI_PX, 8.125f};
    DeUiStyle *style = de_entity_add(f->world, child, f->ui->ids.style);
    *style = (DeUiStyle){.background = {1, 1, 1, 1}, .opacity = 1};
    ui_frame(f);
    EXPECT_FLOAT_EQ(f->ui->scale, 2, 0);
    struct de_ui_render *r = f->ui->render;
    ASSERT_TRUE(r->build.count > 0);
    for (u32 i = 0; i < r->build.count; ++i) {
        const de_vec4 rect = r->build.commands[i].gpu.rect;
        EXPECT_FLOAT_EQ(rect.x, roundf(rect.x), 0);
        EXPECT_FLOAT_EQ(rect.y, roundf(rect.y), 0);
        EXPECT_FLOAT_EQ(rect.x + rect.z, roundf(rect.x + rect.z), 0);
        EXPECT_FLOAT_EQ(rect.y + rect.w, roundf(rect.y + rect.w), 0);
    }
    ASSERT_EQ(de_cvar_set(f->ui->scale_cvar, "1"), DE_OK);
}
TEST_F(ui_pixel, letterbox_pointer_mapping_and_normal_path_unchanged, ui_fx) {
    struct de_ui_render *r = f->ui->render;
    f->ui->desc.pixel_art = true;
    r->pixel_layout_valid = true;
    r->pixel_layout = (de_view_frame_info){.pixel_art = true,
                                           .pixel = {.internal_width = 480, .internal_height = 270},
                                           .viewport_px = {10, 20, 2560, 1440},
                                           .content_px = {80, 45, 2400, 1350}};
    de_vec2 point = de_ui__screen_pointer(f->ui, (de_vec2){10 + 80 + 5 * 120, 20 + 45 + 5 * 50});
    EXPECT_FLOAT_EQ(point.x, 120, 0);
    EXPECT_FLOAT_EQ(point.y, 50, 0);
    f->ui->desc.pixel_art = false;
    point = de_ui__screen_pointer(f->ui, (de_vec2){123, 456});
    EXPECT_FLOAT_EQ(point.x, 123, 0);
    EXPECT_FLOAT_EQ(point.y, 456, 0);
}
TEST(ui_pixel, expansion_edges_snap_consistently) {
    de_vec4 rect = {.25f, .75f, 10.5f, 5.5f};
    de_ui__pixel_snap_rect(&rect);
    EXPECT_FLOAT_EQ(rect.x, 0, 0);
    EXPECT_FLOAT_EQ(rect.y, 1, 0);
    EXPECT_FLOAT_EQ(rect.z, 11, 0);
    EXPECT_FLOAT_EQ(rect.w, 5, 0);
}
#if DE_HAS_RENDER
#include "render/world/render_world.h"
TEST_F(ui_pixel, advisory_resize_mismatch_skips_packet_until_matching_extract, ui_fx) {
    de_ui_destroy(f->ui);
    f->ui = NULL;
    ASSERT_EQ(de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_NULL}, &f->device),
              DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->device, .types = f->types},
                                 &f->renderer),
              DE_OK);
    de_renderer_attach(f->renderer, f->world);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "0"), DE_OK);
    de_view view = de_renderer_main_view(f->renderer);
    de_pixel_art_settings settings = {
        .internal_width = 480, .internal_height = 270, .integer_scale_only = true};
    de_view_set_pixel_art(f->renderer, view, &settings);
    ASSERT_EQ(de_ui_create(
                  &(de_ui_desc){
                      .renderer = f->renderer, .view = view, .pixel_art = true, .max_nodes = 32},
                  &f->ui),
              DE_OK);
    de_ui_attach(f->ui, f->world);
    de_entity root = ui_node(f, (de_entity){0});
    DeUiStyle *style = de_entity_add(f->world, root, f->ui->ids.style);
    *style = (DeUiStyle){.background = {1, 1, 1, 1}, .opacity = 1};
    de_component camera_type = de_component_find(f->world, "DeCamera");
    de_entity camera = de_entity_spawn(f->world);
    *(DeTransform *)de_entity_add(f->world, camera, f->ui->hierarchy.transform) =
        de_transform_identity();
    *(DeCamera *)de_entity_add(f->world, camera, camera_type) = (DeCamera){
        .view = view, .projection = DE_PROJ_ORTHOGRAPHIC, .ortho_height = 270, .near_z = .1f};
    ui_frame(f);
    EXPECT_EQ(f->ui->width, 480u);
    EXPECT_EQ(f->ui->height, 270u);
    settings.internal_width = 320;
    settings.internal_height = 180;
    de_view_set_pixel_art(f->renderer, view, &settings);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    struct de_ui_render *r = f->ui->render;
    EXPECT_EQ(r->published, UINT32_MAX); /* Never submit old-grid quads with new-grid view data. */
    ASSERT_EQ(de__render_extract_and_kick(f->renderer, f->world), DE_OK);
    de_gpu_wait_idle(f->device);
    ui_frame(f);
    EXPECT_EQ(f->ui->width, 320u);
    EXPECT_EQ(f->ui->height, 180u);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    ASSERT_TRUE(r->published != UINT32_MAX);
    EXPECT_EQ(r->snapshots[r->published].camera.pixel.internal_width, 320u);
    EXPECT_TRUE(r->snapshots[r->published].camera_valid);
    ASSERT_EQ(de__render_extract_and_kick(f->renderer, f->world), DE_OK);
    de_gpu_wait_idle(f->device);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "1"), DE_OK);
}
#endif
