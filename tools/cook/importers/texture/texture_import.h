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
 *     renormalized), "mask" (linear data), "environment" (HDR environment: a cube with GGX
 *     prefiltered mips and SH9 irradiance, render/formats/de_cooked_env.h; DENG-100).
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
 *   - `max_decoded_mib` (integer, MiB, default 1024, 1 to 2^20; DENG-106): the largest decoded
 *     image one import may hold: f32 RGBA of every layer and face (16 bytes per texel), or the
 *     stored blocks of a BC pass-through. The decoders check it from the file's header, before
 *     they allocate; an image over it fails with DE_ERR_UNSUPPORTED. The default takes an 8192 x
 *     8192 image; a 16384 x 16384 one needs 4096.
 *   - `compression` (string, default "auto"): "auto", "none", "bc1", "bc3", "bc4", "bc5",
 *     "bc6h" or "bc7" (DENG-99). "auto" picks by usage and source: BC7 for colour (sRGB when
 *     `srgb`), BC5 for normal maps, BC4 for one-channel masks (BC7 UNORM for packed masks), BC6H
 *     for float sources and environments, uncompressed for pixel art. "none" cooks RGBA8 UNORM or
 *     sRGB, or RGBA16F for float sources. A choice that drops data logs a warning (texture_bc.h
 *     has the full rules). Textures whose size is not a multiple of 4 cook uncompressed.
 *   - `quality` (string, default "normal"): "fast", "normal" or "high", the BC encoders' effort.
 *   - `rdo_quality` (float 0 to DE_TEXTURE_RDO_QUALITY_MAX, default 0 = off): the lambda of the
 *     rate-distortion pass of BC1/BC3/BC4/BC5/BC7, which makes blocks compress better in the pack
 *     at some loss of quality (BC6H ignores it).
 *   - `rt_proxy` (bool, default false): cook an opaque silhouette extrusion for sprite RT.
 *     Holes/components are preserved within 32 vertices and 64 triangles. Unsupported topology
 *     logs a warning and omits the proxy; runtime sprites retain alpha-tested geometry.
 *
 * Environment settings (`usage = "environment"` only; DENG-100, env_internal.h):
 *   - `env_size` (integer, default 256): cube face size, a power of two from 4 to 4096.
 *   - `env_samples` (integer, default 1024): GGX samples per prefiltered texel, 1 to 65536.
 *   - `rotation_deg` (float, default 0): rotation of the environment about +Z, counter-clockwise
 *     seen from above.
 *   - `clamp_luminance` (float > 0, optional): firefly clamp of the prefilter input.
 * The source is equirectangular (2:1), a cube cross (4:3, 3:4) or a KTX2 cube. `mips`,
 * `max_size`, `pixel_art`, `alpha_coverage` and `address` do not apply.
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

/** Bump on any change of the importer's output: cached cooks of older versions are not reused.
 * 3: HDR environments (DENG-100). 4: BC compression (DENG-99).
 * 5: topology-preserving sprite proxies and alpha-tested fallback (DENG-488). */
#define DE_TEXTURE_IMPORTER_VERSION 5u

/** The largest `rdo_quality` (lambda of the rate-distortion pass). */
#define DE_TEXTURE_RDO_QUALITY_MAX 100.0

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
