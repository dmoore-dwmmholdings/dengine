/* DENG-55: allocation-free per-peer byte/packet pacing and AIMD state. */
#ifndef DE_NET_CONGESTION_H
#define DE_NET_CONGESTION_H
#include "net/net_internal.h"
#define DE__CC_DEFAULT_KBPS 512u
#define DE__CC_FLOOR_KBPS   32u
typedef struct de__cc_state {
    u64 now_ns, byte_credit, packet_credit, control_ns, min_rtt_ns, latest_rtt_ns;
    u64 bad_since, good_since, loss_events, cut_loss_events;
    u32 rate, cap, mtu, tick_hz;
    bool initialized, bad, bad_pending, good_pending;
} de__cc_state;
/* Decimal kilobits/s. Credits use bytes*1e9 and packets*1e9; four MTUs and
 * ~33ms of packets (2..64, default2) maximum burst. Cap changes/clock regressions are safe. */
void de__cc_init(de__cc_state *cc, u32 kbps, u32 tick_hz, u32 mtu);
void de__cc_tick(de__cc_state *cc, u64 now, u32 kbps, u64 srtt, u32 lost, u32 resolved);
u32 de__cc_budget(const de__cc_state *cc);
void de__cc_sent(de__cc_state *cc, u32 bytes);
void de__cc_ack(de__cc_state *cc, u64 rtt);
void de__cc_loss(de__cc_state *cc);
de__cc_state *de__conn_cc(de__conn *conn);
const de__cc_state *de__conn_cc_view(const de__conn *conn);
#endif
