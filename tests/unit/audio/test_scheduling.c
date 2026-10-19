/* test_scheduling.c: sample-accurate commands (DENG-308). Manual null mode, so every capture is
 * exact: capture frame i is mixer sample i. */
#include "audio_test_util.h"

#include <stdlib.h>

#define CH 2u

/* Pumps `frames` frames in uneven pieces (so reads cross block boundaries at odd offsets). */
static void pump_uneven(de_audio *a, f32 *capture, u32 frames) {
    static const u32 k_pieces[] = {97, 1000, 1, 333, 256, 255, 2048, 7};
    u32 done = 0, k = 0;
    while (done < frames) {
        u32 n = k_pieces[k++ % (sizeof k_pieces / sizeof k_pieces[0])];
        if (n > frames - done)
            n = frames - done;
        de__audio_null_pump(a, n, capture + (size_t)done * CH);
        done += n;
    }
}

static f32 *capture_alloc(u32 frames) {
    f32 *c = (f32 *)calloc((size_t)frames * CH, sizeof(f32));
    ASSERT_TRUE(c != NULL);
    return c;
}

/* Every frame of `cap` is zero except those listed (value `v` on both channels). */
static void expect_clicks(const f32 *cap, u32 frames, const u64 *at, u32 n, f32 v) {
    u32 seen = 0;
    for (u32 i = 0; i < frames; ++i) {
        bool want = false;
        for (u32 k = 0; k < n; ++k)
            want = want || at[k] == i;
        if (want) {
            EXPECT_FLOAT_EQ(cap[i * CH], v * 0.70710678118f, 0.0);
            EXPECT_FLOAT_EQ(cap[i * CH + 1], v * 0.70710678118f, 0.0);
            ++seen;
        } else if (cap[i * CH] != 0.0f || cap[i * CH + 1] != 0.0f) {
            EXPECT_EQ(i, (u32)UINT32_MAX); /* reports the unexpected frame */
            return;
        }
    }
    EXPECT_EQ(seen, n);
}

TEST(audio_sched, click_at_48123) {
    de_audio *a = test_audio_manual(16, 0);
    test_source click;
    test_src_init(&click, TSRC_CONST, 1.0f, 1, 1); /* one frame of 1.0 */
    de_voice v = test_play(a, &click, 48123, false);
    ASSERT_TRUE(DE_HANDLE_VALID(v));
    EXPECT_TRUE(de_audio_playing(a, v));

    const u32 frames = 50000;
    f32 *cap = capture_alloc(frames);
    pump_uneven(a, cap, frames);
    const u64 at[] = {48123};
    expect_clicks(cap, frames, at, 1, 1.0f);
    EXPECT_FALSE(de_audio_playing(a, v)); /* the source ended */
    EXPECT_EQ(de_atomic_load_u32(&a->cmds_late, DE_ATOMIC_RELAXED), 0u);
    free(cap);
    de_audio_destroy(a);
    EXPECT_EQ(test_src_closes(&click), 1u);
}

TEST(audio_sched, clicks_across_block_boundaries) {
    /* Block size 256: samples on, just before and just after block edges, plus the first frame
     * of a block that a pump call ends inside. */
    static const u64 k_at[] = {1, 255, 256, 257, 511, 512, 1023, 1024, 4095, 4096, 9999};
    const u32 n = (u32)(sizeof k_at / sizeof k_at[0]);
    de_audio *a = test_audio_manual(32, 0);
    test_source clicks[sizeof k_at / sizeof k_at[0]];
    /* Scheduled out of order: the pending list sorts them. */
    for (u32 k = n; k-- > 0;) {
        test_src_init(&clicks[k], TSRC_CONST, 0.5f, 1, 1);
        ASSERT_TRUE(DE_HANDLE_VALID(test_play(a, &clicks[k], k_at[k], false)));
    }
    const u32 frames = 12000;
    f32 *cap = capture_alloc(frames);
    pump_uneven(a, cap, frames);
    expect_clicks(cap, frames, k_at, n, 0.5f);
    free(cap);
    de_audio_destroy(a);
}

TEST(audio_sched, start_stop_sample_exact) {
    de_audio *a = test_audio_manual(16, 0);
    test_source s1, s2, s3;
    test_src_init(&s1, TSRC_CONST, 0.25f, 0, 1);
    test_src_init(&s2, TSRC_CONST, 0.5f, 0, 1);
    test_src_init(&s3, TSRC_CONST, 0.125f, 0, 1);
    de_voice v1 = test_play(a, &s1, 1000, false);
    de_voice v2 = test_play(a, &s2, 300, false);
    de_voice v3 = test_play(a, &s3, 2000, false);
    de__voice_stop_at(a, v1, 1777); /* crosses blocks 3..6 */
    de__voice_stop_at(a, v2, 301);  /* one sample */
    de__voice_stop_at(a, v3, 2048); /* ends on a block edge */
    const u32 frames = 4096;
    f32 *cap = capture_alloc(frames);
    pump_uneven(a, cap, frames);
    for (u32 i = 0; i < frames; ++i) {
        f32 want = 0.0f;
        if (i >= 1000 && i < 1777)
            want += 0.25f * 0.70710678118f;
        if (i == 300)
            want += 0.5f * 0.70710678118f;
        if (i >= 2000 && i < 2048)
            want += 0.125f * 0.70710678118f;
        if (cap[i * CH] != want || cap[i * CH + 1] != want) {
            EXPECT_EQ(i, (u32)UINT32_MAX);
            break;
        }
    }
    EXPECT_FALSE(de_audio_playing(a, v1));
    EXPECT_FALSE(de_audio_playing(a, v2));
    EXPECT_FALSE(de_audio_playing(a, v3));
    free(cap);
    de_audio_destroy(a);
    EXPECT_EQ(test_src_closes(&s1) + test_src_closes(&s2) + test_src_closes(&s3), 3u);
}

typedef struct order_log {
    u64 args[256];
    u64 samples[256];
    u32 count;
} order_log;

static void log_fn(de_audio *a, void *user, u64 arg, u64 sample) {
    (void)a;
    order_log *l = (order_log *)user;
    if (l->count < 256) {
        l->args[l->count] = arg;
        l->samples[l->count] = sample;
    }
    ++l->count;
}

TEST(audio_sched, equal_timestamps_fifo) {
    de_audio *a = test_audio_manual(4, 0);
    static order_log olog;
    memset(&olog, 0, sizeof olog);
    /* Interleave three timestamps; within one timestamp the arrival order must hold. */
    for (u64 i = 0; i < 90; ++i) {
        const u64 at = i % 3 == 0 ? 7000 : (i % 3 == 1 ? 6000 : 5000);
        ASSERT_TRUE(de__audio_post_fn(a, log_fn, &olog, i, at));
    }
    de__audio_null_pump(a, 8000, NULL);
    ASSERT_EQ(olog.count, 90u);
    u32 k = 0;
    for (u64 at = 5000; at <= 7000; at += 1000) {
        for (u64 i = 0; i < 90; ++i) {
            const u64 want_at = i % 3 == 0 ? 7000 : (i % 3 == 1 ? 6000 : 5000);
            if (want_at != at)
                continue;
            EXPECT_EQ(olog.args[k], i);
            EXPECT_EQ(olog.samples[k], at);
            ++k;
        }
    }
    /* Same timestamp, posted in two batches with a render in between: still FIFO. */
    memset(&olog, 0, sizeof olog);
    ASSERT_TRUE(de__audio_post_fn(a, log_fn, &olog, 1, 20000));
    de__audio_null_pump(a, 1000, NULL);
    ASSERT_TRUE(de__audio_post_fn(a, log_fn, &olog, 2, 20000));
    ASSERT_TRUE(de__audio_post_fn(a, log_fn, &olog, 3, 19999));
    de__audio_null_pump(a, 20000, NULL);
    ASSERT_EQ(olog.count, 3u);
    EXPECT_EQ(olog.args[0], 3u);
    EXPECT_EQ(olog.args[1], 1u);
    EXPECT_EQ(olog.args[2], 2u);
    de_audio_destroy(a);
}

TEST(audio_sched, late_and_immediate_commands) {
    de_audio *a = test_audio_manual(4, 0);
    static order_log olog;
    memset(&olog, 0, sizeof olog);
    /* 300 frames read: blocks 0 and 1 (0..511) are rendered, so the next block starts at 512. */
    de__audio_null_pump(a, 300, NULL);
    ASSERT_TRUE(de__audio_post_fn(a, log_fn, &olog, 1, 10)); /* already rendered: late */
    ASSERT_TRUE(de__audio_post_fn(a, log_fn, &olog, 2, 0));  /* immediately */
    de__audio_null_pump(a, 512, NULL);
    ASSERT_EQ(olog.count, 2u);
    EXPECT_EQ(olog.samples[0], 512u);
    EXPECT_EQ(olog.samples[1], 512u);
    EXPECT_EQ(olog.args[0], 1u);
    EXPECT_EQ(de_atomic_load_u32(&a->cmds_late, DE_ATOMIC_RELAXED), 1u);

    /* A voice started "now" starts on the first frame of the next rendered block. */
    test_source s;
    test_src_init(&s, TSRC_CONST, 1.0f, 0, 1);
    f32 cap[2048 * CH];
    memset(cap, 0, sizeof cap);
    const u64 clock0 = de_audio_clock(a); /* 812: next frame to play */
    EXPECT_EQ(clock0, 812u);
    de_voice v = test_play(a, &s, 0, false);
    de__audio_null_pump(a, 2048, cap);
    /* Frames 812..1023 were rendered before the play: silence; the voice starts at 1024. */
    for (u32 i = 0; i < 2048; ++i) {
        const f32 want = clock0 + i >= 1024 ? 0.70710678118f : 0.0f;
        if (cap[i * CH] != want) {
            EXPECT_EQ(i, (u32)UINT32_MAX);
            break;
        }
    }
    de_audio_stop(a, v, 0.0f);
    de_audio_destroy(a);
}

TEST(audio_sched, stop_cancels_scheduled_voice) {
    de_audio *a = test_audio_manual(4, 0);
    test_source s;
    test_src_init(&s, TSRC_CONST, 1.0f, 0, 1);
    de_voice v = test_play(a, &s, 10000, false);
    de__audio_null_pump(a, 256, NULL); /* the play is pending now */
    EXPECT_TRUE(de_audio_playing(a, v));
    de_audio_stop(a, v, 0.0f);
    f32 *cap = capture_alloc(12000);
    de__audio_null_pump(a, 12000, cap);
    EXPECT_FALSE(de_audio_playing(a, v));
    for (u32 i = 0; i < 12000 * CH; ++i)
        if (cap[i] != 0.0f) {
            EXPECT_EQ(i, (u32)UINT32_MAX);
            break;
        }
    EXPECT_EQ(test_src_wait_closed(a, &s), 1u);
    free(cap);
    de_audio_destroy(a);
    EXPECT_EQ(test_src_closes(&s), 1u); /* not closed twice */
}

TEST(audio_sched, loop_restarts_on_the_next_sample) {
    de_audio *a = test_audio_manual(4, 0);
    test_source s;
    test_src_init(&s, TSRC_RAMP, 1.0f, 100, 1); /* 1, 2, ..., 100 */
    de_voice v = test_play(a, &s, 50, true);
    f32 *cap = capture_alloc(1000);
    de__audio_null_pump(a, 1000, cap);
    for (u32 i = 0; i < 1000; ++i) {
        const f32 want = i < 50 ? 0.0f : (f32)((i - 50) % 100 + 1) * 0.70710678118f;
        if (cap[i * CH] != want) {
            EXPECT_EQ(i, (u32)UINT32_MAX);
            break;
        }
    }
    EXPECT_TRUE(de_audio_playing(a, v));
    de__voice_stop_at(a, v, 1500);
    de__audio_null_pump(a, 1000, cap);
    EXPECT_FALSE(de_audio_playing(a, v));
    free(cap);
    de_audio_destroy(a);
}

TEST(audio_sched, play_sound_needs_sources) {
    de_audio *a = test_audio_manual(4, 0);
    de_play_desc d;
    memset(&d, 0, sizeof d);
    d.sound.id = 1;
    de_voice v = de_audio_play(a, &d);
    EXPECT_FALSE(DE_HANDLE_VALID(v));
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_EQ(de_error_last()->result, DE_ERR_UNSUPPORTED);
    EXPECT_FALSE(DE_HANDLE_VALID(de_audio_play(a, NULL)));
    EXPECT_FALSE(de_audio_playing(a, v));
    de_audio_destroy(a);
}
