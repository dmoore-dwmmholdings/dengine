/* Tests for platform init, windows, displays and the event pump (DENG-245).
 *
 * Each test starts the platform with video. When no video driver can start (a CI box without a
 * display server), de_platform_init() returns DE_ERR_UNSUPPORTED and the test is skipped. A
 * DE_HEADLESS build has no SDL: there the tests check the stubs instead. */
#include "de_test.h"

#include <dengine/de_platform.h>

#include "../core/test_counting_alloc.h"

#include <stdio.h>
#include <string.h>

#if !defined(DE_HEADLESS)
#include "platform/sdl/sdl_internal.h"
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_WINDOW_ASAN 1
#endif
#endif
#if defined(__APPLE__) && defined(TEST_WINDOW_ASAN)
/* macOS 26: the first display cycle of an NSWindow lays out AppKit's SwiftUI titlebar views,
 * which load vector images through CoreGraphics' PDF lexer. That calls strndup() on a buffer
 * with no NUL inside the bound, which is valid C, but ASan's strict_string_checks=1 (the CI
 * sanitizer presets) reports it as a heap-buffer-overflow. Suppress only interceptor reports
 * whose caller is in CoreGraphics; engine and SDL code are still checked. */
const char *__asan_default_suppressions(void);
const char *__asan_default_suppressions(void) {
    return "interceptor_via_lib:CoreGraphics\n";
}
#endif

typedef struct plat_fx {
    test_counting_alloc alloc;
    bool up;
} plat_fx;

static void plat_fx_setup(plat_fx *f) {
    test_counting_init(&f->alloc);
    de_platform_desc d = {0};
    d.app_name = "dengine test_window";
    d.app_org = "DEngine";
    d.allocator = &f->alloc.iface;
    de_result r = de_platform_init(&d);
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("no video driver");
    ASSERT_EQ(r, DE_OK);
    f->up = true;
    if (de_platform_headless())
        SKIP("headless build: no windows");
}

static void plat_fx_teardown(plat_fx *f) {
    if (!f->up)
        return;
    de_platform_shutdown();
    /* Every block the platform took from its allocator is back. */
    EXPECT_EQ(f->alloc.live_blocks, 0u);
    EXPECT_EQ(f->alloc.size_errors, 0u);
}

static de_window open_hidden(u32 w, u32 h, bool high_dpi) {
    de_window_desc d = {0};
    d.title = "test_window";
    d.width = w;
    d.height = h;
    d.display_index = -1;
    d.high_dpi = high_dpi;
    d.hidden = true;
    de_window win = {0};
    de_result r = de_window_create(&d, &win);
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(DE_HANDLE_VALID(win));
    return win;
}

/* ---------- init ---------- */

TEST(window, init_twice_and_headless_flag) {
    de_platform_desc d = {0};
    d.flags = DE_PLATFORM_HEADLESS;
    ASSERT_EQ(de_platform_init(&d), DE_OK);
    EXPECT_TRUE(de_platform_headless());
    EXPECT_EQ(de_platform_init(&d), DE_ERR_BUSY);

    /* Headless: no displays, no windows, but the pump works. */
    de_window w = {0};
    EXPECT_EQ(de_window_create(NULL, &w), DE_ERR_UNSUPPORTED);
    EXPECT_FALSE(DE_HANDLE_VALID(w));
    EXPECT_EQ(de_display_count(), 0u);
    de_display_info info;
    EXPECT_EQ(de_display_info_get(0, &info), DE_ERR_UNSUPPORTED);
    de_app_event ev[4];
    EXPECT_EQ(de_platform_pump(ev, 4), 0u);
    de_platform_shutdown();
    de_platform_shutdown(); /* a second shutdown does nothing */

    EXPECT_TRUE(de_platform_headless()); /* not initialised */
    EXPECT_EQ(de_platform_pump(ev, 4), 0u);
}

/* ---------- windows ---------- */

TEST_F(window, create_destroy_hidden, plat_fx) {
    (void)f;
    for (int i = 0; i < 100; i++) {
        de_window w = open_hidden(320, 240, (i & 1) != 0);
        u32 pw = 0, ph = 0;
        de_window_size_pixels(w, &pw, &ph);
        EXPECT_GT(pw, 0u);
        EXPECT_GT(ph, 0u);
        de_window_destroy(w);
        /* Window events of a destroyed window are dropped, not reported with a stale handle. */
        de_app_event ev[16];
        while (de_platform_pump(ev, 16) == 16) {
        }
    }
}

TEST_F(window, stale_handle, plat_fx) {
    (void)f;
    de_window a = open_hidden(320, 240, false);
    de_window_destroy(a);

    /* Every call rejects or ignores the stale handle. */
    u32 pw = 1, ph = 1;
    de_window_size_pixels(a, &pw, &ph);
    EXPECT_EQ(pw, 0u);
    EXPECT_EQ(ph, 0u);
    EXPECT_EQ(de_window_set_mode(a, DE_WINDOW_BORDERLESS), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_window_get_mode(a), DE_WINDOW_WINDOWED);
    de_window_set_title(a, "stale");
    de_display_info info = de_window_display(a);
    EXPECT_EQ(info.width, 0u);
    de_window_destroy(a); /* double destroy is ignored */

    /* The slot is reused with a new generation: the old handle stays stale. */
    de_window b = open_hidden(320, 240, false);
    EXPECT_NE(a.id, b.id);
    de_window_size_pixels(a, &pw, &ph);
    EXPECT_EQ(pw, 0u);
    de_window_size_pixels(b, &pw, &ph);
    EXPECT_GT(pw, 0u);

    /* Zero and made-up handles are rejected too. */
    de_window zero = {0}, junk = {0xFFFFFFFFu};
    EXPECT_EQ(de_window_set_mode(zero, DE_WINDOW_WINDOWED), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_window_set_mode(junk, DE_WINDOW_WINDOWED), DE_ERR_INVALID_ARGUMENT);
    de_window_destroy(junk);
    de_window_destroy(b);
}

TEST_F(window, window_limit, plat_fx) {
    (void)f;
    de_window w[DE_WINDOW_MAX];
    for (u32 i = 0; i < DE_WINDOW_MAX; i++)
        w[i] = open_hidden(64, 64, false);
    de_window extra = {0};
    de_window_desc d = {0};
    d.hidden = true;
    EXPECT_EQ(de_window_create(&d, &extra), DE_ERR_BUSY);
    EXPECT_EQ(de_window_create(&d, NULL), DE_ERR_INVALID_ARGUMENT);
    d.display_index = 1000;
    de_window_destroy(w[0]);
    EXPECT_EQ(de_window_create(&d, &extra), DE_ERR_INVALID_ARGUMENT);
    /* The rest are closed by de_platform_shutdown() in the teardown. */
}

TEST_F(window, size_pixels_dpi, plat_fx) {
    (void)f;
    de_window lo = open_hidden(640, 360, false);
    de_window hi = open_hidden(640, 360, true);
    u32 lw = 0, lh = 0, hw = 0, hh = 0;
    de_window_size_pixels(lo, &lw, &lh);
    de_window_size_pixels(hi, &hw, &hh);
    de_display_info disp = de_window_display(hi);
    printf("  display content_scale %.2f; window 640x360: %ux%u px, high_dpi %ux%u px\n",
           (double)disp.content_scale, lw, lh, hw, hh);
    /* With high_dpi the pixel size is the logical size times the window's pixel density. */
    EXPECT_GE(hw, 640u);
    EXPECT_GE(hh, 360u);
    EXPECT_EQ((u64)hw * 360u, (u64)hh * 640u); /* same aspect: one density for both axes */
#if defined(__APPLE__)
    /* macOS: without high_dpi the OS scales a 1x backbuffer; with it the pixel size is
     * logical × backing scale, which SDL reports as the display content scale. */
    EXPECT_EQ(lw, 640u);
    EXPECT_EQ(lh, 360u);
    u32 want = (u32)(640.0f * disp.content_scale + 0.5f);
    EXPECT_EQ(hw, want);
#endif
    de_window_destroy(lo);
    de_window_destroy(hi);
}

TEST_F(window, set_mode_roundtrip, plat_fx) {
    (void)f;
    de_window w = open_hidden(640, 360, false);
    EXPECT_EQ(de_window_get_mode(w), DE_WINDOW_WINDOWED);
    ASSERT_EQ(de_window_set_mode(w, DE_WINDOW_BORDERLESS), DE_OK);
    EXPECT_EQ(de_window_get_mode(w), DE_WINDOW_BORDERLESS);
    de_result r = de_window_set_mode(w, DE_WINDOW_FULLSCREEN);
    if (r == DE_ERR_NOT_FOUND) {
        /* A video driver without fullscreen modes (offscreen, dummy). */
        EXPECT_EQ(de_window_get_mode(w), DE_WINDOW_BORDERLESS);
    } else {
        ASSERT_EQ(r, DE_OK);
        EXPECT_EQ(de_window_get_mode(w), DE_WINDOW_FULLSCREEN);
    }
    ASSERT_EQ(de_window_set_mode(w, DE_WINDOW_WINDOWED), DE_OK);
    EXPECT_EQ(de_window_get_mode(w), DE_WINDOW_WINDOWED);
    EXPECT_EQ(de_window_set_mode(w, (de_window_mode)7), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_window_get_mode(w), DE_WINDOW_WINDOWED);
    de_window_set_title(w, "renamed \xC3\xA9");
    de_window_destroy(w);

    /* A window created in a fullscreen mode reports it. */
    de_window_desc d = {0};
    d.hidden = true;
    d.display_index = -1;
    d.mode = DE_WINDOW_BORDERLESS;
    ASSERT_EQ(de_window_create(&d, &w), DE_OK);
    EXPECT_EQ(de_window_get_mode(w), DE_WINDOW_BORDERLESS);
    de_window_destroy(w);
}

/* ---------- displays ---------- */

TEST_F(window, display_enumeration, plat_fx) {
    (void)f;
    u32 n = de_display_count();
    ASSERT_GE(n, 1u);
    u32 primaries = 0;
    for (u32 i = 0; i < n; i++) {
        de_display_info d;
        ASSERT_EQ(de_display_info_get(i, &d), DE_OK);
        printf("  display %u \"%s\": %ux%u @ %u Hz, scale %.2f, primary %d, hdr %d, SDR white "
               "%.0f nits, max %.0f nits, headroom %.2f\n",
               i, d.name, d.width, d.height, d.refresh_hz, (double)d.content_scale, d.primary,
               d.hdr_capable, (double)d.sdr_white_nits, (double)d.max_nits, (double)d.hdr_headroom);
        EXPECT_GT(d.width, 0u);
        EXPECT_GT(d.height, 0u);
        EXPECT_LE(d.refresh_hz, 1000u);
        EXPECT_TRUE(d.content_scale > 0.0f && d.content_scale <= 8.0f);
        EXPECT_TRUE(d.sdr_white_nits >= 80.0f);
        if (!d.hdr_capable)
            EXPECT_FLOAT_EQ(d.hdr_headroom, 1.0, 1e-6);
        primaries += d.primary;

        u32 count = de_display_modes(i, NULL, 0);
        de_display_mode modes[256];
        u32 got = de_display_modes(i, modes, 256);
        EXPECT_EQ(got, count);
        for (u32 m = 0; m < got && m < 256; m++) {
            EXPECT_GT(modes[m].width, 0u);
            EXPECT_GT(modes[m].height, 0u);
            EXPECT_TRUE(modes[m].pixel_density >= 1.0f);
        }
        if (got > 0)
            printf("  display %u: %u modes, best %ux%u @ %.2f Hz (density %.1f)\n", i, got,
                   modes[0].width, modes[0].height, (double)modes[0].refresh_hz,
                   (double)modes[0].pixel_density);
    }
    EXPECT_EQ(primaries, 1u);

    de_display_info bad;
    EXPECT_EQ(de_display_info_get(n, &bad), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_display_info_get(0, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_display_modes(n, NULL, 0), 0u);

    /* A window's display carries the window's SDR white level and HDR headroom. */
    de_window w = open_hidden(320, 240, true);
    de_display_info wd = de_window_display(w);
    printf("  window display \"%s\": hdr %d, SDR white %.0f nits, max %.0f nits, headroom %.2f\n",
           wd.name, wd.hdr_capable, (double)wd.sdr_white_nits, (double)wd.max_nits,
           (double)wd.hdr_headroom);
    EXPECT_GT(wd.width, 0u);
    EXPECT_TRUE(wd.hdr_headroom >= 1.0f);
    EXPECT_FLOAT_EQ(wd.max_nits, (double)(wd.sdr_white_nits * wd.hdr_headroom), 1e-3);
    de_window_destroy(w);
}

/* ---------- event pump ---------- */

#if !defined(DE_HEADLESS)

/* Pumps until the queue is quiet, then empties the input ring: startup produces display,
 * focus and device-added (keyboard, mouse) events. */
static void drain(void) {
    de_app_event ev[64];
    for (int i = 0; i < 100 && de_platform_pump(ev, 64) != 0; i++) {
    }
    SDL_Event in[64];
    while (de__platform_sdl_input_drain(in, 64) != 0) {
    }
}

TEST_F(window, pump_quit_event, plat_fx) {
    (void)f;
    drain();

    /* Nothing queued: returns 0 at once, every time. */
    de_app_event ev[8];
    u64 t0 = de_time_now_ns();
    for (int i = 0; i < 10; i++)
        EXPECT_EQ(de_platform_pump(ev, 8), 0u);
    u64 dt = de_time_now_ns() - t0;
    EXPECT_LT(dt, 500u * 1000u * 1000u); /* never blocks (10 pumps well under 0.5 s) */

    SDL_Event q;
    SDL_zero(q);
    q.type = SDL_EVENT_QUIT;
    ASSERT_TRUE(SDL_PushEvent(&q));
    u32 n = de_platform_pump(ev, 8);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(ev[0].kind, DE_EV_QUIT);
    EXPECT_FALSE(DE_HANDLE_VALID(ev[0].window));
    EXPECT_EQ(de_platform_pump(ev, 8), 0u);
}

TEST_F(window, pump_window_and_app_events, plat_fx) {
    (void)f;
    de_window w = open_hidden(320, 240, false);
    drain();
    /* DENG-246 installs its raw-input translator as the input hook at init; remove it so this
     * test sees the SDL_Event ring underneath (test_devices.c covers the translator). */
    de__platform_sdl_set_input_hook(NULL, NULL);
    SDL_Window *sw = de__platform_sdl_window_get(w);
    ASSERT_TRUE(sw != NULL);
    SDL_WindowID id = SDL_GetWindowID(sw);

    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.windowID = id;
    e.drop.data = "/tmp/dropped file.txt";
    ASSERT_TRUE(SDL_PushEvent(&e));
    SDL_zero(e);
    e.type = SDL_EVENT_LOW_MEMORY;
    ASSERT_TRUE(SDL_PushEvent(&e));
    SDL_zero(e);
    e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    e.window.windowID = id;
    ASSERT_TRUE(SDL_PushEvent(&e));
    /* An input event goes to the input ring, not to the app events. */
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = id;
    e.key.scancode = SDL_SCANCODE_A;
    ASSERT_TRUE(SDL_PushEvent(&e));

    de_app_event ev[8];
    u32 n = de_platform_pump(ev, 8);
    ASSERT_EQ(n, 3u);
    EXPECT_EQ(ev[0].kind, DE_EV_DROP_FILE);
    EXPECT_EQ(ev[0].window.id, w.id);
    EXPECT_STREQ(ev[0].path, "/tmp/dropped file.txt");
    EXPECT_EQ(ev[1].kind, DE_EV_LOW_MEMORY);
    EXPECT_EQ(ev[2].kind, DE_EV_WINDOW_CLOSE);
    EXPECT_EQ(ev[2].window.id, w.id);

    SDL_Event in[4];
    u32 ni = de__platform_sdl_input_drain(in, 4);
    ASSERT_EQ(ni, 1u);
    EXPECT_EQ(in[0].type, (Uint32)SDL_EVENT_KEY_DOWN);
    EXPECT_EQ(in[0].key.scancode, SDL_SCANCODE_A);
    EXPECT_EQ(de__platform_sdl_input_drain(in, 4), 0u);

    /* A full `events` array leaves the rest queued, in order. */
    for (int i = 0; i < 3; i++) {
        SDL_zero(e);
        e.type = SDL_EVENT_LOW_MEMORY;
        ASSERT_TRUE(SDL_PushEvent(&e));
    }
    SDL_zero(e);
    e.type = SDL_EVENT_QUIT;
    ASSERT_TRUE(SDL_PushEvent(&e));
    EXPECT_EQ(de_platform_pump(ev, 2), 2u);
    EXPECT_EQ(de_platform_pump(ev, 8), 2u);
    EXPECT_EQ(ev[0].kind, DE_EV_LOW_MEMORY);
    EXPECT_EQ(ev[1].kind, DE_EV_QUIT);
    de_window_destroy(w);
}

static u32 g_hook_calls;
static void count_hook(const SDL_Event *ev, void *user) {
    (void)ev;
    *(u32 *)user += 1;
}

TEST_F(window, input_hook_and_ring_overflow, plat_fx) {
    (void)f;
    drain();
    g_hook_calls = 0;
    de__platform_sdl_set_input_hook(count_hook, &g_hook_calls);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    ASSERT_TRUE(SDL_PushEvent(&e));
    EXPECT_EQ(de_platform_pump(NULL, 0), 0u);
    EXPECT_EQ(g_hook_calls, 1u);
    de__platform_sdl_set_input_hook(NULL, NULL);

    /* Without a hook the ring takes DE__PLATFORM_SDL_INPUT_RING events, then counts drops. */
    u32 dropped0 = de__platform_sdl_input_dropped();
    for (u32 i = 0; i < DE__PLATFORM_SDL_INPUT_RING + 10; i++)
        de__platform_sdl_route_input(&e);
    EXPECT_EQ(de__platform_sdl_input_dropped() - dropped0, 10u);
    SDL_Event out[64];
    u32 total = 0, got;
    while ((got = de__platform_sdl_input_drain(out, 64)) != 0)
        total += got;
    EXPECT_EQ(total, (u32)DE__PLATFORM_SDL_INPUT_RING);
}

#else /* DE_HEADLESS */

TEST(window, headless_build_stubs) {
    ASSERT_EQ(de_platform_init(NULL), DE_OK);
    EXPECT_TRUE(de_platform_headless());
    de_window w = {0};
    EXPECT_EQ(de_window_create(NULL, &w), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_window_set_mode(w, DE_WINDOW_WINDOWED), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_display_count(), 0u);
    EXPECT_EQ(de_display_modes(0, NULL, 0), 0u);
    de_app_event ev[2];
    EXPECT_EQ(de_platform_pump(ev, 2), 0u);
    de_platform_shutdown();
}

#endif
