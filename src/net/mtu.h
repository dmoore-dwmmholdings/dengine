/* DENG-54 allocation-free path discovery. Sizes are actual UDP datagram bytes;
 * the channel layer subtracts its transport's current authenticated overhead.
 * Ordinary payload starts/falls back to1200 bytes. Probes never carry app data.
 */
#ifndef DE_NET_MTU_H
#define DE_NET_MTU_H
#include "net/net_internal.h"
#define DE__MTU_BASE_PAYLOAD 1200u
#define DE__MTU_MAX_WIRE     1452u
#define DE__MTU_REPROBE_NS   (600ull * 1000000000ull)
typedef struct de__mtu_state {
    u64 now, next_probe, sent_at, timeout;
    u32 confirmed_wire, lower, upper, candidate, attempts, stage, full_losses;
    u16 probe_sequences[3];
    u64 last_full_loss;
    bool initialized, pending, searching, have_full_loss;
} de__mtu_state;
void de__mtu_init(de__mtu_state *state);
/* Returns a due probe's wire size (zero while waiting/finished). Call sent only
 * after the send callback accepts it; packet pacing remains the channel's job. */
u32 de__mtu_probe_due(de__mtu_state *state, u64 now, u32 overhead, u64 srtt_ns);
void de__mtu_probe_sent(de__mtu_state *state, u16 sequence, u64 now);
void de__mtu_probe_ack(de__mtu_state *state, u16 sequence, u32 wire_bytes, u64 now);
/* Local64-bit transmission generations, not wrapping wire IDs: late outcomes
 * remain distinguishable even after many partial packets or long idle periods. */
void de__mtu_full_ack(de__mtu_state *state, u64 generation);
void de__mtu_full_loss(de__mtu_state *state, u64 generation, u64 now, u32 overhead);
u32 de__mtu_capacity(const de__mtu_state *state, u32 overhead);
#endif
