#include "session.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static struct {
    terminal_session session;
    terminal_journal journal;
    de_entity camera, status, aircraft, popup, suggestion;
    de_load_handle font_load;
    de_font font;
    de_event_reader events;
    bool recording, failed;
    char status_text[256], journal_path[DE_FS_PATH_MAX];
} app;
static const char initial[] = "// Terminal script - editable while aircraft simulation is paused.\n"
                              "function update(aircraft) {\n"
                              "    if (aircraft.altitude < 1000) {\n"
                              "        aircraft.climb(1500);\n"
                              "    }\n"
                              "    aircraft.heading = 270;\n"
                              "    aircraft.speed = 220;\n"
                              "}\n\n"
                              "// Up/Down moves by visible line; Shift extends selection.\n"
                              "// Ctrl/Cmd+Z undoes. Ctrl/Cmd+Shift+Z or Ctrl+Y redoes.\n"
                              "// Click/drag selects. Ctrl/Cmd+C/X/V uses the system clipboard.\n";
static de_component comp(de_engine *e, const char *name) {
    return de_component_find(e->world, name);
}
static de_entity node(de_engine *e, de_entity parent, f32 x, f32 y, f32 width, f32 height) {
    de_entity n = de_entity_spawn(e->world);
    DeUiNode *layout = de_entity_add(e->world, n, comp(e, "DeUiNode"));
    layout->width = (de_ui_len){DE_UI_PX, width};
    layout->height = (de_ui_len){DE_UI_PX, height};
    if (parent.bits)
        (void)de_entity_set_parent(e->world, n, parent);
    *(DeUiAnchor *)de_entity_add(e->world, n, comp(e, "DeUiAnchor")) =
        (DeUiAnchor){.offset_min = {x, y}, .offset_max = {x + width, y + height}};
    return n;
}
static void label(de_engine *e, de_entity n, const char *str, f32 size) {
    *(DeUiText *)de_entity_add(e->world, n, comp(e, "DeUiText")) = (DeUiText){
        .literal = {str, strlen(str)},
        .style = {
            .font = app.font, .size_px = size, .line_height = 1.1f, .color = {.65f, 1, .76f, 1}}};
}
static void observe(de_entity entity, const de_ui_text_edit *edit, void *user) {
    (void)user;
    if (app.recording && entity.bits == app.session.editor.bits)
        app.failed |= !terminal_append(&app.journal, (terminal_entry){TERM_EDIT, *edit});
}
static void operation(u32 kind, u32 keys) {
    terminal_entry entry = {.operation = kind, .edit.caret = keys};
    app.failed |= !terminal_apply(&app.session, &entry);
    if (app.recording)
        app.failed |= !terminal_append(&app.journal, entry);
}
static void reset(de_engine *e) {
    (void)de_ui_text_input_set(e->ui, app.session.editor, (de_str){initial, sizeof(initial) - 1});
    app.session.paused = false;
    app.session.x = app.session.z = 0;
    app.session.walk_ticks = app.session.aircraft_ticks = 0;
    operation(TERM_LEAVE, 0);
}
static void control(de_system_ctx *ctx) {
    de_engine *e = ctx->user;
    de_input *in = e->input;
    if (de_key_pressed(in, DE_KEY_F8)) {
        app.recording = false;
        terminal_journal_clear(&app.journal);
        reset(e);
        app.recording = true;
        app.failed = false;
    }
    if (de_key_pressed(in, DE_KEY_F9)) {
        app.recording = false;
        app.failed |= !terminal_save(&app.journal, app.journal_path);
    }
    if (de_key_pressed(in, DE_KEY_F10)) {
        app.recording = false;
        if (terminal_load(&app.journal, app.journal_path)) {
            reset(e);
            for (u32 i = 0; i < app.journal.count; ++i)
                app.failed |= !terminal_apply(&app.session, &app.journal.entries[i]);
        } else
            app.failed = true;
    }
    if (!app.session.editing &&
        (de_key_pressed(in, DE_KEY_E) || de_ui_focused(e->ui).bits == app.session.editor.bits))
        operation(TERM_ENTER, 0);
    else if (app.session.editing && de_key_pressed(in, DE_KEY_ESCAPE))
        operation(TERM_LEAVE, 0);
    if (de_key_pressed(in, DE_KEY_F6))
        operation(TERM_PAUSE, 0);
    if (app.session.editing && (de_key_pressed(in, DE_KEY_F2) ||
                                ((de_key_down(in, DE_KEY_LCTRL) || de_key_down(in, DE_KEY_LGUI)) &&
                                 de_key_pressed(in, DE_KEY_SPACE))))
        operation(TERM_MENU, 0);
    ((DeUiNode *)de_entity_get(e->world, app.popup, comp(e, "DeUiNode")))->display =
        app.session.editing && app.session.popup_open ? DE_UI_FLEX : DE_UI_NONE;
    const de_ui_event *event;
    while ((event = de_event_next(e->world, &app.events)))
        if (event->kind == DE_UI_EV_CLICK && event->target.bits == app.suggestion.bits) {
            de_ui_text_edit edit = {
                .kind = DE_UI_TEXT_REPLACE,
                .text = {"aircraft.climb(1500);", sizeof("aircraft.climb(1500);") - 1}};
            (void)de_ui_text_input_apply(e->ui, app.session.editor, &edit);
            if (app.session.popup_open)
                operation(TERM_MENU, 0);
            ((DeUiNode *)de_entity_get(e->world, app.popup, comp(e, "DeUiNode")))->display =
                DE_UI_NONE;
            de_ui_set_focus(e->ui, app.session.editor);
        }
}
static void simulate(de_system_ctx *ctx) {
    de_engine *e = ctx->user;
    u32 keys = 0;
    if (de_key_down(e->input, DE_KEY_W))
        keys |= 1;
    if (de_key_down(e->input, DE_KEY_S))
        keys |= 2;
    if (de_key_down(e->input, DE_KEY_A))
        keys |= 4;
    if (de_key_down(e->input, DE_KEY_D))
        keys |= 8;
    operation(TERM_TICK, keys);
}
static void display(de_system_ctx *ctx) {
    de_engine *e = ctx->user;
    snprintf(app.status_text, sizeof app.status_text,
             "%s | Aircraft %s tick %u | %s\nE edit / Esc walk | WASD move | F6 pause\nF8 record / "
             "F9 save / F10 replay | F2 / Ctrl+Space menu",
             app.session.editing    ? "EDITING - cursor free"
             : app.session.captured ? "WALKING - mouse captured"
                                    : "WALKING - capture FAILED",
             app.session.paused ? "PAUSED" : "RUNNING", app.session.aircraft_ticks,
             app.failed      ? "Journal ERROR (full or I/O)"
             : app.recording ? "RECORDING"
                             : "Ready");
    label(e, app.status, app.status_text, 16);
    DeTransform *camera = de_entity_get(e->world, app.camera, comp(e, "DeTransform"));
    camera->position = (de_dvec3){app.session.x, 0, 3 + app.session.z};
    DeTransform *aircraft = de_entity_get(e->world, app.aircraft, comp(e, "DeTransform"));
    aircraft->position.x = sin(app.session.aircraft_ticks * .02) * 2;
}
static de_result init(de_engine *e) {
    memset(&app, 0, sizeof app);
    DE_TRY(de_fs_resolve("user:/terminal-session.tuij", app.journal_path, sizeof app.journal_path));
    DE_TRY(de_gpu_device_create(&(de_gpu_device_desc){0}, &e->gpu));
    DE_TRY(de_renderer_create(
        &(de_renderer_desc){
            .device = e->gpu, .window = e->window, .assets = e->assets, .types = e->types},
        &e->renderer));
    de_renderer_attach(e->renderer, e->world);
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    settings.ao = settings.gi = settings.reflections = settings.shadows = DE_QUALITY_OFF;
    de_renderer_set_settings(e->renderer, &settings);
    DE_TRY(de_text_attach(e->text, e->renderer, e->world));
    DE_TRY(de_fs_mount("terminal", DE_TERMINAL_ROOT, DE_MOUNT_READ_ONLY, 0));
    DE_TRY(de_asset_manifest_load(e->assets, "terminal:/examples/terminal_demo/assets.toml"));
    de_asset_id font = de_asset_id_from("terminal/font");
    app.font = de_asset_font(e->assets, font);
    app.font_load = de_asset_load(e->assets, font, 0);
    if (de_asset_wait(e->assets, app.font_load, 60000000000ULL) != DE_LOAD_READY)
        return DE_FAIL(DE_ERR_IO, "terminal font failed to load");
    de_text_update(e->text);
    DE_TRY(de_ui_create(&(de_ui_desc){.renderer = e->renderer,
                                      .text = e->text,
                                      .input = e->input,
                                      .window = e->window,
                                      .max_nodes = 64,
                                      .reference_height = 720},
                        &e->ui));
    de_ui_attach(e->ui, e->world);
    de_entity panel = node(e, DE_ENTITY_NULL, 0, 0, 640, 400);
    *(DeUiWorldPanel *)de_entity_add(e->world, panel, comp(e, "DeUiWorldPanel")) =
        (DeUiWorldPanel){.size_m = {3.2f, 2}, .pixels_per_meter = 200, .depth_test = true};
    *(DeTransform *)de_entity_add(e->world, panel, comp(e, "DeTransform")) =
        de_transform_identity();
    *(DeUiStyle *)de_entity_add(e->world, panel, comp(e, "DeUiStyle")) =
        (DeUiStyle){.background = {.012f, .027f, .021f, 1},
                    .border_color = {.2f, .6f, .35f, 1},
                    .border_width = 3,
                    .opacity = 1};
    app.status = node(e, panel, 12, 8, 616, 70);
    label(e, app.status, "Terminal starting", 16);
    app.session = (terminal_session){
        .ui = e->ui, .window = e->window, .editor = node(e, panel, 12, 88, 616, 298)};
    (void)de_entity_add(e->world, app.session.editor, comp(e, "DeUiTextInput"));
    (void)de_entity_add(e->world, app.session.editor, comp(e, "DeUiFocusable"));
    label(e, app.session.editor, "", 18);
    ((DeUiTextInput *)de_entity_get(e->world, app.session.editor, comp(e, "DeUiTextInput")))
        ->multiline = true;
    de_entity anchor = node(e, panel, 610, 370, 10, 10);
    app.popup = node(e, panel, 0, 0, 220, 72);
    *(DeUiPopup *)de_entity_add(e->world, app.popup, comp(e, "DeUiPopup")) =
        (DeUiPopup){.anchor = anchor, .placement = DE_UI_POPUP_BOTTOM};
    *(DeUiStyle *)de_entity_add(e->world, app.popup, comp(e, "DeUiStyle")) =
        (DeUiStyle){.background = {.04f, .13f, .09f, 1}, .opacity = 1};
    ((DeUiNode *)de_entity_get(e->world, app.popup, comp(e, "DeUiNode")))->display = DE_UI_NONE;
    app.suggestion = node(e, app.popup, 6, 6, 208, 52);
    label(e, app.suggestion, "aircraft.climb(1500);", 16);
    (void)de_entity_add(e->world, app.suggestion, comp(e, "DeUiButton"));
    app.camera = de_entity_spawn(e->world);
    *(DeTransform *)de_entity_add(e->world, app.camera, comp(e, "DeTransform")) =
        de_transform_identity();
    *(DeCamera *)de_entity_add(e->world, app.camera, comp(e, "DeCamera")) =
        (DeCamera){.fov_y = 1.1f, .near_z = .1f};
    app.aircraft = node(e, DE_ENTITY_NULL, 0, 0, 160, 40);
    *(DeUiWorldPanel *)de_entity_add(e->world, app.aircraft, comp(e, "DeUiWorldPanel")) =
        (DeUiWorldPanel){.size_m = {.8f, .2f}, .pixels_per_meter = 200, .depth_test = true};
    DeTransform pose = de_transform_identity();
    pose.position = (de_dvec3){0, 1.4, -1};
    *(DeTransform *)de_entity_add(e->world, app.aircraft, comp(e, "DeTransform")) = pose;
    label(e, app.aircraft, "< AIRCRAFT >", 20);
    app.events = de_event_reader_create(
        e->world, de_event_register(e->world, de_type_find(e->types, "de_ui_event")));
    de_ui_set_text_edit_observer(e->ui, observe, NULL);
    reset(e);
    return DE_OK;
}
static void shutdown(de_engine *e) {
    de_ui_set_text_edit_observer(e->ui, NULL, NULL);
    (void)de_platform_mouse_relative(e->window, false);
    terminal_journal_clear(&app.journal);
    if (app.font_load.id)
        de_asset_release(e->assets, app.font_load);
}
static void systems(de_engine *e, de_world *w) {
    const char *after[] = {"ui.edit"};
    (void)de_system_add(w, &(de_system_desc){.name = "terminal.control",
                                             .schedule = DE_SCHEDULE_PRE_UPDATE,
                                             .fn = control,
                                             .user = e,
                                             .after = after,
                                             .after_count = 1,
                                             .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE});
    (void)de_system_add(w, &(de_system_desc){.name = "terminal.simulate",
                                             .schedule = DE_SCHEDULE_FIXED_UPDATE,
                                             .fn = simulate,
                                             .user = e,
                                             .flags = DE_SYSTEM_MAIN_THREAD});
    (void)de_system_add(w, &(de_system_desc){.name = "terminal.display",
                                             .schedule = DE_SCHEDULE_UPDATE,
                                             .fn = display,
                                             .user = e,
                                             .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE});
}
static void types(de_engine *e, de_type_registry *registry) {
    (void)e;
    (void)registry;
}
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    static const de_game_api api = {.api_version = DE_GAME_API_VERSION,
                                    .name = "Terminal integration",
                                    .init = init,
                                    .shutdown = shutdown,
                                    .register_systems = systems,
                                    .register_types = types};
    return &api;
}
