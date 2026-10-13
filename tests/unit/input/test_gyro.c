/* test_gyro.c: gyro calibration and gyro aiming (DENG-87). Synthetic gyro and accelerometer
 * samples are injected (input_test_util.h) at 200 Hz on the device clock, with 20 ms frames, so
 * each frame integrates 4 samples (the first sample of a pad has no interval). */
#include "input_test_util.h"

#include "input/haptics_internal.h"

#include "de_alloc_guard.h"
#include "de_test.h"

#include <string.h>

#define PAD               5u
#define FRAME_DT          0.02f
#define SAMPLE_NS         5000000ull /* 200 Hz */
#define SAMPLES_PER_FRAME 4u

typedef struct gyro_rig {
    de_input *in;
    de_gamepad pad;
    u64 sensor_ns;
} gyro_rig;

static bool rig_init(gyro_rig *r) {
    memset(r, 0, sizeof *r);
    r->in = input_test_create(NULL);
    if (r->in == NULL)
        return false;
    input_test_pad_add(r->in, PAD, DE_PLATFORM_PAD_PS5,
                       DE_PLATFORM_PAD_CAP_GYRO | DE_PLATFORM_PAD_CAP_ACCEL, "DualSense");
    de__input_begin_frame_dt(r->in, FRAME_DT);
    r->pad = input_test_pad(r->in, PAD);
    r->sensor_ns = 1000000000ull;
    return DE_HANDLE_VALID(r->pad);
}

/** Queues one gyro sample (and, when `accel` is not NULL, an accelerometer sample before it). */
static void rig_sample(gyro_rig *r, de_vec3 w, const de_vec3 *accel) {
    r->sensor_ns += SAMPLE_NS;
    if (accel != NULL)
        input_test_pad_sensor(r->in, PAD, DE_PLATFORM_SENSOR_ACCEL, accel->x, accel->y, accel->z,
                              r->sensor_ns);
    input_test_pad_sensor(r->in, PAD, DE_PLATFORM_SENSOR_GYRO, w.x, w.y, w.z, r->sensor_ns);
}

/** One frame with SAMPLES_PER_FRAME samples of rate `w`. */
static void rig_frame(gyro_rig *r, de_vec3 w, const de_vec3 *accel) {
    for (u32 i = 0; i < SAMPLES_PER_FRAME; ++i)
        rig_sample(r, w, accel);
    de__input_begin_frame_dt(r->in, FRAME_DT);
}

static de_vec2 rig_aim(const gyro_rig *r) {
    de_vec2 d = {0.0f, 0.0f};
    de_pad_gyro_aim(r->in, r->pad, &d);
    return d;
}

static void rig_configure(gyro_rig *r, const de_gyro_settings *s) {
    de_pad_gyro_configure(r->in, r->pad, s);
}

static const de_vec3 k_level = {0.0f, 9.81f, 0.0f}; /* accelerometer of a pad lying level */

/* ---- Calibration ----------------------------------------------------------------------------- */

TEST(gyro, calibrate_bias) {
    gyro_rig r;
    ASSERT_TRUE(rig_init(&r));
    const de_vec3 b = {0.012f, -0.021f, 0.0047f};
    rig_frame(&r, b, NULL);
    EXPECT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, NULL), DE_GYRO_CALIB_NONE);
    de_vec3 g;
    ASSERT_TRUE(de_pad_gyro(r.in, r.pad, &g)); /* raw until calibrated */
    EXPECT_FLOAT_EQ(g.y, b.y, 1e-7f);

    de_pad_gyro_calibrate(r.in, r.pad, 1.0f);
    EXPECT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, NULL), DE_GYRO_CALIB_RUNNING);
    /* 1 s of samples with a constant bias and a small zero-mean noise (sensor noise). */
    for (u32 f = 0; f < 50; ++f) {
        for (u32 i = 0; i < SAMPLES_PER_FRAME; ++i) {
            const f32 n = (i % 2u == 0u) ? 0.002f : -0.002f;
            rig_sample(&r, (de_vec3){b.x + n, b.y - n, b.z + n}, NULL);
        }
        de__input_begin_frame_dt(r.in, FRAME_DT);
        if (f < 48)
            EXPECT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, NULL), DE_GYRO_CALIB_RUNNING);
    }
    de_vec3 bias;
    ASSERT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, &bias), DE_GYRO_CALIB_DONE);
    EXPECT_FLOAT_EQ(bias.x, b.x, 1e-4f);
    EXPECT_FLOAT_EQ(bias.y, b.y, 1e-4f);
    EXPECT_FLOAT_EQ(bias.z, b.z, 1e-4f);

    /* The bias is removed from the processed gyro and from the aim. */
    rig_frame(&r, b, NULL);
    ASSERT_TRUE(de_pad_gyro(r.in, r.pad, &g));
    EXPECT_FLOAT_EQ(g.x, 0.0f, 1e-4f);
    EXPECT_FLOAT_EQ(g.y, 0.0f, 1e-4f);
    EXPECT_FLOAT_EQ(g.z, 0.0f, 1e-4f);
    EXPECT_FLOAT_EQ(r.in->pads[0].gyro.y, g.y, 0.0f); /* the field gyro bindings read */
    const de_vec2 d = rig_aim(&r);
    EXPECT_FLOAT_EQ(d.x, 0.0f, 1e-5f);
    EXPECT_FLOAT_EQ(d.y, 0.0f, 1e-5f);

    /* seconds <= 0 clears the bias. */
    de_pad_gyro_calibrate(r.in, r.pad, 0.0f);
    EXPECT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, &bias), DE_GYRO_CALIB_NONE);
    EXPECT_FLOAT_EQ(bias.y, 0.0f, 0.0f);
    de_input_destroy(r.in);
}

TEST(gyro, calibrate_rejects_motion) {
    gyro_rig r;
    ASSERT_TRUE(rig_init(&r));
    const de_vec3 b = {0.01f, 0.01f, -0.01f};
    rig_frame(&r, b, NULL);
    de_pad_gyro_calibrate(r.in, r.pad, 1.0f);
    for (u32 f = 0; f < 50; ++f)
        rig_frame(&r, b, NULL);
    ASSERT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, NULL), DE_GYRO_CALIB_DONE);

    /* A pad being moved (0.5 rad/s back and forth) is rejected; the old bias stays. */
    de_pad_gyro_calibrate(r.in, r.pad, 0.5f);
    for (u32 f = 0; f < 25; ++f) {
        for (u32 i = 0; i < SAMPLES_PER_FRAME; ++i) {
            const f32 m = (i % 2u == 0u) ? 0.5f : -0.5f;
            rig_sample(&r, (de_vec3){m, -m, 0.3f * m}, NULL);
        }
        de__input_begin_frame_dt(r.in, FRAME_DT);
    }
    de_vec3 bias;
    EXPECT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, &bias), DE_GYRO_CALIB_REJECTED_MOTION);
    EXPECT_FLOAT_EQ(bias.x, b.x, 1e-4f);

    /* A steady turn (no variance, but far too fast for a bias) is rejected as well. */
    de_pad_gyro_calibrate(r.in, r.pad, 0.5f);
    for (u32 f = 0; f < 25; ++f)
        rig_frame(&r, (de_vec3){0.0f, 0.6f, 0.0f}, NULL);
    EXPECT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, &bias), DE_GYRO_CALIB_REJECTED_MOTION);
    EXPECT_FLOAT_EQ(bias.y, b.y, 1e-4f);

    /* No samples at all. */
    de_pad_gyro_calibrate(r.in, r.pad, 0.2f);
    for (u32 f = 0; f < 10; ++f)
        de__input_begin_frame_dt(r.in, FRAME_DT);
    EXPECT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, NULL), DE_GYRO_CALIB_REJECTED_NO_SAMPLES);

    /* Stale handles and NULL are safe. */
    de_pad_gyro_calibrate(NULL, r.pad, 1.0f);
    de_pad_gyro_calibrate(r.in, (de_gamepad){0}, 1.0f);
    EXPECT_EQ(de_pad_gyro_calibration_state(r.in, (de_gamepad){0}, &bias), DE_GYRO_CALIB_NONE);
    input_test_pad_remove(r.in, PAD);
    de__input_begin_frame_dt(r.in, FRAME_DT);
    EXPECT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, &bias), DE_GYRO_CALIB_NONE);
    EXPECT_FALSE(de_pad_gyro_aim(r.in, r.pad, NULL));
    de_input_destroy(r.in);
}

TEST(gyro, auto_calibration) {
    gyro_rig r;
    ASSERT_TRUE(rig_init(&r));
    de_gyro_settings s = de_gyro_settings_default();
    s.auto_calibrate = true;
    rig_configure(&r, &s);
    const de_vec3 b = {-0.008f, 0.015f, 0.003f};

    /* Lying still for more than a second: the bias is learned. */
    for (u32 f = 0; f < 60; ++f) {
        for (u32 i = 0; i < SAMPLES_PER_FRAME; ++i) {
            const f32 n = (i % 2u == 0u) ? 0.001f : -0.001f;
            rig_sample(&r, (de_vec3){b.x + n, b.y + n, b.z - n}, &k_level);
        }
        de__input_begin_frame_dt(r.in, FRAME_DT);
    }
    de_vec3 bias;
    ASSERT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, &bias), DE_GYRO_CALIB_DONE);
    EXPECT_FLOAT_EQ(bias.x, b.x, 1e-4f);
    EXPECT_FLOAT_EQ(bias.y, b.y, 1e-4f);
    EXPECT_FLOAT_EQ(bias.z, b.z, 1e-4f);

    /* Turning the pad does not change the bias. */
    for (u32 f = 0; f < 100; ++f) {
        const f32 m = (f % 10u < 5u) ? 1.0f : -0.7f;
        rig_frame(&r, (de_vec3){0.2f * m, m, 0.0f}, &k_level);
    }
    ASSERT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, &bias), DE_GYRO_CALIB_DONE);
    EXPECT_FLOAT_EQ(bias.y, b.y, 1e-4f);

    /* A steady gyro while the accelerometer moves (the pad is carried) is not still either. */
    const de_vec3 b2 = {0.05f, 0.05f, 0.05f};
    for (u32 f = 0; f < 100; ++f) {
        const de_vec3 a = {0.0f, (f % 2u == 0u) ? 8.0f : 11.5f, 1.0f};
        rig_frame(&r, b2, &a);
    }
    ASSERT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, &bias), DE_GYRO_CALIB_DONE);
    EXPECT_FLOAT_EQ(bias.x, b.x, 1e-4f);

    /* With auto calibration off, a still pad teaches nothing. */
    s.auto_calibrate = false;
    rig_configure(&r, &s);
    for (u32 f = 0; f < 100; ++f)
        rig_frame(&r, b2, &k_level);
    de_pad_gyro_calibration_state(r.in, r.pad, &bias);
    EXPECT_FLOAT_EQ(bias.x, b.x, 1e-4f);

    /* A pad that connects again starts without bias and with the default settings. */
    input_test_pad_remove(r.in, PAD);
    de__input_begin_frame_dt(r.in, FRAME_DT);
    input_test_pad_add(r.in, PAD, DE_PLATFORM_PAD_PS5, DE_PLATFORM_PAD_CAP_GYRO, "DualSense");
    de__input_begin_frame_dt(r.in, FRAME_DT);
    r.pad = input_test_pad(r.in, PAD);
    EXPECT_EQ(de_pad_gyro_calibration_state(r.in, r.pad, &bias), DE_GYRO_CALIB_NONE);
    de_gyro_settings now;
    ASSERT_TRUE(de_pad_gyro_settings(r.in, r.pad, &now));
    EXPECT_FALSE(now.auto_calibrate);
    de_input_destroy(r.in);
}

/* ---- Aiming ---------------------------------------------------------------------------------- */

TEST(gyro, sensitivity_and_smoothing) {
    gyro_rig r;
    ASSERT_TRUE(rig_init(&r));
    de_gyro_settings s = de_gyro_settings_default();
    EXPECT_FLOAT_EQ(s.sensitivity.x, 1.0f, 0.0f);
    EXPECT_EQ(s.yaw_axis, DE_GYRO_YAW_LOCAL_Y);

    /* Turning right (clockwise seen from above: negative rate around +y) at 1 rad/s aims right
     * 1 rad/s: 0.02 rad per 20 ms frame. Pitching up (positive around +x) aims up. */
    rig_frame(&r, (de_vec3){0.0f, -1.0f, 0.0f}, NULL);
    de_vec2 d = rig_aim(&r);
    EXPECT_FLOAT_EQ(d.x, 0.015f, 1e-6f); /* the first sample has no interval */
    rig_frame(&r, (de_vec3){0.5f, -1.0f, 0.0f}, NULL);
    d = rig_aim(&r);
    EXPECT_FLOAT_EQ(d.x, 0.02f, 1e-6f);
    EXPECT_FLOAT_EQ(d.y, 0.01f, 1e-6f);

    /* Sensitivity and inversion. */
    s.sensitivity = (de_vec2){2.0f, 0.5f};
    s.invert_y = true;
    rig_configure(&r, &s);
    rig_frame(&r, (de_vec3){0.4f, -1.0f, 0.0f}, NULL);
    d = rig_aim(&r);
    EXPECT_FLOAT_EQ(d.x, 0.04f, 1e-6f);
    EXPECT_FLOAT_EQ(d.y, -0.004f, 1e-6f);

    /* Yaw axis: roll (local z), and player space from the accelerometer. */
    s = de_gyro_settings_default();
    s.yaw_axis = DE_GYRO_YAW_LOCAL_Z;
    rig_configure(&r, &s);
    rig_frame(&r, (de_vec3){0.0f, -1.0f, -1.0f}, NULL);
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.02f, 1e-6f);
    s.yaw_axis = DE_GYRO_YAW_PLAYER;
    rig_configure(&r, &s);
    rig_frame(&r, (de_vec3){0.0f, -1.0f, 0.0f}, &k_level); /* level: player yaw = local yaw */
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.02f, 1e-5f);
    const de_vec3 upright = {0.0f, 0.0f, 9.81f}; /* pad held upright, face to the player */
    for (u32 f = 0; f < 60; ++f)                 /* let the gravity estimate settle */
        rig_frame(&r, (de_vec3){0.0f, 0.0f, 0.0f}, &upright);
    rig_frame(&r, (de_vec3){0.0f, 0.0f, -1.0f}, &upright);
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.02f, 1e-4f);
    /* Upright, the pad's own y axis is horizontal: turning around it is no world yaw. */
    rig_frame(&r, (de_vec3){0.0f, -1.0f, 0.0f}, &upright);
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.0f, 1e-4f);

    /* Tightening: below 0.1 rad/s the rate scales by rate / 0.1. */
    s = de_gyro_settings_default();
    s.tightening = 0.1f;
    rig_configure(&r, &s);
    rig_frame(&r, (de_vec3){0.0f, -0.05f, 0.0f}, NULL);
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.05f * 0.5f * FRAME_DT, 1e-7f);
    rig_frame(&r, (de_vec3){0.0f, -0.2f, 0.0f}, NULL);
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.2f * FRAME_DT, 1e-7f);

    /* Tiered smoothing: a small jitter (below half the threshold) is averaged away ... */
    s = de_gyro_settings_default();
    s.smoothing_threshold = 0.2f;
    s.smoothing_time = 0.1f;
    rig_configure(&r, &s);
    f32 worst = 0.0f;
    for (u32 f = 0; f < 20; ++f) {
        rig_frame(&r, (de_vec3){0.0f, (f % 2u == 0u) ? 0.05f : -0.05f, 0.0f}, NULL);
        const f32 x = rig_aim(&r).x;
        if (f >= 5 && (x < 0.0f ? -x : x) > worst)
            worst = x < 0.0f ? -x : x;
    }
    EXPECT_TRUE(worst < 0.5f * 0.05f * FRAME_DT); /* unsmoothed it would be 0.001 per frame */
    /* ... while a fast turn passes unchanged once the window holds only fast samples. */
    for (u32 f = 0; f < 10; ++f)
        rig_frame(&r, (de_vec3){0.0f, -1.0f, 0.0f}, NULL);
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.02f, 1e-6f);

    /* NaN fields take their defaults; a stale handle reports defaults and false. */
    volatile f32 zero = 0.0f; /* not a constant: MSVC rejects a constant 0 / 0 */
    s.sensitivity.x = zero / zero;
    rig_configure(&r, &s);
    de_gyro_settings got;
    ASSERT_TRUE(de_pad_gyro_settings(r.in, r.pad, &got));
    EXPECT_FLOAT_EQ(got.sensitivity.x, 1.0f, 0.0f);
    EXPECT_FALSE(de_pad_gyro_settings(r.in, (de_gamepad){0xFFFFu}, &got));

    /* An invalid handle sets every pad and the pads that connect later. */
    s = de_gyro_settings_default();
    s.sensitivity = (de_vec2){3.0f, 3.0f};
    de_pad_gyro_configure(r.in, (de_gamepad){0}, &s);
    ASSERT_TRUE(de_pad_gyro_settings(r.in, r.pad, &got));
    EXPECT_FLOAT_EQ(got.sensitivity.x, 3.0f, 0.0f);
    input_test_pad_add(r.in, 77, DE_PLATFORM_PAD_PS4, DE_PLATFORM_PAD_CAP_GYRO, "DS4");
    de__input_begin_frame_dt(r.in, FRAME_DT);
    ASSERT_TRUE(de_pad_gyro_settings(r.in, input_test_pad(r.in, 77), &got));
    EXPECT_FLOAT_EQ(got.sensitivity.y, 3.0f, 0.0f);
    de_input_destroy(r.in);
}

TEST(gyro, ratchet_suppresses) {
    gyro_rig r;
    ASSERT_TRUE(rig_init(&r));
    de_gyro_settings s = de_gyro_settings_default();
    s.ratchet_buttons = 1u << DE_PAD_LSHOULDER;
    s.smoothing_threshold = 3.0f; /* smoothing on, so stale samples would show after release */
    rig_configure(&r, &s);
    const de_vec3 turn = {0.3f, -2.0f, 0.0f};

    for (u32 f = 0; f < 10; ++f)
        rig_frame(&r, turn, NULL);
    de_vec2 d = rig_aim(&r);
    EXPECT_FLOAT_EQ(d.x, 0.04f, 1e-5f);

    /* Held: exactly zero, while the processed gyro still reports the rotation. */
    input_test_pad_button(r.in, PAD, DE_PAD_LSHOULDER, true);
    rig_frame(&r, turn, NULL);
    d = rig_aim(&r);
    EXPECT_TRUE(d.x == 0.0f && d.y == 0.0f);
    de_vec3 g;
    ASSERT_TRUE(de_pad_gyro(r.in, r.pad, &g));
    EXPECT_FLOAT_EQ(g.y, -2.0f, 1e-6f);
    for (u32 f = 0; f < 10; ++f) {
        rig_frame(&r, (de_vec3){-1.0f, 3.0f, 0.5f}, NULL); /* the player re-centres the pad */
        d = rig_aim(&r);
        EXPECT_TRUE(d.x == 0.0f && d.y == 0.0f);
    }

    /* Released: the aim resumes with this frame's motion only. No jump. */
    input_test_pad_button(r.in, PAD, DE_PAD_LSHOULDER, false);
    rig_frame(&r, turn, NULL);
    d = rig_aim(&r);
    EXPECT_FLOAT_EQ(d.x, 0.04f, 1e-5f);
    EXPECT_FLOAT_EQ(d.y, 0.006f, 1e-5f);

    /* A press and release inside one frame still holds that frame. */
    input_test_pad_button(r.in, PAD, DE_PAD_LSHOULDER, true);
    input_test_pad_button(r.in, PAD, DE_PAD_LSHOULDER, false);
    rig_frame(&r, turn, NULL);
    d = rig_aim(&r);
    EXPECT_TRUE(d.x == 0.0f && d.y == 0.0f);
    rig_frame(&r, turn, NULL);
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.04f, 1e-5f);

    /* Hold-to-aim mode: the other way round. */
    s.ratchet_mode = DE_GYRO_RATCHET_HOLD_TO_AIM;
    rig_configure(&r, &s);
    rig_frame(&r, turn, NULL);
    d = rig_aim(&r);
    EXPECT_TRUE(d.x == 0.0f && d.y == 0.0f);
    input_test_pad_button(r.in, PAD, DE_PAD_LSHOULDER, true);
    rig_frame(&r, turn, NULL);
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.04f, 1e-5f);
    de_input_destroy(r.in);
}

/* One frame of gyro samples, then the action evaluation (system input.actions in a world). */
static void rig_frame_actions(gyro_rig *r, de_vec3 w) {
    rig_frame(r, w, NULL);
    de__actions_frame_update(r->in, FRAME_DT);
}

TEST(gyro, action_ratchet) {
    gyro_rig r;
    ASSERT_TRUE(rig_init(&r));
    /* A "gyro_off" button action on the R key (any device would do). */
    const de_action_map m = de_action_map_create(r.in, "aim");
    de_action_map_push(r.in, m);
    de_binding b;
    memset(&b, 0, sizeof b);
    b.source = DE_BIND_KEY;
    b.code = DE_KEY_R;
    b.composite_slot = DE_COMPOSITE_NONE;
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = "gyro_off";
    d.type = DE_ACTION_BUTTON;
    d.interaction = DE_INTERACT_PRESS;
    d.bindings = &b;
    d.binding_count = 1;
    const de_action off = de_action_add(r.in, m, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(off));

    de_gyro_settings s = de_gyro_settings_default();
    s.ratchet_action = off;
    s.smoothing_threshold = 3.0f;
    rig_configure(&r, &s);
    de_gyro_settings got;
    ASSERT_TRUE(de_pad_gyro_settings(r.in, r.pad, &got));
    EXPECT_EQ(got.ratchet_action.id, off.id);
    const de_vec3 turn = {0.3f, -2.0f, 0.0f};
    for (u32 f = 0; f < 10; ++f)
        rig_frame_actions(&r, turn);
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.04f, 1e-5f);

    /* Held: exactly zero from the frame the action is held in. */
    input_test_key(r.in, DE_KEY_R, true);
    rig_frame_actions(&r, turn);
    EXPECT_TRUE(de_action_held(r.in, off));
    de_vec2 a = rig_aim(&r);
    EXPECT_TRUE(a.x == 0.0f && a.y == 0.0f);
    for (u32 f = 0; f < 5; ++f) {
        rig_frame_actions(&r, (de_vec3){-1.0f, 3.0f, 0.5f});
        a = rig_aim(&r);
        EXPECT_TRUE(a.x == 0.0f && a.y == 0.0f);
    }

    /* Released: the release frame still reads zero (the gyro update saw the action held), then
     * the aim resumes with each frame's own motion, never with the motion made while held. */
    input_test_key(r.in, DE_KEY_R, false);
    rig_frame_actions(&r, turn);
    a = rig_aim(&r);
    EXPECT_TRUE(a.x == 0.0f && a.y == 0.0f);
    rig_frame_actions(&r, turn);
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.04f, 1e-5f);
    EXPECT_FLOAT_EQ(rig_aim(&r).y, 0.006f, 1e-5f);

    /* A stale action handle is no ratchet. */
    s.ratchet_action = (de_action){0xFFFFFu};
    rig_configure(&r, &s);
    rig_frame_actions(&r, turn);
    rig_frame_actions(&r, turn);
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.04f, 1e-5f);
    de_input_destroy(r.in);
}

TEST(gyro, joycon_pair_and_no_alloc) {
    gyro_rig r;
    ASSERT_TRUE(rig_init(&r));
    /* A Joy-Con pair reports two gyros: only the first kind seen is integrated. */
    for (u32 f = 0; f < 3; ++f) {
        for (u32 i = 0; i < SAMPLES_PER_FRAME; ++i) {
            r.sensor_ns += SAMPLE_NS;
            input_test_pad_sensor(r.in, PAD, DE_PLATFORM_SENSOR_GYRO_L, 0.0f, -1.0f, 0.0f,
                                  r.sensor_ns);
            input_test_pad_sensor(r.in, PAD, DE_PLATFORM_SENSOR_GYRO_R, 0.0f, -1.0f, 0.0f,
                                  r.sensor_ns);
        }
        de__input_begin_frame_dt(r.in, FRAME_DT);
    }
    EXPECT_FLOAT_EQ(rig_aim(&r).x, 0.02f, 1e-6f);

    /* Samples, calibration, smoothing and ratchet allocate nothing per frame. */
    de_gyro_settings s = de_gyro_settings_default();
    s.smoothing_threshold = 0.5f;
    s.tightening = 0.05f;
    s.auto_calibrate = true;
    s.ratchet_buttons = 1u << DE_PAD_RSHOULDER;
    s.yaw_axis = DE_GYRO_YAW_PLAYER;
    rig_configure(&r, &s);
    de_pad_gyro_calibrate(r.in, r.pad, 0.5f);
    rig_frame(&r, (de_vec3){0.0f, 0.0f, 0.0f}, &k_level);

    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    for (u32 f = 0; f < 200; ++f) {
        if (f % 7u == 0u)
            input_test_pad_button(r.in, PAD, DE_PAD_RSHOULDER, (f % 14u) == 0u);
        if (f == 100)
            de_pad_gyro_calibrate(r.in, r.pad, 0.5f);
        rig_frame(&r, (de_vec3){0.01f * (f32)(f % 5u), -0.5f, 0.1f}, &k_level);
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    de_input_destroy(r.in);
}
