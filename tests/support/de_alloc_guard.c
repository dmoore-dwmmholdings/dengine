/*
 * de_alloc_guard.c: the alloc guard (de_alloc_guard.h).
 *
 * One process-wide state, because both sources are process-wide: the allocation hooks have one
 * slot, and the raw-call counter (malloc_interpose.c) has one callback. Counters are relaxed
 * atomics; `counting` is stored with release after the settings, and every callback loads it with
 * acquire before it reads them.
 *
 * Offence sites: a callback reserves a slot with a fetch_add, fills it, then sets the slot's
 * `ready` with release; end copies only ready slots. The callbacks never allocate: the stack is
 * taken with backtrace(), which on macOS walks frame pointers and on glibc loads its unwinder
 * once, so begin calls it once first.
 *
 * DE__GUARD_HAS_RAW (0 or 1) comes from tests/support/CMakeLists.txt: whether
 * malloc_interpose.c is part of this build.
 */
#include "de_alloc_guard.h"

#include "platform/de_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#if defined(__APPLE__) || defined(__GLIBC__)
#include <execinfo.h>
#define DE__GUARD_BACKTRACE 1
#else
#define DE__GUARD_BACKTRACE 0
#endif

#ifndef DE__GUARD_HAS_RAW
#define DE__GUARD_HAS_RAW 0
#endif

DE_STATIC_ASSERT(DE_MEM_TAG_MAX <= 64, "the allow mask is one u64");

typedef struct de__guard_slot {
    de_atomic_u32 ready;
    de_alloc_guard_site site;
} de__guard_slot;

static struct {
    de_atomic_u32 active;   /* a guard is between begin and end */
    de_atomic_u32 counting; /* callbacks count */
    de_atomic_u64 engine_allocs, engine_bytes, engine_frees;
    de_atomic_u64 raw_mallocs, raw_bytes, raw_frees;
    de_atomic_u64 allowed;
    de_atomic_u64 allow_mask;
    de_atomic_u32 allow_raw;
    de_atomic_u32 site_next;
    de__guard_slot sites[DE_ALLOC_GUARD_MAX_SITES];
    /* Written by begin before `counting` is released; read-only while counting. */
    bool this_thread_only;
    uintptr_t owner;
    de_alloc_guard *guard;
    /* The hooks installed before begin. Atomic: a hook may run on another thread while begin
     * or end changes it (it then skips the chain call once). */
    de_atomic_ptr prev;
    de_mem_hooks hooks;
} de__guard;

/* Never allocates (pthread_self, GetCurrentThreadId), so it is safe inside malloc. */
static uintptr_t de__guard_thread_self(void) {
#if defined(_WIN32)
    return (uintptr_t)GetCurrentThreadId();
#else
    return (uintptr_t)pthread_self();
#endif
}

static bool de__guard_counts_here(void) {
    if (!de_atomic_load_u32(&de__guard.counting, DE_ATOMIC_ACQUIRE))
        return false;
    return !de__guard.this_thread_only || de__guard_thread_self() == de__guard.owner;
}

static void de__guard_record(de_alloc_guard_kind kind, u32 tag, size_t size, void *caller) {
    const u32 i = de_atomic_fetch_add_u32(&de__guard.site_next, 1, DE_ATOMIC_RELAXED);
    if (i >= DE_ALLOC_GUARD_MAX_SITES)
        return;
    de__guard_slot *slot = &de__guard.sites[i];
    de_alloc_guard_site *s = &slot->site;
    s->kind = kind;
    s->tag = tag;
    s->size = (u64)size;
    s->caller = caller;
#if DE__GUARD_BACKTRACE
    const int n = backtrace(s->frames, DE_ALLOC_GUARD_MAX_FRAMES);
    s->frame_count = n > 0 ? (u32)n : 0u;
#else
    s->frame_count = 0;
#endif
    de_atomic_store_u32(&slot->ready, 1, DE_ATOMIC_RELEASE);
}

/* Static memory for de__guard_warm_report (below); never passed on to earlier hooks. */
static DE_ALIGN(16) u8 de__guard_warm_block[16];

/* ---- Engine allocations: the de_mem_hooks ------------------------------------------------- */

static void de__guard_on_alloc(void *user, void *ptr, size_t size, u32 tag) {
    (void)user;
    if (de__guard_counts_here()) {
        const u64 mask = de_atomic_load_u64(&de__guard.allow_mask, DE_ATOMIC_RELAXED);
        if (tag < 64 && ((mask >> tag) & 1u) != 0) {
            de_atomic_fetch_add_u64(&de__guard.allowed, 1, DE_ATOMIC_RELAXED);
        } else {
            de_atomic_fetch_add_u64(&de__guard.engine_allocs, 1, DE_ATOMIC_RELAXED);
            de_atomic_fetch_add_u64(&de__guard.engine_bytes, (u64)size, DE_ATOMIC_RELAXED);
            de__guard_record(DE_ALLOC_GUARD_ENGINE, tag, size, NULL);
        }
    }
    const de_mem_hooks *prev =
        (const de_mem_hooks *)de_atomic_load_ptr(&de__guard.prev, DE_ATOMIC_ACQUIRE);
    if (prev != NULL && prev->on_alloc != NULL && ptr != (void *)de__guard_warm_block)
        prev->on_alloc(prev->user, ptr, size, tag);
}

static void de__guard_on_free(void *user, void *ptr, size_t size, u32 tag) {
    (void)user;
    if (de__guard_counts_here())
        de_atomic_fetch_add_u64(&de__guard.engine_frees, 1, DE_ATOMIC_RELAXED);
    const de_mem_hooks *prev =
        (const de_mem_hooks *)de_atomic_load_ptr(&de__guard.prev, DE_ATOMIC_ACQUIRE);
    if (prev != NULL && prev->on_free != NULL && ptr != (void *)de__guard_warm_block)
        prev->on_free(prev->user, ptr, size, tag);
}

/* ---- Raw allocations: malloc_interpose.c -------------------------------------------------- */

#if DE__GUARD_HAS_RAW
static void de__guard_on_raw(int is_free, size_t size, void *caller) {
    if (!de__guard_counts_here())
        return;
    if (is_free) {
        de_atomic_fetch_add_u64(&de__guard.raw_frees, 1, DE_ATOMIC_RELAXED);
    } else if (de_atomic_load_u32(&de__guard.allow_raw, DE_ATOMIC_RELAXED)) {
        de_atomic_fetch_add_u64(&de__guard.allowed, 1, DE_ATOMIC_RELAXED);
    } else {
        de_atomic_fetch_add_u64(&de__guard.raw_mallocs, 1, DE_ATOMIC_RELAXED);
        de_atomic_fetch_add_u64(&de__guard.raw_bytes, (u64)size, DE_ATOMIC_RELAXED);
        de__guard_record(DE_ALLOC_GUARD_RAW, DE_ALLOC_GUARD_TAG_RAW, size, caller);
    }
}
#endif

/* ---- Warm-up ------------------------------------------------------------------------------ */

/* A one-block allocator over static memory that does not report, under a tracking allocator
 * that does. One alloc and free through it, with the guard's hooks installed but not counting,
 * runs the hook path once on this thread: its first touch of the engine's thread-local variables
 * (on macOS dyld allocates them, with malloc, on first use) then happens outside the region. */

static void *de__guard_warm_alloc(void *user, size_t size, size_t align, u32 tag) {
    (void)user, (void)align, (void)tag;
    return size <= sizeof de__guard_warm_block ? de__guard_warm_block : NULL;
}

static void *de__guard_warm_realloc(void *user, void *ptr, size_t old_size, size_t new_size,
                                    size_t align, u32 tag) {
    (void)ptr, (void)old_size;
    return de__guard_warm_alloc(user, new_size, align, tag);
}

static void de__guard_warm_free(void *user, void *ptr, size_t size) {
    (void)user, (void)ptr, (void)size;
}

static void de__guard_warm_report(void) {
    const de_allocator backing = {de__guard_warm_alloc, de__guard_warm_realloc, de__guard_warm_free,
                                  NULL};
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, &backing, DE_MEM_GENERAL);
    void *p = DE_ALLOC(&a, 8, 8, DE_MEM_GENERAL);
    DE_FREE(&a, p, 8);
}

/* ---- API ---------------------------------------------------------------------------------- */

bool de_alloc_guard_counts_raw(void) {
    return DE__GUARD_HAS_RAW != 0;
}

void de_alloc_guard_allow(de_alloc_guard *g, u32 tag) {
    DE_VERIFY(g != NULL);
    if (tag == DE_ALLOC_GUARD_TAG_RAW) {
        g->allow_raw_ = true;
        if (g->active_)
            de_atomic_store_u32(&de__guard.allow_raw, 1, DE_ATOMIC_RELAXED);
    } else if (tag < DE_MEM_TAG_MAX) {
        g->allow_mask_ |= (u64)1 << tag;
        if (g->active_)
            de_atomic_fetch_or_u64(&de__guard.allow_mask, (u64)1 << tag, DE_ATOMIC_RELAXED);
    }
}

void de_alloc_guard_begin(de_alloc_guard *g) {
    DE_VERIFY(g != NULL);
    u32 expected = 0;
    if (!de_atomic_cas_u32(&de__guard.active, &expected, 1, DE_ATOMIC_ACQUIRE, DE_ATOMIC_RELAXED))
        DE_PANIC("de_alloc_guard_begin: another alloc guard is active");

    g->engine_allocs = g->engine_bytes = g->engine_frees = 0;
    g->raw_mallocs = g->raw_bytes = g->raw_frees = 0;
    g->allowed_allocs = 0;
    g->site_count = 0;
    memset(g->sites, 0, sizeof g->sites);

    de_atomic_store_u64(&de__guard.engine_allocs, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&de__guard.engine_bytes, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&de__guard.engine_frees, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&de__guard.raw_mallocs, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&de__guard.raw_bytes, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&de__guard.raw_frees, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&de__guard.allowed, 0, DE_ATOMIC_RELAXED);
    de_atomic_store_u64(&de__guard.allow_mask, g->allow_mask_, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&de__guard.allow_raw, g->allow_raw_ ? 1u : 0u, DE_ATOMIC_RELAXED);
    de_atomic_store_u32(&de__guard.site_next, 0, DE_ATOMIC_RELAXED);
    for (u32 i = 0; i < DE_ALLOC_GUARD_MAX_SITES; ++i)
        de_atomic_store_u32(&de__guard.sites[i].ready, 0, DE_ATOMIC_RELAXED);
    de__guard.this_thread_only = g->this_thread_only;
    de__guard.owner = de__guard_thread_self();
    de__guard.guard = g;
    g->active_ = true;

#if DE__GUARD_BACKTRACE
    /* glibc loads its unwinder (and allocates) on the first call: do that outside the region. */
    void *warm[2];
    (void)backtrace(warm, 2);
#endif

    de__guard.hooks.on_alloc = de__guard_on_alloc;
    de__guard.hooks.on_free = de__guard_on_free;
    de__guard.hooks.user = NULL;
    const de_mem_hooks *prev = de_mem_set_hooks(&de__guard.hooks);
    de_atomic_store_ptr(&de__guard.prev, (void *)(uintptr_t)prev, DE_ATOMIC_RELEASE);
    de__guard_warm_report();
#if DE__GUARD_HAS_RAW
    de__raw_alloc_set_callback(de__guard_on_raw);
#endif
    de_atomic_store_u32(&de__guard.counting, 1, DE_ATOMIC_RELEASE);
}

void de_alloc_guard_end(de_alloc_guard *g) {
    DE_VERIFY(g != NULL);
    if (!g->active_ || de__guard.guard != g)
        DE_PANIC("de_alloc_guard_end: this guard is not active");
    de_atomic_store_u32(&de__guard.counting, 0, DE_ATOMIC_SEQ_CST);
#if DE__GUARD_HAS_RAW
    de__raw_alloc_set_callback(NULL);
#endif
    const de_mem_hooks *prev =
        (const de_mem_hooks *)de_atomic_load_ptr(&de__guard.prev, DE_ATOMIC_RELAXED);
    const de_mem_hooks *cur = de_mem_set_hooks(prev);
    if (cur != &de__guard.hooks)
        DE_PANIC("de_alloc_guard_end: the allocation hooks were replaced during the region");

    g->engine_allocs = de_atomic_load_u64(&de__guard.engine_allocs, DE_ATOMIC_RELAXED);
    g->engine_bytes = de_atomic_load_u64(&de__guard.engine_bytes, DE_ATOMIC_RELAXED);
    g->engine_frees = de_atomic_load_u64(&de__guard.engine_frees, DE_ATOMIC_RELAXED);
    g->raw_mallocs = de_atomic_load_u64(&de__guard.raw_mallocs, DE_ATOMIC_RELAXED);
    g->raw_bytes = de_atomic_load_u64(&de__guard.raw_bytes, DE_ATOMIC_RELAXED);
    g->raw_frees = de_atomic_load_u64(&de__guard.raw_frees, DE_ATOMIC_RELAXED);
    g->allowed_allocs = de_atomic_load_u64(&de__guard.allowed, DE_ATOMIC_RELAXED);
    u32 n = 0;
    for (u32 i = 0; i < DE_ALLOC_GUARD_MAX_SITES; ++i) {
        if (de_atomic_load_u32(&de__guard.sites[i].ready, DE_ATOMIC_ACQUIRE))
            g->sites[n++] = de__guard.sites[i].site;
    }
    g->site_count = n;

    g->active_ = false;
    de__guard.guard = NULL;
    de_atomic_store_ptr(&de__guard.prev, NULL, DE_ATOMIC_RELEASE);
    de_atomic_store_u32(&de__guard.active, 0, DE_ATOMIC_RELEASE);
}

u64 de_alloc_guard_offences(const de_alloc_guard *g) {
    return g->engine_allocs + g->raw_mallocs;
}

bool de_alloc_guard_report(const de_alloc_guard *g, const char *file, int line) {
    if (g == NULL || file == NULL)
        return false;
    if (g->active_) {
        fprintf(stderr, "%s:%d: alloc guard: still active; call de_alloc_guard_end first\n", file,
                line);
        return false;
    }
    if (de_alloc_guard_offences(g) == 0)
        return true;
    fprintf(stderr,
            "%s:%d: alloc guard: %llu engine allocations (%llu bytes), %llu raw allocations "
            "(%llu bytes); %llu engine frees, %llu raw frees, %llu allowed%s\n",
            file, line, (unsigned long long)g->engine_allocs, (unsigned long long)g->engine_bytes,
            (unsigned long long)g->raw_mallocs, (unsigned long long)g->raw_bytes,
            (unsigned long long)g->engine_frees, (unsigned long long)g->raw_frees,
            (unsigned long long)g->allowed_allocs,
            de_alloc_guard_counts_raw() ? "" : " (raw calls not counted in this build)");
    for (u32 i = 0; i < g->site_count; ++i) {
        const de_alloc_guard_site *s = &g->sites[i];
        if (s->kind == DE_ALLOC_GUARD_RAW) {
            fprintf(stderr, "  site %u: raw malloc-family call, %llu bytes, caller %p\n", i + 1,
                    (unsigned long long)s->size, s->caller);
        } else {
            const char *tag = de_mem_tag_name(s->tag);
            fprintf(stderr, "  site %u: engine allocation, %llu bytes, tag %u (%s)\n", i + 1,
                    (unsigned long long)s->size, s->tag, tag != NULL ? tag : "?");
        }
        fflush(stderr);
#if DE__GUARD_BACKTRACE
        if (s->frame_count > 0)
            backtrace_symbols_fd(s->frames, (int)s->frame_count, 2);
#endif
    }
    const u64 offences = de_alloc_guard_offences(g);
    if (offences > g->site_count)
        fprintf(stderr, "  (%llu more without a recorded site)\n",
                (unsigned long long)(offences - g->site_count));
    fflush(stderr);
    return false;
}
