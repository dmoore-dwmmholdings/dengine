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
typedef struct de_pool de_pool;
typedef struct de_pool_desc {
    size_t block_size, block_align;
    u32 blocks_per_page;
    const de_allocator *backing;
    bool thread_safe;
    u32 tag;
} de_pool_desc;
DE_API de_result de_pool_create(const de_pool_desc *desc, de_pool **out);
DE_API void de_pool_destroy(de_pool *p);
DE_API void *de_pool_alloc(de_pool *p);
DE_API void de_pool_free(de_pool *p, void *block);

/* ---------- TLSF (general purpose, O(1)) ---------- */
typedef struct de_tlsf de_tlsf;
DE_API de_result de_tlsf_create(void *memory, size_t bytes, de_tlsf **out);
DE_API de_allocator de_tlsf_allocator(de_tlsf *t);

/* ---------- Stats ---------- */
typedef struct de_mem_stats {
    u64 bytes_live, bytes_peak, alloc_count, free_count;
} de_mem_stats;
DE_API de_mem_stats de_mem_stats_for_tag(u32 tag);

DE_EXTERN_C_END
#endif
