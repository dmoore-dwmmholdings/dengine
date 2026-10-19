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
