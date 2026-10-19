#include "ui_test_util.h"
#include "ui/widgets/advanced_internal.h"
#include "../input/input_test_util.h"
typedef struct scroll_fx {
    ui_fx base;
    de_input *input;
    de_entity scroll, child;
} scroll_fx;
static void scroll_fx_setup(scroll_fx *f) {
    ui_fx_setup(&f->base);
    f->input = input_test_create(NULL);
    f->base.ui->desc.input = f->input;
    de_entity root = ui_node(&f->base, (de_entity){0});
    f->scroll = ui_node(&f->base, root);
    DeUiNode *n = ui_node_edit(&f->base, f->scroll);
    n->width = (de_ui_len){DE_UI_PX, 200};
    n->height = (de_ui_len){DE_UI_PX, 80};
    n->direction = DE_UI_COLUMN;
    DeUiScroll *v = de_entity_add(f->base.world, f->scroll, f->base.ui->advanced->scroll);
    v->vertical = true;
    f->child = ui_node(&f->base, f->scroll);
    n = ui_node_edit(&f->base, f->child);
    n->width = (de_ui_len){DE_UI_PX, 120};
    n->height = (de_ui_len){DE_UI_PX, 400};
    n->shrink = 0;
    ui_frame(&f->base);
    ui_frame(&f->base);
}
static void scroll_fx_teardown(scroll_fx *f) {
    ui_fx_teardown(&f->base);
    de_input_destroy(f->input);
}
TEST_F(ui_scroll, clipping_clamp_and_scroll_into_view, scroll_fx) {
    ui_fx *b = &f->base;
    de_ui_scroll_offset(b->ui, f->scroll, (de_vec2){0, 9999});
    ui_frame(b);
    const DeUiScroll *v = de_entity_get_const(b->world, f->scroll, b->ui->advanced->scroll);
    EXPECT_FLOAT_EQ(v->offset.y, 320, .01f);
    expect_rect(ui_rect(b, f->child), 0, -320, 120, 400);
    de_ui_scroll_offset(b->ui, f->scroll, (de_vec2){0, -10});
    ui_frame(b);
    v = de_entity_get_const(b->world, f->scroll, b->ui->advanced->scroll);
    EXPECT_FLOAT_EQ(v->offset.y, 0, 0);
    de_entity target = ui_node(b, f->child);
    DeUiAnchor *anchor = de_entity_add(b->world, target, b->ui->ids.anchor);
    anchor->offset_min = (de_vec2){0, 300};
    anchor->offset_max = (de_vec2){50, 320};
    ui_frame(b);
    de_ui_scroll_to(b->ui, f->scroll, target);
    ui_frame(b);
    v = de_entity_get_const(b->world, f->scroll, b->ui->advanced->scroll);
    EXPECT_FLOAT_EQ(v->offset.y, 240, .01f);
}
TEST_F(ui_scroll, injected_wheel_and_thumb_drag_with_momentum, scroll_fx) {
    ui_fx *b = &f->base;
    input_test_mouse_move(f->input, 20, 20, 0, 0);
    input_test_mouse_wheel(f->input, 0, -1);
    input_test_frame(f->input);
    ASSERT_EQ(de_world_run_schedule(b->world, DE_SCHEDULE_PRE_UPDATE, INPUT_TEST_DT, 0), DE_OK);
    ui_frame(b);
    const DeUiScroll *v = de_entity_get_const(b->world, f->scroll, b->ui->advanced->scroll);
    EXPECT_TRUE(v->offset.y > 40);
    f32 first = v->offset.y;
    ui_frame(b);
    v = de_entity_get_const(b->world, f->scroll, b->ui->advanced->scroll);
    EXPECT_TRUE(v->offset.y > first);
    de_ui_scroll_offset(b->ui, f->scroll, (de_vec2){0, 0});
    de_ui_scroll_state *state = de_entity_get(b->world, f->scroll, b->ui->advanced->scroll_state);
    state->velocity = (de_vec2){0};
    ui_frame(b);
    de__input_event ev = input_test_event(DE_RAW_MOUSE_BUTTON);
    ev.mouse_button.button = DE_MOUSE_LEFT;
    ev.mouse_button.down = true;
    ev.mouse_button.x = 195;
    ev.mouse_button.y = 5;
    de__input_inject(f->input, &ev);
    input_test_frame(f->input);
    ASSERT_EQ(de_world_run_schedule(b->world, DE_SCHEDULE_PRE_UPDATE, INPUT_TEST_DT, 0), DE_OK);
    input_test_mouse_move(f->input, 195, 65, 0, 60);
    input_test_frame(f->input);
    ASSERT_EQ(de_world_run_schedule(b->world, DE_SCHEDULE_PRE_UPDATE, INPUT_TEST_DT, 0), DE_OK);
    v = de_entity_get_const(b->world, f->scroll, b->ui->advanced->scroll);
    EXPECT_TRUE(v->offset.y > 250);
    EXPECT_TRUE(de_ui_pointer_consumed(b->ui));
}
