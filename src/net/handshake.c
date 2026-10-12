/* handshake.c: the connection packet codec (DENG-52): REQUEST and DENIED, the sealed packet
 * format, and the challenge cookie. The framing is documented at the top of connection.h.
 *
 * Every reader here takes untrusted bytes (DENG-58 fuzzes them): it checks each length before it
 * reads, and returns false instead of guessing. No function allocates or sets the error
 * context. */
#include "net/connection.h"

#include <string.h>

/* REQUEST offsets. */
#define REQ_PID     1u
#define REQ_FLAGS   9u
#define REQ_PK      10u
#define REQ_TOKEN   42u
#define REQ_PADDING (REQ_TOKEN + DE__NET_TOKEN_BYTES)

/* Sealed body sizes. */
#define CHALLENGE_BODY 40u /* epoch (8) + cookie (32) */
#define RESPONSE_BODY  1u  /* flags (1); the token is a plaintext field (DENG-378) */
/* RESPONSE plaintext fields: client_pk (32), epoch (8), cookie (32), token (96). */
#define RESPONSE_EXTRA (DE__NET_KEY_BYTES + 8u + DE__NET_KEY_BYTES + DE__NET_TOKEN_BYTES)
#define ACCEPT_BODY    5u /* slot (4) + flags (1) */

/* The largest CHALLENGE and DENIED must not be bigger than a REQUEST (no amplification). */
DE_STATIC_ASSERT(1u + DE__NET_VARINT_MAX + DE__NET_KEY_BYTES + CHALLENGE_BODY + DE__NET_TAG_BYTES <=
                     DE__NET_REQUEST_BYTES,
                 "CHALLENGE must not be bigger than REQUEST");
DE_STATIC_ASSERT(DE__NET_DENIED_BYTES <= DE__NET_REQUEST_BYTES, "DENIED must not be bigger");
DE_STATIC_ASSERT(REQ_PADDING <= DE__NET_REQUEST_BYTES, "REQUEST layout");
/* The largest associated data: RESPONSE (9 + 168), CHALLENGE (9 + 32 + 32). */
DE_STATIC_ASSERT(9u + RESPONSE_EXTRA <= DE__NET_AD_MAX && 9u + 64u <= DE__NET_AD_MAX, "AD size");

/* ---------- REQUEST ---------- */

u32 de__net_request_write(u8 *out, const de__net_request *r) {
    memset(out, 0, DE__NET_REQUEST_BYTES);
    out[0] = (u8)DE__NET_PACKET_REQUEST;
    de__net_put_u64(out + REQ_PID, r->protocol_id);
    out[REQ_FLAGS] = r->flags;
    memcpy(out + REQ_PK, r->client_pk, DE__NET_KEY_BYTES);
    memcpy(out + REQ_TOKEN, r->token, DE__NET_TOKEN_BYTES);
    return DE__NET_REQUEST_BYTES;
}

bool de__net_request_read(const u8 *in, u32 bytes, de__net_request *out) {
    if (bytes != DE__NET_REQUEST_BYTES || in[0] != (u8)DE__NET_PACKET_REQUEST)
        return false;
    if ((in[REQ_FLAGS] & ~DE__NET_FLAG_PLAINTEXT) != 0)
        return false;
    u8 pad = 0;
    for (u32 i = REQ_PADDING; i < DE__NET_REQUEST_BYTES; ++i)
        pad |= in[i];
    if (pad != 0)
        return false;
    out->protocol_id = de__net_get_u64(in + REQ_PID);
    out->flags = in[REQ_FLAGS];
    memcpy(out->client_pk, in + REQ_PK, DE__NET_KEY_BYTES);
    memcpy(out->token, in + REQ_TOKEN, DE__NET_TOKEN_BYTES);
    return true;
}

/* ---------- DENIED ---------- */

void de__net_request_digest(u8 out[DE__NET_KEY_BYTES], const u8 *request) {
    de__net_hash(out, DE__NET_KEY_BYTES, request, DE__NET_REQUEST_BYTES);
}

static void denied_proof(u8 out[DE__NET_TAG_BYTES], u64 protocol_id, u8 reason,
                         const u8 client_pk[DE__NET_KEY_BYTES],
                         const u8 context[DE__NET_KEY_BYTES]) {
    u8 msg[10 + DE__NET_KEY_BYTES];
    de__net_put_u64(msg, protocol_id);
    msg[8] = (u8)DE__NET_PACKET_DENIED;
    msg[9] = reason;
    memcpy(msg + 10, context, DE__NET_KEY_BYTES);
    de__net_mac(out, DE__NET_TAG_BYTES, client_pk, DE__NET_KEY_BYTES, msg, (u32)sizeof msg);
}

u32 de__net_denied_write(u8 *out, u64 protocol_id, de__net_deny_reason reason,
                         const u8 client_pk[DE__NET_KEY_BYTES],
                         const u8 context[DE__NET_KEY_BYTES]) {
    out[0] = (u8)DE__NET_PACKET_DENIED;
    out[1] = (u8)reason;
    denied_proof(out + 2, protocol_id, (u8)reason, client_pk, context);
    return DE__NET_DENIED_BYTES;
}

bool de__net_denied_read(const u8 *in, u32 bytes, u64 protocol_id,
                         const u8 client_pk[DE__NET_KEY_BYTES], const u8 context[DE__NET_KEY_BYTES],
                         de__net_deny_reason *reason) {
    if (bytes != DE__NET_DENIED_BYTES || in[0] != (u8)DE__NET_PACKET_DENIED)
        return false;
    if (in[1] == 0 || in[1] >= (u8)DE__NET_DENY_REASON_COUNT_)
        return false;
    u8 want[DE__NET_TAG_BYTES];
    denied_proof(want, protocol_id, in[1], client_pk, context);
    if (!de__net_equal16(want, in + 2))
        return false;
    *reason = (de__net_deny_reason)in[1];
    return true;
}

/* ---------- Sealed packets ---------- */

u32 de__net_packet_extra_bytes(de__net_packet_type type) {
    switch (type) {
    case DE__NET_PACKET_CHALLENGE:
        return DE__NET_KEY_BYTES; /* server_pk */
    case DE__NET_PACKET_RESPONSE:
        return RESPONSE_EXTRA; /* client_pk, epoch, cookie, token */
    default:
        return 0;
    }
}

static bool is_sealed_type(u32 type) {
    return type == DE__NET_PACKET_CHALLENGE || type == DE__NET_PACKET_RESPONSE ||
           type == DE__NET_PACKET_ACCEPT || type == DE__NET_PACKET_KEEPALIVE ||
           type == DE__NET_PACKET_PAYLOAD || type == DE__NET_PACKET_DISCONNECT;
}

/* Allowed body sizes per type. */
static bool body_size_ok(de__net_packet_type type, u32 n) {
    switch (type) {
    case DE__NET_PACKET_CHALLENGE:
        return n == CHALLENGE_BODY;
    case DE__NET_PACKET_RESPONSE:
        return n == RESPONSE_BODY;
    case DE__NET_PACKET_ACCEPT:
        return n == ACCEPT_BODY;
    case DE__NET_PACKET_KEEPALIVE:
        return n == 0;
    case DE__NET_PACKET_DISCONNECT:
        return n == 1;
    case DE__NET_PACKET_PAYLOAD:
        return n >= 1 && n <= DE__NET_PAYLOAD_MAX;
    default:
        return false;
    }
}

/* AD = pid | type | extra | bind. */
static u32 packet_ad(u8 *ad, u64 protocol_id, de__net_packet_type type, const u8 *extra,
                     const u8 *bind) {
    u8 tail[DE__NET_AD_MAX];
    u32 extra_bytes = de__net_packet_extra_bytes(type);
    u32 n = 0;
    if (extra_bytes > 0) {
        memcpy(tail, extra, extra_bytes);
        n = extra_bytes;
    }
    if (bind != NULL) {
        memcpy(tail + n, bind, DE__NET_KEY_BYTES);
        n += DE__NET_KEY_BYTES;
    }
    return de__net_ad(ad, protocol_id, (u8)type, tail, n);
}

u32 de__net_packet_seal(u8 *out, u32 cap, de__net_packet_type type, u64 protocol_id, u64 seq,
                        const u8 *extra, const u8 *bind, const u8 *body, u32 body_bytes,
                        const u8 key[DE__NET_KEY_BYTES], de__net_dir dir,
                        de__net_nonce_domain domain, bool plaintext_mode) {
    if (!is_sealed_type((u32)type) || !body_size_ok(type, body_bytes))
        return 0;
    u8 varint[DE__NET_VARINT_MAX];
    u32 vlen = de__net_varint_write(varint, seq);
    u32 extra_bytes = de__net_packet_extra_bytes(type);
    u32 total = 1u + vlen + extra_bytes + body_bytes + DE__NET_TAG_BYTES;
    if (total > cap)
        return 0;
    u8 *p = out;
    *p++ = (u8)type;
    memcpy(p, varint, vlen);
    p += vlen;
    if (extra_bytes > 0) {
        memcpy(p, extra, extra_bytes);
        p += extra_bytes;
    }
    u8 nonce[DE__NET_NONCE_BYTES];
    u8 ad[DE__NET_AD_MAX];
    de__net_nonce(nonce, seq, dir, domain);
    u32 ad_bytes = packet_ad(ad, protocol_id, type, extra, bind);
    de__net_seal(p, p + body_bytes, key, nonce, ad, ad_bytes, body, body_bytes, plaintext_mode);
    return total;
}

bool de__net_packet_parse(u8 *in, u32 bytes, de__net_packet *out) {
    if (bytes < 1u + 1u + DE__NET_TAG_BYTES || bytes > DE__NET_PACKET_MAX)
        return false;
    u32 type = in[0];
    if (!is_sealed_type(type)) /* also refuses reserved high bits */
        return false;
    u64 seq = 0;
    u32 vlen = de__net_varint_read(in + 1, bytes - 1u, &seq);
    if (vlen == 0)
        return false;
    u32 extra_bytes = de__net_packet_extra_bytes((de__net_packet_type)type);
    u32 head = 1u + vlen + extra_bytes;
    if (bytes < head + DE__NET_TAG_BYTES)
        return false;
    u32 body_bytes = bytes - head - DE__NET_TAG_BYTES;
    if (!body_size_ok((de__net_packet_type)type, body_bytes))
        return false;
    out->type = (de__net_packet_type)type;
    out->seq = seq;
    out->extra = in + 1u + vlen;
    out->body = in + head;
    out->body_bytes = body_bytes;
    out->tag = in + head + body_bytes;
    return true;
}

bool de__net_packet_open(de__net_packet *p, u64 protocol_id, const u8 *bind,
                         const u8 key[DE__NET_KEY_BYTES], de__net_dir dir,
                         de__net_nonce_domain domain, bool plaintext_mode) {
    u8 nonce[DE__NET_NONCE_BYTES];
    u8 ad[DE__NET_AD_MAX];
    de__net_nonce(nonce, p->seq, dir, domain);
    u32 ad_bytes = packet_ad(ad, protocol_id, p->type, p->extra, bind);
    return de__net_open(p->body, p->tag, key, nonce, ad, ad_bytes, p->body, p->body_bytes,
                        plaintext_mode);
}

/* ---------- Cookie ---------- */

void de__net_cookie(u8 out[DE__NET_KEY_BYTES], const u8 secret[DE__NET_KEY_BYTES],
                    const de_net_address *from, u64 epoch, const u8 client_pk[DE__NET_KEY_BYTES]) {
    /* A fixed encoding of the address (never the struct bytes: padding, platform layout). */
    u8 msg[2 + 2 + 16 + 4 + 8 + DE__NET_KEY_BYTES];
    u8 *p = msg;
    p[0] = (u8)from->family;
    p[1] = (u8)(from->family >> 8);
    p[2] = (u8)from->port;
    p[3] = (u8)(from->port >> 8);
    p += 4;
    memcpy(p, from->addr, 16);
    p += 16;
    de__net_put_u32(p, from->scope_id);
    p += 4;
    de__net_put_u64(p, epoch);
    p += 8;
    memcpy(p, client_pk, DE__NET_KEY_BYTES);
    de__net_mac(out, DE__NET_KEY_BYTES, secret, DE__NET_KEY_BYTES, msg, (u32)sizeof msg);
}
