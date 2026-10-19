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

/* Final S29 exit: actual advanced widgets and their dynamically generated controls.
 * The game only authors components/tab order and handles public BIND_ITEM/CLICK events;
 * every activation/navigation operation comes from recorded gamepad actions. */
#include "ui/widgets/advanced_internal.h"
typedef struct advanced_menu {
    nav_fx nav;
    de_entity play, music, volume, input, dropdown, tabs, headers[2], scroll, scrolled, list, quit,
        popup, confirm;
    u64 events;
    u32 visited, rows, binds, changes, clicks, exits;
} advanced_menu;
typedef struct advanced_trace {
    u64 focus, events, input, editor, text_hash;
    u32 visited, rows, binds, changes, clicks, exits, selected, tab;
    f32 volume, scroll, list_scroll;
    bool music, dropdown_open, ring, scrolled_visible;
} advanced_trace;
static void advanced_game(advanced_menu *m) {
    nav_fx *f = &m->nav;
    de_ui *ui = f->base.ui;
    de_world *w = f->base.world;
    const de_ui_event *event;
    while ((event = de_event_next(w, &f->events))) {
        m->events ^= (u64)event->kind;
        m->events *= 1099511628211ULL;
        m->events ^= event->target.bits;
        m->events *= 1099511628211ULL;
        u32 value;
        memcpy(&value, &event->value, sizeof value);
        m->events ^= value;
        m->events *= 1099511628211ULL;
        if (event->kind == DE_UI_EV_BIND_ITEM) {
            ++m->binds;
            /* A game populates real pooled rows through the public list event contract. */
            if (de_entity_parent(w, event->target).bits == m->list.bits) {
                DeUiButton *button = de_entity_add(w, event->target, ui->widgets->button);
                button->on_click = de_name_from("choose_item");
                DeUiFocusable *focusable = de_entity_add(w, event->target, ui->widgets->focusable);
                focusable->tab_index = 80 + (i32)event->value;
            }
        }
        if (event->kind == DE_UI_EV_CHANGED)
            ++m->changes;
        if (event->kind == DE_UI_EV_CLICK) {
            ++m->clicks;
            if (event->target.bits == m->quit.bits)
                ui_node_edit(&f->base, m->popup)->display = DE_UI_FLEX;
            if (event->target.bits == m->confirm.bits) {
                ++m->exits;
                ui_node_edit(&f->base, m->popup)->display = DE_UI_NONE;
            }
        }
    }
    de_entity focused = de_ui_focused(ui);
    de_entity controls[] = {m->play, m->music,      m->volume,     m->input,  m->dropdown,
                            m->tabs, m->headers[0], m->headers[1], m->scroll, m->scrolled,
                            m->list, m->quit,       m->confirm};
    for (u32 i = 0; i < sizeof controls / sizeof controls[0]; ++i)
        if (focused.bits && focused.bits == controls[i].bits)
            m->visited |= 1u << i;
    if (focused.bits && de_entity_parent(w, focused).bits == m->list.bits) {
        const de_ui_item_state *row = de_entity_get_const(w, focused, ui->advanced->item);
        if (row && row->index < 6)
            m->rows |= 1u << row->index;
    }
}
static void advanced_setup(advanced_menu *m) {
    nav_fx_setup(&m->nav);
    nav_fx *f = &m->nav;
    de_ui *ui = f->base.ui;
    de_world *w = f->base.world;
    m->events = 1469598103934665603ULL;
    m->play = nav_node(f, f->root, 20, 10, 10, DE_UI_WIDGET_BUTTON);
    m->music = nav_node(f, f->root, 20, 60, 20, DE_UI_WIDGET_TOGGLE);
    m->volume = nav_node(f, f->root, 20, 110, 30, DE_UI_WIDGET_SLIDER);
    m->input = nav_node(f, f->root, 20, 160, 40, DE_UI_WIDGET_INPUT);
    DeUiTextInput *input = de_entity_add(w, m->input, ui->advanced->input);
    memcpy(input->buffer, "Pilot", 6);
    input->length = input->caret = 5;
    m->dropdown = nav_node(f, f->root, 20, 210, 50, DE_UI_WIDGET_DROPDOWN);
    DeUiDropdown *drop = de_entity_add(w, m->dropdown, ui->advanced->dropdown);
    drop->option_count = 3;
    drop->options[0] = de_name_from("One");
    drop->options[1] = de_name_from("Two");
    drop->options[2] = de_name_from("Three");
    m->tabs = nav_node(f, f->root, 260, 20, 60, DE_UI_WIDGET_TABS);
    (void)de_entity_add(w, m->tabs, ui->advanced->tabs);
    DeUiAnchor *a = de_entity_get(w, m->tabs, ui->ids.anchor);
    a->offset_max = (de_vec2){560, 150};
    for (u32 i = 0; i < 2; ++i) {
        de_entity page = ui_node(&f->base, m->tabs);
        DeUiText *text = de_entity_add(w, page, ui->ids.text);
        text->literal = i ? DE_STR("Audio") : DE_STR("Video");
    }
    m->scroll = nav_node(f, f->root, 300, 170, 70, DE_UI_WIDGET_SCROLL);
    a = de_entity_get(w, m->scroll, ui->ids.anchor);
    a->offset_max = (de_vec2){500, 250};
    ((DeUiScroll *)de_entity_add(w, m->scroll, ui->advanced->scroll))->vertical = true;
    m->scrolled = nav_node(f, m->scroll, 0, 300, 71, DE_UI_WIDGET_BUTTON);
    m->list = nav_node(f, f->root, 300, 270, 75, DE_UI_WIDGET_LIST);
    a = de_entity_get(w, m->list, ui->ids.anchor);
    a->offset_max = (de_vec2){500, 350};
    *(DeUiList *)de_entity_add(w, m->list, ui->advanced->list) =
        (DeUiList){.item_count = 6, .item_height = 20};
    m->quit = nav_node(f, f->root, 20, 340, 90, DE_UI_WIDGET_BUTTON);
    m->popup = ui_node(&f->base, f->root);
    *(DeUiPopup *)de_entity_add(w, m->popup, ui->ids.popup) =
        (DeUiPopup){.anchor = m->quit, .modal = true};
    ui_node_edit(&f->base, m->popup)->display = DE_UI_NONE;
    m->confirm = nav_node(f, m->popup, 0, 0, 0, DE_UI_WIDGET_BUTTON);
    for (u32 i = 0; i < 8; ++i) {
        nav_frame(f);
        advanced_game(m);
        de_world_update_events(w);
    }
    const de_ui_tab_state *tabs = de_entity_get_const(w, m->tabs, ui->advanced->tab_state);
    ASSERT_TRUE(tabs);
    u32 i = 0;
    for (de_entity e = de_entity_children_first(w, tabs->headers); e.bits;
         e = de_entity_children_next(w, e)) {
        ASSERT_TRUE(i < 2);
        m->headers[i] = e;
        ((DeUiFocusable *)de_entity_get(w, e, ui->widgets->focusable))->tab_index = 61 + (i32)i;
        ++i;
    }
    ASSERT_EQ(i, 2u);
}
static advanced_trace advanced_snapshot(advanced_menu *m) {
    nav_fx *f = &m->nav;
    de_ui *ui = f->base.ui;
    de_world *w = f->base.world;
    const DeUiTextInput *input = de_entity_get_const(w, m->input, ui->advanced->input);
    u64 text = 1469598103934665603ULL;
    for (u32 i = 0; i < input->length; ++i) {
        text ^= (u8)input->buffer[i];
        text *= 1099511628211ULL;
    }
    const de_ui_popup_state *popup = de_entity_get_const(w, m->dropdown, ui->advanced->popup);
    de_vec4 viewport =
        ((const DeUiNode *)de_entity_get_const(w, m->scroll, ui->ids.node))->computed_rect;
    de_vec4 child =
        ((const DeUiNode *)de_entity_get_const(w, m->scrolled, ui->ids.node))->computed_rect;
    bool visible = child.y >= viewport.y && child.y + child.w <= viewport.y + viewport.w;
    return (advanced_trace){
        .focus = de_ui_focused(ui).bits,
        .events = m->events,
        .input = de_input_tick_checksum(f->input),
        .editor = ui->advanced->active_input.bits,
        .text_hash = text,
        .visited = m->visited,
        .rows = m->rows,
        .binds = m->binds,
        .changes = m->changes,
        .clicks = m->clicks,
        .exits = m->exits,
        .selected =
            ((const DeUiDropdown *)de_entity_get_const(w, m->dropdown, ui->advanced->dropdown))
                ->selected,
        .tab = ((const DeUiTabs *)de_entity_get_const(w, m->tabs, ui->advanced->tabs))->active,
        .volume =
            ((const DeUiSlider *)de_entity_get_const(w, m->volume, ui->widgets->slider))->value,
        .scroll =
            ((const DeUiScroll *)de_entity_get_const(w, m->scroll, ui->advanced->scroll))->offset.y,
        .list_scroll =
            ((const DeUiScroll *)de_entity_get_const(w, m->list, ui->advanced->scroll))->offset.y,
        .music = ((const DeUiToggle *)de_entity_get_const(w, m->music, ui->widgets->toggle))->on,
        .dropdown_open = popup && popup->open,
        .ring = de_ui_focus_visible(ui),
        .scrolled_visible = visible};
}
static void advanced_pad_script(de_input *input, u32 tick) {
    static const struct {
        u32 tick;
        de_pad_button button;
    } presses[] = {{1, DE_PAD_RSHOULDER},  {3, DE_PAD_SOUTH},       {5, DE_PAD_RSHOULDER},
                   {7, DE_PAD_SOUTH},      {9, DE_PAD_RSHOULDER},   {11, DE_PAD_DPAD_RIGHT},
                   {13, DE_PAD_RSHOULDER}, {15, DE_PAD_SOUTH},      {18, DE_PAD_RSHOULDER},
                   {21, DE_PAD_SOUTH},     {24, DE_PAD_DPAD_DOWN},  {27, DE_PAD_SOUTH},
                   {31, DE_PAD_RSHOULDER}, {33, DE_PAD_DPAD_RIGHT}, {35, DE_PAD_DPAD_LEFT},
                   {37, DE_PAD_SOUTH},     {40, DE_PAD_RSHOULDER},  {42, DE_PAD_SOUTH},
                   {45, DE_PAD_RSHOULDER}, {47, DE_PAD_SOUTH},      {50, DE_PAD_RSHOULDER},
                   {52, DE_PAD_RSHOULDER}, {54, DE_PAD_SOUTH},      {57, DE_PAD_RSHOULDER},
                   {59, DE_PAD_RSHOULDER}, {61, DE_PAD_SOUTH},      {63, DE_PAD_RSHOULDER},
                   {65, DE_PAD_SOUTH},     {67, DE_PAD_RSHOULDER},  {69, DE_PAD_SOUTH},
                   {71, DE_PAD_RSHOULDER}, {73, DE_PAD_SOUTH},      {75, DE_PAD_RSHOULDER},
                   {77, DE_PAD_SOUTH},     {79, DE_PAD_RSHOULDER},  {81, DE_PAD_SOUTH},
                   {84, DE_PAD_RSHOULDER}, {86, DE_PAD_SOUTH},      {89, DE_PAD_EAST},
                   {92, DE_PAD_SOUTH},     {95, DE_PAD_SOUTH}};
    for (u32 i = 0; i < sizeof presses / sizeof presses[0]; ++i) {
        if (tick == presses[i].tick)
            input_test_pad_button(input, 7, presses[i].button, true);
        if (tick == presses[i].tick + 1)
            input_test_pad_button(input, 7, presses[i].button, false);
    }
}
TEST(ui_menu_replay, advanced_gamepad_menu_recorded_exit_matches_fresh_inputless_replay) {
    enum { TICKS = 110 };
    advanced_menu live = {0};
    advanced_setup(&live);
    input_test_pad_add(live.nav.input, 7, DE_PLATFORM_PAD_PS5, 0, "S29 advanced menu gamepad");
    nav_frame(&live.nav);
    de_input_recording *record = NULL;
    ASSERT_EQ(de_input_record_begin(
                  live.nav.input, &(de_input_record_desc){.tick_rate = 60, .reserve_ticks = TICKS},
                  &record),
              DE_OK);
    advanced_trace expected[TICKS];
    for (u32 t = 0; t < TICKS; ++t) {
        advanced_pad_script(live.nav.input, t);
        nav_pre(&live.nav);
        nav_fixed(&live.nav);
        ASSERT_EQ(de_input_record_tick(live.nav.input, record, t), DE_OK);
        nav_post(&live.nav);
        advanced_game(&live);
        expected[t] = advanced_snapshot(&live);
        de_world_update_events(live.nav.base.world);
    }
    EXPECT_EQ(live.visited, (1u << 13) - 1u);
    EXPECT_EQ(live.rows, 63u);
    EXPECT_EQ(live.exits, 1u);
    EXPECT_EQ(expected[28].selected, 1u);
    EXPECT_EQ(expected[34].tab, 1u);
    EXPECT_EQ(expected[36].tab, 0u);
    EXPECT_TRUE(expected[16].editor == live.input.bits);
    EXPECT_TRUE(expected[53].scroll > 0);
    EXPECT_TRUE(expected[53].scrolled_visible);
    EXPECT_FLOAT_EQ(expected[TICKS - 1].volume, .1, .00001);
    EXPECT_TRUE(expected[TICKS - 1].music);
    ASSERT_EQ(de_fs_mount("uiadvanced", DE_UI_REPLAY_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    ASSERT_EQ(de_input_record_save(record, "uiadvanced:/advanced_menu.deir"), DE_OK);
    EXPECT_EQ(de_input_recording_tick_count(record), TICKS);
    de_input_recording_destroy(record);
    nav_fx_teardown(&live.nav);
    advanced_menu replay = {0};
    advanced_setup(&replay);
    ASSERT_EQ(de_input_replay_load(replay.nav.input, "uiadvanced:/advanced_menu.deir", &record),
              DE_OK);
    for (u32 t = 0; t < TICKS; ++t) {
        /* No raw event, pad, key, pointer or text injection in the fresh replay world. */
        nav_pre(&replay.nav);
        ASSERT_TRUE(de_input_replay_tick(replay.nav.input, record, t));
        nav_fixed(&replay.nav);
        nav_post(&replay.nav);
        advanced_game(&replay);
        advanced_trace actual = advanced_snapshot(&replay);
#define SAME(field) EXPECT_EQ(actual.field, expected[t].field)
        SAME(focus);
        SAME(events);
        SAME(input);
        SAME(editor);
        SAME(text_hash);
        SAME(visited);
        SAME(rows);
        SAME(binds);
        SAME(changes);
        SAME(clicks);
        SAME(exits);
        SAME(selected);
        SAME(tab);
        SAME(music);
        SAME(dropdown_open);
        SAME(ring);
        SAME(scrolled_visible);
#undef SAME
        EXPECT_FLOAT_EQ(actual.volume, expected[t].volume, 0);
        EXPECT_FLOAT_EQ(actual.scroll, expected[t].scroll, 0);
        EXPECT_FLOAT_EQ(actual.list_scroll, expected[t].list_scroll, 0);
        de_world_update_events(replay.nav.base.world);
    }
    EXPECT_EQ(replay.visited, (1u << 13) - 1u);
    EXPECT_EQ(replay.rows, 63u);
    EXPECT_EQ(replay.exits, 1u);
    de_input_recording_destroy(record);
    nav_fx_teardown(&replay.nav);
    ASSERT_EQ(de_fs_unmount("uiadvanced", DE_UI_REPLAY_DIR), DE_OK);
}
