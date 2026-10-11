/*
 * de_hashmap.h: open-addressing hash map on an explicit allocator (engine-internal, SPEC §5.6).
 *
 * Include as "core/de_hashmap.h". Not part of the public API.
 *
 * Keys and values are fixed-size byte blobs whose sizes are given at init. The map never hashes
 * anything itself: every operation takes the key's 64-bit hash from the caller (use de_hash64,
 * DENG-153, or any well-mixed hash). Equal keys must have equal hashes. Keys are compared with
 * memcmp unless the desc gives a `key_eq` function, so a struct key compared by memcmp must have
 * its padding bytes zeroed.
 *
 * Layout and probing (the design choice the issue asked to document):
 *   - Capacity is 0 or a power of two (at least 8). One allocation holds, per slot, a control
 *     byte, the 64-bit hash, the key and the value.
 *   - Linear probing with tombstones. The low bits of the hash pick the home slot; the probe
 *     walks forward one slot at a time. The control byte is EMPTY, TOMBSTONE, or FULL with a
 *     7-bit fingerprint from the top bits of the hash, so most non-matching slots are rejected
 *     without touching the stored hash or the key.
 *   - Maximum load 0.75, counting live entries plus tombstones, so a probe always ends at an
 *     EMPTY slot. Linear probing over Robin Hood: removes are O(1) (a tombstone, or EMPTY when
 *     the next slot is EMPTY), inserts reuse the first tombstone on their probe path, and
 *     iteration and get stay simple; with a well-mixed hash and load <= 0.75 the probes stay
 *     short.
 *   - The full hash is stored, so growing never needs the caller's hash function.
 *
 * Memory:
 *   - init and get allocate nothing. A put allocates only when the map has to grow: when an
 *     insert would push live entries + tombstones over 0.75 and live entries are above half
 *     the capacity, the capacity doubles. When most of the load is tombstones (live entries at
 *     most half the capacity) the insert instead rehashes in place at the same capacity, which
 *     drops every tombstone and allocates nothing.
 *   - The capacity never changes on get or remove. It goes up only through put (as above) or
 *     de_hashmap_reserve, and down only through de_hashmap_free. de_hashmap_clear keeps it.
 *
 * Adversarial hashes: if many keys share a hash (or a home slot), the map stays correct and every
 * probe still ends (at most `capacity` steps); operations degrade towards O(n).
 *
 * ITERATION ORDER IS NOT DETERMINISTIC. It depends on hash values, capacity and insert/remove
 * history. Simulation code must never iterate a hash map (SPEC §5.6, §5.7 rule 5). In Debug and
 * Dev builds de_hashmap_iter_begin increments a per-thread counter (de_hashmap_iter_count) so the
 * ECS (S05) can assert that a simulation system did not iterate one.
 *
 * Not thread-safe. Pointers returned by get/get_or_put are valid until the next put, reserve,
 * clear or free on the same map.
 */
#ifndef DE_CORE_HASHMAP_H
#define DE_CORE_HASHMAP_H

#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

/** Key compare: true if the keys at `a` and `b` (each `key_size` bytes) are equal. */
typedef bool (*de_hashmap_key_eq_fn)(const void *a, const void *b, u32 key_size, void *user);

/** Hash map settings. */
typedef struct de_hashmap_desc {
    const de_allocator *allocator; /**< required */
    u32 key_size;                  /**< bytes per key, > 0 */
    u32 value_size;                /**< bytes per value; 0 makes a set */
    u32 key_align;   /**< power of two; 0: largest power of two <= 16 dividing key_size */
    u32 value_align; /**< same rule for value_size */
    de_hashmap_key_eq_fn key_eq; /**< NULL: memcmp */
    void *key_eq_user;           /**< passed to key_eq */
    u32 tag;                     /**< memory tag for the allocator (DE_MEM_*) */
} de_hashmap_desc;

/** An open-addressing hash map. Fields are private; read them through the functions below. */
typedef struct de_hashmap {
    u8 *ctrl;    /* capacity control bytes */
    u64 *hashes; /* capacity full hashes */
    u8 *keys;    /* capacity * key_stride */
    u8 *values;  /* capacity * value_stride */
    size_t block_size;
    u32 capacity; /* 0 or a power of two */
    u32 count;    /* live entries */
    u32 tombstones;
    u32 key_size, value_size;
    u32 key_stride, value_stride;
    u32 key_align, value_align, block_align;
    u32 tag;
    de_hashmap_key_eq_fn key_eq;
    void *key_eq_user;
    const de_allocator *a;
} de_hashmap;

/**
 * Initialises an empty map. Allocates nothing. DE_ERR_INVALID_ARGUMENT if the allocator is NULL,
 * key_size is 0, an alignment is not a power of two, or key_size + value_size exceeds 2^30.
 */
de_result de_hashmap_init(de_hashmap *m, const de_hashmap_desc *desc);

/** Gives the memory back. The map is empty with capacity 0 and can be used again. */
void de_hashmap_free(de_hashmap *m);

/** Removes every entry. Keeps the capacity; allocates nothing. */
void de_hashmap_clear(de_hashmap *m);

/**
 * Grows the capacity so that `n` entries fit without growing again. Never shrinks. Also drops
 * the tombstones when it rehashes. DE_ERR_OUT_OF_MEMORY on failure (the map is unchanged).
 */
de_result de_hashmap_reserve(de_hashmap *m, u32 n);

/**
 * The value stored for `key`, or NULL. For a set (value_size 0) a found key gives a pointer to
 * the stored key instead.
 */
void *de_hashmap_get(const de_hashmap *m, u64 hash, const void *key);

/** true if `key` is in the map. */
bool de_hashmap_contains(const de_hashmap *m, u64 hash, const void *key);

/**
 * Inserts `key` with a copy of `*value` (zeroes when `value` is NULL), or overwrites the value
 * of an existing key. On failure (DE_ERR_OUT_OF_MEMORY) the map is unchanged.
 */
de_result de_hashmap_put(de_hashmap *m, u64 hash, const void *key, const void *value);

/**
 * Finds `key`, inserting it with a zeroed value if it is missing. `*out_value` gets the value
 * pointer (for a set, the stored key); `*out_inserted` (may be NULL) says whether the key was
 * new. On failure the map is unchanged and `*out_value` is NULL.
 */
de_result de_hashmap_get_or_put(de_hashmap *m, u64 hash, const void *key, void **out_value,
                                bool *out_inserted);

/**
 * Removes `key`, copying its value to `out_value` unless NULL. false if it was not there. Never
 * allocates or changes the capacity.
 */
bool de_hashmap_remove(de_hashmap *m, u64 hash, const void *key, void *out_value);

/** Live entries. */
static inline u32 de_hashmap_count(const de_hashmap *m) {
    return m->count;
}

/** Slots allocated (0 or a power of two). */
static inline u32 de_hashmap_capacity(const de_hashmap *m) {
    return m->capacity;
}

/* ---- Iteration: NOT deterministic in order; never in simulation code ------------------- */

/** Iteration state. `key` and `value` point at the current entry after a true iter_next. */
typedef struct de_hashmap_iter {
    const de_hashmap *map;
    u32 next;        /* next slot to look at */
    u32 index;       /* slot of the current entry */
    const void *key; /**< current key */
    void *value;     /**< current value (the key for a set); writable */
} de_hashmap_iter;

/**
 * Starts an iteration over every entry, in an order that is NOT deterministic (see the file
 * comment). Use as:
 *
 *     for (de_hashmap_iter it = de_hashmap_iter_begin(&m); de_hashmap_iter_next(&it);)
 *         use(it.key, it.value);
 *
 * Removing the current entry (or any entry) during the iteration is allowed. A put, reserve or
 * clear during the iteration is not. Counts towards de_hashmap_iter_count in Debug/Dev.
 */
de_hashmap_iter de_hashmap_iter_begin(const de_hashmap *m);

/** Moves to the next entry. false when there are no more. */
bool de_hashmap_iter_next(de_hashmap_iter *it);

/** 1 when de_hashmap_iter_begin counts its calls (Debug and Dev builds), else 0. */
#if defined(DE_DEBUG) || defined(DE_DEV)
#define DE_HASHMAP_ITER_TRACKING 1
#else
#define DE_HASHMAP_ITER_TRACKING 0
#endif

/**
 * How many times this thread has called de_hashmap_iter_begin. Always 0 when
 * DE_HASHMAP_ITER_TRACKING is 0. The ECS reads it before and after a simulation system and
 * asserts that it did not change.
 */
u64 de_hashmap_iter_count(void);

/* ---- Typed macros ---------------------------------------------------------------------- */

/** Initialises `m` for keys of type K and values of type V on allocator `alloc`. */
#define DE_HASHMAP_INIT(m, K, V, alloc)                                                            \
    de_hashmap_init((m), &(const de_hashmap_desc){.allocator = (alloc),                            \
                                                  .key_size = (u32)sizeof(K),                      \
                                                  .value_size = (u32)sizeof(V),                    \
                                                  .key_align = (u32)DE_ALIGNOF(K),                 \
                                                  .value_align = (u32)DE_ALIGNOF(V)})
/** de_hashmap_get as a V pointer. */
#define DE_HASHMAP_GET(m, V, hash, key) ((V *)de_hashmap_get((m), (hash), (key)))

DE_EXTERN_C_END

#endif /* DE_CORE_HASHMAP_H */
