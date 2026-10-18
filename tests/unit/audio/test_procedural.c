#include "audio_test_util.h"
#include <math.h>
typedef struct generator {
    u64 sample;
    u32 calls, last_frames;
} generator;
static void generate(f32 *out, u32 frames, u32 channels, void *user) {
    generator *g = user;
    ++g->calls;
    g->last_frames = frames;
    for (u32 i = 0; i < frames; ++i) {
        f32 value = (f32)sin(2 * 3.141592653589793 * 997 * g->sample++ / 48000);
        for (u32 c = 0; c < channels; ++c)
            out[i * channels + c] = value;
    }
}
TEST(audio_procedural, exact_997_hz_and_block_contract) {
    de_audio *a = test_audio_manual(4, 0);
    generator g = {0};
    de_voice v = de_audio_play_procedural(a, generate, &g, 2, (de_bus){0});
    ASSERT_TRUE(v.id != 0);
    f32 out[2048 * 2];
    u64 hits = de__rt_guard_hits();
    de__audio_null_pump(a, 2048, out);
    EXPECT_EQ(de__rt_guard_hits(), hits);
    EXPECT_EQ(g.calls, 8u);
    EXPECT_EQ(g.last_frames, 256u);
    for (u32 i = 0; i < 2048; ++i)
        EXPECT_FLOAT_EQ(out[2 * i], (f32)sin(2 * 3.141592653589793 * 997 * i / 48000), 0);
    de_audio_destroy(a);
}
