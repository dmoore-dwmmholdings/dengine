#include "de_test.h"
#include "net/congestion.h"
#include "net/net_stats.h"
#include <stdio.h>
#include <stdlib.h>
#define MS  1000000ull
#define SEC (1000u * MS)
typedef struct flight {
    u64 sent, ack_at;
    u32 serial;
    bool loss;
} flight;

/* Test-local FIFO bottleneck: finite service rate, no hidden queue clamp or
 * simulator changes. ACKs return after propagation + serialization + queue. */
TEST(net_cc, converges_to_capacity_with_bounded_queue) {
    for (u32 capacity = 24000; capacity <= 56000; capacity += 16000) {
        de__cc_state cc;
        de__cc_init(&cc, 512, 60, 1200);
        de__net_stats stats = {0};
        de__chan_rtt rtt = {0};
        flight *q = calloc(4096, sizeof *q);
        ASSERT_TRUE(q != NULL);
        u32 head = 0, tail = 0;
        u64 service = 0, peak_queue = 0, steady_bytes = 0;
        for (u64 now = 0; now < 30u * SEC; now += MS) {
            while (head < tail && q[head % 4096u].ack_at <= now) {
                flight *p = &q[head++ % 4096u];
                de__stats_outcome(&stats, p->serial, false);
                de__cc_ack(&cc, now - p->sent);
                de__chan_rtt_sample(&rtt, now - p->sent);
            }
            u32 lost, resolved;
            de__stats_loss(&stats, &lost, &resolved);
            de__cc_tick(&cc, now, 512, rtt.smoothed_ns, lost, resolved);
            while (de__cc_budget(&cc) >= 1200) {
                ASSERT_TRUE(tail - head < 4096);
                if (service < now)
                    service = now;
                service += 1200u * SEC / capacity;
                q[tail++ % 4096u] = (flight){now, service + 200u * MS,
                                             de__stats_packet_out(&stats, now, 1200, true), false};
                de__cc_sent(&cc, 1200);
                if (now >= 25u * SEC)
                    steady_bytes += 1200;
                if (service - now > peak_queue)
                    peak_queue = service - now;
            }
        }
        const u64 rate = steady_bytes / 5u;
        printf("capacity%u B/s: actual%llu B/s, peak queue%.3fms, target%u B/s\n", capacity,
               (unsigned long long)rate, (double)peak_queue / MS, cc.rate);
        ASSERT_TRUE(rate * 100u >= (u64)capacity * 80u);
        ASSERT_TRUE(rate * 100u <= (u64)capacity * 120u);
        ASSERT_TRUE(peak_queue < 400u * MS); /* <2x base200ms RTT */
        free(q);
    }
}
TEST(net_cc, two_percent_random_loss_keeps_seventy_percent_target) {
    de__cc_state cc;
    de__cc_init(&cc, 512, 60, 1200);
    de__net_stats stats = {0};
    flight q[512] = {0};
    u32 head = 0, tail = 0, rng = 55, dropped = 0;
    u64 sent_bytes = 0;
    for (u64 now = 0; now < 30u * SEC; now += MS) {
        while (head < tail && q[head % 512u].ack_at <= now) {
            flight *p = &q[head++ % 512u];
            de__stats_outcome(&stats, p->serial, p->loss);
            if (p->loss) {
                de__cc_loss(&cc);
                dropped++;
            } else
                de__cc_ack(&cc, 100u * MS);
        }
        u32 lost, resolved;
        de__stats_loss(&stats, &lost, &resolved);
        de__cc_tick(&cc, now, 512, 100u * MS, lost, resolved);
        while (de__cc_budget(&cc) >= 1200) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            ASSERT_TRUE(tail - head < 512);
            q[tail++ % 512u] =
                (flight){now, now + 100u * MS, de__stats_packet_out(&stats, now, 1200, true),
                         rng % 100u < 2u};
            de__cc_sent(&cc, 1200);
            sent_bytes += 1200;
        }
    }
    printf("random loss:%u/%u, rate%llu B/s\n", dropped, tail,
           (unsigned long long)(sent_bytes / 30u));
    ASSERT_TRUE(dropped > 0);
    ASSERT_TRUE(sent_bytes / 30u >= 64000u * 70u / 100u);
    ASSERT_FALSE(cc.bad);
}
TEST(net_cc, bad_mode_hysteresis_and_rate_floor_cap_and_clock) {
    de__cc_state cc;
    de__cc_init(&cc, 512, 60, 1200);
    de__cc_ack(&cc, 100u * MS);
    de__cc_tick(&cc, 0, 512, 100u * MS, 0, 256);
    for (u64 now = 100u * MS; now <= 600u * MS; now += 100u * MS) {
        de__cc_loss(&cc);
        de__cc_tick(&cc, now, 512, 100u * MS, 100, 256);
        if (now < 600u * MS)
            ASSERT_FALSE(cc.bad);
    }
    ASSERT_TRUE(cc.bad);
    de__cc_tick(&cc, 700u * MS, 512, 100u * MS, 0, 256);
    de__cc_tick(&cc, 2600u * MS, 512, 100u * MS, 0, 256);
    ASSERT_TRUE(cc.bad);
    de__cc_tick(&cc, 2700u * MS, 512, 100u * MS, 0, 256);
    ASSERT_FALSE(cc.bad);
    de__cc_tick(&cc, 2800u * MS, 1, 100u * MS, 0, 256);
    ASSERT_EQ(cc.rate, 4000u);
    const u64 credit = cc.byte_credit;
    de__cc_tick(&cc, 1, 1, 100u * MS, 0, 256);
    ASSERT_EQ(cc.byte_credit, credit);
    de__cc_tick(&cc, UINT64_MAX, 1000000, 100u * MS, 0, 256);
    ASSERT_TRUE(cc.byte_credit <= 4800u * SEC);
    ASSERT_TRUE(de__cc_budget(&cc) <= 4800u);
}

TEST(net_cc, packet_pacing_tracks_tick_rate_and_bad_ten_pps) {
    de__cc_state cc;
    de__cc_init(&cc, 512, 144, 1200);
    de__cc_ack(&cc, 100u * MS);
    u32 good = 0, bad = 0;
    for (u64 now = 0; now < 2000u * MS; now += MS) {
        de__cc_tick(&cc, now, 512, 100u * MS, 0, 256);
        while (de__cc_budget(&cc)) {
            de__cc_sent(&cc, 1);
            if (now >= 1000u * MS)
                good++;
        }
    }
    ASSERT_EQ(good, 144u);
    cc.bad = true;
    cc.good_pending = false;
    cc.packet_credit = 0;
    for (u64 now = 2000u * MS; now < 3000u * MS; now += MS) {
        de__cc_tick(&cc, now, 512, 100u * MS, 100, 256);
        while (de__cc_budget(&cc)) {
            de__cc_sent(&cc, 1);
            bad++;
        }
    }
    ASSERT_EQ(bad, 10u);
}
