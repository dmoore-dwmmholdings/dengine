#include "net/packet.h"
#include "net/crypto.h"
#include <string.h>

static u16 read16(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}
static void write16(u8 *p, u16 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}
void de__chan_packet_header(u8 *out, u16 sequence, bool has_ack, u16 ack, u32 bits) {
    write16(out, sequence);
    write16(out + 2, has_ack ? ack : 0);
    de__net_put_u32(out + 4, has_ack ? bits : 0);
    out[8] = has_ack ? 1 : 0;
}
u32 de__chan_packet_message(u8 *out, u32 cap, de_channel channel, u16 id, const void *data,
                            u32 bytes) {
    if (!out || !data || !bytes || bytes > DE__CHAN_MESSAGE_MAX ||
        (u32)channel > DE_CHANNEL_RELIABLE_UNORDERED)
        return 0;
    u8 length[DE__NET_VARINT_MAX];
    const u32 n = de__net_varint_write(length, bytes);
    if (cap < 3u + n + bytes)
        return 0;
    out[0] = (u8)channel;
    write16(out + 1, id);
    memcpy(out + 3, length, n);
    memcpy(out + 3 + n, data, bytes);
    return 3u + n + bytes;
}
u32 de__chan_packet_fragment(u8 *out, u32 cap, de_channel channel, u16 id, const void *data,
                             u32 bytes) {
    const u32 n = de__chan_packet_message(out, cap, channel, id, data, bytes);
    if (n)
        out[0] |= 0x80u;
    return n;
}
void de__chan_packet_probe(u8 *out, u32 bytes, u16 sequence, bool has_ack, u16 ack, u32 bits) {
    if (!out || bytes < 1200u || bytes > DE__CHAN_PACKET_MAX)
        return;
    memset(out, 0, bytes);
    de__chan_packet_header(out, sequence, has_ack, ack, bits);
    out[8] |= 2u;
}
bool de__chan_packet_read(const void *data, u32 bytes, de__chan_packet *out) {
    if (!data || !out || bytes < DE__CHAN_HEADER || bytes > DE__CHAN_PACKET_MAX)
        return false;
    const u8 *p = data;
    if ((p[8] & ~3u) || (!(p[8] & 1u) && (read16(p + 2) || de__net_get_u32(p + 4))))
        return false;
    out->sequence = read16(p);
    out->ack = read16(p + 2);
    out->ack_bits = de__net_get_u32(p + 4);
    out->has_ack = (p[8] & 1u) != 0;
    out->probe = (p[8] & 2u) != 0;
    out->count = 0;
    if (out->probe) {
        if (bytes < 1200u)
            return false;
        for (u32 i = DE__CHAN_HEADER; i < bytes; ++i)
            if (p[i])
                return false;
        return true;
    }
    u32 at = DE__CHAN_HEADER;
    while (at < bytes) {
        if (bytes - at < 5 || out->count == DE__CHAN_PACKET_MESSAGES ||
            (p[at] & 0x7fu) > DE_CHANNEL_RELIABLE_UNORDERED)
            return false;
        u64 length = 0;
        const u32 n = de__net_varint_read(p + at + 3, bytes - at - 3, &length);
        if (!n || !length || length > DE__CHAN_MESSAGE_MAX || length > bytes - at - 3 - n)
            return false;
        out->messages[out->count++] =
            (de__chan_message_view){(de_channel)(p[at] & 0x7fu), read16(p + at + 1), (u32)length,
                                    p + at + 3 + n, (p[at] & 0x80u) != 0};
        at += 3u + n + (u32)length;
    }
    return true;
}
