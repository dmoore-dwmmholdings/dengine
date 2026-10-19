/*
 * shader_importer.h: the `shader` importer (SPEC §11.6, §12.2; DENG-289): a `.slang` source to
 * SPIR-V, DXIL and metallib per permutation and entry point, with reflection, in the cooked
 * format of src/gpu/shader/shader_format.h. Cooker-library code (part of de_cooker since
 * DENG-409): de-cook and the in-process cooker of Debug and Dev builds use it, never Release or
 * Shipping. Include as "cook/importers/shader/shader_importer.h" (tools and tests only).
 *
 * The cooker's built-in importer table hands it out (de_cooker_builtin_importer("shader")):
 * `de-cook cook` registers it, and so does the runtime type of Debug and Dev builds
 * (de__shader_type_register, de_shader_register_type). A tool or a test can register it with a
 * cooker itself:
 *
 *     de_asset_type_desc t = {.name = "shader", .import = de_shader_import,
 *                             .importer_version = de_shader_importer_version(),
 *                             .extensions = de_shader_import_extensions,
 *                             .extension_count = DE_SHADER_IMPORT_EXTENSION_COUNT};
 *
 * Settings (the manifest asset and `[defaults.shader]`; all are in the settings hash):
 *   - `entry_points` (string array): `"name"` or `"name:stage"`, stage one of vertex, pixel
 *     (or fragment), compute, mesh, amplification (or task), raygen (or raygeneration),
 *     miss, closesthit or anyhit. RT stages produce SPIR-V RT modules / SM6.6 DXIL libraries;
 *     Metal RT pipelines use the explicit native ABI in shaders/common/rt_pipeline_metal.h. A name
 * without a stage needs a [shader("...")] attribute. Default: every [shader("...")] entry point, in
 * source order.
 *   - `permutations` (string array): each a list of defines `"A=1;B"` (B means B=1); the index
 *     in this list is the permutation index at run time. Default: one permutation, no defines.
 *   - `targets` (string array): any of "spirv", "dxil", "metallib". Default by cook platform:
 *     windows dxil + spirv, linux spirv, macos metallib + spirv. A target this host cannot
 *     compile (DXIL without DXC, metallib off macOS) is skipped with a warning; when no target
 *     is left the cook fails.
 *
 * Imports: `import bindless;`, `push_constants`, `math` and `color` are the engine modules
 * (shaders/common/, embedded in the importer). Every other import is read relative to the source
 * with de_asset_import_read_file, so it is part of the cache key and hot reload watches it.
 *
 * Checks: each entry point's push constants must fit 128 bytes on every target (the cook fails
 * otherwise). Reflection recorded per target, permutation and entry point: stage, threadgroup
 * size, push-constant bytes, the bindless tables it reads, mesh output limits.
 */
#ifndef DE_COOK_SHADER_IMPORTER_H
#define DE_COOK_SHADER_IMPORTER_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** Bump on any change of the importer's output. de_shader_importer_version() mixes it with the
 * embedded engine modules and the Slang version. */
#define DE_SHADER_IMPORTER_BASE_VERSION 1u

#define DE_SHADER_IMPORT_EXTENSION_COUNT 1u
/** ".slang". */
extern const char *const de_shader_import_extensions[DE_SHADER_IMPORT_EXTENSION_COUNT];

/**
 * The importer version for de_asset_type_desc.importer_version: a hash of the base version, the
 * sources of the embedded shaders/common/ modules and the pinned Slang version, so editing a
 * common module or updating Slang invalidates every cooked shader. Never 0. Any thread.
 */
u32 de_shader_importer_version(void);

/** The importer (de_asset_type_desc.import). */
de_result de_shader_import(de_asset_import_ctx *ctx);

DE_EXTERN_C_END

#endif /* DE_COOK_SHADER_IMPORTER_H */
