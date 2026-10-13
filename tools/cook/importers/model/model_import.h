/*
 * model_import.h: the `model` importer (DENG-101, SPEC §11.6): glTF 2.0 (`.gltf`, `.glb`) to
 * meshes, materials, textures and a prefab. Cooker-library code: it is compiled into `de_cooker`,
 * so it exists in the tools and in Debug and Dev builds (DE_HAS_COOKER), never in Release or
 * Shipping. Include as "cook/importers/model/model_import.h".
 *
 * Register it with the cooker (or, in the runtime, as de_asset_type_desc.import under
 * `#if DE_HAS_COOKER`, DENG-105):
 *
 *     de_asset_type_desc t = {.name = "model", .import = de_model_import,
 *                             .importer_version = DE_MODEL_IMPORTER_VERSION,
 *                             .extensions = de_model_import_extensions,
 *                             .extension_count = DE_MODEL_IMPORT_EXTENSION_COUNT};
 *
 * Settings (the manifest asset and `[defaults.model]`; every setting is in the settings hash):
 *   - `scale` (float, default 1.0, > 0): uniform scale applied with the axis conversion.
 *   - `import_cameras` (bool, default false): cameras and lights are not imported yet; true logs
 *     a warning.
 *   - `generate_lods`, `lod_error`, `meshlets`: read by the mesh stage (DENG-103).
 *
 * Output: the model bytes (DMDL, render/formats/de_cooked_model.h) and the sub-assets, in this
 * order: textures (`texture:<name>`, type `texture`, cooked by the registered texture importer),
 * materials (`material:<name>`, type `material`, DMAT), meshes (`<node name>`, type `mesh`,
 * DMSH), skeletons and clips (DENG-104), the prefab (`prefab`, type `prefab`, cooked by the
 * registered prefab importer from TOML). The model depends on all of them.
 */
#ifndef DE_COOK_MODEL_IMPORT_H
#define DE_COOK_MODEL_IMPORT_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** Bump on any change of the importer's output: cached cooks of older versions are not reused. */
#define DE_MODEL_IMPORTER_VERSION 1u

#define DE_MODEL_IMPORT_EXTENSION_COUNT 2u
/** ".gltf", ".glb". */
extern const char *const de_model_import_extensions[DE_MODEL_IMPORT_EXTENSION_COUNT];

/** The importer (de_asset_type_desc.import). */
de_result de_model_import(de_asset_import_ctx *ctx);

DE_EXTERN_C_END

#endif /* DE_COOK_MODEL_IMPORT_H */
