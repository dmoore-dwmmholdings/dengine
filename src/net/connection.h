/* connection.h: connection packets, the challenge handshake and the connection lifecycle
 * (DENG-52, SPEC §16.1). Internal to the net module.
 *
 * ==================================================================================================
 * Packet framing
 * ==================================================================================================
 *
 * Every packet starts with a type byte: the low 4 bits are the type, the high 4 bits are reserved
 * and must be 0. All integers are little-endian; `seq` is a LEB128 varint (1 to 10 bytes,
 * minimal encoding only). `pid` is the protocol id (u64: game id and protocol version), which
 * both sides know; it is on the wire only in REQUEST and is part of the associated data of every
 * sealed packet, so a packet of another game or version never authenticates.
 *
 *   type        dir  layout                                                         sealed with
 *   1 REQUEST   C>S  type | pid (8) | flags (1) | client_pk (32) | token (64) |      nothing
 *                    zero padding to DE__NET_REQUEST_BYTES (160)
 *   2 CHALLENGE S>C  type | seq | server_pk (32) | {epoch (8), cookie (32)} | tag   s2c, domain 1
 *   3 RESPONSE  C>S  type | seq | client_pk (32) | epoch (8) | cookie (32) |         c2s
 *                    {token (64), flags (1)} | tag
 *   4 ACCEPT    S>C  type | seq | {slot (4), flags (1)} | tag                          s2c
 *   5 DENIED    S>C  type | reason (1) | proof (16)                                   (proof)
 *   6 KEEPALIVE both type | seq | {} | tag                                            session
 *   7 PAYLOAD   both type | seq | {data (1 .. DE__NET_PAYLOAD_MAX)} | tag             session
 *   8 DISCONNECT both type | seq | {code (1)} | tag                                   session
 *
 * `{...}` is the sealed body: XChaCha20-Poly1305 ciphertext followed by the 16-byte tag. The
 * nonce is `seq | direction | domain` (crypto.h); the associated data is `pid | type | the
 * plaintext fields between seq and the body` (server_pk; client_pk, epoch, cookie), and for a
 * CHALLENGE also the BLAKE2b-256 digest of the REQUEST it answers (not on the wire; the client
 * keeps the digest of what it sent). So every byte of a sealed packet is authenticated, and a
 * CHALLENGE only opens for the exact REQUEST the client sent: a byte changed on the way, in
 * either packet, makes the client drop the CHALLENGE.
 * "session" means the connection's key for that direction: AEAD, or, in a Dev/Debug session that
 * both sides opened with encryption off, the body in plaintext with a keyed BLAKE2b tag.
 *
 * REQUEST flags bit 0: the client asks for plaintext session packets (Dev/Debug). ACCEPT flags
 * bit 0: the session is plaintext (only when both sides asked). Other bits must be 0.
 *
 * ==================================================================================================
 * Handshake (at most 3 round trips; normally 2, then data)
 * ==================================================================================================
 *
 *   1. Client: new ephemeral X25519 key pair; REQUEST with the connect token (or 64 zero bytes
 *      for a direct IP connect). Padded so that no answer is bigger than the request: the
 *      server cannot be used to amplify a spoofed flood.
 *   2. Server, keeping no per-client state: checks size, pid, padding and the token (forged,
 *      expired, direct connect refused, server full, client id already connected: DENIED). Then
 *      X25519 of its epoch key and client_pk, the KDF (crypto.h) and a CHALLENGE sealed with s2c.
 *      The cookie is keyed BLAKE2b(epoch secret, client address | epoch | client_pk): only the
 *      holder of the address that sent the REQUEST receives it. The epoch secret and the epoch
 *      X25519 key pair rotate every DE__NET_EPOCH_NS; the current and the previous epoch are
 *      accepted, older cookies are not, and the old secrets are wiped.
 *   3. Client: X25519 of its secret and server_pk, the same KDF, opens the CHALLENGE (this
 *      authenticates the server's answer), wipes its secret, sends RESPONSE sealed with c2s.
 *   4. Server: recomputes the cookie (cheap, before any X25519), then the keys, opens the
 *      RESPONSE, checks the token again, takes a slot from the pool and answers ACCEPT. The
 *      connection exists from here; a repeated RESPONSE from that address gets the ACCEPT again.
 *   5. Client: opens ACCEPT, CONNECTED.
 *
 * DENIED is not sealed (the server has no key when it refuses a token). Its proof is
 * BLAKE2b-128 keyed with the client's ephemeral public key over `pid | type | reason | context`,
 * the context being the REQUEST digest (a DENIED answering a REQUEST) or the cookie (answering a
 * RESPONSE). A changed byte in the DENIED or in the packet it answers fails it, and a host that
 * never saw the REQUEST cannot forge it. A host that did see it can; it can also drop packets,
 * so nothing is lost.
 *
 * ==================================================================================================
 * Lifecycle
 * ==================================================================================================
 *
 *   client: DISCONNECTED -> SENDING_REQUEST -> SENDING_RESPONSE -> CONNECTED -> DISCONNECTING
 *           -> DISCONNECTED. REQUEST and RESPONSE are resent every `net.handshake_resend_ms`
 *           (100 ms); the handshake fails after `net.connect_timeout_s` (5 s).
 *   server: a pool of `max_peers` slots, allocated at create. A slot is FREE, CONNECTED or
 *           DISCONNECTING.
 *   both:   a connection times out after `net.timeout_s` (10 s) without a valid packet; a side
 *           that sent nothing for `net.keepalive_ms` (1000 ms) sends a KEEPALIVE. A disconnect
 *           sends DISCONNECT three times, one per update (destroy sends the rest at once).
 *
 * Time comes from an injectable clock (de__net_clock), so tests do not wait. Nothing here
 * allocates after create: receive buffers, the slot pool and the send buffer are taken then.
 * A dropped packet only counts (de__net_conn_stats); it never changes state and never sets the
 * error context.
 */
#ifndef DE_NET_CONNECTION_H
#define DE_NET_CONNECTION_H

#include "net/crypto.h"
#include "net/net_internal.h"

/* ---------- Packet constants ---------- */

typedef enum de__net_packet_type {
    DE__NET_PACKET_REQUEST = 1,
    DE__NET_PACKET_CHALLENGE = 2,
    DE__NET_PACKET_RESPONSE = 3,
    DE__NET_PACKET_ACCEPT = 4,
    DE__NET_PACKET_DENIED = 5,
    DE__NET_PACKET_KEEPALIVE = 6,
    DE__NET_PACKET_PAYLOAD = 7,
    DE__NET_PACKET_DISCONNECT = 8,
    DE__NET_PACKET_TYPE_COUNT_
} de__net_packet_type;

/** Largest datagram the connection layer sends or accepts (DENG-54 fragments messages to fit). */
#define DE__NET_PACKET_MAX 1500u
/** Largest PAYLOAD body. */
#define DE__NET_PAYLOAD_MAX (DE__NET_PACKET_MAX - 1u - DE__NET_VARINT_MAX - DE__NET_TAG_BYTES)
/** Size of every REQUEST (fixed, padded). */
#define DE__NET_REQUEST_BYTES 160u
/** Size of every DENIED. */
#define DE__NET_DENIED_BYTES 18u
/** Size of a connect token. */
#define DE__NET_TOKEN_BYTES 64u
/** Length of a challenge epoch: the server's cookie secret and X25519 key rotate this often. */
#define DE__NET_EPOCH_NS (10ull * 1000000000ull)
/** How often a disconnect sends DISCONNECT. */
#define DE__NET_DISCONNECT_COPIES 3u

/** REQUEST / ACCEPT flags. */
#define DE__NET_FLAG_PLAINTEXT 0x01u

/** Why a server refuses a client (the DENIED reason byte). */
typedef enum de__net_deny_reason {
    DE__NET_DENY_NONE = 0,
    DE__NET_DENY_SERVER_FULL = 1,
    DE__NET_DENY_TOKEN_INVALID = 2, /**< forged, changed, or another server's key */
    DE__NET_DENY_TOKEN_EXPIRED = 3,
    DE__NET_DENY_DIRECT_REFUSED = 4,    /**< all-zero token, and the server needs one */
    DE__NET_DENY_ALREADY_CONNECTED = 5, /**< that client id has a connection */
    DE__NET_DENY_REASON_COUNT_
} de__net_deny_reason;

/* ---------- Packet codec (handshake.c; DENG-58 fuzzes it) ---------- */

/** A REQUEST. */
typedef struct de__net_request {
    u64 protocol_id;
    u8 flags;
    u8 client_pk[DE__NET_KEY_BYTES];
    u8 token[DE__NET_TOKEN_BYTES];
} de__net_request;

/** Writes a REQUEST into `out` (DE__NET_REQUEST_BYTES) and returns DE__NET_REQUEST_BYTES. */
u32 de__net_request_write(u8 *out, const de__net_request *r);

/** Reads a REQUEST: exact size, type byte, zero padding, known flags. The protocol id is read, not
 * checked. False for anything else. */
bool de__net_request_read(const u8 *in, u32 bytes, de__net_request *out);

/** BLAKE2b-256 of a REQUEST's DE__NET_REQUEST_BYTES bytes (CHALLENGE binding, DENIED context). */
void de__net_request_digest(u8 out[DE__NET_KEY_BYTES], const u8 *request);

/** Writes a DENIED for the client whose ephemeral public key is `client_pk`, answering the packet
 * whose context (REQUEST digest or cookie) is `context`; returns DE__NET_DENIED_BYTES. */
u32 de__net_denied_write(u8 *out, u64 protocol_id, de__net_deny_reason reason,
                         const u8 client_pk[DE__NET_KEY_BYTES],
                         const u8 context[DE__NET_KEY_BYTES]);

/** Reads a DENIED meant for `client_pk` and `context`: size, type, a known reason, and the proof
 * (constant time). False for anything else. */
bool de__net_denied_read(const u8 *in, u32 bytes, u64 protocol_id,
                         const u8 client_pk[DE__NET_KEY_BYTES], const u8 context[DE__NET_KEY_BYTES],
                         de__net_deny_reason *reason);

/** Size of the plaintext fields between seq and the sealed body, by type (32 for CHALLENGE, 72
 * for RESPONSE, else 0). */
u32 de__net_packet_extra_bytes(de__net_packet_type type);

/**
 * Writes a sealed packet: `type | seq | extra | seal(body) | tag`, `extra` being
 * de__net_packet_extra_bytes(type) bytes. `bind` (NULL, or DE__NET_KEY_BYTES bytes) is added to
 * the associated data without going on the wire (the CHALLENGE's REQUEST digest). Returns the
 * length, or 0 when it would not fit `cap` or the type is not a sealed type.
 */
u32 de__net_packet_seal(u8 *out, u32 cap, de__net_packet_type type, u64 protocol_id, u64 seq,
                        const u8 *extra, const u8 *bind, const u8 *body, u32 body_bytes,
                        const u8 key[DE__NET_KEY_BYTES], de__net_dir dir,
                        de__net_nonce_domain domain, bool plaintext_mode);

/** A sealed packet split into its fields (pointers into the datagram). */
typedef struct de__net_packet {
    de__net_packet_type type;
    u64 seq;
    const u8 *extra; /**< de__net_packet_extra_bytes(type) bytes */
    u8 *body;        /**< sealed body; de__net_packet_open opens it in place */
    u32 body_bytes;
    const u8 *tag;
} de__net_packet;

/**
 * Splits a datagram of a sealed type. Checks the type byte, the varint and the body size the
 * type allows (CHALLENGE 40, RESPONSE 65, ACCEPT 5, KEEPALIVE 0, DISCONNECT 1, PAYLOAD 1 to
 * DE__NET_PAYLOAD_MAX). False for anything else, including REQUEST and DENIED.
 */
bool de__net_packet_parse(u8 *in, u32 bytes, de__net_packet *out);

/** Opens `p->body` in place with `key` (and the same `bind` the sender used, or NULL). False when
 * it does not authenticate (the body is then garbage and must not be used). */
bool de__net_packet_open(de__net_packet *p, u64 protocol_id, const u8 *bind,
                         const u8 key[DE__NET_KEY_BYTES], de__net_dir dir,
                         de__net_nonce_domain domain, bool plaintext_mode);

/** The challenge cookie: keyed BLAKE2b-256(secret, address | epoch | client_pk). */
void de__net_cookie(u8 out[DE__NET_KEY_BYTES], const u8 secret[DE__NET_KEY_BYTES],
                    const de_net_address *from, u64 epoch, const u8 client_pk[DE__NET_KEY_BYTES]);

/* ---------- Clock, timing, callbacks, stats ---------- */

/** Injectable time. `(de__net_clock){0}` is the real clock. */
typedef struct de__net_clock {
    /** Monotonic nanoseconds; NULL = de_time_now_ns. */
    u64 (*now_ns)(void *user);
    /** Wall clock, Unix seconds (token expiry); NULL = time(). */
    u64 (*unix_s)(void *user);
    void *user;
} de__net_clock;

/** Timing of a connection in nanoseconds, from the desc or the cvars. */
typedef struct de__net_timing {
    u64 resend_ns;          /**< `net.handshake_resend_ms`, REQUEST / RESPONSE resend */
    u64 connect_timeout_ns; /**< `net.connect_timeout_s` */
    u64 idle_timeout_ns;    /**< `net.timeout_s` */
    u64 keepalive_ns;       /**< `net.keepalive_ms` */
} de__net_timing;

/** Why a connection ended (callbacks, de__net_client_end_reason). */
typedef enum de__net_end_reason {
    DE__NET_END_NONE = 0,
    DE__NET_END_LOCAL,           /**< this side disconnected (disconnect, kick, destroy) */
    DE__NET_END_REMOTE,          /**< the peer sent DISCONNECT; `detail` = its code */
    DE__NET_END_TIMEOUT,         /**< nothing valid heard for the idle timeout */
    DE__NET_END_CONNECT_TIMEOUT, /**< client: the handshake did not finish in time */
    DE__NET_END_DENIED,          /**< client: DENIED; `detail` = de__net_deny_reason */
    DE__NET_END_ERROR            /**< client: the OS random source or the socket failed */
} de__net_end_reason;

/**
 * Internal events, for S32 (DENG-115) to turn into de_net_event. Called from update (and from
 * disconnect / kick for LOCAL). `slot` is the server's slot index; a client gets the index the
 * server gave it. Any callback may be NULL.
 */
typedef struct de__net_callbacks {
    void (*connected)(void *user, u32 slot);
    /** Not called for timeouts (see timed_out). */
    void (*disconnected)(void *user, u32 slot, de__net_end_reason reason, u8 detail);
    /** TIMEOUT or CONNECT_TIMEOUT. */
    void (*timed_out)(void *user, u32 slot, de__net_end_reason reason);
    /** A PAYLOAD body, valid during the call only. */
    void (*payload)(void *user, u32 slot, const u8 *data, u32 bytes);
    void *user;
} de__net_callbacks;

/** Counters of a client or a server. Plain u64s: read them on the thread that runs update. */
typedef struct de__net_conn_stats {
    u64 packets_received;   /**< datagrams read from the socket */
    u64 packets_sent;       /**< datagrams handed to the socket */
    u64 dropped_malformed;  /**< size, type, varint, padding, flags or protocol id wrong */
    u64 dropped_auth;       /**< tag or DENIED proof did not verify */
    u64 dropped_replay;     /**< sequence already received, or older than the window */
    u64 dropped_cookie;     /**< RESPONSE with an unknown epoch or a wrong cookie */
    u64 dropped_unexpected; /**< well-formed, but not for this state or from an unknown address */
    u64 requests;           /**< server: REQUESTs that passed the format checks */
    u64 challenges_sent;
    u64 denied;         /**< server: DENIED sent; client: DENIED accepted */
    u64 tokens_invalid; /**< server: forged or changed tokens */
    u64 tokens_expired;
    u64 payloads_received;
    u64 payloads_sent;
} de__net_conn_stats;

/** Sum of the dropped_* counters. */
u64 de__net_conn_stats_dropped(const de__net_conn_stats *s);

/* ---------- Server ---------- */

typedef struct de__net_server de__net_server;

/** Options of a server. `(de__net_server_desc){0}` with a socket is valid: one peer, a zero
 * server key (only direct connects make sense then), encrypted, cvar timing, real clock. */
typedef struct de__net_server_desc {
    const de_allocator *allocator; /**< NULL = de_heap_allocator() */
    de__socket *socket;            /**< bound socket; not owned, must outlive the server */
    u64 protocol_id;
    u32 max_peers;                     /**< slots; 0 = 1. At most 4096. */
    u8 server_key[DE__NET_KEY_BYTES];  /**< opens connect tokens */
    bool deny_direct_connect;          /**< refuse all-zero tokens (default: allow, DENG-2 Q15) */
    bool disable_encryption;           /**< Dev/Debug only; ignored in Release and Shipping */
    u32 recv_batch;                    /**< datagrams per socket receive; 0 = 32 */
    u32 idle_timeout_ms, keepalive_ms; /**< 0 = cvar */
    de__net_clock clock;
    de__net_callbacks callbacks;
} de__net_server_desc;

/** State of a server slot. */
typedef enum de__net_slot_state {
    DE__NET_SLOT_FREE = 0,
    DE__NET_SLOT_CONNECTED,
    DE__NET_SLOT_DISCONNECTING
} de__net_slot_state;

/** What a slot holds (de__net_server_slot). */
typedef struct de__net_slot_info {
    de__net_slot_state state;
    u32 generation; /**< increments each time the slot is taken: (slot, generation) names a peer */
    de_net_address address;
    u64 client_id; /**< from the token; 0 for a direct connect */
    u64 user;      /**< from the token */
    bool plaintext;
} de__net_slot_info;

/** Creates a server on `desc->socket`. Allocates everything it will use. */
de_result de__net_server_create(const de__net_server_desc *desc, de__net_server **out);
/** Sends the outstanding DISCONNECTs of every connection, wipes every secret, frees. NULL is a
 * no-op. Calls no callbacks. */
void de__net_server_destroy(de__net_server *s);
/** Receives and handles every waiting datagram, then runs the timers. Once per frame or tick. */
void de__net_server_update(de__net_server *s);
/** Sends a PAYLOAD to a connected slot. DE_ERR_NOT_FOUND (slot not connected),
 * DE_ERR_INVALID_ARGUMENT (0 or more than DE__NET_PAYLOAD_MAX bytes), socket errors. */
de_result de__net_server_send(de__net_server *s, u32 slot, const void *data, u32 bytes);
/** Disconnects a slot: DISCONNECT with `code`, three times; `disconnected` (LOCAL) is called
 * now. No-op for a slot that is not connected. */
void de__net_server_kick(de__net_server *s, u32 slot, u8 code);
/** A slot's state; FREE with zeros for an index out of range. */
de__net_slot_info de__net_server_slot(const de__net_server *s, u32 slot);
/** Number of CONNECTED slots. */
u32 de__net_server_connected_count(const de__net_server *s);
u32 de__net_server_max_peers(const de__net_server *s);
const de__net_conn_stats *de__net_server_stats(const de__net_server *s);
/** Test and debug access to a slot's keys (NULL out of range). */
const de__net_keys *de__net_server_slot_keys(const de__net_server *s, u32 slot);

/* ---------- Client ---------- */

typedef struct de__net_client de__net_client;

typedef enum de__net_client_state {
    DE__NET_CLIENT_DISCONNECTED = 0,
    DE__NET_CLIENT_SENDING_REQUEST,
    DE__NET_CLIENT_SENDING_RESPONSE,
    DE__NET_CLIENT_CONNECTED,
    DE__NET_CLIENT_DISCONNECTING
} de__net_client_state;

/** Options of a client. `(de__net_client_desc){0}` with a socket is valid. */
typedef struct de__net_client_desc {
    const de_allocator *allocator; /**< NULL = de_heap_allocator() */
    de__socket *socket;            /**< bound socket; not owned, must outlive the client */
    u64 protocol_id;
    bool disable_encryption; /**< Dev/Debug only (ask the server for plaintext packets) */
    u32 recv_batch;          /**< datagrams per socket receive; 0 = 8 */
    u32 resend_ms, connect_timeout_ms, idle_timeout_ms, keepalive_ms; /**< 0 = cvar */
    de__net_clock clock;
    de__net_callbacks callbacks;
} de__net_client_desc;

de_result de__net_client_create(const de__net_client_desc *desc, de__net_client **out);
/** Sends the outstanding DISCONNECTs, wipes every secret, frees. NULL is a no-op. */
void de__net_client_destroy(de__net_client *c);
/**
 * Starts a handshake with `server`. `token` NULL (or 64 zero bytes) asks for a direct connect.
 * From DISCONNECTED only (DE_ERR_BUSY otherwise). Sends the first REQUEST now.
 * DE_ERR_INTERNAL when the OS random source fails.
 */
de_result de__net_client_connect(de__net_client *c, const de_net_address *server,
                                 const u8 token[DE__NET_TOKEN_BYTES]);
/** Receives and handles every waiting datagram, then runs the timers. */
void de__net_client_update(de__net_client *c);
/** Sends a PAYLOAD. DE_ERR_NOT_FOUND when not connected; DE_ERR_INVALID_ARGUMENT for 0 or more
 * than DE__NET_PAYLOAD_MAX bytes. */
de_result de__net_client_send(de__net_client *c, const void *data, u32 bytes);
/** Disconnects: DISCONNECT with `code` now and on the next two updates, then DISCONNECTED. A
 * handshake in progress just stops. Calls `disconnected` (LOCAL) when it was connected. */
void de__net_client_disconnect(de__net_client *c, u8 code);
de__net_client_state de__net_client_get_state(const de__net_client *c);
/** Why the last connection ended or the last handshake failed; `detail` as in the callback. */
de__net_end_reason de__net_client_end_reason(const de__net_client *c, u8 *detail);
/** The slot index the server gave this client (valid when CONNECTED). */
u32 de__net_client_slot(const de__net_client *c);
const de__net_conn_stats *de__net_client_stats(const de__net_client *c);
/** Test and debug access to the client's keys. */
const de__net_keys *de__net_client_keys(const de__net_client *c);
/** True when the session sends plaintext packets (both sides turned encryption off). */
bool de__net_client_plaintext(const de__net_client *c);

#endif /* DE_NET_CONNECTION_H */
