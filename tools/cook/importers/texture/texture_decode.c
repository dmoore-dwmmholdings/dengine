/*
 * texture_decode.c: the texture importer's source decoders (DENG-98, SPEC §11.6): source
 * sniffing, PNG and JPEG through stb_image, OpenEXR through tinyexr. KTX2 is in texture_ktx2.c.
 *
 * Every decoder reads the image size from the header first and refuses a size over
 * DE_COOKED_TEXTURE_MAX_SIZE before it decodes a pixel, so a hostile header cannot make it
 * allocate gigabytes. The libraries allocate with malloc; their output is converted to f32 RGBA
 * in a block of the caller's allocator and freed at once.
 */
#include "texture_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <stb_image.h>
#include <tinyexr.h>

#include <limits.h>
#include <string.h>

/* ---- Sniffing -------------------------------------------------------------------------------- */

de_tex_source_kind de_tex_sniff(const void *data, u64 size) {
    static const u8 k_png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    static const u8 k_exr[4] = {0x76, 0x2F, 0x31, 0x01};
    static const u8 k_ktx2[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    const u8 *p = (const u8 *)data;
    if (p == NULL)
        return DE_TEX_SOURCE_UNKNOWN;
    if (size >= sizeof k_png && memcmp(p, k_png, sizeof k_png) == 0)
        return DE_TEX_SOURCE_PNG;
    if (size >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF)
        return DE_TEX_SOURCE_JPEG;
    if (size >= sizeof k_exr && memcmp(p, k_exr, sizeof k_exr) == 0)
        return DE_TEX_SOURCE_EXR;
    if (size >= sizeof k_ktx2 && memcmp(p, k_ktx2, sizeof k_ktx2) == 0)
        return DE_TEX_SOURCE_KTX2;
    return DE_TEX_SOURCE_UNKNOWN;
}

const char *de_tex_source_kind_name(de_tex_source_kind k) {
    switch (k) {
    case DE_TEX_SOURCE_PNG:
        return "PNG";
    case DE_TEX_SOURCE_JPEG:
        return "JPEG";
    case DE_TEX_SOURCE_EXR:
        return "EXR";
    case DE_TEX_SOURCE_KTX2:
        return "KTX2";
    default:
        return "unknown";
    }
}

/* ---- Images ---------------------------------------------------------------------------------- */

void de_tex_image_free(const de_allocator *a, de_tex_image *img) {
    if (img == NULL)
        return;
    if (img->rgba != NULL)
        DE_FREE(a, img->rgba, (size_t)img->rgba_bytes);
    memset(img, 0, sizeof *img);
}

de_result de_tex_image_alloc(const de_allocator *a, de_tex_image *img) {
    u64 texels = (u64)img->width * img->height * (img->layers != 0 ? img->layers : 1u);
    u64 bytes = texels * 4u * sizeof(f32);
    if (texels == 0 || bytes > (u64)SIZE_MAX)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "texture: image of %llu texels",
                       (unsigned long long)texels);
    img->rgba = (f32 *)DE_ALLOC(a, (size_t)bytes, DE_ALIGNOF(f32), DE_MEM_ASSET);
    if (img->rgba == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "texture: out of memory for %ux%u texels", img->width,
                       img->height);
    memset(img->rgba, 0, (size_t)bytes);
    img->rgba_bytes = bytes;
    return DE_OK;
}

de_result de_tex_check_size(u32 width, u32 height, const char *what) {
    if (width == 0 || height == 0)
        return DE_FAIL(DE_ERR_PARSE, "%s: the image is empty (%ux%u)", what, width, height);
    if (width > DE_COOKED_TEXTURE_MAX_SIZE || height > DE_COOKED_TEXTURE_MAX_SIZE)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "%s: %ux%u is larger than the limit of %u", what, width,
                       height, DE_COOKED_TEXTURE_MAX_SIZE);
    return DE_OK;
}

/* ---- PNG and JPEG (stb_image) ---------------------------------------------------------------- */

de_result de_tex_decode_stb(const de_allocator *a, const void *data, u64 size, de_tex_image *out) {
    memset(out, 0, sizeof *out);
    if (size > (u64)INT_MAX)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "image: the file is larger than 2 GiB");
    const stbi_uc *p = (const stbi_uc *)data;
    int len = (int)size;
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(p, len, &w, &h, &comp))
        return DE_FAIL(DE_ERR_PARSE, "image: not a valid PNG or JPEG file (%s)",
                       stbi_failure_reason());
    DE_TRY(de_tex_check_size((u32)w, (u32)h, "image"));
    bool sixteen = stbi_is_16_bit_from_memory(p, len) != 0;
    out->width = (u32)w;
    out->height = (u32)h;
    out->layers = 1;
    out->channels = comp >= 1 && comp <= 4 ? (u32)comp : 4u;
    int dw = 0, dh = 0, dc = 0;
    void *pixels = sixteen ? (void *)stbi_load_16_from_memory(p, len, &dw, &dh, &dc, 4)
                           : (void *)stbi_load_from_memory(p, len, &dw, &dh, &dc, 4);
    if (pixels == NULL)
        return DE_FAIL(DE_ERR_PARSE, "image: decoding failed (%s)", stbi_failure_reason());
    if (dw != w || dh != h) {
        stbi_image_free(pixels);
        return DE_FAIL(DE_ERR_PARSE, "image: the decoded size differs from the header");
    }
    de_result r = de_tex_image_alloc(a, out);
    if (r != DE_OK) {
        stbi_image_free(pixels);
        return r;
    }
    u64 n = (u64)out->width * out->height * 4u;
    if (sixteen) {
        const u16 *s = (const u16 *)pixels;
        for (u64 i = 0; i < n; ++i)
            out->rgba[i] = (f32)s[i] / 65535.0f;
    } else {
        const u8 *s = (const u8 *)pixels;
        for (u64 i = 0; i < n; ++i)
            out->rgba[i] = (f32)s[i] / 255.0f;
    }
    stbi_image_free(pixels);
    return DE_OK;
}

/* ---- OpenEXR (tinyexr) ----------------------------------------------------------------------- */

/* tinyexr's error message, freed after it is copied into the error context. */
static de_result exr_fail(de_result code, const char *what, const char *err) {
    de_result r =
        DE_FAIL(code, "EXR: %s%s%s", what, err != NULL ? ": " : "", err != NULL ? err : "");
    if (err != NULL)
        FreeEXRErrorMessage(err);
    return r;
}

/* Value of channel `c` at texel `i` of a channel plane of pixel type `type` (after the HALF to
 * FLOAT request). */
static f32 exr_value(const unsigned char *plane, int type, u64 i) {
    if (type == TINYEXR_PIXELTYPE_UINT) {
        unsigned int v;
        memcpy(&v, plane + i * sizeof v, sizeof v);
        return (f32)v;
    }
    f32 f;
    memcpy(&f, plane + i * sizeof f, sizeof f);
    return f;
}

de_result de_tex_decode_exr(const de_allocator *a, const void *data, u64 size, de_tex_image *out) {
    memset(out, 0, sizeof *out);
    const unsigned char *p = (const unsigned char *)data;
    EXRVersion ver;
    memset(&ver, 0, sizeof ver);
    if (ParseEXRVersionFromMemory(&ver, p, (size_t)size) != TINYEXR_SUCCESS)
        return DE_FAIL(DE_ERR_PARSE, "EXR: not a valid OpenEXR file");
    if (ver.multipart || ver.non_image)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "EXR: multi-part and deep files are not supported");
    EXRHeader hdr;
    InitEXRHeader(&hdr);
    const char *err = NULL;
    if (ParseEXRHeaderFromMemory(&hdr, &ver, p, (size_t)size, &err) != TINYEXR_SUCCESS) {
        FreeEXRHeader(&hdr);
        return exr_fail(DE_ERR_PARSE, "bad header", err);
    }
    i64 w = (i64)hdr.data_window.max_x - hdr.data_window.min_x + 1;
    i64 h = (i64)hdr.data_window.max_y - hdr.data_window.min_y + 1;
    if (w <= 0 || h <= 0 || w > (i64)UINT32_MAX || h > (i64)UINT32_MAX) {
        FreeEXRHeader(&hdr);
        return DE_FAIL(DE_ERR_PARSE, "EXR: bad data window");
    }
    de_result r = de_tex_check_size((u32)w, (u32)h, "EXR");
    if (r != DE_OK) {
        FreeEXRHeader(&hdr);
        return r;
    }
    /* Channels by name; luminance-only files (Y) become grey. */
    int ir = -1, ig = -1, ib = -1, ia = -1, iy = -1;
    for (int c = 0; c < hdr.num_channels; ++c) {
        const char *n = hdr.channels[c].name;
        if (strcmp(n, "R") == 0)
            ir = c;
        else if (strcmp(n, "G") == 0)
            ig = c;
        else if (strcmp(n, "B") == 0)
            ib = c;
        else if (strcmp(n, "A") == 0)
            ia = c;
        else if (strcmp(n, "Y") == 0)
            iy = c;
        if (hdr.pixel_types[c] == TINYEXR_PIXELTYPE_HALF)
            hdr.requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;
    }
    if (ir < 0 && iy < 0) {
        FreeEXRHeader(&hdr);
        return DE_FAIL(DE_ERR_UNSUPPORTED, "EXR: no R or Y channel");
    }
    EXRImage img;
    InitEXRImage(&img);
    err = NULL;
    if (LoadEXRImageFromMemory(&img, &hdr, p, (size_t)size, &err) != TINYEXR_SUCCESS) {
        FreeEXRImage(&img);
        FreeEXRHeader(&hdr);
        return exr_fail(DE_ERR_PARSE, "decoding failed", err);
    }
    if (img.width != (int)w || img.height != (int)h) {
        FreeEXRImage(&img);
        FreeEXRHeader(&hdr);
        return DE_FAIL(DE_ERR_PARSE, "EXR: the decoded size differs from the header");
    }
    out->width = (u32)w;
    out->height = (u32)h;
    out->layers = 1;
    out->hdr = true;
    out->channels = ir >= 0 ? (ia >= 0 ? 4u : (ig >= 0 && ib >= 0 ? 3u : 1u)) : (ia >= 0 ? 2u : 1u);
    r = de_tex_image_alloc(a, out);
    if (r != DE_OK) {
        FreeEXRImage(&img);
        FreeEXRHeader(&hdr);
        return r;
    }
    /* Source channel per output channel: R G B A, or Y Y Y A. Missing G or B copy R. */
    int src[4];
    src[0] = ir >= 0 ? ir : iy;
    src[1] = ir >= 0 ? (ig >= 0 ? ig : ir) : iy;
    src[2] = ir >= 0 ? (ib >= 0 ? ib : ir) : iy;
    src[3] = ia;
    u32 ow = out->width;
    if (img.tiles == NULL) {
        for (u64 t = 0; t < (u64)ow * out->height; ++t)
            for (u32 c = 0; c < 4; ++c)
                out->rgba[t * 4u + c] =
                    src[c] < 0
                        ? 1.0f
                        : exr_value(img.images[src[c]], hdr.requested_pixel_types[src[c]], t);
    } else {
        /* Level 0 tiles: offsets in tile units, partial tiles at the right and bottom edges. */
        u32 tsx = (u32)hdr.tile_size_x, tsy = (u32)hdr.tile_size_y;
        for (int it = 0; it < img.num_tiles; ++it) {
            const EXRTile *tile = &img.tiles[it];
            if (tile->level_x != 0 || tile->level_y != 0 || tile->offset_x < 0 ||
                tile->offset_y < 0)
                continue;
            for (u32 j = 0; j < (u32)tile->height; ++j)
                for (u32 i = 0; i < (u32)tile->width; ++i) {
                    u64 x = (u64)tile->offset_x * tsx + i, y = (u64)tile->offset_y * tsy + j;
                    if (x >= ow || y >= out->height)
                        continue;
                    u64 t = y * ow + x, s = (u64)j * tsx + i;
                    for (u32 c = 0; c < 4; ++c)
                        out->rgba[t * 4u + c] =
                            src[c] < 0 ? 1.0f
                                       : exr_value(tile->images[src[c]],
                                                   hdr.requested_pixel_types[src[c]], s);
                }
        }
    }
    FreeEXRImage(&img);
    FreeEXRHeader(&hdr);
    /* Non-finite values (NaN, inf) would poison every mip that touches them. */
    for (u64 i = 0; i < (u64)ow * out->height * 4u; ++i) {
        f32 v = out->rgba[i];
        if (v != v)
            out->rgba[i] = 0.0f;
        else if (v > 65504.0f)
            out->rgba[i] = 65504.0f;
        else if (v < -65504.0f)
            out->rgba[i] = -65504.0f;
    }
    return DE_OK;
}
