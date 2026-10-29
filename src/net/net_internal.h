/* net_internal.h: the net module's internal transport interface (DENG-51, SPEC §16.1).
 *
 * Everything above the transport (handshake DENG-52, channels DENG-53, the Dev network simulator
 * DENG-56, the Steam backend DENG-124) talks to a de__socket: a struct of function pointers plus
 * backend state, so OS UDP sockets, in-process loopback sockets and decorators (the simulator
 * wraps another de__socket) are interchangeable.
 *
 * Rules every backend keeps:
 *   - Non-blocking. send_to and recv_from never wait. "Would block" is not an error: send_to
 *     reports 0 bytes sent, recv_from 0 datagrams.
 *   - No heap in send_to / recv_from. All memory is taken at open; datagrams go straight into
 *     the caller's buffers (SPEC §5.3).
 *   - One thread at a time per socket (the stats are plain counters). Different sockets may be
 *     used from different threads.
 *   - Addresses are normalised: an IPv4 peer that reached a dual-stack IPv6 socket is reported
 *     as DE_NET_FAMILY_IPV4, and send_to accepts IPv4 addresses on a dual-stack socket.
 *
 * Append-only after DENG-51: later tasks add declarations (hooks) at the end; do not reorder.
 */
#ifndef DE_NET_INTERNAL_H
#define DE_NET_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_net.h>

/** Default SO_RCVBUF / SO_SNDBUF; the cvar `net.socket_buffer_bytes` overrides it. */
#define DE__NET_SOCKET_BUFFER_DEFAULT (4u * 1024u * 1024u)

/** Datagrams one OS call receives at most (recvmmsg on Linux). recv_from loops for more. Sized
 * for a 64-player server (DENG-2 Q15): one poll per frame drains a burst from every peer. */
#define DE__NET_RECV_BATCH_MAX 64u

/** Largest UDP payload (65535 - 8-byte UDP header - 20-byte IPv4 header). */
#define DE__NET_MAX_UDP_PAYLOAD 65507u

/** One datagram slot for recv_from. The caller sets `data` and `capacity`. */
typedef struct de__datagram {
    void *data;          /**< in: the caller's buffer */
    u32 capacity;        /**< in: its size in bytes */
    u32 bytes;           /**< out: payload size */
    de_net_address from; /**< out: sender (normalised, see the file comment) */
} de__datagram;

/** Counters of one socket. Plain u64s: read them on the thread that uses the socket. */
typedef struct de__socket_stats {
    u64 datagrams_sent;     /**< accepted by send_to (the OS or the loopback queue) */
    u64 bytes_sent;         /**< their payload bytes */
    u64 datagrams_received; /**< returned by recv_from */
    u64 bytes_received;     /**< their payload bytes */
    u64 send_would_block;   /**< send_to calls that sent nothing because the OS buffer was full */
    u64 truncated;          /**< received datagrams dropped because they did not fit the buffer */
    u64 dropped;            /**< datagrams for this socket dropped because its queue was full
                                 (loopback; the OS does not report its own drops) */
} de__socket_stats;

typedef struct de__socket de__socket;

/**
 * A transport socket. Backends fill the function pointers and `user` at open; callers use the
 * de__socket_* wrappers below, which keep the sent/received counters.
 */
struct de__socket {
    /**
     * Sends one datagram to `to`. `*sent` = `bytes` when the datagram was handed over (UDP may
     * still lose it), 0 when the send would block. Errors: DE_ERR_INVALID_ARGUMENT for a
     * datagram larger than the path allows (EMSGSIZE with the Don't-Fragment bit, or the loopback
     * `max_datagram_bytes`; DENG-54's MTU probes rely on this) or an address of the wrong family;
     * DE_ERR_NETWORK for other OS errors (unreachable network, ...).
     */
    de_result (*send_to)(de__socket *s, const de_net_address *to, const void *data, u32 bytes,
                         u32 *sent);
    /**
     * Receives up to `count` datagrams into `msgs[0 .. count)`, returning at once when none are
     * waiting. `*received` = how many; entries `0 .. *received` are filled, in arrival order.
     * A datagram larger than its slot's capacity is dropped (stats.truncated) and its slot is
     * reused, so the buffers of entries may move between slots: the backend may swap the
     * `data` / `capacity` pairs among the `count` entries. Errors: DE_ERR_NETWORK (`*received`
     * still counts the datagrams before the error).
     */
    de_result (*recv_from)(de__socket *s, de__datagram *msgs, u32 count, u32 *received);
    /** Closes the socket and frees it (and `user`). */
    void (*close)(de__socket *s);
    /** Backend state. */
    void *user;
    /** Bound address; for port 0, the port the OS (or the loopback network) picked. */
    de_net_address local;
    de__socket_stats stats;
};

/** send_to with stats. `sent` may be NULL. */
de_result de__socket_send_to(de__socket *s, const de_net_address *to, const void *data, u32 bytes,
                             u32 *sent);
/** recv_from with stats. */
de_result de__socket_recv_from(de__socket *s, de__datagram *msgs, u32 count, u32 *received);
/** close; NULL is a no-op. */
void de__socket_close(de__socket *s);

/* ---------- UDP (socket_posix.c, socket_win32.c) ---------- */

/** Options of a UDP socket. `(de__udp_desc){0}` (or a NULL desc) is the default. */
typedef struct de__udp_desc {
    /** SO_RCVBUF / SO_SNDBUF request in bytes; 0 = the cvar `net.socket_buffer_bytes` (default
     * 4 MiB). The OS may grant less (Linux caps at net.core.rmem_max); see de__udp_get_info. */
    u32 buffer_bytes;
    /** IPv6 only: refuse IPv4 peers (IPV6_V6ONLY = 1). Default false: dual stack. */
    bool v6_only;
    /** Do not set the Don't-Fragment bit (default: set, for MTU discovery, DENG-54). */
    bool allow_fragments;
} de__udp_desc;

/** What a UDP socket got from the OS. */
typedef struct de__udp_info {
    u32 recv_buffer_bytes; /**< SO_RCVBUF as the OS reports it (Linux reports double) */
    u32 send_buffer_bytes; /**< SO_SNDBUF as the OS reports it */
    bool dual_stack;       /**< an IPv6 socket that accepts IPv4 peers */
    bool dont_fragment;    /**< DF is set for the socket's own family */
} de__udp_info;

/**
 * Opens a non-blocking UDP socket bound to `bind_addr` (NULL: `[::]:0` dual stack, or `0.0.0.0:0`
 * when the host has no IPv6). Port 0 picks an ephemeral port; `(*out)->local` has it.
 * `a` NULL = de_heap_allocator(); the socket is one allocation from it, freed by close.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (bad family, NULL out); DE_ERR_ALREADY_EXISTS (address
 *         in use); DE_ERR_UNSUPPORTED (the OS has no such family: no IPv6); DE_ERR_NOT_FOUND
 *         (the address is not on this host); DE_ERR_OUT_OF_MEMORY; DE_ERR_NETWORK.
 */
de_result de__udp_open(const de_net_address *bind_addr, const de__udp_desc *desc,
                       const de_allocator *a, de__socket **out);

/** The socket's OS settings. `s` must come from de__udp_open (asserted); zeros otherwise. */
de__udp_info de__udp_get_info(const de__socket *s);

/** Blocking OS resolve of `host` (no port, no brackets) for de_net_resolve. Per OS file. */
de_result de__net_os_resolve(const char *host, de_net_family family, u16 port, de_net_address *out,
                             u32 max, u32 *count);

/* ---------- Loopback (socket_loopback.c, DE_TRANSPORT_LOOPBACK) ---------- */

/** An in-process network: sockets opened on it reach each other by address. */
typedef struct de__loopback_net de__loopback_net;

/** Options of a loopback network. `(de__loopback_desc){0}` is the default. */
typedef struct de__loopback_desc {
    /** Datagrams each socket's receive queue holds; 0 = 256. A full queue drops new datagrams
     * and counts them (stats.dropped of the receiving socket). */
    u32 queue_capacity;
    /** Largest datagram; 0 = 2048. Bigger sends fail with DE_ERR_INVALID_ARGUMENT, like a UDP
     * send over the path MTU with Don't-Fragment set. */
    u32 max_datagram_bytes;
} de__loopback_desc;

/**
 * Creates a loopback network. `a` NULL = de_heap_allocator(); it also backs every socket's
 * queue. Thread-safe: sockets of one network may be used from different threads (one mutex).
 */
de_result de__loopback_net_create(const de__loopback_desc *desc, const de_allocator *a,
                                  de__loopback_net **out);
/** Destroys the network. Every socket opened on it must be closed first (asserted). */
void de__loopback_net_destroy(de__loopback_net *n);

/**
 * Opens a socket on `n` bound to `bind_addr` (NULL: `127.0.0.1:0`). Port 0 picks a free port from
 * 49152 up. A socket bound to an unspecified address (`0.0.0.0`, `[::]`) receives datagrams for
 * its port at any address; otherwise the destination must equal its address. The sender's
 * address that receivers see is its bound address, with an unspecified address replaced by the
 * loopback address of its family.
 *
 * Datagrams to an address no socket has are dropped silently, as UDP does. Delivery is FIFO per
 * receiving socket. Memory: `queue_capacity * max_datagram_bytes` plus a header, taken here.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT; DE_ERR_ALREADY_EXISTS (a socket has that port at an
 *         overlapping address); DE_ERR_BUSY (no free port); DE_ERR_OUT_OF_MEMORY.
 */
de_result de__loopback_open(de__loopback_net *n, const de_net_address *bind_addr, de__socket **out);

/* ---------- Shared helpers (socket.c, net_address.c) ---------- */

/** The log category `net`. */
de_log_category de__net_log(void);

/** SO_RCVBUF / SO_SNDBUF to request: `desc_bytes`, else the cvar `net.socket_buffer_bytes`
 * (registered on first use). */
u32 de__net_socket_buffer_bytes(u32 desc_bytes);

/** True when `a` is `0.0.0.0` or `::` (any port). */
bool de__net_address_is_unspecified(const de_net_address *a);

/** If `a` is an IPv4-mapped IPv6 address (`::ffff:a.b.c.d`), rewrites it as IPv4. */
void de__net_address_unmap(de_net_address *a);

/**
 * Splits `host:port` or `[host]:port` into the host (copied into `host`, NUL-terminated,
 * brackets removed) and the port. DE_ERR_PARSE when the port is missing or bad, the host empty
 * or longer than `host_cap - 1`.
 */
de_result de__net_split_host_port(const char *text, char *host, u32 host_cap, u16 *port);

/* DENG-53: connection-owned channel state; replace these hooks in DENG-54/55. */
typedef struct de__conn de__conn;
u32 de__mtu_payload(const de__conn *conn);
u32 de__cc_send_budget(const de__conn *conn);
void de__cc_on_ack(de__conn *conn, u32 bytes, u64 rtt_ns);
void de__cc_on_loss(de__conn *conn, u32 bytes);

#endif /* DE_NET_INTERNAL_H */
