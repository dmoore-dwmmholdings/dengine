/* input_script.c: the scripted input session (see input_script.h). SPEC §5.7: integer hashing
 * only, and every float is an exact small fraction (n / 1000), so the raw events are the same
 * bytes on every platform. Built with DE_DETERMINISTIC_FLAGS. */
#include "input_script.h"

#include "input/action_internal.h"

#include <string.h>

#define FRAME_DT (1.0f / 60.0f)

/* ---- Schema ------------------------------------------------------------------------------ */

static de_binding bind(de_binding_source src, u32 code, i8 slot) {
    de_binding b;
    memset(&b, 0, sizeof b);
    b.source = src;
    b.code = code;
    b.composite_slot = slot;
    return b;
}

static bool add(de_input *in, de_action_map m, const char *name, de_action_type type,
                de_interaction it, f32 hold, const de_binding *b, u32 n) {
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = type;
    d.interaction = it;
    d.hold_seconds = hold;
    d.bindings = b;
    d.binding_count = n;
    return DE_HANDLE_VALID(de_action_add(in, m, &d));
}

bool input_script_schema(de_input *in) {
    const de_action_map g = de_action_map_create(in, "gameplay");
    const de_action_map u = de_action_map_create(in, "menu");
    if (!DE_HANDLE_VALID(g) || !DE_HANDLE_VALID(u))
        return false;
    const i8 none = DE_COMPOSITE_NONE;
    bool ok = true;

    const de_binding jump[] = {bind(DE_BIND_KEY, DE_KEY_SPACE, none),
                               bind(DE_BIND_PAD_BUTTON, DE_PAD_SOUTH, none)};
    ok = ok && add(in, g, "jump", DE_ACTION_BUTTON, DE_INTERACT_PRESS, 0.0f, jump, 2);

    const de_binding fire[] = {bind(DE_BIND_MOUSE_BUTTON, DE_MOUSE_LEFT, none),
                               bind(DE_BIND_PAD_AXIS, DE_AXIS_RIGHT_TRIGGER, none)};
    ok = ok && add(in, g, "fire", DE_ACTION_BUTTON, DE_INTERACT_HOLD, 0.1f, fire, 2);

    const de_binding dash[] = {bind(DE_BIND_KEY, DE_KEY_LSHIFT, none)};
    ok = ok && add(in, g, "dash", DE_ACTION_BUTTON, DE_INTERACT_DOUBLE_TAP, 0.0f, dash, 1);

    const de_binding crouch[] = {bind(DE_BIND_KEY, DE_KEY_C, none)};
    ok = ok && add(in, g, "crouch", DE_ACTION_BUTTON, DE_INTERACT_TAP, 0.0f, crouch, 1);

    const de_binding use[] = {bind(DE_BIND_KEY, DE_KEY_E, none)};
    ok = ok && add(in, g, "use", DE_ACTION_BUTTON, DE_INTERACT_RELEASE, 0.0f, use, 1);

    de_binding move[] = {bind(DE_BIND_KEY, DE_KEY_W, DE_COMPOSITE_UP),
                         bind(DE_BIND_KEY, DE_KEY_S, DE_COMPOSITE_DOWN),
                         bind(DE_BIND_KEY, DE_KEY_A, DE_COMPOSITE_LEFT),
                         bind(DE_BIND_KEY, DE_KEY_D, DE_COMPOSITE_RIGHT),
                         bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, none)};
    move[4].deadzone = 0.15f;
    ok = ok && add(in, g, "move", DE_ACTION_AXIS2D, DE_INTERACT_PRESS, 0.0f, move, 5);

    de_binding look[] = {bind(DE_BIND_PAD_AXIS, DE_BIND_RIGHT_STICK, none)};
    look[0].deadzone = 0.1f;
    look[0].invert = true;
    look[0].curve.type = DE_CURVE_POWER;
    look[0].curve.exponent = 2.0f;
    ok = ok && add(in, g, "look", DE_ACTION_AXIS2D, DE_INTERACT_PRESS, 0.0f, look, 1);

    de_binding throttle[] = {bind(DE_BIND_KEY, DE_KEY_R, DE_COMPOSITE_POSITIVE),
                             bind(DE_BIND_KEY, DE_KEY_Q, DE_COMPOSITE_NEGATIVE),
                             bind(DE_BIND_PAD_AXIS, DE_AXIS_LEFT_TRIGGER, none)};
    throttle[2].scale = 0.5f;
    ok = ok && add(in, g, "throttle", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, 0.0f, throttle, 3);

    const de_binding confirm[] = {bind(DE_BIND_KEY, DE_KEY_RETURN, none)};
    ok = ok && add(in, u, "confirm", DE_ACTION_BUTTON, DE_INTERACT_PRESS, 0.0f, confirm, 1);

    de_binding scroll[] = {bind(DE_BIND_MOUSE_WHEEL, DE_BIND_AXIS_Y, none)};
    scroll[0].scale = 0.5f;
    ok = ok && add(in, u, "scroll", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, 0.0f, scroll, 1);

    if (!ok)
        return false;
    de_action_map_push(in, g);
    de_action_map_push(in, u);
    return true;
}

void input_script_actions(const de_input *in, de_action *out) {
    for (u32 i = 0; i < INPUT_SCRIPT_ACTIONS; ++i)
        out[i] = de__action_handle(in, i);
}

/* ---- Events ------------------------------------------------------------------------------ */

static u32 hash2(u32 a, u32 b) {
    u32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

static de__input_event event(de_raw_input_kind kind, u32 frame, u32 seq) {
    de__input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.time_ns = 1000000000ull + (u64)frame * 16666667ull + seq;
    return ev;
}

static void key(de_input *in, u32 frame, u32 *seq, de_key k, bool down) {
    de__input_event ev = event(DE_RAW_KEY, frame, (*seq)++);
    ev.key.code = (u16)k;
    ev.key.down = down;
    de__input_inject(in, &ev);
}

/* Keys of the script; each has its own hold pattern. */
static const de_key k_keys[] = {DE_KEY_SPACE, DE_KEY_LSHIFT, DE_KEY_C,     DE_KEY_E,
                                DE_KEY_W,     DE_KEY_A,      DE_KEY_S,     DE_KEY_D,
                                DE_KEY_R,     DE_KEY_Q,      DE_KEY_RETURN};
#define KEY_COUNT (sizeof k_keys / sizeof k_keys[0])

/** Whether key `k` is held in `frame`: blocks of 2..12 frames, on or off by hash. */
static bool key_held(u32 k, u32 frame) {
    const u32 len = 2u + (hash2(k, 0xABCu) % 11u);
    return (hash2(k + 1u, frame / len) % 3u) == 0;
}

/** A stick or trigger value in thousandths, held for 4-frame blocks. */
static f32 axis_value(u32 axis, u32 frame, bool trigger) {
    const u32 h = hash2(axis + 100u, frame / 4u);
    if (h % 5u == 0)
        return 0.0f; /* released often */
    if (trigger)
        return (f32)(h % 1001u) / 1000.0f;
    return (f32)((i32)(h % 2001u) - 1000) / 1000.0f;
}

void input_script_events(de_input *in, u32 frame) {
    u32 seq = 0;
    if (frame == 0) {
        de_platform_pad_info info;
        memset(&info, 0, sizeof info);
        info.id = INPUT_SCRIPT_PAD;
        info.type = DE_PLATFORM_PAD_PS5;
        info.player_index = -1;
        memcpy(info.name, "script pad", 11);
        de__input_inject_pad_info(in, &info);
        de__input_event ev = event(DE_RAW_PAD_ADDED, frame, seq++);
        ev.device = INPUT_SCRIPT_PAD;
        de__input_inject(in, &ev);
    }
    for (u32 k = 0; k < KEY_COUNT; ++k) {
        const bool now = key_held(k, frame);
        const bool before = frame != 0 && key_held(k, frame - 1u);
        if (now != before) {
            key(in, frame, &seq, k_keys[k], now);
        } else if (!now && hash2(k + 50u, frame) % 37u == 0) {
            /* A tap inside one frame: down and up. */
            key(in, frame, &seq, k_keys[k], true);
            key(in, frame, &seq, k_keys[k], false);
        }
    }
    /* Mouse left: 6-frame blocks. Wheel: a notch now and then. */
    const bool ml = hash2(200u, frame / 6u) % 2u == 0;
    const bool ml_before = frame != 0 && hash2(200u, (frame - 1u) / 6u) % 2u == 0;
    if (ml != ml_before) {
        de__input_event ev = event(DE_RAW_MOUSE_BUTTON, frame, seq++);
        ev.mouse_button.button = (u8)DE_MOUSE_LEFT;
        ev.mouse_button.down = ml;
        ev.mouse_button.clicks = 1;
        de__input_inject(in, &ev);
    }
    if (hash2(300u, frame) % 9u == 0) {
        de__input_event ev = event(DE_RAW_MOUSE_WHEEL, frame, seq++);
        ev.mouse_wheel.dy = (hash2(301u, frame) % 2u) != 0 ? 1.0f : -1.0f;
        de__input_inject(in, &ev);
    }
    /* Pad: south button, both sticks and both triggers, changed at 4-frame block starts. */
    const bool south = hash2(400u, frame / 5u) % 4u == 0;
    const bool south_before = frame != 0 && hash2(400u, (frame - 1u) / 5u) % 4u == 0;
    if (south != south_before) {
        de__input_event ev = event(DE_RAW_PAD_BUTTON, frame, seq++);
        ev.device = INPUT_SCRIPT_PAD;
        ev.pad_button.button = (u8)DE_PAD_SOUTH;
        ev.pad_button.down = south;
        de__input_inject(in, &ev);
    }
    if (frame % 4u == 0) {
        for (u32 a = 0; a < (u32)DE_PAD_AXIS_COUNT_; ++a) {
            const bool trigger = a == DE_AXIS_LEFT_TRIGGER || a == DE_AXIS_RIGHT_TRIGGER;
            de__input_event ev = event(DE_RAW_PAD_AXIS, frame, seq++);
            ev.device = INPUT_SCRIPT_PAD;
            ev.pad_axis.axis = (u8)a;
            ev.pad_axis.value = axis_value(a, frame, trigger);
            de__input_inject(in, &ev);
        }
    }
}

/* ---- Session ----------------------------------------------------------------------------- */

/** Fixed ticks after frame `frame`: mostly 1, sometimes 0 or 2 (the latch must handle both). */
static u32 ticks_after(u32 frame) {
    const u32 h = hash2(500u, frame) % 10u;
    return h == 0 ? 0u : (h == 1 ? 2u : 1u);
}

void input_script_run_live(de_input *in, u32 ticks, input_script_tick_fn fn, void *user) {
    u32 index = 0;
    for (u32 frame = 0; index < ticks; ++frame) {
        input_script_events(in, frame);
        de__input_begin_frame_dt(in, FRAME_DT);
        de__actions_frame_update(in, FRAME_DT);
        const u32 n = ticks_after(frame);
        for (u32 k = 0; k < n && index < ticks; ++k, ++index) {
            const u64 tick = INPUT_SCRIPT_FIRST_TICK + index;
            de__actions_fixed_tick(in, tick);
            if (fn != NULL)
                fn(in, tick, index, user);
        }
    }
}
