#include "audio/backend/wasapi_format.h"
#include <string.h>
u32 de__wasapi_mask(u32 channels) {
    switch (channels) {
    case 1:
        return 0x4; /* front center */
    case 2:
        return 0x3; /* front left/right */
    case 6:
        return 0x3f; /* L R C LFE BL BR */
    case 8:
        return 0x63f; /* L R C LFE BL BR SL SR */
    default:
        return 0;
    }
}
void de__wasapi_candidates(u32 rate, u32 channels, de__wasapi_format out[3]) {
    for (u32 i = 0; i < 3; ++i)
        out[i] =
            (de__wasapi_format){rate, channels, de__wasapi_mask(channels), (de__wasapi_sample)i};
}
bool de__wasapi_utf8(const u16 *wide, char *out, u32 bytes) {
    if (!wide || !out || !bytes)
        return false;
    u32 at = 0;
    for (u32 i = 0; wide[i]; ++i) {
        u32 cp = wide[i];
        if (cp >= 0xd800 && cp <= 0xdbff) {
            u32 low = wide[++i];
            if (low < 0xdc00 || low > 0xdfff) {
                out[0] = 0;
                return false;
            }
            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            out[0] = 0;
            return false;
        }
        u32 count = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (count >= bytes - at) {
            out[0] = 0;
            return false;
        }
        if (count == 1)
            out[at++] = (char)cp;
        else {
            out[at++] = (char)((count == 2   ? 0xc0
                                : count == 3 ? 0xe0
                                             : 0xf0) |
                               (cp >> ((count - 1) * 6)));
            for (u32 n = count - 1; n > 0; --n)
                out[at++] = (char)(0x80 | ((cp >> ((n - 1) * 6)) & 63));
        }
    }
    out[at] = 0;
    return true;
}
void de__wasapi_convert(const f32 *in, void *out, u32 count, de__wasapi_sample sample) {
    if (sample == DE__WASAPI_F32) {
        memcpy(out, in, (size_t)count * sizeof(f32));
        return;
    }
    u8 *bytes = out;
    u32 width = sample == DE__WASAPI_I24 ? 3 : 2;
    for (u32 i = 0; i < count; ++i) {
        f32 value = in[i];
        if (!(value == value))
            value = 0;
        if (value < -1)
            value = -1;
        if (value > 1)
            value = 1;
        i32 scale = width == 3 ? 8388608 : 32768;
        i32 integer = value <= -1 ? -scale : value >= 1 ? scale - 1 : (i32)(value * scale);
        for (u32 n = 0; n < width; ++n)
            bytes[i * width + n] = (u8)((u32)integer >> (8 * n));
    }
}
