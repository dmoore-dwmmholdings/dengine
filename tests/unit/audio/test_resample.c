#include "de_test.h"
#include "audio/dsp/resample.h"
#include <dengine/de_math.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#define FRAMES 44100u
static f32 source_pcm[FRAMES], whole[50000], chunked[50000];
static f64 gain(f64 frequency, u32 in_rate, u32 out_rate) {
    for (u32 i = 0; i < FRAMES; ++i)
        source_pcm[i] = (f32)sin(2 * 3.141592653589793 * frequency * i / in_rate);
    f32 *out = NULL;
    u64 frames = 0;
    const de_allocator *a = de_heap_allocator();
    ASSERT_EQ(de__resample_buffer(a, source_pcm, FRAMES, 1, in_rate, out_rate, &out, &frames),
              DE_OK);
    f64 sum = 0;
    for (u64 i = 500; i + 500 < frames; ++i)
        sum += (f64)out[i] * out[i];
    f64 result = sqrt(sum / (f64)(frames - 1000) * 2);
    DE_FREE(a, out, (size_t)frames * 4);
    return result;
}
TEST(audio_resample, passband_ripple_to_twenty_khz) {
    const f64 freq[] = {100, 1000, 5000, 10000, 15000, 19000, 20000};
    for (u32 i = 0; i < 7; ++i) {
        f64 g = gain(freq[i], 44100, 48000);
        EXPECT_TRUE(fabs(20 * log10(g)) < 0.1);
    }
}
TEST(audio_resample, rejection_above_output_nyquist) {
    f64 g = gain(23000, 48000, 44100);
    EXPECT_TRUE(20 * log10(g) < -80);
}
TEST(audio_resample, streaming_equals_offline_and_allocates_nothing) {
    const de_allocator *heap = de_heap_allocator();
    for (u32 i = 0; i < FRAMES; ++i)
        source_pcm[i] = (f32)sin(i * 0.23);
    f32 *out = NULL;
    u64 count = 0;
    ASSERT_EQ(de__resample_buffer(heap, source_pcm, FRAMES, 1, 44100, 48000, &out, &count), DE_OK);
    de_tracking_allocator tracking;
    de_allocator a = de_tracking_allocator_init(&tracking, NULL, DE_MEM_AUDIO);
    de__resampler *s = NULL;
    ASSERT_EQ(de__resample_create(&a, 1, 44100.0 / 48000, &s), DE_OK);
    de_tracking_stats before = de_tracking_allocator_stats(&tracking);
    u32 pushed = 0, written = 0;
    while (written < count) {
        if (pushed < FRAMES) {
            u32 n = FRAMES - pushed;
            if (n > 137)
                n = 137;
            pushed += de__resample_push(s, source_pcm + pushed, n);
        }
        if (pushed == FRAMES)
            de__resample_finish(s);
        written += de__resample_read(s, chunked + written, 89);
    }
    EXPECT_EQ(written, count);
    for (u32 i = 0; i < written && i < count; ++i)
        EXPECT_FLOAT_EQ(out[i], chunked[i], 1e-6);
    de_tracking_stats after = de_tracking_allocator_stats(&tracking);
    EXPECT_EQ(before.alloc_count, after.alloc_count);
    EXPECT_EQ(before.realloc_count, after.realloc_count);
    de__resample_destroy(s);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
    DE_FREE(heap, out, (size_t)count * 4);
}
TEST(audio_resample, ramped_ratio_changes_and_reset) {
    for (u32 i = 0; i < FRAMES; ++i)
        source_pcm[i] = 0.25f;
    de__resampler *s = NULL;
    ASSERT_EQ(de__resample_create(NULL, 1, 1, &s), DE_OK);
    EXPECT_EQ(de__resample_push(s, source_pcm, 1000), 1000u);
    EXPECT_EQ(de__resample_read(s, whole, 200), 200u);
    de__resample_ratio(s, 1.2, 200);
    EXPECT_EQ(de__resample_read(s, whole, 400), 400u);
    for (u32 i = 0; i < 400; ++i)
        EXPECT_FLOAT_EQ(whole[i], 0.25, 1e-6);
    de__resample_reset(s);
    EXPECT_EQ(de__resample_read(s, whole, 1), 0u);
    de__resample_destroy(s);
}
