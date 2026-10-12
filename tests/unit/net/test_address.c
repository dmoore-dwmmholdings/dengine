/* Tests for de_net_address (DENG-51): parse, format, round trip, equality, hash, resolve. */
#include "de_test.h"

#include <dengine/de_net.h>

#include "net/net_internal.h"

#include <stdio.h>
#include <string.h>

static de_net_address parse_ok(const char *text) {
    de_net_address a;
    de_result r = de_net_address_parse(text, &a);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        fprintf(stderr, "parse '%s' failed: %s\n", text, e != NULL ? e->message : "?");
    }
    ASSERT_EQ(r, DE_OK);
    return a;
}

static void expect_text(const de_net_address *a, const char *want) {
    char buf[DE_NET_ADDRESS_STRING_MAX];
    u32 n = de_net_address_to_string(a, buf, (u32)sizeof buf);
    EXPECT_STREQ(buf, want);
    EXPECT_EQ(n, (u32)strlen(want));
}

/* Builds an IPv6 address from 8 groups. */
static de_net_address v6(const u16 g[8], u16 port) {
    de_net_address a = de_net_address_any(DE_NET_FAMILY_IPV6, port);
    for (u32 i = 0; i < 8; ++i) {
        a.addr[2 * i] = (u8)(g[i] >> 8);
        a.addr[2 * i + 1] = (u8)(g[i] & 0xFF);
    }
    return a;
}

TEST(net, address_parse_valid) {
    de_net_address a = parse_ok("127.0.0.1:7777");
    EXPECT_EQ(a.family, (u16)DE_NET_FAMILY_IPV4);
    EXPECT_EQ(a.port, 7777);
    EXPECT_EQ(a.addr[0], 127);
    EXPECT_EQ(a.addr[3], 1);
    for (u32 i = 4; i < 16; ++i)
        EXPECT_EQ(a.addr[i], 0);
    EXPECT_TRUE(de_net_address_equal(&a, &(de_net_address){0}) == false);
    de_net_address lo4 = de_net_address_loopback(DE_NET_FAMILY_IPV4, 7777);
    EXPECT_TRUE(de_net_address_equal(&a, &lo4));
    de_net_address v4 = de_net_address_ipv4(127, 0, 0, 1, 7777);
    EXPECT_TRUE(de_net_address_equal(&a, &v4));

    a = parse_ok("0.0.0.0:0");
    de_net_address any4 = de_net_address_any(DE_NET_FAMILY_IPV4, 0);
    EXPECT_TRUE(de_net_address_equal(&a, &any4));
    a = parse_ok("255.255.255.255:65535");
    EXPECT_EQ(a.port, 65535);
    EXPECT_EQ(a.addr[2], 255);

    a = parse_ok("[::1]:7777");
    de_net_address lo6 = de_net_address_loopback(DE_NET_FAMILY_IPV6, 7777);
    EXPECT_EQ(a.family, (u16)DE_NET_FAMILY_IPV6);
    EXPECT_TRUE(de_net_address_equal(&a, &lo6));
    a = parse_ok("[::]:0");
    de_net_address any6 = de_net_address_any(DE_NET_FAMILY_IPV6, 0);
    EXPECT_TRUE(de_net_address_equal(&a, &any6));

    /* Every compression position, upper case, full form, dotted tail, zone. */
    const u16 db8[8] = {0x2001, 0xdb8, 0, 0, 0, 0, 0, 1};
    de_net_address want = v6(db8, 80);
    a = parse_ok("[2001:db8::1]:80");
    EXPECT_TRUE(de_net_address_equal(&a, &want));
    a = parse_ok("[2001:DB8:0:0:0:0:0:1]:80");
    EXPECT_TRUE(de_net_address_equal(&a, &want));
    a = parse_ok("[2001:0db8:0000:0000:0000:0000:0000:0001]:80");
    EXPECT_TRUE(de_net_address_equal(&a, &want));
    const u16 full[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    want = v6(full, 1);
    a = parse_ok("[1:2:3:4:5:6:7:8]:1");
    EXPECT_TRUE(de_net_address_equal(&a, &want));
    const u16 head[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    want = v6(head, 1);
    a = parse_ok("[1::]:1");
    EXPECT_TRUE(de_net_address_equal(&a, &want));
    const u16 one_gap[8] = {1, 2, 3, 0, 5, 6, 7, 8};
    want = v6(one_gap, 1);
    a = parse_ok("[1:2:3::5:6:7:8]:1"); /* "::" for a single group is accepted when reading */
    EXPECT_TRUE(de_net_address_equal(&a, &want));
    const u16 mapped[8] = {0, 0, 0, 0, 0, 0xffff, 0xc000, 0x0201};
    want = v6(mapped, 5);
    a = parse_ok("[::ffff:192.0.2.1]:5");
    EXPECT_TRUE(de_net_address_equal(&a, &want));
    EXPECT_EQ(a.family, (u16)DE_NET_FAMILY_IPV6); /* parse keeps the mapped form */
    const u16 tail[8] = {1, 2, 3, 4, 5, 6, 0x0102, 0x0304};
    want = v6(tail, 9);
    a = parse_ok("[1:2:3:4:5:6:1.2.3.4]:9");
    EXPECT_TRUE(de_net_address_equal(&a, &want));
    a = parse_ok("[fe80::1%3]:9");
    EXPECT_EQ(a.scope_id, 3u);
    EXPECT_EQ(a.addr[0], 0xfe);
    EXPECT_EQ(a.addr[15], 1);
    a = parse_ok("[fe80::1%4294967295]:9");
    EXPECT_EQ(a.scope_id, 4294967295u);
}

TEST(net, address_parse_invalid) {
    static const char *const bad[] = {
        "",
        "127.0.0.1",                 /* missing port */
        "127.0.0.1:",                /* empty port */
        "127.0.0.1:65536",           /* port > 65535 */
        "127.0.0.1:99999",           /* port > 65535 */
        "127.0.0.1:123456",          /* too many digits */
        "127.0.0.1:-1",              /* sign */
        "127.0.0.1:+1",              /* sign */
        "127.0.0.1:1x",              /* junk after the port */
        "256.0.0.1:1",               /* octet > 255 */
        "1.2.3:1",                   /* three octets */
        "1.2.3.4.5:1",               /* five octets */
        "01.2.3.4:1",                /* leading zero (octal elsewhere) */
        "1..2.3:1",                  /* empty octet */
        " 1.2.3.4:1",                /* whitespace */
        "1.2.3.4:1 ",                /* whitespace */
        "::1:7777",                  /* IPv6 without brackets */
        "[::1]",                     /* missing port */
        "[::1]:",                    /* empty port */
        "[::1]7777",                 /* no ':' after ']' */
        "[::1:7777",                 /* no ']' */
        "[::1]:65536",               /* port > 65535 */
        "[1:2:3:4:5:6:7:8:9]:1",     /* nine groups */
        "[1:2:3:4:5:6:7]:1",         /* seven groups, no "::" */
        "[1::2::3]:1",               /* two "::" */
        "[12345::]:1",               /* group of five digits */
        "[:1]:1",                    /* single leading ':' */
        "[1:]:1",                    /* single trailing ':' */
        "[:::]:1",                   /* three colons */
        "[1:2:3:4:5:6:7:8::]:1",     /* "::" with eight groups */
        "[g::1]:1",                  /* not hex */
        "[1.2.3.4]:1",               /* IPv4 in brackets */
        "[::1.2.3]:1",               /* short dotted tail */
        "[::1.2.3.4:5]:1",           /* dotted tail not last */
        "[1:2:3:4:5:6:7:1.2.3.4]:1", /* dotted tail makes nine groups */
        "[fe80::1%]:1",              /* empty zone */
        "[fe80::1%eth0]:1",          /* named zone */
        "[fe80::1%4294967296]:1",    /* zone > u32 */
        "[]:1",                      /* empty address */
        "localhost:7777",            /* host names only through de_net_resolve */
        "example.com:80",
        "[localhost]:80",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        de_net_address a;
        memset(&a, 0xAB, sizeof a);
        de_result r = de_net_address_parse(bad[i], &a);
        if (r != DE_ERR_PARSE)
            fprintf(stderr, "accepted or wrong error for '%s': %s\n", bad[i], de_result_str(r));
        EXPECT_EQ(r, DE_ERR_PARSE);
        EXPECT_EQ(a.family, (u16)DE_NET_FAMILY_NONE); /* zeroed on failure */
        EXPECT_EQ(a.port, 0);
    }
    /* Host names: the message points at de_net_resolve. */
    de_net_address a;
    ASSERT_EQ(de_net_address_parse("localhost:7777", &a), DE_ERR_PARSE);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_TRUE(strstr(e->message, "de_net_resolve") != NULL);

    EXPECT_EQ(de_net_address_parse(NULL, &a), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_net_address_parse("1.2.3.4:5", NULL), DE_ERR_INVALID_ARGUMENT);
}

TEST(net, address_to_string) {
    de_net_address a = de_net_address_ipv4(10, 0, 200, 3, 7777);
    expect_text(&a, "10.0.200.3:7777");
    a = de_net_address_loopback(DE_NET_FAMILY_IPV6, 1);
    expect_text(&a, "[::1]:1");
    a = de_net_address_any(DE_NET_FAMILY_IPV6, 0);
    expect_text(&a, "[::]:0");

    /* RFC 5952: longest run, first on a tie, single zero group not compressed, lower case. */
    const u16 tie[8] = {0x2001, 0xdb8, 0, 0, 1, 0, 0, 1};
    a = v6(tie, 80);
    expect_text(&a, "[2001:db8::1:0:0:1]:80");
    const u16 longer[8] = {0x2001, 0, 0, 1, 0, 0, 0, 1};
    a = v6(longer, 80);
    expect_text(&a, "[2001:0:0:1::1]:80");
    const u16 single[8] = {0x2001, 0xdb8, 0, 1, 1, 1, 1, 1};
    a = v6(single, 80);
    expect_text(&a, "[2001:db8:0:1:1:1:1:1]:80");
    const u16 upper[8] = {0xABCD, 0xEF01, 0, 0, 0, 0, 0, 0};
    a = v6(upper, 2);
    expect_text(&a, "[abcd:ef01::]:2");
    const u16 mapped[8] = {0, 0, 0, 0, 0, 0xffff, 0xc000, 0x0201};
    a = v6(mapped, 5);
    expect_text(&a, "[::ffff:192.0.2.1]:5");
    a = parse_ok("[fe80::1%3]:9");
    expect_text(&a, "[fe80::1%3]:9");

    de_net_address none;
    memset(&none, 0, sizeof none);
    expect_text(&none, "(none)");

    /* Longest text fits DE_NET_ADDRESS_STRING_MAX; a small buffer is cut and NUL-terminated. */
    a = parse_ok("[::ffff:255.255.255.255%4294967295]:65535");
    const u16 widest[8] = {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff};
    de_net_address w = v6(widest, 65535);
    w.scope_id = 4294967295u;
    char buf[DE_NET_ADDRESS_STRING_MAX];
    u32 n = de_net_address_to_string(&w, buf, (u32)sizeof buf);
    EXPECT_LT(n, (u32)DE_NET_ADDRESS_STRING_MAX);
    char small[8];
    n = de_net_address_to_string(&w, small, (u32)sizeof small);
    EXPECT_EQ(n, (u32)strlen(buf));
    EXPECT_EQ(strlen(small), (size_t)7);
    EXPECT_EQ(memcmp(small, buf, 7), 0);
}

TEST(net, address_roundtrip) {
    /* Fixed seed: random addresses with runs of zero groups, both families, all ports. */
    u64 x = 0x9E3779B97F4A7C15ull;
    for (u32 iter = 0; iter < 20000; ++iter) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        de_net_address a;
        u16 port = (u16)(x >> 48);
        if ((x & 3u) == 0) {
            a = de_net_address_ipv4((u8)(x >> 8), (u8)(x >> 16), (u8)(x >> 24), (u8)(x >> 32),
                                    port);
        } else {
            u16 g[8];
            u64 y = x * 0xD6E8FEB86659FD93ull;
            for (u32 i = 0; i < 8; ++i) {
                /* About half the groups are zero, so every compression shape shows up. */
                g[i] = ((y >> (i * 2)) & 1u) != 0 ? 0 : (u16)(y >> (16 + i * 5));
                if (((y >> (i + 40)) & 7u) == 0)
                    g[i] = 0xffff;
            }
            a = v6(g, port);
            if ((x & 0x30u) == 0)
                a.scope_id = (u32)(x >> 20);
            if ((x & 0x700u) == 0) { /* mapped form */
                memset(a.addr, 0, 10);
                a.addr[10] = 0xff;
                a.addr[11] = 0xff;
            }
        }
        char buf[DE_NET_ADDRESS_STRING_MAX];
        de_net_address_to_string(&a, buf, (u32)sizeof buf);
        de_net_address b;
        de_result r = de_net_address_parse(buf, &b);
        if (r != DE_OK || !de_net_address_equal(&a, &b))
            fprintf(stderr, "round trip failed for '%s'\n", buf);
        ASSERT_EQ(r, DE_OK);
        ASSERT_TRUE(de_net_address_equal(&a, &b));
        ASSERT_EQ(de_net_address_hash(&a), de_net_address_hash(&b));
        /* Formatting the parsed address gives the same text (the canonical form is stable). */
        char again[DE_NET_ADDRESS_STRING_MAX];
        de_net_address_to_string(&b, again, (u32)sizeof again);
        ASSERT_STREQ(buf, again);
    }
}

TEST(net, address_equal_and_hash) {
    de_net_address a = de_net_address_ipv4(1, 2, 3, 4, 5);
    de_net_address b = a;
    b.addr[10] = 0x55; /* bytes an IPv4 address does not use are ignored */
    EXPECT_TRUE(de_net_address_equal(&a, &b));
    EXPECT_EQ(de_net_address_hash(&a), de_net_address_hash(&b));
    b = a;
    b.port = 6;
    EXPECT_FALSE(de_net_address_equal(&a, &b));
    EXPECT_NE(de_net_address_hash(&a), de_net_address_hash(&b));
    b = a;
    b.addr[3] = 5;
    EXPECT_FALSE(de_net_address_equal(&a, &b));
    EXPECT_NE(de_net_address_hash(&a), de_net_address_hash(&b));

    /* IPv4 and its mapped IPv6 form are different addresses until the transport unmaps. */
    de_net_address m = parse_ok("[::ffff:1.2.3.4]:5");
    EXPECT_FALSE(de_net_address_equal(&a, &m));
    de__net_address_unmap(&m);
    EXPECT_TRUE(de_net_address_equal(&a, &m));
    EXPECT_EQ(de_net_address_hash(&a), de_net_address_hash(&m));

    de_net_address s1 = parse_ok("[fe80::1%1]:9");
    de_net_address s2 = parse_ok("[fe80::1%2]:9");
    EXPECT_FALSE(de_net_address_equal(&s1, &s2));

    /* The hash is part of the wire-independent contract: fixed value for a fixed address. */
    de_net_address lo = de_net_address_loopback(DE_NET_FAMILY_IPV4, 7777);
    u64 h1 = de_net_address_hash(&lo);
    de_net_address lo2 = parse_ok("127.0.0.1:7777");
    EXPECT_EQ(h1, de_net_address_hash(&lo2));

    EXPECT_TRUE(de__net_address_is_unspecified(&(de_net_address){.family = DE_NET_FAMILY_IPV6}));
    EXPECT_FALSE(de__net_address_is_unspecified(&lo));
    de_net_address none;
    memset(&none, 0, sizeof none);
    EXPECT_FALSE(de__net_address_is_unspecified(&none));
    de_net_address bad = de_net_address_any((de_net_family)5, 1);
    EXPECT_EQ(bad.family, (u16)DE_NET_FAMILY_NONE);
}

TEST(net, address_resolve) {
    de_net_address out[8];
    u32 n = 0;
    /* Numeric addresses do not touch the resolver. */
    ASSERT_EQ(de_net_resolve("127.0.0.1:80", DE_NET_FAMILY_NONE, out, 8, &n), DE_OK);
    ASSERT_EQ(n, 1u);
    de_net_address want = de_net_address_ipv4(127, 0, 0, 1, 80);
    EXPECT_TRUE(de_net_address_equal(&out[0], &want));
    ASSERT_EQ(de_net_resolve("[::1]:81", DE_NET_FAMILY_NONE, out, 8, &n), DE_OK);
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(de_net_resolve("[::1]:81", DE_NET_FAMILY_IPV4, out, 8, &n), DE_ERR_NOT_FOUND);

    /* "localhost" comes from the hosts file, so this works offline. */
    ASSERT_EQ(de_net_resolve("localhost:7777", DE_NET_FAMILY_NONE, out, 8, &n), DE_OK);
    ASSERT_GE(n, 1u);
    for (u32 i = 0; i < n; ++i) {
        EXPECT_EQ(out[i].port, 7777);
        EXPECT_TRUE(out[i].family == DE_NET_FAMILY_IPV4 || out[i].family == DE_NET_FAMILY_IPV6);
        for (u32 j = 0; j < i; ++j)
            EXPECT_FALSE(de_net_address_equal(&out[i], &out[j])); /* no duplicates */
    }
    ASSERT_EQ(de_net_resolve("localhost:7777", DE_NET_FAMILY_IPV4, out, 8, &n), DE_OK);
    want = de_net_address_ipv4(127, 0, 0, 1, 7777);
    EXPECT_TRUE(de_net_address_equal(&out[0], &want));
    ASSERT_EQ(de_net_resolve("[localhost]:7", DE_NET_FAMILY_IPV4, out, 1, &n), DE_OK);
    EXPECT_EQ(n, 1u);

    EXPECT_EQ(de_net_resolve("localhost", DE_NET_FAMILY_NONE, out, 8, &n), DE_ERR_PARSE);
    EXPECT_EQ(de_net_resolve("localhost:70000", DE_NET_FAMILY_NONE, out, 8, &n), DE_ERR_PARSE);
    EXPECT_EQ(de_net_resolve(":80", DE_NET_FAMILY_NONE, out, 8, &n), DE_ERR_PARSE);
    EXPECT_EQ(de_net_resolve("localhost:80", DE_NET_FAMILY_NONE, out, 0, &n),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_net_resolve(NULL, DE_NET_FAMILY_NONE, out, 8, &n), DE_ERR_INVALID_ARGUMENT);
    /* .invalid never resolves (RFC 6761). Offline it may be NETWORK rather than NOT_FOUND. */
    de_result r = de_net_resolve("no-such-host.invalid:80", DE_NET_FAMILY_NONE, out, 8, &n);
    EXPECT_TRUE(r == DE_ERR_NOT_FOUND || r == DE_ERR_NETWORK);
    EXPECT_EQ(n, 0u);
}
