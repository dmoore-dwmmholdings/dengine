/*
 * texture_ktx2.c: the KTX2 container reader of the texture importer (DENG-98, SPEC §11.6).
 *
 * Reads the parts of KTX 2.0 (Khronos KTX File Format Specification 2.0) the importer needs: the
 * header, the level index, the basic data format descriptor (colour model, transfer function,
 * samples) and zstd supercompression (scheme 2, through the vendored zstd). Three kinds of
 * payload:
 *
 *   - BC formats with an engine equivalent: every level is kept as stored (de_tex_blocks) and
 *     passed through to DTEX;
 *   - a few uncompressed formats (R8, RG8, RGB8, RGBA8 in UNORM and SRGB, RGBA16F, RGBA32F):
 *     level 0 of every layer and face is decoded to f32 (de_tex_image) and goes through the mip
 *     pipeline like a PNG or EXR;
 *   - Basis Universal (vkFormat UNDEFINED; ETC1S with BasisLZ, UASTC with or without zstd): the
 *     whole file goes to the Basis transcoder (texture_basis.cpp), which decodes level 0.
 *
 * Every offset and length is checked against the file before it is used; sizes over
 * DE_COOKED_TEXTURE_MAX_SIZE and 3D textures are refused. The KTX2 key/value data is ignored.
 */
#include "texture_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "render/formats/de_cooked_le.h"

#include <zstd.h>

#include <string.h>

/* ---- Constants from the KTX2 and Khronos Data Format specifications -------------------------- */

enum {
    KTX2_HEADER_SIZE = 80, /* identifier 12, header 36, index 32 */
    KTX2_LEVEL_SIZE = 24,
    KTX2_SUPERCOMPRESSION_NONE = 0,
    KTX2_SUPERCOMPRESSION_BASISLZ = 1,
    KTX2_SUPERCOMPRESSION_ZSTD = 2,
    KHR_DF_MODEL_ETC1S = 163,
    KHR_DF_MODEL_UASTC = 166,
    KHR_DF_TRANSFER_SRGB = 2,
    KHR_DF_CHANNEL_ALPHA = 15
};

/* The Vulkan formats the reader knows (VkFormat values). */
enum {
    VK_UNDEFINED = 0,
    VK_R8_UNORM = 9,
    VK_R8_SRGB = 15,
    VK_R8G8_UNORM = 16,
    VK_R8G8_SRGB = 22,
    VK_R8G8B8_UNORM = 23,
    VK_R8G8B8_SRGB = 29,
    VK_R8G8B8A8_UNORM = 37,
    VK_R8G8B8A8_SRGB = 43,
    VK_R16G16B16A16_SFLOAT = 97,
    VK_R32G32B32A32_SFLOAT = 109,
    VK_BC1_RGB_UNORM = 131,
    VK_BC1_RGB_SRGB = 132,
    VK_BC1_RGBA_UNORM = 133,
    VK_BC1_RGBA_SRGB = 134,
    VK_BC3_UNORM = 137,
    VK_BC3_SRGB = 138,
    VK_BC4_UNORM = 139,
    VK_BC5_UNORM = 141,
    VK_BC6H_UFLOAT = 143,
    VK_BC7_UNORM = 145,
    VK_BC7_SRGB = 146
};

/* An uncompressed format the reader decodes: channels, bytes per channel, float, sRGB. */
typedef struct raw_format {
    u32 vk;
    u32 channels;
    u32 channel_bytes;
    bool is_float;
    bool srgb;
} raw_format;

static const raw_format k_raw[] = {
    {VK_R8_UNORM, 1, 1, false, false},           {VK_R8_SRGB, 1, 1, false, true},
    {VK_R8G8_UNORM, 2, 1, false, false},         {VK_R8G8_SRGB, 2, 1, false, true},
    {VK_R8G8B8_UNORM, 3, 1, false, false},       {VK_R8G8B8_SRGB, 3, 1, false, true},
    {VK_R8G8B8A8_UNORM, 4, 1, false, false},     {VK_R8G8B8A8_SRGB, 4, 1, false, true},
    {VK_R16G16B16A16_SFLOAT, 4, 2, true, false}, {VK_R32G32B32A32_SFLOAT, 4, 4, true, false},
};

static const raw_format *find_raw(u32 vk) {
    for (u32 i = 0; i < (u32)(sizeof k_raw / sizeof k_raw[0]); ++i)
        if (k_raw[i].vk == vk)
            return &k_raw[i];
    return NULL;
}

/* The engine format of a BC VkFormat, or DE_FMT_UNKNOWN. Linear BC1 and BC3 have no engine
 * format yet (DENG-99 may add them). */
static de_gpu_format bc_format(u32 vk) {
    switch (vk) {
    case VK_BC1_RGB_SRGB:
    case VK_BC1_RGBA_SRGB:
        return DE_FMT_BC1_SRGB;
    case VK_BC3_SRGB:
        return DE_FMT_BC3_SRGB;
    case VK_BC4_UNORM:
        return DE_FMT_BC4_UNORM;
    case VK_BC5_UNORM:
        return DE_FMT_BC5_UNORM;
    case VK_BC6H_UFLOAT:
        return DE_FMT_BC6H_UFLOAT;
    case VK_BC7_UNORM:
        return DE_FMT_BC7_UNORM;
    case VK_BC7_SRGB:
        return DE_FMT_BC7_SRGB;
    default:
        return DE_FMT_UNKNOWN;
    }
}

static bool bc_has_alpha(u32 vk) {
    return vk == VK_BC1_RGBA_SRGB || vk == VK_BC3_SRGB || vk == VK_BC7_UNORM || vk == VK_BC7_SRGB;
}

/* ---- Header ---------------------------------------------------------------------------------- */

typedef struct ktx2_level {
    u64 offset, length, uncompressed;
} ktx2_level;

typedef struct ktx2_file {
    const u8 *p;
    u64 size;
    u32 vk_format, type_size, width, height, depth, layers, faces, levels, scheme;
    u32 dfd_offset, dfd_length;
    u64 sgd_offset, sgd_length; /* supercompression global data (BasisLZ) */
    u8 color_model, transfer;
    bool dfd_alpha; /* a sample with the alpha channel id */
    ktx2_level level[DE_COOKED_TEXTURE_MAX_MIPS];
} ktx2_file;

/* Bytes of the stored BC blocks of every level, layer and face of `f` in format `fmt`. */
static u64 bc_bytes(const ktx2_file *f, de_gpu_format fmt, u32 levels) {
    de_cooked_texture_block blk;
    if (!de_cooked_texture_format_block(fmt, &blk))
        return 0;
    u64 total = 0;
    for (u32 m = 0; m < levels; ++m)
        total += de_cooked_texture_row_bytes(&blk, de_cooked_texture_mip_extent(f->width, m)) *
                 de_cooked_texture_row_count(&blk, de_cooked_texture_mip_extent(f->height, m));
    return total * f->layers * f->faces;
}

static de_result ktx2_parse(const de_allocator *a, const void *data, u64 size, ktx2_file *f) {
    memset(f, 0, sizeof *f);
    f->p = (const u8 *)data;
    f->size = size;
    if (size < KTX2_HEADER_SIZE)
        return DE_FAIL(DE_ERR_PARSE, "KTX2: shorter than the header");
    const u8 *h = f->p + 12;
    f->vk_format = de_cooked_get_u32(h + 0);
    f->type_size = de_cooked_get_u32(h + 4);
    f->width = de_cooked_get_u32(h + 8);
    f->height = de_cooked_get_u32(h + 12);
    f->depth = de_cooked_get_u32(h + 16);
    f->layers = de_cooked_get_u32(h + 20);
    f->faces = de_cooked_get_u32(h + 24);
    f->levels = de_cooked_get_u32(h + 28);
    f->scheme = de_cooked_get_u32(h + 32);
    f->dfd_offset = de_cooked_get_u32(h + 36);
    f->dfd_length = de_cooked_get_u32(h + 40);
    f->sgd_offset = de_cooked_get_u64(h + 52);
    f->sgd_length = de_cooked_get_u64(h + 60);
    if (f->depth > 1)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "KTX2: 3D textures are not supported");
    if (f->height == 0)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "KTX2: 1D textures are not supported");
    DE_TRY(de_tex_check_size(f->width, f->height, "KTX2"));
    if (f->faces != 1 && f->faces != 6)
        return DE_FAIL(DE_ERR_PARSE, "KTX2: face count %u (1 or 6)", f->faces);
    if (f->faces == 6 && f->width != f->height)
        return DE_FAIL(DE_ERR_PARSE, "KTX2: cube faces are not square");
    if (f->layers == 0)
        f->layers = 1;
    if ((u64)f->layers * f->faces > DE_COOKED_TEXTURE_MAX_LAYERS)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "KTX2: %u layers x %u faces is over the limit of %u",
                       f->layers, f->faces, DE_COOKED_TEXTURE_MAX_LAYERS);
    /* levelCount 0 asks the loader to make mips; the file then holds level 0 only. */
    u32 nlevels = f->levels != 0 ? f->levels : 1u;
    if (nlevels > de_cooked_texture_full_mip_count(f->width, f->height))
        return DE_FAIL(DE_ERR_PARSE, "KTX2: %u levels for a %ux%u texture", nlevels, f->width,
                       f->height);
    f->levels = nlevels;
    /* The decoded-bytes limit, from the header alone (DENG-106): the stored blocks of a BC
     * pass-through, else level 0 of every layer and face as f32 RGBA. */
    de_gpu_format bc = bc_format(f->vk_format);
    u64 decoded = bc != DE_FMT_UNKNOWN
                      ? bc_bytes(f, bc, nlevels)
                      : (u64)f->width * f->height * f->layers * f->faces * 4u * sizeof(f32);
    DE_TRY(de_tex_limit_check(a, decoded, "KTX2"));
    if ((u64)KTX2_HEADER_SIZE + (u64)nlevels * KTX2_LEVEL_SIZE > size)
        return DE_FAIL(DE_ERR_PARSE, "KTX2: the level index is cut off");
    for (u32 i = 0; i < nlevels; ++i) {
        const u8 *l = f->p + KTX2_HEADER_SIZE + (u64)i * KTX2_LEVEL_SIZE;
        f->level[i].offset = de_cooked_get_u64(l);
        f->level[i].length = de_cooked_get_u64(l + 8);
        f->level[i].uncompressed = de_cooked_get_u64(l + 16);
        if (!de_cooked_range_ok(size, f->level[i].offset, f->level[i].length))
            return DE_FAIL(DE_ERR_PARSE, "KTX2: level %u lies outside the file", i);
    }
    /* Basic data format descriptor: u32 total size, then the block (3 words, then samples). */
    if (f->dfd_length != 0) {
        if (f->dfd_length < 28 || !de_cooked_range_ok(size, f->dfd_offset, f->dfd_length))
            return DE_FAIL(DE_ERR_PARSE, "KTX2: bad data format descriptor");
        const u8 *d = f->p + f->dfd_offset;
        f->color_model = d[12];
        f->transfer = d[14];
        u32 block_size = (u32)d[10] | ((u32)d[11] << 8);
        if (block_size >= 24 && (u64)block_size + 4u <= f->dfd_length)
            for (u32 s = 24; s + 16u <= block_size; s += 16u)
                if (((d[4 + s + 3] & 0x0Fu)) == KHR_DF_CHANNEL_ALPHA)
                    f->dfd_alpha = true;
    }
    return DE_OK;
}

/* The bytes of level `i`, zstd-decompressed into a block of `a` when the file is supercompressed
 * with zstd (*owned set; free with *owned_size). `expected` is the size the level must have: a
 * zstd frame that claims another size is refused before anything is allocated (DENG-106). */
static de_result level_bytes(const de_allocator *a, const ktx2_file *f, u32 i, u64 expected,
                             const u8 **out, u64 *out_size, u8 **owned, u64 *owned_size) {
    *owned = NULL;
    *owned_size = 0;
    const ktx2_level *l = &f->level[i];
    if (f->scheme == KTX2_SUPERCOMPRESSION_NONE) {
        *out = f->p + l->offset;
        *out_size = l->length;
        return DE_OK;
    }
    if (f->scheme != KTX2_SUPERCOMPRESSION_ZSTD)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "KTX2: supercompression scheme %u is not supported",
                       f->scheme);
    unsigned long long n = ZSTD_getFrameContentSize(f->p + l->offset, (size_t)l->length);
    if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN || n != l->uncompressed ||
        n > (u64)SIZE_MAX)
        return DE_FAIL(DE_ERR_PARSE, "KTX2: level %u: bad zstd frame", i);
    if (n != expected)
        return DE_FAIL(DE_ERR_PARSE, "KTX2: level %u holds %llu bytes, expected %llu", i, n,
                       (unsigned long long)expected);
    u8 *buf = (u8 *)DE_ALLOC(a, n != 0 ? (size_t)n : 1u, 16, DE_MEM_ASSET);
    if (buf == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "KTX2: out of memory for level %u", i);
    size_t got = ZSTD_decompress(buf, (size_t)n, f->p + l->offset, (size_t)l->length);
    if (ZSTD_isError(got) || got != n) {
        DE_FREE(a, buf, n != 0 ? (size_t)n : 1u);
        return DE_FAIL(DE_ERR_PARSE, "KTX2: level %u: zstd: %s", i,
                       ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short frame");
    }
    *out = buf;
    *out_size = n;
    *owned = buf;
    *owned_size = n != 0 ? n : 1u;
    return DE_OK;
}

/* ---- Block payloads (passed through) --------------------------------------------------------- */

void de_tex_blocks_free(const de_allocator *a, de_tex_blocks *b) {
    if (b == NULL)
        return;
    if (b->block != NULL)
        DE_FREE(a, b->block, (size_t)b->data_bytes);
    if (b->data != NULL)
        DE_FREE(a, (void *)b->data, (size_t)b->table_bytes);
    memset(b, 0, sizeof *b);
}

static de_result read_blocks(const de_allocator *a, const ktx2_file *f, de_gpu_format fmt,
                             de_tex_blocks *b) {
    de_cooked_texture_block blk;
    if (!de_cooked_texture_format_block(fmt, &blk))
        return DE_FAIL(DE_ERR_INTERNAL, "KTX2: no block layout for format %u", (u32)fmt);
    u32 images = f->layers * f->faces;
    /* Sizes first: each level holds `images` images of its block rows, tightly. */
    u64 image_size[DE_COOKED_TEXTURE_MAX_MIPS];
    u64 total = 0;
    for (u32 m = 0; m < f->levels; ++m) {
        u32 w = de_cooked_texture_mip_extent(f->width, m);
        u32 h = de_cooked_texture_mip_extent(f->height, m);
        image_size[m] = de_cooked_texture_row_bytes(&blk, w) * de_cooked_texture_row_count(&blk, h);
        total += image_size[m] * images;
    }
    memset(b, 0, sizeof *b);
    b->format = fmt;
    b->width = f->width;
    b->height = f->height;
    b->layers = images;
    b->cube = f->faces == 6;
    b->has_alpha = bc_has_alpha(f->vk_format) || f->dfd_alpha;
    b->mip_count = f->levels;
    u64 count = (u64)images * f->levels;
    b->table_bytes = count * (sizeof(const u8 *) + sizeof(u64));
    b->data = (const u8 **)DE_ALLOC(a, (size_t)b->table_bytes, DE_ALIGNOF(u64), DE_MEM_ASSET);
    b->data_bytes = total != 0 ? total : 1u;
    b->block = (u8 *)DE_ALLOC(a, (size_t)b->data_bytes, 16, DE_MEM_ASSET);
    if (b->data == NULL || b->block == NULL) {
        de_tex_blocks_free(a, b);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "KTX2: out of memory");
    }
    b->size = (u64 *)(void *)(b->data + count);
    /* Destination: layer-major (DTEX order); source: level-major, then layer, then face. */
    u64 dst = 0;
    for (u32 i = 0; i < images; ++i)
        for (u32 m = 0; m < f->levels; ++m) {
            b->data[(u64)i * f->levels + m] = b->block + dst;
            b->size[(u64)i * f->levels + m] = image_size[m];
            dst += image_size[m];
        }
    for (u32 m = 0; m < f->levels; ++m) {
        const u8 *src = NULL;
        u64 n = 0;
        u8 *owned = NULL;
        u64 owned_size = 0;
        de_result r = level_bytes(a, f, m, image_size[m] * images, &src, &n, &owned, &owned_size);
        if (r == DE_OK && n != image_size[m] * images)
            r = DE_FAIL(DE_ERR_PARSE, "KTX2: level %u holds %llu bytes, expected %llu", m,
                        (unsigned long long)n, (unsigned long long)(image_size[m] * images));
        if (r != DE_OK) {
            if (owned != NULL)
                DE_FREE(a, owned, (size_t)owned_size);
            de_tex_blocks_free(a, b);
            return r;
        }
        for (u32 i = 0; i < images; ++i)
            memcpy((void *)b->data[(u64)i * f->levels + m], src + (u64)i * image_size[m],
                   (size_t)image_size[m]);
        if (owned != NULL)
            DE_FREE(a, owned, (size_t)owned_size);
    }
    return DE_OK;
}

/* ---- Uncompressed payloads (decoded) --------------------------------------------------------- */

static de_result read_raw(const de_allocator *a, const ktx2_file *f, const raw_format *rf,
                          de_tex_image *img) {
    memset(img, 0, sizeof *img);
    img->width = f->width;
    img->height = f->height;
    img->layers = f->layers * f->faces;
    img->cube = f->faces == 6;
    img->hdr = rf->is_float;
    img->channels = rf->channels;
    img->has_srgb_hint = !rf->is_float;
    img->srgb_hint = rf->srgb || f->transfer == KHR_DF_TRANSFER_SRGB;
    u64 texel = (u64)rf->channels * rf->channel_bytes;
    u64 image_bytes = texel * f->width * f->height;
    const u8 *src = NULL;
    u64 n = 0;
    u8 *owned = NULL;
    u64 owned_size = 0;
    DE_TRY(level_bytes(a, f, 0, image_bytes * img->layers, &src, &n, &owned, &owned_size));
    de_result r = DE_OK;
    if (n != image_bytes * img->layers)
        r = DE_FAIL(DE_ERR_PARSE, "KTX2: level 0 holds %llu bytes, expected %llu",
                    (unsigned long long)n, (unsigned long long)(image_bytes * img->layers));
    if (r == DE_OK)
        r = de_tex_image_alloc(a, img);
    if (r == DE_OK) {
        u64 texels = (u64)f->width * f->height * img->layers;
        for (u64 t = 0; t < texels; ++t) {
            const u8 *s = src + t * texel;
            f32 v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (u32 c = 0; c < rf->channels; ++c) {
                if (rf->channel_bytes == 1)
                    v[c] = (f32)s[c] / 255.0f;
                else if (rf->channel_bytes == 2)
                    v[c] = de_tex_f16_to_f32((u16)(s[2 * c] | (s[2 * c + 1] << 8)));
                else
                    v[c] = de_cooked_get_f32(s + 4 * c);
            }
            if (rf->channels == 1)
                v[1] = v[2] = v[0];
            f32 *d = img->rgba + t * 4u;
            for (u32 c = 0; c < 4; ++c) {
                f32 x = v[c];
                if (x != x)
                    x = 0.0f;
                d[c] = x > 65504.0f ? 65504.0f : (x < -65504.0f ? -65504.0f : x);
            }
        }
    }
    if (owned != NULL)
        DE_FREE(a, owned, (size_t)owned_size);
    if (r != DE_OK)
        de_tex_image_free(a, img);
    return r;
}

/* ---- Entry ----------------------------------------------------------------------------------- */

de_result de_tex_decode_ktx2(const de_allocator *a, const void *data, u64 size,
                             de_tex_ktx2_kind *kind, de_tex_image *image, de_tex_blocks *blocks) {
    memset(image, 0, sizeof *image);
    memset(blocks, 0, sizeof *blocks);
    ktx2_file f;
    DE_TRY(ktx2_parse(a, data, size, &f));
    if (f.vk_format == VK_UNDEFINED) {
        if (f.color_model != KHR_DF_MODEL_ETC1S && f.color_model != KHR_DF_MODEL_UASTC)
            return DE_FAIL(DE_ERR_UNSUPPORTED,
                           "KTX2: format UNDEFINED with colour model %u (only Basis ETC1S and "
                           "UASTC are supported)",
                           f.color_model);
        if (f.scheme != KTX2_SUPERCOMPRESSION_NONE && f.scheme != KTX2_SUPERCOMPRESSION_BASISLZ &&
            f.scheme != KTX2_SUPERCOMPRESSION_ZSTD)
            return DE_FAIL(DE_ERR_UNSUPPORTED, "KTX2: supercompression scheme %u is not supported",
                           f.scheme);
        /* The Basis transcoder copies the ETC1S global data header (20 bytes) and one 20-byte
         * image descriptor per level, layer and face from the supercompression global data
         * before it checks that the data is that long (DENG-106). */
        if (f.scheme == KTX2_SUPERCOMPRESSION_BASISLZ &&
            (!de_cooked_range_ok(size, f.sgd_offset, f.sgd_length) ||
             f.sgd_length < 20u + 20u * (u64)f.levels * f.layers * f.faces))
            return DE_FAIL(DE_ERR_PARSE, "KTX2: the BasisLZ global data is missing or cut off");
        /* The transcoder allocates a level's uncompressedByteLength before it checks it: a level
         * may not claim more than its UASTC blocks (16 bytes per 4x4 block, every layer and face;
         * BasisLZ levels claim 0). DENG-106. */
        /* UASTC levels hold exactly their blocks: uncompressedByteLength, the stored length (no
         * supercompression) and a zstd frame's content size must all be that, or the transcoder
         * stops on an assertion (found by fuzz_ktx2 in CI). */
        for (u32 m = 0; m < f.levels; ++m) {
            u64 bw = (de_cooked_texture_mip_extent(f.width, m) + 3u) / 4u;
            u64 bh = (de_cooked_texture_mip_extent(f.height, m) + 3u) / 4u;
            u64 most = bw * bh * 16u * f.layers * f.faces;
            const ktx2_level *l = &f.level[m];
            bool bad = l->uncompressed > most;
            if (f.color_model == KHR_DF_MODEL_UASTC) {
                bad = bad || l->uncompressed != most;
                if (f.scheme == KTX2_SUPERCOMPRESSION_NONE)
                    bad = bad || l->length != most;
                else if (f.scheme == KTX2_SUPERCOMPRESSION_ZSTD)
                    bad = bad || ZSTD_getFrameContentSize(f.p + l->offset, (size_t)l->length) !=
                                     (unsigned long long)most;
            }
            if (bad)
                return DE_FAIL(DE_ERR_PARSE,
                               "KTX2: Basis level %u claims %llu bytes (%llu stored); its blocks "
                               "take %llu",
                               m, (unsigned long long)l->uncompressed,
                               (unsigned long long)l->length, (unsigned long long)most);
        }
        *kind = DE_TEX_KTX2_IMAGE;
        DE_TRY(de_tex_basis_decode(a, data, size, image));
        image->cube = f.faces == 6;
        image->has_srgb_hint = true;
        image->srgb_hint = f.transfer == KHR_DF_TRANSFER_SRGB;
        return DE_OK;
    }
    if (f.scheme == KTX2_SUPERCOMPRESSION_BASISLZ)
        return DE_FAIL(DE_ERR_PARSE, "KTX2: BasisLZ supercompression with format %u", f.vk_format);
    de_gpu_format bc = bc_format(f.vk_format);
    if (bc != DE_FMT_UNKNOWN) {
        *kind = DE_TEX_KTX2_BLOCKS;
        return read_blocks(a, &f, bc, blocks);
    }
    const raw_format *rf = find_raw(f.vk_format);
    if (rf != NULL) {
        *kind = DE_TEX_KTX2_IMAGE;
        return read_raw(a, &f, rf, image);
    }
    return DE_FAIL(DE_ERR_UNSUPPORTED,
                   "KTX2: VkFormat %u is not supported (BC1/BC3 sRGB, BC4, BC5, BC6H unsigned, "
                   "BC7, R8/RG8/RGB8/RGBA8, RGBA16F, RGBA32F and Basis ETC1S/UASTC are)",
                   f.vk_format);
}
