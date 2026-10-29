#include "de_test.h"
#include "net/packet.h"

TEST(net_channels, sequence_wrap_and_full_replay_window) {
    de__chan_sequence_window w = {0};
    for (u32 i = 0; i < 2u * 65536u + 1024u; ++i) {
        const u16 seq = (u16)i;
        ASSERT_FALSE(de__chan_sequence_seen(&w, seq));
        de__chan_sequence_mark(&w, seq);
        ASSERT_TRUE(de__chan_sequence_seen(&w, seq));
        if (i >= 1024)
            ASSERT_TRUE(de__chan_sequence_seen(&w, (u16)(seq - 1024u)));
    }
    ASSERT_TRUE(de__chan_sequence_newer(0, 65535));
    ASSERT_FALSE(de__chan_sequence_newer(65535, 0));
    ASSERT_FALSE(de__chan_sequence_newer(5, 5));
    // A large forward jump clears the bitmap, including whole-word shifts.
    w = (de__chan_sequence_window){0};
    de__chan_sequence_mark(&w, 1);
    de__chan_sequence_mark(&w, 1025);
    ASSERT_FALSE(de__chan_sequence_seen(&w, 1024));
    de__chan_sequence_mark(&w, 1024);
    ASSERT_EQ(de__chan_ack_bits(&w), 1u);
}

TEST(net_channels, acknowledgement_covers_all_33_positions_across_wrap) {
    for (u32 start = 65520; start < 65560; ++start) {
        for (u32 distance = 0; distance <= 32; ++distance) {
            const u16 seq = (u16)start, ack = (u16)(start + distance);
            const u32 bit = distance ? 1u << (distance - 1) : 0;
            ASSERT_TRUE(de__chan_acknowledges(seq, ack, bit));
            if (distance)
                ASSERT_FALSE(de__chan_acknowledges(seq, ack, 0));
        }
        ASSERT_FALSE(de__chan_acknowledges((u16)start, (u16)(start + 33), UINT32_MAX));
        de__chan_sequence_window w = {0};
        de__chan_sequence_mark(&w, (u16)start);
        for (u32 i = 1; i <= 32; ++i)
            de__chan_sequence_mark(&w, (u16)(start + i));
        ASSERT_EQ(de__chan_ack_bits(&w), UINT32_MAX);
    }
}

TEST(net_channels, rtt_estimator_converges_and_resend_saturates) {
    de__chan_rtt r = {0};
    ASSERT_EQ(de__chan_resend_ns(&r), 30000000ull);
    de__chan_rtt_sample(&r, 200000000ull);
    ASSERT_EQ(r.smoothed_ns, 200000000ull);
    ASSERT_EQ(r.variance_ns, 100000000ull);
    for (u32 i = 0; i < 100; ++i)
        de__chan_rtt_sample(&r, 200000000ull);
    ASSERT_TRUE(r.variance_ns < 1000);
    ASSERT_TRUE(de__chan_resend_ns(&r) >= 250000000ull);
    de__chan_rtt_sample(&r, UINT64_MAX);
    r.smoothed_ns = UINT64_MAX;
    ASSERT_EQ(de__chan_resend_ns(&r), UINT64_MAX);
}
