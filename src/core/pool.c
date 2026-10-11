/* pool.c: fixed-size block pool (SPEC §5.3).
 *
 * Page layout: `blocks_per_page` blocks of `stride` bytes from the start of the page (the page
 * is allocated with the block alignment), then the page header (the link to the previous page):
 *
 *     | block 0 | block 1 | ... | block n-1 | next page ptr |
 *
 * Free blocks form an intrusive LIFO list through their first 8 bytes. A new page is not
 * threaded onto the list; alloc bumps through it instead, so growing costs O(1), not O(blocks).
 * Pages go back to the backing allocator only in de_pool_destroy.
 *
 * Thread-safe pools take a de_mutex around alloc and free. A lock-free Treiber stack was the
 * other option; it was not chosen because popping must read the link inside a block that
 * another thread may already have popped and be writing (a data race in C11 that TSan reports,
 * however it is tagged against ABA), page growth needs a lock anyway, and an uncontended mutex
 * costs about as much as the CAS loop. See the DENG-149 closing comment.
 */
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_compiler.h"

#include <stdint.h>
#include <string.h>

#define DE__POOL_DEFAULT_ALIGN 16u
#define DE__POOL_DEFAULT_PAGE  ((size_t)64 << 10)

struct de_pool {
    de_allocator backing;
    size_t stride;     /* block_size rounded up to align */
    size_t align;      /* block alignment, >= 8 */
    size_t page_bytes; /* stride * blocks_per_page + the page header */
    size_t blocks_bytes;
    u32 blocks_per_page;
    u32 tag;
    bool thread_safe;
    void *free_list; /* intrusive LIFO of freed blocks */
    u8 *bump;        /* next never-used block in the newest page */
    u8 *bump_end;
    u8 *pages; /* newest page; each page's header links the previous one */
    u32 page_count;
    u64 live;
    de_mutex lock;
};

static size_t de__align_up(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

static bool de__is_pow2(size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/* The page header: the link to the previous page, stored after the blocks. */
static inline u8 **de__page_link(const de_pool *p, u8 *page) {
    return (u8 **)(void *)(page + p->blocks_bytes);
}

de_result de_pool_create(const de_pool_desc *desc, de_pool **out) {
    if (out == NULL || desc == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_pool_create: NULL argument");
    *out = NULL;
    if (desc->block_size == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_pool_create: block_size is 0");
    size_t align = desc->block_align ? desc->block_align : DE__POOL_DEFAULT_ALIGN;
    if (!de__is_pow2(align))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_pool_create: block_align %zu is not a power of two", align);
    if (align < sizeof(void *))
        align = sizeof(void *);
    if (desc->block_size > SIZE_MAX / 4 || align > SIZE_MAX / 4)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_pool_create: block size out of range");
    const size_t stride = de__align_up(desc->block_size, align);
    u32 bpp = desc->blocks_per_page;
    if (bpp == 0) {
        const size_t n = DE__POOL_DEFAULT_PAGE / stride;
        bpp = n == 0 ? 1u : (n > UINT32_MAX ? UINT32_MAX : (u32)n);
    }
    if (stride > (SIZE_MAX / 2) / bpp)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_pool_create: %u blocks of %zu bytes overflow a page", bpp, stride);

    const de_allocator *backing = desc->backing ? desc->backing : de_heap_allocator();
    de_pool *p = (de_pool *)DE_ALLOC(backing, sizeof(de_pool), DE_ALIGNOF(de_pool), desc->tag);
    if (p == NULL)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_pool_create: cannot allocate the pool");
    memset(p, 0, sizeof *p);
    p->backing = *backing;
    p->stride = stride;
    p->align = align;
    p->blocks_per_page = bpp;
    p->blocks_bytes = stride * bpp;
    p->page_bytes = p->blocks_bytes + sizeof(u8 *);
    p->tag = desc->tag;
    p->thread_safe = desc->thread_safe;
    if (p->thread_safe)
        de_mutex_init(&p->lock);
    *out = p;
    return DE_OK;
}

void de_pool_destroy(de_pool *p) {
    if (p == NULL)
        return;
    u8 *page = p->pages;
    while (page != NULL) {
        u8 *prev = *de__page_link(p, page);
        DE_FREE(&p->backing, page, p->page_bytes);
        page = prev;
    }
    if (p->thread_safe)
        de_mutex_destroy(&p->lock);
    const de_allocator backing = p->backing;
    DE_FREE(&backing, p, sizeof *p);
}

static inline void de__pool_lock(de_pool *p) {
    if (p->thread_safe)
        de_mutex_lock(&p->lock);
}

static inline void de__pool_unlock(de_pool *p) {
    if (p->thread_safe)
        de_mutex_unlock(&p->lock);
}

void *de_pool_alloc(de_pool *p) {
    DE_ASSERT(p != NULL);
    de__pool_lock(p);
    void *b = p->free_list;
    if (b != NULL) {
        memcpy(&p->free_list, b, sizeof(void *));
    } else if (p->bump != p->bump_end) {
        b = p->bump;
        p->bump += p->stride;
    } else {
        u8 *page = (u8 *)DE_ALLOC(&p->backing, p->page_bytes, p->align, p->tag);
        if (page == NULL) {
            de__pool_unlock(p);
            DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_pool_alloc: cannot allocate a %zu-byte page",
                         p->page_bytes);
            return NULL;
        }
        *de__page_link(p, page) = p->pages;
        p->pages = page;
        p->page_count++;
        b = page;
        p->bump = page + p->stride;
        p->bump_end = page + p->blocks_bytes;
    }
    p->live++;
    de__pool_unlock(p);
    return b;
}

#if defined(DE_DEBUG)
/* Whether `b` is the start of a block in one of the pool's pages. O(pages). */
static bool de__pool_owns(const de_pool *p, const void *b) {
    const uintptr_t x = (uintptr_t)b;
    for (u8 *page = p->pages; page != NULL; page = *de__page_link(p, page)) {
        const uintptr_t s = (uintptr_t)page;
        if (x >= s && x < s + p->blocks_bytes)
            return (x - s) % p->stride == 0;
    }
    return false;
}
#endif

void de_pool_free(de_pool *p, void *block) {
    if (block == NULL)
        return;
    DE_ASSERT(p != NULL);
    de__pool_lock(p);
#if defined(DE_DEBUG)
    if (!de__pool_owns(p, block)) {
        de__pool_unlock(p);
        DE_PANIC("de_pool_free: %p is not a block of this pool", block);
    }
#endif
    memcpy(block, &p->free_list, sizeof(void *));
    p->free_list = block;
    p->live--;
    de__pool_unlock(p);
}

de_pool_info de_pool_get_info(de_pool *p) {
    de_pool_info info;
    de__pool_lock(p);
    info.block_stride = p->stride;
    info.block_align = p->align;
    info.blocks_per_page = p->blocks_per_page;
    info.page_count = p->page_count;
    info.blocks_live = p->live;
    de__pool_unlock(p);
    return info;
}
