/* hashmap.c: open-addressing hash map, linear probing with tombstones (DENG-151).
 * See core/de_hashmap.h for the design. */
#include "core/de_hashmap.h"
#include "core/de_compiler.h"

#include <stdint.h>
#include <string.h>

/* Control byte of a slot. FULL slots hold 0x80 | the top 7 bits of the hash. */
enum {
    DE__HM_EMPTY = 0x00,
    DE__HM_TOMBSTONE = 0x01,
    DE__HM_PENDING = 0x02, /* only during an in-place rehash: full, not yet placed */
    DE__HM_FULL_BIT = 0x80,
};

#define DE__HM_NONE         UINT32_MAX
#define DE__HM_MIN_CAPACITY 8u
#define DE__HM_MAX_CAPACITY (1u << 31)
#define DE__HM_MAX_ELEM     (1u << 30)

#if DE_HASHMAP_ITER_TRACKING
static DE_THREAD_LOCAL u64 de__hashmap_iter_counter;
#endif

static inline u8 de__hm_fingerprint(u64 hash) {
    return (u8)(DE__HM_FULL_BIT | (u8)(hash >> 57));
}

static inline bool de__hm_is_full(u8 c) {
    return (c & DE__HM_FULL_BIT) != 0;
}

/* Entries allowed (live + tombstones) at a capacity: 0.75 of it. */
static inline u32 de__hm_max_load(u32 capacity) {
    return capacity - capacity / 4u;
}

static inline u8 *de__hm_key(const de_hashmap *m, u32 i) {
    return m->keys + (size_t)i * m->key_stride;
}

static inline void *de__hm_value(const de_hashmap *m, u32 i) {
    if (m->value_size == 0)
        return de__hm_key(m, i);
    return m->values + (size_t)i * m->value_stride;
}

static inline bool de__hm_key_eq(const de_hashmap *m, const void *a, const void *b) {
    if (m->key_eq != NULL)
        return m->key_eq(a, b, m->key_size, m->key_eq_user);
    return memcmp(a, b, m->key_size) == 0;
}

static u32 de__hm_default_align(u32 size) {
    u32 align = 1;
    while (align < 16u && (size & ((align << 1) - 1u)) == 0u)
        align <<= 1;
    return align;
}

static inline size_t de__hm_align_up(size_t v, size_t align) {
    return (v + align - 1u) & ~(align - 1u);
}

de_result de_hashmap_init(de_hashmap *m, const de_hashmap_desc *desc) {
    DE_ASSERT(m != NULL && desc != NULL);
    memset(m, 0, sizeof *m);
    if (desc->key_size == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_hashmap: key_size is 0");
    if (desc->key_size > DE__HM_MAX_ELEM || desc->value_size > DE__HM_MAX_ELEM - desc->key_size)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_hashmap: key_size + value_size > 2^30");
    const u32 ka = desc->key_align ? desc->key_align : de__hm_default_align(desc->key_size);
    const u32 va = desc->value_align
                       ? desc->value_align
                       : (desc->value_size ? de__hm_default_align(desc->value_size) : 1u);
    if ((ka & (ka - 1u)) != 0 || (va & (va - 1u)) != 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_hashmap: alignment %u/%u not a power of two",
                       ka, va);
    m->key_size = desc->key_size;
    m->value_size = desc->value_size;
    m->key_align = ka;
    m->value_align = va;
    m->key_stride = (u32)de__hm_align_up(desc->key_size, ka);
    m->value_stride = (u32)de__hm_align_up(desc->value_size, va);
    m->block_align = 8u;
    if (ka > m->block_align)
        m->block_align = ka;
    if (va > m->block_align)
        m->block_align = va;
    m->tag = desc->tag;
    m->key_eq = desc->key_eq;
    m->key_eq_user = desc->key_eq_user;
    m->a = desc->allocator != NULL ? desc->allocator : de_heap_allocator();
    return DE_OK;
}

/* The arrays of a block of `capacity` slots, as offsets. false if the size overflows size_t. */
typedef struct de__hm_layout {
    size_t hashes, keys, values, total;
} de__hm_layout;

static bool de__hm_layout_for(const de_hashmap *m, u32 capacity, de__hm_layout *out) {
    /* capacity <= 2^31 and key/value strides <= 2^30 + 15 keep every term below 2^62. */
    u64 off = de__hm_align_up(capacity, 8u);
    out->hashes = (size_t)off;
    off += (u64)capacity * sizeof(u64);
    off = de__hm_align_up((size_t)off, m->key_align);
    out->keys = (size_t)off;
    off += (u64)capacity * m->key_stride;
    off = de__hm_align_up((size_t)off, m->value_align);
    out->values = (size_t)off;
    off += (u64)capacity * m->value_stride;
#if SIZE_MAX < UINT64_MAX
    if (off > SIZE_MAX)
        return false;
#endif
    out->total = (size_t)off;
    return true;
}

/* First slot on the probe path of `hash` that is EMPTY. The map must have one. */
static u32 de__hm_find_empty(const de_hashmap *m, u64 hash) {
    const u32 mask = m->capacity - 1u;
    u32 i = (u32)hash & mask;
    while (m->ctrl[i] != DE__HM_EMPTY)
        i = (i + 1u) & mask;
    return i;
}

/* Moves every entry into a new block of `new_capacity` slots. The map is unchanged on failure. */
static de_result de__hm_rehash_to(de_hashmap *m, u32 new_capacity) {
    de__hm_layout l;
    if (!de__hm_layout_for(m, new_capacity, &l))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_hashmap: %u slots overflow size_t", new_capacity);
    u8 *block = (u8 *)DE_ALLOC(m->a, l.total, m->block_align, m->tag);
    if (block == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_hashmap: growing to %u slots (%zu bytes)",
                            new_capacity, l.total);
    de_hashmap old = *m;
    memset(block, DE__HM_EMPTY, new_capacity);
    m->ctrl = block;
    m->hashes = (u64 *)(void *)(block + l.hashes);
    m->keys = block + l.keys;
    m->values = block + l.values;
    m->block_size = l.total;
    m->capacity = new_capacity;
    m->tombstones = 0;
    for (u32 i = 0; i < old.capacity; ++i) {
        if (!de__hm_is_full(old.ctrl[i]))
            continue;
        const u64 h = old.hashes[i];
        const u32 j = de__hm_find_empty(m, h);
        m->ctrl[j] = old.ctrl[i];
        m->hashes[j] = h;
        memcpy(de__hm_key(m, j), de__hm_key(&old, i), m->key_size);
        if (m->value_size)
            memcpy(de__hm_value(m, j), de__hm_value(&old, i), m->value_size);
    }
    if (old.ctrl != NULL)
        DE_FREE(m->a, old.ctrl, old.block_size);
    return DE_OK;
}

static void de__hm_swap_bytes(u8 *a, u8 *b, size_t n) {
    for (size_t k = 0; k < n; ++k) {
        const u8 t = a[k];
        a[k] = b[k];
        b[k] = t;
    }
}

/*
 * Rehashes in place at the same capacity, dropping every tombstone. Allocates nothing.
 *
 * Every live entry is marked PENDING and every tombstone EMPTY. Each PENDING entry then goes to
 * the first slot on its probe path that is not FULL: its own slot (it stays), an EMPTY slot (it
 * moves) or another PENDING slot (the two swap and the displaced entry is placed next). A slot
 * only ever turns FULL, never back, so every slot between an entry's home and its final slot
 * is FULL and a later probe finds it. This is the scheme of Abseil's
 * DropDeletesWithoutResize, with a group size of one.
 */
static void de__hm_drop_tombstones(de_hashmap *m) {
    const u32 cap = m->capacity;
    const u32 mask = cap - 1u;
    for (u32 i = 0; i < cap; ++i)
        m->ctrl[i] = de__hm_is_full(m->ctrl[i]) ? (u8)DE__HM_PENDING : (u8)DE__HM_EMPTY;
    for (u32 i = 0; i < cap; ++i) {
        while (m->ctrl[i] == DE__HM_PENDING) {
            const u64 h = m->hashes[i];
            u32 j = (u32)h & mask;
            while (de__hm_is_full(m->ctrl[j]))
                j = (j + 1u) & mask;
            if (j == i) {
                m->ctrl[i] = de__hm_fingerprint(h);
            } else if (m->ctrl[j] == DE__HM_EMPTY) {
                m->ctrl[j] = de__hm_fingerprint(h);
                m->hashes[j] = h;
                memcpy(de__hm_key(m, j), de__hm_key(m, i), m->key_size);
                if (m->value_size)
                    memcpy(de__hm_value(m, j), de__hm_value(m, i), m->value_size);
                m->ctrl[i] = DE__HM_EMPTY;
            } else {
                /* j is PENDING: swap, place this entry at j, and process the one now at i. */
                m->hashes[i] = m->hashes[j];
                m->hashes[j] = h;
                de__hm_swap_bytes(de__hm_key(m, i), de__hm_key(m, j), m->key_size);
                if (m->value_size)
                    de__hm_swap_bytes((u8 *)de__hm_value(m, i), (u8 *)de__hm_value(m, j),
                                      m->value_size);
                m->ctrl[j] = de__hm_fingerprint(h);
            }
        }
    }
    m->tombstones = 0;
}

/* Smallest capacity (power of two, >= DE__HM_MIN_CAPACITY) whose max load holds `n`. */
static bool de__hm_capacity_for(u32 n, u32 *out) {
    u32 cap = DE__HM_MIN_CAPACITY;
    while (de__hm_max_load(cap) < n) {
        if (cap >= DE__HM_MAX_CAPACITY)
            return false;
        cap <<= 1;
    }
    *out = cap;
    return true;
}

/* Probe result: the slot holding the key, or where to insert it. */
typedef struct de__hm_probe {
    u32 found;      /* slot of the key, or DE__HM_NONE */
    u32 first_tomb; /* first tombstone on the path, or DE__HM_NONE */
    u32 empty;      /* EMPTY slot that ended the probe, or DE__HM_NONE */
} de__hm_probe;

static de__hm_probe de__hm_lookup(const de_hashmap *m, u64 hash, const void *key) {
    de__hm_probe p = {DE__HM_NONE, DE__HM_NONE, DE__HM_NONE};
    if (m->capacity == 0)
        return p;
    const u32 mask = m->capacity - 1u;
    const u8 fp = de__hm_fingerprint(hash);
    u32 i = (u32)hash & mask;
    /* At most `capacity` steps, even if the load invariant were broken: no endless loop. */
    for (u32 step = 0; step < m->capacity; ++step) {
        const u8 c = m->ctrl[i];
        if (c == DE__HM_EMPTY) {
            p.empty = i;
            return p;
        }
        if (c == DE__HM_TOMBSTONE) {
            if (p.first_tomb == DE__HM_NONE)
                p.first_tomb = i;
        } else if (c == fp && m->hashes[i] == hash && de__hm_key_eq(m, key, de__hm_key(m, i))) {
            p.found = i;
            return p;
        }
        i = (i + 1u) & mask;
    }
    return p;
}

void de_hashmap_free(de_hashmap *m) {
    if (m->ctrl != NULL)
        DE_FREE(m->a, m->ctrl, m->block_size);
    m->ctrl = NULL;
    m->hashes = NULL;
    m->keys = NULL;
    m->values = NULL;
    m->block_size = 0;
    m->capacity = 0;
    m->count = 0;
    m->tombstones = 0;
}

void de_hashmap_clear(de_hashmap *m) {
    if (m->capacity != 0)
        memset(m->ctrl, DE__HM_EMPTY, m->capacity);
    m->count = 0;
    m->tombstones = 0;
}

de_result de_hashmap_reserve(de_hashmap *m, u32 n) {
    if (n == 0)
        return DE_OK;
    u32 cap;
    if (!de__hm_capacity_for(n, &cap))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_hashmap: %u entries exceed the maximum capacity",
                       n);
    if (cap <= m->capacity)
        return DE_OK;
    return de__hm_rehash_to(m, cap);
}

void *de_hashmap_get(const de_hashmap *m, u64 hash, const void *key) {
    const de__hm_probe p = de__hm_lookup(m, hash, key);
    return p.found == DE__HM_NONE ? NULL : de__hm_value(m, p.found);
}

bool de_hashmap_contains(const de_hashmap *m, u64 hash, const void *key) {
    return de__hm_lookup(m, hash, key).found != DE__HM_NONE;
}

de_result de_hashmap_get_or_put(de_hashmap *m, u64 hash, const void *key, void **out_value,
                                bool *out_inserted) {
    DE_ASSERT(out_value != NULL);
    *out_value = NULL;
    const de__hm_probe p = de__hm_lookup(m, hash, key);
    if (p.found != DE__HM_NONE) {
        *out_value = de__hm_value(m, p.found);
        if (out_inserted)
            *out_inserted = false;
        return DE_OK;
    }
    u32 slot;
    if (p.first_tomb != DE__HM_NONE) {
        slot = p.first_tomb;
        m->tombstones -= 1;
    } else if (m->capacity != 0 && m->count + m->tombstones + 1u <= de__hm_max_load(m->capacity)) {
        DE_ASSERT(p.empty != DE__HM_NONE);
        slot = p.empty;
    } else {
        /* Over the load limit. Mostly tombstones: clean up in place. Otherwise grow. */
        if (m->capacity != 0 && m->count + 1u <= m->capacity / 2u) {
            de__hm_drop_tombstones(m);
        } else {
            u32 cap;
            if (m->count == UINT32_MAX || !de__hm_capacity_for(m->count + 1u, &cap))
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_hashmap: full at %u entries", m->count);
            if (m->capacity < DE__HM_MAX_CAPACITY && cap < m->capacity * 2u)
                cap = m->capacity * 2u;
            DE_TRY(de__hm_rehash_to(m, cap));
        }
        slot = de__hm_find_empty(m, hash);
    }
    m->ctrl[slot] = de__hm_fingerprint(hash);
    m->hashes[slot] = hash;
    memcpy(de__hm_key(m, slot), key, m->key_size);
    if (m->value_size)
        memset(de__hm_value(m, slot), 0, m->value_size);
    m->count += 1;
    *out_value = de__hm_value(m, slot);
    if (out_inserted)
        *out_inserted = true;
    return DE_OK;
}

de_result de_hashmap_put(de_hashmap *m, u64 hash, const void *key, const void *value) {
    void *v;
    DE_TRY(de_hashmap_get_or_put(m, hash, key, &v, NULL));
    if (m->value_size) {
        if (value != NULL)
            memcpy(v, value, m->value_size);
        else
            memset(v, 0, m->value_size);
    }
    return DE_OK;
}

bool de_hashmap_remove(de_hashmap *m, u64 hash, const void *key, void *out_value) {
    const de__hm_probe p = de__hm_lookup(m, hash, key);
    if (p.found == DE__HM_NONE)
        return false;
    const u32 i = p.found;
    if (out_value != NULL && m->value_size)
        memcpy(out_value, de__hm_value(m, i), m->value_size);
    /* A probe that reaches i stops at the next slot if that one is EMPTY, so i can be EMPTY too. */
    if (m->ctrl[(i + 1u) & (m->capacity - 1u)] == DE__HM_EMPTY) {
        m->ctrl[i] = DE__HM_EMPTY;
    } else {
        m->ctrl[i] = DE__HM_TOMBSTONE;
        m->tombstones += 1;
    }
    m->count -= 1;
    return true;
}

de_hashmap_iter de_hashmap_iter_begin(const de_hashmap *m) {
#if DE_HASHMAP_ITER_TRACKING
    de__hashmap_iter_counter += 1;
#endif
    de_hashmap_iter it;
    it.map = m;
    it.next = 0;
    it.index = DE__HM_NONE;
    it.key = NULL;
    it.value = NULL;
    return it;
}

bool de_hashmap_iter_next(de_hashmap_iter *it) {
    const de_hashmap *m = it->map;
    while (it->next < m->capacity) {
        const u32 i = it->next++;
        if (de__hm_is_full(m->ctrl[i])) {
            it->index = i;
            it->key = de__hm_key(m, i);
            it->value = de__hm_value(m, i);
            return true;
        }
    }
    it->key = NULL;
    it->value = NULL;
    return false;
}

u64 de_hashmap_iter_count(void) {
#if DE_HASHMAP_ITER_TRACKING
    return de__hashmap_iter_counter;
#else
    return 0;
#endif
}
