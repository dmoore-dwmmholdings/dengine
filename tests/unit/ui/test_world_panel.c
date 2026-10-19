#include "ui_test_util.h"
#include "ui/world/world_internal.h"
#include "ui/render/draw_list.h"
#include "ui/widgets/widgets_internal.h"
static de_entity panel_root(ui_fx *f) {
    de_entity root = ui_node(f, (de_entity){0});
    de_component panel = de_component_find(f->world, "DeUiWorldPanel");
    ASSERT_TRUE(panel.id);
    *(DeUiWorldPanel *)de_entity_add(f->world, root, panel) =
        (DeUiWorldPanel){.size_m = {2, 1}, .pixels_per_meter = 100, .depth_test = true};
    return root;
}
TEST_F(ui_world, per_root_panel_pixels_preserve_screen_root, ui_fx) {
    de_entity screen = ui_node(f, (de_entity){0});
    de_entity root = panel_root(f), child = ui_node(f, root);
    DeUiNode *node = ui_node_edit(f, child);
    node->width = (de_ui_len){DE_UI_PERCENT, 100};
    node->height = (de_ui_len){DE_UI_PERCENT, 100};
    ui_frame(f);
    expect_rect(ui_rect(f, root), 0, 0, 200, 100);
    expect_rect(ui_rect(f, child), 0, 0, 200, 100);
    expect_rect(ui_rect(f, screen), 0, 0, 400, 100);
    EXPECT_EQ(f->ui->width, 400u);
    EXPECT_EQ(f->ui->height, 100u);
    de_component panel = de_component_find(f->world, "DeUiWorldPanel");
    ((DeUiWorldPanel *)de_entity_get(f->world, root, panel))->size_m.x = 3;
    ui_frame(f);
    expect_rect(ui_rect(f, root), 0, 0, 300, 100);
    expect_rect(ui_rect(f, screen), 0, 0, 400, 100);
}
TEST_F(ui_world, analytic_projected_button_center_hits_at_hundred_km, ui_fx) {
    de_entity root = panel_root(f), button = ui_node(f, root);
    de_entity_add(f->world, button, f->ui->widgets->button);
    ui_node_edit(f, button)->width = (de_ui_len){DE_UI_PERCENT, 100};
    ui_node_edit(f, button)->height = (de_ui_len){DE_UI_PERCENT, 100};
    ui_frame(f);
    de_view_frame_info camera = {.render_origin = {100000, 100000, 0},
                                 .viewport_px = {0, 0, 800, 400},
                                 .output_width = 800,
                                 .output_height = 400};
    camera.view_matrix = de_m4_look_at((de_vec3){0, 0, 5}, (de_vec3){0}, (de_vec3){0, 1, 0});
    camera.projection = de_m4_perspective_rz(1, 2, .1f);
    camera.view_projection = de_m4_mul(&camera.projection, &camera.view_matrix);
    de_dvec3 origin;
    de_vec3 direction;
    ASSERT_TRUE(de_ui__world_camera_ray(&camera, (de_vec2){400, 200}, &origin, &direction));
    DeGlobalTransform transform = de_transform_identity();
    transform.position = (de_dvec3){100000, 100000, 0};
    DeUiWorldPanel panel = {.size_m = {2, 1}, .pixels_per_meter = 100};
    de_vec2 point;
    f32 distance;
    ASSERT_TRUE(de_ui__world_intersect(&panel, &transform, origin, direction, &point, &distance));
    EXPECT_FLOAT_EQ(point.x, 100, .001f);
    EXPECT_FLOAT_EQ(point.y, 50, .001f);
    EXPECT_TRUE(distance > 0);
    EXPECT_EQ(de_ui__hit_test(f->ui, root, point).bits, button.bits);
    de_ui__pointer_route(f->ui, root, point, true, false, true);
    de_event_reader reader = de_event_reader_create(f->world, f->ui->event);
    de_ui__pointer_route(f->ui, root, point, false, true, false);
    const de_ui_event *event = NULL;
    bool clicked = false;
    while ((event = de_event_next(f->world, &reader)))
        clicked |= event->kind == DE_UI_EV_CLICK && event->target.bits == button.bits;
    EXPECT_TRUE(clicked);
}
TEST(ui_world, ray_mapping_rotated_scaled_and_rejection) {
    DeUiWorldPanel panel = {.size_m = {2, 1}, .pixels_per_meter = 100};
    DeGlobalTransform t = de_transform_identity();
    t.rotation = de_quat_axis_angle((de_vec3){0, 1, 0}, DE_PI * .5f);
    t.scale = (de_vec3){2, 3, 1};
    de_vec2 point;
    f32 distance;
    ASSERT_TRUE(de_ui__world_intersect(&panel, &t, (de_dvec3){10, 0, 0}, (de_vec3){-1, 0, 0},
                                       &point, &distance));
    EXPECT_FLOAT_EQ(point.x, 100, .001f);
    EXPECT_FLOAT_EQ(point.y, 50, .001f);
    EXPECT_FLOAT_EQ(distance, 10, .001f);
    EXPECT_FALSE(de_ui__world_intersect(&panel, &t, (de_dvec3){10, 0, 0}, (de_vec3){0, 1, 0},
                                        &point, &distance));
    EXPECT_FALSE(de_ui__world_intersect(&panel, &t, (de_dvec3){10, 0, 0}, (de_vec3){1, 0, 0},
                                        &point, &distance));
    t.scale.x = 0;
    EXPECT_FALSE(de_ui__world_intersect(&panel, &t, (de_dvec3){10, 0, 0}, (de_vec3){-1, 0, 0},
                                        &point, &distance));
}
TEST_F(ui_world, extract_pose_snapshot_survives_next_simulation, ui_fx) {
    de_entity root = panel_root(f);
    DeTransform *transform = de_entity_add(f->world, root, f->ui->hierarchy.transform);
    *transform = de_transform_identity();
    transform->position = (de_dvec3){100000, 100000, -3};
    ui_frame(f);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    struct de_ui_render *r = f->ui->render;
    u32 first = r->published;
    ASSERT_TRUE(r->snapshots[first].roots[0].transform_valid);
    EXPECT_FLOAT_EQ(r->snapshots[first].roots[0].transform.position.x, 100000, 0);
    transform = de_entity_get(f->world, root, f->ui->hierarchy.transform);
    transform->position.x = 100002;
    ui_frame(f);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    EXPECT_TRUE(first != r->published);
    EXPECT_FLOAT_EQ(r->snapshots[first].roots[0].transform.position.x, 100000, 0);
    EXPECT_FLOAT_EQ(r->snapshots[r->published].roots[0].transform.position.x, 100002, 0);
}
