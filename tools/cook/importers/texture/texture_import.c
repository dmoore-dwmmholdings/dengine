/*
 * texture_import.c: the `texture` importer entry (DENG-98, SPEC §11.6): settings, source
 * dispatch, the linear-space pipeline and the DTEX output. See texture_import.h for the settings
 * and texture_internal.h for the pipeline.
 *
 * Extension points:
 *   - DENG-99 (BC compression): the `compression`, `quality` and `rdo_quality` settings
 *     (read_settings), the format rules (texture_bc_rules.c, through de_tex_choose_format) and
 *     the encoder table (texture_encode.c).
 *   - DENG-100 (HDR environments): the `usage = "environment"` branch in cook_image hands the
 *     decoded image to de_env_cook (env_prefilter.c, env_sh.c).
 *
 * Built with the deterministic float flags (no FMA contraction; tools/cook/CMakeLists.txt).
 */
#include "texture_bc.h"
#include "texture_import.h"
#include "texture_internal.h"
#include "cook/importers/sprite_proxy.h"
#include "env_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "cook/lib/de_cooker.h"

#include <string.h>

const char *const de_texture_import_extensions[DE_TEXTURE_IMPORT_EXTENSION_COUNT] = {
    ".png", ".jpg", ".jpeg", ".exr", ".ktx2"};

de_asset_type_desc de_texture_import_type_desc(void) {
    de_asset_type_desc t;
    memset(&t, 0, sizeof t);
    t.name = "texture";
    t.extensions = de_texture_import_extensions;
    t.extension_count = DE_TEXTURE_IMPORT_EXTENSION_COUNT;
    t.importer_version = DE_TEXTURE_IMPORTER_VERSION;
    t.import = de_texture_import;
    return t;
}

static de_log_category texture_log(void) {
    return de_log_register_category("cook", DE_LOG_INFO);
}

/* ---- Settings -------------------------------------------------------------------------------- */

static const char *const k_usage_names[DE_COOKED_TEXTURE_USAGE_COUNT] = {"color", "normal", "mask",
                                                                         "environment"};

static de_result read_settings(de_asset_import_ctx *ctx, de_tex_settings *s) {
    memset(s, 0, sizeof *s);
    const char *usage = de_asset_import_setting_string(ctx, "usage", "color");
    u32 u = 0;
    while (u < DE_COOKED_TEXTURE_USAGE_COUNT && strcmp(usage, k_usage_names[u]) != 0)
        ++u;
    if (u == DE_COOKED_TEXTURE_USAGE_COUNT)
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "texture: usage \"%s\" (color, normal, mask or environment)",
                                    usage);
    s->usage = (de_cooked_texture_usage)u;
    s->srgb_set = de_asset_import_setting_has(ctx, "srgb");
    s->srgb = de_asset_import_setting_bool(ctx, "srgb", false);
    s->mips = de_asset_import_setting_bool(ctx, "mips", true);
    s->pixel_art = de_asset_import_setting_bool(ctx, "pixel_art", false);
    s->compression = de_asset_import_setting_string(ctx, "compression", "auto");
    if (de_asset_import_setting_has(ctx, "alpha_coverage")) {
        f64 c = de_asset_import_setting_float(ctx, "alpha_coverage", -1.0);
        if (!(c > 0.0 && c < 1.0))
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "texture: alpha_coverage must be a number in (0, 1)");
        s->alpha_coverage = (f32)c;
    }
    if (de_asset_import_setting_has(ctx, "max_size")) {
        i64 m = de_asset_import_setting_int(ctx, "max_size", -1);
        if (m < 1 || m > (i64)DE_COOKED_TEXTURE_MAX_SIZE)
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "texture: max_size must be an integer from 1 to %u",
                                        DE_COOKED_TEXTURE_MAX_SIZE);
        s->max_size = (u32)m;
    }
    const char *addr = de_asset_import_setting_string(ctx, "address", "wrap");
    if (strcmp(addr, "wrap") == 0)
        s->address = DE_TEX_ADDRESS_WRAP;
    else if (strcmp(addr, "clamp") == 0)
        s->address = DE_TEX_ADDRESS_CLAMP;
    else
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "texture: address \"%s\" (wrap or clamp)", addr);
    /* DENG-99: block compression settings. */
    de_tex_compression comp;
    if (!de_tex_compression_parse(s->compression, &comp))
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "texture: compression \"%s\" (auto, none, bc1, bc3, bc4, bc5, "
                                    "bc6h or bc7)",
                                    s->compression);
    const char *quality = de_asset_import_setting_string(ctx, "quality", "normal");
    if (!de_tex_quality_parse(quality, &s->quality))
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "texture: quality \"%s\" (fast, normal or high)", quality);
    if (de_asset_import_setting_has(ctx, "rdo_quality")) {
        f64 q = de_asset_import_setting_float(ctx, "rdo_quality", -1.0);
        if (!(q >= 0.0 && q <= DE_TEXTURE_RDO_QUALITY_MAX))
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "texture: rdo_quality must be a number from 0 (off) to %g",
                                        DE_TEXTURE_RDO_QUALITY_MAX);
        s->rdo_quality = (f32)q;
    }
    return DE_OK;
}

/* The `srgb` value for this source: explicit, from the KTX2 file, or from the usage. */
static void resolve_srgb(de_asset_import_ctx *ctx, de_tex_settings *s, const de_tex_image *img) {
    bool want = s->srgb;
    if (!s->srgb_set) {
        if (s->usage != DE_COOKED_TEXTURE_USAGE_COLOR || img->hdr)
            want = false;
        else
            want = img->has_srgb_hint ? img->srgb_hint : true;
    } else if (want && (img->hdr || s->usage == DE_COOKED_TEXTURE_USAGE_NORMAL ||
                        s->usage == DE_COOKED_TEXTURE_USAGE_ENVIRONMENT)) {
        DE_WARN(texture_log(), "texture \"%s\": srgb = true ignored (%s)",
                de_cooker_import_id_str(ctx),
                img->hdr ? "float source" : "normal maps and environments are linear");
        want = false;
    }
    s->srgb = want;
}

/* ---- Images ---------------------------------------------------------------------------------- */

/* sRGB-encoded colour to linear, in place. Values that are exact bytes (8-bit sources) use a
 * table of the 256 decoded values. */
static void to_linear(de_tex_image *img) {
    f32 table[256];
    for (u32 k = 0; k < 256u; ++k)
        table[k] = de_tex_srgb_to_linear((f32)k / 255.0f);
    u64 n = (u64)img->width * img->height * img->layers;
    for (u64 i = 0; i < n; ++i)
        for (u32 c = 0; c < 3; ++c) {
            f32 *v = &img->rgba[i * 4u + c];
            f32 x = *v * 255.0f + 0.5f;
            u32 k = x > 0.0f ? (x < 255.5f ? (u32)x : 255u) : 0u;
            *v = (f32)k / 255.0f == *v ? table[k] : de_tex_srgb_to_linear(*v);
        }
}

/* Normal map: stored [0, 1] to vectors in [-1, 1] (LDR sources; float sources are signed). */
static void unpack_normals(f32 *rgba, u64 texels) {
    for (u64 i = 0; i < texels; ++i)
        for (u32 c = 0; c < 3; ++c)
            rgba[i * 4u + c] = rgba[i * 4u + c] * 2.0f - 1.0f;
}

static void pack_normals(f32 *rgba, u64 texels) {
    for (u64 i = 0; i < texels; ++i)
        for (u32 c = 0; c < 3; ++c)
            rgba[i * 4u + c] = rgba[i * 4u + c] * 0.5f + 0.5f;
}

static bool any_alpha(const de_tex_image *img) {
    u64 n = (u64)img->width * img->height * img->layers;
    for (u64 i = 0; i < n; ++i)
        if (img->rgba[i * 4u + 3u] < 1.0f)
            return true;
    return false;
}

/* Scales the image down so its longest edge is `max_size`: halvings while both edges stay at or
 * above the target, then one resample (a scale below 2), so the filter stays short. */
static de_result clamp_size(const de_allocator *a, de_tex_image *img, u32 max_size,
                            de_tex_address mode) {
    u32 w = img->width, h = img->height, longest = w > h ? w : h;
    if (max_size == 0 || longest <= max_size)
        return DE_OK;
    u32 tw = (u32)(((u64)w * max_size + longest / 2u) / longest);
    u32 th = (u32)(((u64)h * max_size + longest / 2u) / longest);
    tw = tw != 0 ? tw : 1u;
    th = th != 0 ? th : 1u;
    while (img->width != tw || img->height != th) {
        u32 nw = img->width / 2u >= tw ? img->width / 2u : tw;
        u32 nh = img->height / 2u >= th ? img->height / 2u : th;
        de_tex_image next = *img;
        next.width = nw;
        next.height = nh;
        next.rgba = NULL;
        DE_TRY(de_tex_image_alloc(a, &next));
        de_result r = de_tex_resample(a, img->rgba, img->width, img->height, next.rgba, nw, nh,
                                      img->layers, mode);
        if (r != DE_OK) {
            de_tex_image_free(a, &next);
            return r;
        }
        de_tex_image_free(a, img);
        *img = next;
    }
    return DE_OK;
}

static de_gpu_texture_dim dim_of(bool cube, u32 layers) {
    if (cube)
        return DE_TEX_CUBE;
    return layers > 1u ? DE_TEX_2D_ARRAY : DE_TEX_2D;
}

static de_result write_dtex(de_asset_import_ctx *ctx, const de_allocator *a, de_tex_dtex *t) {
    de_result r = de_asset_import_write(ctx, t->data, t->size);
    de_tex_dtex_free(a, t);
    return r;
}

/* DENG-99: says where the format choice loses data or differs from the setting. */
static void warn_choice(de_asset_import_ctx *ctx, const de_tex_settings *s,
                        const de_tex_format_choice *c) {
    const char *id = de_cooker_import_id_str(ctx);
    const char *name = de_tex_compression_name(c->compression);
    if (c->notes & DE_TEX_NOTE_UNALIGNED)
        DE_WARN(texture_log(),
                "texture \"%s\": compression \"%s\" needs a size that is a multiple of 4 (BC "
                "blocks); cooked uncompressed",
                id, s->compression);
    if (c->notes & DE_TEX_NOTE_DROPS_ALPHA)
        DE_WARN(texture_log(), "texture \"%s\": %s stores no alpha; the source's alpha is dropped",
                id, name);
    if (c->notes & DE_TEX_NOTE_DROPS_CHANNELS)
        DE_WARN(texture_log(), "texture \"%s\": %s keeps only %s of the source's channels", id,
                name, c->compression == DE_TEX_COMPRESSION_BC4 ? "red" : "red and green");
    if (c->notes & DE_TEX_NOTE_CLAMPS_HDR)
        DE_WARN(texture_log(), "texture \"%s\": %s is 8-bit; the float source is clamped to [0, 1]",
                id, name);
    if (c->notes & DE_TEX_NOTE_LINEAR_BC1_BC3)
        DE_WARN(texture_log(),
                "texture \"%s\": %s has only an sRGB format and the data is linear (srgb = false); "
                "cooked as BC7 UNORM",
                id, name);
    if (c->notes & DE_TEX_NOTE_SRGB_IGNORED)
        DE_WARN(texture_log(), "texture \"%s\": %s has no sRGB format; stored linear", id, name);
}

static de_result cook_image(de_asset_import_ctx *ctx, const de_allocator *a, de_tex_settings *s,
                            de_tex_image *img) {
    resolve_srgb(ctx, s, img);
    if (img->cube && img->width != img->height)
        return de_asset_import_fail(ctx, DE_ERR_PARSE, "texture: cube faces are not square");
    /* DENG-100: an environment becomes a prefiltered cube with SH9 (env_prefilter.c). */
    if (s->usage == DE_COOKED_TEXTURE_USAGE_ENVIRONMENT)
        return de_env_cook(ctx, a, s, img);
    if (s->srgb && !img->hdr)
        to_linear(img);
    u64 texels = (u64)img->width * img->height * img->layers;
    bool normal = s->usage == DE_COOKED_TEXTURE_USAGE_NORMAL;
    if (normal && !img->hdr)
        unpack_normals(img->rgba, texels);
    DE_TRY(clamp_size(a, img, s->max_size, s->address));
    bool has_alpha = any_alpha(img);
    de_tex_mip_params mp;
    memset(&mp, 0, sizeof mp);
    mp.address = s->address;
    mp.normal_map = normal;
    de_tex_format_choice choice = de_tex_bc_choose_image(s, img);
    de_gpu_format format = choice.format;
    warn_choice(ctx, s, &choice);
    mp.alpha_coverage = s->alpha_coverage;
    /* Coverage is measured on the alpha the format stores: 8-bit steps for RGBA8 and for the BC
     * formats with alpha (BC3 and BC7 encode from 8-bit alpha); 0 measures the float values. */
    mp.alpha_levels = format == DE_FMT_RGBA8_UNORM || format == DE_FMT_RGBA8_SRGB ||
                              format == DE_FMT_BC3_SRGB || format == DE_FMT_BC7_UNORM ||
                              format == DE_FMT_BC7_SRGB
                          ? 255u
                          : 0u;
    mp.full_chain = s->mips && !s->pixel_art;
    de_tex_chain chain;
    DE_TRY(de_tex_mips_build(a, img, &mp, &chain));
    if (normal)
        for (u32 m = 0; m < chain.mip_count; ++m)
            pack_normals(chain.rgba[m], (u64)chain.width[m] * chain.height[m] * chain.layers);
    de_tex_dtex_desc d;
    memset(&d, 0, sizeof d);
    d.format = format;
    d.dim = dim_of(img->cube, img->layers);
    d.width = img->width;
    d.height = img->height;
    d.layers = img->layers;
    d.mip_count = chain.mip_count;
    d.usage = s->usage;
    d.flags = (s->pixel_art ? DE_COOKED_TEXTURE_PIXEL_ART : 0u) |
              (has_alpha ? DE_COOKED_TEXTURE_HAS_ALPHA : 0u);
    d.encode.quality = s->quality;
    d.encode.rdo_quality = s->rdo_quality;
    d.encode.tasks = de_cooker_import_tasks(ctx);
    de_tex_dtex t;
    de_result r = de_tex_dtex_from_chain(a, &d, &chain, &t);
    de_tex_chain_free(a, &chain);
    if (r != DE_OK)
        return r;
    if (de_asset_import_setting_bool(ctx, "rt_proxy", false)) {
        de_cooked_sprite_proxy proxy;
        if (img->layers != 1 || img->cube) {
            de_tex_dtex_free(a, &t);
            return de_asset_import_fail(ctx, DE_ERR_UNSUPPORTED, "rt_proxy requires one 2D mask");
        }
        /* One authored atlas region per cooked texture; all other regions use alpha boxes. */
        const char *keys[4] = {"rt_proxy_u", "rt_proxy_v", "rt_proxy_width", "rt_proxy_height"};
        f32 region[4];
        for (u32 i = 0; i < 4; ++i)
            region[i] = (f32)de_asset_import_setting_float(ctx, keys[i], i < 2 ? 0 : 1);
        if (!isfinite(region[0]) || !isfinite(region[1]) || !isfinite(region[2]) ||
            !isfinite(region[3]) || region[0] < 0 || region[1] < 0 || region[2] <= 0 ||
            region[3] <= 0 || region[0] + region[2] > 1 || region[1] + region[3] > 1) {
            de_tex_dtex_free(a, &t);
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT, "invalid rt_proxy UV region");
        }
        u32 x = (u32)lroundf(region[0] * img->width), y = (u32)lroundf(region[1] * img->height);
        u32 w = (u32)lroundf(region[2] * img->width), h = (u32)lroundf(region[3] * img->height);
        if (!w || !h || x + w > img->width || y + h > img->height) {
            de_tex_dtex_free(a, &t);
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT, "empty rt_proxy region");
        }
        size_t crop_bytes = (size_t)w * h * 4 * sizeof(f32);
        f32 *crop = DE_ALLOC(a, crop_bytes, 16, DE_MEM_ASSET);
        if (!crop) {
            de_tex_dtex_free(a, &t);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sprite proxy crop");
        }
        for (u32 row = 0; row < h; ++row)
            memcpy(crop + (size_t)row * w * 4, img->rgba + ((size_t)(y + row) * img->width + x) * 4,
                   (size_t)w * 4 * sizeof(f32));
        r = de_sprite_proxy_build(
            a, crop, w, h, (f32)de_asset_import_setting_float(ctx, "rt_proxy_cutoff", .5), &proxy);
        DE_FREE(a, crop, crop_bytes);
        memcpy(proxy.region, region, sizeof region);
        if (r != DE_OK) {
            de_tex_dtex_free(a, &t);
            return r;
        }
        u64 bytes = t.size + DE_SPRITE_PROXY_BYTES;
        u8 *next = DE_ALLOC(a, (size_t)bytes, 16, DE_MEM_ASSET);
        if (!next) {
            de_tex_dtex_free(a, &t);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "sprite proxy output");
        }
        memcpy(next, t.data, (size_t)t.size);
        de_sprite_proxy_write(next + t.size, &proxy);
        DE_FREE(a, t.data, (size_t)t.size);
        t.data = next;
        t.size = bytes;
        de_cooked_put_u64(next + 48, bytes);
    }
    return write_dtex(ctx, a, &t);
}

/* KTX2 with BC payloads: the stored levels, unchanged. */
static de_result cook_blocks(de_asset_import_ctx *ctx, const de_allocator *a,
                             const de_tex_settings *s, const de_tex_blocks *b) {
    if (s->usage == DE_COOKED_TEXTURE_USAGE_ENVIRONMENT)
        return de_asset_import_fail(ctx, DE_ERR_UNSUPPORTED,
                                    "texture: an environment cannot come from block-compressed "
                                    "KTX2 levels (give a float or 8-bit source)");
    u32 first = 0;
    if (s->max_size != 0)
        while (first + 1u < b->mip_count &&
               (de_cooked_texture_mip_extent(b->width, first) > s->max_size ||
                de_cooked_texture_mip_extent(b->height, first) > s->max_size))
            ++first;
    u32 count = b->mip_count - first;
    if (!s->mips || s->pixel_art)
        count = 1;
    u32 w = de_cooked_texture_mip_extent(b->width, first);
    u32 h = de_cooked_texture_mip_extent(b->height, first);
    if (s->mips && !s->pixel_art && count < de_cooked_texture_full_mip_count(w, h))
        DE_WARN(texture_log(),
                "texture \"%s\": the KTX2 file holds %u of %u mips; block-compressed levels are "
                "passed through and the missing mips are not generated",
                de_cooker_import_id_str(ctx), count, de_cooked_texture_full_mip_count(w, h));
    if (w % 4u != 0 || h % 4u != 0)
        return de_asset_import_fail(ctx, DE_ERR_UNSUPPORTED,
                                    "texture: a block-compressed texture of %ux%u (the GPU needs "
                                    "multiples of 4)",
                                    w, h);
    de_tex_dtex_desc d;
    memset(&d, 0, sizeof d);
    d.format = b->format;
    d.dim = dim_of(b->cube, b->layers);
    d.width = w;
    d.height = h;
    d.layers = b->layers;
    d.mip_count = count;
    d.usage = s->usage;
    d.flags = (s->pixel_art ? DE_COOKED_TEXTURE_PIXEL_ART : 0u) |
              (b->has_alpha ? DE_COOKED_TEXTURE_HAS_ALPHA : 0u);
    de_tex_dtex t;
    DE_TRY(de_tex_dtex_from_blocks(a, &d, b, first, &t));
    return write_dtex(ctx, a, &t);
}

/* ---- Entry ----------------------------------------------------------------------------------- */

de_result de_texture_import(de_asset_import_ctx *ctx) {
    de_tex_settings s;
    DE_TRY(read_settings(ctx, &s));
    /* DENG-106: `max_decoded_mib`, the largest decoded image; the decoders check it before they
     * allocate (de_tex_limit carries it with the allocator). */
    i64 mib = (i64)DE_TEX_MAX_DECODED_MIB_DEFAULT;
    if (de_asset_import_setting_has(ctx, "max_decoded_mib")) {
        mib = de_asset_import_setting_int(ctx, "max_decoded_mib", -1);
        if (mib < 1 || mib > (i64)DE_TEX_MAX_DECODED_MIB_MAX)
            return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                        "texture: max_decoded_mib must be an integer from 1 to %u",
                                        DE_TEX_MAX_DECODED_MIB_MAX);
    }
    de_tex_limit limit;
    const de_allocator *a =
        de_tex_limit_init(&limit, de_asset_import_allocator(ctx), (u64)mib << 20);
    u64 size = 0;
    const void *src = de_asset_import_source(ctx, &size);
    de_tex_image img;
    memset(&img, 0, sizeof img);
    de_result r;
    switch (de_tex_sniff(src, size)) {
    case DE_TEX_SOURCE_PNG:
    case DE_TEX_SOURCE_JPEG:
        r = de_tex_decode_stb(a, src, size, &img);
        break;
    case DE_TEX_SOURCE_EXR:
        r = de_tex_decode_exr(a, src, size, &img);
        break;
    case DE_TEX_SOURCE_KTX2: {
        de_tex_ktx2_kind kind = DE_TEX_KTX2_IMAGE;
        de_tex_blocks blocks;
        r = de_tex_decode_ktx2(a, src, size, &kind, &img, &blocks);
        if (r == DE_OK && kind == DE_TEX_KTX2_BLOCKS) {
            r = cook_blocks(ctx, a, &s, &blocks);
            de_tex_blocks_free(a, &blocks);
            return r;
        }
        break;
    }
    default:
        return de_asset_import_fail(ctx, DE_ERR_PARSE,
                                    "texture: \"%s\" is not a PNG, JPEG, EXR or KTX2 file",
                                    de_asset_import_source_path(ctx));
    }
    if (r != DE_OK)
        return r;
    r = cook_image(ctx, a, &s, &img);
    de_tex_image_free(a, &img);
    return r;
}
