/* test_input_state.c: the engine input state built from raw platform events (DENG-83). Events are
 * injected with de__input_inject (input_test_util.h), so no device or SDL is needed. */
#include "input_test_util.h"

#include "de_alloc_guard.h"
#include "de_test.h"

#include <dengine/de_reflect.h>

#include <string.h>

/* ---- Keys ------------------------------------------------------------------------------------ */

TEST(input_state, key_edges) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);

    input_test_frame(in);
    EXPECT_FALSE(de_key_down(in, DE_KEY_W));

    input_test_key(in, DE_KEY_W, true);
    input_test_frame(in);
    EXPECT_TRUE(de_key_down(in, DE_KEY_W));
    EXPECT_TRUE(de_key_pressed(in, DE_KEY_W));
    EXPECT_FALSE(de_key_released(in, DE_KEY_W));
    EXPECT_FALSE(de_key_down(in, DE_KEY_S));

    /* Held: no new edge. */
    input_test_frame(in);
    EXPECT_TRUE(de_key_down(in, DE_KEY_W));
    EXPECT_FALSE(de_key_pressed(in, DE_KEY_W));

    input_test_key(in, DE_KEY_W, false);
    input_test_frame(in);
    EXPECT_FALSE(de_key_down(in, DE_KEY_W));
    EXPECT_FALSE(de_key_pressed(in, DE_KEY_W));
    EXPECT_TRUE(de_key_released(in, DE_KEY_W));

    input_test_frame(in);
    EXPECT_FALSE(de_key_released(in, DE_KEY_W));

    /* An up without a down, and codes out of range, change nothing. */
    input_test_key(in, DE_KEY_E, false);
    input_test_frame(in);
    EXPECT_FALSE(de_key_released(in, DE_KEY_E));
    EXPECT_FALSE(de_key_down(in, DE_KEY_COUNT_));
    EXPECT_FALSE(de_key_down(in, DE_KEY_UNKNOWN));
    EXPECT_FALSE(de_key_down(NULL, DE_KEY_W));

    /* Every raw event of the frame is listed, in order. */
    input_test_key(in, DE_KEY_A, true);
    input_test_mouse_move(in, 1.0f, 2.0f, 1.0f, 0.0f);
    input_test_key(in, DE_KEY_A, false);
    input_test_frame(in);
    ASSERT_EQ(in->frame_event_count, 3u);
    EXPECT_EQ((int)in->frame_events[0].kind, (int)DE_RAW_KEY);
    EXPECT_EQ((int)in->frame_events[1].kind, (int)DE_RAW_MOUSE_MOVE);
    EXPECT_EQ((int)in->frame_events[2].kind, (int)DE_RAW_KEY);
    EXPECT_FALSE(in->frame_events[2].key.down);

    de_input_destroy(in);
}

TEST(input_state, key_down_up_same_frame) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);

    input_test_key(in, DE_KEY_SPACE, true);
    input_test_key(in, DE_KEY_SPACE, false);
    input_test_frame(in);
    EXPECT_TRUE(de_key_pressed(in, DE_KEY_SPACE));
    EXPECT_TRUE(de_key_released(in, DE_KEY_SPACE));
    EXPECT_FALSE(de_key_down(in, DE_KEY_SPACE));

    input_test_frame(in);
    EXPECT_FALSE(de_key_pressed(in, DE_KEY_SPACE));
    EXPECT_FALSE(de_key_released(in, DE_KEY_SPACE));

    /* Held key: up and down again in one frame gives both edges and stays down. */
    input_test_key(in, DE_KEY_SPACE, true);
    input_test_frame(in);
    input_test_key(in, DE_KEY_SPACE, false);
    input_test_key(in, DE_KEY_SPACE, true);
    input_test_frame(in);
    EXPECT_TRUE(de_key_released(in, DE_KEY_SPACE));
    EXPECT_TRUE(de_key_pressed(in, DE_KEY_SPACE));
    EXPECT_TRUE(de_key_down(in, DE_KEY_SPACE));

    /* Mouse and pad buttons follow the same rule. */
    input_test_mouse_button(in, DE_MOUSE_RIGHT, true);
    input_test_mouse_button(in, DE_MOUSE_RIGHT, false);
    input_test_pad_add(in, 5, DE_PLATFORM_PAD_XBOX, 0, "Xbox");
    input_test_pad_button(in, 5, DE_PAD_SOUTH, true);
    input_test_pad_button(in, 5, DE_PAD_SOUTH, false);
    input_test_frame(in);
    EXPECT_TRUE(de_mouse_pressed(in, DE_MOUSE_RIGHT));
    EXPECT_TRUE(de_mouse_released(in, DE_MOUSE_RIGHT));
    EXPECT_FALSE(de_mouse_down(in, DE_MOUSE_RIGHT));
    const de_gamepad p = input_test_pad(in, 5);
    EXPECT_TRUE(de_pad_pressed(in, p, DE_PAD_SOUTH));
    EXPECT_TRUE(de_pad_released(in, p, DE_PAD_SOUTH));
    EXPECT_FALSE(de_pad_down(in, p, DE_PAD_SOUTH));

    de_input_destroy(in);
}

TEST(input_state, key_repeat_not_pressed) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);

    input_test_key(in, DE_KEY_BACKSPACE, true);
    input_test_frame(in);
    EXPECT_TRUE(de_key_pressed(in, DE_KEY_BACKSPACE));

    input_test_key_ex(in, DE_KEY_BACKSPACE, true, true, 0);
    input_test_key_ex(in, DE_KEY_BACKSPACE, true, true, 0);
    input_test_frame(in);
    EXPECT_TRUE(de_key_down(in, DE_KEY_BACKSPACE));
    EXPECT_FALSE(de_key_pressed(in, DE_KEY_BACKSPACE));
    EXPECT_EQ(in->frame_event_count, 2u); /* still listed */

    /* A repeat of a key that is not down (e.g. after a focus loss) does not press it. */
    input_test_key_ex(in, DE_KEY_X, true, true, 0);
    input_test_frame(in);
    EXPECT_FALSE(de_key_down(in, DE_KEY_X));
    EXPECT_FALSE(de_key_pressed(in, DE_KEY_X));

    de_input_destroy(in);
}

TEST(input_state, key_names) {
    EXPECT_STREQ(de_key_name(DE_KEY_A), "a");
    EXPECT_STREQ(de_key_name(DE_KEY_1), "1");
    EXPECT_STREQ(de_key_name(DE_KEY_RETURN), "return");
    EXPECT_STREQ(de_key_name(DE_KEY_LSHIFT), "lshift");
    EXPECT_STREQ(de_key_name(DE_KEY_KP_ENTER), "kp_enter");
    EXPECT_STREQ(de_key_name(DE_KEY_MEDIA_PLAY), "media_play");
    EXPECT_TRUE(de_key_name(DE_KEY_UNKNOWN) == NULL);
    EXPECT_TRUE(de_key_name((de_key)130) == NULL); /* no key at 130 */
    EXPECT_TRUE(de_key_name(DE_KEY_COUNT_) == NULL);
    EXPECT_EQ((int)de_key_from_name("Space"), (int)DE_KEY_SPACE);
    EXPECT_EQ((int)de_key_from_name("f12"), (int)DE_KEY_F12);
    EXPECT_EQ((int)de_key_from_name("nope"), (int)DE_KEY_UNKNOWN);
    EXPECT_EQ((int)de_key_from_name(""), (int)DE_KEY_UNKNOWN);
    EXPECT_EQ((int)de_key_from_name(NULL), (int)DE_KEY_UNKNOWN);
    /* Every named key round-trips. */
    for (u32 k = 1; k < DE_KEY_COUNT_; ++k) {
        const char *n = de_key_name((de_key)k);
        if (n != NULL)
            EXPECT_EQ((u32)de_key_from_name(n), k);
    }
}

/* ---- Mouse ----------------------------------------------------------------------------------- */

TEST(input_state, mouse_delta_wheel_accumulate) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);

    input_test_mouse_move(in, 10.0f, 20.0f, 1.0f, 2.0f);
    input_test_mouse_move(in, 13.0f, 18.0f, 3.0f, -2.0f);
    input_test_mouse_move(in, 13.5f, 18.0f, 0.5f, 0.0f);
    input_test_mouse_wheel(in, 0.0f, 1.0f);
    input_test_mouse_wheel(in, 0.25f, 2.0f);
    input_test_mouse_button(in, DE_MOUSE_LEFT, true);
    input_test_frame(in);
    de_vec2 d = de_mouse_delta(in);
    EXPECT_FLOAT_EQ(d.x, 4.5f, 1e-6);
    EXPECT_FLOAT_EQ(d.y, 0.0f, 1e-6);
    de_vec2 w = de_mouse_wheel(in);
    EXPECT_FLOAT_EQ(w.x, 0.25f, 1e-6);
    EXPECT_FLOAT_EQ(w.y, 3.0f, 1e-6);
    de_vec2 p = de_mouse_position(in);
    EXPECT_FLOAT_EQ(p.x, 0.0f, 1e-6); /* the button event (x 0, y 0) came last */
    EXPECT_TRUE(de_mouse_pressed(in, DE_MOUSE_LEFT));
    EXPECT_TRUE(de_mouse_down(in, DE_MOUSE_LEFT));
    EXPECT_EQ((int)de_input_last_device(in, NULL), (int)DE_INPUT_DEVICE_KEYBOARD_MOUSE);

    /* Deltas reset each frame; the position stays. */
    input_test_mouse_move(in, 40.0f, 50.0f, 2.0f, 2.0f);
    input_test_frame(in);
    p = de_mouse_position(in);
    EXPECT_FLOAT_EQ(p.x, 40.0f, 1e-6);
    EXPECT_FLOAT_EQ(p.y, 50.0f, 1e-6);
    input_test_frame(in);
    d = de_mouse_delta(in);
    w = de_mouse_wheel(in);
    EXPECT_FLOAT_EQ(d.x, 0.0f, 1e-6);
    EXPECT_FLOAT_EQ(d.y, 0.0f, 1e-6);
    EXPECT_FLOAT_EQ(w.y, 0.0f, 1e-6);
    p = de_mouse_position(in);
    EXPECT_FLOAT_EQ(p.x, 40.0f, 1e-6);
    EXPECT_TRUE(de_mouse_down(in, DE_MOUSE_LEFT));
    EXPECT_FALSE(de_mouse_pressed(in, DE_MOUSE_LEFT));

    /* Buttons beyond X2 are only listed. */
    de__input_event ev = input_test_event(DE_RAW_MOUSE_BUTTON);
    ev.mouse_button.button = 7;
    ev.mouse_button.down = true;
    de__input_inject(in, &ev);
    input_test_frame(in);
    EXPECT_EQ(in->frame_event_count, 1u);
    EXPECT_FALSE(de_mouse_down(in, DE_MOUSE_BUTTON_COUNT_));

    de_input_destroy(in);
}

TEST(input_state, relative_mode) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    EXPECT_FALSE(de_mouse_relative(in));

    input_test_mouse_move(in, 100.0f, 100.0f, 0.0f, 0.0f);
    input_test_frame(in);

    /* No window is known (no platform): the mode is kept and the state follows it. */
    de_mouse_set_relative(in, true);
    EXPECT_TRUE(de_mouse_relative(in));
    input_test_mouse_move(in, 100.0f, 100.0f, 25.0f, -5.0f);
    input_test_mouse_move(in, 100.0f, 100.0f, 15.0f, -5.0f);
    input_test_frame(in);
    de_vec2 d = de_mouse_delta(in);
    EXPECT_FLOAT_EQ(d.x, 40.0f, 1e-6);
    EXPECT_FLOAT_EQ(d.y, -10.0f, 1e-6);
    de_vec2 p = de_mouse_position(in);
    EXPECT_FLOAT_EQ(p.x, 100.0f, 1e-6);
    EXPECT_FLOAT_EQ(p.y, 100.0f, 1e-6);

    /* Positions reported in relative mode are not taken. */
    input_test_mouse_move(in, 3.0f, 4.0f, 1.0f, 1.0f);
    input_test_frame(in);
    p = de_mouse_position(in);
    EXPECT_FLOAT_EQ(p.x, 100.0f, 1e-6);

    de_mouse_set_relative(in, false);
    EXPECT_FALSE(de_mouse_relative(in));
    input_test_mouse_move(in, 3.0f, 4.0f, 1.0f, 1.0f);
    input_test_frame(in);
    p = de_mouse_position(in);
    EXPECT_FLOAT_EQ(p.x, 3.0f, 1e-6);
    EXPECT_FLOAT_EQ(p.y, 4.0f, 1e-6);

    de_input_destroy(in);
}

/* ---- Gamepads -------------------------------------------------------------------------------- */

static void expect_stale(const de_input *in, de_gamepad p) {
    EXPECT_FALSE(de_pad_connected(in, p));
    for (u32 b = 0; b < DE_PAD_BUTTON_COUNT_; ++b) {
        EXPECT_FALSE(de_pad_down(in, p, (de_pad_button)b));
        EXPECT_FALSE(de_pad_pressed(in, p, (de_pad_button)b));
        EXPECT_FALSE(de_pad_released(in, p, (de_pad_button)b));
    }
    for (u32 a = 0; a < DE_PAD_AXIS_COUNT_; ++a)
        EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, (de_pad_axis)a), 0.0f, 0.0);
    de_gamepad_info info;
    memset(&info, 0xAB, sizeof info);
    EXPECT_FALSE(de_pad_info(in, p, &info));
    EXPECT_EQ(info.caps, 0u);
    de_vec3 v = {1.0f, 1.0f, 1.0f};
    EXPECT_FALSE(de_pad_gyro(in, p, &v));
    EXPECT_FLOAT_EQ(v.x, 0.0f, 0.0);
    EXPECT_FALSE(de_pad_accel(in, p, &v));
}

TEST(input_state, pad_hotplug_generation) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    EXPECT_EQ(de_gamepad_list(in, NULL, 0), 0u);

    input_test_pad_add(in, 100, DE_PLATFORM_PAD_XBOX, 0, "A");
    input_test_pad_add(in, 200, DE_PLATFORM_PAD_PS4, 0, "B");
    input_test_pad_add(in, 300, DE_PLATFORM_PAD_GENERIC, 0, "C");
    input_test_pad_add(in, 300, DE_PLATFORM_PAD_GENERIC, 0, "C"); /* duplicate add: ignored */
    input_test_frame(in);
    de_gamepad list[DE_INPUT_MAX_PADS];
    ASSERT_EQ(de_gamepad_list(in, list, DE_INPUT_MAX_PADS), 3u);
    const de_gamepad a = input_test_pad(in, 100), b = input_test_pad(in, 200),
                     c = input_test_pad(in, 300);
    EXPECT_EQ(list[0].id, a.id);
    EXPECT_EQ(list[1].id, b.id);
    EXPECT_EQ(list[2].id, c.id);
    EXPECT_EQ(de_gamepad_list(in, list, 1), 3u); /* count, even with a short array */

    input_test_pad_button(in, 200, DE_PAD_NORTH, true);
    input_test_pad_axis(in, 200, DE_AXIS_LEFT_X, 0.75f);
    input_test_frame(in);
    EXPECT_TRUE(de_pad_down(in, b, DE_PAD_NORTH));
    EXPECT_EQ((int)de_input_last_device(in, NULL), (int)DE_INPUT_DEVICE_GAMEPAD);

    /* Disconnect: the handle is stale for every query, and the list closes the gap. */
    input_test_pad_remove(in, 200);
    input_test_frame(in);
    expect_stale(in, b);
    EXPECT_EQ((int)de_input_last_device(in, NULL), (int)DE_INPUT_DEVICE_NONE);
    ASSERT_EQ(de_gamepad_list(in, list, DE_INPUT_MAX_PADS), 2u);
    EXPECT_EQ(list[0].id, a.id);
    EXPECT_EQ(list[1].id, c.id);

    /* Reconnect: same slot, new generation; the old handle stays stale; it is listed last. */
    input_test_pad_add(in, 200, DE_PLATFORM_PAD_PS4, 0, "B");
    input_test_frame(in);
    const de_gamepad b2 = input_test_pad(in, 200);
    EXPECT_TRUE(DE_HANDLE_VALID(b2));
    EXPECT_NE(b2.id, b.id);
    EXPECT_TRUE(de_pad_connected(in, b2));
    expect_stale(in, b);
    ASSERT_EQ(de_gamepad_list(in, list, DE_INPUT_MAX_PADS), 3u);
    EXPECT_EQ(list[2].id, b2.id);

    /* Events of unknown pads, invalid handles and forged slots are ignored. */
    input_test_pad_button(in, 999, DE_PAD_SOUTH, true);
    input_test_frame(in);
    expect_stale(in, (de_gamepad){0});
    expect_stale(in, (de_gamepad){0xFFFFFFFFu});
    expect_stale(in, (de_gamepad){(b2.id & ~0xFu) | 0xFu});

    /* More pads than slots: the extra ones are ignored. */
    for (u32 i = 0; i < DE_INPUT_MAX_PADS; ++i)
        input_test_pad_add(in, 1000 + i, DE_PLATFORM_PAD_GENERIC, 0, "extra");
    input_test_frame(in);
    EXPECT_EQ(de_gamepad_list(in, NULL, 0), (u32)DE_INPUT_MAX_PADS);
    EXPECT_EQ(in->pads_ignored, 3u);
    EXPECT_FALSE(DE_HANDLE_VALID(input_test_pad(in, 1000 + DE_INPUT_MAX_PADS - 1)));

    de_input_destroy(in);
}

TEST(input_state, pad_axis_normalization) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    input_test_pad_add(in, 9, DE_PLATFORM_PAD_PS5,
                       DE_PLATFORM_PAD_CAP_RUMBLE | DE_PLATFORM_PAD_CAP_GYRO |
                           DE_PLATFORM_PAD_CAP_ACCEL | DE_PLATFORM_PAD_CAP_TOUCHPAD |
                           DE_PLATFORM_PAD_CAP_LED,
                       "DualSense Wireless Controller");
    input_test_frame(in);
    const de_gamepad p = input_test_pad(in, 9);
    ASSERT_TRUE(de_pad_connected(in, p));

    de_gamepad_info info;
    ASSERT_TRUE(de_pad_info(in, p, &info));
    EXPECT_EQ((int)info.type, (int)DE_PAD_TYPE_PS5);
    EXPECT_EQ(info.caps, (u32)(DE_PAD_CAP_RUMBLE | DE_PAD_CAP_ADAPTIVE_TRIGGERS | DE_PAD_CAP_GYRO |
                               DE_PAD_CAP_ACCEL | DE_PAD_CAP_TOUCHPAD | DE_PAD_CAP_LED));
    EXPECT_STREQ(info.name, "DualSense Wireless Controller");
    EXPECT_EQ(info.player_index, -1);

    /* Platform: +y down. Engine: +y up. */
    input_test_pad_axis(in, 9, DE_AXIS_LEFT_X, 1.0f);
    input_test_pad_axis(in, 9, DE_AXIS_LEFT_Y, 1.0f); /* pulled back */
    input_test_pad_axis(in, 9, DE_AXIS_RIGHT_X, -0.5f);
    input_test_pad_axis(in, 9, DE_AXIS_RIGHT_Y, -0.25f); /* pushed forward */
    input_test_pad_axis(in, 9, DE_AXIS_LEFT_TRIGGER, 0.5f);
    input_test_pad_axis(in, 9, DE_AXIS_RIGHT_TRIGGER, 1.0f);
    input_test_frame(in);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_LEFT_X), 1.0f, 0.0);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_LEFT_Y), -1.0f, 0.0);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_RIGHT_X), -0.5f, 0.0);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_RIGHT_Y), 0.25f, 0.0);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_LEFT_TRIGGER), 0.5f, 0.0);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_RIGHT_TRIGGER), 1.0f, 0.0);

    /* Out of range values are clamped; NaN becomes 0; no dead zone (tiny values stay). */
    input_test_pad_axis(in, 9, DE_AXIS_LEFT_X, -1.5f);
    input_test_pad_axis(in, 9, DE_AXIS_LEFT_Y, -3.0f);
    input_test_pad_axis(in, 9, DE_AXIS_RIGHT_X, 0.001f);
    input_test_pad_axis(in, 9, DE_AXIS_LEFT_TRIGGER, -0.2f);
    input_test_pad_axis(in, 9, DE_AXIS_RIGHT_TRIGGER, 1.3f);
    volatile f32 zero = 0.0f;
    input_test_pad_axis(in, 9, DE_AXIS_RIGHT_Y, zero / zero);
    input_test_frame(in);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_LEFT_X), -1.0f, 0.0);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_LEFT_Y), 1.0f, 0.0);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_RIGHT_X), 0.001f, 0.0);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_LEFT_TRIGGER), 0.0f, 0.0);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_RIGHT_TRIGGER), 1.0f, 0.0);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_RIGHT_Y), 0.0f, 0.0);
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_PAD_AXIS_COUNT_), 0.0f, 0.0);

    /* Small stick noise does not take the "last device"; a deflection past 0.5 does. */
    input_test_key(in, DE_KEY_A, true);
    input_test_frame(in);
    EXPECT_EQ((int)de_input_last_device(in, NULL), (int)DE_INPUT_DEVICE_KEYBOARD_MOUSE);
    input_test_pad_axis(in, 9, DE_AXIS_LEFT_X, 0.2f);
    input_test_frame(in);
    EXPECT_EQ((int)de_input_last_device(in, NULL), (int)DE_INPUT_DEVICE_KEYBOARD_MOUSE);
    input_test_pad_axis(in, 9, DE_AXIS_LEFT_X, 0.6f);
    input_test_frame(in);
    de_gamepad last = {0};
    EXPECT_EQ((int)de_input_last_device(in, &last), (int)DE_INPUT_DEVICE_GAMEPAD);
    EXPECT_EQ(last.id, p.id);

    /* A generic pad without metadata from the platform: generic, no caps. */
    input_test_pad_add(in, 10, DE_PLATFORM_PAD_SWITCH, DE_PLATFORM_PAD_CAP_TRIGGER_RUMBLE, NULL);
    input_test_frame(in);
    ASSERT_TRUE(de_pad_info(in, input_test_pad(in, 10), &info));
    EXPECT_EQ((int)info.type, (int)DE_PAD_TYPE_SWITCH);
    EXPECT_EQ(info.caps, (u32)DE_PAD_CAP_TRIGGER_RUMBLE);
    de__input_event ev = input_test_event(DE_RAW_PAD_ADDED);
    ev.device = 11; /* no injected info and no platform: a generic pad */
    de__input_inject(in, &ev);
    input_test_frame(in);
    ASSERT_TRUE(de_pad_info(in, input_test_pad(in, 11), &info));
    EXPECT_EQ((int)info.type, (int)DE_PAD_TYPE_GENERIC);
    EXPECT_EQ(info.caps, 0u);
    EXPECT_STREQ(info.name, "Gamepad");

    de_input_destroy(in);
}

TEST(input_state, sensor_samples) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    input_test_pad_add(in, 3, DE_PLATFORM_PAD_PS5,
                       DE_PLATFORM_PAD_CAP_GYRO | DE_PLATFORM_PAD_CAP_ACCEL, "DualSense");
    input_test_pad_add(in, 4, DE_PLATFORM_PAD_SWITCH, DE_PLATFORM_PAD_CAP_GYRO, "Joy-Con (R)");
    input_test_frame(in);
    const de_gamepad p = input_test_pad(in, 3), jc = input_test_pad(in, 4);
    de_vec3 v;
    EXPECT_FALSE(de_pad_gyro(in, p, &v)); /* no sample yet */
    EXPECT_FALSE(de_pad_accel(in, p, &v));

    /* Four gyro samples at 250 Hz and one accel sample in one frame. */
    for (u32 i = 0; i < 4; ++i)
        input_test_pad_sensor(in, 3, DE_PLATFORM_SENSOR_GYRO, 0.1f * (f32)(i + 1), -0.5f, 2.0f,
                              4000000ull * (i + 1));
    input_test_pad_sensor(in, 3, DE_PLATFORM_SENSOR_ACCEL, 0.0f, 9.81f, 0.0f, 16000000ull);
    input_test_pad_sensor(in, 4, DE_PLATFORM_SENSOR_GYRO_R, 1.0f, 2.0f, 3.0f, 0);
    input_test_frame(in);

    ASSERT_TRUE(de_pad_gyro(in, p, &v));
    EXPECT_FLOAT_EQ(v.x, 0.4f, 1e-6); /* the latest sample (the stub passes raw values through) */
    EXPECT_FLOAT_EQ(v.y, -0.5f, 1e-6);
    EXPECT_FLOAT_EQ(v.z, 2.0f, 1e-6);
    ASSERT_TRUE(de_pad_accel(in, p, &v));
    EXPECT_FLOAT_EQ(v.y, 9.81f, 1e-6);
    ASSERT_TRUE(de_pad_gyro(in, jc, &v));
    EXPECT_FLOAT_EQ(v.z, 3.0f, 1e-6);
    EXPECT_FALSE(de_pad_accel(in, jc, &v));

    const de__input_pad *slot = de__input_pad_get_const(in, p);
    ASSERT_TRUE(slot != NULL);
    EXPECT_EQ(slot->gyro_samples, 4u);
    EXPECT_EQ(slot->accel_samples, 1u);
    EXPECT_EQ(slot->gyro_sensor_ns, 16000000ull);
    EXPECT_GT(slot->gyro_time_ns, 0ull);
    /* Every sample is in the frame's event list, with its device time, for integration. */
    u32 gyro_events = 0;
    u64 prev_ns = 0;
    for (u32 i = 0; i < in->frame_event_count; ++i) {
        const de__input_event *e = &in->frame_events[i];
        if (e->kind == DE_RAW_PAD_SENSOR && e->device == 3 &&
            e->sensor.sensor == DE_PLATFORM_SENSOR_GYRO) {
            EXPECT_GT(e->sensor.sensor_time_ns, prev_ns);
            prev_ns = e->sensor.sensor_time_ns;
            ++gyro_events;
        }
    }
    EXPECT_EQ(gyro_events, 4u);

    /* The value persists; the per-frame sample count does not. */
    input_test_frame(in);
    EXPECT_TRUE(de_pad_gyro(in, p, &v));
    EXPECT_FLOAT_EQ(v.x, 0.4f, 1e-6);
    EXPECT_EQ(de__input_pad_get_const(in, p)->gyro_samples, 0u);

    /* Sensor samples are not "activity" (a pad on a desk drifts). */
    input_test_key(in, DE_KEY_A, true);
    input_test_frame(in);
    input_test_pad_sensor(in, 3, DE_PLATFORM_SENSOR_GYRO, 1.0f, 1.0f, 1.0f, 20000000ull);
    input_test_frame(in);
    EXPECT_EQ((int)de_input_last_device(in, NULL), (int)DE_INPUT_DEVICE_KEYBOARD_MOUSE);

    de_input_destroy(in);
}

/* ---- Text ------------------------------------------------------------------------------------ */

TEST(input_state, text_input_utf8) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);

    de_text_input_state t;
    input_test_frame(in);
    de_text_input_get(in, &t);
    EXPECT_STREQ(t.text, "");
    EXPECT_EQ(t.text_bytes, 0u);
    EXPECT_STREQ(t.composition, "");
    EXPECT_FALSE(t.active);

    /* "héllo " + "世界" + "🎮": 2-, 3- and 4-byte sequences, joined in order. */
    input_test_text(in, "h\xC3\xA9llo ");
    input_test_text(in, "\xE4\xB8\x96\xE7\x95\x8C");
    input_test_text(in, "\xF0\x9F\x8E\xAE");
    input_test_text_edit(in, "\xE3\x81\x8B", 1, 0); /* か, cursor after it */
    input_test_frame(in);
    de_text_input_get(in, &t);
    EXPECT_STREQ(t.text, "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C\xF0\x9F\x8E\xAE");
    EXPECT_EQ(t.text_bytes, (u32)strlen(t.text));
    EXPECT_FALSE(t.text_dropped);
    EXPECT_STREQ(t.composition, "\xE3\x81\x8B");
    EXPECT_EQ(t.composition_cursor, 1);
    EXPECT_EQ(t.composition_length, 0);

    /* Next frame: no text; the composition stays until the IME changes it. */
    input_test_frame(in);
    de_text_input_get(in, &t);
    EXPECT_EQ(t.text_bytes, 0u);
    EXPECT_STREQ(t.composition, "\xE3\x81\x8B");
    input_test_text_edit(in, "", -1, -1);
    input_test_frame(in);
    de_text_input_get(in, &t);
    EXPECT_STREQ(t.composition, "");

    /* More than DE_INPUT_TEXT_BYTES in a frame: whole chunks are dropped, the rest is kept. */
    const char *chunk = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                        "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"; /* 30 bytes */
    for (u32 i = 0; i < 10; ++i)
        input_test_text(in, chunk);
    input_test_frame(in);
    de_text_input_get(in, &t);
    EXPECT_TRUE(t.text_dropped);
    EXPECT_EQ(t.text_bytes, 240u); /* 8 chunks */
    EXPECT_EQ(strlen(t.text), (size_t)240);
    EXPECT_EQ((u8)t.text[238], 0xC3u); /* ends on a whole code point */

    /* Without a window (no platform), text input cannot start; stop is harmless. */
    EXPECT_NE(de_text_input_start(in, NULL), DE_OK);
    de_error_clear();
    de_text_input_stop(in);
    de_text_input_get(in, &t);
    EXPECT_FALSE(t.active);

    de_text_input_get(NULL, &t);
    EXPECT_STREQ(t.text, "");
    de_input_destroy(in);
}

/* ---- Focus ----------------------------------------------------------------------------------- */

TEST(input_state, focus_loss_releases) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    input_test_pad_add(in, 1, DE_PLATFORM_PAD_XBOX, 0, "Xbox");
    input_test_frame(in);
    const de_gamepad p = input_test_pad(in, 1);

    input_test_key(in, DE_KEY_W, true);
    input_test_key(in, DE_KEY_LSHIFT, true);
    input_test_mouse_button(in, DE_MOUSE_LEFT, true);
    input_test_pad_button(in, 1, DE_PAD_RSHOULDER, true);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_Y, -1.0f);
    input_test_frame(in);
    ASSERT_TRUE(de_key_down(in, DE_KEY_W));

    de_app_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = DE_EV_WINDOW_FOCUS;
    ev.window.id = 0x10001u;
    ev.a = 0;
    de_input_app_event(in, &ev);
    input_test_frame(in);
    EXPECT_FALSE(de_key_down(in, DE_KEY_W));
    EXPECT_FALSE(de_key_down(in, DE_KEY_LSHIFT));
    EXPECT_TRUE(de_key_released(in, DE_KEY_W));
    EXPECT_TRUE(de_key_released(in, DE_KEY_LSHIFT));
    EXPECT_FALSE(de_mouse_down(in, DE_MOUSE_LEFT));
    EXPECT_TRUE(de_mouse_released(in, DE_MOUSE_LEFT));
    EXPECT_FALSE(de_pad_down(in, p, DE_PAD_RSHOULDER));
    EXPECT_TRUE(de_pad_released(in, p, DE_PAD_RSHOULDER));
    EXPECT_FLOAT_EQ(de_pad_axis_value(in, p, DE_AXIS_LEFT_Y), 0.0f, 0.0);
    EXPECT_TRUE(de_pad_connected(in, p));

    /* The released edges last one frame; nothing comes back by itself. */
    input_test_frame(in);
    EXPECT_FALSE(de_key_released(in, DE_KEY_W));
    EXPECT_FALSE(de_key_down(in, DE_KEY_W));

    /* A focus gain records the window; other app events are ignored. */
    ev.a = 1;
    de_input_app_event(in, &ev);
    EXPECT_EQ(in->focus_window.id, 0x10001u);
    ev.kind = DE_EV_WINDOW_RESIZED;
    ev.a = 0;
    de_input_app_event(in, &ev);
    input_test_key(in, DE_KEY_W, true);
    input_test_frame(in);
    EXPECT_TRUE(de_key_pressed(in, DE_KEY_W));
    de_input_app_event(in, NULL);
    de_input_app_event(NULL, &ev);

    de_input_destroy(in);
}

/* ---- Allocation ------------------------------------------------------------------------------ */

static void queue_busy_frame(de_input *in, u32 i) {
    input_test_key(in, (de_key)(DE_KEY_A + (i % 26)), (i & 1) == 0);
    input_test_key_ex(in, DE_KEY_B, true, true, 0);
    input_test_mouse_move(in, (f32)i, (f32)i, 1.0f, -1.0f);
    input_test_mouse_wheel(in, 0.0f, 1.0f);
    input_test_mouse_button(in, DE_MOUSE_MIDDLE, (i & 1) == 0);
    input_test_pad_button(in, 1, DE_PAD_SOUTH, (i & 1) == 0);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.5f);
    input_test_pad_sensor(in, 1, DE_PLATFORM_SENSOR_GYRO, 0.1f, 0.2f, 0.3f, (u64)i * 4000000u);
    input_test_text(in, "abc");
    input_test_text_edit(in, "x", 1, 0);
    if (i % 10 == 5) {
        input_test_pad_add(in, 2, DE_PLATFORM_PAD_PS4, DE_PLATFORM_PAD_CAP_GYRO, "DS4");
        input_test_pad_remove(in, 2);
    }
}

TEST(input_state, no_alloc_per_frame) {
    de_tracking_allocator tracker;
    const de_allocator ta = de_tracking_allocator_init(&tracker, de_heap_allocator(), 0);
    de_input *in = input_test_create(&ta);
    ASSERT_TRUE(in != NULL);
    const de_tracking_stats after_create = de_tracking_allocator_stats(&tracker);
    /* The state block at create, plus one per hook file that keeps state (DENG-87: gyro.c and
     * haptics.c). Nothing after create. */
    EXPECT_EQ(after_create.alloc_count, 3u);

    /* Warm up: first log / error / pad paths of this thread allocate their per-thread state. */
    input_test_pad_add(in, 1, DE_PLATFORM_PAD_XBOX, DE_PLATFORM_PAD_CAP_GYRO, "Xbox");
    input_test_frame(in);
    queue_busy_frame(in, 5);
    input_test_frame(in);
    de_input_begin_frame(in);

    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    for (u32 i = 0; i < 200; ++i) {
        queue_busy_frame(in, i);
        input_test_frame(in);
        de_input_begin_frame(in); /* the public path: platform drain (nothing here) + clock */
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);

    const de_tracking_stats st = de_tracking_allocator_stats(&tracker);
    EXPECT_EQ(st.alloc_count, after_create.alloc_count);
    de_input_destroy(in);
    EXPECT_EQ(de_tracking_allocator_stats(&tracker).free_count, after_create.alloc_count);
}

TEST(input_state, create_args) {
    EXPECT_EQ(de_input_create(NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de_input_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = de_allocator_null();
    de_input *in = (de_input *)(uintptr_t)1;
    EXPECT_EQ(de_input_create(&d, &in), DE_ERR_OUT_OF_MEMORY);
    EXPECT_TRUE(in == NULL);
    de_error_clear();
    ASSERT_EQ(de_input_create(NULL, &in), DE_OK); /* NULL desc: defaults */
    de_input_destroy(in);
    de_input_destroy(NULL);
    de_input_begin_frame(NULL);
}

/* ---- ECS ------------------------------------------------------------------------------------- */

TEST(input_state, attach_registers_resource_and_systems) {
    de_type_registry *types = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &types), DE_OK);
    de_world_desc wd;
    memset(&wd, 0, sizeof wd);
    wd.types = types;
    de_world *w = NULL;
    ASSERT_EQ(de_world_create(&wd, &w), DE_OK);
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);

    ASSERT_EQ(de_input_attach(in, w), DE_OK);
    const de_type_id t = de_type_find(types, "de_input_resource");
    ASSERT_TRUE(DE_HANDLE_VALID(t));
    const de_resource_id rid = de_resource_find(w, t);
    ASSERT_TRUE(DE_HANDLE_VALID(rid));
    const de_input_resource *res = (const de_input_resource *)de_resource_get(w, rid);
    ASSERT_TRUE(res != NULL);
    EXPECT_TRUE(res->input == in);
    EXPECT_TRUE(de_system_enabled(w, in->sys_actions));
    EXPECT_TRUE(de_system_enabled(w, in->sys_fixed));

    /* Running the schedules calls the stub hooks, which change nothing. */
    input_test_key(in, DE_KEY_W, true);
    input_test_pad_add(in, 1, DE_PLATFORM_PAD_XBOX, 0, "Xbox");
    input_test_frame(in);
    u64 keys_before[DE__INPUT_KEY_WORDS];
    memcpy(keys_before, in->keys_down, sizeof keys_before);
    const u32 events_before = in->frame_event_count;
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_FIRST, INPUT_TEST_DT, 0.0f), DE_OK);
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_PRE_UPDATE, INPUT_TEST_DT, 0.0f), DE_OK);
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_FIXED_UPDATE, INPUT_TEST_DT, 0.0f), DE_OK);
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_FIXED_UPDATE, INPUT_TEST_DT, 0.0f), DE_OK);
    EXPECT_EQ(in->action_frame_runs, 1u);
    EXPECT_EQ(in->action_fixed_runs, 2u);
    EXPECT_MEMEQ(keys_before, in->keys_down, sizeof keys_before);
    EXPECT_EQ(in->frame_event_count, events_before);
    EXPECT_TRUE(de_key_pressed(in, DE_KEY_W));

    /* Attaching again to the same world is allowed; a second input state or world is not. */
    EXPECT_EQ(de_input_attach(in, w), DE_OK);
    EXPECT_EQ(de_resource_find(w, t).id, rid.id);
    de_input *other = input_test_create(NULL);
    ASSERT_TRUE(other != NULL);
    EXPECT_EQ(de_input_attach(other, w), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de_world *w2 = NULL;
    ASSERT_EQ(de_world_create(&wd, &w2), DE_OK);
    EXPECT_EQ(de_input_attach(in, w2), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    EXPECT_EQ(de_input_attach(NULL, w), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();

    /* Detach removes both systems and the resource; then another input state may attach. */
    const de_system sa = in->sys_actions, sf = in->sys_fixed;
    de_input_detach(in);
    EXPECT_FALSE(de_system_enabled(w, sa));
    EXPECT_FALSE(de_system_enabled(w, sf));
    EXPECT_TRUE(de_resource_get(w, rid) == NULL);
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_PRE_UPDATE, INPUT_TEST_DT, 0.0f), DE_OK);
    EXPECT_EQ(in->action_frame_runs, 1u);
    de_input_detach(in); /* twice: nothing */
    EXPECT_EQ(de_input_attach(other, w), DE_OK);
    EXPECT_EQ(de_input_attach(in, w2), DE_OK);

    de_world_destroy(w2);
    de_world_destroy(w);
    de_input_destroy(other);
    de_input_destroy(in);
    de_type_registry_destroy(types);
}

/* DENG-606: the request and backend acknowledgement are deliberately separate. */
typedef struct capture_mock {
    de_window focus;
    bool configured[4];
    u32 count, refusals;
    u32 windows[32];
    bool modes[32];
    u64 now;
} capture_mock;
static de_result mock_capture_set(void *user, de_window window, bool on) {
    capture_mock *m = user;
    if (m->count < 32) {
        m->windows[m->count] = window.id;
        m->modes[m->count] = on;
    }
    ++m->count;
    if (m->refusals) {
        --m->refusals;
        return DE_ERR_IO;
    }
    if (window.id >= 4)
        return DE_ERR_INVALID_ARGUMENT;
    m->configured[window.id] = on;
    return DE_OK;
}
static bool mock_capture_configured(void *user, de_window window) {
    capture_mock *m = user;
    return window.id < 4 && m->configured[window.id];
}
static de_window mock_capture_focus(void *user) {
    return ((capture_mock *)user)->focus;
}
static u64 mock_capture_now(void *user) {
    return ((capture_mock *)user)->now;
}
static de_input *mock_capture_input(capture_mock *m) {
    de_input *in = input_test_create(NULL);
    if (in)
        in->capture_sink = (de__input_capture_sink){mock_capture_set, mock_capture_configured,
                                                    mock_capture_focus, mock_capture_now, m};
    return in;
}
TEST(input_state, capture_waits_for_native_focus_without_first_event) {
    capture_mock m = {0};
    de_input *in = mock_capture_input(&m);
    ASSERT_TRUE(in);
    de_mouse_set_relative(in, true);
    EXPECT_EQ(m.count, 0u);
    EXPECT_TRUE(de_mouse_capture_get(in).pending);
    m.focus.id = 1;
    input_test_frame(in);
    EXPECT_EQ(m.count, 1u);
    EXPECT_TRUE(m.configured[1]);
    EXPECT_FALSE(de_mouse_capture_get(in).pending);
    EXPECT_TRUE(de_mouse_capture_get(in).focused);
    de_input_destroy(in);
    EXPECT_FALSE(m.configured[1]);
}
TEST(input_state, capture_refusal_survives_and_retries_with_backoff) {
    capture_mock m = {.focus = {1}, .refusals = 1};
    de_input *in = mock_capture_input(&m);
    ASSERT_TRUE(in);
    de_mouse_set_relative(in, true);
    de_mouse_capture_status status = de_mouse_capture_get(in);
    EXPECT_TRUE(status.requested);
    EXPECT_TRUE(status.pending);
    EXPECT_FALSE(status.configured);
    EXPECT_EQ(status.last_result, DE_ERR_IO);
    EXPECT_EQ(status.attempts, 1u);
    for (u32 i = 0; i < 30; ++i)
        input_test_frame(in);
    EXPECT_EQ(m.count, 1u);
    m.now = 250000000;
    input_test_frame(in);
    status = de_mouse_capture_get(in);
    EXPECT_EQ(status.last_result, DE_OK);
    EXPECT_FALSE(status.pending);
    EXPECT_TRUE(status.configured);
    EXPECT_EQ(m.count, 2u);
    de_input_destroy(in);
}
TEST(input_state, repeated_capture_intent_and_binding_preserve_refusal_backoff) {
    capture_mock m = {.focus = {1}, .refusals = 1};
    de_input *in = mock_capture_input(&m);
    ASSERT_TRUE(in);
    ASSERT_EQ(de_input_set_window(in, (de_window){1}), DE_OK);
    de_mouse_set_relative(in, true);
    for (u32 i = 1; i < 250; ++i) {
        m.now = (u64)i * 1000000;
        ASSERT_EQ(de_input_set_window(in, (de_window){1}), DE_OK);
        de_mouse_set_relative(in, true);
        input_test_frame(in);
    }
    de_mouse_capture_status status = de_mouse_capture_get(in);
    EXPECT_EQ(m.count, 1u);
    EXPECT_TRUE(status.pending);
    EXPECT_FALSE(status.configured);
    EXPECT_EQ(status.last_result, DE_ERR_IO);
    m.now = 250000000;
    de_mouse_set_relative(in, true);
    EXPECT_EQ(m.count, 2u);
    EXPECT_TRUE(de_mouse_capture_get(in).configured);
    EXPECT_FALSE(de_mouse_capture_get(in).pending);
    de_input_destroy(in);
}
TEST(input_state, repeated_capture_release_preserves_refusal_backoff) {
    capture_mock m = {.focus = {1}};
    de_input *in = mock_capture_input(&m);
    ASSERT_TRUE(in);
    de_mouse_set_relative(in, true);
    m.refusals = 1;
    de_mouse_set_relative(in, false);
    for (u32 i = 1; i < 250; ++i) {
        m.now = (u64)i * 1000000;
        de_mouse_set_relative(in, false);
        input_test_frame(in);
    }
    EXPECT_EQ(m.count, 2u);
    EXPECT_FALSE(de_mouse_capture_get(in).requested);
    EXPECT_TRUE(de_mouse_capture_get(in).pending);
    EXPECT_EQ(de_mouse_capture_get(in).last_result, DE_ERR_IO);
    m.now = 250000000;
    input_test_frame(in);
    EXPECT_EQ(m.count, 3u);
    EXPECT_FALSE(de_mouse_capture_get(in).configured);
    EXPECT_FALSE(de_mouse_capture_get(in).pending);
    de_input_destroy(in);
}
TEST(input_state, changed_capture_window_bypasses_old_window_refusal_backoff) {
    capture_mock m = {.focus = {1}, .refusals = 1};
    de_input *in = mock_capture_input(&m);
    ASSERT_TRUE(in);
    ASSERT_EQ(de_input_set_window(in, (de_window){1}), DE_OK);
    de_mouse_set_relative(in, true);
    EXPECT_EQ(m.count, 1u);
    m.focus.id = 2;
    ASSERT_EQ(de_input_set_window(in, (de_window){2}), DE_OK);
    EXPECT_EQ(m.count, 2u);
    EXPECT_TRUE(de_mouse_capture_get(in).configured);
    EXPECT_FALSE(de_mouse_capture_get(in).pending);
    EXPECT_EQ(de_mouse_capture_get(in).last_result, DE_OK);
    de_input_destroy(in);
}
TEST(input_state, capture_focus_loss_same_window_return_and_escape) {
    capture_mock m = {.focus = {1}};
    de_input *in = mock_capture_input(&m);
    ASSERT_TRUE(in);
    de_mouse_set_relative(in, true);
    de_app_event e = {.kind = DE_EV_WINDOW_FOCUS, .window = {1}, .a = 0};
    m.focus.id = 0;
    de_input_app_event(in, &e);
    input_test_frame(in);
    EXPECT_FALSE(m.configured[1]);
    EXPECT_TRUE(de_mouse_relative(in));
    u32 calls = m.count;
    for (u32 i = 0; i < 30; ++i)
        input_test_frame(in);
    EXPECT_EQ(m.count, calls);
    m.focus.id = 1;
    e.a = 1;
    de_input_app_event(in, &e);
    input_test_frame(in);
    EXPECT_TRUE(m.configured[1]);
    de_mouse_set_relative(in, false);
    e.a = 0;
    m.focus.id = 0;
    de_input_app_event(in, &e);
    input_test_frame(in);
    m.focus.id = 1;
    e.a = 1;
    de_input_app_event(in, &e);
    input_test_frame(in);
    EXPECT_FALSE(m.configured[1]);
    EXPECT_FALSE(de_mouse_relative(in));
    de_input_destroy(in);
}
TEST(input_state, capture_rebind_releases_old_before_new_and_stale_window) {
    capture_mock m = {.focus = {1}};
    de_input *in = mock_capture_input(&m);
    ASSERT_TRUE(in);
    de_mouse_set_relative(in, true);
    m.focus.id = 2;
    ASSERT_EQ(de_input_set_window(in, (de_window){2}), DE_OK);
    ASSERT_EQ(m.count, 3u);
    EXPECT_EQ(m.windows[1], 1u);
    EXPECT_FALSE(m.modes[1]);
    EXPECT_EQ(m.windows[2], 2u);
    EXPECT_TRUE(m.modes[2]);
    /* Destroyed windows have no configured capture; don't retry releasing a stale handle. */
    m.configured[2] = false;
    m.focus.id = 3;
    ASSERT_EQ(de_input_set_window(in, (de_window){3}), DE_OK);
    EXPECT_TRUE(m.configured[3]);
    EXPECT_FALSE(de_mouse_capture_get(in).pending);
    de_input_destroy(in);
}
TEST(input_state, capture_escape_cancels_refusal_and_never_recaptures) {
    capture_mock m = {.focus = {1}, .refusals = 1};
    de_input *in = mock_capture_input(&m);
    ASSERT_TRUE(in);
    de_mouse_set_relative(in, true);
    de_mouse_set_relative(in, false);
    m.now = 1000000000;
    for (u32 i = 0; i < 30; ++i)
        input_test_frame(in);
    EXPECT_EQ(m.count, 1u);
    EXPECT_FALSE(m.configured[1]);
    EXPECT_FALSE(de_mouse_capture_get(in).pending);
    de_input_destroy(in);
}
TEST(input_state, capture_ignores_unrelated_focus_loss_for_explicit_window) {
    capture_mock m = {.focus = {1}};
    de_input *in = mock_capture_input(&m);
    ASSERT_TRUE(in);
    ASSERT_EQ(de_input_set_window(in, (de_window){1}), DE_OK);
    de_mouse_set_relative(in, true);
    input_test_key(in, DE_KEY_W, true);
    input_test_frame(in);
    de_app_event e = {.kind = DE_EV_WINDOW_FOCUS, .window = {2}, .a = 0};
    de_input_app_event(in, &e);
    input_test_frame(in);
    EXPECT_TRUE(de_key_down(in, DE_KEY_W));
    EXPECT_TRUE(m.configured[1]);
    de_input_destroy(in);
}
