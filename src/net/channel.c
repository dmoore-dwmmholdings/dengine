#include "net/channel.h"
#include "net/congestion.h"
#include "net/net_stats.h"
#include <dengine/de_debug.h>
#include <dengine/de_platform.h>
#include <string.h>

#define TICK_PACKETS 64u
typedef struct message {
    u64 last_sent;
    u32 bytes, serial;
    u16 id;
    bool used, sent;
    u8 data[DE__CHAN_MESSAGE_MAX];
} message;
typedef struct reference {
    // Full generation: a late ack of an older packet cannot clear a new message
    // reusing its 16-bit id. A 1024-packet ring cannot retain 2^32 message generations.
    u32 serial;
    u8 channel;
} reference;
typedef struct sent_packet {
    u64 time;
    u32 bytes, count, stats_serial;
    u16 sequence;
    bool valid, loss_reported;
    reference messages[DE__CHAN_PACKET_MESSAGES];
} sent_packet;
struct de__conn {
    de__chan_desc desc;
    de__chan_stats stats;
    message outgoing[3][DE__CHAN_WINDOW], incoming[3][DE__CHAN_WINDOW];
    sent_packet packets[DE__CHAN_ACK_WINDOW];
    de__chan_sequence_window received, unordered;
    de__chan_rtt rtt;
    u32 next[3];
    u16 packet_next, ordered_next;
    u32 head[3], count[3];
    bool ack_pending;
    de__cc_state cc;
    de__net_stats rates;
    de_cvar bandwidth_cvar;
};
_Static_assert(sizeof(de__conn) <= 5u * 1024u * 1024u, "channel storage must remain bounded");
static u64 clock_now(const de__conn *c) {
    return c->desc.now_ns ? c->desc.now_ns(c->desc.clock_user) : de_time_now_ns();
}
static u32 bandwidth(const de__conn *c) {
    if (c->desc.max_bandwidth_kbps)
        return c->desc.max_bandwidth_kbps;
    return DE_HANDLE_VALID(c->bandwidth_cvar) ? (u32)de_cvar_int(c->bandwidth_cvar)
                                              : DE__CC_DEFAULT_KBPS;
}
de_result de__chan_create(const de__chan_desc *desc, de__conn **out) {
    if (out)
        *out = NULL;
    if (!desc || !out || !desc->send_packet)
        return DE_ERR_INVALID_ARGUMENT;
    const de_allocator *a = desc->allocator ? desc->allocator : de_heap_allocator();
    de__conn *c = DE_ALLOC(a, sizeof *c, 16, DE_MEM_NET);
    if (!c)
        return DE_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof *c);
    c->desc = *desc;
    c->desc.allocator = a;
    c->stats.allocated_bytes = sizeof *c;
    if (!desc->max_bandwidth_kbps) {
        const de_cvar_desc cv = {.name = "net.max_bandwidth_kbps",
                                 .help = "Per-connection channel send cap in decimal kilobits/s",
                                 .type = DE_CVAR_INT,
                                 .default_number = DE__CC_DEFAULT_KBPS,
                                 .min = DE__CC_FLOOR_KBPS,
                                 .max = 1000000};
        c->bandwidth_cvar = de_cvar_register(&cv);
    }
    de__cc_init(&c->cc, bandwidth(c), desc->tick_rate_hz, de__mtu_payload(c));
    *out = c;
    return DE_OK;
}
void de__chan_reset(de__conn *c) {
    if (!c)
        return;
    const de__chan_desc desc = c->desc;
    const de_cvar cv = c->bandwidth_cvar;
    memset(c, 0, sizeof *c);
    c->desc = desc;
    c->bandwidth_cvar = cv;
    c->stats.allocated_bytes = sizeof *c;
    de__cc_init(&c->cc, bandwidth(c), desc.tick_rate_hz, de__mtu_payload(c));
}
void de__chan_destroy(de__conn *c) {
    if (c) {
        const de_allocator *a = c->desc.allocator;
        DE_FREE(a, c, sizeof *c);
    }
}
de_result de__chan_send(de__conn *c, de_channel ch, const void *data, u32 bytes) {
    if (!c || (u32)ch > DE_CHANNEL_RELIABLE_UNORDERED || !data || !bytes ||
        bytes > DE__CHAN_MESSAGE_MAX || de__mtu_payload(c) < DE__CHAN_HEADER + 5u + bytes)
        return DE_ERR_INVALID_ARGUMENT;
    message *m = &c->outgoing[ch][c->next[ch] % DE__CHAN_WINDOW];
    if (m->used) {
        if (ch != DE_CHANNEL_UNRELIABLE)
            return DE_ERR_BUSY;
        c->stats.channel[ch].dropped++;
        return DE_OK;
    }
    m->serial = c->next[ch]++;
    m->id = (u16)m->serial;
    m->bytes = bytes;
    m->used = true;
    m->sent = false;
    memcpy(m->data, data, bytes);
    c->stats.pending[ch]++;
    return DE_OK;
}
static void acknowledge(de__conn *c, const de__chan_packet *p) {
    if (!p->has_ack)
        return;
    const u64 now = clock_now(c);
    bool sampled = false;
    for (u32 d = 0; d <= 32; ++d) {
        if (d && !(p->ack_bits & (1u << (d - 1))))
            continue;
        const u16 seq = (u16)(p->ack - d);
        sent_packet *s = &c->packets[seq % DE__CHAN_ACK_WINDOW];
        if (!s->valid || s->sequence != seq)
            continue;
        for (u32 i = 0; i < s->count; ++i) {
            const reference r = s->messages[i];
            message *m = &c->outgoing[r.channel][r.serial % DE__CHAN_WINDOW];
            if (m->used && m->serial == r.serial) {
                m->used = false;
                c->stats.pending[r.channel]--;
            }
        }
        const u64 elapsed = now >= s->time ? now - s->time : 0;
        // One unambiguous packet-time sample per ack header, newest first. Retransmitted
        // messages use a fresh packet sequence, so the specific send time is still known.
        const bool sample = !sampled && now >= s->time;
        if (sample) {
            de__chan_rtt_sample(&c->rtt, elapsed);
            sampled = true;
        }
        de__cc_on_ack(c, s->bytes, sample ? elapsed : 0);
        de__stats_outcome(&c->rates, s->stats_serial, false);
        c->stats.acked++;
        s->valid = false;
    }
}
de_result de__conn_read_payload(de__conn *c, const void *data, u32 bytes) {
    if (!c)
        return DE_ERR_INVALID_ARGUMENT;
    de__chan_packet p;
    if (!de__chan_packet_read(data, bytes, &p)) {
        c->stats.malformed++;
        return DE_ERR_PARSE;
    }
    c->stats.packets_in++;
    de__stats_packet_in(&c->rates, clock_now(c), bytes);
    acknowledge(c, &p);
    if (de__chan_sequence_seen(&c->received, p.sequence)) {
        c->stats.duplicate++;
        if (p.count)
            c->ack_pending = true;
        return DE_OK;
    }
    // Preflight all reliable storage before acknowledging this packet. Backpressure must
    // never acknowledge a message that has nowhere to live. No partially accepted packet.
    bool keep[DE__CHAN_PACKET_MESSAGES] = {false};
    bool ordered[DE__CHAN_WINDOW] = {false};
    de__chan_sequence_window seen = c->unordered;
    u32 unordered_count = c->count[DE_CHANNEL_RELIABLE_UNORDERED];
    u32 unreliable_count = c->count[DE_CHANNEL_UNRELIABLE], dropped = 0;
    for (u32 i = 0; i < p.count; ++i) {
        const de__chan_message_view *m = &p.messages[i];
        if (m->channel == DE_CHANNEL_RELIABLE_ORDERED) {
            const u16 d = (u16)(m->id - c->ordered_next);
            if (d >= 0x8000u)
                continue; // delivered already
            if (d >= DE__CHAN_WINDOW) {
                c->stats.backpressure++;
                return DE_ERR_BUSY;
            }
            const u32 index = m->id % DE__CHAN_WINDOW;
            keep[i] = !c->incoming[m->channel][index].used && !ordered[index];
            ordered[index] = true;
        } else if (m->channel == DE_CHANNEL_RELIABLE_UNORDERED) {
            if (de__chan_sequence_seen(&seen, m->id))
                continue;
            if (unordered_count == DE__CHAN_WINDOW) {
                c->stats.backpressure++;
                return DE_ERR_BUSY;
            }
            de__chan_sequence_mark(&seen, m->id);
            unordered_count++;
            keep[i] = true;
        } else if (unreliable_count < DE__CHAN_WINDOW) {
            unreliable_count++;
            keep[i] = true;
        } else {
            dropped++;
        }
    }
    for (u32 i = 0; i < p.count; ++i) {
        if (!keep[i])
            continue;
        const de__chan_message_view *v = &p.messages[i];
        const u32 ch = (u32)v->channel;
        const u32 index = ch == DE_CHANNEL_RELIABLE_ORDERED
                              ? v->id % DE__CHAN_WINDOW
                              : (c->head[ch] + c->count[ch]) % DE__CHAN_WINDOW;
        message *m = &c->incoming[ch][index];
        m->used = true;
        m->id = v->id;
        m->bytes = v->bytes;
        memcpy(m->data, v->data, v->bytes);
        c->count[ch]++;
    }
    c->unordered = seen;
    c->stats.channel[DE_CHANNEL_UNRELIABLE].dropped += dropped;
    de__chan_sequence_mark(&c->received, p.sequence);
    if (p.count)
        c->ack_pending = true;
    return DE_OK;
}
void de__chan_receive(de__conn *c, de__chan_message_fn fn, void *user) {
    if (!c || !fn)
        return;
    for (u32 ch = 0; ch < 3; ++ch) {
        while (c->count[ch]) {
            const u32 index =
                ch == DE_CHANNEL_RELIABLE_ORDERED ? c->ordered_next % DE__CHAN_WINDOW : c->head[ch];
            message *m = &c->incoming[ch][index];
            if (!m->used || (ch == DE_CHANNEL_RELIABLE_ORDERED && m->id != c->ordered_next))
                break;
            fn(user, (de_channel)ch, m->data, m->bytes);
            c->stats.channel[ch].bytes_in += m->bytes;
            c->stats.channel[ch].msgs_in++;
            de__stats_channel(&c->rates, clock_now(c), (de_channel)ch, m->bytes, true, false);
            m->used = false;
            c->count[ch]--;
            if (ch == DE_CHANNEL_RELIABLE_ORDERED)
                c->ordered_next++;
            else
                c->head[ch] = (c->head[ch] + 1u) % DE__CHAN_WINDOW;
        }
    }
}
static void discard_unreliable(de__conn *c) {
    for (u32 i = 0; i < DE__CHAN_WINDOW; ++i) {
        if (c->outgoing[0][i].used) {
            c->outgoing[0][i].used = false;
            c->stats.pending[0]--;
            c->stats.channel[0].dropped++;
        }
    }
}
static void packet_lost(de__conn *c, sent_packet *p) {
    if (!p->valid || p->loss_reported)
        return;
    p->loss_reported = true;
    c->stats.lost++;
    de__stats_outcome(&c->rates, p->stats_serial, true);
    de__cc_on_loss(c, p->bytes);
}
de_result de__conn_write_packets(de__conn *c, u64 now) {
    if (!c)
        return DE_ERR_INVALID_ARGUMENT;
    u64 timeout = c->rtt.smoothed_ns > UINT64_MAX / 4u ? UINT64_MAX : c->rtt.smoothed_ns * 4u;
    if (timeout < 250000000ull)
        timeout = 250000000ull;
    for (u32 i = 0; i < DE__CHAN_ACK_WINDOW; ++i) {
        sent_packet *p = &c->packets[i];
        if (p->valid && now >= p->time && now - p->time >= timeout)
            packet_lost(c, p);
    }
    u32 lost, resolved;
    de__stats_loss(&c->rates, &lost, &resolved);
    de__cc_tick(&c->cc, now, bandwidth(c), c->rtt.smoothed_ns, lost, resolved);
    DE_PROFILE_PLOT("net.bytes_out", de__conn_stats(c).bytes_out_per_sec);
    DE_PROFILE_PLOT("net.rtt_ms", (f64)c->rtt.smoothed_ns / 1000000.0);
    u32 budget = de__cc_send_budget(c);
    const u32 mtu = de__mtu_payload(c);
    const u32 limit = mtu < DE__CHAN_PACKET_MAX ? mtu : DE__CHAN_PACKET_MAX;
    const u64 resend = de__chan_resend_ns(&c->rtt);
    de_result result = DE_OK;
    for (u32 packets = 0; packets < TICK_PACKETS && budget >= DE__CHAN_HEADER; ++packets) {
        const u32 capacity = budget < limit ? budget : limit;
        if (capacity < DE__CHAN_HEADER)
            break;
        u8 data[DE__CHAN_PACKET_MAX];
        message *selected[DE__CHAN_PACKET_MESSAGES];
        reference refs[DE__CHAN_PACKET_MESSAGES];
        u32 bytes = DE__CHAN_HEADER, count = 0;
        bool resend_pending = false;
        // Globally stable priority: reliable resends, then new reliable, then unreliable.
        for (u32 phase = 0; phase < 3; ++phase) {
            // Do not let a continuous stream of tiny new messages spend every
            // partial refill while a full-size reliable resend waits forever.
            if (phase && resend_pending && capacity < limit)
                break;
            const u32 first = phase == 2 ? 0 : 1, end = phase == 2 ? 1 : 3;
            for (u32 ch = first; ch < end; ++ch) {
                for (u32 offset = 0; offset < DE__CHAN_WINDOW; ++offset) {
                    const u16 id = (u16)(c->next[ch] - DE__CHAN_WINDOW + offset);
                    message *m = &c->outgoing[ch][id % DE__CHAN_WINDOW];
                    if (!m->used || m->id != id || count == DE__CHAN_PACKET_MESSAGES)
                        continue;
                    if (phase == 0 &&
                        (!m->sent || now < m->last_sent || now - m->last_sent < resend))
                        continue;
                    if (phase == 1 && m->sent)
                        continue;
                    if (phase == 0)
                        resend_pending = true;
                    const u32 n = de__chan_packet_message(data + bytes, capacity - bytes,
                                                          (de_channel)ch, id, m->data, m->bytes);
                    if (!n)
                        continue;
                    selected[count] = m;
                    refs[count++] = (reference){m->serial, (u8)ch};
                    bytes += n;
                }
            }
        }
        if (!count && (!c->ack_pending || (resend_pending && capacity < limit)))
            break;
        de__chan_packet_header(data, c->packet_next, c->received.initialized, c->received.newest,
                               de__chan_ack_bits(&c->received));
        result = c->desc.send_packet(c->desc.send_user, data, bytes);
        if (result != DE_OK)
            break;
        sent_packet *packet = &c->packets[c->packet_next % DE__CHAN_ACK_WINDOW];
        packet_lost(c, packet);
        packet->valid = count != 0; /* ACK-only packets do not solicit an ACK. */
        packet->loss_reported = false;
        packet->sequence = c->packet_next++;
        packet->time = now;
        packet->bytes = bytes;
        packet->count = 0;
        packet->stats_serial = de__stats_packet_out(&c->rates, now, bytes, count != 0);
        de__cc_sent(&c->cc, bytes);
        for (u32 i = 0; i < count; ++i) {
            message *m = selected[i];
            de__chan_counters *st = &c->stats.channel[refs[i].channel];
            if (m->sent)
                st->resends++;
            else {
                st->msgs_out++;
                st->bytes_out += m->bytes;
            }
            de__stats_channel(&c->rates, now, (de_channel)refs[i].channel, m->bytes, false,
                              m->sent);
            m->sent = true;
            m->last_sent = now;
            if (refs[i].channel == DE_CHANNEL_UNRELIABLE) {
                m->used = false;
                c->stats.pending[0]--;
            } else {
                packet->messages[packet->count++] = refs[i];
            }
        }
        c->stats.packets_out++;
        c->ack_pending = false;
        budget = de__cc_send_budget(c);
    }
    // Unreliable traffic gets this tick only; it cannot accumulate behind reliable work.
    discard_unreliable(c);
    return result;
}
de__chan_stats de__chan_get_stats(const de__conn *c) {
    if (!c)
        return (de__chan_stats){0};
    de__chan_stats stats = c->stats;
    stats.rtt_ns = c->rtt.smoothed_ns;
    stats.jitter_ns = c->rtt.variance_ns;
    return stats;
}

de__cc_state *de__conn_cc(de__conn *c) {
    return c ? &c->cc : NULL;
}
const de__cc_state *de__conn_cc_view(const de__conn *c) {
    return c ? &c->cc : NULL;
}
de_peer_stats de__conn_stats(const de__conn *c) {
    return c ? de__stats_peer(&c->rates, clock_now(c), c->rtt.smoothed_ns, c->rtt.variance_ns)
             : (de_peer_stats){0};
}
bool de__conn_channel_stats(const de__conn *c, de_channel ch, de__channel_rates *out) {
    if (out)
        *out = (de__channel_rates){0};
    if (!c || !out || (u32)ch > 2u)
        return false;
    *out = de__stats_rates(&c->rates, clock_now(c), ch);
    return true;
}
