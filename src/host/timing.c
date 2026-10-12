/*
 * timing.c: the main loop's timing core (SPEC §10.2). See timing.h.
 *
 * Fixed mode works in integer units of ns × fixed_hz. A step is DE__TIMING_UNIT (10^9) units at
 * every rate, so `acc += scaled_ns * fixed_hz; steps = acc / UNIT; acc %= UNIT` is exact. Overflow
 * bound: scaled_ns <= DE__TIMING_MAX_FRAME_NS * DE__TIMING_MAX_TIME_SCALE = 6e13, times
 * DE__TIMING_MAX_FIXED_HZ = 6e17, plus acc < 1e9, below 2^64 (1.8e19).
 */
#include "host/timing.h"

#include <string.h>

/* The largest f32 below 1: alpha must stay in [0, 1) even when (f32)(acc / UNIT) rounds up. */
#define TIMING_ALPHA_MAX 0.99999994f /* 1 - 2^-24 */

void de__timing_normalize(de_timing *t) {
    if (t->mode != DE_TIMING_FIXED && t->mode != DE_TIMING_VARIABLE)
        t->mode = DE_TIMING_FIXED;
    if (t->fixed_hz == 0)
        t->fixed_hz = DE_TIMING_DEFAULT_FIXED_HZ;
    if (t->fixed_hz > DE__TIMING_MAX_FIXED_HZ)
        t->fixed_hz = DE__TIMING_MAX_FIXED_HZ;
    if (t->max_catchup_steps == 0)
        t->max_catchup_steps = DE_TIMING_DEFAULT_MAX_CATCHUP_STEPS;
    if (!(t->time_scale >= 0.0f)) /* negative or NaN */
        t->time_scale = DE_TIMING_DEFAULT_TIME_SCALE;
    if (t->time_scale > DE__TIMING_MAX_TIME_SCALE)
        t->time_scale = DE__TIMING_MAX_TIME_SCALE;
}

static void timing_reset(de__timing_state *s) {
    s->acc = 0;
    s->scale_frac = 0.0;
    s->alpha = s->timing.mode == DE_TIMING_VARIABLE ? 1.0f : 0.0f;
}

void de__timing_init(de__timing_state *s, const de_timing *t) {
    memset(s, 0, sizeof *s);
    if (t != NULL) {
        s->timing = *t;
    } else {
        s->timing.mode = DE_TIMING_FIXED;
        s->timing.fixed_hz = DE_TIMING_DEFAULT_FIXED_HZ;
        s->timing.max_catchup_steps = DE_TIMING_DEFAULT_MAX_CATCHUP_STEPS;
        s->timing.time_scale = DE_TIMING_DEFAULT_TIME_SCALE;
        s->timing.frame_cap_hz = DE_TIMING_DEFAULT_FRAME_CAP_HZ;
    }
    de__timing_normalize(&s->timing);
    timing_reset(s);
}

bool de__timing_set(de__timing_state *s, const de_timing *t) {
    de_timing n = *t;
    de__timing_normalize(&n);
    const de_timing old = s->timing;
    s->timing = n;
    if (n.mode != old.mode) {
        timing_reset(s);
        return true;
    }
    if (n.mode == DE_TIMING_FIXED && n.fixed_hz != old.fixed_hz) {
        /* acc / old_hz ns are pending; keep them: acc' = acc * new_hz / old_hz. acc < 1e9 and
         * new_hz <= 1e4, so the product fits. A rate rise can leave more than one step pending;
         * the next advance runs them (within the catch-up limit). */
        s->acc = s->acc * (u64)n.fixed_hz / (u64)old.fixed_hz;
    }
    return false;
}

u64 de__timing_step_ns(const de__timing_state *s) {
    const u64 hz = s->timing.fixed_hz;
    return (DE__TIMING_UNIT + hz - 1) / hz;
}

/* real_dt_ns × time_scale in whole ns. The sub-ns part is carried in s->scale_frac, so a scale
 * like 0.3 loses nothing over many frames. At scale 1 the delta passes through unchanged. */
static u64 timing_scale(de__timing_state *s, u64 real_dt_ns) {
    const f32 scale = s->timing.time_scale;
    if (scale == 1.0f)
        return real_dt_ns;
    /* At most 6e13 + 1 here: exact in f64 to well below a nanosecond. */
    const f64 x = (f64)real_dt_ns * (f64)scale + s->scale_frac;
    const u64 whole = (u64)x;
    s->scale_frac = x - (f64)whole;
    return whole;
}

void de__timing_advance(de__timing_state *s, u64 real_dt_ns, de__timing_frame *out) {
    memset(out, 0, sizeof *out);
    if (real_dt_ns > DE__TIMING_MAX_FRAME_NS)
        real_dt_ns = DE__TIMING_MAX_FRAME_NS;

    if (s->timing.time_scale == 0.0f) {
        /* Paused: the simulation does not move, and rendering keeps showing the same blend. */
        out->paused = true;
        out->alpha = s->alpha;
        return;
    }

    const u64 scaled = timing_scale(s, real_dt_ns);
    const u64 hz = s->timing.fixed_hz;
    const u32 max_steps = s->timing.max_catchup_steps;
    out->scaled_ns = scaled;
    out->frame_dt = (f32)((f64)scaled * 1e-9);

    if (s->timing.mode == DE_TIMING_VARIABLE) {
        /* The same catch-up limit as fixed mode: the simulation never advances by more than
         * max_catchup_steps fixed steps in one frame (a hitch would otherwise hand physics a
         * huge dt). */
        const u64 max_ns = (u64)max_steps * DE__TIMING_UNIT / hz;
        u64 step_ns = scaled;
        if (step_ns > max_ns) {
            out->dropped_ns = step_ns - max_ns;
            step_ns = max_ns;
        }
        out->steps = 1;
        out->step_dt = (f32)((f64)step_ns * 1e-9);
        s->alpha = 1.0f;
        out->alpha = 1.0f;
    } else {
        s->acc += scaled * hz;
        u64 steps = s->acc / DE__TIMING_UNIT;
        s->acc -= steps * DE__TIMING_UNIT;
        if (steps > max_steps) {
            /* Drop whole steps only: the leftover fraction stays, so alpha does not jump and the
             * next normal frame runs 0 or 1 steps. */
            out->dropped_ns = (steps - max_steps) * DE__TIMING_UNIT / hz;
            steps = max_steps;
        }
        out->steps = (u32)steps;
        out->step_dt = steps != 0 ? 1.0f / (f32)hz : 0.0f;
        f32 alpha = (f32)((f64)s->acc / (f64)DE__TIMING_UNIT);
        if (alpha > TIMING_ALPHA_MAX)
            alpha = TIMING_ALPHA_MAX;
        s->alpha = alpha;
        out->alpha = alpha;
    }
    s->steps_total += out->steps;
    s->dropped_ns_total += out->dropped_ns;
}
