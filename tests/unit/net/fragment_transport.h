#ifndef DE_TEST_FRAGMENT_TRANSPORT_H
#define DE_TEST_FRAGMENT_TRANSPORT_H
#include "de_test.h"
#include "net/channel.h"
#include "net/connection.h"
#include "net/net_sim.h"
#include <string.h>
#if defined(DE_DEV) || defined(DE_DEBUG)
typedef struct fragment_rig fragment_rig;
typedef struct fragment_link {
    fragment_rig *rig;
    u32 side;
} fragment_link;
typedef struct fragment_end {
    u32 count;
    u8 detail;
} fragment_end;
struct fragment_rig {
    u64 now;
    de__loopback_net *net;
    de__socket *socket[2];
    de__net_client *client;
    de__net_server *server;
    de__conn *channel[2];
    fragment_link link[2];
    fragment_end ended[2];
    bool drop_unreliable_part;
    u32 parts_dropped;
};
static inline u64 fragment_now(void *p) {
    return *(u64 *)p;
}
static inline u64 fragment_unix(void *p) {
    (void)p;
    return 1800000000ull;
}
static inline void fragment_disconnected(void *p, u32 slot, de__net_end_reason why, u8 detail) {
    (void)slot;
    (void)why;
    fragment_end *e = p;
    e->count++;
    e->detail = detail;
}
static inline de_result fragment_encrypted_send(void *p, const void *data, u32 bytes) {
    const fragment_link *l = p;
    fragment_rig *r = l->rig;
    if (r->drop_unreliable_part) {
        de__chan_packet packet;
        ASSERT_TRUE(de__chan_packet_read(data, bytes, &packet));
        for (u32 i = 0; i < packet.count; ++i) {
            const de__chan_message_view *m = &packet.messages[i];
            de__fragment_view f;
            if (m->channel == DE_CHANNEL_UNRELIABLE && m->fragmented &&
                de__fragment_read(m->data, m->bytes, DE__FRAGMENT_MAX_MESSAGE, &f) &&
                f.index == 1) {
                r->parts_dropped++;
                return DE_OK;
            }
        }
    }
    return l->side ? de__net_server_send(r->server, 0, data, bytes)
                   : de__net_client_send(r->client, data, bytes);
}
static inline void fragment_rig_open(fragment_rig *r, u32 receiver_pool, bool fast) {
    memset(r, 0, sizeof *r);
    const de__loopback_desc ld = {.queue_capacity = 1024};
    ASSERT_EQ(de__loopback_net_create(&ld, NULL, &r->net), DE_OK);
    for (u32 i = 0; i < 2; ++i) {
        de__socket *inner = NULL;
        ASSERT_EQ(de__loopback_open(r->net, NULL, &inner), DE_OK);
        const de__net_sim_desc sd = {.queue_capacity = 1024,
                                     .seed = 54 + i,
                                     .now_ns = fragment_now,
                                     .clock_user = &r->now,
                                     .max_datagram = 1400};
        ASSERT_EQ(de__net_sim_wrap_ex(inner, &sd, NULL, &r->socket[i]), DE_OK);
    }
    const de__net_clock clock = {.now_ns = fragment_now, .unix_s = fragment_unix, .user = &r->now};
    const de__net_server_desc sd = {
        .socket = r->socket[1],
        .protocol_id = 54,
        .clock = clock,
        .callbacks = {.disconnected = fragment_disconnected, .user = &r->ended[1]}};
    const de__net_client_desc cd = {
        .socket = r->socket[0],
        .protocol_id = 54,
        .clock = clock,
        .callbacks = {.disconnected = fragment_disconnected, .user = &r->ended[0]}};
    ASSERT_EQ(de__net_server_create(&sd, &r->server), DE_OK);
    ASSERT_EQ(de__net_client_create(&cd, &r->client), DE_OK);
    ASSERT_EQ(de__net_client_connect(r->client, &r->socket[1]->local, NULL), DE_OK);
    for (u32 i = 0; i < 4; ++i) {
        de__net_server_update(r->server);
        de__net_client_update(r->client);
    }
    ASSERT_EQ(de__net_client_get_state(r->client), DE__NET_CLIENT_CONNECTED);
    ASSERT_FALSE(de__net_client_plaintext(r->client));
    for (u32 i = 0; i < 2; ++i) {
        r->link[i] = (fragment_link){r, i};
        const de__chan_desc d = {.send_packet = fragment_encrypted_send,
                                 .send_user = &r->link[i],
                                 .now_ns = fragment_now,
                                 .clock_user = &r->now,
                                 .reassembly_pool_bytes = i ? receiver_pool : 0,
                                 .max_bandwidth_kbps = fast ? 1000000 : 0,
                                 .tick_rate_hz = fast ? 1000 : 0};
        ASSERT_EQ(de__chan_create(&d, &r->channel[i]), DE_OK);
    }
    ASSERT_EQ(de__net_client_bind_channels(r->client, r->channel[0]), DE_OK);
    ASSERT_EQ(de__net_server_bind_channels(r->server, 0, r->channel[1]), DE_OK);
}
static inline de_result fragment_tick(fragment_rig *r, de__chan_message_fn fn, void *user) {
    for (u32 i = 0; i < 2; ++i)
        ASSERT_EQ(de__net_sim_pump(r->socket[i]), DE_OK);
    de__net_server_update(r->server);
    de__net_client_update(r->client);
    de_result result = DE_OK;
    for (u32 i = 0; i < 2; ++i) {
        const de_result sent = de__conn_write_packets(r->channel[i], r->now);
        if (sent != DE_OK)
            result = sent;
    }
    de__net_server_update(r->server);
    de__net_client_update(r->client);
    for (u32 i = 0; i < 2; ++i)
        de__chan_receive(r->channel[i], fn, user);
    r->now += 1000000ull;
    return result;
}
static inline void fragment_rig_close(fragment_rig *r) {
    ASSERT_EQ(de__net_client_bind_channels(r->client, NULL), DE_OK);
    ASSERT_EQ(de__net_server_bind_channels(r->server, 0, NULL), DE_OK);
    for (u32 i = 0; i < 2; ++i)
        de__chan_destroy(r->channel[i]);
    de__net_client_destroy(r->client);
    de__net_server_destroy(r->server);
    for (u32 i = 0; i < 2; ++i)
        de__socket_close(r->socket[i]);
    de__loopback_net_destroy(r->net);
}
#endif
#endif
