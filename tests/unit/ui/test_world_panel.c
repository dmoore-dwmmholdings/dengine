#include "ui_test_util.h"
#include "ui/world/world_internal.h"
#include "ui/render/draw_list.h"
#include "ui/widgets/widgets_internal.h"
#include <stdio.h>
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
TEST_F(ui_world, popups_use_panel_extent_scale_and_ignore_screen_safe_insets, ui_fx) {
    de_ui_set_viewport(f->ui, 400, 200,
                       (de_window_insets){.left = 10, .top = 20, .right = 30, .bottom = 40});
    de_entity roots[] = {panel_root(f), ui_node(f, DE_ENTITY_NULL)};
    de_entity popups[2], children[2];
    for (u32 i = 0; i < 2; ++i) {
        de_entity anchor = ui_node(f, roots[i]);
        *(DeUiAnchor *)de_entity_add(f->world, anchor, f->ui->ids.anchor) =
            (DeUiAnchor){.offset_min = {180, 80}, .offset_max = {190, 90}};
        /* A nested popup still belongs to the containing root's coordinate space. */
        de_entity container = ui_node(f, roots[i]);
        popups[i] = ui_node(f, container);
        DeUiNode *node = ui_node_edit(f, popups[i]);
        node->width = (de_ui_len){DE_UI_PX, 60};
        node->height = (de_ui_len){DE_UI_PX, 40};
        node->safe_area = true;
        *(DeUiPopup *)de_entity_add(f->world, popups[i], f->ui->ids.popup) =
            (DeUiPopup){.anchor = anchor, .placement = DE_UI_POPUP_BOTTOM};
        children[i] = ui_node(f, popups[i]);
        node = ui_node_edit(f, children[i]);
        node->width = (de_ui_len){DE_UI_PX, 10};
        node->height = (de_ui_len){DE_UI_PX, 5};
    }
    ui_frame(f);
    expect_rect(ui_rect(f, popups[0]), 140, 60, 60, 40);
    expect_rect(ui_rect(f, children[0]), 140, 60, 10, 5);
    expect_rect(ui_rect(f, popups[1]), 250, 80, 120, 80);
    expect_rect(ui_rect(f, children[1]), 250, 80, 20, 10);
    EXPECT_FLOAT_EQ(f->ui->scale, 2, 0);
    /* Resize only the panel. Screen placement and scale must remain unchanged. */
    DeUiWorldPanel *panel =
        de_entity_get(f->world, roots[0], de_component_find(f->world, "DeUiWorldPanel"));
    panel->size_m = (de_vec2){1, .5f};
    ui_frame(f);
    expect_rect(ui_rect(f, popups[0]), 40, 10, 60, 40);
    expect_rect(ui_rect(f, children[0]), 40, 10, 10, 5);
    expect_rect(ui_rect(f, popups[1]), 250, 80, 120, 80);
    EXPECT_FLOAT_EQ(f->ui->scale, 2, 0);
}
TEST_F(ui_world, popup_percent_and_oversize_clamps_are_independent_between_panels, ui_fx) {
    de_ui_set_viewport(f->ui, 800, 300, (de_window_insets){.left = 25, .top = 15});
    de_entity first = panel_root(f), second = panel_root(f);
    DeUiWorldPanel *panel =
        de_entity_get(f->world, second, de_component_find(f->world, "DeUiWorldPanel"));
    panel->size_m = (de_vec2){1, .5f};
    de_entity roots[] = {first, second};
    for (u32 i = 0; i < 2; ++i) {
        de_entity popup = ui_node(f, roots[i]);
        DeUiNode *node = ui_node_edit(f, popup);
        node->width = (de_ui_len){DE_UI_PERCENT, 150};
        node->height = (de_ui_len){DE_UI_PERCENT, 125};
        de_entity_add(f->world, popup, f->ui->ids.popup);
        ui_frame(f);
        expect_rect(ui_rect(f, popup), 0, 0, i ? 100 : 200, i ? 50 : 100);
    }
    EXPECT_FLOAT_EQ(f->ui->scale, 3, 0);
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
#if DE_HAS_GPU_METAL
#include "render/world/render_world.h"
#include <stdlib.h>
static const u8 *world_native_capture(ui_fx *f, de_gpu_texture target, de_gpu_buffer buffer) {
    ui_frame(f);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    ASSERT_EQ(de__render_extract_and_kick(f->renderer, f->world), DE_OK);
    de_gpu_wait_idle(f->device);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(f->device, DE_QUEUE_GRAPHICS, "world UI native readback");
    de_gpu_barrier barrier = {
        .texture = target, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, buffer, 0, 512 * 4, target, 0, 0);
    barrier.before = DE_ACCESS_COPY_SRC;
    barrier.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    ASSERT_EQ(de_gpu_queue_submit(f->device, DE_QUEUE_GRAPHICS,
                                  &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}),
              DE_OK);
    de_gpu_wait_idle(f->device);
    return de_gpu_buffer_map(f->device, buffer);
}
TEST_F(ui_world, native_actual_panel_consumer_and_large_origin_readback, ui_fx) {
    de_ui_destroy(f->ui);
    f->ui = NULL;
    if (de_gpu_device_create(&(de_gpu_device_desc){.backend = DE_GPU_METAL,
                                                   .debug_layer = true,
                                                   .pipeline_cache_dir = ""},
                             &f->device) != DE_OK)
        SKIP("Metal device unavailable");
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->device, .types = f->types},
                                 &f->renderer),
              DE_OK);
    de_renderer_attach(f->renderer, f->world);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "0"), DE_OK);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    settings.ao = settings.gi = settings.reflections = settings.shadows = DE_QUALITY_OFF;
    de_renderer_set_settings(f->renderer, &settings);
    de_gpu_texture target = de_gpu_texture_create(
        f->device, &(de_gpu_texture_desc){.width = 512,
                                          .height = 512,
                                          .format = DE_FMT_RGBA8_UNORM,
                                          .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED |
                                                   DE_USAGE_COPY_SRC});
    de_view view =
        de_view_create(f->renderer, &(de_view_desc){.target = target, .name = "world-ui-unit"});
    ASSERT_EQ(de_ui_create(&(de_ui_desc){.renderer = f->renderer,
                                         .view = view,
                                         .reference_height = 512,
                                         .max_nodes = 32},
                           &f->ui),
              DE_OK);
    de_ui_attach(f->ui, f->world);
    de_entity root = panel_root(f);
    DeUiStyle *style = de_entity_add(f->world, root, f->ui->ids.style);
    *style = (DeUiStyle){.background = {0, 1, 0, 1}, .opacity = 1};
    DeTransform transform = de_transform_identity();
    transform.position.z = -3;
    *(DeTransform *)de_entity_add(f->world, root, f->ui->hierarchy.transform) = transform;
    de_entity camera = de_entity_spawn(f->world);
    *(DeTransform *)de_entity_add(f->world, camera, f->ui->hierarchy.transform) =
        de_transform_identity();
    de_component camera_type = de_component_find(f->world, "DeCamera");
    *(DeCamera *)de_entity_add(f->world, camera, camera_type) = (DeCamera){
        .view = view, .projection = DE_PROJ_ORTHOGRAPHIC, .ortho_height = 2, .near_z = .1f};
    de_gpu_buffer buffer =
        de_gpu_buffer_create(f->device, &(de_gpu_buffer_desc){.size = 512 * 512 * 4,
                                                              .usage = DE_USAGE_COPY_DST,
                                                              .memory = DE_MEM_READBACK});
    (void)world_native_capture(f, target, buffer);
    const u8 *pixels = world_native_capture(f, target, buffer);
    const u8 *center = pixels + ((size_t)256 * 512 + 256) * 4;
    printf("World UI native center RGB %u %u %u\n", center[0], center[1], center[2]);
    ASSERT_TRUE(center[1] > 100 && center[0] < 20 && center[2] < 20);
    ASSERT_TRUE(de_ui_get_render_stats(f->ui).draw_calls > 0);
    u8 *original = malloc(512 * 512 * 4);
    ASSERT_TRUE(original);
    memcpy(original, pixels, 512 * 512 * 4);
    DeTransform *pose = de_entity_get(f->world, root, f->ui->hierarchy.transform);
    pose->position.x = pose->position.y = 100000;
    pose = de_entity_get(f->world, camera, f->ui->hierarchy.transform);
    pose->position.x = pose->position.y = 100000;
    pixels = world_native_capture(f, target, buffer);
    u32 changed = 0;
    for (u32 i = 0; i < 512 * 512 * 4; ++i)
        changed += abs((int)pixels[i] - (int)original[i]) > 1;
    printf("World UI100km differing codes>1: %u\n", changed);
    EXPECT_EQ(changed, 0u);
    free(original);
    de_gpu_buffer_destroy(f->device, buffer);
    de_gpu_texture_destroy(f->device, target);
    ASSERT_EQ(de_cvar_set(de_cvar_find("r.pipelined"), "1"), DE_OK);
}
#endif
