/* de_net.h — transport + three session models: server-authoritative, rollback, lockstep. SPEC.md §16. */
#ifndef DE_NET_H
#define DE_NET_H
#include "de_base.h"
#include "de_ecs.h"
DE_EXTERN_C_BEGIN

typedef struct de_net_session de_net_session;
DE_HANDLE(de_peer);
DE_HANDLE(de_rpc);

typedef enum de_net_model { DE_NET_SERVER_AUTHORITATIVE, DE_NET_ROLLBACK, DE_NET_LOCKSTEP } de_net_model;
typedef enum de_net_role  { DE_NET_SERVER, DE_NET_CLIENT, DE_NET_HOST, DE_NET_PEER } de_net_role;
typedef enum de_net_transport { DE_TRANSPORT_UDP, DE_TRANSPORT_STEAM, DE_TRANSPORT_LOOPBACK } de_net_transport;
typedef enum de_channel { DE_CHANNEL_UNRELIABLE, DE_CHANNEL_RELIABLE_ORDERED, DE_CHANNEL_RELIABLE_UNORDERED } de_channel;

typedef struct de_net_desc {
    const de_allocator *allocator;
    de_net_model     model;
    de_net_role      role;
    de_net_transport transport;
    const char      *address;          /* "0.0.0.0:7777" to listen, "host:port" to connect */
    u32  max_peers;
    u32  tick_rate_hz;                 /* must match the fixed simulation rate */
    u32  snapshot_rate_hz;             /* server-authoritative */
    u32  rollback_max_frames;          /* rollback, default 8 */
    u32  input_delay_frames;           /* rollback / lockstep */
    u8   connect_token[64];            /* issued by game backend or Steam */
    bool encrypt;                      /* default true */
} de_net_desc;

DE_API de_result de_net_create(const de_net_desc *desc, de_world *w, de_net_session **out);
DE_API void      de_net_destroy(de_net_session *s);
/* Registers DeNetId/DeNetOwner components and the FixedUpdate send/receive systems. */
DE_API void      de_net_attach(de_net_session *s, de_world *w);
DE_API void      de_net_poll(de_net_session *s);          /* drain sockets; host calls once per frame */

/* ---------- Peers ---------- */
typedef struct de_peer_stats { f32 rtt_ms, jitter_ms, loss; u32 bytes_in_per_sec, bytes_out_per_sec; } de_peer_stats;
typedef enum de_net_event_kind { DE_NET_EV_CONNECTED, DE_NET_EV_DISCONNECTED, DE_NET_EV_DESYNC, DE_NET_EV_TIMEOUT } de_net_event_kind;
typedef struct de_net_event { de_net_event_kind kind; de_peer peer; u64 tick; } de_net_event;

DE_API u32           de_net_peers(const de_net_session *s, de_peer *out, u32 max);
DE_API de_peer       de_net_local_peer(const de_net_session *s);
DE_API de_peer_stats de_net_peer_stats(const de_net_session *s, de_peer p);
DE_API void          de_net_kick(de_net_session *s, de_peer p, const char *reason);

/* ---------- Raw messages ---------- */
DE_API de_result de_net_send(de_net_session *s, de_peer to, de_channel ch, const void *data, u32 bytes);
typedef void (*de_net_message_fn)(de_peer from, const void *data, u32 bytes, void *user);
DE_API void      de_net_on_message(de_net_session *s, de_net_message_fn fn, void *user);

/* ---------- Server-authoritative replication ---------- */
typedef struct DeNetId    { u32 id; } DeNetId;                 /* assigned by server */
typedef struct DeNetOwner { de_peer owner; bool predicted; } DeNetOwner;
typedef struct DeNetRelevance { f32 radius; u32 groups; bool always; } DeNetRelevance;

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
DE_API de_rpc de_net_rpc_register(de_net_session *s, const char *name, de_type_id payload, de_channel ch, de_rpc_fn fn, void *user);
DE_API void   de_net_rpc_call(de_net_session *s, de_rpc rpc, de_rpc_target target, de_entity context, const void *payload);

/* ---------- Rollback / lockstep (inputs only; deterministic simulation required) ---------- */
/* Each tick the game provides local input bytes (usually de_input_pack_actions). */
DE_API void de_net_set_local_input(de_net_session *s, u64 tick, const void *input, u32 bytes);
/* Inside FixedUpdate: confirmed or predicted input for a player at the current tick. */
DE_API u32  de_net_get_input(const de_net_session *s, u32 player, void *dst, u32 max_bytes, bool *predicted);
DE_API bool de_net_is_resimulating(const de_net_session *s);   /* skip audio/VFX side effects */
DE_API u64  de_net_confirmed_tick(const de_net_session *s);
DE_API bool de_net_should_advance(const de_net_session *s);    /* lockstep: false while waiting for inputs */

/* ---------- Debug ---------- */
typedef struct de_net_sim { u32 latency_ms, jitter_ms; f32 loss, duplicate; } de_net_sim;
DE_API void de_net_set_simulation(de_net_session *s, const de_net_sim *sim);   /* Dev builds */

DE_EXTERN_C_END
#endif
