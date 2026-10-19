#include "ui_test_util.h"
#include "ui/widgets/widgets_internal.h"
#include "ui/render/draw_list.h"
#include "../input/input_test_util.h"
typedef struct pointer_fx {
    ui_fx base;
    de_input *input;
} pointer_fx;
static void pointer_fx_setup(pointer_fx *f) {
    ui_fx_setup(&f->base);
    f->input = input_test_create(NULL);
    ASSERT_TRUE(f->input);
    f->base.ui->desc.input = f->input;
}
static void pointer_fx_teardown(pointer_fx *f) {
    ui_fx_teardown(&f->base);
    de_input_destroy(f->input);
}
static de_entity pointer_button(ui_fx *f, de_entity parent, f32 width, f32 height) {
    de_entity e = ui_node(f, parent);
    DeUiNode *node = ui_node_edit(f, e);
    node->width = (de_ui_len){DE_UI_PX, width};
    node->height = (de_ui_len){DE_UI_PX, height};
    DeUiButton *button = de_entity_add(f->world, e, f->ui->widgets->button);
    button->on_click = de_name_from("activate");
    return e;
}
static void pointer_button_at(pointer_fx *f, bool down, f32 x, f32 y) {
    de__input_event event = input_test_event(DE_RAW_MOUSE_BUTTON);
    event.mouse_button.button = DE_MOUSE_LEFT;
    event.mouse_button.down = down;
    event.mouse_button.x = x;
    event.mouse_button.y = y;
    event.mouse_button.clicks = 1;
    de__input_inject(f->input, &event);
}
static void pointer_frame(pointer_fx *f) {
    input_test_frame(f->input);
    ASSERT_EQ(de_world_run_schedule(f->base.world, DE_SCHEDULE_PRE_UPDATE, INPUT_TEST_DT, 0),
              DE_OK);
    ui_frame(&f->base);
}
static u32 clicks(ui_fx *f, de_event_reader *reader, de_entity expected, de_name action) {
    u32 count = 0;
    const de_ui_event *event;
    while ((event = de_event_next(f->world, reader)))
        if (event->kind == DE_UI_EV_CLICK) {
            ++count;
            EXPECT_EQ(event->target.bits, expected.bits);
            EXPECT_EQ(event->action.hash, action.hash);
        }
    return count;
}
TEST_F(ui_pointer, synthetic_input_capture_click_action_and_consumption, pointer_fx) {
    ui_fx *b = &f->base;
    de_entity root = ui_node(b, (de_entity){0}), a = pointer_button(b, root, 80, 40),
              other = pointer_button(b, root, 80, 40);
    (void)other;
    ui_frame(b);
    de_event_reader reader = de_event_reader_create(b->world, b->ui->event);
    input_test_mouse_move(f->input, 20, 20, 0, 0);
    pointer_button_at(f, true, 20, 20);
    pointer_frame(f);
    EXPECT_EQ(b->ui->widgets->pointer.captured.bits, a.bits);
    EXPECT_TRUE(de_ui_pointer_consumed(b->ui));
    input_test_mouse_move(f->input, 120, 20, 100, 0);
    pointer_button_at(f, false, 120, 20);
    pointer_frame(f);
    EXPECT_EQ(clicks(b, &reader, a, de_name_from("activate")), 0u);
    input_test_mouse_move(f->input, 20, 20, -100, 0);
    pointer_button_at(f, true, 20, 20);
    pointer_frame(f);
    pointer_button_at(f, false, 20, 20);
    pointer_frame(f);
    EXPECT_EQ(clicks(b, &reader, a, de_name_from("activate")), 1u);
    EXPECT_EQ(b->ui->widgets->pointer.captured.bits, 0u);
    input_test_mouse_move(f->input, 300, 90, 280, 70);
    pointer_frame(f);
    EXPECT_FALSE(de_ui_pointer_consumed(b->ui));
}
TEST_F(ui_pointer, clip_z_order_popup_and_root_local_hit, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0}), clip = ui_node(f, root);
    DeUiNode *node = ui_node_edit(f, clip);
    node->width = (de_ui_len){DE_UI_PX, 100};
    node->height = (de_ui_len){DE_UI_PX, 60};
    node->clip = true;
    de_entity outside = pointer_button(f, clip, 80, 40);
    DeUiAnchor *anchor = de_entity_add(f->world, outside, f->ui->ids.anchor);
    anchor->offset_min = (de_vec2){150, 0};
    anchor->offset_max = (de_vec2){230, 40};
    de_entity a = pointer_button(f, root, 80, 40), b = pointer_button(f, root, 80, 40);
    anchor = de_entity_add(f->world, a, f->ui->ids.anchor);
    anchor->offset_min = (de_vec2){20, 20};
    anchor->offset_max = (de_vec2){100, 60};
    anchor = de_entity_add(f->world, b, f->ui->ids.anchor);
    anchor->offset_min = (de_vec2){20, 20};
    anchor->offset_max = (de_vec2){100, 60};
    ui_node_edit(f, b)->z_order = 5;
    ui_frame(f);
    EXPECT_EQ(de_ui__hit_test(f->ui, (de_entity){0}, (de_vec2){160, 20}).bits, 0u);
    EXPECT_EQ(de_ui__hit_test(f->ui, (de_entity){0}, (de_vec2){30, 30}).bits, b.bits);
    ui_node_edit(f, a)->z_order = 100;
    DeUiPopup *popup = de_entity_add(f->world, b, f->ui->ids.popup);
    popup->anchor = a;
    popup->placement = DE_UI_POPUP_OVERLAY;
    ui_frame(f);
    EXPECT_EQ(de_ui__hit_test(f->ui, (de_entity){0}, (de_vec2){30, 30}).bits, b.bits);
    EXPECT_EQ(de_ui__hit_test(f->ui, root, (de_vec2){30, 30}).bits, b.bits);
}
TEST_F(ui_pointer, disabled_button_has_no_hover_or_press_and_disabled_style, pointer_fx) {
    ui_fx *b = &f->base;
    de_entity root = ui_node(b, (de_entity){0}), e = pointer_button(b, root, 80, 40);
    ((DeUiButton *)de_entity_get(b->world, e, b->ui->widgets->button))->disabled = true;
    ui_frame(b);
    input_test_mouse_move(f->input, 20, 20, 0, 0);
    pointer_button_at(f, true, 20, 20);
    pointer_frame(f);
    const DeUiInteractionState *state = de_entity_get_const(b->world, e, b->ui->ids.state);
    ASSERT_TRUE(state);
    EXPECT_TRUE(state->disabled);
    EXPECT_FALSE(state->hovered);
    EXPECT_FALSE(state->pressed);
    EXPECT_EQ(b->ui->widgets->pointer.captured.bits, 0u);
    const DeUiStyle *style = de_entity_get_const(b->world, e, b->ui->ids.style);
    ASSERT_TRUE(style);
    EXPECT_FLOAT_EQ(style->opacity, .6f, .00001f);
}
TEST_F(ui_pointer, slider_drag_snaps_clamps_and_emits_once_per_change, pointer_fx) {
    ui_fx *b = &f->base;
    de_entity root = ui_node(b, (de_entity){0}), e = ui_node(b, root);
    DeUiNode *node = ui_node_edit(b, e);
    node->width = (de_ui_len){DE_UI_PX, 200};
    node->height = (de_ui_len){DE_UI_PX, 40};
    *(DeUiSlider *)de_entity_add(b->world, e, b->ui->widgets->slider) =
        (DeUiSlider){.min = 0, .max = 10, .step = .5f};
    ui_frame(b);
    de_event_reader reader = de_event_reader_create(b->world, b->ui->event);
    input_test_mouse_move(f->input, 74, 20, 0, 0);
    pointer_button_at(f, true, 74, 20);
    pointer_frame(f);
    const DeUiSlider *slider = de_entity_get_const(b->world, e, b->ui->widgets->slider);
    EXPECT_FLOAT_EQ(slider->value, 3.5f, .00001f);
    input_test_mouse_move(f->input, 74, 20, 0, 0);
    pointer_frame(f);
    u32 changed = 0;
    const de_ui_event *event;
    while ((event = de_event_next(b->world, &reader)))
        if (event->kind == DE_UI_EV_CHANGED) {
            ++changed;
            EXPECT_FLOAT_EQ(event->value, 3.5f, .00001f);
        }
    EXPECT_EQ(changed, 1u);
    input_test_mouse_move(f->input, 800, 20, 726, 0);
    pointer_button_at(f, false, 800, 20);
    pointer_frame(f);
    slider = de_entity_get_const(b->world, e, b->ui->widgets->slider);
    EXPECT_FLOAT_EQ(slider->value, 10, .00001f);
    changed = 0;
    while ((event = de_event_next(b->world, &reader)))
        if (event->kind == DE_UI_EV_CHANGED)
            ++changed;
    EXPECT_EQ(changed, 1u);
}
