#include "ui_test_util.h"
TEST_F(ui_scale, reference_pixels_cvar_and_integer_edges, ui_fx) {
    f->ui->desc.reference_height = 1080;
    de_ui_set_viewport(f->ui, 3840, 2160, (de_window_insets){0});
    de_entity root = ui_node(f, (de_entity){0}), child = ui_node(f, root);
    DeUiNode *n = ui_node_edit(f, child);
    n->width = (de_ui_len){DE_UI_PX, 100};
    n->height = (de_ui_len){DE_UI_PX, 50};
    ui_frame(f);
    expect_rect(ui_rect(f, child), 0, 0, 200, 100);
    ASSERT_EQ(de_cvar_set(f->ui->scale_cvar, "1.5"), DE_OK);
    ui_frame(f);
    expect_rect(ui_rect(f, child), 0, 0, 300, 150);
    n = ui_node_edit(f, child);
    n->width = (de_ui_len){DE_UI_PX, 33.33f};
    ui_frame(f);
    de_vec4 r = ui_rect(f, child);
    EXPECT_FLOAT_EQ(r.x, roundf(r.x), 0);
    EXPECT_FLOAT_EQ(r.z, roundf(r.z), 0);
    ASSERT_EQ(de_cvar_set(f->ui->scale_cvar, "1"), DE_OK);
}
TEST_F(ui_scale, safe_area_insets_and_stale_window, ui_fx) {
    de_ui_set_viewport(f->ui, 400, 100,
                       (de_window_insets){.left = 20, .top = 10, .right = 30, .bottom = 15});
    de_entity root = ui_node(f, (de_entity){0});
    ui_node_edit(f, root)->safe_area = true;
    de_entity child = ui_node(f, root);
    ui_node_edit(f, child)->grow = 1;
    ui_frame(f);
    expect_rect(ui_rect(f, root), 20, 10, 350, 75);
    expect_rect(ui_rect(f, child), 20, 10, 350, 75);
    de_window_insets insets = de_window_safe_area((de_window){0});
    EXPECT_EQ(insets.left, 0u);
    EXPECT_EQ(insets.top, 0u);
    EXPECT_EQ(insets.right, 0u);
    EXPECT_EQ(insets.bottom, 0u);
}
