/*
 * texture_import.h: the `texture` importer (DENG-98, SPEC §11.6): PNG, JPEG, OpenEXR and KTX2 to
 * the cooked texture format DTEX (render/formats/de_cooked_texture.h) with a linear-space mip
 * chain. Cooker-library code: it is compiled into `de_cooker`, so it exists in the tools and in
 * Debug and Dev builds (DE_HAS_COOKER), never in Release or Shipping. Include as
 * "cook/importers/texture/texture_import.h".
 *
 * Register it with the cooker (or, in the runtime, as de_asset_type_desc.import under
 * `#if DE_HAS_COOKER`, DENG-105):
 *
 *     de_asset_type_desc t = {.name = "texture", .import = de_texture_import,
 *                             .importer_version = DE_TEXTURE_IMPORTER_VERSION,
 *                             .extensions = de_texture_import_extensions,
 *                             .extension_count = DE_TEXTURE_IMPORT_EXTENSION_COUNT};
 *
 * The source kind comes from the file's first bytes, not from its extension.
 *
 * Settings (the manifest asset and `[defaults.texture]`; every setting is in the settings hash,
 * so a change re-cooks):
 *   - `usage` (string, default "color"): "color", "normal" (tangent-space normal map; mips are
 *     renormalized), "mask" (linear data), "environment" (HDR environment; DENG-100 prefilters it,
 *     until then a plain RGBA16F texture).
 *   - `srgb` (bool): the colour channels are sRGB-encoded, and the cooked format is sRGB. Default:
 *     true for "color" with an 8- or 16-bit source (a KTX2 file's transfer function decides for
 *     KTX2), false otherwise. Ignored (with a warning) for float sources and normal maps.
 *   - `mips` (bool, default true): the full chain down to 1x1.
 *   - `pixel_art` (bool, default false): no mips, and the DTEX flag that asks for nearest
 *     sampling (SPEC §12.12).
 *   - `alpha_coverage` (float in (0, 1), optional): an alpha-test cutoff; every mip keeps mip 0's
 *     share of texels at or above it.
 *   - `max_size` (integer 1 to 16384, optional): the longest edge is scaled down to it.
 *   - `address` (string, default "wrap"): "wrap" or "clamp", how the mip filter reads past edges.
 *   - `compression` (string, default "auto"): read by the BC stage (DENG-99). Until then every
 *     texture cooks uncompressed (RGBA8 UNORM or sRGB, RGBA16F for float sources), which is what
 *     "none" asks for; another value logs a warning.
 *
 * KTX2 files with BC payloads are passed through with their mips (no re-encoding); ETC1S and
 * UASTC are transcoded at cook time (Basis Universal) and filtered like any other source.
 *
 * Errors: DE_ERR_INVALID_ARGUMENT (a bad setting), DE_ERR_PARSE (a damaged or unknown file),
 * DE_ERR_UNSUPPORTED (a size over 16384, an unsupported pixel format or file feature),
 * DE_ERR_OUT_OF_MEMORY. Each comes with a message.
 */
#ifndef DE_COOK_TEXTURE_IMPORT_H
#define DE_COOK_TEXTURE_IMPORT_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

/** Bump on any change of the importer's output: cached cooks of older versions are not reused. */
#define DE_TEXTURE_IMPORTER_VERSION 1u

#define DE_TEXTURE_IMPORT_EXTENSION_COUNT 5u
/** ".png", ".jpg", ".jpeg", ".exr", ".ktx2". */
extern const char *const de_texture_import_extensions[DE_TEXTURE_IMPORT_EXTENSION_COUNT];

/** The importer (de_asset_type_desc.import). */
de_result de_texture_import(de_asset_import_ctx *ctx);

/** The texture type with this importer and no loader: what `de-cook` registers when the
 * project's asset system has no `texture` importer (the runtime loader is DENG-105). */
de_asset_type_desc de_texture_import_type_desc(void);

DE_EXTERN_C_END

#endif /* DE_COOK_TEXTURE_IMPORT_H */
