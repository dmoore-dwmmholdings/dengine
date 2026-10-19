#include "audio_test_util.h"
#define G 0.70710678118f
TEST(audio_buses, descendants_multiply_and_mute) {
    de_audio *a = test_audio_manual(4, 0);
    de_bus parent = de_audio_bus_create(a, "Music", de_audio_master(a));
    de_bus child = de_audio_bus_create(a, "Drums", parent);
    ASSERT_TRUE(parent.id && child.id);
    de_audio_bus_volume(a, parent, 0.5f, 0);
    de_audio_bus_volume(a, child, 0.5f, 0);
    test_source source;
    test_src_init(&source, TSRC_CONST, 1, 0, 1);
    de_play_desc d = {0};
    d.bus = child;
    ASSERT_TRUE(de__voice_play_source(a, &source.base, &d).id != 0);
    de__audio_null_pump(a, 512, NULL);
    f32 out[512];
    de__audio_null_pump(a, 256, out);
    EXPECT_FLOAT_EQ(out[0], G * 0.25f, 1e-6);
    de_audio_bus_mute(a, parent, true);
    de__audio_null_pump(a, 256, out);
    for (u32 i = 0; i < 512; ++i)
        EXPECT_FLOAT_EQ(out[i], 0, 0);
    de_audio_bus_mute(a, parent, false);
    de__audio_null_pump(a, 256, out);
    EXPECT_FLOAT_EQ(out[0], G * 0.25f, 1e-6);
    de_audio_destroy(a);
}
TEST(audio_buses, channel_matrices) {
    f32 mono = 1, stereo[2] = {0};
    de__audio_channel_mix(stereo, 2, &mono, 1, 1);
    EXPECT_FLOAT_EQ(stereo[0], G, 0);
    EXPECT_FLOAT_EQ(stereo[1], G, 0);
    f32 input[2] = {1, 0}, average = 0;
    de__audio_channel_mix(&average, 1, input, 2, 1);
    EXPECT_FLOAT_EQ(average, 0.5, 0);
    f32 surround[8] = {0};
    de__audio_channel_mix(surround, 8, input, 2, 1);
    EXPECT_FLOAT_EQ(surround[0], 1, 0);
    EXPECT_FLOAT_EQ(surround[3], 0, 0);
    EXPECT_FLOAT_EQ(surround[2], G, 0);
    f32 down[2] = {0};
    de__audio_channel_mix(down, 2, surround, 8, 1);
    EXPECT_TRUE(down[0] > down[1]);
}
TEST(audio_buses, ids_capacity_and_invalid_parent) {
    de_audio *a = test_audio_manual(1, 0);
    EXPECT_EQ(de_audio_master(a).id, 1u);
    EXPECT_EQ(de_audio_bus_create(a, "bad", (de_bus){999}).id, 0u);
    for (u32 i = 0; i < 63; ++i)
        EXPECT_TRUE(de_audio_bus_create(a, "bus", (de_bus){0}).id != 0);
    EXPECT_EQ(de_audio_bus_create(a, "overflow", (de_bus){0}).id, 0u);
    de_audio_destroy(a);
}

TEST(audio_buses, same_layout_preserves_isolated_channels_and_accumulates_gain) {
    const f32 gains[] = {0, 0.5f, -1};
    for (u32 channels = 1; channels <= 8; ++channels)
        for (u32 active = 0; active < channels; ++active)
            for (u32 g = 0; g < sizeof gains / sizeof gains[0]; ++g) {
                f32 input[8] = {0}, output[8];
                input[active] = 0.25f;
                for (u32 i = 0; i < 8; ++i)
                    output[i] = (f32)(i + 1) * 0.125f;
                de__audio_channel_mix(output, channels, input, channels, gains[g]);
                for (u32 i = 0; i < 8; ++i)
                    EXPECT_FLOAT_EQ(output[i],
                                    (f32)(i + 1) * 0.125f + (i == active ? input[i] * gains[g] : 0),
                                    0);
            }
}

TEST(audio_buses, stereo_voice_preserves_each_speaker) {
    de_audio *a = test_audio_manual(4, 0);
    test_source source;
    test_src_init(&source, TSRC_CONST, 0.01f, 0, 2);
    de_voice voice = test_play(a, &source, 0, false);
    f32 output[256 * 2];
    u64 hits = de__rt_guard_hits();
    de__audio_null_pump(a, 256, output);
    EXPECT_TRUE(de_audio_playing(a, voice));
    EXPECT_EQ(de__rt_guard_hits(), hits);
    for (u32 frame = 0; frame < 256; ++frame)
        for (u32 c = 0; c < 2; ++c)
            EXPECT_FLOAT_EQ(output[frame * 2 + c], 0.01f * (f32)(c + 1), 1e-7);
    de_audio_destroy(a);
    EXPECT_EQ(test_src_closes(&source), 1u);
}
