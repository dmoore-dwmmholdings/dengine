/* display_sdl.c: display enumeration, display modes and HDR/DPI info on SDL3 (DENG-245). */
#include "platform/sdl/sdl_internal.h"

#include <math.h>
#include <string.h>

/* scRGB convention: linear 1.0 = 80 nits. SDL reports the SDR white level and the HDR headroom
 * in those units (SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT is "SDR white in
 * SDL_COLORSPACE_SRGB_LINEAR"). */
#define DE__SCRGB_NITS 80.0f

void de__platform_sdl_displays_refresh(de__platform_sdl_state *st) {
    st->display_count = 0;
    int n = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&n);
    if (!ids)
        return;
    u32 cap = (u32)(sizeof st->displays / sizeof st->displays[0]);
    for (int i = 0; i < n && st->display_count < cap; i++)
        st->displays[st->display_count++] = ids[i];
    SDL_free(ids);
}

u32 de__platform_sdl_display_index(const de__platform_sdl_state *st, SDL_DisplayID id) {
    for (u32 i = 0; i < st->display_count; i++)
        if (st->displays[i] == id)
            return i;
    return ~0u;
}

/* The first engine window on display `id`, for its SDR white level and HDR headroom. */
static SDL_Window *de__window_on_display(de__platform_sdl_state *st, SDL_DisplayID id) {
    for (u32 i = 0; i < DE_WINDOW_MAX; i++) {
        SDL_Window *w = st->windows[i].sdl;
        if (w && SDL_GetDisplayForWindow(w) == id)
            return w;
    }
    return NULL;
}

static u32 de__round_u32(f32 v) {
    return v > 0.0f ? (u32)lroundf(v) : 0u;
}

void de__platform_sdl_display_fill(de__platform_sdl_state *st, SDL_DisplayID id, SDL_Window *win,
                                   de_display_info *out) {
    memset(out, 0, sizeof *out);
    const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(id);
    if (mode) {
        f32 density = mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
        out->width = de__round_u32((f32)mode->w * density);
        out->height = de__round_u32((f32)mode->h * density);
        out->refresh_hz = de__round_u32(mode->refresh_rate);
    }
    out->content_scale = SDL_GetDisplayContentScale(id);
    if (out->content_scale <= 0.0f)
        out->content_scale = 1.0f;
    SDL_PropertiesID props = SDL_GetDisplayProperties(id);
    out->hdr_capable =
        props && SDL_GetBooleanProperty(props, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
    out->primary = SDL_GetPrimaryDisplay() == id;
    const char *name = SDL_GetDisplayName(id);
    if (name) {
        size_t n = strlen(name);
        if (n >= sizeof out->name)
            n = sizeof out->name - 1;
        /* Do not cut a UTF-8 sequence in half. */
        while (n > 0 && ((unsigned char)name[n] & 0xC0u) == 0x80u)
            n--;
        memcpy(out->name, name, n);
        out->name[n] = '\0';
    }

    if (!win)
        win = de__window_on_display(st, id);
    if (win) {
        /* SDL clamps both to at least 1.0 and keeps them up to date on HDR state changes. */
        SDL_PropertiesID wp = SDL_GetWindowProperties(win);
        f32 white = SDL_GetFloatProperty(wp, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
        f32 headroom = SDL_GetFloatProperty(wp, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
        if (white < 1.0f)
            white = 1.0f;
        if (headroom < 1.0f)
            headroom = 1.0f;
        out->sdr_white_nits = white * DE__SCRGB_NITS;
        out->max_nits = out->sdr_white_nits * headroom;
        out->hdr_headroom = headroom;
    } else {
        out->sdr_white_nits = DE__SCRGB_NITS;
        out->max_nits = out->hdr_capable ? 0.0f : DE__SCRGB_NITS;
        out->hdr_headroom = out->hdr_capable ? 0.0f : 1.0f;
    }
}

u32 de_display_count(void) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st || st->headless)
        return 0;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    return st->display_count;
}

de_result de_display_info_get(u32 index, de_display_info *out) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_display_info_get: NULL out");
    memset(out, 0, sizeof *out);
    if (!st || st->headless)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "de_display_info_get: no video (headless)");
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    if (index >= st->display_count)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_display_info_get: index %u of %u displays",
                       index, st->display_count);
    de__platform_sdl_display_fill(st, st->displays[index], NULL, out);
    return DE_OK;
}

u32 de_display_modes(u32 index, de_display_mode *out, u32 max) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st || st->headless || index >= st->display_count)
        return 0;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    int n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(st->displays[index], &n);
    if (!modes)
        return 0;
    /* SDL already sorts best first: size, then pixel density, then refresh rate. */
    for (int i = 0; i < n && out && (u32)i < max; i++) {
        const SDL_DisplayMode *m = modes[i];
        f32 density = m->pixel_density > 0.0f ? m->pixel_density : 1.0f;
        out[i].width = de__round_u32((f32)m->w * density);
        out[i].height = de__round_u32((f32)m->h * density);
        out[i].refresh_hz = m->refresh_rate;
        out[i].pixel_density = density;
    }
    SDL_free(modes);
    return n > 0 ? (u32)n : 0u;
}
