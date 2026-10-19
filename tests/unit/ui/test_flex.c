#include "ui_test_util.h"
TEST_F(ui_flex, grow_justify_stretch_reverse, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0}), children[3];
    for (u32 i = 0; i < 3; ++i) {
        children[i] = ui_node(f, root);
        ui_node_edit(f, children[i])->grow = i == 1 ? 2 : 1;
    }
    ui_frame(f);
    expect_rect(ui_rect(f, children[0]), 0, 0, 100, 100);
    expect_rect(ui_rect(f, children[1]), 100, 0, 200, 100);
    expect_rect(ui_rect(f, children[2]), 300, 0, 100, 100);
    for (u32 i = 0; i < 3; ++i) {
        DeUiNode *n = ui_node_edit(f, children[i]);
        n->grow = 0;
        n->width = (de_ui_len){DE_UI_PX, 50};
    }
    ui_node_edit(f, root)->justify = DE_UI_ALIGN_SPACE_BETWEEN;
    ui_frame(f);
    expect_rect(ui_rect(f, children[0]), 0, 0, 50, 100);
    expect_rect(ui_rect(f, children[1]), 175, 0, 50, 100);
    expect_rect(ui_rect(f, children[2]), 350, 0, 50, 100);
    DeUiNode *n = ui_node_edit(f, root);
    n->direction = DE_UI_ROW_REVERSE;
    n->justify = DE_UI_ALIGN_START;
    ui_frame(f);
    expect_rect(ui_rect(f, children[0]), 350, 0, 50, 100);
    expect_rect(ui_rect(f, children[2]), 250, 0, 50, 100);
    n = ui_node_edit(f, root);
    n->direction = DE_UI_ROW;
    n->justify = DE_UI_ALIGN_SPACE_AROUND;
    ui_frame(f);
    EXPECT_FLOAT_EQ(ui_rect(f, children[0]).x, 41.6667f, .5f);
}
TEST_F(ui_flex, percent_min_max_shrink_padding_margins, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0}), a = ui_node(f, root), b = ui_node(f, root);
    DeUiNode *n = ui_node_edit(f, root);
    n->padding[3] = (de_ui_len){DE_UI_PX, 10};
    n->padding[1] = (de_ui_len){DE_UI_PX, 10};
    n->gap = 10;
    n = ui_node_edit(f, a);
    n->width = (de_ui_len){DE_UI_PERCENT, 50};
    n->max_width = (de_ui_len){DE_UI_PX, 100};
    n->shrink = 0;
    n->height = (de_ui_len){DE_UI_PX, 20};
    n->align_self = DE_UI_ALIGN_CENTER;
    n = ui_node_edit(f, b);
    n->grow = 1;
    n->min_width = (de_ui_len){DE_UI_PX, 50};
    n->margin[1] = (de_ui_len){DE_UI_PX, 5};
    ui_frame(f);
    expect_rect(ui_rect(f, a), 10, 40, 100, 20);
    expect_rect(ui_rect(f, b), 120, 0, 265, 100);
    n = ui_node_edit(f, a);
    n->width = (de_ui_len){DE_UI_PX, 300};
    n->max_width = (de_ui_len){DE_UI_AUTO, 0};
    n->shrink = 1;
    n = ui_node_edit(f, b);
    n->width = (de_ui_len){DE_UI_PX, 300};
    n->grow = 0;
    ui_frame(f);
    EXPECT_TRUE(ui_rect(f, a).z + ui_rect(f, b).z <= 365.5f);
    EXPECT_TRUE(ui_rect(f, b).z >= 50);
}
TEST_F(ui_flex, column_nested_and_alignment, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    DeUiNode *n = ui_node_edit(f, root);
    n->direction = DE_UI_COLUMN;
    n->align_items = DE_UI_ALIGN_END;
    de_entity a = ui_node(f, root), b = ui_node(f, a);
    n = ui_node_edit(f, a);
    n->width = (de_ui_len){DE_UI_PX, 100};
    n->height = (de_ui_len){DE_UI_PX, 50};
    n->padding[0] = (de_ui_len){DE_UI_PX, 5};
    n = ui_node_edit(f, b);
    n->grow = 1;
    ui_frame(f);
    expect_rect(ui_rect(f, a), 300, 0, 100, 50);
    expect_rect(ui_rect(f, b), 300, 5, 100, 45);
}
