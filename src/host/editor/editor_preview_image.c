#include "editor_preview_image.h"
#include <math.h>
#include <string.h>

static void be32(u8 *out, u32 value) {
    for (u32 i = 0; i < 4; ++i)
        out[i] = (u8)(value >> (24u - i * 8u));
}
static u32 crc32(const u8 *bytes, size_t size) {
    u32 crc = ~0u;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (u32 bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static u8 *chunk(u8 *out, const char type[4], u32 size) {
    be32(out, size);
    memcpy(out + 4, type, 4);
    return out + 8;
}
size_t de__ed_preview_png_size(u32 width, u32 height) {
    if (!width || !height || width > 1024 || height > 1024)
        return 0;
    const size_t raw = ((size_t)width * 4u + 1u) * height;
    return 63u + raw + 5u * ((raw + 65534u) / 65535u);
}
de_result de__ed_preview_png(u32 width, u32 height, const u8 *rgba, void *png, size_t capacity,
                             size_t *written) {
    const size_t required = de__ed_preview_png_size(width, height);
    if (!required || !rgba || !png || !written || capacity < required)
        return DE_ERR_INVALID_ARGUMENT;
    u8 *out = png;
    static const u8 signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    memcpy(out, signature, sizeof signature);
    out = chunk(out + 8, "IHDR", 13);
    be32(out, width);
    be32(out + 4, height);
    memcpy(out + 8, (u8[]){8, 6, 0, 0, 0}, 5);
    be32(out + 13, crc32(out - 4, 17));
    const size_t row = (size_t)width * 4u + 1u, raw = row * height;
    const u32 zsize = (u32)(required - 57u);
    u8 *data = chunk(out + 17, "IDAT", zsize);
    out = data;
    *out++ = 0x78; /* zlib, stored DEFLATE blocks; memory and output size are predictable. */
    *out++ = 0x01;
    u32 adler_a = 1, adler_b = 0;
    size_t cursor = 0;
    while (cursor < raw) {
        const u32 count = (u32)(raw - cursor < 65535u ? raw - cursor : 65535u);
        *out++ = cursor + count == raw ? 1 : 0;
        *out++ = (u8)count;
        *out++ = (u8)(count >> 8);
        *out++ = (u8)~count;
        *out++ = (u8)(~count >> 8);
        for (u32 i = 0; i < count; ++i, ++cursor) {
            const size_t column = cursor % row;
            const u8 value = column ? rgba[(cursor / row) * (row - 1u) + column - 1u] : 0;
            *out++ = value;
            adler_a += value;
            if (adler_a >= 65521u)
                adler_a -= 65521u;
            adler_b += adler_a;
            if (adler_b >= 65521u)
                adler_b -= 65521u;
        }
    }
    be32(out, (adler_b << 16) | adler_a);
    out += 4;
    be32(out, crc32(data - 4, (size_t)zsize + 4u));
    out = chunk(out + 4, "IEND", 0);
    be32(out, crc32(out - 4, 4));
    *written = (size_t)(out + 4 - (u8 *)png);
    return DE_OK;
}
void de__ed_preview_rgba(u32 width, u32 height, const void *half, u32 pitch, u8 *rgba) {
    for (u32 y = 0; y < height; ++y) {
        const u16 *row = (const u16 *)((const u8 *)half + (size_t)y * pitch);
        for (u32 x = 0; x < width; ++x) {
            for (u32 c = 0; c < 3; ++c) {
                const u16 h = row[x * 4u + c];
                const u32 exponent = (h >> 10) & 31u, mantissa = h & 1023u;
                f32 value = exponent == 0    ? (f32)mantissa / 16777216.0f
                            : exponent == 31 ? (mantissa ? 0.0f : 65504.0f)
                                             : (1.0f + (f32)mantissa / 1024.0f) *
                                                   ldexpf(1.0f, (i32)exponent - 15);
                if (h & 0x8000u)
                    value = 0;
                value = value >= 1           ? 1
                        : value <= .0031308f ? value * 12.92f
                                             : 1.055f * powf(value, 1.0f / 2.4f) - .055f;
                rgba[((size_t)y * width + x) * 4u + c] = (u8)(value * 255.0f + .5f);
            }
            rgba[((size_t)y * width + x) * 4u + 3u] = 255;
        }
    }
}
