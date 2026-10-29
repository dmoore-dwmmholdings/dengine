#include "de_test.h"
#include "de_alloc_guard.h"
#include "net/net_stats.h"
#include "net/congestion.h"
#include <dengine/de_debug.h>
#include <string.h>
#define MS 1000000ull
static void near(u32 actual, u32 expected) {
    ASSERT_TRUE((u64)actual * 100u >= (u64)expected * 95u);
    ASSERT_TRUE((u64)actual * 100u <= (u64)expected * 105u);
}
TEST(net_stats, ten_second_bandwidth_and_channel_rates_at_all_bucket_phases) {
    de__net_stats s = {0};
    for (u64 now = 0; now < 10000u * MS; now += 10u * MS) {
        de__stats_packet_in(&s, now, 120);
        const u32 serial = de__stats_packet_out(&s, now, 200, true);
        de__stats_outcome(&s, serial, false);
        for (u32 ch = 0; ch < 3; ++ch) {
            de__stats_channel(&s, now, (de_channel)ch, 20u * (ch + 1u), true, false);
            de__stats_channel(&s, now, (de_channel)ch, 30u * (ch + 1u), false, false);
            de__stats_channel(&s, now, (de_channel)ch, 99, false, true);
        }
        if (now < 2000u * MS)
            continue;
        const de_peer_stats p = de__stats_peer(&s, now, 100u * MS, 20u * MS);
        near(p.bytes_in_per_sec, 12000);
        near(p.bytes_out_per_sec, 20000);
        ASSERT_TRUE(p.rtt_ms == 100.0f);
        ASSERT_TRUE(p.jitter_ms == 20.0f);
        for (u32 ch = 0; ch < 3; ++ch) {
            const de__channel_rates r = de__stats_rates(&s, now, (de_channel)ch);
            near(r.bytes_in_per_sec, 2000u * (ch + 1u));
            near(r.bytes_out_per_sec, 3000u * (ch + 1u));
            near(r.msgs_in_per_sec, 100);
            near(r.msgs_out_per_sec, 100);
            near(r.resends_per_sec, 100);
        }
    }
    ASSERT_EQ(s.packets_in, 1000u);
    ASSERT_EQ(s.packets_out, 1000u);
    ASSERT_EQ(de__stats_peer(&s, 12000u * MS, 0, 0).bytes_out_per_sec, 0u);
}
TEST(net_stats, packet_loss_window_late_ack_generation_and_ack_only) {
    de__net_stats s = {0};
    for (u32 i = 0; i < 256; ++i) {
        const u32 id = de__stats_packet_out(&s, 0, 1200, true);
        de__stats_outcome(&s, id, i < 16);
    }
    u32 lost, resolved;
    de__stats_loss(&s, &lost, &resolved);
    ASSERT_EQ(lost, 16u);
    ASSERT_EQ(resolved, 256u);
    ASSERT_TRUE(de__stats_peer(&s, 0, 0, 0).loss == .0625f);
    de__stats_outcome(&s, 0, false);
    de__stats_outcome(&s, 0, true);
    de__stats_loss(&s, &lost, &resolved);
    ASSERT_EQ(lost, 15u);
    const u32 next = de__stats_packet_out(&s, 0, 1200, true);
    ASSERT_EQ(next, 256u);
    de__stats_outcome(&s, 0, false); /* cannot resolve a reused slot */
    de__stats_loss(&s, &lost, &resolved);
    ASSERT_EQ(resolved, 255u);
    de__stats_packet_out(&s, 0, DE__CHAN_HEADER, false);
    ASSERT_EQ(s.next_serial, 257u);
    s.next_serial = UINT32_MAX;
    ASSERT_EQ(de__stats_packet_out(&s, 0, 20, true), UINT32_MAX);
    ASSERT_EQ(de__stats_packet_out(&s, 0, 20, true), 0u);
}
typedef struct wire {
    de__conn *peer;
    u64 *now;
    u32 large;
    u64 last_large, max_gap;
    u64 bytes_out;
} wire;
static de_result send_packet(void *user, const void *data, u32 bytes) {
    wire *w = user;
    if (bytes == 1200) {
        w->large++;
        if (*w->now - w->last_large > w->max_gap)
            w->max_gap = *w->now - w->last_large;
        w->last_large = *w->now;
    }
    const de_result result = w->peer ? de__conn_read_payload(w->peer, data, bytes) : DE_OK;
    if (result == DE_OK)
        w->bytes_out += bytes;
    return result;
}
static u64 clock_ns(void *user) {
    return *(u64 *)user;
}
static void received(void *user, de_channel ch, const void *data, u32 bytes) {
    (void)ch;
    (void)data;
    *(u64 *)user += bytes;
}
TEST(net_cc, reliable_full_size_resends_do_not_starve_behind_tiny_new_messages) {
    u64 now = 0;
    wire w = {.now = &now};
    de__conn *c = NULL;
    const de__chan_desc d = {.send_packet = send_packet,
                             .send_user = &w,
                             .now_ns = clock_ns,
                             .clock_user = &now,
                             .max_bandwidth_kbps = 32,
                             .tick_rate_hz = 1000};
    ASSERT_EQ(de__chan_create(&d, &c), DE_OK);
    u8 full[DE__CHAN_MESSAGE_MAX] = {1}, tiny = 2;
    for (u32 i = 0; i < 4; ++i)
        ASSERT_EQ(de__chan_send(c, DE_CHANNEL_RELIABLE_ORDERED, full, sizeof full), DE_OK);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    u8 incoming[DE__CHAN_PACKET_MAX];
    u16 sequence = 0;
    u64 ignored = 0;
    for (; now < 10000u * MS; now += MS) {
        // Incoming traffic continually requests ACKs, which must not consume
        // every partial byte refill before a full reliable resend can fit.
        de__chan_packet_header(incoming, sequence, false, 0, 0);
        const u32 n =
            de__chan_packet_message(incoming + DE__CHAN_HEADER, sizeof incoming - DE__CHAN_HEADER,
                                    DE_CHANNEL_UNRELIABLE, sequence++, &tiny, 1);
        ASSERT_EQ(de__conn_read_payload(c, incoming, DE__CHAN_HEADER + n), DE_OK);
        de__chan_receive(c, received, &ignored);
        const de_result r = de__chan_send(c, DE_CHANNEL_RELIABLE_UNORDERED, &tiny, 1);
        ASSERT_TRUE(r == DE_OK || r == DE_ERR_BUSY);
        ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    ASSERT_TRUE(w.large > 20);
    ASSERT_TRUE(w.max_gap < 1000u * MS);
    ASSERT_TRUE(de__chan_get_stats(c).channel[1].resends > 20);
    ASSERT_TRUE(de__conn_stats(c).loss > .95f);
    de__chan_destroy(c);
}
TEST(net_stats, actual_channels_default_budget_stats_reset_and_live_cap_no_heap) {
    u64 now = 0, delivered[2] = {0};
    de__conn *c[2] = {0};
    wire w[2] = {{.now = &now}, {.now = &now}};
    for (u32 i = 0; i < 2; ++i) {
        const de__chan_desc d = {
            .send_packet = send_packet, .send_user = &w[i], .now_ns = clock_ns, .clock_user = &now};
        ASSERT_EQ(de__chan_create(&d, &c[i]), DE_OK);
    }
    w[0].peer = c[1];
    w[1].peer = c[0];
    u8 body[100] = {0};
    u64 window_start[2] = {0};
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (; now < 10000u * MS; now += MS) {
        if (now == 9000u * MS)
            for (u32 i = 0; i < 2; ++i)
                window_start[i] = w[i].bytes_out;
        if (now % (10u * MS) == 0)
            for (u32 ch = 0; ch < 3; ++ch)
                ASSERT_EQ(de__chan_send(c[0], (de_channel)ch, body, sizeof body), DE_OK);
        for (u32 i = 0; i < 2; ++i) {
            ASSERT_EQ(de__conn_write_packets(c[i], now), DE_OK);
            de__chan_receive(c[i], received, &delivered[i]);
        }
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    const de__chan_stats st = de__chan_get_stats(c[1]);
    ASSERT_EQ(st.channel[0].bytes_in + st.channel[1].bytes_in + st.channel[2].bytes_in,
              delivered[1]);
    const de_peer_stats peer = de__conn_stats(c[1]);
    ASSERT_TRUE(peer.bytes_in_per_sec > 20000u && peer.bytes_in_per_sec < 40000u);
    for (u32 i = 0; i < 2; ++i) {
        const de_peer_stats measured = de__conn_stats(c[i]);
        near(measured.bytes_out_per_sec, (u32)(w[i].bytes_out - window_start[i]));
        near(measured.bytes_in_per_sec, (u32)(w[1u - i].bytes_out - window_start[1u - i]));
    }
    de__channel_rates r;
    ASSERT_TRUE(de__conn_channel_stats(c[1], DE_CHANNEL_RELIABLE_ORDERED, &r));
    near(r.bytes_in_per_sec, 10000);
    ASSERT_FALSE(de__conn_channel_stats(c[1], (de_channel)3, &r));
    ASSERT_EQ(r.bytes_in_per_sec, 0u);
    const de_cvar cv = de_cvar_find("net.max_bandwidth_kbps");
    ASSERT_EQ(de_cvar_set(cv, "32"), DE_OK);
    ASSERT_EQ(de__conn_write_packets(c[0], now), DE_OK);
    ASSERT_EQ(de__conn_cc_view(c[0])->cap, 4000u);
    ASSERT_EQ(de_cvar_set(cv, "512"), DE_OK);
    de__chan_reset(c[0]);
    ASSERT_EQ(de__conn_stats(c[0]).bytes_out_per_sec, 0u);
    ASSERT_EQ(de__chan_get_stats(c[0]).packets_out, 0u);
    for (u32 i = 0; i < 2; ++i)
        de__chan_destroy(c[i]);
}

TEST(net_stats, actual_loss_timeout_late_ack_and_ack_only_do_not_double_count) {
    u64 now = 0;
    wire w = {.now = &now};
    de__conn *c = NULL;
    const de__chan_desc d = {
        .send_packet = send_packet, .send_user = &w, .now_ns = clock_ns, .clock_user = &now};
    ASSERT_EQ(de__chan_create(&d, &c), DE_OK);
    u8 value = 1, packet[DE__CHAN_PACKET_MAX];
    ASSERT_EQ(de__chan_send(c, DE_CHANNEL_UNRELIABLE, &value, 1), DE_OK);
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
    now = 300u * MS;
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
    ASSERT_EQ(de__chan_get_stats(c).lost, 1u);
    ASSERT_TRUE(de__conn_stats(c).loss == 1.0f);
    de__chan_packet_header(packet, 0, true, 0, 0);
    ASSERT_EQ(de__conn_read_payload(c, packet, DE__CHAN_HEADER), DE_OK);
    ASSERT_EQ(de__conn_read_payload(c, packet, DE__CHAN_HEADER), DE_OK);
    ASSERT_EQ(de__chan_get_stats(c).acked, 1u);
    ASSERT_TRUE(de__conn_stats(c).loss == 0.0f);
    de__chan_packet_header(packet, 1, false, 0, 0);
    const u32 n = de__chan_packet_message(packet + DE__CHAN_HEADER, sizeof packet - DE__CHAN_HEADER,
                                          DE_CHANNEL_UNRELIABLE, 0, &value, 1);
    ASSERT_EQ(de__conn_read_payload(c, packet, DE__CHAN_HEADER + n), DE_OK);
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK); // an ACK-only packet, no ACK expected
    ASSERT_EQ(de__chan_get_stats(c).packets_out, 2u);
    now = 1000u * MS;
    ASSERT_EQ(de__conn_write_packets(c, now), DE_OK);
    ASSERT_EQ(de__chan_get_stats(c).lost, 1u);
    ASSERT_TRUE(de__conn_stats(c).loss == 0.0f);
    de__chan_destroy(c);
}
