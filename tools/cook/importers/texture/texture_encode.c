/*
 * texture_encode.c: the encode stage and the DTEX writer of the texture importer (DENG-98,
 * SPEC §11.6).
 *
 * de_tex_choose_format holds the format rules (since DENG-99 the `compression` table of
 * texture_bc_rules.c), and k_encoders maps a de_gpu_format to the function that encodes one
 * subresource. A BC encoder gets the same input (a mip of f32 RGBA) and writes rows of 4x4 blocks
 * with the given row pitch; the DTEX writer computes block rows from
 * de_cooked_texture_format_block. DENG-98 implements the uncompressed outputs RGBA8_UNORM,
 * RGBA8_SRGB and RGBA16_FLOAT; DENG-99 the BC1/BC3/BC4/BC5/BC6H/BC7 rows (texture_bc.c).
 *
 * Built with the deterministic float flags (no FMA contraction; tools/cook/CMakeLists.txt).
 */
#include "texture_bc.h"
#include "texture_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "render/formats/de_cooked_le.h"

#include <string.h>

/* ---- Format rules ---------------------------------------------------------------------------- */

de_gpu_format de_tex_choose_format(const de_tex_settings *s, const de_tex_image *img) {
    /* DENG-99: the `compression` rules (texture_bc_rules.c). */
    return de_tex_bc_choose_image(s, img).format;
}

/* ---- Half floats ----------------------------------------------------------------------------- */

u16 de_tex_f32_to_f16(f32 f) {
    u32 x;
    memcpy(&x, &f, sizeof x);
    u32 sign = (x >> 16) & 0x8000u;
    u32 ax = x & 0x7FFFFFFFu;
    if (ax > 0x7F800000u)
        return 0; /* NaN */
    if (ax >= 0x477FF000u)
        return (u16)(sign | 0x7BFFu); /* rounds to >= 65520 or inf: the largest finite half */
    if (ax < 0x33000001u)
        return (u16)sign; /* below half the smallest denormal: zero */
    u32 e = ax >> 23;
    u32 m = (ax & 0x7FFFFFu) | 0x800000u;
    if (e < 113u) {
        /* Denormal half q x 2^-24 = m x 2^(e - 150): q = m >> (126 - e), rounded to nearest
         * even. e is 102 to 112 here, so the shift is 14 to 24. */
        u32 shift = 126u - e;
        u32 q = m >> shift;
        u32 rem = m & ((1u << shift) - 1u);
        u32 half = 1u << (shift - 1u);
        if (rem > half || (rem == half && (q & 1u)))
            ++q;
        return (u16)(sign | q);
    }
    u32 q = ((e - 112u) << 10) | ((m >> 13) & 0x3FFu);
    u32 rem = m & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (q & 1u)))
        ++q; /* may carry into the exponent, which is right */
    return (u16)(sign | q);
}

f32 de_tex_f16_to_f32(u16 h) {
    u32 sign = ((u32)h & 0x8000u) << 16;
    u32 e = ((u32)h >> 10) & 0x1Fu;
    u32 m = (u32)h & 0x3FFu;
    u32 x;
    if (e == 0) {
        if (m == 0) {
            x = sign;
        } else {
            /* Denormal: normalize. */
            e = 113u;
            while ((m & 0x400u) == 0) {
                m <<= 1;
                --e;
            }
            x = sign | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (e == 31u) {
        x = sign | 0x7F800000u | (m << 13);
    } else {
        x = sign | ((e + 112u) << 23) | (m << 13);
    }
    f32 f;
    memcpy(&f, &x, sizeof f);
    return f;
}

/* ---- Encoders -------------------------------------------------------------------------------- */

static u8 unorm8(f32 v) {
    if (!(v > 0.0f))
        return 0;
    if (v >= 1.0f)
        return 255;
    return (u8)(v * 255.0f + 0.5f);
}

static de_result enc_rgba8_unorm(const f32 *rgba, u32 w, u32 h, u8 *dst, u32 pitch) {
    for (u32 y = 0; y < h; ++y) {
        const f32 *s = rgba + (u64)y * w * 4u;
        u8 *d = dst + (u64)y * pitch;
        for (u32 i = 0; i < w * 4u; ++i)
            d[i] = unorm8(s[i]);
    }
    return DE_OK;
}

/* Linear to sRGB bytes through the 255 decision points sRGB^-1((k + 0.5) / 255): the byte is the
 * number of points at or below the value. This is rounding in sRGB space, exact up to the EOTF's
 * own accuracy, and needs one pow per point instead of one per texel. Alpha stays linear. */
static de_result enc_rgba8_srgb(const f32 *rgba, u32 w, u32 h, u8 *dst, u32 pitch) {
    f32 point[255];
    for (u32 k = 0; k < 255u; ++k)
        point[k] = de_tex_srgb_to_linear(((f32)k + 0.5f) / 255.0f);
    for (u32 y = 0; y < h; ++y) {
        const f32 *s = rgba + (u64)y * w * 4u;
        u8 *d = dst + (u64)y * pitch;
        for (u32 x = 0; x < w; ++x) {
            for (u32 c = 0; c < 3; ++c) {
                f32 v = s[x * 4u + c];
                u32 lo = 0, hi = 255; /* the answer lies in [lo, hi] */
                while (lo < hi) {
                    u32 mid = (lo + hi) / 2u;
                    if (v >= point[mid])
                        lo = mid + 1u;
                    else
                        hi = mid;
                }
                d[x * 4u + c] = (u8)lo;
            }
            d[x * 4u + 3u] = unorm8(s[x * 4u + 3u]);
        }
    }
    return DE_OK;
}

static de_result enc_rgba16f(const f32 *rgba, u32 w, u32 h, u8 *dst, u32 pitch) {
    for (u32 y = 0; y < h; ++y) {
        const f32 *s = rgba + (u64)y * w * 4u;
        u8 *d = dst + (u64)y * pitch;
        for (u32 i = 0; i < w * 4u; ++i) {
            u16 v = de_tex_f32_to_f16(s[i]);
            d[2u * i] = (u8)v;
            d[2u * i + 1u] = (u8)(v >> 8);
        }
    }
    return DE_OK;
}

typedef de_result (*encode_fn)(const f32 *rgba, u32 w, u32 h, u8 *dst, u32 pitch);
/* DENG-99: encoders that take options and scratch (the BC formats). */
typedef de_result (*encode_ex_fn)(const de_allocator *a, de_gpu_format format, const f32 *rgba,
                                  u32 w, u32 h, u8 *dst, u32 pitch, const de_tex_encode_opts *o);

/* Format -> encoder: `fn` for the plain formats, `ex` for the BC formats. */
static const struct {
    de_gpu_format format;
    encode_fn fn;
    encode_ex_fn ex;
} k_encoders[] = {
    {DE_FMT_RGBA8_UNORM, enc_rgba8_unorm, NULL},
    {DE_FMT_RGBA8_SRGB, enc_rgba8_srgb, NULL},
    {DE_FMT_RGBA16_FLOAT, enc_rgba16f, NULL},
    /* DENG-99: block compression (texture_bc.c). */
    {DE_FMT_BC1_SRGB, NULL, de_tex_bc_encode},
    {DE_FMT_BC3_SRGB, NULL, de_tex_bc_encode},
    {DE_FMT_BC4_UNORM, NULL, de_tex_bc_encode},
    {DE_FMT_BC5_UNORM, NULL, de_tex_bc_encode},
    {DE_FMT_BC6H_UFLOAT, NULL, de_tex_bc_encode},
    {DE_FMT_BC7_UNORM, NULL, de_tex_bc_encode},
    {DE_FMT_BC7_SRGB, NULL, de_tex_bc_encode},
};

static int find_encoder(de_gpu_format f) {
    for (u32 i = 0; i < (u32)(sizeof k_encoders / sizeof k_encoders[0]); ++i)
        if (k_encoders[i].format == f)
            return (int)i;
    return -1;
}

bool de_tex_can_encode(de_gpu_format format) {
    return find_encoder(format) >= 0;
}

de_result de_tex_encode_ex(const de_allocator *a, de_gpu_format format, const f32 *rgba, u32 width,
                           u32 height, u8 *dst, u32 row_pitch, const de_tex_encode_opts *opts) {
    int i = find_encoder(format);
    if (i < 0)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "texture: no encoder for format %u", (u32)format);
    if (k_encoders[i].ex != NULL)
        return k_encoders[i].ex(a, format, rgba, width, height, dst, row_pitch, opts);
    return k_encoders[i].fn(rgba, width, height, dst, row_pitch);
}

de_result de_tex_encode(de_gpu_format format, const f32 *rgba, u32 width, u32 height, u8 *dst,
                        u32 row_pitch) {
    return de_tex_encode_ex(de_heap_allocator(), format, rgba, width, height, dst, row_pitch, NULL);
}

/* ---- DTEX writer ----------------------------------------------------------------------------- */

void de_tex_dtex_free(const de_allocator *a, de_tex_dtex *t) {
    if (t == NULL)
        return;
    if (t->data != NULL)
        DE_FREE(a, t->data, (size_t)t->size);
    if (t->offset != NULL)
        DE_FREE(a, t->offset, (size_t)t->table_bytes);
    memset(t, 0, sizeof *t);
}

de_result de_tex_dtex_begin(const de_allocator *a, const de_tex_dtex_desc *d, de_tex_dtex *out) {
    memset(out, 0, sizeof *out);
    de_cooked_texture_block blk;
    if (!de_cooked_texture_format_block(d->format, &blk))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "texture: DTEX cannot hold format %u", (u32)d->format);
    if (d->mip_count == 0 || d->mip_count > DE_COOKED_TEXTURE_MAX_MIPS || d->layers == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "texture: bad mip or layer count");
    u32 count = d->layers * d->mip_count;
    out->count = count;
    out->table_bytes = (u64)count * (sizeof(u64) + 2u * sizeof(u32));
    out->offset = (u64 *)DE_ALLOC(a, (size_t)out->table_bytes, DE_ALIGNOF(u64), DE_MEM_ASSET);
    if (out->offset == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "texture: out of memory");
    out->row_pitch = (u32 *)(void *)(out->offset + count);
    out->row_count = out->row_pitch + count;
    /* Layout: header, table, then each subresource at the next 512-byte boundary. */
    u64 pos = DE_COOKED_TEXTURE_HEADER_SIZE + (u64)count * DE_COOKED_TEXTURE_SUBRESOURCE_SIZE;
    for (u32 l = 0; l < d->layers; ++l)
        for (u32 m = 0; m < d->mip_count; ++m) {
            u32 i = l * d->mip_count + m;
            u32 w = de_cooked_texture_mip_extent(d->width, m);
            u32 h = de_cooked_texture_mip_extent(d->height, m);
            pos = de_cooked_texture_align(pos, DE_COOKED_TEXTURE_SUBRESOURCE_ALIGN);
            out->offset[i] = pos;
            out->row_pitch[i] = (u32)de_cooked_texture_align(de_cooked_texture_row_bytes(&blk, w),
                                                             DE_COOKED_TEXTURE_ROW_ALIGN);
            out->row_count[i] = de_cooked_texture_row_count(&blk, h);
            pos += (u64)out->row_pitch[i] * out->row_count[i];
        }
    out->size = pos;
    out->data = (u8 *)DE_ALLOC(a, (size_t)pos, 16, DE_MEM_ASSET);
    if (out->data == NULL) {
        de_tex_dtex_free(a, out);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "texture: out of memory for %llu cooked bytes",
                       (unsigned long long)pos);
    }
    /* Zero everything first: padding bytes are part of the output (SPEC §11.5). */
    memset(out->data, 0, (size_t)pos);
    u8 *p = out->data;
    u32 flags = d->flags & ~(u32)(DE_COOKED_TEXTURE_SRGB | DE_COOKED_TEXTURE_CUBE);
    if (de_cooked_texture_format_is_srgb(d->format))
        flags |= DE_COOKED_TEXTURE_SRGB;
    if (d->dim == DE_TEX_CUBE)
        flags |= DE_COOKED_TEXTURE_CUBE;
    memcpy(p, DE_COOKED_TEXTURE_MAGIC, 4);
    de_cooked_put_u32(p + 4, DE_COOKED_TEXTURE_VERSION);
    de_cooked_put_u32(p + 8, (u32)d->dim);
    de_cooked_put_u32(p + 12, (u32)d->format);
    de_cooked_put_u32(p + 16, d->width);
    de_cooked_put_u32(p + 20, d->height);
    de_cooked_put_u32(p + 24, d->layers);
    de_cooked_put_u32(p + 28, d->mip_count);
    de_cooked_put_u32(p + 32, flags);
    de_cooked_put_u32(p + 36, (u32)d->usage);
    de_cooked_put_u32(p + 40, count);
    de_cooked_put_u64(p + 48, pos);
    for (u32 i = 0; i < count; ++i) {
        u8 *e = p + DE_COOKED_TEXTURE_HEADER_SIZE + (u64)i * DE_COOKED_TEXTURE_SUBRESOURCE_SIZE;
        de_cooked_put_u64(e, out->offset[i]);
        de_cooked_put_u64(e + 8, (u64)out->row_pitch[i] * out->row_count[i]);
        de_cooked_put_u32(e + 16, out->row_pitch[i]);
        de_cooked_put_u32(e + 20, out->row_count[i]);
    }
    return DE_OK;
}

de_result de_tex_dtex_from_chain(const de_allocator *a, const de_tex_dtex_desc *d,
                                 const de_tex_chain *chain, de_tex_dtex *out) {
    if (chain->mip_count != d->mip_count || chain->layers != d->layers)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "texture: the chain does not match the header");
    if (!de_tex_can_encode(d->format))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "texture: no encoder for format %u", (u32)d->format);
    DE_TRY(de_tex_dtex_begin(a, d, out));
    for (u32 l = 0; l < d->layers; ++l)
        for (u32 m = 0; m < d->mip_count; ++m) {
            u32 i = l * d->mip_count + m;
            u32 w = chain->width[m], h = chain->height[m];
            const f32 *src = chain->rgba[m] + (u64)l * w * h * 4u;
            de_result r = de_tex_encode_ex(a, d->format, src, w, h, out->data + out->offset[i],
                                           out->row_pitch[i], &d->encode);
            if (r != DE_OK) {
                de_tex_dtex_free(a, out);
                return r;
            }
        }
    return DE_OK;
}

de_result de_tex_dtex_from_blocks(const de_allocator *a, const de_tex_dtex_desc *d,
                                  const de_tex_blocks *b, u32 first_mip, de_tex_dtex *out) {
    if (d->layers != b->layers || first_mip + d->mip_count > b->mip_count || d->format != b->format)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "texture: the blocks do not match the header");
    de_cooked_texture_block blk;
    if (!de_cooked_texture_format_block(d->format, &blk))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "texture: DTEX cannot hold format %u", (u32)d->format);
    DE_TRY(de_tex_dtex_begin(a, d, out));
    for (u32 l = 0; l < d->layers; ++l)
        for (u32 m = 0; m < d->mip_count; ++m) {
            u32 i = l * d->mip_count + m;
            u64 src_i = (u64)l * b->mip_count + first_mip + m;
            u32 w = de_cooked_texture_mip_extent(d->width, m);
            u64 row = de_cooked_texture_row_bytes(&blk, w);
            if ((u64)row * out->row_count[i] != b->size[src_i]) {
                de_tex_dtex_free(a, out);
                return DE_FAIL(DE_ERR_INTERNAL, "texture: block level size mismatch");
            }
            for (u32 y = 0; y < out->row_count[i]; ++y)
                memcpy(out->data + out->offset[i] + (u64)y * out->row_pitch[i],
                       b->data[src_i] + (u64)y * row, (size_t)row);
        }
    return DE_OK;
}
