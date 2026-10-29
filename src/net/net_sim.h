/* DENG-56: Dev/Debug-only datagram decorator. One thread at a time, like de__socket. */
#ifndef DE_NET_SIM_H
#define DE_NET_SIM_H
#include "net/net_internal.h"

#if defined(DE_DEV) || defined(DE_DEBUG)
typedef struct de__net_sim_desc {
    u32 queue_capacity;        /**< per direction; 0 = net.sim.queue_capacity (default 1024) */
    u32 slot_bytes;            /**< retained datagram capacity; 0 = 2048, at most UDP's 65507 */
    bool incoming;             /**< also delay received datagrams; outgoing always participates */
    bool use_cvars;            /**< live net.sim.* settings/seed until an explicit set */
    u64 seed;                  /**< independent outgoing/incoming de_rng streams */
    u64 (*now_ns)(void *user); /**< NULL = de_time_now_ns; monotonic, never sleeps */
    void *clock_user;
} de__net_sim_desc;

typedef struct de__net_sim_stats {
    u64 loss_dropped, queue_dropped, duplicates, errors;
    u64 delivered_out, delivered_in;
    u32 pending_out, pending_in;
    size_t allocated_bytes;
} de__net_sim_stats;

/* Takes ownership of inner ONLY on success. Preallocates all storage, capped at 64 MiB.
 * basic wrap uses live cvars and outgoing-only simulation; ex supplies a clock/seed/inbound.
 * Queued datagrams retain their original delivery decisions across later setting changes.
 * Changing to zero bypasses new datagrams; already scheduled ones still require pumping.
 * With simulation active, oversize (>slot_bytes) sends fail instead of silently truncating.
 * A simulated loss/full queue consumes the datagram as successful UDP loss, not would-block.
 */
de_result de__net_sim_wrap(de__socket *inner, const de_allocator *a, u32 queue_capacity,
                           de__socket **out);
de_result de__net_sim_wrap_ex(de__socket *inner, const de__net_sim_desc *desc,
                              const de_allocator *a, de__socket **out);
de_result de__net_sim_set(de__socket *s, const de_net_sim *settings);
/* send/recv pump outgoing too. Call this on idle ticks; no timer/thread is created. */
de_result de__net_sim_pump(de__socket *s);
de__net_sim_stats de__net_sim_get_stats(const de__socket *s);
#endif
#endif
