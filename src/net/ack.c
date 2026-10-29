#include "net/packet.h"
#include <string.h>

bool de__chan_sequence_newer(u16 a, u16 b) {
    const u16 distance = (u16)(a - b);
    return distance != 0 && distance < 0x8000u;
}
bool de__chan_sequence_seen(const de__chan_sequence_window *w, u16 sequence) {
    if (!w->initialized || de__chan_sequence_newer(sequence, w->newest))
        return false;
    const u16 d = (u16)(w->newest - sequence);
    return d >= DE__CHAN_ACK_WINDOW || ((w->bits[d / 64u] >> (d % 64u)) & 1u) != 0;
}
void de__chan_sequence_mark(de__chan_sequence_window *w, u16 sequence) {
    if (!w->initialized) {
        memset(w, 0, sizeof *w);
        w->initialized = true;
        w->newest = sequence;
    } else if (de__chan_sequence_newer(sequence, w->newest)) {
        const u32 d = (u16)(sequence - w->newest);
        for (u32 i = DE__CHAN_ACK_WINDOW / 64u; i-- > 0;) {
            u64 value = 0;
            if (i >= d / 64u) {
                value = w->bits[i - d / 64u] << (d % 64u);
                if (d % 64u && i > d / 64u)
                    value |= w->bits[i - d / 64u - 1] >> (64u - d % 64u);
            }
            w->bits[i] = value;
        }
        w->newest = sequence;
    }
    const u32 d = (u16)(w->newest - sequence);
    if (d < DE__CHAN_ACK_WINDOW)
        w->bits[d / 64u] |= 1ull << (d % 64u);
}
u32 de__chan_ack_bits(const de__chan_sequence_window *w) {
    return (u32)(w->bits[0] >> 1);
}
bool de__chan_acknowledges(u16 sequence, u16 ack, u32 bits) {
    const u16 d = (u16)(ack - sequence);
    return d == 0 || (d <= 32 && ((bits >> (d - 1)) & 1u) != 0);
}
