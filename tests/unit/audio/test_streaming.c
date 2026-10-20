#include "source_test_util.h"
#include <time.h>

typedef struct stream_contention_fx {
    de_task_system *tasks;
    de_asset_system *assets;
    de_audio *audio;
    de_sem started, release;
    bool started_ready, release_ready;
    de_task_counter blockers;
} stream_contention_fx;
static void stream_contention_fx_setup(stream_contention_fx *f) {
    de_fp_env_init_thread();
    ASSERT_EQ(de_sem_init(&f->started, 0), DE_OK);
    f->started_ready = true;
    ASSERT_EQ(de_sem_init(&f->release, 0), DE_OK);
    f->release_ready = true;
    ASSERT_EQ(
        de_task_system_create(
            &(de_task_system_desc){.worker_count = 1, .background_worker_count = 2}, &f->tasks),
        DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.tasks = f->tasks}, &f->assets),
              DE_OK);
    de__audio_create_opts opts = {true};
    ASSERT_EQ(de__audio_create_ex(
                  &(de_audio_desc){.backend = DE_AUDIO_NULL, .max_voices = 4, .assets = f->assets},
                  &opts, &f->audio),
              DE_OK);
}
static void stream_contention_fx_teardown(stream_contention_fx *f) {
    if (f->release_ready)
        de_sem_post(&f->release, 2);
    if (f->tasks)
        de_task_wait(f->tasks, &f->blockers);
    de_audio_destroy(f->audio);
    de_asset_system_destroy(f->assets);
    if (f->tasks)
        de_task_system_destroy(f->tasks);
    if (f->release_ready)
        de_sem_destroy(&f->release);
    if (f->started_ready)
        de_sem_destroy(&f->started);
    de_io_shutdown();
}
static void block_unrelated_background_task(void *user) {
    stream_contention_fx *f = user;
    de_sem_post(&f->started, 1);
    de_sem_wait(&f->release);
}
TEST_F(audio_streaming, unrelated_background_work_cannot_hold_up_audio_refills,
       stream_contention_fx) {
    de__sound_asset *asset = source_cooked(true, 48000 * 2, false);
    de__audio_source *source = NULL;
    ASSERT_EQ(de__source_stream(f->audio, asset, &source), DE_OK);
    de__sound_asset_release(asset);
    de_voice voice = de__voice_play_source(f->audio, source, &(de_play_desc){0});
    ASSERT_TRUE(voice.id != 0);
    for (u32 i = 0; i < 2; ++i)
        de_task_submit(f->tasks, DE_TASK_POOL_BACKGROUND, block_unrelated_background_task, f,
                       &f->blockers);
    for (u32 i = 0; i < 2; ++i)
        de_sem_wait(&f->started);
    u64 start = de_time_now_ns(), guard = de__rt_guard_hits();
    for (u32 done = 0; done < 48000 * 2;) {
        u32 n = 48000 * 2 - done;
        if (n > 256)
            n = 256;
        de__audio_null_pump(f->audio, n, NULL);
        done += n;
        de_sleep_until_ns(start + (u64)done * 1000000000ull / (48000 * 8));
    }
    de__audio_null_pump(f->audio, 256, NULL);
    EXPECT_EQ(de_audio_get_stats(f->audio).underruns, 0u);
    EXPECT_EQ(de__rt_guard_hits(), guard);
    EXPECT_FALSE(de_audio_playing(f->audio, voice));
}

TEST(audio_streaming, decoder_io_recovers_after_short_read_and_rejected_submit) {
    de__sound_asset *asset = source_cooked(true, 48000 * 2, false);
    de_tracking_allocator tracking;
    de_allocator alloc = de_tracking_allocator_init(&tracking, NULL, DE_MEM_AUDIO);
    de__sound_decoder *decoder = NULL;
    ASSERT_EQ(de__decoder_create(&alloc, asset, &decoder), DE_OK);
    u8 actual[16];
    /* A submitted short read completes asynchronously; a rejected submission
     * has no callback. Neither may leave a stale completion for the next read. */
    EXPECT_EQ(de__decoder_io(decoder, asset->view.size, actual, 1), DE_ERR_IO);
    u64 submitted = decoder->io_requests;
    de_file *file = decoder->file;
    decoder->file = NULL;
    EXPECT_EQ(de__decoder_io(decoder, 0, actual, sizeof actual), DE_ERR_INVALID_ARGUMENT);
    decoder->file = file;
    EXPECT_EQ(decoder->io_requests, submitted);
    de_tracking_stats before = de_tracking_allocator_stats(&tracking);
#if !defined(_WIN32) /* The Windows CRT clock measures elapsed time instead of CPU time. */
    clock_t cpu = clock();
#endif
    u64 start = de_time_now_ns();
    for (u32 i = 0; i < 4096; ++i) {
        u32 offset = i % 32;
        memset(actual, 0xff, sizeof actual);
        ASSERT_EQ(de__decoder_io(decoder, offset, actual, sizeof actual), DE_OK);
        ASSERT_MEMEQ(actual, asset->view.bytes + offset, sizeof actual);
    }
    printf("4096 async decoder reads: wall %.3fms", (double)(de_time_now_ns() - start) / 1e6);
#if !defined(_WIN32)
    printf(", process CPU %.3fms", (double)(clock() - cpu) * 1000 / CLOCKS_PER_SEC);
#endif
    printf("\n");
    EXPECT_EQ(decoder->io_requests, submitted + 4096);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).alloc_count, before.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).realloc_count, before.realloc_count);
    de__decoder_destroy(decoder);
    de__sound_asset_release(asset);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
    de_io_shutdown();
}

TEST(audio_streaming, closing_multiple_sources_drains_queued_refills_and_seeks) {
    de__sound_asset *asset = source_cooked(true, 48000 * 2, false);
    de_tracking_allocator tracking;
    de_allocator alloc = de_tracking_allocator_init(&tracking, NULL, DE_MEM_AUDIO);
    de_audio *audio = NULL;
    de__audio_create_opts opts = {true};
    ASSERT_EQ(de__audio_create_ex(
                  &(de_audio_desc){.allocator = &alloc, .backend = DE_AUDIO_NULL, .max_voices = 4},
                  &opts, &audio),
              DE_OK);
    f32 pcm[16000];
    for (u32 pass = 0; pass < 4; ++pass) {
        de__audio_source *sources[8];
        for (u32 i = 0; i < 8; ++i) {
            ASSERT_EQ(de__source_stream(audio, asset, &sources[i]), DE_OK);
            ASSERT_EQ(sources[i]->vt->read(sources[i], pcm, 16000, 1), 16000u);
        }
        de__stream_service(audio);
        for (u32 i = 0; i < 8; ++i) {
            u32 index = (i * 5) % 8;
            ASSERT_TRUE(sources[index]->vt->seek(sources[index], 48137));
            sources[index]->vt->close(sources[index]);
        }
    }
    de_audio_destroy(audio);
    de__sound_asset_release(asset);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).blocks_live, 0u);
    de_io_shutdown();
}

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

static void opus_seek_cases(u32 channels, bool noise) {
    source_mount();
    const u32 frames = 48000 * 6;
    u64 size;
    u8 *wave = source_wave(frames * channels, 48000, &size);
    wave[22] = (u8)channels;
    wave[32] = (u8)(channels * 2);
    de__snd_w32(wave + 28, 48000 * channels * 2);
    u32 random = 12345;
    for (u32 f = 0; f < frames; ++f)
        for (u32 c = 0; c < channels; ++c) {
            random = random * 1664525u + 1013904223u;
            f64 value = 0.35 * sin(2 * 3.141592653589793 * (997 + c * 2143) * f / 48000);
            if (noise) {
                value += 0.12 * ((f64)(random >> 8) / 8388608 - 1);
                if (f % 12000 == 0)
                    value += 0.3;
                if (f / 24000 % 3 == 0)
                    value *= 0.001;
            }
            i16 sample = de__sound_i16((f32)value);
            u64 at = 44 + ((u64)f * channels + c) * 2;
            wave[at] = (u8)sample;
            wave[at + 1] = (u8)((u16)sample >> 8);
        }
    de_tracking_allocator tracking;
    de_allocator a = de_tracking_allocator_init(&tracking, NULL, DE_MEM_AUDIO);
    de__sound_settings settings = {.stream = true, .stream_set = true, .codec = "opus"};
    void *cooked = NULL;
    u64 cooked_size = 0;
    ASSERT_EQ(de__sound_cook(&a, wave, size, &settings, &cooked, &cooked_size), DE_OK);
    free(wave);
    de__sound_asset *asset = NULL;
    ASSERT_EQ(de__sound_asset_create(&a, cooked, cooked_size, "astest:/cache", &asset), DE_OK);
    DE_FREE(&a, cooked, (size_t)cooked_size);
    de__sound_decoder *d = NULL;
    ASSERT_EQ(de__decoder_create(&a, asset, &d), DE_OK);
    f32 *reference = DE_ALLOC(&a, (size_t)frames * channels * sizeof(f32), 16, DE_MEM_AUDIO);
    ASSERT_TRUE(reference != NULL);
    for (u32 at = 0; at < frames;) {
        u32 n = frames - at;
        if (n > 1024)
            n = 1024;
        ASSERT_EQ(de__decoder_read(d, reference + (size_t)at * channels, n), n);
        at += n;
    }
    u32 boundary = 960 - asset->view.preskip;
    const u32 targets[] = {240128,       0,          120128, 1,     boundary - 1, boundary,
                           boundary + 1, 23999,      24000,  47999, 48000,        48128,
                           frames - 257, frames - 1, frames};
    de_tracking_stats before = de_tracking_allocator_stats(&tracking);
    f32 actual[256 * 8];
    f64 worst = 0;
    for (u32 k = 0; k < sizeof targets / sizeof targets[0]; ++k) {
        u32 target = targets[k];
        ASSERT_TRUE(de__decoder_seek(d, target));
        u32 first_packet = d->packet;
        /* 320ms preroll + <500ms table spacing + preskip; never decode from start late. */
        EXPECT_TRUE(d->skip <= 16 * 960 + 24000 + asset->view.preskip);
        if (target >= 48000)
            EXPECT_TRUE(first_packet > 0);
        u32 expected = frames - target;
        if (expected > 256)
            expected = 256;
        ASSERT_EQ(de__decoder_read(d, actual, 256), expected);
        EXPECT_TRUE(d->packet - first_packet <= 43);
        for (u32 i = 0; i < expected * channels; ++i) {
            f64 error = fabs((f64)actual[i] - reference[(size_t)target * channels + i]);
            if (error > worst)
                worst = error;
            EXPECT_TRUE(error <= 0.002);
        }
    }
    printf("Opus seek channels%u noise%u max error %.8f\n", channels, noise, worst);
    EXPECT_FALSE(de__decoder_seek(d, frames + 1));
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).alloc_count, before.alloc_count);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).realloc_count, before.realloc_count);
    DE_FREE(&a, reference, (size_t)frames * channels * sizeof(f32));
    de__decoder_destroy(d);
    de__sound_asset_release(asset);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
    de_io_shutdown();
}
TEST(audio_streaming, opus_seek_boundaries_late_targets_and_stereo_match_linear) {
    opus_seek_cases(1, false);
    opus_seek_cases(2, true);
}

TEST(audio_streaming, opus_read_ahead_reduces_io_and_preserves_packet_and_seek_output) {
    const u32 frames = 48000 * 24;
    de__sound_asset *asset = source_cooked(true, frames, false);
    /* Exercise multiple read-ahead boundaries, including a partial final buffer. */
    ASSERT_TRUE(asset->view.data_size > 2 * 65536);
    de__sound_decoder *cached = NULL, *uncached = NULL;
    ASSERT_EQ(de__decoder_create(de_heap_allocator(), asset, &cached), DE_OK);
    ASSERT_EQ(de__decoder_create(de_heap_allocator(), asset, &uncached), DE_OK);
    f32 actual[256 * 8], expected[256 * 8];
    for (u32 at = 0; at < frames;) {
        u32 n = frames - at;
        if (n > 256)
            n = 256;
        /* Less than one packet per call: invalidate before each packet decode on
         * the reference, while leaving its predictor and buffered PCM intact. */
        uncached->filled = 0;
        ASSERT_EQ(de__decoder_read(uncached, expected, n), n);
        ASSERT_EQ(de__decoder_read(cached, actual, n), n);
        ASSERT_EQ(memcmp(actual, expected, (size_t)n * asset->view.channels * sizeof(f32)), 0);
        at += n;
    }
    printf("Opus compressed bytes %llu: cached reads %llu, uncached reads %llu\n",
           (unsigned long long)asset->view.data_size, (unsigned long long)cached->io_requests,
           (unsigned long long)uncached->io_requests);
    EXPECT_TRUE(uncached->io_requests >= asset->view.packet_count - 1);
    EXPECT_TRUE(cached->io_requests * 8 < uncached->io_requests);
    EXPECT_EQ(de__decoder_read(cached, actual, 256), 0u);
    EXPECT_EQ(de__decoder_read(uncached, expected, 256), 0u);
    const u32 targets[] = {frames - 2048, 0, 48001, frames / 2, 1, frames - 1, frames};
    for (u32 k = 0; k < sizeof targets / sizeof targets[0]; ++k) {
        ASSERT_TRUE(de__decoder_seek(cached, targets[k]));
        ASSERT_TRUE(de__decoder_seek(uncached, targets[k]));
        uncached->filled = 0;
        u32 n = de__decoder_read(uncached, expected, 256);
        ASSERT_EQ(de__decoder_read(cached, actual, 256), n);
        EXPECT_EQ(memcmp(actual, expected, (size_t)n * asset->view.channels * sizeof(f32)), 0);
    }
    de__decoder_destroy(uncached);
    de__decoder_destroy(cached);
    de__sound_asset_release(asset);
    de_io_shutdown();
}

static void finish_seek_after_read_snapshot(de__audio_source *source, void *user) {
    ++*(u32 *)user;
    de__stream_test_refill_now(source);
}
TEST(audio_streaming, seek_ack_between_cursor_and_generation_loads_discards_stale_samples) {
    de__sound_asset *asset = source_cooked(true, 48000 * 3, false);
    de__sound_decoder *reference = NULL;
    ASSERT_EQ(de__decoder_create(de_heap_allocator(), asset, &reference), DE_OK);
    f32 expected[320], actual[256];
    de_audio *a = test_audio_manual(4, 0);
    de__audio_source *source = NULL;
    ASSERT_EQ(de__source_stream(a, asset, &source), DE_OK);
    de__stream_test_starve(a, true);
    ASSERT_EQ(source->vt->read(source, actual, 73, 1), 73u);
    const u32 targets[] = {48137, 90123, 1137, 70001, 129137, 24001};
    u32 calls = 0;
    for (u32 k = 0; k < sizeof targets / sizeof targets[0]; ++k) {
        ASSERT_TRUE(de__decoder_seek(reference, targets[k]));
        ASSERT_EQ(de__decoder_read(reference, expected, 320), 320u);
        /* Two pending requests: only the latest target may be published. */
        ASSERT_TRUE(source->vt->seek(source, 0));
        ASSERT_TRUE(source->vt->seek(source, targets[k]));
        u64 hits = de__rt_guard_hits();
        de__rt_guard_enter();
        u32 got = source->vt->read(source, actual, 16, 1);
        de__rt_guard_leave();
        EXPECT_EQ(got, 16u);
        EXPECT_EQ(de__rt_guard_hits(), hits);
        for (u32 i = 0; i < 16; ++i)
            EXPECT_FLOAT_EQ(actual[i], 0, 0);
        de__stream_test_read_hook(source, finish_seek_after_read_snapshot, &calls);
        /* Force worker ack publication precisely after the reader snapshots its cursors. */
        ASSERT_EQ(source->vt->read(source, actual, 64, 1), 64u);
        EXPECT_EQ(calls, k + 1);
        de__stream_test_read_hook(source, NULL, NULL);
        for (u32 i = 0; i < 64; ++i)
            ASSERT_FLOAT_EQ(actual[i], expected[i], 0.002);
        de__stream_test_refill_now(source);
        hits = de__rt_guard_hits();
        de__rt_guard_enter();
        got = source->vt->read(source, actual, 256, 1);
        de__rt_guard_leave();
        EXPECT_EQ(got, 256u);
        EXPECT_EQ(de__rt_guard_hits(), hits);
        for (u32 i = 0; i < 256; ++i)
            EXPECT_FLOAT_EQ(actual[i], expected[i + 64], 0.002);
    }
    ASSERT_TRUE(source->vt->seek(source, asset->view.frames));
    de__stream_test_read_hook(source, finish_seek_after_read_snapshot, &calls);
    EXPECT_EQ(source->vt->read(source, actual, 64, 1), 0u);
    de__stream_test_read_hook(source, NULL, NULL);
    ASSERT_TRUE(source->vt->seek(source, 0));
    de__stream_test_refill_now(source);
    ASSERT_EQ(source->vt->read(source, actual, 64, 1), 64u);
    ASSERT_TRUE(de__decoder_seek(reference, 0));
    ASSERT_EQ(de__decoder_read(reference, expected, 64), 64u);
    for (u32 i = 0; i < 64; ++i)
        EXPECT_FLOAT_EQ(actual[i], expected[i], 0.002);
    source->vt->close(source);
    de_audio_destroy(a);
    de__decoder_destroy(reference);
    de__sound_asset_release(asset);
    de_io_shutdown();
}
