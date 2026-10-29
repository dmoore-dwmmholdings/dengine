#include "editor_settings_image.h"
#include "render/formats/de_cooked_env.h"
#include <bcdec.h>
#include <math.h>
#include <string.h>

static f32 half(u16 h) {
    u32 e = (h >> 10) & 31u, m = h & 1023u;
    f32 v = e == 0    ? (f32)m / 16777216.0f
            : e == 31 ? (m ? 0 : 65504.0f)
                      : (1.0f + (f32)m / 1024.0f) * ldexpf(1.0f, (i32)e - 15);
    return h & 0x8000u ? -v : v;
}
static u8 channel(f32 v, bool srgb) {
    if (!isfinite(v))
        v = v > 0 ? 1 : 0;
    v = fmaxf(0, fminf(1, v));
    if (srgb)
        v = v <= .0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - .055f;
    return (u8)(v * 255 + .5f);
}
static const char *name(u32 f) {
    switch (f) {
    case DE_FMT_BC1_SRGB:
        return "BC1 sRGB";
    case DE_FMT_BC3_SRGB:
        return "BC3 sRGB";
    case DE_FMT_BC4_UNORM:
        return "BC4";
    case DE_FMT_BC5_UNORM:
        return "BC5";
    case DE_FMT_BC6H_UFLOAT:
        return "BC6H (RGB clamped)";
    case DE_FMT_BC7_UNORM:
        return "BC7";
    case DE_FMT_BC7_SRGB:
        return "BC7 sRGB";
    case DE_FMT_RGBA8_UNORM:
        return "RGBA8";
    case DE_FMT_RGBA8_SRGB:
        return "RGBA8 sRGB";
    case DE_FMT_BGRA8_UNORM:
        return "BGRA8";
    case DE_FMT_BGRA8_SRGB:
        return "BGRA8 sRGB";
    case DE_FMT_R8_UNORM:
        return "R8";
    case DE_FMT_RG8_UNORM:
        return "RG8";
    case DE_FMT_R16_FLOAT:
        return "R16F (clamped)";
    case DE_FMT_RG16_FLOAT:
        return "RG16F (clamped)";
    case DE_FMT_RGBA16_FLOAT:
        return "RGBA16F (clamped)";
    case DE_FMT_R32_FLOAT:
        return "R32F (clamped)";
    case DE_FMT_RGBA32_FLOAT:
        return "RGBA32F (clamped)";
    default:
        return NULL;
    }
}
de_result de__ed_settings_pixels(const void *data, u64 bytes, u32 width, u32 height, u8 *rgba,
                                 u32 *source_width, u32 *source_height, const char **format) {
    if (!rgba || !width || !height || width > 256 || height > 256 || !source_width ||
        !source_height || !format)
        return DE_ERR_INVALID_ARGUMENT;
    de_cooked_texture_view view;
    const char *why = NULL;
    if (bytes >= DE_COOKED_TEXTURE_HEADER_SIZE && data &&
        de_cooked_get_u32((const u8 *)data + 36) == DE_COOKED_TEXTURE_USAGE_ENVIRONMENT) {
        de_cooked_env_view env;
        if (!de_cooked_env_parse(data, bytes, &env, &why))
            return DE_ERR_PARSE;
        view = env.texture;
    } else if (!de_cooked_texture_parse(data, bytes, &view, &why))
        return DE_ERR_PARSE;
    const char *label = name(view.format);
    if (!label)
        return DE_ERR_UNSUPPORTED;
    de_cooked_texture_subresource mip;
    if (!de_cooked_texture_subresource_at(&view, 0, &mip))
        return DE_ERR_PARSE;
    const bool linear_color = !de_cooked_texture_format_is_srgb(view.format) &&
                              (view.usage == DE_COOKED_TEXTURE_USAGE_COLOR ||
                               view.usage == DE_COOKED_TEXTURE_USAGE_ENVIRONMENT);
    u32 previous_x = UINT32_MAX, previous_y = UINT32_MAX;
    u8 block[64];
    f32 hdr[48];
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) {
            const u32 sx = (u32)((u64)x * mip.width / width),
                      sy = (u32)((u64)y * mip.height / height);
            u8 *out = rgba + ((size_t)y * width + x) * 4;
            f32 pixel[4] = {0, 0, 0, 1};
            if (view.block.width == 4) {
                u32 bx = sx / 4, by = sy / 4;
                if (bx != previous_x || by != previous_y) {
                    const u8 *src = view.blob + mip.offset + (u64)by * mip.row_pitch +
                                    (u64)bx * view.block.bytes;
                    memset(block, 0, sizeof block);
                    memset(hdr, 0, sizeof hdr);
                    switch (view.format) {
                    case DE_FMT_BC1_SRGB:
                        bcdec_bc1(src, block, 16);
                        break;
                    case DE_FMT_BC3_SRGB:
                        bcdec_bc3(src, block, 16);
                        break;
                    case DE_FMT_BC4_UNORM: {
                        u8 r[16];
                        bcdec_bc4(src, r, 4);
                        for (u32 i = 0; i < 16; ++i) {
                            block[i * 4] = block[i * 4 + 1] = block[i * 4 + 2] = r[i];
                            block[i * 4 + 3] = 255;
                        }
                        break;
                    }
                    case DE_FMT_BC5_UNORM: {
                        u8 rg[32];
                        bcdec_bc5(src, rg, 8);
                        for (u32 i = 0; i < 16; ++i) {
                            block[i * 4] = rg[i * 2];
                            block[i * 4 + 1] = rg[i * 2 + 1];
                            block[i * 4 + 3] = 255;
                        }
                        break;
                    }
                    case DE_FMT_BC6H_UFLOAT:
                        bcdec_bc6h_float(src, hdr, 12, 0);
                        break;
                    case DE_FMT_BC7_UNORM:
                    case DE_FMT_BC7_SRGB:
                        bcdec_bc7(src, block, 16);
                        break;
                    default:
                        return DE_ERR_UNSUPPORTED;
                    }
                    previous_x = bx;
                    previous_y = by;
                }
                const u32 i = (sy % 4) * 4 + sx % 4;
                if (view.format == DE_FMT_BC6H_UFLOAT)
                    memcpy(pixel, hdr + i * 3, 3 * sizeof(f32));
                else
                    for (u32 c = 0; c < 4; ++c)
                        pixel[c] = (f32)block[i * 4 + c] / 255;
            } else {
                const u8 *src =
                    view.blob + mip.offset + (u64)sy * mip.row_pitch + (u64)sx * view.block.bytes;
                switch (view.format) {
                case DE_FMT_RGBA8_UNORM:
                case DE_FMT_RGBA8_SRGB:
                    for (u32 c = 0; c < 4; ++c)
                        pixel[c] = (f32)src[c] / 255;
                    break;
                case DE_FMT_BGRA8_UNORM:
                case DE_FMT_BGRA8_SRGB:
                    pixel[0] = (f32)src[2] / 255;
                    pixel[1] = (f32)src[1] / 255;
                    pixel[2] = (f32)src[0] / 255;
                    pixel[3] = (f32)src[3] / 255;
                    break;
                case DE_FMT_R8_UNORM:
                    pixel[0] = pixel[1] = pixel[2] = (f32)src[0] / 255;
                    break;
                case DE_FMT_RG8_UNORM:
                    pixel[0] = (f32)src[0] / 255;
                    pixel[1] = (f32)src[1] / 255;
                    break;
                case DE_FMT_R16_FLOAT:
                case DE_FMT_RG16_FLOAT:
                case DE_FMT_RGBA16_FLOAT: {
                    u32 count = view.format == DE_FMT_R16_FLOAT    ? 1
                                : view.format == DE_FMT_RG16_FLOAT ? 2
                                                                   : 4;
                    for (u32 c = 0; c < count; ++c)
                        pixel[c] = half((u16)(src[c * 2] | (u16)src[c * 2 + 1] << 8));
                    if (count == 1)
                        pixel[1] = pixel[2] = pixel[0];
                    break;
                }
                case DE_FMT_R32_FLOAT:
                case DE_FMT_RGBA32_FLOAT: {
                    u32 count = view.format == DE_FMT_R32_FLOAT ? 1 : 4;
                    memcpy(pixel, src, count * sizeof(f32));
                    if (count == 1)
                        pixel[1] = pixel[2] = pixel[0];
                    break;
                }
                default:
                    return DE_ERR_UNSUPPORTED;
                }
            }
            for (u32 c = 0; c < 3; ++c)
                out[c] = channel(pixel[c], linear_color);
            out[3] = channel(pixel[3], false);
        }
    *source_width = view.width;
    *source_height = view.height;
    *format = label;
    return DE_OK;
}
