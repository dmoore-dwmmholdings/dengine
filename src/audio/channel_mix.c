#include "audio/audio_internal.h"
/* SMPTE/WAV order: L,R,C,LFE,SL,SR,BL,BR. LFE is excluded from downmix;
 * center/surround use -3dB. Stereo upmix routes fronts, center=(L+R)/sqrt(2),
 * surrounds at -3dB. Mono is equal-power center for stereo, stereo->mono average. */
void de__audio_channel_mix(f32 *out, u32 och, const f32 *in, u32 ich, f32 gain) {
    f32 l = in[0], r = ich > 1 ? in[1] : in[0];
    if (ich == 1) {
        l *= 0.70710678118f;
        r = l;
    }
    if (ich == 4) {
        l += 0.70710678118f * in[2];
        r += 0.70710678118f * in[3];
    }
    if (ich == 3 || ich >= 5) {
        l += 0.70710678118f * in[2];
        r += 0.70710678118f * in[2];
    }
    if (ich == 5) {
        l += 0.70710678118f * in[3];
        r += 0.70710678118f * in[4];
    }
    if (ich == 7) {
        l += 0.5f * in[4] + 0.70710678118f * in[5];
        r += 0.5f * in[4] + 0.70710678118f * in[6];
    }
    if (ich == 6 || ich == 8) {
        l += 0.70710678118f * in[4];
        r += 0.70710678118f * in[5];
    }
    if (ich >= 8) {
        l += 0.70710678118f * in[6];
        r += 0.70710678118f * in[7];
    }
    if (och == 1) {
        out[0] += (ich == 1 ? in[0] : (l + r) * 0.5f) * gain;
        return;
    }
    out[0] += l * gain;
    out[1] += r * gain;
    if (och >= 3)
        out[2] += (l + r) * 0.70710678118f * gain;
    if (och >= 6) {
        out[4] += l * 0.70710678118f * gain;
        out[5] += r * 0.70710678118f * gain;
    }
    if (och >= 8) {
        out[6] += l * 0.5f * gain;
        out[7] += r * 0.5f * gain;
    }
}
