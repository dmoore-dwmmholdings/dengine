/* mpmc.c: Vyukov bounded MPMC queue with doubled cell sequences (DENG-152). The algorithm and
 * the ordering argument are in core/de_mpmc.h. */
#include "core/de_mpmc.h"

#include <string.h>

static bool is_pow2_u32(u32 v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/* Largest power of two that divides `size`, capped at 16. */
static u32 natural_align(u32 size) {
    u32 a = size & (0u - size);
    return a > 16 ? 16 : a;
}

static inline de_atomic_u64 *cell_seq(const de_mpmc *q, u64 pos) {
    return (de_atomic_u64 *)(void *)(q->cells + (size_t)(pos & q->mask) * q->stride);
}

static inline u8 *cell_data(const de_mpmc *q, u64 pos) {
    return q->cells + (size_t)(pos & q->mask) * q->stride + q->data_offset;
}

de_result de_mpmc_init(de_mpmc *q, const de_mpmc_desc *desc) {
    if (q == NULL || desc == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_mpmc_init: q and desc are required");
    memset(q, 0, sizeof *q);

    u32 capacity = desc->capacity ? desc->capacity : DE_MPMC_DEFAULT_CAPACITY;
    u32 elem_size = desc->elem_size ? desc->elem_size : (u32)sizeof(void *);
    u32 align = desc->elem_align ? desc->elem_align : natural_align(elem_size);

    if (!is_pow2_u32(capacity) || capacity > DE_MPMC_MAX_CAPACITY)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_mpmc_init: capacity %u is not a power of two in [1, 2^31]", capacity);
    if (!is_pow2_u32(align) || align > DE_CACHE_LINE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_mpmc_init: elem_align %u is not a power of two <= %u", align,
                       (u32)DE_CACHE_LINE);
    const de_allocator *alloc = desc->allocator != NULL ? desc->allocator : de_heap_allocator();

    /* Cell: the u64 sequence first, then the element at its alignment; the stride keeps both
     * the next sequence (8 bytes) and the next element aligned. */
    u64 cell_align = align > sizeof(de_atomic_u64) ? align : sizeof(de_atomic_u64);
    u64 data_offset = (sizeof(de_atomic_u64) + align - 1) & ~(u64)(align - 1);
    u64 stride = (data_offset + elem_size + cell_align - 1) & ~(cell_align - 1);
    if (stride > UINT32_MAX || stride * capacity > (u64)(SIZE_MAX / 2))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_mpmc_init: %u elements of %u bytes do not fit in memory", capacity,
                       elem_size);
    size_t bytes = (size_t)(stride * capacity);

    u8 *cells = (u8 *)DE_ALLOC(alloc, bytes, DE_CACHE_LINE, desc->tag);
    if (cells == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_mpmc_init: cannot allocate %zu bytes", bytes);

    q->cells = cells;
    q->mask = (u64)capacity - 1;
    q->capacity = capacity;
    q->elem_size = elem_size;
    q->stride = (u32)stride;
    q->data_offset = (u32)data_offset;
    q->tag = desc->tag;
    q->bytes = bytes;
    q->allocator = alloc;
    /* Cell i waits for the producer of position i. */
    for (u64 i = 0; i < capacity; i++)
        de_atomic_init_u64(cell_seq(q, i), 2 * i);
    de_atomic_init_u64(&q->enqueue_pos, 0);
    de_atomic_init_u64(&q->dequeue_pos, 0);
    return DE_OK;
}

void de_mpmc_destroy(de_mpmc *q) {
    if (q == NULL)
        return;
    if (q->cells != NULL)
        DE_FREE(q->allocator, q->cells, q->bytes);
    memset(q, 0, sizeof *q);
}

bool de_mpmc_try_push(de_mpmc *q, const void *elem) {
    u64 pos = de_atomic_load_u64(&q->enqueue_pos, DE_ATOMIC_RELAXED);
    de_atomic_u64 *seq;
    for (;;) {
        seq = cell_seq(q, pos);
        /* Acquire: the consumer that emptied this cell released it after reading the bytes. */
        u64 s = de_atomic_load_u64(seq, DE_ATOMIC_ACQUIRE);
        i64 diff = (i64)(s - 2 * pos);
        if (diff == 0) {
            /* The cell waits for position pos: try to own it. Relaxed: ownership only, the
             * cell's seq orders the data. On failure pos receives the current value. */
            if (de_atomic_cas_weak_u64(&q->enqueue_pos, &pos, pos + 1, DE_ATOMIC_RELAXED,
                                       DE_ATOMIC_RELAXED))
                break;
        } else if (diff < 0) {
            return false; /* still holds position pos - capacity: full */
        } else {
            pos = de_atomic_load_u64(&q->enqueue_pos, DE_ATOMIC_RELAXED); /* lost the race */
        }
    }
    memcpy(cell_data(q, pos), elem, q->elem_size);
    /* Publish: pairs with the consumer's acquire load of seq. */
    de_atomic_store_u64(seq, 2 * pos + 1, DE_ATOMIC_RELEASE);
    return true;
}

bool de_mpmc_try_pop(de_mpmc *q, void *out) {
    u64 pos = de_atomic_load_u64(&q->dequeue_pos, DE_ATOMIC_RELAXED);
    de_atomic_u64 *seq;
    for (;;) {
        seq = cell_seq(q, pos);
        /* Acquire: pairs with the producer's release, so the element bytes are visible. */
        u64 s = de_atomic_load_u64(seq, DE_ATOMIC_ACQUIRE);
        i64 diff = (i64)(s - (2 * pos + 1));
        if (diff == 0) {
            if (de_atomic_cas_weak_u64(&q->dequeue_pos, &pos, pos + 1, DE_ATOMIC_RELAXED,
                                       DE_ATOMIC_RELAXED))
                break;
        } else if (diff < 0) {
            return false; /* position pos not published yet: empty */
        } else {
            pos = de_atomic_load_u64(&q->dequeue_pos, DE_ATOMIC_RELAXED);
        }
    }
    if (out != NULL)
        memcpy(out, cell_data(q, pos), q->elem_size);
    /* Hand the cell to the producer one lap later: pairs with its acquire load of seq. */
    de_atomic_store_u64(seq, 2 * (pos + q->mask + 1), DE_ATOMIC_RELEASE);
    return true;
}

u32 de_mpmc_size_approx(const de_mpmc *q) {
    /* Dequeue first, so a concurrent pop cannot make the difference negative in the common
     * case; clamp both ways anyway, the two loads are not one snapshot. */
    u64 d = de_atomic_load_u64(&q->dequeue_pos, DE_ATOMIC_ACQUIRE);
    u64 e = de_atomic_load_u64(&q->enqueue_pos, DE_ATOMIC_ACQUIRE);
    i64 n = (i64)(e - d);
    if (n <= 0)
        return 0;
    return n > (i64)q->capacity ? q->capacity : (u32)n;
}

u32 de_mpmc_capacity(const de_mpmc *q) {
    return q->capacity;
}
