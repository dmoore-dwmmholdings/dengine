/* Smoke test for de_tp_opus: encode a few frames of a stereo tone and decode them again, twice,
 * and check that the two encodes give the same bytes (the fixed-point build is deterministic). */
#include <opus.h>
#include <opus_multistream.h>

#include <string.h>

#include "de_test.h"

enum { k_rate = 48000, k_frame = 960, k_frames = 10, k_max_packet = 1500 };

/* A deterministic integer tone (a triangle wave, so no libm). */
static void fill_tone(opus_int16 *pcm, int frame0) {
    for (int i = 0; i < k_frame; ++i) {
        int t = (frame0 + i) % 100;
        int v = t < 50 ? t * 400 - 10000 : (100 - t) * 400 - 10000;
        pcm[2 * i] = (opus_int16)v;
        pcm[2 * i + 1] = (opus_int16)(-v / 2);
    }
}

static int encode_all(unsigned char packets[k_frames][k_max_packet], int sizes[k_frames]) {
    int err = 0;
    OpusEncoder *enc = opus_encoder_create(k_rate, 2, OPUS_APPLICATION_AUDIO, &err);
    if (enc == NULL || err != OPUS_OK)
        return -1;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(96000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));
    opus_int16 pcm[k_frame * 2];
    for (int f = 0; f < k_frames; ++f) {
        fill_tone(pcm, f * k_frame);
        sizes[f] = opus_encode(enc, pcm, k_frame, packets[f], k_max_packet);
        if (sizes[f] <= 0) {
            opus_encoder_destroy(enc);
            return -1;
        }
    }
    opus_encoder_destroy(enc);
    return 0;
}

static unsigned char g_a[k_frames][k_max_packet];
static unsigned char g_b[k_frames][k_max_packet];

TEST(de_tp_smoke_opus, encode_decode_deterministic) {
    EXPECT_TRUE(strncmp(opus_get_version_string(), "libopus", 7) == 0);
    int sa[k_frames], sb[k_frames];
    ASSERT_EQ(encode_all(g_a, sa), 0);
    ASSERT_EQ(encode_all(g_b, sb), 0);
    for (int f = 0; f < k_frames; ++f) {
        ASSERT_EQ(sa[f], sb[f]);
        EXPECT_EQ(memcmp(g_a[f], g_b[f], (size_t)sa[f]), 0);
    }

    int err = 0;
    OpusDecoder *dec = opus_decoder_create(k_rate, 2, &err);
    ASSERT_TRUE(dec != NULL);
    opus_int16 out[k_frame * 2];
    for (int f = 0; f < k_frames; ++f)
        EXPECT_EQ(opus_decode(dec, g_a[f], sa[f], out, k_frame, 0), k_frame);
    opus_decoder_destroy(dec);
}

TEST(de_tp_smoke_opus, multistream_surround_create) {
    int err = 0, streams = 0, coupled = 0;
    unsigned char mapping[6];
    OpusMSEncoder *enc = opus_multistream_surround_encoder_create(
        k_rate, 6, 1, &streams, &coupled, mapping, OPUS_APPLICATION_AUDIO, &err);
    ASSERT_TRUE(enc != NULL);
    EXPECT_EQ(err, OPUS_OK);
    EXPECT_EQ(streams, 4);
    EXPECT_EQ(coupled, 2);
    opus_multistream_encoder_destroy(enc);
}
