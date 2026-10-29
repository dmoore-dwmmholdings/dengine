#include "net/congestion.h"
#include <string.h>
#define SECOND 1000000000ull
#define FLOOR  (DE__CC_FLOOR_KBPS * 125u)
static u32 packet_burst(const de__cc_state *c) {
    const u32 burst = c->tick_hz / 30u;
    return c->bad || burst < 2u ? 2u : burst > 64u ? 64u : burst;
}
static u32 cap_bytes(u32 kbps) {
    if (kbps < DE__CC_FLOOR_KBPS)
        kbps = DE__CC_FLOOR_KBPS;
    if (kbps > 1000000u)
        kbps = 1000000u;
    return kbps * 125u;
}
void de__cc_init(de__cc_state *c, u32 kbps, u32 hz, u32 mtu) {
    memset(c, 0, sizeof *c);
    c->cap = cap_bytes(kbps);
    c->rate = FLOOR; /* probe upward rather than burst at an unknown path's cap */
    c->mtu = mtu ? mtu : 1200u;
    c->tick_hz = hz ? hz : 60u;
    if (c->tick_hz > 1000000u)
        c->tick_hz = 1000000u;
    c->byte_credit = (u64)c->mtu * 4u * SECOND;
    c->packet_credit = (u64)packet_burst(c) * SECOND;
}
void de__cc_ack(de__cc_state *c, u64 rtt) {
    if (!rtt)
        return; /* zero means no fresh timing sample, including other ACK bits */
    c->latest_rtt_ns = rtt;
    if (rtt && (!c->min_rtt_ns || rtt < c->min_rtt_ns))
        c->min_rtt_ns = rtt;
}
void de__cc_loss(de__cc_state *c) {
    c->loss_events++;
}
void de__cc_tick(de__cc_state *c, u64 now, u32 kbps, u64 srtt, u32 lost, u32 resolved) {
    c->cap = cap_bytes(kbps);
    if (c->rate > c->cap)
        c->rate = c->cap;
    if (!c->initialized) {
        c->initialized = true;
        c->now_ns = c->control_ns = now;
        return;
    }
    if (now < c->now_ns)
        return;
    const u64 delta = now - c->now_ns, elapsed = delta > SECOND ? SECOND : delta;
    const u64 burst = (u64)c->mtu * 4u * SECOND, minted = elapsed * c->rate;
    c->byte_credit = minted >= burst - c->byte_credit ? burst : c->byte_credit + minted;
    const u64 packets = elapsed * (c->bad ? 10u : c->tick_hz);
    const u64 packet_max = (u64)packet_burst(c) * SECOND;
    if (c->packet_credit > packet_max)
        c->packet_credit = packet_max;
    c->packet_credit =
        packets >= packet_max - c->packet_credit ? packet_max : c->packet_credit + packets;
    c->now_ns = now;
    const bool enough = resolved >= 32u;
    const bool high_loss = enough && (u64)lost * 100u > (u64)resolved * 5u;
    // A raw sample detects a building queue before the 1/8 SRTT catches up.
    // Stop cutting on old SRTT once returned samples show the queue drained.
    const bool high_delay =
        c->min_rtt_ns && ((c->latest_rtt_ns > c->min_rtt_ns &&
                           c->latest_rtt_ns - c->min_rtt_ns > c->min_rtt_ns / 2u) ||
                          (srtt > c->min_rtt_ns && srtt - c->min_rtt_ns > c->min_rtt_ns / 2u &&
                           c->latest_rtt_ns > c->min_rtt_ns &&
                           c->latest_rtt_ns - c->min_rtt_ns > c->min_rtt_ns / 4u));
    const bool severe =
        (enough && (u64)lost * 5u > resolved) ||
        (c->min_rtt_ns && srtt > c->min_rtt_ns && srtt - c->min_rtt_ns > c->min_rtt_ns);
    if (!c->bad) {
        if (severe && !c->bad_pending) {
            c->bad_pending = true;
            c->bad_since = now;
        }
        if (!severe)
            c->bad_pending = false;
        if (c->bad_pending && now - c->bad_since >= SECOND / 2u) {
            c->bad = true;
            c->bad_pending = false;
            c->good_pending = false;
            if (c->packet_credit > SECOND)
                c->packet_credit = SECOND;
        }
    } else {
        const bool good =
            enough && !high_loss &&
            (!c->min_rtt_ns || srtt <= c->min_rtt_ns || srtt - c->min_rtt_ns <= c->min_rtt_ns / 4u);
        if (good && !c->good_pending) {
            c->good_pending = true;
            c->good_since = now;
        }
        if (!good)
            c->good_pending = false;
        if (c->good_pending && now - c->good_since >= 2u * SECOND) {
            c->bad = false;
            c->good_pending = false;
        }
    }
    const u64 interval = srtt > SECOND / 10u ? srtt : SECOND / 10u;
    if (now - c->control_ns < interval)
        return;
    c->control_ns = now;
    if (high_delay || (high_loss && c->loss_events != c->cut_loss_events)) {
        c->rate = c->rate * 3u / 4u;
        if (c->rate < FLOOR)
            c->rate = FLOOR;
        c->cut_loss_events = c->loss_events;
        const u64 small_burst = (u64)c->mtu * SECOND;
        if (c->byte_credit > small_burst)
            c->byte_credit = small_burst;
    } else if (!high_loss && !high_delay) {
        const u64 next = (u64)c->rate + c->mtu;
        c->rate = next > c->cap ? c->cap : (u32)next;
    }
}
u32 de__cc_budget(const de__cc_state *c) {
    if (c->packet_credit < SECOND)
        return 0;
    const u64 bytes = c->byte_credit / SECOND;
    return bytes > UINT32_MAX ? UINT32_MAX : (u32)bytes;
}
void de__cc_sent(de__cc_state *c, u32 bytes) {
    const u64 charge = (u64)bytes * SECOND;
    c->byte_credit = charge > c->byte_credit ? 0 : c->byte_credit - charge;
    c->packet_credit = c->packet_credit < SECOND ? 0 : c->packet_credit - SECOND;
}
u32 de__cc_send_budget(const de__conn *c) {
    const de__cc_state *state = de__conn_cc_view(c);
    return state ? de__cc_budget(state) : 0;
}
void de__cc_on_ack(de__conn *c, u32 bytes, u64 rtt) {
    (void)bytes;
    if (c)
        de__cc_ack(de__conn_cc(c), rtt);
}
void de__cc_on_loss(de__conn *c, u32 bytes) {
    (void)bytes;
    if (c)
        de__cc_loss(de__conn_cc(c));
}
