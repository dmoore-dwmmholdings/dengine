/* haptics.c: rumble, trigger rumble and DualSense adaptive triggers (DENG-87). SPEC §15.
 *
 * The engine owns effect durations: each call stores the requested effect per pad slot, sends it
 * at once through the device sink, and de__haptics_update() counts it down in frame time and
 * stops it at the end. The device is asked to run an effect a little longer than the engine
 * will (DE__HAPTICS_DEVICE_MARGIN_MS), so the engine's stop is what ends it, and a stalled
 * process still stops rumbling. Strengths are scaled by the cvars input.haptics_scale and
 * input.haptics_enabled; the update re-sends running effects when the scale changes.
 *
 * Per pad slot, the state remembers what the device runs now ("sent"), so nothing is re-sent
 * while nothing changes. A slot whose pad changed (other device or generation) starts clean. */
#include "haptics_internal.h"

#include <dengine/de_debug.h>
#include <dengine/de_memory.h>

#include <string.h>

/* Log-once bits per pad (hap_pad.logged). */
enum {
    HAP_LOG_NO_RUMBLE = 1u << 0,
    HAP_LOG_NO_TRIGGER_RUMBLE = 1u << 1,
    HAP_LOG_NO_ADAPTIVE = 1u << 2,
    HAP_LOG_BAD_EFFECT = 1u << 3,
    HAP_LOG_SINK_RUMBLE = 1u << 4,
    HAP_LOG_SINK_TRIGGER_RUMBLE = 1u << 5,
    HAP_LOG_SINK_EFFECT = 1u << 6
};

/** One pair of motors (the rumble motors, or the trigger motors). */
typedef struct hap_motor {
    f32 a, b;           /* requested strengths, [0, 1] */
    f32 remaining;      /* seconds of frame time left */
    bool active;        /* a requested effect is running */
    f32 sent_a, sent_b; /* what the device runs now (scaled); 0, 0 = stopped */
} hap_motor;

typedef struct hap_pad {
    u32 device, generation; /* the pad this state belongs to */
    hap_motor rumble, trigger;
    /* Adaptive triggers, [0] left and [1] right: the requested effect (valid once `fx_set`), and
     * the block the device has (valid once `fx_sent_valid`). */
    de_trigger_effect fx[2];
    bool fx_set[2];
    u8 fx_sent[2][DE__DS5_TRIGGER_BYTES];
    bool fx_sent_valid[2];
    u32 logged;
} hap_pad;

struct de__haptics {
    de__haptics_sink sink;
    de_cvar cv_scale, cv_enabled;
    hap_pad pads[DE_INPUT_MAX_PADS];
};

/* ---------- Platform sink ---------- */

static de_result platform_rumble(void *user, u32 device, f32 low, f32 high, u32 ms) {
    (void)user;
    return de_platform_pad_rumble(device, low, high, ms);
}

static de_result platform_trigger_rumble(void *user, u32 device, f32 left, f32 right, u32 ms) {
    (void)user;
    return de_platform_pad_trigger_rumble(device, left, right, ms);
}

static de_result platform_send_effect(void *user, u32 device, const void *data, u32 size) {
    (void)user;
    return de_platform_pad_send_effect(device, data, size);
}

static de__haptics_sink platform_sink(void) {
    de__haptics_sink s;
    s.rumble = platform_rumble;
    s.trigger_rumble = platform_trigger_rumble;
    s.send_effect = platform_send_effect;
    s.user = NULL;
    return s;
}

/* ---------- Helpers ---------- */

static f32 clamp01(f32 v) {
    if (!(v > 0.0f)) /* also NaN */
        return 0.0f;
    return v < 1.0f ? v : 1.0f;
}

/** Milliseconds the device is asked to run an effect with `remaining` seconds left. */
static u32 device_ms(f32 remaining) {
    const f32 max_ms = 65535.0f - (f32)DE__HAPTICS_DEVICE_MARGIN_MS; /* SDL caps at 0xFFFF */
    f32 ms = remaining * 1000.0f;
    if (!(ms > 0.0f))
        ms = 0.0f;
    if (ms > max_ms)
        ms = max_ms;
    u32 whole = (u32)ms;
    if ((f32)whole < ms)
        whole++; /* round up */
    return whole + DE__HAPTICS_DEVICE_MARGIN_MS;
}

/** The haptics state of slot `slot`, reset when another pad now uses the slot. */
static hap_pad *pad_state(de_input *in, u32 slot) {
    const de__input_pad *pad = &in->pads[slot];
    hap_pad *hp = &in->haptics->pads[slot];
    if (hp->device != pad->device || hp->generation != pad->generation) {
        memset(hp, 0, sizeof *hp);
        hp->device = pad->device;
        hp->generation = pad->generation;
    }
    return hp;
}

/** Slot of live handle `p`, or DE_INPUT_MAX_PADS. */
static u32 slot_of(de_input *in, de_gamepad p) {
    const de__input_pad *pad = de__input_pad_get(in, p);
    return pad != NULL ? (u32)(pad - in->pads) : DE_INPUT_MAX_PADS;
}

static void log_once(de_input *in, u32 slot, hap_pad *hp, u32 bit, bool warn, const char *what) {
    if (hp->logged & bit)
        return;
    hp->logged |= bit;
    const de__input_pad *pad = &in->pads[slot];
    if (warn) {
        DE_WARN(in->log, "gamepad %u (%s): %s", (unsigned)pad->device, pad->info.name, what);
    } else {
#if !defined(DE_SHIPPING)
        /* A missing capability is normal (an Xbox pad has no adaptive triggers): Dev info only. */
        DE_INFO(in->log, "gamepad %u (%s): %s", (unsigned)pad->device, pad->info.name, what);
#else
        (void)pad;
        (void)what;
#endif
    }
}

/** Logs a failed sink call once per pad and kind, and clears the error. */
static void sink_result(de_input *in, u32 slot, hap_pad *hp, de_result r, u32 bit,
                        const char *what) {
    if (r == DE_OK)
        return;
    const de_error *e = de_error_last();
    if (!(hp->logged & bit)) {
        hp->logged |= bit;
        DE_WARN(in->log, "gamepad %u (%s): %s failed: %s", (unsigned)in->pads[slot].device,
                in->pads[slot].info.name, what, e != NULL ? e->message : "?");
    }
    de_error_clear();
}

f32 de__haptics_effective_scale(const de_input *in) {
    if (in == NULL || in->haptics == NULL)
        return 1.0f;
    const struct de__haptics *h = in->haptics;
    /* A cvar that could not be registered keeps the default (on, full strength). */
    if (DE_HANDLE_VALID(h->cv_enabled) && !de_cvar_bool(h->cv_enabled))
        return 0.0f;
    return DE_HANDLE_VALID(h->cv_scale) ? clamp01((f32)de_cvar_float(h->cv_scale)) : 1.0f;
}

/* ---------- Motors ---------- */

/**
 * Brings the device in line with motor `m`: sends the scaled strengths when they differ from
 * what the device runs, or always when `force` (a new call: refreshes the device timer). Sends a
 * stop when the effect ended or scales to zero while the device still runs something.
 */
static void motor_apply(de_input *in, u32 slot, hap_pad *hp, hap_motor *m, bool trigger, f32 scale,
                        bool force) {
    const f32 ta = m->active ? m->a * scale : 0.0f;
    const f32 tb = m->active ? m->b * scale : 0.0f;
    const bool on = ta > 0.0f || tb > 0.0f;
    const bool running = m->sent_a > 0.0f || m->sent_b > 0.0f;
    if (!on && !running)
        return; /* nothing to run, nothing running */
    if (!force && ta == m->sent_a && tb == m->sent_b)
        return;
    const u32 ms = on ? device_ms(m->remaining) : 0u;
    const de__haptics_sink *s = &in->haptics->sink;
    const u32 dev = in->pads[slot].device;
    if (trigger)
        sink_result(in, slot, hp, s->trigger_rumble(s->user, dev, ta, tb, ms),
                    HAP_LOG_SINK_TRIGGER_RUMBLE, "trigger rumble");
    else
        sink_result(in, slot, hp, s->rumble(s->user, dev, ta, tb, ms), HAP_LOG_SINK_RUMBLE,
                    "rumble");
    m->sent_a = ta;
    m->sent_b = tb;
}

static void motor_start(de_input *in, de_gamepad p, f32 a, f32 b, f32 seconds, bool trigger) {
    if (in == NULL)
        return;
    const u32 slot = slot_of(in, p);
    if (slot == DE_INPUT_MAX_PADS)
        return;
    hap_pad *hp = pad_state(in, slot);
    const u32 cap = trigger ? DE_PAD_CAP_TRIGGER_RUMBLE : DE_PAD_CAP_RUMBLE;
    if (!(in->pads[slot].info.caps & cap)) {
        log_once(in, slot, hp, trigger ? HAP_LOG_NO_TRIGGER_RUMBLE : HAP_LOG_NO_RUMBLE, false,
                 trigger ? "no trigger rumble; de_pad_trigger_rumble ignored"
                         : "no rumble; de_pad_rumble ignored");
        return;
    }
    hap_motor *m = trigger ? &hp->trigger : &hp->rumble;
    if (seconds > DE_PAD_RUMBLE_MAX_SECONDS)
        seconds = DE_PAD_RUMBLE_MAX_SECONDS;
    m->a = clamp01(a);
    m->b = clamp01(b);
    m->active = seconds > DE__HAPTICS_END_EPSILON; /* NaN and <= 0 stop */
    m->remaining = m->active ? seconds : 0.0f;
    motor_apply(in, slot, hp, m, trigger, de__haptics_effective_scale(in), true);
}

void de_pad_rumble(de_input *in, de_gamepad p, f32 low, f32 high, f32 seconds) {
    motor_start(in, p, low, high, seconds, false);
}

void de_pad_trigger_rumble(de_input *in, de_gamepad p, f32 left, f32 right, f32 seconds) {
    motor_start(in, p, left, right, seconds, true);
}

/* ---------- Adaptive triggers ---------- */

static void put_u16(u8 *b, u32 v) {
    b[0] = (u8)(v & 0xFFu);
    b[1] = (u8)((v >> 8) & 0xFFu);
}

static void put_u32(u8 *b, u32 v) {
    put_u16(b, v & 0xFFFFu);
    put_u16(b + 2, v >> 16);
}

/** Zones `from`..9 active, each with the 3-bit value `v`: the zone masks of the feedback and
 * vibration effects. */
static void zone_masks(u32 from, u32 v, u32 *active, u32 *values) {
    *active = 0;
    *values = 0;
    for (u32 i = from; i < 10u; ++i) {
        *active |= 1u << i;
        *values |= (v & 7u) << (3u * i);
    }
}

bool de__haptics_encode_trigger(const de_trigger_effect *fx, f32 scale,
                                u8 out[DE__DS5_TRIGGER_BYTES]) {
    de_trigger_effect off;
    memset(&off, 0, sizeof off);
    if (fx == NULL)
        fx = &off;
    switch (fx->mode) {
    case DE_TRIGGER_OFF:
        break;
    case DE_TRIGGER_FEEDBACK:
        if (fx->start > 9u || fx->strength > 8u)
            return false;
        break;
    case DE_TRIGGER_WEAPON:
        if (fx->start < 2u || fx->start > 7u || fx->end <= fx->start || fx->end > 8u ||
            fx->strength > 8u)
            return false;
        break;
    case DE_TRIGGER_VIBRATION:
        if (fx->start > 9u || fx->strength > 8u)
            return false;
        break;
    default:
        return false;
    }

    /* Scaled strength 0..8, rounded; full scale keeps the exact value. */
    u32 strength = fx->strength;
    const f32 s = clamp01(scale);
    if (s < 1.0f)
        strength = (u32)((f32)strength * s + 0.5f);

    u8 b[DE__DS5_TRIGGER_BYTES];
    memset(b, 0, sizeof b);
    u32 active = 0, values = 0;
    const bool off_now = fx->mode == DE_TRIGGER_OFF || strength == 0u ||
                         (fx->mode == DE_TRIGGER_VIBRATION && fx->frequency == 0u);
    if (off_now) {
        b[0] = (u8)DE__DS5_EFFECT_OFF;
    } else if (fx->mode == DE_TRIGGER_FEEDBACK) {
        zone_masks(fx->start, strength - 1u, &active, &values);
        b[0] = (u8)DE__DS5_EFFECT_FEEDBACK;
        put_u16(b + 1, active);
        put_u32(b + 3, values);
    } else if (fx->mode == DE_TRIGGER_WEAPON) {
        const u32 zones = (1u << fx->start) | (1u << fx->end);
        b[0] = (u8)DE__DS5_EFFECT_WEAPON;
        put_u16(b + 1, zones);
        b[3] = (u8)(strength - 1u);
    } else { /* vibration */
        zone_masks(fx->start, strength - 1u, &active, &values);
        b[0] = (u8)DE__DS5_EFFECT_VIBRATION;
        put_u16(b + 1, active);
        put_u32(b + 3, values);
        b[9] = fx->frequency;
    }
    memcpy(out, b, sizeof b);
    return true;
}

/** Sends trigger `side` (0 left, 1 right) when its scaled block differs from what the device has,
 * or always when `force`. Triggers never set through de_input are left alone. */
static void trigger_apply(de_input *in, u32 slot, hap_pad *hp, u32 side, f32 scale, bool force) {
    if (!hp->fx_set[side])
        return;
    u8 block[DE__DS5_TRIGGER_BYTES];
    if (!de__haptics_encode_trigger(&hp->fx[side], scale, block))
        return; /* not reached: effects are checked when they are set */
    if (!force && hp->fx_sent_valid[side] && memcmp(block, hp->fx_sent[side], sizeof block) == 0)
        return;
    u8 packet[DE__DS5_EFFECT_PACKET_BYTES];
    memset(packet, 0, sizeof packet);
    packet[DE__DS5_OFF_ENABLE_BITS1] =
        (u8)(side == 1u ? DE__DS5_ENABLE_RIGHT_TRIGGER : DE__DS5_ENABLE_LEFT_TRIGGER);
    memcpy(packet + (side == 1u ? DE__DS5_OFF_RIGHT_TRIGGER : DE__DS5_OFF_LEFT_TRIGGER), block,
           sizeof block);
    const de__haptics_sink *s = &in->haptics->sink;
    sink_result(in, slot, hp,
                s->send_effect(s->user, in->pads[slot].device, packet, (u32)sizeof packet),
                HAP_LOG_SINK_EFFECT, "adaptive trigger effect");
    memcpy(hp->fx_sent[side], block, sizeof block);
    hp->fx_sent_valid[side] = true;
}

void de_pad_adaptive_trigger(de_input *in, de_gamepad p, bool right, const de_trigger_effect *fx) {
    if (in == NULL)
        return;
    const u32 slot = slot_of(in, p);
    if (slot == DE_INPUT_MAX_PADS)
        return;
    hap_pad *hp = pad_state(in, slot);
    if (!(in->pads[slot].info.caps & DE_PAD_CAP_ADAPTIVE_TRIGGERS)) {
        log_once(in, slot, hp, HAP_LOG_NO_ADAPTIVE, false,
                 "no adaptive triggers; de_pad_adaptive_trigger ignored");
        return;
    }
    u8 check[DE__DS5_TRIGGER_BYTES];
    if (!de__haptics_encode_trigger(fx, 1.0f, check)) {
        log_once(in, slot, hp, HAP_LOG_BAD_EFFECT, true,
                 "adaptive trigger effect with a field out of range ignored (see de_trigger_mode)");
        return;
    }
    const u32 side = right ? 1u : 0u;
    memset(&hp->fx[side], 0, sizeof hp->fx[side]);
    if (fx != NULL)
        hp->fx[side] = *fx;
    hp->fx_set[side] = true;
    trigger_apply(in, slot, hp, side, de__haptics_effective_scale(in), true);
}

/* ---------- Hooks ---------- */

de_result de__haptics_init(de_input *in) {
    struct de__haptics *h = (struct de__haptics *)DE_ALLOC(
        &in->alloc, sizeof *h, DE_ALIGNOF(struct de__haptics), in->mem_tag);
    if (h == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "haptics state: %u bytes", (unsigned)sizeof *h);
    memset(h, 0, sizeof *h);
    h->sink = platform_sink();

    /* Process-wide cvars: registering again returns the existing ones. A failure keeps the
     * default behaviour (haptics on, full strength) rather than failing the input state. */
    de_cvar_desc d;
    memset(&d, 0, sizeof d);
    d.name = "input.haptics_scale";
    d.help = "Strength of rumble, trigger rumble and adaptive triggers, 0 to 1 (accessibility)";
    d.type = DE_CVAR_FLOAT;
    d.flags = DE_CVAR_SAVED;
    d.default_number = 1.0;
    d.min = 0.0;
    d.max = 1.0;
    h->cv_scale = de_cvar_register(&d);
    memset(&d, 0, sizeof d);
    d.name = "input.haptics_enabled";
    d.help = "Rumble, trigger rumble and adaptive triggers on or off (accessibility)";
    d.type = DE_CVAR_BOOL;
    d.flags = DE_CVAR_SAVED;
    d.default_number = 1.0;
    h->cv_enabled = de_cvar_register(&d);
    if (!DE_HANDLE_VALID(h->cv_scale) || !DE_HANDLE_VALID(h->cv_enabled)) {
        const de_error *e = de_error_last();
        DE_WARN(in->log, "haptics cvars not registered (%s); haptics stay on at full strength",
                e != NULL ? e->message : "?");
        de_error_clear();
    }
    in->haptics = h;
    return DE_OK;
}

void de__haptics_shutdown(de_input *in) {
    struct de__haptics *h = in->haptics;
    if (h == NULL)
        return;
    /* Leave no pad rumbling or with a trigger effect after the input state is gone. */
    for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i) {
        if (in->pads[i].device == 0)
            continue;
        hap_pad *hp = pad_state(in, i);
        hp->rumble.active = false;
        hp->trigger.active = false;
        motor_apply(in, i, hp, &hp->rumble, false, 0.0f, false);
        motor_apply(in, i, hp, &hp->trigger, true, 0.0f, false);
        for (u32 side = 0; side < 2u; ++side) {
            if (hp->fx_set[side] && hp->fx_sent_valid[side] &&
                hp->fx_sent[side][0] != (u8)DE__DS5_EFFECT_OFF) {
                memset(&hp->fx[side], 0, sizeof hp->fx[side]);
                trigger_apply(in, i, hp, side, 1.0f, false);
            }
        }
    }
    in->haptics = NULL;
    DE_FREE(&in->alloc, h, sizeof *h);
}

void de__haptics_update(de_input *in, f32 dt) {
    if (in == NULL || in->haptics == NULL)
        return;
    if (!(dt > 0.0f))
        dt = 0.0f;
    const f32 scale = de__haptics_effective_scale(in);
    for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i) {
        if (in->pads[i].device == 0)
            continue;
        hap_pad *hp = pad_state(in, i);
        hap_motor *motors[2] = {&hp->rumble, &hp->trigger};
        for (u32 k = 0; k < 2u; ++k) {
            hap_motor *m = motors[k];
            if (m->active) {
                m->remaining -= dt;
                if (m->remaining <= DE__HAPTICS_END_EPSILON) {
                    m->active = false;
                    m->remaining = 0.0f;
                }
            }
            motor_apply(in, i, hp, m, k == 1u, scale, false);
        }
        trigger_apply(in, i, hp, 0u, scale, false);
        trigger_apply(in, i, hp, 1u, scale, false);
    }
}

void de__haptics_set_sink(de_input *in, const de__haptics_sink *sink) {
    if (in == NULL || in->haptics == NULL)
        return;
    in->haptics->sink = sink != NULL ? *sink : platform_sink();
}
