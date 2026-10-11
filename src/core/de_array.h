/*
 * de_array.h: growable array on an explicit allocator (engine-internal, SPEC §5.6).
 *
 * Include as "core/de_array.h". Not part of the public API.
 *
 * A de_array stores `count` elements of `elem_size` bytes contiguously in `data`. It allocates
 * only when it grows (push, insert, reserve, resize past the capacity), through the allocator
 * given at init, and gives everything back on de_array_free. Growth is ×1.5 through the
 * allocator's realloc, so `data` may move whenever the array grows: do not keep element
 * pointers across a call that can grow it.
 *
 * The functions work on `void *` and an element size. The DE_ARRAY_* macros add the element
 * type, and in Debug/Dev builds check that sizeof(T) matches the array's element size.
 *
 *     DE_ARRAY(vec3) points;                       // documents the element type; a de_array
 *     DE_ARRAY_INIT(&points, vec3, alloc);
 *     DE_TRY(DE_ARRAY_PUSH(&points, vec3, (vec3){1, 2, 3}));
 *     DE_ARRAY_AT(&points, vec3, 0).x = 5;         // bounds-checked in Debug/Dev
 *     de_array_free(&points);
 *
 * A zero-initialised de_array (`(de_array){0}`) is empty and may be freed, but it has no
 * allocator or element size: call an init function before adding elements.
 *
 * Not thread-safe. The allocator must outlive the array.
 */
#ifndef DE_CORE_ARRAY_H
#define DE_CORE_ARRAY_H

#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

/** A growable array. Fields are readable; change them only through the functions below. */
typedef struct de_array {
    void *data;            /**< `capacity` elements; NULL while capacity is 0 */
    u32 count;             /**< elements in use */
    u32 capacity;          /**< elements allocated */
    u32 elem_size;         /**< bytes per element (> 0) */
    u32 elem_align;        /**< alignment of `data` (a power of two) */
    u32 tag;               /**< memory tag passed to the allocator (DE_MEM_*) */
    const de_allocator *a; /**< where `data` comes from */
} de_array;

/** Marks a de_array that holds elements of type T. Expands to `de_array`. */
#define DE_ARRAY(T) de_array

/**
 * Initialises an empty array. Allocates nothing. `elem_size` must be > 0 and `elem_align` a
 * power of two (0 means the largest power of two, up to 16, that divides `elem_size`).
 */
void de_array_init(de_array *arr, const de_allocator *a, u32 elem_size, u32 elem_align, u32 tag);

/**
 * Makes room for at least `min_capacity` elements. Never shrinks. Fails with
 * DE_ERR_OUT_OF_MEMORY (the array is unchanged) if the allocator fails or the size overflows.
 */
de_result de_array_reserve(de_array *arr, u32 min_capacity);

/**
 * Sets `count` to `new_count`. New elements are zeroed. Grows like de_array_reserve; on
 * failure the array is unchanged.
 */
de_result de_array_resize(de_array *arr, u32 new_count);

/** Appends a copy of `*elem` (zeroes when `elem` is NULL). On failure the array is unchanged. */
de_result de_array_push(de_array *arr, const void *elem);

/** Appends `n` elements copied from `elems` (zeroes when `elems` is NULL). All or nothing. */
de_result de_array_push_n(de_array *arr, const void *elems, u32 n);

/**
 * Inserts a copy of `*elem` (zeroes when NULL) at `index` (<= count), moving the elements from
 * `index` on up by one. On failure the array is unchanged.
 */
de_result de_array_insert(de_array *arr, u32 index, const void *elem);

/** Removes the last element, copying it to `out` unless NULL. false if the array is empty. */
bool de_array_pop(de_array *arr, void *out);

/** Removes element `index` (< count) by moving the last element into its place. O(1). */
void de_array_remove_swap(de_array *arr, u32 index);

/** Removes element `index` (< count), moving the elements after it down. Keeps the order. */
void de_array_remove_ordered(de_array *arr, u32 index);

/** Sets count to 0. Keeps the memory. */
void de_array_clear(de_array *arr);

/**
 * Gives the memory back to the allocator and leaves the array empty with capacity 0. The
 * allocator and element size stay, so the array can be used again.
 */
void de_array_free(de_array *arr);

/* ---- Inline helpers -------------------------------------------------------------------- */

/** Pointer to element `index` (< count, checked in Debug/Dev). */
static inline void *de_array_at(const de_array *arr, u32 index) {
    DE_ASSERT(index < arr->count);
    return (u8 *)arr->data + (size_t)index * arr->elem_size;
}

/* Checks a typed macro's element size and returns `arr`, so the macros evaluate it once. */
static inline de_array *de__array_typed(de_array *arr, size_t elem_size) {
    DE_ASSERT(arr->elem_size == elem_size);
    (void)elem_size;
    return arr;
}

/* ---- Typed macros ---------------------------------------------------------------------- */

/** Initialises `arr` for elements of type T, tag DE_MEM_GENERAL. */
#define DE_ARRAY_INIT(arr, T, alloc)                                                               \
    de_array_init((arr), (alloc), (u32)sizeof(T), (u32)DE_ALIGNOF(T), DE_MEM_GENERAL)
/** Initialises `arr` for elements of type T with a memory tag. */
#define DE_ARRAY_INIT_TAG(arr, T, alloc, tag)                                                      \
    de_array_init((arr), (alloc), (u32)sizeof(T), (u32)DE_ALIGNOF(T), (tag))
/** Element `i` as an lvalue of type T. Bounds-checked in Debug/Dev. */
#define DE_ARRAY_AT(arr, T, i) (((T *)de_array_at(de__array_typed((arr), sizeof(T)), (i)))[0])
/** The elements as a T pointer (NULL while capacity is 0). */
#define DE_ARRAY_DATA(arr, T) ((T *)de__array_typed((arr), sizeof(T))->data)
/** The last element as an lvalue of type T. The array must not be empty. */
#define DE_ARRAY_LAST(arr, T) DE_ARRAY_AT((arr), T, (arr)->count - 1u)
/**
 * Appends a value of type T. The value may contain commas: DE_ARRAY_PUSH(&a, v2, (v2){1, 2}).
 * Returns de_result.
 */
#define DE_ARRAY_PUSH(arr, T, ...)                                                                 \
    de_array_push(de__array_typed((arr), sizeof(T)), (T[1]){__VA_ARGS__})
/** Inserts a value of type T at index `i`. Returns de_result. */
#define DE_ARRAY_INSERT(arr, T, i, ...)                                                            \
    de_array_insert(de__array_typed((arr), sizeof(T)), (i), (T[1]){__VA_ARGS__})
/** Pops the last element into `*out` (a T pointer, or NULL). Returns bool. */
#define DE_ARRAY_POP(arr, T, out)                                                                  \
    de_array_pop(de__array_typed((arr), sizeof(T)), 1 ? (out) : (T *)0)

DE_EXTERN_C_END

#endif /* DE_CORE_ARRAY_H */
