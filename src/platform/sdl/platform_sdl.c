/* platform_sdl.c: de_platform_init / de_platform_shutdown on SDL3 (SPEC §6, DENG-245). */
#include "platform/sdl/sdl_internal.h"

/* Only for SDL_SetMainReady(): the engine owns main(), so SDL must not rename it. */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include <string.h>

/* The one platform state. SDL is process-wide, so the layer on top of it is too (like logging
 * and the cvar registry); every entry point checks for NULL. */
static de__platform_sdl_state *de__platform;

de__platform_sdl_state *de__platform_sdl_get(void) {
    return de__platform;
}

static void de__copy_str(char *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n >= cap)
        n = cap - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

de_result de_platform_init(const de_platform_desc *desc) {
    static const de_platform_desc defaults = {0};
    if (!desc)
        desc = &defaults;
    if (de__platform)
        return DE_FAIL(DE_ERR_BUSY, "platform: already initialised");

    const de_allocator *a = desc->allocator ? desc->allocator : de_heap_allocator();
    if (!a->alloc || !a->free)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "platform: allocator without alloc/free");

    de__platform_sdl_state *st = DE_NEW(a, de__platform_sdl_state, DE_MEM_GENERAL);
    if (!st)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "platform: no memory for the platform state");
    memset(st, 0, sizeof *st);
    st->alloc = a;
    st->main_thread = de_thread_current_id();
    st->headless = (desc->flags & DE_PLATFORM_HEADLESS) != 0;
    de__copy_str(st->app_name, sizeof st->app_name, desc->app_name ? desc->app_name : "DEngine");

    de_arena_desc ad = {0};
    ad.reserve_bytes = DE__PLATFORM_SDL_PATH_ARENA_BYTES;
    ad.name = "platform drop paths";
    de_result r = de_arena_create(&ad, &st->paths);
    if (r != DE_OK) {
        DE_FREE(a, st, sizeof *st);
        return DE_FAIL_WRAP(r, "platform: drop-path arena");
    }

    /* The engine owns main(): SDL_main is never used (SDL_MAIN_HANDLED semantics). */
    SDL_SetMainReady();
    SDL_SetAppMetadata(st->app_name, desc->app_version, desc->app_identifier);
    if (desc->app_org)
        SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING, desc->app_org);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, "game");

    /* Video brings in events. Headless only needs events (DE_EV_QUIT from SIGINT/SIGTERM). */
    SDL_InitFlags flags = st->headless ? SDL_INIT_EVENTS : SDL_INIT_VIDEO;
    if (!SDL_Init(flags)) {
        de_result fail = st->headless ? DE_ERR_INTERNAL : DE_ERR_UNSUPPORTED;
        r = DE_FAIL(fail, "platform: SDL_Init(%s) failed: %s", st->headless ? "events" : "video",
                    SDL_GetError());
        SDL_Quit();
        de_arena_destroy(st->paths);
        DE_FREE(a, st, sizeof *st);
        return r;
    }

    if (!st->headless)
        de__platform_sdl_displays_refresh(st);
    de__platform = st;
    return DE_OK;
}

void de_platform_shutdown(void) {
    de__platform_sdl_state *st = de__platform;
    if (!st)
        return;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    de__platform_sdl_windows_destroy_all(st);
    SDL_Quit();
    de__platform = NULL;
    de_arena_destroy(st->paths);
    DE_FREE(st->alloc, st, sizeof *st);
}

bool de_platform_headless(void) {
    return !de__platform || de__platform->headless;
}
