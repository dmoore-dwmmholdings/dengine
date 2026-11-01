#include "de_test.h"
#include "host/splash.h"
#include "host/host_internal.h"
#include <string.h>

TEST(splash, reveal_hold_and_fade_are_bounded) {
    de__splash_sample start = de__splash_at(0, false);
    EXPECT_TRUE(start.alpha == 0.f);
    EXPECT_TRUE(start.text_alpha == 0.f);
    EXPECT_FALSE(start.done);
    de__splash_sample hold = de__splash_at(900000000ull, false);
    EXPECT_TRUE(hold.alpha == 1.f);
    EXPECT_TRUE(hold.reveal == 1.f);
    EXPECT_TRUE(hold.text_alpha == 1.f);
    EXPECT_FALSE(hold.done);
    de__splash_sample end = de__splash_at(DE__SPLASH_DURATION_NS, false);
    EXPECT_TRUE(end.done);
    EXPECT_TRUE(end.alpha == 0.f);
    EXPECT_TRUE(end.background == 0.f);
    f32 previous = 0;
    for (u64 t = 0; t <= 550000000ull; t += 1000000ull) {
        de__splash_sample s = de__splash_at(t, false);
        EXPECT_TRUE(s.alpha >= previous && s.alpha <= 1);
        previous = s.alpha;
        EXPECT_TRUE(s.text_alpha >= 0 && s.text_alpha <= 1);
    }
    previous = 1;
    for (u64 t = 1250000000ull; t <= DE__SPLASH_DURATION_NS; t += 1000000ull) {
        de__splash_sample s = de__splash_at(t, false);
        EXPECT_TRUE(s.alpha <= previous && s.alpha >= 0);
        previous = s.alpha;
    }
    EXPECT_TRUE(de__splash_at(UINT64_MAX, false).done);
}
TEST(splash, still_mode_has_no_reveal_motion) {
    de__splash_sample s = de__splash_at(0, true);
    EXPECT_TRUE(s.alpha == 1.f);
    EXPECT_TRUE(s.reveal == 1.f);
    EXPECT_TRUE(s.settle == 1.f);
    EXPECT_TRUE(s.text_alpha == 1.f);
    EXPECT_FALSE(s.done);
    EXPECT_TRUE(de__splash_at(650000000ull, true).done);
}
TEST(splash, windowless_and_automation_policy) {
    de__host host;
    memset(&host, 0, sizeof host);
    de_engine engine;
    memset(&engine, 0, sizeof engine);
    EXPECT_FALSE(de__splash_enabled(&host));
    host.engine = &engine;
    EXPECT_FALSE(de__splash_enabled(&host));
    engine.window.id = 1;
    EXPECT_TRUE(de__splash_enabled(&host));
    host.desc.skip_splash = true;
    EXPECT_FALSE(de__splash_enabled(&host));
    host.desc.skip_splash = false;
    host.desc.headless = true;
    EXPECT_FALSE(de__splash_enabled(&host));
    host.desc.headless = false;
    host.desc.offscreen = true;
    EXPECT_FALSE(de__splash_enabled(&host));
    host.desc.offscreen = false;
    char *args[] = {"game", "--frames=1", "--splash", "--no-splash"};
    host.desc.argv = args;
    host.desc.argc = 2;
    EXPECT_FALSE(de__splash_enabled(&host));
    host.desc.argc = 3;
    EXPECT_TRUE(de__splash_enabled(&host));
    host.desc.argc = 4;
    EXPECT_FALSE(de__splash_enabled(&host));
    host.desc.argv = NULL;
    host.desc.argc = 0;
    host.loop_max_frames = 1;
    EXPECT_FALSE(de__splash_enabled(&host));
}

TEST(splash, close_only_stops_its_own_window_or_global_quit) {
    de_app_event event = {.kind = DE_EV_WINDOW_CLOSE, .window = {2}};
    EXPECT_FALSE(de__splash_quit_event(&event, (de_window){1}));
    EXPECT_TRUE(de__splash_quit_event(&event, (de_window){2}));
    event.kind = DE_EV_WINDOW_RESIZED;
    EXPECT_FALSE(de__splash_quit_event(&event, (de_window){2}));
    event.kind = DE_EV_QUIT;
    EXPECT_TRUE(de__splash_quit_event(&event, (de_window){1}));
    EXPECT_FALSE(de__splash_quit_event(NULL, (de_window){1}));
}
