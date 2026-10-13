/* gyro.c: gyro calibration and gyro aiming (DENG-87). SPEC §15.
 *
 * de__gyro_update() runs in de_input_begin_frame() after the frame's events are applied. For each
 * pad it walks the frame's raw events (de_input.frame_events) and handles every gyro and
 * accelerometer sample in arrival order:
 *   - the sample interval comes from the device timestamps (the host receive times when the
 *     device has none), capped at DE__GYRO_MAX_SAMPLE_DT;
 *   - a running manual calibration accumulates the raw sample (Welford, f64);
 *   - automatic calibration, when on, tracks a "still" window (gyro and accelerometer within a
 *     small distance of their running means); a window of DE__GYRO_STILL_SECONDS sets the bias;
 *   - the aim integrates the bias-free rate: yaw axis choice, tiered smoothing, tightening,
 *     sensitivity and inversion, times the sample interval.
 * The processed gyro (de__input_pad.gyro, what de_pad_gyro() returns and DENG-84 gyro bindings
 * read) is the frame's latest raw sample minus the bias.
 *
 * Joy-Con pairs report a left and a right gyro; only the first gyro sensor kind a pad sends is
 * integrated, so the rotation is not counted twice.
 *
 * Gyro data is device input, not simulation state (the recorded action stream of DENG-89 is the
 * deterministic input), but the module is built as deterministic code, so the math uses only
 * + - * / and de_dsqrt (check_determinism_src). */
#include "action_internal.h"
#include "haptics_internal.h"

#include <dengine/de_memory.h>

#include <string.h>

typedef struct gyro_pad {
    u32 device, generation; /* the pad this state belongs to */
    de_gyro_settings settings;
    de_vec3 bias;
    de_gyro_calibration calib;

    /* Manual calibration (de_pad_gyro_calibrate). */
    bool calibrating;
    f32 calib_seconds, calib_elapsed;
    u64 calib_n;
    f64 calib_mean[3], calib_m2[3];

    /* Automatic calibration: the current still window. */
    u64 still_n;
    f64 still_mean[3];
    f32 still_time;
    u64 still_accel_n;
    f64 still_accel_mean[3];

    /* Sample timing of the integrated gyro sensor. */
    u32 sensor; /* de_platform_sensor_type integrated; 0 = none seen yet */
    bool have_prev;
    u64 prev_sensor_ns, prev_time_ns;

    /* Gravity direction (low-passed accelerometer), for player-space yaw. */
    de_vec3 grav;
    bool have_grav;

    /* Aim: this frame's delta, and the smoothing buffer (ring of rate * weight). */
    de_vec2 aim;
    bool aim_valid;
    de_vec2 smooth_v[DE__GYRO_SMOOTH_CAP];
    f32 smooth_dt[DE__GYRO_SMOOTH_CAP];
    u32 smooth_head, smooth_count;
} gyro_pad;

struct de__gyro {
    de_gyro_settings defaults; /* what connecting pads get (de_pad_gyro_configure with id 0) */
    gyro_pad pads[DE_INPUT_MAX_PADS];
};

/* ---------- Helpers ---------- */

static f32 absf(f32 v) {
    return v < 0.0f ? -v : v;
}

static f32 clampf(f32 v, f32 lo, f32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Vector lengths through de_dsqrt: the module is built as deterministic code, which may not call
 * the fast-math API (check_determinism_src). */
static f32 len2(de_vec2 v) {
    return de_dsqrt(v.x * v.x + v.y * v.y);
}

static f32 len3(de_vec3 v) {
    return de_dsqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

static bool is_gyro(u32 sensor) {
    return sensor == DE_PLATFORM_SENSOR_GYRO || sensor == DE_PLATFORM_SENSOR_GYRO_L ||
           sensor == DE_PLATFORM_SENSOR_GYRO_R;
}

static bool is_accel(u32 sensor) {
    return sensor == DE_PLATFORM_SENSOR_ACCEL || sensor == DE_PLATFORM_SENSOR_ACCEL_L ||
           sensor == DE_PLATFORM_SENSOR_ACCEL_R;
}

de_gyro_settings de_gyro_settings_default(void) {
    de_gyro_settings s;
    memset(&s, 0, sizeof s);
    s.sensitivity = (de_vec2){1.0f, 1.0f};
    s.yaw_axis = DE_GYRO_YAW_LOCAL_Y;
    s.smoothing_time = 0.125f;
    s.ratchet_mode = DE_GYRO_RATCHET_HOLD_TO_PAUSE;
    return s;
}

/** `s` with NaN fields replaced by their default and ranges applied. */
static de_gyro_settings sanitize(const de_gyro_settings *s) {
    const de_gyro_settings d = de_gyro_settings_default();
    de_gyro_settings r = s != NULL ? *s : d;
    if (r.sensitivity.x != r.sensitivity.x)
        r.sensitivity.x = d.sensitivity.x;
    if (r.sensitivity.y != r.sensitivity.y)
        r.sensitivity.y = d.sensitivity.y;
    if ((u32)r.yaw_axis > (u32)DE_GYRO_YAW_PLAYER)
        r.yaw_axis = d.yaw_axis;
    if ((u32)r.ratchet_mode > (u32)DE_GYRO_RATCHET_HOLD_TO_AIM)
        r.ratchet_mode = d.ratchet_mode;
    r.tightening = r.tightening > 0.0f ? r.tightening : 0.0f; /* NaN, negative: off */
    r.smoothing_threshold = r.smoothing_threshold > 0.0f ? r.smoothing_threshold : 0.0f;
    r.smoothing_time = r.smoothing_time == r.smoothing_time ? clampf(r.smoothing_time, 0.0f, 1.0f)
                                                            : d.smoothing_time;
    r.ratchet_buttons &= (1u << DE_PAD_BUTTON_COUNT_) - 1u;
    return r;
}

static void still_reset(gyro_pad *gp) {
    gp->still_n = 0;
    gp->still_time = 0.0f;
    gp->still_accel_n = 0;
}

static void smooth_clear(gyro_pad *gp) {
    gp->smooth_head = 0;
    gp->smooth_count = 0;
}

/** The gyro state of slot `slot`, reset when another pad now uses the slot (it then gets the
 * default settings). */
static gyro_pad *pad_state(const de_input *in, u32 slot) {
    const de__input_pad *pad = &in->pads[slot];
    gyro_pad *gp = &in->gyro->pads[slot];
    if (gp->device != pad->device || gp->generation != pad->generation) {
        memset(gp, 0, sizeof *gp);
        gp->device = pad->device;
        gp->generation = pad->generation;
        gp->settings = in->gyro->defaults;
    }
    return gp;
}

/** Slot of live handle `p`, or DE_INPUT_MAX_PADS. */
static u32 slot_of(const de_input *in, de_gamepad p) {
    const de__input_pad *pad = de__input_pad_get_const(in, p);
    return pad != NULL ? (u32)(pad - in->pads) : DE_INPUT_MAX_PADS;
}

static de_vec3 sub_bias(de_vec3 v, de_vec3 b) {
    return (de_vec3){v.x - b.x, v.y - b.y, v.z - b.z};
}

/* ---------- Calibration ---------- */

/** Welford update of `mean` / `m2` (3 axes) with sample `v`; `n` is the count after it. */
static void welford_add(u64 n, f64 mean[3], f64 m2[3], de_vec3 v) {
    const f64 x[3] = {(f64)v.x, (f64)v.y, (f64)v.z};
    for (u32 i = 0; i < 3u; ++i) {
        const f64 d = x[i] - mean[i];
        mean[i] += d / (f64)n;
        if (m2 != NULL)
            m2[i] += d * (x[i] - mean[i]);
    }
}

static f32 mean_length(const f64 mean[3]) {
    return de_dsqrt((f32)(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]));
}

static void calib_finish(gyro_pad *gp) {
    gp->calibrating = false;
    still_reset(gp);
    if (gp->calib_n == 0) {
        gp->calib = DE_GYRO_CALIB_REJECTED_NO_SAMPLES;
        return;
    }
    for (u32 i = 0; i < 3u; ++i) {
        const f64 var = gp->calib_m2[i] / (f64)gp->calib_n;
        if (de_dsqrt((f32)var) > DE__GYRO_CALIB_MAX_STDDEV) {
            gp->calib = DE_GYRO_CALIB_REJECTED_MOTION;
            return;
        }
    }
    if (mean_length(gp->calib_mean) > DE__GYRO_CALIB_MAX_BIAS) {
        gp->calib = DE_GYRO_CALIB_REJECTED_MOTION; /* a steady turn, not a bias */
        return;
    }
    gp->bias = (de_vec3){(f32)gp->calib_mean[0], (f32)gp->calib_mean[1], (f32)gp->calib_mean[2]};
    gp->calib = DE_GYRO_CALIB_DONE;
}

static bool far_from(const f64 mean[3], de_vec3 v, f32 limit) {
    return absf((f32)((f64)v.x - mean[0])) > limit || absf((f32)((f64)v.y - mean[1])) > limit ||
           absf((f32)((f64)v.z - mean[2])) > limit;
}

static void still_accel(gyro_pad *gp, de_vec3 a) {
    if (gp->still_accel_n > 0 && far_from(gp->still_accel_mean, a, DE__GYRO_STILL_ACCEL_DEV))
        still_reset(gp); /* the pad moved */
    gp->still_accel_n++;
    welford_add(gp->still_accel_n, gp->still_accel_mean, NULL, a);
}

static void still_gyro(gyro_pad *gp, de_vec3 w, f32 sdt) {
    if (gp->still_n > 0 && far_from(gp->still_mean, w, DE__GYRO_STILL_GYRO_DEV))
        still_reset(gp);
    gp->still_n++;
    welford_add(gp->still_n, gp->still_mean, NULL, w);
    gp->still_time += sdt;
    if (gp->still_time + 1e-4f < DE__GYRO_STILL_SECONDS)
        return;
    if (mean_length(gp->still_mean) <= DE__GYRO_CALIB_MAX_BIAS) {
        gp->bias =
            (de_vec3){(f32)gp->still_mean[0], (f32)gp->still_mean[1], (f32)gp->still_mean[2]};
        gp->calib = DE_GYRO_CALIB_DONE;
    }
    still_reset(gp); /* a new window follows the bias as it drifts */
}

/* ---------- Aim ---------- */

/** Yaw and pitch rates of bias-free rotation `w`, in aim directions (+x right, +y up). Turning
 * the pad left is a positive (counter-clockwise) rate around its up axis, so yaw is negated. */
static de_vec2 aim_rate(const gyro_pad *gp, de_vec3 w) {
    f32 yaw = w.y;
    switch (gp->settings.yaw_axis) {
    case DE_GYRO_YAW_LOCAL_Z:
        yaw = w.z;
        break;
    case DE_GYRO_YAW_PLAYER: {
        const f32 g = len3(gp->grav);
        if (gp->have_grav && g > 1e-3f) {
            /* Rotation around the real up axis; the pitch axis (x) does not turn the aim. A turn
             * made with the pad tilted is boosted by the relax factor, up to the full y/z rate. */
            const f32 world = (gp->grav.y * w.y + gp->grav.z * w.z) / g;
            const f32 full = len2((de_vec2){w.y, w.z});
            f32 mag = absf(world) * DE__GYRO_PLAYER_RELAX;
            if (mag > full)
                mag = full;
            yaw = world < 0.0f ? -mag : mag;
        }
        break;
    }
    case DE_GYRO_YAW_LOCAL_Y:
    default:
        break;
    }
    return (de_vec2){-yaw, w.x};
}

/** Tiered smoothing: the part of `v` below the threshold goes through the averaging buffer
 * (weighted by sample interval, over smoothing_time), the rest passes directly. */
static de_vec2 smooth(gyro_pad *gp, de_vec2 v, f32 sdt) {
    const f32 t = gp->settings.smoothing_threshold;
    if (!(t > 0.0f))
        return v;
    const f32 half = 0.5f * t;
    const f32 direct = clampf((len2(v) - half) / half, 0.0f, 1.0f);
    const de_vec2 rest = {v.x * (1.0f - direct), v.y * (1.0f - direct)};

    gp->smooth_head = (gp->smooth_head + 1u) % DE__GYRO_SMOOTH_CAP;
    gp->smooth_v[gp->smooth_head] = rest;
    gp->smooth_dt[gp->smooth_head] = sdt;
    if (gp->smooth_count < DE__GYRO_SMOOTH_CAP)
        gp->smooth_count++;

    de_vec2 avg = rest;
    const f32 window = gp->settings.smoothing_time;
    if (window > 0.0f) {
        f32 acc = 0.0f;
        de_vec2 sum = {0.0f, 0.0f};
        u32 idx = gp->smooth_head;
        for (u32 k = 0; k < gp->smooth_count && acc < window; ++k) {
            f32 w = gp->smooth_dt[idx];
            if (acc + w > window)
                w = window - acc;
            sum.x += gp->smooth_v[idx].x * w;
            sum.y += gp->smooth_v[idx].y * w;
            acc += w;
            idx = (idx + DE__GYRO_SMOOTH_CAP - 1u) % DE__GYRO_SMOOTH_CAP;
        }
        if (acc > 0.0f)
            avg = (de_vec2){sum.x / acc, sum.y / acc};
    }
    return (de_vec2){v.x * direct + avg.x, v.y * direct + avg.y};
}

static de_vec2 tighten(const gyro_pad *gp, de_vec2 v) {
    const f32 t = gp->settings.tightening;
    if (!(t > 0.0f))
        return v;
    const f32 m = len2(v);
    if (m >= t)
        return v;
    const f32 k = m / t;
    return (de_vec2){v.x * k, v.y * k};
}

static void aim_add(gyro_pad *gp, de_vec3 w, f32 sdt) {
    de_vec2 v = aim_rate(gp, w);
    v = smooth(gp, v, sdt);
    v = tighten(gp, v);
    const de_gyro_settings *s = &gp->settings;
    v.x *= s->invert_x ? -s->sensitivity.x : s->sensitivity.x;
    v.y *= s->invert_y ? -s->sensitivity.y : s->sensitivity.y;
    gp->aim.x += v.x * sdt;
    gp->aim.y += v.y * sdt;
}

/** True while ratchet action `a` is held in the frame view (the last action evaluation). */
static bool ratchet_action_held(const de_input *in, de_action a) {
    de__action_state st;
    return DE_HANDLE_VALID(a) && de__action_frame_state_get(in, a, &st) && st.held;
}

/** True when the ratchet keeps the aim at zero. A button that went down and up within the frame
 * counts as held. The action is read from the frame view: in de__gyro_update that is the previous
 * frame's evaluation, in the aim query the current one. */
static bool ratchet_holds(const de_input *in, const gyro_pad *gp, const de__input_pad *pad) {
    const u32 mask = gp->settings.ratchet_buttons;
    const de_action act = gp->settings.ratchet_action;
    if (mask == 0 && !DE_HANDLE_VALID(act))
        return false;
    const bool held = ((pad->buttons | pad->pressed) & mask) != 0 || ratchet_action_held(in, act);
    return gp->settings.ratchet_mode == DE_GYRO_RATCHET_HOLD_TO_AIM ? !held : held;
}

/* ---------- Samples ---------- */

/** Interval since the previous integrated sample, seconds: device clock first, then host time. */
static f32 sample_dt(gyro_pad *gp, const de__input_event *ev) {
    f32 dt = 0.0f;
    if (gp->have_prev) {
        const u64 s = ev->sensor.sensor_time_ns;
        if (s != 0 && gp->prev_sensor_ns != 0 && s > gp->prev_sensor_ns)
            dt = (f32)((f64)(s - gp->prev_sensor_ns) * 1e-9);
        else if (ev->time_ns > gp->prev_time_ns)
            dt = (f32)((f64)(ev->time_ns - gp->prev_time_ns) * 1e-9);
        if (dt > DE__GYRO_MAX_SAMPLE_DT)
            dt = DE__GYRO_MAX_SAMPLE_DT;
    }
    gp->have_prev = true;
    gp->prev_sensor_ns = ev->sensor.sensor_time_ns;
    gp->prev_time_ns = ev->time_ns;
    return dt;
}

static void on_accel(gyro_pad *gp, de_vec3 a) {
    if (!gp->have_grav) {
        gp->grav = a;
        gp->have_grav = true;
    } else {
        /* Low-pass: gravity changes slowly, hand motion adds quick spikes. */
        gp->grav.x += (a.x - gp->grav.x) * 0.1f;
        gp->grav.y += (a.y - gp->grav.y) * 0.1f;
        gp->grav.z += (a.z - gp->grav.z) * 0.1f;
    }
    if (gp->settings.auto_calibrate && !gp->calibrating)
        still_accel(gp, a);
}

static void on_gyro(gyro_pad *gp, const de__input_event *ev, bool hold) {
    const de_vec3 w = {ev->sensor.data[0], ev->sensor.data[1], ev->sensor.data[2]};
    const f32 sdt = sample_dt(gp, ev);
    if (gp->calibrating) {
        gp->calib_n++;
        welford_add(gp->calib_n, gp->calib_mean, gp->calib_m2, w);
    } else if (gp->settings.auto_calibrate) {
        still_gyro(gp, w, sdt);
    }
    gp->aim_valid = true;
    if (!hold && sdt > 0.0f)
        aim_add(gp, sub_bias(w, gp->bias), sdt);
}

/* ---------- Hooks ---------- */

de_result de__gyro_init(de_input *in) {
    struct de__gyro *g = (struct de__gyro *)DE_ALLOC(&in->alloc, sizeof *g,
                                                     DE_ALIGNOF(struct de__gyro), in->mem_tag);
    if (g == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "gyro state: %u bytes", (unsigned)sizeof *g);
    memset(g, 0, sizeof *g);
    g->defaults = de_gyro_settings_default();
    in->gyro = g;
    return DE_OK;
}

void de__gyro_shutdown(de_input *in) {
    if (in->gyro == NULL)
        return;
    DE_FREE(&in->alloc, in->gyro, sizeof *in->gyro);
    in->gyro = NULL;
}

void de__gyro_update(de_input *in, f32 dt) {
    if (in == NULL || in->gyro == NULL)
        return;
    if (!(dt > 0.0f))
        dt = 0.0f;
    for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i) {
        de__input_pad *pad = &in->pads[i];
        if (pad->device == 0)
            continue;
        gyro_pad *gp = pad_state(in, i);
        const bool hold = ratchet_holds(in, gp, pad);
        gp->aim = (de_vec2){0.0f, 0.0f};

        for (u32 e = 0; e < in->frame_event_count; ++e) {
            const de__input_event *ev = &in->frame_events[e];
            if (ev->kind != DE_RAW_PAD_SENSOR || ev->device != pad->device)
                continue;
            const u32 sensor = ev->sensor.sensor;
            if (is_accel(sensor)) {
                on_accel(gp, (de_vec3){ev->sensor.data[0], ev->sensor.data[1], ev->sensor.data[2]});
            } else if (is_gyro(sensor)) {
                if (gp->sensor == 0)
                    gp->sensor = sensor;
                if (sensor == gp->sensor)
                    on_gyro(gp, ev, hold);
            }
        }

        if (gp->calibrating) {
            gp->calib_elapsed += dt;
            if (gp->calib_elapsed + 1e-4f >= gp->calib_seconds)
                calib_finish(gp);
        }
        if (hold) {
            /* Exactly zero, and nothing from before the hold leaks out after it. */
            gp->aim = (de_vec2){0.0f, 0.0f};
            smooth_clear(gp);
        }
        pad->gyro = pad->has_gyro ? sub_bias(pad->gyro_raw, gp->bias) : (de_vec3){0.0f, 0.0f, 0.0f};
    }
}

/* ---------- Public API ---------- */

void de_pad_gyro_calibrate(de_input *in, de_gamepad p, f32 seconds) {
    if (in == NULL || in->gyro == NULL)
        return;
    const u32 slot = slot_of(in, p);
    if (slot == DE_INPUT_MAX_PADS)
        return;
    gyro_pad *gp = pad_state(in, slot);
    if (!(seconds > 0.0f)) { /* also NaN: clear */
        gp->calibrating = false;
        gp->bias = (de_vec3){0.0f, 0.0f, 0.0f};
        gp->calib = DE_GYRO_CALIB_NONE;
        still_reset(gp);
        de__input_pad *pad = &in->pads[slot];
        pad->gyro = pad->has_gyro ? pad->gyro_raw : (de_vec3){0.0f, 0.0f, 0.0f};
        return;
    }
    gp->calibrating = true;
    gp->calib_seconds = clampf(seconds, 0.1f, 10.0f);
    gp->calib_elapsed = 0.0f;
    gp->calib_n = 0;
    memset(gp->calib_mean, 0, sizeof gp->calib_mean);
    memset(gp->calib_m2, 0, sizeof gp->calib_m2);
    gp->calib = DE_GYRO_CALIB_RUNNING;
}

de_gyro_calibration de_pad_gyro_calibration_state(const de_input *in, de_gamepad p, de_vec3 *bias) {
    if (bias != NULL)
        *bias = (de_vec3){0.0f, 0.0f, 0.0f};
    if (in == NULL || in->gyro == NULL)
        return DE_GYRO_CALIB_NONE;
    const u32 slot = slot_of(in, p);
    if (slot == DE_INPUT_MAX_PADS)
        return DE_GYRO_CALIB_NONE;
    const gyro_pad *gp = &in->gyro->pads[slot];
    if (gp->device != in->pads[slot].device || gp->generation != in->pads[slot].generation)
        return DE_GYRO_CALIB_NONE; /* connected this frame: no state yet */
    if (bias != NULL)
        *bias = gp->bias;
    return gp->calib;
}

void de_pad_gyro_configure(de_input *in, de_gamepad p, const de_gyro_settings *s) {
    if (in == NULL || in->gyro == NULL)
        return;
    const de_gyro_settings v = sanitize(s);
    if (!DE_HANDLE_VALID(p)) {
        in->gyro->defaults = v;
        for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i)
            if (in->pads[i].device != 0)
                pad_state(in, i)->settings = v;
        return;
    }
    const u32 slot = slot_of(in, p);
    if (slot == DE_INPUT_MAX_PADS)
        return;
    gyro_pad *gp = pad_state(in, slot);
    gp->settings = v;
    smooth_clear(gp);
    if (!v.auto_calibrate)
        still_reset(gp);
}

bool de_pad_gyro_settings(const de_input *in, de_gamepad p, de_gyro_settings *out) {
    de_gyro_settings r = de_gyro_settings_default();
    bool ok = false;
    if (in != NULL && in->gyro != NULL) {
        if (!DE_HANDLE_VALID(p)) {
            r = in->gyro->defaults;
            ok = true;
        } else {
            const u32 slot = slot_of(in, p);
            if (slot != DE_INPUT_MAX_PADS) {
                const gyro_pad *gp = &in->gyro->pads[slot];
                const bool same = gp->device == in->pads[slot].device &&
                                  gp->generation == in->pads[slot].generation;
                r = same ? gp->settings : in->gyro->defaults;
                ok = true;
            }
        }
    }
    if (out != NULL)
        *out = r;
    return ok;
}

bool de__gyro_aim_slot(const de_input *in, u32 slot, de_vec2 *delta) {
    bool ok = false;
    de_vec2 d = {0.0f, 0.0f};
    if (in != NULL && in->gyro != NULL && slot < DE_INPUT_MAX_PADS && in->pads[slot].device != 0) {
        const gyro_pad *gp = &in->gyro->pads[slot];
        if (gp->device == in->pads[slot].device && gp->generation == in->pads[slot].generation &&
            gp->aim_valid) {
            /* The ratchet again, with this frame's action evaluation: an action ratchet gives an
             * exact zero in the frame it is held, not one frame later. */
            if (!ratchet_holds(in, gp, &in->pads[slot]))
                d = gp->aim;
            ok = true;
        }
    }
    if (delta != NULL)
        *delta = d;
    return ok;
}

bool de_pad_gyro_aim(const de_input *in, de_gamepad p, de_vec2 *delta) {
    const u32 slot = in != NULL ? slot_of(in, p) : DE_INPUT_MAX_PADS;
    return de__gyro_aim_slot(in, slot, delta);
}
