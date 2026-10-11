/*
 * test_counting_alloc.h: a counting de_allocator for the container tests (DENG-151).
 *
 * Wraps malloc. Counts calls and live bytes, checks that every free/realloc passes the size the
 * block was allocated with and an alignment the block satisfies, and can fail on purpose after
 * a number of successful allocations. Test-only; DENG-149's tracking allocator / DENG-155's
 * alloc guard may replace it.
 */
#ifndef DE_TEST_COUNTING_ALLOC_H
#define DE_TEST_COUNTING_ALLOC_H

#include <dengine/de_memory.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_counting_alloc {
    de_allocator iface;
    u64 allocs;       /* successful alloc calls, and reallocs from NULL */
    u64 reallocs;     /* successful realloc calls of an existing block */
    u64 frees;        /* free calls of a non-NULL block */
    u64 live_blocks;  /* blocks not yet freed */
    u64 live_bytes;   /* bytes in those blocks */
    u64 size_errors;  /* free/realloc with a size other than the block's */
    u64 align_errors; /* blocks returned with the wrong alignment */
    i64 fail_after;   /* < 0: never fail; else allow this many more successful allocations */
} test_counting_alloc;

/* Header stored in front of each block: the malloc pointer and the requested size. */
typedef struct test_counting_hdr {
    void *raw;
    size_t size;
} test_counting_hdr;

static inline void *test_counting_raw_alloc(test_counting_alloc *c, size_t size, size_t align) {
    if (align < sizeof(void *))
        align = sizeof(void *);
    u8 *raw = (u8 *)malloc(size + align + sizeof(test_counting_hdr));
    if (raw == NULL)
        return NULL;
    uintptr_t p = (uintptr_t)(raw + sizeof(test_counting_hdr));
    p = (p + align - 1u) & ~(uintptr_t)(align - 1u);
    test_counting_hdr *h = (test_counting_hdr *)(void *)(p - sizeof(test_counting_hdr));
    h->raw = raw;
    h->size = size;
    if ((p & (align - 1u)) != 0)
        c->align_errors++;
    return (void *)p;
}

static inline test_counting_hdr *test_counting_hdr_of(void *p) {
    return (test_counting_hdr *)(void *)((u8 *)p - sizeof(test_counting_hdr));
}

static inline bool test_counting_may_alloc(test_counting_alloc *c) {
    if (c->fail_after < 0)
        return true;
    if (c->fail_after == 0)
        return false;
    c->fail_after--;
    return true;
}

static inline void *test_counting_alloc_fn(void *user, size_t size, size_t align, u32 tag) {
    (void)tag;
    test_counting_alloc *c = (test_counting_alloc *)user;
    if (!test_counting_may_alloc(c)) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "test_counting_alloc: injected failure");
        return NULL;
    }
    void *p = test_counting_raw_alloc(c, size, align);
    if (p == NULL)
        return NULL;
    c->allocs++;
    c->live_blocks++;
    c->live_bytes += size;
    return p;
}

static inline void test_counting_free_fn(void *user, void *ptr, size_t size) {
    test_counting_alloc *c = (test_counting_alloc *)user;
    if (ptr == NULL)
        return;
    test_counting_hdr *h = test_counting_hdr_of(ptr);
    if (h->size != size)
        c->size_errors++;
    c->frees++;
    c->live_blocks--;
    c->live_bytes -= h->size;
    free(h->raw);
}

static inline void *test_counting_realloc_fn(void *user, void *ptr, size_t old_size,
                                             size_t new_size, size_t align, u32 tag) {
    test_counting_alloc *c = (test_counting_alloc *)user;
    if (ptr == NULL)
        return test_counting_alloc_fn(user, new_size, align, tag);
    test_counting_hdr *h = test_counting_hdr_of(ptr);
    if (h->size != old_size)
        c->size_errors++;
    if (((uintptr_t)ptr & (align - 1u)) != 0)
        c->align_errors++;
    if (!test_counting_may_alloc(c)) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "test_counting_alloc: injected failure");
        return NULL;
    }
    void *p = test_counting_raw_alloc(c, new_size, align);
    if (p == NULL)
        return NULL;
    memcpy(p, ptr, old_size < new_size ? old_size : new_size);
    c->live_bytes -= h->size;
    c->live_bytes += new_size;
    free(h->raw);
    c->reallocs++;
    return p;
}

static inline void test_counting_init(test_counting_alloc *c) {
    memset(c, 0, sizeof *c);
    c->fail_after = -1;
    c->iface.alloc = test_counting_alloc_fn;
    c->iface.realloc = test_counting_realloc_fn;
    c->iface.free = test_counting_free_fn;
    c->iface.user = c;
}

/* Allocation calls of any kind so far. */
static inline u64 test_counting_calls(const test_counting_alloc *c) {
    return c->allocs + c->reallocs;
}

#endif /* DE_TEST_COUNTING_ALLOC_H */
