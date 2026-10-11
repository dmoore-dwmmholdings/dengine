/* devices_headless.c: the raw input and devices API for DE_HEADLESS builds, which do not link
 * SDL (SPEC §16.3; DENG-246). Built with platform_headless.c instead of the *_sdl.c files.
 * There are no devices: event reads return 0, everything else DE_ERR_UNSUPPORTED or zero. */
#include <dengine/de_platform.h>

#include <string.h>

u32 de_platform_input_events(de_raw_input_event *out, u32 max) {
    (void)out;
    (void)max;
    return 0;
}

u64 de_platform_input_dropped(void) {
    return 0;
}

/* ---------- gamepads ---------- */

u32 de_platform_pads(u32 *out, u32 max) {
    (void)out;
    (void)max;
    return 0;
}

de_result de_platform_pad_info_get(u32 pad, de_platform_pad_info *out) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "gamepad info: NULL out");
    memset(out, 0, sizeof *out);
    return DE_FAIL(DE_ERR_NOT_FOUND, "gamepad %u is not connected (headless build)", (unsigned)pad);
}

de_result de_platform_pad_rumble(u32 pad, f32 low, f32 high, u32 duration_ms) {
    (void)low;
    (void)high;
    (void)duration_ms;
    return DE_FAIL(DE_ERR_NOT_FOUND, "gamepad %u is not connected (headless build)", (unsigned)pad);
}

de_result de_platform_pad_trigger_rumble(u32 pad, f32 left, f32 right, u32 duration_ms) {
    (void)left;
    (void)right;
    (void)duration_ms;
    return DE_FAIL(DE_ERR_NOT_FOUND, "gamepad %u is not connected (headless build)", (unsigned)pad);
}

de_result de_platform_pad_set_led(u32 pad, u8 r, u8 g, u8 b) {
    (void)r;
    (void)g;
    (void)b;
    return DE_FAIL(DE_ERR_NOT_FOUND, "gamepad %u is not connected (headless build)", (unsigned)pad);
}

de_result de_platform_pad_send_effect(u32 pad, const void *data, u32 size) {
    if (!data || size == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "gamepad effect: no data");
    return DE_FAIL(DE_ERR_NOT_FOUND, "gamepad %u is not connected (headless build)", (unsigned)pad);
}

bool de_platform_pad_has_sensor(u32 pad, de_platform_sensor_type type) {
    (void)pad;
    (void)type;
    return false;
}

de_result de_platform_pad_sensor_enable(u32 pad, de_platform_sensor_type type, bool on) {
    (void)type;
    (void)on;
    return DE_FAIL(DE_ERR_NOT_FOUND, "gamepad %u is not connected (headless build)", (unsigned)pad);
}

/* ---------- device sensors ---------- */

u32 de_platform_sensors(de_platform_sensor_info *out, u32 max) {
    (void)out;
    (void)max;
    return 0;
}

de_result de_platform_sensor_enable(u32 sensor, bool on) {
    (void)sensor;
    (void)on;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "sensors: headless build");
}

de_result de_platform_sensor_read(u32 sensor, f32 out[3]) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sensor read: NULL out");
    return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "sensor %u is not open (headless build)",
                   (unsigned)sensor);
}

/* ---------- clipboard, cursor, text input ---------- */

de_result de_platform_clipboard_set(const char *text) {
    if (!text)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "clipboard: NULL text");
    return DE_FAIL(DE_ERR_UNSUPPORTED, "clipboard: headless build");
}

size_t de_platform_clipboard_get(char *buf, size_t cap) {
    if (buf && cap > 0)
        buf[0] = '\0';
    return 0;
}

de_result de_platform_cursor_show(bool visible) {
    (void)visible;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "cursor: headless build");
}

bool de_platform_cursor_visible(void) {
    return false;
}

de_result de_platform_cursor_set(de_platform_cursor shape) {
    (void)shape;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "cursor: headless build");
}

de_result de_platform_mouse_relative(de_window w, bool on) {
    (void)w;
    (void)on;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "mouse: headless build");
}

bool de_platform_mouse_relative_get(de_window w) {
    (void)w;
    return false;
}

de_result de_platform_mouse_warp(de_window w, f32 x, f32 y) {
    (void)w;
    (void)x;
    (void)y;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "mouse: headless build");
}

de_result de_platform_text_input_start(de_window w, const de_platform_text_area *area) {
    (void)w;
    (void)area;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "text input: headless build");
}

de_result de_platform_text_input_area(de_window w, const de_platform_text_area *area) {
    (void)w;
    (void)area;
    return DE_FAIL(DE_ERR_UNSUPPORTED, "text input: headless build");
}

void de_platform_text_input_stop(de_window w) {
    (void)w;
}

bool de_platform_text_input_active(de_window w) {
    (void)w;
    return false;
}

/* ---------- file dialogs ---------- */

de_result de_platform_file_dialog(const de_file_dialog_desc *desc, de_file_dialog_fn fn,
                                  void *user) {
    (void)user;
    if (!desc || !fn)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "file dialog: NULL desc or callback");
    return DE_FAIL(DE_ERR_UNSUPPORTED, "file dialog: headless build");
}
