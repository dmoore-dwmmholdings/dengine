/*
 * de_spsc.h: bounded single-producer / single-consumer ring queue (SPEC §5.6). Internal engine
 * header, not installed: include as "core/de_spsc.h".
 *
 * One thread pushes, one thread pops; neither ever blocks or takes a lock. Elements are copied
 * in and out by value (`elem_size` bytes each). The ring storage comes from the desc's
 * de_allocator (default: the heap) in de_spsc_init() and nothing is allocated afterwards.
 *
 * Which thread may call what:
 *   producer only : de_spsc_push
 *   consumer only : de_spsc_pop, de_spsc_peek
 *   any thread    : de_spsc_size_approx, de_spsc_capacity
 *   no other call running : de_spsc_init, de_spsc_destroy
 * Producer and consumer may change over time (hand the role to another thread through any
 * release/acquire pair, e.g. a mutex or a thread join), but at most one thread holds each role
 * at a time.
 *
 * Memory ordering (why acquire/release is enough and no seq_cst is needed):
 *   `tail` (next slot to write) is stored only by the producer; `head` (next slot to read) only
 *   by the consumer. Both are u64 counters that only grow; the slot is `index & mask`, so the
 *   queue holds `tail - head` elements and is full when that equals the capacity.
 *   - Push writes the slot with plain stores, then publishes it with store(tail, t + 1,
 *     release). Pop reads `tail` with acquire before it reads the slot. The release/acquire
 *     pair makes the slot's bytes visible to the consumer before it can see the new tail.
 *   - Pop reads the slot, then store(head, h + 1, release). Push reads `head` with acquire
 *     before it overwrites a slot. That pair guarantees the consumer's reads of the old bytes
 *     happened before the producer's writes of the new bytes (no torn element).
 *   - Each side keeps a private copy of the other side's index (`head_cache` on the producer's
 *     cache line, `tail_cache` on the consumer's) and reloads the shared index only when the
 *     copy says full or empty. A stale copy can only make the queue look fuller or emptier
 *     than it is, never the opposite, so it is safe; it removes most cross-core traffic.
 *   - Each side reads its own index with relaxed order: only that thread writes it.
 *   No ABA: the indices are 64-bit and never wrap in practice (2^64 operations).
 *
 * Layout: the producer's fields, the consumer's fields and the read-only fields sit on three
 * separate cache lines (DE_CACHE_LINE), so the two threads do not false-share. A de_spsc must
 * therefore be stored at DE_CACHE_LINE alignment: a global, a local, or memory from an
 * allocator asked for DE_ALIGNOF(de_spsc) (DE_NEW does that). Plain malloc is not enough.
 */
#ifndef DE_CORE_SPSC_H
#define DE_CORE_SPSC_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "platform/de_atomic.h"

/* Destructive-interference size: Apple Silicon cores move 128-byte lines, the other targets
 * (x86-64, other arm64) 64-byte lines. Also defined, identically, by core/de_mpmc.h. */
#ifndef DE_CACHE_LINE
#if defined(__APPLE__) && defined(__aarch64__)
#define DE_CACHE_LINE 128
#else
#define DE_CACHE_LINE 64
#endif
#endif

/* The indices are u64; a lock-based fallback would make the queue neither lock-free nor safe in
 * a signal handler. platform/de_atomic.h asserts the same; repeated here because the queues'
 * correctness argument depends on it. */
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "de_spsc needs lock-free 64-bit atomics");

/** Default capacity when de_spsc_desc.capacity is 0. */
#define DE_SPSC_DEFAULT_CAPACITY 256u
/** Largest accepted capacity (elements). */
#define DE_SPSC_MAX_CAPACITY (1u << 31)

/** Parameters of de_spsc_init(). `(de_spsc_desc){0}` is valid. */
typedef struct de_spsc_desc {
    /** Number of elements; a power of two in [1, DE_SPSC_MAX_CAPACITY]. 0 means
     * DE_SPSC_DEFAULT_CAPACITY. Every slot is usable: a queue of capacity 1 holds one element. */
    u32 capacity;
    /** Bytes per element. 0 means sizeof(void *) (a queue of pointers). */
    u32 elem_size;
    /** Alignment of each element in the ring (power of two, at most DE_CACHE_LINE). 0 means
     * the largest power of two that divides elem_size, capped at 16. */
    u32 elem_align;
    /** Memory tag passed to the allocator. */
    u32 tag;
    /** Where the ring storage comes from. NULL means de_heap_allocator(). */
    const de_allocator *allocator;
} de_spsc_desc;

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324) /* structure was padded due to alignment specifier: intended */
#endif

/** A SPSC queue. Treat every field as private; it is a struct only so callers can embed it. */
typedef struct de_spsc {
    /* Producer's cache line. */
    DE_ALIGN(DE_CACHE_LINE) de_atomic_u64 tail;
    u64 head_cache;
    /* Consumer's cache line. */
    DE_ALIGN(DE_CACHE_LINE) de_atomic_u64 head;
    u64 tail_cache;
    /* Read-only after init. */
    DE_ALIGN(DE_CACHE_LINE) u8 *slots;
    u64 mask;
    u32 capacity;
    u32 elem_size;
    u32 stride;
    u32 tag;
    size_t bytes;
    const de_allocator *allocator;
} de_spsc;

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

/**
 * Initialises `q` and allocates `capacity * stride` bytes from `desc->allocator`.
 * Fails with DE_ERR_INVALID_ARGUMENT (bad capacity or alignment) or
 * DE_ERR_OUT_OF_MEMORY, and sets the error context; `q` is then left zeroed.
 */
de_result de_spsc_init(de_spsc *q, const de_spsc_desc *desc);

/** Frees the ring. Elements still queued are dropped without notice. `q` is zeroed. A zeroed
 * (never initialised or failed) queue may be destroyed. */
void de_spsc_destroy(de_spsc *q);

/** Producer: copies `elem_size` bytes from `elem` into the queue. Returns false, and copies
 * nothing, when the queue is full. Never blocks. */
bool de_spsc_push(de_spsc *q, const void *elem);

/** Consumer: moves the oldest element into `out` (`elem_size` bytes) and removes it. `out` may
 * be NULL to drop the element. Returns false when the queue is empty. Never blocks. */
bool de_spsc_pop(de_spsc *q, void *out);

/** Consumer: the oldest element in place, or NULL when the queue is empty. The pointer stays
 * valid, and the element unchanged, until this consumer's next de_spsc_pop. */
void *de_spsc_peek(de_spsc *q);

/** Any thread: number of queued elements at some recent moment, in [0, capacity]. Exact only
 * when neither side is running. */
u32 de_spsc_size_approx(const de_spsc *q);

/** Any thread: the capacity in elements. */
u32 de_spsc_capacity(const de_spsc *q);

#endif /* DE_CORE_SPSC_H */
