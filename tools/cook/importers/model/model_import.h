/*
 * model_import.h: the `model` importer (DENG-101, SPEC §11.6): glTF 2.0 (`.gltf`, `.glb`) and FBX
 * (`.fbx`, DENG-102) to meshes, materials, textures and a prefab. Cooker-library code: it is
 * compiled into `de_cooker`, so it exists in the tools and in Debug and Dev builds
 * (DE_HAS_COOKER), never in Release or Shipping. Include as "cook/importers/model/model_import.h".
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
 *   - `generate_lods` (bool, default true): the LOD chain (up to 8 LODs); false keeps LOD 0.
 *   - `lod_error` (float, default 0.01, 0 to 1): the screen-space error, as a fraction of the
 *     viewport height, at which the runtime switches LODs by default (stored in each mesh).
 *   - `meshlets` (bool, default true): meshlets per LOD; false writes indexed LODs only (debug).
 *   - `max_position_error` (float, metres, default 0.0005, >= 0): the largest position error
 *     16-bit quantization may make; a mesh whose 16-bit step would exceed it keeps f32 positions.
 *   - `axis_up`, `axis_front` (strings, default "auto"; FBX only, DENG-102): override the file's
 *     axis settings when they are wrong (fbx_import.h). `scale` multiplies after the FBX unit
 *     conversion to metres.
 *   - `anim_sample_rate`, `anim_error`, `anim_error_angle`, `anim_joint_names`: skeleton and clip
 *     cooking (DENG-104, anim_cook.h).
 *   - `max_decoded_mib` (integer, MiB, default 1024, 1 to 2^20; DENG-106): the most decoded data
 *     one import may hold: every block of the model IR (vertex streams, indices, keys, the cooked
 *     blobs), checked before each block is allocated; also the memory limit of each ufbx
 *     allocator, and passed on to the texture cooks of the model's images. An import over it fails
 *     with DE_ERR_UNSUPPORTED.
 *
 * Output: the model bytes (DMDL, render/formats/de_cooked_model.h) and the sub-assets, in this
 * order: textures (`texture:<name>`, type `texture`, cooked by the registered texture importer),
 * materials (`material:<name>`, type `material`, DMAT), meshes (`<node name>`, type `mesh`,
 * DMSH v2: quantized vertices, LOD chain, meshlets; DENG-103), skeletons (`skeleton:<name>`, type
 * `skeleton`, DSKL, anim/formats/de_cooked_skeleton.h), clips (`anim:<name>`, type `anim_clip`,
 * DCLP, anim/formats/de_cooked_clip.h), the prefab (`prefab`, type `prefab`, cooked by the
 * registered prefab importer from TOML). The model depends on all of them.
 */
#ifndef DE_COOK_MODEL_IMPORT_H
#define DE_COOK_MODEL_IMPORT_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** Bump on any change of the importer's output: cached cooks of older versions are not reused. */
#define DE_MODEL_IMPORTER_VERSION                                                                  \
    4u /* 2: meshes v2 (DENG-103); 3: skeletons and clips (DENG-104); 4: faster clip key           \
          reduction and check passes (DENG-399) */

/** Default of the `max_decoded_mib` setting (DENG-106) and its largest value (1 TiB). */
#define DE_MODEL_MAX_DECODED_MIB_DEFAULT 1024
#define DE_MODEL_MAX_DECODED_MIB_MAX     (1u << 20)

#define DE_MODEL_IMPORT_EXTENSION_COUNT 3u
/** ".gltf", ".glb", ".fbx" (DENG-102). */
extern const char *const de_model_import_extensions[DE_MODEL_IMPORT_EXTENSION_COUNT];

/** The importer (de_asset_type_desc.import). */
de_result de_model_import(de_asset_import_ctx *ctx);

DE_EXTERN_C_END

#endif /* DE_COOK_MODEL_IMPORT_H */
