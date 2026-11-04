#include "nav_test_util.h"
/* A real gamepad-only menu: Play -> Music -> Volume -> Quit -> cancel confirmation ->
 * Quit -> Confirm. Recording passes through raw S21 pad input and normal action resolution;
 * replay loads the on-disk DEIR into a fresh world with no device or live input. */
typedef struct menu {
    nav_fx nav;
    de_entity play, music, volume, quit, popup, confirm;
    u32 clicks, changes, exits, visited;
    u64 events;
} menu;
typedef struct menu_trace {
    u64 focus, events, input;
    u32 clicks, changes, exits, visited;
    f32 volume;
    bool music;
} menu_trace;
static void menu_setup(menu *m) {
    nav_fx_setup(&m->nav);
    nav_fx *f = &m->nav;
    m->play = nav_node(f, f->root, 20, 10, 0, DE_UI_WIDGET_BUTTON);
    m->music = nav_node(f, f->root, 20, 70, 0, DE_UI_WIDGET_TOGGLE);
    m->volume = nav_node(f, f->root, 20, 130, 0, DE_UI_WIDGET_SLIDER);
    m->quit = nav_node(f, f->root, 20, 190, 0, DE_UI_WIDGET_BUTTON);
    m->popup = ui_node(&f->base, f->root);
    DeUiPopup *p = de_entity_add(f->base.world, m->popup, f->base.ui->ids.popup);
    p->modal = true;
    p->anchor = m->quit;
    ui_node_edit(&f->base, m->popup)->display = DE_UI_NONE;
    m->confirm = nav_node(f, m->popup, 0, 0, 0, DE_UI_WIDGET_BUTTON);
    m->events = 1469598103934665603ULL;
    nav_frame(f);
}
static void menu_game(menu *m) {
    nav_fx *f = &m->nav;
    de_ui *ui = f->base.ui;
    const de_ui_event *event;
    while ((event = de_event_next(f->base.world, &f->events))) {
        m->events ^= (u64)event->kind;
        m->events *= 1099511628211ULL;
        m->events ^= event->target.bits;
        m->events *= 1099511628211ULL;
        m->events ^= event->action.hash;
        m->events *= 1099511628211ULL;
        if (event->kind == DE_UI_EV_CLICK) {
            ++m->clicks;
            if (event->target.bits == m->quit.bits)
                ui_node_edit(&f->base, m->popup)->display = DE_UI_FLEX;
            if (event->target.bits == m->confirm.bits) {
                ++m->exits;
                ui_node_edit(&f->base, m->popup)->display = DE_UI_NONE;
            }
        }
        if (event->kind == DE_UI_EV_CHANGED)
            ++m->changes;
    }
    de_entity focused = de_ui_focused(ui);
    de_entity controls[] = {m->play, m->music, m->volume, m->quit, m->confirm};
    for (u32 i = 0; i < 5; ++i)
        if (focused.bits == controls[i].bits)
            m->visited |= 1u << i;
}
static menu_trace trace(menu *m) {
    nav_fx *f = &m->nav;
    de_ui *ui = f->base.ui;
    return (menu_trace){
        .focus = de_ui_focused(ui).bits,
        .events = m->events,
        .input = de_input_tick_checksum(f->input),
        .clicks = m->clicks,
        .changes = m->changes,
        .exits = m->exits,
        .visited = m->visited,
        .music =
            ((const DeUiToggle *)de_entity_get_const(f->base.world, m->music, ui->widgets->toggle))
                ->on,
        .volume =
            ((const DeUiSlider *)de_entity_get_const(f->base.world, m->volume, ui->widgets->slider))
                ->value};
}
static void pad_script(de_input *in, u32 tick) {
    static const struct {
        u32 tick;
        de_pad_button button;
    } presses[] = {{1, DE_PAD_RSHOULDER},  {3, DE_PAD_SOUTH},      {5, DE_PAD_DPAD_DOWN},
                   {7, DE_PAD_SOUTH},      {9, DE_PAD_DPAD_DOWN},  {11, DE_PAD_DPAD_RIGHT},
                   {13, DE_PAD_DPAD_LEFT}, {15, DE_PAD_RSHOULDER}, {17, DE_PAD_SOUTH},
                   {20, DE_PAD_EAST},      {22, DE_PAD_SOUTH},     {25, DE_PAD_SOUTH}};
    for (u32 i = 0; i < sizeof presses / sizeof presses[0]; ++i) {
        if (tick == presses[i].tick)
            input_test_pad_button(in, 7, presses[i].button, true);
        if (tick == presses[i].tick + 1)
            input_test_pad_button(in, 7, presses[i].button, false);
    }
}
TEST(ui_menu_replay, gamepad_only_sample_menu_record_save_replay_without_input) {
    enum { TICKS = 40 };
    menu live = {0};
    menu_setup(&live);
    input_test_pad_add(live.nav.input, 7, DE_PLATFORM_PAD_PS5, 0, "Sample menu gamepad");
    nav_frame(&live.nav);
    de_input_recording *recording = NULL;
    ASSERT_EQ(de_input_record_begin(
                  live.nav.input, &(de_input_record_desc){.tick_rate = 60, .reserve_ticks = TICKS},
                  &recording),
              DE_OK);
    menu_trace expected[TICKS];
    for (u32 t = 0; t < TICKS; ++t) {
        pad_script(live.nav.input, t);
        nav_pre(&live.nav);
        nav_fixed(&live.nav);
        ASSERT_EQ(de_input_record_tick(live.nav.input, recording, t), DE_OK);
        nav_post(&live.nav);
        menu_game(&live);
        expected[t] = trace(&live);
        de_world_update_events(live.nav.base.world);
    }
    EXPECT_EQ(live.visited, 31u);
    EXPECT_EQ(live.clicks, 4u);
    EXPECT_EQ(live.changes, 3u);
    EXPECT_EQ(live.exits, 1u);
    EXPECT_TRUE(expected[TICKS - 1].music);
    ASSERT_EQ(de_fs_mount("uimenu", DE_UI_REPLAY_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_input_record_save(recording, "uimenu:/sample_menu.deir"), DE_OK);
    EXPECT_EQ(de_input_recording_tick_count(recording), TICKS);
    de_input_recording_destroy(recording);
    nav_fx_teardown(&live.nav);
    menu replay = {0};
    menu_setup(&replay);
    ASSERT_EQ(de_input_replay_load(replay.nav.input, "uimenu:/sample_menu.deir", &recording),
              DE_OK);
    /* No calls to any raw event helper in this loop, including no pad connection. */
    for (u32 t = 0; t < TICKS; ++t) {
        nav_pre(&replay.nav);
        ASSERT_TRUE(de_input_replay_tick(replay.nav.input, recording, t));
        nav_fixed(&replay.nav);
        nav_post(&replay.nav);
        menu_game(&replay);
        menu_trace actual = trace(&replay);
        EXPECT_EQ(actual.focus, expected[t].focus);
        EXPECT_EQ(actual.events, expected[t].events);
        EXPECT_EQ(actual.input, expected[t].input);
        EXPECT_EQ(actual.visited, expected[t].visited);
        EXPECT_EQ(actual.clicks, expected[t].clicks);
        EXPECT_EQ(actual.changes, expected[t].changes);
        EXPECT_EQ(actual.exits, expected[t].exits);
        EXPECT_EQ(actual.music, expected[t].music);
        EXPECT_FLOAT_EQ(actual.volume, expected[t].volume, 0);
        de_world_update_events(replay.nav.base.world);
    }
    EXPECT_EQ(replay.visited, 31u);
    EXPECT_EQ(replay.exits, 1u);
    de_input_recording_destroy(recording);
    nav_fx_teardown(&replay.nav);
    ASSERT_EQ(de_fs_unmount("uimenu", DE_UI_REPLAY_DIR), DE_OK);
}
