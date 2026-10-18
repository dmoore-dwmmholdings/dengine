/* Smoke test for de_tp_dr_libs: write a tiny WAV in memory with dr_wav and read it back, and check
 * that dr_flac links and rejects data that is not FLAC. */
#include <dr_flac.h>
#include <dr_wav.h>

#include <string.h>

#include "de_test.h"

TEST(de_tp_smoke_dr_libs, wav_roundtrip_in_memory) {
    const drwav_int16 frames[4][2] = {{0, 1}, {100, -100}, {32767, -32768}, {-5, 5}};
    void *buf = NULL;
    size_t size = 0;
    drwav_data_format fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = 2;
    fmt.sampleRate = 22050;
    fmt.bitsPerSample = 16;
    drwav w;
    ASSERT_TRUE(drwav_init_memory_write(&w, &buf, &size, &fmt, NULL));
    EXPECT_EQ(drwav_write_pcm_frames(&w, 4, frames), (drwav_uint64)4);
    drwav_uninit(&w);
    ASSERT_TRUE(buf != NULL);

    drwav r;
    ASSERT_TRUE(drwav_init_memory(&r, buf, size, NULL));
    EXPECT_EQ(r.channels, 2u);
    EXPECT_EQ(r.sampleRate, 22050u);
    EXPECT_EQ(r.totalPCMFrameCount, (drwav_uint64)4);
    drwav_int16 back[4][2];
    EXPECT_EQ(drwav_read_pcm_frames_s16(&r, 4, &back[0][0]), (drwav_uint64)4);
    EXPECT_EQ(memcmp(back, frames, sizeof back), 0);
    drwav_uninit(&r);
    drwav_free(buf, NULL);
}

TEST(de_tp_smoke_dr_libs, flac_rejects_non_flac) {
    static const unsigned char junk[64] = {'R', 'I', 'F', 'F'};
    drflac *f = drflac_open_memory(junk, sizeof junk, NULL);
    EXPECT_TRUE(f == NULL);
    if (f != NULL)
        drflac_close(f);
}
