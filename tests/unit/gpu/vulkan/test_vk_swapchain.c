/* test_vk_swapchain.c — Vulkan swapchain on a hidden window (DENG-284). CTest names:
 * gpu_vulkan.vk.<test>.
 *
 * swapchain_present_resize: 120 frames of acquire / clear / present on a hidden 320x240 window,
 * then the window is resized to 200x150 (through SDL) and the swapchain with it (new images at
 * the window's new pixel size), then 30 more frames, then 10 frames each with IMMEDIATE and
 * MAILBOX swapchains; zero validation errors. It SKIPs without a video
 * driver (no display), without a Vulkan loader or device, or when the device cannot present to
 * the window. The window's surface comes from SDL's Vulkan loader and the device from
 * vk_loader.c's: when DE_VULKAN_LIBRARY is set and SDL_VULKAN_LIBRARY is not, the test sets
 * SDL_VULKAN_LIBRARY to it so both use the same library.
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv */
#endif
#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(DE_VK_TEST_SDL)
#include <SDL3/SDL.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_SWAPCHAIN_ASAN 1
#endif
#endif
#if defined(__APPLE__) && defined(TEST_SWAPCHAIN_ASAN)
/* As in test_window.c: CoreGraphics' PDF lexer calls strndup() in a way strict_string_checks=1
 * reports although it is valid. Engine and SDL code stay checked. */
const char *__asan_default_suppressions(void);
const char *__asan_default_suppressions(void) {
    return "interceptor_via_lib:CoreGraphics\n";
}
#endif

static const char *errchain(char *buf, size_t cap) {
    size_t used = 0;
    buf[0] = '\0';
    for (const de_error *e = de_error_last(); e && used + 1 < cap; e = e->cause) {
        int w = snprintf(buf + used, cap - used, "%s%s", used ? " <- " : "", e->message);
        if (w < 0)
            break;
        used += (size_t)w < cap - used ? (size_t)w : cap - used - 1;
    }
    return buf;
}

static void share_loader_with_sdl(void) {
    const char *lib = getenv("DE_VULKAN_LIBRARY");
    if (!lib || !lib[0] || getenv("SDL_VULKAN_LIBRARY"))
        return;
#if defined(_WIN32)
    _putenv_s("SDL_VULKAN_LIBRARY", lib);
#else
    setenv("SDL_VULKAN_LIBRARY", lib, 0);
#endif
}

typedef struct sc_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    bool platform;
    de_window window;
    de_gpu_device *d;
    de_gpu_swapchain sc;
    char skip[1200];
    char err[1100];
} sc_fx;

static void sc_fx_setup(sc_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
    share_loader_with_sdl();
    de_platform_desc pd = {0};
    pd.app_name = "dengine test_vk_swapchain";
    pd.app_org = "DEngine";
    de_result r = de_platform_init(&pd);
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("no video driver (no display)");
    ASSERT_EQ(r, DE_OK);
    f->platform = true;

    de_window_desc wd = {0};
    wd.title = "test_vk_swapchain";
    wd.width = 320;
    wd.height = 240;
    wd.display_index = -1;
    wd.hidden = true;
    wd.surface = DE_WINDOW_SURFACE_VULKAN;
    r = de_window_create(&wd, &f->window);
    if (r == DE_ERR_UNSUPPORTED) {
        snprintf(f->skip, sizeof f->skip, "no Vulkan window: %s", errchain(f->err, sizeof f->err));
        SKIP(f->skip);
    }
    ASSERT_EQ(r, DE_OK);

    de_gpu_device_desc desc = {.backend = DE_GPU_VULKAN,
                               .debug_layer = true,
                               .gpu_validation = true,
                               .allocator = &f->alloc};
    r = de_gpu_device_create(&desc, &f->d);
    if (r == DE_ERR_UNSUPPORTED) {
        desc.adapter = 1; /* not Base-tier capable (MoltenVK): force it */
        r = de_gpu_device_create(&desc, &f->d);
    }
    if (r == DE_ERR_UNSUPPORTED) {
        snprintf(f->skip, sizeof f->skip, "no usable Vulkan device: %s",
                 errchain(f->err, sizeof f->err));
        SKIP(f->skip);
    }
    ASSERT_EQ(r, DE_OK);
}

static void sc_fx_teardown(sc_fx *f) {
    if (f->d) {
        if (f->sc.id)
            de_gpu_swapchain_destroy(f->d, f->sc);
        u32 warnings = 0;
        EXPECT_EQ(de__gpu_vk_validation_messages(f->d, &warnings), 0u);
        de_gpu_device_destroy(f->d);
        de_tracking_stats st = de_tracking_allocator_stats(&f->tracker);
        EXPECT_EQ(st.blocks_live, 0u);
        EXPECT_EQ(st.bytes_live, 0u);
    }
    if (DE_HANDLE_VALID(f->window))
        de_window_destroy(f->window);
    if (f->platform)
        de_platform_shutdown();
}

/* One frame: acquire, clear to a colour that changes per frame, present. DE_ERR_BUSY (out of date)
 * recreates the swapchain at the window's pixel size and counts as a frame not shown. Returns
 * whether the frame was presented; `*image` gets the image used. */
static bool frame(sc_fx *f, u32 n, de_gpu_texture *image) {
    de_gpu_device *d = f->d;
    de_gpu_texture img = {0};
    de_result r = de_gpu_swapchain_acquire(d, f->sc, &img);
    if (r == DE_ERR_BUSY) {
        u32 w = 0, h = 0;
        de_window_size_pixels(f->window, &w, &h);
        EXPECT_EQ(de_gpu_swapchain_resize(d, f->sc, w, h), DE_OK);
        return false;
    }
    EXPECT_EQ(r, DE_OK);
    if (r != DE_OK)
        return false;
    *image = img;
    de_gpu_cmd *c = de_gpu_cmd_begin(d, DE_QUEUE_GRAPHICS, "present frame");
    EXPECT_TRUE(c != NULL);
    if (!c)
        return false;
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.texture = img;
    b.before = DE_ACCESS_NONE; /* contents discarded */
    b.after = DE_ACCESS_RENDER_TARGET;
    de_gpu_cmd_barriers(c, &b, 1);
    const f32 t = (f32)(n % 60u) / 60.0f;
    de_gpu_attachment color = {
        .texture = img, .clear = true, .clear_color = {t, 0.25f, 1.0f - t, 1.0f}, .store = true};
    de_gpu_cmd_begin_render(c, &color, 1, NULL);
    de_gpu_cmd_end_render(c);
    b.before = DE_ACCESS_RENDER_TARGET;
    b.after = DE_ACCESS_PRESENT;
    de_gpu_cmd_barriers(c, &b, 1);
    EXPECT_EQ(
        de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &(de_gpu_submit){.cmds = &c, .cmd_count = 1}),
        DE_OK);
    r = de_gpu_swapchain_present(d, f->sc);
    if (r == DE_ERR_BUSY) {
        u32 w = 0, h = 0;
        de_window_size_pixels(f->window, &w, &h);
        EXPECT_EQ(de_gpu_swapchain_resize(d, f->sc, w, h), DE_OK);
        return true; /* presented, but the next one needs the new size */
    }
    EXPECT_EQ(r, DE_OK);
    de_platform_pump(NULL, 0);
    return r == DE_OK;
}

static void image_size(de_gpu_device *d, de_gpu_texture img, u32 *w, u32 *h) {
    de__gpu_texture *t = de__gpu_get_texture(d, img);
    *w = t ? t->width : 0u;
    *h = t ? t->height : 0u;
}

TEST_F(vk, swapchain_present_resize, sc_fx) {
    f->sc = de_gpu_swapchain_create(f->d, &(de_gpu_swapchain_desc){.window = f->window,
                                                                   .image_count = 3,
                                                                   .present = DE_PRESENT_VSYNC});
    if (!DE_HANDLE_VALID(f->sc) && de_error_last() &&
        de_error_last()->result == DE_ERR_UNSUPPORTED) {
        snprintf(f->skip, sizeof f->skip, "cannot present: %s", errchain(f->err, sizeof f->err));
        SKIP(f->skip);
    }
    ASSERT_TRUE(DE_HANDLE_VALID(f->sc));
    EXPECT_EQ(de_gpu_swapchain_colorspace(f->d, f->sc), DE_CS_SRGB);

    u32 pw = 0, ph = 0;
    de_window_size_pixels(f->window, &pw, &ph);
    u32 presented = 0;
    de_gpu_texture img = {0};
    for (u32 n = 0; n < 120; ++n)
        presented += frame(f, n, &img) ? 1u : 0u;
    printf("120 frames: %u presented at %ux%u (window %ux%u pixels)\n", presented,
           (unsigned)(de__gpu_get_texture(f->d, img) ? de__gpu_get_texture(f->d, img)->width : 0),
           (unsigned)(de__gpu_get_texture(f->d, img) ? de__gpu_get_texture(f->d, img)->height : 0),
           pw, ph);
    EXPECT_GE(presented, 118u); /* at most a couple of out-of-date frames */
    u32 w0 = 0, h0 = 0;
    image_size(f->d, img, &w0, &h0);
    EXPECT_EQ(w0, pw);
    EXPECT_EQ(h0, ph);

    /* Resize the window (the host does this on DE_EV_WINDOW_RESIZED), then the swapchain: the old
     * images are gone, new ones come at the window's new pixel size. */
#if defined(DE_VK_TEST_SDL)
    SDL_Window *sw = (SDL_Window *)de_window_native(f->window).sdl_window;
    ASSERT_TRUE(sw != NULL);
    EXPECT_TRUE(SDL_SetWindowSize(sw, 200, 150));
    SDL_SyncWindow(sw);
    de_platform_pump(NULL, 0);
#endif
    u32 pw2 = 0, ph2 = 0;
    de_window_size_pixels(f->window, &pw2, &ph2);
    const de_gpu_texture old = img;
    ASSERT_EQ(de_gpu_swapchain_resize(f->d, f->sc, pw2, ph2), DE_OK);
    EXPECT_TRUE(de__gpu_get_texture(f->d, old) == NULL);
    de_gpu_texture fresh = {0};
    ASSERT_EQ(de_gpu_swapchain_acquire(f->d, f->sc, &fresh), DE_OK);
    EXPECT_TRUE(fresh.id != old.id);
    u32 w1 = 0, h1 = 0;
    image_size(f->d, fresh, &w1, &h1);
    printf("after resize: window %ux%u pixels, images %ux%u\n", pw2, ph2, w1, h1);
    EXPECT_EQ(w1, pw2);
    EXPECT_EQ(h1, ph2);
#if defined(DE_VK_TEST_SDL)
    EXPECT_TRUE(w1 != pw || h1 != ph); /* the size really changed */
#endif
    /* The acquired image is presented by the first frame below (acquire returns it again). */
    presented = 0;
    for (u32 n = 0; n < 30; ++n)
        presented += frame(f, n, &img) ? 1u : 0u;
    EXPECT_GE(presented, 28u);

    /* The other present modes (each falls back to FIFO when the surface lacks it). */
    static const de_gpu_present k_modes[] = {DE_PRESENT_IMMEDIATE, DE_PRESENT_MAILBOX};
    for (u32 m = 0; m < 2; ++m) {
        de_gpu_swapchain_destroy(f->d, f->sc);
        f->sc = de_gpu_swapchain_create(
            f->d, &(de_gpu_swapchain_desc){.window = f->window, .present = k_modes[m]});
        ASSERT_TRUE(DE_HANDLE_VALID(f->sc));
        presented = 0;
        for (u32 n = 0; n < 10; ++n)
            presented += frame(f, n, &img) ? 1u : 0u;
        EXPECT_GE(presented, 9u);
    }
    de_gpu_wait_idle(f->d);
}
