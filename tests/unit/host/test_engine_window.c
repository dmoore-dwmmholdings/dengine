/*
 * test_engine_window.c: `(de__engine_desc){0}` is valid (SPEC §5.1) and opens a window from the
 * game.toml defaults (DENG-90). Label `window`: skips when no video driver can start (CI
 * runners without a display). A DE_HEADLESS build has no video, so the engine runs headless.
 */
#include "de_test.h"

#include "host/engine_internal.h"

#include <string.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_ENGINE_WINDOW_ASAN 1
#endif
#endif
#if defined(__APPLE__) && defined(TEST_ENGINE_WINDOW_ASAN)
/* Same as tests/unit/platform/test_window.c: on macOS 26 the first display cycle of a visible
 * NSWindow loads titlebar images through CoreGraphics' PDF lexer, whose strndup() on a buffer
 * without a NUL inside the bound is valid C but trips ASan's strict_string_checks=1. Suppress
 * only interceptor reports whose caller is in CoreGraphics; engine and SDL code are still
 * checked. */
const char *__asan_default_suppressions(void);
const char *__asan_default_suppressions(void) {
    return "interceptor_via_lib:CoreGraphics\n";
}
#endif

TEST(engine_window, zero_desc_defaults) {
    de__engine_desc d;
    memset(&d, 0, sizeof d);
    de_engine *e = NULL;
    de_result r = de__engine_create(&d, &e);
    if (r == DE_ERR_UNSUPPORTED || r == DE_ERR_INTERNAL)
        SKIP("no video driver: the platform cannot open a window here");
    ASSERT_EQ(r, DE_OK);
    EXPECT_TRUE(e->allocator == de_heap_allocator());
    EXPECT_TRUE(e->world != NULL);
    EXPECT_EQ(de_engine_get_timing(e).fixed_hz, 60u);
    if (de_platform_headless()) {
        EXPECT_FALSE(DE_HANDLE_VALID(e->window));
    } else {
        ASSERT_TRUE(DE_HANDLE_VALID(e->window));
        u32 w = 0, h = 0;
        de_window_size_pixels(e->window, &w, &h);
        EXPECT_GT(w, 0u);
        EXPECT_GT(h, 0u);
        EXPECT_EQ(de_window_get_mode(e->window), DE_WINDOW_WINDOWED);
    }
    de__engine_destroy(e);

    /* A NULL desc is the same as a zeroed one. */
    r = de__engine_create(NULL, &e);
    ASSERT_EQ(r, DE_OK);
    de__engine_destroy(e);
}
