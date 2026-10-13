/*
 * texture_bc.c: the BC encode stage of the texture importer (DENG-99, SPEC §11.6). It turns one
 * subresource of f32 RGBA into rows of 4x4 blocks: gathers each block (edge texels repeated where
 * the mip ends inside a block), converts it to what the encoder takes (sRGB or UNORM bytes, or
 * half floats for BC6H), and hands fixed ranges of DE_TEX_BC_GRAIN blocks to the C++ encoders
 * (texture_bc_enc.cpp).
 *
 * Threads: the ranges run through de_task_parallel_for with a fixed grain when a task system is
 * given, else one after another on the calling thread. A range reads only its own blocks and
 * writes only its own bytes of the output, and the encoders keep no state between calls, so the
 * output is the same for every worker count (SPEC §5.7 rule 5, §11.5).
 *
 * Built with the deterministic float flags (no FMA contraction; tools/cook/CMakeLists.txt).
 */
#include "texture_bc.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_task.h>

#include <string.h>

bool de_tex_bc_is_bc(de_gpu_format format) {
    switch (format) {
    case DE_FMT_BC1_SRGB:
    case DE_FMT_BC3_SRGB:
    case DE_FMT_BC4_UNORM:
    case DE_FMT_BC5_UNORM:
    case DE_FMT_BC6H_UFLOAT:
    case DE_FMT_BC7_UNORM:
    case DE_FMT_BC7_SRGB:
        return true;
    default:
        return false;
    }
}

/* Bytes of one block's texels as the encoders take them: 16 RGBA8 texels, or 16 RGB halves. */
#define BC_TEXELS_RGBA8 64u
#define BC_TEXELS_HALF  96u

/* The largest half the BC6H encoder gets: 0x7BFE (65472), one step below the largest finite half
 * (0x7BFF, 65504). fast_encode_bc6h asserts max < 0x7BFF on its low-variance path, and a block
 * with one channel saturated at 0x7BFF and another varying reaches it with max = 0x7BFF
 * (DENG-401). The step costs at most 0.05% at the very top of the range. */
#define BC6H_HALF_MAX 0x7BFEu

typedef struct bc_job {
    de_gpu_format format;
    const f32 *rgba;
    u32 width, height;
    u32 blocks_x, blocks_y;
    u32 block_bytes;
    u8 *dst;
    u32 row_pitch;
    bool srgb;
    bool half;
    de_tex_bc_params params;
    f32 srgb_point[255]; /**< sRGB^-1((k + 0.5) / 255): the byte decision points */
    u8 *failed;          /**< one flag per range */
} bc_job;

static u8 unorm8(f32 v) {
    if (!(v > 0.0f))
        return 0;
    if (v >= 1.0f)
        return 255;
    return (u8)(v * 255.0f + 0.5f);
}

/* Linear to an sRGB byte: the number of decision points at or below the value (the rounding of
 * texture_encode.c's RGBA8 sRGB encoder, so BC and uncompressed sRGB agree before compression). */
static u8 srgb8(const f32 *point, f32 v) {
    u32 lo = 0, hi = 255;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2u;
        if (v >= point[mid])
            lo = mid + 1u;
        else
            hi = mid;
    }
    return (u8)lo;
}

/* The texels of block (bx, by) into `out`, in raster order inside the block. Texels past the
 * mip's edge repeat the last column or row. */
static void gather(const bc_job *j, u32 bx, u32 by, u8 *out) {
    for (u32 ty = 0; ty < 4u; ++ty) {
        u32 y = by * 4u + ty;
        y = y < j->height ? y : j->height - 1u;
        for (u32 tx = 0; tx < 4u; ++tx) {
            u32 x = bx * 4u + tx;
            x = x < j->width ? x : j->width - 1u;
            const f32 *s = j->rgba + ((u64)y * j->width + x) * 4u;
            u32 t = ty * 4u + tx;
            if (j->half) {
                for (u32 c = 0; c < 3u; ++c) {
                    f32 v = s[c];
                    /* BC6H unsigned: negatives, -Inf and NaN to 0. +Inf and values that
                     * overflow a half convert to 0x7BFF, then go down to BC6H_HALF_MAX. */
                    v = v > 0.0f ? v : 0.0f;
                    u16 h = de_tex_f32_to_f16(v);
                    h = h < BC6H_HALF_MAX ? h : (u16)BC6H_HALF_MAX;
                    memcpy(out + (t * 3u + c) * 2u, &h, sizeof h);
                }
            } else {
                u8 *d = out + t * 4u;
                for (u32 c = 0; c < 3u; ++c)
                    d[c] = j->srgb ? srgb8(j->srgb_point, s[c]) : unorm8(s[c]);
                d[3] = unorm8(s[3]);
            }
        }
    }
}

/* One range of blocks: gather, encode, copy each block to its place in the rows. */
static void encode_range(u32 begin, u32 end, void *user) {
    bc_job *j = (bc_job *)user;
    /* Zeroed (28 KB, small next to encoding 256 blocks): GCC cannot see that gather() writes
     * every byte the encoders read, and -Wmaybe-uninitialized would fail the build. */
    u8 texels[DE_TEX_BC_GRAIN * BC_TEXELS_HALF];
    u8 blocks[DE_TEX_BC_GRAIN * 16u];
    memset(texels, 0, sizeof texels);
    memset(blocks, 0, sizeof blocks);
    u32 stride = j->half ? BC_TEXELS_HALF : BC_TEXELS_RGBA8;
    u32 n = end - begin;
    for (u32 i = 0; i < n; ++i) {
        u32 b = begin + i;
        gather(j, b % j->blocks_x, b / j->blocks_x, texels + i * stride);
    }
    if (!de_tex_bc_shim_encode(j->format, texels, n, blocks, &j->params)) {
        j->failed[begin / DE_TEX_BC_GRAIN] = 1;
        return;
    }
    for (u32 i = 0; i < n; ++i) {
        u32 b = begin + i;
        u32 bx = b % j->blocks_x, by = b / j->blocks_x;
        memcpy(j->dst + (u64)by * j->row_pitch + (u64)bx * j->block_bytes,
               blocks + i * j->block_bytes, j->block_bytes);
    }
}

de_result de_tex_bc_encode(const de_allocator *a, de_gpu_format format, const f32 *rgba, u32 width,
                           u32 height, u8 *dst, u32 row_pitch, const de_tex_encode_opts *opts) {
    de_cooked_texture_block blk;
    if (!de_tex_bc_is_bc(format) || !de_cooked_texture_format_block(format, &blk))
        return DE_FAIL(DE_ERR_UNSUPPORTED, "texture: %u is not a BC format", (u32)format);
    if (width == 0 || height == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "texture: empty subresource");
    de_tex_encode_opts none;
    memset(&none, 0, sizeof none);
    if (opts == NULL)
        opts = &none;
    de_tex_bc_shim_start(); /* the tables, once, before any worker reads them */
    /* The job is large (the sRGB points), so it lives in the allocator, not on the stack. */
    bc_job *j = (bc_job *)DE_ALLOC(a, sizeof *j, DE_ALIGNOF(bc_job), DE_MEM_ASSET);
    if (j == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "texture: out of memory");
    memset(j, 0, sizeof *j);
    j->format = format;
    j->rgba = rgba;
    j->width = width;
    j->height = height;
    j->blocks_x = (width + 3u) / 4u;
    j->blocks_y = (height + 3u) / 4u;
    j->block_bytes = blk.bytes;
    j->dst = dst;
    j->row_pitch = row_pitch;
    j->srgb = de_cooked_texture_format_is_srgb(format);
    j->half = format == DE_FMT_BC6H_UFLOAT;
    j->params.quality = opts->quality;
    j->params.rdo_lambda = opts->rdo_quality > 0.0f ? opts->rdo_quality : 0.0f;
    /* sRGB colour: weight the error the way the eye sees it (bc7enc's perceptual mode). */
    j->params.perceptual = j->srgb;
    if (j->srgb)
        for (u32 k = 0; k < 255u; ++k)
            j->srgb_point[k] = de_tex_srgb_to_linear(((f32)k + 0.5f) / 255.0f);
    u32 count = j->blocks_x * j->blocks_y;
    u32 ranges = (count + DE_TEX_BC_GRAIN - 1u) / DE_TEX_BC_GRAIN;
    j->failed = (u8 *)DE_ALLOC(a, ranges, 1, DE_MEM_ASSET);
    if (j->failed == NULL) {
        DE_FREE(a, j, sizeof *j);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "texture: out of memory");
    }
    memset(j->failed, 0, ranges);
    if (opts->tasks != NULL && ranges > 1u)
        de_task_parallel_for(opts->tasks, count, DE_TEX_BC_GRAIN, encode_range, j);
    else
        for (u32 b = 0; b < count; b += DE_TEX_BC_GRAIN)
            encode_range(b, b + DE_TEX_BC_GRAIN < count ? b + DE_TEX_BC_GRAIN : count, j);
    bool failed = false;
    for (u32 r = 0; r < ranges; ++r)
        failed = failed || j->failed[r] != 0;
    DE_FREE(a, j->failed, ranges);
    DE_FREE(a, j, sizeof *j);
    if (failed)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "texture: the BC encoder ran out of memory");
    return DE_OK;
}
