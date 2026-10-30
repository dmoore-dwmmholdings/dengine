#include "de_test.h"
#include "de_alloc_guard.h"
#include "net/mtu.h"
#include "net/congestion.h"
#include "fragment_transport.h"

#define MS 1000000ull
static u64 converge(de__mtu_state *s, u32 maximum, u32 overhead, u16 *sequence) {
    for (u64 now = 0; now < 5000 * MS; now += 10 * MS) {
        const u32 size = de__mtu_probe_due(s, now, overhead, 0);
        if (size) {
            ASSERT_TRUE(size <= DE__MTU_MAX_WIRE);
            const u16 seq = (*sequence)++;
            de__mtu_probe_sent(s, seq, now);
            if (size <= maximum)
                de__mtu_probe_ack(s, seq, size, now);
        }
        if (s->initialized && !s->searching)
            return now;
    }
    ASSERT_TRUE(false);
    return 0;
}
TEST(net_mtu, probe_converges_to_wire_limit_without_allocations) {
    de__mtu_state s;
    de__mtu_init(&s);
    ASSERT_EQ(de__mtu_capacity(&s, 27), 1200u);
    u16 sequence = 65530;
    de_alloc_guard guard = {0};
    de_alloc_guard_begin(&guard);
    const u64 done = converge(&s, 1400, 27, &sequence);
    de_alloc_guard_end(&guard);
    DE_ALLOC_GUARD_ASSERT_ZERO(&guard);
    ASSERT_TRUE(done < 5000 * MS);
    ASSERT_EQ(s.confirmed_wire, 1400u);
    ASSERT_EQ(de__mtu_capacity(&s, 27), 1373u);
    ASSERT_EQ(s.upper, 1401u);
    ASSERT_EQ(de__mtu_probe_due(&s, s.next_probe - 1, 27, 0), 0u);
}
TEST(net_mtu, lost_probes_keep_lower_ack_requires_sent_candidate) {
    de__mtu_state s = {0};
    ASSERT_EQ(de__mtu_probe_due(&s, 0, 19, 0), 0u);
    ASSERT_EQ(de__mtu_probe_due(&s, 100 * MS, 19, 0), 1280u);
    de__mtu_probe_sent(&s, 10, 100 * MS);
    de__mtu_probe_ack(&s, 11, 1280, 110 * MS);
    ASSERT_EQ(de__mtu_capacity(&s, 19), 1200u);
    de__mtu_probe_ack(&s, 10, 1400, 110 * MS);
    ASSERT_EQ(de__mtu_capacity(&s, 19), 1200u);
    ASSERT_EQ(de__mtu_probe_due(&s, 200 * MS, 19, 0), 1280u);
    de__mtu_probe_sent(&s, 12, 200 * MS);
    /* The first attempt's late authenticated ACK is still valid for this size. */
    de__mtu_probe_ack(&s, 10, 1280, 210 * MS);
    ASSERT_EQ(de__mtu_capacity(&s, 19), 1261u);
    ASSERT_EQ(de__mtu_probe_due(&s, 210 * MS, 19, 0), 1400u);
    for (u32 i = 0; i < 3; ++i) {
        de__mtu_probe_sent(&s, (u16)(20 + i), (210 + i * 100) * MS);
        ASSERT_EQ(de__mtu_capacity(&s, 19), 1261u);
        const u32 due = de__mtu_probe_due(&s, (310 + i * 100) * MS, 19, 0);
        ASSERT_EQ(due, i == 2 ? 1340u : 1400u);
    }
    ASSERT_EQ(s.upper, 1400u);
}
TEST(net_mtu, blackhole_three_full_losses_wrap_late_ack_and_reprobe) {
    de__mtu_state s = {0};
    u16 seq = 0;
    const u64 done = converge(&s, 1452, 19, &seq);
    ASSERT_EQ(s.confirmed_wire, 1452u);
    de__mtu_full_loss(&s, 65534, done, 19);
    de__mtu_full_loss(&s, 65535, done, 19);
    ASSERT_EQ(s.confirmed_wire, 1452u);
    de__mtu_full_ack(&s, 65534); /* an old good packet cannot hide the later losses */
    de__mtu_full_loss(&s, 65536, done, 19);
    ASSERT_EQ(de__mtu_capacity(&s, 19), 1200u);
    ASSERT_EQ(de__mtu_probe_due(&s, done + 999 * MS, 19, 0), 0u);
    ASSERT_EQ(de__mtu_probe_due(&s, done + 1000 * MS, 19, 0), 1280u);
    de__mtu_probe_sent(&s, 1, done + 1000 * MS);
    de__mtu_probe_ack(&s, 1, 1280, done + 1010 * MS);
    const u64 retry = done + 1010 * MS;
    de__mtu_probe_due(&s, retry, 19, 0);
    de__mtu_probe_sent(&s, 2, retry);
    de__mtu_probe_ack(&s, 2, 1400, retry);
    de__mtu_probe_due(&s, retry, 19, 0);
    de__mtu_probe_sent(&s, 3, retry);
    de__mtu_probe_ack(&s, 3, 1452, retry);
    const u64 reprobe = s.next_probe;
    ASSERT_EQ(reprobe, retry + DE__MTU_REPROBE_NS);
    ASSERT_EQ(de__mtu_probe_due(&s, reprobe, 19, 0), 1452u);
    de__mtu_probe_sent(&s, 4, reprobe);
    de__mtu_probe_ack(&s, 4, 1452, reprobe);
    ASSERT_EQ(s.next_probe, reprobe + DE__MTU_REPROBE_NS);
    /* Even a maximum-sized current path is actually re-probed, not skipped. */
    ASSERT_EQ(s.confirmed_wire, 1452u);
    de__mtu_full_ack(&s, 100000);
    de__mtu_full_loss(&s, 99999, reprobe, 19); /* older loss cannot cross a later good packet */
    de__mtu_full_loss(&s, 100001, reprobe, 19);
    de__mtu_full_loss(&s, 100002, reprobe, 19);
    ASSERT_EQ(s.confirmed_wire, 1452u);
    de__mtu_full_loss(&s, 100003, reprobe, 19);
    ASSERT_EQ(de__mtu_capacity(&s, 19), 1200u);
}

#if defined(DE_DEV) || defined(DE_DEBUG)
static void ignore_message(void *user, de_channel ch, const void *data, u32 bytes) {
    (void)user;
    (void)ch;
    (void)data;
    (void)bytes;
}
TEST(net_mtu, encrypted_path_1400_converges_and_three_full_losses_fall_back) {
    fragment_rig r;
    fragment_rig_open(&r, 0, true);
    bool converged = false;
    for (u32 i = 0; i < 5000; ++i) {
        ASSERT_EQ(fragment_tick(&r, ignore_message, NULL), DE_OK);
        const de__mtu_state *a = de__conn_mtu_view(r.channel[0]);
        const de__mtu_state *b = de__conn_mtu_view(r.channel[1]);
        if (!a->searching && !b->searching && a->confirmed_wire == 1400 &&
            b->confirmed_wire == 1400) {
            converged = true;
            break;
        }
    }
    ASSERT_TRUE(converged);
    ASSERT_TRUE(r.now < 5000 * MS);
    ASSERT_EQ(de__mtu_payload(r.channel[0]) + de__conn_wire_overhead(r.channel[0]), 1400u);
    ASSERT_TRUE(de__net_sim_get_stats(r.socket[0]).mtu_dropped >= 3u);
    /* Cross the outer sequence's variable-width nonce boundary while filling the
     * current wire MTU exactly. No ordinary datagram may exceed the real1400 cap. */
    const u64 dropped_before = de__net_sim_get_stats(r.socket[0]).mtu_dropped;
    u8 full[DE__CHAN_MESSAGE_MAX] = {0}, filler[256] = {0};
    for (u32 i = 0; i < 260; ++i) {
        de__conn_cc(r.channel[0])->rate = de__conn_cc_view(r.channel[0])->cap;
        const u32 extra = de__mtu_payload(r.channel[0]) - 1200u - 5u;
        ASSERT_TRUE(extra >= 128 && extra <= sizeof filler);
        ASSERT_EQ(de__chan_send(r.channel[0], DE_CHANNEL_RELIABLE_ORDERED, full, sizeof full),
                  DE_OK);
        ASSERT_EQ(de__chan_send(r.channel[0], DE_CHANNEL_RELIABLE_ORDERED, filler, extra), DE_OK);
        ASSERT_EQ(fragment_tick(&r, ignore_message, NULL), DE_OK);
    }
    ASSERT_EQ(de__net_sim_get_stats(r.socket[0]).mtu_dropped, dropped_before);
    ASSERT_EQ(de__net_sim_set_max_datagram(r.socket[0], 1300), DE_OK);
    de__conn_cc(r.channel[0])->rate = de__conn_cc_view(r.channel[0])->cap;
    for (u32 i = 0; i < 3; ++i) {
        const u32 extra = de__mtu_payload(r.channel[0]) - 1200u - 5u;
        ASSERT_EQ(de__chan_send(r.channel[0], DE_CHANNEL_RELIABLE_ORDERED, full, sizeof full),
                  DE_OK);
        ASSERT_EQ(de__chan_send(r.channel[0], DE_CHANNEL_RELIABLE_ORDERED, filler, extra), DE_OK);
    }
    ASSERT_EQ(fragment_tick(&r, ignore_message, NULL), DE_OK);
    ASSERT_EQ(de__net_sim_get_stats(r.socket[0]).mtu_dropped - dropped_before, 3u);
    bool fallback = false;
    for (u32 i = 0; i < 1000; ++i) {
        ASSERT_EQ(fragment_tick(&r, ignore_message, NULL), DE_OK);
        if (!de__conn_mtu_view(r.channel[0])->confirmed_wire) {
            fallback = true;
            break;
        }
    }
    ASSERT_TRUE(fallback);
    ASSERT_EQ(de__mtu_payload(r.channel[0]), 1200u);
    ASSERT_EQ(de__chan_get_stats(r.channel[0]).lost, 3u);
    fragment_rig_close(&r);
}
#endif
