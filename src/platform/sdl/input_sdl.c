/* input_sdl.c: the raw input ring and the translation of SDL input events into
 * de_raw_input_event (SPEC §6, §15; DENG-246). Also the devices init/shutdown and text input.
 *
 * The pump hands every input event to de__platform_sdl_devices_route() (the DENG-245 input
 * hook). It is translated at once, because SDL frees event strings at the next poll, and pushed
 * into a ring allocated at init: no allocation per event. A full ring drops its oldest event.
 */
#include "platform/sdl/sdl_internal.h"

#include <string.h>

/* How often the SDL-to-engine clock offset is measured again (100 ms of SDL time). */
#define DE__CLOCK_RESYNC_NS ((Uint64)100000000u)

DE_STATIC_ASSERT(sizeof(de_raw_input_event) == 64, "de_raw_input_event is one cache line");
DE_STATIC_ASSERT(sizeof(((de_raw_text *)0)->utf8) == 32, "text events carry 32 bytes inline");

/* The raw numbering is SDL's, which de_input's enums copy (DENG-246 header notes). */
DE_STATIC_ASSERT(SDL_GAMEPAD_BUTTON_SOUTH == 0 && SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14 &&
                     SDL_GAMEPAD_BUTTON_MISC1 == 15 && SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1 == 16 &&
                     SDL_GAMEPAD_BUTTON_TOUCHPAD == 20 && SDL_GAMEPAD_BUTTON_COUNT == 26,
                 "SDL gamepad button order changed");
DE_STATIC_ASSERT(SDL_GAMEPAD_AXIS_LEFTX == 0 && SDL_GAMEPAD_AXIS_RIGHT_TRIGGER == 5,
                 "SDL gamepad axis order changed");

/* ---------- ring ---------- */

void de__platform_sdl_ring_push(de__platform_sdl_devices *dv, const de_raw_input_event *ev) {
    if (dv->ring_count == DE_PLATFORM_INPUT_RING) {
        /* Drop the oldest: the newest input is what the game must not miss. */
        dv->ring_head = (dv->ring_head + 1u) % DE_PLATFORM_INPUT_RING;
        dv->ring_count--;
        dv->ring_dropped++;
    }
    u32 tail = (dv->ring_head + dv->ring_count) % DE_PLATFORM_INPUT_RING;
    dv->ring[tail] = *ev;
    dv->ring_count++;
}

u32 de_platform_input_events(de_raw_input_event *out, u32 max) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st || !st->devices || !out)
        return 0;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    de__platform_sdl_devices *dv = st->devices;
    u32 n = 0;
    while (n < max && dv->ring_count > 0) {
        /* Copy in up to two contiguous runs instead of one event at a time. */
        u32 run = DE_PLATFORM_INPUT_RING - dv->ring_head;
        if (run > dv->ring_count)
            run = dv->ring_count;
        if (run > max - n)
            run = max - n;
        memcpy(out + n, dv->ring + dv->ring_head, (size_t)run * sizeof *out);
        n += run;
        dv->ring_head = (dv->ring_head + run) % DE_PLATFORM_INPUT_RING;
        dv->ring_count -= run;
    }
    return n;
}

u64 de_platform_input_dropped(void) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    return st && st->devices ? st->devices->ring_dropped : 0u;
}

/* ---------- translation ---------- */

u64 de__platform_sdl_time_ns(de__platform_sdl_devices *dv, Uint64 sdl_ticks_ns) {
    /* Both clocks are monotonic and run at the same rate; only their origins differ. The offset
     * is measured with the SDL read between two engine reads (paired with their midpoint), and
     * measured again every DE__CLOCK_RESYNC_NS so drift between the two sources stays small.
     * A fixed offset keeps the order of events; the clamp keeps it across a re-measure. */
    Uint64 ticks = SDL_GetTicksNS();
    if (!dv->clock_synced || ticks - dv->clock_sync_ticks > DE__CLOCK_RESYNC_NS) {
        u64 before = de_time_now_ns();
        ticks = SDL_GetTicksNS();
        u64 mid = before + (de_time_now_ns() - before) / 2u;
        dv->clock_offset = (i64)(mid - (u64)ticks);
        dv->clock_sync_ticks = ticks;
        dv->clock_synced = true;
    }
    if (sdl_ticks_ns == 0 || sdl_ticks_ns > ticks)
        sdl_ticks_ns = ticks; /* unstamped, or stamped ahead of the clock: use "now" */
    i64 t = (i64)sdl_ticks_ns + dv->clock_offset;
    u64 r = t > 0 ? (u64)t : 0u;
    if (r < dv->last_time_ns)
        r = dv->last_time_ns;
    dv->last_time_ns = r;
    return r;
}

static u16 de__mods(SDL_Keymod m) {
    u16 r = 0;
    if (m & SDL_KMOD_LSHIFT)
        r |= DE_KEYMOD_LSHIFT;
    if (m & SDL_KMOD_RSHIFT)
        r |= DE_KEYMOD_RSHIFT;
    if (m & SDL_KMOD_LCTRL)
        r |= DE_KEYMOD_LCTRL;
    if (m & SDL_KMOD_RCTRL)
        r |= DE_KEYMOD_RCTRL;
    if (m & SDL_KMOD_LALT)
        r |= DE_KEYMOD_LALT;
    if (m & SDL_KMOD_RALT)
        r |= DE_KEYMOD_RALT;
    if (m & SDL_KMOD_LGUI)
        r |= DE_KEYMOD_LGUI;
    if (m & SDL_KMOD_RGUI)
        r |= DE_KEYMOD_RGUI;
    if (m & SDL_KMOD_NUM)
        r |= DE_KEYMOD_NUM;
    if (m & SDL_KMOD_CAPS)
        r |= DE_KEYMOD_CAPS;
    if (m & SDL_KMOD_MODE)
        r |= DE_KEYMOD_ALTGR;
    return r;
}

/* SDL buttons are 1 left, 2 middle, 3 right, 4 X1, 5 X2; de_mouse_button is left, right,
 * middle, X1, X2. */
static u8 de__mouse_button(Uint8 b) {
    switch (b) {
    case SDL_BUTTON_LEFT:
        return 0;
    case SDL_BUTTON_RIGHT:
        return 1;
    case SDL_BUTTON_MIDDLE:
        return 2;
    default:
        return b > 0 ? (u8)(b - 1u) : 0u;
    }
}

/* Length of the longest prefix of `s` (at most `max` bytes) that ends on a UTF-8 code point
 * boundary. */
static size_t de__utf8_prefix(const char *s, size_t len, size_t max) {
    if (len <= max)
        return len;
    size_t n = max;
    /* Back up over continuation bytes (10xxxxxx) to the lead byte of the cut code point. */
    while (n > 0 && ((unsigned char)s[n] & 0xC0u) == 0x80u)
        n--;
    return n;
}

static void de__text_events(de__platform_sdl_devices *dv, de_raw_input_event *base,
                            const char *text) {
    if (!text)
        return;
    size_t len = strlen(text);
    const size_t cap = sizeof base->text.utf8 - 1u;
    while (len > 0) {
        size_t n = de__utf8_prefix(text, len, cap);
        if (n == 0)
            n = 1; /* invalid UTF-8 with a run of continuation bytes: make progress anyway */
        memset(&base->text, 0, sizeof base->text);
        memcpy(base->text.utf8, text, n);
        de__platform_sdl_ring_push(dv, base);
        text += n;
        len -= n;
    }
}

static f32 de__axis(SDL_GamepadAxis axis, Sint16 v) {
    if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        return v <= 0 ? 0.0f : (f32)v / 32767.0f;
    f32 f = (f32)v / 32767.0f; /* -32768 → slightly below -1: clamp */
    return f < -1.0f ? -1.0f : f;
}

void de__platform_sdl_devices_route(const SDL_Event *ev, void *user) {
    de__platform_sdl_state *st = (de__platform_sdl_state *)user;
    de__platform_sdl_devices *dv = st->devices;
    de_raw_input_event e;
    memset(&e, 0, sizeof e);
    e.time_ns = de__platform_sdl_time_ns(dv, ev->common.timestamp);

    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        e.kind = DE_RAW_KEY;
        e.window = de__platform_sdl_window_from_id(st, ev->key.windowID);
        e.device = ev->key.which;
        e.key.code = (u16)ev->key.scancode; /* SDL scancodes are USB HID usage codes */
        e.key.mods = de__mods(ev->key.mod);
        e.key.sym =
            (ev->key.key & (SDLK_SCANCODE_MASK | SDLK_EXTENDED_MASK)) ? 0u : (u32)ev->key.key;
        e.key.down = ev->key.down;
        e.key.repeat = ev->key.repeat;
        break;
    case SDL_EVENT_TEXT_INPUT:
        e.kind = DE_RAW_TEXT;
        e.window = de__platform_sdl_window_from_id(st, ev->text.windowID);
        de__text_events(dv, &e, ev->text.text);
        return;
    case SDL_EVENT_TEXT_EDITING: {
        e.kind = DE_RAW_TEXT_EDIT;
        e.window = de__platform_sdl_window_from_id(st, ev->edit.windowID);
        const char *t = ev->edit.text ? ev->edit.text : "";
        size_t n = de__utf8_prefix(t, strlen(t), sizeof e.text.utf8 - 1u);
        memcpy(e.text.utf8, t, n);
        e.text.start = ev->edit.start;
        e.text.length = ev->edit.length;
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        e.kind = DE_RAW_MOUSE_MOVE;
        e.window = de__platform_sdl_window_from_id(st, ev->motion.windowID);
        e.device = ev->motion.which;
        e.mouse_move.x = ev->motion.x;
        e.mouse_move.y = ev->motion.y;
        e.mouse_move.dx = ev->motion.xrel;
        e.mouse_move.dy = ev->motion.yrel;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        e.kind = DE_RAW_MOUSE_BUTTON;
        e.window = de__platform_sdl_window_from_id(st, ev->button.windowID);
        e.device = ev->button.which;
        e.mouse_button.x = ev->button.x;
        e.mouse_button.y = ev->button.y;
        e.mouse_button.button = de__mouse_button(ev->button.button);
        e.mouse_button.down = ev->button.down;
        e.mouse_button.clicks = ev->button.clicks;
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        e.kind = DE_RAW_MOUSE_WHEEL;
        e.window = de__platform_sdl_window_from_id(st, ev->wheel.windowID);
        e.device = ev->wheel.which;
        /* "Natural" scrolling flips the values; undo it so +y always means away from the user. */
        f32 sign = ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        e.mouse_wheel.dx = ev->wheel.x * sign;
        e.mouse_wheel.dy = ev->wheel.y * sign;
        e.mouse_wheel.x = ev->wheel.mouse_x;
        e.mouse_wheel.y = ev->wheel.mouse_y;
        break;
    }

    case SDL_EVENT_GAMEPAD_ADDED:
        if (!de__platform_sdl_pad_added(dv, ev->gdevice.which))
            return;
        e.kind = DE_RAW_PAD_ADDED;
        e.device = ev->gdevice.which;
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (!de__platform_sdl_pad_removed(dv, ev->gdevice.which))
            return;
        e.kind = DE_RAW_PAD_REMOVED;
        e.device = ev->gdevice.which;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (!de__platform_sdl_pad_known(dv, ev->gbutton.which))
            return;
        e.kind = DE_RAW_PAD_BUTTON;
        e.device = ev->gbutton.which;
        e.pad_button.button = ev->gbutton.button;
        e.pad_button.down = ev->gbutton.down;
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (!de__platform_sdl_pad_known(dv, ev->gaxis.which))
            return;
        e.kind = DE_RAW_PAD_AXIS;
        e.device = ev->gaxis.which;
        e.pad_axis.axis = ev->gaxis.axis;
        e.pad_axis.value = de__axis((SDL_GamepadAxis)ev->gaxis.axis, ev->gaxis.value);
        break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        if (!de__platform_sdl_pad_known(dv, ev->gtouchpad.which))
            return;
        e.kind = DE_RAW_PAD_TOUCHPAD;
        e.device = ev->gtouchpad.which;
        e.pad_touch.touchpad = (u8)ev->gtouchpad.touchpad;
        e.pad_touch.finger = (u8)ev->gtouchpad.finger;
        e.pad_touch.phase = ev->type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN ? (u8)DE_TOUCH_DOWN
                            : ev->type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP ? (u8)DE_TOUCH_UP
                                                                        : (u8)DE_TOUCH_MOVE;
        e.pad_touch.x = ev->gtouchpad.x;
        e.pad_touch.y = ev->gtouchpad.y;
        e.pad_touch.pressure = ev->gtouchpad.pressure;
        break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        if (!de__platform_sdl_pad_known(dv, ev->gsensor.which))
            return;
        e.kind = DE_RAW_PAD_SENSOR;
        e.device = ev->gsensor.which;
        e.sensor.sensor =
            (u32)de__platform_sdl_sensor_type_from_sdl((SDL_SensorType)ev->gsensor.sensor);
        e.sensor.data[0] = ev->gsensor.data[0];
        e.sensor.data[1] = ev->gsensor.data[1];
        e.sensor.data[2] = ev->gsensor.data[2];
        e.sensor.sensor_time_ns = ev->gsensor.sensor_timestamp;
        break;

    default:
        /* Joystick (non-gamepad), touchscreen, pinch, pen, clipboard, device-sensor and
         * keyboard/mouse hot-plug events are not part of the raw stream. Device sensors are read
         * with de_platform_sensor_read(). */
        return;
    }
    de__platform_sdl_ring_push(dv, &e);
}

/* ---------- init / shutdown ---------- */

de_result de__platform_sdl_devices_init(de__platform_sdl_state *st) {
    de__platform_sdl_devices *dv = DE_NEW(st->alloc, de__platform_sdl_devices, DE_MEM_GENERAL);
    if (!dv)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "platform: no memory for the input ring");
    memset(dv, 0, sizeof *dv);
    st->devices = dv;

    if (!st->headless) {
        /* Missing gamepad or sensor support is not fatal: those calls then report nothing. */
        dv->pads_ok = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        if (!dv->pads_ok)
            DE_WARN(DE_LOG_CAT_PLATFORM, "platform: no gamepad support: %s", SDL_GetError());
        dv->sensors_ok = SDL_InitSubSystem(SDL_INIT_SENSOR);
        if (!dv->sensors_ok)
            DE_WARN(DE_LOG_CAT_PLATFORM, "platform: no sensor support: %s", SDL_GetError());
    }
    de__platform_sdl_set_input_hook(de__platform_sdl_devices_route, st);
    return DE_OK;
}

void de__platform_sdl_devices_shutdown(de__platform_sdl_state *st) {
    de__platform_sdl_devices *dv = st->devices;
    if (!dv)
        return;
    de__platform_sdl_dialogs_shutdown();
    if (st->input_hook == de__platform_sdl_devices_route)
        de__platform_sdl_set_input_hook(NULL, NULL);
    if (!st->headless) {
        for (u32 i = 0; i < DE_WINDOW_MAX; i++)
            if (st->windows[i].sdl && SDL_TextInputActive(st->windows[i].sdl))
                SDL_StopTextInput(st->windows[i].sdl);
    }
    de__platform_sdl_pads_close_all(dv);
    de__platform_sdl_sensors_close_all(dv);
    de__platform_sdl_cursors_free(dv);
    st->devices = NULL;
    DE_FREE(st->alloc, dv, sizeof *dv);
}

/* ---------- text input ---------- */

static SDL_Window *de__text_window(de_window w, de_result *r) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st || st->headless) {
        *r = DE_FAIL(DE_ERR_UNSUPPORTED, "text input: no video (headless)");
        return NULL;
    }
    SDL_Window *win = de__platform_sdl_window_get(w);
    if (!win)
        *r = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "text input: stale window handle");
    return win;
}

static de_result de__set_area(SDL_Window *win, const de_platform_text_area *area) {
    SDL_Rect rc = {0, 0, 0, 0};
    int cursor = 0;
    if (area) {
        rc.x = area->x;
        rc.y = area->y;
        rc.w = area->w;
        rc.h = area->h;
        cursor = area->cursor;
    }
    if (!SDL_SetTextInputArea(win, area ? &rc : NULL, cursor))
        return DE_FAIL(DE_ERR_IO, "text input: set area: %s", SDL_GetError());
    return DE_OK;
}

de_result de_platform_text_input_start(de_window w, const de_platform_text_area *area) {
    de_result r = DE_OK;
    SDL_Window *win = de__text_window(w, &r);
    if (!win)
        return r;
    r = de__set_area(win, area);
    if (r != DE_OK)
        return r;
    if (!SDL_StartTextInput(win))
        return DE_FAIL(DE_ERR_IO, "text input: start: %s", SDL_GetError());
    return DE_OK;
}

de_result de_platform_text_input_area(de_window w, const de_platform_text_area *area) {
    de_result r = DE_OK;
    SDL_Window *win = de__text_window(w, &r);
    if (!win)
        return r;
    return de__set_area(win, area);
}

void de_platform_text_input_stop(de_window w) {
    SDL_Window *win = de__platform_sdl_window_get(w);
    if (win)
        SDL_StopTextInput(win);
}

bool de_platform_text_input_active(de_window w) {
    SDL_Window *win = de__platform_sdl_window_get(w);
    return win && SDL_TextInputActive(win);
}
