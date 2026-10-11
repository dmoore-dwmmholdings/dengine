/* arena.c: the virtual-memory arena, the arena allocator and the null allocator (SPEC §5.3).
 *
 * Layout of one arena's reservation:
 *
 *     base                start                pos           committed        reserved
 *     | de_arena header    | pushed blocks ...  |  committed,   |  reserved,      |
 *     | (first page)       |                    |  free         |  PROT_NONE      |
 *
 * The header lives in the reservation itself, so the arena never touches the heap. Positions
 * (`pos`, marks) are byte offsets from `base`. `dirty` is the highest position written since
 * those pages were last committed fresh: memory between `dirty` and `committed` still reads as
 * zero, so de_arena_push_zero only clears what may be dirty.
 */
#include <dengine/de_memory.h>

#include "core/de_compiler.h"
#include "core/vmem.h"
#include "platform/de_atomic.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DE__KIB ((size_t)1 << 10)
#define DE__GIB ((size_t)1 << 30)

/* The defaults of de_arena_desc. 64 GiB of address space per arena is cheap on 64-bit hosts
 * (macOS arm64 and x86-64 Windows/Linux give user space 128 TiB). */
#define DE__ARENA_DEFAULT_RESERVE ((size_t)64 * DE__GIB)
#define DE__ARENA_DEFAULT_COMMIT  ((size_t)64 * DE__KIB)
/* Reservations larger than this are halved on failure down to this size. */
#define DE__ARENA_MIN_FALLBACK  DE__GIB
#define DE__ARENA_DEFAULT_ALIGN 16u
#define DE__ARENA_NAME_MAX      32

DE_STATIC_ASSERT(sizeof(void *) == 8, "the virtual arena assumes a 64-bit address space");

struct de_arena {
    u8 *base;
    size_t pos;       /* next free byte, offset from base */
    size_t start;     /* first byte after the header; de_arena_reset goes here */
    size_t committed; /* [base, base + committed) is readable and writable */
    size_t reserved;  /* size of the reservation */
    size_t dirty;     /* highest pos since the pages above it were committed fresh */
    size_t peak;      /* highest pos since creation */
    size_t commit_step;
    size_t decommit_threshold;
    u32 tag;
    char name[DE__ARENA_NAME_MAX];
};

static size_t de__align_up(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

static bool de__is_pow2(size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
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

/* ---- Creation ---------------------------------------------------------------------------- */

/* Logs the first reservation fallback of the process (DENG-150's logger is not available to
 * core yet, so this goes to stderr). */
static void de__log_fallback_once(size_t wanted, size_t got) {
    static de_atomic_u32 logged = DE_ATOMIC_INIT(0);
    if (de_atomic_exchange_u32(&logged, 1, DE_ATOMIC_RELAXED) != 0)
        return;
    fprintf(stderr,
            "dengine: arena reservation of %zu MiB failed; using %zu MiB (logged once per "
            "process)\n",
            wanted >> 20, got >> 20);
}

de_result de_arena_create(const de_arena_desc *desc, de_arena **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_arena_create: out is NULL");
    *out = NULL;
    de_arena_desc d = {0};
    if (desc != NULL)
        d = *desc;

    const size_t page = de__vmem_page_size();
    const size_t gran = de__vmem_reserve_granularity();
    size_t step = d.commit_step ? d.commit_step : DE__ARENA_DEFAULT_COMMIT;
    size_t want = d.reserve_bytes ? d.reserve_bytes : DE__ARENA_DEFAULT_RESERVE;
    if (step > SIZE_MAX / 2 || want > SIZE_MAX / 2)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_arena_create: size out of range");
    step = de__align_up(step, page);
    want = de__align_up(want, gran);
    /* The header needs at least its first page committed. */
    const size_t header = de__align_up(sizeof(de_arena), 64);
    if (want < step)
        want = de__align_up(step, gran);

    size_t size = want;
    u8 *base = (u8 *)de__vmem_reserve(size);
    while (base == NULL && size > DE__ARENA_MIN_FALLBACK) {
        size = de__align_up(size / 2, gran);
        if (size < DE__ARENA_MIN_FALLBACK)
            size = DE__ARENA_MIN_FALLBACK;
        base = (u8 *)de__vmem_reserve(size);
    }
    if (base == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_arena_create(%s): cannot reserve %zu bytes",
                       d.name ? d.name : "arena", want);
    if (size != want)
        de__log_fallback_once(want, size);

    const size_t first = step < size ? step : size;
    if (!de__vmem_commit(base, first)) {
        de__vmem_release(base, size);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_arena_create(%s): cannot commit %zu bytes",
                       d.name ? d.name : "arena", first);
    }

    de_arena *a = (de_arena *)base;
    a->base = base;
    a->start = header;
    a->pos = header;
    a->committed = first;
    a->reserved = size;
    a->dirty = header;
    a->peak = header;
    a->commit_step = step;
    a->decommit_threshold = d.decommit_threshold;
    a->tag = d.tag;
    const char *name = d.name ? d.name : "arena";
    size_t n = strlen(name);
    if (n >= DE__ARENA_NAME_MAX)
        n = DE__ARENA_NAME_MAX - 1;
    memcpy(a->name, name, n);
    a->name[n] = '\0';
    *out = a;
    return DE_OK;
}

void de_arena_destroy(de_arena *a) {
    if (a == NULL)
        return;
    /* Copy out first: the header is inside the range being released. */
    u8 *base = a->base;
    size_t reserved = a->reserved;
    de__vmem_release(base, reserved);
}

/* ---- Push ---------------------------------------------------------------------------------- */

/* Makes sure [base, base + end) is committed. Commits whole steps, clamped to the reservation. */
static bool de__arena_commit_to(de_arena *a, size_t end) {
    if (end <= a->committed)
        return true;
    size_t target = de__align_up(end, a->commit_step);
    if (target > a->reserved || target < end)
        target = a->reserved;
    if (!de__vmem_commit(a->base + a->committed, target - a->committed))
        return false;
    a->committed = target;
    return true;
}

/* The push without the debug fill. */
static void *de__arena_push_raw(de_arena *a, size_t size, size_t align) {
    DE_ASSERT(a != NULL);
    if (align == 0)
        align = DE__ARENA_DEFAULT_ALIGN;
    if (DE_UNLIKELY(!de__is_pow2(align))) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "arena %s: alignment %zu is not a power of two", a->name,
                align);
        return NULL;
    }
    /* Align the address, not the offset: base is only page-aligned and align may exceed that. */
    const uintptr_t base = (uintptr_t)a->base;
    const uintptr_t cur = base + a->pos;
    const uintptr_t aligned = (cur + (align - 1)) & ~(uintptr_t)(align - 1);
    const size_t off = (size_t)(aligned - base);
    if (DE_UNLIKELY(aligned < cur || off > a->reserved || size > a->reserved - off)) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                "arena %s: push of %zu bytes (align %zu) exceeds the %zu-byte reservation "
                "(used %zu)",
                a->name, size, align, a->reserved, a->pos);
        return NULL;
    }
    const size_t end = off + size;
    if (end > a->committed && DE_UNLIKELY(!de__arena_commit_to(a, end))) {
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "arena %s: the OS refused to commit %zu bytes", a->name,
                end - a->committed);
        return NULL;
    }
    a->pos = end;
    if (end > a->peak)
        a->peak = end;
    if (end > a->dirty)
        a->dirty = end;
    return a->base + off;
}

void *de_arena_push(de_arena *a, size_t size, size_t align) {
    void *p = de__arena_push_raw(a, size, align);
    if (p != NULL)
        de__debug_fill(p, 0xCD, size);
    return p;
}

void *de_arena_push_zero(de_arena *a, size_t size, size_t align) {
    /* Read dirty before the push raises it: bytes at or above it were never handed out since
     * they were committed, so they are still zero. */
    const size_t dirty = a->dirty;
    u8 *p = (u8 *)de__arena_push_raw(a, size, align);
    if (p != NULL) {
        const size_t off = (size_t)(p - a->base);
        if (off < dirty)
            memset(p, 0, (dirty - off) < size ? (dirty - off) : size);
    }
    return p;
}

/* ---- Marks and reset ----------------------------------------------------------------------- */

de_arena_mark de_arena_mark_get(const de_arena *a) {
    de_arena_mark m = {a->pos};
    return m;
}

void de_arena_reset_to(de_arena *a, de_arena_mark m) {
    DE_ASSERT(a != NULL);
    DE_ASSERT(m.pos >= a->start && m.pos <= a->pos);
    size_t pos = m.pos;
    if (pos < a->start)
        pos = a->start;
    if (pos > a->pos)
        return; /* a mark from above the current position: nothing to pop */
    de__debug_fill(a->base + pos, 0xDD, a->pos - pos);
    a->pos = pos;

    if (a->decommit_threshold != 0) {
        size_t keep = pos > a->decommit_threshold ? pos : a->decommit_threshold;
        keep = de__align_up(keep, a->commit_step);
        if (keep < a->committed) {
            de__vmem_decommit(a->base + keep, a->committed - keep);
            a->committed = keep;
            if (a->dirty > keep)
                a->dirty = keep;
        }
    }
}

void de_arena_reset(de_arena *a) {
    de_arena_mark m = {a->start};
    de_arena_reset_to(a, m);
}

de_arena_info de_arena_get_info(const de_arena *a) {
    de_arena_info info;
    info.used = a->pos;
    info.peak = a->peak;
    info.committed = a->committed;
    info.reserved = a->reserved;
    info.tag = a->tag;
    info.name = a->name;
    return info;
}

/* ---- Arena allocator ----------------------------------------------------------------------- */

static void *de__arena_alloc_fn(void *user, size_t size, size_t align, u32 tag) {
    (void)tag;
    return de_arena_push((de_arena *)user, size, align);
}

static void *de__arena_realloc_fn(void *user, void *ptr, size_t old_size, size_t new_size,
                                  size_t align, u32 tag) {
    (void)tag;
    de_arena *a = (de_arena *)user;
    if (ptr == NULL)
        return de_arena_push(a, new_size, align);
    if (align == 0)
        align = DE__ARENA_DEFAULT_ALIGN;

    u8 *p = (u8 *)ptr;
    const size_t off = (size_t)(p - a->base);
    const bool is_last = off <= a->pos && a->pos - off == old_size;
    if (is_last && ((uintptr_t)p & (align - 1)) == 0) {
        /* The most recent block: move the end in place. */
        if (new_size > a->reserved - off) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                    "arena %s: realloc to %zu bytes exceeds the %zu-byte reservation", a->name,
                    new_size, a->reserved);
            return NULL;
        }
        const size_t end = off + new_size;
        if (end > a->committed && !de__arena_commit_to(a, end)) {
            DE_FAIL(DE_ERR_OUT_OF_MEMORY, "arena %s: the OS refused to commit %zu bytes", a->name,
                    end - a->committed);
            return NULL;
        }
        if (new_size > old_size)
            de__debug_fill(p + old_size, 0xCD, new_size - old_size);
        else
            de__debug_fill(p + new_size, 0xDD, old_size - new_size);
        a->pos = end;
        if (end > a->peak)
            a->peak = end;
        if (end > a->dirty)
            a->dirty = end;
        return p;
    }
    if (new_size <= old_size && ((uintptr_t)p & (align - 1)) == 0)
        return p; /* shrinking an older block: keep it where it is */
    void *q = de_arena_push(a, new_size, align);
    if (q != NULL)
        memcpy(q, p, old_size < new_size ? old_size : new_size);
    return q;
}

static void de__arena_free_fn(void *user, void *ptr, size_t size) {
    (void)user;
    (void)ptr;
    (void)size;
}

de_allocator de_arena_allocator(de_arena *a) {
    de_allocator al;
    al.alloc = de__arena_alloc_fn;
    al.realloc = de__arena_realloc_fn;
    al.free = de__arena_free_fn;
    al.user = a;
    return al;
}

/* ---- Null allocator ------------------------------------------------------------------------ */

static void *de__null_alloc_fn(void *user, size_t size, size_t align, u32 tag) {
    (void)user;
    (void)align;
    (void)tag;
    DE_FAIL(DE_ERR_OUT_OF_MEMORY, "null allocator: alloc of %zu bytes", size);
    return NULL;
}

static void *de__null_realloc_fn(void *user, void *ptr, size_t old_size, size_t new_size,
                                 size_t align, u32 tag) {
    (void)user;
    (void)ptr;
    (void)old_size;
    (void)align;
    (void)tag;
    DE_FAIL(DE_ERR_OUT_OF_MEMORY, "null allocator: realloc to %zu bytes", new_size);
    return NULL;
}

static void de__null_free_fn(void *user, void *ptr, size_t size) {
    (void)user;
    (void)ptr;
    (void)size;
}

static const de_allocator de__null_allocator = {de__null_alloc_fn, de__null_realloc_fn,
                                                de__null_free_fn, NULL};

const de_allocator *de_allocator_null(void) {
    return &de__null_allocator;
}
