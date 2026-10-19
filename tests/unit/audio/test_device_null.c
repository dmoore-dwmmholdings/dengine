/* test_device_null.c: the null backend in realtime and manual mode, the device list, device
 * switches and simulated hot-plug, and the clock across switches (DENG-308). */
#include "audio_test_util.h"

#include <stdlib.h>

TEST(audio_device, realtime_mode_runs) {
    de_audio *a = test_audio_realtime(8, 0);
    test_source s;
    test_src_init(&s, TSRC_CONST, 0.1f, 2400, 1); /* 50 ms */
    de_voice v = test_play(a, &s, 0, false);
    ASSERT_TRUE(DE_HANDLE_VALID(v));
    /* The null device consumes at the real-time rate: 0.2 s of audio. */
    ASSERT_TRUE(test_wait(a, test_pred_mixed_at_least, 9600, 5000));
    const u64 end = de_time_now_ns() + 5000000000ull;
    while (de__audio_null_frames_played(a) < 9600 && de_time_now_ns() < end)
        de_sleep_ns(2000000ull);
    EXPECT_TRUE(de__audio_null_frames_played(a) >= 9600u);
    EXPECT_TRUE(de_audio_clock(a) > 0u);
    EXPECT_TRUE(de_audio_clock(a) <= test_frames_mixed(a));
    /* The voice ended on the mixer thread; the control thread closes its source. */
    EXPECT_FALSE(de_audio_playing(a, v));
    while (test_src_closes(&s) == 0 && de_time_now_ns() < end)
        de_sleep_ns(2000000ull);
    EXPECT_EQ(test_src_closes(&s), 1u);
    de_audio_destroy(a);
    EXPECT_EQ(test_src_closes(&s), 1u);
}

/* Same commands, two manual systems: identical output. */
static void render_scene(f32 *cap, u32 frames) {
    de_audio *a = test_audio_manual(16, 0);
    test_source s[3];
    test_src_init(&s[0], TSRC_RAMP, 0.001f, 777, 1);
    test_src_init(&s[1], TSRC_CONST, 0.2f, 0, 2);
    test_src_init(&s[2], TSRC_RAMP, 0.0001f, 50, 1);
    test_play(a, &s[0], 10, false);
    de_voice v = test_play(a, &s[1], 1234, false);
    test_play(a, &s[2], 3000, true);
    de__voice_stop_at(a, v, 4321);
    de__audio_null_pump(a, frames, cap);
    de_audio_destroy(a);
}

TEST(audio_device, manual_mode_deterministic) {
    const u32 frames = 8192;
    f32 *c1 = (f32 *)calloc((size_t)frames * 2, sizeof(f32));
    f32 *c2 = (f32 *)calloc((size_t)frames * 2, sizeof(f32));
    ASSERT_TRUE(c1 != NULL && c2 != NULL);
    render_scene(c1, frames);
    render_scene(c2, frames);
    EXPECT_MEMEQ(c1, c2, (size_t)frames * 2 * sizeof(f32));
    EXPECT_FLOAT_EQ(c1[2 * 10], 0.001 * 0.70710678118, 1e-9);
    EXPECT_FLOAT_EQ(c1[2 * 1234 + 1], 0.4, 1e-6);
    free(c1);
    free(c2);
}

TEST(audio_device, list_devices) {
    de_audio *a = test_audio_manual(4, 0);
    de_audio_device_info info[4];
    ASSERT_EQ(de_audio_list_devices(a, info, 4), 1u);
    EXPECT_STREQ(info[0].id, "null");
    EXPECT_TRUE(info[0].is_default);
    EXPECT_EQ(info[0].channels, 2u);
    ASSERT_EQ(de__audio_null_add_device(a, "usb", "USB headset", false), DE_OK);
    EXPECT_EQ(de__audio_null_add_device(a, "usb", "again", false), DE_ERR_ALREADY_EXISTS);
    ASSERT_EQ(de_audio_list_devices(a, info, 1), 2u); /* total, though only one written */
    ASSERT_EQ(de_audio_list_devices(a, NULL, 0), 2u);
    ASSERT_EQ(de_audio_list_devices(a, info, 4), 2u);
    EXPECT_STREQ(info[1].id, "usb");
    EXPECT_STREQ(info[1].name, "USB headset");
    EXPECT_FALSE(info[1].is_default);
    ASSERT_EQ(de__audio_null_set_default(a, "usb"), DE_OK);
    de_audio_list_devices(a, info, 4);
    EXPECT_FALSE(info[0].is_default);
    EXPECT_TRUE(info[1].is_default);
    ASSERT_EQ(de__audio_null_remove_device(a, "usb"), DE_OK);
    ASSERT_EQ(de_audio_list_devices(a, info, 4), 1u);
    EXPECT_TRUE(info[0].is_default); /* the default moved to what is left */
    EXPECT_EQ(de__audio_null_remove_device(a, "usb"), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de__audio_null_set_default(a, "nope"), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_audio_set_device(a, "nope"), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_audio_set_device(a, "null"), DE_OK);
    EXPECT_EQ(de_audio_set_device(a, NULL), DE_OK);
    EXPECT_EQ(de_audio_list_devices(NULL, info, 4), 0u);
    de_audio_destroy(a);
}

/* ---- Device switches ------------------------------------------------------------------------- */

typedef struct clock_watch {
    u64 last_clock;
    u64 decreases;
    u64 samples;
} clock_watch;

static void watch(de_audio *a, clock_watch *w) {
    const u64 c = de_audio_clock(a);
    if (c < w->last_clock)
        ++w->decreases;
    w->last_clock = c;
    ++w->samples;
}

/* Waits until the device manager opened `n` streams in total, watching the clock meanwhile. */
static bool wait_opens(de_audio *a, u32 n, clock_watch *w) {
    const u64 end = de_time_now_ns() + 5000000000ull;
    while (de__audio_device_opens(a) < n) {
        if (de_time_now_ns() > end)
            return false;
        watch(a, w);
        de_sleep_ns(2000000ull);
    }
    return true;
}

/* Watches the clock for `ms` and checks that the mixer kept going. */
static void run_for(de_audio *a, u32 ms, clock_watch *w) {
    const u64 mixed = test_frames_mixed(a);
    const u64 end = de_time_now_ns() + (u64)ms * 1000000ull;
    while (de_time_now_ns() < end) {
        watch(a, w);
        de_sleep_ns(2000000ull);
    }
    EXPECT_TRUE(test_wait(a, test_pred_mixed_at_least, mixed + 1024, 5000));
}

TEST(audio_device, hotplug_keeps_clock_monotonic) {
    de_audio *a = test_audio_realtime(8, 0);
    test_source s;
    test_src_init(&s, TSRC_CONST, 0.1f, 0, 1);
    de_voice v = test_play(a, &s, 0, false);
    clock_watch w = {0, 0, 0};
    u32 opens = de__audio_device_opens(a);
    EXPECT_EQ(opens, 1u);
    run_for(a, 50, &w);

    /* A new default appears: the output follows it. */
    ASSERT_EQ(de__audio_null_add_device(a, "usb", "USB", true), DE_OK);
    ASSERT_TRUE(wait_opens(a, ++opens, &w));
    run_for(a, 50, &w);
    /* It goes away: back to the remaining default. */
    ASSERT_EQ(de__audio_null_remove_device(a, "usb"), DE_OK);
    ASSERT_TRUE(wait_opens(a, ++opens, &w));
    run_for(a, 50, &w);

    /* An explicit device: chosen synchronously. */
    ASSERT_EQ(de__audio_null_add_device(a, "hdmi", "HDMI", false), DE_OK);
    run_for(a, 20, &w); /* not the default: nothing reopens */
    EXPECT_EQ(de__audio_device_opens(a), opens);
    ASSERT_EQ(de_audio_set_device(a, "hdmi"), DE_OK);
    EXPECT_EQ(de__audio_device_opens(a), ++opens);
    EXPECT_STREQ(a->dev.current_id, "hdmi");
    run_for(a, 50, &w);
    /* A default change does not move an explicit choice. */
    ASSERT_EQ(de__audio_null_add_device(a, "usb2", "USB 2", true), DE_OK);
    run_for(a, 30, &w);
    EXPECT_EQ(de__audio_device_opens(a), opens);
    /* Unplugged: falls back to the default; plugged back: returns to it. */
    ASSERT_EQ(de__audio_null_remove_device(a, "hdmi"), DE_OK);
    ASSERT_TRUE(wait_opens(a, ++opens, &w));
    EXPECT_STREQ(a->dev.current_id, "");
    run_for(a, 50, &w);
    ASSERT_EQ(de__audio_null_add_device(a, "hdmi", "HDMI", false), DE_OK);
    ASSERT_TRUE(wait_opens(a, ++opens, &w));
    EXPECT_STREQ(a->dev.current_id, "hdmi");
    run_for(a, 50, &w);
    /* Follow the default again. */
    ASSERT_EQ(de_audio_set_device(a, NULL), DE_OK);
    EXPECT_EQ(de__audio_device_opens(a), ++opens);
    run_for(a, 50, &w);

    EXPECT_EQ(w.decreases, 0u);
    EXPECT_TRUE(w.samples > 50u);
    EXPECT_TRUE(de_audio_playing(a, v)); /* the voice survived every switch */
    EXPECT_TRUE(de_audio_clock(a) > 0u);
    de_audio_destroy(a);
}

static bool pred_underruns_above(de_audio *a, u64 n) {
    return de_audio_get_stats(a).underruns > n;
}

TEST(audio_device, switch_flushes_and_counts_underruns) {
    de_audio *a = test_audio_realtime(8, 0);
    clock_watch w = {0, 0, 0};
    run_for(a, 30, &w);
    /* Stall the mixer, then switch: the flushed ring and the stall give underruns. */
    const u32 u0 = de_audio_get_stats(a).underruns;
    de_atomic_store_u32(&a->test_stall, 1, DE_ATOMIC_RELAXED);
    de_sleep_ns(10000000ull); /* the block being rendered, if any, is finished */
    const u64 clock_before = de_audio_clock(a);
    ASSERT_EQ(de__audio_null_add_device(a, "usb", "USB", false), DE_OK);
    ASSERT_EQ(de_audio_set_device(a, "usb"), DE_OK);
    /* The ring was dropped: the clock jumped to the end of what had been rendered. */
    EXPECT_TRUE(de_audio_clock(a) >= clock_before);
    EXPECT_EQ(de_audio_clock(a), test_frames_mixed(a));
    EXPECT_TRUE(test_wait(a, pred_underruns_above, u0, 5000));
    const u64 clock_stalled = de_audio_clock(a);
    de_sleep_ns(20000000ull);
    EXPECT_EQ(de_audio_clock(a), clock_stalled); /* nothing plays: the clock stands still */
    de_atomic_store_u32(&a->test_stall, 0, DE_ATOMIC_RELAXED);
    run_for(a, 50, &w);
    EXPECT_TRUE(de_audio_clock(a) > clock_stalled);
    EXPECT_EQ(w.decreases, 0u);
    de_audio_destroy(a);
}
