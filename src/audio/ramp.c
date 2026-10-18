#include "audio/audio_internal.h"
void de__audio_ramp_set(de__audio_ramp *r, f32 target, u32 samples) {
    r->target = target;
    r->remaining = samples;
    r->delta = samples ? (target - r->value) / samples : 0;
    if (!samples)
        r->value = target;
}
f32 de__audio_ramp_next(de__audio_ramp *r) {
    f32 value = r->value;
    if (r->remaining) {
        r->value += r->delta;
        if (--r->remaining == 0)
            r->value = r->target;
    }
    return value;
}
