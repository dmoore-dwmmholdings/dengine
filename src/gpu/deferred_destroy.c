/* deferred_destroy.c — objects wait here between their destroy call and the GPU finishing with
 * them (DENG-281).
 *
 * A destroy call records, per queue, the internal timeline value of the last submission at that
 * moment. The entry is released once every queue's completed value has reached its recorded
 * value. Each queue's submitted value only grows, so recorded values (almost) never decrease
 * along the FIFO, and a collect stops at the first entry that is not complete. Two threads that
 * destroy at the same moment can push in the other order than they read the values; the entry
 * behind then waits for the one in front, which delays it but never releases anything early.
 */
#include "gpu/gpu_backend.h"

#include <string.h>

void de__gpu_deferred_init(de__gpu_deferred *q, const de_allocator *alloc) {
    memset(q, 0, sizeof *q);
    q->alloc = alloc ? alloc : de_heap_allocator();
    de_mutex_init(&q->lock);
}

void de__gpu_deferred_destroy(de__gpu_deferred *q) {
    if (q->ring)
        DE_FREE(q->alloc, q->ring, sizeof(de__gpu_deferred_entry) * q->cap);
    de_mutex_destroy(&q->lock);
    memset(q, 0, sizeof *q);
}

/* Doubles the ring, unwrapping it to start at 0. Called with the lock held. */
static de_result grow(de__gpu_deferred *q) {
    u32 cap = q->cap ? q->cap * 2u : 256u;
    de__gpu_deferred_entry *ring =
        (de__gpu_deferred_entry *)DE_ALLOC(q->alloc, sizeof(de__gpu_deferred_entry) * cap,
                                           DE_ALIGNOF(de__gpu_deferred_entry), DE_MEM_RENDER);
    if (!ring)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "gpu deferred-destroy queue: %u entries", cap);
    for (u32 i = 0; i < q->count; ++i)
        ring[i] = q->ring[(q->head + i) % q->cap];
    if (q->ring)
        DE_FREE(q->alloc, q->ring, sizeof(de__gpu_deferred_entry) * q->cap);
    q->ring = ring;
    q->cap = cap;
    q->head = 0;
    return DE_OK;
}

de_result de__gpu_deferred_push(de__gpu_deferred *q, u32 kind, u32 index,
                                const u64 values[DE_QUEUE_COUNT_]) {
    de_mutex_lock(&q->lock);
    if (q->count == q->cap) {
        de_result r = grow(q);
        if (r != DE_OK) {
            de_mutex_unlock(&q->lock);
            return r;
        }
    }
    de__gpu_deferred_entry *e = &q->ring[(q->head + q->count) % q->cap];
    memcpy(e->values, values, sizeof e->values);
    e->kind = kind;
    e->index = index;
    ++q->count;
    de_mutex_unlock(&q->lock);
    return DE_OK;
}

static bool complete(const de__gpu_deferred_entry *e, const u64 completed[DE_QUEUE_COUNT_]) {
    if (!completed)
        return true;
    for (u32 i = 0; i < DE_QUEUE_COUNT_; ++i)
        if (e->values[i] > completed[i])
            return false;
    return true;
}

u32 de__gpu_deferred_collect(de__gpu_deferred *q, const u64 completed[DE_QUEUE_COUNT_],
                             de__gpu_reclaim_fn fn, void *user) {
    /* Entries are popped in batches under the lock and reclaimed outside it: a reclaim may
     * destroy other objects (a swapchain its images), which pushes onto this queue again. */
    enum { BATCH = 64 };
    de__gpu_deferred_entry batch[BATCH];
    u32 total = 0;
    for (;;) {
        u32 n = 0;
        de_mutex_lock(&q->lock);
        while (n < BATCH && q->count > 0 && complete(&q->ring[q->head], completed)) {
            batch[n++] = q->ring[q->head];
            q->head = (q->head + 1u) % q->cap;
            --q->count;
        }
        de_mutex_unlock(&q->lock);
        for (u32 i = 0; i < n; ++i)
            fn(user, batch[i].kind, batch[i].index);
        total += n;
        if (n < BATCH)
            return total;
    }
}

u32 de__gpu_deferred_pending(de__gpu_deferred *q) {
    de_mutex_lock(&q->lock);
    u32 n = q->count;
    de_mutex_unlock(&q->lock);
    return n;
}
