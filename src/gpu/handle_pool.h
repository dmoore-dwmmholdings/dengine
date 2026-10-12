/* handle_pool.h — generational slot pools behind the de_gpu handles (internal, DENG-281).
 *
 * A handle id is `index | generation << 20`: 20 bits of slot index (at most 2^20 slots per pool)
 * and 12 bits of generation, which is never 0, so an id is never 0. Each slot holds the object's
 * element (front-end record plus the backend payload) inline, 16-byte aligned.
 *
 * Lifetime of a slot:
 *   alloc   -> live; the returned id matches the slot's generation.
 *   retire  -> the generation is bumped, so every id of the object is stale at once; the element
 *              stays readable by index (de__gpu_pool_at) while its destruction is deferred.
 *   release -> the slot joins the FIFO free list. FIFO spreads reuse over all free slots, so a
 *              12-bit generation wraps only after 4095 reuses of the *same* slot.
 *
 * Slots live in fixed-size chunks that never move, so element pointers stay valid while the pool
 * grows. Lookups take no lock: they read the chunk pointer and the generation atomically. Alloc,
 * retire and release take the pool mutex. Concurrent use of one object while another thread
 * destroys it is a caller bug; the generation check still rejects it once the retire is visible.
 */
#ifndef DE_GPU_HANDLE_POOL_H
#define DE_GPU_HANDLE_POOL_H
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include "platform/de_atomic.h"

#define DE__GPU_HANDLE_INDEX_BITS 20u
#define DE__GPU_HANDLE_INDEX_MASK ((1u << DE__GPU_HANDLE_INDEX_BITS) - 1u)
#define DE__GPU_HANDLE_GEN_MAX    ((1u << (32u - DE__GPU_HANDLE_INDEX_BITS)) - 1u) /* 4095 */
#define DE__GPU_POOL_MAX_SLOTS    (1u << DE__GPU_HANDLE_INDEX_BITS)
#define DE__GPU_POOL_CHUNK_SLOTS  256u

static inline u32 de__gpu_handle_index(u32 id) {
    return id & DE__GPU_HANDLE_INDEX_MASK;
}
static inline u32 de__gpu_handle_gen(u32 id) {
    return id >> DE__GPU_HANDLE_INDEX_BITS;
}
static inline u32 de__gpu_handle_make(u32 index, u32 gen) {
    return index | (gen << DE__GPU_HANDLE_INDEX_BITS);
}

typedef struct de__gpu_pool {
    const de_allocator *alloc;
    const char *kind;      /**< "buffer", "texture", ...: messages */
    u32 elem_size;         /**< element bytes, rounded up to 16 */
    u32 slot_stride;       /**< 16-byte slot header + element */
    u32 max_slots;         /**< at most DE__GPU_POOL_MAX_SLOTS */
    u32 chunk_count;       /**< entries in `chunks` */
    de_atomic_ptr *chunks; /**< chunk pointers; NULL until the chunk is first used */
    de_atomic_u32 used;    /**< slots ever handed out (bump index) */
    de_mutex lock;
    u32 free_head, free_tail, free_count; /**< FIFO through the slot headers */
    u32 live;                             /**< allocated and not retired */
    u32 retired;                          /**< retired, not yet released */
} de__gpu_pool;

/** Sets up an empty pool. `max_slots` 0 = DE__GPU_POOL_MAX_SLOTS; larger values are an error. */
de_result de__gpu_pool_init(de__gpu_pool *p, const de_allocator *alloc, const char *kind,
                            u32 elem_size, u32 max_slots);
/** Frees every chunk. Elements are not visited: retire and destroy objects first. */
void de__gpu_pool_destroy(de__gpu_pool *p);

/**
 * Takes a free slot (zeroed element) and returns its id, or 0 with DE_ERR_OUT_OF_MEMORY when the
 * pool is full or a chunk cannot be allocated. `*elem` gets the element.
 */
u32 de__gpu_pool_alloc(de__gpu_pool *p, void **elem);
/** The element of a live id, or NULL for 0, a stale id or a forged index. No lock, no error. */
void *de__gpu_pool_get(const de__gpu_pool *p, u32 id);
/** True if `id` names a slot of this pool (index in range) whose generation does not match. */
bool de__gpu_pool_is_stale(const de__gpu_pool *p, u32 id);
/**
 * Makes `id` stale (bumps the generation) and returns its slot index, or
 * DE__GPU_POOL_MAX_SLOTS when `id` is not live. The element stays readable by index.
 */
u32 de__gpu_pool_retire(de__gpu_pool *p, u32 id);
/** Element of slot `index` (live or retired). No checks. */
void *de__gpu_pool_at(const de__gpu_pool *p, u32 index);
/** Puts a retired slot on the free list. */
void de__gpu_pool_release(de__gpu_pool *p, u32 index);
/** Id of a live slot `index` (current generation), for walking live objects at shutdown. */
u32 de__gpu_pool_id_at(const de__gpu_pool *p, u32 index);
/** True if slot `index` holds a live object (allocated, not retired). */
bool de__gpu_pool_is_live(const de__gpu_pool *p, u32 index);

typedef struct de__gpu_pool_stats {
    u32 live, retired, free, used, chunks;
} de__gpu_pool_stats;
de__gpu_pool_stats de__gpu_pool_get_stats(de__gpu_pool *p);

#endif
