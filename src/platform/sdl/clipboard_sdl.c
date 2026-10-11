/* clipboard_sdl.c: system clipboard text (SPEC §6; DENG-246). Needs video, so headless
 * platforms report DE_ERR_UNSUPPORTED / empty. */
#include "platform/sdl/sdl_internal.h"

#include <string.h>

static bool de__clipboard_ok(void) {
    de__platform_sdl_state *st = de__platform_sdl_get();
    if (!st || st->headless)
        return false;
    DE__PLATFORM_SDL_ASSERT_MAIN(st);
    return true;
}

de_result de_platform_clipboard_set(const char *text) {
    if (!text)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "clipboard: NULL text");
    if (!de__clipboard_ok())
        return DE_FAIL(DE_ERR_UNSUPPORTED, "clipboard: no video (headless)");
    if (!SDL_SetClipboardText(text))
        return DE_FAIL(DE_ERR_IO, "clipboard: set: %s", SDL_GetError());
    return DE_OK;
}

size_t de_platform_clipboard_get(char *buf, size_t cap) {
    if (buf && cap > 0)
        buf[0] = '\0';
    if (!de__clipboard_ok())
        return 0;
    char *text = SDL_GetClipboardText(); /* "" (never NULL) when empty or on error */
    if (!text)
        return 0;
    size_t len = strlen(text);
    if (buf && cap > 0) {
        size_t n = len < cap ? len : cap - 1u;
        /* Do not split a code point: back up over continuation bytes of the cut one. */
        if (n < len)
            while (n > 0 && ((unsigned char)text[n] & 0xC0u) == 0x80u)
                n--;
        memcpy(buf, text, n);
        buf[n] = '\0';
    }
    SDL_free(text);
    return len;
}
