/* crypto.h: the net module's crypto primitives (DENG-52, SPEC §16.1, §22).
 *
 * A thin layer over Monocypher, the only crypto library the engine uses: no primitive here is the
 * engine's own. Everything above it (connect tokens, the handshake, the connection layer) calls
 * these functions and never includes <monocypher.h> itself.
 *
 *   - Packet AEAD: XChaCha20-Poly1305 (crypto_aead_lock / crypto_aead_unlock). The 24-byte nonce
 *     is built from the packet sequence, a direction bit and a domain byte (de__net_nonce), so a
 *     key never sees the same nonce twice. The associated data is the protocol id, the type byte
 *     and any plaintext header fields of the packet (de__net_ad).
 *   - Plaintext-with-MAC mode for Dev/Debug sessions that turn encryption off: the body stays
 *     readable and a 16-byte keyed BLAKE2b tag still authenticates it.
 *   - Key agreement: X25519 (crypto_x25519), then a keyed BLAKE2b KDF that gives one key per
 *     direction (client to server, server to client). With a connect token, the token's
 *     connection key is mixed in first (de__net_derive_keys_psk, DENG-378): that is what
 *     authenticates the server to the client and the client to the server.
 *   - Replay window: 256 sequences behind the newest one.
 *   - Constant-time compares (crypto_verify16/32) and crypto_wipe for every secret.
 *   - OS randomness (getentropy, rand_s) for ephemeral keys, token nonces and server secrets.
 *
 * Nothing here allocates. Nothing here sets the error context on the per-packet paths (a dropped
 * packet is a counter, not an error).
 */
#ifndef DE_NET_CRYPTO_H
#define DE_NET_CRYPTO_H

#include <dengine/de_base.h>

#define DE__NET_KEY_BYTES   32u  /**< symmetric keys, X25519 keys and shared secrets */
#define DE__NET_TAG_BYTES   16u  /**< AEAD / MAC tag */
#define DE__NET_NONCE_BYTES 24u  /**< XChaCha20 nonce */
#define DE__NET_AD_MAX      192u /**< largest associated data a packet uses (RESPONSE: 177) */

/** Direction bit of the nonce: which side sealed the packet. */
typedef enum de__net_dir {
    DE__NET_DIR_C2S = 0, /**< client to server */
    DE__NET_DIR_S2C = 1  /**< server to client */
} de__net_dir;

/** Domain byte of the nonce. Session packets and the server's stateless CHALLENGE use the same
 * server-to-client key; the domain keeps their nonces apart (the CHALLENGE sequence is a
 * server-wide counter, not the connection's). */
typedef enum de__net_nonce_domain {
    DE__NET_DOMAIN_SESSION = 0,
    DE__NET_DOMAIN_CHALLENGE = 1
} de__net_nonce_domain;

/** The two directional keys of one connection. */
typedef struct de__net_keys {
    u8 c2s[DE__NET_KEY_BYTES];
    u8 s2c[DE__NET_KEY_BYTES];
} de__net_keys;

/* ---------- AEAD ---------- */

/** Builds the 24-byte nonce: sequence (u64 LE) | direction | domain | 14 zero bytes. */
void de__net_nonce(u8 out[DE__NET_NONCE_BYTES], u64 sequence, de__net_dir dir,
                   de__net_nonce_domain domain);

/** Builds the associated data `protocol_id (u64 LE) | type | extra[0 .. extra_bytes)` into
 * `out` (DE__NET_AD_MAX bytes) and returns its length; 0 when it does not fit. */
u32 de__net_ad(u8 *out, u64 protocol_id, u8 type, const u8 *extra, u32 extra_bytes);

/**
 * Seals `bytes` of `plain` into `cipher` (same size; may alias `plain`) and writes the 16-byte
 * tag. `plaintext_mode` (Dev/Debug sessions with encryption off) copies the body unchanged and
 * writes a keyed BLAKE2b tag over nonce, AD and body instead: still authenticated, not secret.
 */
void de__net_seal(u8 *cipher, u8 tag[DE__NET_TAG_BYTES], const u8 key[DE__NET_KEY_BYTES],
                  const u8 nonce[DE__NET_NONCE_BYTES], const u8 *ad, u32 ad_bytes, const u8 *plain,
                  u32 bytes, bool plaintext_mode);

/** Opens what de__net_seal made. Returns false (and leaves `plain` unspecified, never partially
 * trusted) when the tag does not match: any changed byte of key, nonce, AD, body or tag. */
bool de__net_open(u8 *plain, const u8 tag[DE__NET_TAG_BYTES], const u8 key[DE__NET_KEY_BYTES],
                  const u8 nonce[DE__NET_NONCE_BYTES], const u8 *ad, u32 ad_bytes, const u8 *cipher,
                  u32 bytes, bool plaintext_mode);

/* ---------- Key agreement ---------- */

/** A new X25519 key pair from OS randomness. DE_ERR_INTERNAL when the OS has no randomness. */
de_result de__net_x25519_keypair(u8 secret[DE__NET_KEY_BYTES], u8 public_key[DE__NET_KEY_BYTES]);

/** The public key of `secret`. */
void de__net_x25519_public(u8 public_key[DE__NET_KEY_BYTES], const u8 secret[DE__NET_KEY_BYTES]);

/**
 * X25519 of `my_secret` and `their_public`, then the KDF:
 *
 *     c2s = BLAKE2b-256(key = shared, "dengine net c2s" | protocol_id | client_pk | server_pk)
 *     s2c = BLAKE2b-256(key = shared, "dengine net s2c" | protocol_id | client_pk | server_pk)
 *
 * Both sides pass the same client and server public keys, so they get the same pair. The raw
 * shared secret is wiped before return. Returns false (keys wiped) when the shared secret is all
 * zero, which a low-order public key from a malicious peer gives.
 */
bool de__net_derive_keys(de__net_keys *out, const u8 my_secret[DE__NET_KEY_BYTES],
                         const u8 their_public[DE__NET_KEY_BYTES], u64 protocol_id,
                         const u8 client_pk[DE__NET_KEY_BYTES],
                         const u8 server_pk[DE__NET_KEY_BYTES]);

/**
 * de__net_derive_keys bound to a pre-shared connection key (DENG-378). The connection key comes
 * from the connect token: the client has it in clear, the server reads it from the sealed part.
 * The X25519 shared secret is mixed with it first:
 *
 *     prk = BLAKE2b-256(key = psk, "dengine net psk\0" | shared)
 *
 * and `prk` then takes the place of `shared` in the two KDF lines above. Without `psk` an
 * attacker cannot get the keys even when it made the X25519 exchange itself (a man in the middle),
 * and without the ephemeral X25519 secrets a later leak of `psk` does not open recorded sessions.
 * `psk` NULL is de__net_derive_keys (a direct connect: no connection key). `prk` and the raw shared
 * secret are wiped before return. False (keys wiped) for an all-zero shared secret.
 */
bool de__net_derive_keys_psk(de__net_keys *out, const u8 my_secret[DE__NET_KEY_BYTES],
                             const u8 their_public[DE__NET_KEY_BYTES], u64 protocol_id,
                             const u8 client_pk[DE__NET_KEY_BYTES],
                             const u8 server_pk[DE__NET_KEY_BYTES], const u8 *psk);

/** Keyed BLAKE2b with an output of `hash_bytes` (1 to 64). Used for the challenge cookie, the
 * DENIED proof and the plaintext-mode MAC. */
void de__net_mac(u8 *hash, u32 hash_bytes, const u8 *key, u32 key_bytes, const u8 *msg,
                 u32 msg_bytes);

/** Unkeyed BLAKE2b with an output of `hash_bytes` (1 to 64). */
void de__net_hash(u8 *hash, u32 hash_bytes, const u8 *msg, u32 msg_bytes);

/* ---------- Compares, wiping, randomness ---------- */

/** Constant-time compares: true when equal. */
bool de__net_equal16(const u8 a[16], const u8 b[16]);
bool de__net_equal32(const u8 a[32], const u8 b[32]);

/** Zeroes `bytes` at `p` in a way the compiler cannot remove (crypto_wipe). */
void de__net_wipe(void *p, size_t bytes);

/** Fills `out` with OS randomness (getentropy / rand_s). DE_ERR_INTERNAL when the OS fails
 * (`out` is zeroed then, never left half random). No allocation. */
de_result de__net_random(void *out, size_t bytes);

/* ---------- Replay window ---------- */

/** Number of sequences the window remembers behind the newest one. */
#define DE__NET_REPLAY_WINDOW 256u

/**
 * Remembers which of the last DE__NET_REPLAY_WINDOW sequences were received. Bit i of `bits`
 * stands for sequence `newest - i`. `(de__net_replay){0}` is "nothing received yet".
 */
typedef struct de__net_replay {
    u64 newest;
    u64 bits[DE__NET_REPLAY_WINDOW / 64u];
    bool any;
} de__net_replay;

/** True when `seq` is new: newer than everything seen, or inside the window and not seen yet.
 * False for a repeat, or a sequence too old for the window. Does not change the window. */
bool de__net_replay_check(const de__net_replay *w, u64 seq);

/** Records `seq` as received. Call it only after the packet authenticated (a forged packet must
 * not move the window). */
void de__net_replay_mark(de__net_replay *w, u64 seq);

/* ---------- Varints ---------- */

/** Largest varint (a u64 in LEB128). */
#define DE__NET_VARINT_MAX 10u

/** Writes `v` as LEB128 (7 bits per byte, low group first) and returns its length (1 to 10). */
u32 de__net_varint_write(u8 *out, u64 v);

/** Reads a LEB128 u64 from `in[0 .. avail)`. Returns its length, or 0 for a truncated, overlong
 * (non-minimal, more than 10 bytes) or out-of-range encoding. */
u32 de__net_varint_read(const u8 *in, u32 avail, u64 *v);

/* ---------- Little-endian helpers ---------- */

void de__net_put_u32(u8 *p, u32 v);
void de__net_put_u64(u8 *p, u64 v);
u32 de__net_get_u32(const u8 *p);
u64 de__net_get_u64(const u8 *p);

#endif /* DE_NET_CRYPTO_H */
