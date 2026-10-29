#include "de_test.h"
#include "net/packet.h"
#include <string.h>

TEST(net_channels, packet_codec_bounds_and_canonical_lengths) {
    u8 data[DE__CHAN_PACKET_MAX] = {0}, payload[DE__CHAN_MESSAGE_MAX];
    memset(payload, 0xa5, sizeof payload);
    de__chan_packet_header(data, 65535, true, 17, 0x80000001u);
    u32 n = de__chan_packet_message(data + DE__CHAN_HEADER, sizeof data - DE__CHAN_HEADER,
                                    DE_CHANNEL_RELIABLE_ORDERED, 65535, payload, sizeof payload);
    ASSERT_EQ(n + DE__CHAN_HEADER, sizeof data);
    de__chan_packet p;
    ASSERT_TRUE(de__chan_packet_read(data, sizeof data, &p));
    ASSERT_EQ(p.sequence, 65535u);
    ASSERT_EQ(p.ack_bits, 0x80000001u);
    ASSERT_EQ(p.count, 1u);
    ASSERT_EQ(p.messages[0].id, 65535u);
    ASSERT_EQ(p.messages[0].bytes, sizeof payload);
    ASSERT_TRUE(memcmp(p.messages[0].data, payload, sizeof payload) == 0);
    ASSERT_EQ(de__chan_packet_message(data, sizeof data, (de_channel)3, 0, payload, 1), 0u);
    ASSERT_EQ(de__chan_packet_message(data, 4, DE_CHANNEL_UNRELIABLE, 0, payload, 1), 0u);
    data[8] |= 2;
    ASSERT_FALSE(de__chan_packet_read(data, sizeof data, &p));
    de__chan_packet_header(data, 0, false, 0, 0);
    data[9] = 0;
    data[10] = 0;
    data[11] = 0;
    data[12] = 0x81;
    data[13] = 0;
    data[14] = 42;
    ASSERT_FALSE(de__chan_packet_read(data, 15, &p)); // nonminimal length
    data[12] = 2;
    ASSERT_FALSE(de__chan_packet_read(data, 14, &p)); // truncated body
    data[12] = 0;
    ASSERT_FALSE(de__chan_packet_read(data, 14, &p)); // empty message
    data[2] = 1;
    ASSERT_FALSE(de__chan_packet_read(data, DE__CHAN_HEADER, &p)); // absent ack must be canonical
}

TEST(net_channels, packet_fills_minimum_messages_and_accepts_ack_only) {
    u8 data[DE__CHAN_PACKET_MAX] = {0}, value = 7;
    de__chan_packet_header(data, 0, false, 99, UINT32_MAX);
    de__chan_packet p;
    ASSERT_TRUE(de__chan_packet_read(data, DE__CHAN_HEADER, &p));
    ASSERT_FALSE(p.has_ack);
    ASSERT_EQ(p.count, 0u);
    u32 bytes = DE__CHAN_HEADER;
    for (u32 i = 0; i < DE__CHAN_PACKET_MESSAGES; ++i)
        bytes += de__chan_packet_message(data + bytes, sizeof data - bytes, DE_CHANNEL_UNRELIABLE,
                                         (u16)i, &value, 1);
    ASSERT_EQ(bytes, 1199u);
    ASSERT_TRUE(de__chan_packet_read(data, bytes, &p));
    ASSERT_EQ(p.count, DE__CHAN_PACKET_MESSAGES);
    ASSERT_EQ(de__chan_packet_message(data + bytes, sizeof data - bytes, DE_CHANNEL_UNRELIABLE, 0,
                                      &value, 1),
              0u);
}
