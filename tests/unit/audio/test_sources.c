#include "source_test_util.h"
#include "cook/lib/de_cooker.h"
#include "asset/manifest_internal.h"
TEST(audio_sources, pcm_formats_and_eight_channels_exact) {
    de_audio *a = test_audio_manual(4, 0);
    for (u32 codec = 1; codec <= 2; ++codec)
        for (u32 channels = 1; channels <= 8; ++channels) {
            de__sound_asset *asset = source_pcm(codec, channels, 128);
            de__audio_source *source = NULL;
            ASSERT_EQ(de__source_resident(a, asset, &source), DE_OK);
            de__sound_asset_release(asset);
            f32 pcm[128 * 8];
            u64 hits = de__rt_guard_hits();
            de__rt_guard_enter();
            u32 n = source->vt->read(source, pcm, 128, channels);
            de__rt_guard_leave();
            EXPECT_EQ(n, 128u);
            EXPECT_EQ(de__rt_guard_hits(), hits);
            for (u32 f = 0; f < 128; ++f)
                for (u32 c = 0; c < channels; ++c)
                    EXPECT_FLOAT_EQ(pcm[f * channels + c],
                                    ((i32)(f % 100) * 100 + (i32)c) / 32768.0f, 0);
            source->vt->close(source);
        }
    de_audio_destroy(a);
}
TEST(audio_sources, adpcm_matches_reference_decode) {
    u64 n = 128 + 512;
    u8 bytes[640] = {0};
    memcpy(bytes, "DSND", 4);
    de__snd_w32(bytes + 4, 1);
    de__snd_w32(bytes + 8, DE__DSND_ADPCM);
    de__snd_w32(bytes + 12, 1);
    de__snd_w32(bytes + 16, 48000);
    de__snd_w64(bytes + 24, 1017);
    de__snd_w64(bytes + 48, 128);
    de__snd_w64(bytes + 56, 512);
    de__snd_w32(bytes + 76, 1017);
    de__snd_w32(bytes + 80, 512);
    for (u32 i = 4; i < 512; ++i)
        bytes[128 + i] = (u8)i;
    de__sound_asset *asset = NULL;
    ASSERT_EQ(de__sound_asset_create(de_heap_allocator(), bytes, n, NULL, &asset), DE_OK);
    de_audio *a = test_audio_manual(4, 0);
    de__audio_source *source = NULL;
    ASSERT_EQ(de__source_resident(a, asset, &source), DE_OK);
    de__sound_asset_release(asset);
    f32 output[1017];
    u64 hits = de__rt_guard_hits();
    de__rt_guard_enter();
    ASSERT_EQ(source->vt->read(source, output, 1017, 1), 1017u);
    de__rt_guard_leave();
    EXPECT_EQ(de__rt_guard_hits(), hits);
    i32 predictor = 0, index = 0;
    EXPECT_FLOAT_EQ(output[0], 0, 0);
    for (u32 i = 1; i < 1017; ++i) {
        u32 t = i - 1;
        u32 code = (bytes[132 + t / 2] >> ((t & 1) * 4)) & 15;
        EXPECT_FLOAT_EQ(output[i], de__ima_decode(code, &predictor, &index) / 32768.0f, 0);
    }
    source->vt->close(source);
    de_audio_destroy(a);
}
TEST(audio_sources, resident_loop_points_are_sample_accurate) {
    de_audio *a = test_audio_manual(4, 0);
    de__sound_asset *asset = source_pcm(DE__DSND_PCM_F32, 1, 10);
    asset->view.flags = DE__DSND_LOOP;
    asset->view.loop_start = 3;
    asset->view.loop_end = 7;
    de__audio_source *source = NULL;
    ASSERT_EQ(de__source_resident(a, asset, &source), DE_OK);
    de__sound_asset_release(asset);
    de_play_desc d = {0};
    d.loop = true;
    de_voice v = de__voice_play_source(a, source, &d);
    f32 out[512];
    de__audio_null_pump(a, 256, out);
    for (u32 i = 0; i < 256; ++i) {
        u32 frame = i < 7 ? i : 3 + (i - 7) % 4;
        EXPECT_FLOAT_EQ(out[2 * i], frame * 100 / 32768.0f * 0.70710678118f, 1e-7);
    }
    EXPECT_TRUE(de_audio_playing(a, v));
    de_audio_destroy(a);
}
TEST(audio_sources, manifest_sound_cooks_loads_and_plays) {
    source_mount();
    u64 n;
    u8 *wave = source_wave(4800, 48000, &n);
    ASSERT_EQ(de_file_write_all_atomic("astest:/click.wav", wave, n), DE_OK);
    free(wave);
    wave = source_wave(96000, 48000, &n);
    ASSERT_EQ(de_file_write_all_atomic("astest:/title.wav", wave, n), DE_OK);
    free(wave);
    static const char manifest[] =
        "[assets]\n\"sfx/click\"={type=\"sound\",source=\"click.wav\"}\n"
        "\"music/title\"={type=\"sound\",source=\"title.wav\",stream=true,codec=\"opus\"}\n";
    ASSERT_EQ(de_file_write_all_atomic("astest:/assets.toml", manifest, sizeof manifest - 1),
              DE_OK);
    de_asset_system_desc desc = {0};
    desc.cache_dir = "astest:/cache";
    de_asset_system *assets = NULL;
    ASSERT_EQ(de_asset_system_create(&desc, &assets), DE_OK);
    ASSERT_EQ(de_audio_register_sound_type(assets), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(assets, "astest:/assets.toml"), DE_OK);
    de_load_handle load = de_asset_load(assets, de_asset_id_from("sfx/click"), 0);
    ASSERT_EQ(de_asset_wait(assets, load, 5000000000ull), DE_LOAD_READY);
    de_load_handle music = de_asset_load(assets, de_asset_id_from("music/title"), 0);
    ASSERT_EQ(de_asset_wait(assets, music, 5000000000ull), DE_LOAD_READY);
    de_audio_desc ad = {0};
    ad.assets = assets;
    ad.backend = DE_AUDIO_NULL;
    ad.max_voices = 4;
    de_audio *a = NULL;
    de__audio_create_opts options = {true};
    ASSERT_EQ(de__audio_create_ex(&ad, &options, &a), DE_OK);
    de_play_desc play = {0};
    play.sound = de_asset_sound(assets, de_asset_id_from("sfx/click"));
    de_voice voice = de_audio_play(a, &play);
    ASSERT_TRUE(voice.id != 0);
    f32 out[512];
    de__audio_null_pump(a, 256, out);
    bool nonzero = false;
    for (u32 i = 0; i < 512; ++i)
        nonzero |= out[i] != 0;
    EXPECT_TRUE(nonzero);
    de_audio_stop(a, voice, 0);
    de__audio_null_pump(a, 512, NULL);
    play.sound = de_asset_sound(assets, de_asset_id_from("music/title"));
    voice = de_audio_play(a, &play);
    ASSERT_TRUE(voice.id != 0);
    de__audio_null_pump(a, 256, out);
    nonzero = false;
    for (u32 i = 0; i < 512; ++i)
        nonzero |= out[i] != 0;
    EXPECT_TRUE(nonzero);
    de_audio_destroy(a);
    de_asset_release(assets, music);
    de_asset_release(assets, load);
    de_asset_system_destroy(assets);
    de_io_shutdown();
}
