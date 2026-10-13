/*
 * de_cooked_texture.h: the cooked texture format DTEX, version 1 (SPEC §11.6; DENG-98). Internal;
 * include as "render/formats/de_cooked_texture.h".
 *
 * A `texture` asset in upload-ready layout. The texture importer (tools/cook/importers/texture/)
 * writes it; the runtime texture loader (DENG-105) copies each subresource straight from the
 * staging ring with de_gpu_cmd_copy_buffer_to_texture. The layout follows the D3D12 copy rules,
 * which also satisfy Vulkan and Metal: every row pitch is a multiple of 256 bytes
 * (DE_COOKED_TEXTURE_ROW_ALIGN) and every subresource starts at a multiple of 512 bytes
 * (DE_COOKED_TEXTURE_SUBRESOURCE_ALIGN) from the start of the blob. A loader that places the blob
 * at a 512-aligned staging offset can copy every subresource without repacking.
 *
 * Every field is little-endian (de_cooked_le.h). Layout:
 *
 *   header        DE_COOKED_TEXTURE_HEADER_SIZE (64) bytes:
 *                    0  u8[4] magic "DTEX"
 *                    4  u32   format version (DE_COOKED_TEXTURE_VERSION)
 *                    8  u32   dimension (de_gpu_texture_dim: 2D, 2D_ARRAY or CUBE; 3D is not
 *                             used by version 1)
 *                   12  u32   pixel format (de_gpu_format)
 *                   16  u32   width  (1 .. DE_COOKED_TEXTURE_MAX_SIZE)
 *                   20  u32   height (1 .. DE_COOKED_TEXTURE_MAX_SIZE)
 *                   24  u32   depth_or_layers: array layers; for CUBE 6 x cubes (faces +X -X +Y
 *                             -Y +Z -Z per cube); 1 for 2D
 *                   28  u32   mip_count (1 .. the full chain to 1x1)
 *                   32  u32   flags (DE_COOKED_TEXTURE_SRGB, _CUBE, _PIXEL_ART, _HAS_ALPHA)
 *                   36  u32   usage (de_cooked_texture_usage)
 *                   40  u32   subresource count S = depth_or_layers x mip_count
 *                   44  u32   reserved, 0
 *                   48  u64   total size of the blob in bytes
 *                   56  u64   reserved, 0
 *   subresources  S x DE_COOKED_TEXTURE_SUBRESOURCE_SIZE (24) bytes, in storage order: for each
 *                 layer (or face) in order, its mips from largest to smallest. Subresource i is
 *                 layer i / mip_count, mip i % mip_count (the D3D12 subresource index).
 *                   0  u64   offset of the data from the start of the blob (multiple of 512)
 *                   8  u64   size in bytes (row_pitch x row_count)
 *                  16  u32   row pitch in bytes (multiple of 256, at least one row of the mip)
 *                  20  u32   row count: pixel rows, or block rows for block-compressed formats
 *   payload       the subresources, in table order, each at its offset. Padding between them and
 *                 at the end of each row is zero, so a cook is byte-identical.
 *
 * The mip sizes are not stored: mip m of a W x H texture is max(1, W >> m) x max(1, H >> m). A
 * block-compressed mip covers ceil(w / 4) x ceil(h / 4) blocks.
 *
 * Bump DE_COOKED_TEXTURE_VERSION on any change.
 */
#ifndef DE_RENDER_FORMATS_COOKED_TEXTURE_H
#define DE_RENDER_FORMATS_COOKED_TEXTURE_H

#include "render/formats/de_cooked_le.h"

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>

DE_EXTERN_C_BEGIN

#define DE_COOKED_TEXTURE_MAGIC             "DTEX"
#define DE_COOKED_TEXTURE_VERSION           1u
#define DE_COOKED_TEXTURE_HEADER_SIZE       64u
#define DE_COOKED_TEXTURE_SUBRESOURCE_SIZE  24u
#define DE_COOKED_TEXTURE_ROW_ALIGN         256u
#define DE_COOKED_TEXTURE_SUBRESOURCE_ALIGN 512u
/** The largest width or height (and the import limit). */
#define DE_COOKED_TEXTURE_MAX_SIZE 16384u
/** The most mips: the full chain of a 16384 texture. */
#define DE_COOKED_TEXTURE_MAX_MIPS 15u
/** The most array layers (cube faces count one each). */
#define DE_COOKED_TEXTURE_MAX_LAYERS 2048u

/** Header flags. */
enum {
    DE_COOKED_TEXTURE_SRGB = 1u << 0,      /**< the format is an sRGB format */
    DE_COOKED_TEXTURE_CUBE = 1u << 1,      /**< a cube map (dimension CUBE) */
    DE_COOKED_TEXTURE_PIXEL_ART = 1u << 2, /**< sample with nearest filtering (SPEC §12.12) */
    DE_COOKED_TEXTURE_HAS_ALPHA = 1u << 3  /**< some texel of mip 0 has alpha below 1 */
};
#define DE_COOKED_TEXTURE_FLAGS_KNOWN 0xFu

/** What the texture holds (the importer's `usage` setting). */
typedef enum de_cooked_texture_usage {
    DE_COOKED_TEXTURE_USAGE_COLOR = 0,      /**< colour (sRGB unless `srgb = false`) */
    DE_COOKED_TEXTURE_USAGE_NORMAL = 1,     /**< tangent-space normal map, xyz in rgb */
    DE_COOKED_TEXTURE_USAGE_MASK = 2,       /**< linear data (roughness, occlusion, masks) */
    DE_COOKED_TEXTURE_USAGE_ENVIRONMENT = 3 /**< HDR environment: a prefiltered cube followed by
                                                 an ENVS trailer (de_cooked_env.h, DENG-100) */
} de_cooked_texture_usage;
#define DE_COOKED_TEXTURE_USAGE_COUNT 4u

/** Block size of a format: 1x1 for plain formats, 4x4 for BC. */
typedef struct de_cooked_texture_block {
    u32 width, height; /**< texels per block */
    u32 bytes;         /**< bytes per block */
} de_cooked_texture_block;

/**
 * The block layout of a colour format DTEX may hold. False for DE_FMT_UNKNOWN, depth formats and
 * values past the enum.
 */
static inline bool de_cooked_texture_format_block(u32 format, de_cooked_texture_block *out) {
    u32 w = 1, h = 1, b = 0;
    switch (format) {
    case DE_FMT_R8_UNORM:
        b = 1;
        break;
    case DE_FMT_RG8_UNORM:
    case DE_FMT_R16_FLOAT:
        b = 2;
        break;
    case DE_FMT_RGBA8_UNORM:
    case DE_FMT_RGBA8_SRGB:
    case DE_FMT_BGRA8_UNORM:
    case DE_FMT_BGRA8_SRGB:
    case DE_FMT_RG16_FLOAT:
    case DE_FMT_R32_UINT:
    case DE_FMT_R32_FLOAT:
    case DE_FMT_R11G11B10_FLOAT:
    case DE_FMT_RGB10A2_UNORM:
        b = 4;
        break;
    case DE_FMT_RGBA16_FLOAT:
    case DE_FMT_RG32_UINT:
    case DE_FMT_RG32_FLOAT:
        b = 8;
        break;
    case DE_FMT_RGBA32_FLOAT:
        b = 16;
        break;
    case DE_FMT_BC1_SRGB:
    case DE_FMT_BC4_UNORM:
        w = h = 4;
        b = 8;
        break;
    case DE_FMT_BC3_SRGB:
    case DE_FMT_BC5_UNORM:
    case DE_FMT_BC6H_UFLOAT:
    case DE_FMT_BC7_UNORM:
    case DE_FMT_BC7_SRGB:
        w = h = 4;
        b = 16;
        break;
    default:
        return false;
    }
    if (out != NULL) {
        out->width = w;
        out->height = h;
        out->bytes = b;
    }
    return true;
}

/** True for the sRGB formats. */
static inline bool de_cooked_texture_format_is_srgb(u32 format) {
    return format == DE_FMT_RGBA8_SRGB || format == DE_FMT_BGRA8_SRGB ||
           format == DE_FMT_BC1_SRGB || format == DE_FMT_BC3_SRGB || format == DE_FMT_BC7_SRGB;
}

/** Size of mip `mip` along an axis of `size` texels: max(1, size >> mip). */
static inline u32 de_cooked_texture_mip_extent(u32 size, u32 mip) {
    u32 v = mip < 32u ? size >> mip : 0u;
    return v != 0u ? v : 1u;
}

/** The number of mips of the full chain of a w x h texture (down to 1x1). */
static inline u32 de_cooked_texture_full_mip_count(u32 w, u32 h) {
    u32 m = w > h ? w : h, n = 1;
    while (m > 1u) {
        m >>= 1;
        ++n;
    }
    return n;
}

/** Tight bytes of one row (of pixels or blocks) of a mip `width` texels wide. */
static inline u64 de_cooked_texture_row_bytes(const de_cooked_texture_block *b, u32 width) {
    return (u64)((width + b->width - 1u) / b->width) * b->bytes;
}

/** Rows (of pixels or blocks) of a mip `height` texels high. */
static inline u32 de_cooked_texture_row_count(const de_cooked_texture_block *b, u32 height) {
    return (height + b->height - 1u) / b->height;
}

/** `v` rounded up to a multiple of `a` (a power of two). */
static inline u64 de_cooked_texture_align(u64 v, u64 a) {
    return (v + a - 1u) & ~(a - 1u);
}

/** One subresource: where its rows are. */
typedef struct de_cooked_texture_subresource {
    u64 offset;    /**< from the start of the blob; a multiple of 512 */
    u64 size;      /**< row_pitch x row_count */
    u32 row_pitch; /**< a multiple of 256 */
    u32 row_count; /**< pixel rows, or block rows */
    u32 width;     /**< texels (derived from the header and the mip) */
    u32 height;
} de_cooked_texture_subresource;

/** A checked DTEX blob. The subresource table stays in the blob; read it with
 * de_cooked_texture_subresource_at. */
typedef struct de_cooked_texture_view {
    u32 dim;    /**< de_gpu_texture_dim */
    u32 format; /**< de_gpu_format */
    u32 width, height, depth_or_layers, mip_count;
    u32 flags;
    u32 usage; /**< de_cooked_texture_usage */
    u32 subresource_count;
    de_cooked_texture_block block;
    u64 size; /**< the blob size */
    const u8 *blob;
} de_cooked_texture_view;

/**
 * Subresource `index` (layer index / mip_count, mip index % mip_count) of a view that
 * de_cooked_texture_parse accepted. False when `index` is out of range.
 */
static inline bool de_cooked_texture_subresource_at(const de_cooked_texture_view *v, u32 index,
                                                    de_cooked_texture_subresource *out) {
    if (v == NULL || v->blob == NULL || index >= v->subresource_count)
        return false;
    const u8 *e =
        v->blob + DE_COOKED_TEXTURE_HEADER_SIZE + (u64)index * DE_COOKED_TEXTURE_SUBRESOURCE_SIZE;
    u32 mip = index % v->mip_count;
    out->offset = de_cooked_get_u64(e);
    out->size = de_cooked_get_u64(e + 8);
    out->row_pitch = de_cooked_get_u32(e + 16);
    out->row_count = de_cooked_get_u32(e + 20);
    out->width = de_cooked_texture_mip_extent(v->width, mip);
    out->height = de_cooked_texture_mip_extent(v->height, mip);
    return true;
}

/* The checks of de_cooked_texture_parse: NULL when the blob is valid, else the reason. Written
 * without goto so the header also compiles as C++ (texture_basis.cpp includes it). */
static inline const char *de__cooked_texture_check(const u8 *p, u64 size,
                                                   de_cooked_texture_view *out) {
    if (p == NULL || size < DE_COOKED_TEXTURE_HEADER_SIZE)
        return "shorter than the header";
    if (memcmp(p, DE_COOKED_TEXTURE_MAGIC, 4) != 0)
        return "not a cooked texture (magic)";
    if (de_cooked_get_u32(p + 4) != DE_COOKED_TEXTURE_VERSION)
        return "unsupported format version";
    out->dim = de_cooked_get_u32(p + 8);
    out->format = de_cooked_get_u32(p + 12);
    out->width = de_cooked_get_u32(p + 16);
    out->height = de_cooked_get_u32(p + 20);
    out->depth_or_layers = de_cooked_get_u32(p + 24);
    out->mip_count = de_cooked_get_u32(p + 28);
    out->flags = de_cooked_get_u32(p + 32);
    out->usage = de_cooked_get_u32(p + 36);
    out->subresource_count = de_cooked_get_u32(p + 40);
    const u64 total = de_cooked_get_u64(p + 48);
    if (de_cooked_get_u32(p + 44) != 0u || de_cooked_get_u64(p + 56) != 0u)
        return "reserved header fields are not 0";
    if (!de_cooked_texture_format_block(out->format, &out->block))
        return "unknown pixel format";
    if (out->width == 0u || out->height == 0u || out->width > DE_COOKED_TEXTURE_MAX_SIZE ||
        out->height > DE_COOKED_TEXTURE_MAX_SIZE)
        return "bad width or height";
    if (out->mip_count == 0u ||
        out->mip_count > de_cooked_texture_full_mip_count(out->width, out->height))
        return "bad mip count";
    if (out->depth_or_layers == 0u || out->depth_or_layers > DE_COOKED_TEXTURE_MAX_LAYERS)
        return "bad layer count";
    const bool cube = (out->flags & DE_COOKED_TEXTURE_CUBE) != 0u;
    if (out->dim == (u32)DE_TEX_2D) {
        if (out->depth_or_layers != 1u || cube)
            return "a 2D texture has one layer and no cube flag";
    } else if (out->dim == (u32)DE_TEX_2D_ARRAY) {
        if (cube)
            return "an array texture has no cube flag";
    } else if (out->dim == (u32)DE_TEX_CUBE) {
        if (!cube || out->depth_or_layers % 6u != 0u || out->width != out->height)
            return "a cube needs the cube flag, square faces and 6 faces per cube";
    } else {
        return "unsupported dimension";
    }
    if ((out->flags & ~(u32)DE_COOKED_TEXTURE_FLAGS_KNOWN) != 0u ||
        ((out->flags & DE_COOKED_TEXTURE_SRGB) != 0u) !=
            de_cooked_texture_format_is_srgb(out->format))
        return "bad flags";
    if (out->usage >= DE_COOKED_TEXTURE_USAGE_COUNT)
        return "bad usage";
    if ((u64)out->subresource_count != (u64)out->depth_or_layers * out->mip_count)
        return "subresource count is not layers x mips";
    if (total != size)
        return "total size does not match the blob";
    const u64 table_end = (u64)DE_COOKED_TEXTURE_HEADER_SIZE +
                          (u64)out->subresource_count * DE_COOKED_TEXTURE_SUBRESOURCE_SIZE;
    if (table_end > size)
        return "subresource table out of range";
    out->blob = p;
    out->size = size;
    u64 next = table_end;
    for (u32 i = 0; i < out->subresource_count; ++i) {
        de_cooked_texture_subresource s;
        (void)de_cooked_texture_subresource_at(out, i, &s);
        const u64 row = de_cooked_texture_row_bytes(&out->block, s.width);
        if (s.offset % DE_COOKED_TEXTURE_SUBRESOURCE_ALIGN != 0u || s.offset < next)
            return "subresource offset not 512-aligned or out of order";
        if (s.row_pitch % DE_COOKED_TEXTURE_ROW_ALIGN != 0u || (u64)s.row_pitch < row ||
            s.row_count != de_cooked_texture_row_count(&out->block, s.height) ||
            s.size != (u64)s.row_pitch * s.row_count)
            return "bad row pitch, row count or size";
        if (!de_cooked_range_ok(size, s.offset, s.size))
            return "subresource out of range";
        next = s.offset + s.size;
    }
    return NULL;
}

/**
 * Checks a DTEX blob and fills `*out`: magic, version, dimension, format, sizes (at most
 * DE_COOKED_TEXTURE_MAX_SIZE, mips at most the full chain, cube faces), flags, usage, the total
 * size, and every subresource (512-aligned offset, 256-aligned pitch that holds a row, the row
 * count of its mip, size = pitch x rows, inside the blob, in order without overlap). The reserved
 * fields must be 0. Allocation-free. False on any error, with a short reason in `*why` (nullable).
 */
static inline bool de_cooked_texture_parse(const void *data, u64 size, de_cooked_texture_view *out,
                                           const char **why) {
    memset(out, 0, sizeof *out);
    const char *w = de__cooked_texture_check((const u8 *)data, size, out);
    if (w == NULL)
        return true;
    memset(out, 0, sizeof *out);
    if (why != NULL)
        *why = w;
    return false;
}

DE_EXTERN_C_END

#endif /* DE_RENDER_FORMATS_COOKED_TEXTURE_H */
