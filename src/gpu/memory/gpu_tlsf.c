/* gpu_tlsf.c — offset-based TLSF with out-of-band block metadata (DENG-282). See gpu_tlsf.h.
 *
 * Size classes (sizes are multiples of 256):
 *   below 8 KiB: first level 0, 32 linear classes of 256 bytes;
 *   from 8 KiB:  first level = msb - 12, second level = the 5 bits below the msb.
 * 1 TiB has msb 40, so 29 first levels fit a u32 bitmap.
 *
 * Blocks are nodes in physical order (prev_phys/next_phys) that tile [0, capacity) exactly. Free
 * blocks are also on their class's list (prev_free/next_free). Two free blocks are never
 * neighbours. Nodes come from a de_pool; two spares are kept so that an allocation, which splits
 * at most twice, never fails half-way.
 */
#include "gpu/memory/gpu_tlsf.h"

#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#define G_LOG2   8u
#define SL_LOG2  5u
#define SL_COUNT (1u << SL_LOG2)
#define SMALL    ((u64)SL_COUNT << G_LOG2) /* 8 KiB */
#define FL_SHIFT (SL_LOG2 + G_LOG2 - 1u)   /* fl = msb - FL_SHIFT for sizes >= SMALL */
#define FL_COUNT 29u                       /* msb 40 (1 TiB) -> fl 28 */
#define GRAN     DE__GPU_TLSF_GRANULARITY

DE_STATIC_ASSERT(FL_COUNT <= 32, "first-level bitmap is a u32");
DE_STATIC_ASSERT(((u64)1 << (FL_COUNT - 1 + FL_SHIFT)) == DE__GPU_TLSF_MAX_CAPACITY,
                 "the top first level holds the largest capacity");

struct de__gpu_tlsf_block {
    u64 offset;
    u64 size;
    de__gpu_tlsf_block *prev_phys, *next_phys;
    de__gpu_tlsf_block *prev_free, *next_free;
    u32 is_free;
};

struct de__gpu_tlsf {
    const de_allocator *alloc;
    de_pool *nodes;
    de__gpu_tlsf_block *spare[2];
    u32 spare_count;
    u64 capacity;
    u64 used;
    u32 used_blocks, free_blocks;
    u32 fl_bitmap;
    u32 sl_bitmap[FL_COUNT];
    de__gpu_tlsf_block *heads[FL_COUNT][SL_COUNT];
    de__gpu_tlsf_block *first; /* the block at offset 0 */
};

/* ---- Bit helpers ------------------------------------------------------------------------- */

static inline u32 msb64(u64 v) {
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long i;
    _BitScanReverse64(&i, v);
    return (u32)i;
#else
    return 63u - (u32)__builtin_clzll(v);
#endif
}

static inline u32 msb32(u32 v) {
    return msb64((u64)v);
}

static inline u32 lsb32(u32 v) {
#if defined(_MSC_VER) && !defined(__clang__)
    unsigned long i;
    _BitScanForward(&i, v);
    return (u32)i;
#else
    return (u32)__builtin_ctz(v);
#endif
}

static inline u64 align_up(u64 v, u64 a) {
    return (v + (a - 1)) & ~(a - 1);
}

/* ---- Size classes ------------------------------------------------------------------------ */

static inline void mapping_insert(u64 size, u32 *fl, u32 *sl) {
    if (size < SMALL) {
        *fl = 0;
        *sl = (u32)(size >> G_LOG2);
    } else {
        const u32 m = msb64(size);
        *sl = (u32)(size >> (m - SL_LOG2)) ^ SL_COUNT;
        *fl = m - FL_SHIFT;
    }
}

/* The first class whose every block holds `size`. */
static inline void mapping_search(u64 size, u32 *fl, u32 *sl) {
    if (size >= SMALL)
        size += ((u64)1 << (msb64(size) - SL_LOG2)) - 1;
    mapping_insert(size, fl, sl);
}

/* ---- Nodes ------------------------------------------------------------------------------- */

static bool fill_spares(de__gpu_tlsf *t) {
    while (t->spare_count < 2) {
        de__gpu_tlsf_block *b = (de__gpu_tlsf_block *)de_pool_alloc(t->nodes);
        if (!b)
            return false;
        t->spare[t->spare_count++] = b;
    }
    return true;
}

static de__gpu_tlsf_block *take_node(de__gpu_tlsf *t) {
    DE_ASSERT(t->spare_count > 0);
    de__gpu_tlsf_block *b = t->spare[--t->spare_count];
    memset(b, 0, sizeof *b);
    return b;
}

static void give_node(de__gpu_tlsf *t, de__gpu_tlsf_block *b) {
    if (t->spare_count < 2)
        t->spare[t->spare_count++] = b;
    else
        de_pool_free(t->nodes, b);
}

/* ---- Free lists -------------------------------------------------------------------------- */

static void list_insert(de__gpu_tlsf *t, de__gpu_tlsf_block *b) {
    u32 fl, sl;
    mapping_insert(b->size, &fl, &sl);
    de__gpu_tlsf_block *head = t->heads[fl][sl];
    b->next_free = head;
    b->prev_free = NULL;
    if (head)
        head->prev_free = b;
    t->heads[fl][sl] = b;
    t->fl_bitmap |= 1u << fl;
    t->sl_bitmap[fl] |= 1u << sl;
    b->is_free = 1;
    ++t->free_blocks;
}

static void list_remove(de__gpu_tlsf *t, de__gpu_tlsf_block *b) {
    u32 fl, sl;
    mapping_insert(b->size, &fl, &sl);
    if (b->prev_free)
        b->prev_free->next_free = b->next_free;
    else
        t->heads[fl][sl] = b->next_free;
    if (b->next_free)
        b->next_free->prev_free = b->prev_free;
    if (!t->heads[fl][sl]) {
        t->sl_bitmap[fl] &= ~(1u << sl);
        if (!t->sl_bitmap[fl])
            t->fl_bitmap &= ~(1u << fl);
    }
    b->prev_free = b->next_free = NULL;
    b->is_free = 0;
    --t->free_blocks;
}

/* A free block of at least `size`, removed from its list, or NULL. */
static de__gpu_tlsf_block *find_free(de__gpu_tlsf *t, u64 size) {
    u32 fl, sl;
    mapping_search(size, &fl, &sl);
    if (fl >= FL_COUNT)
        return NULL;
    u32 sl_map = t->sl_bitmap[fl] & (~0u << sl);
    if (!sl_map) {
        const u32 fl_map = (u32)((u64)t->fl_bitmap & (~(u64)0 << (fl + 1)));
        if (!fl_map)
            return NULL;
        fl = lsb32(fl_map);
        sl_map = t->sl_bitmap[fl];
    }
    sl = lsb32(sl_map);
    de__gpu_tlsf_block *b = t->heads[fl][sl];
    list_remove(t, b);
    return b;
}

/* Exact-fit fallback, O(1): the head of the request's own class (and of the class of the request
 * plus its worst-case alignment gap), taken if it holds `size` at `align` from its real offset.
 * Good fit alone cannot use a block smaller than the next class boundary above the request, so
 * without this a fresh heap of exactly the request size (or a whole range asked for with an
 * alignment) would be refused. */
static de__gpu_tlsf_block *find_exact(de__gpu_tlsf *t, u64 size, u64 align) {
    u32 fl[2], sl[2];
    mapping_insert(size, &fl[0], &sl[0]);
    mapping_insert(size + (align - GRAN), &fl[1], &sl[1]);
    for (u32 i = 0; i < 2; ++i) {
        if (fl[i] >= FL_COUNT || (i == 1 && fl[1] == fl[0] && sl[1] == sl[0]))
            continue;
        de__gpu_tlsf_block *b = t->heads[fl[i]][sl[i]];
        if (b && (((b->offset + align - 1) & ~(align - 1)) - b->offset) + size <= b->size) {
            list_remove(t, b);
            return b;
        }
    }
    return NULL;
}

/* Puts node `n` into the physical chain right after `b`. */
static void link_after(de__gpu_tlsf_block *b, de__gpu_tlsf_block *n) {
    n->prev_phys = b;
    n->next_phys = b->next_phys;
    if (b->next_phys)
        b->next_phys->prev_phys = n;
    b->next_phys = n;
}

/* Takes node `n` (a right neighbour) out of the physical chain. */
static void unlink(de__gpu_tlsf_block *n) {
    if (n->prev_phys)
        n->prev_phys->next_phys = n->next_phys;
    if (n->next_phys)
        n->next_phys->prev_phys = n->prev_phys;
}

/* ---- API --------------------------------------------------------------------------------- */

de_result de__gpu_tlsf_create(const de_allocator *alloc, u64 capacity, de__gpu_tlsf **out) {
    if (!out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__gpu_tlsf_create: out is NULL");
    *out = NULL;
    capacity &= ~(GRAN - 1);
    if (capacity == 0 || capacity > DE__GPU_TLSF_MAX_CAPACITY)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de__gpu_tlsf_create: capacity %llu is outside 256 B .. 1 TiB",
                       (unsigned long long)capacity);
    const de_allocator *a = alloc ? alloc : de_heap_allocator();
    de__gpu_tlsf *t =
        (de__gpu_tlsf *)DE_ALLOC(a, sizeof *t, DE_ALIGNOF(de__gpu_tlsf), DE_MEM_RENDER);
    if (!t)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__gpu_tlsf_create: control structure");
    memset(t, 0, sizeof *t);
    t->alloc = a;
    t->capacity = capacity;
    de_pool_desc pd = {.block_size = sizeof(de__gpu_tlsf_block),
                       .block_align = DE_ALIGNOF(de__gpu_tlsf_block),
                       .blocks_per_page = 256,
                       .backing = a,
                       .tag = DE_MEM_RENDER};
    de_result r = de_pool_create(&pd, &t->nodes);
    if (r != DE_OK) {
        DE_FREE(a, t, sizeof *t);
        return r;
    }
    if (!fill_spares(t)) {
        de_pool_destroy(t->nodes);
        DE_FREE(a, t, sizeof *t);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__gpu_tlsf_create: block nodes");
    }
    de__gpu_tlsf_block *b = take_node(t);
    b->offset = 0;
    b->size = capacity;
    t->first = b;
    list_insert(t, b);
    *out = t;
    return DE_OK;
}

void de__gpu_tlsf_destroy(de__gpu_tlsf *t) {
    if (!t)
        return;
    de_pool_destroy(t->nodes); /* every node, spares included */
    const de_allocator *a = t->alloc;
    DE_FREE(a, t, sizeof *t);
}

de__gpu_tlsf_block *de__gpu_tlsf_alloc(de__gpu_tlsf *t, u64 size, u64 align, u64 *offset) {
    if (align < GRAN)
        align = GRAN;
    if ((align & (align - 1)) != 0 || size > t->capacity || align > t->capacity)
        return NULL;
    size = size ? align_up(size, GRAN) : GRAN;
    /* Two splits at most (leading gap, tail): have both nodes before touching anything. */
    if (!fill_spares(t))
        return NULL;
    /* Free blocks start at multiples of GRAN, so the gap to `align` is at most align - GRAN. */
    de__gpu_tlsf_block *b = find_free(t, size + (align - GRAN));
    if (!b)
        b = find_exact(t, size, align);
    if (!b)
        return NULL;

    const u64 aligned = align_up(b->offset, align);
    const u64 gap = aligned - b->offset;
    if (gap) {
        /* The gap becomes a free block in front. Its left neighbour is used (b was free). */
        de__gpu_tlsf_block *f = take_node(t);
        f->offset = b->offset;
        f->size = gap;
        f->prev_phys = b->prev_phys;
        f->next_phys = b;
        if (b->prev_phys)
            b->prev_phys->next_phys = f;
        else
            t->first = f;
        b->prev_phys = f;
        b->offset = aligned;
        b->size -= gap;
        list_insert(t, f);
    }
    if (b->size > size) {
        /* The tail becomes a free block. Its right neighbour is used (b was free). */
        de__gpu_tlsf_block *rest = take_node(t);
        rest->offset = b->offset + size;
        rest->size = b->size - size;
        b->size = size;
        link_after(b, rest);
        list_insert(t, rest);
    }
    t->used += b->size;
    ++t->used_blocks;
    *offset = b->offset;
    return b;
}

void de__gpu_tlsf_free(de__gpu_tlsf *t, de__gpu_tlsf_block *b) {
    if (!b)
        return;
    DE_ASSERT(!b->is_free);
    t->used -= b->size;
    --t->used_blocks;
    de__gpu_tlsf_block *prev = b->prev_phys;
    if (prev && prev->is_free) {
        list_remove(t, prev);
        prev->size += b->size;
        unlink(b);
        give_node(t, b);
        b = prev;
    }
    de__gpu_tlsf_block *next = b->next_phys;
    if (next && next->is_free) {
        list_remove(t, next);
        b->size += next->size;
        unlink(next);
        give_node(t, next);
    }
    list_insert(t, b);
}

u64 de__gpu_tlsf_block_offset(const de__gpu_tlsf_block *b) {
    return b->offset;
}

u64 de__gpu_tlsf_block_size(const de__gpu_tlsf_block *b) {
    return b->size;
}

bool de__gpu_tlsf_is_empty(const de__gpu_tlsf *t) {
    return t->used_blocks == 0;
}

void de__gpu_tlsf_get_stats(const de__gpu_tlsf *t, de__gpu_tlsf_stats *out) {
    memset(out, 0, sizeof *out);
    out->capacity = t->capacity;
    out->used = t->used;
    out->free = t->capacity - t->used;
    out->used_blocks = t->used_blocks;
    out->free_blocks = t->free_blocks;
    if (t->fl_bitmap) {
        /* The largest block is in the highest non-empty class; that list is unsorted. */
        const u32 fl = msb32(t->fl_bitmap);
        const u32 sl = msb32(t->sl_bitmap[fl]);
        for (const de__gpu_tlsf_block *b = t->heads[fl][sl]; b; b = b->next_free)
            if (b->size > out->largest_free)
                out->largest_free = b->size;
    }
    if (out->free && out->largest_free < out->free)
        out->fragmentation = 1.0f - (f32)((double)out->largest_free / (double)out->free);
}

#define CHECK(c, what)                                                                             \
    do {                                                                                           \
        if (!(c))                                                                                  \
            return DE_FAIL(DE_ERR_INTERNAL, "de__gpu_tlsf_check: %s", (what));                     \
    } while (0)

de_result de__gpu_tlsf_check(const de__gpu_tlsf *t) {
    u64 pos = 0, used = 0;
    u32 used_blocks = 0, free_blocks = 0;
    CHECK(t->first && t->first->prev_phys == NULL, "first block has a left neighbour");
    for (const de__gpu_tlsf_block *b = t->first; b; b = b->next_phys) {
        CHECK(b->offset == pos, "blocks do not tile the range");
        CHECK(b->size > 0 && (b->size & (GRAN - 1)) == 0, "block size is not a granule multiple");
        CHECK(!b->next_phys || b->next_phys->prev_phys == b, "broken physical links");
        if (b->is_free) {
            CHECK(!(b->next_phys && b->next_phys->is_free), "two free blocks are neighbours");
            u32 fl, sl;
            mapping_insert(b->size, &fl, &sl);
            CHECK(fl < FL_COUNT, "free block class out of range");
            CHECK((t->fl_bitmap & (1u << fl)) && (t->sl_bitmap[fl] & (1u << sl)),
                  "free block's class bit is clear");
            bool listed = false;
            for (const de__gpu_tlsf_block *e = t->heads[fl][sl]; e && !listed; e = e->next_free)
                listed = e == b;
            CHECK(listed, "free block missing from its class list");
            ++free_blocks;
        } else {
            used += b->size;
            ++used_blocks;
        }
        pos += b->size;
    }
    CHECK(pos == t->capacity, "blocks do not end at the capacity");
    CHECK(used == t->used && used_blocks == t->used_blocks, "used counters disagree");
    CHECK(free_blocks == t->free_blocks, "free block counter disagrees");
    u32 listed = 0;
    for (u32 fl = 0; fl < FL_COUNT; ++fl) {
        CHECK(((t->fl_bitmap >> fl) & 1u) == (t->sl_bitmap[fl] != 0 ? 1u : 0u),
              "first-level bit is wrong");
        for (u32 sl = 0; sl < SL_COUNT; ++sl) {
            const de__gpu_tlsf_block *h = t->heads[fl][sl];
            CHECK(((t->sl_bitmap[fl] >> sl) & 1u) == (h != NULL ? 1u : 0u),
                  "second-level bit is wrong");
            CHECK(!h || h->prev_free == NULL, "list head has a predecessor");
            for (const de__gpu_tlsf_block *e = h; e; e = e->next_free) {
                u32 efl, esl;
                mapping_insert(e->size, &efl, &esl);
                CHECK(e->is_free && efl == fl && esl == sl, "listed block is used or misfiled");
                ++listed;
            }
        }
    }
    CHECK(listed == free_blocks, "free lists hold blocks that are not in the range");
    return DE_OK;
}
