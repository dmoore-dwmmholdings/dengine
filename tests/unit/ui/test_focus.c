#include "nav_test_util.h"
#include "de_alloc_guard.h"
TEST_F(ui_focus, set_clear_events_and_despawn_hidden_cleanup, nav_fx) {
    de_ui *ui = f->base.ui;
    de_entity a = nav_node(f, f->root, 0, 0, 0, DE_UI_WIDGET_BUTTON);
    de_entity b = nav_node(f, f->root, 70, 0, 0, DE_UI_WIDGET_BUTTON);
    nav_frame(f);
    de_ui_set_focus(ui, a);
    de_ui_set_focus(ui, a);
    EXPECT_EQ(nav_events(f, DE_UI_EV_FOCUS), 1u);
    EXPECT_TRUE(((const DeUiInteractionState *)de_entity_get_const(f->base.world, a, ui->ids.state))
                    ->focused);
    de_ui_set_focus(ui, b);
    EXPECT_EQ(nav_events(f, DE_UI_EV_BLUR), 1u);
    de_entity_despawn(f->base.world, b);
    nav_frame(f);
    EXPECT_EQ(de_ui_focused(ui).bits, 0u);
    de_ui_set_focus(ui, a);
    ui_node_edit(&f->base, f->root)->display = DE_UI_NONE;
    nav_frame(f);
    EXPECT_EQ(de_ui_focused(ui).bits, 0u);
    de_ui_set_focus(ui, a);
    EXPECT_EQ(de_ui_focused(ui).bits, 0u);
    de_ui_set_focus(NULL, a);
    EXPECT_EQ(de_ui_focused(NULL).bits, 0u);
}
TEST_F(ui_focus, keyboard_tab_shift_submit_toggle_slider_and_cancel_root, nav_fx) {
    de_ui *ui = f->base.ui;
    de_entity a = nav_node(f, f->root, 0, 0, 0, DE_UI_WIDGET_BUTTON);
    de_entity b = nav_node(f, f->root, 0, 70, 0, DE_UI_WIDGET_TOGGLE);
    de_entity c = nav_node(f, f->root, 0, 140, 0, DE_UI_WIDGET_SLIDER);
    nav_frame(f);
    nav_key(f, DE_KEY_TAB);
    EXPECT_EQ(de_ui_focused(ui).bits, a.bits);
    nav_key(f, DE_KEY_RETURN);
    EXPECT_EQ(nav_events(f, DE_UI_EV_CLICK), 1u);
    nav_key(f, DE_KEY_TAB);
    EXPECT_EQ(de_ui_focused(ui).bits, b.bits);
    nav_key(f, DE_KEY_SPACE);
    EXPECT_TRUE(
        ((const DeUiToggle *)de_entity_get_const(f->base.world, b, ui->widgets->toggle))->on);
    nav_key(f, DE_KEY_DOWN);
    EXPECT_EQ(de_ui_focused(ui).bits, c.bits);
    nav_key(f, DE_KEY_RIGHT);
    EXPECT_FLOAT_EQ(
        ((const DeUiSlider *)de_entity_get_const(f->base.world, c, ui->widgets->slider))->value, .1,
        .00001);
    nav_key(f, DE_KEY_LEFT);
    EXPECT_FLOAT_EQ(
        ((const DeUiSlider *)de_entity_get_const(f->base.world, c, ui->widgets->slider))->value, 0,
        .00001);
    input_test_key(f->input, DE_KEY_LSHIFT, true);
    nav_frame(f);
    nav_key(f, DE_KEY_TAB);
    EXPECT_EQ(de_ui_focused(ui).bits, b.bits);
    input_test_key(f->input, DE_KEY_LSHIFT, false);
    nav_frame(f);
    (void)nav_events(f, DE_UI_EV_FOCUS);
    nav_key(f, DE_KEY_ESCAPE);
    const de_ui_event *event = de_event_next(f->base.world, &f->events);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->kind, DE_UI_EV_SUBMIT);
    EXPECT_EQ(event->target.bits, f->root.bits);
    EXPECT_EQ(event->action.hash, de_name_from("cancel").hash);
}
TEST_F(ui_focus, modal_nested_trap_cancel_restore_and_skip_external_neighbor, nav_fx) {
    de_ui *ui = f->base.ui;
    de_entity a = nav_node(f, f->root, 0, 0, 0, DE_UI_WIDGET_BUTTON);
    de_entity popup = ui_node(&f->base, f->root);
    DeUiPopup *p = de_entity_add(f->base.world, popup, ui->ids.popup);
    p->modal = true;
    ui_node_edit(&f->base, popup)->display = DE_UI_NONE;
    de_entity b = nav_node(f, popup, 10, 10, 0, DE_UI_WIDGET_BUTTON);
    nav_frame(f);
    de_ui_set_focus(ui, a);
    ui_node_edit(&f->base, popup)->display = DE_UI_FLEX;
    nav_frame(f);
    EXPECT_EQ(de_ui_focused(ui).bits, b.bits);
    EXPECT_EQ(de_action_map_flags(f->input, ui->focus->map), DE_INPUT_CONTEXT_BLOCKS_LOWER);
    ((DeUiFocusable *)de_entity_get(f->base.world, b, ui->focus->focusable))->right = a;
    nav_key(f, DE_KEY_RIGHT);
    EXPECT_EQ(de_ui_focused(ui).bits, b.bits);
    de_ui_set_focus(ui, a);
    EXPECT_EQ(de_ui_focused(ui).bits, b.bits);
    de_entity nested = ui_node(&f->base, popup);
    p = de_entity_add(f->base.world, nested, ui->ids.popup);
    p->modal = true;
    de_entity c = nav_node(f, nested, 0, 0, 0, DE_UI_WIDGET_BUTTON);
    nav_frame(f);
    EXPECT_EQ(de_ui_focused(ui).bits, c.bits);
    nav_key(f, DE_KEY_ESCAPE);
    EXPECT_EQ(de_ui_focused(ui).bits, b.bits);
    nav_key(f, DE_KEY_ESCAPE);
    EXPECT_EQ(de_ui_focused(ui).bits, a.bits);
    EXPECT_EQ(de_action_map_flags(f->input, ui->focus->map), DE_INPUT_CONTEXT_PASSTHROUGH);
}
TEST_F(ui_focus, zero_one_multiple_fixed_ticks_submit_once_and_repeat_timing, nav_fx) {
    de_ui *ui = f->base.ui;
    de_entity e[12];
    for (u32 i = 0; i < 12; ++i)
        e[i] = nav_node(f, f->root, 0, (f32)i * 28, 0, DE_UI_WIDGET_BUTTON);
    nav_frame(f);
    de_ui_set_focus(ui, e[0]);
    input_test_key(f->input, DE_KEY_RETURN, true);
    nav_pre(f);
    nav_post(f);
    EXPECT_EQ(nav_events(f, DE_UI_EV_CLICK), 0u);
    nav_fixed(f);
    nav_fixed(f);
    nav_fixed(f);
    nav_post(f);
    EXPECT_EQ(nav_events(f, DE_UI_EV_CLICK), 1u);
    input_test_key(f->input, DE_KEY_RETURN, false);
    nav_frame(f);
    input_test_key(f->input, DE_KEY_DOWN, true);
    nav_frame(f);
    EXPECT_EQ(de_ui_focused(ui).bits, e[1].bits);
    for (u32 i = 0; i < 23; ++i)
        nav_frame(f);
    EXPECT_EQ(de_ui_focused(ui).bits, e[1].bits);
    nav_frame(f);
    EXPECT_EQ(de_ui_focused(ui).bits, e[2].bits);
    for (u32 i = 0; i < 5; ++i)
        nav_frame(f);
    EXPECT_EQ(de_ui_focused(ui).bits, e[3].bits);
    input_test_key(f->input, DE_KEY_DOWN, false);
    nav_frame(f);
    EXPECT_EQ(de_ui_focused(ui).bits, e[3].bits);
    input_test_key(f->input, DE_KEY_UP, true);
    nav_pre(f);
    for (u32 i = 0; i < 30; ++i)
        nav_fixed(f);
    nav_post(f);
    EXPECT_EQ(de_ui_focused(ui).bits, e[0].bits);
}
TEST_F(ui_focus, mouse_hides_ring_navigation_restores_logical_focus, nav_fx) {
    de_ui *ui = f->base.ui;
    de_entity a = nav_node(f, f->root, 0, 0, 0, DE_UI_WIDGET_BUTTON);
    nav_frame(f);
    de_ui_set_focus(ui, a);
    EXPECT_TRUE(de_ui_focus_visible(ui));
    for (u32 i = 0; i < 12; ++i)
        nav_frame(f);
    const DeUiStyle *style = de_entity_get_const(f->base.world, a, ui->ids.style);
    ASSERT_TRUE(style);
    EXPECT_FLOAT_EQ(style->border_width, 2, .0001);
    input_test_mouse_move(f->input, 500, 350, 10, 1);
    nav_frame(f);
    EXPECT_FALSE(de_ui_focus_visible(ui));
    EXPECT_EQ(de_ui_focused(ui).bits, a.bits);
    EXPECT_TRUE(((const DeUiInteractionState *)de_entity_get_const(f->base.world, a, ui->ids.state))
                    ->focused);
    for (u32 i = 0; i < 12; ++i)
        nav_frame(f);
    style = de_entity_get_const(f->base.world, a, ui->ids.style);
    EXPECT_FLOAT_EQ(style->border_width, 1, .0001);
    nav_key(f, DE_KEY_UP);
    EXPECT_TRUE(de_ui_focus_visible(ui));
    for (u32 i = 0; i < 12; ++i)
        nav_frame(f);
    style = de_entity_get_const(f->base.world, a, ui->ids.style);
    EXPECT_FLOAT_EQ(style->border_width, 2, .0001);
}
static u32 scroll_calls;
static de_entity scrolled_container, scrolled_child;
static void into_view(de_ui *ui, de_entity container, de_entity child) {
    (void)ui;
    ++scroll_calls;
    scrolled_container = container;
    scrolled_child = child;
}
TEST_F(ui_focus, nullable_scroll_hook_and_context_modes_preserve_game_map, nav_fx) {
    de_ui *ui = f->base.ui;
    de_entity container = nav_node(f, f->root, 0, 0, -1, DE_UI_WIDGET_SCROLL);
    de_entity a = nav_node(f, container, 0, 500, 0, DE_UI_WIDGET_BUTTON);
    nav_frame(f);
    de_ui_set_focus(ui, a); /* No319 scroll callback yet: safely no-op. */
    de_ui_set_focus(ui, DE_ENTITY_NULL);
    scroll_calls = 0;
    ui->widgets->table.scroll_into_view = into_view;
    de_ui_set_focus(ui, a);
    EXPECT_EQ(scroll_calls, 1u);
    EXPECT_EQ(scrolled_container.bits, container.bits);
    EXPECT_EQ(scrolled_child.bits, a.bits);
    de_ui_set_input_mode(ui, DE_UI_INPUT_MODAL);
    nav_frame(f);
    EXPECT_EQ(de_action_map_flags(f->input, ui->focus->map), DE_INPUT_CONTEXT_BLOCKS_LOWER);
    de_ui_set_input_mode(ui, DE_UI_INPUT_NONE);
    nav_frame(f);
    EXPECT_FALSE(de_action_map_active(f->input, ui->focus->map));
    de_ui_set_input_mode(ui, DE_UI_INPUT_NON_MODAL);
    nav_frame(f);
    EXPECT_TRUE(de_action_map_active(f->input, ui->focus->map));
    EXPECT_EQ(de_action_map_flags(f->input, ui->focus->map), DE_INPUT_CONTEXT_PASSTHROUGH);
}
TEST_F(ui_focus, warmed_focus_traversal_has_no_frame_heap_traffic, nav_fx) {
    de_ui *ui = f->base.ui;
    for (u32 i = 0; i < 20; ++i)
        (void)nav_node(f, f->root, 0, (f32)i * 28, 0, DE_UI_WIDGET_BUTTON);
    for (u32 i = 0; i < 50; ++i) {
        nav_key(f, DE_KEY_TAB);
        de_world_update_events(f->base.world);
    }
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 i = 0; i < 100; ++i) {
        nav_key(f, DE_KEY_TAB);
        de_world_update_events(f->base.world);
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    EXPECT_TRUE(de_ui_focused(ui).bits);
}
TEST_F(ui_focus, modal_consumes_bound_game_actions_nonmodal_passes_through, nav_fx) {
    de_ui *ui = f->base.ui;
    de_action_map game = de_action_map_create(f->input, "game");
    de_binding binding = {
        .source = DE_BIND_KEY, .code = DE_KEY_RETURN, .composite_slot = DE_COMPOSITE_NONE};
    de_action fire =
        de_action_add(f->input, game,
                      &(de_action_desc){.name = "fire", .bindings = &binding, .binding_count = 1});
    nav_frame(f);
    de_action_map_pop(f->input, ui->focus->map);
    de_action_map_push(f->input, game);
    de_action_map_push(f->input, ui->focus->map);
    nav_frame(f);
    input_test_key(f->input, DE_KEY_RETURN, true);
    nav_pre(f);
    EXPECT_TRUE(de_action_triggered(f->input, fire));
    nav_fixed(f);
    nav_post(f);
    input_test_key(f->input, DE_KEY_RETURN, false);
    nav_frame(f);
    de_ui_set_input_mode(ui, DE_UI_INPUT_MODAL);
    nav_frame(f);
    input_test_key(f->input, DE_KEY_RETURN, true);
    nav_pre(f);
    EXPECT_FALSE(de_action_triggered(f->input, fire));
    nav_fixed(f);
    nav_post(f);
    input_test_key(f->input, DE_KEY_RETURN, false);
    nav_frame(f);
    de_ui_set_input_mode(ui, DE_UI_INPUT_NONE);
    nav_frame(f);
    input_test_key(f->input, DE_KEY_RETURN, true);
    nav_pre(f);
    EXPECT_TRUE(de_action_triggered(f->input, fire));
    nav_fixed(f);
    nav_post(f);
}
TEST_F(ui_focus, existing_ui_map_keeps_custom_bindings_and_context_ownership, nav_fx) {
    de_ui_destroy(f->base.ui);
    f->base.ui = NULL;
    de_action_map map = de_action_map_find(f->input, "ui");
    de_binding binding = {
        .source = DE_BIND_KEY, .code = DE_KEY_Q, .composite_slot = DE_COMPOSITE_NONE};
    de_action custom = de_action_add(
        f->input, map,
        &(de_action_desc){.name = "submit", .bindings = &binding, .binding_count = 1});
    ASSERT_TRUE(custom.id);
    ASSERT_EQ(
        de_ui_create(&(de_ui_desc){.input = f->input, .reference_height = 400, .max_nodes = 512},
                     &f->base.ui),
        DE_OK);
    de_ui_set_viewport(f->base.ui, 600, 400, (de_window_insets){0});
    de_ui_attach(f->base.ui, f->base.world);
    de_entity e = nav_node(f, f->root, 0, 0, 0, DE_UI_WIDGET_BUTTON);
    f->events = de_event_reader_create(f->base.world, f->base.ui->event);
    nav_frame(f);
    de_ui_set_focus(f->base.ui, e);
    nav_key(f, DE_KEY_RETURN);
    EXPECT_EQ(nav_events(f, DE_UI_EV_CLICK), 0u);
    nav_key(f, DE_KEY_Q);
    EXPECT_EQ(nav_events(f, DE_UI_EV_CLICK), 1u);
    EXPECT_EQ(de_action_find(f->input, map, "submit").id, custom.id);
}
TEST_F(ui_focus, nonmodal_popup_cancel_does_not_trap_and_dead_restore_is_cleared, nav_fx) {
    de_ui *ui = f->base.ui;
    de_entity a = nav_node(f, f->root, 0, 0, 0, DE_UI_WIDGET_BUTTON);
    de_entity popup = ui_node(&f->base, f->root);
    DeUiPopup *p = de_entity_add(f->base.world, popup, ui->ids.popup);
    p->modal = false;
    (void)nav_node(f, popup, 0, 0, 0, DE_UI_WIDGET_BUTTON);
    nav_frame(f);
    de_ui_set_focus(ui, a);
    EXPECT_EQ(de_ui_focused(ui).bits, a.bits);
    nav_key(f, DE_KEY_ESCAPE);
    EXPECT_EQ(ui_node_edit(&f->base, popup)->display, DE_UI_NONE);
    p = de_entity_get(f->base.world, popup, ui->ids.popup);
    p->modal = true;
    ui_node_edit(&f->base, popup)->display = DE_UI_FLEX;
    nav_frame(f);
    de_entity_despawn(f->base.world, a);
    nav_key(f, DE_KEY_ESCAPE);
    EXPECT_EQ(de_ui_focused(ui).bits, 0u);
}
