/* Tests for the transport sockets (DENG-51): OS UDP on 127.0.0.1 and ::1, dual stack,
 * non-blocking receive, batched receive, truncation, errors, no heap in send/recv, and the
 * in-process loopback backend.
 *
 * IPv6 tests skip when the host has no IPv6 loopback (some CI containers). UDP on the loopback
 * interface is not lost or reordered in practice; the receive helpers still poll with a deadline
 * instead of assuming the datagram is there the moment send returns. */
#include "de_test.h"

#include "de_alloc_guard.h"

#include <dengine/de_net.h>
#include <dengine/de_platform.h>

#include "net/net_internal.h"

#include <stdio.h>
#include <string.h>

#define NS_PER_MS 1000000ull

/* ---------- helpers ---------- */

/* Receives into msgs until `want` datagrams arrived in total or `timeout_ms` passed. */
static u32 recv_wait(de__socket *s, de__datagram *msgs, u32 count, u32 want, u32 timeout_ms) {
    u64 deadline = de_time_now_ns() + (u64)timeout_ms * NS_PER_MS;
    u32 total = 0;
    while (total < want) {
        u32 got = 0;
        ASSERT_EQ(de__socket_recv_from(s, msgs + total, count - total, &got), DE_OK);
        total += got;
        if (total >= want || de_time_now_ns() > deadline)
            break;
    }
    return total;
}

static de_result open_udp(const char *addr, de__socket **out) {
    de_net_address a;
    ASSERT_EQ(de_net_address_parse(addr, &a), DE_OK);
    return de__udp_open(&a, NULL, NULL, out);
}

/* Opens [::1]:0, or skips the test when the host has no IPv6 loopback. */
static de__socket *open_v6_or_skip(const char *addr) {
    de__socket *s = NULL;
    de_result r = open_udp(addr, &s);
    if (r == DE_ERR_UNSUPPORTED || r == DE_ERR_NOT_FOUND)
        SKIP("no IPv6 on this host");
    ASSERT_EQ(r, DE_OK);
    return s;
}

/* One datagram a -> b, checked on arrival (payload, sender). */
static void roundtrip_one(de__socket *a, de__socket *b, const de_net_address *b_addr,
                          const char *text) {
    u32 sent = 0;
    u32 len = (u32)strlen(text);
    ASSERT_EQ(de__socket_send_to(a, b_addr, text, len, &sent), DE_OK);
    ASSERT_EQ(sent, len);
    char buf[256];
    de__datagram m = {buf, (u32)sizeof buf, 0, {0}};
    ASSERT_EQ(recv_wait(b, &m, 1, 1, 2000), 1u);
    ASSERT_EQ(m.bytes, len);
    EXPECT_EQ(memcmp(buf, text, len), 0);
    EXPECT_EQ(m.from.port, a->local.port);
}

/* ---------- UDP ---------- */

TEST(net, udp_v4_roundtrip) {
    de__socket *a = NULL, *b = NULL;
    ASSERT_EQ(open_udp("127.0.0.1:0", &a), DE_OK);
    ASSERT_EQ(open_udp("127.0.0.1:0", &b), DE_OK);
    EXPECT_EQ(a->local.family, (u16)DE_NET_FAMILY_IPV4);
    EXPECT_NE(a->local.port, 0); /* ephemeral port reported */
    EXPECT_NE(a->local.port, b->local.port);

    roundtrip_one(a, b, &b->local, "hello");
    roundtrip_one(b, a, &a->local, "world, back");

    /* The sender address a receiver sees is the one to answer: equal to a's bound address. */
    u32 sent = 0;
    ASSERT_EQ(de__socket_send_to(a, &b->local, "x", 1, &sent), DE_OK);
    char buf[16];
    de__datagram m = {buf, (u32)sizeof buf, 0, {0}};
    ASSERT_EQ(recv_wait(b, &m, 1, 1, 2000), 1u);
    EXPECT_TRUE(de_net_address_equal(&m.from, &a->local));

    /* Batched receive: 100 datagrams in arrival order, more than one recvmmsg batch. */
    enum { N = 100 };
    for (u32 i = 0; i < N; ++i) {
        u32 v = i * 2654435761u;
        ASSERT_EQ(de__socket_send_to(a, &b->local, &v, sizeof v, &sent), DE_OK);
        ASSERT_EQ(sent, (u32)sizeof v);
    }
    static u32 slots[N + 28][4];
    de__datagram msgs[N + 28];
    for (u32 i = 0; i < N + 28; ++i)
        msgs[i] = (de__datagram){slots[i], (u32)sizeof slots[i], 0, {0}};
    ASSERT_EQ(recv_wait(b, msgs, N + 28, N, 2000), (u32)N);
    for (u32 i = 0; i < N; ++i) {
        ASSERT_EQ(msgs[i].bytes, 4u);
        u32 v;
        memcpy(&v, msgs[i].data, 4);
        ASSERT_EQ(v, i * 2654435761u);
    }
    EXPECT_EQ(a->stats.datagrams_sent, (u64)(N + 2));
    EXPECT_EQ(b->stats.datagrams_received, (u64)(N + 2));
    EXPECT_EQ(b->stats.bytes_received, (u64)(5 + 1 + 4 * N));

    de__udp_info info = de__udp_get_info(a);
    EXPECT_TRUE(info.dont_fragment);
    EXPECT_FALSE(info.dual_stack);
    EXPECT_GT(info.recv_buffer_bytes, 0u);
    EXPECT_GT(info.send_buffer_bytes, 0u);
    de__socket_close(a);
    de__socket_close(b);
}

TEST(net, udp_v6_roundtrip) {
    de__socket *a = open_v6_or_skip("[::1]:0");
    de__socket *b = NULL;
    ASSERT_EQ(open_udp("[::1]:0", &b), DE_OK);
    EXPECT_EQ(a->local.family, (u16)DE_NET_FAMILY_IPV6);
    EXPECT_NE(a->local.port, 0);
    roundtrip_one(a, b, &b->local, "hello v6");
    roundtrip_one(b, a, &a->local, "and back");
    de__udp_info info = de__udp_get_info(a);
    EXPECT_TRUE(info.dont_fragment);
    /* An IPv6 socket cannot take an IPv4 destination when it is v6-only. */
    de__udp_desc only = {0};
    only.v6_only = true;
    de__socket *c = NULL;
    de_net_address lo6 = de_net_address_loopback(DE_NET_FAMILY_IPV6, 0);
    ASSERT_EQ(de__udp_open(&lo6, &only, NULL, &c), DE_OK);
    EXPECT_FALSE(de__udp_get_info(c).dual_stack);
    de__socket_close(c);
    de__socket_close(a);
    de__socket_close(b);
}

TEST(net, udp_dual_stack) {
    de__socket *v6 = open_v6_or_skip("[::]:0");
    de__socket *v4 = NULL;
    ASSERT_EQ(open_udp("127.0.0.1:0", &v4), DE_OK);
    EXPECT_TRUE(de__udp_get_info(v6).dual_stack);

    /* IPv4 sender -> dual-stack IPv6 socket: the sender shows up as plain IPv4. */
    de_net_address to = de_net_address_ipv4(127, 0, 0, 1, v6->local.port);
    u32 sent = 0;
    ASSERT_EQ(de__socket_send_to(v4, &to, "ping", 4, &sent), DE_OK);
    char buf[32];
    de__datagram m = {buf, (u32)sizeof buf, 0, {0}};
    ASSERT_EQ(recv_wait(v6, &m, 1, 1, 2000), 1u);
    EXPECT_EQ(m.bytes, 4u);
    EXPECT_EQ(m.from.family, (u16)DE_NET_FAMILY_IPV4);
    EXPECT_TRUE(de_net_address_equal(&m.from, &v4->local));

    /* And the IPv6 socket answers to that IPv4 address. */
    ASSERT_EQ(de__socket_send_to(v6, &m.from, "pong", 4, &sent), DE_OK);
    ASSERT_EQ(sent, 4u);
    de__datagram r = {buf, (u32)sizeof buf, 0, {0}};
    ASSERT_EQ(recv_wait(v4, &r, 1, 1, 2000), 1u);
    EXPECT_EQ(memcmp(buf, "pong", 4), 0);
    EXPECT_EQ(r.from.port, v6->local.port);
    de__socket_close(v4);
    de__socket_close(v6);
}

TEST(net, udp_nonblocking_empty) {
    de__socket *s = NULL;
    ASSERT_EQ(open_udp("127.0.0.1:0", &s), DE_OK);
    char buf[64];
    de__datagram m[4];
    for (u32 i = 0; i < 4; ++i)
        m[i] = (de__datagram){buf, (u32)sizeof buf, 0, {0}};
    /* Each call returns at once with nothing; the median of 101 calls is far below 1 ms (a single
     * call can be preempted on a loaded runner, which is why the median is checked). */
    enum { CALLS = 101 };
    u64 ns[CALLS];
    for (u32 i = 0; i < CALLS; ++i) {
        u32 got = 99;
        u64 t0 = de_time_now_ns();
        ASSERT_EQ(de__socket_recv_from(s, m, 4, &got), DE_OK);
        ns[i] = de_time_now_ns() - t0;
        ASSERT_EQ(got, 0u);
    }
    for (u32 i = 1; i < CALLS; ++i) /* insertion sort */
        for (u32 j = i; j > 0 && ns[j - 1] > ns[j]; --j) {
            u64 t = ns[j];
            ns[j] = ns[j - 1];
            ns[j - 1] = t;
        }
    EXPECT_LT(ns[CALLS / 2], NS_PER_MS);
    EXPECT_EQ(s->stats.datagrams_received, 0u);
    de__socket_close(s);
}

TEST(net, udp_truncated_and_errors) {
    de__socket *a = NULL, *b = NULL;
    ASSERT_EQ(open_udp("127.0.0.1:0", &a), DE_OK);
    ASSERT_EQ(open_udp("127.0.0.1:0", &b), DE_OK);

    /* A datagram bigger than its slot is dropped and counted; the next ones arrive intact. */
    u8 big[100];
    memset(big, 7, sizeof big);
    u32 sent = 0;
    ASSERT_EQ(de__socket_send_to(a, &b->local, big, sizeof big, &sent), DE_OK);
    ASSERT_EQ(de__socket_send_to(a, &b->local, "12345", 5, &sent), DE_OK);
    ASSERT_EQ(de__socket_send_to(a, &b->local, "abcdef", 6, &sent), DE_OK);
    u8 s0[10], s1[200], s2[200];
    de__datagram m[3] = {
        {s0, (u32)sizeof s0, 0, {0}}, {s1, (u32)sizeof s1, 0, {0}}, {s2, (u32)sizeof s2, 0, {0}}};
    /* Wait until all three are queued so one call sees them together. */
    u64 deadline = de_time_now_ns() + 2000 * NS_PER_MS;
    u32 got = 0;
    while (got < 2 && de_time_now_ns() < deadline) {
        u32 n = 0;
        ASSERT_EQ(de__socket_recv_from(b, m + got, 3 - got, &n), DE_OK);
        got += n;
    }
    ASSERT_EQ(got, 2u);
    EXPECT_EQ(b->stats.truncated, 1u);
    ASSERT_EQ(m[0].bytes, 5u);
    EXPECT_EQ(memcmp(m[0].data, "12345", 5), 0);
    ASSERT_EQ(m[1].bytes, 6u);
    EXPECT_EQ(memcmp(m[1].data, "abcdef", 6), 0);

    /* Address in use, bad family, wrong destination family, oversize datagram. */
    de__socket *c = NULL;
    EXPECT_EQ(de__udp_open(&a->local, NULL, NULL, &c), DE_ERR_ALREADY_EXISTS);
    EXPECT_TRUE(c == NULL);
    de_net_address none;
    memset(&none, 0, sizeof none);
    EXPECT_EQ(de__udp_open(&none, NULL, NULL, &c), DE_ERR_INVALID_ARGUMENT);
    de_net_address v6 = de_net_address_loopback(DE_NET_FAMILY_IPV6, 9);
    EXPECT_EQ(de__socket_send_to(a, &v6, "x", 1, &sent), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sent, 0u);
    static u8 huge[DE__NET_MAX_UDP_PAYLOAD + 1];
    EXPECT_EQ(de__socket_send_to(a, &b->local, huge, sizeof huge, &sent), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__udp_open(NULL, NULL, NULL, NULL), DE_ERR_INVALID_ARGUMENT);

    /* NULL bind: dual-stack [::]:0, or 0.0.0.0:0 without IPv6. */
    ASSERT_EQ(de__udp_open(NULL, NULL, NULL, &c), DE_OK);
    EXPECT_TRUE(de__net_address_is_unspecified(&c->local));
    EXPECT_NE(c->local.port, 0);
    de__socket_close(c);

    /* An explicit buffer request is applied (the OS may round it). */
    de__udp_desc d = {0};
    d.buffer_bytes = 256u * 1024u;
    de_net_address lo = de_net_address_loopback(DE_NET_FAMILY_IPV4, 0);
    ASSERT_EQ(de__udp_open(&lo, &d, NULL, &c), DE_OK);
    EXPECT_GE(de__udp_get_info(c).recv_buffer_bytes, 128u * 1024u);
    de__socket_close(c);

    de__socket_close(NULL); /* no-op */
    de__socket_close(a);
    de__socket_close(b);
}

TEST(net, udp_send_recv_no_alloc) {
    de__socket *a = NULL, *b = NULL;
    ASSERT_EQ(open_udp("127.0.0.1:0", &a), DE_OK);
    ASSERT_EQ(open_udp("127.0.0.1:0", &b), DE_OK);
    de__loopback_net *net = NULL;
    ASSERT_EQ(de__loopback_net_create(NULL, NULL, &net), DE_OK);
    de__socket *la = NULL, *lb = NULL;
    ASSERT_EQ(de__loopback_open(net, NULL, &la), DE_OK);
    ASSERT_EQ(de__loopback_open(net, NULL, &lb), DE_OK);

    u8 payload[1200];
    for (u32 i = 0; i < sizeof payload; ++i)
        payload[i] = (u8)i;
    u8 buf[2][1500];
    de__datagram m[2] = {{buf[0], (u32)sizeof buf[0], 0, {0}},
                         {buf[1], (u32)sizeof buf[1], 0, {0}}};
    u32 sent = 0;
    /* Warm-up: first calls outside the guarded region. */
    ASSERT_EQ(de__socket_send_to(a, &b->local, payload, sizeof payload, &sent), DE_OK);
    ASSERT_EQ(recv_wait(b, m, 2, 1, 2000), 1u);
    ASSERT_EQ(de__socket_send_to(la, &lb->local, payload, sizeof payload, &sent), DE_OK);
    ASSERT_EQ(recv_wait(lb, m, 2, 1, 2000), 1u);

    enum { ROUNDS = 500 };
    u32 udp_got = 0, lb_got = 0;
    de_alloc_guard g = {0};
    de_alloc_guard_begin(&g);
    for (u32 i = 0; i < ROUNDS; ++i) {
        u32 n = 0;
        if (de__socket_send_to(a, &b->local, payload, sizeof payload, &sent) != DE_OK)
            break;
        /* Bounded poll without sleeping (a sleep is not what is being measured). */
        for (u32 spin = 0; spin < 1000000 && n == 0; ++spin)
            if (de__socket_recv_from(b, m, 2, &n) != DE_OK)
                break;
        udp_got += n;
        n = 0;
        (void)de__socket_send_to(la, &lb->local, payload, sizeof payload, &sent);
        (void)de__socket_recv_from(lb, m, 2, &n);
        lb_got += n;
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_EQ(udp_got, (u32)ROUNDS);
    EXPECT_EQ(lb_got, (u32)ROUNDS);

    de__socket_close(la);
    de__socket_close(lb);
    de__loopback_net_destroy(net);
    de__socket_close(a);
    de__socket_close(b);
}

/* ---------- Loopback ---------- */

TEST(net, loopback_fifo_and_drop) {
    de__loopback_desc d = {0};
    d.queue_capacity = 4;
    d.max_datagram_bytes = 64;
    de__loopback_net *net = NULL;
    ASSERT_EQ(de__loopback_net_create(&d, NULL, &net), DE_OK);
    de__socket *a = NULL, *b = NULL;
    ASSERT_EQ(de__loopback_open(net, NULL, &a), DE_OK);
    ASSERT_EQ(de__loopback_open(net, NULL, &b), DE_OK);
    EXPECT_EQ(a->local.family, (u16)DE_NET_FAMILY_IPV4);
    EXPECT_GE(a->local.port, 49152);
    EXPECT_NE(a->local.port, b->local.port);

    /* Six datagrams into a queue of four: the first four arrive in order, two are dropped. */
    for (u32 i = 0; i < 6; ++i) {
        u32 sent = 0;
        ASSERT_EQ(de__socket_send_to(a, &b->local, &i, sizeof i, &sent), DE_OK);
        EXPECT_EQ(sent, (u32)sizeof i); /* a drop is not a send failure, as with UDP */
    }
    u32 vals[8];
    de__datagram m[8];
    for (u32 i = 0; i < 8; ++i)
        m[i] = (de__datagram){&vals[i], (u32)sizeof vals[i], 0, {0}};
    u32 got = 0;
    ASSERT_EQ(de__socket_recv_from(b, m, 8, &got), DE_OK);
    ASSERT_EQ(got, 4u);
    for (u32 i = 0; i < 4; ++i) {
        EXPECT_EQ(vals[i], i);
        EXPECT_TRUE(de_net_address_equal(&m[i].from, &a->local));
    }
    EXPECT_EQ(b->stats.dropped, 2u);
    EXPECT_EQ(b->stats.datagrams_received, 4u);
    ASSERT_EQ(de__socket_recv_from(b, m, 8, &got), DE_OK);
    EXPECT_EQ(got, 0u);

    /* Interleaved sends and receives keep FIFO order across the ring's wrap. */
    u32 next_send = 100, next_recv = 100;
    for (u32 round = 0; round < 50; ++round) {
        for (u32 k = 0; k < 3; ++k, ++next_send) {
            u32 sent = 0;
            ASSERT_EQ(de__socket_send_to(a, &b->local, &next_send, 4, &sent), DE_OK);
        }
        ASSERT_EQ(de__socket_recv_from(b, m, 2, &got), DE_OK);
        ASSERT_EQ(got, 2u);
        for (u32 k = 0; k < got; ++k)
            ASSERT_EQ(vals[k], next_recv++);
        ASSERT_EQ(de__socket_recv_from(b, m, 8, &got), DE_OK);
        for (u32 k = 0; k < got; ++k)
            ASSERT_EQ(vals[k], next_recv++);
    }
    EXPECT_EQ(next_recv, next_send);
    EXPECT_EQ(b->stats.dropped, 2u);

    /* Oversize: refused like a UDP send over the MTU with Don't-Fragment. */
    u8 big[65];
    u32 sent = 1;
    EXPECT_EQ(de__socket_send_to(a, &b->local, big, sizeof big, &sent), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(sent, 0u);
    /* Nobody at that address: lost silently. */
    de_net_address nowhere = de_net_address_ipv4(127, 0, 0, 1, 7);
    EXPECT_EQ(de__socket_send_to(a, &nowhere, "x", 1, &sent), DE_OK);

    /* A datagram larger than the receiver's slot is dropped and counted. */
    u8 small_buf[2];
    de__datagram small = {small_buf, 2, 0, {0}};
    ASSERT_EQ(de__socket_send_to(a, &b->local, "xyz", 3, &sent), DE_OK);
    ASSERT_EQ(de__socket_recv_from(b, &small, 1, &got), DE_OK);
    EXPECT_EQ(got, 0u);
    EXPECT_EQ(b->stats.truncated, 1u);

    de__socket_close(a);
    de__socket_close(b);
    de__loopback_net_destroy(net);
}

TEST(net, loopback_addresses) {
    de__loopback_net *net = NULL;
    ASSERT_EQ(de__loopback_net_create(NULL, NULL, &net), DE_OK);
    /* A server on 0.0.0.0:7777 receives what is sent to 127.0.0.1:7777. */
    de_net_address any = de_net_address_any(DE_NET_FAMILY_IPV4, 7777);
    de__socket *server = NULL, *client = NULL, *other = NULL;
    ASSERT_EQ(de__loopback_open(net, &any, &server), DE_OK);
    EXPECT_TRUE(de_net_address_equal(&server->local, &any));
    de_net_address taken = de_net_address_ipv4(127, 0, 0, 1, 7777);
    EXPECT_EQ(de__loopback_open(net, &taken, &other), DE_ERR_ALREADY_EXISTS);
    EXPECT_EQ(de__loopback_open(net, &any, &other), DE_ERR_ALREADY_EXISTS);
    EXPECT_TRUE(other == NULL);
    de_net_address v6 = de_net_address_loopback(DE_NET_FAMILY_IPV6, 0);
    ASSERT_EQ(de__loopback_open(net, &v6, &client), DE_OK);
    EXPECT_EQ(client->local.family, (u16)DE_NET_FAMILY_IPV6);

    u32 sent = 0, got = 0;
    ASSERT_EQ(de__socket_send_to(client, &taken, "hi", 2, &sent), DE_OK);
    char buf[8];
    de__datagram m = {buf, (u32)sizeof buf, 0, {0}};
    ASSERT_EQ(de__socket_recv_from(server, &m, 1, &got), DE_OK);
    ASSERT_EQ(got, 1u);
    EXPECT_TRUE(de_net_address_equal(&m.from, &client->local));
    /* The server's replies come from 127.0.0.1:7777, not 0.0.0.0. */
    ASSERT_EQ(de__socket_send_to(server, &m.from, "ok", 2, &sent), DE_OK);
    ASSERT_EQ(de__socket_recv_from(client, &m, 1, &got), DE_OK);
    ASSERT_EQ(got, 1u);
    EXPECT_TRUE(de_net_address_equal(&m.from, &taken));

    /* Closing frees the port. */
    de__socket_close(server);
    ASSERT_EQ(de__loopback_open(net, &taken, &other), DE_OK);
    de__socket_close(other);
    de__socket_close(client);
    de__loopback_net_destroy(net);

    /* Out of memory at create and open. */
    EXPECT_EQ(de__loopback_net_create(NULL, de_allocator_null(), &net), DE_ERR_OUT_OF_MEMORY);
    de__loopback_desc too_big = {0};
    too_big.max_datagram_bytes = DE__NET_MAX_UDP_PAYLOAD + 1;
    EXPECT_EQ(de__loopback_net_create(&too_big, NULL, &net), DE_ERR_INVALID_ARGUMENT);
}

TEST(net, socket_buffer_cvar) {
    /* The cvar is registered on first use with the 4 MiB default. */
    EXPECT_EQ(de__net_socket_buffer_bytes(0), DE__NET_SOCKET_BUFFER_DEFAULT);
    EXPECT_EQ(de__net_socket_buffer_bytes(12345), 12345u);
}
