/*
 * texture_bc.h: block compression of the texture importer (DENG-99, SPEC §11.6, §11.2): the
 * `compression` rules (texture_bc_rules.c) and the BC1/BC3/BC4/BC5/BC6H/BC7 encoders
 * (texture_bc.c, with the C++ encoders behind texture_bc_enc.cpp). Private to
 * tools/cook/importers/texture/ and its tests.
 *
 * Encoders: bc7enc (BC7) and rgbcx (BC1, BC3, BC4, BC5) from the vendored bc7enc_rdo, with its
 * ert pass for `rdo_quality`; BC6H through the BC6H encoder of the vendored Basis Universal
 * transcoder (astc_6x6_hdr::fast_encode_bc6h). See the Trama page "Decision: BC encoders are
 * bc7enc_rdo and the Basis Universal BC6H encoder; compression = auto picks by usage".
 */
#ifndef DE_TEXTURE_BC_H
#define DE_TEXTURE_BC_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>

#include "render/formats/de_cooked_texture.h"
#include "texture_internal.h"

DE_EXTERN_C_BEGIN

/* ---- Rules (texture_bc_rules.c) -------------------------------------------------------------- */

/** The `compression` setting. */
typedef enum de_tex_compression {
    DE_TEX_COMPRESSION_AUTO = 0, /**< by usage and source (the rule table) */
    DE_TEX_COMPRESSION_NONE,     /**< RGBA8 (sRGB or UNORM) or RGBA16F */
    DE_TEX_COMPRESSION_BC1,
    DE_TEX_COMPRESSION_BC3,
    DE_TEX_COMPRESSION_BC4,
    DE_TEX_COMPRESSION_BC5,
    DE_TEX_COMPRESSION_BC6H,
    DE_TEX_COMPRESSION_BC7,
    DE_TEX_COMPRESSION_COUNT
} de_tex_compression;

/** Parses a `compression` value ("auto", "none", "bc1", "bc3", "bc4", "bc5", "bc6h", "bc7"). */
bool de_tex_compression_parse(const char *s, de_tex_compression *out);
/** The setting text of a value ("auto", ...); "?" past the enum. */
const char *de_tex_compression_name(de_tex_compression c);

/** Parses a `quality` value ("fast", "normal", "high"). */
bool de_tex_quality_parse(const char *s, de_tex_quality *out);

/** What the rules know about the source. */
typedef enum de_tex_source_class {
    DE_TEX_SOURCE_CLASS_LDR = 0,    /**< 8- or 16-bit source with 2 to 4 stored channels */
    DE_TEX_SOURCE_CLASS_LDR_SINGLE, /**< 8- or 16-bit source with one stored channel (grey) */
    DE_TEX_SOURCE_CLASS_HDR,        /**< float source (EXR, float KTX2): linear, maybe above 1 */
    DE_TEX_SOURCE_CLASS_COUNT
} de_tex_source_class;

/** The inputs of the format choice. */
typedef struct de_tex_format_query {
    de_tex_compression compression;
    de_cooked_texture_usage usage;
    bool srgb; /**< resolved `srgb` (false for float sources, normal maps and environments) */
    bool pixel_art;
    de_tex_source_class source;
    bool has_alpha;     /**< some texel of mip 0 has alpha below 1 */
    bool block_aligned; /**< width and height are multiples of 4 (BC needs it for mip 0) */
} de_tex_format_query;

/** Why a choice differs from what was asked (bits of de_tex_format_choice.notes). */
enum {
    DE_TEX_NOTE_DROPS_ALPHA = 1u << 0, /**< the format stores no alpha, and the source has some */
    DE_TEX_NOTE_DROPS_CHANNELS = 1u << 1, /**< BC4 keeps red only, BC5 red and green */
    DE_TEX_NOTE_CLAMPS_HDR = 1u << 2, /**< a float source in an 8-bit format: clamped to [0, 1] */
    DE_TEX_NOTE_LINEAR_BC1_BC3 = 1u << 3, /**< BC1/BC3 asked for linear data: the engine has only
                                               the sRGB formats, so BC7 UNORM is used */
    DE_TEX_NOTE_UNALIGNED = 1u << 4,      /**< mip 0 is not a multiple of 4: uncompressed */
    DE_TEX_NOTE_SRGB_IGNORED = 1u << 5    /**< BC4, BC5 and BC6H have no sRGB variant */
};

/** The result of the format choice. */
typedef struct de_tex_format_choice {
    de_gpu_format format;
    de_tex_compression compression; /**< what the format is: never AUTO */
    u32 notes;                      /**< DE_TEX_NOTE_* */
} de_tex_format_choice;

/**
 * The cooked format for a texture. `auto` follows the rule table (first match wins):
 *
 *   pixel_art                              -> none (RGBA8, or RGBA16F for float sources)
 *   float source, or usage = environment   -> BC6H unsigned float
 *   usage = normal                         -> BC5 (x and y; z is rebuilt in the shader)
 *   usage = mask, one stored channel       -> BC4
 *   usage = mask, more channels            -> BC7 UNORM (packed masks keep every channel)
 *   usage = color                          -> BC7, sRGB when `srgb`
 *
 * An explicit value is honoured, with notes where it loses data. `none` gives RGBA16F for float
 * sources and environments, else RGBA8 sRGB or UNORM. A texture whose mip 0 is not a multiple of
 * 4 texels on both axes is not block-compressed (DE_TEX_NOTE_UNALIGNED). Pure; any thread.
 */
de_tex_format_choice de_tex_bc_choose(const de_tex_format_query *q);

/** The source class of a decoded image. */
de_tex_source_class de_tex_source_class_of(const de_tex_image *img);

/** de_tex_bc_choose for an image (mip 0, as it is cooked) and the settings: the query's alpha,
 * block alignment and source class come from the image. An unknown `compression` counts as
 * "auto" (the importer refuses it before). */
de_tex_format_choice de_tex_bc_choose_image(const de_tex_settings *s, const de_tex_image *img);

/** The format choice for an environment cube (DENG-100): `usage = environment`, linear, no
 * alpha, faces of `face_size` texels (a power of two of at least 4, so always block-aligned).
 * `auto` gives BC6H; `none` gives RGBA16F. */
de_tex_format_choice de_tex_bc_choose_env(const de_tex_settings *s, const de_tex_image *img,
                                          u32 face_size);

/* ---- Encoders (texture_bc.c) ----------------------------------------------------------------- */

/** True for the BC formats this file encodes (BC1 sRGB, BC3 sRGB, BC4, BC5, BC6H, BC7). */
bool de_tex_bc_is_bc(de_gpu_format format);

/**
 * Encodes one subresource into BC blocks: `rgba` is width x height f32 RGBA (sRGB formats get
 * linear values and encode them as sRGB bytes; UNORM formats get values in [0, 1]; BC6H gets
 * linear values, negatives clamp to 0). Mips and edges that do not fill a 4x4 block are padded
 * by repeating the last column and row. Row r of blocks goes to `dst + r * row_pitch`; bytes past
 * a row's blocks are not written.
 *
 * Blocks are encoded in fixed ranges of DE_TEX_BC_GRAIN blocks, on `opts->tasks` when it is not
 * NULL (de_task_parallel_for). Each range has its own encoder state and its own rate-distortion
 * pass, so the output is the same for any worker count. Scratch comes from `a` (calling thread).
 * @return DE_OK; DE_ERR_UNSUPPORTED (not a BC format); DE_ERR_OUT_OF_MEMORY.
 */
de_result de_tex_bc_encode(const de_allocator *a, de_gpu_format format, const f32 *rgba, u32 width,
                           u32 height, u8 *dst, u32 row_pitch, const de_tex_encode_opts *opts);

/** Blocks per range of de_tex_bc_encode: part of the output's definition when the
 * rate-distortion pass is on (it looks back only inside a range). Never change it without
 * bumping DE_TEXTURE_IMPORTER_VERSION. */
#define DE_TEX_BC_GRAIN 256u

/* ---- C++ encoders (texture_bc_enc.cpp) ------------------------------------------------------- */

/** Encoder parameters of one range, from the options. */
typedef struct de_tex_bc_params {
    de_tex_quality quality;
    f32 rdo_lambda;  /**< 0 = no rate-distortion pass */
    bool perceptual; /**< BC7: weight errors for sRGB colour */
} de_tex_bc_params;

/** Starts the encoders' global tables once (thread-safe; every entry below calls it). */
void de_tex_bc_shim_start(void);

/**
 * Encodes `count` blocks. `pixels` holds count x 16 texels in raster order inside each block:
 * RGBA8 (4 bytes a texel) for BC1/BC3/BC4/BC5/BC7, where BC4 reads red and BC5 red and green;
 * RGB half floats (3 u16 a texel) for BC6H. `out` gets count blocks, tight. Then, with
 * `p->rdo_lambda > 0`, the rate-distortion pass runs over these blocks only (BC1 to BC5, BC7).
 * Returns false only when the C++ code threw (out of memory).
 */
bool de_tex_bc_shim_encode(de_gpu_format format, const void *pixels, u32 count, u8 *out,
                           const de_tex_bc_params *p);

DE_EXTERN_C_END

#endif /* DE_TEXTURE_BC_H */
