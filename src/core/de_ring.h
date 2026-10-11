/*
 * de_ring.h: fixed-capacity ring buffer (FIFO), single-threaded (engine-internal, SPEC §5.6).
 * Header-only.
 *
 * Include as "core/de_ring.h". Not part of the public API. For rings shared between threads use
 * the lock-free queues (DENG-152).
 *
 * The capacity is a power of two, fixed at init. The storage is allocated once by de_ring_init
 * (and given back by de_ring_free) or supplied by the caller (de_ring_init_buffer, never
 * allocates). Push, pop and peek never allocate.
 *
 * When the ring is full, a push either fails (default) or, with `overwrite` set, drops the
 * oldest element to make room: a "keep the last N" history.
 *
 *     de_ring r;
 *     DE_TRY(DE_RING_INIT(&r, frame_stat, alloc, 64, true));   // last 64 frames
 *     DE_RING_PUSH(&r, frame_stat, stat);
 *     for (u32 i = 0; i < de_ring_count(&r); ++i)
 *         use(DE_RING_AT(&r, frame_stat, i));                 // 0 = oldest
 *     de_ring_free(&r);
 *
 * Read and write positions are free-running u32 counters; the slot is the counter & (capacity
 * - 1). The capacity is at most 2^31 so that the count (write - read) never wraps ambiguously.
 */
#ifndef DE_CORE_RING_H
#define DE_CORE_RING_H

#include <dengine/de_memory.h>

#include <string.h>

DE_EXTERN_C_BEGIN

/** Largest capacity. */
#define DE_RING_MAX_CAPACITY (1u << 31)

/** A ring buffer. Fields are readable; change them only through the functions below. */
typedef struct de_ring {
    u8 *data;              /**< capacity * elem_size bytes */
    u32 elem_size;         /**< bytes per element (> 0) */
    u32 capacity;          /**< a power of two */
    u32 read;              /**< free-running read position */
    u32 write;             /**< free-running write position */
    bool overwrite;        /**< push to a full ring drops the oldest element */
    u32 elem_align;        /**< alignment of `data` */
    u32 tag;               /**< memory tag */
    u64 dropped;           /**< elements dropped by overwriting pushes since init */
    const de_allocator *a; /**< owner of `data`; NULL for caller storage */
} de_ring;

/* Common part of the init functions. */
static inline de_result de__ring_check(u32 elem_size, u32 capacity) {
    if (elem_size == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_ring: elem_size is 0");
    if (capacity == 0 || (capacity & (capacity - 1u)) != 0 || capacity > DE_RING_MAX_CAPACITY)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_ring: capacity %u is not a power of two in [1, 2^31]", capacity);
    return DE_OK;
}

/**
 * A ring of `capacity` (a power of two) elements of `elem_size` bytes over caller storage
 * `buffer` (at least capacity * elem_size bytes, suitably aligned). Never allocates.
 * DE_ERR_INVALID_ARGUMENT for a bad size or capacity.
 */
static inline de_result de_ring_init_buffer(de_ring *r, void *buffer, u32 elem_size, u32 capacity,
                                            bool overwrite) {
    memset(r, 0, sizeof *r);
    DE_TRY(de__ring_check(elem_size, capacity));
    DE_ASSERT(buffer != NULL);
    r->data = (u8 *)buffer;
    r->elem_size = elem_size;
    r->capacity = capacity;
    r->overwrite = overwrite;
    return DE_OK;
}

/**
 * A ring of `capacity` (a power of two) elements on allocator `a`; the storage is allocated
 * here, once. `elem_align` 0 means 16. DE_ERR_INVALID_ARGUMENT for a bad size or capacity,
 * DE_ERR_OUT_OF_MEMORY if the allocation fails.
 */
static inline de_result de_ring_init(de_ring *r, const de_allocator *a, u32 elem_size,
                                     u32 elem_align, u32 capacity, bool overwrite, u32 tag) {
    memset(r, 0, sizeof *r);
    DE_ASSERT(a != NULL);
    DE_TRY(de__ring_check(elem_size, capacity));
    if (elem_align == 0)
        elem_align = 16;
    const u64 bytes = (u64)elem_size * capacity;
    if (bytes > (u64)SIZE_MAX)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_ring: %u x %u bytes overflow size_t", capacity,
                       elem_size);
    void *p = DE_ALLOC(a, (size_t)bytes, elem_align, tag);
    if (p == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_ring: %u elements of %u bytes", capacity,
                            elem_size);
    r->data = (u8 *)p;
    r->elem_size = elem_size;
    r->capacity = capacity;
    r->overwrite = overwrite;
    r->elem_align = elem_align;
    r->tag = tag;
    r->a = a;
    return DE_OK;
}

/** Gives allocated storage back. The ring must be initialised again before use. */
static inline void de_ring_free(de_ring *r) {
    if (r->a != NULL && r->data != NULL)
        DE_FREE(r->a, r->data, (size_t)r->capacity * r->elem_size);
    memset(r, 0, sizeof *r);
}

static inline u32 de_ring_count(const de_ring *r) {
    return r->write - r->read;
}

static inline bool de_ring_empty(const de_ring *r) {
    return r->write == r->read;
}

static inline bool de_ring_full(const de_ring *r) {
    return r->write - r->read == r->capacity;
}

static inline void *de__ring_slot(const de_ring *r, u32 pos) {
    return r->data + (size_t)(pos & (r->capacity - 1u)) * r->elem_size;
}

/**
 * Appends a copy of `*elem` (zeroes when NULL). On a full ring: with `overwrite` the oldest
 * element is dropped (counted in `dropped`) and true is returned; without it nothing changes and
 * false is returned.
 */
static inline bool de_ring_push(de_ring *r, const void *elem) {
    if (de_ring_full(r)) {
        if (!r->overwrite)
            return false;
        r->read += 1;
        r->dropped += 1;
    }
    void *slot = de__ring_slot(r, r->write);
    if (elem != NULL)
        memcpy(slot, elem, r->elem_size);
    else
        memset(slot, 0, r->elem_size);
    r->write += 1;
    return true;
}

/** Removes the oldest element, copying it to `out` unless NULL. false if empty. */
static inline bool de_ring_pop(de_ring *r, void *out) {
    if (de_ring_empty(r))
        return false;
    if (out != NULL)
        memcpy(out, de__ring_slot(r, r->read), r->elem_size);
    r->read += 1;
    return true;
}

/** Pointer to the oldest element, or NULL if empty. Valid until the next push or pop. */
static inline void *de_ring_peek(const de_ring *r) {
    return de_ring_empty(r) ? NULL : de__ring_slot(r, r->read);
}

/** Pointer to the newest element, or NULL if empty. */
static inline void *de_ring_peek_newest(const de_ring *r) {
    return de_ring_empty(r) ? NULL : de__ring_slot(r, r->write - 1u);
}

/** Pointer to element `i` counted from the oldest (i < count, checked in Debug/Dev). */
static inline void *de_ring_at(const de_ring *r, u32 i) {
    DE_ASSERT(i < de_ring_count(r));
    return de__ring_slot(r, r->read + i);
}

/** Drops every element. Keeps the storage. */
static inline void de_ring_clear(de_ring *r) {
    r->read = r->write;
}

/* Checks a typed macro's element size and returns `r`. */
static inline de_ring *de__ring_typed(de_ring *r, size_t elem_size) {
    DE_ASSERT(r->elem_size == elem_size);
    (void)elem_size;
    return r;
}

/** de_ring_init for elements of type T, tag DE_MEM_GENERAL. */
#define DE_RING_INIT(r, T, alloc, capacity, overwrite)                                             \
    de_ring_init((r), (alloc), (u32)sizeof(T), (u32)DE_ALIGNOF(T), (capacity), (overwrite),        \
                 DE_MEM_GENERAL)
/** Pushes a value of type T (may contain commas). Returns bool. */
#define DE_RING_PUSH(r, T, ...) de_ring_push(de__ring_typed((r), sizeof(T)), (T[1]){__VA_ARGS__})
/** Pops into `*out` (a T pointer, or NULL). Returns bool. */
#define DE_RING_POP(r, T, out) de_ring_pop(de__ring_typed((r), sizeof(T)), 1 ? (out) : (T *)0)
/** Element `i` from the oldest, as an lvalue of type T. */
#define DE_RING_AT(r, T, i) (((T *)de_ring_at(de__ring_typed((r), sizeof(T)), (i)))[0])

DE_EXTERN_C_END

#endif /* DE_CORE_RING_H */
