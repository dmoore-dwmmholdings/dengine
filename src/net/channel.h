/* DENG-53 internal per-connection channel state. Public sessions are S32 DENG-115. */
#ifndef DE_NET_CHANNEL_H
#define DE_NET_CHANNEL_H
#include "net/packet.h"

typedef struct de__chan_desc {
    const de_allocator *allocator;
    /* Accepted datagram = DE_OK (including simulated/network loss). Error = not accepted.
     * Typically calls de__net_client_send or de__net_server_send for encryption/routing. */
    de_result (*send_packet)(void *user, const void *data, u32 bytes);
    void *send_user;
    u64 (*now_ns)(void *user);
    void *clock_user;
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

/* One bounded allocation at create, including all 256-message channel windows and
 * the 1024 sent-packet records. No send/read/write/receive allocations, locks or timers.
 * Single thread, no reentrant read/write/receive/reset/destroy; message callbacks may send.
 * Reset before reusing storage for a new authenticated connection. Destruction is not owned
 * by the transport binding: unbind before destroy. Queued payloads are copied.
 * Each write emits at most64 packets within the congestion budget; remaining unreliable
 * traffic is dropped this tick. Counters count unique first sends/application deliveries;
 * resends are additional message transmissions, not additional unique bytes/messages. */
de_result de__chan_create(const de__chan_desc *desc, de__conn **out);
void de__chan_reset(de__conn *conn);
void de__chan_destroy(de__conn *conn);
de_result de__chan_send(de__conn *conn, de_channel channel, const void *data, u32 bytes);
typedef void (*de__chan_message_fn)(void *user, de_channel channel, const void *data, u32 bytes);
void de__chan_receive(de__conn *conn, de__chan_message_fn fn, void *user);
de_result de__conn_write_packets(de__conn *conn, u64 now_ns);
de_result de__conn_read_payload(de__conn *conn, const void *bytes, u32 len);
de__chan_stats de__chan_get_stats(const de__conn *conn);
#endif
