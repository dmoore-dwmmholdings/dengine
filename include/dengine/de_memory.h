/* de_memory.h — allocator interface and engine allocators. SPEC.md §5.3. */
#ifndef DE_MEMORY_H
#define DE_MEMORY_H
#include "de_base.h"
DE_EXTERN_C_BEGIN

typedef struct de_allocator {
    void *(*alloc)(void *user, size_t size, size_t align, u32 tag);
    void *(*realloc)(void *user, void *ptr, size_t old_size, size_t new_size, size_t align, u32 tag);
    void  (*free)(void *user, void *ptr, size_t size);
    void  *user;
} de_allocator;

#define DE_ALLOC(a, size, align, tag)  ((a)->alloc((a)->user, (size), (align), (tag)))
#define DE_FREE(a, ptr, size)          ((a)->free((a)->user, (ptr), (size)))
#define DE_NEW(a, T, tag)              ((T *)DE_ALLOC((a), sizeof(T), DE_ALIGNOF(T), (tag)))

/* Memory tags for tracking (Debug/Dev). Modules register more with de_mem_register_tag. */
enum { DE_MEM_GENERAL = 0, DE_MEM_ECS, DE_MEM_ASSET, DE_MEM_RENDER, DE_MEM_PHYSICS,
       DE_MEM_AUDIO, DE_MEM_NET, DE_MEM_UI, DE_MEM_SCRATCH, DE_MEM_TAG_BUILTIN_COUNT_ };
DE_API u32 de_mem_register_tag(const char *name);

DE_API const de_allocator *de_heap_allocator(void);    /* TLSF default heap, thread-safe */

/* ---------- Virtual arena ---------- */
typedef struct de_arena de_arena;
typedef struct de_arena_desc {
    size_t reserve_bytes;   /* address space reserved up front (default 64 GiB) */
    size_t commit_step;     /* commit granularity (default 64 KiB) */
    u32    tag;
    const char *name;
} de_arena_desc;
typedef struct de_arena_mark { size_t pos; } de_arena_mark;

DE_API de_result de_arena_create(const de_arena_desc *desc, de_arena **out);
DE_API void      de_arena_destroy(de_arena *a);
DE_API void     *de_arena_push(de_arena *a, size_t size, size_t align);
DE_API void     *de_arena_push_zero(de_arena *a, size_t size, size_t align);
DE_API de_arena_mark de_arena_mark_get(const de_arena *a);
DE_API void      de_arena_reset_to(de_arena *a, de_arena_mark m);
DE_API void      de_arena_reset(de_arena *a);
DE_API de_allocator de_arena_allocator(de_arena *a);   /* free is a no-op */
#define DE_ARENA_PUSH(a, T, n) ((T *)de_arena_push((a), sizeof(T) * (n), DE_ALIGNOF(T)))

/* ---------- Frame arenas (one per in-flight frame) ---------- */
DE_API de_arena *de_frame_arena(void);                  /* current frame, reset at frame start */

/* ---------- Thread scratch ---------- */
typedef struct de_scratch { de_arena *arena; de_arena_mark mark; } de_scratch;
/* Pass arenas already in use as `conflicts` so nested scratch never aliases them. */
DE_API de_scratch de_scratch_begin(de_arena *const *conflicts, u32 conflict_count);
DE_API void       de_scratch_end(de_scratch s);

/* ---------- Pool (fixed-size blocks) ---------- */
typedef struct de_pool de_pool;
typedef struct de_pool_desc {
    size_t block_size, block_align;
    u32    blocks_per_page;
    const de_allocator *backing;
    bool   thread_safe;
    u32    tag;
} de_pool_desc;
DE_API de_result de_pool_create(const de_pool_desc *desc, de_pool **out);
DE_API void      de_pool_destroy(de_pool *p);
DE_API void     *de_pool_alloc(de_pool *p);
DE_API void      de_pool_free(de_pool *p, void *block);

/* ---------- TLSF (general purpose, O(1)) ---------- */
typedef struct de_tlsf de_tlsf;
DE_API de_result de_tlsf_create(void *memory, size_t bytes, de_tlsf **out);
DE_API de_allocator de_tlsf_allocator(de_tlsf *t);

/* ---------- Stats ---------- */
typedef struct de_mem_stats { u64 bytes_live, bytes_peak, alloc_count, free_count; } de_mem_stats;
DE_API de_mem_stats de_mem_stats_for_tag(u32 tag);

DE_EXTERN_C_END
#endif
