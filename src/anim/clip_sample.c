#include "anim_internal.h"

#include <dengine/de_math.h>

/* Finite: not +-inf or NaN (no <math.h> in a deterministic module, SPEC §5.7). */
static bool finite(f64 v) {
    return v - v == 0;
}

f32 de__anim_time(f32 time, f32 duration, f32 speed, f32 dt, bool loop, bool *ended) {
    *ended = false;
    if (!(duration > 0) || !finite(duration))
        return 0;
    f64 next = (finite(time) ? time : 0) + (f64)(finite(speed) ? speed : 0) * (finite(dt) ? dt : 0);
    if (loop) {
        next = de_dfmod((f32)next, duration);
        if (next < 0)
            next += duration;
    } else {
        if (next >= duration) {
            next = duration;
            *ended = speed > 0 && dt > 0;
        }
        if (next <= 0) {
            next = 0;
            *ended = speed < 0 && dt > 0;
        }
    }
    return (f32)next;
}
bool de__anim_sample(const de_cooked_clip_view *clip, const de_cooked_skeleton_view *skeleton,
                     f32 time, de_cooked_joint_pose *out) {
    return de__anim_clip_sample(clip, skeleton, time, out);
}
