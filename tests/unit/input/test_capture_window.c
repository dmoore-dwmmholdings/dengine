#include "de_test.h"
#include "input/input_internal.h"
#include "platform/sdl/sdl_internal.h"

typedef struct capture_window_fx {
    bool platform;
    de_input *input;
    de_window first, second;
} capture_window_fx;
static void capture_window_fx_setup(capture_window_fx *f) {
    de_result r = de_platform_init(&(de_platform_desc){.app_name = "DENG-606 capture regression"});
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("native video unavailable");
    ASSERT_EQ(r, DE_OK);
    f->platform = true;
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "1");
    SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
    ASSERT_EQ(de_window_create(
                  &(de_window_desc){.title = "DENG-606 capture test", .width = 400, .height = 240},
                  &f->first),
              DE_OK);
    ASSERT_EQ(de_input_create(NULL, &f->input), DE_OK);
}
static void capture_window_fx_teardown(capture_window_fx *f) {
    if (f->input)
        de_input_destroy(f->input);
    if (f->second.id)
        de_window_destroy(f->second);
    if (f->first.id)
        de_window_destroy(f->first);
    if (f->platform)
        de_platform_shutdown();
}
static void pump(capture_window_fx *f) {
    de_app_event events[32];
    u32 n;
    do {
        n = de_platform_pump(events, 32);
        for (u32 i = 0; i < n; ++i)
            de_input_app_event(f->input, &events[i]);
    } while (n == 32);
    de_input_begin_frame(f->input);
}
static bool focus(capture_window_fx *f, de_window w) {
    SDL_ShowWindow(de__platform_sdl_window_get(w));
    SDL_RaiseWindow(de__platform_sdl_window_get(w));
    const u64 end = de_time_now_ns() + 3000000000ull;
    do {
        pump(f);
        if (de_platform_keyboard_focus().id == w.id)
            return true;
        de_sleep_ns(5000000);
    } while (de_time_now_ns() < end);
    return false;
}
TEST_F(input_capture_window, native_focus_start_loss_return_and_escape, capture_window_fx) {
    ASSERT_EQ(de_input_set_window(f->input, f->first), DE_OK);
    de_mouse_set_relative(f->input, true);
    if (!focus(f, f->first))
        SKIP("OS did not grant keyboard focus");
    pump(f);
    de_mouse_capture_status s = de_mouse_capture_get(f->input);
    ASSERT_TRUE(s.focused);
    ASSERT_TRUE(s.configured);
    EXPECT_FALSE(s.pending);
    SDL_HideWindow(de__platform_sdl_window_get(f->first));
    pump(f);
    s = de_mouse_capture_get(f->input);
    EXPECT_TRUE(s.requested);
    EXPECT_FALSE(s.configured);
    ASSERT_TRUE(focus(f, f->first));
    pump(f);
    EXPECT_TRUE(de_mouse_capture_get(f->input).configured);
    de_mouse_set_relative(f->input, false);
    SDL_HideWindow(de__platform_sdl_window_get(f->first));
    pump(f);
    ASSERT_TRUE(focus(f, f->first));
    pump(f);
    EXPECT_FALSE(de_mouse_capture_get(f->input).configured);
    EXPECT_FALSE(de_mouse_relative(f->input));
}
TEST_F(input_capture_window, native_replacement_window_and_text_handoff, capture_window_fx) {
    ASSERT_TRUE(focus(f, f->first));
    de_mouse_set_relative(f->input, true);
    pump(f);
    ASSERT_EQ(de_input_set_window(f->input, f->first), DE_OK);
    de_mouse_set_relative(f->input, false);
    ASSERT_EQ(de_text_input_start(f->input, NULL), DE_OK);
    EXPECT_TRUE(f->input->text_active);
    ASSERT_EQ(de_window_create(
                  &(de_window_desc){.title = "DENG-606 replacement", .width = 420, .height = 260},
                  &f->second),
              DE_OK);
    ASSERT_EQ(de_input_set_window(f->input, f->second), DE_OK);
    EXPECT_FALSE(f->input->text_active);
    ASSERT_TRUE(focus(f, f->second));
    de_mouse_set_relative(f->input, true);
    pump(f);
    EXPECT_FALSE(de_platform_mouse_relative_get(f->first));
    EXPECT_TRUE(de_mouse_capture_get(f->input).configured);
    EXPECT_EQ(de_mouse_capture_get(f->input).window.id, f->second.id);
}
