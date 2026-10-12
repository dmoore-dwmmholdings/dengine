/* Tests for the net crypto layer (DENG-52): Monocypher's own test vectors for the primitives the
 * engine uses, the packet AEAD (and its plaintext-MAC mode) against tampering, the directional
 * key derivation, the replay window, and varints. */
#include "de_test.h"

#include <dengine/de_net.h>

#include "net/crypto.h"

#include <monocypher.h>

/* Upstream's TIS-CI vector subset (third_party/monocypher/tests/). It defines a static array for
 * every primitive; only some are used here. */
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "tis-ci-vectors.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <string.h>

/* ---------- Monocypher vectors ---------- */

#define VEC_MAX 1024u

typedef struct vec {
    u8 buf[VEC_MAX];
    u32 size;
} vec;

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Decodes the next hex string of the vector list. */
static void next_vec(const char ***it, const char **end, vec *out) {
    ASSERT_TRUE(*it < end);
    const char *s = **it;
    (*it)++;
    size_t n = strlen(s);
    ASSERT_EQ(n % 2u, (size_t)0);
    ASSERT_LE(n / 2u, (size_t)VEC_MAX);
    for (size_t i = 0; i < n / 2u; ++i) {
        int hi = hex_digit(s[2 * i]), lo = hex_digit(s[2 * i + 1]);
        ASSERT_TRUE(hi >= 0 && lo >= 0);
        out->buf[i] = (u8)(hi * 16 + lo);
    }
    out->size = (u32)(n / 2u);
}

/* Static: the vectors are a few hundred bytes each, more than a test should put on its stack. */
static vec g_in[5], g_want, g_got;

TEST(net, monocypher_vectors) {
    u32 checked = 0;

    /* XChaCha20 (the stream cipher of the packet AEAD). */
    {
        const char **it = xchacha20_vectors, **end = xchacha20_vectors + nb_xchacha20_vectors;
        while (it < end) {
            for (u32 i = 0; i < 4; ++i)
                next_vec(&it, end, &g_in[i]); /* key, nonce, plain, counter */
            next_vec(&it, end, &g_want);
            ASSERT_EQ(g_in[3].size, 8u);
            u64 ctr = de__net_get_u64(g_in[3].buf);
            crypto_chacha20_x(g_got.buf, g_in[2].buf, g_in[2].size, g_in[0].buf, g_in[1].buf, ctr);
            ASSERT_EQ(g_want.size, g_in[2].size);
            EXPECT_MEMEQ(g_got.buf, g_want.buf, g_want.size);
            checked++;
        }
    }
    /* Poly1305 (its MAC). */
    {
        const char **it = poly1305_vectors, **end = poly1305_vectors + nb_poly1305_vectors;
        while (it < end) {
            next_vec(&it, end, &g_in[0]); /* key */
            next_vec(&it, end, &g_in[1]); /* message */
            next_vec(&it, end, &g_want);
            crypto_poly1305(g_got.buf, g_in[1].buf, g_in[1].size, g_in[0].buf);
            EXPECT_MEMEQ(g_got.buf, g_want.buf, 16);
            checked++;
        }
    }
    /* XChaCha20-Poly1305: lock gives mac | cipher; unlock gives the text back; a changed byte
     * fails. */
    {
        const char **it = aead_ietf_vectors, **end = aead_ietf_vectors + nb_aead_ietf_vectors;
        while (it < end) {
            for (u32 i = 0; i < 4; ++i)
                next_vec(&it, end, &g_in[i]); /* key, nonce, ad, text */
            next_vec(&it, end, &g_want);
            u32 n = g_in[3].size;
            ASSERT_EQ(g_want.size, n + 16u);
            crypto_aead_lock(g_got.buf + 16, g_got.buf, g_in[0].buf, g_in[1].buf, g_in[2].buf,
                             g_in[2].size, g_in[3].buf, n);
            EXPECT_MEMEQ(g_got.buf, g_want.buf, g_want.size);
            u8 plain[VEC_MAX];
            EXPECT_EQ(crypto_aead_unlock(plain, g_want.buf, g_in[0].buf, g_in[1].buf, g_in[2].buf,
                                         g_in[2].size, g_want.buf + 16, n),
                      0);
            EXPECT_MEMEQ(plain, g_in[3].buf, n);
            g_want.buf[0] ^= 0x01u;
            EXPECT_EQ(crypto_aead_unlock(plain, g_want.buf, g_in[0].buf, g_in[1].buf, g_in[2].buf,
                                         g_in[2].size, g_want.buf + 16, n),
                      -1);
            checked++;
        }
    }
    /* Keyed BLAKE2b (KDF, cookie, DENIED proof). */
    {
        const char **it = blake2b_vectors, **end = blake2b_vectors + nb_blake2b_vectors;
        while (it < end) {
            next_vec(&it, end, &g_in[0]); /* message */
            next_vec(&it, end, &g_in[1]); /* key */
            next_vec(&it, end, &g_want);
            crypto_blake2b_keyed(g_got.buf, g_want.size, g_in[1].buf, g_in[1].size, g_in[0].buf,
                                 g_in[0].size);
            EXPECT_MEMEQ(g_got.buf, g_want.buf, g_want.size);
            checked++;
        }
    }
    /* X25519 (key agreement). */
    {
        const char **it = x25519_vectors, **end = x25519_vectors + nb_x25519_vectors;
        while (it < end) {
            next_vec(&it, end, &g_in[0]); /* scalar */
            next_vec(&it, end, &g_in[1]); /* point */
            next_vec(&it, end, &g_want);
            crypto_x25519(g_got.buf, g_in[0].buf, g_in[1].buf);
            EXPECT_MEMEQ(g_got.buf, g_want.buf, 32);
            checked++;
        }
    }
    /* Every vector group of the five primitives (45 in Monocypher 4.0.3's TIS-CI set). */
    EXPECT_EQ(checked,
              (u32)(nb_xchacha20_vectors / 5 + nb_poly1305_vectors / 3 + nb_aead_ietf_vectors / 5 +
                    nb_blake2b_vectors / 3 + nb_x25519_vectors / 3));
    EXPECT_GE(checked, 45u);
}

/* ---------- Packet AEAD ---------- */

static void fill(u8 *p, u32 n, u8 seed) {
    for (u32 i = 0; i < n; ++i)
        p[i] = (u8)(seed + i * 31u);
}

static void aead_tamper(bool plaintext_mode) {
    u8 key[32], nonce[24], ad[DE__NET_AD_MAX], plain[100], cipher[100], tag[16], out[100];
    fill(key, 32, 1);
    de__net_nonce(nonce, 12345, DE__NET_DIR_C2S, DE__NET_DOMAIN_SESSION);
    u8 extra[4] = {9, 8, 7, 6};
    u32 ad_bytes = de__net_ad(ad, 0x1122334455667788ull, 7, extra, sizeof extra);
    ASSERT_EQ(ad_bytes, 13u);
    fill(plain, sizeof plain, 77);

    de__net_seal(cipher, tag, key, nonce, ad, ad_bytes, plain, sizeof plain, plaintext_mode);
    if (plaintext_mode)
        EXPECT_MEMEQ(cipher, plain, sizeof plain); /* readable, still authenticated */
    else
        EXPECT_NE(memcmp(cipher, plain, sizeof plain), 0);
    ASSERT_TRUE(
        de__net_open(out, tag, key, nonce, ad, ad_bytes, cipher, sizeof cipher, plaintext_mode));
    EXPECT_MEMEQ(out, plain, sizeof plain);

    /* Every single-bit flip of body, tag, AD, nonce and key fails. */
    for (u32 i = 0; i < sizeof cipher * 8u; ++i) {
        cipher[i / 8] ^= (u8)(1u << (i % 8));
        EXPECT_FALSE(de__net_open(out, tag, key, nonce, ad, ad_bytes, cipher, sizeof cipher,
                                  plaintext_mode));
        cipher[i / 8] ^= (u8)(1u << (i % 8));
    }
    for (u32 i = 0; i < 16u * 8u; ++i) {
        tag[i / 8] ^= (u8)(1u << (i % 8));
        EXPECT_FALSE(de__net_open(out, tag, key, nonce, ad, ad_bytes, cipher, sizeof cipher,
                                  plaintext_mode));
        tag[i / 8] ^= (u8)(1u << (i % 8));
    }
    for (u32 i = 0; i < ad_bytes; ++i) {
        ad[i] ^= 0x40u;
        EXPECT_FALSE(de__net_open(out, tag, key, nonce, ad, ad_bytes, cipher, sizeof cipher,
                                  plaintext_mode));
        ad[i] ^= 0x40u;
    }
    for (u32 i = 0; i < 24u; ++i) {
        nonce[i] ^= 0x02u;
        EXPECT_FALSE(de__net_open(out, tag, key, nonce, ad, ad_bytes, cipher, sizeof cipher,
                                  plaintext_mode));
        nonce[i] ^= 0x02u;
    }
    key[31] ^= 0x80u;
    EXPECT_FALSE(
        de__net_open(out, tag, key, nonce, ad, ad_bytes, cipher, sizeof cipher, plaintext_mode));
    key[31] ^= 0x80u;
    /* Truncated AD (a shorter header) fails too. */
    EXPECT_FALSE(de__net_open(out, tag, key, nonce, ad, ad_bytes - 1u, cipher, sizeof cipher,
                              plaintext_mode));
    /* Still opens after all that (nothing was left changed). */
    EXPECT_TRUE(
        de__net_open(out, tag, key, nonce, ad, ad_bytes, cipher, sizeof cipher, plaintext_mode));
}

TEST(net, aead_roundtrip_and_tamper) {
    aead_tamper(false);
    aead_tamper(true);

    /* Nonces differ by sequence, direction and domain. */
    u8 a[24], b[24], c[24], d[24];
    de__net_nonce(a, 5, DE__NET_DIR_C2S, DE__NET_DOMAIN_SESSION);
    de__net_nonce(b, 6, DE__NET_DIR_C2S, DE__NET_DOMAIN_SESSION);
    de__net_nonce(c, 5, DE__NET_DIR_S2C, DE__NET_DOMAIN_SESSION);
    de__net_nonce(d, 5, DE__NET_DIR_S2C, DE__NET_DOMAIN_CHALLENGE);
    EXPECT_NE(memcmp(a, b, 24), 0);
    EXPECT_NE(memcmp(a, c, 24), 0);
    EXPECT_NE(memcmp(c, d, 24), 0);

    /* In-place sealing and opening (the connection layer opens bodies in place). */
    u8 key[32], nonce[24], buf[64], ref[64], tag[16], ad[DE__NET_AD_MAX];
    fill(key, 32, 3);
    fill(buf, sizeof buf, 4);
    memcpy(ref, buf, sizeof buf);
    de__net_nonce(nonce, 1, DE__NET_DIR_S2C, DE__NET_DOMAIN_SESSION);
    u32 ad_bytes = de__net_ad(ad, 42, 7, NULL, 0);
    de__net_seal(buf, tag, key, nonce, ad, ad_bytes, buf, sizeof buf, false);
    ASSERT_TRUE(de__net_open(buf, tag, key, nonce, ad, ad_bytes, buf, sizeof buf, false));
    EXPECT_MEMEQ(buf, ref, sizeof buf);

    /* Empty body (KEEPALIVE): the tag alone authenticates the header. */
    de__net_seal(NULL, tag, key, nonce, ad, ad_bytes, NULL, 0, false);
    EXPECT_TRUE(de__net_open(NULL, tag, key, nonce, ad, ad_bytes, NULL, 0, false));
    ad[8] ^= 1u;
    EXPECT_FALSE(de__net_open(NULL, tag, key, nonce, ad, ad_bytes, NULL, 0, false));

    /* AD that does not fit is refused, not cut. */
    u8 big[DE__NET_AD_MAX];
    memset(big, 0, sizeof big);
    EXPECT_EQ(de__net_ad(ad, 1, 1, big, DE__NET_AD_MAX), 0u);
}

/* ---------- Key derivation ---------- */

TEST(net, kdf_directional_keys) {
    u8 cs[32], cp[32], ss[32], sp[32];
    ASSERT_EQ(de__net_x25519_keypair(cs, cp), DE_OK);
    ASSERT_EQ(de__net_x25519_keypair(ss, sp), DE_OK);
    u8 check[32];
    de__net_x25519_public(check, cs);
    EXPECT_MEMEQ(check, cp, 32);

    de__net_keys client, server;
    ASSERT_TRUE(de__net_derive_keys(&client, cs, sp, 77, cp, sp));
    ASSERT_TRUE(de__net_derive_keys(&server, ss, cp, 77, cp, sp));
    EXPECT_MEMEQ(client.c2s, server.c2s, 32);
    EXPECT_MEMEQ(client.s2c, server.s2c, 32);
    EXPECT_NE(memcmp(client.c2s, client.s2c, 32), 0); /* one key per direction */

    /* Another protocol id, or swapped roles, give other keys. */
    de__net_keys other;
    ASSERT_TRUE(de__net_derive_keys(&other, cs, sp, 78, cp, sp));
    EXPECT_NE(memcmp(other.c2s, client.c2s, 32), 0);
    EXPECT_NE(memcmp(other.s2c, client.s2c, 32), 0);
    ASSERT_TRUE(de__net_derive_keys(&other, cs, sp, 77, sp, cp));
    EXPECT_NE(memcmp(other.c2s, client.c2s, 32), 0);

    /* A third party's key pair does not get the same keys. */
    u8 es[32], ep[32];
    ASSERT_EQ(de__net_x25519_keypair(es, ep), DE_OK);
    ASSERT_TRUE(de__net_derive_keys(&other, es, sp, 77, cp, sp));
    EXPECT_NE(memcmp(other.c2s, client.c2s, 32), 0);

    /* Low-order points (all zero, one) give an all-zero shared secret: refused, keys wiped. */
    u8 zero[32] = {0}, one[32] = {1};
    memset(&other, 0xAB, sizeof other);
    EXPECT_FALSE(de__net_derive_keys(&other, cs, zero, 77, cp, zero));
    for (u32 i = 0; i < 32; ++i)
        EXPECT_EQ(other.c2s[i] | other.s2c[i], 0);
    EXPECT_FALSE(de__net_derive_keys(&other, cs, one, 77, cp, one));

    /* Two key pairs never repeat (OS randomness). */
    EXPECT_NE(memcmp(cs, ss, 32), 0);

    /* Wiping. */
    de__net_wipe(&client, sizeof client);
    for (u32 i = 0; i < 32; ++i)
        EXPECT_EQ(client.c2s[i] | client.s2c[i], 0);
}

/* ---------- Replay window ---------- */

/* check + mark, as the connection layer does after a packet authenticated. */
static bool replay_accept(de__net_replay *w, u64 seq) {
    if (!de__net_replay_check(w, seq))
        return false;
    de__net_replay_mark(w, seq);
    return true;
}

TEST(net, replay_window) {
    de__net_replay w;
    memset(&w, 0, sizeof w);
    EXPECT_TRUE(replay_accept(&w, 10)); /* the first sequence may be anything */
    EXPECT_FALSE(replay_accept(&w, 10));
    EXPECT_TRUE(replay_accept(&w, 11));
    EXPECT_TRUE(replay_accept(&w, 9)); /* older than the first, inside the window */
    EXPECT_FALSE(replay_accept(&w, 9));

    /* Out of order inside the window: each exactly once. */
    memset(&w, 0, sizeof w);
    EXPECT_TRUE(replay_accept(&w, 1000));
    for (u64 s = 999; s > 1000 - DE__NET_REPLAY_WINDOW; s -= 2)
        EXPECT_TRUE(replay_accept(&w, s));
    for (u64 s = 999; s > 1000 - DE__NET_REPLAY_WINDOW; s -= 2)
        EXPECT_FALSE(replay_accept(&w, s));
    for (u64 s = 998; s > 1000 - DE__NET_REPLAY_WINDOW; s -= 2)
        EXPECT_TRUE(replay_accept(&w, s));
    /* Exactly the window edge: 1000 - 255 is in, 1000 - 256 is too old. */
    EXPECT_FALSE(replay_accept(&w, 1000 - (DE__NET_REPLAY_WINDOW - 1u))); /* seen above */
    EXPECT_FALSE(replay_accept(&w, 1000 - DE__NET_REPLAY_WINDOW));
    EXPECT_FALSE(replay_accept(&w, 0));

    /* Moving forward by less than a word, across words, and past the window. */
    memset(&w, 0, sizeof w);
    for (u64 s = 0; s < 300; s += 3)
        EXPECT_TRUE(replay_accept(&w, s));
    for (u64 s = 300 - DE__NET_REPLAY_WINDOW + 1; s < 300; ++s)
        EXPECT_EQ(de__net_replay_check(&w, s), s % 3u != 0);
    EXPECT_TRUE(replay_accept(&w, 365)); /* +68: bit shift across a word */
    EXPECT_FALSE(replay_accept(&w, 297));
    EXPECT_TRUE(replay_accept(&w, 298));
    EXPECT_TRUE(replay_accept(&w, 365 + 5000));       /* past the window: all cleared */
    EXPECT_FALSE(replay_accept(&w, 365));             /* now too old */
    EXPECT_TRUE(replay_accept(&w, 365 + 5000 - 255)); /* oldest slot still open */
    EXPECT_FALSE(replay_accept(&w, 365 + 5000 - 255));
    EXPECT_TRUE(replay_accept(&w, UINT64_MAX - 1)); /* huge jump */
    EXPECT_FALSE(replay_accept(&w, UINT64_MAX - 1));
    EXPECT_TRUE(replay_accept(&w, UINT64_MAX));

    /* check() alone never moves the window. */
    memset(&w, 0, sizeof w);
    de__net_replay_mark(&w, 50);
    EXPECT_TRUE(de__net_replay_check(&w, 60));
    EXPECT_TRUE(de__net_replay_check(&w, 60));
    EXPECT_TRUE(de__net_replay_check(&w, 49));
}

/* ---------- Varints ---------- */

TEST(net, varint_roundtrip) {
    static const u64 values[] = {0,         1,          127,        128,        16383,
                                 16384,     0xFFFFFFFF, 1ull << 35, 1ull << 56, UINT64_MAX - 1,
                                 UINT64_MAX};
    for (u32 i = 0; i < sizeof values / sizeof values[0]; ++i) {
        u8 buf[DE__NET_VARINT_MAX];
        u32 n = de__net_varint_write(buf, values[i]);
        ASSERT_GE(n, 1u);
        ASSERT_LE(n, DE__NET_VARINT_MAX);
        u64 back = 0;
        EXPECT_EQ(de__net_varint_read(buf, n, &back), n);
        EXPECT_EQ(back, values[i]);
        if (n > 1)
            EXPECT_EQ(de__net_varint_read(buf, n - 1u, &back), 0u); /* truncated */
    }
    u64 v = 0;
    const u8 overlong[] = {0x80, 0x00};
    EXPECT_EQ(de__net_varint_read(overlong, 2, &v), 0u);
    const u8 too_big[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02};
    EXPECT_EQ(de__net_varint_read(too_big, 10, &v), 0u);
    const u8 eleven[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    EXPECT_EQ(de__net_varint_read(eleven, 11, &v), 0u);
    EXPECT_EQ(de__net_varint_read(overlong, 0, &v), 0u);
}

/* ---------- Randomness ---------- */

TEST(net, os_random) {
    u8 a[300], b[300];
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    ASSERT_EQ(de__net_random(a, sizeof a), DE_OK); /* more than one getentropy call */
    ASSERT_EQ(de__net_random(b, sizeof b), DE_OK);
    EXPECT_NE(memcmp(a, b, sizeof a), 0);
    u32 zeros = 0;
    for (u32 i = 0; i < sizeof a; ++i)
        zeros += a[i] == 0 ? 1u : 0u;
    EXPECT_LT(zeros, 20u); /* about 1 in 256 expected */
    u8 server_key[DE_NET_SERVER_KEY_BYTES];
    EXPECT_EQ(de_net_server_key_generate(server_key), DE_OK);
    EXPECT_EQ(de_net_server_key_generate(NULL), DE_ERR_INVALID_ARGUMENT);
}
