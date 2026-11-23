/* test_layout_preview.c: de_ui_layout_preview, the editor's authoritative layout readback at a
 * preview viewport, and child order from de_entity_set_parent_at in flex layout (DENG-768). The
 * fixture's live viewport is 400x100 with reference height 100 (scale 1). */
#include "ui_test_util.h"

static de_entity sized(ui_fx *f, de_entity parent, f32 w, f32 h) {
    de_entity e = ui_node(f, parent);
    DeUiNode *n = ui_node_edit(f, e);
    n->width = (de_ui_len){DE_UI_PX, w};
    n->height = (de_ui_len){DE_UI_PX, h};
    return e;
}

static de_vec4 rect_of(const de_ui_preview_rect *r, u32 n, de_entity e) {
    for (u32 i = 0; i < n; ++i)
        if (r[i].entity.bits == e.bits)
            return r[i].rect;
    EXPECT_TRUE(!"entity missing from the preview");
    return (de_vec4){0};
}

TEST_F(ui_layout_preview, rects_at_preview_viewport_and_live_restored, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    ui_node_edit(f, root)->direction = DE_UI_ROW;
    de_entity a = sized(f, root, 50, 20), b = sized(f, root, 30, 10);
    ui_node_edit(f, a)->margin[3] = (de_ui_len){DE_UI_PX, 5};
    ui_frame(f);
    expect_rect(ui_rect(f, a), 5, 0, 50, 20);
    expect_rect(ui_rect(f, b), 55, 0, 30, 10);

    /* 800x200 is scale 2; ui_scale 1.5 makes it 3. */
    de_ui_preview_rect out[8];
    u32 count = 0;
    f32 scale = 0;
    de_ui_preview_viewport vp = {.width = 800, .height = 200, .ui_scale = 1.5f};
    ASSERT_EQ(de_ui_layout_preview(f->ui, root, &vp, out, 8, &count, &scale), DE_OK);
    ASSERT_EQ(count, 3u);
    EXPECT_FLOAT_EQ(scale, 3, 1e-5f);
    EXPECT_EQ(out[0].entity.bits, root.bits); /* pre-order: the root, then children in order */
    EXPECT_EQ(out[1].entity.bits, a.bits);
    EXPECT_EQ(out[2].entity.bits, b.bits);
    expect_rect(rect_of(out, count, root), 0, 0, 800, 200);
    expect_rect(rect_of(out, count, a), 15, 0, 150, 60);
    expect_rect(rect_of(out, count, b), 165, 0, 90, 30);

    /* The live rects are untouched, and the next frame keeps them. */
    expect_rect(ui_rect(f, a), 5, 0, 50, 20);
    expect_rect(ui_rect(f, b), 55, 0, 30, 10);
    EXPECT_FLOAT_EQ(f->ui->scale, 1, 1e-6f);
    ui_frame(f);
    expect_rect(ui_rect(f, a), 5, 0, 50, 20);
    expect_rect(ui_rect(f, b), 55, 0, 30, 10);
    EXPECT_FLOAT_EQ(de_ui_get_layout_stats(f->ui).scale, 1, 1e-6f);

    /* ui_scale <= 0 uses the ui.scale cvar (1). */
    vp.ui_scale = 0;
    ASSERT_EQ(de_ui_layout_preview(f->ui, root, &vp, out, 8, &count, &scale), DE_OK);
    EXPECT_FLOAT_EQ(scale, 2, 1e-5f);
    expect_rect(rect_of(out, count, a), 10, 0, 100, 40);
}

TEST_F(ui_layout_preview, safe_area_pixel_art_and_subtree, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    ui_node_edit(f, root)->safe_area = true;
    de_entity panel = ui_node(f, root);
    ui_node_edit(f, panel)->grow = 1;
    de_entity leaf = sized(f, panel, 10, 10);
    de_ui_preview_rect out[8];
    u32 count = 0;
    f32 scale = 0;
    /* Before any frame: the preview gathers the nodes itself. */
    de_ui_preview_viewport vp = {.width = 300,
                                 .height = 100,
                                 .ui_scale = 1,
                                 .safe = {.left = 20, .top = 10, .right = 30, .bottom = 15}};
    ASSERT_EQ(de_ui_layout_preview(f->ui, root, &vp, out, 8, &count, &scale), DE_OK);
    ASSERT_EQ(count, 3u);
    expect_rect(rect_of(out, count, root), 20, 10, 250, 75);
    expect_rect(rect_of(out, count, leaf), 20, 10, 10, 10);
    /* Pixel art: integer scale round(ui_scale), whatever the height. */
    vp = (de_ui_preview_viewport){.width = 320, .height = 180, .ui_scale = 2.4f, .pixel_art = true};
    ASSERT_EQ(de_ui_layout_preview(f->ui, root, &vp, out, 8, &count, &scale), DE_OK);
    EXPECT_FLOAT_EQ(scale, 2, 0);
    expect_rect(rect_of(out, count, leaf), 0, 0, 20, 20);
    EXPECT_FALSE(f->ui->desc.pixel_art);
    /* A subtree root reports only its subtree. */
    vp = (de_ui_preview_viewport){.width = 400, .height = 100, .ui_scale = 1};
    ASSERT_EQ(de_ui_layout_preview(f->ui, panel, &vp, out, 8, &count, NULL), DE_OK);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(out[0].entity.bits, panel.bits);
    EXPECT_EQ(out[1].entity.bits, leaf.bits);
    ui_frame(f);
    expect_rect(ui_rect(f, root), 0, 0, 400, 100);
}

TEST_F(ui_layout_preview, capacity_and_errors, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    for (u32 i = 0; i < 4; ++i)
        (void)sized(f, root, 10, 10);
    ui_frame(f);
    de_ui_preview_rect out[2];
    u32 count = 0;
    de_ui_preview_viewport vp = {.width = 400, .height = 100};
    /* Too small: the total count, and the first `capacity` rects. */
    ASSERT_EQ(de_ui_layout_preview(f->ui, root, &vp, out, 2, &count, NULL), DE_OK);
    EXPECT_EQ(count, 5u);
    EXPECT_EQ(out[0].entity.bits, root.bits);
    ASSERT_EQ(de_ui_layout_preview(f->ui, root, &vp, NULL, 0, &count, NULL), DE_OK);
    EXPECT_EQ(count, 5u);
    de_entity plain = de_entity_spawn(f->world);
    EXPECT_EQ(de_ui_layout_preview(f->ui, plain, &vp, out, 2, &count, NULL), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_ui_layout_preview(f->ui, (de_entity){0}, &vp, out, 2, &count, NULL),
              DE_ERR_NOT_FOUND);
    de_ui_preview_viewport zero = {.width = 0, .height = 100};
    EXPECT_EQ(de_ui_layout_preview(f->ui, root, &zero, out, 2, &count, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_layout_preview(f->ui, root, NULL, out, 2, &count, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_layout_preview(f->ui, root, &vp, out, 2, NULL, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_layout_preview(NULL, root, &vp, out, 2, &count, NULL),
              DE_ERR_INVALID_ARGUMENT);
}

TEST_F(ui_layout_preview, set_parent_at_reorders_flex_children, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    ui_node_edit(f, root)->direction = DE_UI_ROW;
    de_entity a = sized(f, root, 10, 10), b = sized(f, root, 20, 10), c = sized(f, root, 30, 10);
    ui_frame(f);
    expect_rect(ui_rect(f, c), 30, 0, 30, 10);
    /* One op moves c to the front; the row reflows on the next frame. */
    ASSERT_EQ(de_entity_set_parent_at(f->world, c, root, 0), DE_OK);
    ui_frame(f);
    expect_rect(ui_rect(f, c), 0, 0, 30, 10);
    expect_rect(ui_rect(f, a), 30, 0, 10, 10);
    expect_rect(ui_rect(f, b), 40, 0, 20, 10);
    /* A new node placed at index 1. */
    de_entity d = ui_node(f, (de_entity){0});
    ui_node_edit(f, d)->width = (de_ui_len){DE_UI_PX, 5};
    ui_node_edit(f, d)->height = (de_ui_len){DE_UI_PX, 5};
    ASSERT_EQ(de_entity_set_parent_at(f->world, d, root, 1), DE_OK);
    ui_frame(f);
    expect_rect(ui_rect(f, d), 30, 0, 5, 5);
    expect_rect(ui_rect(f, a), 35, 0, 10, 10);
}
