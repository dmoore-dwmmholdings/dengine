/* de_shader.h — Slang shader assets for code that draws with de_gpu directly (DENG-409).
 * SPEC.md §12.2, §11.6.
 *
 * A `shader` asset is one `.slang` source cooked to SPIR-V, DXIL and metallib, per permutation and
 * per entry point, with reflection (src/gpu/shader/). This header lets an application that owns
 * an asset system register the type and hand the cooked code to de_gpu_pipeline_create:
 *
 *     DE_TRY(de_shader_register_type(assets, NULL));
 *     de_shader sh = de_asset_shader(assets, de_asset_id_from("shaders/triangle"));
 *     ... load it (de_asset_request) and wait until it is READY ...
 *     de_gpu_pipeline_desc pd = {0};
 *     de_shader_reflection vs_refl;
 *     DE_TRY(de_shader_get_code(assets, sh, backend, 0, "vs_main", &pd.vs, &vs_refl));
 *     DE_TRY(de_shader_get_code(assets, sh, backend, 0, "ps_main", &pd.ps, NULL));
 *
 * Debug and Dev builds with the tools (DE_HAS_COOKER) cook `.slang` sources on demand in SOURCE
 * mode through the in-process cooker and its `.decache`; Release and Shipping load cooked data
 * from packs (`de-cook cook` writes it). With the renderer (S12) the renderer registers the type
 * itself: do not call de_shader_register_type then.
 *
 * Not available without a GPU module (DE_HEADLESS): the functions are not linked there.
 */
#ifndef DE_SHADER_H
#define DE_SHADER_H
#include "de_asset.h"
#include "de_base.h"
#include "de_gpu.h"
#include "de_memory.h"
DE_EXTERN_C_BEGIN

/** The pipeline stage of a compiled entry point. */
typedef enum de_shader_stage {
    DE_SHADER_STAGE_NONE = 0,
    DE_SHADER_STAGE_VERTEX,
    DE_SHADER_STAGE_PIXEL,
    DE_SHADER_STAGE_COMPUTE,
    DE_SHADER_STAGE_MESH,
    DE_SHADER_STAGE_AMPLIFICATION, /**< task shader on Vulkan, object shader on Metal */
    DE_SHADER_STAGE_RAYGEN,
    DE_SHADER_STAGE_MISS,
    DE_SHADER_STAGE_CLOSEST_HIT,
    DE_SHADER_STAGE_ANY_HIT
} de_shader_stage;

/** What the cooker recorded about one compiled entry point. */
typedef struct de_shader_reflection {
    de_shader_stage stage;
    u32 threadgroup[3];      /**< compute, mesh and amplification; 0 otherwise */
    u32 push_constant_bytes; /**< the bytes of the push-constant block it uses; at most 128 */
    u32 mesh_max_vertices;   /**< mesh shaders; 0 otherwise */
    u32 mesh_max_primitives; /**< mesh shaders; 0 otherwise */
} de_shader_reflection;

/**
 * Registers the `shader` asset type (extension ".slang") with `assets`. Loaded shaders are copied
 * into memory from `allocator` (NULL = de_heap_allocator()), which must outlive `assets`. In
 * builds with the cooker (DE_HAS_COOKER) the type has the Slang importer, so SOURCE mode cooks
 * `.slang` sources; elsewhere it loads cooked data only. Once per asset system, on the main
 * thread, before the first shader asset is requested.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`assets` NULL), DE_ERR_ALREADY_EXISTS (a `shader` type
 *         is registered), DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_shader_register_type(de_asset_system *assets, const de_allocator *allocator);

/**
 * The code of entry point `entry` of permutation `permutation` (its index in the asset's
 * `permutations` setting; 0 without permutations) of a READY shader, for `backend`: DXIL for
 * D3D12, SPIR-V for Vulkan and NULL, metallib for Metal, the platform's for DE_GPU_DEFAULT.
 * `out` is ready for de_gpu_pipeline_desc (vs, ps, cs, ...); `refl` may be NULL. The code stays
 * valid until the asset is evicted or reloaded. Any thread.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (a NULL argument, a stale handle, not a shader);
 *         DE_ERR_BUSY (the asset is not loaded yet); DE_ERR_NOT_FOUND (no such entry point or
 *         permutation, or the shader was not cooked for that backend: DXIL needs DXC on the
 *         cooking host, metallib a macOS host).
 */
DE_API de_result de_shader_get_code(de_asset_system *assets, de_shader shader,
                                    de_gpu_backend backend, u32 permutation, const char *entry,
                                    de_gpu_shader_blob *out, de_shader_reflection *refl);

DE_EXTERN_C_END
#endif /* DE_SHADER_H */
