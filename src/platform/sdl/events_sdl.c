/* events_sdl.c: de_platform_pump on SDL3: app/window events out, input events routed to the
 * input layer (SPEC §6, §17; DENG-245). */
#include "platform/sdl/sdl_internal.h"

#include <string.h>

/* ---------- Input routing ---------- */

bool de__platform_sdl_is_input_event(Uint32 type) {
    /* Keyboard and text (0x300), mouse (0x400), joystick (0x600), gamepad (0x650), touch and
     * pinch (0x700), clipboard (0x900); sensor (0x1200), pen (0x1300). */
    return (type >= SDL_EVENT_KEY_DOWN && type < SDL_EVENT_DROP_FILE) ||
           (type >= SDL_EVENT_SENSOR_UPDATE && type < SDL_EVENT_CAMERA_DEVICE_ADDED);
}

void de__platform_sdl_route_input(const SDL_Event *ev) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st)
        return;
    if (st->input_hook) {
        st->input_hook(ev, st->input_hook_user);
        return;
    }
    if (st->input_count == DE__PLATFORM_SDL_INPUT_RING) {
        st->input_dropped++;
        return;
    }
    u32 tail = (st->input_head + st->input_count) % DE__PLATFORM_SDL_INPUT_RING;
    st->input[tail] = *ev;
    st->input_count++;
}

void de__platform_sdl_set_input_hook(de__platform_sdl_input_hook fn, void *user) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st)
        return;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    st->input_hook = fn;
    st->input_hook_user = fn ? user : NULL;
}

u32 de__platform_sdl_input_drain(SDL_Event *out, u32 max) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st || !out)
        return 0;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    u32 n = 0;
    while (n < max && st->input_count > 0) {
        out[n++] = st->input[st->input_head];
        st->input_head = (st->input_head + 1u) % DE__PLATFORM_SDL_INPUT_RING;
        st->input_count--;
    }
    return n;
}

u32 de__platform_sdl_input_dropped(void) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    return st ? st->input_dropped : 0u;
}

/* ---------- App events ---------- */

/* Translates one non-input SDL event. Returns false when it is not an app event. */
static bool de__translate(de__platform_sdl_state *st, const SDL_Event *ev, de_app_event *out) {
    memset(out, 0, sizeof *out);
    switch (ev->type) {
    case SDL_EVENT_QUIT:
        out->kind = DE_EV_QUIT;
        return true;
    case SDL_EVENT_LOW_MEMORY:
        out->kind = DE_EV_LOW_MEMORY;
        return true;

    case SDL_EVENT_DISPLAY_ADDED:
    case SDL_EVENT_DISPLAY_REMOVED:
        de__platform_sdl_displays_refresh(st);
        out->kind = DE_EV_DISPLAY_CHANGED;
        out->a = de__platform_sdl_display_index(st, ev->display.displayID);
        out->b = ev->type == SDL_EVENT_DISPLAY_ADDED ? DE_DISPLAY_ADDED : DE_DISPLAY_REMOVED;
        return true;
    case SDL_EVENT_DISPLAY_ORIENTATION:
    case SDL_EVENT_DISPLAY_MOVED:
    case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
    case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
    case SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED:
        out->kind = DE_EV_DISPLAY_CHANGED;
        out->a = de__platform_sdl_display_index(st, ev->display.displayID);
        out->b = DE_DISPLAY_MODE;
        return true;

    case SDL_EVENT_DROP_FILE: {
        out->kind = DE_EV_DROP_FILE;
        out->window = de__platform_sdl_window_from_id(st, ev->drop.windowID);
        const char *src = ev->drop.data ? ev->drop.data : "";
        size_t n = strlen(src) + 1;
        char *copy = (char *)de_arena_push(st->paths, n, 1);
        if (!copy) {
            de_error_clear();
            DE_WARN(DE_LOG_CAT_PLATFORM, "pump: dropped file path of %zu bytes did not fit", n);
            return false;
        }
        memcpy(copy, src, n);
        out->path = copy;
        return true;
    }
    default:
        break;
    }

    if (ev->type < SDL_EVENT_WINDOW_FIRST || ev->type > SDL_EVENT_WINDOW_LAST)
        return false;

    /* Window events: only for engine windows. */
    de__platform_sdl_window *slot = de__platform_sdl_slot_from_id(st, ev->window.windowID);
    if (!slot)
        return false;
    out->window = de__platform_sdl_window_from_id(st, ev->window.windowID);
    switch (ev->type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        out->kind = DE_EV_WINDOW_RESIZED;
        out->a = ev->window.data1 > 0 ? (u32)ev->window.data1 : 0u;
        out->b = ev->window.data2 > 0 ? (u32)ev->window.data2 : 0u;
        return true;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        out->kind = DE_EV_WINDOW_FOCUS;
        out->a = ev->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        return true;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED: {
        bool minimized = ev->type == SDL_EVENT_WINDOW_MINIMIZED;
        if (minimized == slot->minimized)
            return false; /* RESTORED after MAXIMIZED, or a repeat: no change to report */
        slot->minimized = minimized;
        out->kind = DE_EV_WINDOW_MINIMIZED;
        out->a = minimized;
        return true;
    }
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        out->kind = DE_EV_WINDOW_CLOSE;
        return true;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        out->kind = DE_EV_DISPLAY_CHANGED;
        out->a = de__platform_sdl_display_index(st, (SDL_DisplayID)ev->window.data1);
        out->b = DE_DISPLAY_WINDOW_MOVED;
        return true;
    case SDL_EVENT_WINDOW_HDR_STATE_CHANGED:
        out->kind = DE_EV_DISPLAY_CHANGED;
        out->a = de__platform_sdl_display_index(st, SDL_GetDisplayForWindow(slot->sdl));
        out->b = DE_DISPLAY_HDR;
        return true;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        out->kind = DE_EV_DISPLAY_CHANGED;
        out->a = de__platform_sdl_display_index(st, SDL_GetDisplayForWindow(slot->sdl));
        out->b = DE_DISPLAY_SCALE;
        return true;
    default:
        return false;
    }
}

u32 de_platform_pump(de_app_event *events, u32 max_events) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st)
        return 0;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    if (!events)
        max_events = 0;

    /* The paths handed out by the previous pump expire now. */
    de_arena_reset(st->paths);

    u32 n = 0;
    SDL_Event ev;
    /* Once `events` is full, stop taking events from SDL, so the rest keep their order for the
     * next call. With max_events == 0 everything is drained and app events are discarded. */
    while ((max_events == 0 || n < max_events) && SDL_PollEvent(&ev)) {
        if (de__platform_sdl_is_input_event(ev.type)) {
            de__platform_sdl_route_input(&ev);
            continue;
        }
        de_app_event out;
        if (!de__translate(st, &ev, &out))
            continue;
        if (max_events)
            events[n++] = out;
    }
    return n;
}
