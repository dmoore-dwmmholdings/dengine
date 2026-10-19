#include "audio_test_util.h"
#include "audio/backend/linux_dl.h"
#include <stdlib.h>
TEST(audio_linux, missing_library_or_symbol_leaves_no_callable_partial_table) {
    void *handle = NULL;
    void (*first)(void) = NULL, (*second)(void) = NULL;
    de__linux_symbol missing = {"malloc", &first, sizeof first};
    EXPECT_FALSE(de__linux_load("/definitely/missing/dengine-audio.so", &missing, 1, &handle));
    EXPECT_TRUE(handle == NULL && first == NULL);
#if defined(__APPLE__)
    const char *system = "/usr/lib/libSystem.B.dylib";
#else
    const char *system = "libc.so.6";
#endif
    de__linux_symbol symbols[] = {{"malloc", &first, sizeof first},
                                  {"de_audio_missing_symbol", &second, sizeof second}};
    EXPECT_FALSE(de__linux_load(system, symbols, 2, &handle));
    EXPECT_TRUE(handle == NULL && first == NULL && second == NULL);
    ASSERT_TRUE(de__linux_load(system, &missing, 1, &handle));
    EXPECT_TRUE(first != NULL);
    de__linux_unload(handle);
}
#if defined(__linux__) && DE_AUDIO_HAS_ALSA
typedef struct render_count {
    de_atomic_u64 frames;
    de_atomic_u32 xruns;
} render_count;
static void silence(void *user, f32 *out, u32 frames) {
    render_count *c = user;
    memset(out, 0, (size_t)frames * 2 * sizeof(f32));
    de_atomic_fetch_add_u64(&c->frames, frames, DE_ATOMIC_RELAXED);
}
static void xrun(void *user) {
    render_count *c = user;
    de_atomic_fetch_add_u32(&c->xruns, 1, DE_ATOMIC_RELAXED);
}
static void alsa_null(bool inject) {
    de_audio *guard = test_audio_manual(2, 0);
    de__audio_backend_desc d = {0};
    d.allocator = de_heap_allocator();
    void *b = NULL;
    if (de__audio_backend_alsa.create(&d, &b) != DE_OK) {
        de_audio_destroy(guard);
        SKIP("ALSA runtime library unavailable");
    }
    render_count count;
    de_atomic_init_u64(&count.frames, 0);
    de_atomic_init_u32(&count.xruns, 0);
    de__audio_stream_desc desc = {48000, 2, 256, silence, &count, xrun};
    de__audio_stream *stream = NULL;
    ASSERT_EQ(de__audio_backend_alsa.open(b, "null", &desc, &stream), DE_OK);
    u64 hits = de__rt_guard_hits();
    ASSERT_EQ(stream->vt->start(stream), DE_OK);
    if (inject)
        de__alsa_test_xrun(stream);
    de_sleep_ns(1000000000ull);
    stream->vt->stop(stream);
    EXPECT_TRUE(de_atomic_load_u64(&count.frames, DE_ATOMIC_RELAXED) >= 47000u);
    EXPECT_TRUE(de_atomic_load_u64(&count.frames, DE_ATOMIC_RELAXED) < 60000u);
    EXPECT_EQ(de_atomic_load_u32(&count.xruns, DE_ATOMIC_RELAXED), inject ? 1u : 0u);
    EXPECT_EQ(de__rt_guard_hits(), hits);
    stream->vt->close(stream);
    de__audio_backend_alsa.destroy(b);
    de_audio_destroy(guard);
}
#endif
TEST(audio_linux, alsa_null_pcm_one_second_and_zero_xruns) {
#if defined(__linux__) && DE_AUDIO_HAS_ALSA
    alsa_null(false);
#else
    SKIP("ALSA null PCM runtime requires Linux");
#endif
}
TEST(audio_linux, alsa_fault_injected_xrun_recovers_and_is_counted) {
#if defined(__linux__) && DE_AUDIO_HAS_ALSA
    alsa_null(true);
#else
    SKIP("ALSA XRUN runtime requires Linux");
#endif
}
TEST(audio_linux, selection_order_and_missing_library_fallback) {
#if defined(__linux__)
    de__linux_test_missing(3);
    de_audio_desc d = {0};
    d.max_voices = 2;
    de_audio *a = NULL;
    ASSERT_EQ(de_audio_create(&d, &a), DE_OK);
    EXPECT_EQ(a->dev.vt->kind, DE_AUDIO_NULL);
    de_audio_destroy(a);
    de__linux_test_missing(1);
    ASSERT_EQ(de_audio_create(&d, &a), DE_OK);
    EXPECT_TRUE(a->dev.vt->kind == DE_AUDIO_ALSA || a->dev.vt->kind == DE_AUDIO_NULL);
    de_audio_destroy(a);
    de__linux_test_missing(0);
#else
    SKIP("OS backend selection requires Linux");
#endif
}
TEST(audio_linux, pipewire_dummy_sink_and_rt_callback) {
#if defined(__linux__) && DE_AUDIO_HAS_PIPEWIRE
    de_audio_desc d = {0};
    d.backend = DE_AUDIO_PIPEWIRE;
    d.max_voices = 2;
    de_audio *a = NULL;
    de_result result = de_audio_create(&d, &a);
    if (result == DE_ERR_UNSUPPORTED)
        SKIP("PipeWire library or daemon unavailable (dummy sink not configured)");
    ASSERT_EQ(result, DE_OK);
    if (!de_audio_list_devices(a, NULL, 0)) {
        de_audio_destroy(a);
        SKIP("PipeWire has no sink (configure a dummy sink)");
    }
    EXPECT_FALSE(a->dev.stream_is_fallback);
    u64 hits = de__rt_guard_hits(), clock = de_audio_clock(a);
    de_sleep_ns(1000000000ull);
    EXPECT_TRUE(de_audio_clock(a) - clock >= 43200u);
    EXPECT_EQ(de__rt_guard_hits(), hits);
    de_audio_destroy(a);
#else
    SKIP("PipeWire dummy sink runtime requires Linux");
#endif
}
