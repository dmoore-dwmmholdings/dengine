#include "de_test.h"
#include "de_alloc_guard.h"
#include "net/channel.h"
#include "net/congestion.h"
#include "net/connection.h"
#include "net/net_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS    1000000ull
#define TOTAL (2u * 65536u + 513u)
typedef struct capture {
    u8 data[4][DE__CHAN_PACKET_MAX];
    u32 bytes[4], count;
    bool fail;
} capture;
static de_result capture_send(void *user, const void *data, u32 bytes) {
    capture *c = user;
    if (c->fail)
        return DE_ERR_BUSY;
    ASSERT_TRUE(bytes <= DE__CHAN_PACKET_MAX);
    const u32 at = c->count++ % 4u;
    c->bytes[at] = bytes;
    memcpy(c->data[at], data, bytes);
    return DE_OK;
}
static u64 fake_now(void *user) {
    return *(u64 *)user;
}
static de__conn *captured(capture *out, u64 *now) {
    de__conn *c = NULL;
    const de__chan_desc d = {.send_packet = capture_send,
                             .send_user = out,
                             .now_ns = fake_now,
                             .clock_user = now,
                             .max_bandwidth_kbps = 1000000,
                             .tick_rate_hz = 1000000};
    ASSERT_EQ(de__chan_create(&d, &c), DE_OK);
    // Codec/window cases assume a warmed high-capacity link; default startup
    // pacing is qualified separately by test_net_congestion.
    de__conn_cc(c)->rate = de__conn_cc_view(c)->cap;
    return c;
}
static void ack(de__conn *c, u16 peer_sequence, u16 sequence, u32 bits) {
    u8 data[DE__CHAN_HEADER];
    de__chan_packet_header(data, peer_sequence, true, sequence, bits);
    ASSERT_EQ(de__conn_read_payload(c, data, sizeof data), DE_OK);
}
static u32 packet(u8 *data, u16 sequence, de_channel ch, u16 id, const void *body, u32 n) {
    de__chan_packet_header(data, sequence, false, 0, 0);
    const u32 m = de__chan_packet_message(data + DE__CHAN_HEADER,
                                          DE__CHAN_PACKET_MAX - DE__CHAN_HEADER, ch, id, body, n);
    ASSERT_TRUE(m != 0);
    return DE__CHAN_HEADER + m;
}
static void count_message(void *user, de_channel ch, const void *data, u32 bytes) {
    (void)ch;
    (void)data;
    (void)bytes;
    (*(u32 *)user)++;
}

TEST(net_channels, assembly_prioritizes_resends_then_new_reliable_then_unreliable) {
    capture out = {0};
    u64 now = 0;
    de__conn *c = captured(&out, &now);
    u8 body[DE__CHAN_MESSAGE_MAX];
    memset(body, 4, sizeof body);
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_UNRELIABLE, body, sizeof body), DE_OK);
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_RELIABLE_ORDERED, body, sizeof body), DE_OK);
    memset(body, 5, sizeof body); // queued bytes belong to the connection, not this buffer
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
    ASSERT_EQ(out.count, 2u);
    de__chan_packet p;
    ASSERT_TRUE(de__chan_packet_read(out.data[0], out.bytes[0], &p));
    ASSERT_EQ(p.messages[0].channel, DE_CHANNEL_RELIABLE_ORDERED);
    ASSERT_EQ(p.messages[0].data[0], 4u);
    ASSERT_EQ(out.bytes[0], 1200u);
    ASSERT_TRUE(de__chan_packet_read(out.data[1], out.bytes[1], &p));
    ASSERT_EQ(p.messages[0].channel, DE_CHANNEL_UNRELIABLE);
    out.count = 0;
    now = 31 * MS;
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_RELIABLE_ORDERED, body, 1), DE_OK);
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_UNRELIABLE, body, sizeof body), DE_OK);
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
    ASSERT_EQ(out.count, 3u);
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_TRUE(de__chan_packet_read(out.data[i], out.bytes[i], &p));
        ASSERT_EQ(p.count, 1u);
        ASSERT_EQ(p.messages[0].channel,
                  i == 2 ? DE_CHANNEL_UNRELIABLE : DE_CHANNEL_RELIABLE_ORDERED);
        ASSERT_EQ(p.messages[0].id, i == 0 ? 0u : 1u);
        ASSERT_EQ(p.messages[0].data[0], i == 0 ? 4u : 5u);
    }
    const de__chan_stats st = de__chan_get_stats(c);
    ASSERT_EQ(st.channel[1].msgs_out, 2u);
    ASSERT_EQ(st.channel[1].bytes_out, sizeof body + 1u);
    ASSERT_EQ(st.channel[1].resends, 1u);
    de__chan_destroy(c);
}

TEST(net_channels, rejects_invalid_send_and_bounds_send_storage_and_failed_flush) {
    capture out = {0};
    u64 now = 0;
    u8 value = 1;
    de__conn *c = captured(&out, &now);
    ASSERT_TRUE(de__chan_get_stats(c).allocated_bytes <= 5u * 1024u * 1024u);
    ASSERT_EQ(de__chan_send(c, (de_channel)3, &value, 1), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_UNRELIABLE, NULL, 1), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_UNRELIABLE, &value, 0), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_UNRELIABLE, &value, DE__CHAN_MESSAGE_MAX + 1u),
              DE_ERR_INVALID_ARGUMENT);
    for (u32 i = 0; i < DE__CHAN_WINDOW; ++i) {
        ASSERT_EQ(de__chan_send(c, DE_CHANNEL_RELIABLE_ORDERED, &value, 1), DE_OK);
        ASSERT_EQ(de__chan_send(c, DE_CHANNEL_UNRELIABLE, &value, 1), DE_OK);
    }
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_RELIABLE_ORDERED, &value, 1), DE_ERR_BUSY);
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_UNRELIABLE, &value, 1), DE_OK);
    out.fail = true;
    ASSERT_EQ(de__conn_write_packets(c, now), DE_ERR_BUSY);
    de__chan_stats st = de__chan_get_stats(c);
    ASSERT_EQ(st.pending[1], 256u);
    ASSERT_EQ(st.channel[1].msgs_out, 0u);
    ASSERT_EQ(st.pending[0], 0u);
    ASSERT_EQ(st.channel[0].dropped, 257u);
    out.fail = false;
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
    ASSERT_EQ(de__chan_get_stats(c).channel[1].msgs_out, 256u);
    de__chan_reset(c);
    ASSERT_EQ(de__chan_get_stats(c).pending[1], 0u);
    de__chan_destroy(c);
}

TEST(net_channels, malformed_body_cannot_ack_or_partially_deliver_and_absent_ack_is_not_zero) {
    capture out = {0};
    u64 now = 0;
    u8 value = 9, data[DE__CHAN_PACKET_MAX];
    de__conn *c = captured(&out, &now);
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_RELIABLE_ORDERED, &value, 1), DE_OK);
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
    de__chan_packet_header(data, 0, false, 0, 0);
    ASSERT_EQ(de__conn_read_payload(c, data, DE__CHAN_HEADER), DE_OK);
    ASSERT_EQ(de__chan_get_stats(c).pending[1], 1u);
    const u32 bytes = packet(data, 1, DE_CHANNEL_RELIABLE_ORDERED, 0, &value, 1);
    de__chan_packet_header(data, 1, true, 0, 0);
    data[bytes] = 0; // malformed final message after a valid one
    ASSERT_EQ(de__conn_read_payload(c, data, bytes + 1), DE_ERR_PARSE);
    ASSERT_EQ(de__chan_get_stats(c).pending[1], 1u);
    u32 delivered = 0;
    de__chan_receive(c, count_message, &delivered);
    ASSERT_EQ(delivered, 0u);
    ASSERT_EQ(de__conn_read_payload(c, data, bytes), DE_OK);
    ASSERT_EQ(de__chan_get_stats(c).pending[1], 0u);
    de__chan_receive(c, count_message, &delivered);
    ASSERT_EQ(delivered, 1u);
    // ACK-only packets must not create an infinite acknowledgement ping-pong.
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
    const u32 sent = out.count;
    ack(c, 2, 0, 0);
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
    ASSERT_EQ(out.count, sent);
    de__chan_destroy(c);
}

TEST(net_channels, receive_backpressure_never_acknowledges_unretained_reliable_data) {
    for (u32 kind = 1; kind <= 2; ++kind) {
        capture out = {0};
        u64 now = 0;
        u8 value = 1, data[DE__CHAN_PACKET_MAX];
        de__conn *c = captured(&out, &now);
        for (u32 i = 0; i < 256; ++i) {
            const u32 n = packet(data, (u16)i, (de_channel)kind, (u16)i, &value, 1);
            ASSERT_EQ(de__conn_read_payload(c, data, n), DE_OK);
        }
        const u32 n = packet(data, 256, (de_channel)kind, 256, &value, 1);
        ASSERT_EQ(de__conn_read_payload(c, data, n), DE_ERR_BUSY);
        ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
        de__chan_packet p;
        ASSERT_TRUE(de__chan_packet_read(out.data[0], out.bytes[0], &p));
        ASSERT_EQ(p.ack, 255u);
        u32 delivered = 0;
        de__chan_receive(c, count_message, &delivered);
        ASSERT_EQ(delivered, 256u);
        ASSERT_EQ(de__conn_read_payload(c, data, n), DE_OK);
        de__chan_receive(c, count_message, &delivered);
        ASSERT_EQ(delivered, 257u);
        ASSERT_EQ(de__conn_read_payload(c, data, n), DE_OK);
        de__chan_receive(c, count_message, &delivered);
        ASSERT_EQ(delivered, 257u);
        de__chan_destroy(c);
    }
}

TEST(net_channels, stale_packet_ack_cannot_clear_reused_message_id_generation) {
    capture out = {0};
    u64 now = 0;
    u8 value = 1;
    de__conn *c = captured(&out, &now);
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_RELIABLE_ORDERED, &value, 1), DE_OK);
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK); // packet0 carries message0
    now = 31 * MS;
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK); // packet1 resends message0
    ack(c, 0, 1, 0); // packet0's ref stays in the sent ring, but message0 is free
    u32 sent = 1;
    u16 peer_sequence = 1;
    while (sent < 65536) {
        now += MS; // replenish the real token bucket; this case isolates ID-generation safety
        const u32 count =
            65536u - sent < DE__CHAN_PACKET_MESSAGES ? 65536u - sent : DE__CHAN_PACKET_MESSAGES;
        for (u32 i = 0; i < count; ++i)
            ASSERT_EQ(de__chan_send(c, DE_CHANNEL_RELIABLE_ORDERED, &value, 1), DE_OK);
        ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
        de__chan_packet p;
        const u32 at = (out.count - 1u) % 4u;
        ASSERT_TRUE(de__chan_packet_read(out.data[at], out.bytes[at], &p));
        ack(c, peer_sequence++, p.sequence, UINT32_MAX);
        sent += count;
    }
    ASSERT_TRUE(out.count < 1024u); // old packet0 is still a live record
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_RELIABLE_ORDERED, &value, 1),
              DE_OK); // id0, generation65536
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
    ack(c, peer_sequence, 0, 0);
    ASSERT_EQ(de__chan_get_stats(c).pending[1], 1u);
    de__chan_destroy(c);
}

TEST(net_channels, actual_sent_ring_accepts_every_selective_ack_bit) {
    for (u32 distance = 0; distance <= 32; ++distance) {
        capture out = {0};
        u64 now = 0;
        u8 value = 1;
        de__conn *c = captured(&out, &now);
        ASSERT_EQ(de__chan_send(c, DE_CHANNEL_RELIABLE_ORDERED, &value, 1), DE_OK);
        ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
        for (u32 i = 0; i < distance; ++i) {
            ASSERT_EQ(de__chan_send(c, DE_CHANNEL_UNRELIABLE, &value, 1), DE_OK);
            ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
        }
        if (distance) {
            ack(c, 0, (u16)distance, 0);
            ASSERT_EQ(de__chan_get_stats(c).pending[1], 1u);
        }
        ack(c, 1, (u16)distance, distance ? 1u << (distance - 1) : 0);
        ASSERT_EQ(de__chan_get_stats(c).pending[1], 0u);
        de__chan_destroy(c);
    }
}

#if defined(DE_DEV) || defined(DE_DEBUG)
/* Real encrypted client/server plus the DENG-56 decorator on both outgoing sockets. */
typedef struct link {
    de__net_server *server;
    de__net_client *client;
    bool from_server;
} link;
typedef struct rig {
    u64 now;
    u32 raw_received;
    de__loopback_net *net;
    de__socket *socket[2];
    de__net_server *server;
    de__net_client *client;
    de__conn *channel[2];
    link link[2];
} rig;
typedef struct record {
    u32 count[3], ordered;
    u8 seen[3][TOTAL];
} record;
static u64 fake_unix(void *user) {
    (void)user;
    return 1800000000ull;
}
static de_result encrypted_send(void *user, const void *data, u32 bytes) {
    link *l = user;
    return l->from_server ? de__net_server_send(l->server, 0, data, bytes)
                          : de__net_client_send(l->client, data, bytes);
}
static void raw_received(void *user, u32 slot, const u8 *data, u32 bytes) {
    rig *r = user;
    ASSERT_EQ(slot, 0u);
    ASSERT_EQ(bytes, 3u);
    ASSERT_TRUE(memcmp(data, "raw", 3) == 0);
    r->raw_received++;
}
static void open_rig(rig *r) {
    memset(r, 0, sizeof *r);
    const de__loopback_desc ld = {.queue_capacity = 512};
    ASSERT_EQ(de__loopback_net_create(&ld, NULL, &r->net), DE_OK);
    for (u32 i = 0; i < 2; ++i) {
        de__socket *inner = NULL;
        ASSERT_EQ(de__loopback_open(r->net, NULL, &inner), DE_OK);
        const de__net_sim_desc sd = {
            .queue_capacity = 512, .seed = 53u + i, .now_ns = fake_now, .clock_user = &r->now};
        ASSERT_EQ(de__net_sim_wrap_ex(inner, &sd, NULL, &r->socket[i]), DE_OK);
    }
    const de__net_clock clock = {.now_ns = fake_now, .unix_s = fake_unix, .user = &r->now};
    const de__net_server_desc sd = {.socket = r->socket[1],
                                    .protocol_id = 53,
                                    .clock = clock,
                                    .callbacks = {.payload = raw_received, .user = r}};
    const de__net_client_desc cd = {.socket = r->socket[0],
                                    .protocol_id = 53,
                                    .clock = clock,
                                    .callbacks = {.payload = raw_received, .user = r}};
    ASSERT_EQ(de__net_server_create(&sd, &r->server), DE_OK);
    ASSERT_EQ(de__net_client_create(&cd, &r->client), DE_OK);
    ASSERT_EQ(de__net_client_connect(r->client, &r->socket[1]->local, NULL), DE_OK);
    for (u32 i = 0; i < 4; ++i) {
        de__net_server_update(r->server);
        de__net_client_update(r->client);
    }
    ASSERT_EQ(de__net_client_get_state(r->client), DE__NET_CLIENT_CONNECTED);
    for (u32 i = 0; i < 2; ++i) {
        r->link[i] = (link){r->server, r->client, i == 1};
        const de__chan_desc d = {.send_packet = encrypted_send,
                                 .send_user = &r->link[i],
                                 .now_ns = fake_now,
                                 .clock_user = &r->now,
                                 .tick_rate_hz = 200};
        ASSERT_EQ(de__chan_create(&d, &r->channel[i]), DE_OK);
    }
    ASSERT_EQ(de__net_client_bind_channels(r->client, r->channel[0]), DE_OK);
    ASSERT_EQ(de__net_server_bind_channels(r->server, 0, r->channel[1]), DE_OK);
    ASSERT_EQ(de__net_client_bind_channels(r->client, r->channel[0]), DE_ERR_BUSY);
}
static void close_rig(rig *r) {
    ASSERT_EQ(de__net_client_bind_channels(r->client, NULL), DE_OK);
    ASSERT_EQ(de__net_server_bind_channels(r->server, 0, NULL), DE_OK);
    de__chan_destroy(r->channel[0]);
    de__chan_destroy(r->channel[1]);
    de__net_client_destroy(r->client);
    de__net_server_destroy(r->server);
    de__socket_close(r->socket[0]);
    de__socket_close(r->socket[1]);
    de__loopback_net_destroy(r->net);
}
typedef struct body {
    u32 id, canary;
} body;
static void record_message(void *user, de_channel ch, const void *data, u32 bytes) {
    record *r = user;
    body b;
    ASSERT_EQ(bytes, sizeof b);
    memcpy(&b, data, sizeof b);
    ASSERT_EQ(b.canary, 0x1234abcdu);
    ASSERT_TRUE(b.id < TOTAL);
    ASSERT_EQ(r->seen[ch][b.id], 0u);
    r->seen[ch][b.id] = 1;
    r->count[ch]++;
    if (ch == DE_CHANNEL_RELIABLE_ORDERED) {
        ASSERT_EQ(b.id, r->ordered);
        r->ordered++;
    }
}
static void tick(rig *r, record *a, record *b) {
    ASSERT_EQ(de__net_sim_pump(r->socket[0]), DE_OK);
    ASSERT_EQ(de__net_sim_pump(r->socket[1]), DE_OK);
    de__net_server_update(r->server);
    de__net_client_update(r->client);
    ASSERT_EQ(de__conn_write_packets(r->channel[0], r->now), DE_OK);
    ASSERT_EQ(de__conn_write_packets(r->channel[1], r->now), DE_OK);
    de__net_server_update(r->server);
    de__net_client_update(r->client);
    de__chan_receive(r->channel[0], record_message, a);
    de__chan_receive(r->channel[1], record_message, b);
    r->now += 5 * MS;
}

TEST(net_channels, reconnect_discards_bindings_and_fresh_bind_resets_pending_messages) {
    rig r;
    open_rig(&r);
    const u8 value = 9;
    ASSERT_EQ(de__chan_send(r.channel[0], DE_CHANNEL_RELIABLE_ORDERED, &value, 1), DE_OK);
    const u32 generation = de__net_server_slot(r.server, 0).generation;
    de__net_client_disconnect(r.client, 1);
    for (u32 i = 0; i < 4; ++i) {
        de__net_server_update(r.server);
        de__net_client_update(r.client);
        r.now += MS;
    }
    ASSERT_EQ(de__net_client_get_state(r.client), DE__NET_CLIENT_DISCONNECTED);
    ASSERT_EQ(de__net_client_connect(r.client, &r.socket[1]->local, NULL), DE_OK);
    for (u32 i = 0; i < 4; ++i) {
        de__net_server_update(r.server);
        de__net_client_update(r.client);
        r.now += MS;
    }
    ASSERT_EQ(de__net_client_get_state(r.client), DE__NET_CLIENT_CONNECTED);
    ASSERT_TRUE(de__net_server_slot(r.server, 0).generation != generation);
    ASSERT_EQ(de__net_client_send(r.client, "raw", 3), DE_OK);
    ASSERT_EQ(de__net_server_send(r.server, 0, "raw", 3), DE_OK);
    de__net_server_update(r.server);
    de__net_client_update(r.client);
    ASSERT_EQ(r.raw_received, 2u); // neither old non-owning pointer survived reconnect
    ASSERT_EQ(de__net_client_bind_channels(r.client, r.channel[0]), DE_OK);
    ASSERT_EQ(de__net_server_bind_channels(r.server, 0, r.channel[1]), DE_OK);
    ASSERT_EQ(de__chan_get_stats(r.channel[0]).pending[1], 0u);
    ASSERT_EQ(de__chan_send(r.channel[0], DE_CHANNEL_RELIABLE_ORDERED, &value, 1), DE_OK);
    ASSERT_EQ(de__conn_write_packets(r.channel[0], r.now), DE_OK);
    de__net_server_update(r.server);
    u32 delivered = 0;
    de__chan_receive(r.channel[1], count_message, &delivered);
    ASSERT_EQ(delivered, 1u);
    close_rig(&r);
}

TEST(net_channels, binding_cleanup_is_idempotent_during_disconnect_and_offline) {
    rig r;
    open_rig(&r);
    de__net_client_disconnect(r.client, 1);
    de__net_server_kick(r.server, 0, 1);
    ASSERT_EQ(de__net_client_bind_channels(r.client, NULL), DE_OK);
    ASSERT_EQ(de__net_server_bind_channels(r.server, 0, NULL), DE_OK);
    for (u32 i = 0; i < 4; ++i) {
        de__net_server_update(r.server);
        de__net_client_update(r.client);
        r.now += MS;
    }
    ASSERT_EQ(de__net_client_get_state(r.client), DE__NET_CLIENT_DISCONNECTED);
    ASSERT_EQ(de__net_server_slot(r.server, 0).state, DE__NET_SLOT_FREE);
    ASSERT_EQ(de__net_client_bind_channels(r.client, NULL), DE_OK);
    ASSERT_EQ(de__net_server_bind_channels(r.server, 0, NULL), DE_OK);
    ASSERT_EQ(de__net_client_bind_channels(r.client, r.channel[0]), DE_ERR_NOT_FOUND);
    ASSERT_EQ(de__net_server_bind_channels(r.server, 0, r.channel[1]), DE_ERR_NOT_FOUND);
    close_rig(&r);
}

TEST(net_channels, ten_thousand_encrypted_messages_under_loss_jitter_duplication_without_heap) {
    rig r;
    open_rig(&r);
    record *received[2] = {calloc(1, sizeof(record)), calloc(1, sizeof(record))};
    ASSERT_TRUE(received[0] && received[1]);
    const de_net_sim sim = {100, 20, .2f, .2f};
    for (u32 i = 0; i < 2; ++i)
        ASSERT_EQ(de__net_sim_set(r.socket[i], &sim), DE_OK);
    u32 sent[2][3] = {{0}};
    u64 at_two_seconds = 0;
    bool done = false;
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 step = 0; step < 120000; ++step) { // AIMD legitimately throttles sustained20% loss
        for (u32 side = 0; side < 2; ++side)
            for (u32 ch = 0; ch < 3; ++ch) {
                for (u32 n = 0; n < 16 && sent[side][ch] < 10000; ++n) {
                    const body b = {sent[side][ch], 0x1234abcd};
                    const de_result result =
                        de__chan_send(r.channel[side], (de_channel)ch, &b, sizeof b);
                    if (result == DE_ERR_BUSY)
                        break;
                    ASSERT_EQ(result, DE_OK);
                    sent[side][ch]++;
                }
            }
        tick(&r, received[0], received[1]);
        if (!at_two_seconds && r.now >= 2000 * MS)
            at_two_seconds = de__chan_get_stats(r.channel[0]).rtt_ns;
        done = true;
        for (u32 side = 0; side < 2; ++side) {
            const de__chan_stats st = de__chan_get_stats(r.channel[side]);
            if (received[side]->count[1] != 10000 || received[side]->count[2] != 10000 ||
                st.pending[1] || st.pending[2])
                done = false;
        }
        if (done)
            break;
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    ASSERT_TRUE(done);
    ASSERT_TRUE(at_two_seconds >= 180 * MS && at_two_seconds <= 220 * MS);
    for (u32 side = 0; side < 2; ++side) {
        const de__chan_stats st = de__chan_get_stats(r.channel[side]);
        for (u32 ch = 1; ch < 3; ++ch) {
            ASSERT_EQ(st.channel[ch].msgs_in, 10000u);
            ASSERT_EQ(st.channel[ch].msgs_out, 10000u);
            ASSERT_EQ(st.channel[ch].bytes_in, 10000u * sizeof(body));
            ASSERT_EQ(st.channel[ch].bytes_out, 10000u * sizeof(body));
            ASSERT_TRUE(st.channel[ch].resends > 0);
        }
        ASSERT_TRUE(received[side]->count[0] > 0 && received[side]->count[0] <= 10000);
        printf("side %u: ordered/unordered 10000 exactly once, unreliable %u, resends %llu/%llu, "
               "storage %zu bytes\n",
               side, received[side]->count[0], (unsigned long long)st.channel[1].resends,
               (unsigned long long)st.channel[2].resends, st.allocated_bytes);
    }
    printf("two-second RTT %.3f ms for 100+100 ms one-way delay, completed at %.3f s\n",
           (double)at_two_seconds / MS, (double)r.now / (1000 * MS));
    free(received[0]);
    free(received[1]);
    close_rig(&r);
}

TEST(net_channels, encrypted_packet_and_message_sequences_cross_wrap_twice) {
    rig r;
    open_rig(&r);
    record *received[2] = {calloc(1, sizeof(record)), calloc(1, sizeof(record))};
    ASSERT_TRUE(received[0] && received[1]);
    for (u32 id = 0; id < TOTAL; ++id) {
        const body b = {id, 0x1234abcd};
        for (u32 ch = 0; ch < 3; ++ch)
            ASSERT_EQ(de__chan_send(r.channel[0], (de_channel)ch, &b, sizeof b), DE_OK);
        tick(&r, received[0], received[1]);
    }
    tick(&r, received[0], received[1]);
    tick(&r, received[0], received[1]);
    for (u32 ch = 0; ch < 3; ++ch)
        ASSERT_EQ(received[1]->count[ch], TOTAL);
    ASSERT_TRUE(de__chan_get_stats(r.channel[0]).packets_out >= TOTAL);
    ASSERT_EQ(de__chan_get_stats(r.channel[0]).pending[1], 0u);
    ASSERT_EQ(de__chan_get_stats(r.channel[0]).pending[2], 0u);
    free(received[0]);
    free(received[1]);
    close_rig(&r);
}
#endif
