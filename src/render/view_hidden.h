#ifndef DE_RENDER_VIEW_HIDDEN_H
#define DE_RENDER_VIEW_HIDDEN_H

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

typedef struct de__view_hidden de__view_hidden;
/** Immutable sorted full entity IDs, shared by main/render sides and Extract frames. */
de_result de__view_hidden_create(const de_allocator *alloc, const u64 *ids, u32 count,
                                 const de__view_hidden **out);
const de__view_hidden *de__view_hidden_retain(const de__view_hidden *policy);
void de__view_hidden_release(const de__view_hidden *policy);
u32 de__view_hidden_count(const de__view_hidden *policy);
const u64 *de__view_hidden_ids(const de__view_hidden *policy);
bool de__view_hidden_contains(const de__view_hidden *policy, u64 entity);
u64 de__view_hidden_hash(const de__view_hidden *policy);
bool de__view_hidden_equal(const de__view_hidden *a, const de__view_hidden *b);

DE_EXTERN_C_END

#endif
