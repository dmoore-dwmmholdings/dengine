/* de_net.h — transport + three session models: server-authoritative, rollback, lockstep. SPEC.md
 * §16. */
#ifndef DE_NET_H
#define DE_NET_H
#include "de_base.h"
#include "de_ecs.h"
DE_EXTERN_C_BEGIN

/* ---------- Transport: network addresses (DENG-51) ---------- */

/** Address family of a de_net_address. */
typedef enum de_net_family {
    DE_NET_FAMILY_NONE = 0, /**< no address (a zeroed de_net_address) */
    DE_NET_FAMILY_IPV4 = 4,
    DE_NET_FAMILY_IPV6 = 6
} de_net_family;

/**
 * An IPv4 or IPv6 address and a UDP port. Plain data: copy it, compare it with
 * de_net_address_equal (never memcmp), use de_net_address_hash as a hash-map key.
 * `(de_net_address){0}` is "no address" (DE_NET_FAMILY_NONE).
 *
 * The transport never hands out IPv4-mapped IPv6 addresses (`::ffff:a.b.c.d`): a datagram that
 * reaches a dual-stack IPv6 socket from an IPv4 peer reports the peer as DE_NET_FAMILY_IPV4, so
 * one peer has one address whatever socket it reached. de_net_address_parse keeps a mapped
 * address as written (IPv6), so `[::ffff:1.2.3.4]:5` and `1.2.3.4:5` are not equal.
 */
typedef struct de_net_address {
    u16 family;   /**< de_net_family */
    u16 port;     /**< host byte order; 0 = any (binding to it picks an ephemeral port) */
    u8 addr[16];  /**< network byte order. IPv4 uses addr[0..3]; the other bytes are zero. */
    u32 scope_id; /**< IPv6 zone index for link-local addresses (`%3`); 0 = none, and 0 for IPv4 */
} de_net_address;
DE_STATIC_ASSERT(sizeof(de_net_address) == 24, "de_net_address layout is part of the ABI");

/** Buffer size that holds any de_net_address_to_string result with its NUL
 * (`[ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255%4294967295]:65535` is 65 bytes). */
#define DE_NET_ADDRESS_STRING_MAX 72

/**
 * Parses a numeric address with a port: `a.b.c.d:port` or `[ipv6]:port`, the IPv6 part in any
 * RFC 4291 text form (`::` compression, a trailing dotted IPv4, hex in either case) with an
 * optional numeric zone (`[fe80::1%3]:7777`). The port is required, 0 to 65535. IPv4 octets are
 * decimal 0 to 255 without leading zeros (`010` is refused: other parsers read it as octal).
 * No whitespace, no host names: names go through de_net_resolve. Pure function, any thread.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT for a NULL argument; DE_ERR_PARSE for anything else,
 *         with a message that says what is wrong. `*out` is zeroed on failure.
 */
DE_API de_result de_net_address_parse(const char *text, de_net_address *out);

/**
 * Formats `a` as de_net_address_parse reads it: `1.2.3.4:7777`, `[::1]:7777`, `[fe80::1%3]:9`.
 * IPv6 uses the RFC 5952 canonical form (lowercase, longest run of two or more zero groups
 * compressed, the first one on a tie, `::ffff:` mapped addresses with a dotted tail), so
 * to_string then parse gives back an equal address. DE_NET_FAMILY_NONE (or an unknown family)
 * gives `(none)`. Always NUL-terminates when `buf_size > 0`, cutting the text if it does not fit
 * (DE_NET_ADDRESS_STRING_MAX always fits).
 *
 * @return the length of the full text without the NUL, as snprintf does.
 */
DE_API u32 de_net_address_to_string(const de_net_address *a, char *buf, u32 buf_size);

/** True when both have the same family, port, address bytes (4 for IPv4) and zone. */
DE_API bool de_net_address_equal(const de_net_address *a, const de_net_address *b);

/** 64-bit hash of the fields that de_net_address_equal compares; equal addresses hash equal.
 * Stable across runs and platforms. */
DE_API u64 de_net_address_hash(const de_net_address *a);

/** `0.0.0.0:port` or `[::]:port` (bind to every interface). Other families give NONE. */
DE_API de_net_address de_net_address_any(de_net_family family, u16 port);

/** `127.0.0.1:port` or `[::1]:port`. Other families give NONE. */
DE_API de_net_address de_net_address_loopback(de_net_family family, u16 port);

/** The IPv4 address `a.b.c.d:port`. */
DE_API de_net_address de_net_address_ipv4(u8 a, u8 b, u8 c, u8 d, u16 port);

/**
 * Resolves `host:port` (or `[host]:port`; an IPv6 literal needs the brackets) with the OS
 * resolver (getaddrinfo). A numeric address is parsed without asking the OS. **Blocks** for as
 * long as DNS takes (seconds): call it only from the low-priority background task pool
 * (de_task.h), never on the frame thread (SPEC §8).
 *
 * @param family  DE_NET_FAMILY_IPV4 or _IPV6 to keep only that family; DE_NET_FAMILY_NONE for
 *                both, in the order the OS prefers (RFC 6724).
 * @param out     receives up to `max` distinct addresses, each with the port.
 * @param count   receives how many were written.
 * @return DE_OK (at least one address); DE_ERR_INVALID_ARGUMENT (NULL argument, `max` 0);
 *         DE_ERR_PARSE (no port, bad port, empty host); DE_ERR_NOT_FOUND (the name has no
 *         address of the requested family); DE_ERR_NETWORK (the resolver failed, for example no
 *         network). Thread-safe.
 */
DE_API de_result de_net_resolve(const char *text, de_net_family family, de_net_address *out,
                                u32 max, u32 *count);

/* ---------- End of transport: network addresses ---------- */

/* ---------- Transport: connect tokens (DENG-52) ---------- */

/** Size of a connect token (de_net_desc.connect_token). */
#define DE_NET_CONNECT_TOKEN_BYTES 64
/** Size of the server key that seals and opens connect tokens. */
#define DE_NET_SERVER_KEY_BYTES 32

/** What a connect token carries. The game backend chooses the values; the engine reads them. */
typedef struct de_net_connect_token_info {
    u64 client_id;     /**< the backend's id of the player; one connection per id at a time */
    u64 expire_unix_s; /**< the server refuses the token from this time on (Unix seconds, UTC) */
    u64 user;          /**< opaque to the engine (a session id, a team, ...) */
} de_net_connect_token_info;

/**
 * Makes a connect token: the 24 bytes of `client_id`, `expire_unix_s` and `user`, sealed with
 * `server_key` (XChaCha20-Poly1305, a random 24-byte nonce), 64 bytes in all:
 * `nonce (24) | ciphertext (24) | tag (16)`. Only a holder of `server_key` can make or read a
 * token; a client hands it over as it got it (de_net_desc.connect_token). Normally a game
 * backend makes tokens; a dedicated server or a test may call this too. Thread-safe.
 *
 * An all-zero token is not a token: it asks for a direct IP connect (DENG-2 Q15), which the
 * server allows or refuses by its own setting.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL argument); DE_ERR_INTERNAL (the OS random source
 *         failed; `out` is zeroed).
 */
DE_API de_result de_net_connect_token_generate(const u8 server_key[DE_NET_SERVER_KEY_BYTES],
                                               u64 client_id, u64 expire_unix_s, u64 user,
                                               u8 out[DE_NET_CONNECT_TOKEN_BYTES]);

/**
 * Opens a connect token on the server side. The token is valid while `now_unix_s` is less than
 * its `expire_unix_s`. Constant-time in the token bytes; sets no error context for a bad token
 * (servers check untrusted tokens at packet rate). Thread-safe.
 *
 * @param out  receives the token's values on DE_OK; zeroed otherwise. May be NULL.
 * @return DE_OK; DE_ERR_NOT_FOUND (an all-zero token: a direct connect, no credentials);
 *         DE_ERR_PARSE (forged, changed, or sealed with another key); DE_ERR_TIMEOUT (expired);
 *         DE_ERR_INVALID_ARGUMENT (NULL key or token).
 */
DE_API de_result de_net_connect_token_verify(const u8 server_key[DE_NET_SERVER_KEY_BYTES],
                                             const u8 token[DE_NET_CONNECT_TOKEN_BYTES],
                                             u64 now_unix_s, de_net_connect_token_info *out);

/** Fills `out` with a new random server key (OS randomness). DE_ERR_INTERNAL when the OS random
 * source fails; DE_ERR_INVALID_ARGUMENT for NULL. */
DE_API de_result de_net_server_key_generate(u8 out[DE_NET_SERVER_KEY_BYTES]);

/* ---------- End of transport: connect tokens ---------- */

typedef struct de_net_session de_net_session;
DE_HANDLE(de_peer);
DE_HANDLE(de_rpc);

typedef enum de_net_model {
    DE_NET_SERVER_AUTHORITATIVE,
    DE_NET_ROLLBACK,
    DE_NET_LOCKSTEP
} de_net_model;
typedef enum de_net_role { DE_NET_SERVER, DE_NET_CLIENT, DE_NET_HOST, DE_NET_PEER } de_net_role;
/** What carries the datagrams. All three sit behind the same internal socket interface
 * (src/net/net_internal.h), so the protocol above them does not change. */
typedef enum de_net_transport {
    DE_TRANSPORT_UDP,     /**< OS UDP sockets, IPv4 and dual-stack IPv6 (DENG-51) */
    DE_TRANSPORT_STEAM,   /**< Steam Networking Sockets (S37, DENG-124) */
    DE_TRANSPORT_LOOPBACK /**< in-process queues, no OS sockets: tests and listen servers */
} de_net_transport;
typedef enum de_channel {
    DE_CHANNEL_UNRELIABLE,
    DE_CHANNEL_RELIABLE_ORDERED,
    DE_CHANNEL_RELIABLE_UNORDERED
} de_channel;

typedef struct de_net_desc {
    const de_allocator *allocator;
    de_net_model model;
    de_net_role role;
    de_net_transport transport;
    const char *address; /* "0.0.0.0:7777" to listen, "host:port" to connect */
    u32 max_peers;
    u32 tick_rate_hz;        /* must match the fixed simulation rate */
    u32 snapshot_rate_hz;    /* server-authoritative */
    u32 rollback_max_frames; /* rollback, default 8 */
    u32 input_delay_frames;  /* rollback / lockstep */
    u8 connect_token[64];    /* issued by game backend or Steam */
    bool encrypt;            /* default true */
} de_net_desc;

DE_API de_result de_net_create(const de_net_desc *desc, de_world *w, de_net_session **out);
DE_API void de_net_destroy(de_net_session *s);
/* Registers DeNetId/DeNetOwner components and the FixedUpdate send/receive systems. */
DE_API void de_net_attach(de_net_session *s, de_world *w);
DE_API void de_net_poll(de_net_session *s); /* drain sockets; host calls once per frame */

/* ---------- Peers ---------- */
typedef struct de_peer_stats {
    f32 rtt_ms, jitter_ms, loss;
    u32 bytes_in_per_sec, bytes_out_per_sec;
} de_peer_stats;
typedef enum de_net_event_kind {
    DE_NET_EV_CONNECTED,
    DE_NET_EV_DISCONNECTED,
    DE_NET_EV_DESYNC,
    DE_NET_EV_TIMEOUT
} de_net_event_kind;
typedef struct de_net_event {
    de_net_event_kind kind;
    de_peer peer;
    u64 tick;
} de_net_event;

DE_API u32 de_net_peers(const de_net_session *s, de_peer *out, u32 max);
DE_API de_peer de_net_local_peer(const de_net_session *s);
DE_API de_peer_stats de_net_peer_stats(const de_net_session *s, de_peer p);
DE_API void de_net_kick(de_net_session *s, de_peer p, const char *reason);

/* ---------- Raw messages ---------- */
DE_API de_result de_net_send(de_net_session *s, de_peer to, de_channel ch, const void *data,
                             u32 bytes);
typedef void (*de_net_message_fn)(de_peer from, const void *data, u32 bytes, void *user);
DE_API void de_net_on_message(de_net_session *s, de_net_message_fn fn, void *user);

/* ---------- Server-authoritative replication ---------- */
typedef struct DeNetId {
    u32 id;
} DeNetId; /* assigned by server */
typedef struct DeNetOwner {
    de_peer owner;
    bool predicted;
} DeNetOwner;
typedef struct DeNetRelevance {
    f32 radius;
    u32 groups;
    bool always;
} DeNetRelevance;

/* Components whose fields carry DE_FIELD_REPLICATE are replicated automatically. */
DE_API void de_net_replicate_component(de_net_session *s, de_component c, u32 priority);
DE_API void de_net_spawn_replicated(de_net_session *s, de_entity e, de_name prefab);
typedef bool (*de_relevance_fn)(de_peer p, de_entity e, void *user);
DE_API void de_net_set_relevance_fn(de_net_session *s, de_relevance_fn fn, void *user);

/* Lag compensation: run a hit test against world state as the given peer saw it. */
DE_API void de_net_rewind_begin(de_net_session *s, de_peer viewer);
DE_API void de_net_rewind_end(de_net_session *s);

/* RPCs: payload type is a reflected struct. */
typedef enum de_rpc_target { DE_RPC_SERVER, DE_RPC_OWNER, DE_RPC_ALL_CLIENTS } de_rpc_target;
typedef void (*de_rpc_fn)(de_world *w, de_peer from, const void *payload, void *user);
DE_API de_rpc de_net_rpc_register(de_net_session *s, const char *name, de_type_id payload,
                                  de_channel ch, de_rpc_fn fn, void *user);
DE_API void de_net_rpc_call(de_net_session *s, de_rpc rpc, de_rpc_target target, de_entity context,
                            const void *payload);

/* ---------- Rollback / lockstep (inputs only; deterministic simulation required) ---------- */
/* Each tick the game provides local input bytes (usually de_input_pack_actions). */
DE_API void de_net_set_local_input(de_net_session *s, u64 tick, const void *input, u32 bytes);
/* Inside FixedUpdate: confirmed or predicted input for a player at the current tick. */
DE_API u32 de_net_get_input(const de_net_session *s, u32 player, void *dst, u32 max_bytes,
                            bool *predicted);
DE_API bool de_net_is_resimulating(const de_net_session *s); /* skip audio/VFX side effects */
DE_API u64 de_net_confirmed_tick(const de_net_session *s);
DE_API bool
de_net_should_advance(const de_net_session *s); /* lockstep: false while waiting for inputs */

/* ---------- Debug ---------- */
typedef struct de_net_sim {
    u32 latency_ms, jitter_ms;
    f32 loss, duplicate;
} de_net_sim;
DE_API void de_net_set_simulation(de_net_session *s, const de_net_sim *sim); /* Dev builds */

DE_EXTERN_C_END
#endif
