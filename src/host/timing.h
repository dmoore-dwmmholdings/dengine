/*
 * timing.h: the main loop's timing core (internal to src/host/, not installed). SPEC §10.2.
 *
 * Include as "host/timing.h". Owner: DENG-91.
 *
 * Pure arithmetic: no clock, no log, no allocation. The loop (loop.c) measures the real frame
 * delta and asks de__timing_advance() how many FixedUpdate steps to run, with which dt, and the
 * interpolation alpha. Tests drive it with a fake clock.
 *
 * Fixed mode keeps the accumulator in integer units of `ns × fixed_hz`: one fixed step is
 * exactly DE__TIMING_UNIT units for every rate, so no rounding ever accumulates. 1 s of real time
 * at 60 Hz is exactly 60 steps, after any split of that second into frames, and 10^6 s is exactly
 * 6 * 10^7 steps. Only the outputs `dt` and `alpha` are floats.
 */
#ifndef DE_HOST_TIMING_H
#define DE_HOST_TIMING_H

#include <dengine/de_game.h>

DE_EXTERN_C_BEGIN

/** Accumulator units in one fixed step (and in one nanosecond times fixed_hz): 10^9. */
#define DE__TIMING_UNIT 1000000000ull

/** Real frame deltas above this (60 s) are cut to it before they are scaled. A debugger pause or
 * a sleeping laptop then cannot overflow the accumulator; the catch-up clamp drops it anyway. */
#define DE__TIMING_MAX_FRAME_NS (60ull * 1000000000ull)

/** Largest fixed_hz the core accepts (game.toml's `timing.fixed_hz` range); higher is clamped. */
#define DE__TIMING_MAX_FIXED_HZ 10000u

/** Largest time scale the core accepts (game.toml's `timing.time_scale` range). */
#define DE__TIMING_MAX_TIME_SCALE 1000.0f

/** The timing state of one loop. Plain data; zero it, then de__timing_init(). */
typedef struct de__timing_state {
    /** The timing in use (normalised by de__timing_normalize). */
    de_timing timing;
    /** Fixed mode: scaled time not yet simulated, in ns × fixed_hz. Below DE__TIMING_UNIT after
     * every advance. 0 in variable mode. */
    u64 acc;
    /** The sub-nanosecond part of the last scaled delta (time_scale != 1), carried to the next
     * frame so scaling does not drift. In [0, 1). */
    f64 scale_frac;
    /** The last interpolation alpha. Frozen while paused. */
    f32 alpha;
    /** FixedUpdate steps since init. */
    u64 steps_total;
    /** Scaled nanoseconds the catch-up clamp dropped since init. */
    u64 dropped_ns_total;
} de__timing_state;

/** What one frame does (de__timing_advance). */
typedef struct de__timing_frame {
    /** FixedUpdate runs this frame: 0..max_catchup_steps (fixed), 1 (variable), 0 (paused). */
    u32 steps;
    /** `dt` of each FixedUpdate run, in seconds. Fixed: `1.0f / (f32)fixed_hz`, the same every
     * step (SPEC §5.7). Variable: the scaled frame delta (cut to max_catchup_steps fixed steps).
     * 0 when steps is 0. */
    f32 step_dt;
    /** `dt` of the other schedules (First, PreUpdate, Update, PostUpdate, Extract, Last): the
     * scaled frame delta in seconds. 0 while paused. */
    f32 frame_dt;
    /** Interpolation alpha for the schedules after FixedUpdate. Fixed: the leftover fraction of
     * a step, in [0, 1). Variable: 1 (the state is current; nothing to interpolate). Paused: the
     * last alpha, unchanged. */
    f32 alpha;
    /** The scaled frame delta in nanoseconds (before the catch-up clamp). */
    u64 scaled_ns;
    /** Scaled nanoseconds the catch-up clamp dropped this frame (0 normally). */
    u64 dropped_ns;
    /** time_scale is 0: no FixedUpdate, alpha frozen. */
    bool paused;
} de__timing_frame;

/**
 * Normalises `*t` in place: fixed_hz 0 → DE_TIMING_DEFAULT_FIXED_HZ, above
 * DE__TIMING_MAX_FIXED_HZ → that; max_catchup_steps 0 → DE_TIMING_DEFAULT_MAX_CATCHUP_STEPS;
 * a negative or NaN time_scale → 1.0, above DE__TIMING_MAX_TIME_SCALE → that; an unknown mode →
 * DE_TIMING_FIXED. frame_cap_hz is left alone (0 is uncapped). A time scale of 0 stays 0 (pause).
 */
void de__timing_normalize(de_timing *t);

/** Starts `s` with timing `t` (NULL: the defaults): empty accumulator, alpha 0 (fixed) or 1
 * (variable). */
void de__timing_init(de__timing_state *s, const de_timing *t);

/**
 * Switches `s` to timing `t` (normalised first). A mode change resets the accumulator, the scale
 * fraction and alpha (as de__timing_init). A fixed_hz change in fixed mode rescales the
 * accumulator, so the time already accumulated is kept. Other fields apply as they are.
 * @return true when the accumulator was reset (the mode changed).
 */
bool de__timing_set(de__timing_state *s, const de_timing *t);

/**
 * One frame: `real_dt_ns` of real time passed since the last frame. Scales it by time_scale and
 * fills `*out` (see de__timing_frame).
 *
 * Fixed: adds the scaled time to the accumulator and runs `floor(acc / step)` steps. More than
 * max_catchup_steps are cut to max_catchup_steps and the excess whole steps are dropped
 * (out->dropped_ns); the leftover part of a step is kept, so alpha stays continuous and the next
 * normal frame runs 0 or 1 steps. Variable: one step with the scaled delta, cut to
 * max_catchup_steps fixed steps (the rest is dropped). Paused (time_scale 0): no step, alpha
 * frozen, nothing accumulates.
 */
void de__timing_advance(de__timing_state *s, u64 real_dt_ns, de__timing_frame *out);

/** The length of one fixed step in whole nanoseconds, rounded up: the smallest real delta that
 * runs one step at time scale 1 from an empty accumulator. */
u64 de__timing_step_ns(const de__timing_state *s);

DE_EXTERN_C_END
#endif /* DE_HOST_TIMING_H */
