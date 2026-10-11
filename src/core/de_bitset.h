/*
 * de_bitset.h: bit set over 64-bit words (engine-internal, SPEC §5.6). Header-only.
 *
 * Include as "core/de_bitset.h". Not part of the public API.
 *
 * Two kinds, same functions:
 *   - fixed: over caller storage (de_bitset_init_fixed); never allocates; can be resized up to
 *     the storage it was given.
 *   - growable: on an allocator (de_bitset_init); allocates only when de_bitset_resize grows
 *     past the words it has; de_bitset_free gives them back.
 *
 * Bits at and above `bit_count` in the last word are always 0, so popcount and find-first never
 * see them. Indices must be < bit_count (checked in Debug/Dev).
 *
 *     u64 words[DE_BITSET_WORDS(100)];
 *     de_bitset b;
 *     de_bitset_init_fixed(&b, words, DE_BITSET_WORDS(100), 100);
 *     de_bitset_set(&b, 42);
 *     for (u32 i = de_bitset_find_first_set(&b, 0); i != DE_BITSET_NONE;
 *          i = de_bitset_find_first_set(&b, i + 1))
 *         ...
 *
 * Not thread-safe.
 */
#ifndef DE_CORE_BITSET_H
#define DE_CORE_BITSET_H

#include <dengine/de_memory.h>

#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

DE_EXTERN_C_BEGIN

/** Words needed for `bits` bits. */
#define DE_BITSET_WORDS(bits) (((bits) + 63u) / 64u)
/** Returned by the find functions when there is no such bit. */
#define DE_BITSET_NONE UINT32_MAX

/** A bit set. Fields are readable; change them only through the functions below. */
typedef struct de_bitset {
    u64 *words;            /**< word_capacity words */
    u32 bit_count;         /**< bits in use */
    u32 word_capacity;     /**< words available */
    const de_allocator *a; /**< NULL for a fixed bitset */
    u32 tag;               /**< memory tag for the allocator */
} de_bitset;

/* ---- Bit helpers (internal) ------------------------------------------------------------- */

/* Index of the lowest set bit of v (v != 0). */
static inline u32 de__bitset_ctz64(u64 v) {
    DE_ASSERT(v != 0);
#if defined(__GNUC__) || defined(__clang__)
    return (u32)__builtin_ctzll(v);
#elif defined(_MSC_VER) && defined(_WIN64)
    unsigned long idx;
    _BitScanForward64(&idx, v);
    return (u32)idx;
#else
    u32 n = 0;
    while ((v & 1u) == 0) {
        v >>= 1;
        ++n;
    }
    return n;
#endif
}

/* Set bits in v. Portable SWAR on MSVC, where __popcnt64 needs the POPCNT instruction. */
static inline u32 de__bitset_popcount64(u64 v) {
#if defined(__GNUC__) || defined(__clang__)
    return (u32)__builtin_popcountll(v);
#else
    v = v - ((v >> 1) & 0x5555555555555555ull);
    v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
    v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0Full;
    return (u32)((v * 0x0101010101010101ull) >> 56);
#endif
}

/* Mask of the bits in use in the last word (all ones when bit_count is a multiple of 64). */
static inline u64 de__bitset_tail_mask(u32 bit_count) {
    const u32 r = bit_count & 63u;
    return r ? ((u64)1 << r) - 1u : ~(u64)0;
}

static inline u32 de__bitset_word_count(const de_bitset *b) {
    return (u32)DE_BITSET_WORDS((u64)b->bit_count);
}

/* ---- Init / size ------------------------------------------------------------------------ */

/**
 * A fixed bitset of `bit_count` bits over `words` (`word_count` words, at least
 * DE_BITSET_WORDS(bit_count)). Clears the bits in use. Never allocates.
 */
static inline void de_bitset_init_fixed(de_bitset *b, u64 *words, u32 word_count, u32 bit_count) {
    DE_ASSERT(DE_BITSET_WORDS((u64)bit_count) <= word_count);
    b->words = words;
    b->bit_count = bit_count;
    b->word_capacity = word_count;
    b->a = NULL;
    b->tag = 0;
    if (bit_count)
        memset(words, 0, (size_t)DE_BITSET_WORDS((u64)bit_count) * sizeof(u64));
}

/** An empty growable bitset on allocator `a`. Allocates nothing. */
static inline void de_bitset_init(de_bitset *b, const de_allocator *a, u32 tag) {
    DE_ASSERT(a != NULL);
    b->words = NULL;
    b->bit_count = 0;
    b->word_capacity = 0;
    b->a = a;
    b->tag = tag;
}

/**
 * Sets the size to `bit_count`. New bits are 0; bits cut off are dropped. A growable bitset
 * allocates when it needs more words (×1.5 growth); a fixed one fails with DE_ERR_OUT_OF_MEMORY
 * past its storage. On failure nothing changes.
 */
static inline de_result de_bitset_resize(de_bitset *b, u32 bit_count) {
    const u32 old_words = de__bitset_word_count(b);
    const u32 new_words = (u32)DE_BITSET_WORDS((u64)bit_count);
    if (new_words > b->word_capacity) {
        if (b->a == NULL)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                           "de_bitset: %u bits exceed fixed storage of %u words", bit_count,
                           b->word_capacity);
        u64 cap = (u64)b->word_capacity + b->word_capacity / 2u;
        if (cap < new_words)
            cap = new_words;
        u64 *w = (u64 *)DE_REALLOC(b->a, b->words, (size_t)b->word_capacity * sizeof(u64),
                                   (size_t)cap * sizeof(u64), DE_ALIGNOF(u64), b->tag);
        if (w == NULL)
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_bitset: growing to %u bits", bit_count);
        b->words = w;
        b->word_capacity = (u32)cap;
    }
    if (new_words > old_words)
        memset(b->words + old_words, 0, (size_t)(new_words - old_words) * sizeof(u64));
    b->bit_count = bit_count;
    if (new_words)
        b->words[new_words - 1u] &= de__bitset_tail_mask(bit_count);
    return DE_OK;
}

/** Gives a growable bitset's memory back; it is empty afterwards. A fixed one becomes empty. */
static inline void de_bitset_free(de_bitset *b) {
    if (b->a != NULL && b->words != NULL) {
        DE_FREE(b->a, b->words, (size_t)b->word_capacity * sizeof(u64));
        b->words = NULL;
        b->word_capacity = 0;
    }
    b->bit_count = 0;
}

/* ---- Bits ------------------------------------------------------------------------------- */

static inline void de_bitset_set(de_bitset *b, u32 i) {
    DE_ASSERT(i < b->bit_count);
    b->words[i >> 6] |= (u64)1 << (i & 63u);
}

static inline void de_bitset_clear(de_bitset *b, u32 i) {
    DE_ASSERT(i < b->bit_count);
    b->words[i >> 6] &= ~((u64)1 << (i & 63u));
}

static inline void de_bitset_assign(de_bitset *b, u32 i, bool v) {
    if (v)
        de_bitset_set(b, i);
    else
        de_bitset_clear(b, i);
}

static inline bool de_bitset_test(const de_bitset *b, u32 i) {
    DE_ASSERT(i < b->bit_count);
    return (b->words[i >> 6] >> (i & 63u)) & 1u;
}

/** Sets every bit in use. */
static inline void de_bitset_set_all(de_bitset *b) {
    const u32 n = de__bitset_word_count(b);
    if (n == 0)
        return;
    memset(b->words, 0xFF, (size_t)n * sizeof(u64));
    b->words[n - 1u] = de__bitset_tail_mask(b->bit_count);
}

/** Clears every bit. */
static inline void de_bitset_clear_all(de_bitset *b) {
    const u32 n = de__bitset_word_count(b);
    if (n)
        memset(b->words, 0, (size_t)n * sizeof(u64));
}

/** Number of set bits. */
static inline u32 de_bitset_popcount(const de_bitset *b) {
    const u32 n = de__bitset_word_count(b);
    u32 c = 0;
    for (u32 w = 0; w < n; ++w)
        c += de__bitset_popcount64(b->words[w]);
    return c;
}

/** Lowest set bit at index >= `from`, or DE_BITSET_NONE. `from` may be >= bit_count. */
static inline u32 de_bitset_find_first_set(const de_bitset *b, u32 from) {
    if (from >= b->bit_count)
        return DE_BITSET_NONE;
    const u32 n = de__bitset_word_count(b);
    u32 w = from >> 6;
    u64 v = b->words[w] & (~(u64)0 << (from & 63u));
    for (;;) {
        if (v != 0)
            return (w << 6) + de__bitset_ctz64(v);
        if (++w >= n)
            return DE_BITSET_NONE;
        v = b->words[w];
    }
}

/** Lowest clear bit at index >= `from` (and < bit_count), or DE_BITSET_NONE. */
static inline u32 de_bitset_find_first_clear(const de_bitset *b, u32 from) {
    if (from >= b->bit_count)
        return DE_BITSET_NONE;
    const u32 n = de__bitset_word_count(b);
    u32 w = from >> 6;
    u64 v = ~b->words[w] & (~(u64)0 << (from & 63u));
    for (;;) {
        if (w == n - 1u)
            v &= de__bitset_tail_mask(b->bit_count);
        if (v != 0)
            return (w << 6) + de__bitset_ctz64(v);
        if (++w >= n)
            return DE_BITSET_NONE;
        v = ~b->words[w];
    }
}

/** dst &= src. Both must have the same bit_count. */
static inline void de_bitset_and(de_bitset *dst, const de_bitset *src) {
    DE_ASSERT(dst->bit_count == src->bit_count);
    const u32 n = de__bitset_word_count(dst);
    for (u32 w = 0; w < n; ++w)
        dst->words[w] &= src->words[w];
}

/** dst |= src. Both must have the same bit_count. */
static inline void de_bitset_or(de_bitset *dst, const de_bitset *src) {
    DE_ASSERT(dst->bit_count == src->bit_count);
    const u32 n = de__bitset_word_count(dst);
    for (u32 w = 0; w < n; ++w)
        dst->words[w] |= src->words[w];
}

/** dst &= ~src. Both must have the same bit_count. */
static inline void de_bitset_and_not(de_bitset *dst, const de_bitset *src) {
    DE_ASSERT(dst->bit_count == src->bit_count);
    const u32 n = de__bitset_word_count(dst);
    for (u32 w = 0; w < n; ++w)
        dst->words[w] &= ~src->words[w];
}

DE_EXTERN_C_END

#endif /* DE_CORE_BITSET_H */
