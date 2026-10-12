/* input_state.c: applying raw events to the input state, and the state queries (DENG-83). */
#include "input_internal.h"

#include <string.h>

/* ---------- Helpers ---------- */

static bool bit_get(const u64 *set, u32 i) {
    return (set[i >> 6] >> (i & 63u)) & 1u;
}
static void bit_set(u64 *set, u32 i) {
    set[i >> 6] |= (u64)1 << (i & 63u);
}
static void bit_clear(u64 *set, u32 i) {
    set[i >> 6] &= ~((u64)1 << (i & 63u));
}

static f32 clampf(f32 v, f32 lo, f32 hi) {
    /* NaN becomes 0 (inside both ranges): a broken device must not poison the state. */
    if (v != v)
        return 0.0f;
    if (v < lo)
        return lo;
    return v > hi ? hi : v;
}

static f32 absf(f32 v) {
    return v < 0.0f ? -v : v;
}

static void set_kbm_active(de_input *in) {
    in->last_device = DE_INPUT_DEVICE_KEYBOARD_MOUSE;
    in->last_pad = (de_gamepad){0};
}

static void note_window(de_input *in, de_window w) {
    if (DE_HANDLE_VALID(w) && w.id != in->focus_window.id) {
        in->focus_window = w;
        if (in->relative && !DE_HANDLE_VALID(in->window))
            in->relative_dirty = true;
    }
}

/* ---------- Pads ---------- */

de_gamepad de__input_pad_handle(const de_input *in, u32 slot) {
    de_gamepad g;
    g.id = (in->pads[slot].generation << DE__INPUT_PAD_SLOT_BITS) | (slot + 1u);
    return g;
}

const de__input_pad *de__input_pad_get_const(const de_input *in, de_gamepad p) {
    if (in == NULL || p.id == 0)
        return NULL;
    const u32 slot = (p.id & ((1u << DE__INPUT_PAD_SLOT_BITS) - 1u)) - 1u;
    if (slot >= DE_INPUT_MAX_PADS)
        return NULL;
    const de__input_pad *pad = &in->pads[slot];
    if (pad->device == 0 || pad->generation != (p.id >> DE__INPUT_PAD_SLOT_BITS))
        return NULL;
    return pad;
}

de__input_pad *de__input_pad_get(de_input *in, de_gamepad p) {
    return (de__input_pad *)de__input_pad_get_const(in, p);
}

/** Slot index of live pad `device`, or DE_INPUT_MAX_PADS. */
static u32 pad_slot_of(const de_input *in, u32 device) {
    if (device == 0)
        return DE_INPUT_MAX_PADS;
    for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i)
        if (in->pads[i].device == device)
            return i;
    return DE_INPUT_MAX_PADS;
}

static u32 map_caps(const de_platform_pad_info *pi) {
    u32 caps = 0;
    if (pi->caps & DE_PLATFORM_PAD_CAP_RUMBLE)
        caps |= DE_PAD_CAP_RUMBLE;
    if (pi->caps & DE_PLATFORM_PAD_CAP_TRIGGER_RUMBLE)
        caps |= DE_PAD_CAP_TRIGGER_RUMBLE;
    if (pi->caps & DE_PLATFORM_PAD_CAP_LED)
        caps |= DE_PAD_CAP_LED;
    if (pi->caps & DE_PLATFORM_PAD_CAP_GYRO)
        caps |= DE_PAD_CAP_GYRO;
    if (pi->caps & DE_PLATFORM_PAD_CAP_ACCEL)
        caps |= DE_PAD_CAP_ACCEL;
    if (pi->caps & DE_PLATFORM_PAD_CAP_TOUCHPAD)
        caps |= DE_PAD_CAP_TOUCHPAD;
    /* The platform has no flag for effect packets; every DualSense has adaptive triggers. */
    if (pi->type == DE_PLATFORM_PAD_PS5)
        caps |= DE_PAD_CAP_ADAPTIVE_TRIGGERS;
    return caps;
}

/** Metadata of a connecting pad: injected info first, then the platform. */
static void pad_lookup_info(de_input *in, u32 device, de_platform_pad_info *pi) {
    for (u32 i = 0; i < in->inject_pad_info_count; ++i) {
        if (in->inject_pad_info[i].id == device) {
            *pi = in->inject_pad_info[i];
            in->inject_pad_info[i] = in->inject_pad_info[--in->inject_pad_info_count];
            return;
        }
    }
    if (de_platform_pad_info_get(device, pi) != DE_OK) {
        de_error_clear();
        memset(pi, 0, sizeof *pi);
        pi->id = device;
        pi->type = DE_PLATFORM_PAD_GENERIC;
        pi->player_index = -1;
        memcpy(pi->name, "Gamepad", sizeof "Gamepad");
    }
}

static void pad_added(de_input *in, u32 device) {
    if (device == 0 || pad_slot_of(in, device) < DE_INPUT_MAX_PADS)
        return; /* duplicate add */
    u32 slot = DE_INPUT_MAX_PADS;
    for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i) {
        if (in->pads[i].device == 0) {
            slot = i;
            break;
        }
    }
    if (slot == DE_INPUT_MAX_PADS) {
        in->pads_ignored++;
        DE_WARN(in->log, "gamepad %u ignored: %d pads are connected already", (unsigned)device,
                DE_INPUT_MAX_PADS);
        return;
    }
    de_platform_pad_info pi;
    pad_lookup_info(in, device, &pi);

    de__input_pad *pad = &in->pads[slot];
    const u32 gen_mask = 0xFFFFFFFFu >> DE__INPUT_PAD_SLOT_BITS;
    u32 gen = (pad->generation + 1u) & gen_mask;
    if (gen == 0)
        gen = 1;
    memset(pad, 0, sizeof *pad);
    pad->device = device;
    pad->generation = gen;
    pad->connect_seq = ++in->pad_seq;
    pad->info.type = (de_pad_type)pi.type;
    pad->info.caps = map_caps(&pi);
    pad->info.vendor = pi.vendor;
    pad->info.product = pi.product;
    pad->info.player_index = pi.player_index;
    memcpy(pad->info.name, pi.name, sizeof pad->info.name);
    pad->info.name[sizeof pad->info.name - 1] = '\0';
    in->pad_order[in->pad_count++] = (u8)slot;

    if (!in->pad_sensors_off) {
        /* Errors (an injected pad, a refusing device) only mean no samples. */
        if ((pad->info.caps & DE_PAD_CAP_GYRO) &&
            de_platform_pad_sensor_enable(device, DE_PLATFORM_SENSOR_GYRO, true) != DE_OK)
            de_error_clear();
        if ((pad->info.caps & DE_PAD_CAP_ACCEL) &&
            de_platform_pad_sensor_enable(device, DE_PLATFORM_SENSOR_ACCEL, true) != DE_OK)
            de_error_clear();
    }
}

static void pad_removed(de_input *in, u32 device) {
    const u32 slot = pad_slot_of(in, device);
    if (slot == DE_INPUT_MAX_PADS)
        return;
    const de_gamepad h = de__input_pad_handle(in, slot);
    if (in->last_device == DE_INPUT_DEVICE_GAMEPAD && in->last_pad.id == h.id) {
        in->last_device = DE_INPUT_DEVICE_NONE;
        in->last_pad = (de_gamepad){0};
    }
    /* Keep the generation: the next pad in this slot gets a new one, so `h` stays stale. */
    const u32 gen = in->pads[slot].generation;
    memset(&in->pads[slot], 0, sizeof in->pads[slot]);
    in->pads[slot].generation = gen;
    u32 j = 0;
    for (u32 i = 0; i < in->pad_count; ++i)
        if (in->pad_order[i] != slot)
            in->pad_order[j++] = in->pad_order[i];
    in->pad_count = j;
}

static void set_pad_active(de_input *in, u32 slot) {
    in->last_device = DE_INPUT_DEVICE_GAMEPAD;
    in->last_pad = de__input_pad_handle(in, slot);
}

static void pad_sensor(de_input *in, u32 slot, const de__input_event *ev) {
    de__input_pad *pad = &in->pads[slot];
    const de_vec3 v = {ev->sensor.data[0], ev->sensor.data[1], ev->sensor.data[2]};
    switch (ev->sensor.sensor) {
    /* Joy-Con pairs report a left and a right sensor; the last sample wins. */
    case DE_PLATFORM_SENSOR_GYRO:
    case DE_PLATFORM_SENSOR_GYRO_L:
    case DE_PLATFORM_SENSOR_GYRO_R:
        pad->gyro_raw = v;
        pad->gyro_time_ns = ev->time_ns;
        pad->gyro_sensor_ns = ev->sensor.sensor_time_ns;
        pad->gyro_samples++;
        pad->has_gyro = true;
        break;
    case DE_PLATFORM_SENSOR_ACCEL:
    case DE_PLATFORM_SENSOR_ACCEL_L:
    case DE_PLATFORM_SENSOR_ACCEL_R:
        pad->accel = v;
        pad->accel_time_ns = ev->time_ns;
        pad->accel_sensor_ns = ev->sensor.sensor_time_ns;
        pad->accel_samples++;
        pad->has_accel = true;
        break;
    default:
        break;
    }
}

/* ---------- Text ---------- */

/** strlen bounded by `cap` (strnlen is POSIX, not C11). */
static size_t str_len_max(const char *s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n] != '\0')
        ++n;
    return n;
}

static void text_append(de_input *in, const char *utf8) {
    const size_t n = str_len_max(utf8, sizeof(((de_raw_text *)0)->utf8));
    if (n == 0)
        return;
    /* The platform splits long text between code points, so dropping a whole chunk keeps the
     * buffer valid UTF-8. */
    if (in->text_len + n > DE_INPUT_TEXT_BYTES) {
        in->text_dropped = true;
        return;
    }
    memcpy(in->text + in->text_len, utf8, n);
    in->text_len += (u32)n;
    in->text[in->text_len] = '\0';
}

static void text_edit(de_input *in, const de_raw_text *t) {
    size_t n = str_len_max(t->utf8, sizeof t->utf8);
    if (n > DE_INPUT_COMPOSITION_BYTES)
        n = DE_INPUT_COMPOSITION_BYTES;
    memcpy(in->composition, t->utf8, n);
    in->composition[n] = '\0';
    in->comp_cursor = t->start;
    in->comp_length = t->length;
}

/* ---------- Applying events ---------- */

void de__input_clear_frame(de_input *in) {
    memset(in->keys_pressed, 0, sizeof in->keys_pressed);
    memset(in->keys_released, 0, sizeof in->keys_released);
    in->mouse_pressed = 0;
    in->mouse_released = 0;
    in->mouse_delta = (de_vec2){0.0f, 0.0f};
    in->mouse_wheel = (de_vec2){0.0f, 0.0f};
    for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i) {
        de__input_pad *pad = &in->pads[i];
        pad->pressed = 0;
        pad->released = 0;
        pad->gyro_samples = 0;
        pad->accel_samples = 0;
    }
    in->text_len = 0;
    in->text[0] = '\0';
    in->text_dropped = false;
    in->frame_event_count = 0;
    in->frame_events_dropped = 0;
}

static void key_event(de_input *in, const de_raw_key *k) {
    in->key_mods = k->mods;
    if (k->repeat || k->code == 0 || k->code >= DE_KEY_COUNT_)
        return; /* repeats change nothing; unknown codes are only listed */
    const u32 c = k->code;
    if (k->down) {
        if (!bit_get(in->keys_down, c)) {
            bit_set(in->keys_down, c);
            bit_set(in->keys_pressed, c);
        }
        set_kbm_active(in);
    } else if (bit_get(in->keys_down, c)) {
        bit_clear(in->keys_down, c);
        bit_set(in->keys_released, c);
    }
}

static void mouse_button_event(de_input *in, const de_raw_mouse_button *b) {
    if (!in->relative)
        in->mouse_pos = (de_vec2){b->x, b->y};
    if (b->button >= DE_MOUSE_BUTTON_COUNT_)
        return;
    const u8 bit = (u8)(1u << b->button);
    if (b->down) {
        if (!(in->mouse_down & bit)) {
            in->mouse_down |= bit;
            in->mouse_pressed |= bit;
        }
        set_kbm_active(in);
    } else if (in->mouse_down & bit) {
        in->mouse_down &= (u8)~bit;
        in->mouse_released |= bit;
    }
}

static void pad_button_event(de_input *in, u32 slot, const de_raw_pad_button *b) {
    if (b->button >= DE_PAD_BUTTON_COUNT_)
        return; /* MISC2..MISC6: listed in frame_events only */
    de__input_pad *pad = &in->pads[slot];
    const u32 bit = 1u << b->button;
    if (b->down) {
        if (!(pad->buttons & bit)) {
            pad->buttons |= bit;
            pad->pressed |= bit;
        }
        set_pad_active(in, slot);
    } else if (pad->buttons & bit) {
        pad->buttons &= ~bit;
        pad->released |= bit;
    }
}

static void pad_axis_event(de_input *in, u32 slot, const de_raw_pad_axis *a) {
    if (a->axis >= DE_PAD_AXIS_COUNT_)
        return;
    f32 v;
    if (a->axis == DE_AXIS_LEFT_TRIGGER || a->axis == DE_AXIS_RIGHT_TRIGGER) {
        v = clampf(a->value, 0.0f, 1.0f);
    } else {
        v = clampf(a->value, -1.0f, 1.0f);
        /* The platform reports +y down (as the device does); the engine uses +y up. */
        if (a->axis == DE_AXIS_LEFT_Y || a->axis == DE_AXIS_RIGHT_Y)
            v = -v;
    }
    in->pads[slot].axes[a->axis] = v;
    if (absf(v) >= DE__INPUT_ACTIVITY_AXIS)
        set_pad_active(in, slot);
}

void de__input_apply(de_input *in, const de__input_event *ev) {
    if (in->frame_event_count < DE__INPUT_FRAME_EVENTS) {
        in->frame_events[in->frame_event_count++] = *ev;
    } else {
        in->frame_events_dropped++;
        in->events_dropped_total++;
    }

    switch (ev->kind) {
    case DE_RAW_KEY:
        note_window(in, ev->window);
        key_event(in, &ev->key);
        break;
    case DE_RAW_TEXT:
        text_append(in, ev->text.utf8);
        break;
    case DE_RAW_TEXT_EDIT:
        text_edit(in, &ev->text);
        break;
    case DE_RAW_MOUSE_MOVE:
        note_window(in, ev->window);
        if (!in->relative)
            in->mouse_pos = (de_vec2){ev->mouse_move.x, ev->mouse_move.y};
        in->mouse_delta.x += ev->mouse_move.dx;
        in->mouse_delta.y += ev->mouse_move.dy;
        if (ev->mouse_move.dx != 0.0f || ev->mouse_move.dy != 0.0f)
            set_kbm_active(in);
        break;
    case DE_RAW_MOUSE_BUTTON:
        note_window(in, ev->window);
        mouse_button_event(in, &ev->mouse_button);
        break;
    case DE_RAW_MOUSE_WHEEL:
        note_window(in, ev->window);
        if (!in->relative)
            in->mouse_pos = (de_vec2){ev->mouse_wheel.x, ev->mouse_wheel.y};
        in->mouse_wheel.x += ev->mouse_wheel.dx;
        in->mouse_wheel.y += ev->mouse_wheel.dy;
        if (ev->mouse_wheel.dx != 0.0f || ev->mouse_wheel.dy != 0.0f)
            set_kbm_active(in);
        break;
    case DE_RAW_PAD_ADDED:
        pad_added(in, ev->device);
        break;
    case DE_RAW_PAD_REMOVED:
        pad_removed(in, ev->device);
        break;
    case DE_RAW_PAD_BUTTON:
    case DE_RAW_PAD_AXIS:
    case DE_RAW_PAD_SENSOR: {
        const u32 slot = pad_slot_of(in, ev->device);
        if (slot == DE_INPUT_MAX_PADS)
            break; /* an ignored pad (all slots taken) or an event before its add */
        if (ev->kind == DE_RAW_PAD_BUTTON)
            pad_button_event(in, slot, &ev->pad_button);
        else if (ev->kind == DE_RAW_PAD_AXIS)
            pad_axis_event(in, slot, &ev->pad_axis);
        else
            pad_sensor(in, slot, ev);
        break;
    }
    case DE_RAW_PAD_TOUCHPAD:
    default:
        break; /* listed in frame_events only */
    }
}

void de__input_release_all(de_input *in) {
    for (u32 w = 0; w < DE__INPUT_KEY_WORDS; ++w) {
        in->keys_released[w] |= in->keys_down[w];
        in->keys_down[w] = 0;
    }
    in->mouse_released |= in->mouse_down;
    in->mouse_down = 0;
    for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i) {
        de__input_pad *pad = &in->pads[i];
        pad->released |= pad->buttons;
        pad->buttons = 0;
        memset(pad->axes, 0, sizeof pad->axes);
    }
}

/* ---------- Queries ---------- */

static bool key_ok(const de_input *in, de_key k) {
    return in != NULL && (u32)k > 0 && (u32)k < DE_KEY_COUNT_;
}

bool de_key_down(const de_input *in, de_key k) {
    return key_ok(in, k) && bit_get(in->keys_down, (u32)k);
}
bool de_key_pressed(const de_input *in, de_key k) {
    return key_ok(in, k) && bit_get(in->keys_pressed, (u32)k);
}
bool de_key_released(const de_input *in, de_key k) {
    return key_ok(in, k) && bit_get(in->keys_released, (u32)k);
}

static bool mouse_bit(const de_input *in, de_mouse_button b, u8 set) {
    return in != NULL && (u32)b < DE_MOUSE_BUTTON_COUNT_ && ((set >> (u32)b) & 1u);
}
bool de_mouse_down(const de_input *in, de_mouse_button b) {
    return in != NULL && mouse_bit(in, b, in->mouse_down);
}
bool de_mouse_pressed(const de_input *in, de_mouse_button b) {
    return in != NULL && mouse_bit(in, b, in->mouse_pressed);
}
bool de_mouse_released(const de_input *in, de_mouse_button b) {
    return in != NULL && mouse_bit(in, b, in->mouse_released);
}
de_vec2 de_mouse_position(const de_input *in) {
    return in != NULL ? in->mouse_pos : (de_vec2){0.0f, 0.0f};
}
de_vec2 de_mouse_delta(const de_input *in) {
    return in != NULL ? in->mouse_delta : (de_vec2){0.0f, 0.0f};
}
de_vec2 de_mouse_wheel(const de_input *in) {
    return in != NULL ? in->mouse_wheel : (de_vec2){0.0f, 0.0f};
}

u32 de_gamepad_list(const de_input *in, de_gamepad *out, u32 max) {
    if (in == NULL)
        return 0;
    if (out != NULL)
        for (u32 i = 0; i < in->pad_count && i < max; ++i)
            out[i] = de__input_pad_handle(in, in->pad_order[i]);
    return in->pad_count;
}

bool de_pad_connected(const de_input *in, de_gamepad p) {
    return de__input_pad_get_const(in, p) != NULL;
}

bool de_pad_info(const de_input *in, de_gamepad p, de_gamepad_info *out) {
    if (out == NULL)
        return false;
    const de__input_pad *pad = de__input_pad_get_const(in, p);
    if (pad == NULL) {
        memset(out, 0, sizeof *out);
        return false;
    }
    *out = pad->info;
    return true;
}

static bool pad_bit(const de_input *in, de_gamepad p, de_pad_button b, int which) {
    const de__input_pad *pad = de__input_pad_get_const(in, p);
    if (pad == NULL || (u32)b >= DE_PAD_BUTTON_COUNT_)
        return false;
    const u32 set = which == 0 ? pad->buttons : which == 1 ? pad->pressed : pad->released;
    return (set >> (u32)b) & 1u;
}
bool de_pad_down(const de_input *in, de_gamepad p, de_pad_button b) {
    return pad_bit(in, p, b, 0);
}
bool de_pad_pressed(const de_input *in, de_gamepad p, de_pad_button b) {
    return pad_bit(in, p, b, 1);
}
bool de_pad_released(const de_input *in, de_gamepad p, de_pad_button b) {
    return pad_bit(in, p, b, 2);
}

f32 de_pad_axis_value(const de_input *in, de_gamepad p, de_pad_axis a) {
    const de__input_pad *pad = de__input_pad_get_const(in, p);
    if (pad == NULL || (u32)a >= DE_PAD_AXIS_COUNT_)
        return 0.0f;
    return pad->axes[a];
}

bool de_pad_gyro(const de_input *in, de_gamepad p, de_vec3 *rad_per_sec) {
    const de__input_pad *pad = de__input_pad_get_const(in, p);
    const bool ok = pad != NULL && pad->has_gyro;
    if (rad_per_sec != NULL)
        *rad_per_sec = ok ? pad->gyro : (de_vec3){0.0f, 0.0f, 0.0f};
    return ok;
}

bool de_pad_accel(const de_input *in, de_gamepad p, de_vec3 *m_per_s2) {
    const de__input_pad *pad = de__input_pad_get_const(in, p);
    const bool ok = pad != NULL && pad->has_accel;
    if (m_per_s2 != NULL)
        *m_per_s2 = ok ? pad->accel : (de_vec3){0.0f, 0.0f, 0.0f};
    return ok;
}

de_input_device de_input_last_device(const de_input *in, de_gamepad *pad) {
    if (pad != NULL)
        *pad = (de_gamepad){0};
    if (in == NULL)
        return DE_INPUT_DEVICE_NONE;
    if (pad != NULL && in->last_device == DE_INPUT_DEVICE_GAMEPAD)
        *pad = in->last_pad;
    return in->last_device;
}

void de_text_input_get(const de_input *in, de_text_input_state *out) {
    if (out == NULL)
        return;
    memset(out, 0, sizeof *out);
    if (in == NULL) {
        out->text = "";
        out->composition = "";
        out->composition_cursor = -1;
        out->composition_length = -1;
        return;
    }
    out->text = in->text;
    out->text_bytes = in->text_len;
    out->text_dropped = in->text_dropped;
    out->composition = in->composition;
    out->composition_cursor = in->comp_cursor;
    out->composition_length = in->comp_length;
    out->active = in->text_active;
}
