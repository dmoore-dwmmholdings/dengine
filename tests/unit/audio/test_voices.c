#include "audio_test_util.h"
#include <math.h>
#define G 0.70710678118f
TEST(audio_voices, fade_in_exact_4800_samples) {
    de_audio *a = test_audio_manual(4, 0);
    test_source source;
    test_src_init(&source, TSRC_CONST, 1, 0, 1);
    de_play_desc d = {0};
    d.fade_in = 0.1f;
    ASSERT_TRUE(de__voice_play_source(a, &source.base, &d).id != 0);
    static f32 out[5120 * 2];
    de__audio_null_pump(a, 5120, out);
    for (u32 i = 0; i < 4800; ++i)
        EXPECT_FLOAT_EQ(out[2 * i], G * (f32)i / 4800, 0.0001);
    EXPECT_FLOAT_EQ(out[9600], G, 1e-6);
    de_audio_destroy(a);
}
TEST(audio_voices, stop_fade_and_hard_stop_click_bound) {
    de_audio *a = test_audio_manual(4, 0);
    test_source source;
    test_src_init(&source, TSRC_CONST, 1, 0, 1);
    de_voice v = test_play(a, &source, 0, false);
    de__audio_null_pump(a, 256, NULL);
    de_audio_stop(a, v, 0.01f);
    f32 out[512 * 2];
    de__audio_null_pump(a, 512, out);
    EXPECT_FLOAT_EQ(out[0], G, 1e-6);
    for (u32 i = 1; i < 480; ++i)
        EXPECT_TRUE(fabsf(out[2 * i] - out[2 * (i - 1)]) < 0.002);
    EXPECT_FLOAT_EQ(out[2 * 480], 0, 0);
    EXPECT_FALSE(de_audio_playing(a, v));
    de_audio_destroy(a);
    a = test_audio_manual(4, 0);
    test_src_init(&source, TSRC_CONST, 1, 0, 1);
    v = test_play(a, &source, 0, false);
    de__audio_null_pump(a, 256, NULL);
    de_audio_stop(a, v, 0);
    de__audio_null_pump(a, 256, out);
    for (u32 i = 1; i < 256; ++i)
        EXPECT_TRUE(fabsf(out[2 * i] - out[2 * (i - 1)]) < 0.004);
    EXPECT_FALSE(de_audio_playing(a, v));
    de_audio_destroy(a);
}
typedef struct sine_source {
    de__audio_source base;
    u64 frame;
    f64 frequency;
} sine_source;
static u32 sine_read(de__audio_source *s, f32 *out, u32 n, u32 channels) {
    sine_source *t = (sine_source *)s;
    for (u32 f = 0; f < n; ++f) {
        f32 v = (f32)sin(2 * 3.141592653589793 * t->frequency * (f64)t->frame++ / s->rate);
        for (u32 c = 0; c < channels; ++c)
            out[f * channels + c] = v;
    }
    return n;
}
static const de__audio_source_vtbl sine_vt = {sine_read, NULL, NULL};
static u32 peak(const f32 *capture, u32 first, u32 bins) {
    f64 maximum = 0;
    u32 found = 0;
    for (u32 k = first; k < first + bins; ++k) {
        f64 re = 0, im = 0;
        for (u32 i = 0; i < 4096; ++i) {
            f64 phase = 2 * 3.141592653589793 * k * i / 4096;
            re += capture[(i + 256) * 2] * cos(phase);
            im += capture[(i + 256) * 2] * sin(phase);
        }
        f64 power = re * re + im * im;
        if (power > maximum) {
            maximum = power;
            found = k;
        }
    }
    return found;
}
TEST(audio_voices, pitch_fft_and_44100_rate_conversion) {
    static f32 capture[4608 * 2];
    for (u32 test = 0; test < 2; ++test) {
        de_audio *a = test_audio_manual(4, 0);
        sine_source source = {0};
        source.base.vt = &sine_vt;
        source.base.channels = 1;
        source.base.rate = test ? 44100 : 48000;
        source.frequency = 1000;
        de_play_desc d = {0};
        d.pitch = test ? 1 : 2;
        ASSERT_TRUE(de__voice_play_source(a, &source.base, &d).id != 0);
        de__audio_null_pump(a, 4608, capture);
        u32 bin = peak(capture, test ? 80 : 165, 12);
        EXPECT_TRUE(test ? (bin >= 84 && bin <= 86) : (bin >= 170 && bin <= 172));
        de_audio_destroy(a);
    }
}
TEST(audio_voices, priority_then_oldest_steal_and_generation) {
    de_audio *a = test_audio_manual(3, 0);
    test_source source[4];
    de_voice voices[4];
    for (u32 i = 0; i < 3; ++i) {
        test_src_init(&source[i], TSRC_CONST, 0.1f, 0, 1);
        de_play_desc d = {0};
        d.priority = i ? 9 : 5;
        voices[i] = de__voice_play_source(a, &source[i].base, &d);
    }
    de__audio_null_pump(a, 256, NULL);
    test_src_init(&source[3], TSRC_CONST, 0.2f, 0, 1);
    voices[3] = test_play(a, &source[3], 0, false);
    ASSERT_TRUE(voices[3].id != 0);
    EXPECT_EQ(de__voice_slot(voices[3]), de__voice_slot(voices[1]));
    EXPECT_NE(voices[3].id, voices[1].id);
    EXPECT_FALSE(de_audio_playing(a, voices[1]));
    de__audio_null_pump(a, 256, NULL);
    EXPECT_EQ(test_src_wait_closed(a, &source[1]), 1u);
    EXPECT_TRUE(de_audio_playing(a, voices[3]));
    de_audio_destroy(a);
    for (u32 i = 0; i < 4; ++i)
        EXPECT_EQ(test_src_closes(&source[i]), 1u);
}
TEST(audio_voices, volume_and_spatial_parameters_are_queued) {
    de_audio *a = test_audio_manual(4, 0);
    test_source source;
    test_src_init(&source, TSRC_CONST, 1, 0, 1);
    de_voice voice = test_play(a, &source, 0, false);
    de__audio_null_pump(a, 256, NULL);
    de_audio_set_volume(a, voice, 0.25f, 0);
    de_audio_set_position(a, voice, (de_dvec3){1, 2, 3}, (de_vec3){4, 5, 6});
    de__audio_null_pump(a, 256, NULL);
    f32 out[512];
    de__audio_null_pump(a, 256, out);
    EXPECT_FLOAT_EQ(out[0], G * 0.25f, 1e-6);
    EXPECT_FLOAT_EQ(a->voices[de__voice_slot(voice)].position.z, 3, 0);
    de_audio_destroy(a);
}
TEST(audio_voices, tiny_loop_keeps_playing) {
    const u32 lengths[] = {1, 7, 16, 257};
    for (u32 k = 0; k < sizeof lengths / sizeof lengths[0]; ++k) {
        de_audio *a = test_audio_manual(4, 0);
        test_source source;
        test_src_init(&source, TSRC_RAMP, 0.001f, lengths[k], 1);
        de_voice voice = test_play(a, &source, 0, true);
        f32 out[1536];
        u64 hits = de__rt_guard_hits();
        de__audio_null_pump(a, 768, out);
        EXPECT_TRUE(de_audio_playing(a, voice));
        EXPECT_EQ(de__rt_guard_hits(), hits);
        for (u32 i = 0; i < 768; ++i)
            EXPECT_FLOAT_EQ(out[i * 2], G * (f32)(i % lengths[k] + 1) * 0.001f, 1e-6);
        de_audio_destroy(a);
        EXPECT_EQ(test_src_closes(&source), 1u);
    }
}
static u32 empty_loop_read(de__audio_source *s, f32 *out, u32 frames, u32 channels) {
    (void)s;
    (void)out;
    (void)frames;
    (void)channels;
    return 0;
}
static bool failed_loop_seek(de__audio_source *s, u64 frame) {
    (void)s;
    (void)frame;
    return false;
}
TEST(audio_voices, looping_eof_without_progress_retires) {
    const de__audio_source_vtbl variants[] = {{empty_loop_read, test_src_seek, test_src_close},
                                              {test_src_read, failed_loop_seek, test_src_close},
                                              {test_src_read, NULL, test_src_close}};
    for (u32 i = 0; i < sizeof variants / sizeof variants[0]; ++i) {
        de_audio *a = test_audio_manual(4, 0);
        test_source source;
        test_src_init(&source, TSRC_CONST, 1, 1, 1);
        source.base.vt = &variants[i];
        de_voice voice = test_play(a, &source, 0, true);
        f32 out[512];
        de__audio_null_pump(a, 256, out);
        EXPECT_FALSE(de_audio_playing(a, voice));
        for (u32 frame = i == 0 ? 0 : 1; frame < 256; ++frame)
            EXPECT_FLOAT_EQ(out[frame * 2], 0, 0);
        de_audio_destroy(a);
        EXPECT_EQ(test_src_closes(&source), 1u);
    }
}
TEST(audio_voices, allocation_and_lock_free_256_voices) {
    de_tracking_allocator track;
    de_allocator alloc = de_tracking_allocator_init(&track, NULL, DE_MEM_AUDIO);
    de_audio_desc d = {0};
    d.backend = DE_AUDIO_NULL;
    d.max_voices = 256;
    d.allocator = &alloc;
    de_audio *a = NULL;
    de__audio_create_opts options = {true};
    ASSERT_EQ(de__audio_create_ex(&d, &options, &a), DE_OK);
    static test_source source[256];
    for (u32 i = 0; i < 256; ++i) {
        test_src_init(&source[i], TSRC_CONST, 0.001f, 0, 1);
        ASSERT_TRUE(test_play(a, &source[i], 0, false).id != 0);
    }
    de_tracking_stats before = de_tracking_allocator_stats(&track);
    u64 hits = de__rt_guard_hits();
    de__audio_null_pump(a, 4800, NULL);
    de_tracking_stats after = de_tracking_allocator_stats(&track);
    EXPECT_EQ(before.alloc_count, after.alloc_count);
    EXPECT_EQ(before.realloc_count, after.realloc_count);
    EXPECT_EQ(de__rt_guard_hits(), hits);
    de_audio_destroy(a);
    EXPECT_EQ(de_tracking_allocator_stats(&track).bytes_live, 0u);
}

TEST(audio_voices, pitch_changes_reject_aliases_and_restore_passband_without_allocations) {
    de_tracking_allocator tracking;
    de_allocator alloc = de_tracking_allocator_init(&tracking, NULL, DE_MEM_AUDIO);
    de_audio_desc desc = {.backend = DE_AUDIO_NULL, .allocator = &alloc, .max_voices = 4};
    de_audio *a = NULL;
    de__audio_create_opts opts = {true};
    ASSERT_EQ(de__audio_create_ex(&desc, &opts, &a), DE_OK);
    sine_source source = {0};
    source.base.vt = &sine_vt;
    source.base.channels = 1;
    source.base.rate = 48000;
    source.frequency = 18000;
    de_play_desc play = {.pitch = 1.01f};
    de_voice voice = de__voice_play_source(a, &source.base, &play);
    ASSERT_TRUE(voice.id != 0);
    f32 output[2048 * 2];
    f64 rms[3];
    de_tracking_stats before = de_tracking_allocator_stats(&tracking);
    u64 hits = de__rt_guard_hits();
    for (u32 stage = 0; stage < 3; ++stage) {
        if (stage)
            de_audio_set_pitch(a, voice, stage == 1 ? 2 : 1.01f);
        de__audio_null_pump(a, 2048, output);
        f64 sum = 0;
        for (u32 frame = 512; frame < 2048; ++frame)
            sum += (f64)output[frame * 2] * output[frame * 2];
        rms[stage] = sqrt(sum / 1536);
        EXPECT_TRUE(de_audio_playing(a, voice));
    }
    EXPECT_TRUE(rms[0] > 0.4);
    EXPECT_TRUE(rms[1] / rms[0] < 0.0001);
    EXPECT_TRUE(fabs(20 * log10(rms[2] / rms[0])) < 0.1);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).alloc_count, before.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).realloc_count, before.realloc_count);
    EXPECT_EQ(de__rt_guard_hits(), hits);
    de_audio_destroy(a);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
}
