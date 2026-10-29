#include "net/net_stats.h"
#include <string.h>
static de__rate_bucket *bucket(de__net_stats *s, u64 now) {
    const u64 epoch = now / DE__NET_RATE_NS;
    de__rate_bucket *b = &s->buckets[epoch % 11u];
    if (!b->valid || b->epoch != epoch) {
        memset(b, 0, sizeof *b);
        b->valid = true;
        b->epoch = epoch;
    }
    return b;
}
void de__stats_packet_in(de__net_stats *s, u64 now, u32 bytes) {
    bucket(s, now)->bytes_in += bytes;
    s->packets_in++;
}
u32 de__stats_packet_out(de__net_stats *s, u64 now, u32 bytes, bool ack) {
    bucket(s, now)->bytes_out += bytes;
    s->packets_out++;
    if (!ack)
        return 0;
    const u32 serial = s->next_serial++;
    s->packets[serial % 256u] = (de__loss_record){serial, 1};
    return serial;
}
void de__stats_outcome(de__net_stats *s, u32 serial, bool lost) {
    de__loss_record *p = &s->packets[serial % 256u];
    if (p->outcome && p->serial == serial && (p->outcome != 2 || !lost))
        p->outcome = lost ? 3 : 2;
}
void de__stats_loss(const de__net_stats *s, u32 *lost, u32 *resolved) {
    *lost = *resolved = 0;
    for (u32 i = 0; i < 256; ++i) {
        *lost += s->packets[i].outcome == 3;
        *resolved += s->packets[i].outcome >= 2;
    }
}
void de__stats_channel(de__net_stats *s, u64 now, de_channel ch, u32 bytes, bool incoming,
                       bool resend) {
    if ((u32)ch > 2u)
        return;
    de__chan_counters *c = &bucket(s, now)->channel[ch];
    if (incoming) {
        c->bytes_in += bytes;
        c->msgs_in++;
    } else if (resend)
        c->resends++;
    else {
        c->bytes_out += bytes;
        c->msgs_out++;
    }
}
static u64 weight(const de__rate_bucket *b, u64 now) {
    const u64 epoch = now / DE__NET_RATE_NS;
    if (!b->valid || b->epoch > epoch || epoch - b->epoch > 10u)
        return 0;
    return epoch - b->epoch == 10u ? DE__NET_RATE_NS - now % DE__NET_RATE_NS : DE__NET_RATE_NS;
}
static u64 scaled(u64 value, u64 w) {
    return value / DE__NET_RATE_NS * w + value % DE__NET_RATE_NS * w / DE__NET_RATE_NS;
}
static u32 rate32(u64 value) {
    return value > UINT32_MAX ? UINT32_MAX : (u32)value;
}
de_peer_stats de__stats_peer(const de__net_stats *s, u64 now, u64 rtt, u64 jitter) {
    u64 in = 0, out = 0;
    for (u32 i = 0; i < 11; ++i) {
        const u64 w = weight(&s->buckets[i], now);
        in += scaled(s->buckets[i].bytes_in, w);
        out += scaled(s->buckets[i].bytes_out, w);
    }
    u32 lost, resolved;
    de__stats_loss(s, &lost, &resolved);
    return (de_peer_stats){(f32)((f64)rtt / 1000000.0), (f32)((f64)jitter / 1000000.0),
                           resolved ? (f32)lost / (f32)resolved : 0, rate32(in), rate32(out)};
}
de__channel_rates de__stats_rates(const de__net_stats *s, u64 now, de_channel ch) {
    if ((u32)ch > 2u)
        return (de__channel_rates){0};
    u64 in = 0, out = 0, mi = 0, mo = 0, resend = 0;
    for (u32 i = 0; i < 11; ++i) {
        const u64 w = weight(&s->buckets[i], now);
        const de__chan_counters *c = &s->buckets[i].channel[ch];
        in += scaled(c->bytes_in, w);
        out += scaled(c->bytes_out, w);
        mi += scaled(c->msgs_in, w);
        mo += scaled(c->msgs_out, w);
        resend += scaled(c->resends, w);
    }
    return (de__channel_rates){rate32(in), rate32(out), rate32(mi), rate32(mo), rate32(resend)};
}
