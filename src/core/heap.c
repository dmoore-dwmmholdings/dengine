/* heap.c: the process-wide default heap, de_heap_allocator() (SPEC §5.3).
 *
 * Small blocks (<= DE_HEAP_LARGE_THRESHOLD, alignment <= 64 KiB) come from one TLSF behind a
 * de_mutex. The TLSF lives in a single virtual-memory reservation:
 *
 *     base                                                                 base + reserved
 *     | control + pool 0 (4 MiB) | pool 1 | pool 2 | ...  committed | reserved, PROT_NONE |
 *
 * When the TLSF cannot serve a request, the next piece of the reservation (4 MiB, or more for a
 * request that needs it) is committed and added as a new pool, and the request is retried. Pools
 * are never given back. Whether a pointer is small is one range check against the reservation,
 * which never changes after initialisation.
 *
 * Large blocks get their own reservation: one page holding a header in front of the data, then
 * the data, committed exactly; free releases the reservation. They need no lock.
 *
 * The heap is created by the first call that needs it (any thread): a CAS on `state` picks the
 * initialising thread, the others spin until it publishes the result with a release store.
 *
 * In DE_MEM_TRACKING builds every block is reported to the tag stats and hooks: allocs after the
 * lock is released (a hook may allocate), frees before the block goes back. The tag of a small
 * block is kept in its TLSF header, the tag of a large block in its header.
 */
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "core/de_compiler.h"
#include "core/mem_internal.h"
#include "core/vmem.h"
#include "platform/de_atomic.h"

#include <stdint.h>
#include <string.h>

#define DE__MIB ((size_t)1 << 20)
#define DE__GIB ((size_t)1 << 30)

#define DE__HEAP_RESERVE        ((size_t)64 * DE__GIB)
#define DE__HEAP_MIN_RESERVE    ((size_t)256 * DE__MIB)
#define DE__HEAP_POOL_STEP      ((size_t)4 * DE__MIB)
#define DE__HEAP_MAX_TLSF_ALIGN ((size_t)64 << 10)
/* Room a pool needs beyond the request: pool header, block header, sentinel, and the front gap
 * an aligned search may leave. */
#define DE__HEAP_POOL_SLACK ((size_t)256)

enum { DE__HEAP_NONE = 0, DE__HEAP_INITIALISING, DE__HEAP_READY, DE__HEAP_FAILED };

typedef struct de__heap_state {
    de_atomic_u32 state;
    de_mutex lock;
    de_tlsf *tlsf;
    u8 *base;         /* written once before READY, read without the lock */
    size_t reserved;  /* idem */
    size_t committed; /* under lock */
    u32 pool_count;   /* under lock */
    de_atomic_u64 large_count;
    de_atomic_u64 large_bytes;
} de__heap_state;

static de__heap_state de__heap;

#define DE__LARGE_MAGIC 0x4C524745u /* "LRGE" */

typedef struct de__large_hdr {
    u8 *base;        /* reservation */
    size_t reserved; /* its size */
    size_t capacity; /* committed data bytes from the user pointer */
    size_t mapped;   /* committed bytes in all (header page + data) */
    u32 tag;
    u32 magic;
} de__large_hdr;

static size_t de__align_up(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

static bool de__is_pow2(size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/* ---- Initialisation ---------------------------------------------------------------------- */

static bool de__heap_init(void) {
    const size_t gran = de__vmem_reserve_granularity();
    size_t size = DE__HEAP_RESERVE;
    u8 *base = (u8 *)de__vmem_reserve(size);
    while (base == NULL && size > DE__HEAP_MIN_RESERVE) {
        size = de__align_up(size / 2, gran);
        base = (u8 *)de__vmem_reserve(size);
    }
    if (base == NULL)
        return false;
    if (!de__vmem_commit(base, DE__HEAP_POOL_STEP)) {
        de__vmem_release(base, size);
        return false;
    }
    de_tlsf *t = NULL;
    if (de_tlsf_create(base, DE__HEAP_POOL_STEP, &t) != DE_OK) {
        de__vmem_release(base, size);
        return false;
    }
    de_mutex_init(&de__heap.lock);
    de__heap.tlsf = t;
    de__heap.base = base;
    de__heap.reserved = size;
    de__heap.committed = DE__HEAP_POOL_STEP;
    de__heap.pool_count = 1;
    return true;
}

/* True when the heap is usable; creates it on first use. */
static bool de__heap_ready(void) {
    u32 s = de_atomic_load_u32(&de__heap.state, DE_ATOMIC_ACQUIRE);
    if (DE_LIKELY(s == DE__HEAP_READY))
        return true;
    if (s == DE__HEAP_NONE && de_atomic_cas_u32(&de__heap.state, &s, DE__HEAP_INITIALISING,
                                                DE_ATOMIC_ACQUIRE, DE_ATOMIC_ACQUIRE)) {
        const bool ok = de__heap_init();
        de_atomic_store_u32(&de__heap.state, ok ? DE__HEAP_READY : DE__HEAP_FAILED,
                            DE_ATOMIC_RELEASE);
        return ok;
    }
    while ((s = de_atomic_load_u32(&de__heap.state, DE_ATOMIC_ACQUIRE)) == DE__HEAP_INITIALISING)
        de_cpu_pause();
    return s == DE__HEAP_READY;
}

/* Whether `ptr` is a small (TLSF) block. base/reserved are written once, before the release
 * store of READY, and never change; the acquire load makes them safe to read here even when a
 * large block is freed while another thread creates the heap. */
static inline bool de__heap_owns(const void *ptr) {
    if (de_atomic_load_u32(&de__heap.state, DE_ATOMIC_ACQUIRE) != DE__HEAP_READY)
        return false;
    const uintptr_t p = (uintptr_t)ptr;
    const uintptr_t b = (uintptr_t)de__heap.base;
    return p >= b && p - b < de__heap.reserved;
}

/* Commits the next pool, big enough for `size` at `align`. Under the lock. */
static bool de__heap_grow(size_t size, size_t align) {
    const size_t page = de__vmem_page_size();
    const size_t need = de__align_up(size + align + DE__HEAP_POOL_SLACK, page);
    size_t step = need > DE__HEAP_POOL_STEP ? need : DE__HEAP_POOL_STEP;
    const size_t left = de__heap.reserved - de__heap.committed;
    if (step > left)
        step = left;
    if (step < need)
        return false;
    u8 *p = de__heap.base + de__heap.committed;
    if (!de__vmem_commit(p, step))
        return false;
    if (de_tlsf_add_pool(de__heap.tlsf, p, step) != DE_OK) {
        de__vmem_decommit(p, step);
        return false;
    }
    de__heap.committed += step;
    de__heap.pool_count++;
    return true;
}

/* ---- Large blocks ------------------------------------------------------------------------ */

static inline bool de__is_large(size_t size, size_t align) {
    return size > DE_HEAP_LARGE_THRESHOLD || align > DE__HEAP_MAX_TLSF_ALIGN;
}

static inline de__large_hdr *de__large_hdr_of(const void *ptr) {
    return (de__large_hdr *)((u8 *)ptr - sizeof(de__large_hdr));
}

static void *de__large_alloc(size_t size, size_t align, u32 tag) {
    const size_t page = de__vmem_page_size();
    const size_t gran = de__vmem_reserve_granularity();
    const size_t a = align > page ? align : page;
    if (size > SIZE_MAX / 4 || a > SIZE_MAX / 4)
        return NULL;
    const size_t data = de__align_up(size == 0 ? 1 : size, page);
    /* The user pointer is a-aligned and at least one page in: base + page <= ptr <= base + a. */
    const size_t total = de__align_up(data + a, gran);
    u8 *base = (u8 *)de__vmem_reserve(total);
    if (base == NULL)
        return NULL;
    const uintptr_t up = ((uintptr_t)base + page + (a - 1)) & ~(uintptr_t)(a - 1);
    u8 *ptr = (u8 *)up;
    if (!de__vmem_commit(ptr - page, page + data)) {
        de__vmem_release(base, total);
        return NULL;
    }
    de__large_hdr *h = de__large_hdr_of(ptr);
    h->base = base;
    h->reserved = total;
    h->capacity = data;
    h->mapped = page + data;
    h->tag = tag;
    h->magic = DE__LARGE_MAGIC;
    de_atomic_fetch_add_u64(&de__heap.large_count, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_add_u64(&de__heap.large_bytes, (u64)h->mapped, DE_ATOMIC_RELAXED);
    return ptr;
}

static void de__large_free(void *ptr) {
    de__large_hdr *h = de__large_hdr_of(ptr);
    DE_ASSERT(h->magic == DE__LARGE_MAGIC);
    u8 *base = h->base;
    const size_t reserved = h->reserved;
    de_atomic_fetch_sub_u64(&de__heap.large_count, 1, DE_ATOMIC_RELAXED);
    de_atomic_fetch_sub_u64(&de__heap.large_bytes, (u64)h->mapped, DE_ATOMIC_RELAXED);
    de__vmem_release(base, reserved);
}

/* ---- Allocator functions ----------------------------------------------------------------- */

#if DE_MEM_TRACKING
/* The tag a live block was allocated with. Reads only the caller's own block header, which no
 * other thread writes while the block is live. */
static u32 de__heap_block_tag(const void *ptr) {
    return de__heap_owns(ptr) ? de__tlsf_block_tag(ptr) : de__large_hdr_of(ptr)->tag;
}
#endif

static void *de__heap_alloc_raw(size_t size, size_t align, u32 tag) {
    if (de__is_large(size, align))
        return de__large_alloc(size, align, tag);
    if (!de__heap_ready())
        return NULL;
    de_mutex_lock(&de__heap.lock);
    void *p = de__tlsf_alloc(de__heap.tlsf, size, align, tag);
    if (p == NULL && de__heap_grow(size, align))
        p = de__tlsf_alloc(de__heap.tlsf, size, align, tag);
    de_mutex_unlock(&de__heap.lock);
    return p;
}

void *de__heap_alloc_fn(void *user, size_t size, size_t align, u32 tag) {
    (void)user;
    if (align == 0)
        align = 16;
    if (DE_UNLIKELY(!de__is_pow2(align))) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "heap: alignment %zu is not a power of two", align);
        return NULL;
    }
    void *p = de__heap_alloc_raw(size, align, tag);
    if (DE_UNLIKELY(p == NULL)) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "heap: alloc of %zu bytes (align %zu) failed", size, align);
        return NULL;
    }
#if DE_MEM_TRACKING
    de__mem_report_alloc(p, size, tag);
#endif
    return p;
}

static void de__heap_free_fn(void *user, void *ptr, size_t size) {
    (void)user;
    (void)size;
    if (ptr == NULL)
        return;
#if DE_MEM_TRACKING
    de__mem_report_free(ptr, size, de__heap_block_tag(ptr));
#endif
    if (de__heap_owns(ptr)) {
        de_mutex_lock(&de__heap.lock);
        de__tlsf_free(de__heap.tlsf, ptr);
        de_mutex_unlock(&de__heap.lock);
    } else {
        de__large_free(ptr);
    }
}

static void *de__heap_realloc_fn(void *user, void *ptr, size_t old_size, size_t new_size,
                                 size_t align, u32 tag) {
    if (ptr == NULL)
        return de__heap_alloc_fn(user, new_size, align, tag);
    if (align == 0)
        align = 16;
    if (DE_UNLIKELY(!de__is_pow2(align))) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "heap: alignment %zu is not a power of two", align);
        return NULL;
    }
    const bool small_old = de__heap_owns(ptr);
    const bool small_new = !de__is_large(new_size, align);
#if DE_MEM_TRACKING
    const u32 old_tag = de__heap_block_tag(ptr);
#endif
    bool in_place = false;
    size_t cap;
    if (small_old) {
        cap = de__tlsf_block_size(ptr);
        if (small_new) {
            de_mutex_lock(&de__heap.lock);
            in_place = de__tlsf_resize_in_place(de__heap.tlsf, ptr, new_size, align, tag);
            de_mutex_unlock(&de__heap.lock);
        }
    } else {
        de__large_hdr *h = de__large_hdr_of(ptr);
        cap = h->capacity;
        if (!small_new && new_size <= cap && ((uintptr_t)ptr & (align - 1)) == 0) {
            h->tag = tag;
            in_place = true;
        }
    }
    if (in_place) {
#if DE_MEM_TRACKING
        /* The block stays ours throughout, so reporting after the resize is safe. */
        de__mem_report_free(ptr, old_size, old_tag);
        de__mem_report_alloc(ptr, new_size, tag);
#endif
        return ptr;
    }
    void *q = de__heap_alloc_fn(user, new_size, align, tag);
    if (q == NULL)
        return NULL;
    size_t n = old_size < new_size ? old_size : new_size;
    memcpy(q, ptr, n < cap ? n : cap);
    de__heap_free_fn(user, ptr, old_size);
    return q;
}

static const de_allocator de__heap_allocator = {de__heap_alloc_fn, de__heap_realloc_fn,
                                                de__heap_free_fn, NULL};

const de_allocator *de_heap_allocator(void) {
    return &de__heap_allocator;
}

/* ---- Info -------------------------------------------------------------------------------- */

de_heap_info de_heap_get_info(void) {
    de_heap_info info;
    memset(&info, 0, sizeof info);
    if (de__heap_ready()) {
        de_mutex_lock(&de__heap.lock);
        info.reserved = de__heap.reserved;
        info.committed = de__heap.committed;
        info.pool_count = de__heap.pool_count;
        de_mutex_unlock(&de__heap.lock);
    }
    info.large_count = de_atomic_load_u64(&de__heap.large_count, DE_ATOMIC_RELAXED);
    info.large_bytes = de_atomic_load_u64(&de__heap.large_bytes, DE_ATOMIC_RELAXED);
    return info;
}

de_result de_heap_check(de_tlsf_info *out) {
    if (!de__heap_ready())
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "heap: could not be created");
    de_mutex_lock(&de__heap.lock);
    const de_result r = de_tlsf_check(de__heap.tlsf, out);
    de_mutex_unlock(&de__heap.lock);
    return r;
}
