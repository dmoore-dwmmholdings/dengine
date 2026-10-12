/* Tests for connect tokens (DENG-52): generate and verify, expiry, forgery and the all-zero
 * direct-connect token. */
#include "de_test.h"

#include <dengine/de_net.h>

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

    /* Every changed byte is a forgery. */
    for (u32 i = 0; i < DE_NET_CONNECT_TOKEN_BYTES; ++i) {
        token[i] ^= 0x10u;
        EXPECT_EQ(de_net_connect_token_verify(key, token, 5, &info), DE_ERR_PARSE);
        EXPECT_EQ(info.client_id, 0u); /* zeroed on failure */
        token[i] ^= 0x10u;
    }
    /* Another server's key. */
    u8 other[DE_NET_SERVER_KEY_BYTES];
    make_key(other, 2);
    EXPECT_EQ(de_net_connect_token_verify(other, token, 5, &info), DE_ERR_PARSE);
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
