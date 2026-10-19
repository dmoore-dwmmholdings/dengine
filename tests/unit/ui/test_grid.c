#include "ui_test_util.h"
TEST_F(ui_grid, fixed_fr_auto_percent_and_spans, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    ui_node_edit(f, root)->display = DE_UI_GRID;
    DeUiGrid *grid = de_entity_add(f->world, root, f->ui->ids.grid);
    *grid = (DeUiGrid){.columns = {{DE_UI_PX, 100}, {DE_UI_FR, 1}, {DE_UI_FR, 2}},
                       .column_count = 3,
                       .rows = {{DE_UI_FR, 1}},
                       .row_count = 1};
    de_entity c[3];
    for (u32 i = 0; i < 3; ++i)
        c[i] = ui_node(f, root);
    ui_frame(f);
    expect_rect(ui_rect(f, c[0]), 0, 0, 100, 100);
    expect_rect(ui_rect(f, c[1]), 100, 0, 100, 100);
    expect_rect(ui_rect(f, c[2]), 200, 0, 200, 100);
    DeUiGridItem *item = de_entity_add(f->world, c[0], f->ui->ids.grid_item);
    *item = (DeUiGridItem){.column = 1, .row = 0, .column_span = 2, .row_span = 1};
    ui_frame(f);
    expect_rect(ui_rect(f, c[0]), 100, 0, 300, 100);
    EXPECT_FLOAT_EQ(ui_rect(f, c[1]).x, 0, .5f);
    EXPECT_TRUE(ui_rect(f, c[2]).y >= 100);
    grid = de_entity_get(f->world, root, f->ui->ids.grid);
    *grid = (DeUiGrid){.columns = {{DE_UI_PERCENT, 25}, {DE_UI_AUTO, 0}, {DE_UI_FR, 1}},
                       .column_count = 3,
                       .column_gap = 10,
                       .rows = {{DE_UI_PX, 30}, {DE_UI_FR, 1}},
                       .row_count = 2,
                       .row_gap = 10};
    de_entity_remove(f->world, c[0], f->ui->ids.grid_item);
    ui_node_edit(f, c[1])->width = (de_ui_len){DE_UI_PX, 60};
    ui_frame(f);
    expect_rect(ui_rect(f, c[0]), 0, 0, 100, 30);
    expect_rect(ui_rect(f, c[1]), 110, 0, 60, 30);
    expect_rect(ui_rect(f, c[2]), 180, 0, 220, 30);
}
TEST_F(ui_grid, row_major_auto_placement_and_row_spans, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    ui_node_edit(f, root)->display = DE_UI_GRID;
    DeUiGrid *grid = de_entity_add(f->world, root, f->ui->ids.grid);
    *grid = (DeUiGrid){.columns = {{DE_UI_FR, 1}, {DE_UI_FR, 1}},
                       .column_count = 2,
                       .rows = {{DE_UI_PX, 20}, {DE_UI_PX, 30}},
                       .row_count = 2,
                       .row_gap = 5};
    de_entity a = ui_node(f, root), b = ui_node(f, root), c = ui_node(f, root);
    DeUiGridItem *span = de_entity_add(f->world, a, f->ui->ids.grid_item);
    *span = (DeUiGridItem){.column = 0, .row = 0, .column_span = 1, .row_span = 2};
    ui_frame(f);
    expect_rect(ui_rect(f, a), 0, 0, 200, 55);
    expect_rect(ui_rect(f, b), 200, 0, 200, 20);
    expect_rect(ui_rect(f, c), 200, 25, 200, 30);
}
