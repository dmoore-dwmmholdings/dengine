/*
 * deque.c: Chase-Lev work-stealing deque (Lê et al. 2013). The ordering argument is in
 * deque.h; the comments here say which step of the paper each line is.
 */
#include "task/deque.h"

#include <stddef.h>

static i64 de__diff(u64 a, u64 b) {
    return (i64)(a - b);
}

static de__deque_ring *de__ring_alloc(const de_allocator *a, u64 capacity, u32 tag) {
    const size_t bytes = sizeof(de__deque_ring) + (size_t)capacity * sizeof(de_atomic_ptr);
    de__deque_ring *r = (de__deque_ring *)DE_ALLOC(a, bytes, DE_ALIGNOF(de__deque_ring), tag);
    if (r == NULL)
        return NULL;
    r->retired_next = NULL;
    r->mask = capacity - 1;
    for (u64 i = 0; i < capacity; ++i)
        de_atomic_init_ptr(&r->slots[i], NULL);
    return r;
}

static void de__ring_free(const de_allocator *a, de__deque_ring *r) {
    const size_t bytes = sizeof(de__deque_ring) + (size_t)(r->mask + 1) * sizeof(de_atomic_ptr);
    DE_FREE(a, r, bytes);
}

de_result de__deque_init(de__deque *d, u32 capacity, const de_allocator *allocator, u32 tag) {
    de_atomic_init_u64(&d->top, 0);
    de_atomic_init_u64(&d->bottom, 0);
    de_atomic_init_ptr(&d->ring, NULL);
    d->retired = NULL;
    d->allocator = NULL;
    d->tag = tag;
    if (capacity == 0)
        capacity = DE__DEQUE_DEFAULT_CAPACITY;
    if ((capacity & (capacity - 1)) != 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "deque capacity %u is not a power of two",
                       capacity);
    const de_allocator *a = allocator != NULL ? allocator : de_heap_allocator();
    de__deque_ring *r = de__ring_alloc(a, capacity, tag);
    if (r == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "deque ring of %u items", capacity);
    d->allocator = a;
    de_atomic_init_ptr(&d->ring, r);
    return DE_OK;
}

void de__deque_destroy(de__deque *d) {
    de__deque_ring *r = (de__deque_ring *)de_atomic_load_ptr(&d->ring, DE_ATOMIC_RELAXED);
    if (r != NULL)
        de__ring_free(d->allocator, r);
    while (d->retired != NULL) {
        de__deque_ring *next = d->retired->retired_next;
        de__ring_free(d->allocator, d->retired);
        d->retired = next;
    }
    de_atomic_init_ptr(&d->ring, NULL);
}

/* Owner: replaces `old` by a ring twice the size holding items [t, b). */
static de__deque_ring *de__deque_grow(de__deque *d, de__deque_ring *old, u64 t, u64 b) {
    const u64 capacity = (old->mask + 1) * 2;
    de__deque_ring *r = de__ring_alloc(d->allocator, capacity, d->tag);
    if (r == NULL)
        return NULL;
    for (u64 i = t; i != b; ++i) {
        void *item = de_atomic_load_ptr(&old->slots[i & old->mask], DE_ATOMIC_RELAXED);
        de_atomic_store_ptr(&r->slots[i & r->mask], item, DE_ATOMIC_RELAXED);
    }
    /* Thieves that loaded `old` keep reading it until they reload the ring; it stays valid. */
    old->retired_next = d->retired;
    d->retired = old;
    de_atomic_store_ptr(&d->ring, r, DE_ATOMIC_RELEASE);
    return r;
}

bool de__deque_push(de__deque *d, void *item) {
    DE_ASSERT(item != NULL);
    const u64 b = de_atomic_load_u64(&d->bottom, DE_ATOMIC_RELAXED);
    const u64 t = de_atomic_load_u64(&d->top, DE_ATOMIC_ACQUIRE);
    de__deque_ring *r = (de__deque_ring *)de_atomic_load_ptr(&d->ring, DE_ATOMIC_RELAXED);
    if (de__diff(b, t) > (i64)r->mask) { /* full: b - t == capacity */
        r = de__deque_grow(d, r, t, b);
        if (r == NULL)
            return false;
    }
    de_atomic_store_ptr(&r->slots[b & r->mask], item, DE_ATOMIC_RELAXED);
    /* Paper: release fence + relaxed store. Publishes the item and the record it points to. */
    de_atomic_store_u64(&d->bottom, b + 1, DE_ATOMIC_RELEASE);
    return true;
}

void *de__deque_pop(de__deque *d) {
    const u64 b = de_atomic_load_u64(&d->bottom, DE_ATOMIC_RELAXED) - 1;
    de__deque_ring *r = (de__deque_ring *)de_atomic_load_ptr(&d->ring, DE_ATOMIC_RELAXED);
    /* Paper: relaxed store, seq_cst fence, relaxed load of top. */
    de_atomic_store_u64(&d->bottom, b, DE_ATOMIC_SEQ_CST);
    u64 t = de_atomic_load_u64(&d->top, DE_ATOMIC_SEQ_CST);
    if (de__diff(t, b) > 0) { /* empty: restore bottom */
        de_atomic_store_u64(&d->bottom, b + 1, DE_ATOMIC_RELEASE);
        return NULL;
    }
    void *item = de_atomic_load_ptr(&r->slots[b & r->mask], DE_ATOMIC_RELAXED);
    if (t == b) {
        /* The last item: race the thieves for it through top. */
        if (!de_atomic_cas_u64(&d->top, &t, t + 1, DE_ATOMIC_SEQ_CST, DE_ATOMIC_RELAXED))
            item = NULL; /* a thief took it */
        de_atomic_store_u64(&d->bottom, b + 1, DE_ATOMIC_RELEASE);
    }
    return item;
}

de__steal_result de__deque_steal(de__deque *d, void **out) {
    u64 t = de_atomic_load_u64(&d->top, DE_ATOMIC_SEQ_CST);
    const u64 b = de_atomic_load_u64(&d->bottom, DE_ATOMIC_SEQ_CST);
    if (de__diff(t, b) >= 0)
        return DE__STEAL_EMPTY;
    de__deque_ring *r = (de__deque_ring *)de_atomic_load_ptr(&d->ring, DE_ATOMIC_ACQUIRE);
    void *item = de_atomic_load_ptr(&r->slots[t & r->mask], DE_ATOMIC_RELAXED);
    if (!de_atomic_cas_u64(&d->top, &t, t + 1, DE_ATOMIC_SEQ_CST, DE_ATOMIC_RELAXED))
        return DE__STEAL_ABORT;
    *out = item;
    return DE__STEAL_OK;
}

u64 de__deque_size_approx(const de__deque *d) {
    const u64 t = de_atomic_load_u64(&d->top, DE_ATOMIC_ACQUIRE);
    const u64 b = de_atomic_load_u64(&d->bottom, DE_ATOMIC_ACQUIRE);
    const i64 n = de__diff(b, t);
    return n > 0 ? (u64)n : 0;
}

u64 de__deque_capacity(const de__deque *d) {
    const de__deque_ring *r =
        (const de__deque_ring *)de_atomic_load_ptr(&d->ring, DE_ATOMIC_ACQUIRE);
    return r != NULL ? r->mask + 1 : 0;
}
