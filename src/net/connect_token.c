/* connect_token.c: connect tokens (DENG-52). See de_net.h ("Transport: connect tokens").
 *
 * Layout, 64 bytes: nonce (24, random) | ciphertext (24) | tag (16). The plaintext is
 * client_id | expire_unix_s | user, each u64 little-endian. Sealed with the server key by
 * XChaCha20-Poly1305; the associated data is a fixed label, so a token cannot be mistaken for any
 * other sealed message of the engine. */
#include "net/crypto.h"

#include <dengine/de_net.h>

#include <string.h>

#define TOKEN_NONCE  0u
#define TOKEN_CIPHER 24u
#define TOKEN_TAG    48u
#define TOKEN_PLAIN  24u

DE_STATIC_ASSERT(TOKEN_TAG + DE__NET_TAG_BYTES == DE_NET_CONNECT_TOKEN_BYTES,
                 "connect token layout");
DE_STATIC_ASSERT(DE_NET_SERVER_KEY_BYTES == DE__NET_KEY_BYTES, "server key size");

static const u8 k_token_ad[] = "dengine connect token v1";

de_result de_net_connect_token_generate(const u8 server_key[DE_NET_SERVER_KEY_BYTES], u64 client_id,
                                        u64 expire_unix_s, u64 user,
                                        u8 out[DE_NET_CONNECT_TOKEN_BYTES]) {
    if (server_key == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_net_connect_token_generate: NULL argument");
    de_result r = de__net_random(out + TOKEN_NONCE, DE__NET_NONCE_BYTES);
    if (r != DE_OK) {
        memset(out, 0, DE_NET_CONNECT_TOKEN_BYTES);
        return r;
    }
    u8 plain[TOKEN_PLAIN];
    de__net_put_u64(plain, client_id);
    de__net_put_u64(plain + 8, expire_unix_s);
    de__net_put_u64(plain + 16, user);
    de__net_seal(out + TOKEN_CIPHER, out + TOKEN_TAG, server_key, out + TOKEN_NONCE, k_token_ad,
                 (u32)(sizeof k_token_ad - 1), plain, TOKEN_PLAIN, false);
    de__net_wipe(plain, sizeof plain);
    return DE_OK;
}

de_result de_net_connect_token_verify(const u8 server_key[DE_NET_SERVER_KEY_BYTES],
                                      const u8 token[DE_NET_CONNECT_TOKEN_BYTES], u64 now_unix_s,
                                      de_net_connect_token_info *out) {
    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (server_key == NULL || token == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_net_connect_token_verify: NULL argument");
    /* All zero: a direct connect. OR the bytes so the time does not depend on where the first
     * non-zero byte is. */
    u8 any = 0;
    for (u32 i = 0; i < DE_NET_CONNECT_TOKEN_BYTES; ++i)
        any |= token[i];
    if (any == 0)
        return DE_ERR_NOT_FOUND;
    u8 plain[TOKEN_PLAIN];
    if (!de__net_open(plain, token + TOKEN_TAG, server_key, token + TOKEN_NONCE, k_token_ad,
                      (u32)(sizeof k_token_ad - 1), token + TOKEN_CIPHER, TOKEN_PLAIN, false))
        return DE_ERR_PARSE;
    de_net_connect_token_info info;
    info.client_id = de__net_get_u64(plain);
    info.expire_unix_s = de__net_get_u64(plain + 8);
    info.user = de__net_get_u64(plain + 16);
    de__net_wipe(plain, sizeof plain);
    if (now_unix_s >= info.expire_unix_s)
        return DE_ERR_TIMEOUT;
    if (out != NULL)
        *out = info;
    return DE_OK;
}

de_result de_net_server_key_generate(u8 out[DE_NET_SERVER_KEY_BYTES]) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_net_server_key_generate: NULL argument");
    return de__net_random(out, DE_NET_SERVER_KEY_BYTES);
}
