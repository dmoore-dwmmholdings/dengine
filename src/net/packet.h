/* DENG-53 channel PAYLOAD format (inside the DENG-52 authenticated packet):
 *   packet sequence u16 LE | ack u16 LE | ack_bits u32 LE | flags u8
 *   repeated: channel u8 | message id u16 LE | length minimal LEB128 | bytes
 * flags bit0 means ack is present; bit1 marks a padded MTU probe; others are zero.
 * A probe has >=1200 channel-payload bytes, only zero padding after the header,
 * no messages, and solicits an ACK. Probe size is matched to the actual encrypted
 * datagram by the transport overhead callback. A normal message's channel byte
 * reserves bit7 for fragmentation (fragment.h); its lower7 bits are channel0..2.
 * The acknowledgement-present flag
 * prevents an initial absent ack from acknowledging packet zero, including after wrap.
 * ack acknowledges itself and bit N acknowledges ack-(N+1), for 33 packets total.
 * All channels carry an id; unreliable packets are deduplicated by packet sequence.
 * Empty packets carry acknowledgements only and do not themselves solicit an ack.
 * Maximum channel payload is1452 (initial1200). At most288
 * messages fit (a message needs at least 5 bytes). Reject the entire malformed body
 * before mutating acknowledgement/delivery state. One connection owns all sequence
 * spaces; its authenticated outer replay window excludes packets from an old wrap.
 */
#ifndef DE_NET_PACKET_H
#define DE_NET_PACKET_H
#include "net/net_internal.h"

#define DE__CHAN_PACKET_MAX      1452u
#define DE__CHAN_HEADER          9u
#define DE__CHAN_MESSAGE_MAX     (1200u - DE__CHAN_HEADER - 5u)
#define DE__CHAN_PACKET_MESSAGES ((DE__CHAN_PACKET_MAX - DE__CHAN_HEADER) / 5u)
#define DE__CHAN_WINDOW          256u
#define DE__CHAN_ACK_WINDOW      1024u

typedef struct de__chan_message_view {
    de_channel channel;
    u16 id;
    u32 bytes;
    const u8 *data;
    bool fragmented;
} de__chan_message_view;
typedef struct de__chan_packet {
    u16 sequence, ack;
    u32 ack_bits, count;
    bool has_ack;
    bool probe;
    de__chan_message_view messages[DE__CHAN_PACKET_MESSAGES];
} de__chan_packet;
bool de__chan_packet_read(const void *data, u32 bytes, de__chan_packet *out);
void de__chan_packet_header(u8 *out, u16 sequence, bool has_ack, u16 ack, u32 ack_bits);
u32 de__chan_packet_message(u8 *out, u32 capacity, de_channel channel, u16 id, const void *data,
                            u32 bytes);
u32 de__chan_packet_fragment(u8 *out, u32 capacity, de_channel channel, u16 id, const void *data,
                             u32 bytes);
void de__chan_packet_probe(u8 *out, u32 bytes, u16 sequence, bool has_ack, u16 ack, u32 ack_bits);

typedef struct de__chan_sequence_window {
    u64 bits[DE__CHAN_ACK_WINDOW / 64u];
    u16 newest;
    bool initialized;
} de__chan_sequence_window;
bool de__chan_sequence_newer(u16 a, u16 b);
bool de__chan_sequence_seen(const de__chan_sequence_window *w, u16 sequence);
void de__chan_sequence_mark(de__chan_sequence_window *w, u16 sequence);
u32 de__chan_ack_bits(const de__chan_sequence_window *w);
bool de__chan_acknowledges(u16 sequence, u16 ack, u32 bits);

typedef struct de__chan_rtt {
    u64 smoothed_ns, variance_ns;
    bool initialized;
} de__chan_rtt;
/* RFC6298's alpha=1/8, beta=1/4 estimator, not TCP's RTO policy. */
void de__chan_rtt_sample(de__chan_rtt *rtt, u64 sample_ns);
u64 de__chan_resend_ns(const de__chan_rtt *rtt);
#endif
