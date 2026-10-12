/* Tests for the connection layer (DENG-52): the challenge handshake over the loopback transport
 * and over UDP 127.0.0.1, payloads, tampering, replay, DENIED, timeouts with an injected clock,
 * the allocation-free stateless challenge, redundant DISCONNECTs and key wiping. DENG-378: the
 * connection key of a token authenticates both ends (a man in the middle, a stolen token, a
 * replayed RESPONSE), and what a direct connect still lacks.
 *
 * A "tap" socket sits between each endpoint and its loopback socket: it can hold back outgoing
 * datagrams (so a test can change and deliver them itself) or drop some of them. */
#include "de_test.h"

#include "de_alloc_guard.h"

#include <dengine/de_net.h>
#include <dengine/de_platform.h>

#include "net/connection.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define PROTOCOL_ID 0x44454E47000100ull /* "DENG", version 1.0 */
#define NS_PER_MS   1000000ull
#define NOW_UNIX    1800000000ull

/* ---------- Fake clock ---------- */

typedef struct fake_clock {
    u64 ns;
    u64 unix_s;
} fake_clock;

static u64 fake_now_ns(void *user) {
    return ((fake_clock *)user)->ns;
}

static u64 fake_unix_s(void *user) {
    return ((fake_clock *)user)->unix_s;
}

static de__net_clock clock_of(fake_clock *c) {
    de__net_clock k;
    k.now_ns = fake_now_ns;
    k.unix_s = fake_unix_s;
    k.user = c;
    return k;
}

/* ---------- Event recorder ---------- */

typedef struct events {
    u32 connected, disconnected, timed_out, payloads;
    u32 last_slot;
    de__net_end_reason last_reason;
    u8 last_detail;
    u8 last_payload[DE__NET_PAYLOAD_MAX];
    u32 last_bytes;
} events;

static void ev_connected(void *user, u32 slot) {
    events *e = (events *)user;
    e->connected++;
    e->last_slot = slot;
}

static void ev_disconnected(void *user, u32 slot, de__net_end_reason reason, u8 detail) {
    events *e = (events *)user;
    e->disconnected++;
    e->last_slot = slot;
    e->last_reason = reason;
    e->last_detail = detail;
}

static void ev_timed_out(void *user, u32 slot, de__net_end_reason reason) {
    events *e = (events *)user;
    e->timed_out++;
    e->last_slot = slot;
    e->last_reason = reason;
}

static void ev_payload(void *user, u32 slot, const u8 *data, u32 bytes) {
    events *e = (events *)user;
    e->payloads++;
    e->last_slot = slot;
    e->last_bytes = bytes;
    memcpy(e->last_payload, data, bytes);
}

static de__net_callbacks callbacks_of(events *e) {
    de__net_callbacks cb;
    cb.connected = ev_connected;
    cb.disconnected = ev_disconnected;
    cb.timed_out = ev_timed_out;
    cb.payload = ev_payload;
    cb.user = e;
    return cb;
}

/* ---------- Tap socket ---------- */

#define TAP_CAP 16u

typedef struct tap {
    de__socket sock;
    de__socket *inner;
    bool hold; /* keep outgoing datagrams in `held` instead of sending them */
    u32 held_count;
    u8 held[TAP_CAP][DE__NET_PACKET_MAX];
    u32 held_bytes[TAP_CAP];
    de_net_address held_to[TAP_CAP];
    u32 drop_type; /* drop outgoing datagrams of this type ... */
    u32 drop_left; /* ... this many more times */
    u32 sent_type[16];
} tap;

static de_result tap_send(de__socket *s, const de_net_address *to, const void *data, u32 bytes,
                          u32 *sent) {
    tap *t = (tap *)s->user;
    u32 type = bytes > 0 ? ((const u8 *)data)[0] & 15u : 0u;
    t->sent_type[type]++;
    if (t->hold) {
        if (t->held_count < TAP_CAP) {
            memcpy(t->held[t->held_count], data, bytes);
            t->held_bytes[t->held_count] = bytes;
            t->held_to[t->held_count] = *to;
            t->held_count++;
        }
        *sent = bytes;
        return DE_OK;
    }
    if (t->drop_left > 0 && t->drop_type == type) {
        t->drop_left--;
        *sent = bytes;
        return DE_OK;
    }
    return de__socket_send_to(t->inner, to, data, bytes, sent);
}

static de_result tap_recv(de__socket *s, de__datagram *msgs, u32 count, u32 *received) {
    tap *t = (tap *)s->user;
    return de__socket_recv_from(t->inner, msgs, count, received);
}

static void tap_close(de__socket *s) {
    (void)s;
}

static void tap_init(tap *t, de__socket *inner) {
    memset(t, 0, sizeof *t);
    t->inner = inner;
    t->sock.send_to = tap_send;
    t->sock.recv_from = tap_recv;
    t->sock.close = tap_close;
    t->sock.user = t;
    t->sock.local = inner->local;
}

/* Sends a datagram from the tap's own address, around the endpoint. */
static void tap_inject(tap *t, const de_net_address *to, const u8 *data, u32 bytes) {
    u32 sent = 0;
    ASSERT_EQ(de__socket_send_to(t->inner, to, data, bytes, &sent), DE_OK);
    ASSERT_EQ(sent, bytes);
}

/* Index of the last held datagram of `type`. */
static u32 tap_find(const tap *t, de__net_packet_type type) {
    u32 found = TAP_CAP;
    for (u32 i = t->held_count; i-- > 0 && found == TAP_CAP;)
        if ((t->held[i][0] & 15u) == (u32)type)
            found = i;
    if (found == TAP_CAP)
        FAIL("no held datagram of that type"); /* does not return */
    return found;
}

/* ---------- Rig: one server, up to 4 clients on a loopback network ---------- */

#define RIG_CLIENTS 4u

typedef struct rig_opts {
    u32 max_peers;
    bool deny_direct;
    bool server_plaintext;
    bool client_plaintext;
} rig_opts;

typedef struct rig {
    fake_clock clock;
    de__loopback_net *net;
    de__socket *server_sock;
    de__socket *client_sock[RIG_CLIENTS];
    tap server_tap;
    tap client_tap[RIG_CLIENTS];
    de__net_server *server;
    de__net_client *client[RIG_CLIENTS];
    events sev;
    events cev[RIG_CLIENTS];
    u8 key[DE_NET_SERVER_KEY_BYTES];
    de_net_address server_addr;
} rig;

static rig g_rig; /* big (taps hold datagrams): static, not on the stack */

static rig *rig_create(const rig_opts *o) {
    rig *r = &g_rig;
    memset(r, 0, sizeof *r);
    r->clock.ns = 1000ull * 1000000000ull;
    r->clock.unix_s = NOW_UNIX;
    de__loopback_desc ld = {0};
    ld.queue_capacity = 1024;
    ASSERT_EQ(de__loopback_net_create(&ld, NULL, &r->net), DE_OK);
    r->server_addr = de_net_address_ipv4(127, 0, 0, 1, 40000);
    ASSERT_EQ(de__loopback_open(r->net, &r->server_addr, &r->server_sock), DE_OK);
    tap_init(&r->server_tap, r->server_sock);
    for (u32 i = 0; i < RIG_CLIENTS; ++i) {
        ASSERT_EQ(de__loopback_open(r->net, NULL, &r->client_sock[i]), DE_OK);
        tap_init(&r->client_tap[i], r->client_sock[i]);
    }
    ASSERT_EQ(de_net_server_key_generate(r->key), DE_OK);

    de__net_server_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.socket = &r->server_tap.sock;
    sd.protocol_id = PROTOCOL_ID;
    sd.max_peers = o->max_peers != 0 ? o->max_peers : 4u;
    memcpy(sd.server_key, r->key, sizeof r->key);
    sd.deny_direct_connect = o->deny_direct;
    sd.disable_encryption = o->server_plaintext;
    sd.idle_timeout_ms = 10000;
    sd.keepalive_ms = 1000;
    sd.clock = clock_of(&r->clock);
    sd.callbacks = callbacks_of(&r->sev);
    ASSERT_EQ(de__net_server_create(&sd, &r->server), DE_OK);

    for (u32 i = 0; i < RIG_CLIENTS; ++i) {
        de__net_client_desc cd;
        memset(&cd, 0, sizeof cd);
        cd.socket = &r->client_tap[i].sock;
        cd.protocol_id = PROTOCOL_ID;
        cd.disable_encryption = o->client_plaintext;
        cd.resend_ms = 100;
        cd.connect_timeout_ms = 5000;
        cd.idle_timeout_ms = 10000;
        cd.keepalive_ms = 1000;
        cd.clock = clock_of(&r->clock);
        cd.callbacks = callbacks_of(&r->cev[i]);
        ASSERT_EQ(de__net_client_create(&cd, &r->client[i]), DE_OK);
    }
    return r;
}

static void rig_destroy(rig *r) {
    for (u32 i = 0; i < RIG_CLIENTS; ++i)
        de__net_client_destroy(r->client[i]);
    de__net_server_destroy(r->server);
    for (u32 i = 0; i < RIG_CLIENTS; ++i)
        de__socket_close(r->client_sock[i]);
    de__socket_close(r->server_sock);
    de__loopback_net_destroy(r->net);
}

static void make_token(rig *r, u64 client_id, u8 out[DE_NET_CONNECT_TOKEN_BYTES]) {
    ASSERT_EQ(
        de_net_connect_token_generate(r->key, client_id, NOW_UNIX + 3600, client_id * 10u, out),
        DE_OK);
}

/* One step: every client updates, then the server, then the clocks move `ms`. */
static void rig_step(rig *r, u32 ms) {
    for (u32 i = 0; i < RIG_CLIENTS; ++i)
        de__net_client_update(r->client[i]);
    de__net_server_update(r->server);
    r->clock.ns += (u64)ms * NS_PER_MS;
}

/* Connects client `i` with a fresh token for `client_id`; returns the steps it took. */
static u32 rig_connect(rig *r, u32 i, u64 client_id) {
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    make_token(r, client_id, token);
    ASSERT_EQ(de__net_client_connect(r->client[i], &r->server_addr, token), DE_OK);
    u32 steps = 0;
    while (de__net_client_get_state(r->client[i]) != DE__NET_CLIENT_CONNECTED && steps < 100) {
        rig_step(r, 1);
        steps++;
    }
    ASSERT_EQ(de__net_client_get_state(r->client[i]), DE__NET_CLIENT_CONNECTED);
    return steps;
}

/* ---------- Happy paths ---------- */

TEST(net, handshake_loopback_happy) {
    rig_opts o = {0};
    rig *r = rig_create(&o);
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    make_token(r, 77, token);
    ASSERT_EQ(de__net_client_connect(r->client[0], &r->server_addr, token), DE_OK);
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_SENDING_REQUEST);
    EXPECT_EQ(r->client_tap[0].sent_type[DE__NET_PACKET_REQUEST], 1u);

    /* Round trip 1: REQUEST -> CHALLENGE. Round trip 2: RESPONSE -> ACCEPT. */
    de__net_server_update(r->server);
    EXPECT_EQ(r->server_tap.sent_type[DE__NET_PACKET_CHALLENGE], 1u);
    EXPECT_EQ(de__net_server_connected_count(r->server), 0u); /* no state for a challenge */
    de__net_client_update(r->client[0]);
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_SENDING_RESPONSE);
    de__net_server_update(r->server);
    EXPECT_EQ(de__net_server_connected_count(r->server), 1u);
    EXPECT_EQ(r->sev.connected, 1u);
    de__net_client_update(r->client[0]);
    ASSERT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_CONNECTED);
    EXPECT_EQ(r->cev[0].connected, 1u);
    /* Two round trips, no resends. */
    EXPECT_EQ(r->client_tap[0].sent_type[DE__NET_PACKET_REQUEST], 1u);
    EXPECT_EQ(r->client_tap[0].sent_type[DE__NET_PACKET_RESPONSE], 1u);
    EXPECT_LE(r->client_tap[0].sent_type[DE__NET_PACKET_REQUEST] +
                  r->client_tap[0].sent_type[DE__NET_PACKET_RESPONSE],
              3u);

    u32 slot = de__net_client_slot(r->client[0]);
    de__net_slot_info info = de__net_server_slot(r->server, slot);
    EXPECT_EQ(info.state, DE__NET_SLOT_CONNECTED);
    EXPECT_EQ(info.client_id, 77u);
    EXPECT_EQ(info.user, 770u);
    EXPECT_EQ(info.generation, 1u);
    EXPECT_TRUE(de_net_address_equal(&info.address, &r->client_sock[0]->local));
    EXPECT_FALSE(info.plaintext);
    /* Both sides derived the same keys; each direction has its own. */
    const de__net_keys *ck = de__net_client_keys(r->client[0]);
    const de__net_keys *sk = de__net_server_slot_keys(r->server, slot);
    EXPECT_MEMEQ(ck->c2s, sk->c2s, 32);
    EXPECT_MEMEQ(ck->s2c, sk->s2c, 32);
    EXPECT_NE(memcmp(ck->c2s, ck->s2c, 32), 0);

    /* PAYLOADs both ways. */
    const char hello[] = "hello server";
    ASSERT_EQ(de__net_client_send(r->client[0], hello, sizeof hello), DE_OK);
    de__net_server_update(r->server);
    EXPECT_EQ(r->sev.payloads, 1u);
    EXPECT_EQ(r->sev.last_slot, slot);
    EXPECT_EQ(r->sev.last_bytes, (u32)sizeof hello);
    EXPECT_MEMEQ(r->sev.last_payload, hello, sizeof hello);
    u8 big[DE__NET_PAYLOAD_MAX];
    for (u32 i = 0; i < sizeof big; ++i)
        big[i] = (u8)(i * 3u);
    ASSERT_EQ(de__net_server_send(r->server, slot, big, sizeof big), DE_OK);
    de__net_client_update(r->client[0]);
    EXPECT_EQ(r->cev[0].payloads, 1u);
    EXPECT_EQ(r->cev[0].last_bytes, (u32)sizeof big);
    EXPECT_MEMEQ(r->cev[0].last_payload, big, sizeof big);
    /* The wire does not carry the plaintext. */
    EXPECT_EQ(de__net_conn_stats_dropped(de__net_server_stats(r->server)), 0u);
    EXPECT_EQ(de__net_conn_stats_dropped(de__net_client_stats(r->client[0])), 0u);

    /* Sends that cannot work. */
    EXPECT_EQ(de__net_client_send(r->client[0], big, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__net_client_send(r->client[0], big, DE__NET_PAYLOAD_MAX + 1u),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__net_server_send(r->server, slot + 1u, big, 4), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de__net_client_send(r->client[1], big, 4), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de__net_client_connect(r->client[0], &r->server_addr, token), DE_ERR_BUSY);

    /* A second client with a direct connect (all-zero token, allowed by default). */
    ASSERT_EQ(de__net_client_connect(r->client[1], &r->server_addr, NULL), DE_OK);
    for (u32 i = 0; i < 4; ++i)
        rig_step(r, 1);
    ASSERT_EQ(de__net_client_get_state(r->client[1]), DE__NET_CLIENT_CONNECTED);
    info = de__net_server_slot(r->server, de__net_client_slot(r->client[1]));
    EXPECT_EQ(info.client_id, 0u);
    EXPECT_EQ(de__net_server_connected_count(r->server), 2u);
    rig_destroy(r);
}

TEST(net, handshake_udp_happy) {
    de__socket *ss = NULL, *cs = NULL;
    de_net_address bind = de_net_address_ipv4(127, 0, 0, 1, 0);
    ASSERT_EQ(de__udp_open(&bind, NULL, NULL, &ss), DE_OK);
    ASSERT_EQ(de__udp_open(&bind, NULL, NULL, &cs), DE_OK);
    u8 key[DE_NET_SERVER_KEY_BYTES];
    ASSERT_EQ(de_net_server_key_generate(key), DE_OK);
    events sev, cev;
    memset(&sev, 0, sizeof sev);
    memset(&cev, 0, sizeof cev);

    de__net_server_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.socket = ss;
    sd.protocol_id = PROTOCOL_ID;
    sd.max_peers = 8;
    memcpy(sd.server_key, key, sizeof key);
    sd.callbacks = callbacks_of(&sev);
    de__net_server *server = NULL;
    ASSERT_EQ(de__net_server_create(&sd, &server), DE_OK);
    de__net_client_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.socket = cs;
    cd.protocol_id = PROTOCOL_ID;
    cd.callbacks = callbacks_of(&cev);
    de__net_client *client = NULL;
    ASSERT_EQ(de__net_client_create(&cd, &client), DE_OK);

    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    ASSERT_EQ(de_net_connect_token_generate(key, 5, (u64)time(NULL) + 600, 1, token), DE_OK);
    ASSERT_EQ(de__net_client_connect(client, &ss->local, token), DE_OK);
    /* Real clock, real sockets: poll until connected (the resend timer covers a lost datagram). */
    u64 deadline = de_time_now_ns() + 3000ull * NS_PER_MS;
    while (de__net_client_get_state(client) != DE__NET_CLIENT_CONNECTED &&
           de_time_now_ns() < deadline) {
        de__net_server_update(server);
        de__net_client_update(client);
    }
    ASSERT_EQ(de__net_client_get_state(client), DE__NET_CLIENT_CONNECTED);
    EXPECT_EQ(sev.connected, 1u);
    EXPECT_EQ(cev.connected, 1u);
    EXPECT_LE(de__net_client_stats(client)->packets_sent, 3u); /* REQUEST, RESPONSE (+1 resend) */

    const char ping[] = "ping over udp";
    ASSERT_EQ(de__net_client_send(client, ping, sizeof ping), DE_OK);
    deadline = de_time_now_ns() + 2000ull * NS_PER_MS;
    while (sev.payloads == 0 && de_time_now_ns() < deadline)
        de__net_server_update(server);
    ASSERT_EQ(sev.payloads, 1u);
    EXPECT_MEMEQ(sev.last_payload, ping, sizeof ping);
    const char pong[] = "pong";
    ASSERT_EQ(de__net_server_send(server, sev.last_slot, pong, sizeof pong), DE_OK);
    deadline = de_time_now_ns() + 2000ull * NS_PER_MS;
    while (cev.payloads == 0 && de_time_now_ns() < deadline)
        de__net_client_update(client);
    ASSERT_EQ(cev.payloads, 1u);
    EXPECT_MEMEQ(cev.last_payload, pong, sizeof pong);

    /* Clean disconnect reaches the server. */
    de__net_client_disconnect(client, 9);
    deadline = de_time_now_ns() + 2000ull * NS_PER_MS;
    while (sev.disconnected == 0 && de_time_now_ns() < deadline) {
        de__net_client_update(client);
        de__net_server_update(server);
    }
    EXPECT_EQ(sev.disconnected, 1u);
    EXPECT_EQ(sev.last_reason, DE__NET_END_REMOTE);
    EXPECT_EQ(sev.last_detail, 9);

    de__net_client_destroy(client);
    de__net_server_destroy(server);
    de__socket_close(cs);
    de__socket_close(ss);
}

/* ---------- Tampering ---------- */

/* Snapshot of everything a delivered packet could change. */
typedef struct snapshot {
    de__net_client_state cstate;
    u32 connected;
    u32 sev_total, cev_total;
    u64 dropped;
} snapshot;

static snapshot snap(rig *r) {
    snapshot s;
    s.cstate = de__net_client_get_state(r->client[0]);
    s.connected = de__net_server_connected_count(r->server);
    s.sev_total = r->sev.connected + r->sev.disconnected + r->sev.timed_out + r->sev.payloads;
    s.cev_total =
        r->cev[0].connected + r->cev[0].disconnected + r->cev[0].timed_out + r->cev[0].payloads;
    s.dropped = de__net_conn_stats_dropped(de__net_server_stats(r->server)) +
                de__net_conn_stats_dropped(de__net_client_stats(r->client[0])) +
                de__net_server_stats(r->server)->tokens_invalid;
    return s;
}

/*
 * Delivers every one-byte change (two masks per byte) of `pkt` to `to` from `from`, lets both
 * sides handle it (and anything they answered), and checks after each one: nothing changed and a
 * drop counter went up. Then delivers the original, which must be accepted (`accept` = true) to
 * show the changes were the cause.
 */
static void flip_all(rig *r, tap *from, const de_net_address *to, const u8 *pkt, u32 bytes) {
    static const u8 masks[2] = {0x01, 0x80};
    u8 copy[DE__NET_PACKET_MAX];
    for (u32 i = 0; i < bytes; ++i) {
        for (u32 m = 0; m < 2; ++m) {
            memcpy(copy, pkt, bytes);
            copy[i] ^= masks[m];
            snapshot before = snap(r);
            tap_inject(from, to, copy, bytes);
            de__net_server_update(r->server);
            de__net_client_update(r->client[0]);
            de__net_server_update(r->server);
            snapshot after = snap(r);
            if (after.cstate != before.cstate || after.connected != before.connected ||
                after.sev_total != before.sev_total || after.cev_total != before.cev_total ||
                after.dropped <= before.dropped) {
                char what[128];
                snprintf(what, sizeof what,
                         "changed packet not dropped cleanly: type %u, byte %u of %u, mask 0x%02x",
                         pkt[0] & 15u, i, bytes, masks[m]);
                FAIL(what); /* expected: dropped, counted, no state change; does not return */
            }
        }
    }
}

TEST(net, handshake_tamper) {
    rig_opts o = {0};
    rig *r = rig_create(&o);
    tap *ct = &r->client_tap[0];
    tap *st = &r->server_tap;
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    make_token(r, 1, token);

    /* REQUEST: held, changed, delivered. A change the server cannot see (client_pk, the flags
     * bit) or that makes it answer DENIED (token) gives an answer bound to other bytes, which the
     * client drops. */
    ct->hold = true;
    ASSERT_EQ(de__net_client_connect(r->client[0], &r->server_addr, token), DE_OK);
    u32 idx = tap_find(ct, DE__NET_PACKET_REQUEST);
    u8 request[DE__NET_PACKET_MAX];
    u32 request_bytes = ct->held_bytes[idx];
    memcpy(request, ct->held[idx], request_bytes);
    ct->hold = false;
    flip_all(r, ct, &r->server_addr, request, request_bytes);
    EXPECT_EQ(de__net_server_stats(r->server)->challenges_sent +
                  de__net_server_stats(r->server)->denied,
              de__net_client_stats(r->client[0])->dropped_auth);

    /* CHALLENGE: the original request goes through, the server's answer is held. */
    st->hold = true;
    st->held_count = 0;
    tap_inject(ct, &r->server_addr, request, request_bytes);
    de__net_server_update(r->server);
    idx = tap_find(st, DE__NET_PACKET_CHALLENGE);
    u8 challenge[DE__NET_PACKET_MAX];
    u32 challenge_bytes = st->held_bytes[idx];
    memcpy(challenge, st->held[idx], challenge_bytes);
    st->hold = false;
    flip_all(r, st, &r->client_sock[0]->local, challenge, challenge_bytes);
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_SENDING_REQUEST);

    /* RESPONSE: the original challenge goes through, the client's RESPONSE is held. */
    ct->hold = true;
    ct->held_count = 0;
    tap_inject(st, &r->client_sock[0]->local, challenge, challenge_bytes);
    de__net_client_update(r->client[0]);
    ASSERT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_SENDING_RESPONSE);
    idx = tap_find(ct, DE__NET_PACKET_RESPONSE);
    u8 response[DE__NET_PACKET_MAX];
    u32 response_bytes = ct->held_bytes[idx];
    memcpy(response, ct->held[idx], response_bytes);
    ct->hold = false;
    flip_all(r, ct, &r->server_addr, response, response_bytes);
    EXPECT_EQ(de__net_server_connected_count(r->server), 0u);

    /* ACCEPT. */
    st->hold = true;
    st->held_count = 0;
    tap_inject(ct, &r->server_addr, response, response_bytes);
    de__net_server_update(r->server);
    ASSERT_EQ(de__net_server_connected_count(r->server), 1u);
    idx = tap_find(st, DE__NET_PACKET_ACCEPT);
    u8 accept[DE__NET_PACKET_MAX];
    u32 accept_bytes = st->held_bytes[idx];
    memcpy(accept, st->held[idx], accept_bytes);
    st->hold = false;
    flip_all(r, st, &r->client_sock[0]->local, accept, accept_bytes);
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_SENDING_RESPONSE);
    tap_inject(st, &r->client_sock[0]->local, accept, accept_bytes);
    de__net_client_update(r->client[0]);
    ASSERT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_CONNECTED);

    /* KEEPALIVE, PAYLOAD and DISCONNECT, both directions. */
    u32 slot = de__net_client_slot(r->client[0]);
    for (u32 dir = 0; dir < 2; ++dir) {
        tap *sender = dir == 0 ? ct : st;
        const de_net_address *to = dir == 0 ? &r->server_addr : &r->client_sock[0]->local;
        sender->hold = true;
        sender->held_count = 0;
        const u8 msg[5] = {1, 2, 3, 4, 5};
        if (dir == 0) {
            ASSERT_EQ(de__net_client_send(r->client[0], msg, sizeof msg), DE_OK);
            r->clock.ns += 1500ull * NS_PER_MS; /* past keepalive_ms */
            de__net_client_update(r->client[0]);
        } else {
            ASSERT_EQ(de__net_server_send(r->server, slot, msg, sizeof msg), DE_OK);
            r->clock.ns += 1500ull * NS_PER_MS;
            de__net_server_update(r->server);
        }
        sender->hold = false;
        static const de__net_packet_type types[2] = {DE__NET_PACKET_PAYLOAD,
                                                     DE__NET_PACKET_KEEPALIVE};
        for (u32 t = 0; t < 2; ++t) {
            idx = tap_find(sender, types[t]);
            u8 pkt[DE__NET_PACKET_MAX];
            u32 n = sender->held_bytes[idx];
            memcpy(pkt, sender->held[idx], n);
            flip_all(r, sender, to, pkt, n);
        }
    }
    /* DISCONNECT from the client, last: the original must then disconnect. */
    ct->hold = true;
    ct->held_count = 0;
    de__net_client_disconnect(r->client[0], 3);
    ct->hold = false;
    idx = tap_find(ct, DE__NET_PACKET_DISCONNECT);
    u8 disc[DE__NET_PACKET_MAX];
    u32 disc_bytes = ct->held_bytes[idx];
    memcpy(disc, ct->held[idx], disc_bytes);
    /* The client is DISCONNECTING and sends its other two copies on its next updates; drop
     * them (so only the changed copies reach the server) and let it finish. */
    ct->drop_type = DE__NET_PACKET_DISCONNECT;
    ct->drop_left = 2;
    de__net_client_update(r->client[0]);
    de__net_client_update(r->client[0]);
    ASSERT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_DISCONNECTED);
    u32 sev_disconnected = r->sev.disconnected;
    flip_all(r, ct, &r->server_addr, disc, disc_bytes);
    EXPECT_EQ(de__net_server_connected_count(r->server), 1u);
    EXPECT_EQ(r->sev.disconnected, sev_disconnected);
    tap_inject(ct, &r->server_addr, disc, disc_bytes);
    de__net_server_update(r->server);
    EXPECT_EQ(de__net_server_connected_count(r->server), 0u);
    EXPECT_EQ(r->sev.disconnected, sev_disconnected + 1u);
    rig_destroy(r);
}

/* A DENIED changed on the way is dropped; the original ends the handshake. */
TEST(net, handshake_tamper_denied) {
    rig_opts o = {0};
    o.deny_direct = true;
    rig *r = rig_create(&o);
    tap *st = &r->server_tap;
    st->hold = true;
    ASSERT_EQ(de__net_client_connect(r->client[0], &r->server_addr, NULL), DE_OK);
    de__net_server_update(r->server);
    u32 idx = tap_find(st, DE__NET_PACKET_DENIED);
    u8 denied[DE__NET_PACKET_MAX];
    u32 n = st->held_bytes[idx];
    memcpy(denied, st->held[idx], n);
    EXPECT_EQ(n, DE__NET_DENIED_BYTES);
    st->hold = false;
    flip_all(r, st, &r->client_sock[0]->local, denied, n);
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_SENDING_REQUEST);
    tap_inject(st, &r->client_sock[0]->local, denied, n);
    de__net_client_update(r->client[0]);
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_DISCONNECTED);
    u8 detail = 0;
    EXPECT_EQ(de__net_client_end_reason(r->client[0], &detail), DE__NET_END_DENIED);
    EXPECT_EQ(detail, (u8)DE__NET_DENY_DIRECT_REFUSED);
    rig_destroy(r);
}

/* ---------- Replay ---------- */

TEST(net, handshake_replay) {
    rig_opts o = {0};
    rig *r = rig_create(&o);
    tap *ct = &r->client_tap[0];
    rig_connect(r, 0, 1);

    /* Five PAYLOADs held, then delivered out of order: each is accepted exactly once. */
    ct->hold = true;
    ct->held_count = 0;
    for (u8 i = 0; i < 5; ++i)
        ASSERT_EQ(de__net_client_send(r->client[0], &i, 1), DE_OK);
    ct->hold = false;
    ASSERT_EQ(ct->held_count, 5u);
    static const u32 order[5] = {3, 1, 4, 0, 2};
    for (u32 k = 0; k < 5; ++k) {
        tap_inject(ct, &r->server_addr, ct->held[order[k]], ct->held_bytes[order[k]]);
        de__net_server_update(r->server);
        EXPECT_EQ(r->sev.payloads, k + 1u);
        EXPECT_EQ(r->sev.last_payload[0], (u8)order[k]);
    }
    /* Replays: all dropped, counted, no payload. */
    u64 replay_before = de__net_server_stats(r->server)->dropped_replay;
    for (u32 k = 0; k < 5; ++k) {
        tap_inject(ct, &r->server_addr, ct->held[k], ct->held_bytes[k]);
        de__net_server_update(r->server);
    }
    EXPECT_EQ(r->sev.payloads, 5u);
    EXPECT_EQ(de__net_server_stats(r->server)->dropped_replay, replay_before + 5u);

    /* A packet that fell out of the window (more than 256 newer ones arrived) is dropped. */
    u8 old[DE__NET_PACKET_MAX];
    u32 old_bytes = ct->held_bytes[0];
    memcpy(old, ct->held[0], old_bytes);
    for (u32 i = 0; i < DE__NET_REPLAY_WINDOW + 1u; ++i) {
        u8 b = 7;
        ASSERT_EQ(de__net_client_send(r->client[0], &b, 1), DE_OK);
        if (i % 64u == 0)
            de__net_server_update(r->server);
    }
    de__net_server_update(r->server);
    u32 payloads = r->sev.payloads;
    tap_inject(ct, &r->server_addr, old, old_bytes);
    de__net_server_update(r->server);
    EXPECT_EQ(r->sev.payloads, payloads);
    EXPECT_EQ(de__net_server_stats(r->server)->dropped_replay, replay_before + 6u);

    /* Server to client too. */
    tap *st = &r->server_tap;
    st->hold = true;
    st->held_count = 0;
    u8 b = 42;
    ASSERT_EQ(de__net_server_send(r->server, de__net_client_slot(r->client[0]), &b, 1), DE_OK);
    st->hold = false;
    tap_inject(st, &r->client_sock[0]->local, st->held[0], st->held_bytes[0]);
    tap_inject(st, &r->client_sock[0]->local, st->held[0], st->held_bytes[0]);
    de__net_client_update(r->client[0]);
    EXPECT_EQ(r->cev[0].payloads, 1u);
    EXPECT_EQ(de__net_client_stats(r->client[0])->dropped_replay, 1u);
    rig_destroy(r);
}

/* A RESPONSE sent again (its ACCEPT was lost) gets the ACCEPT again; a cookie from an epoch
 * that rotated out is refused. */
TEST(net, handshake_lost_accept_and_old_cookie) {
    rig_opts o = {0};
    rig *r = rig_create(&o);
    tap *ct = &r->client_tap[0];
    tap *st = &r->server_tap;
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    make_token(r, 1, token);
    st->drop_type = DE__NET_PACKET_ACCEPT;
    st->drop_left = 2;
    ct->hold = false;
    ASSERT_EQ(de__net_client_connect(r->client[0], &r->server_addr, token), DE_OK);
    u32 steps = 0;
    while (de__net_client_get_state(r->client[0]) != DE__NET_CLIENT_CONNECTED && steps < 50) {
        rig_step(r, 60);
        steps++;
    }
    ASSERT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_CONNECTED);
    EXPECT_EQ(st->sent_type[DE__NET_PACKET_ACCEPT], 3u);
    EXPECT_EQ(ct->sent_type[DE__NET_PACKET_RESPONSE], 3u);
    EXPECT_EQ(r->sev.connected, 1u); /* one connection, not three */

    /* A cookie is bound to the address that sent the REQUEST, and to its epoch. */
    make_token(r, 2, token);
    ASSERT_EQ(de__net_client_connect(r->client[1], &r->server_addr, token), DE_OK);
    r->client_tap[1].hold = true;
    de__net_server_update(r->server);
    de__net_client_update(r->client[1]);
    u32 idx = tap_find(&r->client_tap[1], DE__NET_PACKET_RESPONSE);
    u8 resp[DE__NET_PACKET_MAX];
    u32 n = r->client_tap[1].held_bytes[idx];
    memcpy(resp, r->client_tap[1].held[idx], n);
    r->client_tap[1].hold = false;
    u64 cookie_before = de__net_server_stats(r->server)->dropped_cookie;
    tap_inject(&r->client_tap[2], &r->server_addr, resp, n); /* another address */
    de__net_server_update(r->server);
    EXPECT_EQ(de__net_server_stats(r->server)->dropped_cookie, cookie_before + 1u);
    r->clock.ns += 25000ull * NS_PER_MS;                     /* two epochs later */
    de__net_server_update(r->server);                        /* rotates */
    tap_inject(&r->client_tap[1], &r->server_addr, resp, n); /* the right address, too late */
    de__net_server_update(r->server);
    EXPECT_EQ(de__net_server_stats(r->server)->dropped_cookie, cookie_before + 2u);
    EXPECT_EQ(r->sev.connected, 1u);
    rig_destroy(r);
}

/* ---------- DENIED ---------- */

TEST(net, handshake_denied_full) {
    rig_opts o = {0};
    o.max_peers = 2;
    rig *r = rig_create(&o);
    rig_connect(r, 0, 1);
    rig_connect(r, 1, 2);
    EXPECT_EQ(de__net_server_connected_count(r->server), 2u);

    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    make_token(r, 3, token);
    ASSERT_EQ(de__net_client_connect(r->client[2], &r->server_addr, token), DE_OK);
    for (u32 i = 0; i < 3; ++i)
        rig_step(r, 1);
    EXPECT_EQ(de__net_client_get_state(r->client[2]), DE__NET_CLIENT_DISCONNECTED);
    u8 detail = 0;
    EXPECT_EQ(de__net_client_end_reason(r->client[2], &detail), DE__NET_END_DENIED);
    EXPECT_EQ(detail, (u8)DE__NET_DENY_SERVER_FULL);
    EXPECT_EQ(r->cev[2].disconnected, 1u);
    EXPECT_EQ(r->cev[2].last_reason, DE__NET_END_DENIED);
    EXPECT_EQ(de__net_server_connected_count(r->server), 2u);
    EXPECT_EQ(de__net_server_stats(r->server)->denied, 1u);

    /* A slot frees up: the next attempt gets in. */
    de__net_client_disconnect(r->client[0], 0);
    for (u32 i = 0; i < 3; ++i)
        rig_step(r, 1);
    EXPECT_EQ(de__net_server_connected_count(r->server), 1u);
    rig_connect(r, 2, 3);
    EXPECT_EQ(de__net_server_connected_count(r->server), 2u);
    rig_destroy(r);
}

static void expect_denied(rig *r, u32 i, const u8 *token, de__net_deny_reason want) {
    ASSERT_EQ(de__net_client_connect(r->client[i], &r->server_addr, token), DE_OK);
    for (u32 k = 0; k < 3; ++k)
        rig_step(r, 1);
    u8 detail = 0;
    EXPECT_EQ(de__net_client_get_state(r->client[i]), DE__NET_CLIENT_DISCONNECTED);
    EXPECT_EQ(de__net_client_end_reason(r->client[i], &detail), DE__NET_END_DENIED);
    EXPECT_EQ(detail, (u8)want);
}

TEST(net, handshake_denied_token) {
    rig_opts o = {0};
    o.deny_direct = true;
    rig *r = rig_create(&o);
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];

    /* Expired. */
    ASSERT_EQ(de_net_connect_token_generate(r->key, 1, NOW_UNIX - 1, 0, token), DE_OK);
    expect_denied(r, 0, token, DE__NET_DENY_TOKEN_EXPIRED);
    EXPECT_EQ(de__net_server_stats(r->server)->tokens_expired, 1u);
    /* Forged: another key. */
    u8 other[DE_NET_SERVER_KEY_BYTES];
    ASSERT_EQ(de_net_server_key_generate(other), DE_OK);
    ASSERT_EQ(de_net_connect_token_generate(other, 1, NOW_UNIX + 60, 0, token), DE_OK);
    expect_denied(r, 0, token, DE__NET_DENY_TOKEN_INVALID);
    EXPECT_EQ(de__net_server_stats(r->server)->tokens_invalid, 1u);
    /* Direct connect refused by this server. */
    expect_denied(r, 0, NULL, DE__NET_DENY_DIRECT_REFUSED);
    EXPECT_EQ(de__net_server_connected_count(r->server), 0u);

    /* The same client id twice. */
    rig_connect(r, 0, 9);
    make_token(r, 9, token);
    expect_denied(r, 1, token, DE__NET_DENY_ALREADY_CONNECTED);
    EXPECT_EQ(de__net_server_connected_count(r->server), 1u);

    /* A token that expires between CHALLENGE and RESPONSE is refused at RESPONSE. */
    ASSERT_EQ(de_net_connect_token_generate(r->key, 11, NOW_UNIX + 1, 0, token), DE_OK);
    ASSERT_EQ(de__net_client_connect(r->client[2], &r->server_addr, token), DE_OK);
    de__net_server_update(r->server);    /* CHALLENGE */
    r->clock.unix_s += 5;                /* now expired */
    de__net_client_update(r->client[2]); /* RESPONSE */
    de__net_server_update(r->server);    /* DENIED (context: the cookie) */
    de__net_client_update(r->client[2]);
    u8 detail = 0;
    EXPECT_EQ(de__net_client_end_reason(r->client[2], &detail), DE__NET_END_DENIED);
    EXPECT_EQ(detail, (u8)DE__NET_DENY_TOKEN_EXPIRED);
    rig_destroy(r);
}

/* ---------- Timeouts ---------- */

TEST(net, handshake_timeout) {
    rig_opts o = {0};
    rig *r = rig_create(&o);
    rig_connect(r, 0, 1);
    u32 slot = de__net_client_slot(r->client[0]);

    /* Both sides idle: keepalives (1 Hz) keep the connection for a minute. */
    for (u32 i = 0; i < 600; ++i)
        rig_step(r, 100);
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_CONNECTED);
    EXPECT_EQ(de__net_server_connected_count(r->server), 1u);
    EXPECT_GE(r->client_tap[0].sent_type[DE__NET_PACKET_KEEPALIVE], 50u);
    EXPECT_LE(r->client_tap[0].sent_type[DE__NET_PACKET_KEEPALIVE], 70u);
    EXPECT_GE(r->server_tap.sent_type[DE__NET_PACKET_KEEPALIVE], 50u);

    /* The client goes silent: the server times it out after exactly the idle time (10 s). */
    r->client_tap[0].drop_type = DE__NET_PACKET_KEEPALIVE;
    r->client_tap[0].drop_left = 1000000;
    u64 silent_from = r->clock.ns;
    u32 steps = 0;
    while (de__net_server_connected_count(r->server) == 1u && steps < 1000) {
        rig_step(r, 100);
        steps++;
    }
    EXPECT_EQ(de__net_server_connected_count(r->server), 0u);
    EXPECT_EQ(r->sev.timed_out, 1u);
    EXPECT_EQ(r->sev.last_slot, slot);
    EXPECT_EQ(r->sev.last_reason, DE__NET_END_TIMEOUT);
    EXPECT_EQ(r->sev.disconnected, 0u); /* timeouts are their own event */
    /* 10 s after the last KEEPALIVE that arrived, which was at most 1 s before the silence. */
    u64 waited_ms = (r->clock.ns - silent_from) / NS_PER_MS;
    EXPECT_GE(waited_ms, 9000u);
    EXPECT_LE(waited_ms, 10200u);

    /* The client hears nothing more either, and times out on its side. */
    steps = 0;
    while (de__net_client_get_state(r->client[0]) == DE__NET_CLIENT_CONNECTED && steps < 1000) {
        rig_step(r, 100);
        steps++;
    }
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_DISCONNECTED);
    EXPECT_EQ(r->cev[0].timed_out, 1u);
    EXPECT_EQ(de__net_client_end_reason(r->client[0], NULL), DE__NET_END_TIMEOUT);

    /* No server at all: REQUEST is resent every 100 ms, then the connect timeout (5 s). */
    de_net_address nobody = de_net_address_ipv4(127, 0, 0, 1, 45000);
    u32 before = r->client_tap[1].sent_type[DE__NET_PACKET_REQUEST];
    ASSERT_EQ(de__net_client_connect(r->client[1], &nobody, NULL), DE_OK);
    u64 start = r->clock.ns;
    steps = 0;
    while (de__net_client_get_state(r->client[1]) != DE__NET_CLIENT_DISCONNECTED && steps < 1000) {
        rig_step(r, 10);
        steps++;
    }
    EXPECT_EQ(de__net_client_end_reason(r->client[1], NULL), DE__NET_END_CONNECT_TIMEOUT);
    EXPECT_EQ(r->cev[1].timed_out, 1u);
    EXPECT_EQ(r->cev[1].last_reason, DE__NET_END_CONNECT_TIMEOUT);
    u64 took_ms = (r->clock.ns - start) / NS_PER_MS;
    EXPECT_GE(took_ms, 5000u);
    EXPECT_LE(took_ms, 5020u);
    u32 requests = r->client_tap[1].sent_type[DE__NET_PACKET_REQUEST] - before;
    EXPECT_GE(requests, 49u);
    EXPECT_LE(requests, 51u);
    rig_destroy(r);
}

/* ---------- No allocation while answering a REQUEST flood ---------- */

TEST(net, stateless_challenge_no_alloc) {
    rig_opts o = {0};
    o.max_peers = 4;
    rig *r = rig_create(&o);
    rig_connect(r, 0, 1); /* one real connection, to show the flood does not disturb it */
    /* Move to 0.6 s before an epoch boundary (keeping the connection alive), so the 1 s flood
     * below crosses a rotation of the challenge secrets. */
    while (r->clock.ns % DE__NET_EPOCH_NS < DE__NET_EPOCH_NS - 600ull * NS_PER_MS)
        rig_step(r, 100);
    ASSERT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_CONNECTED);

    /* Three attacker sockets on the same network. */
    de__socket *atk[3];
    for (u32 i = 0; i < 3; ++i)
        ASSERT_EQ(de__loopback_open(r->net, NULL, &atk[i]), DE_OK);
    /* A mix of REQUESTs: valid tokens (answered with CHALLENGE), forged and expired tokens and
     * direct connects (DENIED / CHALLENGE), wrong protocol id and garbage (dropped). */
    enum { KINDS = 6 };
    static u8 req[KINDS][DE__NET_REQUEST_BYTES];
    for (u32 k = 0; k < KINDS; ++k) {
        de__net_request q;
        memset(&q, 0, sizeof q);
        q.protocol_id = k == 4 ? PROTOCOL_ID + 1 : PROTOCOL_ID;
        for (u32 i = 0; i < 32; ++i)
            q.client_pk[i] = (u8)(k * 40u + i + 1u);
        u8 whole[DE_NET_CONNECT_TOKEN_BYTES]; /* only the sealed part goes on the wire */
        memset(whole, 0, sizeof whole);
        if (k == 0)
            make_token(r, 100, whole);
        else if (k == 1)
            memset(whole, 0x5A, sizeof whole);
        else if (k == 2)
            ASSERT_EQ(de_net_connect_token_generate(r->key, 5, NOW_UNIX - 10, 0, whole), DE_OK);
        memcpy(q.token, whole, sizeof q.token);
        /* k == 3: all-zero token (direct connect) */
        de__net_request_write(req[k], &q);
        if (k == 5)
            req[k][DE__NET_REQUEST_BYTES - 10u] = 0xFF; /* non-zero padding */
    }
    u8 sink[DE__NET_PACKET_MAX * 4];
    de__datagram drain[4];

    /* Warm-up outside the guarded region. */
    for (u32 k = 0; k < KINDS; ++k)
        ASSERT_EQ(de__socket_send_to(atk[0], &r->server_addr, req[k], DE__NET_REQUEST_BYTES, NULL),
                  DE_OK);
    de__net_server_update(r->server);

    const de__net_conn_stats before = *de__net_server_stats(r->server);
    enum { ROUNDS = 100 };
    de_alloc_guard g = {0};
    de_alloc_guard_begin(&g);
    for (u32 round = 0; round < ROUNDS; ++round) {
        for (u32 a = 0; a < 3; ++a)
            for (u32 k = 0; k < KINDS; ++k)
                (void)de__socket_send_to(atk[a], &r->server_addr, req[k], DE__NET_REQUEST_BYTES,
                                         NULL);
        de__net_server_update(r->server);
        /* Drain the answers so the attacker queues never fill. */
        for (u32 a = 0; a < 3; ++a) {
            u32 got = 0;
            do {
                for (u32 i = 0; i < 4; ++i) {
                    drain[i].data = sink + i * DE__NET_PACKET_MAX;
                    drain[i].capacity = DE__NET_PACKET_MAX;
                }
                got = 0;
                (void)de__socket_recv_from(atk[a], drain, 4, &got);
            } while (got == 4);
        }
        r->clock.ns += 10ull * NS_PER_MS;
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);

    const de__net_conn_stats *after = de__net_server_stats(r->server);
    const u64 per_kind = 3u * ROUNDS;
    EXPECT_EQ(after->requests - before.requests, per_kind * 4u); /* k = 0..3 parse */
    EXPECT_EQ(after->dropped_malformed - before.dropped_malformed, per_kind * 2u); /* k = 4, 5 */
    EXPECT_EQ(after->challenges_sent - before.challenges_sent, per_kind * 2u);     /* k = 0, 3 */
    EXPECT_EQ(after->denied - before.denied, per_kind * 2u);                       /* k = 1, 2 */
    /* Stateless: no slot was taken, and the real connection is untouched. */
    EXPECT_EQ(de__net_server_connected_count(r->server), 1u);
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_CONNECTED);
    for (u32 i = 0; i < 3; ++i)
        de__socket_close(atk[i]);
    rig_destroy(r);
}

/* ---------- Disconnect ---------- */

TEST(net, disconnect_redundant) {
    rig_opts o = {0};
    rig *r = rig_create(&o);
    rig_connect(r, 0, 1);
    rig_connect(r, 1, 2);

    /* Client disconnect: three DISCONNECTs, one per update. The first two are lost; the third
     * still reaches the server. */
    tap *ct = &r->client_tap[0];
    ct->drop_type = DE__NET_PACKET_DISCONNECT;
    ct->drop_left = 2;
    de__net_client_disconnect(r->client[0], 4);
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_DISCONNECTING);
    EXPECT_EQ(r->cev[0].disconnected, 1u);
    EXPECT_EQ(r->cev[0].last_reason, DE__NET_END_LOCAL);
    EXPECT_EQ(ct->sent_type[DE__NET_PACKET_DISCONNECT], 1u);
    de__net_server_update(r->server);
    EXPECT_EQ(r->sev.disconnected, 0u);
    de__net_client_update(r->client[0]);
    EXPECT_EQ(ct->sent_type[DE__NET_PACKET_DISCONNECT], 2u);
    de__net_client_update(r->client[0]);
    EXPECT_EQ(ct->sent_type[DE__NET_PACKET_DISCONNECT], 3u);
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_DISCONNECTED);
    de__net_client_update(r->client[0]);
    EXPECT_EQ(ct->sent_type[DE__NET_PACKET_DISCONNECT], 3u); /* exactly three */
    de__net_server_update(r->server);
    EXPECT_EQ(r->sev.disconnected, 1u);
    EXPECT_EQ(r->sev.last_reason, DE__NET_END_REMOTE);
    EXPECT_EQ(r->sev.last_detail, 4);
    EXPECT_EQ(de__net_server_connected_count(r->server), 1u);

    /* Server kick: same, and all three copies arrive; the client ends once, later copies are
     * dropped as unexpected. */
    u32 slot = de__net_client_slot(r->client[1]);
    de__net_server_kick(r->server, slot, 6);
    EXPECT_EQ(r->sev.disconnected, 2u);
    EXPECT_EQ(r->sev.last_reason, DE__NET_END_LOCAL);
    EXPECT_EQ(de__net_server_slot(r->server, slot).state, DE__NET_SLOT_DISCONNECTING);
    EXPECT_EQ(de__net_server_connected_count(r->server), 0u);
    de__net_server_update(r->server);
    de__net_server_update(r->server);
    EXPECT_EQ(r->server_tap.sent_type[DE__NET_PACKET_DISCONNECT], 3u);
    EXPECT_EQ(de__net_server_slot(r->server, slot).state, DE__NET_SLOT_FREE);
    de__net_client_update(r->client[1]);
    EXPECT_EQ(de__net_client_get_state(r->client[1]), DE__NET_CLIENT_DISCONNECTED);
    EXPECT_EQ(r->cev[1].disconnected, 1u);
    EXPECT_EQ(r->cev[1].last_reason, DE__NET_END_REMOTE);
    EXPECT_EQ(r->cev[1].last_detail, 6);
    EXPECT_EQ(de__net_client_stats(r->client[1])->dropped_unexpected, 2u);

    /* The slot is reused with a new generation. */
    rig_connect(r, 1, 2);
    EXPECT_EQ(de__net_server_slot(r->server, de__net_client_slot(r->client[1])).generation, 2u);

    /* Destroy sends all three at once. */
    u32 before = r->client_tap[1].sent_type[DE__NET_PACKET_DISCONNECT];
    de__net_client_destroy(r->client[1]);
    r->client[1] = NULL;
    EXPECT_EQ(r->client_tap[1].sent_type[DE__NET_PACKET_DISCONNECT], before + 3u);
    de__net_server_update(r->server);
    EXPECT_EQ(de__net_server_connected_count(r->server), 0u);
    rig_destroy(r);
}

/* ---------- Key wiping ---------- */

/* An allocator that, before it frees a block, looks for known secrets in it. */
typedef struct scan_alloc {
    de_allocator a;
    const de_allocator *backing;
    u8 needles[8][32];
    u32 needle_count;
    u32 found;
    u32 frees;
    u32 live;
} scan_alloc;

static void *scan_alloc_fn(void *user, size_t size, size_t align, u32 tag) {
    scan_alloc *s = (scan_alloc *)user;
    s->live++;
    return DE_ALLOC(s->backing, size, align, tag);
}

static void *scan_realloc_fn(void *user, void *ptr, size_t old_size, size_t new_size, size_t align,
                             u32 tag) {
    scan_alloc *s = (scan_alloc *)user;
    return DE_REALLOC(s->backing, ptr, old_size, new_size, align, tag);
}

static void scan_free_fn(void *user, void *ptr, size_t size) {
    scan_alloc *s = (scan_alloc *)user;
    const u8 *p = (const u8 *)ptr;
    for (u32 n = 0; n < s->needle_count; ++n)
        for (size_t i = 0; i + 32 <= size; ++i)
            if (memcmp(p + i, s->needles[n], 32) == 0)
                s->found++;
    s->frees++;
    s->live--;
    DE_FREE(s->backing, ptr, size);
}

TEST(net, handshake_key_wipe) {
    scan_alloc sa;
    memset(&sa, 0, sizeof sa);
    sa.backing = de_heap_allocator();
    sa.a.alloc = scan_alloc_fn;
    sa.a.realloc = scan_realloc_fn;
    sa.a.free = scan_free_fn;
    sa.a.user = &sa;

    fake_clock clock = {5000ull * 1000000000ull, NOW_UNIX};
    de__loopback_net *net = NULL;
    ASSERT_EQ(de__loopback_net_create(NULL, NULL, &net), DE_OK);
    de__socket *ss = NULL, *cs = NULL;
    de_net_address saddr = de_net_address_ipv4(127, 0, 0, 1, 41000);
    ASSERT_EQ(de__loopback_open(net, &saddr, &ss), DE_OK);
    ASSERT_EQ(de__loopback_open(net, NULL, &cs), DE_OK);
    u8 key[DE_NET_SERVER_KEY_BYTES];
    ASSERT_EQ(de_net_server_key_generate(key), DE_OK);

    de__net_server_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.allocator = &sa.a;
    sd.socket = ss;
    sd.protocol_id = PROTOCOL_ID;
    sd.max_peers = 2;
    memcpy(sd.server_key, key, sizeof key);
    sd.clock = clock_of(&clock);
    de__net_server *server = NULL;
    ASSERT_EQ(de__net_server_create(&sd, &server), DE_OK);
    de__net_client_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.allocator = &sa.a;
    cd.socket = cs;
    cd.protocol_id = PROTOCOL_ID;
    cd.clock = clock_of(&clock);
    de__net_client *client = NULL;
    ASSERT_EQ(de__net_client_create(&cd, &client), DE_OK);
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    ASSERT_EQ(de_net_connect_token_generate(key, 1, NOW_UNIX + 60, 0, token), DE_OK);
    ASSERT_EQ(de__net_client_connect(client, &saddr, token), DE_OK);
    for (u32 i = 0; i < 3; ++i) {
        de__net_server_update(server);
        de__net_client_update(client);
    }
    ASSERT_EQ(de__net_client_get_state(client), DE__NET_CLIENT_CONNECTED);
    const u8 secret_msg[32] = "a payload that must not linger";
    ASSERT_EQ(de__net_client_send(client, secret_msg, sizeof secret_msg), DE_OK);
    de__net_server_update(server);

    /* Disconnect wipes the client's keys at once (a struct inspection). */
    const de__net_keys *ck = de__net_client_keys(client);
    memcpy(sa.needles[0], ck->c2s, 32);
    memcpy(sa.needles[1], ck->s2c, 32);
    memcpy(sa.needles[2], key, 32);
    memcpy(sa.needles[3], secret_msg, 32);
    memcpy(sa.needles[4], token + DE_NET_CONNECT_TOKEN_SEALED_BYTES, 32); /* connection key */
    sa.needle_count = 5;
    de__net_client_disconnect(client, 0);
    de__net_client_update(client);
    de__net_client_update(client);
    ASSERT_EQ(de__net_client_get_state(client), DE__NET_CLIENT_DISCONNECTED);
    for (u32 i = 0; i < 32; ++i)
        EXPECT_EQ(ck->c2s[i] | ck->s2c[i], 0);
    de__net_server_update(server);
    const de__net_keys *sk = de__net_server_slot_keys(server, 0);
    for (u32 i = 0; i < 32; ++i)
        EXPECT_EQ(sk->c2s[i] | sk->s2c[i], 0);

    /* Connect again, then destroy both while connected: the freed memory holds no key, no server
     * key and no received plaintext. */
    ASSERT_EQ(de__net_client_connect(client, &saddr, token), DE_OK);
    for (u32 i = 0; i < 3; ++i) {
        de__net_server_update(server);
        de__net_client_update(client);
    }
    ASSERT_EQ(de__net_client_get_state(client), DE__NET_CLIENT_CONNECTED);
    ASSERT_EQ(de__net_client_send(client, secret_msg, sizeof secret_msg), DE_OK);
    de__net_server_update(server);
    memcpy(sa.needles[5], ck->c2s, 32);
    memcpy(sa.needles[6], ck->s2c, 32);
    sa.needle_count = 7;
    de__net_client_destroy(client);
    de__net_server_destroy(server);
    EXPECT_EQ(sa.frees, 2u);
    EXPECT_EQ(sa.live, 0u);
    EXPECT_EQ(sa.found, 0u);
    de__socket_close(cs);
    de__socket_close(ss);
    de__loopback_net_destroy(net);
}

/* ---------- Plaintext sessions (Dev/Debug) ---------- */

TEST(net, handshake_plaintext_mode) {
    rig_opts o = {0};
    o.server_plaintext = true;
    o.client_plaintext = true;
    rig *r = rig_create(&o);
    rig_connect(r, 0, 1);
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
    const bool expect_plain = true;
#else
    const bool expect_plain = false; /* Release and Shipping always encrypt */
#endif
    EXPECT_EQ(de__net_client_plaintext(r->client[0]), expect_plain);
    tap *ct = &r->client_tap[0];
    ct->hold = true;
    ct->held_count = 0;
    const char msg[] = "readable in a capture";
    ASSERT_EQ(de__net_client_send(r->client[0], msg, sizeof msg), DE_OK);
    ct->hold = false;
    bool visible = false;
    for (u32 i = 0; i + sizeof msg <= ct->held_bytes[0]; ++i)
        visible = visible || memcmp(ct->held[0] + i, msg, sizeof msg) == 0;
    EXPECT_EQ(visible, expect_plain);
    /* Still authenticated: a changed byte is dropped. */
    flip_all(r, ct, &r->server_addr, ct->held[0], ct->held_bytes[0]);
    tap_inject(ct, &r->server_addr, ct->held[0], ct->held_bytes[0]);
    de__net_server_update(r->server);
    EXPECT_EQ(r->sev.payloads, 1u);
    EXPECT_MEMEQ(r->sev.last_payload, msg, sizeof msg);

    /* Only one side asking: encrypted. */
    rig_destroy(r);
    o.server_plaintext = false;
    r = rig_create(&o);
    rig_connect(r, 0, 1);
    EXPECT_FALSE(de__net_client_plaintext(r->client[0]));
    EXPECT_FALSE(de__net_server_slot(r->server, de__net_client_slot(r->client[0])).plaintext);
    rig_destroy(r);
}

/* ---------- Server authentication (DENG-378) ---------- */

/* The connection key mixed into the KDF: both ends agree only when they use the same key; without
 * one it is the DENG-52 derivation. */
TEST(net, kdf_connection_key) {
    u8 cs[32], cp[32], ss[32], sp[32], psk[32], psk2[32];
    ASSERT_EQ(de__net_x25519_keypair(cs, cp), DE_OK);
    ASSERT_EQ(de__net_x25519_keypair(ss, sp), DE_OK);
    ASSERT_EQ(de__net_random(psk, sizeof psk), DE_OK);
    memcpy(psk2, psk, sizeof psk2);
    psk2[31] ^= 1u;
    de__net_keys client, server, other, plain;
    ASSERT_TRUE(de__net_derive_keys_psk(&client, cs, sp, 77, cp, sp, psk));
    ASSERT_TRUE(de__net_derive_keys_psk(&server, ss, cp, 77, cp, sp, psk));
    EXPECT_MEMEQ(client.c2s, server.c2s, 32);
    EXPECT_MEMEQ(client.s2c, server.s2c, 32);
    EXPECT_NE(memcmp(client.c2s, client.s2c, 32), 0);
    /* One bit of the key changes both keys. */
    ASSERT_TRUE(de__net_derive_keys_psk(&other, ss, cp, 77, cp, sp, psk2));
    EXPECT_NE(memcmp(client.c2s, other.c2s, 32), 0);
    EXPECT_NE(memcmp(client.s2c, other.s2c, 32), 0);
    /* No key: the DENG-52 derivation, and not the keyed one. */
    ASSERT_TRUE(de__net_derive_keys_psk(&other, ss, cp, 77, cp, sp, NULL));
    ASSERT_TRUE(de__net_derive_keys(&plain, ss, cp, 77, cp, sp));
    EXPECT_MEMEQ(other.c2s, plain.c2s, 32);
    EXPECT_MEMEQ(other.s2c, plain.s2c, 32);
    EXPECT_NE(memcmp(client.c2s, plain.c2s, 32), 0);
    /* A man in the middle runs X25519 with the client itself; without the key its keys differ. */
    u8 ms[32], mp[32];
    ASSERT_EQ(de__net_x25519_keypair(ms, mp), DE_OK);
    de__net_keys victim, mitm;
    ASSERT_TRUE(de__net_derive_keys_psk(&victim, cs, mp, 77, cp, mp, psk));
    ASSERT_TRUE(de__net_derive_keys_psk(&mitm, ms, cp, 77, cp, mp, NULL));
    EXPECT_NE(memcmp(victim.s2c, mitm.s2c, 32), 0);
    EXPECT_NE(memcmp(victim.c2s, mitm.c2s, 32), 0);
    /* A low-order point is still refused, key or not. */
    static const u8 zero[32] = {0};
    EXPECT_FALSE(de__net_derive_keys_psk(&other, cs, zero, 77, cp, zero, psk));
    for (u32 i = 0; i < 32; ++i)
        EXPECT_EQ(other.c2s[i] | other.s2c[i], 0);
}

/* What a host on the path can send as the server's answer to `request`: a CHALLENGE from its own
 * X25519 key, bound to the REQUEST it saw, with the keys it can derive (`psk`: the key it uses,
 * NULL for none). Returns the length; `keys` gets its keys. */
static u32 mitm_challenge(const u8 *request, u32 request_bytes, const u8 *psk, u8 *out,
                          de__net_keys *keys) {
    de__net_request req;
    ASSERT_TRUE(de__net_request_read(request, request_bytes, &req));
    u8 digest[DE__NET_KEY_BYTES];
    de__net_request_digest(digest, request);
    u8 sk[DE__NET_KEY_BYTES], pk[DE__NET_KEY_BYTES];
    ASSERT_EQ(de__net_x25519_keypair(sk, pk), DE_OK);
    ASSERT_TRUE(
        de__net_derive_keys_psk(keys, sk, req.client_pk, PROTOCOL_ID, req.client_pk, pk, psk));
    u8 body[8 + DE__NET_KEY_BYTES];
    de__net_put_u64(body, 1);   /* an epoch ... */
    memset(body + 8, 0xC0, 32); /* ... and a cookie of its choice */
    u32 n = de__net_packet_seal(out, DE__NET_PACKET_MAX, DE__NET_PACKET_CHALLENGE, PROTOCOL_ID, 0,
                                pk, digest, body, sizeof body, keys->s2c, DE__NET_DIR_S2C,
                                DE__NET_DOMAIN_CHALLENGE, false);
    ASSERT_NE(n, 0u);
    return n;
}

static bool all_zero(const u8 *p, u32 n) {
    u8 any = 0;
    for (u32 i = 0; i < n; ++i)
        any |= p[i];
    return any == 0;
}

/* A man in the middle answers the client's REQUEST as the server: the client drops every answer,
 * holds no keys, and still connects to the real server. An impostor server (its own server key)
 * can only refuse the token. */
TEST(net, handshake_mitm_refused) {
    rig_opts o = {0};
    rig *r = rig_create(&o);
    tap *ct = &r->client_tap[0];
    tap *st = &r->server_tap;
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    make_token(r, 1, token);
    ct->hold = true;
    ASSERT_EQ(de__net_client_connect(r->client[0], &r->server_addr, token), DE_OK);
    u32 idx = tap_find(ct, DE__NET_PACKET_REQUEST);
    u8 request[DE__NET_PACKET_MAX];
    u32 request_bytes = ct->held_bytes[idx];
    memcpy(request, ct->held[idx], request_bytes);
    ct->hold = false;
    /* The token on the wire is the sealed part only: the connection key is not in the REQUEST. */
    EXPECT_MEMEQ(request + 42, token, DE_NET_CONNECT_TOKEN_SEALED_BYTES);
    for (u32 i = 0; i + 32 <= request_bytes; ++i)
        EXPECT_NE(memcmp(request + i, token + DE_NET_CONNECT_TOKEN_SEALED_BYTES, 32), 0);

    /* The MITM's answers, sent from the server's address (it is on the path): with no key (the
     * DENG-52 handshake, which this attack completed) and with guessed keys. */
    u8 guess[DE__NET_KEY_BYTES];
    memset(guess, 0, sizeof guess);
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        if (attempt == 2)
            ASSERT_EQ(de__net_random(guess, sizeof guess), DE_OK);
        u8 pkt[DE__NET_PACKET_MAX];
        de__net_keys mk;
        u32 n = mitm_challenge(request, request_bytes, attempt == 0 ? NULL : guess, pkt, &mk);
        u64 auth_before = de__net_client_stats(r->client[0])->dropped_auth;
        tap_inject(st, &r->client_sock[0]->local, pkt, n);
        de__net_client_update(r->client[0]);
        EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_SENDING_REQUEST);
        EXPECT_EQ(de__net_client_stats(r->client[0])->dropped_auth, auth_before + 1u);
        /* No shared keys: the client kept nothing it derived. */
        const de__net_keys *ck = de__net_client_keys(r->client[0]);
        EXPECT_TRUE(all_zero(ck->c2s, 32) && all_zero(ck->s2c, 32));
    }
    EXPECT_EQ(r->cev[0].connected, 0u);
    EXPECT_EQ(ct->sent_type[DE__NET_PACKET_RESPONSE], 0u);

    /* The real server still connects: the REQUEST reaches it, the handshake completes. */
    tap_inject(ct, &r->server_addr, request, request_bytes);
    u32 steps = 0;
    while (de__net_client_get_state(r->client[0]) != DE__NET_CLIENT_CONNECTED && steps < 20) {
        rig_step(r, 1);
        steps++;
    }
    ASSERT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_CONNECTED);
    const de__net_keys *ck = de__net_client_keys(r->client[0]);
    const de__net_keys *sk = de__net_server_slot_keys(r->server, de__net_client_slot(r->client[0]));
    EXPECT_MEMEQ(ck->c2s, sk->c2s, 32);
    EXPECT_MEMEQ(ck->s2c, sk->s2c, 32);
    EXPECT_EQ(de__net_server_slot(r->server, de__net_client_slot(r->client[0])).client_id, 1u);

    /* An impostor server at another address (the client was routed there) with its own server
     * key: it cannot read the token, so it can only refuse it. */
    de_net_address fake_addr = de_net_address_ipv4(127, 0, 0, 1, 40001);
    de__socket *fake_sock = NULL;
    ASSERT_EQ(de__loopback_open(r->net, &fake_addr, &fake_sock), DE_OK);
    de__net_server_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.socket = fake_sock;
    fd.protocol_id = PROTOCOL_ID;
    fd.max_peers = 4;
    ASSERT_EQ(de_net_server_key_generate(fd.server_key), DE_OK);
    fd.clock = clock_of(&r->clock);
    de__net_server *fake = NULL;
    ASSERT_EQ(de__net_server_create(&fd, &fake), DE_OK);
    make_token(r, 2, token);
    ASSERT_EQ(de__net_client_connect(r->client[1], &fake_addr, token), DE_OK);
    for (u32 i = 0; i < 4; ++i) {
        de__net_server_update(fake);
        rig_step(r, 1);
    }
    EXPECT_EQ(de__net_client_get_state(r->client[1]), DE__NET_CLIENT_DISCONNECTED);
    u8 detail = 0;
    EXPECT_EQ(de__net_client_end_reason(r->client[1], &detail), DE__NET_END_DENIED);
    EXPECT_EQ(detail, (u8)DE__NET_DENY_TOKEN_INVALID);
    EXPECT_EQ(de__net_server_connected_count(fake), 0u);
    const de__net_keys *k1 = de__net_client_keys(r->client[1]);
    EXPECT_TRUE(all_zero(k1->c2s, 32) && all_zero(k1->s2c, 32));
    de__net_server_destroy(fake);
    de__socket_close(fake_sock);
    rig_destroy(r);
}

/* A token captured on the wire is useless without the connection key: the thief never gets a
 * connection, and the real client still connects with it. */
TEST(net, handshake_stolen_token) {
    rig_opts o = {0};
    rig *r = rig_create(&o);
    tap *ct = &r->client_tap[0];
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    make_token(r, 7, token);

    /* The real client's REQUEST and RESPONSE, captured. */
    ct->hold = true;
    ASSERT_EQ(de__net_client_connect(r->client[0], &r->server_addr, token), DE_OK);
    u32 idx = tap_find(ct, DE__NET_PACKET_REQUEST);
    tap_inject(ct, &r->server_addr, ct->held[idx], ct->held_bytes[idx]);
    de__net_server_update(r->server);
    de__net_client_update(r->client[0]);
    ASSERT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_SENDING_RESPONSE);
    idx = tap_find(ct, DE__NET_PACKET_RESPONSE);
    u8 response[DE__NET_PACKET_MAX];
    u32 response_bytes = ct->held_bytes[idx];
    memcpy(response, ct->held[idx], response_bytes);
    de__net_client_disconnect(r->client[0], 0); /* the real client waits */
    ct->hold = false;

    /* The thief has the sealed part (it is on the wire) and guesses the key. A token without a
     * key is refused at once. */
    u8 stolen[DE_NET_CONNECT_TOKEN_BYTES];
    memcpy(stolen, token, DE_NET_CONNECT_TOKEN_SEALED_BYTES);
    memset(stolen + DE_NET_CONNECT_TOKEN_SEALED_BYTES, 0, DE_NET_CONNECT_TOKEN_KEY_BYTES);
    EXPECT_EQ(de__net_client_connect(r->client[1], &r->server_addr, stolen),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__net_client_get_state(r->client[1]), DE__NET_CLIENT_DISCONNECTED);
    ASSERT_EQ(
        de__net_random(stolen + DE_NET_CONNECT_TOKEN_SEALED_BYTES, DE_NET_CONNECT_TOKEN_KEY_BYTES),
        DE_OK);
    ASSERT_EQ(de__net_client_connect(r->client[1], &r->server_addr, stolen), DE_OK);
    u32 steps = 0;
    while (de__net_client_get_state(r->client[1]) != DE__NET_CLIENT_DISCONNECTED && steps < 200) {
        rig_step(r, 50);
        steps++;
    }
    /* The server answered the REQUESTs (the token is valid) but the thief could open none of the
     * CHALLENGEs, so it never sent a RESPONSE and timed out. */
    EXPECT_EQ(de__net_client_end_reason(r->client[1], NULL), DE__NET_END_CONNECT_TIMEOUT);
    EXPECT_GE(de__net_client_stats(r->client[1])->dropped_auth, 1u);
    EXPECT_EQ(r->client_tap[1].sent_type[DE__NET_PACKET_RESPONSE], 0u);
    EXPECT_EQ(r->sev.connected, 0u);
    EXPECT_EQ(de__net_server_connected_count(r->server), 0u);

    /* The captured RESPONSE (it carries the token, a cookie and the client's key) does not help
     * either: from the thief's address the cookie is wrong. */
    u64 cookie_before = de__net_server_stats(r->server)->dropped_cookie;
    tap_inject(&r->client_tap[1], &r->server_addr, response, response_bytes);
    de__net_server_update(r->server);
    EXPECT_EQ(de__net_server_stats(r->server)->dropped_cookie, cookie_before + 1u);
    EXPECT_EQ(de__net_server_connected_count(r->server), 0u);

    /* The real client connects with the same token. */
    ASSERT_EQ(de__net_client_connect(r->client[0], &r->server_addr, token), DE_OK);
    steps = 0;
    while (de__net_client_get_state(r->client[0]) != DE__NET_CLIENT_CONNECTED && steps < 20) {
        rig_step(r, 1);
        steps++;
    }
    ASSERT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_CONNECTED);
    EXPECT_EQ(de__net_server_slot(r->server, de__net_client_slot(r->client[0])).client_id, 7u);
    rig_destroy(r);
}

/* A RESPONSE captured on the wire and sent again from the client's address after its connection
 * ended: before DENG-378 it made a ghost slot (and blocked the client id) until the idle
 * timeout. */
TEST(net, handshake_response_replay) {
    rig_opts o = {0};
    rig *r = rig_create(&o);
    tap *ct = &r->client_tap[0];
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    make_token(r, 3, token);
    ASSERT_EQ(de__net_client_connect(r->client[0], &r->server_addr, token), DE_OK);
    de__net_server_update(r->server); /* CHALLENGE */
    ct->hold = true;
    de__net_client_update(r->client[0]); /* RESPONSE, held */
    ct->hold = false;
    u32 idx = tap_find(ct, DE__NET_PACKET_RESPONSE);
    u8 response[DE__NET_PACKET_MAX];
    u32 response_bytes = ct->held_bytes[idx];
    memcpy(response, ct->held[idx], response_bytes);
    tap_inject(ct, &r->server_addr, response, response_bytes);
    de__net_server_update(r->server);
    de__net_client_update(r->client[0]);
    ASSERT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_CONNECTED);
    EXPECT_EQ(r->sev.connected, 1u);

    /* The client leaves; the RESPONSE comes again, inside its epoch, from the same address. */
    de__net_client_disconnect(r->client[0], 0);
    for (u32 i = 0; i < 3; ++i)
        rig_step(r, 1);
    ASSERT_EQ(de__net_server_connected_count(r->server), 0u);
    u64 replay_before = de__net_server_stats(r->server)->dropped_replay;
    tap_inject(ct, &r->server_addr, response, response_bytes);
    de__net_server_update(r->server);
    EXPECT_EQ(de__net_server_connected_count(r->server), 0u);
    EXPECT_EQ(r->sev.connected, 1u);
    EXPECT_EQ(de__net_server_stats(r->server)->dropped_replay, replay_before + 1u);

    /* So the client id is free: the client comes back at once. */
    rig_connect(r, 0, 3);
    EXPECT_EQ(r->sev.connected, 2u);
    rig_destroy(r);
}

/* What a direct connect (all-zero token) still lacks: no connection key, so a host on the path
 * that answers the REQUEST itself completes the key exchange with the client. This test pins the
 * documented gap (Trama "Decision: Connect tokens carry a connection key that authenticates both
 * ends"); a change that closes it changes this test. */
TEST(net, handshake_direct_connect_unauthenticated) {
    rig_opts o = {0};
    rig *r = rig_create(&o);
    tap *ct = &r->client_tap[0];
    ct->hold = true;
    ASSERT_EQ(de__net_client_connect(r->client[0], &r->server_addr, NULL), DE_OK);
    u32 idx = tap_find(ct, DE__NET_PACKET_REQUEST);
    u8 request[DE__NET_PACKET_MAX];
    u32 request_bytes = ct->held_bytes[idx];
    memcpy(request, ct->held[idx], request_bytes);
    ct->hold = false;
    u8 pkt[DE__NET_PACKET_MAX];
    de__net_keys mk;
    u32 n = mitm_challenge(request, request_bytes, NULL, pkt, &mk);
    tap_inject(&r->server_tap, &r->client_sock[0]->local, pkt, n);
    de__net_client_update(r->client[0]);
    EXPECT_EQ(de__net_client_get_state(r->client[0]), DE__NET_CLIENT_SENDING_RESPONSE);
    const de__net_keys *ck = de__net_client_keys(r->client[0]);
    EXPECT_MEMEQ(ck->c2s, mk.c2s, 32);
    EXPECT_MEMEQ(ck->s2c, mk.s2c, 32);
    /* A direct connect to the real server still works (DENG-2 Q15). */
    de__net_client_disconnect(r->client[0], 0);
    ASSERT_EQ(de__net_client_connect(r->client[1], &r->server_addr, NULL), DE_OK);
    for (u32 i = 0; i < 4; ++i)
        rig_step(r, 1);
    EXPECT_EQ(de__net_client_get_state(r->client[1]), DE__NET_CLIENT_CONNECTED);
    rig_destroy(r);
}
