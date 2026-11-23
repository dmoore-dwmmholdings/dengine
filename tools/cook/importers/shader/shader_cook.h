/*
 * shader_cook.h: the shader importer's compile path for generated Slang (DENG-770). Internal to
 * the cooker library; include as "cook/importers/shader/shader_cook.h".
 *
 * de__shader_cook compiles a module held in memory into the cooked shader format
 * (gpu/shader/shader_format.h), as the `shader` importer does for a `.slang` source, but with:
 *   - one entry point, named by the caller (no `entry_points` setting);
 *   - one permutation, without defines (no `permutations` setting);
 *   - the caller's import reader (no reads through the import context);
 *   - the bytes returned to the caller instead of written as the asset.
 * The output targets come from the `targets` setting of `ctx`, else the platform default, as in
 * the `shader` importer. Errors fail `ctx` (de_asset_import_fail) with the target and Slang's
 * diagnostics.
 */
#ifndef DE_COOK_SHADER_COOK_H
#define DE_COOK_SHADER_COOK_H

#include "cook/importers/shader/slang_compile.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

typedef struct de__shader_cook_desc {
    de_asset_import_ctx *ctx; /**< settings (`targets`), platform, allocator, errors */
    const char *path;         /**< the module's path for Slang and the messages */
    const char *source;
    u64 source_size;
    const char *entry; /**< the one entry point */
    u32 stage;         /**< its de__shader_stage */
    de__slang_read_fn read;
    void *read_user;
} de__shader_cook_desc;

/**
 * Compiles `d->source` for every output target and returns the cooked shader in `*out`
 * (`*out_size` bytes, from de_asset_import_allocator(d->ctx); free with DE_FREE and the size).
 * Deterministic: the same inputs give the same bytes.
 * @return DE_OK; the de_asset_import_fail result otherwise (`*out` is NULL).
 */
de_result de__shader_cook(const de__shader_cook_desc *d, u8 **out, size_t *out_size);

DE_EXTERN_C_END

#endif /* DE_COOK_SHADER_COOK_H */
