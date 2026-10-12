/* audio_test_util.h: shared helpers of the audio unit tests (DENG-308). Test sources, and
 * creating a system in manual (pumped) or realtime null mode. */
#ifndef DE_AUDIO_TEST_UTIL_H
#define DE_AUDIO_TEST_UTIL_H

#include "audio/audio_internal.h"
#include "de_test.h"

#include <string.h>

/* ---- Test sources ---------------------------------------------------------------------------- */

typedef enum test_src_kind {
    TSRC_CONST, /* every sample = value (channel c: value * (c + 1)) */
    TSRC_RAMP,  /* sample of frame i = (i + 1) * value (channel c: times c + 1) */
} test_src_kind;

typedef struct test_source {
    de__audio_source base; /* first */
    u32 kind;
    f32 value;
    u64 pos;
    u64 length;           /* 0 = endless */
    de_atomic_u32 closes; /* times close ran (any thread) */
} test_source;

static inline u32 test_src_read(de__audio_source *s, f32 *out, u32 frames, u32 channels) {
    test_source *t = (test_source *)s;
    u64 n = frames;
    if (t->length != 0)
        n = t->pos >= t->length ? 0 : (t->length - t->pos < n ? t->length - t->pos : n);
    for (u64 i = 0; i < n; ++i) {
        f32 base = t->kind == TSRC_RAMP ? (f32)(t->pos + i + 1) * t->value : t->value;
        for (u32 c = 0; c < channels; ++c)
            out[i * channels + c] = base * (f32)(c + 1);
    }
    t->pos += n;
    return (u32)n;
}

static inline bool test_src_seek(de__audio_source *s, u64 frame) {
    ((test_source *)s)->pos = frame;
    return true;
}

static inline void test_src_close(de__audio_source *s) {
    de_atomic_fetch_add_u32(&((test_source *)s)->closes, 1, DE_ATOMIC_RELEASE);
}

static const de__audio_source_vtbl k_test_src_vt = {test_src_read, test_src_seek, test_src_close};

static inline void test_src_init(test_source *t, u32 kind, f32 value, u64 length, u32 channels) {
    memset(t, 0, sizeof *t);
    t->base.vt = &k_test_src_vt;
    t->base.channels = channels;
    t->base.rate = 48000;
    t->base.flags = DE__SOURCE_SEEKABLE | (length == 0 ? DE__SOURCE_ENDLESS : 0u);
    t->base.length_frames = length;
    t->kind = kind;
    t->value = value;
    t->length = length;
    de_atomic_init_u32(&t->closes, 0);
}

static inline u32 test_src_closes(const test_source *t) {
    return de_atomic_load_u32(&t->closes, DE_ATOMIC_ACQUIRE);
}

/* Collects ended voices on this thread and waits (bounded) until `t` was closed: the control
 * thread may be closing it at the same moment. */
static inline u32 test_src_wait_closed(de_audio *a, const test_source *t) {
    const u64 end = de_time_now_ns() + 5000000000ull;
    de__audio_collect(a);
    while (test_src_closes(t) == 0 && de_time_now_ns() < end)
        de_sleep_ns(2000000ull);
    return test_src_closes(t);
}

/* Plays `t` on a new voice at `at` (0 = next block). */
static inline de_voice test_play(de_audio *a, test_source *t, u64 at, bool loop) {
    de_play_desc d;
    memset(&d, 0, sizeof d);
    d.start_at_sample = at;
    d.loop = loop;
    return de__voice_play_source(a, &t->base, &d);
}

/* ---- Systems --------------------------------------------------------------------------------- */

static inline de_audio *test_audio_manual(u32 voices, u32 commands) {
    de_audio_desc d;
    memset(&d, 0, sizeof d);
    d.backend = DE_AUDIO_NULL;
    d.max_voices = voices;
    d.command_capacity = commands;
    de__audio_create_opts o = {true};
    de_audio *a = NULL;
    de_result r = de__audio_create_ex(&d, &o, &a);
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(a != NULL);
    return a;
}

static inline de_audio *test_audio_realtime(u32 voices, u32 commands) {
    de_audio_desc d;
    memset(&d, 0, sizeof d);
    d.backend = DE_AUDIO_NULL;
    d.max_voices = voices;
    d.command_capacity = commands;
    de_audio *a = NULL;
    de_result r = de_audio_create(&d, &a);
    ASSERT_EQ(r, DE_OK);
    ASSERT_TRUE(a != NULL);
    return a;
}

static inline u64 test_frames_mixed(const de_audio *a) {
    return de_atomic_load_u64(&a->frames_mixed, DE_ATOMIC_RELAXED);
}

/* Waits (2 ms OS sleeps, bounded by `timeout_ms`) until `pred(a, arg)` holds. */
typedef bool (*test_pred_fn)(de_audio *a, u64 arg);
static inline bool test_wait(de_audio *a, test_pred_fn pred, u64 arg, u32 timeout_ms) {
    const u64 end = de_time_now_ns() + (u64)timeout_ms * 1000000ull;
    while (!pred(a, arg)) {
        if (de_time_now_ns() > end)
            return false;
        de_sleep_ns(2000000ull);
    }
    return true;
}

static inline bool test_pred_mixed_at_least(de_audio *a, u64 frames) {
    return test_frames_mixed(a) >= frames;
}

#endif /* DE_AUDIO_TEST_UTIL_H */
