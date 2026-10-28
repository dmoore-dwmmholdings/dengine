#include "render/view_hidden.h"
#include "platform/de_atomic.h"

#include <stdlib.h>
#include <string.h>

struct de__view_hidden {
    de_atomic_u32 refs;
    de_allocator alloc;
    u64 bytes, hash;
    u32 count;
};
static int compare_id(const void *a, const void *b) {
    const u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return (x > y) - (x < y);
}
const u64 *de__view_hidden_ids(const de__view_hidden *p) {
    return p ? (const u64 *)(p + 1) : NULL;
}
u32 de__view_hidden_count(const de__view_hidden *p) {
    return p ? p->count : 0;
}
u64 de__view_hidden_hash(const de__view_hidden *p) {
    return p ? p->hash : 0;
}
bool de__view_hidden_equal(const de__view_hidden *a, const de__view_hidden *b) {
    if (a == b)
        return true;
    if (de__view_hidden_count(a) != de__view_hidden_count(b) ||
        de__view_hidden_hash(a) != de__view_hidden_hash(b))
        return false;
    return !a || memcmp(de__view_hidden_ids(a), de__view_hidden_ids(b),
                        (size_t)a->count * sizeof(u64)) == 0;
}
de_result de__view_hidden_create(const de_allocator *alloc, const u64 *ids, u32 count,
                                 const de__view_hidden **out) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "hidden entity output");
    *out = NULL;
    if (!alloc || (count && !ids))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "hidden entity list");
    if (!count)
        return DE_OK;
    const u64 bytes = sizeof(de__view_hidden) + (u64)count * sizeof(u64);
    de__view_hidden *p = DE_ALLOC(alloc, bytes, 8, DE_MEM_RENDER);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "hidden entity list");
    memset(p, 0, sizeof *p);
    p->alloc = *alloc;
    p->bytes = bytes;
    de_atomic_store_u32(&p->refs, 1, DE_ATOMIC_RELAXED);
    u64 *sorted = (u64 *)(p + 1);
    memcpy(sorted, ids, (size_t)count * sizeof *sorted);
    qsort(sorted, count, sizeof *sorted, compare_id);
    for (u32 i = 0; i < count; ++i)
        if (sorted[i] && (!p->count || sorted[i] != sorted[p->count - 1]))
            sorted[p->count++] = sorted[i];
    if (!p->count) {
        de__view_hidden_release(p);
        return DE_OK;
    }
    p->hash = UINT64_C(14695981039346656037);
    for (u32 i = 0; i < p->count; ++i) {
        p->hash ^= sorted[i];
        p->hash *= UINT64_C(1099511628211);
    }
    *out = p;
    return DE_OK;
}
const de__view_hidden *de__view_hidden_retain(const de__view_hidden *p) {
    if (p)
        (void)de_atomic_fetch_add_u32(&((de__view_hidden *)p)->refs, 1, DE_ATOMIC_RELAXED);
    return p;
}
void de__view_hidden_release(const de__view_hidden *p) {
    if (p && de_atomic_fetch_sub_u32(&((de__view_hidden *)p)->refs, 1, DE_ATOMIC_ACQ_REL) == 1)
        DE_FREE(&p->alloc, (void *)p, p->bytes);
}
bool de__view_hidden_contains(const de__view_hidden *p, u64 entity) {
    const u64 *ids = de__view_hidden_ids(p);
    u32 lo = 0, hi = de__view_hidden_count(p);
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        if (ids[mid] < entity)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < de__view_hidden_count(p) && ids[lo] == entity;
}
