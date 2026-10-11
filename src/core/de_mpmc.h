/*
 * de_mpmc.h: bounded multi-producer / multi-consumer queue (SPEC §5.6), after Dmitry Vyukov's
 * "bounded MPMC queue" (per-cell sequence numbers). Internal engine header, not installed:
 * include as "core/de_mpmc.h".
 *
 * Any number of threads push and pop at the same time. de_mpmc_try_push and de_mpmc_try_pop
 * never block and never take a lock: they return false when the queue is full or empty. A
 * thread is never stuck behind a stalled thread in try_push/try_pop; it can only see the queue
 * as full or empty for a moment longer (see "Stalls" below). Elements are copied in and out by
 * value (`elem_size` bytes). The cells come from the desc's de_allocator (default: the heap) in
 * de_mpmc_init() and nothing is allocated afterwards.
 *
 * Which thread may call what:
 *   any thread            : de_mpmc_try_push, de_mpmc_try_pop, de_mpmc_size_approx,
 *                           de_mpmc_capacity, and the blocking helpers below
 *   no other call running : de_mpmc_init, de_mpmc_destroy
 *
 * The algorithm. Two shared counters only grow: `enqueue_pos` (the next position a producer
 * claims) and `dequeue_pos` (the next position a consumer claims). Position p lives in cell
 * `p & mask`. Each cell has a sequence number `seq` that says what the cell is waiting for:
 *     seq == 2p      the cell is empty and waits for the producer of position p
 *     seq == 2p + 1  the cell holds the element of position p and waits for its consumer
 * At init, cell i has seq = 2i. A producer at position p reads the cell's seq:
 *     seq == 2p  -> claim p with a CAS on enqueue_pos, copy the element in,
 *                   store(seq, 2p + 1, release)
 *     seq <  2p  -> the cell still holds position p - capacity: the queue is full
 *     seq >  2p  -> another producer already took p: reload enqueue_pos and retry
 * A consumer at position p does the mirror image against 2p + 1, and after copying the element
 * out stores seq = 2(p + capacity), which hands the cell to the producer one lap later.
 *
 * Why 2p / 2p + 1 instead of Vyukov's p / p + 1: in the original, "full with position p" and
 * "empty, waiting for position p + capacity" are seq = p + 1 and seq = p + capacity. With
 * capacity 1 they are the same number, so a producer would overwrite an unread element
 * (Vyukov's code asserts capacity >= 2). Doubling keeps the two states apart for every
 * capacity, including 1. The comparisons use the signed difference of u64 values, so the
 * arithmetic stays correct even if the counters wrap (they do not, in practice).
 *
 * Memory ordering (acquire/release only; no seq_cst):
 *   - The element's bytes are published by the release store of the cell's seq, and a consumer
 *     reads seq with acquire before it copies the bytes out. Same in the other direction: a
 *     consumer's release of seq = 2(p + capacity) happens after its reads of the bytes, and the
 *     next producer's acquire of seq comes before its writes. So all element data is ordered
 *     by the cell's seq alone.
 *   - The CAS on enqueue_pos/dequeue_pos only decides which thread owns a position; it carries
 *     no data, so it is relaxed (success and failure). A stale read of a position is harmless:
 *     the seq check rejects it and the thread retries.
 *   - No ABA: a position is claimed once (the counters only grow, 64-bit), and a cell's seq
 *     names the exact position it waits for, not just "full" or "empty".
 *
 * Stalls. If a producer claims position p and is then descheduled before it publishes the
 * cell, consumers see the queue as empty at p (try_pop returns false) even if later positions
 * are already published. Nothing is lost: the element appears when that producer resumes. The
 * same holds for a stalled consumer and full. This is the design's known trade-off.
 *
 * Layout: enqueue_pos, dequeue_pos and the read-only fields sit on separate cache lines
 * (DE_CACHE_LINE). A de_mpmc must therefore be stored at DE_CACHE_LINE alignment: a global, a
 * local, or memory from an allocator asked for DE_ALIGNOF(de_mpmc) (DE_NEW does that).
 */
#ifndef DE_CORE_MPMC_H
#define DE_CORE_MPMC_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "platform/de_atomic.h"

/* Destructive-interference size: Apple Silicon cores move 128-byte lines, the other targets
 * (x86-64, other arm64) 64-byte lines. Also defined, identically, by core/de_spsc.h. */
#ifndef DE_CACHE_LINE
#if defined(__APPLE__) && defined(__aarch64__)
#define DE_CACHE_LINE 128
#else
#define DE_CACHE_LINE 64
#endif
#endif

/* Positions and cell sequences are u64; see de_spsc.h for why lock-free is required. */
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "de_mpmc needs lock-free 64-bit atomics");

/** Default capacity when de_mpmc_desc.capacity is 0. */
#define DE_MPMC_DEFAULT_CAPACITY 256u
/** Largest accepted capacity (elements). */
#define DE_MPMC_MAX_CAPACITY (1u << 31)

/** Parameters of de_mpmc_init(). `(de_mpmc_desc){0}` is valid. */
typedef struct de_mpmc_desc {
    /** Number of elements; a power of two in [1, DE_MPMC_MAX_CAPACITY]. 0 means
     * DE_MPMC_DEFAULT_CAPACITY. Every cell is usable: a queue of capacity 1 holds one element. */
    u32 capacity;
    /** Bytes per element. 0 means sizeof(void *) (a queue of pointers). */
    u32 elem_size;
    /** Alignment of each element (power of two, at most DE_CACHE_LINE). 0 means the largest
     * power of two that divides elem_size, capped at 16. */
    u32 elem_align;
    /** Memory tag passed to the allocator. */
    u32 tag;
    /** Where the cells come from. NULL means de_heap_allocator(). */
    const de_allocator *allocator;
} de_mpmc_desc;

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4324) /* structure was padded due to alignment specifier: intended */
#endif

/** A MPMC queue. Treat every field as private; it is a struct only so callers can embed it. */
typedef struct de_mpmc {
    DE_ALIGN(DE_CACHE_LINE) de_atomic_u64 enqueue_pos;
    DE_ALIGN(DE_CACHE_LINE) de_atomic_u64 dequeue_pos;
    /* Read-only after init. Cell i is at cells + i * stride: a de_atomic_u64 seq, then the
     * element at data_offset. */
    DE_ALIGN(DE_CACHE_LINE) u8 *cells;
    u64 mask;
    u32 capacity;
    u32 elem_size;
    u32 stride;
    u32 data_offset;
    u32 tag;
    size_t bytes;
    const de_allocator *allocator;
} de_mpmc;

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

/**
 * Initialises `q` and allocates `capacity` cells from `desc->allocator`.
 * Fails with DE_ERR_INVALID_ARGUMENT (bad capacity or alignment) or
 * DE_ERR_OUT_OF_MEMORY, and sets the error context; `q` is then left zeroed.
 */
de_result de_mpmc_init(de_mpmc *q, const de_mpmc_desc *desc);

/** Frees the cells. Elements still queued are dropped without notice. `q` is zeroed. A zeroed
 * (never initialised or failed) queue may be destroyed. */
void de_mpmc_destroy(de_mpmc *q);

/** Copies `elem_size` bytes from `elem` into the queue. Returns false, and copies nothing,
 * when the queue is full. Never blocks. */
bool de_mpmc_try_push(de_mpmc *q, const void *elem);

/** Moves the oldest available element into `out` (`elem_size` bytes; NULL drops it). Returns
 * false when the queue is empty. Never blocks. */
bool de_mpmc_try_pop(de_mpmc *q, void *out);

/** Number of queued elements at some recent moment, in [0, capacity]. Includes elements whose
 * push or pop is still in progress. Exact only when no thread is using the queue. */
u32 de_mpmc_size_approx(const de_mpmc *q);

/** The capacity in elements. */
u32 de_mpmc_capacity(const de_mpmc *q);

/* ---------- Blocking helpers (optional) ----------
 *
 * A consumer that has nothing else to do can sleep instead of polling try_pop. The helpers pair
 * the queue with a de_sem (DENG-147) whose count is the number of published elements no
 * sleeping consumer has claimed yet. The caller owns the semaphore (de_sem_init(&s, 0)).
 *
 * Rules, when a queue is used with a semaphore:
 *   - Every push goes through de_mpmc_push_post (or posts the semaphore once per successful
 *     try_push).
 *   - Every pop goes through de_mpmc_pop_wait / de_mpmc_pop_wait_timeout / de_mpmc_pop_try.
 *     A plain de_mpmc_try_pop would take an element that a sleeper was promised, and that
 *     sleeper would then spin until another element arrives.
 * A waiter that got a count may still find its element unpublished for a moment (a producer
 * that claimed an earlier position has not finished copying; see "Stalls"), so it spins with
 * de_cpu_pause until the element appears. That spin ends as soon as the stalled producer runs.
 *
 * These are static inline, so de_core has no link-time dependency on de_platform (platform
 * links core, not the other way round); a caller of the helpers links de_platform. */

/** Pushes `elem` and, on success, posts `items` once. Returns false (no post) when full. */
static inline bool de_mpmc_push_post(de_mpmc *q, de_sem *items, const void *elem) {
    if (!de_mpmc_try_push(q, elem))
        return false;
    de_sem_post(items, 1);
    return true;
}

/* Internal: after a successful wait, take the element we were promised. */
static inline void de__mpmc_pop_claimed(de_mpmc *q, void *out) {
    while (!de_mpmc_try_pop(q, out))
        de_cpu_pause();
}

/** Sleeps until an element is available, then pops it into `out` (NULL drops it). */
static inline void de_mpmc_pop_wait(de_mpmc *q, de_sem *items, void *out) {
    de_sem_wait(items);
    de__mpmc_pop_claimed(q, out);
}

/** Like de_mpmc_pop_wait, but gives up after `timeout_ns`. Returns false on timeout. */
static inline bool de_mpmc_pop_wait_timeout(de_mpmc *q, de_sem *items, void *out, u64 timeout_ns) {
    if (!de_sem_wait_timeout(items, timeout_ns))
        return false;
    de__mpmc_pop_claimed(q, out);
    return true;
}

/** Pops without sleeping, keeping the semaphore count right. Returns false when empty. */
static inline bool de_mpmc_pop_try(de_mpmc *q, de_sem *items, void *out) {
    if (!de_sem_try_wait(items))
        return false;
    de__mpmc_pop_claimed(q, out);
    return true;
}

#endif /* DE_CORE_MPMC_H */
