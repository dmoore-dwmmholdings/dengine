/* window_sdl.c: de_window on SDL3: a generational slot table (SPEC §5.1, §6; DENG-245). */
#include "platform/sdl/sdl_internal.h"

#include <string.h>

/* Handle layout (de_base.h: low bits index, high bits generation): bits 0-7 = slot + 1,
 * bits 8-31 = generation (1 .. 0xFFFFFF). Zero is never a valid handle. */
#define DE__WIN_SLOT_BITS 8u
#define DE__WIN_GEN_MASK  0xFFFFFFu
DE_STATIC_ASSERT(DE_WINDOW_MAX < (1u << DE__WIN_SLOT_BITS), "slot index must fit in 8 bits");

static de_window de__make_handle(u32 slot, u32 gen) {
    de_window h = {(gen << DE__WIN_SLOT_BITS) | (slot + 1u)};
    return h;
}

/* The slot of a live handle, or NULL (zero, stale or out-of-range handle, or no platform). */
static de__platform_sdl_window *de__slot(de__platform_sdl_state *st, de_window w) {
    if (!st || w.id == 0)
        return NULL;
    u32 idx = (w.id & ((1u << DE__WIN_SLOT_BITS) - 1u));
    if (idx == 0 || idx > DE_WINDOW_MAX)
        return NULL;
    de__platform_sdl_window *s = &st->windows[idx - 1u];
    if (!s->sdl || s->gen != (w.id >> DE__WIN_SLOT_BITS))
        return NULL;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    return s;
}

SDL_Window *de__platform_sdl_window_get(de_window w) {
    de__platform_sdl_window *s = de__slot(de__platform_sdl_get(), w);
    return s ? s->sdl : NULL;
}

de__platform_sdl_window *de__platform_sdl_slot_from_id(de__platform_sdl_state *st,
                                                       SDL_WindowID id) {
    for (u32 i = 0; i < DE_WINDOW_MAX; i++)
        if (st->windows[i].sdl && st->windows[i].id == id)
            return &st->windows[i];
    return NULL;
}

de_window de__platform_sdl_window_from_id(const de__platform_sdl_state *st, SDL_WindowID id) {
    for (u32 i = 0; i < DE_WINDOW_MAX; i++)
        if (st->windows[i].sdl && st->windows[i].id == id)
            return de__make_handle(i, st->windows[i].gen);
    de_window none = {0};
    return none;
}

/* Applies `mode` to `win`. Exclusive fullscreen picks the display mode closest to the window's
 * logical size on the window's display. */
static de_result de__apply_mode(SDL_Window *win, de_window_mode mode) {
    switch (mode) {
    case DE_WINDOW_WINDOWED:
        if (!SDL_SetWindowFullscreen(win, false))
            return DE_FAIL(DE_ERR_INTERNAL, "window: leave fullscreen: %s", SDL_GetError());
        return DE_OK;
    case DE_WINDOW_BORDERLESS:
        /* A NULL mode is SDL's borderless fullscreen desktop. */
        if (!SDL_SetWindowFullscreenMode(win, NULL) || !SDL_SetWindowFullscreen(win, true))
            return DE_FAIL(DE_ERR_INTERNAL, "window: borderless fullscreen: %s", SDL_GetError());
        return DE_OK;
    case DE_WINDOW_FULLSCREEN: {
        SDL_DisplayID display = SDL_GetDisplayForWindow(win);
        int w = 0, h = 0;
        SDL_GetWindowSize(win, &w, &h);
        bool hd = (SDL_GetWindowFlags(win) & SDL_WINDOW_HIGH_PIXEL_DENSITY) != 0;
        SDL_DisplayMode closest;
        if (!display || !SDL_GetClosestFullscreenDisplayMode(display, w, h, 0.0f, hd, &closest))
            return DE_FAIL(DE_ERR_NOT_FOUND, "window: no fullscreen mode near %dx%d: %s", w, h,
                           SDL_GetError());
        if (!SDL_SetWindowFullscreenMode(win, &closest) || !SDL_SetWindowFullscreen(win, true))
            return DE_FAIL(DE_ERR_INTERNAL, "window: exclusive fullscreen %dx%d@%.2f: %s",
                           closest.w, closest.h, (double)closest.refresh_rate, SDL_GetError());
        return DE_OK;
    }
    }
    return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "window: bad mode %d", (int)mode);
}

de_result de_window_create(const de_window_desc *desc, de_window *out) {
    static const de_window_desc defaults = {0};
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_window_create: NULL out");
    out->id = 0;
    if (!desc)
        desc = &defaults;
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_window_create: de_platform_init not called");
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    if (st->headless)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "de_window_create: headless platform");
    if ((u32)desc->mode > (u32)DE_WINDOW_FULLSCREEN)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_window_create: bad mode %d", (int)desc->mode);

    u32 slot = DE_WINDOW_MAX;
    for (u32 i = 0; i < DE_WINDOW_MAX; i++) {
        if (!st->windows[i].sdl) {
            slot = i;
            break;
        }
    }
    if (slot == DE_WINDOW_MAX)
        return DE_FAIL(DE_ERR_BUSY, "de_window_create: %u windows already open", DE_WINDOW_MAX);

    SDL_DisplayID display;
    if (desc->display_index < 0) {
        display = SDL_GetPrimaryDisplay();
    } else if ((u32)desc->display_index < st->display_count) {
        display = st->displays[desc->display_index];
    } else {
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_window_create: display %d of %u",
                       (int)desc->display_index, st->display_count);
    }

    SDL_WindowFlags flags = 0;
    if (desc->resizable)
        flags |= SDL_WINDOW_RESIZABLE;
    if (desc->high_dpi)
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (desc->hidden)
        flags |= SDL_WINDOW_HIDDEN;
    de_result sr = de__platform_sdl_surface_flags(st, desc->surface, &flags); /* DENG-250 */
    if (sr != DE_OK)
        return DE_FAIL_WRAP(sr, "de_window_create: surface");

    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_window_create: %s", SDL_GetError());
    const char *title = desc->title ? desc->title : st->app_name;
    i64 pos = (i64)SDL_WINDOWPOS_CENTERED_DISPLAY(display);
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                          desc->width ? (i64)desc->width : 1280);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                          desc->height ? (i64)desc->height : 720);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, pos);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, pos);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, (i64)flags);
    SDL_Window *win = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!win)
        return DE_FAIL(DE_ERR_INTERNAL, "de_window_create: SDL_CreateWindow: %s", SDL_GetError());

    if (desc->mode != DE_WINDOW_WINDOWED) {
        de_result r = de__apply_mode(win, desc->mode);
        if (r != DE_OK) {
            SDL_DestroyWindow(win);
            return DE_FAIL_WRAP(r, "de_window_create: initial mode");
        }
    }

    de__platform_sdl_window *s = &st->windows[slot];
    if (s->gen == 0)
        s->gen = 1;
    s->sdl = win;
    s->id = SDL_GetWindowID(win);
    s->mode = desc->mode;
    s->minimized = false;
    s->surface = desc->surface;
    s->metal_view = NULL;
    *out = de__make_handle(slot, s->gen);
    return DE_OK;
}

static void de__destroy_slot(de__platform_sdl_window *s) {
    de__platform_sdl_surface_release(s); /* the Metal view goes before its window (DENG-250) */
    SDL_DestroyWindow(s->sdl);
    s->sdl = NULL;
    s->id = 0;
    /* Bump the generation so every copy of the old handle is now stale; skip 0 on wrap. */
    s->gen = (s->gen + 1u) & DE__WIN_GEN_MASK;
    if (s->gen == 0)
        s->gen = 1;
}

void de_window_destroy(de_window w) {
    de__platform_sdl_window *s = de__slot(de__platform_sdl_get(), w);
    if (s)
        de__destroy_slot(s);
}

void de__platform_sdl_windows_destroy_all(de__platform_sdl_state *st) {
    for (u32 i = 0; i < DE_WINDOW_MAX; i++)
        if (st->windows[i].sdl)
            de__destroy_slot(&st->windows[i]);
}

void de_window_size_pixels(de_window w, u32 *width, u32 *height) {
    int pw = 0, ph = 0;
    de__platform_sdl_window *s = de__slot(de__platform_sdl_get(), w);
    if (s && !SDL_GetWindowSizeInPixels(s->sdl, &pw, &ph))
        pw = ph = 0;
    if (width)
        *width = pw > 0 ? (u32)pw : 0u;
    if (height)
        *height = ph > 0 ? (u32)ph : 0u;
}

void de_window_size_points(de_window w, u32 *width, u32 *height) {
    int pw = 0, ph = 0;
    de__platform_sdl_window *s = de__slot(de__platform_sdl_get(), w);
    if (s && !SDL_GetWindowSize(s->sdl, &pw, &ph))
        pw = ph = 0;
    if (width)
        *width = pw > 0 ? (u32)pw : 0u;
    if (height)
        *height = ph > 0 ? (u32)ph : 0u;
}

de_window_insets de_window_safe_area(de_window w) {
    de__platform_sdl_window *slot = de__slot(de__platform_sdl_get(), w);
    if (!slot)
        return (de_window_insets){0};
    SDL_Rect safe;
    int width = 0, height = 0, pixels_w = 0, pixels_h = 0;
    if (!SDL_GetWindowSize(slot->sdl, &width, &height) || width <= 0 || height <= 0 ||
        !SDL_GetWindowSizeInPixels(slot->sdl, &pixels_w, &pixels_h) ||
        !SDL_GetWindowSafeArea(slot->sdl, &safe))
        return (de_window_insets){0};
    int right = width - safe.x - safe.w, bottom = height - safe.y - safe.h;
    return (de_window_insets){
        safe.x > 0 ? (u32)(((u64)safe.x * (u32)pixels_w + (u32)width / 2u) / (u32)width) : 0,
        safe.y > 0 ? (u32)(((u64)safe.y * (u32)pixels_h + (u32)height / 2u) / (u32)height) : 0,
        right > 0 ? (u32)(((u64)right * (u32)pixels_w + (u32)width / 2u) / (u32)width) : 0,
        bottom > 0 ? (u32)(((u64)bottom * (u32)pixels_h + (u32)height / 2u) / (u32)height) : 0};
}

de_result de_window_set_mode(de_window w, de_window_mode mode) {
    de__platform_sdl_window *s = de__slot(de__platform_sdl_get(), w);
    if (!s)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_window_set_mode: stale window %#x", w.id);
    de_result r = de__apply_mode(s->sdl, mode);
    if (r == DE_OK)
        s->mode = mode;
    return r;
}

de_window_mode de_window_get_mode(de_window w) {
    de__platform_sdl_window *s = de__slot(de__platform_sdl_get(), w);
    return s ? s->mode : DE_WINDOW_WINDOWED;
}

void de_window_set_title(de_window w, const char *title) {
    de__platform_sdl_window *s = de__slot(de__platform_sdl_get(), w);
    if (s && title)
        SDL_SetWindowTitle(s->sdl, title);
}

de_display_info de_window_display(de_window w) {
    de_display_info info;
    memset(&info, 0, sizeof info);
    de__platform_sdl_state *st = de__platform_sdl_get();
    de__platform_sdl_window *s = de__slot(st, w);
    if (!s)
        return info;
    SDL_DisplayID id = SDL_GetDisplayForWindow(s->sdl);
    if (id)
        de__platform_sdl_display_fill(st, id, s->sdl, &info);
    return info;
}
