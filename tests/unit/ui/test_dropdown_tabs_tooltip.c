#include "ui_test_util.h"
#include "ui/widgets/advanced_internal.h"
TEST_F(ui_advanced, dropdown_flips_selects_once_and_cancel_preserves_value, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0}), drop = ui_node(f, root);
    DeUiAnchor *anchor = de_entity_add(f->world, drop, f->ui->ids.anchor);
    anchor->offset_min = (de_vec2){10, 70};
    anchor->offset_max = (de_vec2){150, 95};
    DeUiDropdown *d = de_entity_add(f->world, drop, f->ui->advanced->dropdown);
    d->option_count = 2;
    d->options[0] = de_name_from("one");
    d->options[1] = de_name_from("two");
    ui_frame(f);
    de_ui__dropdown_submit(f->ui, drop);
    ui_frame(f);
    const de_ui_popup_state *s = de_entity_get_const(f->world, drop, f->ui->advanced->popup);
    ASSERT_TRUE(s->open);
    const DeUiPopup *popup = de_entity_get_const(f->world, s->popup, f->ui->ids.popup);
    EXPECT_EQ(popup->placement, DE_UI_POPUP_TOP);
    EXPECT_TRUE(ui_rect(f, s->popup).y < ui_rect(f, drop).y);
    de_entity second =
        de_entity_children_next(f->world, de_entity_children_first(f->world, s->popup));
    de_event_reader reader = de_event_reader_create(f->world, f->ui->event);
    de_ui__button_submit(f->ui, second);
    ui_frame(f);
    d = de_entity_get(f->world, drop, f->ui->advanced->dropdown);
    EXPECT_EQ(d->selected, 1u);
    s = de_entity_get_const(f->world, drop, f->ui->advanced->popup);
    EXPECT_FALSE(s->open);
    const de_ui_event *event;
    u32 changed = 0;
    while ((event = de_event_next(f->world, &reader)))
        if (event->kind == DE_UI_EV_CHANGED && event->target.bits == drop.bits)
            ++changed;
    EXPECT_EQ(changed, 1u);
    de_ui__dropdown_submit(f->ui, drop);
    de_ui__dropdown_cancel(f->ui, drop);
    d = de_entity_get(f->world, drop, f->ui->advanced->dropdown);
    EXPECT_EQ(d->selected, 1u);
}
TEST_F(ui_advanced, tabs_generate_headers_hide_inactive_pages_and_emit_change, ui_fx) {
    de_entity root = ui_node(f, (de_entity){0}), tabs = ui_node(f, root), one = ui_node(f, tabs),
              two = ui_node(f, tabs);
    de_entity_add(f->world, tabs, f->ui->advanced->tabs);
    ui_node_edit(f, tabs)->direction = DE_UI_COLUMN;
    ui_node_edit(f, tabs)->width = (de_ui_len){DE_UI_PX, 200};
    ui_node_edit(f, tabs)->height = (de_ui_len){DE_UI_PX, 90};
    ui_frame(f);
    const de_ui_tab_state *state = de_entity_get_const(f->world, tabs, f->ui->advanced->tab_state);
    ASSERT_TRUE(state);
    EXPECT_EQ(state->count, 2u);
    EXPECT_EQ(de_entity_children_count(f->world, state->headers), 2u);
    EXPECT_TRUE(ui_rect(f, one).w > 0);
    EXPECT_FLOAT_EQ(ui_rect(f, two).w, 0, 0);
    de_event_reader reader = de_event_reader_create(f->world, f->ui->event);
    de_ui__tabs_increment(f->ui, tabs);
    ui_frame(f);
    EXPECT_FLOAT_EQ(ui_rect(f, one).w, 0, 0);
    EXPECT_TRUE(ui_rect(f, two).w > 0);
    const de_ui_event *event;
    u32 changed = 0;
    while ((event = de_event_next(f->world, &reader)))
        if (event->kind == DE_UI_EV_CHANGED && event->target.bits == tabs.bits)
            ++changed;
    EXPECT_EQ(changed, 1u);
}
TEST_F(ui_advanced, tooltip_delay_focus_safe_bounds_and_press_hide, ui_fx) {
    de_ui_set_viewport(f->ui, 400, 100,
                       (de_window_insets){.left = 10, .right = 10, .top = 5, .bottom = 5});
    de_entity root = ui_node(f, (de_entity){0}), e = ui_node(f, root);
    DeUiAnchor *anchor = de_entity_add(f->world, e, f->ui->ids.anchor);
    anchor->offset_min = (de_vec2){350, 70};
    anchor->offset_max = (de_vec2){390, 90};
    DeUiTooltip *t = de_entity_add(f->world, e, f->ui->advanced->tooltip);
    t->delay = .2f;
    t->loc_id = de_name_from("tip");
    DeUiInteractionState *interaction = de_entity_add(f->world, e, f->ui->ids.state);
    interaction->focused = true;
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, .1f, 0), DE_OK);
    const de_ui_tooltip_state *s = de_entity_get_const(f->world, e, f->ui->advanced->tooltip_state);
    ASSERT_TRUE(s);
    EXPECT_EQ(s->popup.bits, 0u);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, .11f, 0), DE_OK);
    s = de_entity_get_const(f->world, e, f->ui->advanced->tooltip_state);
    ASSERT_TRUE(s->popup.bits);
    de_vec4 rect = ui_rect(f, s->popup);
    EXPECT_TRUE(rect.x >= 10 && rect.x + rect.z <= 390);
    EXPECT_TRUE(rect.y >= 5 && rect.y + rect.w <= 95);
    interaction = de_entity_get(f->world, e, f->ui->ids.state);
    interaction->pressed = true;
    ui_frame(f);
    s = de_entity_get_const(f->world, e, f->ui->advanced->tooltip_state);
    EXPECT_EQ(s->popup.bits, 0u);
}
#include "../input/input_test_util.h"
TEST_F(ui_advanced, actual_escape_and_outside_click_close_dropdown_without_selection, ui_fx) {
    de_input *input = input_test_create(NULL);
    ASSERT_TRUE(input);
    de_ui_attach(f->ui, NULL);
    f->ui->desc.input = input;
    de_ui_attach(f->ui, f->world);
    de_entity root = ui_node(f, (de_entity){0}), e = ui_node(f, root);
    DeUiNode *n = ui_node_edit(f, e);
    n->width = (de_ui_len){DE_UI_PX, 120};
    n->height = (de_ui_len){DE_UI_PX, 32};
    DeUiDropdown *d = de_entity_add(f->world, e, f->ui->advanced->dropdown);
    d->option_count = 2;
    d->selected = 1;
    ui_frame(f);
    de_ui__dropdown_submit(f->ui, e);
    ui_frame(f);
    input_test_frame(input);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_FIXED_UPDATE, INPUT_TEST_DT, 0), DE_OK);
    ui_frame(f);
    input_test_key(input, DE_KEY_ESCAPE, true);
    input_test_frame(input);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_PRE_UPDATE, INPUT_TEST_DT, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_FIXED_UPDATE, INPUT_TEST_DT, 0), DE_OK);
    ui_frame(f);
    ui_frame(f);
    const de_ui_popup_state *s = de_entity_get_const(f->world, e, f->ui->advanced->popup);
    EXPECT_FALSE(s->open);
    input_test_key(input, DE_KEY_ESCAPE, false);
    input_test_frame(input);
    de_ui__dropdown_submit(f->ui, e);
    ui_frame(f);
    de__input_event ev = input_test_event(DE_RAW_MOUSE_BUTTON);
    ev.mouse_button.button = DE_MOUSE_LEFT;
    ev.mouse_button.down = true;
    ev.mouse_button.x = 350;
    ev.mouse_button.y = 90;
    de__input_inject(input, &ev);
    input_test_frame(input);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_PRE_UPDATE, INPUT_TEST_DT, 0), DE_OK);
    ui_frame(f);
    s = de_entity_get_const(f->world, e, f->ui->advanced->popup);
    EXPECT_FALSE(s->open);
    d = de_entity_get(f->world, e, f->ui->advanced->dropdown);
    EXPECT_EQ(d->selected, 1u);
    de_ui_attach(f->ui, NULL);
    f->ui->desc.input = NULL;
    de_input_destroy(input);
}

#if DE_HAS_RENDER
static de_entity generated_popup_panel(ui_fx *f) {
    de_entity root = ui_node(f, DE_ENTITY_NULL);
    de_component panel = de_component_find(f->world, "DeUiWorldPanel");
    ASSERT_TRUE(panel.id);
    *(DeUiWorldPanel *)de_entity_add(f->world, root, panel) =
        (DeUiWorldPanel){.size_m = {2, 1}, .pixels_per_meter = 100, .depth_test = true};
    return root;
}
static bool generated_popup_owned_by(ui_fx *f, de_entity popup, de_entity root) {
    for (de_entity parent = de_entity_parent(f->world, popup); parent.bits;
         parent = de_entity_parent(f->world, parent))
        if (parent.bits == root.bits)
            return true;
    return false;
}
TEST_F(ui_advanced, generated_dropdown_preserves_world_panel_units_and_ownership, ui_fx) {
    de_ui_set_viewport(f->ui, 400, 200,
                       (de_window_insets){.left = 10, .top = 20, .right = 30, .bottom = 40});
    de_entity root = generated_popup_panel(f), owner = ui_node(f, root);
    *(DeUiAnchor *)de_entity_add(f->world, owner, f->ui->ids.anchor) =
        (DeUiAnchor){.offset_min = {140, 60}, .offset_max = {190, 80}};
    DeUiDropdown *drop = de_entity_add(f->world, owner, f->ui->advanced->dropdown);
    drop->option_count = 2;
    drop->options[0] = de_name_from("one");
    drop->options[1] = de_name_from("two");
    ui_frame(f);
    de_ui__dropdown_submit(f->ui, owner);
    ui_frame(f);
    const de_ui_popup_state *state = de_entity_get_const(f->world, owner, f->ui->advanced->popup);
    ASSERT_TRUE(state && state->open);
    de_entity popup = state->popup;
    EXPECT_TRUE(generated_popup_owned_by(f, popup, root));
    expect_rect(ui_rect(f, popup), 140, 0, 50, 64);
    de_entity row = de_entity_children_first(f->world, popup);
    EXPECT_FLOAT_EQ(ui_rect(f, row).w, 32, .5f);
    EXPECT_FLOAT_EQ(f->ui->scale, 2, 0);
    DeUiWorldPanel *panel =
        de_entity_get(f->world, root, de_component_find(f->world, "DeUiWorldPanel"));
    panel->size_m = (de_vec2){1.6f, .5f};
    ui_frame(f);
    expect_rect(ui_rect(f, popup), 110, 0, 50, 50);
    EXPECT_FLOAT_EQ(f->ui->scale, 2, 0);
    de_ui__dropdown_cancel(f->ui, owner);
    EXPECT_FALSE(de_entity_alive(f->world, popup));
    EXPECT_FALSE(de_entity_alive(f->world, row));
}
TEST_F(ui_advanced, generated_tooltip_stays_on_its_world_panel, ui_fx) {
    de_ui_set_viewport(f->ui, 400, 200,
                       (de_window_insets){.left = 10, .top = 20, .right = 30, .bottom = 40});
    de_entity root = generated_popup_panel(f), owner = ui_node(f, root);
    *(DeUiAnchor *)de_entity_add(f->world, owner, f->ui->ids.anchor) =
        (DeUiAnchor){.offset_min = {150, 65}, .offset_max = {190, 85}};
    de_entity_add(f->world, owner, f->ui->advanced->tooltip);
    ((DeUiInteractionState *)de_entity_add(f->world, owner, f->ui->ids.state))->focused = true;
    ui_frame(f);
    const de_ui_tooltip_state *state =
        de_entity_get_const(f->world, owner, f->ui->advanced->tooltip_state);
    ASSERT_TRUE(state && state->popup.bits);
    EXPECT_TRUE(generated_popup_owned_by(f, state->popup, root));
    expect_rect(ui_rect(f, state->popup), 0, 60, 200, 40);
    EXPECT_FLOAT_EQ(f->ui->scale, 2, 0);
}
TEST_F(ui_advanced, generated_screen_dropdown_retains_scaled_safe_area, ui_fx) {
    de_ui_set_viewport(f->ui, 400, 200,
                       (de_window_insets){.left = 10, .top = 20, .right = 30, .bottom = 40});
    de_entity root = ui_node(f, DE_ENTITY_NULL), owner = ui_node(f, root);
    *(DeUiAnchor *)de_entity_add(f->world, owner, f->ui->ids.anchor) =
        (DeUiAnchor){.offset_min = {40, 20}, .offset_max = {100, 35}};
    DeUiDropdown *drop = de_entity_add(f->world, owner, f->ui->advanced->dropdown);
    drop->option_count = 2;
    ui_frame(f);
    de_ui__dropdown_submit(f->ui, owner);
    ui_frame(f);
    const de_ui_popup_state *state = de_entity_get_const(f->world, owner, f->ui->advanced->popup);
    ASSERT_TRUE(state && state->open);
    expect_rect(ui_rect(f, state->popup), 80, 20, 120, 128);
    de_entity row = de_entity_children_first(f->world, state->popup);
    EXPECT_FLOAT_EQ(ui_rect(f, row).w, 64, .5f);
}
TEST_F(ui_advanced, generated_tooltip_pointer_uses_panel_units_and_rejects_other_roots, ui_fx) {
    de_ui_set_viewport(f->ui, 400, 200, (de_window_insets){0});
    de_entity root = generated_popup_panel(f), container = ui_node(f, root);
    ui_node_edit(f, container)->clip = true;
    de_entity owner = ui_node(f, container);
    *(DeUiAnchor *)de_entity_add(f->world, owner, f->ui->ids.anchor) =
        (DeUiAnchor){.offset_min = {150, 65}, .offset_max = {190, 85}};
    de_entity_add(f->world, owner, f->ui->advanced->tooltip);
    ui_frame(f);
    f->ui->widgets->pointer.root = root;
    f->ui->widgets->pointer.position = (de_vec2){170, 75};
    ((DeUiInteractionState *)de_entity_add(f->world, owner, f->ui->ids.state))->hovered = true;
    ui_frame(f);
    const de_ui_tooltip_state *state =
        de_entity_get_const(f->world, owner, f->ui->advanced->tooltip_state);
    ASSERT_TRUE(state && state->popup.bits && state->anchor.bits);
    de_entity popup = state->popup, anchor = state->anchor;
    EXPECT_EQ(de_entity_parent(f->world, popup).bits, root.bits);
    expect_rect(ui_rect(f, anchor), 182, 87, 1, 1);
    expect_rect(ui_rect(f, popup), 0, 60, 200, 40);
    ((DeUiInteractionState *)de_entity_get(f->world, owner, f->ui->ids.state))->hovered = false;
    ui_frame(f);
    EXPECT_FALSE(de_entity_alive(f->world, popup));
    EXPECT_FALSE(de_entity_alive(f->world, anchor));
    f->ui->widgets->pointer.root = generated_popup_panel(f);
    ((DeUiInteractionState *)de_entity_get(f->world, owner, f->ui->ids.state))->hovered = true;
    ui_frame(f);
    state = de_entity_get_const(f->world, owner, f->ui->advanced->tooltip_state);
    ASSERT_TRUE(state && state->popup.bits);
    EXPECT_EQ(state->anchor.bits, 0u);
    const DeUiPopup *place = de_entity_get_const(f->world, state->popup, f->ui->ids.popup);
    EXPECT_EQ(place->anchor.bits, owner.bits);
}
#endif

#if defined(DE_UI_TEST_FONTS) && DE_HAS_RENDER
#include "../text/runtime_test_util.h"
TEST_F(ui_advanced, tooltip_measures_wrapped_text_and_clips_to_screen_and_panel_bounds, ui_fx) {
    runtime_fx fonts = {0};
    runtime_fx_setup(&fonts);
    f->ui->desc.text = fonts.t;
    static const char content[] = "A tooltip stays within the safe area.";
    for (u32 variant = 0; variant < 4; ++variant) {
        bool world = variant >= 2;
        de_ui_set_viewport(
            f->ui, variant == 1 ? 140 : 400, variant == 1 ? 100 : 200,
            variant == 1 ? (de_window_insets){5, 5, 5, 5}
                         : (de_window_insets){.left = 10, .top = 20, .right = 30, .bottom = 40});
        de_entity root = world ? generated_popup_panel(f) : ui_node(f, DE_ENTITY_NULL);
        if (variant == 3) {
            DeUiWorldPanel *panel =
                de_entity_get(f->world, root, de_component_find(f->world, "DeUiWorldPanel"));
            panel->size_m = (de_vec2){.8f, .6f};
        }
        de_entity owner = ui_node(f, root);
        *(DeUiAnchor *)de_entity_add(f->world, owner, f->ui->ids.anchor) =
            (DeUiAnchor){.offset_min = {10, 10}, .offset_max = {50, 30}};
        DeUiText *label = de_entity_add(f->world, owner, f->ui->ids.text);
        label->style = (de_text_style){
            .font = fonts.latin, .size_px = 20, .line_height = 1, .color = {1, 1, 1, 1}};
        de_entity_add(f->world, owner, f->ui->advanced->tooltip);
        ((DeUiInteractionState *)de_entity_add(f->world, owner, f->ui->ids.state))->focused = true;
        ui_frame(f);
        const de_ui_tooltip_state *state =
            de_entity_get_const(f->world, owner, f->ui->advanced->tooltip_state);
        ASSERT_TRUE(state && state->popup.bits);
        de_entity popup = state->popup;
        label = de_entity_get(f->world, popup, f->ui->ids.text);
        label->literal = (de_str){content, sizeof(content) - 1};
        ui_frame(f);
        de_vec4 rect = ui_rect(f, popup);
        label = de_entity_get(f->world, popup, f->ui->ids.text);
        f32 scale = world ? 1 : f->ui->scale;
        f32 available_height = world ? (variant == 3 ? 60 : 100) : (variant == 1 ? 90 : 140);
        f32 available_width = world ? (variant == 3 ? 80 : 200) : (variant == 1 ? 130 : 360);
        de_text_style style = label->style;
        style.size_px *= scale;
        de_text_layout *layout = NULL;
        ASSERT_EQ(
            de_text_layout_build(fonts.t,
                                 &(de_text_layout_desc){.utf8 = {content, sizeof(content) - 1},
                                                        .style = &style,
                                                        .max_width = rect.z - 2 * scale},
                                 NULL, &layout),
            DE_OK);
        de_vec2 measured = de_text_layout_size(layout);
        de_text_layout_free(layout);
        f32 expected_height = fminf(available_height, fmaxf(40 * scale, measured.y + 2 * scale));
        printf("tooltip variant%u rect %.1fx%.1f measured %.1f expected %.1f\n", variant, rect.z,
               rect.w, measured.y, expected_height);
        EXPECT_FLOAT_EQ(rect.w, expected_height, .5f);
        EXPECT_TRUE(rect.z <= available_width);
        const DeUiNode *node = de_entity_get_const(f->world, popup, f->ui->ids.node);
        EXPECT_TRUE(node->clip);
        f32 left = world ? 0 : (variant == 1 ? 5 : 10);
        f32 top = world ? 0 : (variant == 1 ? 5 : 20);
        EXPECT_TRUE(rect.x >= left && rect.x + rect.z <= left + available_width + .5f);
        EXPECT_TRUE(rect.y >= top && rect.y + rect.w <= top + available_height + .5f);
        if (variant == 2) {
            DeUiWorldPanel *panel =
                de_entity_get(f->world, root, de_component_find(f->world, "DeUiWorldPanel"));
            panel->size_m = (de_vec2){.8f, .6f};
            ui_frame(f);
            EXPECT_FLOAT_EQ(ui_rect(f, popup).z, 80, .5f);
            EXPECT_FLOAT_EQ(ui_rect(f, popup).w, 60, .5f);
            panel = de_entity_get(f->world, root, de_component_find(f->world, "DeUiWorldPanel"));
            panel->size_m = (de_vec2){2, 1};
            ui_frame(f);
            EXPECT_FLOAT_EQ(ui_rect(f, popup).w, expected_height, .5f);
            label = de_entity_get(f->world, popup, f->ui->ids.text);
            label->literal = (de_str){"Tip", 3};
            ui_frame(f);
            EXPECT_FLOAT_EQ(ui_rect(f, popup).w, 40, .5f);
        }
        EXPECT_FLOAT_EQ(f->ui->scale, variant == 1 ? 1 : 2, 0);
        ((DeUiInteractionState *)de_entity_get(f->world, owner, f->ui->ids.state))->focused = false;
        ui_frame(f);
        EXPECT_FALSE(de_entity_alive(f->world, popup));
        de_entity_despawn_recursive(f->world, root);
    }
    f->ui->desc.text = NULL;
    runtime_fx_teardown(&fonts);
}
#endif
