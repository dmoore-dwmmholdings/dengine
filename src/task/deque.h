/*
 * deque.h: the Chase-Lev work-stealing deque of one frame worker (SPEC §8). Internal to
 * src/task/; include as "task/deque.h".
 *
 * The owner pushes and pops at the bottom (LIFO, good cache locality for nested work). Any
 * other thread steals at the top (FIFO, takes the oldest and usually largest piece of work).
 * The algorithm is Chase and Lev (SPAA 2005) in the C11-atomics formulation of Lê, Pop, Cohen
 * and Zappa Nardelli, "Correct and Efficient Work-Stealing for Weak Memory Models" (PPoPP 2013).
 * Items are non-NULL pointers.
 *
 * Which thread may call what:
 *   owner only        : de__deque_push, de__deque_pop
 *   any thread        : de__deque_steal, de__deque_size_approx
 *   no other call     : de__deque_init, de__deque_destroy
 *
 * Memory order. Lê et al. use relaxed operations plus standalone seq_cst fences in pop and
 * steal. TSan does not model standalone fences (see the Trama page "Decision: Atomics go
 * through platform/de_atomic.h"), so this version puts the ordering on the operations instead:
 *   - push: the item goes into its slot (relaxed atomic store), then `bottom` is stored with
 *     release. A thief that reads `bottom` with acquire sees the item and everything the owner
 *     wrote before it (the task record's fields).
 *   - pop: `bottom = b - 1` is a seq_cst store and the following load of `top` is seq_cst, which
 *     gives the store-load order the fence gave. The stores that restore `bottom` are release
 *     (relaxed in the paper), so every value of `bottom` a thief can read is a release store and
 *     synchronizes with it.
 *   - steal: `top` and `bottom` are loaded seq_cst (the paper: acquire, seq_cst fence, acquire),
 *     the buffer pointer with acquire (the paper: consume), the slot relaxed, and the claim is a
 *     seq_cst CAS on `top`, as in the paper.
 *   - Slots are atomics, so a thief that reads a slot the owner is overwriting (it then loses
 *     the CAS and drops the value) is not a data race.
 *
 * Growth. When a push finds the ring full, the owner copies the live range into a ring twice
 * the size and publishes it with a release store. Thieves may still read the old ring, so it is
 * kept on a retired list and freed only by de__deque_destroy. The ring never shrinks.
 */
#ifndef DE_TASK_DEQUE_H
#define DE_TASK_DEQUE_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "core/de_mpmc.h" /* DE_CACHE_LINE */
#include "platform/de_atomic.h"

/** Initial ring capacity when de__deque_init gets 0. */
#define DE__DEQUE_DEFAULT_CAPACITY 1024u

/**
 * One ring: this header, then mask + 1 slots (de_atomic_ptr) in the same allocation. Index i
 * lives in de__ring_slots(r)[i & mask]. The slots are not a flexible array member because MSVC
 * cl reports one as C4200 ("nonstandard extension") at /W4.
 */
typedef struct de__deque_ring {
    struct de__deque_ring *retired_next; /* owner only: the retired list */
    u64 mask;
} de__deque_ring;

/* The first slot starts right after the header: it must be aligned there, and the allocation
 * (aligned for the header) must be aligned enough for it. */
_Static_assert(sizeof(de__deque_ring) % DE_ALIGNOF(de_atomic_ptr) == 0,
               "deque ring header size must keep the slots aligned");
_Static_assert(DE_ALIGNOF(de_atomic_ptr) <= DE_ALIGNOF(de__deque_ring),
               "deque ring alignment must cover the slot alignment");

/** The slot array of `r`. */
static inline de_atomic_ptr *de__ring_slots(de__deque_ring *r) {
    return (de_atomic_ptr *)(r + 1);
}

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324) /* structure was padded due to alignment specifier: intended */
#endif

/** A deque. Treat every field as private. Store it at DE_CACHE_LINE alignment. */
typedef struct de__deque {
    /* Thieves CAS `top`; the owner writes `bottom`. Separate lines so steals and pushes do not
     * bounce one line between cores. Both are indices that only grow, compared as signed
     * differences. */
    DE_ALIGN(DE_CACHE_LINE) de_atomic_u64 top;
    DE_ALIGN(DE_CACHE_LINE) de_atomic_u64 bottom;
    de_atomic_ptr ring;      /* de__deque_ring *: owner stores (release), thieves load (acquire) */
    de__deque_ring *retired; /* owner only */
    const de_allocator *allocator;
    u32 tag;
} de__deque;

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

typedef enum de__steal_result {
    DE__STEAL_OK,    /* *out holds the item */
    DE__STEAL_EMPTY, /* nothing to steal */
    DE__STEAL_ABORT  /* lost a race with the owner or another thief; worth retrying */
} de__steal_result;

/**
 * Initialises `d` with a ring of `capacity` items (a power of two; 0 = default) from
 * `allocator` (NULL = de_heap_allocator()). DE_ERR_INVALID_ARGUMENT or DE_ERR_OUT_OF_MEMORY on
 * failure (error context set; `d` is left zeroed and may be destroyed).
 */
de_result de__deque_init(de__deque *d, u32 capacity, const de_allocator *allocator, u32 tag);
/** Frees the current and every retired ring. Items still queued are dropped. */
void de__deque_destroy(de__deque *d);
/** Owner: pushes non-NULL `item` at the bottom. false only when the ring must grow and the
 * allocator fails (the item is then not queued). */
bool de__deque_push(de__deque *d, void *item);
/** Owner: pops the most recently pushed item, or NULL when the deque is empty. */
void *de__deque_pop(de__deque *d);
/** Any thread: takes the oldest item into *out. */
de__steal_result de__deque_steal(de__deque *d, void **out);
/** Any thread: the number of items at some recent moment (0 when empty). */
u64 de__deque_size_approx(const de__deque *d);
/** Owner (or no concurrent push): the current ring capacity. For tests. */
u64 de__deque_capacity(const de__deque *d);

#endif /* DE_TASK_DEQUE_H */
