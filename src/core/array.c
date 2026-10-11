/* array.c: growable array on an explicit allocator (DENG-151). See core/de_array.h. */
#include "core/de_array.h"

#include <stdint.h>
#include <string.h>

/* Smallest capacity a growing array allocates, so small arrays do not realloc on every push. */
#define DE__ARRAY_MIN_CAPACITY 4u

static u32 de__array_default_align(u32 elem_size) {
    u32 align = 1;
    while (align < 16u && (elem_size & ((align << 1) - 1u)) == 0u)
        align <<= 1;
    return align;
}

void de_array_init(de_array *arr, const de_allocator *a, u32 elem_size, u32 elem_align, u32 tag) {
    DE_ASSERT(arr != NULL);
    DE_ASSERT(a != NULL);
    DE_ASSERT(elem_size > 0);
    if (elem_align == 0)
        elem_align = de__array_default_align(elem_size);
    DE_ASSERT((elem_align & (elem_align - 1u)) == 0u);
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
    arr->elem_size = elem_size;
    arr->elem_align = elem_align;
    arr->tag = tag;
    arr->a = a;
}

/* Reallocates to exactly `new_capacity` elements (> capacity). */
static de_result de__array_realloc(de_array *arr, u32 new_capacity) {
    DE_ASSERT(arr->a != NULL && arr->elem_size > 0);
    if ((u64)new_capacity * arr->elem_size > (u64)SIZE_MAX)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_array: %u elements of %u bytes overflow size_t",
                       new_capacity, arr->elem_size);
    const size_t old_size = (size_t)arr->capacity * arr->elem_size;
    const size_t new_size = (size_t)new_capacity * arr->elem_size;
    void *p = DE_REALLOC(arr->a, arr->data, old_size, new_size, arr->elem_align, arr->tag);
    if (p == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_array: growing to %u elements of %u bytes",
                            new_capacity, arr->elem_size);
    arr->data = p;
    arr->capacity = new_capacity;
    return DE_OK;
}

de_result de_array_reserve(de_array *arr, u32 min_capacity) {
    if (min_capacity <= arr->capacity)
        return DE_OK;
    return de__array_realloc(arr, min_capacity);
}

/* Makes room for `extra` more elements, growing ×1.5 (at least to what is needed). */
static de_result de__array_grow_for(de_array *arr, u32 extra) {
    if (extra > UINT32_MAX - arr->count)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_array: count overflows u32 (%u + %u)", arr->count,
                       extra);
    const u32 needed = arr->count + extra;
    if (needed <= arr->capacity)
        return DE_OK;
    u64 cap = (u64)arr->capacity + arr->capacity / 2u;
    if (cap < DE__ARRAY_MIN_CAPACITY)
        cap = DE__ARRAY_MIN_CAPACITY;
    if (cap < needed)
        cap = needed;
    if (cap > UINT32_MAX)
        cap = UINT32_MAX;
    return de__array_realloc(arr, (u32)cap);
}

de_result de_array_resize(de_array *arr, u32 new_count) {
    if (new_count > arr->count) {
        DE_TRY(de__array_grow_for(arr, new_count - arr->count));
        memset((u8 *)arr->data + (size_t)arr->count * arr->elem_size, 0,
               (size_t)(new_count - arr->count) * arr->elem_size);
    }
    arr->count = new_count;
    return DE_OK;
}

de_result de_array_push(de_array *arr, const void *elem) {
    return de_array_push_n(arr, elem, 1);
}

de_result de_array_push_n(de_array *arr, const void *elems, u32 n) {
    if (n == 0)
        return DE_OK;
    DE_TRY(de__array_grow_for(arr, n));
    u8 *dst = (u8 *)arr->data + (size_t)arr->count * arr->elem_size;
    const size_t bytes = (size_t)n * arr->elem_size;
    if (elems != NULL)
        memcpy(dst, elems, bytes);
    else
        memset(dst, 0, bytes);
    arr->count += n;
    return DE_OK;
}

de_result de_array_insert(de_array *arr, u32 index, const void *elem) {
    DE_ASSERT(index <= arr->count);
    DE_TRY(de__array_grow_for(arr, 1));
    const size_t es = arr->elem_size;
    u8 *at = (u8 *)arr->data + (size_t)index * es;
    memmove(at + es, at, (size_t)(arr->count - index) * es);
    if (elem != NULL)
        memcpy(at, elem, es);
    else
        memset(at, 0, es);
    arr->count += 1;
    return DE_OK;
}

bool de_array_pop(de_array *arr, void *out) {
    if (arr->count == 0)
        return false;
    arr->count -= 1;
    if (out != NULL)
        memcpy(out, (u8 *)arr->data + (size_t)arr->count * arr->elem_size, arr->elem_size);
    return true;
}

void de_array_remove_swap(de_array *arr, u32 index) {
    DE_ASSERT(index < arr->count);
    const u32 last = arr->count - 1u;
    if (index != last) {
        const size_t es = arr->elem_size;
        memcpy((u8 *)arr->data + (size_t)index * es, (u8 *)arr->data + (size_t)last * es, es);
    }
    arr->count = last;
}

void de_array_remove_ordered(de_array *arr, u32 index) {
    DE_ASSERT(index < arr->count);
    const size_t es = arr->elem_size;
    u8 *at = (u8 *)arr->data + (size_t)index * es;
    memmove(at, at + es, (size_t)(arr->count - index - 1u) * es);
    arr->count -= 1;
}

void de_array_clear(de_array *arr) {
    arr->count = 0;
}

void de_array_free(de_array *arr) {
    if (arr->data != NULL)
        DE_FREE(arr->a, arr->data, (size_t)arr->capacity * arr->elem_size);
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
}
