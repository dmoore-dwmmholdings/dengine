/* cursor_sdl.c: cursor visibility and shape, relative mouse mode and warping (SPEC §6;
 * DENG-246). */
#include "platform/sdl/sdl_internal.h"

/* de_platform_cursor copies the first entries of SDL_SystemCursor. */
DE_STATIC_ASSERT(SDL_SYSTEM_CURSOR_DEFAULT == (int)DE_CURSOR_DEFAULT &&
                     SDL_SYSTEM_CURSOR_NWSE_RESIZE == (int)DE_CURSOR_RESIZE_NWSE &&
                     SDL_SYSTEM_CURSOR_NS_RESIZE == (int)DE_CURSOR_RESIZE_NS &&
                     SDL_SYSTEM_CURSOR_POINTER == (int)DE_CURSOR_POINTER,
                 "SDL system cursor order changed");

/* The devices state when video is up, else NULL. */
static de__platform_sdl_devices *de__video_devices(void) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st || st->headless || !st->devices)
        return NULL;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    return st->devices;
}

void de__platform_sdl_cursors_free(de__platform_sdl_devices *dv) {
    for (u32 i = 0; i < DE_CURSOR_COUNT_; i++) {
        if (dv->cursors[i]) {
            SDL_DestroyCursor(dv->cursors[i]);
            dv->cursors[i] = NULL;
        }
    }
}

de_result de_platform_cursor_show(bool visible) {
    if (!de__video_devices())
        return DE_FAIL(DE_ERR_UNSUPPORTED, "cursor: no video (headless)");
    if (!(visible ? SDL_ShowCursor() : SDL_HideCursor()))
        return DE_FAIL(DE_ERR_IO, "cursor: show/hide: %s", SDL_GetError());
    return DE_OK;
}

bool de_platform_cursor_visible(void) {
    return de__video_devices() && SDL_CursorVisible();
}

de_result de_platform_cursor_set(de_platform_cursor shape) {
    de__platform_sdl_devices *dv = de__video_devices();
    if (!dv)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "cursor: no video (headless)");
    if ((u32)shape >= DE_CURSOR_COUNT_)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "cursor: bad shape %d", (int)shape);
    if (!dv->cursors[shape]) {
        dv->cursors[shape] = SDL_CreateSystemCursor((SDL_SystemCursor)shape);
        if (!dv->cursors[shape])
            return DE_FAIL(DE_ERR_UNSUPPORTED, "cursor: shape %d: %s", (int)shape, SDL_GetError());
    }
    if (!SDL_SetCursor(dv->cursors[shape]))
        return DE_FAIL(DE_ERR_IO, "cursor: set: %s", SDL_GetError());
    return DE_OK;
}

/* The SDL window of `w`, or NULL with the error in `*r`. */
static SDL_Window *de__mouse_window(de_window w, de_result *r) {
    if (!de__video_devices()) {
        *r = DE_FAIL(DE_ERR_UNSUPPORTED, "mouse: no video (headless)");
        return NULL;
    }
    SDL_Window *win = de__platform_sdl_window_get(w);
    if (!win)
        *r = DE_FAIL(DE_ERR_INVALID_ARGUMENT, "mouse: stale window handle");
    return win;
}

de_result de_platform_mouse_relative(de_window w, bool on) {
    de_result r = DE_OK;
    SDL_Window *win = de__mouse_window(w, &r);
    if (!win)
        return r;
    if (!SDL_SetWindowRelativeMouseMode(win, on))
        return DE_FAIL(DE_ERR_IO, "mouse: relative mode: %s", SDL_GetError());
    return DE_OK;
}

bool de_platform_mouse_relative_get(de_window w) {
    SDL_Window *win = de__platform_sdl_window_get(w);
    return win && SDL_GetWindowRelativeMouseMode(win);
}

de_result de_platform_mouse_warp(de_window w, f32 x, f32 y) {
    de_result r = DE_OK;
    SDL_Window *win = de__mouse_window(w, &r);
    if (!win)
        return r;
    SDL_WarpMouseInWindow(win, x, y);
    return DE_OK;
}
