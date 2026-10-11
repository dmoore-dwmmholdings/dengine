/*
 * de_alloc_guard.h: counts heap traffic in a region of code, for "no heap allocation in the
 * frame" tests (SPEC §5.3). Test-only: library target `de_test_support`, never installed.
 *
 *     de_alloc_guard g = {0};
 *     warm_up();                       // first-use allocations happen outside the region
 *     de_alloc_guard_begin(&g);
 *     run_one_frame();
 *     de_alloc_guard_end(&g);
 *     DE_ALLOC_GUARD_ASSERT_ZERO(&g);  // needs de_test.h; prints the offending call sites
 *
 * Link it with `de_add_test(NAME x SOURCES ... LIBS de_test_support)`.
 *
 * What is counted, between begin and end, on every thread (or only the thread that called
 * begin, with `this_thread_only`):
 *
 *   - Engine allocations: every block a reporting allocator hands out, through the allocation
 *     hooks (de_mem_set_hooks). That is de_heap_allocator() in DE_MEM_TRACKING builds (Debug and
 *     Dev), and any de_tracking_allocator. Release and Shipping heaps do not report, so there
 *     the guard sees only tracking allocators (see DE_ALLOC_GUARD_SEES_HEAP).
 *   - Raw allocations: calls of malloc, calloc, realloc, reallocf, valloc, aligned_alloc and
 *     posix_memalign. macOS: the calls of every image, through DYLD_INTERPOSE in the small
 *     library de_malloc_interpose. Linux: the calls made by the objects of the test executable
 *     (engine and test code; not the calls inside libc), through `ld --wrap`. Not counted on
 *     Windows, nor in macOS ASan or TSan builds, whose runtimes replace malloc themselves; see
 *     de_alloc_guard_counts_raw().
 *
 * Allocations (and reallocations) are offences unless their tag is allowed; frees are counted
 * but never offences. The first DE_ALLOC_GUARD_MAX_SITES offences keep a call stack.
 *
 * One guard can be active at a time in the process. The guard's hooks chain to the hooks that
 * were installed before begin, and end puts those back, so begin/end must nest correctly with
 * other de_mem_set_hooks users (Tracy, S06).
 *
 * Things that allocate on first use and so belong before begin: the first log call of a thread
 * (its buffer), the first scratch of a thread, the first de_name_from of a string (dev intern
 * table), and on macOS the first access to any thread-local variable of an image (dyld allocates
 * the thread's block of them).
 */
#ifndef DE_ALLOC_GUARD_H
#define DE_ALLOC_GUARD_H

#include <dengine/de_memory.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Offences that keep a call stack. */
#define DE_ALLOC_GUARD_MAX_SITES 8
/** Frames kept per call stack. */
#define DE_ALLOC_GUARD_MAX_FRAMES 16
/** Pseudo-tag for de_alloc_guard_allow: raw malloc-family calls are allowed. */
#define DE_ALLOC_GUARD_TAG_RAW 0xFFFFFFFFu

/** 1 when de_heap_allocator() reports to the hooks, so heap blocks are counted. */
#define DE_ALLOC_GUARD_SEES_HEAP DE_MEM_TRACKING

/** What kind of call an offence was. */
typedef enum de_alloc_guard_kind {
    DE_ALLOC_GUARD_ENGINE = 0, /**< a block from a reporting de_allocator */
    DE_ALLOC_GUARD_RAW = 1,    /**< a malloc-family call */
} de_alloc_guard_kind;

/** One recorded offence. */
typedef struct de_alloc_guard_site {
    de_alloc_guard_kind kind;
    u32 tag;         /**< memory tag (engine), DE_ALLOC_GUARD_TAG_RAW (raw) */
    u64 size;        /**< requested bytes */
    u32 frame_count; /**< valid entries in frames; 0 where no backtrace is available */
    void *caller;    /**< return address of the allocating call (raw), or NULL */
    void *frames[DE_ALLOC_GUARD_MAX_FRAMES]; /**< innermost first; the guard's own frames too */
} de_alloc_guard_site;

/**
 * A guard. Zero-initialise it; set the settings before begin; read the results after end.
 * Every field without a trailing underscore is public.
 */
typedef struct de_alloc_guard {
    /* Settings (read by de_alloc_guard_begin). */
    bool this_thread_only; /**< count only the thread that calls begin (default: all threads) */

    /* Results (written by de_alloc_guard_end). */
    u64 engine_allocs;  /**< engine allocations with a tag that is not allowed */
    u64 engine_bytes;   /**< their requested bytes */
    u64 engine_frees;   /**< engine frees (never offences) */
    u64 raw_mallocs;    /**< malloc-family calls that returned a block, unless raw is allowed */
    u64 raw_bytes;      /**< their requested bytes */
    u64 raw_frees;      /**< free() of a non-NULL pointer (never offences) */
    u64 allowed_allocs; /**< allocations, engine or raw, that the allowlist let through */
    u32 site_count;     /**< offences recorded in `sites` (at most DE_ALLOC_GUARD_MAX_SITES) */
    de_alloc_guard_site sites[DE_ALLOC_GUARD_MAX_SITES];

    /* Private. */
    u64 allow_mask_; /* bit t: tag t is allowed */
    bool allow_raw_;
    bool active_;
} de_alloc_guard;

/** True when this build counts raw malloc-family calls (see the file comment). */
bool de_alloc_guard_counts_raw(void);

/**
 * Allows allocations with memory tag `tag` (< DE_MEM_TAG_MAX), or raw malloc-family calls for
 * DE_ALLOC_GUARD_TAG_RAW: they are counted in `allowed_allocs`, not as offences. Nothing is
 * allowed by default. Call before begin or during the region. Other tags are ignored.
 */
void de_alloc_guard_allow(de_alloc_guard *g, u32 tag);

/**
 * Starts counting: clears the results, installs the guard's allocation hooks (chained to the
 * ones installed before) and the raw-call counter. Panics if another guard is active.
 */
void de_alloc_guard_begin(de_alloc_guard *g);

/**
 * Stops counting, restores the hooks installed before begin, and writes the results into `g`.
 * Allocations still running on other threads when end is called may be missed: stop or
 * synchronise with the threads of the region first. Panics if `g` is not the active guard, or
 * if the hooks were replaced during the region and not put back.
 */
void de_alloc_guard_end(de_alloc_guard *g);

/** Offences: engine_allocs + raw_mallocs. */
u64 de_alloc_guard_offences(const de_alloc_guard *g);

/**
 * When `g` (ended) has offences, prints the counts and every recorded site with its call stack
 * to stderr, prefixed with `file:line`, and returns false. Returns true (and prints nothing)
 * when there are none. Returns false for a guard that is still active.
 */
bool de_alloc_guard_report(const de_alloc_guard *g, const char *file, int line);

/**
 * Fails the current test (de_test.h FAIL) when the ended guard `g` has offences, after printing
 * them with de_alloc_guard_report. Include de_test.h before using it.
 */
#define DE_ALLOC_GUARD_ASSERT_ZERO(g)                                                              \
    do {                                                                                           \
        if (!de_alloc_guard_report((g), __FILE__, __LINE__))                                       \
            FAIL("alloc guard: heap allocation in a guarded region (sites above)");                \
    } while (0)

/* ---- Internal: the raw-call counter (tests/support/malloc_interpose.c) ---- */

/** Called after each counted malloc-family call: `is_free` 0 for an allocation, 1 for a free. */
typedef void (*de__raw_alloc_fn)(int is_free, size_t size, void *caller);
/** Installs `fn` (NULL: none). Defined only in builds where de_alloc_guard_counts_raw() is true. */
void de__raw_alloc_set_callback(de__raw_alloc_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* DE_ALLOC_GUARD_H */
