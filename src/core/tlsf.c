/* tlsf.c: two-level segregated fit allocator (SPEC §5.3), after M. Masmano, I. Ripoll,
 * A. Crespo, J. Real: "TLSF: a New Dynamic Memory Allocator for Real-Time Systems" (ECRTS 2004).
 *
 * Size classes. A free block of payload size s is filed under (fl, sl):
 *   s <  512: fl = 0, sl = s / 16                     (32 exact classes of 16 bytes)
 *   s >= 512: fl = msb(s) - 8, sl = the 5 bits below the msb
 * so each power-of-two range is split in 32 classes. fl_bitmap has a bit per non-empty first
 * level, sl_bitmap[fl] a bit per non-empty list. A search rounds the request up to the next class
 * boundary, so any block in that class or above fits: two find-first-set operations find it
 * without looking at a single block (good fit, O(1)).
 *
 * Block layout (all blocks 16-byte aligned, payload sizes multiples of 16):
 *
 *     | prev_phys (8) | meta (8) | payload ...                     | next block header ...
 *                                 ^ user pointer = block + 16
 *
 * meta = payload size (bits 4..39) | FREE (bit 0) | tag (bits 56..63). prev_phys always points
 * at the physically previous block (NULL for a pool's first block), so free merges with both
 * neighbours in O(1). A free block keeps its list links in the first 16 payload bytes, hence the
 * minimum payload of 16. Two free blocks are never neighbours: free merges at once.
 *
 * A pool is [pool header 16 | first block ... | sentinel header 16]. The sentinel is a used
 * block of size 0, so a merge never runs past the end of a pool; pools are linked from the
 * control structure for de_tlsf_check.
 */
#include <dengine/de_memory.h>

#include "core/de_compiler.h"
#include "core/mem_internal.h"

#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

DE_STATIC_ASSERT(sizeof(void *) == 8 && sizeof(size_t) == 8, "the TLSF assumes 64-bit pointers");

#define DE__ALIGN_LOG2    4
#define DE__ALIGN         ((size_t)1 << DE__ALIGN_LOG2) /* 16: block and payload granularity */
#define DE__SL_LOG2       5
#define DE__SL_COUNT      (1u << DE__SL_LOG2)             /* 32 classes per power of two */
#define DE__FL_SHIFT      (DE__SL_LOG2 + DE__ALIGN_LOG2)  /* 9 */
#define DE__SMALL_BLOCK   ((size_t)1 << DE__FL_SHIFT)     /* 512: below it, classes are linear */
#define DE__FL_MAX        40                              /* payloads < 2^40 */
#define DE__FL_COUNT      (DE__FL_MAX - DE__FL_SHIFT + 1) /* 32 */
#define DE__HDR           ((size_t)16)                    /* block header */
#define DE__MIN_PAYLOAD   ((size_t)16)                    /* room for the free-list links */
#define DE__MIN_BLOCK     (DE__HDR + DE__MIN_PAYLOAD)     /* smallest block that can be split off */
#define DE__POOL_HDR      ((size_t)16)
#define DE__POOL_OVERHEAD (DE__POOL_HDR + DE__HDR + DE__HDR) /* pool header, first + sentinel */
#define DE__POOL_MAX      ((size_t)1 << DE__FL_MAX)

#define DE__META_FREE      ((u64)1)
#define DE__META_SIZE_MASK ((((u64)1 << DE__FL_MAX) - 1) & ~(u64)(DE__ALIGN - 1))
#define DE__META_TAG_SHIFT 56

DE_STATIC_ASSERT(DE__FL_COUNT <= 32, "fl_bitmap is 32 bits");
DE_STATIC_ASSERT(DE__TLSF_MAX_ALLOC * 2 < DE__POOL_MAX, "searches must stay below FL_MAX");

typedef struct de__blk {
    struct de__blk *prev_phys;
    u64 meta;
    /* Valid only while the block is free (they overlay the payload). */
    struct de__blk *next_free;
    struct de__blk *prev_free;
} de__blk;

typedef struct de__pool_hdr {
    struct de__pool_hdr *next;
    u64 bytes; /* bytes from this header to the end of the sentinel */
} de__pool_hdr;

DE_STATIC_ASSERT(sizeof(de__pool_hdr) == DE__POOL_HDR, "pool header size");
DE_STATIC_ASSERT(offsetof(de__blk, next_free) == DE__HDR, "block header size");

struct de_tlsf {
    u32 fl_bitmap;
    u32 sl_bitmap[DE__FL_COUNT];
    de__blk *heads[DE__FL_COUNT][DE__SL_COUNT];
    de__pool_hdr *pools;
    u32 pool_count;
    u64 capacity; /* sum of the payload of each pool's single block when empty */
};

/* ---- Bit helpers ------------------------------------------------------------------------- */

/* Index of the highest set bit; v != 0. */
static inline u32 de__msb64(u64 v) {
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long i;
    _BitScanReverse64(&i, v);
    return (u32)i;
#else
    return 63u - (u32)__builtin_clzll(v);
#endif
}

/* Index of the lowest set bit; v != 0. */
static inline u32 de__lsb32(u32 v) {
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long i;
    _BitScanForward(&i, v);
    return (u32)i;
#else
    return (u32)__builtin_ctz(v);
#endif
}

static inline size_t de__align_up(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

static inline bool de__is_pow2(size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/* ---- Block helpers ----------------------------------------------------------------------- */

static inline size_t de__blk_size(const de__blk *b) {
    return (size_t)(b->meta & DE__META_SIZE_MASK);
}

static inline bool de__blk_is_free(const de__blk *b) {
    return (b->meta & DE__META_FREE) != 0;
}

static inline void de__blk_set_size(de__blk *b, size_t size) {
    b->meta = (b->meta & ~DE__META_SIZE_MASK) | (u64)size;
}

static inline void de__blk_set_free(de__blk *b, bool f) {
    b->meta = f ? (b->meta | DE__META_FREE) : (b->meta & ~DE__META_FREE);
}

static inline void de__blk_set_tag(de__blk *b, u32 tag) {
    b->meta =
        (b->meta & ~((u64)0xFF << DE__META_TAG_SHIFT)) | ((u64)(tag & 0xFFu) << DE__META_TAG_SHIFT);
}

static inline u8 *de__blk_payload(const de__blk *b) {
    return (u8 *)b + DE__HDR;
}

static inline de__blk *de__blk_from_payload(const void *p) {
    return (de__blk *)((u8 *)p - DE__HDR);
}

static inline de__blk *de__blk_next(const de__blk *b) {
    return (de__blk *)(de__blk_payload(b) + de__blk_size(b));
}

static void de__debug_fill(void *p, int byte, size_t n) {
#if defined(DE_DEBUG)
    if (n != 0)
        memset(p, byte, n);
#else
    (void)p;
    (void)byte;
    (void)n;
#endif
}

/* ---- Size classes ------------------------------------------------------------------------ */

/* The class a free block of payload `size` is filed under. */
static inline void de__mapping_insert(size_t size, u32 *fl, u32 *sl) {
    if (size < DE__SMALL_BLOCK) {
        *fl = 0;
        *sl = (u32)(size >> DE__ALIGN_LOG2);
    } else {
        const u32 m = de__msb64((u64)size);
        *sl = (u32)(size >> (m - DE__SL_LOG2)) ^ DE__SL_COUNT;
        *fl = m - (DE__FL_SHIFT - 1);
    }
}

/* The first class whose every block holds `size`: round up to the next class boundary. */
static inline void de__mapping_search(size_t size, u32 *fl, u32 *sl) {
    if (size >= DE__SMALL_BLOCK)
        size += ((size_t)1 << (de__msb64((u64)size) - DE__SL_LOG2)) - 1;
    de__mapping_insert(size, fl, sl);
}

/* ---- Free lists -------------------------------------------------------------------------- */

static void de__list_insert(de_tlsf *t, de__blk *b) {
    u32 fl, sl;
    de__mapping_insert(de__blk_size(b), &fl, &sl);
    de__blk *head = t->heads[fl][sl];
    b->next_free = head;
    b->prev_free = NULL;
    if (head != NULL)
        head->prev_free = b;
    t->heads[fl][sl] = b;
    t->fl_bitmap |= 1u << fl;
    t->sl_bitmap[fl] |= 1u << sl;
}

static void de__list_remove(de_tlsf *t, de__blk *b) {
    u32 fl, sl;
    de__mapping_insert(de__blk_size(b), &fl, &sl);
    if (b->prev_free != NULL)
        b->prev_free->next_free = b->next_free;
    else
        t->heads[fl][sl] = b->next_free;
    if (b->next_free != NULL)
        b->next_free->prev_free = b->prev_free;
    if (t->heads[fl][sl] == NULL) {
        t->sl_bitmap[fl] &= ~(1u << sl);
        if (t->sl_bitmap[fl] == 0)
            t->fl_bitmap &= ~(1u << fl);
    }
}

/* A free block of at least `size` payload, removed from its list, or NULL. Two bitmap scans. */
static de__blk *de__find_free(de_tlsf *t, size_t size) {
    u32 fl, sl;
    de__mapping_search(size, &fl, &sl);
    if (fl >= DE__FL_COUNT)
        return NULL;
    u32 sl_map = t->sl_bitmap[fl] & (~0u << sl);
    if (sl_map == 0) {
        /* Nothing large enough on this level: the smallest non-empty higher level. */
        const u32 fl_map = (u32)((u64)t->fl_bitmap & (~(u64)0 << (fl + 1)));
        if (fl_map == 0)
            return NULL;
        fl = de__lsb32(fl_map);
        sl_map = t->sl_bitmap[fl];
    }
    sl = de__lsb32(sl_map);
    de__blk *b = t->heads[fl][sl];
    de__list_remove(t, b);
    return b;
}

/* ---- Split and merge --------------------------------------------------------------------- */

/* Splits the tail of block `b` (not in a list) off as a free block when at least DE__MIN_BLOCK is
 * left after `size` payload bytes. The tail is merged with a free next neighbour and listed. */
static void de__trim_tail(de_tlsf *t, de__blk *b, size_t size) {
    const size_t cur = de__blk_size(b);
    if (cur < size + DE__MIN_BLOCK)
        return;
    de__blk *rest = (de__blk *)(de__blk_payload(b) + size);
    de__blk *next = de__blk_next(b);
    size_t rest_size = cur - size - DE__HDR;
    rest->prev_phys = b;
    rest->meta = 0;
    de__blk_set_size(b, size);
    if (de__blk_is_free(next)) {
        de__list_remove(t, next);
        rest_size += DE__HDR + de__blk_size(next);
        next = de__blk_next(next);
    }
    de__blk_set_size(rest, rest_size);
    de__blk_set_free(rest, true);
    next->prev_phys = rest;
    de__list_insert(t, rest);
}

/* ---- Allocation -------------------------------------------------------------------------- */

static inline size_t de__adjust_size(size_t size) {
    return size < DE__MIN_PAYLOAD ? DE__MIN_PAYLOAD : de__align_up(size, DE__ALIGN);
}

void *de__tlsf_alloc(de_tlsf *t, size_t size, size_t align, u32 tag) {
    if (align < DE__ALIGN)
        align = DE__ALIGN;
    if (!de__is_pow2(align) || size > DE__TLSF_MAX_ALLOC || align > DE__TLSF_MAX_ALLOC)
        return NULL;
    const size_t asz = de__adjust_size(size);
    /* For a larger alignment, search for enough room to skip to an aligned address and still
     * leave a valid free block in front (gap 0 or >= DE__MIN_BLOCK): at most align + 16. */
    const size_t req = align > DE__ALIGN ? asz + align + DE__MIN_BLOCK : asz;
    de__blk *b = de__find_free(t, req);
    if (b == NULL)
        return NULL;

    if (align > DE__ALIGN) {
        const uintptr_t p = (uintptr_t)de__blk_payload(b);
        uintptr_t aligned = (p + (align - 1)) & ~(uintptr_t)(align - 1);
        size_t gap = (size_t)(aligned - p);
        if (gap != 0 && gap < DE__MIN_BLOCK) {
            aligned += align;
            gap += align;
        }
        if (gap != 0) {
            /* Free the gap as its own block in front; its previous neighbour is used (no two
             * free neighbours), so it needs no merge. */
            de__blk *nb = de__blk_from_payload((void *)aligned);
            de__blk *next = de__blk_next(b);
            nb->prev_phys = b;
            nb->meta = 0;
            de__blk_set_size(nb, de__blk_size(b) - gap);
            next->prev_phys = nb;
            de__blk_set_size(b, gap - DE__HDR);
            de__blk_set_free(b, true);
            de__list_insert(t, b);
            b = nb;
        }
    }

    de__blk_set_free(b, false);
    de__trim_tail(t, b, asz);
    de__blk_set_tag(b, tag);
    de__debug_fill(de__blk_payload(b), 0xCD, size);
    return de__blk_payload(b);
}

void de__tlsf_free(de_tlsf *t, void *ptr) {
    if (ptr == NULL)
        return;
    de__blk *b = de__blk_from_payload(ptr);
    DE_ASSERT(!de__blk_is_free(b));
    if (de__blk_size(b) > DE__MIN_PAYLOAD)
        de__debug_fill(de__blk_payload(b) + DE__MIN_PAYLOAD, 0xDD,
                       de__blk_size(b) - DE__MIN_PAYLOAD);
    b->meta = (b->meta & DE__META_SIZE_MASK) | DE__META_FREE; /* clears the tag */

    de__blk *prev = b->prev_phys;
    if (prev != NULL && de__blk_is_free(prev)) {
        de__list_remove(t, prev);
        de__blk_set_size(prev, de__blk_size(prev) + DE__HDR + de__blk_size(b));
        b = prev;
        de__blk_next(b)->prev_phys = b;
    }
    de__blk *next = de__blk_next(b);
    if (de__blk_is_free(next)) {
        de__list_remove(t, next);
        de__blk_set_size(b, de__blk_size(b) + DE__HDR + de__blk_size(next));
        de__blk_next(b)->prev_phys = b;
    }
    de__list_insert(t, b);
}

bool de__tlsf_resize_in_place(de_tlsf *t, void *ptr, size_t new_size, size_t align, u32 tag) {
    if (align < DE__ALIGN)
        align = DE__ALIGN;
    if (!de__is_pow2(align) || new_size > DE__TLSF_MAX_ALLOC || ((uintptr_t)ptr & (align - 1)))
        return false;
    de__blk *b = de__blk_from_payload(ptr);
    const size_t asz = de__adjust_size(new_size);
    const size_t cur = de__blk_size(b);
    if (asz > cur) {
        de__blk *next = de__blk_next(b);
        if (!de__blk_is_free(next) || cur + DE__HDR + de__blk_size(next) < asz)
            return false;
        de__list_remove(t, next);
        de__blk_set_size(b, cur + DE__HDR + de__blk_size(next));
        de__blk_next(b)->prev_phys = b;
        de__debug_fill(de__blk_payload(b) + cur, 0xCD, asz - cur);
    }
    de__trim_tail(t, b, asz);
    de__blk_set_tag(b, tag);
    return true;
}

u32 de__tlsf_block_tag(const void *ptr) {
    return (u32)(de__blk_from_payload(ptr)->meta >> DE__META_TAG_SHIFT);
}

size_t de__tlsf_block_size(const void *ptr) {
    return de__blk_size(de__blk_from_payload(ptr));
}

/* ---- Pools and creation ------------------------------------------------------------------ */

size_t de_tlsf_control_size(void) {
    return de__align_up(sizeof(de_tlsf), DE__ALIGN);
}

/* Aligns [memory, memory + bytes) to 16 inside; 0 if too small. */
static size_t de__pool_bytes(void *memory, size_t bytes, u8 **start) {
    const uintptr_t p = (uintptr_t)memory;
    const uintptr_t a = (p + (DE__ALIGN - 1)) & ~(uintptr_t)(DE__ALIGN - 1);
    if (a < p || bytes < (size_t)(a - p))
        return 0;
    *start = (u8 *)a;
    return (bytes - (size_t)(a - p)) & ~(DE__ALIGN - 1);
}

de_result de_tlsf_add_pool(de_tlsf *t, void *memory, size_t bytes) {
    if (t == NULL || memory == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_tlsf_add_pool: NULL argument");
    u8 *start = NULL;
    const size_t n = de__pool_bytes(memory, bytes, &start);
    if (n < DE__POOL_OVERHEAD + DE__MIN_PAYLOAD)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_tlsf_add_pool: %zu bytes is too small", bytes);
    if (n > DE__POOL_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_tlsf_add_pool: %zu bytes is larger than the 2^40-byte maximum", bytes);

    de__pool_hdr *ph = (de__pool_hdr *)start;
    ph->bytes = (u64)n;
    ph->next = t->pools;
    t->pools = ph;
    t->pool_count++;

    const size_t payload = n - DE__POOL_OVERHEAD;
    de__blk *b = (de__blk *)(start + DE__POOL_HDR);
    b->prev_phys = NULL;
    b->meta = 0;
    de__blk_set_size(b, payload);
    de__blk_set_free(b, true);
    de__blk *sentinel = de__blk_next(b);
    sentinel->prev_phys = b;
    sentinel->meta = 0; /* used, size 0 */
    de__list_insert(t, b);
    t->capacity += payload;
    return DE_OK;
}

de_result de_tlsf_create(void *memory, size_t bytes, de_tlsf **out) {
    if (out == NULL || memory == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_tlsf_create: NULL argument");
    *out = NULL;
    u8 *start = NULL;
    const size_t n = de__pool_bytes(memory, bytes, &start);
    const size_t ctl = de_tlsf_control_size();
    if (n < ctl + DE__POOL_OVERHEAD + DE__MIN_PAYLOAD)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_tlsf_create: %zu bytes cannot hold the %zu-byte control structure and "
                       "a pool",
                       bytes, ctl);
    de_tlsf *t = (de_tlsf *)start;
    memset(t, 0, sizeof *t);
    DE_TRY(de_tlsf_add_pool(t, start + ctl, n - ctl));
    *out = t;
    return DE_OK;
}

/* ---- Allocator interface ----------------------------------------------------------------- */

static void *de__tlsf_alloc_fn(void *user, size_t size, size_t align, u32 tag) {
    void *p = de__tlsf_alloc((de_tlsf *)user, size, align, tag);
    if (DE_UNLIKELY(p == NULL))
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "tlsf: alloc of %zu bytes (align %zu) failed", size, align);
    return p;
}

static void *de__tlsf_realloc_fn(void *user, void *ptr, size_t old_size, size_t new_size,
                                 size_t align, u32 tag) {
    de_tlsf *t = (de_tlsf *)user;
    if (ptr == NULL)
        return de__tlsf_alloc_fn(user, new_size, align, tag);
    if (de__tlsf_resize_in_place(t, ptr, new_size, align, tag))
        return ptr;
    void *q = de__tlsf_alloc_fn(user, new_size, align, tag);
    if (q == NULL)
        return NULL;
    size_t n = old_size < new_size ? old_size : new_size;
    const size_t cap = de__tlsf_block_size(ptr);
    memcpy(q, ptr, n < cap ? n : cap);
    de__tlsf_free(t, ptr);
    return q;
}

static void de__tlsf_free_fn(void *user, void *ptr, size_t size) {
    (void)size;
    de__tlsf_free((de_tlsf *)user, ptr);
}

de_allocator de_tlsf_allocator(de_tlsf *t) {
    de_allocator a;
    a.alloc = de__tlsf_alloc_fn;
    a.realloc = de__tlsf_realloc_fn;
    a.free = de__tlsf_free_fn;
    a.user = t;
    return a;
}

/* ---- Consistency check ------------------------------------------------------------------- */

#define DE__CHECK(cond, ...)                                                                       \
    do {                                                                                           \
        if (!(cond))                                                                               \
            return DE_FAIL(DE_ERR_INTERNAL, __VA_ARGS__);                                          \
    } while (0)

de_result de_tlsf_check(const de_tlsf *t, de_tlsf_info *out) {
    DE__CHECK(t != NULL, "tlsf check: NULL");
    de_tlsf_info info;
    memset(&info, 0, sizeof info);
    size_t phys_free = 0;
    u64 phys_free_blocks = 0;

    /* Physical walk of every pool. */
    u32 pools = 0;
    u64 capacity = 0;
    for (const de__pool_hdr *ph = t->pools; ph != NULL; ph = ph->next) {
        pools++;
        DE__CHECK(pools <= t->pool_count, "tlsf check: pool list longer than %u", t->pool_count);
        const u8 *end = (const u8 *)ph + ph->bytes;
        capacity += ph->bytes - DE__POOL_OVERHEAD;
        const de__blk *prev = NULL;
        const de__blk *b = (const de__blk *)((const u8 *)ph + DE__POOL_HDR);
        for (;;) {
            DE__CHECK((const u8 *)b + DE__HDR <= end, "tlsf check: block %p past pool end",
                      (const void *)b);
            DE__CHECK(((uintptr_t)b & (DE__ALIGN - 1)) == 0, "tlsf check: block %p misaligned",
                      (const void *)b);
            DE__CHECK(b->prev_phys == prev, "tlsf check: block %p has a wrong prev_phys",
                      (const void *)b);
            const size_t s = de__blk_size(b);
            if (s == 0 && !de__blk_is_free(b)) {
                DE__CHECK((const u8 *)b + DE__HDR == end, "tlsf check: sentinel %p not at end",
                          (const void *)b);
                break;
            }
            DE__CHECK(s >= DE__MIN_PAYLOAD, "tlsf check: block %p payload %zu too small",
                      (const void *)b, s);
            if (de__blk_is_free(b)) {
                DE__CHECK(prev == NULL || !de__blk_is_free(prev),
                          "tlsf check: free neighbours at %p", (const void *)b);
                phys_free += s;
                phys_free_blocks++;
                info.free_blocks++;
                if (s > info.largest_free)
                    info.largest_free = s;
            } else {
                info.used_bytes += s;
                info.used_blocks++;
            }
            prev = b;
            b = de__blk_next(b);
        }
    }
    DE__CHECK(pools == t->pool_count, "tlsf check: %u pools listed, %u counted", pools,
              t->pool_count);
    DE__CHECK(capacity == t->capacity, "tlsf check: capacity %llu, expected %llu",
              (unsigned long long)capacity, (unsigned long long)t->capacity);

    /* The free lists and bitmaps. */
    size_t list_free = 0;
    u64 list_blocks = 0;
    for (u32 fl = 0; fl < DE__FL_COUNT; ++fl) {
        DE__CHECK((((t->fl_bitmap >> fl) & 1u) != 0) == (t->sl_bitmap[fl] != 0),
                  "tlsf check: fl_bitmap bit %u disagrees with sl_bitmap", fl);
        for (u32 sl = 0; sl < DE__SL_COUNT; ++sl) {
            const de__blk *b = t->heads[fl][sl];
            DE__CHECK((((t->sl_bitmap[fl] >> sl) & 1u) != 0) == (b != NULL),
                      "tlsf check: sl_bitmap[%u] bit %u disagrees with its list", fl, sl);
            const de__blk *prev = NULL;
            for (; b != NULL; prev = b, b = b->next_free) {
                DE__CHECK(++list_blocks <= phys_free_blocks,
                          "tlsf check: more listed blocks than free blocks (cycle?)");
                DE__CHECK(de__blk_is_free(b), "tlsf check: used block %p in list %u/%u",
                          (const void *)b, fl, sl);
                DE__CHECK(b->prev_free == prev, "tlsf check: bad prev_free at %p", (const void *)b);
                u32 bfl, bsl;
                de__mapping_insert(de__blk_size(b), &bfl, &bsl);
                DE__CHECK(bfl == fl && bsl == sl,
                          "tlsf check: block %p of size %zu in list %u/%u, belongs in %u/%u",
                          (const void *)b, de__blk_size(b), fl, sl, bfl, bsl);
                list_free += de__blk_size(b);
            }
        }
    }
    DE__CHECK(list_blocks == phys_free_blocks, "tlsf check: %llu listed, %llu free blocks",
              (unsigned long long)list_blocks, (unsigned long long)phys_free_blocks);
    DE__CHECK(list_free == phys_free, "tlsf check: %zu listed bytes, %zu free bytes", list_free,
              phys_free);

    info.capacity = (size_t)capacity;
    info.free_bytes = list_free;
    info.pool_count = pools;
    if (out != NULL)
        *out = info;
    return DE_OK;
}
