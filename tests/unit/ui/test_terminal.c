#include "nav_test_util.h"
#include "ui/widgets/advanced_internal.h"
#include "../../../examples/terminal_demo/session.h"
#include <stdio.h>
typedef struct terminal_fx {
    nav_fx nav;
    terminal_session session;
    terminal_journal journal;
} terminal_fx;
static void terminal_fx_setup(terminal_fx *f) {
    nav_fx_setup(&f->nav);
    ui_fx *b = &f->nav.base;
    de_component panel = de_component_find(b->world, "DeUiWorldPanel");
    *(DeUiWorldPanel *)de_entity_add(b->world, f->nav.root, panel) =
        (DeUiWorldPanel){.size_m = {3.2f, 2}, .pixels_per_meter = 200, .depth_test = true};
    de_entity editor = ui_node(b, f->nav.root);
    DeUiNode *n = ui_node_edit(b, editor);
    n->width = (de_ui_len){DE_UI_PX, 616};
    n->height = (de_ui_len){DE_UI_PX, 298};
    (void)de_entity_add(b->world, editor, b->ui->advanced->input);
    (void)de_entity_add(b->world, editor, b->ui->widgets->focusable);
    nav_frame(&f->nav);
    f->session = (terminal_session){.ui = b->ui, .editor = editor};
    ASSERT_TRUE(terminal_apply(&f->session, &(terminal_entry){.operation = TERM_LEAVE}));
}
static void terminal_fx_teardown(terminal_fx *f) {
    terminal_journal_clear(&f->journal);
    nav_fx_teardown(&f->nav);
}
static void terminal_observer(de_entity entity, const de_ui_text_edit *edit, void *user) {
    terminal_fx *f = user;
    ASSERT_EQ(entity.bits, f->session.editor.bits);
    ASSERT_TRUE(terminal_append(&f->journal, (terminal_entry){TERM_EDIT, *edit}));
}
static void terminal_operation(terminal_fx *f, u32 op, u32 keys) {
    terminal_entry e = {.operation = op, .edit.caret = keys};
    ASSERT_TRUE(terminal_apply(&f->session, &e));
    ASSERT_TRUE(terminal_append(&f->journal, e));
}
TEST(ui_terminal, walk_edit_pause_save_and_fresh_world_replay) {
    terminal_fx live = {0};
    terminal_fx_setup(&live);
    de_ui_set_text_edit_observer(live.nav.base.ui, terminal_observer, &live);
    terminal_operation(&live, TERM_TICK, 1);
    EXPECT_EQ(live.session.walk_ticks, 1u);
    EXPECT_EQ(live.session.aircraft_ticks, 1u);
    EXPECT_TRUE(live.session.captured);
    terminal_operation(&live, TERM_ENTER, 0);
    nav_frame(&live.nav);
    EXPECT_FALSE(live.session.captured);
    EXPECT_EQ(de_ui_focused(live.nav.base.ui).bits, live.session.editor.bits);
    terminal_operation(&live, TERM_PAUSE, 0);
    for (u32 i = 0; i < 16; ++i) {
        input_test_text(live.nav.input, "aircraft.climb(1500);\n");
        /* The host can omit FixedUpdate entirely while paused. UI still processes frames. */
        nav_pre(&live.nav);
        nav_post(&live.nav);
        terminal_operation(&live, TERM_TICK, 1);
    }
    de_str text = de_ui_text_input_get(live.nav.base.ui, live.session.editor);
    EXPECT_EQ(text.len, 352u);
    EXPECT_EQ(live.session.walk_ticks, 1u);     /* W cannot walk while editing. */
    EXPECT_EQ(live.session.aircraft_ticks, 1u); /* Simulation paused, editor responsive. */
    input_test_key(live.nav.input, DE_KEY_LCTRL, true);
    input_test_key(live.nav.input, DE_KEY_Z, true);
    nav_frame(&live.nav);
    EXPECT_EQ(de_ui_text_input_get(live.nav.base.ui, live.session.editor).len, 330u);
    input_test_key(live.nav.input, DE_KEY_Z, false);
    nav_frame(&live.nav);
    input_test_key(live.nav.input, DE_KEY_LCTRL, false);
    input_test_key(live.nav.input, DE_KEY_ESCAPE, true);
    nav_frame(&live.nav);
    EXPECT_EQ(de_ui_focused(live.nav.base.ui).bits, 0u);
    terminal_operation(&live, TERM_LEAVE, 0);
    nav_frame(&live.nav);
    EXPECT_TRUE(live.session.captured);
    terminal_operation(&live, TERM_TICK, 8);
    EXPECT_EQ(live.session.walk_ticks, 2u);
    EXPECT_EQ(live.session.aircraft_ticks, 1u);
    terminal_operation(&live, TERM_PAUSE, 0);
    terminal_operation(&live, TERM_TICK, 0);
    EXPECT_EQ(live.session.aircraft_ticks, 2u);
    char expected[512];
    text = de_ui_text_input_get(live.nav.base.ui, live.session.editor);
    ASSERT_TRUE(text.len < sizeof expected);
    memcpy(expected, text.ptr, text.len + 1);
    terminal_session state = live.session;
    char path[512];
    snprintf(path, sizeof path, "%s/terminal.tuij", DE_UI_REPLAY_DIR);
    ASSERT_TRUE(terminal_save(&live.journal, path));
    terminal_fx_teardown(&live);
    terminal_fx replay = {0};
    terminal_fx_setup(&replay);
    ASSERT_TRUE(terminal_load(&replay.journal, path));
    /* No raw-device injection, no copied document snapshots, fresh ECS/UI/input. */
    for (u32 i = 0; i < replay.journal.count; ++i)
        ASSERT_TRUE(terminal_apply(&replay.session, &replay.journal.entries[i]));
    EXPECT_TRUE(
        strcmp(de_ui_text_input_get(replay.nav.base.ui, replay.session.editor).ptr, expected) == 0);
    EXPECT_EQ(replay.session.walk_ticks, state.walk_ticks);
    EXPECT_EQ(replay.session.aircraft_ticks, state.aircraft_ticks);
    EXPECT_EQ(replay.session.editing, state.editing);
    EXPECT_EQ(replay.session.paused, state.paused);
    EXPECT_EQ(replay.session.captured, state.captured);
    EXPECT_EQ(replay.session.popup_open, state.popup_open);
    EXPECT_FLOAT_EQ(replay.session.x, state.x, 0);
    EXPECT_FLOAT_EQ(replay.session.z, state.z, 0);
    EXPECT_EQ(de_ui_focused(replay.nav.base.ui).bits, 0u);
    terminal_fx_teardown(&replay);
    remove(path);
}
TEST_F(ui_terminal, crt_autocomplete_and_menu_stay_in_panel_on_resize, terminal_fx) {
    ui_fx *b = &f->nav.base;
    de_ui_set_viewport(b->ui, 1200, 800, (de_window_insets){20, 20, 20, 20});
    de_entity anchor = ui_node(b, f->nav.root);
    *(DeUiAnchor *)de_entity_add(b->world, anchor, b->ui->ids.anchor) =
        (DeUiAnchor){.offset_min = {610, 370}, .offset_max = {620, 380}};
    de_entity popup = ui_node(b, f->nav.root);
    DeUiNode *n = ui_node_edit(b, popup);
    n->width = (de_ui_len){DE_UI_PX, 220};
    n->height = (de_ui_len){DE_UI_PX, 72};
    *(DeUiPopup *)de_entity_add(b->world, popup, b->ui->ids.popup) =
        (DeUiPopup){.anchor = anchor, .placement = DE_UI_POPUP_BOTTOM};
    nav_frame(&f->nav);
    expect_rect(ui_rect(b, popup), 420, 328, 220, 72);
    DeUiWorldPanel *panel =
        de_entity_get(b->world, f->nav.root, de_component_find(b->world, "DeUiWorldPanel"));
    panel->size_m = (de_vec2){1, .25f};
    nav_frame(&f->nav);
    expect_rect(ui_rect(b, popup), 0, 0, 200, 50);
}
