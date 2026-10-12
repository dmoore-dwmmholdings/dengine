/* Smoke test for de_tp_monocypher: an XChaCha20-Poly1305 round trip, a rejected forgery, and an
 * X25519 key agreement. The vector tests are in tests/unit/net/test_crypto.c. */
#include <string.h>

#include <monocypher.h>

#include "de_test.h"

TEST(de_tp_smoke_monocypher, aead_roundtrip) {
    uint8_t key[32], nonce[24], mac[16];
    for (int i = 0; i < 32; ++i)
        key[i] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < 24; ++i)
        nonce[i] = (uint8_t)(i + 100);
    static const char msg[] = "monocypher smoke";
    uint8_t cipher[sizeof msg], plain[sizeof msg];
    crypto_aead_lock(cipher, mac, key, nonce, NULL, 0, (const uint8_t *)msg, sizeof msg);
    EXPECT_NE(memcmp(cipher, msg, sizeof msg), 0);
    ASSERT_EQ(crypto_aead_unlock(plain, mac, key, nonce, NULL, 0, cipher, sizeof cipher), 0);
    EXPECT_MEMEQ(plain, msg, sizeof msg);
    cipher[3] ^= 1u;
    EXPECT_EQ(crypto_aead_unlock(plain, mac, key, nonce, NULL, 0, cipher, sizeof cipher), -1);
}

TEST(de_tp_smoke_monocypher, x25519_agreement) {
    uint8_t sa[32], sb[32], pa[32], pb[32], ka[32], kb[32];
    for (int i = 0; i < 32; ++i) {
        sa[i] = (uint8_t)(i * 3 + 5);
        sb[i] = (uint8_t)(255 - i * 5);
    }
    crypto_x25519_public_key(pa, sa);
    crypto_x25519_public_key(pb, sb);
    crypto_x25519(ka, sa, pb);
    crypto_x25519(kb, sb, pa);
    EXPECT_EQ(crypto_verify32(ka, kb), 0);
    crypto_wipe(sa, sizeof sa);
    for (int i = 0; i < 32; ++i)
        EXPECT_EQ(sa[i], 0);
}
