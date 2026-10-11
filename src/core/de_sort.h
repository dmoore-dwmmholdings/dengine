/*
 * de_sort.h: deterministic sorting (internal, not installed). SPEC §5.6, §5.7.
 *
 * Include as "core/de_sort.h". Both sorts are stable: elements that compare equal keep their
 * input order. The result therefore depends only on the input order and the comparison, never on
 * addresses, threads or the C library (qsort is neither stable nor the same everywhere), so the
 * deterministic modules (ECS sim schedules, physics, netcode) sort with these.
 *
 * Scratch memory: arrays longer than DE_SORT_SMALL need one temporary copy (count * size bytes),
 * taken from `scratch` and given back before return. A frame arena or thread scratch allocator is
 * a good fit. Shorter arrays are insertion-sorted in place and never touch `scratch`, which may
 * then be NULL.
 */
#ifndef DE_CORE_SORT_H
#define DE_CORE_SORT_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

/** Arrays of at most this many elements are sorted without scratch memory. */
#define DE_SORT_SMALL 32

/**
 * Three-way comparison: negative if `a` sorts before `b`, 0 if they are equal, positive if after.
 * `user` is the pointer given to de_sort_stable. Must be a consistent total preorder.
 */
typedef int (*de_sort_cmp_fn)(const void *a, const void *b, void *user);

/**
 * Stable sort of `count` elements of `size` bytes at `base`: insertion-sorted runs of 16, then
 * bottom-up merge passes. O(n log n) comparisons, O(n) scratch.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL `base` with count > 0, `size` 0, NULL `cmp`, or a
 *         NULL `scratch` when count > DE_SORT_SMALL); DE_ERR_OUT_OF_MEMORY if `scratch` fails.
 *         On failure the array is unchanged and the error context is set.
 */
de_result de_sort_stable(void *base, size_t count, size_t size, de_sort_cmp_fn cmp, void *user,
                         const de_allocator *scratch);

/** Key and payload for de_radix_sort_u32. `index` is carried along (usually an array index). */
typedef struct de_sort_key32 {
    u32 key;
    u32 index;
} de_sort_key32;

/** Key and payload for de_radix_sort_u64. */
typedef struct de_sort_key64 {
    u64 key;
    u64 index;
} de_sort_key64;

/**
 * Stable LSD radix sort by ascending `key`: 8-bit digits, one counting pass per digit, and passes
 * whose digit is the same for every key are skipped. Same result as de_sort_stable comparing
 * `key` only. Errors as de_sort_stable.
 */
de_result de_radix_sort_u32(de_sort_key32 *items, size_t count, const de_allocator *scratch);

/** de_radix_sort_u32 for 64-bit keys (up to 8 passes). */
de_result de_radix_sort_u64(de_sort_key64 *items, size_t count, const de_allocator *scratch);

#endif /* DE_CORE_SORT_H */
