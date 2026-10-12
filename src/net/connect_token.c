/* connect_token.c: connect tokens (DENG-52, DENG-378). See de_net.h ("Transport: connect tokens").
 *
 * Layout, 128 bytes: the sealed part (96) | the connection key in clear (32). The sealed part is
 * nonce (24, random) | ciphertext (56) | tag (16); its plaintext is client_id | expire_unix_s |
 * user (each u64 little-endian) | key (32). Sealed with the server key by XChaCha20-Poly1305; the
 * associated data is a fixed label, so a token cannot be mistaken for any other sealed message of
 * the engine. The label names the layout version: a DENG-52 token (v1, 64 bytes, no key) never
 * opens as a v2 one. */
#include "net/connection.h"

#include <dengine/de_net.h>

#include <string.h>

#define TOKEN_NONCE  0u
#define TOKEN_CIPHER 24u
#define TOKEN_PLAIN  (24u + DE_NET_CONNECT_TOKEN_KEY_BYTES)
#define TOKEN_TAG    (TOKEN_CIPHER + TOKEN_PLAIN)
#define TOKEN_KEY    DE_NET_CONNECT_TOKEN_SEALED_BYTES

DE_STATIC_ASSERT(TOKEN_TAG + DE__NET_TAG_BYTES == DE_NET_CONNECT_TOKEN_SEALED_BYTES,
                 "connect token layout");
DE_STATIC_ASSERT(TOKEN_KEY + DE_NET_CONNECT_TOKEN_KEY_BYTES == DE_NET_CONNECT_TOKEN_BYTES,
                 "connect token layout");
DE_STATIC_ASSERT(DE_NET_SERVER_KEY_BYTES == DE__NET_KEY_BYTES, "server key size");
DE_STATIC_ASSERT(DE_NET_CONNECT_TOKEN_KEY_BYTES == DE__NET_KEY_BYTES, "connection key size");
DE_STATIC_ASSERT(DE__NET_TOKEN_BYTES == DE_NET_CONNECT_TOKEN_SEALED_BYTES, "wire token size");

static const u8 k_token_ad[] = "dengine connect token v2";

de_result de_net_connect_token_generate(const u8 server_key[DE_NET_SERVER_KEY_BYTES], u64 client_id,
                                        u64 expire_unix_s, u64 user,
                                        u8 out[DE_NET_CONNECT_TOKEN_BYTES]) {
    if (server_key == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_net_connect_token_generate: NULL argument");
    u8 plain[TOKEN_PLAIN];
    de_result r = de__net_random(out + TOKEN_NONCE, DE__NET_NONCE_BYTES);
    if (r == DE_OK)
        r = de__net_random(plain + 24, DE_NET_CONNECT_TOKEN_KEY_BYTES);
    if (r != DE_OK) {
        de__net_wipe(plain, sizeof plain);
        memset(out, 0, DE_NET_CONNECT_TOKEN_BYTES);
        return r;
    }
    de__net_put_u64(plain, client_id);
    de__net_put_u64(plain + 8, expire_unix_s);
    de__net_put_u64(plain + 16, user);
    memcpy(out + TOKEN_KEY, plain + 24, DE_NET_CONNECT_TOKEN_KEY_BYTES);
    de__net_seal(out + TOKEN_CIPHER, out + TOKEN_TAG, server_key, out + TOKEN_NONCE, k_token_ad,
                 (u32)(sizeof k_token_ad - 1), plain, TOKEN_PLAIN, false);
    de__net_wipe(plain, sizeof plain);
    return DE_OK;
}

de_result de__net_connect_token_open(const u8 server_key[DE__NET_KEY_BYTES],
                                     const u8 sealed[DE__NET_TOKEN_BYTES],
                                     de_net_connect_token_info *out, u8 key[DE__NET_KEY_BYTES]) {
    memset(out, 0, sizeof *out);
    memset(key, 0, DE__NET_KEY_BYTES);
    /* All zero: a direct connect. OR the bytes so the time does not depend on where the first
     * non-zero byte is. */
    u8 any = 0;
    for (u32 i = 0; i < DE__NET_TOKEN_BYTES; ++i)
        any |= sealed[i];
    if (any == 0)
        return DE_ERR_NOT_FOUND;
    u8 plain[TOKEN_PLAIN];
    if (!de__net_open(plain, sealed + TOKEN_TAG, server_key, sealed + TOKEN_NONCE, k_token_ad,
                      (u32)(sizeof k_token_ad - 1), sealed + TOKEN_CIPHER, TOKEN_PLAIN, false))
        return DE_ERR_PARSE;
    out->client_id = de__net_get_u64(plain);
    out->expire_unix_s = de__net_get_u64(plain + 8);
    out->user = de__net_get_u64(plain + 16);
    memcpy(key, plain + 24, DE__NET_KEY_BYTES);
    de__net_wipe(plain, sizeof plain);
    return DE_OK;
}

de_result de_net_connect_token_verify(const u8 server_key[DE_NET_SERVER_KEY_BYTES],
                                      const u8 token[DE_NET_CONNECT_TOKEN_SEALED_BYTES],
                                      u64 now_unix_s, de_net_connect_token_info *out) {
    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (server_key == NULL || token == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_net_connect_token_verify: NULL argument");
    de_net_connect_token_info info;
    u8 key[DE__NET_KEY_BYTES];
    de_result r = de__net_connect_token_open(server_key, token, &info, key);
    de__net_wipe(key, sizeof key);
    if (r == DE_OK && now_unix_s >= info.expire_unix_s)
        r = DE_ERR_TIMEOUT;
    if (r == DE_OK && out != NULL)
        *out = info;
    de__net_wipe(&info, sizeof info);
    return r;
}

de_result de_net_server_key_generate(u8 out[DE_NET_SERVER_KEY_BYTES]) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_net_server_key_generate: NULL argument");
    return de__net_random(out, DE_NET_SERVER_KEY_BYTES);
}
