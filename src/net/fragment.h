/* DENG-54 fragment body, inside a channel sub-message explicitly marked fragmented:
 *   message id u32 LE | index u16 LE | count u16 LE | total bytes u32 LE | data
 * Data slices are always 1024 bytes, except the final slice. Stable slices survive
 * path-MTU fallback without changing an in-flight reliable sub-message's identity.
 * count = ceil(total/1024), 2..256; index < count; body length must match exactly.
 * Channel reliability/ordering/deduplication applies to each sub-message first.
 * A pool owns all retained bytes; callback data is borrowed only during delivery.
 * No allocation in push/expire/reset. Single thread, no reentrant pool operations.
 */
#ifndef DE_NET_FRAGMENT_H
#define DE_NET_FRAGMENT_H
#include "net/net_internal.h"

#define DE__FRAGMENT_HEADER       12u
#define DE__FRAGMENT_SLICE        1024u
#define DE__FRAGMENT_MAX_MESSAGE  (256u * 1024u)
#define DE__FRAGMENT_DEFAULT_POOL (1024u * 1024u)
#define DE__FRAGMENT_MAX_POOL     (16u * 1024u * 1024u)
#define DE__FRAGMENT_TIMEOUT_NS   1000000000ull
#define DE__FRAGMENT_SLOTS        256u

typedef struct de__fragment_pool de__fragment_pool;
typedef struct de__fragment_desc {
    const de_allocator *allocator;
    u32 max_message_bytes, pool_bytes; /* zero: defaults above, fixed at connection setup */
    u64 timeout_ns;                    /* zero: one second, for unreliable messages */
} de__fragment_desc;
typedef struct de__fragment_view {
    u32 id, total, bytes;
    u16 index, count;
    const u8 *data;
} de__fragment_view;
typedef struct de__fragment_stats {
    u32 pool_bytes, used_bytes, peak_bytes, active;
    u64 completed, dropped, malformed;
    size_t allocated_bytes;
} de__fragment_stats;
typedef void (*de__fragment_message_fn)(void *, de_channel, const void *, u32);

de_result de__fragment_create(const de__fragment_desc *desc, de__fragment_pool **out);
void de__fragment_destroy(de__fragment_pool *pool);
void de__fragment_reset(de__fragment_pool *pool);
bool de__fragment_read(const void *body, u32 bytes, u32 max_message, de__fragment_view *out);
u32 de__fragment_write(void *body, u32 capacity, u32 id, u16 index, const void *message, u32 bytes);
/* PARSE drops/tombstones an inconsistent message. OUT_OF_MEMORY on a reliable
 * message is a fatal protocol error for the caller to disconnect with a reason.
 * BUSY drops a new unreliable message when only reliable reservations remain.
 * Pool pressure evicts oldest unreliable reservations first. Reliable reservations
 * remain until completion/reset; packet retransmission can take longer than 1s.
 */
de_result de__fragment_push(de__fragment_pool *pool, de_channel channel, const void *body,
                            u32 bytes, u64 now_ns, de__fragment_message_fn deliver, void *user);
void de__fragment_expire(de__fragment_pool *pool, u64 now_ns);
void de__fragment_reject(de__fragment_pool *pool, de_channel channel, u32 id, u64 now_ns);
de__fragment_stats de__fragment_get_stats(const de__fragment_pool *pool);
#endif
