#include "anim_internal.h"
#include <string.h>
#include <float.h>

static bool finite32(f32 x) {
    u32 bits;
    memcpy(&bits, &x, sizeof bits);
    return (bits & 0x7f800000u) != 0x7f800000u;
}
/* Exact binary long division on IEEE significands. This avoids narrowing a long
 * tick clock to f32 before wrapping, or calling the platform libm fmod. */
f64 de__anim_wrap_time(f64 value, f64 period) {
    u64 x, y;
    memcpy(&x, &value, sizeof x);
    memcpy(&y, &period, sizeof y);
    if (!y || y >= UINT64_C(0x7ff0000000000000))
        return 0;
    bool negative = (x >> 63) != 0;
    x &= UINT64_C(0x7fffffffffffffff);
    if (x >= UINT64_C(0x7ff0000000000000))
        return 0;
    if (x >= y) {
        i32 ex = (i32)(x >> 52), ey = (i32)(y >> 52);
        x &= UINT64_C(0xfffffffffffff);
        y &= UINT64_C(0xfffffffffffff);
        if (ex)
            x |= UINT64_C(1) << 52;
        else {
            ex = 1;
            while (!(x & (UINT64_C(1) << 52))) {
                x <<= 1;
                --ex;
            }
        }
        if (ey)
            y |= UINT64_C(1) << 52;
        else {
            ey = 1;
            while (!(y & (UINT64_C(1) << 52))) {
                y <<= 1;
                --ey;
            }
        }
        while (ex > ey) {
            if (x >= y)
                x -= y;
            x <<= 1;
            --ex;
        }
        if (x >= y)
            x -= y;
        if (!x)
            return 0;
        while (!(x & (UINT64_C(1) << 52))) {
            x <<= 1;
            --ex;
        }
        if (ex > 0)
            x = (x & UINT64_C(0xfffffffffffff)) | (u64)ex << 52;
        else
            x >>= (u32)(1 - ex);
    }
    f64 result;
    memcpy(&result, &x, sizeof result);
    if (negative && result != 0)
        result = period - result;
    return result < period ? result : 0;
}

void de__anim_clock_step(de__anim_clock *c, DeAnimator *a, f32 dt, f32 duration, u64 key, bool seek,
                         bool *ended) {
    *ended = false;
    f32 speed = finite32(a->speed) ? a->speed : 0;
    if (!finite32(dt) || dt < 0)
        dt = 0;
    if (!(duration > 0) || !finite32(duration)) {
        a->time = 0;
        return;
    }
    if (!c->initialized || c->clip_key != key || c->duration != duration || seek) {
        c->phase = finite32(a->time) ? (f64)a->time : 0;
        c->exact_time = c->phase;
        c->segment_ticks = 0;
    } else if (c->dt != dt || c->speed != speed || c->segment_ticks == UINT64_MAX) {
        c->phase = c->exact_time;
        c->segment_ticks = 0;
    }
    c->initialized = true;
    c->clip_key = key;
    c->duration = duration;
    c->dt = dt;
    c->speed = speed;
    ++c->ticks;
    if (a->playing && dt > 0)
        ++c->segment_ticks;
    f64 time = c->phase + (f64)c->segment_ticks * (f64)dt * (f64)speed;
    if (a->loop)
        time = de__anim_wrap_time(time, duration);
    else if (time >= duration) {
        time = duration;
        *ended = a->playing && speed > 0 && dt > 0;
    } else if (time <= 0) {
        time = 0;
        *ended = a->playing && speed < 0 && dt > 0;
    }
    if (*ended) {
        a->playing = false;
        c->phase = time;
        c->segment_ticks = 0;
    }
    c->playing = a->playing;
    c->exact_time = time;
    c->output_time = a->time = (f32)time;
}

/* Graph time is unbounded; each leaf applies its own speed/duration/loop policy.
 * The negative duration tag distinguishes graph clocks from skeletal clip clocks. */
void de__anim_graph_clock_step(de__anim_clock *c, DeAnimator *a, f32 dt, u64 key, bool seek) {
    f32 speed = finite32(a->speed) ? a->speed : 0;
    if (!finite32(dt) || dt < 0)
        dt = 0;
    if (!c->initialized || c->clip_key != key || c->duration != -1 || seek) {
        c->phase = finite32(a->time) ? (f64)a->time : 0;
        c->exact_time = c->phase;
        c->segment_ticks = 0;
    } else if (c->dt != dt || c->speed != speed || c->segment_ticks == UINT64_MAX) {
        c->phase = c->exact_time;
        c->segment_ticks = 0;
    }
    c->initialized = true;
    c->clip_key = key;
    c->duration = -1;
    c->dt = dt;
    c->speed = speed;
    ++c->ticks;
    if (a->playing && dt > 0)
        ++c->segment_ticks;
    c->exact_time = c->phase + (f64)c->segment_ticks * (f64)dt * (f64)speed;
    c->playing = a->playing;
    /* Preserve a finite public f32 value; leaf sampling uses exact_time instead. */
    f64 time = c->exact_time;
    c->output_time = a->time = time > FLT_MAX ? FLT_MAX : time < -FLT_MAX ? -FLT_MAX : (f32)time;
}
