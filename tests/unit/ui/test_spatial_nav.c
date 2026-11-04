#include "nav_test_util.h"
TEST_F(ui_spatial, grid_all_cells_every_direction_no_implicit_wrap, nav_fx) {
    de_entity grid[9];
    for (u32 i = 0; i < 9; ++i)
        grid[i] =
            nav_node(f, f->root, (f32)(i % 3) * 70, (f32)(i / 3) * 60, 0, DE_UI_WIDGET_BUTTON);
    nav_frame(f);
    for (u32 i = 0; i < 9; ++i) {
        EXPECT_EQ(de_ui__spatial_nav(f->base.ui, grid[i], DE_UI_NAV_UP).bits,
                  i >= 3 ? grid[i - 3].bits : 0u);
        EXPECT_EQ(de_ui__spatial_nav(f->base.ui, grid[i], DE_UI_NAV_DOWN).bits,
                  i < 6 ? grid[i + 3].bits : 0u);
        EXPECT_EQ(de_ui__spatial_nav(f->base.ui, grid[i], DE_UI_NAV_LEFT).bits,
                  i % 3 ? grid[i - 1].bits : 0u);
        EXPECT_EQ(de_ui__spatial_nav(f->base.ui, grid[i], DE_UI_NAV_RIGHT).bits,
                  i % 3 < 2 ? grid[i + 1].bits : 0u);
    }
}
TEST_F(ui_spatial, irregular_edge_gap_tie_tree_order_explicit_hidden_disabled, nav_fx) {
    de_entity source = nav_node(f, f->root, 0, 100, 0, DE_UI_WIDGET_BUTTON);
    /* Spawn in the reverse of tree order, to catch accidental archetype/entity sorting. */
    de_entity second = nav_node(f, DE_ENTITY_NULL, 80, 130, 0, DE_UI_WIDGET_BUTTON);
    de_entity first = nav_node(f, f->root, 80, 70, 0, DE_UI_WIDGET_BUTTON);
    ASSERT_EQ(de_entity_set_parent(f->base.world, second, f->root), DE_OK);
    de_entity far = nav_node(f, f->root, 200, 100, 0, DE_UI_WIDGET_BUTTON);
    nav_frame(f);
    EXPECT_EQ(de_ui__spatial_nav(f->base.ui, source, DE_UI_NAV_RIGHT).bits, first.bits);
    DeUiFocusable *links = de_entity_get(f->base.world, source, f->base.ui->focus->focusable);
    links->right = far;
    EXPECT_EQ(de_ui__spatial_nav(f->base.ui, source, DE_UI_NAV_RIGHT).bits, far.bits);
    DeUiInteractionState *disabled = de_entity_get(f->base.world, far, f->base.ui->ids.state);
    disabled->disabled = true;
    EXPECT_EQ(de_ui__spatial_nav(f->base.ui, source, DE_UI_NAV_RIGHT).bits, first.bits);
    ui_node_edit(&f->base, first)->display = DE_UI_NONE;
    EXPECT_EQ(de_ui__spatial_nav(f->base.ui, source, DE_UI_NAV_RIGHT).bits, second.bits);
    links = de_entity_get(f->base.world, source, f->base.ui->focus->focusable);
    links->left = second;
    EXPECT_EQ(de_ui__spatial_nav(f->base.ui, source, DE_UI_NAV_LEFT).bits, second.bits);
}
TEST_F(ui_spatial, tab_index_then_tree_order_and_negative_programmatic_focus, nav_fx) {
    de_entity a = nav_node(f, f->root, 0, 0, 2, DE_UI_WIDGET_BUTTON);
    de_entity b = nav_node(f, f->root, 70, 0, 0, DE_UI_WIDGET_BUTTON);
    de_entity c = nav_node(f, f->root, 140, 0, 0, DE_UI_WIDGET_BUTTON);
    de_entity d = nav_node(f, f->root, 210, 0, -1, DE_UI_WIDGET_BUTTON);
    nav_frame(f);
    de_ui *ui = f->base.ui;
    EXPECT_EQ(de_ui__tab_nav(ui, DE_ENTITY_NULL, false).bits, b.bits);
    EXPECT_EQ(de_ui__tab_nav(ui, b, false).bits, c.bits);
    EXPECT_EQ(de_ui__tab_nav(ui, c, false).bits, a.bits);
    EXPECT_EQ(de_ui__tab_nav(ui, a, false).bits, b.bits);
    EXPECT_EQ(de_ui__tab_nav(ui, b, true).bits, a.bits);
    de_ui_set_focus(ui, d);
    EXPECT_EQ(de_ui_focused(ui).bits, d.bits);
}
