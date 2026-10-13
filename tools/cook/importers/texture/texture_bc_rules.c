/*
 * texture_bc_rules.c: the `compression` rules of the texture importer (DENG-99, SPEC §11.2
 * "bc7 color, bc5 normals, bc4 masks, bc6h hdr", §11.6). Two tables: k_auto picks a compression
 * for `compression = "auto"`, k_formats turns a compression into a de_gpu_format. See texture_bc.h
 * for the rules in words.
 */
#include "texture_bc.h"

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>

#include <string.h>

/* ---- Setting values -------------------------------------------------------------------------- */

static const char *const k_compression_names[DE_TEX_COMPRESSION_COUNT] = {
    "auto", "none", "bc1", "bc3", "bc4", "bc5", "bc6h", "bc7"};

bool de_tex_compression_parse(const char *s, de_tex_compression *out) {
    if (s == NULL)
        return false;
    for (u32 i = 0; i < DE_TEX_COMPRESSION_COUNT; ++i)
        if (strcmp(s, k_compression_names[i]) == 0) {
            *out = (de_tex_compression)i;
            return true;
        }
    return false;
}

const char *de_tex_compression_name(de_tex_compression c) {
    return (u32)c < DE_TEX_COMPRESSION_COUNT ? k_compression_names[c] : "?";
}

bool de_tex_quality_parse(const char *s, de_tex_quality *out) {
    static const struct {
        const char *name;
        de_tex_quality q;
    } k[] = {{"fast", DE_TEX_QUALITY_FAST},
             {"normal", DE_TEX_QUALITY_NORMAL},
             {"high", DE_TEX_QUALITY_HIGH}};
    if (s == NULL)
        return false;
    for (u32 i = 0; i < (u32)(sizeof k / sizeof k[0]); ++i)
        if (strcmp(s, k[i].name) == 0) {
            *out = k[i].q;
            return true;
        }
    return false;
}

de_tex_source_class de_tex_source_class_of(const de_tex_image *img) {
    if (img->hdr)
        return DE_TEX_SOURCE_CLASS_HDR;
    return img->channels == 1u ? DE_TEX_SOURCE_CLASS_LDR_SINGLE : DE_TEX_SOURCE_CLASS_LDR;
}

/* ---- compression = "auto" -------------------------------------------------------------------- */

#define USAGE_BIT(u) (1u << (u))
#define ANY_USAGE                                                                                  \
    (USAGE_BIT(DE_COOKED_TEXTURE_USAGE_COLOR) | USAGE_BIT(DE_COOKED_TEXTURE_USAGE_NORMAL) |        \
     USAGE_BIT(DE_COOKED_TEXTURE_USAGE_MASK) | USAGE_BIT(DE_COOKED_TEXTURE_USAGE_ENVIRONMENT))
#define SOURCE_BIT(s) (1u << (s))
#define ANY_SOURCE                                                                                 \
    (SOURCE_BIT(DE_TEX_SOURCE_CLASS_LDR) | SOURCE_BIT(DE_TEX_SOURCE_CLASS_LDR_SINGLE) |            \
     SOURCE_BIT(DE_TEX_SOURCE_CLASS_HDR))

enum { PIXEL_ART_ANY = 0, PIXEL_ART_YES = 1, PIXEL_ART_NO = 2 };

/* One rule: it matches when the usage and the source class are in its masks and pixel_art fits.
 * The first matching rule wins; the last rule matches everything, so every combination of
 * (usage, srgb, source class, pixel_art) has an answer. `srgb` never picks the compression, only
 * the sRGB or UNORM variant (k_formats). */
static const struct auto_rule {
    u32 usages;
    u32 sources;
    u32 pixel_art;
    de_tex_compression result;
} k_auto[] = {
    /* Pixel art keeps exact texels: uncompressed unless the manifest names a format. */
    {ANY_USAGE, ANY_SOURCE, PIXEL_ART_YES, DE_TEX_COMPRESSION_NONE},
    /* Float sources and environments: BC6H, the one HDR BC format. */
    {ANY_USAGE, SOURCE_BIT(DE_TEX_SOURCE_CLASS_HDR), PIXEL_ART_ANY, DE_TEX_COMPRESSION_BC6H},
    {USAGE_BIT(DE_COOKED_TEXTURE_USAGE_ENVIRONMENT), ANY_SOURCE, PIXEL_ART_ANY,
     DE_TEX_COMPRESSION_BC6H},
    /* Normal maps: x and y in BC5's two channels; z = sqrt(1 - x^2 - y^2) in the shader. */
    {USAGE_BIT(DE_COOKED_TEXTURE_USAGE_NORMAL), ANY_SOURCE, PIXEL_ART_ANY, DE_TEX_COMPRESSION_BC5},
    /* A one-channel mask: BC4. A mask with more channels (packed occlusion, roughness, metal)
     * would lose all but red in BC4, so it takes BC7 UNORM. */
    {USAGE_BIT(DE_COOKED_TEXTURE_USAGE_MASK), SOURCE_BIT(DE_TEX_SOURCE_CLASS_LDR_SINGLE),
     PIXEL_ART_ANY, DE_TEX_COMPRESSION_BC4},
    {USAGE_BIT(DE_COOKED_TEXTURE_USAGE_MASK), ANY_SOURCE, PIXEL_ART_ANY, DE_TEX_COMPRESSION_BC7},
    /* Colour, also from a grey source (BC4 would sample as red without a swizzle): BC7. */
    {ANY_USAGE, ANY_SOURCE, PIXEL_ART_ANY, DE_TEX_COMPRESSION_BC7},
};

static de_tex_compression auto_compression(const de_tex_format_query *q) {
    u32 usage = USAGE_BIT((u32)q->usage), source = SOURCE_BIT((u32)q->source);
    u32 pixel_art = q->pixel_art ? PIXEL_ART_YES : PIXEL_ART_NO;
    for (u32 i = 0; i < (u32)(sizeof k_auto / sizeof k_auto[0]); ++i) {
        const struct auto_rule *r = &k_auto[i];
        if ((r->usages & usage) != 0 && (r->sources & source) != 0 &&
            (r->pixel_art == PIXEL_ART_ANY || r->pixel_art == pixel_art))
            return r->result;
    }
    return DE_TEX_COMPRESSION_BC7; /* not reached: the last rule matches everything */
}

/* ---- Compression -> format ------------------------------------------------------------------- */

/* Per compression: the format for sRGB data, the format for linear data, the channels it keeps
 * (4 = rgba, 3 = rgb, 2 = rg, 1 = r) and whether it holds HDR values. de_gpu.h has no linear BC1
 * or BC3, so linear data asked for in BC1 or BC3 uses BC7 UNORM (same or better quality, BC1
 * twice the size). */
static const struct format_row {
    de_gpu_format srgb;
    de_gpu_format linear;
    u32 channels;
    bool hdr;
} k_formats[DE_TEX_COMPRESSION_COUNT] = {
    [DE_TEX_COMPRESSION_AUTO] = {DE_FMT_UNKNOWN, DE_FMT_UNKNOWN, 0, false},
    [DE_TEX_COMPRESSION_NONE] = {DE_FMT_RGBA8_SRGB, DE_FMT_RGBA8_UNORM, 4, false},
    [DE_TEX_COMPRESSION_BC1] = {DE_FMT_BC1_SRGB, DE_FMT_BC7_UNORM, 3, false},
    [DE_TEX_COMPRESSION_BC3] = {DE_FMT_BC3_SRGB, DE_FMT_BC7_UNORM, 4, false},
    [DE_TEX_COMPRESSION_BC4] = {DE_FMT_BC4_UNORM, DE_FMT_BC4_UNORM, 1, false},
    [DE_TEX_COMPRESSION_BC5] = {DE_FMT_BC5_UNORM, DE_FMT_BC5_UNORM, 2, false},
    [DE_TEX_COMPRESSION_BC6H] = {DE_FMT_BC6H_UFLOAT, DE_FMT_BC6H_UFLOAT, 3, true},
    [DE_TEX_COMPRESSION_BC7] = {DE_FMT_BC7_SRGB, DE_FMT_BC7_UNORM, 4, false},
};

de_tex_format_choice de_tex_bc_choose(const de_tex_format_query *q) {
    de_tex_format_choice c;
    memset(&c, 0, sizeof c);
    de_tex_compression comp = q->compression;
    if ((u32)comp >= DE_TEX_COMPRESSION_COUNT)
        comp = DE_TEX_COMPRESSION_AUTO;
    if (comp == DE_TEX_COMPRESSION_AUTO)
        comp = auto_compression(q);
    bool hdr_source = q->source == DE_TEX_SOURCE_CLASS_HDR;
    if (comp != DE_TEX_COMPRESSION_NONE && !q->block_aligned) {
        /* The D3D12 copy and creation rules want BC textures to start at a multiple of 4. */
        c.notes |= DE_TEX_NOTE_UNALIGNED;
        comp = DE_TEX_COMPRESSION_NONE;
    }
    c.compression = comp;
    if (comp == DE_TEX_COMPRESSION_NONE) {
        /* Uncompressed: the DENG-98 formats. */
        if (hdr_source || q->usage == DE_COOKED_TEXTURE_USAGE_ENVIRONMENT)
            c.format = DE_FMT_RGBA16_FLOAT;
        else
            c.format = q->srgb ? DE_FMT_RGBA8_SRGB : DE_FMT_RGBA8_UNORM;
        return c;
    }
    const struct format_row *row = &k_formats[comp];
    c.format = q->srgb ? row->srgb : row->linear;
    if (q->srgb && row->srgb == row->linear)
        c.notes |= DE_TEX_NOTE_SRGB_IGNORED;
    if (!q->srgb && row->linear == DE_FMT_BC7_UNORM && comp != DE_TEX_COMPRESSION_BC7)
        c.notes |= DE_TEX_NOTE_LINEAR_BC1_BC3;
    if (q->has_alpha && row->channels < 4u && !(c.notes & DE_TEX_NOTE_LINEAR_BC1_BC3))
        c.notes |= DE_TEX_NOTE_DROPS_ALPHA;
    if (row->channels < 3u && q->usage != DE_COOKED_TEXTURE_USAGE_NORMAL &&
        q->source != DE_TEX_SOURCE_CLASS_LDR_SINGLE)
        c.notes |= DE_TEX_NOTE_DROPS_CHANNELS;
    if (hdr_source && !row->hdr)
        c.notes |= DE_TEX_NOTE_CLAMPS_HDR;
    return c;
}

de_tex_format_choice de_tex_bc_choose_image(const de_tex_settings *s, const de_tex_image *img) {
    de_tex_format_query q;
    memset(&q, 0, sizeof q);
    if (s->compression == NULL || !de_tex_compression_parse(s->compression, &q.compression))
        q.compression = DE_TEX_COMPRESSION_AUTO;
    q.usage = s->usage;
    q.srgb = s->srgb;
    q.pixel_art = s->pixel_art;
    q.source = de_tex_source_class_of(img);
    u64 n = (u64)img->width * img->height * img->layers;
    for (u64 i = 0; i < n && !q.has_alpha; ++i)
        q.has_alpha = img->rgba[i * 4u + 3u] < 1.0f;
    q.block_aligned = img->width % 4u == 0 && img->height % 4u == 0;
    return de_tex_bc_choose(&q);
}

de_tex_format_choice de_tex_bc_choose_env(const de_tex_settings *s, const de_tex_image *img,
                                          u32 face_size) {
    de_tex_format_query q;
    memset(&q, 0, sizeof q);
    if (s->compression == NULL || !de_tex_compression_parse(s->compression, &q.compression))
        q.compression = DE_TEX_COMPRESSION_AUTO;
    q.usage = DE_COOKED_TEXTURE_USAGE_ENVIRONMENT;
    q.srgb = false;
    q.pixel_art = false; /* environments have no pixel-art mode */
    q.source = de_tex_source_class_of(img);
    q.has_alpha = false; /* the prefilter writes alpha 1 */
    q.block_aligned = face_size % 4u == 0;
    return de_tex_bc_choose(&q);
}
