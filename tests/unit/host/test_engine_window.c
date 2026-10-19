/*
 * test_engine_window.c: `(de__engine_desc){0}` is valid (SPEC §5.1) and opens a window from the
 * game.toml defaults (DENG-90). Label `window`: skips when no video driver can start (CI
 * runners without a display). A DE_HEADLESS build has no video, so the engine runs headless.
 */
#include "de_test.h"

#include "host/engine_internal.h"

#include <string.h>
#include <dengine/de_gpu.h>
#include <dengine/de_render.h>

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
#if DE_HAS_RENDER
typedef struct native_host_window {
    de_engine *engine;
    de_gpu_device *device;
    de_renderer *renderer;
} native_host_window;
static void native_host_window_setup(native_host_window *f) {
    (void)f;
}
static void native_host_window_teardown(native_host_window *f) {
    de_renderer_destroy(f->renderer);
    de_gpu_device_destroy(f->device);
    de__engine_destroy(f->engine);
}
static void host_renderer(native_host_window *f, bool vulkan) {
    de__game_config config;
    de__game_config_defaults(&config);
    config.window_width = 128;
    config.window_height = 64;
    config.task_workers = 1;
    if (vulkan) {
        config.window_surface_auto = false;
        config.window_surface = DE_WINDOW_SURFACE_VULKAN;
    }
    de_result result = de__engine_create(&(de__engine_desc){.config = &config}, &f->engine);
    if (result == DE_ERR_UNSUPPORTED)
        SKIP("requested native window surface unavailable");
    ASSERT_EQ(result, DE_OK);
    if (de_platform_headless())
        SKIP("headless platform has no native surface");
    result = de_gpu_device_create(
        &(de_gpu_device_desc){.backend = vulkan ? DE_GPU_VULKAN : DE_GPU_DEFAULT,
                              .pipeline_cache_dir = ""},
        &f->device);
    if (result == DE_ERR_UNSUPPORTED)
        SKIP("requested GPU backend unavailable");
    ASSERT_EQ(result, DE_OK);
    ASSERT_EQ(de_renderer_create(&(de_renderer_desc){.device = f->device,
                                                     .window = f->engine->window,
                                                     .assets = f->engine->assets,
                                                     .types = f->engine->types},
                                 &f->renderer),
              DE_OK);
    EXPECT_TRUE(f->renderer != NULL);
    if (de_gpu_get_caps(f->device)->backend == DE_GPU_METAL)
        EXPECT_TRUE(de_window_native(f->engine->window).metal_layer != NULL);
}
TEST_F(engine_window, default_surface_allows_game_owned_renderer, native_host_window) {
    host_renderer(f, false);
}
TEST_F(engine_window, explicit_vulkan_surface_allows_game_owned_renderer, native_host_window) {
    host_renderer(f, true);
}
#endif
