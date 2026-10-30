#include "de_test.h"
#include "de_alloc_guard.h"
#include "net/net_sim.h"
#include <dengine/de_debug.h>
#include <dengine/de_math.h>
#include <dengine/de_platform.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#if defined(DE_DEV) || defined(DE_DEBUG)
#define NS_MS 1000000ull
typedef struct rig {
    de__loopback_net *net;
    de__socket *tx, *rx;
    u64 ns;
} rig;
static u64 fake_now(void *p) {
    return *(u64 *)p;
}
static void open_rig(rig *r, u32 cap, u64 seed, bool incoming) {
    memset(r, 0, sizeof *r);
    const de__loopback_desc lb = {.queue_capacity = 512};
    ASSERT_EQ(de__loopback_net_create(&lb, NULL, &r->net), DE_OK);
    de__socket *tx = NULL, *rx = NULL;
    ASSERT_EQ(de__loopback_open(r->net, NULL, &tx), DE_OK);
    ASSERT_EQ(de__loopback_open(r->net, NULL, &rx), DE_OK);
    const de__net_sim_desc d = {.queue_capacity = cap,
                                .seed = seed,
                                .incoming = incoming,
                                .now_ns = fake_now,
                                .clock_user = &r->ns};
    if (incoming) {
        r->tx = tx;
        ASSERT_EQ(de__net_sim_wrap_ex(rx, &d, NULL, &r->rx), DE_OK);
    } else {
        r->rx = rx;
        ASSERT_EQ(de__net_sim_wrap_ex(tx, &d, NULL, &r->tx), DE_OK);
    }
}
static void close_rig(rig *r) {
    de__socket_close(r->tx);
    de__socket_close(r->rx);
    de__loopback_net_destroy(r->net);
}
typedef struct packet {
    u64 emitted;
    u32 id, canary;
} packet;
static void send_packet(rig *r, u32 id) {
    packet p = {.emitted = r->ns, .id = id, .canary = 0x12345678};
    u32 sent = 0;
    ASSERT_EQ(de__socket_send_to(r->tx, &r->rx->local, &p, sizeof p, &sent), DE_OK);
    ASSERT_EQ(sent, sizeof p);
}
static u32 receive(rig *r, packet *p, u32 cap) {
    de__datagram messages[64];
    ASSERT_TRUE(cap <= 64);
    for (u32 i = 0; i < cap; ++i)
        messages[i] = (de__datagram){.data = p + i, .capacity = sizeof p[i]};
    u32 got = 0;
    ASSERT_EQ(de__socket_recv_from(r->rx, messages, cap, &got), DE_OK);
    for (u32 i = 0; i < got; ++i) {
        ASSERT_EQ(messages[i].bytes, sizeof p[i]);
        ASSERT_TRUE(de_net_address_equal(&messages[i].from, &r->tx->local));
        ASSERT_EQ(p[i].canary, 0x12345678u);
    }
    return got;
}

TEST(net_sim, zero_passthrough_and_no_heap) {
    rig r;
    open_rig(&r, 16, 1, false);
    de__socket *basic = NULL;
    ASSERT_EQ(de__net_sim_wrap(r.tx, NULL, 16, &basic), DE_OK);
    r.tx = basic;
    packet p;
    send_packet(&r, 0);
    ASSERT_EQ(receive(&r, &p, 1), 1u);
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    for (u32 i = 0; i < 10000; ++i) {
        send_packet(&r, i);
        ASSERT_EQ(receive(&r, &p, 1), 1u);
        ASSERT_EQ(p.id, i);
    }
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    EXPECT_EQ(de__net_sim_get_stats(r.tx).pending_out, 0u);
    EXPECT_EQ(r.tx->stats.datagrams_sent, 10001u);
    close_rig(&r);
}

TEST(net_sim, max_datagram_drops_outgoing_and_incoming_even_without_other_conditions) {
    for (u32 inbound = 0; inbound < 2; ++inbound) {
        rig r;
        open_rig(&r, 8, 54, inbound != 0);
        de__socket *sim = inbound ? r.rx : r.tx;
        ASSERT_EQ(de__net_sim_set_max_datagram(sim, 1400), DE_OK);
        u8 data[1401] = {54}, storage[3][1401];
        de__datagram slots[3];
        for (u32 i = 0; i < 3; ++i)
            slots[i] = (de__datagram){.data = storage[i], .capacity = sizeof storage[i]};
        de_alloc_guard guard = {0};
        de_alloc_guard_begin(&guard);
        for (u32 i = 0; i < 3; ++i) {
            u32 sent = 0;
            const u32 bytes = i == 1 ? 1400 : 1401;
            ASSERT_EQ(de__socket_send_to(r.tx, &r.rx->local, data, bytes, &sent), DE_OK);
            ASSERT_EQ(sent, bytes);
        }
        u32 got = 0;
        ASSERT_EQ(de__socket_recv_from(r.rx, slots, 3, &got), DE_OK);
        de_alloc_guard_end(&guard);
        DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
        ASSERT_EQ(got, 1u);
        ASSERT_EQ(slots[0].bytes, 1400u);
        ASSERT_EQ(((u8 *)slots[0].data)[0], 54u);
        ASSERT_TRUE(slots[0].data != slots[1].data && slots[0].data != slots[2].data &&
                    slots[1].data != slots[2].data);
        ASSERT_EQ(de__net_sim_get_stats(sim).mtu_dropped, 2u);
        close_rig(&r);
    }
}
TEST(net_sim, changed_path_limit_also_drops_already_delayed_datagrams) {
    rig r;
    open_rig(&r, 8, 54, false);
    const de_net_sim delayed = {50, 0, 0, 0};
    ASSERT_EQ(de__net_sim_set(r.tx, &delayed), DE_OK);
    u8 data[1401] = {0};
    u32 sent = 0;
    ASSERT_EQ(de__socket_send_to(r.tx, &r.rx->local, data, sizeof data, &sent), DE_OK);
    ASSERT_EQ(de__net_sim_get_stats(r.tx).pending_out, 1u);
    ASSERT_EQ(de__net_sim_set_max_datagram(r.tx, 1400), DE_OK);
    r.ns = 50u * NS_MS;
    ASSERT_EQ(de__net_sim_pump(r.tx), DE_OK);
    ASSERT_EQ(de__net_sim_get_stats(r.tx).pending_out, 0u);
    ASSERT_EQ(de__net_sim_get_stats(r.tx).mtu_dropped, 1u);
    de__datagram slot = {.data = data, .capacity = sizeof data};
    u32 got = 0;
    ASSERT_EQ(de__socket_recv_from(r.rx, &slot, 1, &got), DE_OK);
    ASSERT_EQ(got, 0u);
    close_rig(&r);
}

typedef struct measure {
    u64 hash, delay_sum;
    u32 delivered, unique, repeated, reordered, last_id;
} measure;
static measure traffic_mode(de_net_sim settings, u64 seed, bool guard_heap, bool incoming) {
    rig r;
    open_rig(&r, 512, seed, incoming);
    de__socket *wrapped = incoming ? r.rx : r.tx;
    ASSERT_EQ(de__net_sim_set(wrapped, &settings), DE_OK);
    u8 seen[10000] = {0};
    measure m = {.hash = 1469598103934665603ull};
    de_alloc_guard guard = {0};
    if (guard_heap)
        de_alloc_guard_begin(&guard);
    for (u32 ms = 0; ms < 10000 + settings.latency_ms + settings.jitter_ms + 2; ++ms) {
        r.ns = (u64)ms * NS_MS;
        if (ms < 10000)
            send_packet(&r, ms);
        ASSERT_EQ(de__net_sim_pump(wrapped), DE_OK);
        packet packets[64];
        u32 got;
        while ((got = receive(&r, packets, 64)) != 0) {
            for (u32 i = 0; i < got; ++i) {
                packet p = packets[i];
                ASSERT_TRUE(p.id < 10000);
                const u64 delay = (r.ns - p.emitted) / NS_MS;
                const u32 low = settings.latency_ms > settings.jitter_ms
                                    ? settings.latency_ms - settings.jitter_ms
                                    : 0;
                ASSERT_TRUE(delay >= low && delay <= settings.latency_ms + settings.jitter_ms);
                if (m.delivered && p.id < m.last_id)
                    m.reordered++;
                m.last_id = p.id;
                m.delivered++;
                m.delay_sum += delay;
                if (!seen[p.id]++)
                    m.unique++;
                else
                    m.repeated++;
                m.hash = (m.hash ^ p.id) * 1099511628211ull;
                m.hash = (m.hash ^ r.ns) * 1099511628211ull;
            }
        }
    }
    if (guard_heap) {
        de_alloc_guard_end(&guard);
        DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    }
    const de__net_sim_stats stats = de__net_sim_get_stats(wrapped);
    EXPECT_EQ(stats.pending_out, 0u);
    EXPECT_EQ(stats.queue_dropped, 0u);
    EXPECT_EQ(incoming ? stats.delivered_in : stats.delivered_out, m.delivered);
    close_rig(&r);
    return m;
}
static measure traffic(de_net_sim settings, u64 seed, bool guard_heap) {
    return traffic_mode(settings, seed, guard_heap, false);
}
TEST(net_sim, latency_mean_bounds_reordering_and_no_heap) {
    const measure m = traffic((de_net_sim){.latency_ms = 100, .jitter_ms = 20}, 56, true);
    EXPECT_EQ(m.delivered, 10000u);
    EXPECT_EQ(m.unique, 10000u);
    EXPECT_TRUE(m.reordered > 0);
    const f64 mean = (f64)m.delay_sum / m.delivered;
    EXPECT_TRUE(mean >= 95 && mean <= 105);
    printf("simulation: mean latency %.3f ms; reordered %u of 10000\n", mean, m.reordered);
}
TEST(net_sim, loss_rate_over_ten_thousand) {
    const measure m =
        traffic((de_net_sim){.latency_ms = 100, .jitter_ms = 20, .loss = .2f}, 56, false);
    EXPECT_TRUE(m.unique >= 7900 && m.unique <= 8100);
    EXPECT_EQ(m.repeated, 0u);
    printf("simulation: loss %.2f%% (%u unique delivered)\n", (10000.0 - m.unique) / 100.0,
           m.unique);
}
TEST(net_sim, duplicate_rate_over_ten_thousand) {
    const measure m =
        traffic((de_net_sim){.latency_ms = 100, .jitter_ms = 20, .duplicate = .2f}, 56, false);
    EXPECT_EQ(m.unique, 10000u);
    EXPECT_TRUE(m.repeated >= 1900 && m.repeated <= 2100);
    printf("simulation: duplicates %.2f%% (%u extra delivered)\n", m.repeated / 100.0, m.repeated);
}
TEST(net_sim, seeded_drop_duplicate_delay_decisions_repeat) {
    const de_net_sim settings = {.latency_ms = 100, .jitter_ms = 20, .loss = .2f, .duplicate = .1f};
    measure a = traffic(settings, 999, false), b = traffic(settings, 999, false),
            c = traffic(settings, 1000, false);
    EXPECT_EQ(a.hash, b.hash);
    EXPECT_NE(a.hash, c.hash);
    EXPECT_EQ(a.delivered, b.delivered);
}
TEST(net_sim, full_queue_counts_udp_loss_and_setting_changes_keep_scheduled_packets) {
    rig r;
    open_rig(&r, 1, 1, false);
    const de_net_sim settings = {.latency_ms = 100, .duplicate = 1};
    ASSERT_EQ(de__net_sim_set(r.tx, &settings), DE_OK);
    send_packet(&r, 11);
    send_packet(&r, 12);
    EXPECT_EQ(de__net_sim_get_stats(r.tx).queue_dropped, 3u);
    EXPECT_EQ(r.tx->stats.dropped, 3u);
    EXPECT_EQ(de__net_sim_get_stats(r.tx).allocated_bytes > 0, true);
    const de_net_sim zero = {0};
    ASSERT_EQ(de__net_sim_set(r.tx, &zero), DE_OK);
    send_packet(&r, 13);
    packet p;
    ASSERT_EQ(receive(&r, &p, 1), 1u);
    EXPECT_EQ(p.id, 13u);
    r.ns = 100 * NS_MS;
    ASSERT_EQ(de__net_sim_pump(r.tx), DE_OK);
    ASSERT_EQ(receive(&r, &p, 1), 1u);
    EXPECT_EQ(p.id, 11u);
    EXPECT_EQ(receive(&r, &p, 1), 0u);
    close_rig(&r);
}

typedef struct blocked_socket {
    de__socket socket;
    bool blocked, closed;
    u32 calls, ids[4], count;
} blocked_socket;
static de_result blocked_send(de__socket *socket, const de_net_address *to, const void *data,
                              u32 bytes, u32 *sent) {
    (void)to;
    blocked_socket *s = (blocked_socket *)socket;
    s->calls++;
    *sent = 0;
    if (!s->blocked) {
        if (s->count < 4)
            s->ids[s->count++] = ((const packet *)data)->id;
        *sent = bytes;
    }
    return DE_OK;
}
static de_result blocked_recv(de__socket *s, de__datagram *m, u32 n, u32 *got) {
    (void)s;
    (void)m;
    (void)n;
    *got = 0;
    return DE_OK;
}
static void blocked_close(de__socket *s) {
    ((blocked_socket *)s)->closed = true;
}
TEST(net_sim, would_block_retains_copied_payload_and_never_spins) {
    u64 time = 0;
    blocked_socket inner = {
        .socket = {.send_to = blocked_send, .recv_from = blocked_recv, .close = blocked_close},
        .blocked = true};
    const de__net_sim_desc d = {.queue_capacity = 2, .now_ns = fake_now, .clock_user = &time};
    de__socket *wrapped = NULL;
    ASSERT_EQ(de__net_sim_wrap_ex(&inner.socket, &d, NULL, &wrapped), DE_OK);
    const de_net_sim settings = {.latency_ms = 100};
    ASSERT_EQ(de__net_sim_set(wrapped, &settings), DE_OK);
    de_net_address address = de_net_address_loopback(DE_NET_FAMILY_IPV4, 7777);
    u32 sent;
    packet p = {.id = 11};
    ASSERT_EQ(de__socket_send_to(wrapped, &address, &p, sizeof p, &sent), DE_OK);
    p.id = 12;
    ASSERT_EQ(de__socket_send_to(wrapped, &address, &p, sizeof p, &sent), DE_OK);
    p.id = 99;
    time = 100 * NS_MS;
    ASSERT_EQ(de__net_sim_pump(wrapped), DE_OK);
    EXPECT_EQ(inner.calls, 1u);
    EXPECT_EQ(de__net_sim_get_stats(wrapped).pending_out, 2u);
    inner.blocked = false;
    ASSERT_EQ(de__net_sim_pump(wrapped), DE_OK);
    EXPECT_EQ(inner.count, 2u);
    EXPECT_EQ(inner.ids[0], 11u);
    EXPECT_EQ(inner.ids[1], 12u);
    de__socket_close(wrapped);
    EXPECT_TRUE(inner.closed);
}
TEST(net_sim, incoming_delay_and_truncation) {
    rig r;
    open_rig(&r, 3, 3, true);
    const de_net_sim settings = {.latency_ms = 10};
    ASSERT_EQ(de__net_sim_set(r.rx, &settings), DE_OK);
    send_packet(&r, 1);
    packet p;
    EXPECT_EQ(receive(&r, &p, 1), 0u);
    r.ns = 9 * NS_MS;
    EXPECT_EQ(receive(&r, &p, 1), 0u);
    r.ns = 10 * NS_MS;
    ASSERT_EQ(receive(&r, &p, 1), 1u);
    EXPECT_EQ(p.id, 1u);
    send_packet(&r, 2);
    EXPECT_EQ(receive(&r, &p, 1), 0u);
    r.ns += 10 * NS_MS;
    char tiny;
    de__datagram message = {.data = &tiny, .capacity = 1};
    u32 got;
    ASSERT_EQ(de__socket_recv_from(r.rx, &message, 1, &got), DE_OK);
    EXPECT_EQ(got, 0u);
    EXPECT_EQ(r.rx->stats.truncated, 1u);
    close_rig(&r);
}
TEST(net_sim, invalid_limits_and_settings_keep_socket_ownership) {
    rig r;
    open_rig(&r, 4, 0, false);
    const de_net_sim good = {.latency_ms = 10};
    ASSERT_EQ(de__net_sim_set(r.tx, &good), DE_OK);
    for (u32 i = 0; i < 4; ++i) {
        de_net_sim bad = good;
        if (i == 0)
            bad.loss = NAN;
        if (i == 1)
            bad.duplicate = INFINITY;
        if (i == 2)
            bad.loss = -1;
        if (i == 3)
            bad.jitter_ms = 5001;
        EXPECT_EQ(de__net_sim_set(r.tx, &bad), DE_ERR_INVALID_ARGUMENT);
    }
    send_packet(&r, 7);
    EXPECT_EQ(de__net_sim_get_stats(r.tx).pending_out, 1u);
    de__socket *out = NULL;
    const de__net_sim_desc huge = {.queue_capacity = 65536, .slot_bytes = 65507, .incoming = true};
    EXPECT_EQ(de__net_sim_wrap_ex(r.rx, &huge, NULL, &out), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(out == NULL);
    r.ns = 10 * NS_MS;
    ASSERT_EQ(de__net_sim_pump(r.tx), DE_OK);
    packet p;
    ASSERT_EQ(receive(&r, &p, 1), 1u);
    close_rig(&r);
}
TEST(net_sim, zero_delay_duplication_is_immediate) {
    rig r;
    open_rig(&r, 4, 3, false);
    const de_net_sim settings = {.duplicate = 1};
    ASSERT_EQ(de__net_sim_set(r.tx, &settings), DE_OK);
    send_packet(&r, 8);
    packet p[2];
    ASSERT_EQ(receive(&r, p, 2), 2u);
    EXPECT_EQ(p[0].id, 8u);
    EXPECT_EQ(p[1].id, 8u);
    EXPECT_EQ(de__net_sim_get_stats(r.tx).pending_out, 0u);
    close_rig(&r);
}
TEST(net_sim, incoming_zero_bypasses_a_full_previously_scheduled_queue) {
    rig r;
    open_rig(&r, 1, 3, true);
    const de_net_sim delayed = {.latency_ms = 10}, zero = {0};
    ASSERT_EQ(de__net_sim_set(r.rx, &delayed), DE_OK);
    send_packet(&r, 1);
    packet p;
    ASSERT_EQ(receive(&r, &p, 1), 0u);
    ASSERT_EQ(de__net_sim_set(r.rx, &zero), DE_OK);
    send_packet(&r, 2);
    ASSERT_EQ(receive(&r, &p, 1), 1u);
    EXPECT_EQ(p.id, 2u);
    r.ns = 10 * NS_MS;
    ASSERT_EQ(receive(&r, &p, 1), 1u);
    EXPECT_EQ(p.id, 1u);
    EXPECT_EQ(de__net_sim_get_stats(r.rx).queue_dropped, 0u);
    close_rig(&r);
}
TEST(net_sim, incoming_loss_duplication_and_seed_are_reproducible) {
    const de_net_sim settings = {.latency_ms = 100, .jitter_ms = 20, .loss = .2f, .duplicate = .1f};
    measure a = traffic_mode(settings, 56, true, true), b = traffic_mode(settings, 56, false, true);
    EXPECT_EQ(a.hash, b.hash);
    EXPECT_TRUE(a.unique >= 7900 && a.unique <= 8100);
    EXPECT_TRUE(a.repeated >= 690 && a.repeated <= 910);
}
TEST(net_sim, actual_udp_delivery_waits_for_injected_release_time) {
    de_net_address bind = de_net_address_loopback(DE_NET_FAMILY_IPV4, 0);
    de__socket *inner = NULL, *receiver = NULL, *wrapped = NULL;
    ASSERT_EQ(de__udp_open(&bind, NULL, NULL, &inner), DE_OK);
    ASSERT_EQ(de__udp_open(&bind, NULL, NULL, &receiver), DE_OK);
    u64 time = 0;
    const de__net_sim_desc d = {.queue_capacity = 8, .now_ns = fake_now, .clock_user = &time};
    ASSERT_EQ(de__net_sim_wrap_ex(inner, &d, NULL, &wrapped), DE_OK);
    const de_net_sim settings = {.latency_ms = 10};
    ASSERT_EQ(de__net_sim_set(wrapped, &settings), DE_OK);
    const packet p = {.id = 77, .canary = 0x12345678};
    u32 sent;
    ASSERT_EQ(de__socket_send_to(wrapped, &receiver->local, &p, sizeof p, &sent), DE_OK);
    packet received;
    de__datagram m = {.data = &received, .capacity = sizeof received};
    u32 got;
    ASSERT_EQ(de__socket_recv_from(receiver, &m, 1, &got), DE_OK);
    EXPECT_EQ(got, 0u);
    time = 9 * NS_MS;
    ASSERT_EQ(de__net_sim_pump(wrapped), DE_OK);
    ASSERT_EQ(de__socket_recv_from(receiver, &m, 1, &got), DE_OK);
    EXPECT_EQ(got, 0u);
    time = 10 * NS_MS;
    ASSERT_EQ(de__net_sim_pump(wrapped), DE_OK);
    const u64 deadline = de_time_now_ns() + 1000 * NS_MS;
    do {
        ASSERT_EQ(de__socket_recv_from(receiver, &m, 1, &got), DE_OK);
    } while (!got && de_time_now_ns() < deadline);
    ASSERT_EQ(got, 1u);
    EXPECT_EQ(received.id, 77u);
    EXPECT_TRUE(de_net_address_equal(&m.from, &wrapped->local));
    de__socket_close(wrapped);
    de__socket_close(receiver);
}
TEST(net_sim, live_cvars_and_explicit_settings_have_clear_ownership) {
    rig r;
    open_rig(&r, 4, 1, false);
    de__socket *wrapped = NULL;
    const de__net_sim_desc d = {
        .queue_capacity = 8, .use_cvars = true, .now_ns = fake_now, .clock_user = &r.ns};
    ASSERT_EQ(de__net_sim_wrap_ex(r.tx, &d, NULL, &wrapped), DE_OK);
    r.tx = wrapped;
    ASSERT_EQ(de_cvar_set(de_cvar_find("net.sim.latency_ms"), "10"), DE_OK);
    send_packet(&r, 3);
    packet p;
    ASSERT_EQ(receive(&r, &p, 1), 0u);
    r.ns = 10 * NS_MS;
    ASSERT_EQ(de__net_sim_pump(r.tx), DE_OK);
    ASSERT_EQ(receive(&r, &p, 1), 1u);
    const de_net_sim zero = {0};
    ASSERT_EQ(de__net_sim_set(r.tx, &zero), DE_OK);
    ASSERT_EQ(de_cvar_set(de_cvar_find("net.sim.loss"), "1"), DE_OK);
    send_packet(&r, 4);
    ASSERT_EQ(receive(&r, &p, 1), 1u);
    EXPECT_EQ(p.id, 4u);
    ASSERT_EQ(de_cvar_set(de_cvar_find("net.sim.latency_ms"), "0"), DE_OK);
    ASSERT_EQ(de_cvar_set(de_cvar_find("net.sim.loss"), "0"), DE_OK);
    close_rig(&r);
}
#endif
