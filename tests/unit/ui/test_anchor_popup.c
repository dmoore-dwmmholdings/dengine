#include "ui_test_util.h"
TEST_F(ui_anchor, top_right_across_viewports_and_popup_safe_clamp, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0}), hud = ui_node(f, root);
    DeUiAnchor *anchor = de_entity_add(f->world, hud, f->ui->ids.anchor);
    *anchor = (DeUiAnchor){.anchor_min = {1, 0},
                           .anchor_max = {1, 0},
                           .offset_min = {-100, 10},
                           .offset_max = {-10, 40}};
    ui_frame(f);
    expect_rect(ui_rect(f, hud), 300, 10, 90, 30);
    de_ui_set_viewport(f->ui, 800, 100, (de_window_insets){0});
    ui_frame(f);
    expect_rect(ui_rect(f, hud), 700, 10, 90, 30);
    de_entity popup = ui_node(f, root);
    DeUiNode *n = ui_node_edit(f, popup);
    n->width = (de_ui_len){DE_UI_PX, 150};
    n->height = (de_ui_len){DE_UI_PX, 70};
    DeUiPopup *p = de_entity_add(f->world, popup, f->ui->ids.popup);
    *p = (DeUiPopup){.anchor = hud, .placement = DE_UI_POPUP_BOTTOM, .modal = true};
    de_ui_set_viewport(f->ui, 800, 100,
                       (de_window_insets){.left = 5, .top = 5, .right = 20, .bottom = 10});
    ui_frame(f);
    expect_rect(ui_rect(f, popup), 630, 20, 150, 70);
    p = de_entity_get(f->world, popup, f->ui->ids.popup);
    p->placement = DE_UI_POPUP_LEFT;
    ui_frame(f);
    expect_rect(ui_rect(f, popup), 550, 10, 150, 70);
}
