#include "net/mtu.h"
#include "net/channel.h"
#include <string.h>

#define FIRST_PROBE_NS 100000000ull
static u64 after(u64 now, u64 duration) {
    return UINT64_MAX - now < duration ? UINT64_MAX : now + duration;
}
void de__mtu_init(de__mtu_state *s) {
    memset(s, 0, sizeof *s);
}
u32 de__mtu_capacity(const de__mtu_state *s, u32 overhead) {
    if (overhead > DE__MTU_MAX_WIRE - DE__MTU_BASE_PAYLOAD)
        return DE__MTU_BASE_PAYLOAD;
    return s && s->confirmed_wire > overhead + DE__MTU_BASE_PAYLOAD ? s->confirmed_wire - overhead
                                                                    : DE__MTU_BASE_PAYLOAD;
}
static void finish(de__mtu_state *s, u64 now) {
    s->searching = false;
    s->pending = false;
    s->candidate = 0;
    s->next_probe = after(now, DE__MTU_REPROBE_NS);
}
static void choose(de__mtu_state *s, u64 now) {
    s->attempts = 0;
    s->pending = false;
    s->next_probe = now;
    if (s->stage < 3) {
        static const u32 sizes[] = {1280, 1400, 1452};
        while (s->stage < 3 && sizes[s->stage] <= s->lower)
            s->stage++;
        if (s->stage < 3) {
            s->candidate = sizes[s->stage];
            return;
        }
    }
    if (s->upper <= s->lower + 1u) {
        finish(s, now);
        return;
    }
    s->candidate = s->lower + (s->upper - s->lower) / 2u;
}
static void begin(de__mtu_state *s, u64 now, u32 overhead) {
    s->lower = s->confirmed_wire ? s->confirmed_wire : DE__MTU_BASE_PAYLOAD + overhead;
    s->upper = DE__MTU_MAX_WIRE + 1u;
    s->stage = 0;
    s->searching = true;
    if (s->confirmed_wire) {
        s->stage = 3;
        s->candidate = s->confirmed_wire;
        s->attempts = 0;
        s->pending = false;
        s->next_probe = now;
        return;
    }
    choose(s, now);
}
u32 de__mtu_probe_due(de__mtu_state *s, u64 now, u32 overhead, u64 srtt) {
    if (!s || overhead > DE__MTU_MAX_WIRE - DE__MTU_BASE_PAYLOAD)
        return 0;
    if (!s->initialized || now < s->now) {
        de__mtu_init(s);
        s->initialized = true;
        begin(s, now, overhead);
        s->next_probe = after(now, FIRST_PROBE_NS);
    }
    s->now = now;
    s->timeout = srtt > UINT64_MAX / 2u ? UINT64_MAX : srtt * 2u;
    if (s->timeout < FIRST_PROBE_NS)
        s->timeout = FIRST_PROBE_NS;
    if (!s->searching && now >= s->next_probe)
        begin(s, now, overhead);
    if (s->pending && now >= s->sent_at && now - s->sent_at >= s->timeout) {
        s->pending = false;
        if (s->attempts == 3) {
            if (s->candidate == s->confirmed_wire) {
                s->confirmed_wire = 0;
                begin(s, now, overhead);
                s->next_probe = after(now, 1000000000ull);
            } else {
                s->upper = s->candidate;
                s->stage = 3;
                choose(s, now);
            }
        }
    }
    return s->searching && !s->pending && now >= s->next_probe ? s->candidate : 0;
}
void de__mtu_probe_sent(de__mtu_state *s, u16 sequence, u64 now) {
    if (!s || !s->searching || s->pending || s->attempts >= 3)
        return;
    s->probe_sequences[s->attempts++] = sequence;
    s->sent_at = now;
    s->pending = true;
}
void de__mtu_probe_ack(de__mtu_state *s, u16 sequence, u32 bytes, u64 now) {
    if (!s || !s->searching || !s->attempts || bytes != s->candidate)
        return;
    bool found = false;
    for (u32 i = 0; i < s->attempts; ++i)
        if (s->probe_sequences[i] == sequence)
            found = true;
    if (!found)
        return;
    const bool confirmation = s->confirmed_wire == bytes;
    s->confirmed_wire = bytes;
    s->lower = bytes;
    if (confirmation)
        s->stage = 0;
    else
        s->stage++;
    choose(s, now);
}
void de__mtu_full_ack(de__mtu_state *s, u64 sequence) {
    if (s && (!s->have_full_loss || sequence > s->last_full_loss)) {
        s->full_losses = 0;
        s->have_full_loss = true;
        s->last_full_loss = sequence;
    }
}
void de__mtu_full_loss(de__mtu_state *s, u64 sequence, u64 now, u32 overhead) {
    if (!s || overhead > DE__MTU_MAX_WIRE - DE__MTU_BASE_PAYLOAD ||
        (s->have_full_loss && sequence <= s->last_full_loss))
        return;
    s->last_full_loss = sequence;
    s->have_full_loss = true;
    if (++s->full_losses < 3)
        return;
    s->confirmed_wire = 0;
    begin(s, now, overhead);
    s->next_probe = after(now, 1000000000ull);
    s->full_losses = 0;
}

u32 de__mtu_payload(const de__conn *conn) {
    return de__mtu_capacity(de__conn_mtu_view(conn), de__conn_wire_overhead(conn));
}
