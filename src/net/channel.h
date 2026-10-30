/* DENG-53 internal per-connection channel state. Public sessions are S32 DENG-115. */
#ifndef DE_NET_CHANNEL_H
#define DE_NET_CHANNEL_H
#include "net/packet.h"
#include "net/fragment.h"
#include "net/mtu.h"

typedef struct de__chan_desc {
    const de_allocator *allocator;
    /* Accepted datagram = DE_OK (including simulated/network loss). Error = not accepted.
     * Typically calls de__net_client_send or de__net_server_send for encryption/routing. */
    de_result (*send_packet)(void *user, const void *data, u32 bytes);
    void *send_user;
    u64 (*now_ns)(void *user);
    void *clock_user;
    /* 0 uses the live net.max_bandwidth_kbps cvar (default512, floor32).
     * A nonzero cap is connection-local; tick rate defaults to60 packets/s. */
    u32 max_bandwidth_kbps, tick_rate_hz;
    u32 max_message_bytes, reassembly_pool_bytes; /* zero uses setup-time cvars */
    bool disable_mtu_discovery; /* fixed1200 payload for a backend without probes */
} de__chan_desc;
typedef struct de__chan_counters {
    u64 bytes_in, bytes_out, msgs_in, msgs_out, resends, dropped;
} de__chan_counters;
typedef struct de__chan_stats {
    de__chan_counters channel[3];
    u64 packets_in, packets_out, acked, lost, malformed, duplicate, backpressure;
    u64 rtt_ns, jitter_ns;
    u32 pending[3];
    size_t allocated_bytes;
} de__chan_stats;

/* A bounded window block (<=5MiB), plus one setup allocation for the fragment
 * pool (default1MiB data plus fixed metadata). No send/read/write/receive
 * allocations, locks or background timers.
 * Single thread, no reentrant read/write/receive/reset/destroy; message callbacks may send.
 * Reset before reusing storage for a new authenticated connection. Destruction is not owned
 * by the transport binding: unbind before destroy. Queued payloads are copied.
 * Each write emits at most64 packets within the congestion budget. Ordinary unreliable
 * traffic gets one tick; fragmented unreliable traffic gets up to1s to emit its parts.
 * Counters count whole application messages/bytes once, with reliable sub-message resends
 * separate. stats.pending counts retained sub-messages, not whole fragmented messages. */
de_result de__chan_create(const de__chan_desc *desc, de__conn **out);
void de__chan_reset(de__conn *conn);
void de__chan_destroy(de__conn *conn);
de_result de__chan_send(de__conn *conn, de_channel channel, const void *data, u32 bytes);
typedef void (*de__chan_message_fn)(void *user, de_channel channel, const void *data, u32 bytes);
void de__chan_receive(de__conn *conn, de__chan_message_fn fn, void *user);
de_result de__conn_write_packets(de__conn *conn, u64 now_ns);
de_result de__conn_read_payload(de__conn *conn, const void *bytes, u32 len);
de__chan_stats de__chan_get_stats(const de__conn *conn);
/* The authenticated transport supplies its actual next datagram header/tag bytes,
 * so padded probes and packet packing remain correct across outer varint changes.
 * Binding/reset clears this non-owning callback before the transport can disappear. */
void de__chan_wire_overhead(de__conn *conn, u32 (*overhead)(void *), void *user);
de__fragment_stats de__chan_fragment_stats(const de__conn *conn);
const de__mtu_state *de__conn_mtu_view(const de__conn *conn);
u32 de__conn_wire_overhead(const de__conn *conn);
/* Explicit DISCONNECT detail codes; zero means no protocol failure. */
#define DE__CHAN_ERROR_FRAGMENT_FORMAT 0xf1u
#define DE__CHAN_ERROR_FRAGMENT_POOL   0xf2u
u8 de__chan_protocol_error(const de__conn *conn);
#endif
