/* input_test_util.h: event injection helpers for the tests of the input module (DENG-83 ..
 * DENG-89). Each helper queues one raw event with de__input_inject(); input_test_frame() starts
 * the next frame, which applies them in order. No SDL, no platform, no devices.
 *
 *     de_input *in = input_test_create(NULL);
 *     input_test_key(in, DE_KEY_W, true);
 *     input_test_pad_add(in, 7, DE_PLATFORM_PAD_PS5, DE_PLATFORM_PAD_CAP_GYRO, "DualSense");
 *     input_test_frame(in);
 *     de_gamepad p = input_test_pad(in, 7);
 */
#ifndef DE_INPUT_TEST_UTIL_H
#define DE_INPUT_TEST_UTIL_H
#include "input/input_internal.h"

#include <string.h>

/** Frame delta the helpers use (60 Hz). */
#define INPUT_TEST_DT (1.0f / 60.0f)

/** A zeroed raw event of `kind`, stamped with a fake increasing host time. */
static inline de__input_event input_test_event(de_raw_input_kind kind) {
    static u64 s_time_ns = 1000000;
    de__input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.time_ns = (s_time_ns += 1000);
    return ev;
}

/** de_input_create() with an allocator (NULL: the heap) and defaults; NULL on failure. */
static inline de_input *input_test_create(const de_allocator *a) {
    de_input_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = a;
    de_input *in = NULL;
    if (de_input_create(&d, &in) != DE_OK)
        return NULL;
    return in;
}

/** Starts the next frame with dt = INPUT_TEST_DT (applies the queued events). */
static inline void input_test_frame(de_input *in) {
    de__input_begin_frame_dt(in, INPUT_TEST_DT);
}

static inline void input_test_key_ex(de_input *in, de_key k, bool down, bool repeat, u16 mods) {
    de__input_event ev = input_test_event(DE_RAW_KEY);
    ev.key.code = (u16)k;
    ev.key.down = down;
    ev.key.repeat = repeat;
    ev.key.mods = mods;
    de__input_inject(in, &ev);
}
static inline void input_test_key(de_input *in, de_key k, bool down) {
    input_test_key_ex(in, k, down, false, 0);
}

static inline void input_test_mouse_move(de_input *in, f32 x, f32 y, f32 dx, f32 dy) {
    de__input_event ev = input_test_event(DE_RAW_MOUSE_MOVE);
    ev.mouse_move.x = x;
    ev.mouse_move.y = y;
    ev.mouse_move.dx = dx;
    ev.mouse_move.dy = dy;
    de__input_inject(in, &ev);
}

static inline void input_test_mouse_button(de_input *in, de_mouse_button b, bool down) {
    de__input_event ev = input_test_event(DE_RAW_MOUSE_BUTTON);
    ev.mouse_button.button = (u8)b;
    ev.mouse_button.down = down;
    ev.mouse_button.clicks = 1;
    de__input_inject(in, &ev);
}

static inline void input_test_mouse_wheel(de_input *in, f32 dx, f32 dy) {
    de__input_event ev = input_test_event(DE_RAW_MOUSE_WHEEL);
    ev.mouse_wheel.dx = dx;
    ev.mouse_wheel.dy = dy;
    de__input_inject(in, &ev);
}

/** Connects pad `device` (instance id, not 0) with the given platform family, platform caps
 * (de_platform_pad_cap) and name. */
static inline void input_test_pad_add(de_input *in, u32 device, de_platform_pad_type type,
                                      u32 platform_caps, const char *name) {
    de_platform_pad_info info;
    memset(&info, 0, sizeof info);
    info.id = device;
    info.type = type;
    info.caps = platform_caps;
    info.player_index = -1;
    if (name != NULL) {
        size_t n = strlen(name);
        if (n >= sizeof info.name)
            n = sizeof info.name - 1;
        memcpy(info.name, name, n);
    }
    de__input_inject_pad_info(in, &info);
    de__input_event ev = input_test_event(DE_RAW_PAD_ADDED);
    ev.device = device;
    de__input_inject(in, &ev);
}

static inline void input_test_pad_remove(de_input *in, u32 device) {
    de__input_event ev = input_test_event(DE_RAW_PAD_REMOVED);
    ev.device = device;
    de__input_inject(in, &ev);
}

static inline void input_test_pad_button(de_input *in, u32 device, de_pad_button b, bool down) {
    de__input_event ev = input_test_event(DE_RAW_PAD_BUTTON);
    ev.device = device;
    ev.pad_button.button = (u8)b;
    ev.pad_button.down = down;
    de__input_inject(in, &ev);
}

/** Queues a raw axis value, in the PLATFORM convention (sticks +y down). */
static inline void input_test_pad_axis(de_input *in, u32 device, de_pad_axis a, f32 raw_value) {
    de__input_event ev = input_test_event(DE_RAW_PAD_AXIS);
    ev.device = device;
    ev.pad_axis.axis = (u8)a;
    ev.pad_axis.value = raw_value;
    de__input_inject(in, &ev);
}

static inline void input_test_pad_sensor(de_input *in, u32 device, de_platform_sensor_type type,
                                         f32 x, f32 y, f32 z, u64 sensor_time_ns) {
    de__input_event ev = input_test_event(DE_RAW_PAD_SENSOR);
    ev.device = device;
    ev.sensor.sensor = (u32)type;
    ev.sensor.data[0] = x;
    ev.sensor.data[1] = y;
    ev.sensor.data[2] = z;
    ev.sensor.sensor_time_ns = sensor_time_ns;
    de__input_inject(in, &ev);
}

/** Queues committed text; `utf8` must be at most 31 bytes (one raw event). */
static inline void input_test_text(de_input *in, const char *utf8) {
    de__input_event ev = input_test_event(DE_RAW_TEXT);
    size_t n = strlen(utf8);
    if (n >= sizeof ev.text.utf8)
        n = sizeof ev.text.utf8 - 1;
    memcpy(ev.text.utf8, utf8, n);
    de__input_inject(in, &ev);
}

/** Queues an IME composition string with its cursor and selection length. */
static inline void input_test_text_edit(de_input *in, const char *utf8, i32 start, i32 length) {
    de__input_event ev = input_test_event(DE_RAW_TEXT_EDIT);
    size_t n = strlen(utf8);
    if (n >= sizeof ev.text.utf8)
        n = sizeof ev.text.utf8 - 1;
    memcpy(ev.text.utf8, utf8, n);
    ev.text.start = start;
    ev.text.length = length;
    de__input_inject(in, &ev);
}

/** The handle of connected pad `device`, or an invalid handle. */
static inline de_gamepad input_test_pad(const de_input *in, u32 device) {
    for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i)
        if (in->pads[i].device == device && device != 0)
            return de__input_pad_handle(in, i);
    return (de_gamepad){0};
}

#endif
