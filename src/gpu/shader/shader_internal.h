/*
 * shader_internal.h: the runtime `shader` asset type (SPEC §11.6, §12.2; DENG-289). Module
 * `gpu/shader` (target de_gpu_shader): it sits between the RHI and the asset system, reading
 * cooked blobs (shader_format.h) behind de_shader handles for pipeline creation (S10+).
 *
 * The renderer, or whoever creates pipelines, registers the type once per asset system. In
 * Debug and Dev builds with the tools (DE_HAS_COOKER) the type gets the cooker library's built-in
 * shader importer (de_cooker_builtin_importer), so SOURCE mode compiles .slang on demand; other
 * builds load cooked data only (DENG-409). Code without the renderer uses the public de_shader.h
 * (de_shader_register_type, de_shader_get_code), which has no reload callbacks.
 *
 *     de__shader_types *st = NULL;
 *     de__shader_type_desc d = {0};
 *     DE_TRY(de__shader_type_register(assets, &d, &st));
 *     ...
 *     de_shader sh = de_asset_shader(assets, de_asset_id_from("shaders/blur"));
 *     de_gpu_shader_blob cs;
 *     DE_TRY(de__shader_blob(assets, sh, caps.backend, 0, "cs_main", &cs, NULL));
 *     ...
 *     de_asset_system_destroy(assets);
 *     de__shader_types_destroy(st);
 *
 * Hot reload (Debug and Dev): when a shader asset reloads, every callback added with
 * de__shader_on_reload runs on the main thread inside de_asset_system_update, after the swap, so
 * pipelines built from the old blobs can be rebuilt (de_asset_type_desc.reloaded).
 *
 * Threads: registration, the callbacks and de__shader_on_reload on the main thread.
 * de__shader_blob on any thread while the asset stays loaded; the blob it returns is valid until
 * the asset is evicted or reloaded (the retired object lives reload_retire_frames updates more).
 */
#ifndef DE_GPU_SHADER_INTERNAL_H
#define DE_GPU_SHADER_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>

#include "gpu/shader/shader_format.h"

DE_EXTERN_C_BEGIN

/** The registered type: its reload callbacks. */
typedef struct de__shader_types de__shader_types;

typedef struct de__shader_type_desc {
    const de_allocator *allocator; /**< the type's own memory; NULL = de_heap_allocator() */
    /** The importer; NULL = the cooker's built-in shader importer in builds with the cooker
     * (DE_HAS_COOKER), cooked data only elsewhere. Tests pass their own. */
    de_result (*import)(de_asset_import_ctx *ctx);
    u32 importer_version; /**< with `import`: its version */
} de__shader_type_desc;

/**
 * Registers the `shader` asset type (extension ".slang") with `s`. Destroy the returned state
 * with de__shader_types_destroy after the asset system is destroyed. Main thread.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_ALREADY_EXISTS (a `shader` type exists),
 *         DE_ERR_OUT_OF_MEMORY.
 */
de_result de__shader_type_register(de_asset_system *s, const de__shader_type_desc *d,
                                   de__shader_types **out);
/** Frees the registration state. Destroy the asset system first. NULL is ignored. */
void de__shader_types_destroy(de__shader_types *t);

/** Called when shader asset `id` reloaded; its handle now resolves to the new blobs. */
typedef void (*de__shader_reload_fn)(de_asset_id id, void *user);
/** Adds a reload callback (at most 64). Main thread.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT, DE_ERR_OUT_OF_MEMORY (64 callbacks). */
de_result de__shader_on_reload(de__shader_types *t, de__shader_reload_fn fn, void *user);
/** Removes a callback added with the same `fn` and `user`. Main thread. */
void de__shader_on_reload_remove(de__shader_types *t, de__shader_reload_fn fn, void *user);

/** The reflection of one compiled entry point (shader_format.h, de__shader_record). */
typedef struct de__shader_reflection {
    u32 stage; /**< de__shader_stage */
    u32 threadgroup[3];
    u32 push_constant_bytes;
    u32 bindless_flags; /**< de__shader_bindless_flags */
    u32 mesh_max_vertices, mesh_max_primitives;
} de__shader_reflection;

/** The cooked format target a backend consumes: D3D12 DXIL, Vulkan and NULL SPIR-V, Metal
 * metallib; DE_GPU_DEFAULT is the platform default backend. */
de__shader_target de__shader_target_for_backend(de_gpu_backend backend);

/**
 * The code of entry point `entry` of permutation `permutation` (its index in the asset's
 * `permutations` setting; 0 without permutations) for `backend`, and its reflection (`refl` may
 * be NULL). `out->entry` points at the entry name inside the blob. Any thread; see the header.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL argument, stale handle, not a shader);
 *         DE_ERR_BUSY (the asset is not loaded yet); DE_ERR_NOT_FOUND (no such entry point or
 *         permutation, or the target was not cooked: DXIL cooked without DXC).
 */
de_result de__shader_blob(de_asset_system *s, de_shader shader, de_gpu_backend backend,
                          u32 permutation, const char *entry, de_gpu_shader_blob *out,
                          de__shader_reflection *refl);

/** The number of permutations of a loaded shader (0 when it is not loaded). Any thread. */
u32 de__shader_permutation_count(de_asset_system *s, de_shader shader);
/** The defines of permutation `i` as written in the settings ("A=1;B"), or NULL. Any thread. */
const char *de__shader_permutation_defines(de_asset_system *s, de_shader shader, u32 i);

/* ---- Format view (the loader; tests and fuzzing) --------------------------------------------- */

/** A checked view of a cooked blob: every offset and size is inside it. */
typedef struct de__shader_view {
    const u8 *base;
    u64 size;
    de__shader_header header;
    const u8 *entries;      /**< de__shader_entry[entry_count], unaligned */
    const u8 *permutations; /**< de__shader_permutation[permutation_count] */
    const u8 *records;      /**< de__shader_record[record_count] */
    const char *strings;    /**< strings_size bytes, last one NUL */
} de__shader_view;

/**
 * Checks a cooked blob and fills `out`: magic, version, table bounds, NUL-terminated strings,
 * records in range and sorted. Allocation-free; any thread. On failure `*out` is zeroed.
 * @return DE_OK, DE_ERR_VERSION_MISMATCH (another format version) or DE_ERR_PARSE (the reason
 *         is in the error context).
 */
de_result de__shader_view_init(const void *data, u64 size, de__shader_view *out);
/** Record `i` of a checked view (copied: the table is not aligned). */
de__shader_record de__shader_view_record(const de__shader_view *v, u32 i);
/** Entry `i`, its name in `*name`. */
de__shader_entry de__shader_view_entry(const de__shader_view *v, u32 i, const char **name);
/** The defines string of permutation `i`. */
const char *de__shader_view_permutation(const de__shader_view *v, u32 i);
/** The index of the record of (target, permutation, entry), or -1. */
i32 de__shader_view_find(const de__shader_view *v, u32 target, u32 permutation, u32 entry);

DE_EXTERN_C_END

#endif /* DE_GPU_SHADER_INTERNAL_H */
