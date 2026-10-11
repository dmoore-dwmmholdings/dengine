/* sort.c: stable merge sort and LSD radix sort (core/de_sort.h). SPEC §5.6, §5.7.
 *
 * Determinism: the output is a function of the input order and the comparison only. No qsort, no
 * pointer comparisons, no randomised pivots. Ties always take the left (earlier) element. */
#include "core/de_sort.h"

#include <string.h>

/* Length of the insertion-sorted runs the merge passes start from. */
#define SORT_RUN 16
/* Alignment of the scratch copy: enough for any element type the engine sorts. */
#define SORT_SCRATCH_ALIGN 16

/* Swaps two non-overlapping elements of `size` bytes through a small stack buffer. */
static void swap_elems(char *a, char *b, size_t size) {
    char tmp[64];
    while (size != 0) {
        const size_t n = size < sizeof tmp ? size : sizeof tmp;
        memcpy(tmp, a, n);
        memcpy(a, b, n);
        memcpy(b, tmp, n);
        a += n;
        b += n;
        size -= n;
    }
}

/* Stable insertion sort of [lo, hi) by adjacent swaps: needs no element-sized temporary. */
static void insertion_sort(char *base, size_t lo, size_t hi, size_t size, de_sort_cmp_fn cmp,
                           void *user) {
    for (size_t i = lo + 1; i < hi; ++i) {
        for (size_t j = i; j > lo; --j) {
            char *cur = base + j * size;
            char *prev = cur - size;
            if (cmp(prev, cur, user) <= 0) /* equal keeps order: stable */
                break;
            swap_elems(prev, cur, size);
        }
    }
}

/* Merges src[lo, mid) and src[mid, hi) into dst[lo, hi). On a tie the left element goes first. */
static void merge(const char *src, char *dst, size_t lo, size_t mid, size_t hi, size_t size,
                  de_sort_cmp_fn cmp, void *user) {
    if (mid >= hi || cmp(src + (mid - 1) * size, src + mid * size, user) <= 0) {
        /* One run only, or the runs are already in order. */
        memcpy(dst + lo * size, src + lo * size, (hi - lo) * size);
        return;
    }
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (cmp(src + j * size, src + i * size, user) < 0)
            memcpy(dst + k++ * size, src + j++ * size, size);
        else
            memcpy(dst + k++ * size, src + i++ * size, size);
    }
    if (i < mid)
        memcpy(dst + k * size, src + i * size, (mid - i) * size);
    else if (j < hi)
        memcpy(dst + k * size, src + j * size, (hi - j) * size);
}

/* The scratch copy of `count` elements of `size` bytes, or NULL with the error context set. */
static void *scratch_alloc(const de_allocator *scratch, size_t count, size_t size,
                           de_result *err) {
    if (scratch == NULL) {
        *err = DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "sorting %zu elements needs a scratch allocator (more than %d)", count,
                       DE_SORT_SMALL);
        return NULL;
    }
    if (size != 0 && count > (size_t)-1 / size) {
        *err = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sort scratch size overflows (%zu x %zu)", count,
                       size);
        return NULL;
    }
    void *p = DE_ALLOC(scratch, count * size, SORT_SCRATCH_ALIGN, DE_MEM_SCRATCH);
    if (p == NULL)
        *err = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "cannot allocate %zu bytes of sort scratch",
                       count * size);
    return p;
}

de_result de_sort_stable(void *base, size_t count, size_t size, de_sort_cmp_fn cmp, void *user,
                         const de_allocator *scratch) {
    if (size == 0 || cmp == NULL || (base == NULL && count != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_sort_stable: bad arguments");
    if (count < 2)
        return DE_OK;
    char *b = (char *)base;
    if (count <= DE_SORT_SMALL) {
        insertion_sort(b, 0, count, size, cmp, user);
        return DE_OK;
    }
    de_result err = DE_OK;
    char *tmp = (char *)scratch_alloc(scratch, count, size, &err);
    if (tmp == NULL)
        return err;

    for (size_t lo = 0; lo < count; lo += SORT_RUN)
        insertion_sort(b, lo, lo + SORT_RUN < count ? lo + SORT_RUN : count, size, cmp, user);

    /* Bottom-up passes, alternating between the array and the scratch copy. */
    char *src = b, *dst = tmp;
    for (size_t width = SORT_RUN; width < count; width *= 2) {
        for (size_t lo = 0; lo < count; lo += 2 * width) {
            const size_t mid = lo + width < count ? lo + width : count;
            const size_t hi = mid + width < count ? mid + width : count;
            merge(src, dst, lo, mid, hi, size, cmp, user);
        }
        char *t = src;
        src = dst;
        dst = t;
    }
    if (src != b)
        memcpy(b, src, count * size);
    DE_FREE(scratch, tmp, count * size);
    return DE_OK;
}

/* ---------- Radix sort ---------- */

/* Shared LSD driver over an item type T with KEY_BYTES 8-bit digits in `.key`. A macro so the
 * inner loops are compiled per item type. */
#define DE__RADIX_SORT(T, KEY_BYTES)                                                               \
    do {                                                                                           \
        if (items == NULL && count != 0)                                                           \
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "radix sort: NULL items");                     \
        if (count < 2)                                                                             \
            return DE_OK;                                                                          \
        if (count <= DE_SORT_SMALL) {                                                              \
            for (size_t i = 1; i < count; ++i) {                                                   \
                const T cur = items[i];                                                            \
                size_t j = i;                                                                      \
                for (; j > 0 && items[j - 1].key > cur.key; --j)                                   \
                    items[j] = items[j - 1];                                                       \
                items[j] = cur;                                                                    \
            }                                                                                      \
            return DE_OK;                                                                          \
        }                                                                                          \
        de_result err = DE_OK;                                                                     \
        T *tmp = (T *)scratch_alloc(scratch, count, sizeof(T), &err);                              \
        if (tmp == NULL)                                                                           \
            return err;                                                                            \
        size_t hist[KEY_BYTES][256];                                                               \
        memset(hist, 0, sizeof hist);                                                              \
        for (size_t i = 0; i < count; ++i)                                                         \
            for (unsigned d = 0; d < (KEY_BYTES); ++d)                                             \
                ++hist[d][(items[i].key >> (8u * d)) & 0xFFu];                                     \
        T *src = items, *dst = tmp;                                                                \
        for (unsigned d = 0; d < (KEY_BYTES); ++d) {                                               \
            size_t *h = hist[d];                                                                   \
            /* Every key has the same digit here: the pass would not move anything. */            \
            if (h[(src[0].key >> (8u * d)) & 0xFFu] == count)                                      \
                continue;                                                                          \
            size_t sum = 0;                                                                        \
            for (unsigned v = 0; v < 256; ++v) {                                                   \
                const size_t c = h[v];                                                             \
                h[v] = sum;                                                                        \
                sum += c;                                                                          \
            }                                                                                      \
            for (size_t i = 0; i < count; ++i)                                                     \
                dst[h[(src[i].key >> (8u * d)) & 0xFFu]++] = src[i];                               \
            T *t = src;                                                                            \
            src = dst;                                                                             \
            dst = t;                                                                               \
        }                                                                                          \
        if (src != items)                                                                          \
            memcpy(items, src, count * sizeof(T));                                                 \
        DE_FREE(scratch, tmp, count * sizeof(T));                                                  \
        return DE_OK;                                                                              \
    } while (0)

de_result de_radix_sort_u32(de_sort_key32 *items, size_t count, const de_allocator *scratch) {
    DE__RADIX_SORT(de_sort_key32, 4);
}

de_result de_radix_sort_u64(de_sort_key64 *items, size_t count, const de_allocator *scratch) {
    DE__RADIX_SORT(de_sort_key64, 8);
}
