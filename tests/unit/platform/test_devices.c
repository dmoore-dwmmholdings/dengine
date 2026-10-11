/* Tests for raw input events, gamepads, sensors, clipboard, cursor, text input and file dialogs
 * (DENG-246).
 *
 * Events are pushed into SDL's queue (SDL_PushEvent) or come from SDL virtual gamepads, then go
 * through de_platform_pump() and come out of de_platform_input_events(). Tests that need video
 * (windows, clipboard, cursor, dialogs) skip when no video driver starts; event translation
 * tests fall back to a headless platform instead. A DE_HEADLESS build checks the stubs. */
#include "de_test.h"

#include <dengine/de_platform.h>

#include "../core/test_counting_alloc.h"

#include <stdlib.h>
#include <string.h>

#if !defined(DE_HEADLESS)
#include "platform/sdl/sdl_internal.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TEST_DEVICES_ASAN 1
#endif
#endif
#if defined(__APPLE__) && defined(TEST_DEVICES_ASAN)
/* Same macOS 26 CoreGraphics strndup report as in test_window.c (NSWindow titlebar layout). */
const char *__asan_default_suppressions(void);
const char *__asan_default_suppressions(void) {
    return "interceptor_via_lib:CoreGraphics\n";
}
#endif

typedef struct dev_fx {
    test_counting_alloc alloc;
    bool up;
    bool video; /* false: fell back to a headless platform */
} dev_fx;

static void dev_start(dev_fx *f, bool need_video) {
    test_counting_init(&f->alloc);
    de_platform_desc d = {0};
    d.app_name = "dengine test_devices";
    d.allocator = &f->alloc.iface;
    de_result r = de_platform_init(&d);
    if (r == DE_ERR_UNSUPPORTED && !need_video) {
        d.flags = DE_PLATFORM_HEADLESS;
        r = de_platform_init(&d);
    }
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("no video driver");
    ASSERT_EQ(r, DE_OK);
    f->up = true;
    f->video = !de_platform_headless();
}

/* Video fixture: windows, clipboard, gamepads. */
static void vid_fx_setup(dev_fx *f) {
    dev_start(f, true);
}
/* Any fixture: translation of pushed events works without video too. */
static void any_fx_setup(dev_fx *f) {
    dev_start(f, false);
}

static void dev_fx_teardown(dev_fx *f) {
    if (!f->up)
        return;
    de_platform_shutdown();
    EXPECT_EQ(f->alloc.live_blocks, 0u);
    EXPECT_EQ(f->alloc.size_errors, 0u);
}

typedef dev_fx vid_fx;
typedef dev_fx any_fx;
#define vid_fx_teardown dev_fx_teardown
#define any_fx_teardown dev_fx_teardown

/* Pumps until SDL's queue is empty, then pops every raw event into `out` (at most `max`). */
static u32 pump_raw(de_raw_input_event *out, u32 max) {
    de_app_event app[16];
    while (de_platform_pump(app, 16) == 16) {
    }
    return de_platform_input_events(out, max);
}

static void flush_raw(void) {
    de_raw_input_event tmp[64];
    while (pump_raw(tmp, 64) == 64) {
    }
}

static void push(SDL_Event *e) {
    ASSERT_TRUE(SDL_PushEvent(e));
}

static de_window open_hidden(void) {
    de_window_desc d = {0};
    d.title = "test_devices";
    d.width = 320;
    d.height = 240;
    d.display_index = -1;
    d.hidden = true;
    de_window w = {0};
    ASSERT_EQ(de_window_create(&d, &w), DE_OK);
    return w;
}

/* Error allowed when an SDL timestamp is moved to the de_time_now_ns() timebase (two clock
 * reads; each clock rounds to its own tick). */
#define TIME_SLACK_NS 20000u

/* SKIP does not return, so this returns only when pads can be tested. A `return` after SKIP is
 * unreachable code, which MSVC rejects (C4702). */
static void need_pads(void) {
    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        SKIP("no gamepad subsystem");
}

/* ---------- keyboard and mouse ---------- */

TEST_F(devices, key_hid_codes, any_fx) {
    flush_raw();
    de_window w = {0};
    SDL_WindowID wid = 0;
    if (f->video) {
        w = open_hidden();
        wid = SDL_GetWindowID(de__platform_sdl_window_get(w));
        flush_raw();
    }
    u64 t0 = de_time_now_ns();

    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = wid;
    e.key.scancode = SDL_SCANCODE_A;
    e.key.key = SDLK_A;
    e.key.mod = SDL_KMOD_LSHIFT | SDL_KMOD_RCTRL | SDL_KMOD_CAPS;
    e.key.down = true;
    push(&e);
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_Z;
    e.key.key = SDLK_Z;
    e.key.down = true;
    e.key.repeat = true;
    push(&e);
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.scancode = SDL_SCANCODE_RETURN;
    e.key.key = SDLK_RETURN;
    push(&e);
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_F1;
    e.key.key = SDLK_F1; /* not a character: sym 0 */
    e.key.mod = SDL_KMOD_LGUI | SDL_KMOD_RALT | SDL_KMOD_MODE;
    e.key.down = true;
    push(&e);

    de_raw_input_event ev[8];
    u32 n = pump_raw(ev, 8);
    u64 t1 = de_time_now_ns();
    ASSERT_EQ(n, 4u);
    for (u32 i = 0; i < n; i++) {
        EXPECT_EQ(ev[i].kind, DE_RAW_KEY);
        /* Pushed events are stamped when pushed: inside [t0, t1] in the engine timebase, give
         * or take the rounding of converting between the two clocks. */
        EXPECT_GE(ev[i].time_ns + TIME_SLACK_NS, t0);
        EXPECT_LE(ev[i].time_ns, t1 + TIME_SLACK_NS);
        if (i > 0)
            EXPECT_GE(ev[i].time_ns, ev[i - 1].time_ns);
    }
    /* HID usage codes equal SDL scancodes: A = 4, Z = 29, Return = 40, F1 = 58. */
    EXPECT_EQ(ev[0].key.code, 4u);
    EXPECT_TRUE(ev[0].key.down);
    EXPECT_FALSE(ev[0].key.repeat);
    EXPECT_EQ(ev[0].key.mods, (u32)(DE_KEYMOD_LSHIFT | DE_KEYMOD_RCTRL | DE_KEYMOD_CAPS));
    EXPECT_EQ(ev[0].key.sym, (u32)'a');
    EXPECT_EQ(ev[0].window.id, w.id);
    EXPECT_EQ(ev[1].key.code, 29u);
    EXPECT_TRUE(ev[1].key.repeat);
    EXPECT_EQ(ev[1].key.sym, (u32)'z');
    EXPECT_EQ(ev[2].key.code, 40u);
    EXPECT_FALSE(ev[2].key.down);
    EXPECT_EQ(ev[2].key.sym, (u32)'\r');
    EXPECT_EQ(ev[2].window.id, 0u);
    EXPECT_EQ(ev[3].key.code, 58u);
    EXPECT_EQ(ev[3].key.sym, 0u);
    EXPECT_EQ(ev[3].key.mods, (u32)(DE_KEYMOD_LGUI | DE_KEYMOD_RALT | DE_KEYMOD_ALTGR));

    if (f->video)
        de_window_destroy(w);
}

TEST_F(devices, mouse_events, any_fx) {
    flush_raw();
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.which = 7;
    e.motion.x = 10.5f;
    e.motion.y = 20.25f;
    e.motion.xrel = -3.0f;
    e.motion.yrel = 4.5f;
    push(&e);
    static const Uint8 sdl_buttons[5] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE,
                                         SDL_BUTTON_X1, SDL_BUTTON_X2};
    for (u32 i = 0; i < 5; i++) {
        SDL_zero(e);
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.button = sdl_buttons[i];
        e.button.down = true;
        e.button.clicks = 2;
        e.button.x = 1.0f;
        e.button.y = 2.0f;
        push(&e);
    }
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = 1.5f;
    e.wheel.x = -0.5f;
    e.wheel.direction = SDL_MOUSEWHEEL_FLIPPED; /* "natural" scrolling: undone */
    e.wheel.mouse_x = 5.0f;
    e.wheel.mouse_y = 6.0f;
    push(&e);
    /* Touchscreen and pen events are not part of the raw stream. */
    SDL_zero(e);
    e.type = SDL_EVENT_FINGER_DOWN;
    push(&e);

    de_raw_input_event ev[16];
    u32 n = pump_raw(ev, 16);
    ASSERT_EQ(n, 7u);
    EXPECT_EQ(ev[0].kind, DE_RAW_MOUSE_MOVE);
    EXPECT_EQ(ev[0].device, 7u);
    EXPECT_FLOAT_EQ(ev[0].mouse_move.x, 10.5, 0.0);
    EXPECT_FLOAT_EQ(ev[0].mouse_move.y, 20.25, 0.0);
    EXPECT_FLOAT_EQ(ev[0].mouse_move.dx, -3.0, 0.0);
    EXPECT_FLOAT_EQ(ev[0].mouse_move.dy, 4.5, 0.0);
    /* de_mouse_button order: left, right, middle, X1, X2. */
    for (u32 i = 0; i < 5; i++) {
        EXPECT_EQ(ev[1 + i].kind, DE_RAW_MOUSE_BUTTON);
        EXPECT_EQ(ev[1 + i].mouse_button.button, i);
        EXPECT_TRUE(ev[1 + i].mouse_button.down);
        EXPECT_EQ(ev[1 + i].mouse_button.clicks, 2u);
    }
    EXPECT_EQ(ev[6].kind, DE_RAW_MOUSE_WHEEL);
    EXPECT_FLOAT_EQ(ev[6].mouse_wheel.dy, -1.5, 0.0);
    EXPECT_FLOAT_EQ(ev[6].mouse_wheel.dx, 0.5, 0.0);
    EXPECT_FLOAT_EQ(ev[6].mouse_wheel.x, 5.0, 0.0);
}

/* ---------- text ---------- */

TEST_F(devices, text_input_utf8, any_fx) {
    flush_raw();
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = "h\xC3\xA9llo"; /* "héllo" */
    push(&e);

    /* 30 ASCII bytes, then the 3-byte euro sign across the 31-byte limit, then more. */
    static const char long_text[] = "abcdefghijklmnopqrstuvwxyz0123"
                                    "\xE2\x82\xAC"
                                    "bc \xF0\x9F\x98\x80 end";
    SDL_zero(e);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = long_text;
    push(&e);

    SDL_zero(e);
    e.type = SDL_EVENT_TEXT_EDITING;
    e.edit.text = "\xE3\x81\x8B"; /* か, IME composition */
    e.edit.start = 1;
    e.edit.length = 0;
    push(&e);

    de_raw_input_event ev[8];
    u32 n = pump_raw(ev, 8);
    ASSERT_EQ(n, 4u);
    EXPECT_EQ(ev[0].kind, DE_RAW_TEXT);
    EXPECT_STREQ(ev[0].text.utf8, "h\xC3\xA9llo");
    EXPECT_EQ(ev[0].text.start, 0);

    /* The long text is split between code points and joins back to the original. */
    EXPECT_EQ(ev[1].kind, DE_RAW_TEXT);
    EXPECT_EQ(ev[2].kind, DE_RAW_TEXT);
    EXPECT_EQ(strlen(ev[1].text.utf8), 30u);
    char joined[64];
    size_t l1 = strlen(ev[1].text.utf8), l2 = strlen(ev[2].text.utf8);
    ASSERT_LT(l1 + l2, sizeof joined);
    memcpy(joined, ev[1].text.utf8, l1);
    memcpy(joined + l1, ev[2].text.utf8, l2 + 1u);
    EXPECT_STREQ(joined, long_text);

    EXPECT_EQ(ev[3].kind, DE_RAW_TEXT_EDIT);
    EXPECT_STREQ(ev[3].text.utf8, "\xE3\x81\x8B");
    EXPECT_EQ(ev[3].text.start, 1);
    EXPECT_EQ(ev[3].text.length, 0);

    if (f->video) {
        de_window w = open_hidden();
        de_platform_text_area area = {10, 20, 200, 24, 8};
        EXPECT_EQ(de_platform_text_input_start(w, &area), DE_OK);
        EXPECT_TRUE(de_platform_text_input_active(w));
        area.cursor = 16;
        EXPECT_EQ(de_platform_text_input_area(w, &area), DE_OK);
        de_platform_text_input_stop(w);
        EXPECT_FALSE(de_platform_text_input_active(w));
        de_window_destroy(w);
        EXPECT_EQ(de_platform_text_input_start(w, NULL), DE_ERR_INVALID_ARGUMENT);
        EXPECT_FALSE(de_platform_text_input_active(w));
    } else {
        de_window none = {0};
        EXPECT_EQ(de_platform_text_input_start(none, NULL), DE_ERR_UNSUPPORTED);
    }
}

/* ---------- ring ---------- */

TEST_F(devices, ring_overflow, any_fx) {
    flush_raw();
    u64 dropped0 = de_platform_input_dropped();
    const u32 extra = 10;
    const u32 total = DE_PLATFORM_INPUT_RING + extra;
    for (u32 i = 0; i < total; i++) {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.scancode = (SDL_Scancode)(4u + i % 200u);
        e.key.down = true;
        push(&e);
    }
    /* The ring was allocated at init: pumping and draining take nothing from the allocator. */
    u64 allocs0 = f->alloc.allocs;
    de_app_event app[4];
    while (de_platform_pump(app, 4) == 4) {
    }
    EXPECT_EQ(de_platform_input_dropped() - dropped0, (u64)extra);

    /* Oldest first dropped: what is left starts at event `extra` and is in order. Drain in
     * uneven chunks so the copy crosses the ring's wrap point. */
    de_raw_input_event ev[97];
    u32 seen = 0;
    u32 got;
    while ((got = de_platform_input_events(ev, 97)) != 0) {
        for (u32 k = 0; k < got; k++) {
            u32 i = extra + seen + k;
            EXPECT_EQ(ev[k].kind, DE_RAW_KEY);
            EXPECT_EQ(ev[k].key.code, 4u + i % 200u);
        }
        seen += got;
    }
    EXPECT_EQ(seen, (u32)DE_PLATFORM_INPUT_RING);
    EXPECT_EQ(f->alloc.allocs, allocs0);
    EXPECT_EQ(de_platform_input_events(ev, 97), 0u);
    EXPECT_EQ(de_platform_input_events(NULL, 4), 0u);
}

/* ---------- gamepads ---------- */

typedef struct vpad_calls {
    Uint16 low, high, left, right;
    Uint8 r, g, b;
    u8 effect[8];
    int effect_size;
    int rumbles, trigger_rumbles, leds, effects;
} vpad_calls;

static bool SDLCALL vp_rumble(void *u, Uint16 lo, Uint16 hi) {
    vpad_calls *c = (vpad_calls *)u;
    c->low = lo;
    c->high = hi;
    c->rumbles++;
    return true;
}
static bool SDLCALL vp_trig(void *u, Uint16 l, Uint16 r) {
    vpad_calls *c = (vpad_calls *)u;
    c->left = l;
    c->right = r;
    c->trigger_rumbles++;
    return true;
}
static bool SDLCALL vp_led(void *u, Uint8 r, Uint8 g, Uint8 b) {
    vpad_calls *c = (vpad_calls *)u;
    c->r = r;
    c->g = g;
    c->b = b;
    c->leds++;
    return true;
}
static bool SDLCALL vp_effect(void *u, const void *data, int size) {
    vpad_calls *c = (vpad_calls *)u;
    c->effect_size = size;
    memcpy(c->effect, data, size < 8 ? (size_t)size : 8u);
    c->effects++;
    return true;
}
static bool SDLCALL vp_sensors(void *u, bool on) {
    (void)u;
    (void)on;
    return true;
}

/* Attaches a virtual gamepad with every button and axis; `calls` != NULL adds the haptics
 * callbacks, `sensors` a gyro + accelerometer and one 2-finger touchpad. */
static SDL_JoystickID attach_pad(const char *name, vpad_calls *calls, bool sensors) {
    static const SDL_VirtualJoystickTouchpadDesc touchpad = {2, {0, 0, 0}};
    static const SDL_VirtualJoystickSensorDesc sensor_desc[2] = {{SDL_SENSOR_GYRO, 250.0f},
                                                                 {SDL_SENSOR_ACCEL, 250.0f}};
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = name;
    if (sensors) {
        d.ntouchpads = 1;
        d.touchpads = &touchpad;
        d.nsensors = 2;
        d.sensors = sensor_desc;
        d.SetSensorsEnabled = vp_sensors;
    }
    if (calls) {
        d.userdata = calls;
        d.Rumble = vp_rumble;
        d.RumbleTriggers = vp_trig;
        d.SetLED = vp_led;
        d.SendEffect = vp_effect;
    }
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    ASSERT_NE(id, 0u);
    return id;
}

/* Pumps and returns the first event of `kind` for `pad` (removing nothing else from `ev`). */
static const de_raw_input_event *find_ev(const de_raw_input_event *ev, u32 n,
                                         de_raw_input_kind kind, u32 pad) {
    for (u32 i = 0; i < n; i++)
        if (ev[i].kind == kind && ev[i].device == pad)
            return &ev[i];
    return NULL;
}

static const de_raw_input_event *find_axis(const de_raw_input_event *ev, u32 n, u32 pad, u8 axis) {
    for (u32 i = 0; i < n; i++)
        if (ev[i].kind == DE_RAW_PAD_AXIS && ev[i].device == pad && ev[i].pad_axis.axis == axis)
            return &ev[i];
    return NULL;
}

TEST_F(devices, virtual_pad_events, vid_fx) {
    (void)f;
    need_pads();
    flush_raw();
    /* Real controllers on the host may be connected too: count relative to them. */
    u32 base = de_platform_pads(NULL, 0);
    ASSERT_LT(base, 12u);
    SDL_JoystickID id = attach_pad("DEngine virtual pad", NULL, true);
    de_raw_input_event ev[64];
    u32 n = pump_raw(ev, 64);
    ASSERT_TRUE(find_ev(ev, n, DE_RAW_PAD_ADDED, id) != NULL);

    u32 pads[DE_PLATFORM_PAD_MAX];
    ASSERT_EQ(de_platform_pads(pads, DE_PLATFORM_PAD_MAX), base + 1u);
    EXPECT_EQ(pads[base], (u32)id); /* connection order: the newest is last */
    de_platform_pad_info info;
    ASSERT_EQ(de_platform_pad_info_get(id, &info), DE_OK);
    EXPECT_EQ(info.id, (u32)id);
    EXPECT_STREQ(info.name, "DEngine virtual pad");
    EXPECT_EQ(info.type, DE_PLATFORM_PAD_GENERIC);
    EXPECT_EQ(info.touchpads, 1u);
    EXPECT_EQ(info.caps, (u32)(DE_PLATFORM_PAD_CAP_GYRO | DE_PLATFORM_PAD_CAP_ACCEL |
                               DE_PLATFORM_PAD_CAP_TOUCHPAD));
    EXPECT_TRUE(de_platform_pad_has_sensor(id, DE_PLATFORM_SENSOR_GYRO));
    EXPECT_TRUE(de_platform_pad_has_sensor(id, DE_PLATFORM_SENSOR_ACCEL));
    EXPECT_FALSE(de_platform_pad_has_sensor(id, DE_PLATFORM_SENSOR_GYRO_L));

    /* Drive the pad through SDL's virtual joystick API. */
    SDL_Joystick *joy = SDL_OpenJoystick(id);
    ASSERT_TRUE(joy != NULL);
    ASSERT_TRUE(SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true));
    ASSERT_TRUE(SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, true));
    ASSERT_TRUE(SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 32767));
    ASSERT_TRUE(SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, -32768));
    ASSERT_TRUE(SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHTX, -16384));
    ASSERT_TRUE(SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767));
    ASSERT_TRUE(SDL_SetJoystickVirtualTouchpad(joy, 0, 1, true, 0.25f, 0.75f, 1.0f));
    ASSERT_EQ(de_platform_pad_sensor_enable(id, DE_PLATFORM_SENSOR_GYRO, true), DE_OK);
    const float gyro[3] = {0.5f, -1.25f, 2.0f};
    u64 t0 = de_time_now_ns();
    ASSERT_TRUE(SDL_SendJoystickVirtualSensorData(joy, SDL_SENSOR_GYRO, 123456789u, gyro, 3));

    n = pump_raw(ev, 64);
    const de_raw_input_event *b = find_ev(ev, n, DE_RAW_PAD_BUTTON, id);
    ASSERT_TRUE(b != NULL);
    EXPECT_EQ(b->pad_button.button, 0u); /* DE_PAD_SOUTH */
    EXPECT_TRUE(b->pad_button.down);
    u32 paddles = 0;
    for (u32 i = 0; i < n; i++)
        if (ev[i].kind == DE_RAW_PAD_BUTTON && ev[i].pad_button.button == 19u) /* PADDLE4 */
            paddles++;
    EXPECT_EQ(paddles, 1u);

    const de_raw_input_event *a = find_axis(ev, n, id, 0); /* LEFT_X */
    ASSERT_TRUE(a != NULL);
    EXPECT_FLOAT_EQ(a->pad_axis.value, 1.0, 0.0);
    a = find_axis(ev, n, id, 1); /* LEFT_Y: -32768 is clamped to -1 */
    ASSERT_TRUE(a != NULL);
    EXPECT_FLOAT_EQ(a->pad_axis.value, -1.0, 0.0);
    a = find_axis(ev, n, id, 2); /* RIGHT_X */
    ASSERT_TRUE(a != NULL);
    EXPECT_FLOAT_EQ(a->pad_axis.value, -0.5, 1e-4);
    a = find_axis(ev, n, id, 4); /* LEFT_TRIGGER: [0, 1] */
    ASSERT_TRUE(a != NULL);
    EXPECT_FLOAT_EQ(a->pad_axis.value, 1.0, 1e-4);

    const de_raw_input_event *t = find_ev(ev, n, DE_RAW_PAD_TOUCHPAD, id);
    ASSERT_TRUE(t != NULL);
    EXPECT_EQ(t->pad_touch.phase, (u32)DE_TOUCH_DOWN);
    EXPECT_EQ(t->pad_touch.finger, 1u);
    EXPECT_FLOAT_EQ(t->pad_touch.x, 0.25, 1e-6);
    EXPECT_FLOAT_EQ(t->pad_touch.y, 0.75, 1e-6);
    EXPECT_FLOAT_EQ(t->pad_touch.pressure, 1.0, 1e-6);

    const de_raw_input_event *s = find_ev(ev, n, DE_RAW_PAD_SENSOR, id);
    ASSERT_TRUE(s != NULL);
    EXPECT_EQ(s->sensor.sensor, (u32)DE_PLATFORM_SENSOR_GYRO);
    EXPECT_FLOAT_EQ(s->sensor.data[0], 0.5, 0.0);
    EXPECT_FLOAT_EQ(s->sensor.data[1], -1.25, 0.0);
    EXPECT_FLOAT_EQ(s->sensor.data[2], 2.0, 0.0);
    EXPECT_EQ(s->sensor.sensor_time_ns, (u64)123456789u);
    EXPECT_GE(s->time_ns + TIME_SLACK_NS, t0);
    EXPECT_LE(s->time_ns, de_time_now_ns() + TIME_SLACK_NS);

    /* Release the button: one up event. */
    ASSERT_TRUE(SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false));
    n = pump_raw(ev, 64);
    b = find_ev(ev, n, DE_RAW_PAD_BUTTON, id);
    ASSERT_TRUE(b != NULL);
    EXPECT_EQ(b->pad_button.button, 0u);
    EXPECT_FALSE(b->pad_button.down);

    /* Detach: PAD_REMOVED, and the id is stale afterwards. */
    SDL_CloseJoystick(joy);
    ASSERT_TRUE(SDL_DetachVirtualJoystick(id));
    n = pump_raw(ev, 64);
    ASSERT_TRUE(find_ev(ev, n, DE_RAW_PAD_REMOVED, id) != NULL);
    EXPECT_EQ(de_platform_pads(pads, DE_PLATFORM_PAD_MAX), base);
    EXPECT_EQ(de_platform_pad_info_get(id, &info), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_platform_pad_rumble(id, 1.0f, 1.0f, 100), DE_ERR_NOT_FOUND);
}

TEST_F(devices, unsupported_haptics, vid_fx) {
    (void)f;
    need_pads();
    flush_raw();
    u32 base = de_platform_pads(NULL, 0);
    ASSERT_LT(base, 12u);
    /* A pad without rumble motors, LED, effect channel or sensors. */
    SDL_JoystickID bare = attach_pad("DEngine bare pad", NULL, false);
    flush_raw();
    de_platform_pad_info info;
    ASSERT_EQ(de_platform_pad_info_get(bare, &info), DE_OK);
    EXPECT_EQ(info.caps, 0u);
    EXPECT_EQ(de_platform_pad_rumble(bare, 1.0f, 0.5f, 200), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_platform_pad_trigger_rumble(bare, 1.0f, 1.0f, 200), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_platform_pad_set_led(bare, 255, 0, 0), DE_ERR_UNSUPPORTED);
    static const u8 packet[4] = {0x02, 0x0C, 0x40, 0x7F};
    EXPECT_EQ(de_platform_pad_send_effect(bare, packet, sizeof packet), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_platform_pad_send_effect(bare, NULL, 4), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_platform_pad_sensor_enable(bare, DE_PLATFORM_SENSOR_GYRO, true),
              DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_platform_pad_sensor_enable(bare, DE_PLATFORM_SENSOR_UNKNOWN, true),
              DE_ERR_UNSUPPORTED);
    EXPECT_FALSE(de_platform_pad_has_sensor(bare, DE_PLATFORM_SENSOR_GYRO));
    EXPECT_EQ(de_platform_pad_rumble(0, 1.0f, 1.0f, 1), DE_ERR_NOT_FOUND);

    /* A pad with every haptics feature: the calls reach the device with scaled values. */
    /* Static: the pad stays attached until shutdown, whose close stops the motors through the
     * callbacks after this function has returned. */
    static vpad_calls calls;
    memset(&calls, 0, sizeof calls);
    SDL_JoystickID full = attach_pad("DEngine haptic pad", &calls, false);
    flush_raw();
    ASSERT_EQ(de_platform_pad_info_get(full, &info), DE_OK);
    EXPECT_EQ(info.caps, (u32)(DE_PLATFORM_PAD_CAP_RUMBLE | DE_PLATFORM_PAD_CAP_TRIGGER_RUMBLE |
                               DE_PLATFORM_PAD_CAP_LED));
    EXPECT_EQ(de_platform_pad_rumble(full, 1.0f, 0.5f, 200), DE_OK);
    EXPECT_EQ(calls.rumbles, 1);
    EXPECT_EQ(calls.low, 65535u);
    EXPECT_EQ(calls.high, 32768u);
    EXPECT_EQ(de_platform_pad_trigger_rumble(full, 2.0f, -1.0f, 200), DE_OK); /* clamped */
    EXPECT_EQ(calls.trigger_rumbles, 1);
    EXPECT_EQ(calls.left, 65535u);
    EXPECT_EQ(calls.right, 0u);
    EXPECT_EQ(de_platform_pad_set_led(full, 10, 20, 30), DE_OK);
    EXPECT_EQ(calls.leds, 1);
    EXPECT_EQ(calls.r, 10u);
    EXPECT_EQ(calls.g, 20u);
    EXPECT_EQ(calls.b, 30u);
    EXPECT_EQ(de_platform_pad_send_effect(full, packet, sizeof packet), DE_OK);
    EXPECT_EQ(calls.effects, 1);
    EXPECT_EQ(calls.effect_size, 4);
    EXPECT_MEMEQ(calls.effect, packet, 4);

    u32 pads[DE_PLATFORM_PAD_MAX];
    ASSERT_EQ(de_platform_pads(pads, DE_PLATFORM_PAD_MAX), base + 2u);
    EXPECT_EQ(pads[base], (u32)bare); /* connection order */
    EXPECT_EQ(pads[base + 1u], (u32)full);
    EXPECT_EQ(de_platform_pads(NULL, 0), base + 2u);

    ASSERT_TRUE(SDL_DetachVirtualJoystick(bare));
    flush_raw();
    ASSERT_EQ(de_platform_pads(pads, DE_PLATFORM_PAD_MAX), base + 1u);
    EXPECT_EQ(pads[base], (u32)full); /* the table closed the gap */
    /* The second pad stays attached: shutdown closes it. */
}

/* ---------- device sensors ---------- */

TEST_F(devices, device_sensors, vid_fx) {
    (void)f;
    u32 count = de_platform_sensors(NULL, 0);
    de_platform_sensor_info info[8];
    u32 listed = de_platform_sensors(info, 8);
    EXPECT_EQ(listed, count);
    f32 v[3];
    EXPECT_EQ(de_platform_sensor_read(0xFFFFFFF0u, v), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_platform_sensor_read(0xFFFFFFF0u, NULL), DE_ERR_INVALID_ARGUMENT);
    if (count == 0) {
        EXPECT_EQ(de_platform_sensor_enable(0xFFFFFFF0u, true), DE_ERR_NOT_FOUND);
        SKIP("no device sensors on this machine");
    }
    ASSERT_EQ(de_platform_sensor_enable(info[0].id, true), DE_OK);
    flush_raw();
    EXPECT_EQ(de_platform_sensor_read(info[0].id, v), DE_OK);
    EXPECT_EQ(de_platform_sensor_enable(info[0].id, false), DE_OK);
    EXPECT_EQ(de_platform_sensor_read(info[0].id, v), DE_ERR_INVALID_ARGUMENT);
}

/* ---------- clipboard ---------- */

TEST_F(devices, clipboard_roundtrip, vid_fx) {
    (void)f;
    /* Keep the user's clipboard: read it first, put it back at the end. */
    size_t old_len = de_platform_clipboard_get(NULL, 0);
    char *old = (char *)malloc(old_len + 1u);
    ASSERT_TRUE(old != NULL);
    de_platform_clipboard_get(old, old_len + 1u);

    static const char text[] = "DEngine clipboard \xE2\x9C\x93 \xC3\xBCn\xC3\xAF"
                               "c\xC3\xB8"
                               "d\xC3\xA9 \xE5\x89\xAA\xE8\xB4\xB4\xE6\x9D\xBF";
    de_result r = de_platform_clipboard_set(text);
    if (r != DE_OK) {
        free(old);
        SKIP("the OS refused the clipboard");
    }
    EXPECT_EQ(de_platform_clipboard_get(NULL, 0), sizeof text - 1u);
    char buf[128];
    EXPECT_EQ(de_platform_clipboard_get(buf, sizeof buf), sizeof text - 1u);
    EXPECT_STREQ(buf, text);

    /* A short buffer gets a prefix cut at a code point boundary: "DEngine clipboard " is 18
     * bytes, the check mark 3 more, so 20 bytes of room stop before it. */
    char small[21];
    EXPECT_EQ(de_platform_clipboard_get(small, sizeof small), sizeof text - 1u);
    EXPECT_STREQ(small, "DEngine clipboard ");
    EXPECT_EQ(de_platform_clipboard_set(NULL), DE_ERR_INVALID_ARGUMENT);

    EXPECT_EQ(de_platform_clipboard_set(old), DE_OK);
    free(old);
}

/* ---------- cursor and mouse modes ---------- */

TEST_F(devices, cursor_and_mouse_modes, vid_fx) {
    (void)f;
    for (u32 s = 0; s < DE_CURSOR_COUNT_; s++) {
        de_result r = de_platform_cursor_set((de_platform_cursor)s);
        EXPECT_TRUE(r == DE_OK || r == DE_ERR_UNSUPPORTED);
    }
    EXPECT_EQ(de_platform_cursor_set(DE_CURSOR_COUNT_), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_platform_cursor_set(DE_CURSOR_DEFAULT), DE_OK);
    EXPECT_EQ(de_platform_cursor_show(false), DE_OK);
    EXPECT_FALSE(de_platform_cursor_visible());
    EXPECT_EQ(de_platform_cursor_show(true), DE_OK);
    EXPECT_TRUE(de_platform_cursor_visible());

    de_window w = open_hidden();
    EXPECT_FALSE(de_platform_mouse_relative_get(w));
    EXPECT_EQ(de_platform_mouse_relative(w, true), DE_OK);
    EXPECT_TRUE(de_platform_mouse_relative_get(w));
    EXPECT_EQ(de_platform_mouse_relative(w, false), DE_OK);
    EXPECT_FALSE(de_platform_mouse_relative_get(w));
    de_window_destroy(w);
    /* Stale windows are rejected (the warp would move the real cursor, so only this case). */
    EXPECT_EQ(de_platform_mouse_relative(w, true), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_platform_mouse_warp(w, 1.0f, 1.0f), DE_ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(de_platform_mouse_relative_get(w));
}

/* ---------- file dialogs ---------- */

typedef struct fake_dialog {
    SDL_FileDialogType type;
    SDL_DialogFileCallback cb;
    void *userdata;
    int nfilters;
    char title[64];
    char pattern0[32];
    bool many;
    bool complete_now; /* call back from inside the show call (like macOS app-modal panels) */
} fake_dialog;

static fake_dialog g_fake[8];
static u32 g_fake_count;

static void SDLCALL fake_show(SDL_FileDialogType type, SDL_DialogFileCallback cb, void *userdata,
                              SDL_PropertiesID props) {
    fake_dialog *d = &g_fake[g_fake_count++ % 8];
    bool now = d->complete_now;
    memset(d, 0, sizeof *d);
    d->type = type;
    d->cb = cb;
    d->userdata = userdata;
    d->nfilters = (int)SDL_GetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 0);
    const SDL_DialogFileFilter *fl = (const SDL_DialogFileFilter *)SDL_GetPointerProperty(
        props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, NULL);
    if (fl && d->nfilters > 0)
        SDL_strlcpy(d->pattern0, fl[0].pattern, sizeof d->pattern0);
    const char *title = SDL_GetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "");
    SDL_strlcpy(d->title, title, sizeof d->title);
    d->many = SDL_GetBooleanProperty(props, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
    if (now) {
        static const char *const files[] = {"/tmp/a.png", "/tmp/b c.jpg", NULL};
        cb(userdata, files, 1);
        d->cb = NULL;
    }
}

typedef struct dialog_result {
    int calls;
    de_result result;
    u32 count;
    i32 filter;
    char first[64];
    char second[64];
} dialog_result;

static void on_dialog(void *user, de_result result, const char *const *paths, u32 count,
                      i32 filter) {
    dialog_result *r = (dialog_result *)user;
    r->calls++;
    r->result = result;
    r->count = count;
    r->filter = filter;
    if (count > 0)
        SDL_strlcpy(r->first, paths[0], sizeof r->first);
    if (count > 1)
        SDL_strlcpy(r->second, paths[1], sizeof r->second);
}

/* Completes a stashed fake dialog from another thread, as Windows and Linux portals do. */
static i32 cancel_from_thread(void *user) {
    fake_dialog *d = (fake_dialog *)user;
    static const char *const none[] = {NULL};
    d->cb(d->userdata, none, -1);
    return 0;
}

TEST(devices, dialog_delivery) {
    test_counting_alloc alloc;
    test_counting_init(&alloc);
    de_platform_desc pd = {0};
    pd.allocator = &alloc.iface;
    de_result r = de_platform_init(&pd);
    if (r == DE_ERR_UNSUPPORTED)
        SKIP("no video driver");
    ASSERT_EQ(r, DE_OK);
    de__platform_sdl_dialog_set_show(fake_show);
    memset(g_fake, 0, sizeof g_fake);
    g_fake_count = 0;

    static const de_file_dialog_filter filters[2] = {{"Images", "png;jpg"}, {"All", "*"}};
    de_file_dialog_desc d = {0};
    d.kind = DE_FILE_DIALOG_OPEN;
    d.filters = filters;
    d.filter_count = 2;
    d.title = "Pick images";
    d.default_path = "/tmp";
    d.allow_many = true;

    /* Invalid calls never call back. */
    dialog_result bad;
    memset(&bad, 0, sizeof bad);
    EXPECT_EQ(de_platform_file_dialog(&d, NULL, &bad), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_platform_file_dialog(NULL, on_dialog, &bad), DE_ERR_INVALID_ARGUMENT);
    de_file_dialog_filter broken = {"Broken", NULL};
    de_file_dialog_desc d2 = d;
    d2.filters = &broken;
    d2.filter_count = 1;
    EXPECT_EQ(de_platform_file_dialog(&d2, on_dialog, &bad), DE_ERR_INVALID_ARGUMENT);
    d2 = d;
    d2.kind = (de_file_dialog_kind)7;
    EXPECT_EQ(de_platform_file_dialog(&d2, on_dialog, &bad), DE_ERR_INVALID_ARGUMENT);
    d2 = d;
    d2.parent.id = 0x12345u;
    EXPECT_EQ(de_platform_file_dialog(&d2, on_dialog, &bad), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(g_fake_count, 0u);

    /* 1. The OS answers inside the show call: the callback still waits for the pump. */
    dialog_result one;
    memset(&one, 0, sizeof one);
    g_fake[0].complete_now = true;
    ASSERT_EQ(de_platform_file_dialog(&d, on_dialog, &one), DE_OK);
    EXPECT_EQ(g_fake[0].type, SDL_FILEDIALOG_OPENFILE);
    EXPECT_EQ(g_fake[0].nfilters, 2);
    EXPECT_STREQ(g_fake[0].pattern0, "png;jpg");
    EXPECT_STREQ(g_fake[0].title, "Pick images");
    EXPECT_TRUE(g_fake[0].many);
    EXPECT_EQ(one.calls, 0);
    de_platform_pump(NULL, 0);
    EXPECT_EQ(one.calls, 1);
    EXPECT_EQ(one.result, DE_OK);
    EXPECT_EQ(one.count, 2u);
    EXPECT_EQ(one.filter, 1);
    EXPECT_STREQ(one.first, "/tmp/a.png");
    EXPECT_STREQ(one.second, "/tmp/b c.jpg");
    de_platform_pump(NULL, 0);
    EXPECT_EQ(one.calls, 1); /* exactly once */

    /* 2. Cancelled from another thread; folder dialogs ignore filters. */
    dialog_result two;
    memset(&two, 0, sizeof two);
    d2 = d;
    d2.kind = DE_FILE_DIALOG_FOLDER;
    ASSERT_EQ(de_platform_file_dialog(&d2, on_dialog, &two), DE_OK);
    fake_dialog *fd = &g_fake[1];
    EXPECT_EQ(fd->type, SDL_FILEDIALOG_OPENFOLDER);
    EXPECT_EQ(fd->nfilters, 0);
    de_thread *th = NULL;
    ASSERT_EQ(de_thread_create(cancel_from_thread, fd, "dialog cb", DE_THREAD_NORMAL, 0, &th),
              DE_OK);
    de_thread_join(th);
    EXPECT_EQ(two.calls, 0);
    de_platform_pump(NULL, 0);
    EXPECT_EQ(two.calls, 1);
    EXPECT_EQ(two.result, DE_ERR_CANCELLED);
    EXPECT_EQ(two.count, 0u);

    /* 3. The OS fails: DE_ERR_IO with the reason in the error context. */
    dialog_result three;
    memset(&three, 0, sizeof three);
    d2 = d;
    d2.kind = DE_FILE_DIALOG_SAVE;
    ASSERT_EQ(de_platform_file_dialog(&d2, on_dialog, &three), DE_OK);
    EXPECT_EQ(g_fake[2].type, SDL_FILEDIALOG_SAVEFILE);
    EXPECT_FALSE(g_fake[2].many); /* no multi-select for save */
    SDL_SetError("portal not available");
    g_fake[2].cb(g_fake[2].userdata, NULL, -1);
    de_platform_pump(NULL, 0);
    EXPECT_EQ(three.calls, 1);
    EXPECT_EQ(three.result, DE_ERR_IO);

    /* 4. DE_PLATFORM_DIALOG_MAX open dialogs: one more is refused. */
    dialog_result open[DE_PLATFORM_DIALOG_MAX];
    memset(open, 0, sizeof open);
    for (u32 i = 0; i < DE_PLATFORM_DIALOG_MAX; i++)
        ASSERT_EQ(de_platform_file_dialog(&d, on_dialog, &open[i]), DE_OK);
    EXPECT_EQ(de_platform_file_dialog(&d, on_dialog, &bad), DE_ERR_BUSY);
    de_platform_pump(NULL, 0);
    for (u32 i = 0; i < DE_PLATFORM_DIALOG_MAX; i++)
        EXPECT_EQ(open[i].calls, 0);
    /* Finish one before shutdown: shutdown still delivers its real result. */
    fake_dialog *late = &g_fake[3 + 1];
    static const char *const one_file[] = {"/tmp/one.txt", NULL};
    g_fake[3].cb(g_fake[3].userdata, one_file, 0);

    /* 5. Shutdown calls every outstanding callback; open ones are cancelled. */
    de_platform_shutdown();
    EXPECT_EQ(open[0].calls, 1);
    EXPECT_EQ(open[0].result, DE_OK);
    EXPECT_STREQ(open[0].first, "/tmp/one.txt");
    for (u32 i = 1; i < DE_PLATFORM_DIALOG_MAX; i++) {
        EXPECT_EQ(open[i].calls, 1);
        EXPECT_EQ(open[i].result, DE_ERR_CANCELLED);
    }
    /* A late OS answer after shutdown is dropped (and frees its request). */
    late->cb(late->userdata, one_file, 0);
    EXPECT_EQ(open[1].calls, 1);
    for (u32 i = 2; i < DE_PLATFORM_DIALOG_MAX; i++) {
        fake_dialog *rest = &g_fake[3 + i];
        rest->cb(rest->userdata, NULL, -1);
    }
    EXPECT_EQ(bad.calls, 0);
    de__platform_sdl_dialog_set_show(NULL);
    EXPECT_EQ(alloc.live_blocks, 0u);
}

/* ---------- headless platform ---------- */

TEST(devices, headless_unsupported) {
    de_platform_desc d = {0};
    d.flags = DE_PLATFORM_HEADLESS;
    ASSERT_EQ(de_platform_init(&d), DE_OK);
    EXPECT_EQ(de_platform_pads(NULL, 0), 0u);
    EXPECT_EQ(de_platform_pad_rumble(1, 1.0f, 1.0f, 10), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_platform_sensors(NULL, 0), 0u);
    EXPECT_EQ(de_platform_sensor_enable(1, true), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_platform_clipboard_set("x"), DE_ERR_UNSUPPORTED);
    char buf[4] = "abc";
    EXPECT_EQ(de_platform_clipboard_get(buf, sizeof buf), 0u);
    EXPECT_STREQ(buf, "");
    EXPECT_EQ(de_platform_cursor_show(true), DE_ERR_UNSUPPORTED);
    EXPECT_FALSE(de_platform_cursor_visible());
    EXPECT_EQ(de_platform_cursor_set(DE_CURSOR_TEXT), DE_ERR_UNSUPPORTED);
    de_window none = {0};
    EXPECT_EQ(de_platform_mouse_relative(none, true), DE_ERR_UNSUPPORTED);
    de_file_dialog_desc fd = {0};
    EXPECT_EQ(de_platform_file_dialog(&fd, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    de_platform_shutdown();

    /* Not initialised at all. */
    de_raw_input_event ev[2];
    EXPECT_EQ(de_platform_input_events(ev, 2), 0u);
    EXPECT_EQ(de_platform_input_dropped(), 0u);
    EXPECT_EQ(de_platform_pads(NULL, 0), 0u);
}

#else /* DE_HEADLESS */

TEST(devices, headless_build_stubs) {
    ASSERT_EQ(de_platform_init(NULL), DE_OK);
    de_raw_input_event ev[2];
    EXPECT_EQ(de_platform_input_events(ev, 2), 0u);
    EXPECT_EQ(de_platform_pads(NULL, 0), 0u);
    EXPECT_EQ(de_platform_pad_rumble(1, 1.0f, 1.0f, 10), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_platform_clipboard_set("x"), DE_ERR_UNSUPPORTED);
    EXPECT_EQ(de_platform_cursor_show(true), DE_ERR_UNSUPPORTED);
    de_file_dialog_desc fd = {0};
    EXPECT_EQ(de_platform_file_dialog(&fd, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    de_platform_shutdown();
}

#endif
