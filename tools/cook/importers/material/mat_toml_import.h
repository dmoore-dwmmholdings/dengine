/*
 * mat_toml_import.h: the `material` importer for `.mat.toml` files (DENG-302, SPEC §11.2, §12.6):
 * a material written by hand, cooked to DMAT (render/formats/de_cooked_material.h) like the
 * materials the model importer writes. Cooker-library code (de_cooker): the tools, and Debug and
 * Dev builds (DE_HAS_COOKER), never Release or Shipping. Include as
 * "cook/importers/material/mat_toml_import.h".
 *
 * The manifest names the type (examples/assets.toml):
 *
 *     "env/stone" = { type = "material", source = "materials/stone.mat.toml" }
 *
 * Source: top-level keys only, named after the fields of de_standard_material (de_render.h):
 *
 *     model = "standard"                 # "standard" (default), "unlit", "sprite", "terrain",
 *                                        # "water" or a game model's name (registered at run time)
 *     base_color = [0.5, 0.5, 0.5]       # 3 or 4 numbers (alpha 1); default white
 *     metallic = 0.0                     # every scalar field: a number
 *     roughness = 0.8
 *     emissive = [0.0, 0.0, 0.0]         # every colour / vector field: 3 numbers
 *     alpha_mode = "mask"                # "opaque", "mask", "blend"
 *     double_sided = true                # booleans: double_sided, unlit
 *     base_color_tex = "env/stone_albedo"   # a texture by asset ID (a cook dependency)
 *     normal_tex = "env/stone_normal"
 *     normal_texcoord = 1                # the UV set of a texture slot (0 or 1)
 *
 * Texture keys are `<slot>_tex` for the de_cooked_tex_slot slots (base_color, metal_rough, normal,
 * occlusion, emissive, clearcoat, clearcoat_roughness, clearcoat_normal, sheen_color,
 * sheen_roughness, specular, specular_color, transmission, thickness, diffuse_transmission,
 * diffuse_transmission_color, anisotropy, iridescence, iridescence_thickness), with
 * `<slot>_texcoord`. The cooked parameters without a de_standard_material field are accepted too:
 * occlusion_strength, clearcoat_normal_scale. Missing keys take the glTF defaults
 * (de_cooked_material_params_default). Any other key, a table, or a value of the wrong type fails
 * the import, and the message lists every unknown key with its line.
 *
 * A source that is already a cooked material (it starts with the DMAT magic) is checked and passed
 * through unchanged, with its textures as dependencies.
 *
 * Output: DMAT version 2: the flags (double-sided, unlit, one bit per KHR extension whose keys the
 * file uses), the alpha mode, the model name's hash (0 for "standard" and "unlit"), the parameters
 * and the textures sorted by slot. It depends only on the values, never on the key order or the
 * formatting, so the bytes are the same on every run and host. Every texture is a dependency.
 */
#ifndef DE_COOK_MAT_TOML_IMPORT_H
#define DE_COOK_MAT_TOML_IMPORT_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** Bump on any change of the importer's output. */
#define DE_MAT_TOML_IMPORTER_VERSION 1u

/** The importer (de_asset_type_desc.import). */
de_result de_mat_toml_import(de_asset_import_ctx *ctx);

/** The `material` type with this importer (no extensions: the manifest names the type). */
de_asset_type_desc de_mat_toml_import_type_desc(void);

DE_EXTERN_C_END

#endif /* DE_COOK_MAT_TOML_IMPORT_H */
