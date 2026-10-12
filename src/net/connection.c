/* connection.c: the client and server connection state machines (DENG-52; connection keys and
 * the accepted-RESPONSE ring DENG-378). The packet framing, the handshake and the lifecycle are
 * described at the top of connection.h.
 *
 * Rules kept here:
 *   - Everything is allocated at create; update, send, kick and disconnect never allocate.
 *   - A packet is acted on only after it authenticated, and the replay window moves only then.
 *     A dropped packet bumps one counter and changes nothing else.
 *   - Every secret (keys, ephemeral X25519 secrets, epoch secrets, the server key, tokens) is
 *     wiped with crypto_wipe when it is no longer needed, and at destroy.
 *   - Slots are found by walking the pool in index order (max_peers is small: DENG-2 Q15 sets
 *     64 players), so behaviour never depends on hashing.
 */
#include "net/connection.h"

#include <dengine/de_debug.h>
#include <dengine/de_platform.h>

#include <string.h>
#include <time.h>

/* Plaintext sessions are a Dev/Debug feature (SPEC §16.1, DENG-52): other builds always encrypt. */
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#define NET_ALLOW_PLAINTEXT 1
#else
#define NET_ALLOW_PLAINTEXT 0
#endif

#define NS_PER_MS 1000000ull
#define NS_PER_S  1000000000ull

#define SERVER_DEFAULT_BATCH 32u
#define CLIENT_DEFAULT_BATCH 8u
#define MAX_PEERS_LIMIT      4096u
/** Datagrams one update handles at most; the rest wait for the next update. */
#define UPDATE_MAX_DATAGRAMS 4096u
/** Accepted-RESPONSE ring: this many entries per peer slot, and at least ACCEPTED_MIN. */
#define ACCEPTED_PER_PEER 4u
#define ACCEPTED_MIN      64u

/* ---------- Shared helpers ---------- */

static u64 clock_now(const de__net_clock *c) {
    return c->now_ns != NULL ? c->now_ns(c->user) : de_time_now_ns();
}

static u64 clock_unix(const de__net_clock *c) {
    if (c->unix_s != NULL)
        return c->unix_s(c->user);
    time_t t = time(NULL);
    return t > 0 ? (u64)t : 0;
}

static i64 cvar_int(const char *name, const char *help, i64 def, i64 min, i64 max) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.help = help;
    d.type = DE_CVAR_INT;
    d.default_number = (f64)def;
    d.min = (f64)min;
    d.max = (f64)max;
    de_cvar cv = de_cvar_register(&d);
    return DE_HANDLE_VALID(cv) ? de_cvar_int(cv) : def;
}

static f64 cvar_float(const char *name, const char *help, f64 def, f64 min, f64 max) {
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.help = help;
    d.type = DE_CVAR_FLOAT;
    d.default_number = def;
    d.min = min;
    d.max = max;
    de_cvar cv = de_cvar_register(&d);
    return DE_HANDLE_VALID(cv) ? de_cvar_float(cv) : def;
}

/* Timing from the desc, or the cvars for fields left 0. Registers the cvars (create time only). */
static de__net_timing make_timing(u32 resend_ms, u32 connect_timeout_ms, u32 idle_timeout_ms,
                                  u32 keepalive_ms) {
    de__net_timing t;
    t.resend_ns = resend_ms != 0
                      ? (u64)resend_ms * NS_PER_MS
                      : (u64)cvar_int("net.handshake_resend_ms",
                                      "Resend interval of handshake packets (REQUEST, RESPONSE)",
                                      100, 10, 5000) *
                            NS_PER_MS;
    t.connect_timeout_ns =
        connect_timeout_ms != 0
            ? (u64)connect_timeout_ms * NS_PER_MS
            : (u64)(cvar_float("net.connect_timeout_s",
                               "Seconds a client tries to connect before it gives up", 5.0, 0.1,
                               600.0) *
                    1000.0) *
                  NS_PER_MS;
    t.idle_timeout_ns =
        idle_timeout_ms != 0
            ? (u64)idle_timeout_ms * NS_PER_MS
            : (u64)(cvar_float("net.timeout_s",
                               "Seconds without a valid packet before a connection times out", 10.0,
                               0.1, 3600.0) *
                    1000.0) *
                  NS_PER_MS;
    t.keepalive_ns = keepalive_ms != 0
                         ? (u64)keepalive_ms * NS_PER_MS
                         : (u64)cvar_int("net.keepalive_ms",
                                         "A side that sent nothing for this long sends a KEEPALIVE",
                                         1000, 10, 60000) *
                               NS_PER_MS;
    return t;
}

static size_t align_up(size_t v, size_t a) {
    return (v + a - 1u) & ~(a - 1u);
}

u64 de__net_conn_stats_dropped(const de__net_conn_stats *s) {
    return s->dropped_malformed + s->dropped_auth + s->dropped_replay + s->dropped_cookie +
           s->dropped_unexpected;
}

/* PAYLOAD dispatch: the one place a received PAYLOAD body leaves the connection layer.
 * DENG-53 routing hook: route `data` to the channel layer here (one line). */
static void dispatch_payload(const de__net_callbacks *cb, u32 slot, const u8 *data, u32 bytes) {
    if (cb->payload != NULL)
        cb->payload(cb->user, slot, data, bytes);
}

/* Sends `bytes` of `data`; counts it. Socket errors and full send buffers lose the packet, as
 * the network may; the timers and resends cover it. */
static void send_raw(de__socket *sock, de__net_conn_stats *st, const de_net_address *to,
                     const u8 *data, u32 bytes) {
    u32 sent = 0;
    if (de__socket_send_to(sock, to, data, bytes, &sent) == DE_OK && sent == bytes)
        st->packets_sent++;
}

/* Packets whose body follows the session's plaintext mode. Handshake packets are always AEAD. */
static bool is_session_type(de__net_packet_type t) {
    return t == DE__NET_PACKET_KEEPALIVE || t == DE__NET_PACKET_PAYLOAD ||
           t == DE__NET_PACKET_DISCONNECT;
}

/* ==================================================================================================
 * Server
 * ==================================================================================================
 */

typedef struct srv_slot {
    de__net_slot_state state;
    u32 generation;
    de_net_address address;
    u64 client_id;
    u64 user;
    bool plaintext;
    de__net_keys keys;
    u64 send_seq;
    de__net_replay replay;
    u64 last_recv_ns;
    u64 last_send_ns;
    u32 disconnect_left;
    u8 disconnect_code;
} srv_slot;

/* One challenge epoch: the cookie secret and the X25519 key pair the server answers REQUESTs
 * with. Two are kept: the current one and the one before. */
typedef struct srv_epoch {
    bool valid;
    u64 id;
    u8 cookie_secret[DE__NET_KEY_BYTES];
    u8 secret[DE__NET_KEY_BYTES];
    u8 public_key[DE__NET_KEY_BYTES];
} srv_epoch;

/* A RESPONSE the server accepted: its epoch and the client's ephemeral public key. A client makes
 * a new key pair for every handshake, so the pair names one RESPONSE; a RESPONSE that matches an
 * entry is a replay. Entries need no expiry: once their epoch rotates out, the cookie check
 * refuses the RESPONSE anyway. */
typedef struct srv_accepted {
    u64 epoch;
    u8 client_pk[DE__NET_KEY_BYTES];
} srv_accepted;

struct de__net_server {
    const de_allocator *alloc;
    size_t alloc_size;
    de__socket *socket;
    u64 protocol_id;
    u32 max_peers;
    u8 server_key[DE__NET_KEY_BYTES];
    bool deny_direct;
    bool allow_plaintext;
    de__net_timing timing;
    de__net_clock clock;
    de__net_callbacks cb;
    srv_epoch epochs[2];
    u32 current_epoch; /* index into epochs */
    u64 challenge_seq; /* nonce sequence of CHALLENGEs, server-wide */
    u64 now_ns;
    srv_slot *slots;
    srv_accepted *accepted; /* ring of accepted_cap entries; accepted_next is the oldest */
    u32 accepted_cap;
    u32 accepted_next;
    u32 recv_batch;
    de__datagram *msgs;
    u8 *recv_data;
    de__net_conn_stats stats;
    u8 send_buf[DE__NET_PACKET_MAX];
};

static void epoch_wipe(srv_epoch *e) {
    de__net_wipe(e, sizeof *e);
}

/* Makes sure the current epoch is the one for `now`. A new epoch gets new random secrets; the
 * previous one is kept only when it is the epoch just before. */
static void srv_rotate(de__net_server *s) {
    u64 id = s->now_ns / DE__NET_EPOCH_NS;
    srv_epoch *cur = &s->epochs[s->current_epoch];
    if (cur->valid && cur->id == id)
        return;
    u32 next = s->current_epoch ^ 1u;
    srv_epoch *e = &s->epochs[next];
    epoch_wipe(e);
    if (!cur->valid || cur->id + 1u != id)
        epoch_wipe(cur);
    if (de__net_random(e->cookie_secret, sizeof e->cookie_secret) == DE_OK &&
        de__net_x25519_keypair(e->secret, e->public_key) == DE_OK) {
        e->id = id;
        e->valid = true;
    } else {
        epoch_wipe(e); /* no randomness: no challenges until it works again */
    }
    s->current_epoch = next;
}

static const srv_epoch *srv_find_epoch(const de__net_server *s, u64 id) {
    for (u32 i = 0; i < 2; ++i)
        if (s->epochs[i].valid && s->epochs[i].id == id)
            return &s->epochs[i];
    return NULL;
}

static srv_slot *srv_find_address(de__net_server *s, const de_net_address *a) {
    for (u32 i = 0; i < s->max_peers; ++i)
        if (s->slots[i].state != DE__NET_SLOT_FREE && de_net_address_equal(&s->slots[i].address, a))
            return &s->slots[i];
    return NULL;
}

static bool srv_client_id_taken(const de__net_server *s, u64 client_id) {
    if (client_id == 0) /* direct connects have no id */
        return false;
    for (u32 i = 0; i < s->max_peers; ++i)
        if (s->slots[i].state == DE__NET_SLOT_CONNECTED && s->slots[i].client_id == client_id)
            return true;
    return false;
}

static srv_slot *srv_free_slot(de__net_server *s) {
    for (u32 i = 0; i < s->max_peers; ++i)
        if (s->slots[i].state == DE__NET_SLOT_FREE)
            return &s->slots[i];
    return NULL;
}

static u32 srv_index(const de__net_server *s, const srv_slot *sl) {
    return (u32)(sl - s->slots);
}

static void srv_slot_release(srv_slot *sl) {
    u32 gen = sl->generation;
    de__net_wipe(sl, sizeof *sl);
    sl->generation = gen;
    sl->state = DE__NET_SLOT_FREE;
}

static void srv_send_sealed(de__net_server *s, srv_slot *sl, de__net_packet_type type,
                            const u8 *body, u32 bytes) {
    u32 n = de__net_packet_seal(s->send_buf, DE__NET_PACKET_MAX, type, s->protocol_id, sl->send_seq,
                                NULL, NULL, body, bytes, sl->keys.s2c, DE__NET_DIR_S2C,
                                DE__NET_DOMAIN_SESSION, sl->plaintext && is_session_type(type));
    sl->send_seq++;
    if (n == 0)
        return;
    send_raw(s->socket, &s->stats, &sl->address, s->send_buf, n);
    sl->last_send_ns = s->now_ns;
}

static void srv_send_accept(de__net_server *s, srv_slot *sl) {
    u8 body[5];
    de__net_put_u32(body, srv_index(s, sl));
    body[4] = sl->plaintext ? (u8)DE__NET_FLAG_PLAINTEXT : 0u;
    srv_send_sealed(s, sl, DE__NET_PACKET_ACCEPT, body, sizeof body);
}

static void srv_send_denied(de__net_server *s, const de_net_address *to, de__net_deny_reason why,
                            const u8 client_pk[DE__NET_KEY_BYTES],
                            const u8 context[DE__NET_KEY_BYTES]) {
    u32 n = de__net_denied_write(s->send_buf, s->protocol_id, why, client_pk, context);
    send_raw(s->socket, &s->stats, to, s->send_buf, n);
    s->stats.denied++;
}

/* Opens a token (the sealed part from a REQUEST or a RESPONSE): its values and its connection
 * key. DE_OK, DE_ERR_NOT_FOUND (direct connect: no key) or DE_ERR_PARSE (forged). */
static de_result srv_open_token(const de__net_server *s, const u8 *token,
                                de_net_connect_token_info *info, u8 key[DE__NET_KEY_BYTES]) {
    return de__net_connect_token_open(s->server_key, token, info, key);
}

/* The server's rules for an opened token, as REQUEST and RESPONSE both apply them.
 * DE__NET_DENY_NONE when the client may have a slot. */
static de__net_deny_reason srv_token_policy(de__net_server *s, de_result opened,
                                            const de_net_connect_token_info *info) {
    if (opened == DE_ERR_NOT_FOUND) {
        if (s->deny_direct)
            return DE__NET_DENY_DIRECT_REFUSED;
    } else if (opened != DE_OK) {
        s->stats.tokens_invalid++;
        return DE__NET_DENY_TOKEN_INVALID;
    } else if (clock_unix(&s->clock) >= info->expire_unix_s) {
        s->stats.tokens_expired++;
        return DE__NET_DENY_TOKEN_EXPIRED;
    }
    if (srv_client_id_taken(s, info->client_id))
        return DE__NET_DENY_ALREADY_CONNECTED;
    for (u32 i = 0; i < s->max_peers; ++i)
        if (s->slots[i].state == DE__NET_SLOT_FREE)
            return DE__NET_DENY_NONE;
    return DE__NET_DENY_SERVER_FULL;
}

/* True when a RESPONSE with this epoch and client key was accepted before. Called only after the
 * cookie check, so its cost (a walk of the ring, no crypto) is paid only for RESPONSEs that a
 * host on the path of the REQUEST's address sent. */
static bool srv_accepted_before(const de__net_server *s, u64 epoch,
                                const u8 client_pk[DE__NET_KEY_BYTES]) {
    for (u32 i = 0; i < s->accepted_cap; ++i)
        if (s->accepted[i].epoch == epoch &&
            memcmp(s->accepted[i].client_pk, client_pk, DE__NET_KEY_BYTES) == 0)
            return true;
    return false;
}

/* Remembers an accepted RESPONSE, over the oldest entry. */
static void srv_remember_accepted(de__net_server *s, u64 epoch,
                                  const u8 client_pk[DE__NET_KEY_BYTES]) {
    srv_accepted *a = &s->accepted[s->accepted_next];
    a->epoch = epoch;
    memcpy(a->client_pk, client_pk, DE__NET_KEY_BYTES);
    s->accepted_next = (s->accepted_next + 1u) % s->accepted_cap;
}

/* REQUEST from an address without a slot: answer with a CHALLENGE or DENIED, keep nothing. */
static void srv_on_request(de__net_server *s, const de_net_address *from, const u8 *data,
                           u32 bytes) {
    de__net_request req;
    if (!de__net_request_read(data, bytes, &req) || req.protocol_id != s->protocol_id) {
        s->stats.dropped_malformed++;
        return;
    }
    s->stats.requests++;
    /* Binds the answer to these exact bytes: a REQUEST changed on the way gets an answer the
     * client cannot open. */
    u8 digest[DE__NET_KEY_BYTES];
    de__net_request_digest(digest, data);
    de_net_connect_token_info info;
    u8 psk[DE__NET_KEY_BYTES];
    de_result opened = srv_open_token(s, req.token, &info, psk);
    de__net_deny_reason why = srv_token_policy(s, opened, &info);
    de__net_wipe(&info, sizeof info);
    if (why != DE__NET_DENY_NONE) {
        srv_send_denied(s, from, why, req.client_pk, digest);
        de__net_wipe(psk, sizeof psk);
        de__net_wipe(&req, sizeof req);
        return;
    }
    const srv_epoch *ep = &s->epochs[s->current_epoch];
    de__net_keys keys;
    /* With a token, the keys need its connection key: only the client that holds it can open the
     * CHALLENGE (so a captured token is useless), and only a holder of the server key could have
     * read it (so the client knows the answer is the server's). */
    bool derived = ep->valid && de__net_derive_keys_psk(
                                    &keys, ep->secret, req.client_pk, s->protocol_id, req.client_pk,
                                    ep->public_key, opened == DE_OK ? psk : NULL);
    de__net_wipe(psk, sizeof psk);
    if (!derived) {
        s->stats.dropped_auth++; /* a low-order public key, or no randomness for an epoch */
        de__net_wipe(&req, sizeof req);
        return;
    }
    u8 body[8 + DE__NET_KEY_BYTES];
    de__net_put_u64(body, ep->id);
    de__net_cookie(body + 8, ep->cookie_secret, from, ep->id, req.client_pk);
    u32 n = de__net_packet_seal(s->send_buf, DE__NET_PACKET_MAX, DE__NET_PACKET_CHALLENGE,
                                s->protocol_id, s->challenge_seq++, ep->public_key, digest, body,
                                sizeof body, keys.s2c, DE__NET_DIR_S2C, DE__NET_DOMAIN_CHALLENGE,
                                false);
    de__net_wipe(&keys, sizeof keys);
    de__net_wipe(body, sizeof body);
    de__net_wipe(&req, sizeof req);
    if (n != 0) {
        send_raw(s->socket, &s->stats, from, s->send_buf, n);
        s->stats.challenges_sent++;
    }
}

/* RESPONSE from an address without a slot: the cookie, the accepted ring, the token, then the
 * keys; on success the client gets a slot and an ACCEPT. Every DENIED here answers a RESPONSE
 * that authenticated (so a changed RESPONSE never makes the server refuse the real client). */
static void srv_on_response(de__net_server *s, const de_net_address *from, de__net_packet *p) {
    const u8 *client_pk = p->extra;
    u64 epoch_id = de__net_get_u64(p->extra + DE__NET_KEY_BYTES);
    const u8 *cookie = p->extra + DE__NET_KEY_BYTES + 8u;
    const u8 *token = p->extra + DE__NET_KEY_BYTES + 8u + DE__NET_KEY_BYTES;
    const srv_epoch *ep = srv_find_epoch(s, epoch_id);
    if (ep == NULL) {
        s->stats.dropped_cookie++;
        return;
    }
    u8 want[DE__NET_KEY_BYTES];
    de__net_cookie(want, ep->cookie_secret, from, epoch_id, client_pk);
    bool cookie_ok = de__net_equal32(want, cookie);
    de__net_wipe(want, sizeof want);
    if (!cookie_ok) {
        s->stats.dropped_cookie++;
        return;
    }
    if (srv_accepted_before(s, epoch_id, client_pk)) {
        s->stats.dropped_replay++; /* a captured RESPONSE sent again after its connection ended */
        return;
    }
    de_net_connect_token_info info;
    u8 psk[DE__NET_KEY_BYTES];
    de_result opened = srv_open_token(s, token, &info, psk);
    de__net_keys keys;
    memset(&keys, 0, sizeof keys);
    bool ok = opened != DE_ERR_PARSE && /* a forged token has no key: nothing to check with */
              de__net_derive_keys_psk(&keys, ep->secret, client_pk, s->protocol_id, client_pk,
                                      ep->public_key, opened == DE_OK ? psk : NULL);
    de__net_wipe(psk, sizeof psk);
    if (!ok || !de__net_packet_open(p, s->protocol_id, NULL, keys.c2s, DE__NET_DIR_C2S,
                                    DE__NET_DOMAIN_SESSION, false)) {
        de__net_wipe(&keys, sizeof keys);
        de__net_wipe(&info, sizeof info);
        s->stats.dropped_auth++;
        return;
    }
    u8 flags = p->body[0];
    de__net_wipe(p->body, p->body_bytes);
    if ((flags & ~DE__NET_FLAG_PLAINTEXT) != 0) {
        de__net_wipe(&keys, sizeof keys);
        de__net_wipe(&info, sizeof info);
        s->stats.dropped_malformed++;
        return;
    }
    de__net_deny_reason why = srv_token_policy(s, opened, &info);
    if (why != DE__NET_DENY_NONE) {
        de__net_wipe(&keys, sizeof keys);
        de__net_wipe(&info, sizeof info);
        srv_send_denied(s, from, why, client_pk, cookie);
        return;
    }
    srv_remember_accepted(s, epoch_id, client_pk);
    srv_slot *sl = srv_free_slot(s); /* srv_token_policy saw one */
    u32 gen = sl->generation + 1u;
    memset(sl, 0, sizeof *sl);
    sl->state = DE__NET_SLOT_CONNECTED;
    sl->generation = gen;
    sl->address = *from;
    sl->client_id = info.client_id;
    sl->user = info.user;
    sl->plaintext = s->allow_plaintext && (flags & DE__NET_FLAG_PLAINTEXT) != 0;
    sl->keys = keys;
    de__net_replay_mark(&sl->replay, p->seq);
    sl->last_recv_ns = s->now_ns;
    sl->last_send_ns = s->now_ns;
    de__net_wipe(&keys, sizeof keys);
    de__net_wipe(&info, sizeof info);
    srv_send_accept(s, sl);
    if (s->cb.connected != NULL)
        s->cb.connected(s->cb.user, srv_index(s, sl));
}

/* A sealed packet from the address of a slot. */
static void srv_on_session(de__net_server *s, srv_slot *sl, de__net_packet *p) {
    if (sl->state != DE__NET_SLOT_CONNECTED ||
        (p->type != DE__NET_PACKET_RESPONSE && !is_session_type(p->type))) {
        s->stats.dropped_unexpected++;
        return;
    }
    if (!de__net_replay_check(&sl->replay, p->seq)) {
        s->stats.dropped_replay++;
        return;
    }
    if (!de__net_packet_open(p, s->protocol_id, NULL, sl->keys.c2s, DE__NET_DIR_C2S,
                             DE__NET_DOMAIN_SESSION, sl->plaintext && is_session_type(p->type))) {
        s->stats.dropped_auth++;
        return;
    }
    de__net_replay_mark(&sl->replay, p->seq);
    sl->last_recv_ns = s->now_ns;
    u32 index = srv_index(s, sl);
    switch (p->type) {
    case DE__NET_PACKET_RESPONSE:
        /* The client did not get the ACCEPT yet. */
        de__net_wipe(p->body, p->body_bytes);
        srv_send_accept(s, sl);
        break;
    case DE__NET_PACKET_PAYLOAD:
        s->stats.payloads_received++;
        dispatch_payload(&s->cb, index, p->body, p->body_bytes);
        break;
    case DE__NET_PACKET_DISCONNECT: {
        u8 code = p->body[0];
        srv_slot_release(sl);
        if (s->cb.disconnected != NULL)
            s->cb.disconnected(s->cb.user, index, DE__NET_END_REMOTE, code);
        break;
    }
    default: /* KEEPALIVE: last_recv_ns is all it does */
        break;
    }
}

static void srv_on_datagram(de__net_server *s, const de_net_address *from, u8 *data, u32 bytes) {
    s->stats.packets_received++;
    if (bytes == 0) {
        s->stats.dropped_malformed++;
        return;
    }
    srv_slot *sl = srv_find_address(s, from);
    if (data[0] == (u8)DE__NET_PACKET_REQUEST) {
        if (sl != NULL)
            s->stats.dropped_unexpected++; /* already connected from there */
        else
            srv_on_request(s, from, data, bytes);
        return;
    }
    de__net_packet p;
    if (!de__net_packet_parse(data, bytes, &p)) {
        s->stats.dropped_malformed++;
        return;
    }
    if (sl != NULL)
        srv_on_session(s, sl, &p);
    else if (p.type == DE__NET_PACKET_RESPONSE)
        srv_on_response(s, from, &p);
    else
        s->stats.dropped_unexpected++;
}

static void srv_send_disconnect(de__net_server *s, srv_slot *sl) {
    u8 code = sl->disconnect_code;
    srv_send_sealed(s, sl, DE__NET_PACKET_DISCONNECT, &code, 1);
}

de_result de__net_server_create(const de__net_server_desc *desc, de__net_server **out) {
    if (out == NULL || desc == NULL || desc->socket == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net server create: NULL desc, socket or out");
    *out = NULL;
    u32 peers = desc->max_peers != 0 ? desc->max_peers : 1u;
    if (peers > MAX_PEERS_LIMIT)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net server create: max_peers %u is over %u", peers,
                       MAX_PEERS_LIMIT);
    const de_allocator *a = desc->allocator != NULL ? desc->allocator : de_heap_allocator();
    u32 batch = desc->recv_batch != 0 ? desc->recv_batch : SERVER_DEFAULT_BATCH;
    if (batch > 1024u)
        batch = 1024u;

    u32 accepted_cap = peers * ACCEPTED_PER_PEER;
    if (accepted_cap < ACCEPTED_MIN)
        accepted_cap = ACCEPTED_MIN;
    size_t off_slots = align_up(sizeof(de__net_server), 16);
    size_t off_accepted = align_up(off_slots + (size_t)peers * sizeof(srv_slot), 16);
    size_t off_msgs = align_up(off_accepted + (size_t)accepted_cap * sizeof(srv_accepted), 16);
    size_t off_data = align_up(off_msgs + (size_t)batch * sizeof(de__datagram), 16);
    size_t size = off_data + (size_t)batch * DE__NET_PACKET_MAX;
    u8 *mem = (u8 *)DE_ALLOC(a, size, 16, DE_MEM_NET);
    if (mem == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "net server create: no memory for %zu bytes", size);
    memset(mem, 0, off_data);
    de__net_server *s = (de__net_server *)(void *)mem;
    s->alloc = a;
    s->alloc_size = size;
    s->socket = desc->socket;
    s->protocol_id = desc->protocol_id;
    s->max_peers = peers;
    memcpy(s->server_key, desc->server_key, sizeof s->server_key);
    s->deny_direct = desc->deny_direct_connect;
    s->allow_plaintext = NET_ALLOW_PLAINTEXT && desc->disable_encryption;
    s->timing = make_timing(0, 0, desc->idle_timeout_ms, desc->keepalive_ms);
    s->clock = desc->clock;
    s->cb = desc->callbacks;
    s->slots = (srv_slot *)(void *)(mem + off_slots);
    s->accepted = (srv_accepted *)(void *)(mem + off_accepted);
    s->accepted_cap = accepted_cap;
    s->recv_batch = batch;
    s->msgs = (de__datagram *)(void *)(mem + off_msgs);
    s->recv_data = mem + off_data;
    s->now_ns = clock_now(&s->clock);
    srv_rotate(s);
    *out = s;
    return DE_OK;
}

void de__net_server_destroy(de__net_server *s) {
    if (s == NULL)
        return;
    s->now_ns = clock_now(&s->clock);
    for (u32 i = 0; i < s->max_peers; ++i) {
        srv_slot *sl = &s->slots[i];
        if (sl->state == DE__NET_SLOT_CONNECTED) {
            sl->disconnect_code = 0;
            sl->disconnect_left = DE__NET_DISCONNECT_COPIES;
        }
        if (sl->state != DE__NET_SLOT_FREE)
            while (sl->disconnect_left-- > 0)
                srv_send_disconnect(s, sl);
    }
    const de_allocator *a = s->alloc;
    size_t size = s->alloc_size;
    /* Slots, epochs, the server key, received datagrams and the send buffer may all hold secrets
     * or plaintext. */
    de__net_wipe(s, size);
    DE_FREE(a, s, size);
}

void de__net_server_update(de__net_server *s) {
    s->now_ns = clock_now(&s->clock);
    srv_rotate(s);
    u32 handled = 0;
    while (handled < UPDATE_MAX_DATAGRAMS) {
        for (u32 i = 0; i < s->recv_batch; ++i) {
            s->msgs[i].data = s->recv_data + (size_t)i * DE__NET_PACKET_MAX;
            s->msgs[i].capacity = DE__NET_PACKET_MAX;
        }
        u32 got = 0;
        de_result r = de__socket_recv_from(s->socket, s->msgs, s->recv_batch, &got);
        for (u32 i = 0; i < got; ++i)
            srv_on_datagram(s, &s->msgs[i].from, (u8 *)s->msgs[i].data, s->msgs[i].bytes);
        handled += got;
        if (r != DE_OK || got < s->recv_batch)
            break;
    }
    for (u32 i = 0; i < s->max_peers; ++i) {
        srv_slot *sl = &s->slots[i];
        if (sl->state == DE__NET_SLOT_CONNECTED) {
            if (s->now_ns - sl->last_recv_ns >= s->timing.idle_timeout_ns) {
                srv_slot_release(sl);
                if (s->cb.timed_out != NULL)
                    s->cb.timed_out(s->cb.user, i, DE__NET_END_TIMEOUT);
            } else if (s->now_ns - sl->last_send_ns >= s->timing.keepalive_ns) {
                srv_send_sealed(s, sl, DE__NET_PACKET_KEEPALIVE, NULL, 0);
            }
        } else if (sl->state == DE__NET_SLOT_DISCONNECTING) {
            srv_send_disconnect(s, sl);
            if (--sl->disconnect_left == 0)
                srv_slot_release(sl);
        }
    }
}

de_result de__net_server_send(de__net_server *s, u32 slot, const void *data, u32 bytes) {
    if (s == NULL || (data == NULL && bytes > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net server send: NULL argument");
    if (bytes == 0 || bytes > DE__NET_PAYLOAD_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net server send: %u bytes (1 to %u allowed)",
                       bytes, (u32)DE__NET_PAYLOAD_MAX);
    if (slot >= s->max_peers || s->slots[slot].state != DE__NET_SLOT_CONNECTED)
        return DE_FAIL(DE_ERR_NOT_FOUND, "net server send: slot %u is not connected", slot);
    srv_send_sealed(s, &s->slots[slot], DE__NET_PACKET_PAYLOAD, (const u8 *)data, bytes);
    s->stats.payloads_sent++;
    return DE_OK;
}

void de__net_server_kick(de__net_server *s, u32 slot, u8 code) {
    if (s == NULL || slot >= s->max_peers || s->slots[slot].state != DE__NET_SLOT_CONNECTED)
        return;
    srv_slot *sl = &s->slots[slot];
    sl->state = DE__NET_SLOT_DISCONNECTING;
    sl->disconnect_code = code;
    srv_send_disconnect(s, sl);
    sl->disconnect_left = DE__NET_DISCONNECT_COPIES - 1u;
    if (s->cb.disconnected != NULL)
        s->cb.disconnected(s->cb.user, slot, DE__NET_END_LOCAL, code);
}

de__net_slot_info de__net_server_slot(const de__net_server *s, u32 slot) {
    de__net_slot_info info;
    memset(&info, 0, sizeof info);
    if (s == NULL || slot >= s->max_peers)
        return info;
    const srv_slot *sl = &s->slots[slot];
    info.state = sl->state;
    info.generation = sl->generation;
    info.address = sl->address;
    info.client_id = sl->client_id;
    info.user = sl->user;
    info.plaintext = sl->plaintext;
    return info;
}

u32 de__net_server_connected_count(const de__net_server *s) {
    u32 n = 0;
    for (u32 i = 0; s != NULL && i < s->max_peers; ++i)
        n += s->slots[i].state == DE__NET_SLOT_CONNECTED ? 1u : 0u;
    return n;
}

u32 de__net_server_max_peers(const de__net_server *s) {
    return s != NULL ? s->max_peers : 0u;
}

const de__net_conn_stats *de__net_server_stats(const de__net_server *s) {
    return &s->stats;
}

const de__net_keys *de__net_server_slot_keys(const de__net_server *s, u32 slot) {
    return s != NULL && slot < s->max_peers ? &s->slots[slot].keys : NULL;
}

/* ==================================================================================================
 * Client
 * ==================================================================================================
 */

struct de__net_client {
    const de_allocator *alloc;
    size_t alloc_size;
    de__socket *socket;
    u64 protocol_id;
    bool want_plaintext;
    de__net_timing timing;
    de__net_clock clock;
    de__net_callbacks cb;

    de__net_client_state state;
    de__net_end_reason end_reason;
    u8 end_detail;
    de_net_address server;
    u8 token[DE__NET_TOKEN_BYTES]; /* the sealed part; all zero for a direct connect */
    u8 psk[DE__NET_KEY_BYTES];     /* the token's connection key; wiped once the keys are derived */
    bool has_psk;                  /* false for a direct connect */
    u8 secret[DE__NET_KEY_BYTES];  /* ephemeral; wiped once the keys are derived */
    u8 public_key[DE__NET_KEY_BYTES];
    u8 request_digest[DE__NET_KEY_BYTES]; /* of the REQUEST sent (resends are identical) */
    u64 epoch;
    u8 cookie[DE__NET_KEY_BYTES];
    de__net_keys keys;
    u64 send_seq;
    de__net_replay replay;
    u32 slot;
    bool plaintext;
    u64 connect_start_ns;
    u64 last_send_ns;
    u64 last_recv_ns;
    u32 disconnect_left;
    u8 disconnect_code;
    u64 now_ns;

    u32 recv_batch;
    de__datagram *msgs;
    u8 *recv_data;
    de__net_conn_stats stats;
    u8 send_buf[DE__NET_PACKET_MAX];
};

/* Wipes every secret and handshake value; keeps configuration, counters and the end reason. */
static void cli_wipe_session(de__net_client *c) {
    de__net_wipe(c->token, sizeof c->token);
    de__net_wipe(c->psk, sizeof c->psk);
    c->has_psk = false;
    de__net_wipe(c->secret, sizeof c->secret);
    de__net_wipe(c->public_key, sizeof c->public_key);
    de__net_wipe(c->request_digest, sizeof c->request_digest);
    de__net_wipe(c->cookie, sizeof c->cookie);
    de__net_wipe(&c->keys, sizeof c->keys);
    de__net_wipe(&c->replay, sizeof c->replay);
    c->epoch = 0;
    c->send_seq = 0;
    c->slot = 0;
    c->plaintext = false;
    c->disconnect_left = 0;
}

static void cli_send(de__net_client *c, const u8 *data, u32 bytes) {
    send_raw(c->socket, &c->stats, &c->server, data, bytes);
    c->last_send_ns = c->now_ns;
}

static void cli_send_request(de__net_client *c) {
    de__net_request req;
    req.protocol_id = c->protocol_id;
    req.flags = c->want_plaintext ? (u8)DE__NET_FLAG_PLAINTEXT : 0u;
    memcpy(req.client_pk, c->public_key, sizeof req.client_pk);
    memcpy(req.token, c->token, sizeof req.token);
    u32 n = de__net_request_write(c->send_buf, &req);
    de__net_wipe(&req, sizeof req);
    de__net_request_digest(c->request_digest, c->send_buf);
    cli_send(c, c->send_buf, n);
}

static void cli_send_response(de__net_client *c) {
    u8 extra[DE__NET_KEY_BYTES + 8u + DE__NET_KEY_BYTES + DE__NET_TOKEN_BYTES];
    memcpy(extra, c->public_key, DE__NET_KEY_BYTES);
    de__net_put_u64(extra + DE__NET_KEY_BYTES, c->epoch);
    memcpy(extra + DE__NET_KEY_BYTES + 8u, c->cookie, DE__NET_KEY_BYTES);
    memcpy(extra + DE__NET_KEY_BYTES + 8u + DE__NET_KEY_BYTES, c->token, DE__NET_TOKEN_BYTES);
    u8 flags = c->want_plaintext ? (u8)DE__NET_FLAG_PLAINTEXT : 0u;
    u32 n = de__net_packet_seal(c->send_buf, DE__NET_PACKET_MAX, DE__NET_PACKET_RESPONSE,
                                c->protocol_id, c->send_seq++, extra, NULL, &flags, 1u, c->keys.c2s,
                                DE__NET_DIR_C2S, DE__NET_DOMAIN_SESSION, false);
    if (n != 0)
        cli_send(c, c->send_buf, n);
}

static void cli_send_sealed(de__net_client *c, de__net_packet_type type, const u8 *body,
                            u32 bytes) {
    u32 n =
        de__net_packet_seal(c->send_buf, DE__NET_PACKET_MAX, type, c->protocol_id, c->send_seq++,
                            NULL, NULL, body, bytes, c->keys.c2s, DE__NET_DIR_C2S,
                            DE__NET_DOMAIN_SESSION, c->plaintext && is_session_type(type));
    if (n != 0)
        cli_send(c, c->send_buf, n);
}

/* Ends the connection or the handshake without sending anything. */
static void cli_end(de__net_client *c, de__net_end_reason reason, u8 detail) {
    bool was_connected = c->state == DE__NET_CLIENT_CONNECTED;
    u32 slot = c->slot;
    cli_wipe_session(c);
    c->state = DE__NET_CLIENT_DISCONNECTED;
    c->end_reason = reason;
    c->end_detail = detail;
    if (reason == DE__NET_END_TIMEOUT || reason == DE__NET_END_CONNECT_TIMEOUT) {
        if (c->cb.timed_out != NULL)
            c->cb.timed_out(c->cb.user, slot, reason);
    } else if ((was_connected || reason == DE__NET_END_DENIED) && c->cb.disconnected != NULL) {
        c->cb.disconnected(c->cb.user, slot, reason, detail);
    }
}

static void cli_on_challenge(de__net_client *c, de__net_packet *p) {
    const u8 *server_pk = p->extra;
    de__net_keys keys;
    /* With a connection key, a CHALLENGE opens only if its sender could read the token: the
     * server, or another holder of the server key. */
    if (!de__net_derive_keys_psk(&keys, c->secret, server_pk, c->protocol_id, c->public_key,
                                 server_pk, c->has_psk ? c->psk : NULL) ||
        !de__net_packet_open(p, c->protocol_id, c->request_digest, keys.s2c, DE__NET_DIR_S2C,
                             DE__NET_DOMAIN_CHALLENGE, false)) {
        de__net_wipe(&keys, sizeof keys);
        c->stats.dropped_auth++;
        return;
    }
    c->keys = keys;
    de__net_wipe(&keys, sizeof keys);
    c->epoch = de__net_get_u64(p->body);
    memcpy(c->cookie, p->body + 8, DE__NET_KEY_BYTES);
    de__net_wipe(p->body, p->body_bytes);
    de__net_wipe(c->secret, sizeof c->secret); /* forward secrecy: the keys are all we need */
    de__net_wipe(c->psk, sizeof c->psk);
    c->state = DE__NET_CLIENT_SENDING_RESPONSE;
    cli_send_response(c);
}

static void cli_on_sealed(de__net_client *c, de__net_packet *p) {
    if (p->type == DE__NET_PACKET_CHALLENGE) {
        if (c->state == DE__NET_CLIENT_SENDING_REQUEST)
            cli_on_challenge(c, p);
        else
            c->stats.dropped_unexpected++;
        return;
    }
    bool accept_ok =
        p->type == DE__NET_PACKET_ACCEPT &&
        (c->state == DE__NET_CLIENT_SENDING_RESPONSE || c->state == DE__NET_CLIENT_CONNECTED);
    bool session_ok = is_session_type(p->type) && c->state == DE__NET_CLIENT_CONNECTED;
    if (!accept_ok && !session_ok) {
        c->stats.dropped_unexpected++;
        return;
    }
    if (!de__net_replay_check(&c->replay, p->seq)) {
        c->stats.dropped_replay++;
        return;
    }
    if (!de__net_packet_open(p, c->protocol_id, NULL, c->keys.s2c, DE__NET_DIR_S2C,
                             DE__NET_DOMAIN_SESSION, c->plaintext && is_session_type(p->type))) {
        c->stats.dropped_auth++;
        return;
    }
    if (p->type == DE__NET_PACKET_ACCEPT && c->state == DE__NET_CLIENT_SENDING_RESPONSE) {
        u8 flags = p->body[4];
        if ((flags & ~DE__NET_FLAG_PLAINTEXT) != 0 ||
            ((flags & DE__NET_FLAG_PLAINTEXT) != 0 && !c->want_plaintext)) {
            c->stats.dropped_malformed++;
            return;
        }
        de__net_replay_mark(&c->replay, p->seq);
        c->slot = de__net_get_u32(p->body);
        c->plaintext = (flags & DE__NET_FLAG_PLAINTEXT) != 0;
        c->state = DE__NET_CLIENT_CONNECTED;
        c->last_recv_ns = c->now_ns;
        de__net_wipe(c->token, sizeof c->token);
        de__net_wipe(c->cookie, sizeof c->cookie);
        if (c->cb.connected != NULL)
            c->cb.connected(c->cb.user, c->slot);
        return;
    }
    de__net_replay_mark(&c->replay, p->seq);
    c->last_recv_ns = c->now_ns;
    switch (p->type) {
    case DE__NET_PACKET_PAYLOAD:
        c->stats.payloads_received++;
        dispatch_payload(&c->cb, c->slot, p->body, p->body_bytes);
        break;
    case DE__NET_PACKET_DISCONNECT:
        cli_end(c, DE__NET_END_REMOTE, p->body[0]);
        break;
    default: /* KEEPALIVE, or an ACCEPT repeated after a resent RESPONSE */
        break;
    }
}

static void cli_on_datagram(de__net_client *c, const de_net_address *from, u8 *data, u32 bytes) {
    c->stats.packets_received++;
    if (!de_net_address_equal(from, &c->server) || c->state == DE__NET_CLIENT_DISCONNECTED ||
        c->state == DE__NET_CLIENT_DISCONNECTING) {
        c->stats.dropped_unexpected++;
        return;
    }
    if (bytes == 0) {
        c->stats.dropped_malformed++;
        return;
    }
    if (data[0] == (u8)DE__NET_PACKET_DENIED) {
        de__net_deny_reason why = DE__NET_DENY_NONE;
        if (bytes != DE__NET_DENIED_BYTES) {
            c->stats.dropped_malformed++;
        } else if (c->state != DE__NET_CLIENT_SENDING_REQUEST &&
                   c->state != DE__NET_CLIENT_SENDING_RESPONSE) {
            c->stats.dropped_unexpected++;
        } else if (!(c->state == DE__NET_CLIENT_SENDING_RESPONSE &&
                     de__net_denied_read(data, bytes, c->protocol_id, c->public_key, c->cookie,
                                         &why)) &&
                   !de__net_denied_read(data, bytes, c->protocol_id, c->public_key,
                                        c->request_digest, &why)) {
            /* A DENIED answers a RESPONSE (context: cookie) or a REQUEST (context: digest). */
            c->stats.dropped_auth++;
        } else {
            c->stats.denied++;
            cli_end(c, DE__NET_END_DENIED, (u8)why);
        }
        return;
    }
    de__net_packet p;
    if (!de__net_packet_parse(data, bytes, &p)) {
        c->stats.dropped_malformed++;
        return;
    }
    cli_on_sealed(c, &p);
}

de_result de__net_client_create(const de__net_client_desc *desc, de__net_client **out) {
    if (out == NULL || desc == NULL || desc->socket == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net client create: NULL desc, socket or out");
    *out = NULL;
    const de_allocator *a = desc->allocator != NULL ? desc->allocator : de_heap_allocator();
    u32 batch = desc->recv_batch != 0 ? desc->recv_batch : CLIENT_DEFAULT_BATCH;
    if (batch > 1024u)
        batch = 1024u;
    size_t off_msgs = align_up(sizeof(de__net_client), 16);
    size_t off_data = align_up(off_msgs + (size_t)batch * sizeof(de__datagram), 16);
    size_t size = off_data + (size_t)batch * DE__NET_PACKET_MAX;
    u8 *mem = (u8 *)DE_ALLOC(a, size, 16, DE_MEM_NET);
    if (mem == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "net client create: no memory for %zu bytes", size);
    memset(mem, 0, off_data);
    de__net_client *c = (de__net_client *)(void *)mem;
    c->alloc = a;
    c->alloc_size = size;
    c->socket = desc->socket;
    c->protocol_id = desc->protocol_id;
    c->want_plaintext = NET_ALLOW_PLAINTEXT && desc->disable_encryption;
    c->timing = make_timing(desc->resend_ms, desc->connect_timeout_ms, desc->idle_timeout_ms,
                            desc->keepalive_ms);
    c->clock = desc->clock;
    c->cb = desc->callbacks;
    c->recv_batch = batch;
    c->msgs = (de__datagram *)(void *)(mem + off_msgs);
    c->recv_data = mem + off_data;
    c->state = DE__NET_CLIENT_DISCONNECTED;
    *out = c;
    return DE_OK;
}

void de__net_client_destroy(de__net_client *c) {
    if (c == NULL)
        return;
    c->now_ns = clock_now(&c->clock);
    if (c->state == DE__NET_CLIENT_CONNECTED) {
        c->disconnect_code = 0;
        c->disconnect_left = DE__NET_DISCONNECT_COPIES;
    }
    if (c->state == DE__NET_CLIENT_CONNECTED || c->state == DE__NET_CLIENT_DISCONNECTING) {
        u8 code = c->disconnect_code;
        while (c->disconnect_left-- > 0)
            cli_send_sealed(c, DE__NET_PACKET_DISCONNECT, &code, 1);
    }
    const de_allocator *a = c->alloc;
    size_t size = c->alloc_size;
    de__net_wipe(c, size);
    DE_FREE(a, c, size);
}

de_result de__net_client_connect(de__net_client *c, const de_net_address *server,
                                 const u8 token[DE_NET_CONNECT_TOKEN_BYTES]) {
    if (c == NULL || server == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net client connect: NULL argument");
    if (c->state != DE__NET_CLIENT_DISCONNECTED)
        return DE_FAIL(DE_ERR_BUSY, "net client connect: not disconnected (state %d)",
                       (int)c->state);
    u8 any_sealed = 0, any_key = 0;
    for (u32 i = 0; token != NULL && i < DE__NET_TOKEN_BYTES; ++i)
        any_sealed |= token[i];
    for (u32 i = 0; token != NULL && i < DE__NET_KEY_BYTES; ++i)
        any_key |= token[DE__NET_TOKEN_BYTES + i];
    if (any_sealed != 0 && any_key == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "net client connect: the token has no connection key (its last %u bytes)",
                       (u32)DE__NET_KEY_BYTES);
    cli_wipe_session(c);
    c->now_ns = clock_now(&c->clock);
    c->server = *server;
    if (any_sealed != 0) {
        memcpy(c->token, token, DE__NET_TOKEN_BYTES);
        memcpy(c->psk, token + DE__NET_TOKEN_BYTES, DE__NET_KEY_BYTES);
        c->has_psk = true;
    }
    c->end_reason = DE__NET_END_NONE;
    c->end_detail = 0;
    de_result r = de__net_x25519_keypair(c->secret, c->public_key);
    if (r != DE_OK) {
        cli_wipe_session(c);
        c->end_reason = DE__NET_END_ERROR;
        return r;
    }
    c->state = DE__NET_CLIENT_SENDING_REQUEST;
    c->connect_start_ns = c->now_ns;
    cli_send_request(c);
    return DE_OK;
}

void de__net_client_update(de__net_client *c) {
    c->now_ns = clock_now(&c->clock);
    u32 handled = 0;
    while (handled < UPDATE_MAX_DATAGRAMS) {
        for (u32 i = 0; i < c->recv_batch; ++i) {
            c->msgs[i].data = c->recv_data + (size_t)i * DE__NET_PACKET_MAX;
            c->msgs[i].capacity = DE__NET_PACKET_MAX;
        }
        u32 got = 0;
        de_result r = de__socket_recv_from(c->socket, c->msgs, c->recv_batch, &got);
        for (u32 i = 0; i < got; ++i)
            cli_on_datagram(c, &c->msgs[i].from, (u8 *)c->msgs[i].data, c->msgs[i].bytes);
        handled += got;
        if (r != DE_OK || got < c->recv_batch)
            break;
    }
    switch (c->state) {
    case DE__NET_CLIENT_SENDING_REQUEST:
    case DE__NET_CLIENT_SENDING_RESPONSE:
        if (c->now_ns - c->connect_start_ns >= c->timing.connect_timeout_ns)
            cli_end(c, DE__NET_END_CONNECT_TIMEOUT, 0);
        else if (c->now_ns - c->last_send_ns >= c->timing.resend_ns) {
            if (c->state == DE__NET_CLIENT_SENDING_REQUEST)
                cli_send_request(c);
            else
                cli_send_response(c);
        }
        break;
    case DE__NET_CLIENT_CONNECTED:
        if (c->now_ns - c->last_recv_ns >= c->timing.idle_timeout_ns)
            cli_end(c, DE__NET_END_TIMEOUT, 0);
        else if (c->now_ns - c->last_send_ns >= c->timing.keepalive_ns)
            cli_send_sealed(c, DE__NET_PACKET_KEEPALIVE, NULL, 0);
        break;
    case DE__NET_CLIENT_DISCONNECTING: {
        u8 code = c->disconnect_code;
        cli_send_sealed(c, DE__NET_PACKET_DISCONNECT, &code, 1);
        if (--c->disconnect_left == 0) {
            cli_wipe_session(c);
            c->state = DE__NET_CLIENT_DISCONNECTED;
        }
        break;
    }
    default:
        break;
    }
}

de_result de__net_client_send(de__net_client *c, const void *data, u32 bytes) {
    if (c == NULL || (data == NULL && bytes > 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net client send: NULL argument");
    if (bytes == 0 || bytes > DE__NET_PAYLOAD_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "net client send: %u bytes (1 to %u allowed)",
                       bytes, (u32)DE__NET_PAYLOAD_MAX);
    if (c->state != DE__NET_CLIENT_CONNECTED)
        return DE_FAIL(DE_ERR_NOT_FOUND, "net client send: not connected");
    c->now_ns = clock_now(&c->clock);
    cli_send_sealed(c, DE__NET_PACKET_PAYLOAD, (const u8 *)data, bytes);
    c->stats.payloads_sent++;
    return DE_OK;
}

void de__net_client_disconnect(de__net_client *c, u8 code) {
    if (c == NULL)
        return;
    if (c->state == DE__NET_CLIENT_SENDING_REQUEST || c->state == DE__NET_CLIENT_SENDING_RESPONSE) {
        cli_wipe_session(c);
        c->state = DE__NET_CLIENT_DISCONNECTED;
        c->end_reason = DE__NET_END_LOCAL;
        c->end_detail = code;
        return;
    }
    if (c->state != DE__NET_CLIENT_CONNECTED)
        return;
    c->now_ns = clock_now(&c->clock);
    c->state = DE__NET_CLIENT_DISCONNECTING;
    c->end_reason = DE__NET_END_LOCAL;
    c->end_detail = code;
    c->disconnect_code = code;
    cli_send_sealed(c, DE__NET_PACKET_DISCONNECT, &code, 1);
    c->disconnect_left = DE__NET_DISCONNECT_COPIES - 1u;
    if (c->cb.disconnected != NULL)
        c->cb.disconnected(c->cb.user, c->slot, DE__NET_END_LOCAL, code);
}

de__net_client_state de__net_client_get_state(const de__net_client *c) {
    return c != NULL ? c->state : DE__NET_CLIENT_DISCONNECTED;
}

de__net_end_reason de__net_client_end_reason(const de__net_client *c, u8 *detail) {
    if (detail != NULL)
        *detail = c != NULL ? c->end_detail : 0u;
    return c != NULL ? c->end_reason : DE__NET_END_NONE;
}

u32 de__net_client_slot(const de__net_client *c) {
    return c != NULL ? c->slot : 0u;
}

const de__net_conn_stats *de__net_client_stats(const de__net_client *c) {
    return &c->stats;
}

const de__net_keys *de__net_client_keys(const de__net_client *c) {
    return &c->keys;
}

bool de__net_client_plaintext(const de__net_client *c) {
    return c != NULL && c->plaintext;
}
