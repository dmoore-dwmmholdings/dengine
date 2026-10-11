/* tracking_alloc.c: the tracking/counting allocator (SPEC §5.3).
 *
 * A de_tracking_allocator is caller storage holding a copy of the backing allocator, the tag,
 * relaxed atomic counters and the failure budget, so one tracker can be shared by threads.
 *
 * Reporting: a tracker reports to the tag stats and hooks (mem_tags.c) unless its backing does
 * already (the heap in DE_MEM_TRACKING builds, or another tracker, which either reports or sits
 * on something that does). That rule makes every block reported exactly once.
 *
 * A reporting realloc reports the free of the old block before calling the backing realloc
 * (which may hand the old block to another thread), then the alloc of the result. If the backing
 * realloc fails, the old block is reported again as allocated, so the stats stay right.
 */
#include <dengine/de_memory.h>

#include "core/de_compiler.h"
#include "core/mem_internal.h"
#include "platform/de_atomic.h"

#include <string.h>

typedef struct de__tracker {
    de_allocator backing;
    u32 tag;
    bool reports;
    de_atomic_u32 armed;  /* failure injection on */
    de_atomic_u64 budget; /* requests still allowed to succeed while armed */
    de_atomic_u64 alloc_count;
    de_atomic_u64 realloc_count;
    de_atomic_u64 free_count;
    de_atomic_u64 fail_count;
    de_atomic_u64 bytes_live;
    de_atomic_u64 bytes_peak;
} de__tracker;

DE_STATIC_ASSERT(sizeof(de__tracker) <= sizeof(((de_tracking_allocator *)0)->opaque_),
                 "de_tracking_allocator storage is too small");
DE_STATIC_ASSERT(DE_ALIGNOF(de__tracker) <= 8, "de_tracking_allocator storage is 8-aligned");

static inline de__tracker *de__tracker_of(de_tracking_allocator *t) {
    return (de__tracker *)(void *)t->opaque_;
}

static inline const de__tracker *de__tracker_of_c(const de_tracking_allocator *t) {
    return (const de__tracker *)(const void *)t->opaque_;
}

/* Takes one request from the failure budget; false when the request must fail. */
static bool de__take_budget(de__tracker *k) {
    if (DE_LIKELY(de_atomic_load_u32(&k->armed, DE_ATOMIC_ACQUIRE) == 0))
        return true;
    u64 b = de_atomic_load_u64(&k->budget, DE_ATOMIC_RELAXED);
    do {
        if (b == 0)
            return false;
    } while (!de_atomic_cas_weak_u64(&k->budget, &b, b - 1, DE_ATOMIC_RELAXED, DE_ATOMIC_RELAXED));
    return true;
}

static void de__add_live(de__tracker *k, u64 add, u64 sub) {
    const u64 live = de_atomic_fetch_add_u64(&k->bytes_live, add, DE_ATOMIC_RELAXED) + add;
    if (sub != 0)
        de_atomic_fetch_sub_u64(&k->bytes_live, sub, DE_ATOMIC_RELAXED);
    const u64 now = live - sub;
    u64 peak = de_atomic_load_u64(&k->bytes_peak, DE_ATOMIC_RELAXED);
    while (now > peak && !de_atomic_cas_weak_u64(&k->bytes_peak, &peak, now, DE_ATOMIC_RELAXED,
                                                 DE_ATOMIC_RELAXED)) {
    }
}

static void *de__injected_failure(de__tracker *k, size_t size) {
    de_atomic_fetch_add_u64(&k->fail_count, 1, DE_ATOMIC_RELAXED);
    DE_FAIL(DE_ERR_OUT_OF_MEMORY, "tracking allocator: injected failure (%zu bytes)", size);
    return NULL;
}

void *de__tracking_alloc_fn(void *user, size_t size, size_t align, u32 tag) {
    (void)tag;
    de__tracker *k = (de__tracker *)user;
    if (!de__take_budget(k))
        return de__injected_failure(k, size);
    void *p = DE_ALLOC(&k->backing, size, align, k->tag);
    if (p == NULL) {
        de_atomic_fetch_add_u64(&k->fail_count, 1, DE_ATOMIC_RELAXED);
        return NULL;
    }
    de_atomic_fetch_add_u64(&k->alloc_count, 1, DE_ATOMIC_RELAXED);
    de__add_live(k, (u64)size, 0);
    if (k->reports)
        de__mem_report_alloc(p, size, k->tag);
    return p;
}

static void *de__tracking_realloc_fn(void *user, void *ptr, size_t old_size, size_t new_size,
                                     size_t align, u32 tag) {
    if (ptr == NULL)
        return de__tracking_alloc_fn(user, new_size, align, tag);
    de__tracker *k = (de__tracker *)user;
    if (!de__take_budget(k))
        return de__injected_failure(k, new_size);
    if (k->reports)
        de__mem_report_free(ptr, old_size, k->tag);
    void *q = DE_REALLOC(&k->backing, ptr, old_size, new_size, align, k->tag);
    if (q == NULL) {
        if (k->reports)
            de__mem_report_alloc(ptr, old_size, k->tag); /* the old block is still live */
        de_atomic_fetch_add_u64(&k->fail_count, 1, DE_ATOMIC_RELAXED);
        return NULL;
    }
    de_atomic_fetch_add_u64(&k->realloc_count, 1, DE_ATOMIC_RELAXED);
    de__add_live(k, (u64)new_size, (u64)old_size);
    if (k->reports)
        de__mem_report_alloc(q, new_size, k->tag);
    return q;
}

static void de__tracking_free_fn(void *user, void *ptr, size_t size) {
    if (ptr == NULL)
        return;
    de__tracker *k = (de__tracker *)user;
    if (k->reports)
        de__mem_report_free(ptr, size, k->tag);
    DE_FREE(&k->backing, ptr, size);
    de_atomic_fetch_add_u64(&k->free_count, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_sub_u64(&k->bytes_live, (u64)size, DE_ATOMIC_RELAXED);
}

/* Whether blocks from `b` are reported already, by `b` or by something below it. */
static bool de__backing_reports(const de_allocator *b) {
    if (b->alloc == de__tracking_alloc_fn)
        return true;
#if DE_MEM_TRACKING
    if (b->alloc == de__heap_alloc_fn)
        return true;
#endif
    return false;
}

de_allocator de_tracking_allocator_init(de_tracking_allocator *t, const de_allocator *backing,
                                        u32 tag) {
    DE_ASSERT(t != NULL);
    memset(t, 0, sizeof *t);
    de__tracker *k = de__tracker_of(t);
    k->backing = backing != NULL ? *backing : *de_heap_allocator();
    k->tag = tag;
    k->reports = !de__backing_reports(&k->backing);
    de_atomic_init_u32(&k->armed, 0);
    de_atomic_init_u64(&k->budget, 0);
    de_atomic_init_u64(&k->alloc_count, 0);
    de_atomic_init_u64(&k->realloc_count, 0);
    de_atomic_init_u64(&k->free_count, 0);
    de_atomic_init_u64(&k->fail_count, 0);
    de_atomic_init_u64(&k->bytes_live, 0);
    de_atomic_init_u64(&k->bytes_peak, 0);

    de_allocator a;
    a.alloc = de__tracking_alloc_fn;
    a.realloc = de__tracking_realloc_fn;
    a.free = de__tracking_free_fn;
    a.user = k;
    return a;
}

de_tracking_stats de_tracking_allocator_stats(const de_tracking_allocator *t) {
    const de__tracker *k = de__tracker_of_c(t);
    de_tracking_stats s;
    s.alloc_count = de_atomic_load_u64(&k->alloc_count, DE_ATOMIC_RELAXED);
    s.realloc_count = de_atomic_load_u64(&k->realloc_count, DE_ATOMIC_RELAXED);
    s.free_count = de_atomic_load_u64(&k->free_count, DE_ATOMIC_RELAXED);
    s.fail_count = de_atomic_load_u64(&k->fail_count, DE_ATOMIC_RELAXED);
    s.blocks_live = s.alloc_count - s.free_count;
    s.bytes_live = de_atomic_load_u64(&k->bytes_live, DE_ATOMIC_RELAXED);
    s.bytes_peak = de_atomic_load_u64(&k->bytes_peak, DE_ATOMIC_RELAXED);
    return s;
}

void de_tracking_allocator_fail_after(de_tracking_allocator *t, i64 n) {
    de__tracker *k = de__tracker_of(t);
    if (n < 0) {
        de_atomic_store_u32(&k->armed, 0, DE_ATOMIC_RELEASE);
        return;
    }
    de_atomic_store_u64(&k->budget, (u64)n, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&k->armed, 1, DE_ATOMIC_RELEASE);
}
