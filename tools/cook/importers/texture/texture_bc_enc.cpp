// texture_bc_enc.cpp: the C++ BC encoders behind the texture importer's C interface (DENG-99;
// DENG-2 Q1: C++ only behind a C interface inside one module). It exports two C functions,
// de_tex_bc_shim_start and de_tex_bc_shim_encode (texture_bc.h).
//
//   BC7                bc7enc (bc7enc_rdo), modes 1, 5, 6, 7 and the uber search per `quality`
//   BC1, BC3, BC4, BC5 rgbcx (bc7enc_rdo): 4-colour BC1 only (no 3-colour blocks: their black
//                      index reads as transparent on BC1 RGBA, which is how GPUs sample BC1)
//   BC6H               the Basis Universal transcoder's BC6H encoder
//                      (basist::astc_6x6_hdr::fast_encode_bc6h, the one it uses for UASTC HDR)
//   rate-distortion    ert::reduce_entropy (bc7enc_rdo) over the blocks of one call, BC1 to BC5
//                      and BC7, when `rdo_quality` > 0
//
// Determinism: every call works on its own blocks with its own state; the encoders' global
// tables are set up once (function-local statics, thread-safe in C++11) and only read afterwards.
// None of the encoders uses threads or random restarts. The vendored code is built without FMA
// contraction.
#include "texture_bc.h"

// basisu_astc_hdr_core.h (the BC6H encoder's declaration) needs the other two first, in this order.
// clang-format off
#include <basisu_transcoder.h>
#include <basisu_astc_helpers.h>
#include <basisu_astc_hdr_core.h>
// clang-format on

#include <bc7decomp.h>
#include <bc7enc.h>
#include <ert.h>
#include <rgbcx.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace {

void start_once() {
    static const bool started = [] {
        bc7enc_compress_block_init();
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        return true;
    }();
    (void)started;
    // The BC6H encoder's tables are part of the transcoder's start (texture_basis.cpp).
    de_tex_basis_start();
}

// ---- Block encoders -------------------------------------------------------------------------

void bc7_params(const de_tex_bc_params *p, bc7enc_compress_block_params *bp) {
    bc7enc_compress_block_params_init(bp);
    if (p->perceptual)
        bc7enc_compress_block_params_init_perceptual_weights(bp);
    else
        bc7enc_compress_block_params_init_linear_weights(bp);
    switch (p->quality) {
    case DE_TEX_QUALITY_FAST:
        bp->m_uber_level = 0;
        bp->m_max_partitions = 16;
        bp->m_mode17_partition_estimation_filterbank = true;
        break;
    case DE_TEX_QUALITY_HIGH:
        bp->m_uber_level = BC7ENC_MAX_UBER_LEVEL;
        bp->m_max_partitions = BC7ENC_MAX_PARTITIONS;
        bp->m_mode17_partition_estimation_filterbank = false;
        break;
    case DE_TEX_QUALITY_NORMAL:
    default:
        bp->m_uber_level = 1;
        bp->m_max_partitions = BC7ENC_MAX_PARTITIONS;
        break;
    }
}

std::uint32_t bc1_level(de_tex_quality q) {
    switch (q) {
    case DE_TEX_QUALITY_FAST:
        return 2;
    case DE_TEX_QUALITY_HIGH:
        return rgbcx::MAX_LEVEL;
    case DE_TEX_QUALITY_NORMAL:
    default:
        return 10;
    }
}

std::uint32_t bc4_radius(de_tex_quality q) {
    return q == DE_TEX_QUALITY_HIGH ? 5u : rgbcx::BC4_DEFAULT_SEARCH_RAD;
}

void encode_bc4_block(void *dst, const std::uint8_t *px, de_tex_quality q) {
    if (q == DE_TEX_QUALITY_FAST)
        rgbcx::encode_bc4(dst, px, 4);
    else
        rgbcx::encode_bc4_hq(dst, px, 4, bc4_radius(q));
}

basist::astc_6x6_hdr::fast_bc6h_params bc6h_params(de_tex_quality q) {
    basist::astc_6x6_hdr::fast_bc6h_params bp;
    switch (q) {
    case DE_TEX_QUALITY_FAST:
        bp.m_hq_ls = false;
        bp.m_num_diff_endpoint_modes_to_try =
            basist::astc_6x6_hdr::BC6H_NUM_DIFF_ENDPOINT_MODES_TO_TRY_2;
        bp.m_max_2subset_pats_to_try = 0;
        break;
    case DE_TEX_QUALITY_HIGH:
        bp.m_hq_ls = true;
        bp.m_num_diff_endpoint_modes_to_try =
            basist::astc_6x6_hdr::BC6H_NUM_DIFF_ENDPOINT_MODES_TO_TRY_9;
        bp.m_max_2subset_pats_to_try = 32; // every pattern (BC6H_NUM_PATS)
        bp.m_brute_force_weight4_assignment = true;
        break;
    case DE_TEX_QUALITY_NORMAL:
    default:
        bp.m_hq_ls = true;
        bp.m_num_diff_endpoint_modes_to_try =
            basist::astc_6x6_hdr::BC6H_NUM_DIFF_ENDPOINT_MODES_TO_TRY_4;
        bp.m_max_2subset_pats_to_try = 4;
        break;
    }
    return bp;
}

// ---- Rate-distortion (ert) ------------------------------------------------------------------

bool unpack_bc1_block(const void *block, ert::color_rgba *px, std::uint32_t, void *) {
    // 4-colour blocks only are written, so the 3-colour flag never matters.
    rgbcx::unpack_bc1(block, px, true, rgbcx::bc1_approx_mode::cBC1Ideal);
    return true;
}

bool unpack_bc4_block(const void *block, ert::color_rgba *px, std::uint32_t, void *) {
    std::memset(px, 0, sizeof(ert::color_rgba) * 16);
    rgbcx::unpack_bc4(block, reinterpret_cast<std::uint8_t *>(px), 4);
    return true;
}

bool unpack_bc7_block(const void *block, ert::color_rgba *px, std::uint32_t, void *) {
    return bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba *>(px));
}

float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

ert::reduce_entropy_params ert_params(float lambda, std::uint32_t comps, bool bc7) {
    ert::reduce_entropy_params ep;
    ep.m_lambda = lambda;
    // bc7enc_rdo's defaults (rdo_bc_encoder.cpp): a 128-byte window, and a smooth-block error
    // scale that grows with lambda so flat areas do not band.
    ep.m_lookback_window_size = 128;
    const float t = std::min(1.0f, lambda / 4.0f);
    ep.m_smooth_block_max_mse_scale =
        bc7 || comps >= 3 ? lerpf(15.0f, 50.0f, t) : lerpf(10.0f, 30.0f, t);
    for (std::uint32_t c = comps; c < 4; ++c)
        ep.m_color_weights[c] = 0;
    return ep;
}

// One channel of every texel (for BC4 parts), as ert pixels with the value in component 0.
std::vector<ert::color_rgba> channel_pixels(const std::uint8_t *px, std::uint32_t count,
                                            std::uint32_t channel) {
    std::vector<ert::color_rgba> out(static_cast<std::size_t>(count) * 16u);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i].m_c[0] = px[i * 4u + channel];
        out[i].m_c[1] = out[i].m_c[2] = out[i].m_c[3] = 0;
    }
    return out;
}

void rdo(de_gpu_format f, const std::uint8_t *px, std::uint32_t count, std::uint8_t *out,
         float lambda) {
    std::uint32_t modified = 0;
    // ert::color_rgba is four bytes, the layout of the RGBA8 texels.
    const ert::color_rgba *rgba = reinterpret_cast<const ert::color_rgba *>(px);
    switch (f) {
    case DE_FMT_BC7_UNORM:
    case DE_FMT_BC7_SRGB: {
        ert::reduce_entropy_params ep = ert_params(lambda, 4, true);
        ert::reduce_entropy(out, count, 16, 16, 4, 4, 4, rgba, ep, modified, unpack_bc7_block,
                            nullptr);
        break;
    }
    case DE_FMT_BC1_SRGB: {
        ert::reduce_entropy_params ep = ert_params(lambda, 3, false);
        ert::reduce_entropy(out, count, 8, 8, 4, 4, 3, rgba, ep, modified, unpack_bc1_block,
                            nullptr);
        break;
    }
    case DE_FMT_BC3_SRGB: {
        std::vector<ert::color_rgba> a = channel_pixels(px, count, 3);
        ert::reduce_entropy_params ea = ert_params(lambda, 1, false);
        ert::reduce_entropy(out, count, 16, 8, 4, 4, 1, a.data(), ea, modified, unpack_bc4_block,
                            nullptr);
        ert::reduce_entropy_params ep = ert_params(lambda, 3, false);
        ert::reduce_entropy(out + 8, count, 16, 8, 4, 4, 3, rgba, ep, modified, unpack_bc1_block,
                            nullptr);
        break;
    }
    case DE_FMT_BC4_UNORM: {
        std::vector<ert::color_rgba> r = channel_pixels(px, count, 0);
        ert::reduce_entropy_params ep = ert_params(lambda, 1, false);
        ert::reduce_entropy(out, count, 8, 8, 4, 4, 1, r.data(), ep, modified, unpack_bc4_block,
                            nullptr);
        break;
    }
    case DE_FMT_BC5_UNORM: {
        ert::reduce_entropy_params ep = ert_params(lambda, 1, false);
        for (std::uint32_t c = 0; c < 2; ++c) {
            std::vector<ert::color_rgba> ch = channel_pixels(px, count, c);
            ert::reduce_entropy(out + 8u * c, count, 16, 8, 4, 4, 1, ch.data(), ep, modified,
                                unpack_bc4_block, nullptr);
        }
        break;
    }
    default:
        break; // BC6H: no unpacker for ert; the pass is skipped
    }
}

bool encode(de_gpu_format f, const void *pixels, std::uint32_t count, std::uint8_t *out,
            const de_tex_bc_params *p) {
    start_once();
    const std::uint8_t *px = static_cast<const std::uint8_t *>(pixels);
    switch (f) {
    case DE_FMT_BC7_UNORM:
    case DE_FMT_BC7_SRGB: {
        bc7enc_compress_block_params bp;
        bc7_params(p, &bp);
        for (std::uint32_t b = 0; b < count; ++b)
            bc7enc_compress_block(out + 16u * b, px + 64u * b, &bp);
        break;
    }
    case DE_FMT_BC1_SRGB: {
        const std::uint32_t level = bc1_level(p->quality);
        for (std::uint32_t b = 0; b < count; ++b)
            rgbcx::encode_bc1(level, out + 8u * b, px + 64u * b, false, false);
        break;
    }
    case DE_FMT_BC3_SRGB: {
        const std::uint32_t level = bc1_level(p->quality);
        for (std::uint32_t b = 0; b < count; ++b) {
            if (p->quality == DE_TEX_QUALITY_FAST)
                rgbcx::encode_bc3(level, out + 16u * b, px + 64u * b);
            else
                rgbcx::encode_bc3_hq(level, out + 16u * b, px + 64u * b, bc4_radius(p->quality));
        }
        break;
    }
    case DE_FMT_BC4_UNORM:
        for (std::uint32_t b = 0; b < count; ++b)
            encode_bc4_block(out + 8u * b, px + 64u * b, p->quality);
        break;
    case DE_FMT_BC5_UNORM:
        for (std::uint32_t b = 0; b < count; ++b) {
            encode_bc4_block(out + 16u * b, px + 64u * b, p->quality);
            encode_bc4_block(out + 16u * b + 8u, px + 64u * b + 1u, p->quality);
        }
        break;
    case DE_FMT_BC6H_UFLOAT: {
        const basist::astc_6x6_hdr::fast_bc6h_params bp = bc6h_params(p->quality);
        const basist::half_float *h = static_cast<const basist::half_float *>(pixels);
        for (std::uint32_t b = 0; b < count; ++b)
            basist::astc_6x6_hdr::fast_encode_bc6h(
                h + 48u * b, reinterpret_cast<basist::bc6h_block *>(out + 16u * b), bp);
        return true;
    }
    default:
        return false;
    }
    if (p->rdo_lambda > 0.0f)
        rdo(f, px, count, out, p->rdo_lambda);
    return true;
}

} // namespace

extern "C" void de_tex_bc_shim_start(void) {
    start_once();
}

extern "C" bool de_tex_bc_shim_encode(de_gpu_format format, const void *pixels, u32 count, u8 *out,
                                      const de_tex_bc_params *p) {
    try {
        return encode(format, pixels, count, out, p);
    } catch (...) {
        return false; // std::bad_alloc from ert's buffers; nothing else throws
    }
}
