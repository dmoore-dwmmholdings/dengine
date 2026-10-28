/*
 * texture_internal.h: what the files of the texture importer share (DENG-98): the settings, the
 * decoded image, the mip chain, the encoders and the DTEX writer. Tool-private;
 * the Aseprite atlas stage also reuses the DTEX writer. The texture importer entry
 * is texture_import.h.
 *
 * The pipeline of one import (texture_import.c):
 *
 *   source bytes ─ sniff ─┬─ PNG / JPEG (texture_decode.c, stb_image) ─┐
 *                         ├─ EXR (texture_decode.c, tinyexr)           ├─ de_tex_image (f32 RGBA)
 *                         └─ KTX2 (texture_ktx2.c) ─┬─ raw formats ────┤
 *                                                   ├─ Basis ETC1S / UASTC (texture_basis.cpp)
 *                                                   └─ BC payloads ─ passed through with their
 *                                                                    mips (de_tex_blocks)
 *   de_tex_image ─ to linear ─ max_size ─ mips (texture_mips.c) ─ encode (texture_encode.c) ─ DTEX
 *
 * Memory: every buffer comes from the allocator passed in (the import's scratch allocator in the
 * importer, a tracking allocator in the tests). The third-party decoders allocate with malloc;
 * their output is copied and freed at once.
 */
#ifndef DE_TEXTURE_INTERNAL_H
#define DE_TEXTURE_INTERNAL_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_task.h>

#include "render/formats/de_cooked_texture.h"

DE_EXTERN_C_BEGIN

/* ---- Settings (texture_import.c) ------------------------------------------------------------- */

/** Encoder effort (`quality` setting, DENG-99): how hard the BC encoders search. The value 0 is
 * the default, so a zeroed struct asks for normal quality. */
typedef enum de_tex_quality {
    DE_TEX_QUALITY_NORMAL = 0,
    DE_TEX_QUALITY_FAST = 1,
    DE_TEX_QUALITY_HIGH = 2
} de_tex_quality;

/** How the filter reads past an edge (`address` setting). */
typedef enum de_tex_address { DE_TEX_ADDRESS_WRAP = 0, DE_TEX_ADDRESS_CLAMP = 1 } de_tex_address;

/** The import settings, after defaults and checks. */
typedef struct de_tex_settings {
    bool srgb;     /**< resolved: the source colour is sRGB-encoded and the output is sRGB */
    bool srgb_set; /**< `srgb` was given */
    bool mips;     /**< full chain to 1x1 */
    de_cooked_texture_usage usage;
    const char *compression; /**< "auto" by default; checked by de_tex_compression_parse and
                                  turned into a format by de_tex_bc_choose (texture_bc.h) */
    de_tex_quality quality;  /**< `quality`: "fast", "normal" (default) or "high" (DENG-99) */
    f32 rdo_quality;         /**< `rdo_quality`: lambda of the rate-distortion pass; 0 = off */
    bool pixel_art;          /**< no mips, nearest sampling flag */
    f32 alpha_coverage;      /**< alpha-test cutoff whose coverage the mips keep; 0 = off */
    u32 max_size;            /**< longest edge after import; 0 = no limit */
    de_tex_address address;
} de_tex_settings;

/* ---- Decoded images (texture_decode.c, texture_ktx2.c, texture_basis.cpp) -------------------- */

/** The kind of a source file, from its first bytes (never from its extension). */
typedef enum de_tex_source_kind {
    DE_TEX_SOURCE_UNKNOWN = 0,
    DE_TEX_SOURCE_PNG,
    DE_TEX_SOURCE_JPEG,
    DE_TEX_SOURCE_EXR,
    DE_TEX_SOURCE_KTX2
} de_tex_source_kind;

de_tex_source_kind de_tex_sniff(const void *data, u64 size);
const char *de_tex_source_kind_name(de_tex_source_kind k);

/**
 * A decoded image: `layers` images of width x height texels, f32 RGBA, rows top to bottom, layer
 * after layer. LDR sources hold their stored values in [0, 1] (still sRGB-encoded when they are
 * sRGB); HDR sources (EXR, float KTX2) hold linear values.
 */
typedef struct de_tex_image {
    u32 width, height;
    u32 layers; /**< array layers x faces (6 per cube) */
    bool cube;
    bool hdr;       /**< float source: linear values, may be outside [0, 1] */
    bool srgb_hint; /**< the file says sRGB (KTX2 DFD transfer function) */
    bool has_srgb_hint;
    u32 channels;   /**< channels the source stored (1 to 4); missing ones are filled */
    f32 *rgba;      /**< layers x height x width x 4 */
    u64 rgba_bytes; /**< size of the `rgba` block, for freeing */
} de_tex_image;

/** Frees the pixels of an image (from allocator `a`) and zeroes it. */
void de_tex_image_free(const de_allocator *a, de_tex_image *img);
/** Allocates zeroed pixels for an image whose sizes are set. DE_ERR_OUT_OF_MEMORY. */
de_result de_tex_image_alloc(const de_allocator *a, de_tex_image *img);

/** Checks a source width and height against DE_COOKED_TEXTURE_MAX_SIZE (DE_ERR_UNSUPPORTED). */
de_result de_tex_check_size(u32 width, u32 height, const char *what);

/** PNG (8/16 bit; grey, grey+alpha, RGB, RGBA, palette) and JPEG through stb_image. The size is
 * checked from the header before any pixel is decoded. DE_ERR_PARSE, DE_ERR_UNSUPPORTED. */
de_result de_tex_decode_stb(const de_allocator *a, const void *data, u64 size, de_tex_image *out);

/** OpenEXR (half, float or uint channels; R, G, B, A, Y) through tinyexr. The header is parsed
 * and the size checked before the pixels are decoded. DE_ERR_PARSE, DE_ERR_UNSUPPORTED. */
de_result de_tex_decode_exr(const de_allocator *a, const void *data, u64 size, de_tex_image *out);

/* ---- KTX2 (texture_ktx2.c) ------------------------------------------------------------------- */

/** Block-compressed subresources passed through unchanged (KTX2 with BC payloads). */
typedef struct de_tex_blocks {
    de_gpu_format format;
    u32 width, height;
    u32 layers; /**< array layers x faces */
    bool cube;
    bool has_alpha;
    u32 mip_count;
    /** layers x mip_count tight subresources (rows of blocks), layer after layer, mips largest
     * first: the order of DTEX. */
    const u8 **data;
    u64 *size;
    u64 data_bytes; /**< for freeing: the block holding every subresource */
    u8 *block;
    u64 table_bytes;
} de_tex_blocks;

void de_tex_blocks_free(const de_allocator *a, de_tex_blocks *b);

/** What a KTX2 file decodes to. */
typedef enum de_tex_ktx2_kind {
    DE_TEX_KTX2_IMAGE = 1, /**< raw formats or Basis payloads: a de_tex_image (level 0) */
    DE_TEX_KTX2_BLOCKS = 2 /**< BC payloads: de_tex_blocks */
} de_tex_ktx2_kind;

/**
 * Reads a KTX2 file: checks the header, the level index and the DFD, undoes zstd
 * supercompression, and either decodes level 0 of every layer and face (uncompressed formats;
 * ETC1S and UASTC through the Basis transcoder) into `*image`, or keeps every level of a BC
 * format in `*blocks`.
 * @return DE_OK; DE_ERR_PARSE (a damaged file); DE_ERR_UNSUPPORTED (3D textures, a format or
 *         supercompression without an engine equivalent, sizes over the limit);
 *         DE_ERR_OUT_OF_MEMORY.
 */
de_result de_tex_decode_ktx2(const de_allocator *a, const void *data, u64 size,
                             de_tex_ktx2_kind *kind, de_tex_image *image, de_tex_blocks *blocks);

/**
 * The Basis Universal transcoder (texture_basis.cpp, C++): transcodes level 0 of every layer and
 * face of a KTX2 file with ETC1S or UASTC LDR payloads to RGBA8 and stores it in `*out` as f32.
 * The caller has checked the KTX2 header. DE_ERR_PARSE, DE_ERR_UNSUPPORTED, DE_ERR_OUT_OF_MEMORY.
 */
de_result de_tex_basis_decode(const de_allocator *a, const void *data, u64 size, de_tex_image *out);

/** Starts the Basis Universal transcoder's global tables once (thread-safe). Its BC6H encoder
 * (texture_bc_enc.cpp, DENG-99) needs them too, so both files start it through this one call. */
void de_tex_basis_start(void);

/* ---- sRGB (texture_mips.c) ------------------------------------------------------------------- */

/** sRGB EOTF and its inverse on [0, 1] (values outside are clamped). Deterministic (de_dpow). */
f32 de_tex_srgb_to_linear(f32 c);
f32 de_tex_linear_to_srgb(f32 l);

/* ---- Mips (texture_mips.c) ------------------------------------------------------------------- */

/** A mip chain: mip_count levels of `layers` images each, f32 RGBA. */
typedef struct de_tex_chain {
    u32 layers;
    u32 mip_count;
    u32 width[DE_COOKED_TEXTURE_MAX_MIPS];
    u32 height[DE_COOKED_TEXTURE_MAX_MIPS];
    f32 *rgba[DE_COOKED_TEXTURE_MAX_MIPS]; /**< layers x height x width x 4 */
} de_tex_chain;

void de_tex_chain_free(const de_allocator *a, de_tex_chain *c);

/** How to build mips. */
typedef struct de_tex_mip_params {
    de_tex_address address;
    bool normal_map;    /**< rgb is a vector in [-1, 1]: renormalized in every mip */
    f32 alpha_coverage; /**< > 0: every mip keeps the alpha-test coverage of mip 0 at this cutoff */
    u32 alpha_levels;   /**< steps the output stores alpha with (255 for 8-bit UNORM, 0 = float):
                             coverage is measured on the stored values */
    bool full_chain;    /**< false: mip 0 only */
} de_tex_mip_params;

/**
 * Builds the chain from `base` (linear values; for a normal map unit vectors in rgb). Mip 0 is a
 * copy of `base` (renormalized for a normal map). Each next mip halves the previous one with
 * floor sizes (at least 1) through a separable Kaiser-windowed sinc (radius 3 output texels,
 * alpha 4), with weights computed once per (source size, destination size) pair, in f64 with
 * deterministic math, and edges read by `address`. The result does not depend on threads: the
 * filter runs on the calling thread in a fixed order. DE_ERR_OUT_OF_MEMORY.
 */
de_result de_tex_mips_build(const de_allocator *a, const de_tex_image *base,
                            const de_tex_mip_params *p, de_tex_chain *out);

/**
 * Resamples `layers` f32 RGBA images from sw x sh to dw x dh (each at most the source size) with
 * the mip filter. `dst` holds layers x dh x dw x 4 floats. DE_ERR_OUT_OF_MEMORY.
 */
de_result de_tex_resample(const de_allocator *a, const f32 *src, u32 sw, u32 sh, f32 *dst, u32 dw,
                          u32 dh, u32 layers, de_tex_address address);

/** The fraction of texels of one image with alpha at or above `cutoff`. */
f32 de_tex_alpha_coverage(const f32 *rgba, u32 width, u32 height, f32 cutoff);

/* ---- Encode (texture_encode.c) --------------------------------------------------------------- */

/** How the encoders work (DENG-99). `(de_tex_encode_opts){0}` is valid: normal quality, no
 * rate-distortion pass, everything on the calling thread. The output never depends on `tasks`. */
typedef struct de_tex_encode_opts {
    de_tex_quality quality;
    f32 rdo_quality;       /**< > 0: lambda of the rate-distortion pass of BC1/3/4/5/7 */
    de_task_system *tasks; /**< NULL = the calling thread */
} de_tex_encode_opts;

/** Picks the cooked format for an image and the settings (DENG-99 adds the BC rules here). */
de_gpu_format de_tex_choose_format(const de_tex_settings *s, const de_tex_image *img);

/** True when an encoder exists for `format`. */
bool de_tex_can_encode(de_gpu_format format);

/**
 * Encodes one subresource: `rgba` (width x height f32 RGBA, values as stored: sRGB formats get
 * linear values and encode them; UNORM formats get values in [0, 1]) into rows of the format at
 * `dst`, `row_pitch` bytes apart. Bytes past each row's end are not written (the DTEX writer
 * zero-fills them). DE_ERR_UNSUPPORTED when no encoder exists.
 */
de_result de_tex_encode(de_gpu_format format, const f32 *rgba, u32 width, u32 height, u8 *dst,
                        u32 row_pitch);
/** de_tex_encode with encoder options (DENG-99: quality, rate-distortion, tasks). NULL `opts`
 * means `(de_tex_encode_opts){0}`. Encoders allocate (the BC block buffers) from `a`. */
de_result de_tex_encode_ex(const de_allocator *a, de_gpu_format format, const f32 *rgba, u32 width,
                           u32 height, u8 *dst, u32 row_pitch, const de_tex_encode_opts *opts);

/** f32 to IEEE half (round to nearest even; NaN to 0; overflow to ±65504). */
u16 de_tex_f32_to_f16(f32 f);
/** IEEE half to f32 (exact). */
f32 de_tex_f16_to_f32(u16 h);

/* ---- DTEX writer (texture_encode.c) ---------------------------------------------------------- */

/** The header fields of a DTEX blob. */
typedef struct de_tex_dtex_desc {
    de_gpu_texture_dim dim;
    de_gpu_format format;
    u32 width, height, layers, mip_count;
    u32 flags; /**< DE_COOKED_TEXTURE_*; SRGB and CUBE are set from format and dim */
    de_cooked_texture_usage usage;
    de_tex_encode_opts encode; /**< DENG-99: encoder options (zero = defaults) */
} de_tex_dtex_desc;

/**
 * A zero-filled DTEX blob with header and subresource table written: the payload of
 * subresource i starts at `subresource[i]` with `row_pitch[i]`. The caller fills the payload.
 */
typedef struct de_tex_dtex {
    u8 *data;
    u64 size;
    u32 count;
    u64 *offset;     /**< count entries */
    u32 *row_pitch;  /**< count entries */
    u32 *row_count;  /**< count entries */
    u64 table_bytes; /**< for freeing */
} de_tex_dtex;

/** Lays out and allocates a DTEX blob (DE_ERR_UNSUPPORTED for a format DTEX cannot hold,
 * DE_ERR_OUT_OF_MEMORY). */
de_result de_tex_dtex_begin(const de_allocator *a, const de_tex_dtex_desc *d, de_tex_dtex *out);
void de_tex_dtex_free(const de_allocator *a, de_tex_dtex *t);

/** Encodes a whole chain into a new DTEX blob. */
de_result de_tex_dtex_from_chain(const de_allocator *a, const de_tex_dtex_desc *d,
                                 const de_tex_chain *chain, de_tex_dtex *out);
/** Copies pass-through blocks into a new DTEX blob. */
de_result de_tex_dtex_from_blocks(const de_allocator *a, const de_tex_dtex_desc *d,
                                  const de_tex_blocks *b, u32 first_mip, de_tex_dtex *out);

/* ---- Decoded-bytes limit (texture_decode.c; DENG-106) ---------------------------------------- */

/** Default of the `max_decoded_mib` setting: the largest decoded image of one import. */
#define DE_TEX_MAX_DECODED_MIB_DEFAULT 1024u
/** Largest `max_decoded_mib` value accepted (1 TiB). */
#define DE_TEX_MAX_DECODED_MIB_MAX (1u << 20)

/**
 * Carries the decoded-bytes limit of one import to the decoders: an allocator that forwards every
 * call to `parent` and holds `limit`. The importer passes `&alloc` as the allocator of every stage,
 * so the decoder signatures stay as they are. A decoder calls de_tex_limit_check with the size of
 * the image it is about to decode, before any pixel is allocated (by it or by a third-party
 * library).
 */
typedef struct de_tex_limit {
    de_allocator alloc; /**< forwards to `parent`; give this to the stages */
    const de_allocator *parent;
    u64 limit; /**< bytes */
} de_tex_limit;

/** Sets up `l` (`parent` NULL = the heap). Returns `&l->alloc`. */
const de_allocator *de_tex_limit_init(de_tex_limit *l, const de_allocator *parent, u64 limit);

/**
 * DE_OK when a decoded image of `bytes` (f32 RGBA, every layer and face; or the stored blocks of
 * a BC pass-through) fits the limit of `a`. An allocator that is not a de_tex_limit has no limit.
 * Else DE_ERR_UNSUPPORTED, with a message that names `what`, the size and the setting.
 */
de_result de_tex_limit_check(const de_allocator *a, u64 bytes, const char *what);

DE_EXTERN_C_END

#endif /* DE_TEXTURE_INTERNAL_H */
