/* test_editor_pick.c: the editor's UI hit test and select hooks (DENG-779, item 3 of DENG-768):
 * de_ui_pick (the stack of UI nodes under a point, topmost first), de_ui_pick_ray (the nearest
 * world panel under an editor camera ray) and de_ui_node_world_corners (a selected node's outline
 * on its panel). The fixture's live viewport is 400x100 at scale 1, with no renderer. */
#include "ui_test_util.h"

static de_entity boxed(ui_fx *f, de_entity parent, f32 x, f32 y, f32 w, f32 h) {
    de_entity e = ui_node(f, parent);
    *(DeUiAnchor *)de_entity_add(f->world, e, f->ui->ids.anchor) =
        (DeUiAnchor){.offset_min = {x, y}, .offset_max = {x + w, y + h}};
    return e;
}
static de_entity panel_at(ui_fx *f, de_dvec3 position) {
    de_entity root = ui_node(f, (de_entity){0});
    de_component panel = de_component_find(f->world, "DeUiWorldPanel");
    ASSERT_TRUE(panel.id);
    *(DeUiWorldPanel *)de_entity_add(f->world, root, panel) =
        (DeUiWorldPanel){.size_m = {2, 1}, .pixels_per_meter = 100, .depth_test = true};
    de_transform t = de_transform_identity();
    t.position = position;
    *(DeTransform *)de_entity_add(f->world, root, f->ui->hierarchy.transform) = t;
    *(DeGlobalTransform *)de_entity_add(f->world, root, f->ui->hierarchy.global_transform) = t;
    return root;
}
static u32 pick(ui_fx *f, de_entity root, f32 x, f32 y, de_ui_pick_hit *out, u32 capacity) {
    u32 count = 99;
    EXPECT_EQ(de_ui_pick(f->ui, root, (de_vec2){x, y}, out, capacity, &count), DE_OK);
    return count;
}

TEST_F(ui_editor_pick, stack_is_topmost_first_and_hits_plain_nodes, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    de_entity a = boxed(f, root, 10, 10, 100, 50);
    de_entity b = boxed(f, root, 50, 20, 100, 50); /* later sibling: drawn over a */
    de_entity inner = boxed(f, b, 10, 10, 20, 20); /* anchors are parent-relative: (60, 30) */
    ui_frame(f);
    de_ui_pick_hit hits[8];
    ASSERT_EQ(pick(f, (de_entity){0}, 65, 35, hits, 8), 4u);
    EXPECT_EQ(hits[0].entity.bits, inner.bits);
    EXPECT_EQ(hits[1].entity.bits, b.bits);
    EXPECT_EQ(hits[2].entity.bits, a.bits);
    EXPECT_EQ(hits[3].entity.bits, root.bits);
    EXPECT_EQ(hits[0].root.bits, root.bits);
    EXPECT_EQ(hits[0].depth, 2u);
    EXPECT_EQ(hits[1].depth, 1u);
    EXPECT_EQ(hits[3].depth, 0u);
    expect_rect(hits[0].rect, 60, 30, 20, 20);
    /* Only a and the root under (20, 15). */
    ASSERT_EQ(pick(f, (de_entity){0}, 20, 15, hits, 8), 2u);
    EXPECT_EQ(hits[0].entity.bits, a.bits);
    /* Capacity: the count is the whole stack, out gets the topmost entries. */
    ASSERT_EQ(pick(f, (de_entity){0}, 65, 35, hits, 1), 4u);
    EXPECT_EQ(hits[0].entity.bits, inner.bits);
    u32 count = 0;
    ASSERT_EQ(de_ui_pick(f->ui, (de_entity){0}, (de_vec2){65, 35}, NULL, 0, &count), DE_OK);
    EXPECT_EQ(count, 4u);
    /* Outside every node. */
    ASSERT_EQ(pick(f, (de_entity){0}, 500, 35, hits, 8), 0u);
}

TEST_F(ui_editor_pick, z_order_popups_and_later_roots_come_first, ui_fx) {
    de_entity first = ui_node(f, (de_entity){0});
    de_entity raised = boxed(f, first, 0, 0, 100, 100);
    ui_node_edit(f, raised)->z_order = 5;
    de_entity later = boxed(f, first, 0, 0, 100, 100);
    ui_frame(f);
    de_ui_pick_hit hits[8];
    ASSERT_EQ(pick(f, (de_entity){0}, 5, 5, hits, 8), 3u);
    EXPECT_EQ(hits[0].entity.bits, raised.bits); /* z_order beats tree order */
    EXPECT_EQ(hits[1].entity.bits, later.bits);
    /* A second screen root is drawn after the first, so it is on top. */
    de_entity second = ui_node(f, (de_entity){0});
    ui_frame(f);
    ASSERT_EQ(pick(f, (de_entity){0}, 5, 5, hits, 8), 4u);
    EXPECT_EQ(hits[0].entity.bits, second.bits);
    EXPECT_EQ(hits[0].root.bits, second.bits);
    EXPECT_EQ(hits[1].entity.bits, raised.bits);
}

TEST_F(ui_editor_pick, clip_and_display_none_hide_nodes, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    de_entity clip = boxed(f, root, 0, 0, 50, 50);
    ui_node_edit(f, clip)->clip = true;
    de_entity spill = boxed(f, clip, 40, 0, 40, 20); /* half outside its clip */
    de_entity gone = boxed(f, root, 100, 0, 50, 50);
    ui_node_edit(f, gone)->display = DE_UI_NONE;
    ui_frame(f);
    de_ui_pick_hit hits[8];
    ASSERT_EQ(pick(f, (de_entity){0}, 45, 5, hits, 8), 3u);
    EXPECT_EQ(hits[0].entity.bits, spill.bits);
    ASSERT_EQ(pick(f, (de_entity){0}, 70, 5, hits, 8), 1u); /* the clipped half is not hit */
    EXPECT_EQ(hits[0].entity.bits, root.bits);
    ASSERT_EQ(pick(f, (de_entity){0}, 120, 5, hits, 8), 1u);
    EXPECT_EQ(hits[0].entity.bits, root.bits);
}

TEST_F(ui_editor_pick, roots_world_panels_and_arguments, ui_fx) {
    de_entity screen = ui_node(f, (de_entity){0});
    de_entity panel = panel_at(f, (de_dvec3){0, 0, 0});
    de_entity label = boxed(f, panel, 20, 10, 40, 20);
    ui_frame(f);
    de_ui_pick_hit hits[8];
    /* The screen pick leaves world panels out. */
    ASSERT_EQ(pick(f, (de_entity){0}, 30, 15, hits, 8), 1u);
    EXPECT_EQ(hits[0].entity.bits, screen.bits);
    /* A panel root: the point is in the panel's pixels. */
    ASSERT_EQ(pick(f, panel, 30, 15, hits, 8), 2u);
    EXPECT_EQ(hits[0].entity.bits, label.bits);
    EXPECT_EQ(hits[0].root.bits, panel.bits);
    EXPECT_EQ(hits[1].entity.bits, panel.bits);
    /* A screen root picks only its own subtree. */
    ASSERT_EQ(pick(f, screen, 30, 15, hits, 8), 1u);

    u32 count = 7;
    EXPECT_EQ(de_ui_pick(f->ui, label, (de_vec2){30, 15}, hits, 8, &count),
              DE_ERR_NOT_FOUND); /* not a root */
    EXPECT_EQ(count, 0u);
    de_entity plain = de_entity_spawn(f->world);
    ASSERT_TRUE(plain.bits);
    EXPECT_EQ(de_ui_pick(f->ui, plain, (de_vec2){30, 15}, hits, 8, &count), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_ui_pick(NULL, screen, (de_vec2){0, 0}, hits, 8, &count),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_pick(f->ui, screen, (de_vec2){0, 0}, hits, 8, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_pick(f->ui, screen, (de_vec2){0, 0}, NULL, 8, &count),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_pick(f->ui, screen, (de_vec2){NAN, 0}, hits, 8, &count),
              DE_ERR_INVALID_ARGUMENT);
}

TEST_F(ui_editor_pick, ray_picks_the_nearest_panel_and_its_stack, ui_fx) {
    /* Two 2x1 m panels facing +z, 100 px/m. The near one (z = 0) spans x 99999.5..100001.5,
     * the far one (z = -3) spans x 99999..100001, so only the far one covers x < 99999.5. */
    de_entity near = panel_at(f, (de_dvec3){100000.5, 0, 0});
    de_entity far = panel_at(f, (de_dvec3){100000, 0, -3});
    de_entity button = boxed(f, near, 0, 0, 50, 50); /* the near panel's top-left quarter */
    ui_frame(f);
    de_ui_panel_pick hit_panel;
    de_ui_pick_hit hits[8];
    u32 count = 0;
    /* Down -z through x = 99999.75, y = 0.25: the near panel's pixel (25, 25), on its button. */
    ASSERT_EQ(de_ui_pick_ray(f->ui, (de_dvec3){100000 - 0.25, 0.25, 5}, (de_vec3){0, 0, -1},
                             &hit_panel, hits, 8, &count),
              DE_OK);
    EXPECT_EQ(hit_panel.panel.bits, near.bits);
    EXPECT_FLOAT_EQ(hit_panel.pixel.x, 25, .01f);
    EXPECT_FLOAT_EQ(hit_panel.pixel.y, 25, .01f);
    EXPECT_FLOAT_EQ(hit_panel.distance, 5, .001f);
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(hits[0].entity.bits, button.bits);
    EXPECT_EQ(hits[1].entity.bits, near.bits);
    /* Left of the near panel, the ray reaches the far one. */
    ASSERT_EQ(de_ui_pick_ray(f->ui, (de_dvec3){99999.2, 0, 5}, (de_vec3){0, 0, -1}, &hit_panel,
                             hits, 8, &count),
              DE_OK);
    EXPECT_EQ(hit_panel.panel.bits, far.bits);
    EXPECT_FLOAT_EQ(hit_panel.pixel.x, 20, .01f);
    EXPECT_FLOAT_EQ(hit_panel.distance, 8, .001f);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(hits[0].entity.bits, far.bits);
    /* A miss: no panel, no hits. */
    ASSERT_EQ(de_ui_pick_ray(f->ui, (de_dvec3){0, 0, 5}, (de_vec3){0, 0, -1}, &hit_panel, hits,
                             8, &count),
              DE_OK);
    EXPECT_EQ(hit_panel.panel.bits, 0ull);
    EXPECT_EQ(count, 0u);
    /* Arguments. */
    EXPECT_EQ(de_ui_pick_ray(f->ui, (de_dvec3){0, 0, 5}, (de_vec3){0, 0, 0}, NULL, hits, 8,
                             &count),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_pick_ray(f->ui, (de_dvec3){NAN, 0, 5}, (de_vec3){0, 0, -1}, NULL, hits, 8,
                             &count),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_ui_pick_ray(f->ui, (de_dvec3){0, 0, 5}, (de_vec3){0, 0, -1}, NULL, hits, 8,
                             NULL),
              DE_ERR_INVALID_ARGUMENT);
}

TEST_F(ui_editor_pick, world_corners_outline_a_selected_node, ui_fx) {
    de_entity panel = panel_at(f, (de_dvec3){100000, 0, 0});
    de_entity node = boxed(f, panel, 50, 25, 100, 50); /* the panel's centre half */
    de_entity screen = ui_node(f, (de_entity){0});
    ui_frame(f);
    de_dvec3 c[4];
    de_entity owner = {0};
    ASSERT_EQ(de_ui_node_world_corners(f->ui, node, c, &owner), DE_OK);
    EXPECT_EQ(owner.bits, panel.bits);
    const f64 expect[4][2] = {{-.5, .25}, {.5, .25}, {.5, -.25}, {-.5, -.25}};
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ((f32)(c[i].x - 100000), (f32)expect[i][0], 1e-4f);
        EXPECT_FLOAT_EQ((f32)c[i].y, (f32)expect[i][1], 1e-4f);
        EXPECT_FLOAT_EQ((f32)c[i].z, 0, 1e-4f);
    }
    /* A ray back through the outline's centre selects the same node. */
    de_ui_pick_hit hits[4];
    u32 count = 0;
    ASSERT_EQ(de_ui_pick_ray(f->ui, (de_dvec3){100000, 0, 2}, (de_vec3){0, 0, -1}, NULL, hits, 4,
                             &count),
              DE_OK);
    ASSERT_TRUE(count > 0);
    EXPECT_EQ(hits[0].entity.bits, node.bits);
    /* Screen UI has no world outline. */
    EXPECT_EQ(de_ui_node_world_corners(f->ui, screen, c, NULL), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_ui_node_world_corners(f->ui, node, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
}
