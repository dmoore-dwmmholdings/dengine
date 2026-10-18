#include "audio_test_util.h"
#include "audio/backend/wasapi_format.h"
#include <math.h>
#if defined(_WIN32)
#include "audio/backend/wasapi_internal.h"
#endif
TEST(audio_wasapi, shared_float_format_and_channel_masks) {
    EXPECT_EQ(de__wasapi_mask(2), 3u);
    EXPECT_EQ(de__wasapi_mask(6), 0x3fu);
    EXPECT_EQ(de__wasapi_mask(8), 0x63fu);
    EXPECT_EQ(de__wasapi_mask(5), 0u);
#if defined(_WIN32)
    de__wasapi_format f = {48000, 6, de__wasapi_mask(6), DE__WASAPI_F32};
    WAVEFORMATEXTENSIBLE w;
    de__wasapi_wave(&f, &w);
    EXPECT_EQ(w.Format.nSamplesPerSec, 48000u);
    EXPECT_EQ(w.Format.wBitsPerSample, 32u);
    EXPECT_EQ(w.Format.nBlockAlign, 24u);
    EXPECT_EQ(w.dwChannelMask, 0x3fu);
    EXPECT_TRUE(IsEqualGUID(&w.SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
#endif
}
TEST(audio_wasapi, exclusive_negotiates_float_then_int24_then_int16_at_device_rate) {
    de__wasapi_format f[3];
    de__wasapi_candidates(44100, 8, f);
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_EQ(f[i].rate, 44100u);
        EXPECT_EQ(f[i].channels, 8u);
        EXPECT_EQ(f[i].mask, 0x63fu);
    }
    EXPECT_EQ(f[0].sample, DE__WASAPI_F32);
    EXPECT_EQ(f[1].sample, DE__WASAPI_I24);
    EXPECT_EQ(f[2].sample, DE__WASAPI_I16);
}
TEST(audio_wasapi, utf16_friendly_names_and_invalid_surrogates) {
    const u16 name[] = {'U', 'S', 'B', ' ', 0x1f3, ' ', 0xd83c, 0xdfa7, 0};
    char out[64];
    ASSERT_TRUE(de__wasapi_utf8(name, out, sizeof out));
    EXPECT_STREQ(out, "USB \xc7\xb3 \xf0\x9f\x8e\xa7");
    EXPECT_FALSE(de__wasapi_utf8(name, out, 4));
    const u16 bad[] = {0xd800, 0};
    EXPECT_FALSE(de__wasapi_utf8(bad, out, sizeof out));
}
TEST(audio_wasapi, preallocated_integer_conversion_clips_and_is_rt_safe) {
    de_audio *a = test_audio_manual(2, 0);
    const f32 in[] = {-2, -1, -.5f, 0, .5f, 1, 2, NAN};
    const u8 expected16[] = {0, 128, 0, 128, 0, 192, 0, 0, 0, 64, 255, 127, 255, 127, 0, 0};
    const u8 expected24[] = {0, 0, 128, 0,   0,   128, 0,   0,   192, 0, 0, 0,
                             0, 0, 64,  255, 255, 127, 255, 255, 127, 0, 0, 0};
    u8 out[32];
    u64 hits = de__rt_guard_hits();
    de__rt_guard_enter();
    de__wasapi_convert(in, out, 8, DE__WASAPI_I16);
    de__rt_guard_leave();
    EXPECT_MEMEQ(out, expected16, sizeof expected16);
    de__rt_guard_enter();
    de__wasapi_convert(in, out, 8, DE__WASAPI_I24);
    de__rt_guard_leave();
    EXPECT_MEMEQ(out, expected24, sizeof expected24);
    EXPECT_EQ(de__rt_guard_hits(), hits);
    de_audio_destroy(a);
}
#if defined(_WIN32)
static de_audio *native(void) {
    de_audio_desc d = {0};
    d.backend = DE_AUDIO_WASAPI;
    d.max_voices = 4;
    de_audio *a = NULL;
    de_result result = de_audio_create(&d, &a);
    if (result == DE_ERR_UNSUPPORTED)
        SKIP("Windows audio endpoint service unavailable");
    ASSERT_EQ(result, DE_OK);
    if (!de_audio_list_devices(a, NULL, 0)) {
        de_audio_destroy(a);
        SKIP("No Windows output endpoint");
    }
    EXPECT_FALSE(a->dev.stream_is_fallback);
    return a;
}
#endif
TEST(audio_wasapi, open_default_device_and_reopen_on_invalidated) {
#if defined(_WIN32)
    de_audio *a = native();
    u32 opens = de__audio_device_opens(a);
    u64 clock = de_audio_clock(a), start = de_time_now_ns();
    de__wasapi_post(a->dev.backend, DE__AUDIO_EV_STREAM_LOST);
    while (de__audio_device_opens(a) == opens && de_time_now_ns() - start < 500000000ull)
        de_sleep_ns(1000000);
    EXPECT_TRUE(de__audio_device_opens(a) > opens);
    EXPECT_TRUE(de_audio_clock(a) >= clock);
    EXPECT_FALSE(a->dev.stream_is_fallback);
    de_audio_destroy(a);
#else
    SKIP("WASAPI runtime requires Windows hardware");
#endif
}
TEST(audio_wasapi, exclusive_failure_falls_back_to_real_shared_endpoint) {
#if defined(_WIN32)
    de_cvar_desc d = {0};
    d.name = "audio.wasapi_exclusive";
    de_cvar cv = de_cvar_register(&d);
    ASSERT_EQ(de_cvar_set(cv, "true"), DE_OK);
    de__wasapi_test_fail_exclusive(true);
    de_audio *a = native();
    ASSERT_FALSE(a->dev.stream_is_fallback);
    EXPECT_FALSE(de__wasapi_is_exclusive(a->dev.stream));
    de_audio_destroy(a);
    de__wasapi_test_fail_exclusive(false);
    ASSERT_EQ(de_cvar_set(cv, "false"), DE_OK);
#else
    SKIP("WASAPI exclusive/shared fallback requires Windows hardware");
#endif
}
