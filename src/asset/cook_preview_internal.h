/* DENG-696: detached importer comparison; no live asset loads, cache or manifest writes. */
#ifndef DE_ASSET_COOK_PREVIEW_INTERNAL_H
#define DE_ASSET_COOK_PREVIEW_INTERNAL_H
#include "asset/manifest_internal.h"

DE_EXTERN_C_BEGIN

typedef struct de__cook_preview_side {
    const void *data;
    u64 size, total_bytes; /* root plus every generated subasset, not estimated pack size */
    u32 child_count;
    const void *private_;
} de__cook_preview_side;
typedef struct de__cook_preview_child {
    const char *name, *type;
    const void *data;
    u64 size;
} de__cook_preview_child;
typedef struct de__cook_preview {
    de__cook_preview_side before, after;
    void *private_;
} de__cook_preview;

/* Caller owns a detached, immutable SOURCE asset system with loaded manifest and component
 * registry (for generated prefabs). Run on its owning worker. `entry` is the proposed complete
 * manifest entry, not a patch: omitted keys inherit type defaults. Sources/types cannot change.
 * Locale resolution and the engine's currently ignored `variants` policy match ordinary cooks.
 * The supplied allocator owns all cooker work/results; no cache or runtime slot is touched.
 * out is assigned only on success. Cooker-free builds return UNSUPPORTED without changing out. */
de_result de__asset_cook_preview(de_asset_system *snapshot, const char *id,
                                 const de__manifest_kv *entry, u32 count, de_cook_platform platform,
                                 de__cook_preview *out);
bool de__asset_cook_preview_child(const de__cook_preview_side *side, u32 index,
                                  de__cook_preview_child *out);
void de__asset_cook_preview_free(de__cook_preview *preview);
/* Worker-side final source check. BUSY means captured manifest/source bytes changed/deleted.
 * Both sides share captured per-file bytes; this is not an atomic multi-file transaction. */
de_result de__asset_cook_preview_validate(const de__cook_preview *preview);
DE_EXTERN_C_END
#endif
