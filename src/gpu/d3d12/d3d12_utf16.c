/* d3d12_utf16.c — strict UTF-8 to UTF-16 for D3D12 names (DENG-752). Portable: no Windows
 * headers, so the unit test runs on every host. */
#include "gpu/d3d12/d3d12_internal.h"

#include <dengine/de_memory.h>

#include <limits.h>

/* Decodes one code point at s (up to end). Returns the byte length, or 0 for a malformed
 * sequence: a bad lead byte, a missing or bad continuation, an overlong form, a surrogate, or a
 * value above U+10FFFF. */
static size_t decode(const u8 *s, const u8 *end, u32 *cp) {
    u8 b = s[0];
    size_t n;
    u32 v, min;
    if (b < 0x80) {
        *cp = b;
        return 1;
    } else if (b >= 0xC2 && b <= 0xDF) {
        n = 2, v = b & 0x1Fu, min = 0x80;
    } else if (b >= 0xE0 && b <= 0xEF) {
        n = 3, v = b & 0x0Fu, min = 0x800;
    } else if (b >= 0xF0 && b <= 0xF4) {
        n = 4, v = b & 0x07u, min = 0x10000;
    } else {
        return 0;
    }
    if ((size_t)(end - s) < n)
        return 0;
    for (size_t i = 1; i < n; ++i) {
        if ((s[i] & 0xC0u) != 0x80u)
            return 0;
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    if (v < min || v > 0x10FFFFu || (v >= 0xD800u && v <= 0xDFFFu))
        return 0;
    *cp = v;
    return n;
}

de_result de__d3d12_utf8_to_utf16(const de_allocator *a, const char *utf8, u16 **out,
                                  size_t *out_units) {
    if (!a || !utf8 || !out || !out_units)
        return DE_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_units = 0;
    const u8 *s = (const u8 *)utf8;
    size_t len = 0;
    while (s[len])
        ++len;
    const u8 *end = s + len;
    /* Pass 1: validate and count. */
    size_t units = 0;
    for (const u8 *p = s; p < end;) {
        u32 cp;
        size_t n = decode(p, end, &cp);
        if (!n)
            return DE_ERR_INVALID_ARGUMENT;
        p += n;
        units += cp >= 0x10000u ? 2 : 1;
    }
    units += 1; /* the terminator */
    if (units > SIZE_MAX / sizeof(u16))
        return DE_ERR_OUT_OF_MEMORY;
    u16 *buf = DE_ALLOC(a, units * sizeof(u16), DE_ALIGNOF(u16), DE_MEM_RENDER);
    if (!buf)
        return DE_ERR_OUT_OF_MEMORY;
    /* Pass 2: write. */
    size_t w = 0;
    for (const u8 *p = s; p < end;) {
        u32 cp;
        p += decode(p, end, &cp);
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            buf[w++] = (u16)(0xD800u | (cp >> 10));
            buf[w++] = (u16)(0xDC00u | (cp & 0x3FFu));
        } else {
            buf[w++] = (u16)cp;
        }
    }
    buf[w] = 0;
    *out = buf;
    *out_units = units;
    return DE_OK;
}

de_result de__d3d12_rt_entry_to_utf16(const de_allocator *a, const char *entry, u16 **out,
                                      size_t *out_units) {
    if (!entry)
        entry = "main";
    else if (!entry[0]) {
        if (out)
            *out = NULL;
        if (out_units)
            *out_units = 0;
        return DE_ERR_INVALID_ARGUMENT; /* an empty export name is not a name */
    }
    return de__d3d12_utf8_to_utf16(a, entry, out, out_units);
}
