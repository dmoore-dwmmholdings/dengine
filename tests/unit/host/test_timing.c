/*
 * test_timing.c: the main loop's timing core (src/host/timing.c, DENG-91, SPEC §10.2).
 *
 * No clock: every test feeds de__timing_advance() frame deltas it chose itself.
 */
#include "de_test.h"

#include "host/timing.h"

#include <string.h>

#define MS 1000000ull
#define S  1000000000ull

static de_timing timing(de_timing_mode mode, u32 hz, u32 catchup, f32 scale) {
    de_timing t;
    memset(&t, 0, sizeof t);
    t.mode = mode;
    t.fixed_hz = hz;
    t.max_catchup_steps = catchup;
    t.time_scale = scale;
    return t;
}

/* xorshift64: deterministic "arbitrary" frame deltas. */
static u64 rng_next(u64 *s) {
    u64 x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

/* Feeds `total_ns` in random deltas of 0..max_dt_ns (the last one cut to fit). Returns the steps;
 * checks every alpha is in [0, 1) and every step dt is 1/hz. */
static u64 feed_random(de__timing_state *s, u64 total_ns, u64 max_dt_ns, u64 seed, u32 *frames) {
    u64 left = total_ns, steps = 0;
    u32 n = 0;
    const f32 fixed_dt = 1.0f / (f32)s->timing.fixed_hz;
    while (left > 0) {
        u64 dt = rng_next(&seed) % (max_dt_ns + 1);
        if (dt > left)
            dt = left;
        de__timing_frame f;
        de__timing_advance(s, dt, &f);
        steps += f.steps;
        ++n;
        EXPECT_TRUE(f.alpha >= 0.0f);
        EXPECT_TRUE(f.alpha < 1.0f);
        EXPECT_EQ(f.dropped_ns, 0u);
        if (f.steps != 0)
            EXPECT_TRUE(f.step_dt == fixed_dt); /* the same f32 every step (SPEC §5.7) */
        left -= dt;
    }
    if (frames != NULL)
        *frames = n;
    return steps;
}

TEST(timing, fixed_exact_steps) {
    /* 1 s at 60 Hz, split into arbitrary frames (0..40 ms; never more than 3 steps a frame, so
     * the catch-up limit of 4 does not apply): exactly 60 steps. */
    for (u64 seed = 1; seed <= 20; ++seed) {
        de__timing_state s;
        de_timing t = timing(DE_TIMING_FIXED, 60, 4, 1.0f);
        de__timing_init(&s, &t);
        u32 frames = 0;
        EXPECT_EQ(feed_random(&s, S, 40 * MS, seed * 0x9E3779B97F4A7C15ull, &frames), 60u);
        EXPECT_GT(frames, 40u);
        EXPECT_EQ(s.acc, 0u); /* 1 s is a whole number of 60 Hz steps */
        EXPECT_EQ(s.steps_total, 60u);
    }

    /* Other rates, 10 s in 1..7 ms frames: exactly 10 * hz steps (7 Hz and 144 Hz have no whole
     * nanosecond step, so a nanosecond accumulator would drift). */
    static const u32 rates[] = {1, 7, 30, 60, 120, 144, 240, 1000, 10000};
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; ++i) {
        de__timing_state s;
        de_timing t = timing(DE_TIMING_FIXED, rates[i], 1000, 1.0f);
        de__timing_init(&s, &t);
        EXPECT_EQ(feed_random(&s, 10 * S, 7 * MS, 77 + i, NULL), 10u * rates[i]);
    }

    /* No drift over a long run: 100000 frames of 1/60 s rounded to 16666667 ns (each a little
     * long) is 1666.6666700 s, so floor(that * 60) = 100000 steps; 1666.6666 s of 16666666 ns
     * frames (each a little short) is 99999 steps. */
    de__timing_state s;
    de_timing t = timing(DE_TIMING_FIXED, 60, 4, 1.0f);
    de__timing_init(&s, &t);
    de__timing_frame f;
    for (int i = 0; i < 100000; ++i)
        de__timing_advance(&s, 16666667, &f);
    EXPECT_EQ(s.steps_total, 100000u);
    de__timing_init(&s, &t);
    for (int i = 0; i < 100000; ++i)
        de__timing_advance(&s, 16666666, &f);
    EXPECT_EQ(s.steps_total, 99999u);

    /* Alpha is the leftover fraction: a quarter step leaves alpha 0.25. */
    de__timing_init(&s, &t);
    de__timing_advance(&s, 16666667 / 4 + 1, &f);
    EXPECT_EQ(f.steps, 0u);
    EXPECT_FLOAT_EQ(f.alpha, 0.25, 1e-6);
    EXPECT_FLOAT_EQ(f.frame_dt, 0.0041666, 1e-6);
    EXPECT_TRUE(f.step_dt == 0.0f); /* no step this frame */

    /* The step length the loop uses for its first frame runs exactly one step. */
    de__timing_init(&s, &t);
    EXPECT_EQ(de__timing_step_ns(&s), 16666667u);
    de__timing_advance(&s, de__timing_step_ns(&s), &f);
    EXPECT_EQ(f.steps, 1u);
    de__timing_init(&s, &t);
    de__timing_advance(&s, de__timing_step_ns(&s) - 1, &f);
    EXPECT_EQ(f.steps, 0u);

    /* Alpha never rounds up to 1: one unit short of a step. */
    de__timing_init(&s, &t);
    s.acc = DE__TIMING_UNIT - 61;
    de__timing_advance(&s, 1, &f); /* + 60 units */
    EXPECT_EQ(f.steps, 0u);
    EXPECT_TRUE(f.alpha < 1.0f);
}

TEST(timing, catchup_clamp) {
    de__timing_state s;
    de_timing t = timing(DE_TIMING_FIXED, 60, 4, 1.0f);
    de__timing_init(&s, &t);
    de__timing_frame f;

    /* A quarter step pending, then a 1 s hitch: 4 steps, the other 56 whole steps dropped, the
     * quarter kept. */
    de__timing_advance(&s, 4166667, &f);
    EXPECT_EQ(f.steps, 0u);
    const f32 alpha_before = f.alpha;
    de__timing_advance(&s, S, &f);
    EXPECT_EQ(f.steps, 4u);
    EXPECT_EQ(f.dropped_ns, 56u * S / 60u);
    EXPECT_EQ(s.dropped_ns_total, f.dropped_ns);
    EXPECT_FLOAT_EQ(f.alpha, alpha_before, 1e-6); /* the leftover fraction is kept */
    EXPECT_TRUE(f.step_dt == 1.0f / 60.0f);

    /* The next normal frame (one 60 Hz period) runs 0 or 1 steps, never a burst. */
    de__timing_advance(&s, 16666667, &f);
    EXPECT_LE(f.steps, 1u);
    EXPECT_EQ(f.dropped_ns, 0u);
    /* And steady frames afterwards run 1 each. */
    u32 total = 0;
    for (int i = 0; i < 60; ++i) {
        de__timing_advance(&s, 16666667, &f);
        total += f.steps;
        EXPECT_LE(f.steps, 2u);
    }
    EXPECT_GE(total, 59u);
    EXPECT_LE(total, 61u);

    /* Exactly max_catchup_steps of time is no hitch: nothing dropped. */
    de__timing_init(&s, &t);
    de__timing_advance(&s, 4u * 16666667u, &f);
    EXPECT_EQ(f.steps, 4u);
    EXPECT_EQ(f.dropped_ns, 0u);

    /* A huge delta (a debugger stop) is cut to DE__TIMING_MAX_FRAME_NS first: no overflow. */
    t = timing(DE_TIMING_FIXED, 10000, 1000, 1000.0f);
    de__timing_init(&s, &t);
    de__timing_advance(&s, ~0ull, &f);
    EXPECT_EQ(f.steps, 1000u);
    EXPECT_EQ(f.scaled_ns, DE__TIMING_MAX_FRAME_NS * 1000u);
    EXPECT_LT(s.acc, DE__TIMING_UNIT);
}

TEST(timing, variable_one_step) {
    de__timing_state s;
    de_timing t = timing(DE_TIMING_VARIABLE, 60, 4, 1.0f);
    de__timing_init(&s, &t);
    EXPECT_FLOAT_EQ(s.alpha, 1.0, 0.0);
    de__timing_frame f;
    u64 seed = 12345;
    for (int i = 0; i < 1000; ++i) {
        const u64 dt = rng_next(&seed) % (60 * MS); /* below the 66.7 ms catch-up limit */
        de__timing_advance(&s, dt, &f);
        EXPECT_EQ(f.steps, 1u);
        EXPECT_TRUE(f.step_dt == (f32)((f64)dt * 1e-9)); /* the frame delta, as f32 */
        EXPECT_TRUE(f.frame_dt == f.step_dt);
        EXPECT_FLOAT_EQ(f.alpha, 1.0, 0.0); /* the state is current */
        EXPECT_EQ(f.dropped_ns, 0u);
    }
    EXPECT_EQ(s.steps_total, 1000u);
    EXPECT_EQ(s.acc, 0u); /* nothing accumulates */

    /* Time scale applies to the step dt. */
    t.time_scale = 0.5f;
    de__timing_set(&s, &t);
    de__timing_advance(&s, 20 * MS, &f);
    EXPECT_EQ(f.steps, 1u);
    EXPECT_FLOAT_EQ(f.step_dt, 0.010, 1e-9);
    EXPECT_EQ(f.scaled_ns, 10 * MS);

    /* A hitch is cut to max_catchup_steps fixed steps (4 / 60 s), the rest dropped. */
    t.time_scale = 1.0f;
    de__timing_set(&s, &t);
    de__timing_advance(&s, S, &f);
    EXPECT_EQ(f.steps, 1u);
    EXPECT_FLOAT_EQ(f.step_dt, 4.0 / 60.0, 1e-7);
    EXPECT_EQ(f.dropped_ns, S - 4u * S / 60u);
    EXPECT_FLOAT_EQ(f.frame_dt, 1.0, 1e-7); /* the other schedules see the real delta */
}

TEST(timing, time_scale_pause) {
    de__timing_state s;
    de_timing t = timing(DE_TIMING_FIXED, 60, 1000, 1.0f);
    de__timing_init(&s, &t);
    de__timing_frame f;
    de__timing_advance(&s, 16666667 + 8333333, &f); /* 1.5 steps */
    EXPECT_EQ(f.steps, 1u);
    const f32 alpha = f.alpha;
    EXPECT_FLOAT_EQ(alpha, 0.5, 1e-6);

    /* Paused: no step, alpha frozen, nothing accumulates however long the pause is. */
    t.time_scale = 0.0f;
    EXPECT_FALSE(de__timing_set(&s, &t));
    for (int i = 0; i < 100; ++i) {
        de__timing_advance(&s, 50 * MS, &f);
        EXPECT_TRUE(f.paused);
        EXPECT_EQ(f.steps, 0u);
        EXPECT_TRUE(f.alpha == alpha);
        EXPECT_TRUE(f.frame_dt == 0.0f);
        EXPECT_EQ(f.dropped_ns, 0u);
    }
    EXPECT_EQ(s.steps_total, 1u);

    /* Resumed: the half step pending before the pause is still there. */
    t.time_scale = 1.0f;
    de__timing_set(&s, &t);
    de__timing_advance(&s, 8333334, &f);
    EXPECT_FALSE(f.paused);
    EXPECT_EQ(f.steps, 1u);

    /* Scale 2: 1 s of real time is 120 steps at 60 Hz. */
    t.time_scale = 2.0f;
    de__timing_init(&s, &t);
    EXPECT_EQ(feed_random(&s, S, 10 * MS, 99, NULL), 120u);

    /* Scale 0.3: 10 s of real time in odd frames is exactly 3 s of game time (the sub-ns
     * remainder is carried), 180 steps. */
    t.time_scale = 0.3f;
    de__timing_init(&s, &t);
    u64 scaled = 0;
    for (int i = 0; i < 10000; ++i) { /* 10000 frames of 1 ms */
        de__timing_advance(&s, MS, &f);
        scaled += f.scaled_ns;
    }
    /* 0.3f is 0.30000001192...: 10 s of it is 3.0000001192 s. */
    EXPECT_GE(scaled, 3000000119u);
    EXPECT_LE(scaled, 3000000120u);
    EXPECT_EQ(s.steps_total, 180u);

    /* Paused in variable mode: no step either. */
    t = timing(DE_TIMING_VARIABLE, 60, 4, 0.0f);
    de__timing_init(&s, &t);
    de__timing_advance(&s, 16 * MS, &f);
    EXPECT_EQ(f.steps, 0u);
    EXPECT_TRUE(f.paused);
}

TEST(timing, mode_switch) {
    de__timing_state s;
    de_timing fixed = timing(DE_TIMING_FIXED, 60, 4, 1.0f);
    de_timing variable = timing(DE_TIMING_VARIABLE, 60, 4, 1.0f);
    de__timing_init(&s, &fixed);
    de__timing_frame f;
    de__timing_advance(&s, 10 * MS, &f); /* 0.6 step pending */
    EXPECT_GT(s.acc, 0u);

    /* The same timing again changes nothing. */
    EXPECT_FALSE(de__timing_set(&s, &fixed));
    EXPECT_GT(s.acc, 0u);

    /* Fixed → variable: the accumulator is reset; the next frame runs one step of its delta. */
    EXPECT_TRUE(de__timing_set(&s, &variable));
    EXPECT_EQ(s.acc, 0u);
    de__timing_advance(&s, 5 * MS, &f);
    EXPECT_EQ(f.steps, 1u);
    EXPECT_FLOAT_EQ(f.step_dt, 0.005, 1e-9);

    /* Variable → fixed: starts empty, so 10 ms (0.6 step) runs nothing and alpha is 0.6. */
    EXPECT_TRUE(de__timing_set(&s, &fixed));
    EXPECT_EQ(s.acc, 0u);
    EXPECT_FLOAT_EQ(s.alpha, 0.0, 0.0);
    de__timing_advance(&s, 10 * MS, &f);
    EXPECT_EQ(f.steps, 0u);
    EXPECT_FLOAT_EQ(f.alpha, 0.6, 1e-6);

    /* A rate change keeps the pending time: 0.6 step at 60 Hz (10 ms) is 1.2 steps at 120 Hz. */
    de_timing fast = timing(DE_TIMING_FIXED, 120, 4, 1.0f);
    EXPECT_FALSE(de__timing_set(&s, &fast));
    de__timing_advance(&s, 0, &f);
    EXPECT_EQ(f.steps, 1u);
    EXPECT_FLOAT_EQ(f.alpha, 0.2, 1e-6);
    EXPECT_TRUE(f.step_dt == 1.0f / 120.0f);
}

TEST(timing, normalize_defaults) {
    /* Zero fields keep the defaults; out-of-range values are clamped. Time scale 0 stays 0. */
    de_timing t;
    memset(&t, 0, sizeof t);
    de__timing_normalize(&t);
    EXPECT_EQ(t.mode, DE_TIMING_FIXED);
    EXPECT_EQ(t.fixed_hz, DE_TIMING_DEFAULT_FIXED_HZ);
    EXPECT_EQ(t.max_catchup_steps, DE_TIMING_DEFAULT_MAX_CATCHUP_STEPS);
    EXPECT_FLOAT_EQ(t.time_scale, 0.0, 0.0);
    EXPECT_EQ(t.frame_cap_hz, 0u);

    t = timing((de_timing_mode)7, 1000000, 3, -2.0f);
    de__timing_normalize(&t);
    EXPECT_EQ(t.mode, DE_TIMING_FIXED);
    EXPECT_EQ(t.fixed_hz, DE__TIMING_MAX_FIXED_HZ);
    EXPECT_EQ(t.max_catchup_steps, 3u);
    EXPECT_FLOAT_EQ(t.time_scale, 1.0, 0.0);

    t = timing(DE_TIMING_VARIABLE, 60, 4, 1e30f);
    de__timing_normalize(&t);
    EXPECT_FLOAT_EQ(t.time_scale, DE__TIMING_MAX_TIME_SCALE, 0.0);

    /* NULL timing: the defaults. */
    de__timing_state s;
    de__timing_init(&s, NULL);
    EXPECT_EQ(s.timing.mode, DE_TIMING_FIXED);
    EXPECT_EQ(s.timing.fixed_hz, 60u);
    EXPECT_EQ(s.timing.max_catchup_steps, 4u);
    EXPECT_FLOAT_EQ(s.timing.time_scale, 1.0, 0.0);
}
