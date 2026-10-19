#include "audio_test_util.h"
#include "audio/backend/coreaudio_internal.h"
#include <math.h>
#include <stdio.h>
static void *backend(void) {
    de__audio_backend_desc d = {0};
    d.allocator = de_heap_allocator();
    void *b = NULL;
    ASSERT_EQ(de__audio_backend_coreaudio.create(&d, &b), DE_OK);
    return b;
}
TEST(audio_coreaudio, channel_layouts) {
    EXPECT_EQ(de__ca_layout(2), kAudioChannelLayoutTag_Stereo);
    EXPECT_EQ(de__ca_layout(6), kAudioChannelLayoutTag_MPEG_5_1_A);
    EXPECT_EQ(de__ca_layout(8), kAudioChannelLayoutTag_MPEG_7_1_C);
    EXPECT_EQ(de__ca_layout(4), kAudioChannelLayoutTag_Unknown | 4u);
}
static void note(void *user, u32 events) {
    *(u32 *)user |= events;
}
TEST(audio_coreaudio, listener_only_posts_control_events) {
    de__coreaudio b = {0};
    u32 events = 0;
    b.notify = note;
    b.notify_user = &events;
    de_atomic_init_u32(&b.closing, 0);
    de_atomic_init_u32(&b.listeners_active, 0);
    AudioObjectPropertyAddress addresses[] = {
        de__ca_address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal),
        de__ca_address(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal),
        de__ca_address(kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal)};
    ASSERT_EQ(de__ca_listener(kAudioObjectSystemObject, 3, addresses, &b), noErr);
    EXPECT_EQ(events, DE__AUDIO_EV_DEVICES_CHANGED | DE__AUDIO_EV_DEFAULT_CHANGED |
                          DE__AUDIO_EV_STREAM_LOST);
    EXPECT_EQ(de_atomic_load_u32(&b.listeners_active, DE_ATOMIC_RELAXED), 0u);
    de_atomic_store_u32(&b.closing, 1, DE_ATOMIC_RELEASE);
    events = 0;
    de__ca_listener(kAudioObjectSystemObject, 3, addresses, &b);
    EXPECT_EQ(events, 0u);
}
static void fill(void *user, f32 *out, u32 frames) {
    *(u32 *)user += frames;
    for (u32 i = 0; i < frames * 2; ++i)
        out[i] = (f32)i / 1024;
}
TEST(audio_coreaudio, callback_is_interleaved_allocation_and_lock_free) {
    de_audio *a = test_audio_manual(2, 0); /* installs actual RT allocation and lock hooks */
    de__coreaudio_stream s = {0};
    u32 rendered = 0;
    s.desc = (de__audio_stream_desc){48000, 2, 256, fill, &rendered, NULL};
    de_atomic_init_u64(&s.callbacks, 0);
    de_atomic_init_u64(&s.frames, 0);
    f32 out[512];
    AudioBufferList buffers = {0};
    buffers.mNumberBuffers = 1;
    buffers.mBuffers[0] = (AudioBuffer){2, sizeof out, out};
    u64 hits = de__rt_guard_hits();
    ASSERT_EQ(de__ca_render(&s, NULL, NULL, 0, 256, &buffers), noErr);
    EXPECT_EQ(rendered, 256u);
    EXPECT_EQ(de_atomic_load_u64(&s.callbacks, DE_ATOMIC_RELAXED), 1u);
    EXPECT_EQ(de_atomic_load_u64(&s.frames, DE_ATOMIC_RELAXED), 256u);
    for (u32 i = 0; i < 512; ++i)
        EXPECT_FLOAT_EQ(out[i], (f32)i / 1024, 0);
    buffers.mBuffers[0].mNumberChannels = 1; /* unexpected OS format is safely silenced */
    de__ca_render(&s, NULL, NULL, 0, 256, &buffers);
    for (u32 i = 0; i < 512; ++i)
        EXPECT_FLOAT_EQ(out[i], 0, 0);
    EXPECT_EQ(de__rt_guard_hits(), hits);
    de_audio_destroy(a);
}
TEST(audio_coreaudio, enumerate_output_names_and_uids) {
    void *b = backend();
    de_audio_device_info devices[32];
    u32 count = de__audio_backend_coreaudio.enumerate(b, devices, 32);
    EXPECT_EQ(de__audio_backend_coreaudio.enumerate(b, NULL, 0), count);
    for (u32 i = 0; i < count && i < 32; ++i) {
        printf("Core Audio: %s UID=%s channels=%u default=%u\n", devices[i].name, devices[i].id,
               devices[i].channels, devices[i].is_default);
        EXPECT_TRUE(devices[i].name[0] && devices[i].id[0]);
        EXPECT_TRUE(devices[i].channels > 0);
        EXPECT_TRUE(de__ca_resolve(b, devices[i].id) != kAudioObjectUnknown);
    }
    de__audio_backend_coreaudio.destroy(b);
    if (!count)
        SKIP("No Core Audio output endpoint on this host");
}
static de_audio *native(void) {
    void *b = backend();
    u32 count = de__audio_backend_coreaudio.enumerate(b, NULL, 0);
    de__audio_backend_coreaudio.destroy(b);
    if (!count)
        SKIP("No Core Audio output endpoint on this host");
    de_audio_desc d = {0};
    d.backend = DE_AUDIO_COREAUDIO;
    d.max_voices = 4;
    de_audio *a = NULL;
    ASSERT_EQ(de_audio_create(&d, &a), DE_OK);
    ASSERT_FALSE(a->dev.stream_is_fallback);
    ASSERT_EQ(a->dev.stream->vt->kind, DE_AUDIO_COREAUDIO);
    return a;
}
typedef struct tone_state {
    u64 frame;
    de_atomic_u64 calls;
} tone_state;
static void tone(f32 *out, u32 frames, u32 channels, void *user) {
    tone_state *s = user;
    for (u32 f = 0; f < frames; ++f) {
        f32 v = (f32)(0.04 * sin(2 * 3.141592653589793 * 440 * s->frame++ / 48000));
        for (u32 c = 0; c < channels; ++c)
            out[f * channels + c] = v;
    }
    de_atomic_fetch_add_u64(&s->calls, 1, DE_ATOMIC_RELAXED);
}
TEST(audio_coreaudio, default_device_one_second_440hz_full_mixer) {
    de_audio *a = native();
    tone_state generator = {0};
    de_atomic_init_u64(&generator.calls, 0);
    de_voice voice = de_audio_play_procedural(a, tone, &generator, 2, (de_bus){0});
    ASSERT_TRUE(voice.id != 0);
    u64 hits = de__rt_guard_hits(), clock = de_audio_clock(a);
    u32 underruns = de_audio_get_stats(a).underruns;
    de_sleep_ns(1000000000ull);
    de_mutex_lock(&a->dev.lock);
    de__coreaudio_stream *s = (de__coreaudio_stream *)a->dev.stream;
    u64 callbacks = de_atomic_load_u64(&s->callbacks, DE_ATOMIC_RELAXED);
    u64 frames = de_atomic_load_u64(&s->frames, DE_ATOMIC_RELAXED);
    de_mutex_unlock(&a->dev.lock);
    printf("Native 440Hz: callbacks=%llu frames=%llu underruns=%u\n", (unsigned long long)callbacks,
           (unsigned long long)frames, de_audio_get_stats(a).underruns - underruns);
    EXPECT_TRUE(callbacks >= 40 && callbacks <= 1000);
    EXPECT_TRUE(de_audio_clock(a) - clock >= 43200u);
    EXPECT_TRUE(de_audio_clock(a) - clock <= 57600u);
    EXPECT_TRUE(frames >= 43200u);
    EXPECT_TRUE(de_atomic_load_u64(&generator.calls, DE_ATOMIC_RELAXED) >= 150u);
    EXPECT_EQ(de_audio_get_stats(a).underruns, underruns);
    EXPECT_EQ(de__rt_guard_hits(), hits);
    de_audio_destroy(a);
}
TEST(audio_coreaudio, uid_switch_and_listener_reopen_monotonic_within_500ms) {
    de_audio *a = native();
    de_audio_device_info devices[16];
    u32 count = de_audio_list_devices(a, devices, 16);
    ASSERT_TRUE(count > 0);
    u64 clock = de_audio_clock(a);
    ASSERT_EQ(de_audio_set_device(a, devices[0].id), DE_OK);
    EXPECT_TRUE(de_audio_clock(a) >= clock);
    ASSERT_FALSE(a->dev.stream_is_fallback);
    if (count >= 2)
        ASSERT_EQ(de_audio_set_device(a, devices[1].id), DE_OK);
    ASSERT_EQ(de_audio_set_device(a, NULL), DE_OK);
    u32 opens = de__audio_device_opens(a);
    AudioObjectPropertyAddress p =
        de__ca_address(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
    u64 start = de_time_now_ns();
    de__ca_listener(kAudioObjectSystemObject, 1, &p, a->dev.backend);
    while (de__audio_device_opens(a) == opens && de_time_now_ns() - start < 500000000ull) {
        EXPECT_TRUE(de_audio_clock(a) >= clock);
        clock = de_audio_clock(a);
        de_sleep_ns(1000000);
    }
    EXPECT_TRUE(de__audio_device_opens(a) > opens);
    EXPECT_TRUE(de_time_now_ns() - start < 500000000ull);
    EXPECT_FALSE(a->dev.stream_is_fallback);
    /* Fault-injected device-alive listener exercises the same control-thread recovery path. */
    opens = de__audio_device_opens(a);
    p.mSelector = kAudioDevicePropertyDeviceIsAlive;
    start = de_time_now_ns();
    de__ca_listener(kAudioObjectSystemObject, 1, &p, a->dev.backend);
    while (de__audio_device_opens(a) == opens && de_time_now_ns() - start < 500000000ull)
        de_sleep_ns(1000000);
    EXPECT_TRUE(de__audio_device_opens(a) > opens);
    EXPECT_TRUE(de_audio_clock(a) >= clock);
    de_audio_destroy(a);
}
