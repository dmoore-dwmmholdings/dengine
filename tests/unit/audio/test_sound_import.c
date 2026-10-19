#define _CRT_SECURE_NO_WARNINGS
#include "de_test.h"
#include "cook/importers/sound/sound_import.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
static u8 *wave(u32 rate, u32 frames, u32 bits, bool floating, u64 *size) {
    u32 stride = bits / 8;
    *size = 44 + (u64)frames * stride;
    u8 *w = calloc(1, (size_t)*size);
    ASSERT_TRUE(w != NULL);
    memcpy(w, "RIFF", 4);
    de__snd_w32(w + 4, (u32)*size - 8);
    memcpy(w + 8, "WAVEfmt ", 8);
    de__snd_w32(w + 16, 16);
    w[20] = floating ? 3 : 1;
    w[22] = 1;
    de__snd_w32(w + 24, rate);
    de__snd_w32(w + 28, rate * stride);
    w[32] = (u8)stride;
    w[34] = (u8)bits;
    memcpy(w + 36, "data", 4);
    de__snd_w32(w + 40, frames * stride);
    for (u32 i = 0; i < frames; ++i) {
        f32 sample = (f32)(0.5 * sin(i * 0.075));
        if (floating) {
            u32 n;
            memcpy(&n, &sample, 4);
            de__snd_w32(w + 44 + (u64)i * 4, n);
        } else {
            i32 n = (i32)(sample * (bits == 24 ? 8388608 : 32768));
            for (u32 j = 0; j < stride; ++j)
                w[44 + (u64)i * stride + j] = (u8)((u32)n >> (8 * j));
        }
    }
    return w;
}
static void cook_twice(const void *bytes, u64 size, de__sound_settings *settings, u32 codec) {
    const de_allocator *a = de_heap_allocator();
    void *one = NULL, *two = NULL;
    u64 n = 0, m = 0;
    ASSERT_EQ(de__sound_cook(a, bytes, size, settings, &one, &n), DE_OK);
    ASSERT_EQ(de__sound_cook(a, bytes, size, settings, &two, &m), DE_OK);
    EXPECT_EQ(n, m);
    EXPECT_MEMEQ(one, two, (size_t)n);
    de__sound_view view;
    ASSERT_EQ(de__sound_parse(one, n, &view), DE_OK);
    EXPECT_EQ(view.codec, codec);
    DE_FREE(a, one, (size_t)n);
    DE_FREE(a, two, (size_t)m);
}
TEST(sound_import, malformed_vorbis_comment_lengths_reject_without_writes) {
    const char *paths[] = {DE_SOUND_REGRESSION_DIR "/vorbis-start-decoder-3323b755.ogg",
                           DE_SOUND_REGRESSION_DIR "/vorbis-comment-cleanup-c9ffaa9a.ogg",
                           DE_SOUND_REGRESSION_DIR "/vorbis-lookup-power-3dc71b61.ogg"};
    for (u32 i = 0; i < 3; ++i) {
        FILE *file = fopen(paths[i], "rb");
        ASSERT_TRUE(file != NULL);
        u8 bytes[16384];
        size_t size = fread(bytes, 1, sizeof bytes, file);
        int extra = fgetc(file);
        fclose(file);
        ASSERT_EQ(extra, EOF);
        ASSERT_GT(size, 0u);
        void *output = NULL;
        u64 output_size = 0;
        de__sound_settings settings = {0};
        EXPECT_EQ(
            de__sound_cook(de_heap_allocator(), bytes, size, &settings, &output, &output_size),
            DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(output == NULL);
    }
}
TEST(sound_import, wav_formats_exact_float_pcm_and_repeat) {
    const u32 bits[] = {16, 24, 32};
    for (u32 k = 0; k < 3; ++k) {
        u64 size;
        u8 *w = wave(48000, 4096, bits[k], k == 2, &size);
        de__sound_settings s = {0};
        s.stream_set = true;
        cook_twice(w, size, &s, DE__DSND_PCM_F32);
        void *out = NULL;
        u64 n = 0;
        ASSERT_EQ(de__sound_cook(NULL, w, size, &s, &out, &n), DE_OK);
        de__sound_view v;
        ASSERT_EQ(de__sound_parse(out, n, &v), DE_OK);
        for (u32 i = 0; i < 4096; ++i) {
            u32 got = de__snd_u32(v.data + i * 4);
            f32 sample;
            memcpy(&sample, &got, 4);
            f32 expected;
            if (k == 2) {
                u32 value = de__snd_u32(w + 44 + i * 4);
                memcpy(&expected, &value, 4);
            } else if (k == 0) {
                u16 value = (u16)w[44 + i * 2] | (u16)w[45 + i * 2] << 8;
                expected = (i16)value / 32768.0f;
            } else {
                u32 value = (u32)w[44 + i * 3] | (u32)w[45 + i * 3] << 8 | (u32)w[46 + i * 3] << 16;
                i32 signed_value = (i32)(value << 8);
                expected = signed_value / 2147483648.0f;
            }
            EXPECT_FLOAT_EQ(sample, expected, 0);
        }
        DE_FREE(de_heap_allocator(), out, (size_t)n);
        free(w);
    }
}
TEST(sound_import, adpcm_repeat_and_decode_error) {
    u64 size;
    u8 *w = wave(48000, 4096, 16, false, &size);
    de__sound_settings s = {0};
    s.compression = "adpcm";
    cook_twice(w, size, &s, DE__DSND_ADPCM);
    void *out = NULL;
    u64 n = 0;
    ASSERT_EQ(de__sound_cook(NULL, w, size, &s, &out, &n), DE_OK);
    de__sound_view v;
    ASSERT_EQ(de__sound_parse(out, n, &v), DE_OK);
    f64 error = 0;
    for (u32 block = 0; block < 4; ++block) {
        const u8 *p = v.data + block * 512;
        i32 pred = (i16)((u16)p[0] | (u16)p[1] << 8), index = p[2];
        for (u32 j = 0; j < 1017; ++j) {
            if (j) {
                u32 t = j - 1;
                (void)de__ima_decode((p[4 + t / 2] >> ((t & 1) * 4)) & 15, &pred, &index);
            }
            u32 i = block * 1017 + j;
            i16 ref = (i16)((u16)w[44 + i * 2] | (u16)w[45 + i * 2] << 8);
            f64 diff = (pred - ref) / 32768.0;
            error += diff * diff;
        }
    }
    EXPECT_TRUE(sqrt(error / (4 * 1017)) < 0.04);
    DE_FREE(de_heap_allocator(), out, (size_t)n);
    free(w);
}
static u8 *fixture(const char *name, u64 *size) {
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/%s", DE_SOUND_DATA_DIR, name);
    FILE *f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ(fseek(f, 0, SEEK_END), 0);
    long n = ftell(f);
    ASSERT_TRUE(n > 0);
    rewind(f);
    u8 *p = malloc((size_t)n);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(fread(p, 1, (size_t)n, f), (size_t)n);
    fclose(f);
    *size = (u64)n;
    return p;
}
TEST(sound_import, flac_vorbis_stream_passthrough_and_downmix) {
    u64 size;
    u8 *f = fixture("tone.flac", &size);
    de__sound_settings s = {0};
    cook_twice(f, size, &s, DE__DSND_PCM_F32);
    free(f);
    f = fixture("tone.ogg", &size);
    cook_twice(f, size, &s, DE__DSND_PCM_F32);
    s.stream_set = true;
    s.stream = true;
    cook_twice(f, size, &s, DE__DSND_VORBIS);
    s.mono = true;
    s.codec = "opus";
    cook_twice(f, size, &s, DE__DSND_OPUS);
    free(f);
}
TEST(sound_import, sixty_second_opus_repeat_threshold_and_loops) {
    u64 size;
    u8 *w = wave(48000, 48000 * 60, 16, false, &size);
    de__sound_settings s = {0};
    s.loop = true;
    s.loop_start = 100;
    s.loop_end = 48000 * 59;
    cook_twice(w, size, &s, DE__DSND_OPUS);
    free(w);
}
TEST(sound_import, resampled_opus_and_loop_settings) {
    u64 size;
    u8 *w = wave(44100, 44100, 16, false, &size);
    de__sound_settings s = {0};
    s.stream_set = true;
    s.stream = true;
    s.loop = true;
    s.loop_start = 441;
    s.loop_end = 44100;
    void *out = NULL;
    u64 n = 0;
    ASSERT_EQ(de__sound_cook(NULL, w, size, &s, &out, &n), DE_OK);
    de__sound_view v;
    ASSERT_EQ(de__sound_parse(out, n, &v), DE_OK);
    EXPECT_EQ(v.rate, 48000u);
    EXPECT_EQ(v.frames, 48000u);
    EXPECT_EQ(v.loop_start, 480u);
    EXPECT_EQ(v.loop_end, 48000u);
    DE_FREE(de_heap_allocator(), out, (size_t)n);
    free(w);
}
TEST(sound_import, corrupt_inputs_and_settings) {
    de__sound_settings s = {0};
    void *out = NULL;
    u64 n = 0;
    u8 junk[64] = {0};
    EXPECT_EQ(de__sound_cook(NULL, junk, sizeof junk, &s, &out, &n), DE_ERR_INVALID_ARGUMENT);
    u64 size;
    u8 *w = wave(48000, 100, 16, false, &size);
    EXPECT_EQ(de__sound_cook(NULL, w, size - 1, &s, &out, &n), DE_ERR_INVALID_ARGUMENT);
    s.loop = true;
    s.loop_start = 200;
    EXPECT_EQ(de__sound_cook(NULL, w, size, &s, &out, &n), DE_ERR_INVALID_ARGUMENT);
    s.loop = false;
    s.loop_start = 0;
    s.compression = "broken";
    EXPECT_EQ(de__sound_cook(NULL, w, size, &s, &out, &n), DE_ERR_INVALID_ARGUMENT);
    free(w);
}
TEST(sound_import, wav_smpl_loop_metadata) {
    u64 size;
    u8 *w = wave(48000, 100, 16, false, &size);
    u64 total = size + 68;
    u8 *all = calloc(1, (size_t)total);
    ASSERT_TRUE(all != NULL);
    memcpy(all, w, (size_t)size);
    free(w);
    de__snd_w32(all + 4, (u32)total - 8);
    memcpy(all + size, "smpl", 4);
    de__snd_w32(all + size + 4, 60);
    de__snd_w32(all + size + 8 + 28, 1);
    de__snd_w32(all + size + 8 + 44, 10);
    de__snd_w32(all + size + 8 + 48, 89);
    de__sound_settings s = {0};
    void *out = NULL;
    u64 n = 0;
    ASSERT_EQ(de__sound_cook(NULL, all, total, &s, &out, &n), DE_OK);
    de__sound_view v;
    ASSERT_EQ(de__sound_parse(out, n, &v), DE_OK);
    EXPECT_TRUE((v.flags & DE__DSND_LOOP) != 0);
    EXPECT_EQ(v.loop_start, 10u);
    EXPECT_EQ(v.loop_end, 90u);
    DE_FREE(de_heap_allocator(), out, (size_t)n);
    free(all);
}
#include "cook/lib/de_cooker.h"
#include "asset/manifest_internal.h"
#include <dengine/de_platform.h>
TEST(sound_import, cooker_registry_end_to_end_and_allocator_cleanup) {
    de_tracking_allocator tracking;
    de_allocator a = de_tracking_allocator_init(&tracking, NULL, DE_MEM_AUDIO);
    de_cooker *c = NULL;
    de_cooker_desc desc = {0};
    desc.allocator = &a;
    desc.verify_determinism = DE_COOK_VERIFY_ON;
    ASSERT_EQ(de_cooker_create(&desc, &c), DE_OK);
    de_asset_type_desc type = {0};
    ASSERT_TRUE(de_cooker_builtin_importer("sound", &type));
    ASSERT_EQ(de_cooker_register_type(c, &type), DE_OK);
    ASSERT_EQ(de_fs_mount("soundtest", DE_SOUND_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    u64 size;
    u8 *wav = wave(48000, 1024, 16, false, &size);
    ASSERT_EQ(de_file_write_all_atomic("soundtest:/sound.wav", wav, size), DE_OK);
    free(wav);
    de__manifest_asset asset = {0};
    asset.id = "sfx/test";
    asset.type = "sound";
    asset.source = "sound.wav";
    asset.file = "soundtest:/assets.toml";
    de_cook_input input = {0};
    input.asset = &asset;
    input.asset_root = "soundtest:/";
    de_cook_result result = {0};
    ASSERT_EQ(de_cooker_cook(c, &input, &result), DE_OK);
    de__sound_view view;
    ASSERT_EQ(de__sound_parse(result.data, result.size, &view), DE_OK);
    EXPECT_EQ(view.frames, 1024u);
    de_cook_result_free(&result);
    de_cooker_destroy(c);
    (void)de_fs_unmount("soundtest", DE_SOUND_TMP_DIR);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
}
TEST(sound_import, ima_and_ms_adpcm_wav_sources) {
    const char *names[] = {"ima.wav", "ms.wav"};
    for (u32 i = 0; i < 2; ++i) {
        u64 size;
        u8 *data = fixture(names[i], &size);
        de__sound_settings s = {0};
        cook_twice(data, size, &s, DE__DSND_PCM_F32);
        free(data);
    }
}

TEST(sound_import, manifest_loop_setting_overrides_authored_wav_and_vorbis) {
    de_tracking_allocator tracking;
    de_allocator a = de_tracking_allocator_init(&tracking, NULL, DE_MEM_AUDIO);
    de_cooker *c = NULL;
    de_cooker_desc desc = {.allocator = &a, .verify_determinism = DE_COOK_VERIFY_ON};
    ASSERT_EQ(de_cooker_create(&desc, &c), DE_OK);
    de_asset_type_desc type = de_sound_import_type_desc();
    ASSERT_EQ(de_cooker_register_type(c, &type), DE_OK);
    ASSERT_EQ(de_fs_mount("looptest", DE_SOUND_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    for (u32 format = 0; format < 2; ++format) {
        u64 size;
        u8 *bytes;
        const char *name;
        if (format == 0) {
            bytes = wave(48000, 100, 16, false, &size);
            bytes = realloc(bytes, (size_t)size + 68);
            ASSERT_TRUE(bytes != NULL);
            memset(bytes + size, 0, 68);
            de__snd_w32(bytes + 4, (u32)size + 68 - 8);
            memcpy(bytes + size, "smpl", 4);
            de__snd_w32(bytes + size + 4, 60);
            de__snd_w32(bytes + size + 8 + 28, 1);
            de__snd_w32(bytes + size + 8 + 44, 10);
            de__snd_w32(bytes + size + 8 + 48, 89);
            size += 68;
            name = "loop.wav";
        } else {
            bytes = fixture("tone-loops.ogg", &size);
            name = "loop.ogg";
        }
        char path[128];
        snprintf(path, sizeof path, "looptest:/%s", name);
        ASSERT_EQ(de_file_write_all_atomic(path, bytes, size), DE_OK);
        free(bytes);
        /* Check both resident output and native streaming codecs. */
        for (u32 stream = 0; stream < 2; ++stream) {
            de__manifest_kv settings[] = {
                {"loop", {.type = DE__MANIFEST_BOOL, .u.b = false}},
                {"stream", {.type = DE__MANIFEST_BOOL, .u.b = stream != 0}}};
            de__manifest_asset asset = {.id = "loop",
                                        .type = "sound",
                                        .source = name,
                                        .file = "looptest:/assets.toml",
                                        .settings = settings,
                                        .setting_count = 2};
            de_cook_input input = {.asset = &asset, .asset_root = "looptest:/"};
            for (u32 mode = 0; mode < 3; ++mode) {
                /* Absent inherits; explicit false disables; explicit true enables. */
                asset.settings = mode == 0 ? settings + 1 : settings;
                asset.setting_count = mode == 0 ? 1 : 2;
                settings[0].value.u.b = mode == 2;
                de_cook_result result = {0};
                ASSERT_EQ(de_cooker_cook(c, &input, &result), DE_OK);
                de__sound_view view;
                ASSERT_EQ(de__sound_parse(result.data, result.size, &view), DE_OK);
                EXPECT_EQ((view.flags & DE__DSND_LOOP) != 0, mode != 1);
                EXPECT_EQ(view.loop_start, 10u);
                EXPECT_EQ(view.loop_end, 90u);
                EXPECT_EQ((view.flags & DE__DSND_STREAM) != 0, stream != 0);
                de_cook_result_free(&result);
            }
        }
    }
    de_cooker_destroy(c);
    (void)de_fs_unmount("looptest", DE_SOUND_TMP_DIR);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
}
