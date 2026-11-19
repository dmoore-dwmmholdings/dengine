/*
 * mg_import.h: the `material_graph` importer (DENG-770, SPEC §12.6). Part of the cooker library
 * when it has Slang (DE_COOKER_HAS_MATERIAL_GRAPH_IMPORTER).
 *
 * The source is a material graph file (the editor's `kind = "material"` TOML, DENG-579). The
 * importer:
 *   1. generates the graph's Slang module (de__mg_generate); graph errors fail the cook with the
 *      node, pin and message of each;
 *   2. compiles its cs_resolve_model for each output target (de__shader_cook; the `targets`
 *      setting as for `shader` assets). Slang errors are mapped back to their nodes. The module's
 *      engine imports (visbuffer/resolve_model, ...) are read from the engine shader root:
 *      $DE_ENGINE_SHADER_ROOT, else the engine's shaders/ directory of this build;
 *   3. writes the asset as DMGR (render/formats/de_cooked_material_graph.h): the model name, the
 *      exposed parameter layout (rows, components, texture slots, defaults and UI ranges) and
 *      the cooked shader;
 *   4. adds the sub-asset "<id>#material" (type `material`, DMAT): the defaults at their row
 *      positions, model_hash naming the model, the bound textures.
 *
 * Settings (all optional):
 *   model        the material model name, 1 to 31 bytes of [A-Za-z0-9_-]; default: the source
 *                file name up to its first '.'. Not a built-in model name.
 *   targets      as for `shader`: ["spirv", "dxil", "metallib"]; default by platform.
 *   textures     ["<param>=<asset id>", ...]: the texture asset of a texture parameter.
 *   alpha_mode   "opaque" (default), "mask" or "blend" for the default material.
 *   double_sided bool, for the default material.
 *
 * Determinism: the output depends only on the graph, the settings, the engine shader sources and
 * the Slang version (the importer version hashes the last two).
 */
#ifndef DE_COOK_MG_IMPORT_H
#define DE_COOK_MG_IMPORT_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** Bump on any change of the importer's output. de_mg_importer_version() mixes it with the shader
 * importer's version and the engine shader sources. */
#define DE_MG_IMPORTER_BASE_VERSION 2u

/** The importer version: base version, shader importer version, engine shader root contents.
 * Computed once per process. Never 0. */
u32 de_mg_importer_version(void);

/** The importer (de_asset_type_desc.import). */
de_result de_mg_import(de_asset_import_ctx *ctx);

/** The type description: name "material_graph", no extensions (name the type in the manifest). */
de_asset_type_desc de_mg_import_type_desc(void);

/** The engine shader root the importer reads engine modules from. */
const char *de_mg_engine_shader_root(void);

DE_EXTERN_C_END

#endif /* DE_COOK_MG_IMPORT_H */
