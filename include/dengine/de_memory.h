/* de_memory.h — allocator interface and engine allocators. SPEC.md §5.3. */
#ifndef DE_MEMORY_H
#define DE_MEMORY_H
#include "de_base.h"
DE_EXTERN_C_BEGIN

/**
 * Allocator interface (SPEC §5.3). Every subsystem's create function takes one; nothing in the
 * engine calls malloc directly.
 *
 * - `alloc` returns `size` bytes aligned to `align` (a power of two), or NULL on failure.
 * - `realloc` resizes a block from this allocator. `ptr == NULL` behaves like `alloc`. On failure
 *   it returns NULL and the old block is unchanged.
 * - `free` releases a block; `size` is the size it was allocated (or last resized) with.
 * - `tag` is a memory tag (DE_MEM_*) for tracking; allocators that do not track ignore it.
 *
 * Allocators that fail set the calling thread's error context (DE_ERR_OUT_OF_MEMORY).
 */
typedef struct de_allocator {
    void *(*alloc)(void *user, size_t size, size_t align, u32 tag);
    void *(*realloc)(void *user, void *ptr, size_t old_size, size_t new_size, size_t align,
                     u32 tag);
    void (*free)(void *user, void *ptr, size_t size);
    void *user;
} de_allocator;

#define DE_ALLOC(a, size, align, tag) ((a)->alloc((a)->user, (size), (align), (tag)))
#define DE_REALLOC(a, ptr, old_size, new_size, align, tag)                                         \
    ((a)->realloc((a)->user, (ptr), (old_size), (new_size), (align), (tag)))
#define DE_FREE(a, ptr, size) ((a)->free((a)->user, (ptr), (size)))
#define DE_NEW(a, T, tag)     ((T *)DE_ALLOC((a), sizeof(T), DE_ALIGNOF(T), (tag)))

/* Memory tags for tracking (Debug/Dev). Modules register more with de_mem_register_tag. */
enum {
    DE_MEM_GENERAL = 0,
    DE_MEM_ECS,
    DE_MEM_ASSET,
    DE_MEM_RENDER,
    DE_MEM_PHYSICS,
    DE_MEM_AUDIO,
    DE_MEM_NET,
    DE_MEM_UI,
    DE_MEM_SCRATCH,
    DE_MEM_TAG_BUILTIN_COUNT_
};
DE_API u32 de_mem_register_tag(const char *name);

DE_API const de_allocator *de_heap_allocator(void); /* TLSF default heap, thread-safe */

/**
 * An allocator whose `alloc` and `realloc` always fail: they return NULL and set
 * DE_ERR_OUT_OF_MEMORY. `free` does nothing. For tests of out-of-memory paths. Never NULL.
 */
DE_API const de_allocator *de_allocator_null(void);

/* ---------- Virtual arena ---------- */

/**
 * A virtual-memory arena: one contiguous address range reserved up front, committed in
 * `commit_step` pieces as pushes need it, and reset in O(1). Pointers never move, so a pushed
 * block stays valid until a reset below it. An arena is not thread-safe.
 *
 * The arena's own bookkeeping lives at the start of its reservation, so creating, pushing,
 * resetting and destroying an arena never calls malloc.
 *
 * In Debug builds (DE_DEBUG) pushed memory is filled with 0xCD and memory given back by a reset
 * with 0xDD, so use of uninitialised or stale arena memory shows up.
 */
typedef struct de_arena de_arena;

/** Arena settings. `(de_arena_desc){0}` is valid and gives the defaults. */
typedef struct de_arena_desc {
    /** Address space to reserve (default 64 GiB). If the OS refuses a reservation larger than
     * 1 GiB, it is halved until it succeeds or reaches 1 GiB; that is logged once per process. */
    size_t reserve_bytes;
    /** Commit granularity (default 64 KiB), rounded up to the OS page size. */
    size_t commit_step;
    /** When non-zero, a reset gives committed memory above max(new position, this many bytes)
     * back to the OS. 0 (default): keep it committed, so a reset never makes a system call. */
    size_t decommit_threshold;
    u32 tag;          /**< memory tag (DE_MEM_*), reported by de_arena_get_info */
    const char *name; /**< debug name, copied (first 31 bytes); NULL gives "arena" */
} de_arena_desc;

/** A position in an arena, from de_arena_mark_get, for de_arena_reset_to. */
typedef struct de_arena_mark {
    size_t pos;
} de_arena_mark;

/** What an arena is using. All sizes in bytes. */
typedef struct de_arena_info {
    size_t used;      /**< pushed bytes, including alignment padding and the arena header */
    size_t peak;      /**< largest `used` since creation */
    size_t committed; /**< committed (readable, writable) bytes */
    size_t reserved;  /**< reserved address space; the most `used` can reach */
    u32 tag;
    const char *name; /**< valid while the arena exists */
} de_arena_info;

/** Creates an arena. Fails with DE_ERR_OUT_OF_MEMORY if no address space can be reserved. */
DE_API de_result de_arena_create(const de_arena_desc *desc, de_arena **out);
/** Releases the arena's whole address range. NULL is ignored. */
DE_API void de_arena_destroy(de_arena *a);
/**
 * Pushes `size` bytes aligned to `align` (a power of two; 0 means 16). Returns NULL and sets
 * DE_ERR_OUT_OF_MEMORY when the reservation is exhausted or the OS refuses to commit, and
 * DE_ERR_INVALID_ARGUMENT when `align` is not a power of two. A failed push changes nothing.
 */
DE_API void *de_arena_push(de_arena *a, size_t size, size_t align);
/** Like de_arena_push, and the memory is zeroed. */
DE_API void *de_arena_push_zero(de_arena *a, size_t size, size_t align);
/** The current position, for a later de_arena_reset_to. */
DE_API de_arena_mark de_arena_mark_get(const de_arena *a);
/**
 * Pops everything pushed after mark `m`. O(1) unless `decommit_threshold` asks for memory to go
 * back to the OS. `m` must not be above the current position (Debug/Dev assert).
 */
DE_API void de_arena_reset_to(de_arena *a, de_arena_mark m);
/** Pops everything. Same cost as de_arena_reset_to. */
DE_API void de_arena_reset(de_arena *a);
/** Usage numbers of the arena. */
DE_API de_arena_info de_arena_get_info(const de_arena *a);
/**
 * An allocator that pushes onto `a`. `free` is a no-op. `realloc` of the most recent block grows
 * or shrinks it in place (same pointer); any other block is copied to a new push. The tag is
 * ignored. The allocator is valid while `a` exists.
 */
DE_API de_allocator de_arena_allocator(de_arena *a);
#define DE_ARENA_PUSH(a, T, n) ((T *)de_arena_push((a), sizeof(T) * (n), DE_ALIGNOF(T)))

/* ---------- Frame arenas (one per in-flight frame) ---------- */

/** Default and maximum number of frame arenas. */
#define DE_FRAME_ARENA_DEFAULT_COUNT 2
#define DE_FRAME_ARENA_MAX_COUNT     4

/**
 * Creates `count` frame arenas (0 means DE_FRAME_ARENA_DEFAULT_COUNT, at most
 * DE_FRAME_ARENA_MAX_COUNT), one per frame in flight. The app host calls this at startup (S06);
 * games do not. DE_ERR_ALREADY_EXISTS if they exist, DE_ERR_INVALID_ARGUMENT if `count` is too
 * large. Not thread-safe against the other frame-arena calls.
 */
DE_API de_result de_frame_arenas_init(u32 count);
/** Destroys the frame arenas. de_frame_arena() returns NULL afterwards. */
DE_API void de_frame_arenas_shutdown(void);
/**
 * Starts frame `frame_index`: resets arena `frame_index % count` and makes it current. The app
 * host calls this at frame start, when no other thread uses that arena any more.
 */
DE_API void de_frame_arena_begin(u64 frame_index);
/**
 * The current frame's arena, reset at frame start; NULL before de_frame_arena_begin. Reading it
 * is a single atomic load and safe from any thread. Pushing is not thread-safe: per-frame data
 * pushed from worker threads needs its own arena (thread scratch, or the S04 task graph's).
 */
DE_API de_arena *de_frame_arena(void);

/* ---------- Thread scratch ---------- */

/**
 * Per-thread temporary memory: each thread has two arenas, created on first use and released
 * when the thread exits. de_scratch_begin picks one that is not in `conflicts` and records its
 * position; de_scratch_end pops back to it. Scopes nest (end them in reverse order).
 *
 * Pass arenas already in use as `conflicts` (for example the arena a caller gave you for the
 * result) so nested scratch never aliases them. A scratch must end on the thread that began it.
 * Panics if both of the thread's arenas are in `conflicts`, or if the arenas cannot be created.
 */
typedef struct de_scratch {
    de_arena *arena;
    de_arena_mark mark;
} de_scratch;
DE_API de_scratch de_scratch_begin(de_arena *const *conflicts, u32 conflict_count);
DE_API void de_scratch_end(de_scratch s);

/* ---------- Pool (fixed-size blocks) ---------- */

/**
 * A pool of fixed-size blocks (ECS chunks, physics bodies, audio voices). Blocks come from pages
 * allocated from the backing allocator; a page is never given back before de_pool_destroy, so
 * alloc and free are O(1) and a freed block is reused by the next alloc (LIFO). Block memory is
 * not zeroed.
 *
 * A pool with `thread_safe` takes a de_mutex around alloc and free; without it the pool must be
 * used by one thread at a time.
 */
typedef struct de_pool de_pool;

/** Pool settings. `block_size` is required; everything else has a default when zero. */
typedef struct de_pool_desc {
    /** Bytes per block (required). Rounded up to a multiple of the alignment, at least 8.
     * `block_align`: a power of two; 0 means 16. */
    size_t block_size, block_align;
    /** Blocks per page; 0 fills pages of about 64 KiB (at least one block). */
    u32 blocks_per_page;
    /** Where pages and the pool itself come from; NULL means de_heap_allocator(). Must outlive
     * the pool, and be thread-safe if `thread_safe` is set and the pool may grow on any thread. */
    const de_allocator *backing;
    /** Guard alloc and free with a mutex, so any thread may call them. */
    bool thread_safe;
    /** Memory tag passed to the backing allocator for pages. */
    u32 tag;
} de_pool_desc;

/** Creates a pool. DE_ERR_INVALID_ARGUMENT for a zero `block_size`, a bad alignment or sizes
 * that overflow; DE_ERR_OUT_OF_MEMORY if the backing allocator fails. No page is allocated yet. */
DE_API de_result de_pool_create(const de_pool_desc *desc, de_pool **out);
/** Gives every page back to the backing allocator, live blocks included. NULL is ignored. */
DE_API void de_pool_destroy(de_pool *p);
/** One block aligned to the pool's alignment, or NULL (DE_ERR_OUT_OF_MEMORY) when a new page
 * is needed and the backing allocator fails. */
DE_API void *de_pool_alloc(de_pool *p);
/** Returns a block from this pool. NULL is ignored. Debug builds check that the block belongs
 * to the pool. */
DE_API void de_pool_free(de_pool *p, void *block);

/* ---------- TLSF (general purpose, O(1)) ---------- */

/**
 * Two-level segregated fit allocator (Masmano et al., 2004) over caller memory. Alloc and free
 * are O(1): two bitmap searches, no loop over blocks. Neighbouring free blocks are merged at
 * once. Blocks are 16-byte aligned; larger power-of-two alignments (tested up to 4 KiB) cost a
 * larger search, not a longer one. Each block has a 16-byte header. Not thread-safe:
 * de_heap_allocator() wraps one in a mutex.
 *
 * Requests are rounded up to the next of 32 size classes per power of two (good fit), so a
 * request may fail although a free block is up to 1/32 larger than it.
 *
 * The largest block is 2^39 bytes. The allocator's `tag` is kept in the block header.
 */
typedef struct de_tlsf de_tlsf;
/**
 * Puts the TLSF control structure (de_tlsf_control_size() bytes, after aligning `memory` to 16)
 * at the start of `memory` and makes the rest the first pool. `memory` must stay valid and is not
 * freed by the TLSF. DE_ERR_INVALID_ARGUMENT if `bytes` cannot hold the control structure and a
 * pool of at least one block (or the pool is larger than 2^40 bytes).
 */
DE_API de_result de_tlsf_create(void *memory, size_t bytes, de_tlsf **out);
/** An allocator on `t`; valid while `t`'s memory is. Failures set DE_ERR_OUT_OF_MEMORY. */
DE_API de_allocator de_tlsf_allocator(de_tlsf *t);

/* ---------- Stats ---------- */

/**
 * Per-tag numbers, all since process start. `bytes_live` is requested bytes not yet freed,
 * `bytes_peak` its highest value. Counted by reporting allocators (see de_mem_set_hooks below):
 * a realloc counts as one free and one alloc.
 */
typedef struct de_mem_stats {
    u64 bytes_live, bytes_peak, alloc_count, free_count;
} de_mem_stats;
/** The numbers of `tag`. A tag that was never registered gives all zeros. Thread-safe. */
DE_API de_mem_stats de_mem_stats_for_tag(u32 tag);

/* ---------- Pool info, TLSF growth and checks, heap, tags, hooks, tracking (DENG-149) ----------
 */

/** What a pool holds. */
typedef struct de_pool_info {
    size_t block_stride; /**< bytes between blocks: block_size rounded up to the alignment */
    size_t block_align;  /**< alignment of every block */
    u32 blocks_per_page; /**< blocks in one page */
    u32 page_count;      /**< pages allocated from the backing allocator */
    u64 blocks_live;     /**< blocks allocated and not freed */
} de_pool_info;
/** Usage numbers of a pool. Takes the pool's mutex if it is thread-safe. */
DE_API de_pool_info de_pool_get_info(de_pool *p);

/** Bytes de_tlsf_create takes from the start of its memory for the control structure. */
DE_API size_t de_tlsf_control_size(void);

/**
 * Adds `bytes` of caller memory (aligned to 16 inside) as another pool of `t`. Blocks never span
 * or merge across pools. O(1). DE_ERR_INVALID_ARGUMENT if the memory is too small for one block
 * (64 bytes after alignment) or larger than 2^40 bytes.
 */
DE_API de_result de_tlsf_add_pool(de_tlsf *t, void *memory, size_t bytes);

/** What a TLSF holds, from a walk of every block (de_tlsf_check). */
typedef struct de_tlsf_info {
    size_t capacity;     /**< payload bytes when every block is free: one block per pool */
    size_t free_bytes;   /**< payload bytes in free blocks (sum over the free lists) */
    size_t used_bytes;   /**< payload bytes in used blocks */
    size_t largest_free; /**< payload of the largest free block */
    u32 pool_count;
    u32 free_blocks;
    u32 used_blocks;
} de_tlsf_info;

/**
 * Walks every pool and every free list and checks the TLSF's invariants: physical links and
 * sizes, no two neighbouring free blocks, every free block in the list of its size class and
 * every listed block free, bitmaps that match the lists, and free-list bytes equal to the free
 * bytes found in the walk. O(blocks): for tests and debugging. Returns DE_ERR_INTERNAL (with
 * the broken invariant in the error message) or DE_OK; fills `*out` if `out` is not NULL.
 */
DE_API de_result de_tlsf_check(const de_tlsf *t, de_tlsf_info *out);

/**
 * de_heap_allocator() (declared above): the process-wide default heap. Thread-safe, created on
 * first use, never destroyed. Small blocks come from a TLSF guarded by a de_mutex. Its pools sit
 * in one virtual-memory reservation; when the TLSF is full, the next piece of the reservation is
 * committed and added as a new pool. Blocks larger than DE_HEAP_LARGE_THRESHOLD (or aligned to
 * more than 64 KiB) are mapped straight from the OS and unmapped on free. In DE_MEM_TRACKING
 * builds the heap reports every block to the tag stats and the hooks (see de_mem_set_hooks).
 */
#define DE_HEAP_LARGE_THRESHOLD ((size_t)1 << 20)

/** What the default heap holds. Thread-safe; a snapshot. */
typedef struct de_heap_info {
    size_t reserved;  /**< address space reserved for the TLSF pools */
    size_t committed; /**< bytes of that reservation committed (control structure + pools) */
    u32 pool_count;   /**< TLSF pools: 1 at start, one more each time the heap grows */
    u64 large_count;  /**< live blocks mapped straight from the OS */
    u64 large_bytes;  /**< bytes mapped for them, headers included */
} de_heap_info;
DE_API de_heap_info de_heap_get_info(void);
/** de_tlsf_check on the heap's TLSF, under its mutex. For tests. */
DE_API de_result de_heap_check(de_tlsf_info *out);

/** Most memory tags, builtin ones included. */
#define DE_MEM_TAG_MAX 64

/**
 * 1 when de_heap_allocator() reports its blocks to the tag stats and the hooks: Debug and Dev
 * builds of the engine. 0 in Release and Shipping, where only tracking allocators report.
 */
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#define DE_MEM_TRACKING 1
#else
#define DE_MEM_TRACKING 0
#endif

/*
 * de_mem_register_tag(name) (declared above): the tag with that name, registered on first use
 * (names are compared in full and kept to 31 bytes). The builtin DE_MEM_* tags come first and are
 * named "general", "ecs", "asset", "render", "physics", "audio", "net", "ui", "scratch". When all
 * DE_MEM_TAG_MAX tags exist, it returns DE_MEM_GENERAL and sets DE_ERR_OUT_OF_MEMORY. A NULL or
 * empty name gives DE_MEM_GENERAL. Thread-safe.
 */

/** The name of a registered tag, or NULL. Valid for the life of the process. */
DE_API const char *de_mem_tag_name(u32 tag);
/** Number of registered tags (builtin included): valid tags are 0 .. count - 1. */
DE_API u32 de_mem_tag_count(void);

/**
 * Callbacks that see every block a reporting allocator hands out and takes back: the heap in
 * DE_MEM_TRACKING builds, and tracking allocators. Each block is reported once, by the lowest
 * reporting allocator in a chain. Tracy (S06) and the frame alloc guard (DENG-155) install these.
 *
 * `on_free` comes before the block can be reused, `on_alloc` after the block is allocated and
 * before the caller gets it. A realloc reports a free of the old block, then an alloc of the new
 * one (also when it did not move). Sizes are the requested sizes. Either callback may be NULL.
 * They run on the allocating thread, possibly on many threads at once. A callback may allocate;
 * allocations made inside a callback are counted in the stats but do not call the hooks again.
 */
typedef struct de_mem_hooks {
    void (*on_alloc)(void *user, void *ptr, size_t size, u32 tag);
    void (*on_free)(void *user, void *ptr, size_t size, u32 tag);
    void *user;
} de_mem_hooks;

/**
 * Installs `hooks` (NULL uninstalls) and returns the hooks installed before. Only the pointer is
 * kept: `*hooks` must stay valid and unchanged while installed, and until calls already running
 * on other threads have returned. Callable at any time, also before the first allocation; with
 * no hooks installed a report costs one relaxed atomic load.
 *
 * There is one slot; the last setter wins. To keep an earlier installer working, call the
 * returned hooks from yours (chaining), and on uninstall set the returned hooks back. Uninstall
 * in the reverse order of install.
 */
DE_API const de_mem_hooks *de_mem_set_hooks(const de_mem_hooks *hooks);

/**
 * A counting allocator around another one: counts calls and bytes, can be told to fail, and
 * reports to the tag stats and hooks. Storage is caller-owned and opaque; every operation on it
 * is thread-safe (atomic counters). It must outlive the allocator that de_tracking_allocator_init
 * returned.
 */
typedef struct de_tracking_allocator {
    DE_ALIGN(8) u8 opaque_[128];
} de_tracking_allocator;

/** Counts of one tracking allocator since init. Bytes are requested sizes. */
typedef struct de_tracking_stats {
    u64 alloc_count;   /**< successful allocs, and reallocs of NULL */
    u64 realloc_count; /**< successful reallocs of a block */
    u64 free_count;    /**< frees of a non-NULL block */
    u64 fail_count;    /**< allocs and reallocs that returned NULL (injected or real) */
    u64 blocks_live;   /**< alloc_count - free_count */
    u64 bytes_live;
    u64 bytes_peak;
} de_tracking_stats;

/**
 * Initialises `t` around `backing` (NULL means de_heap_allocator()) and returns the allocator.
 * Every block goes to `backing` with `tag`, whatever tag the caller passed, so a module can
 * account everything it allocates to its own tag.
 *
 * `t` reports to the tag stats and hooks, unless `backing` already does: the heap in a
 * DE_MEM_TRACKING build, or another tracking allocator. So a block is never reported twice.
 * When trackers are stacked, the lowest one's tag is the one that is reported.
 */
DE_API de_allocator de_tracking_allocator_init(de_tracking_allocator *t,
                                               const de_allocator *backing, u32 tag);
/** The counts of `t`. Exact when no other thread is allocating through it. */
DE_API de_tracking_stats de_tracking_allocator_stats(const de_tracking_allocator *t);
/**
 * Arms failure injection: the next `n` allocs and reallocs through `t` succeed (if `backing`
 * does), and every one after them returns NULL with DE_ERR_OUT_OF_MEMORY, without calling
 * `backing`. `n < 0` disarms it. Frees always work.
 */
DE_API void de_tracking_allocator_fail_after(de_tracking_allocator *t, i64 n);

DE_EXTERN_C_END
#endif
