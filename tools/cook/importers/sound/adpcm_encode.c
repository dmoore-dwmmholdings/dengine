#include "cook/importers/sound/sound_import.h"
i16 de__sound_i16(f32 v) {
    if (v <= -1)
        return -32768;
    if (v >= 1)
        return 32767;
    return (i16)(v * 32768);
}
void de__sound_adpcm(const f32 *samples, u64 frames, u32 channels, u8 *out) {
    u64 blocks = (frames + 1016) / 1017;
    for (u64 block = 0; block < blocks; ++block)
        for (u32 c = 0; c < channels; ++c) {
            u8 *p = out + (block * channels + c) * 512;
            memset(p, 0, 512);
            i32 predictor = de__sound_i16(samples[(block * 1017) * channels + c]), index = 0;
            p[0] = (u8)predictor;
            p[1] = (u8)((u16)predictor >> 8);
            for (u32 n = 1; n < 1017; ++n) {
                u64 frame = block * 1017 + n;
                i32 target =
                    frame < frames ? de__sound_i16(samples[frame * channels + c]) : predictor;
                i32 diff = target - predictor;
                u32 code = diff < 0 ? 8u : 0u;
                if (diff < 0)
                    diff = -diff;
                i32 step = de__ima_step[index];
                if (diff >= step) {
                    code |= 4;
                    diff -= step;
                }
                if (diff >= step / 2) {
                    code |= 2;
                    diff -= step / 2;
                }
                if (diff >= step / 4)
                    code |= 1;
                (void)de__ima_decode(code, &predictor, &index);
                u32 at = n - 1;
                p[4 + at / 2] |= (u8)(code << ((at & 1) * 4));
            }
        }
}
