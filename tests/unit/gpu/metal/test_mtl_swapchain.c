/* test_mtl_swapchain.c — the CAMetalLayer swapchain on a hidden window (DENG-288). CTest name:
 * gpu_metal.mtl.swapchain_present_resize.
 *
 * Mirrors DENG-284's Vulkan test: 120 frames of acquire / clear / present on a hidden 320x240
 * window with VSYNC and 3 images, then the window is resized to 200x150 (through SDL) and the
 * swapchain with it (new images at the window's new pixel size), then 30 more frames, then 10
 * frames each with IMMEDIATE and MAILBOX swapchains. Metal checks on top: the layer's settings
 * (device, BGRA8 sRGB, drawable count, displaySyncEnabled per mode, drawable size), and one frame
 * read back from the drawable (the clear colour, sRGB-encoded). It SKIPs without a video driver
 * (no display) or a Metal device. Metal API and shader validation are on (test_mtl_device.c,
 * test_mtl_cmd.c).
 */
#include "de_test.h"

#include <dengine/de_gpu.h>
#include <dengine/de_platform.h>

#include "gpu/metal/mtl_internal.h"
#include "mtl_cmd_helpers.h"

#include <stdio.h>
#include <string.h>

#if defined(DE_MTL_TEST_SDL)
#include <SDL3/SDL.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_SWAPCHAIN_ASAN 1
#endif
#endif
#if defined(TEST_SWAPCHAIN_ASAN)
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

typedef struct sc_fx {
    de_tracking_allocator tracker;
    de_allocator alloc;
    bool platform;
    de_window window;
    de_gpu_device *d;
    const de__gpu_backend_vtable *backend;
    de_gpu_swapchain sc;
    char skip[1200];
    char err[1100];
} sc_fx;

static void sc_fx_setup(sc_fx *f) {
    f->alloc = de_tracking_allocator_init(&f->tracker, NULL, DE_MEM_RENDER);
    de_platform_desc pd = {0};
    pd.app_name = "dengine test_mtl_swapchain";
    pd.app_org = "DEngine";
    de_result r = de_platform_init(&pd);
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("no video driver (no display)");
    ASSERT_EQ(r, DE_OK);
    f->platform = true;

    de_window_desc wd = {0};
    wd.title = "test_mtl_swapchain";
    wd.width = 320;
    wd.height = 240;
    wd.display_index = -1;
    wd.hidden = true;
    wd.surface = DE_WINDOW_SURFACE_METAL;
    r = de_window_create(&wd, &f->window);
    if (r == DE_ERR_UNSUPPORTED) {
        snprintf(f->skip, sizeof f->skip, "no Metal window: %s", errchain(f->err, sizeof f->err));
        SKIP(f->skip);
    }
    ASSERT_EQ(r, DE_OK);

    de_gpu_device_desc desc = {.backend = DE_GPU_METAL, .allocator = &f->alloc};
    r = de_gpu_device_create(&desc, &f->d);
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("no Metal device on this machine");
    ASSERT_EQ(r, DE_OK);
    f->backend = f->d->vt;
}

static void sc_fx_teardown(sc_fx *f) {
    if (f->d) {
        if (f->backend)
            f->d->vt = f->backend;
        if (f->sc.id)
            de_gpu_swapchain_destroy(f->d, f->sc);
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

static de_test_mtl_layer layer_info(sc_fx *f) {
    de_test_mtl_layer li;
    de_test_mtl_layer_info(de_window_native(f->window).metal_layer, f->d, &li);
    return li;
}

/* One frame: acquire, clear to a colour that changes per frame, present. With `readback` set,
 * the image is also copied to it before the present (row pitch 256-aligned). Returns whether the
 * frame was presented; `*image` gets the image used. */
static bool frame(sc_fx *f, u32 n, de_gpu_texture *image, de_gpu_buffer readback, u32 pitch,
                  de_gpu_fence fence, u64 *fence_value) {
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
    if (readback.id) {
        b.before = DE_ACCESS_RENDER_TARGET;
        b.after = DE_ACCESS_COPY_SRC;
        de_gpu_cmd_barriers(c, &b, 1);
        de_gpu_cmd_copy_texture_to_buffer(c, readback, 0, pitch, img, 0, 0);
        b.before = DE_ACCESS_COPY_SRC;
    } else {
        b.before = DE_ACCESS_RENDER_TARGET;
    }
    b.after = DE_ACCESS_PRESENT;
    de_gpu_cmd_barriers(c, &b, 1);
    de_gpu_submit s = {.cmds = &c, .cmd_count = 1};
    if (fence.id) {
        s.signal = fence;
        s.signal_value = ++*fence_value;
    }
    EXPECT_EQ(de_gpu_queue_submit(d, DE_QUEUE_GRAPHICS, &s), DE_OK);
    r = de_gpu_swapchain_present(d, f->sc);
    EXPECT_EQ(r, DE_OK);
    de_platform_pump(NULL, 0);
    return r == DE_OK;
}

static bool plain_frame(sc_fx *f, u32 n, de_gpu_texture *image) {
    u64 unused = 0;
    return frame(f, n, image, (de_gpu_buffer){0}, 0, (de_gpu_fence){0}, &unused);
}

static void image_size(de_gpu_device *d, de_gpu_texture img, u32 *w, u32 *h) {
    de__gpu_texture *t = de__gpu_get_texture(d, img);
    *w = t ? t->width : 0u;
    *h = t ? t->height : 0u;
}

TEST_F(mtl, swapchain_present_resize, sc_fx) {
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
    /* Only sRGB until S18, as on Vulkan. */
    de_gpu_swapchain hdr = de_gpu_swapchain_create(
        f->d, &(de_gpu_swapchain_desc){.window = f->window, .colorspace = DE_CS_EDR});
    EXPECT_FALSE(DE_HANDLE_VALID(hdr));

    u32 pw = 0, ph = 0;
    de_window_size_pixels(f->window, &pw, &ph);
    de_test_mtl_layer li = layer_info(f);
    ASSERT_TRUE(li.is_metal_layer);
    EXPECT_TRUE(li.device_set);
    EXPECT_TRUE(li.bgra8_srgb);
    EXPECT_FALSE(li.framebuffer_only);
    EXPECT_TRUE(li.display_sync);
    EXPECT_EQ(li.max_drawables, 3u);
    EXPECT_EQ((u32)li.drawable_w, pw);
    EXPECT_EQ((u32)li.drawable_h, ph);

    /* Frame 0 read back from the drawable: (0, 0.25, 1, 1) in BGRA8 sRGB is (255, 137, 0, 255). */
    const u32 pitch = (pw * 4u + 255u) & ~255u;
    de_gpu_buffer rb = de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = (u64)pitch * ph,
                                                                        .usage = DE_USAGE_COPY_DST,
                                                                        .memory = DE_MEM_READBACK,
                                                                        .name = "frame readback"});
    de_gpu_fence fence = de_gpu_fence_create(f->d, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(rb) && DE_HANDLE_VALID(fence));
    u64 fv = 0;
    de_gpu_texture img = {0};
    u32 presented = frame(f, 0, &img, rb, pitch, fence, &fv) ? 1u : 0u;
    ASSERT_EQ(de_gpu_fence_wait(f->d, fence, fv, (u64)60 * 1000 * 1000 * 1000), DE_OK);
    const u8 *px = (const u8 *)de_gpu_buffer_map(f->d, rb);
    ASSERT_TRUE(px != NULL);
    u32 bad = 0;
    for (u32 y = 0; y < ph; ++y)
        for (u32 x = 0; x < pw; ++x) {
            const u8 *p = px + (size_t)y * pitch + (size_t)x * 4;
            bad += !(p[0] == 255 && (p[1] == 137 || p[1] == 136) && p[2] == 0 && p[3] == 255);
        }
    if (bad)
        printf("frame 0: %u texels differ; texel 0 = %u %u %u %u\n", bad, px[0], px[1], px[2],
               px[3]);
    EXPECT_EQ(bad, 0u);
    de_gpu_buffer_destroy(f->d, rb);
    de_gpu_fence_destroy(f->d, fence);

    for (u32 n = 1; n < 120; ++n)
        presented += plain_frame(f, n, &img) ? 1u : 0u;
    printf("120 frames: %u presented (window %ux%u pixels)\n", presented, pw, ph);
    EXPECT_GE(presented, 118u);

    /* Resize the window (the host does this on DE_EV_WINDOW_RESIZED), then the swapchain: the old
     * images are gone, new ones come at the window's new pixel size. */
#if defined(DE_MTL_TEST_SDL)
    SDL_Window *sw = (SDL_Window *)de_window_native(f->window).sdl_window;
    ASSERT_TRUE(sw != NULL);
    EXPECT_TRUE(SDL_SetWindowSize(sw, 200, 150));
    SDL_SyncWindow(sw);
    de_platform_pump(NULL, 0);
#endif
    u32 pw2 = 0, ph2 = 0;
    de_window_size_pixels(f->window, &pw2, &ph2);
    /* An image acquired before the resize is dropped by it. */
    de_gpu_texture before = {0};
    ASSERT_EQ(de_gpu_swapchain_acquire(f->d, f->sc, &before), DE_OK);
    ASSERT_EQ(de_gpu_swapchain_resize(f->d, f->sc, pw2, ph2), DE_OK);
    EXPECT_TRUE(de__gpu_get_texture(f->d, before) == NULL);
    EXPECT_TRUE(de__gpu_get_texture(f->d, img) == NULL);
    de_gpu_texture fresh = {0};
    ASSERT_EQ(de_gpu_swapchain_acquire(f->d, f->sc, &fresh), DE_OK);
    EXPECT_TRUE(fresh.id != before.id);
    /* Until it is presented, acquire returns the same image. */
    de_gpu_texture again = {0};
    ASSERT_EQ(de_gpu_swapchain_acquire(f->d, f->sc, &again), DE_OK);
    EXPECT_EQ(again.id, fresh.id);
    u32 w1 = 0, h1 = 0;
    image_size(f->d, fresh, &w1, &h1);
    printf("after resize: window %ux%u pixels, images %ux%u\n", pw2, ph2, w1, h1);
    EXPECT_EQ(w1, pw2);
    EXPECT_EQ(h1, ph2);
    li = layer_info(f);
    EXPECT_EQ((u32)li.drawable_w, pw2);
    EXPECT_EQ((u32)li.drawable_h, ph2);
#if defined(DE_MTL_TEST_SDL)
    EXPECT_TRUE(w1 != pw || h1 != ph); /* the size really changed */
#endif
    /* The acquired image is presented by the first frame below (acquire returns it again). */
    presented = 0;
    for (u32 n = 0; n < 30; ++n)
        presented += plain_frame(f, n, &img) ? 1u : 0u;
    EXPECT_GE(presented, 28u);

    /* The other present modes: IMMEDIATE turns display sync off; Metal has no MAILBOX (VSYNC). */
    static const de_gpu_present k_modes[] = {DE_PRESENT_IMMEDIATE, DE_PRESENT_MAILBOX};
    for (u32 m = 0; m < 2; ++m) {
        de_gpu_swapchain_destroy(f->d, f->sc);
        f->sc = de_gpu_swapchain_create(
            f->d, &(de_gpu_swapchain_desc){.window = f->window, .present = k_modes[m]});
        ASSERT_TRUE(DE_HANDLE_VALID(f->sc));
        li = layer_info(f);
        EXPECT_EQ(li.display_sync, k_modes[m] != DE_PRESENT_IMMEDIATE);
        EXPECT_EQ(li.max_drawables, 3u); /* image_count 0: the default */
        presented = 0;
        for (u32 n = 0; n < 10; ++n)
            presented += plain_frame(f, n, &img) ? 1u : 0u;
        EXPECT_GE(presented, 9u);
    }

    /* Present without an acquired image is an error; a zero-size swapchain is busy. */
    EXPECT_EQ(de_gpu_swapchain_present(f->d, f->sc), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_gpu_swapchain_resize(f->d, f->sc, 0, 0), DE_OK);
    de_gpu_texture none = {0};
    EXPECT_EQ(de_gpu_swapchain_acquire(f->d, f->sc, &none), DE_ERR_BUSY);
    de_gpu_wait_idle(f->d);
}

static const de__gpu_backend_vtable *acquire_backend;
static de__gpu_backend_vtable acquire_probe;
static bool acquire_called;
static de_result check_collected_acquire(de_gpu_device *d, de__gpu_swapchain *s,
                                         de_gpu_texture *image) {
    acquire_called = true;
    EXPECT_EQ(de__gpu_deferred_count(d), 0u);
    return acquire_backend->swapchain_acquire(d, s, image);
}
TEST_F(mtl, swapchain_collects_completed_before_acquire, sc_fx) {
    f->sc = de_gpu_swapchain_create(f->d, &(de_gpu_swapchain_desc){.window = f->window});
    ASSERT_TRUE(f->sc.id);
    de_gpu_buffer buffer = de_gpu_buffer_create(
        f->d,
        &(de_gpu_buffer_desc){.size = 256, .memory = DE_MEM_UPLOAD, .usage = DE_USAGE_COPY_SRC});
    ASSERT_TRUE(buffer.id);
    de_gpu_buffer_destroy(f->d, buffer);
    ASSERT_GT(de__gpu_deferred_count(f->d), 0u);
    acquire_backend = f->d->vt;
    acquire_probe = *acquire_backend;
    acquire_probe.swapchain_acquire = check_collected_acquire;
    f->d->vt = &acquire_probe;
    acquire_called = false;
    de_gpu_texture image;
    EXPECT_EQ(de_gpu_swapchain_acquire(f->d, f->sc, &image), DE_OK);
    EXPECT_TRUE(acquire_called);
    f->d->vt = acquire_backend;
}

static u64 delay_completed(de_gpu_device *device, de_gpu_queue queue) {
    (void)device;
    (void)queue;
    return 0; /* Emulate delayed CPU observation of otherwise progressing GPU submissions. */
}
TEST_F(mtl, swapchain_present_does_not_pin_drawable_texture_until_collection, sc_fx) {
    f->sc = de_gpu_swapchain_create(
        f->d, &(de_gpu_swapchain_desc){
                  .window = f->window, .image_count = 3, .present = DE_PRESENT_IMMEDIATE});
    ASSERT_TRUE(f->sc.id);
    acquire_probe = *f->backend;
    acquire_probe.queue_completed = delay_completed;
    f->d->vt = &acquire_probe;
    for (u32 i = 0; i < 8; ++i) {
        de_gpu_texture image;
        ASSERT_TRUE(plain_frame(f, i, &image));
    }
    EXPECT_GT(de__gpu_deferred_count(f->d), 3u);
    f->d->vt = f->backend;
}

typedef struct resize_worker_case {
    sc_fx *f;
    u32 rendered;
} resize_worker_case;
static i32 resize_without_runloop(void *user) {
    resize_worker_case *c = user;
    sc_fx *f = c->f;
    for (u32 i = 0; i < 8; ++i) {
        const u32 width = 64 + i * 8, height = 80 + i * 4;
        de_result r = de_gpu_swapchain_resize(f->d, f->sc, width, height);
        if (r != DE_OK)
            return (i32)r;
        de_gpu_texture image;
        r = de_gpu_swapchain_acquire(f->d, f->sc, &image);
        if (r != DE_OK)
            return (i32)r;
        de__gpu_texture *texture = de__gpu_get_texture(f->d, image);
        if (!texture || texture->width != width || texture->height != height)
            return -1;
        de_gpu_cmd *cmd = de_gpu_cmd_begin(f->d, DE_QUEUE_GRAPHICS, "worker resize");
        if (!cmd)
            return -2;
        de_gpu_barrier b = {.texture = image, .after = DE_ACCESS_RENDER_TARGET};
        de_gpu_cmd_barriers(cmd, &b, 1);
        de_gpu_attachment color = {.texture = image, .clear = true, .store = true};
        de_gpu_cmd_begin_render(cmd, &color, 1, NULL);
        de_gpu_cmd_end_render(cmd);
        b.before = DE_ACCESS_RENDER_TARGET;
        b.after = DE_ACCESS_PRESENT;
        de_gpu_cmd_barriers(cmd, &b, 1);
        r = de_gpu_queue_submit(f->d, DE_QUEUE_GRAPHICS,
                                &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1});
        if (r != DE_OK)
            return (i32)r;
        r = de_gpu_swapchain_present(f->d, f->sc);
        if (r != DE_OK)
            return (i32)r;
        ++c->rendered;
    }
    return 0;
}
TEST_F(mtl, swapchain_resize_render_thread_without_runloop, sc_fx) {
    f->sc = de_gpu_swapchain_create(f->d, &(de_gpu_swapchain_desc){.window = f->window});
    ASSERT_TRUE(f->sc.id);
    resize_worker_case c = {.f = f};
    de_thread *thread;
    ASSERT_EQ(de_thread_create(resize_without_runloop, &c, "swapchain-resize", DE_THREAD_NORMAL, 0,
                               &thread),
              DE_OK);
    EXPECT_EQ(de_thread_join(thread), 0);
    EXPECT_EQ(c.rendered, 8u);
}
