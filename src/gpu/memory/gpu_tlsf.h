/* gpu_tlsf.h — offset-based TLSF for device memory (internal, DENG-282).
 *
 * Device memory has no CPU address, so this TLSF manages a range of byte offsets [0, capacity)
 * and keeps every block's metadata out of band, in nodes from a CPU pool. One instance manages
 * one device heap (gpu_heap.c). The bucket math follows the S02 TLSF (src/core/tlsf.c): 32
 * second-level classes per power of two, a first-level and a second-level bitmap, good-fit search
 * by rounding the request up to the next class. That code is private to core and works on
 * in-band headers, so this is an independent copy for offsets.
 *
 * - Granularity: every offset and size is a multiple of DE__GPU_TLSF_GRANULARITY (256 bytes,
 *   the strictest buffer placement alignment of D3D12 and Vulkan). Requests are rounded up.
 * - Alignment: any power of two (0 means the granularity). The leading gap of an aligned block
 *   is given back as a free block; with metadata out of band any gap can be a block.
 * - Cost: alloc and free are O(1): two bitmap scans and at most two node splits or merges. The
 *   price of the good fit: a request can fail while a free block up to 1/32 larger exists. When
 *   the good-fit search fails, the head block of the request's own class is tried as an exact
 *   fit (one or two blocks, still O(1)), so a range of exactly the request size always serves it.
 * - Not thread-safe: the heap manager holds its memory class's mutex around every call.
 */
#ifndef DE_GPU_TLSF_H
#define DE_GPU_TLSF_H
#include <dengine/de_memory.h>

/** Offsets and sizes are multiples of this. */
#define DE__GPU_TLSF_GRANULARITY ((u64)256)
/** Largest capacity (1 TiB). */
#define DE__GPU_TLSF_MAX_CAPACITY ((u64)1 << 40)

typedef struct de__gpu_tlsf de__gpu_tlsf;
/** One block of the range (opaque). An allocation is identified by its block. */
typedef struct de__gpu_tlsf_block de__gpu_tlsf_block;

typedef struct de__gpu_tlsf_stats {
    u64 capacity;     /**< bytes managed */
    u64 used;         /**< bytes in allocated blocks (rounded sizes) */
    u64 free;         /**< capacity - used */
    u64 largest_free; /**< size of the largest free block */
    u32 used_blocks;
    u32 free_blocks;   /**< 1 when everything is free (fully coalesced) */
    f32 fragmentation; /**< 1 - largest_free / free; 0 when nothing or everything is free */
} de__gpu_tlsf_stats;

/**
 * A TLSF over [0, capacity). `capacity` is rounded down to the granularity and must be at least
 * one granule and at most DE__GPU_TLSF_MAX_CAPACITY. `alloc` (NULL = heap) backs the control
 * structure and the node pool.
 */
de_result de__gpu_tlsf_create(const de_allocator *alloc, u64 capacity, de__gpu_tlsf **out);
void de__gpu_tlsf_destroy(de__gpu_tlsf *t);

/**
 * Allocates `size` bytes (0 counts as one granule) at an offset that is a multiple of `align`
 * (a power of two; 0 means the granularity). Returns the block and writes its offset, or returns
 * NULL without setting an error when nothing fits (or the node pool cannot grow); the caller
 * decides whether that is an error.
 */
de__gpu_tlsf_block *de__gpu_tlsf_alloc(de__gpu_tlsf *t, u64 size, u64 align, u64 *offset);
/** Frees an allocated block; merges it with free neighbours. O(1). */
void de__gpu_tlsf_free(de__gpu_tlsf *t, de__gpu_tlsf_block *b);

u64 de__gpu_tlsf_block_offset(const de__gpu_tlsf_block *b);
/** The block's size: the request rounded up to the granularity. */
u64 de__gpu_tlsf_block_size(const de__gpu_tlsf_block *b);

/** Counters, plus the largest free block (scans the top non-empty class list only). */
void de__gpu_tlsf_get_stats(const de__gpu_tlsf *t, de__gpu_tlsf_stats *out);
/** True when nothing is allocated. O(1). */
bool de__gpu_tlsf_is_empty(const de__gpu_tlsf *t);

/**
 * Walks every block and free list and checks every invariant (contiguous blocks covering the
 * range, no two free neighbours, every free block listed once in its class, counters). Returns
 * DE_ERR_INTERNAL naming the broken invariant. O(blocks). For tests.
 */
de_result de__gpu_tlsf_check(const de__gpu_tlsf *t);

#endif
