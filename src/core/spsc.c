/* spsc.c: bounded single-producer / single-consumer ring (DENG-152). The ordering argument is
 * in core/de_spsc.h. */
#include "core/de_spsc.h"

#include <string.h>

static bool is_pow2_u32(u32 v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/* Largest power of two that divides `size`, capped at 16. */
static u32 natural_align(u32 size) {
    u32 a = size & (0u - size);
    return a > 16 ? 16 : a;
}

de_result de_spsc_init(de_spsc *q, const de_spsc_desc *desc) {
    if (q == NULL || desc == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_spsc_init: q and desc are required");
    memset(q, 0, sizeof *q);

    u32 capacity = desc->capacity ? desc->capacity : DE_SPSC_DEFAULT_CAPACITY;
    u32 elem_size = desc->elem_size ? desc->elem_size : (u32)sizeof(void *);
    u32 align = desc->elem_align ? desc->elem_align : natural_align(elem_size);

    if (!is_pow2_u32(capacity) || capacity > DE_SPSC_MAX_CAPACITY)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_spsc_init: capacity %u is not a power of two in [1, 2^31]", capacity);
    if (!is_pow2_u32(align) || align > DE_CACHE_LINE)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_spsc_init: elem_align %u is not a power of two <= %u", align,
                       (u32)DE_CACHE_LINE);
    const de_allocator *alloc = desc->allocator != NULL ? desc->allocator : de_heap_allocator();

    /* Round each slot up to its alignment so every element in the ring is aligned. */
    u64 stride = ((u64)elem_size + align - 1) & ~(u64)(align - 1);
    if (stride > UINT32_MAX || stride * capacity > (u64)(SIZE_MAX / 2))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_spsc_init: %u elements of %u bytes do not fit in memory", capacity,
                       elem_size);
    size_t bytes = (size_t)(stride * capacity);

    u8 *slots = (u8 *)DE_ALLOC(alloc, bytes, DE_CACHE_LINE, desc->tag);
    if (slots == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_spsc_init: cannot allocate %zu bytes", bytes);

    de_atomic_init_u64(&q->tail, 0);
    de_atomic_init_u64(&q->head, 0);
    q->head_cache = 0;
    q->tail_cache = 0;
    q->slots = slots;
    q->mask = (u64)capacity - 1;
    q->capacity = capacity;
    q->elem_size = elem_size;
    q->stride = (u32)stride;
    q->tag = desc->tag;
    q->bytes = bytes;
    q->allocator = alloc;
    return DE_OK;
}

void de_spsc_destroy(de_spsc *q) {
    if (q == NULL)
        return;
    if (q->slots != NULL)
        DE_FREE(q->allocator, q->slots, q->bytes);
    memset(q, 0, sizeof *q);
}

bool de_spsc_push(de_spsc *q, const void *elem) {
    u64 t = de_atomic_load_u64(&q->tail, DE_ATOMIC_RELAXED); /* only we write tail */
    if (t - q->head_cache == q->capacity) {
        /* Looks full: refresh. Acquire pairs with the consumer's release of head, so its reads
         * of the slot we are about to overwrite are complete. */
        q->head_cache = de_atomic_load_u64(&q->head, DE_ATOMIC_ACQUIRE);
        if (t - q->head_cache == q->capacity)
            return false;
    }
    memcpy(q->slots + (size_t)(t & q->mask) * q->stride, elem, q->elem_size);
    /* Publish the slot: pairs with the consumer's acquire load of tail. */
    de_atomic_store_u64(&q->tail, t + 1, DE_ATOMIC_RELEASE);
    return true;
}

/* Consumer: the slot of the oldest element, or NULL when empty. */
static u8 *front_slot(de_spsc *q, u64 h) {
    if (h == q->tail_cache) {
        /* Looks empty: refresh. Acquire pairs with the producer's release of tail. */
        q->tail_cache = de_atomic_load_u64(&q->tail, DE_ATOMIC_ACQUIRE);
        if (h == q->tail_cache)
            return NULL;
    }
    return q->slots + (size_t)(h & q->mask) * q->stride;
}

bool de_spsc_pop(de_spsc *q, void *out) {
    u64 h = de_atomic_load_u64(&q->head, DE_ATOMIC_RELAXED); /* only we write head */
    u8 *slot = front_slot(q, h);
    if (slot == NULL)
        return false;
    if (out != NULL)
        memcpy(out, slot, q->elem_size);
    /* Release the slot: pairs with the producer's acquire load of head. */
    de_atomic_store_u64(&q->head, h + 1, DE_ATOMIC_RELEASE);
    return true;
}

void *de_spsc_peek(de_spsc *q) {
    return front_slot(q, de_atomic_load_u64(&q->head, DE_ATOMIC_RELAXED));
}

u32 de_spsc_size_approx(const de_spsc *q) {
    /* Head first: the acquire load of head makes the tail the consumer saw before it moved head
     * visible, so t >= h. A later tail may be more than a capacity ahead of the head we read;
     * clamp. The signed check is defensive. */
    u64 h = de_atomic_load_u64(&q->head, DE_ATOMIC_ACQUIRE);
    u64 t = de_atomic_load_u64(&q->tail, DE_ATOMIC_ACQUIRE);
    i64 n = (i64)(t - h);
    if (n <= 0)
        return 0;
    return n > (i64)q->capacity ? q->capacity : (u32)n;
}

u32 de_spsc_capacity(const de_spsc *q) {
    return q->capacity;
}
