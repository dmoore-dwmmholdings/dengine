/* haptics_internal.h: internals of haptics.c and gyro.c (DENG-87), shared with the tests and with
 * the action evaluation of DENG-84 (gyro bindings).
 *
 * Haptics go out through a device sink. By default the sink is the platform layer
 * (de_platform_pad_rumble / _trigger_rumble / _send_effect); tests install a mock that records
 * the calls, so they need no device and no SDL. */
#ifndef DE_INPUT_HAPTICS_INTERNAL_H
#define DE_INPUT_HAPTICS_INTERNAL_H
#include "input_internal.h"

DE_EXTERN_C_BEGIN

/* ---------- Haptics ---------- */

/** Extra milliseconds on top of the remaining effect time that the device is asked to run an
 * effect. The engine stops effects itself (de__haptics_update); the device timer only ends an
 * effect when the engine stops calling, so a stalled process does not rumble forever. */
#define DE__HAPTICS_DEVICE_MARGIN_MS 250u
/** Remaining time at or below which an effect counts as ended (absorbs f32 rounding of the
 * per-frame countdown). */
#define DE__HAPTICS_END_EPSILON 1e-5f

/** Bytes of one DualSense trigger effect block. */
#define DE__DS5_TRIGGER_BYTES 11u
/** Bytes of the DualSense effect packet (SDL's DS5EffectsState_t) sent through
 * de_platform_pad_send_effect(). */
#define DE__DS5_EFFECT_PACKET_BYTES 47u
/** Offsets in the effect packet. */
#define DE__DS5_OFF_ENABLE_BITS1  0u
#define DE__DS5_OFF_RIGHT_TRIGGER 10u
#define DE__DS5_OFF_LEFT_TRIGGER  21u
/** ucEnableBits1 flags: modify the right / left trigger effect. */
#define DE__DS5_ENABLE_RIGHT_TRIGGER 0x04u
#define DE__DS5_ENABLE_LEFT_TRIGGER  0x08u
/** DualSense trigger effect type bytes. */
#define DE__DS5_EFFECT_OFF       0x05u
#define DE__DS5_EFFECT_FEEDBACK  0x21u
#define DE__DS5_EFFECT_WEAPON    0x25u
#define DE__DS5_EFFECT_VIBRATION 0x26u

/** Where haptics go. Each function returns DE_OK or the platform's error (the caller clears it
 * and logs once). `device` is the platform instance id (de__input_pad.device). */
typedef struct de__haptics_sink {
    de_result (*rumble)(void *user, u32 device, f32 low, f32 high, u32 duration_ms);
    de_result (*trigger_rumble)(void *user, u32 device, f32 left, f32 right, u32 duration_ms);
    de_result (*send_effect)(void *user, u32 device, const void *data, u32 size);
    void *user;
} de__haptics_sink;

/** Replaces the sink of `in` (copied). NULL puts the platform sink back. Main thread only. */
void de__haptics_set_sink(de_input *in, const de__haptics_sink *sink);

/**
 * Encodes `fx` as a DualSense trigger effect block (DE__DS5_TRIGGER_BYTES bytes in `out`, the
 * table in de_input.h at de_trigger_mode), with its strength multiplied by `scale` in [0, 1]
 * (rounded; 0 turns the effect off). NULL `fx` is DE_TRIGGER_OFF.
 * @return false (and `out` untouched) when a field is out of its range.
 */
bool de__haptics_encode_trigger(const de_trigger_effect *fx, f32 scale,
                                u8 out[DE__DS5_TRIGGER_BYTES]);

/** The haptics scale in effect: `input.haptics_scale` clamped to [0, 1], or 0 when
 * `input.haptics_enabled` is false. */
f32 de__haptics_effective_scale(const de_input *in);

/* ---------- Gyro ---------- */

/** Longest sample interval integrated, seconds: a longer gap (a dropped sample burst, a pad that
 * was idle) counts as this long. */
#define DE__GYRO_MAX_SAMPLE_DT 0.05f
/** Manual calibration limits: per-axis standard deviation and mean magnitude, rad/s. */
#define DE__GYRO_CALIB_MAX_STDDEV 0.03f
#define DE__GYRO_CALIB_MAX_BIAS   0.15f
/** Automatic calibration: a sample further than this from the running mean ends the still
 * window (gyro rad/s, accelerometer m/s²); a window this long (seconds) sets the bias. */
#define DE__GYRO_STILL_GYRO_DEV  0.05f
#define DE__GYRO_STILL_ACCEL_DEV 0.5f
#define DE__GYRO_STILL_SECONDS   1.0f
/** Smoothing buffer, samples. */
#define DE__GYRO_SMOOTH_CAP 64u
/** Player-space yaw: how much a tilted turn is boosted, at most up to the full rate (the "relax
 * factor" of the player-space method). */
#define DE__GYRO_PLAYER_RELAX 1.41f

/** This frame's gyro aim of the pad in slot `slot` (radians, +x right, +y up): what
 * de_pad_gyro_aim() returns, for gyro bindings (DE_BIND_GYRO, DENG-84). False (and zero) for a
 * free slot or a pad without gyro samples. */
bool de__gyro_aim_slot(const de_input *in, u32 slot, de_vec2 *delta);

DE_EXTERN_C_END
#endif
