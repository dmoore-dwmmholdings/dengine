/* DENG-55: ten 100ms intervals plus a partial boundary bucket; 256 packet
 * outcomes. No allocation and no global state. All times are monotonic ns. */
#ifndef DE_NET_STATS_H
#define DE_NET_STATS_H
#include "net/channel.h"
#define DE__NET_RATE_NS 100000000ull
typedef struct de__channel_rates {
    u32 bytes_in_per_sec, bytes_out_per_sec;
    u32 msgs_in_per_sec, msgs_out_per_sec, resends_per_sec;
} de__channel_rates;
typedef struct de__rate_bucket {
    u64 epoch, bytes_in, bytes_out;
    de__chan_counters channel[3];
    bool valid;
} de__rate_bucket;
typedef struct de__loss_record {
    u32 serial;
    u8 outcome; /* 0 empty, 1 unresolved, 2 acknowledged, 3 timed out */
} de__loss_record;
typedef struct de__net_stats {
    de__rate_bucket buckets[11];
    de__loss_record packets[256];
    u32 next_serial;
    u64 packets_in, packets_out;
} de__net_stats;
void de__stats_packet_in(de__net_stats *stats, u64 now, u32 bytes);
u32 de__stats_packet_out(de__net_stats *stats, u64 now, u32 bytes, bool expects_ack);
void de__stats_outcome(de__net_stats *stats, u32 serial, bool lost);
void de__stats_loss(const de__net_stats *stats, u32 *lost, u32 *resolved);
void de__stats_channel(de__net_stats *stats, u64 now, de_channel channel, u32 bytes, bool incoming,
                       bool resend);
de_peer_stats de__stats_peer(const de__net_stats *stats, u64 now, u64 rtt, u64 jitter);
de__channel_rates de__stats_rates(const de__net_stats *stats, u64 now, de_channel channel);
/* Peer bytes include channel framing/ack-only packets, excluding outer crypto,
 * IP and UDP. Channel rates count unique application bytes/messages, with resends
 * separate. Weighting the partial boundary assumes uniform traffic within100ms.
 * Unresolved packets are excluded from loss; late ACKs correct retained timeouts.
 * Public sessions/panels expose these in DENG-115/122. */
de_peer_stats de__conn_stats(const de__conn *conn);
bool de__conn_channel_stats(const de__conn *conn, de_channel channel, de__channel_rates *out);
#endif
