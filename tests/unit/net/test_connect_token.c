/* Tests for connect tokens (DENG-52): generate and verify, expiry, forgery and the all-zero
 * direct-connect token. DENG-378: the connection key (sealed for the server, in clear for the
 * client). */
#include "de_test.h"

#include <dengine/de_net.h>

#include "net/connection.h"

#include <string.h>

static void make_key(u8 key[DE_NET_SERVER_KEY_BYTES], u8 seed) {
    for (u32 i = 0; i < DE_NET_SERVER_KEY_BYTES; ++i)
        key[i] = (u8)(seed * 13u + i);
}

TEST(net, connect_token_generate_verify) {
    u8 key[DE_NET_SERVER_KEY_BYTES];
    make_key(key, 1);
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    ASSERT_EQ(de_net_connect_token_generate(key, 0x0102030405060708ull, 2000000000ull,
                                            0xCAFEF00Dull, token),
              DE_OK);
    de_net_connect_token_info info;
    ASSERT_EQ(de_net_connect_token_verify(key, token, 1999999999ull, &info), DE_OK);
    EXPECT_EQ(info.client_id, 0x0102030405060708ull);
    EXPECT_EQ(info.expire_unix_s, 2000000000ull);
    EXPECT_EQ(info.user, 0xCAFEF00Dull);
    EXPECT_EQ(de_net_connect_token_verify(key, token, 0, NULL), DE_OK); /* out may be NULL */

    /* The values are sealed: they do not appear in the token bytes. */
    u8 id_le[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    for (u32 i = 0; i + 8 <= DE_NET_CONNECT_TOKEN_BYTES; ++i)
        EXPECT_NE(memcmp(token + i, id_le, 8), 0);

    /* Two tokens with the same values differ (random nonce) and both verify. */
    u8 token2[DE_NET_CONNECT_TOKEN_BYTES];
    ASSERT_EQ(de_net_connect_token_generate(key, 0x0102030405060708ull, 2000000000ull,
                                            0xCAFEF00Dull, token2),
              DE_OK);
    EXPECT_NE(memcmp(token, token2, sizeof token), 0);
    EXPECT_EQ(de_net_connect_token_verify(key, token2, 5, NULL), DE_OK);

    /* Every changed byte of the sealed part is a forgery. */
    for (u32 i = 0; i < DE_NET_CONNECT_TOKEN_SEALED_BYTES; ++i) {
        token[i] ^= 0x10u;
        EXPECT_EQ(de_net_connect_token_verify(key, token, 5, &info), DE_ERR_PARSE);
        EXPECT_EQ(info.client_id, 0u); /* zeroed on failure */
        token[i] ^= 0x10u;
    }
    /* Another server's key. */
    u8 other[DE_NET_SERVER_KEY_BYTES];
    make_key(other, 2);
    EXPECT_EQ(de_net_connect_token_verify(other, token, 5, &info), DE_ERR_PARSE);
    /* The server reads only the sealed part: the key part may be absent (a server never sees
     * it) or anything. */
    EXPECT_EQ(de_net_connect_token_verify(key, token, 5, &info), DE_OK);
    u8 sealed_only[DE_NET_CONNECT_TOKEN_SEALED_BYTES];
    memcpy(sealed_only, token, sizeof sealed_only);
    EXPECT_EQ(de_net_connect_token_verify(key, sealed_only, 5, &info), DE_OK);
    /* Random bytes. */
    u8 junk[DE_NET_CONNECT_TOKEN_BYTES];
    for (u32 i = 0; i < sizeof junk; ++i)
        junk[i] = (u8)(i * 7u + 1u);
    EXPECT_EQ(de_net_connect_token_verify(key, junk, 5, &info), DE_ERR_PARSE);

    EXPECT_EQ(de_net_connect_token_generate(NULL, 1, 2, 3, token), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_net_connect_token_generate(key, 1, 2, 3, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_net_connect_token_verify(NULL, token, 5, &info), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_net_connect_token_verify(key, NULL, 5, &info), DE_ERR_INVALID_ARGUMENT);
}

TEST(net, connect_token_expired) {
    u8 key[DE_NET_SERVER_KEY_BYTES];
    make_key(key, 3);
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    ASSERT_EQ(de_net_connect_token_generate(key, 42, 1000, 7, token), DE_OK);
    de_net_connect_token_info info;
    EXPECT_EQ(de_net_connect_token_verify(key, token, 999, &info), DE_OK);
    EXPECT_EQ(de_net_connect_token_verify(key, token, 1000, &info), DE_ERR_TIMEOUT);
    EXPECT_EQ(info.client_id, 0u);
    EXPECT_EQ(de_net_connect_token_verify(key, token, 5000000000ull, &info), DE_ERR_TIMEOUT);
    /* An already-expired token can be made (a backend with a wrong clock); it never verifies. */
    ASSERT_EQ(de_net_connect_token_generate(key, 42, 0, 7, token), DE_OK);
    EXPECT_EQ(de_net_connect_token_verify(key, token, 0, &info), DE_ERR_TIMEOUT);
}

TEST(net, connect_token_zero_direct_connect) {
    u8 key[DE_NET_SERVER_KEY_BYTES];
    make_key(key, 4);
    u8 zero[DE_NET_CONNECT_TOKEN_BYTES];
    memset(zero, 0, sizeof zero);
    de_net_connect_token_info info;
    memset(&info, 0xAB, sizeof info);
    EXPECT_EQ(de_net_connect_token_verify(key, zero, 5, &info), DE_ERR_NOT_FOUND);
    EXPECT_EQ(info.client_id, 0u);
    EXPECT_EQ(info.user, 0u);
    /* One non-zero byte is not a direct connect but a (bad) token. */
    zero[63] = 1;
    EXPECT_EQ(de_net_connect_token_verify(key, zero, 5, &info), DE_ERR_PARSE);
    zero[63] = 0;
    zero[DE_NET_CONNECT_TOKEN_SEALED_BYTES - 1] = 1;
    EXPECT_EQ(de_net_connect_token_verify(key, zero, 5, &info), DE_ERR_PARSE);
    zero[DE_NET_CONNECT_TOKEN_SEALED_BYTES - 1] = 0;
    /* Only the sealed part decides: a key part alone is still a direct connect. */
    zero[DE_NET_CONNECT_TOKEN_SEALED_BYTES] = 1;
    EXPECT_EQ(de_net_connect_token_verify(key, zero, 5, &info), DE_ERR_NOT_FOUND);
    zero[DE_NET_CONNECT_TOKEN_SEALED_BYTES] = 0;
    zero[0] = 1;
    EXPECT_EQ(de_net_connect_token_verify(key, zero, 5, &info), DE_ERR_PARSE);
    /* A generated token is never all zero. */
    u8 token[DE_NET_CONNECT_TOKEN_BYTES];
    u8 any = 0;
    ASSERT_EQ(de_net_connect_token_generate(key, 0, 1, 0, token), DE_OK);
    for (u32 i = 0; i < sizeof token; ++i)
        any |= token[i];
    EXPECT_NE(any, 0);
}

/* The connection key (DENG-378): new for each token, never in the sealed bytes in clear, and the
 * same key the server opens from the sealed part. */
TEST(net, connect_token_connection_key) {
    u8 key[DE_NET_SERVER_KEY_BYTES];
    make_key(key, 5);
    u8 a[DE_NET_CONNECT_TOKEN_BYTES], b[DE_NET_CONNECT_TOKEN_BYTES];
    ASSERT_EQ(de_net_connect_token_generate(key, 9, 1000, 1, a), DE_OK);
    ASSERT_EQ(de_net_connect_token_generate(key, 9, 1000, 1, b), DE_OK);
    const u8 *ka = a + DE_NET_CONNECT_TOKEN_SEALED_BYTES;
    const u8 *kb = b + DE_NET_CONNECT_TOKEN_SEALED_BYTES;
    EXPECT_NE(memcmp(ka, kb, DE_NET_CONNECT_TOKEN_KEY_BYTES), 0);
    u8 any = 0;
    for (u32 i = 0; i < DE_NET_CONNECT_TOKEN_KEY_BYTES; ++i)
        any |= ka[i];
    EXPECT_NE(any, 0);
    for (u32 i = 0; i + 8 <= DE_NET_CONNECT_TOKEN_SEALED_BYTES; ++i)
        EXPECT_NE(memcmp(a + i, ka, 8), 0); /* not in clear in the sealed part */

    /* The server side gets the same key, the values, and leaves the expiry to its caller. */
    de_net_connect_token_info info;
    u8 opened[DE__NET_KEY_BYTES];
    ASSERT_EQ(de__net_connect_token_open(key, a, &info, opened), DE_OK);
    EXPECT_MEMEQ(opened, ka, DE_NET_CONNECT_TOKEN_KEY_BYTES);
    EXPECT_EQ(info.client_id, 9u);
    EXPECT_EQ(info.expire_unix_s, 1000u);
    EXPECT_EQ(info.user, 1u);
    EXPECT_EQ(de_net_connect_token_verify(key, a, 1000, NULL), DE_ERR_TIMEOUT);
    /* A forged or another server's token gives no key; neither does a direct connect. */
    u8 other[DE_NET_SERVER_KEY_BYTES];
    make_key(other, 6);
    EXPECT_EQ(de__net_connect_token_open(other, a, &info, opened), DE_ERR_PARSE);
    for (u32 i = 0; i < DE__NET_KEY_BYTES; ++i)
        EXPECT_EQ(opened[i], 0);
    EXPECT_EQ(info.client_id, 0u);
    u8 zero[DE_NET_CONNECT_TOKEN_BYTES];
    memset(zero, 0, sizeof zero);
    EXPECT_EQ(de__net_connect_token_open(key, zero, &info, opened), DE_ERR_NOT_FOUND);
    for (u32 i = 0; i < DE__NET_KEY_BYTES; ++i)
        EXPECT_EQ(opened[i], 0);
    /* A DENG-52 (v1) token never opens as a v2 one: its layout and label differ. Simulate it by
     * sealing the old 24-byte plaintext under the old label. */
    u8 v1[DE_NET_CONNECT_TOKEN_BYTES];
    memset(v1, 0, sizeof v1);
    static const u8 v1_ad[] = "dengine connect token v1";
    u8 plain[24] = {9};
    for (u32 i = 0; i < 24; ++i)
        v1[i] = (u8)(i + 1u);
    de__net_seal(v1 + 24, v1 + 48, key, v1, v1_ad, (u32)(sizeof v1_ad - 1), plain, 24, false);
    EXPECT_EQ(de_net_connect_token_verify(key, v1, 5, NULL), DE_ERR_PARSE);
}
