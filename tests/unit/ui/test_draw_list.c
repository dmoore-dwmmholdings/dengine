#include "ui_test_util.h"
#include "ui/render/draw_list.h"
#include "de_alloc_guard.h"
#include <stdio.h>
static DeUiStyle *style(ui_fx *f, de_entity entity, de_vec4 color, f32 opacity) {
    DeUiStyle *s = de_entity_add(f->world, entity, f->ui->ids.style);
    *s = (DeUiStyle){.background = color, .opacity = opacity, .text_color = {1, 1, 1, 1}};
    return s;
}
static void dimensions(ui_fx *f, de_entity e, f32 w, f32 h) {
    DeUiNode *n = ui_node_edit(f, e);
    n->width = (de_ui_len){DE_UI_PX, w};
    n->height = (de_ui_len){DE_UI_PX, h};
    n->shrink = 0;
}
static struct de_ui_render *render(ui_fx *f) {
    return f->ui->render;
}
TEST_F(ui_draw, command_generation_opacity_sort_popup, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    style(f, root, (de_vec4){.1f, .1f, .1f, 1}, .5f);
    de_entity a = ui_node(f, root), b = ui_node(f, root), popup = ui_node(f, root);
    dimensions(f, a, 20, 20);
    dimensions(f, b, 20, 20);
    dimensions(f, popup, 20, 20);
    style(f, a, (de_vec4){1, 0, 0, 1}, .5f);
    style(f, b, (de_vec4){0, 1, 0, 1}, 1);
    style(f, popup, (de_vec4){0, 0, 1, 1}, 1);
    ui_node_edit(f, a)->z_order = 10;
    ui_node_edit(f, b)->z_order = -1;
    ui_node_edit(f, popup)->z_order = -100;
    DeUiPopup *p = de_entity_add(f->world, popup, f->ui->ids.popup);
    *p = (DeUiPopup){.anchor = a, .placement = DE_UI_POPUP_OVERLAY};
    ui_frame(f);
    struct de_ui_render *r = render(f);
    ASSERT_EQ(r->build.count, 4u);
    EXPECT_FLOAT_EQ(r->build.commands[0].gpu.color.y, 1, 0); /* child z=-1 precedes root z=0 */
    EXPECT_FLOAT_EQ(r->build.commands[2].gpu.color.x, 1, 0);
    EXPECT_FLOAT_EQ(r->build.commands[2].gpu.color.w, .25f, 0);
    EXPECT_FLOAT_EQ(r->build.commands[3].gpu.color.z, 1, 0); /* popup wins despite low z */
}
TEST(ui_draw, nine_slice_three_sizes_and_small_destination) {
    de_ui_gpu_quad quads[9];
    for (u32 width = 100; width <= 300; width += 100) {
        ASSERT_EQ(de_ui__nine_slice((de_vec4){10, 20, (f32)width, 80}, (de_vec4){0, 0, 1, 1},
                                    (de_vec4){8, 6, 8, 6}, 32, 24, 2, quads),
                  9u);
        expect_rect(quads[0].rect, 10, 20, 16, 12);
        expect_rect(quads[8].rect, (f32)width - 6, 88, 16, 12);
        EXPECT_FLOAT_EQ(quads[4].uv.x, .25f, 1e-6f);
        EXPECT_FLOAT_EQ(quads[4].uv.y, .25f, 1e-6f);
        EXPECT_FLOAT_EQ(quads[4].uv.z, .75f, 1e-6f);
        EXPECT_FLOAT_EQ(quads[4].uv.w, .75f, 1e-6f);
    }
    u32 count = de_ui__nine_slice((de_vec4){0, 0, 8, 6}, (de_vec4){0, 0, 1, 1},
                                  (de_vec4){8, 6, 8, 6}, 32, 24, 1, quads);
    for (u32 i = 0; i < count; ++i) {
        EXPECT_TRUE(quads[i].rect.x + quads[i].rect.z <= 8);
        EXPECT_TRUE(quads[i].rect.y + quads[i].rect.w <= 6);
    }
    EXPECT_EQ(de_ui__nine_slice((de_vec4){0, 0, 10, 10}, (de_vec4){0, 0, 1, 1},
                                (de_vec4){1, 1, 1, 1}, 0, 24, 1, quads),
              0u);
}
TEST_F(ui_draw, nested_clip_intersection, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0}), parent = ui_node(f, root),
              child = ui_node(f, parent);
    dimensions(f, parent, 100, 50);
    ui_node_edit(f, parent)->clip = true;
    dimensions(f, child, 200, 80);
    style(f, child, (de_vec4){1, 1, 1, 1}, 1);
    ui_frame(f);
    struct de_ui_render *r = render(f);
    ASSERT_EQ(r->build.count, 1u);
    expect_rect(r->build.commands[0].clip, 0, 0, 100, 50);
    expect_rect(de_ui__clip_intersect((de_vec4){0, 0, 10, 10}, (de_vec4){20, 20, 10, 10}), 20, 20,
                0, 0);
}
TEST_F(ui_draw, overflow_truncates_without_overwrite, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    for (u32 i = 0; i < 8; ++i) {
        de_entity e = ui_node(f, root);
        dimensions(f, e, 10, 10);
        style(f, e, (de_vec4){1, 1, 1, 1}, 1);
    }
    de_cvar max = render(f)->max_quads;
    ASSERT_EQ(de_cvar_set(max, "2"), DE_OK);
    ui_frame(f);
    EXPECT_EQ(render(f)->build.count, 2u);
    EXPECT_EQ(de_ui_get_render_stats(f->ui).truncated, 6u);
    ASSERT_EQ(de_cvar_set(max, "65536"), DE_OK);
}
TEST_F(ui_draw, extract_copy_preserves_previous_frame, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    style(f, root, (de_vec4){1, 0, 0, 1}, 1);
    ui_frame(f);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    struct de_ui_render *r = render(f);
    u32 first = r->published;
    u64 token = r->snapshots[first].generation;
    style(f, root, (de_vec4){0, 1, 0, 1}, 1);
    ui_frame(f);
    EXPECT_FLOAT_EQ(r->snapshots[first].commands[0].gpu.color.x, 1, 0);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    EXPECT_TRUE(first != r->published);
    EXPECT_TRUE(token != r->snapshots[r->published].generation);
    EXPECT_FLOAT_EQ(r->snapshots[first].commands[0].gpu.color.x, 1, 0);
    EXPECT_FLOAT_EQ(r->snapshots[r->published].commands[0].gpu.color.y, 1, 0);
}
TEST_F(ui_draw, two_thousand_widgets_static_zero_alloc_build_extract, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0});
    for (u32 i = 0; i < 2000; ++i) {
        de_entity e = ui_node(f, root);
        dimensions(f, e, 4, 4);
        style(f, e, (de_vec4){1, 1, 1, 1}, 1);
    }
    for (u32 i = 0; i < 8; ++i) {
        ui_frame(f);
        ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
    }
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    f64 total = 0;
    for (u32 i = 0; i < 1000; ++i) {
        ui_frame(f);
        ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, 0, 0), DE_OK);
        total += de_ui_get_render_stats(f->ui).build_extract_ms;
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    printf("2000-widget build+Extract mean %.4fms, quads%u\n", total / 1000,
           de_ui_get_render_stats(f->ui).quads);
    EXPECT_EQ(de_ui_get_render_stats(f->ui).quads, 2000u);
    EXPECT_TRUE(total / 1000 <= .5);
}
