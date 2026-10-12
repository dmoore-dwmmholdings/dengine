/* crypto.c: the net module's crypto primitives over Monocypher (DENG-52). See crypto.h. */

/* getentropy() is not POSIX: ask for the platform extensions (glibc >= 2.25, macOS 10.12). */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
/* rand_s (the UCRT wrapper around RtlGenRandom, a CSPRNG) is declared only with _CRT_RAND_S. */
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S
#endif

#include "net/crypto.h"

#include <monocypher.h>

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* rand_s is in <stdlib.h>. */
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/random.h>
#else
#include <unistd.h>
#endif

/* ---------- Little-endian helpers ---------- */

void de__net_put_u32(u8 *p, u32 v) {
    for (u32 i = 0; i < 4; ++i)
        p[i] = (u8)(v >> (8u * i));
}

void de__net_put_u64(u8 *p, u64 v) {
    for (u32 i = 0; i < 8; ++i)
        p[i] = (u8)(v >> (8u * i));
}

u32 de__net_get_u32(const u8 *p) {
    u32 v = 0;
    for (u32 i = 0; i < 4; ++i)
        v |= (u32)p[i] << (8u * i);
    return v;
}

u64 de__net_get_u64(const u8 *p) {
    u64 v = 0;
    for (u32 i = 0; i < 8; ++i)
        v |= (u64)p[i] << (8u * i);
    return v;
}

/* ---------- AEAD ---------- */

void de__net_nonce(u8 out[DE__NET_NONCE_BYTES], u64 sequence, de__net_dir dir,
                   de__net_nonce_domain domain) {
    memset(out, 0, DE__NET_NONCE_BYTES);
    de__net_put_u64(out, sequence);
    out[8] = (u8)dir;
    out[9] = (u8)domain;
}

u32 de__net_ad(u8 *out, u64 protocol_id, u8 type, const u8 *extra, u32 extra_bytes) {
    if (extra_bytes > DE__NET_AD_MAX - 9u)
        return 0;
    de__net_put_u64(out, protocol_id);
    out[8] = type;
    if (extra_bytes > 0)
        memcpy(out + 9, extra, extra_bytes);
    return 9u + extra_bytes;
}

/* The plaintext-mode tag: BLAKE2b-128 keyed with a key derived from the session key (a key is
 * never used by two different constructions), over nonce | AD | body. */
static void plaintext_tag(u8 tag[DE__NET_TAG_BYTES], const u8 key[DE__NET_KEY_BYTES],
                          const u8 nonce[DE__NET_NONCE_BYTES], const u8 *ad, u32 ad_bytes,
                          const u8 *body, u32 bytes) {
    static const u8 label[] = "dengine net plaintext mac";
    u8 mac_key[DE__NET_KEY_BYTES];
    crypto_blake2b_keyed(mac_key, sizeof mac_key, key, DE__NET_KEY_BYTES, label, sizeof label - 1);
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, DE__NET_TAG_BYTES, mac_key, sizeof mac_key);
    crypto_blake2b_update(&ctx, nonce, DE__NET_NONCE_BYTES);
    crypto_blake2b_update(&ctx, ad, ad_bytes);
    crypto_blake2b_update(&ctx, body, bytes);
    crypto_blake2b_final(&ctx, tag);
    crypto_wipe(mac_key, sizeof mac_key);
}

/* Monocypher may do pointer arithmetic on a buffer even when its size is 0; never hand it NULL. */
static u8 g_empty[1];

void de__net_seal(u8 *cipher, u8 tag[DE__NET_TAG_BYTES], const u8 key[DE__NET_KEY_BYTES],
                  const u8 nonce[DE__NET_NONCE_BYTES], const u8 *ad, u32 ad_bytes, const u8 *plain,
                  u32 bytes, bool plaintext_mode) {
    if (cipher == NULL || plain == NULL) {
        DE_ASSERT(bytes == 0);
        cipher = g_empty;
        plain = g_empty;
    }
    if (ad == NULL) {
        DE_ASSERT(ad_bytes == 0);
        ad = g_empty;
    }
    if (plaintext_mode) {
        if (bytes > 0 && cipher != plain)
            memmove(cipher, plain, bytes);
        plaintext_tag(tag, key, nonce, ad, ad_bytes, cipher, bytes);
        return;
    }
    crypto_aead_lock(cipher, tag, key, nonce, ad, ad_bytes, plain, bytes);
}

bool de__net_open(u8 *plain, const u8 tag[DE__NET_TAG_BYTES], const u8 key[DE__NET_KEY_BYTES],
                  const u8 nonce[DE__NET_NONCE_BYTES], const u8 *ad, u32 ad_bytes, const u8 *cipher,
                  u32 bytes, bool plaintext_mode) {
    u8 scratch[1];
    if (cipher == NULL || plain == NULL) {
        DE_ASSERT(bytes == 0);
        cipher = g_empty;
        plain = scratch;
    }
    if (ad == NULL) {
        DE_ASSERT(ad_bytes == 0);
        ad = g_empty;
    }
    if (plaintext_mode) {
        u8 want[DE__NET_TAG_BYTES];
        plaintext_tag(want, key, nonce, ad, ad_bytes, cipher, bytes);
        if (crypto_verify16(want, tag) != 0)
            return false;
        if (bytes > 0 && plain != cipher)
            memmove(plain, cipher, bytes);
        return true;
    }
    /* crypto_aead_unlock checks the tag before it decrypts, and writes nothing on failure. */
    return crypto_aead_unlock(plain, tag, key, nonce, ad, ad_bytes, cipher, bytes) == 0;
}

/* ---------- Key agreement ---------- */

void de__net_x25519_public(u8 public_key[DE__NET_KEY_BYTES], const u8 secret[DE__NET_KEY_BYTES]) {
    crypto_x25519_public_key(public_key, secret);
}

de_result de__net_x25519_keypair(u8 secret[DE__NET_KEY_BYTES], u8 public_key[DE__NET_KEY_BYTES]) {
    de_result r = de__net_random(secret, DE__NET_KEY_BYTES);
    if (r != DE_OK) {
        memset(public_key, 0, DE__NET_KEY_BYTES);
        return r;
    }
    crypto_x25519_public_key(public_key, secret);
    return DE_OK;
}

static void kdf_one(u8 out[DE__NET_KEY_BYTES], const u8 shared[DE__NET_KEY_BYTES],
                    const char label[16], u64 protocol_id, const u8 client_pk[DE__NET_KEY_BYTES],
                    const u8 server_pk[DE__NET_KEY_BYTES]) {
    u8 pid[8];
    de__net_put_u64(pid, protocol_id);
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, DE__NET_KEY_BYTES, shared, DE__NET_KEY_BYTES);
    crypto_blake2b_update(&ctx, (const u8 *)label, 16);
    crypto_blake2b_update(&ctx, pid, sizeof pid);
    crypto_blake2b_update(&ctx, client_pk, DE__NET_KEY_BYTES);
    crypto_blake2b_update(&ctx, server_pk, DE__NET_KEY_BYTES);
    crypto_blake2b_final(&ctx, out);
}

bool de__net_derive_keys(de__net_keys *out, const u8 my_secret[DE__NET_KEY_BYTES],
                         const u8 their_public[DE__NET_KEY_BYTES], u64 protocol_id,
                         const u8 client_pk[DE__NET_KEY_BYTES],
                         const u8 server_pk[DE__NET_KEY_BYTES]) {
    static const u8 zero[DE__NET_KEY_BYTES] = {0};
    u8 shared[DE__NET_KEY_BYTES];
    crypto_x25519(shared, my_secret, their_public);
    /* An all-zero result means a low-order point: the peer chose it, and the keys would be known
     * to anyone. */
    if (crypto_verify32(shared, zero) == 0) {
        crypto_wipe(shared, sizeof shared);
        crypto_wipe(out, sizeof *out);
        return false;
    }
    /* Labels are exactly 16 bytes (the NUL of the literal is not hashed). */
    kdf_one(out->c2s, shared, "dengine net c2s\0", protocol_id, client_pk, server_pk);
    kdf_one(out->s2c, shared, "dengine net s2c\0", protocol_id, client_pk, server_pk);
    crypto_wipe(shared, sizeof shared);
    return true;
}

void de__net_mac(u8 *hash, u32 hash_bytes, const u8 *key, u32 key_bytes, const u8 *msg,
                 u32 msg_bytes) {
    crypto_blake2b_keyed(hash, hash_bytes, key, key_bytes, msg, msg_bytes);
}

void de__net_hash(u8 *hash, u32 hash_bytes, const u8 *msg, u32 msg_bytes) {
    crypto_blake2b(hash, hash_bytes, msg, msg_bytes);
}

/* ---------- Compares, wiping, randomness ---------- */

bool de__net_equal16(const u8 a[16], const u8 b[16]) {
    return crypto_verify16(a, b) == 0;
}

bool de__net_equal32(const u8 a[32], const u8 b[32]) {
    return crypto_verify32(a, b) == 0;
}

void de__net_wipe(void *p, size_t bytes) {
    crypto_wipe(p, bytes);
}

de_result de__net_random(void *out, size_t bytes) {
    u8 *p = (u8 *)out;
    size_t done = 0;
#if defined(_WIN32)
    while (done < bytes) {
        unsigned int v = 0;
        if (rand_s(&v) != 0)
            break;
        size_t n = bytes - done < sizeof v ? bytes - done : sizeof v;
        memcpy(p + done, &v, n);
        crypto_wipe(&v, sizeof v);
        done += n;
    }
#else
    /* getentropy returns at most 256 bytes per call. */
    while (done < bytes) {
        size_t n = bytes - done < 256u ? bytes - done : 256u;
        if (getentropy(p + done, n) != 0)
            break;
        done += n;
    }
#endif
    if (done < bytes) {
        crypto_wipe(out, bytes);
        return DE_FAIL(DE_ERR_INTERNAL, "net: the OS random source failed");
    }
    return DE_OK;
}

/* ---------- Replay window ---------- */

bool de__net_replay_check(const de__net_replay *w, u64 seq) {
    if (!w->any || seq > w->newest)
        return true;
    u64 age = w->newest - seq;
    if (age >= DE__NET_REPLAY_WINDOW)
        return false;
    return (w->bits[age / 64u] & ((u64)1 << (age % 64u))) == 0;
}

/* Shifts the window so that bit i becomes bit i + n (older). */
static void replay_shift(de__net_replay *w, u64 n) {
    const u32 words = DE__NET_REPLAY_WINDOW / 64u;
    if (n >= DE__NET_REPLAY_WINDOW) {
        memset(w->bits, 0, sizeof w->bits);
        return;
    }
    u32 word_shift = (u32)(n / 64u);
    u32 bit_shift = (u32)(n % 64u);
    for (u32 i = words; i-- > 0;) {
        u64 v = 0;
        if (i >= word_shift) {
            u32 src = i - word_shift;
            v = w->bits[src] << bit_shift;
            if (bit_shift != 0 && src > 0)
                v |= w->bits[src - 1] >> (64u - bit_shift);
        }
        w->bits[i] = v;
    }
}

void de__net_replay_mark(de__net_replay *w, u64 seq) {
    if (!w->any) {
        memset(w->bits, 0, sizeof w->bits);
        w->any = true;
        w->newest = seq;
        w->bits[0] = 1;
        return;
    }
    if (seq > w->newest) {
        replay_shift(w, seq - w->newest);
        w->newest = seq;
        w->bits[0] |= 1;
        return;
    }
    u64 age = w->newest - seq;
    if (age < DE__NET_REPLAY_WINDOW)
        w->bits[age / 64u] |= (u64)1 << (age % 64u);
}

/* ---------- Varints ---------- */

u32 de__net_varint_write(u8 *out, u64 v) {
    u32 n = 0;
    do {
        u8 b = (u8)(v & 0x7Fu);
        v >>= 7;
        if (v != 0)
            b |= 0x80u;
        out[n++] = b;
    } while (v != 0);
    return n;
}

u32 de__net_varint_read(const u8 *in, u32 avail, u64 *v) {
    u64 r = 0;
    for (u32 i = 0; i < DE__NET_VARINT_MAX && i < avail; ++i) {
        u8 b = in[i];
        u64 group = (u64)(b & 0x7Fu);
        /* The 10th byte holds bit 63 only. */
        if (i == DE__NET_VARINT_MAX - 1u && group > 1u)
            return 0;
        r |= group << (7u * i);
        if ((b & 0x80u) == 0) {
            /* Minimal encodings only: a last byte of 0 (after the first) is overlong. */
            if (i > 0 && b == 0)
                return 0;
            *v = r;
            return i + 1u;
        }
    }
    return 0;
}
