#include "source_test_util.h"
static void playback(bool opus, bool loop, bool realtime) {
    de__sound_asset *asset = source_cooked(opus, 48000 * 60, loop);
    EXPECT_TRUE(asset->resident == NULL);
    EXPECT_TRUE(asset->metadata_size < asset->view.data_size);
    de_audio *a = test_audio_manual(4, 0);
    de__audio_source *source = NULL;
    ASSERT_EQ(de__source_stream(a, asset, &source), DE_OK);
    f32 boundary[256 * 8] = {0};
    if (loop) {
        de__sound_decoder *reference = NULL;
        ASSERT_EQ(de__decoder_create(de_heap_allocator(), asset, &reference), DE_OK);
        f32 discard[1024 * 8];
        for (u32 at = 0; at < 48000;) {
            u32 n = 48000 - at;
            if (n > 1024)
                n = 1024;
            ASSERT_EQ(de__decoder_read(reference, discard, n), n);
            at += n;
        }
        ASSERT_EQ(de__decoder_read(reference, boundary, 256), 256u);
        de__decoder_destroy(reference);
    }
    u32 source_channels = asset->view.channels;
    de__sound_asset_release(asset);
    de_play_desc desc = {0};
    desc.loop = loop;
    de_voice v = de__voice_play_source(a, source, &desc);
    ASSERT_TRUE(v.id != 0);
    u32 before = de_audio_get_stats(a).underruns;
    u64 hits = de__rt_guard_hits();
    u32 frames = loop ? 48000 * 4 : 48000 * 60;
    u64 start = de_time_now_ns();
    for (u32 done = 0; done < frames;) {
        u32 n = frames - done;
        if (n > 256)
            n = 256;
        f32 capture[512];
        de__audio_null_pump(a, n, capture);
        if (loop && done <= 96000 && done + n > 96000) {
            u32 offset = 96000 - done;
            for (u32 f = offset; f < n; ++f) {
                f32 expected = boundary[(f - offset) * source_channels];
                if (source_channels == 1)
                    expected *= 0.70710678118f;
                EXPECT_FLOAT_EQ(capture[f * 2], expected, 0.002);
            }
        }
        done += n;
        u64 due = start + (u64)done * 1000000000ull / (48000u * (realtime ? 1u : 8u));
        u64 now = de_time_now_ns();
        if (due > now)
            de_sleep_ns(due - now);
    }
    EXPECT_EQ(de_audio_get_stats(a).underruns, before);
    EXPECT_EQ(de__rt_guard_hits(), hits);
    if (loop)
        EXPECT_TRUE(de_audio_playing(a, v));
    else {
        de__audio_null_pump(a, 256, NULL);
        EXPECT_FALSE(de_audio_playing(a, v));
    }
    de_audio_destroy(a);
    de_io_shutdown();
}
TEST(audio_streaming, opus_sixty_seconds_at_eight_times) {
    playback(true, false, false);
}
TEST(audio_streaming, vorbis_sixty_seconds_at_eight_times) {
    playback(false, false, false);
}
TEST(audio_streaming, opus_loop_points_without_underrun) {
    playback(true, true, false);
}
TEST(audio_streaming, vorbis_loop_points_without_underrun) {
    playback(false, true, false);
}
TEST(audio_streaming, worker_starvation_is_silence_and_counted) {
    de__sound_asset *asset = source_cooked(true, 48000 * 2, false);
    de_audio *a = test_audio_manual(4, 0);
    de__audio_source *source = NULL;
    ASSERT_EQ(de__source_stream(a, asset, &source), DE_OK);
    de__sound_asset_release(asset);
    de__stream_test_starve(a, true);
    de_play_desc desc = {0};
    de_voice voice = de__voice_play_source(a, source, &desc);
    ASSERT_TRUE(voice.id != 0);
    f32 out[512];
    de__audio_null_pump(a, 48000, NULL);
    de__audio_null_pump(a, 256, out);
    EXPECT_TRUE(de_audio_get_stats(a).underruns > 0);
    for (u32 i = 0; i < 512; ++i)
        EXPECT_FLOAT_EQ(out[i], 0, 0);
    de_audio_destroy(a);
    de_io_shutdown();
}
TEST(audio_streaming, decoder_seek_uses_tables_and_matches_linear_reference) {
    for (u32 codec = 0; codec < 2; ++codec) {
        de__sound_asset *asset = source_cooked(codec == 0, 48000 * 3, false);
        de__sound_decoder *d = NULL;
        ASSERT_EQ(de__decoder_create(de_heap_allocator(), asset, &d), DE_OK);
        f32 scratch[1024 * 8], reference[256 * 8], actual[256 * 8];
        u32 channels = asset->view.channels;
        for (u32 i = 0; i < 47; ++i)
            ASSERT_EQ(de__decoder_read(d, scratch, 1024), 1024u);
        ASSERT_EQ(de__decoder_read(d, reference, 256), 256u);
        ASSERT_TRUE(de__decoder_seek(d, 47 * 1024));
        ASSERT_EQ(de__decoder_read(d, actual, 256), 256u);
        for (u32 i = 0; i < 256 * channels; ++i)
            EXPECT_FLOAT_EQ(actual[i], reference[i], codec == 0 ? 0.002 : 1e-5);
        de__decoder_destroy(d);
        de__sound_asset_release(asset);
        de_io_shutdown();
    }
}

TEST(audio_streaming, opus_sixty_seconds_realtime) {
    playback(true, false, true);
}
TEST(audio_streaming, vorbis_sixty_seconds_realtime) {
    playback(false, false, true);
}
