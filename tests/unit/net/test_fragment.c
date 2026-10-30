#include "de_test.h"
#include "de_alloc_guard.h"
#include "net/fragment.h"
#include "net/crypto.h"
#include "fragment_transport.h"
#include <stdio.h>
#include <string.h>

static u8 payload[DE__FRAGMENT_MAX_MESSAGE];
typedef struct delivered {
    const u8 *expected;
    u32 bytes, count;
    de_channel channel;
} delivered;
static void receive(void *user, de_channel ch, const void *data, u32 bytes) {
    delivered *d = user;
    ASSERT_EQ(ch, d->channel);
    ASSERT_EQ(bytes, d->bytes);
    ASSERT_EQ(memcmp(data, d->expected, bytes), 0);
    d->count++;
}
static de__fragment_pool *create(u32 pool_bytes) {
    de__fragment_pool *p = NULL;
    const de__fragment_desc desc = {.pool_bytes = pool_bytes};
    ASSERT_EQ(de__fragment_create(&desc, &p), DE_OK);
    return p;
}
static de_result push(de__fragment_pool *p, de_channel ch, u32 id, u16 index, u32 total, u64 now,
                      delivered *out) {
    u8 body[DE__FRAGMENT_HEADER + DE__FRAGMENT_SLICE];
    const u32 n = de__fragment_write(body, sizeof body, id, index, payload, total);
    ASSERT_TRUE(n != 0);
    return de__fragment_push(p, ch, body, n, now, receive, out);
}
TEST(net_fragment, maximum_reverse_duplicates_no_alloc) {
    for (u32 i = 0; i < sizeof payload; ++i)
        payload[i] = (u8)(i * 37u + i / 1024u);
    de__fragment_pool *p = create(0);
    delivered out = {payload, sizeof payload, 0, DE_CHANNEL_RELIABLE_UNORDERED};
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 i = 256; i > 0; --i) {
        ASSERT_EQ(push(p, out.channel, 0xffffffffu, (u16)(i - 1), sizeof payload, 1, &out), DE_OK);
        ASSERT_EQ(push(p, out.channel, 0xffffffffu, (u16)(i - 1), sizeof payload, 2, &out), DE_OK);
        ASSERT_EQ(out.count, i == 1 ? 1u : 0u);
    }
    de__fragment_expire(p, UINT64_MAX);
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    const de__fragment_stats s = de__fragment_get_stats(p);
    ASSERT_EQ(s.completed, 1u);
    ASSERT_EQ(s.used_bytes, 0u);
    ASSERT_EQ(s.peak_bytes, sizeof payload);
    ASSERT_TRUE(s.allocated_bytes < DE__FRAGMENT_DEFAULT_POOL + 65536u);
    de__fragment_destroy(p);
}
TEST(net_fragment, partial_unreliable_expires_reliable_retains) {
    de__fragment_pool *p = create(0);
    delivered out = {payload, 2049, 0, DE_CHANNEL_UNRELIABLE};
    ASSERT_EQ(push(p, out.channel, 10, 0, out.bytes, 0, &out), DE_OK);
    ASSERT_EQ(push(p, out.channel, 10, 2, out.bytes, 0, &out), DE_OK);
    de__fragment_expire(p, DE__FRAGMENT_TIMEOUT_NS - 1);
    ASSERT_EQ(de__fragment_get_stats(p).active, 1u);
    ASSERT_EQ(out.count, 0u);
    de__fragment_expire(p, DE__FRAGMENT_TIMEOUT_NS);
    ASSERT_EQ(de__fragment_get_stats(p).active, 0u);
    ASSERT_EQ(de__fragment_get_stats(p).used_bytes, 0u);
    ASSERT_EQ(push(p, out.channel, 10, 1, out.bytes, DE__FRAGMENT_TIMEOUT_NS, &out), DE_OK);
    ASSERT_EQ(out.count, 0u);
    ASSERT_EQ(de__fragment_get_stats(p).active, 0u);
    out.channel = DE_CHANNEL_RELIABLE_ORDERED;
    ASSERT_EQ(push(p, out.channel, 10, 2, out.bytes, 0, &out), DE_OK);
    de__fragment_expire(p, 5 * DE__FRAGMENT_TIMEOUT_NS);
    ASSERT_EQ(de__fragment_get_stats(p).active, 1u);
    ASSERT_EQ(push(p, out.channel, 10, 0, out.bytes, 5 * DE__FRAGMENT_TIMEOUT_NS, &out), DE_OK);
    ASSERT_EQ(push(p, out.channel, 10, 1, out.bytes, 5 * DE__FRAGMENT_TIMEOUT_NS, &out), DE_OK);
    ASSERT_EQ(out.count, 1u);
    de__fragment_destroy(p);
}
TEST(net_fragment, pool_cap_evicts_oldest_unreliable_never_reliable) {
    de__fragment_pool *p = create(4096);
    delivered out = {payload, 2048, 0, DE_CHANNEL_UNRELIABLE};
    ASSERT_EQ(push(p, out.channel, 10, 0, out.bytes, 0, &out), DE_OK);
    ASSERT_EQ(push(p, out.channel, 11, 0, out.bytes, 10, &out), DE_OK);
    ASSERT_EQ(push(p, DE_CHANNEL_RELIABLE_ORDERED, 12, 0, out.bytes, 20, &out), DE_OK);
    ASSERT_EQ(de__fragment_get_stats(p).dropped, 1u);
    ASSERT_EQ(push(p, out.channel, 10, 1, out.bytes, 21, &out), DE_OK);
    ASSERT_EQ(out.count, 0u); /* retired oldest cannot be recreated by late parts */
    ASSERT_EQ(push(p, DE_CHANNEL_RELIABLE_UNORDERED, 13, 0, out.bytes, 30, &out), DE_OK);
    ASSERT_EQ(push(p, DE_CHANNEL_RELIABLE_ORDERED, 14, 0, out.bytes, 40, &out),
              DE_ERR_OUT_OF_MEMORY);
    ASSERT_EQ(push(p, out.channel, 15, 0, out.bytes, 50, &out), DE_ERR_BUSY);
    de__fragment_expire(p, 20 * DE__FRAGMENT_TIMEOUT_NS);
    de__fragment_stats s = de__fragment_get_stats(p);
    ASSERT_EQ(s.used_bytes, 4096u);
    ASSERT_EQ(s.peak_bytes, 4096u);
    ASSERT_EQ(s.active, 2u);
    de__fragment_reset(p);
    ASSERT_EQ(de__fragment_get_stats(p).used_bytes, 0u);
    ASSERT_EQ(de__fragment_get_stats(p).allocated_bytes, s.allocated_bytes);
    de__fragment_destroy(p);
}
TEST(net_fragment, validation_bounds_inconsistency_and_conflicting_duplicates) {
    u8 body[DE__FRAGMENT_HEADER + DE__FRAGMENT_SLICE], bad[sizeof body];
    const u32 n = de__fragment_write(body, sizeof body, 15, 0, payload, 2048);
    ASSERT_EQ(n, sizeof body);
    de__fragment_view v;
    for (u32 bytes = 0; bytes < n; ++bytes)
        ASSERT_FALSE(de__fragment_read(body, bytes, DE__FRAGMENT_MAX_MESSAGE, &v));
    ASSERT_FALSE(de__fragment_read(NULL, n, DE__FRAGMENT_MAX_MESSAGE, &v));
    ASSERT_FALSE(de__fragment_read(body, n, 2047, &v));
    ASSERT_FALSE(de__fragment_read(body, n, DE__FRAGMENT_MAX_MESSAGE, NULL));
    for (u32 field = 0; field < 5; ++field) {
        memcpy(bad, body, n);
        switch (field) {
        case 0:
            bad[4] = 2;
            break; /* index == count */
        case 1:
            bad[6] = 0;
            break;
        case 2:
            bad[6] = 0xff;
            bad[7] = 0xff;
            break;
        case 3:
            de__net_put_u32(bad + 8, UINT32_MAX);
            break;
        default:
            de__net_put_u32(bad + 8, 1024);
            break;
        }
        ASSERT_FALSE(de__fragment_read(bad, n, DE__FRAGMENT_MAX_MESSAGE, &v));
    }
    de__fragment_pool *p = create(0);
    delivered out = {payload, 2048, 0, DE_CHANNEL_RELIABLE_UNORDERED};
    ASSERT_EQ(de__fragment_push(p, out.channel, body, n, 0, receive, &out), DE_OK);
    memcpy(bad, body, n);
    bad[DE__FRAGMENT_HEADER] ^= 1;
    ASSERT_EQ(de__fragment_push(p, out.channel, bad, n, 1, receive, &out), DE_ERR_PARSE);
    ASSERT_EQ(de__fragment_get_stats(p).used_bytes, 0u);
    ASSERT_EQ(push(p, out.channel, 15, 1, out.bytes, 2, &out), DE_OK);
    ASSERT_EQ(out.count, 0u);
    ASSERT_EQ(push(p, out.channel, 16, 0, out.bytes, 3, &out), DE_OK);
    const u32 changed = de__fragment_write(bad, sizeof bad, 16, 0, payload, 2049);
    ASSERT_EQ(de__fragment_push(p, out.channel, bad, changed, 4, receive, &out), DE_ERR_PARSE);
    ASSERT_EQ(de__fragment_get_stats(p).active, 0u);
    /* An invalid header also invalidates its already accepted partial message. */
    ASSERT_EQ(push(p, out.channel, 17, 0, out.bytes, 5, &out), DE_OK);
    de__fragment_write(bad, sizeof bad, 17, 0, payload, out.bytes);
    bad[4] = 255;
    ASSERT_EQ(de__fragment_push(p, out.channel, bad, n, 6, receive, &out), DE_ERR_PARSE);
    ASSERT_EQ(de__fragment_get_stats(p).active, 0u);
    ASSERT_EQ(out.count, 0u);
    de__fragment_destroy(p);
}

#if defined(DE_DEV) || defined(DE_DEBUG)
typedef struct large_delivery {
    u32 count[3], tails;
} large_delivery;
static void large_received(void *user, de_channel ch, const void *data, u32 bytes) {
    large_delivery *d = user;
    if (bytes == 4) {
        ASSERT_EQ(ch, DE_CHANNEL_RELIABLE_ORDERED);
        ASSERT_EQ(memcmp(data, "tail", 4), 0);
        ASSERT_EQ(d->count[ch], 1u);
        d->tails++;
        return;
    }
    ASSERT_TRUE(ch == DE_CHANNEL_RELIABLE_ORDERED || ch == DE_CHANNEL_RELIABLE_UNORDERED);
    ASSERT_EQ(bytes, sizeof payload);
    ASSERT_EQ(memcmp(data, payload, bytes), 0);
    ASSERT_EQ(d->count[ch], 0u);
    d->count[ch]++;
}
TEST(net_fragment, encrypted_reliable_256k_both_channels_under_ten_percent_loss_no_heap) {
    for (u32 i = 0; i < sizeof payload; ++i)
        payload[i] = (u8)(i * 17u + i / 1024u);
    fragment_rig r;
    fragment_rig_open(&r, 0, false);
    const de_net_sim sim = {40, 10, .1f, .1f};
    for (u32 i = 0; i < 2; ++i)
        ASSERT_EQ(de__net_sim_set(r.socket[i], &sim), DE_OK);
    large_delivery out = {0};
    bool tail = false, done = false;
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    ASSERT_EQ(de__chan_send(r.channel[0], DE_CHANNEL_RELIABLE_ORDERED, payload, sizeof payload),
              DE_OK);
    ASSERT_EQ(de__chan_send(r.channel[0], DE_CHANNEL_RELIABLE_UNORDERED, payload, sizeof payload),
              DE_OK);
    for (u32 step = 0; step < 300000; ++step) {
        if (!tail) {
            const de_result sent =
                de__chan_send(r.channel[0], DE_CHANNEL_RELIABLE_ORDERED, "tail", 4);
            ASSERT_TRUE(sent == DE_OK || sent == DE_ERR_BUSY);
            tail = sent == DE_OK;
        }
        ASSERT_EQ(fragment_tick(&r, large_received, &out), DE_OK);
        const de__chan_stats s = de__chan_get_stats(r.channel[0]);
        if (out.count[1] == 1 && out.count[2] == 1 && out.tails == 1 && !s.pending[1] &&
            !s.pending[2]) {
            done = true;
            break;
        }
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    ASSERT_TRUE(done);
    const de__chan_stats tx = de__chan_get_stats(r.channel[0]),
                         rx = de__chan_get_stats(r.channel[1]);
    ASSERT_EQ(tx.channel[1].msgs_out, 2u);
    ASSERT_EQ(tx.channel[2].msgs_out, 1u);
    ASSERT_EQ(tx.channel[1].bytes_out, sizeof payload + 4u);
    ASSERT_EQ(tx.channel[2].bytes_out, sizeof payload);
    ASSERT_EQ(rx.channel[1].msgs_in, 2u);
    ASSERT_EQ(rx.channel[2].msgs_in, 1u);
    ASSERT_EQ(de__chan_fragment_stats(r.channel[1]).used_bytes, 0u);
    ASSERT_TRUE(de__chan_fragment_stats(r.channel[1]).peak_bytes <= DE__FRAGMENT_DEFAULT_POOL);
    printf("encrypted256KiB ordered+unordered, tail ordered,10%%loss+duplication: %.3fs fake time, "
           "pool peak%u\n",
           (double)r.now / 1000000000.0, de__chan_fragment_stats(r.channel[1]).peak_bytes);
    fragment_rig_close(&r);
}
TEST(net_fragment, encrypted_unreliable_missing_part_never_delivers_reclaims_pool) {
    fragment_rig r;
    fragment_rig_open(&r, 0, false);
    r.drop_unreliable_part = true;
    delivered out = {payload, 4096, 0, DE_CHANNEL_UNRELIABLE};
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    ASSERT_EQ(de__chan_send(r.channel[0], out.channel, payload, out.bytes), DE_OK);
    for (u32 i = 0; i < 2500; ++i)
        ASSERT_EQ(fragment_tick(&r, receive, &out), DE_OK);
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    ASSERT_EQ(r.parts_dropped, 1u);
    ASSERT_EQ(out.count, 0u);
    ASSERT_TRUE(de__chan_fragment_stats(r.channel[1]).peak_bytes > 0);
    ASSERT_EQ(de__chan_fragment_stats(r.channel[1]).used_bytes, 0u);
    ASSERT_EQ(de__chan_get_stats(r.channel[0]).pending[0], 0u);
    fragment_rig_close(&r);
}
TEST(net_fragment, encrypted_reliable_pool_protocol_error_disconnects_with_reason) {
    fragment_rig r;
    fragment_rig_open(&r, 1024, true);
    delivered out = {payload, 2048, 0, DE_CHANNEL_RELIABLE_ORDERED};
    ASSERT_EQ(de__chan_send(r.channel[0], out.channel, payload, out.bytes), DE_OK);
    for (u32 i = 0; i < 10 && !r.ended[0].count; ++i)
        (void)fragment_tick(&r, receive, &out);
    ASSERT_EQ(out.count, 0u);
    ASSERT_EQ(r.ended[1].count, 1u);
    ASSERT_EQ(r.ended[1].detail, DE__CHAN_ERROR_FRAGMENT_POOL);
    ASSERT_EQ(r.ended[0].count, 1u);
    ASSERT_EQ(r.ended[0].detail, DE__CHAN_ERROR_FRAGMENT_POOL);
    ASSERT_TRUE(de__net_client_get_state(r.client) != DE__NET_CLIENT_CONNECTED);
    ASSERT_EQ(de__conn_wire_overhead(r.channel[0]), 0u);
    fragment_rig_close(&r);
}
TEST(net_fragment, encrypted_malformed_reliable_fragment_disconnects_before_pool_write) {
    fragment_rig r;
    fragment_rig_open(&r, 0, true);
    u8 fragment[DE__FRAGMENT_HEADER + DE__FRAGMENT_SLICE], packet[DE__CHAN_PACKET_MAX];
    const u32 n = de__fragment_write(fragment, sizeof fragment, 0, 0, payload, 2048);
    fragment[6] = 0; /* count contradicts total and index */
    de__chan_packet_header(packet, 0, false, 0, 0);
    const u32 framed =
        de__chan_packet_fragment(packet + DE__CHAN_HEADER, sizeof packet - DE__CHAN_HEADER,
                                 DE_CHANNEL_RELIABLE_ORDERED, 0, fragment, n);
    ASSERT_TRUE(framed > 0);
    ASSERT_EQ(de__net_client_send(r.client, packet, DE__CHAN_HEADER + framed), DE_OK);
    de__net_server_update(r.server);
    de__net_client_update(r.client);
    ASSERT_EQ(r.ended[1].detail, DE__CHAN_ERROR_FRAGMENT_FORMAT);
    ASSERT_EQ(r.ended[0].detail, DE__CHAN_ERROR_FRAGMENT_FORMAT);
    ASSERT_EQ(de__chan_fragment_stats(r.channel[1]).peak_bytes, 0u);
    fragment_rig_close(&r);
}
TEST(net_fragment, ordered_raw_message_cannot_interrupt_an_unfinished_fragmented_message) {
    fragment_rig r;
    fragment_rig_open(&r, 0, true);
    u8 fragment[DE__FRAGMENT_HEADER + DE__FRAGMENT_SLICE], packet[DE__CHAN_PACKET_MAX], value = 1;
    const u32 n = de__fragment_write(fragment, sizeof fragment, 0, 0, payload, 2048);
    de__chan_packet_header(packet, 0, false, 0, 0);
    u32 bytes = DE__CHAN_HEADER +
                de__chan_packet_fragment(packet + DE__CHAN_HEADER, sizeof packet - DE__CHAN_HEADER,
                                         DE_CHANNEL_RELIABLE_ORDERED, 0, fragment, n);
    bytes += de__chan_packet_message(packet + bytes, sizeof packet - bytes,
                                     DE_CHANNEL_RELIABLE_ORDERED, 1, &value, 1);
    ASSERT_EQ(de__net_client_send(r.client, packet, bytes), DE_OK);
    de__net_server_update(r.server);
    delivered out = {payload, 2048, 0, DE_CHANNEL_RELIABLE_ORDERED};
    de__chan_receive(r.channel[1], receive, &out);
    ASSERT_EQ(out.count, 0u);
    de__net_server_update(r.server);
    de__net_client_update(r.client);
    ASSERT_EQ(r.ended[0].detail, DE__CHAN_ERROR_FRAGMENT_FORMAT);
    ASSERT_EQ(r.ended[1].detail, DE__CHAN_ERROR_FRAGMENT_FORMAT);
    fragment_rig_close(&r);
}
#endif
