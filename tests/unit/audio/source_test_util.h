#ifndef DE_AUDIO_SOURCE_TEST_UTIL_H
#define DE_AUDIO_SOURCE_TEST_UTIL_H
#include "audio_test_util.h"
#include "audio/source/stream_internal.h"
#include "cook/importers/sound/sound_import.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static inline void source_mount(void) {
    de_fp_env_init_thread();
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("astest", DE_AUDIO_SOURCE_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
}
static inline u8 *source_wave(u32 frames, u32 rate, u64 *size) {
    *size = 44 + (u64)frames * 2;
    u8 *b = calloc(1, (size_t)*size);
    ASSERT_TRUE(b != NULL);
    memcpy(b, "RIFF", 4);
    de__snd_w32(b + 4, (u32)*size - 8);
    memcpy(b + 8, "WAVEfmt ", 8);
    de__snd_w32(b + 16, 16);
    b[20] = 1;
    b[22] = 1;
    de__snd_w32(b + 24, rate);
    de__snd_w32(b + 28, rate * 2);
    b[32] = 2;
    b[34] = 16;
    memcpy(b + 36, "data", 4);
    de__snd_w32(b + 40, frames * 2);
    for (u32 i = 0; i < frames; ++i) {
        i16 sample = (i16)(12000 * sin(2 * 3.141592653589793 * 997 * i / rate));
        b[44 + (u64)i * 2] = (u8)sample;
        b[45 + (u64)i * 2] = (u8)((u16)sample >> 8);
    }
    return b;
}
static inline de__sound_asset *source_cooked(bool opus, u32 frames, bool loop) {
    source_mount();
    u64 size = 0;
    u8 *input = NULL;
    if (opus)
        input = source_wave(frames, 48000, &size);
    else {
        FILE *f = fopen(DE_AUDIO_SOURCE_DATA_DIR "/music-60s.ogg", "rb");
        ASSERT_TRUE(f != NULL);
        ASSERT_EQ(fseek(f, 0, SEEK_END), 0);
        long n = ftell(f);
        rewind(f);
        ASSERT_TRUE(n > 0);
        size = (u64)n;
        input = malloc((size_t)size);
        ASSERT_TRUE(input != NULL);
        ASSERT_EQ(fread(input, 1, (size_t)size, f), (size_t)size);
        fclose(f);
    }
    de__sound_settings settings = {0};
    settings.stream_set = true;
    settings.stream = true;
    settings.codec = opus ? "opus" : "vorbis";
    if (loop) {
        settings.loop = true;
        settings.loop_start = 48000;
        settings.loop_end = 96000;
    }
    void *cooked = NULL;
    u64 cooked_size = 0;
    ASSERT_EQ(de__sound_cook(NULL, input, size, &settings, &cooked, &cooked_size), DE_OK);
    free(input);
    de__sound_asset *asset = NULL;
    ASSERT_EQ(
        de__sound_asset_create(de_heap_allocator(), cooked, cooked_size, "astest:/cache", &asset),
        DE_OK);
    DE_FREE(de_heap_allocator(), cooked, (size_t)cooked_size);
    return asset;
}
static inline de__sound_asset *source_pcm(u32 codec, u32 channels, u32 frames) {
    u64 bytes = (u64)channels * frames * (codec == DE__DSND_PCM16 ? 2 : 4);
    u64 size = 128 + bytes;
    u8 *b = calloc(1, (size_t)size);
    ASSERT_TRUE(b != NULL);
    memcpy(b, "DSND", 4);
    de__snd_w32(b + 4, 1);
    de__snd_w32(b + 8, codec);
    de__snd_w32(b + 12, channels);
    de__snd_w32(b + 16, 48000);
    de__snd_w64(b + 24, frames);
    de__snd_w64(b + 48, 128);
    de__snd_w64(b + 56, bytes);
    for (u32 f = 0; f < frames; ++f)
        for (u32 c = 0; c < channels; ++c) {
            i16 sample = (i16)((i32)(f % 100) * 100 + (i32)c);
            u64 i = (u64)f * channels + c;
            if (codec == DE__DSND_PCM16) {
                b[128 + i * 2] = (u8)sample;
                b[129 + i * 2] = (u8)((u16)sample >> 8);
            } else {
                f32 value = sample / 32768.0f;
                u32 bits;
                memcpy(&bits, &value, 4);
                de__snd_w32(b + 128 + i * 4, bits);
            }
        }
    de__sound_asset *asset = NULL;
    ASSERT_EQ(de__sound_asset_create(de_heap_allocator(), b, size, NULL, &asset), DE_OK);
    free(b);
    return asset;
}
#endif
