/*
 * mem_internal.h: glue between the DENG-149 allocators (internal to core; include as
 * "core/mem_internal.h").
 *
 * - tlsf.c exports raw entry points that never touch the error context, so the heap can retry
 *   after growing without formatting an error message first.
 * - mem_tags.c exports the reporting sink (tag stats + hooks) used by the heap and by tracking
 *   allocators.
 * - heap.c and tracking_alloc.c export their alloc functions so a tracking allocator can tell
 *   whether its backing allocator already reports.
 */
#ifndef DE_CORE_MEM_INTERNAL_H
#define DE_CORE_MEM_INTERNAL_H

#include <dengine/de_memory.h>

/* ---- tlsf.c ---- */

/** Largest request de__tlsf_alloc accepts (bytes); larger ones fail. */
#define DE__TLSF_MAX_ALLOC ((size_t)1 << 38)

/** Like the allocator's alloc, but sets no error on failure. `align` 0 means 16. */
void *de__tlsf_alloc(de_tlsf *t, size_t size, size_t align, u32 tag);
/** Frees a block of `t`. NULL is ignored. */
void de__tlsf_free(de_tlsf *t, void *ptr);
/**
 * Resizes `ptr` in place to `new_size` if it satisfies `align` and the block (with a free
 * neighbour after it) is large enough; shrinking always works. Returns false and changes nothing
 * otherwise. The block's tag becomes `tag`.
 */
bool de__tlsf_resize_in_place(de_tlsf *t, void *ptr, size_t new_size, size_t align, u32 tag);
/** The tag stored in a used block's header. */
u32 de__tlsf_block_tag(const void *ptr);
/** Payload bytes of a used block (at least what was requested). */
size_t de__tlsf_block_size(const void *ptr);

/* ---- mem_tags.c ---- */

/** Records an allocation of `size` bytes under `tag` (stats, then hooks). */
void de__mem_report_alloc(void *ptr, size_t size, u32 tag);
/** Records a free (hooks, then stats). Call before the block can be reused. */
void de__mem_report_free(void *ptr, size_t size, u32 tag);

/* ---- heap.c / tracking_alloc.c ---- */

void *de__heap_alloc_fn(void *user, size_t size, size_t align, u32 tag);
void *de__tracking_alloc_fn(void *user, size_t size, size_t align, u32 tag);

#endif /* DE_CORE_MEM_INTERNAL_H */
