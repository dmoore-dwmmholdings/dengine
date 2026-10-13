/* test_rebind.c: runtime rebinding (DENG-86): listen mode, the capture rules, suppression of the
 * listening map and of the captured input, conflicts and the apply policies. Input is injected
 * with de__input_inject (input_test_util.h); each frame runs de_input's frame step and then the
 * action hook, as the input.actions system does. */
#include "input_test_util.h"

#include "input/action_internal.h"

#include "de_alloc_guard.h"
#include "de_test.h"

#include <string.h>

/* ---- Helpers --------------------------------------------------------------------------------- */

static void act_frame(de_input *in) {
    input_test_frame(in);
    de__actions_frame_update(in, INPUT_TEST_DT);
}

static de_binding bind(de_binding_source src, u32 code, i8 slot) {
    de_binding b;
    memset(&b, 0, sizeof b);
    b.source = src;
    b.code = code;
    b.composite_slot = slot;
    return b;
}

static de_binding key(de_key k) {
    return bind(DE_BIND_KEY, (u32)k, DE_COMPOSITE_NONE);
}

static de_action add(de_input *in, de_action_map m, const char *name, de_action_type type,
                     const de_binding *b, u32 n, bool rebindable) {
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = type;
    d.interaction = DE_INTERACT_PRESS;
    d.bindings = b;
    d.binding_count = n;
    d.rebindable = rebindable;
    return de_action_add(in, m, &d);
}

static de_binding binding_of(const de_input *in, de_action a, u32 i) {
    de_binding b;
    memset(&b, 0, sizeof b);
    EXPECT_TRUE(de__action_get_binding(in, a, i, &b));
    return b;
}

/* A gameplay map: jump = Space / pad South, interact = F, move = WASD + left stick,
 * look = right stick, fire = right trigger, and a fixed (not rebindable) "pause" on G. */
typedef struct fixture {
    de_input *in;
    de_action_map gameplay;
    de_action jump, interact, move, look, fire, pause;
} fixture;

static void fixture_init(fixture *f) {
    memset(f, 0, sizeof *f);
    f->in = input_test_create(NULL);
    ASSERT_TRUE(f->in != NULL);
    f->gameplay = de_action_map_create(f->in, "gameplay");
    ASSERT_TRUE(DE_HANDLE_VALID(f->gameplay));
    de_action_map_push(f->in, f->gameplay);
    const de_binding jump[2] = {key(DE_KEY_SPACE), bind(DE_BIND_PAD_BUTTON, DE_PAD_SOUTH, -1)};
    f->jump = add(f->in, f->gameplay, "jump", DE_ACTION_BUTTON, jump, 2, true);
    const de_binding interact[1] = {key(DE_KEY_F)};
    f->interact = add(f->in, f->gameplay, "interact", DE_ACTION_BUTTON, interact, 1, true);
    const de_binding move[5] = {
        bind(DE_BIND_KEY, DE_KEY_W, DE_COMPOSITE_UP),
        bind(DE_BIND_KEY, DE_KEY_S, DE_COMPOSITE_DOWN),
        bind(DE_BIND_KEY, DE_KEY_A, DE_COMPOSITE_LEFT),
        bind(DE_BIND_KEY, DE_KEY_D, DE_COMPOSITE_RIGHT),
        bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_NONE),
    };
    f->move = add(f->in, f->gameplay, "move", DE_ACTION_AXIS2D, move, 5, true);
    de_binding look = bind(DE_BIND_PAD_AXIS, DE_BIND_RIGHT_STICK, DE_COMPOSITE_NONE);
    look.deadzone = 0.15f;
    f->look = add(f->in, f->gameplay, "look", DE_ACTION_AXIS2D, &look, 1, true);
    const de_binding fire[1] = {bind(DE_BIND_PAD_AXIS, DE_AXIS_RIGHT_TRIGGER, -1)};
    f->fire = add(f->in, f->gameplay, "fire", DE_ACTION_BUTTON, fire, 1, true);
    const de_binding pause[1] = {key(DE_KEY_G)};
    f->pause = add(f->in, f->gameplay, "pause", DE_ACTION_BUTTON, pause, 1, false);
    ASSERT_TRUE(DE_HANDLE_VALID(f->jump) && DE_HANDLE_VALID(f->interact) &&
                DE_HANDLE_VALID(f->move) && DE_HANDLE_VALID(f->look) && DE_HANDLE_VALID(f->fire) &&
                DE_HANDLE_VALID(f->pause));
    act_frame(f->in);
}

static void pad_on(de_input *in) {
    input_test_pad_add(in, 1, DE_PLATFORM_PAD_XBOX, DE_PLATFORM_PAD_CAP_GYRO, "Xbox");
    act_frame(in);
}

/* Presses and releases key `k` in two frames. */
static void tap_key(de_input *in, de_key k) {
    input_test_key(in, k, true);
    act_frame(in);
    input_test_key(in, k, false);
    act_frame(in);
}

/* ---- Listen mode ----------------------------------------------------------------------------- */

TEST(rebind, listen_key) {
    fixture f;
    fixture_init(&f);
    de_input *in = f.in;
    pad_on(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_IDLE);

    const de_binding_source kb[1] = {DE_BIND_KEY};
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    de_binding got;
    de_action conflict;
    EXPECT_FALSE(de_rebind_poll(in, &got, &conflict));

    /* A keyboard-only slot ignores the pad (buttons, sticks, triggers) and the mouse. */
    input_test_pad_button(in, 1, DE_PAD_SOUTH, true);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.9f);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_TRIGGER, 1.0f);
    input_test_mouse_button(in, DE_MOUSE_LEFT, true);
    act_frame(in);
    EXPECT_FALSE(de_rebind_poll(in, &got, &conflict));
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    input_test_pad_button(in, 1, DE_PAD_SOUTH, false);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.0f);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_TRIGGER, 0.0f);
    input_test_mouse_button(in, DE_MOUSE_LEFT, false);
    act_frame(in);

    /* F captures DE_BIND_KEY / F. */
    input_test_key(in, DE_KEY_F, true);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_CAPTURED);
    ASSERT_TRUE(de_rebind_poll(in, &got, &conflict));
    EXPECT_EQ((u32)got.source, (u32)DE_BIND_KEY);
    EXPECT_EQ(got.code, (u32)DE_KEY_F);
    EXPECT_EQ(got.modifier_key, 0u);
    EXPECT_FALSE(got.invert);
    /* The result is returned once; the binding is not applied by the poll. */
    EXPECT_FALSE(de_rebind_poll(in, &got, &conflict));
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_CAPTURED);
    EXPECT_EQ(binding_of(in, f.jump, 0).code, (u32)DE_KEY_SPACE);

    /* An input held when listening starts is captured only after a new press. */
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    act_frame(in); /* F still held */
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    input_test_key(in, DE_KEY_F, false);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    /* Events of the frame of de_rebind_begin are not scanned: begin, then the same frame's
     * press is already applied, so H (pressed in the frame before begin) does not count. */
    input_test_key(in, DE_KEY_H, true);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_CAPTURED);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ(got.code, (u32)DE_KEY_H);
    input_test_key(in, DE_KEY_H, false);
    act_frame(in);

    input_test_key(in, DE_KEY_J, true);
    act_frame(in);
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING); /* J went down before begin */

    de_rebind_cancel(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_IDLE);
    EXPECT_FALSE(de_rebind_poll(in, &got, &conflict));
    de_input_destroy(in);
}

TEST(rebind, allowed_filter) {
    fixture f;
    fixture_init(&f);
    de_input *in = f.in;
    pad_on(in);

    /* A pad-only slot ignores keys and mouse buttons and takes a pad button. */
    const de_binding_source pad[2] = {DE_BIND_PAD_BUTTON, DE_BIND_PAD_AXIS};
    ASSERT_EQ(de_rebind_begin(in, f.jump, 1, pad, 2), DE_OK);
    input_test_key(in, DE_KEY_K, true);
    input_test_mouse_button(in, DE_MOUSE_RIGHT, true);
    input_test_mouse_wheel(in, 0.0f, 3.0f);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    input_test_pad_button(in, 1, DE_PAD_WEST, true);
    act_frame(in);
    de_binding got;
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ((u32)got.source, (u32)DE_BIND_PAD_BUTTON);
    EXPECT_EQ(got.code, (u32)DE_PAD_WEST);
    input_test_key(in, DE_KEY_K, false);
    input_test_mouse_button(in, DE_MOUSE_RIGHT, false);
    input_test_pad_button(in, 1, DE_PAD_WEST, false);
    act_frame(in);

    /* The default list ignores the mouse delta; it takes the wheel and mouse buttons. */
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, NULL, 0), DE_OK);
    input_test_mouse_move(in, 0.0f, 0.0f, 200.0f, 0.0f);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    input_test_mouse_wheel(in, 0.0f, -1.0f);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ((u32)got.source, (u32)DE_BIND_MOUSE_WHEEL);
    EXPECT_EQ(got.code, DE_BIND_AXIS_Y);
    EXPECT_TRUE(got.invert); /* scrolled towards the user */

    /* The mouse delta when it is listed; +y is up in action space. */
    const de_binding_source mouse[1] = {DE_BIND_MOUSE_DELTA};
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, mouse, 1), DE_OK);
    input_test_mouse_move(in, 0.0f, 0.0f, 3.0f, -10.0f); /* below the threshold */
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    input_test_mouse_move(in, 0.0f, 0.0f, 0.0f, -10.0f); /* 20 points up in total */
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ((u32)got.source, (u32)DE_BIND_MOUSE_DELTA);
    EXPECT_EQ(got.code, DE_BIND_AXIS_Y);
    EXPECT_FALSE(got.invert);

    /* The gyro when it is listed: the axis turned fastest, with its sign. */
    const de_binding_source gyro[1] = {DE_BIND_GYRO};
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, gyro, 1), DE_OK);
    input_test_pad_sensor(in, 1, DE_PLATFORM_SENSOR_GYRO, 0.2f, -0.5f, 0.1f, 1000000);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING); /* below the threshold */
    input_test_pad_sensor(in, 1, DE_PLATFORM_SENSOR_GYRO, 0.2f, -2.0f, 0.1f, 6000000);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ((u32)got.source, (u32)DE_BIND_GYRO);
    EXPECT_EQ(got.code, DE_BIND_AXIS_Y);
    EXPECT_TRUE(got.invert);

    /* A bad list is refused. */
    const de_binding_source bad[1] = {(de_binding_source)99};
    EXPECT_EQ(de_rebind_begin(in, f.jump, 0, bad, 1), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_rebind_begin(in, f.jump, 0, NULL, 2), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de_input_destroy(in);
}

TEST(rebind, axis_threshold_and_sign) {
    fixture f;
    fixture_init(&f);
    de_input *in = f.in;
    pad_on(in);
    const de_binding_source axes[1] = {DE_BIND_PAD_AXIS};

    /* A stick held when listening starts must come back to rest first. */
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.9f);
    act_frame(in);
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, axes, 1), DE_OK);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.95f);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.0f);
    act_frame(in);
    /* Below the threshold: nothing. */
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, -0.45f);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    /* Past it, to the left: left_x with invert, so pushing left reads positive. */
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, -0.7f);
    act_frame(in);
    de_binding got;
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ((u32)got.source, (u32)DE_BIND_PAD_AXIS);
    EXPECT_EQ(got.code, (u32)DE_AXIS_LEFT_X);
    EXPECT_TRUE(got.invert);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.0f);
    act_frame(in);

    /* Stick y: the platform reports +y down; pushing up is +y in action space (no invert). */
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, axes, 1), DE_OK);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_Y, -0.8f);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ(got.code, (u32)DE_AXIS_RIGHT_Y);
    EXPECT_FALSE(got.invert);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_Y, 0.0f);
    act_frame(in);

    /* A composite slot keeps its slot: "move left" bound to the stick pushed left. */
    ASSERT_EQ(de_rebind_begin(in, f.move, 2, axes, 1), DE_OK);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, -1.0f);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ(got.code, (u32)DE_AXIS_LEFT_X);
    EXPECT_EQ((int)got.composite_slot, DE_COMPOSITE_LEFT);
    EXPECT_TRUE(got.invert);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.0f);
    act_frame(in);

    /* A whole-stick slot of a 2D action takes the stick (no sign), keeps the deadzone of the
     * same source, and ignores a trigger, which cannot bind it. */
    ASSERT_EQ(de_rebind_begin(in, f.look, 0, axes, 1), DE_OK);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_TRIGGER, 1.0f);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_Y, 0.9f);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ(got.code, (u32)DE_BIND_LEFT_STICK);
    EXPECT_FALSE(got.invert);
    EXPECT_FLOAT_EQ(got.deadzone, 0.15, 1.0e-6);
    EXPECT_EQ((int)got.composite_slot, DE_COMPOSITE_NONE);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_Y, 0.0f);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_TRIGGER, 0.0f);
    act_frame(in);

    /* A trigger for a button slot. */
    ASSERT_EQ(de_rebind_begin(in, f.fire, 0, axes, 1), DE_OK);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_TRIGGER, 0.6f);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ(got.code, (u32)DE_AXIS_LEFT_TRIGGER);
    EXPECT_FALSE(got.invert);
    de_input_destroy(in);
}

TEST(rebind, chord_capture) {
    fixture f;
    fixture_init(&f);
    de_input *in = f.in;
    const de_binding_source kb[1] = {DE_BIND_KEY};

    /* Shift held, then E: a chord. */
    ASSERT_EQ(de_rebind_begin(in, f.interact, 0, kb, 1), DE_OK);
    input_test_key(in, DE_KEY_LSHIFT, true);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING); /* a modifier alone waits */
    input_test_key(in, DE_KEY_E, true);
    act_frame(in);
    de_binding got;
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ(got.code, (u32)DE_KEY_E);
    EXPECT_EQ(got.modifier_key, (u32)DE_KEY_LSHIFT);
    input_test_key(in, DE_KEY_E, false);
    input_test_key(in, DE_KEY_LSHIFT, false);
    act_frame(in);

    /* A modifier held before listening starts also makes a chord. */
    input_test_key(in, DE_KEY_RCTRL, true);
    act_frame(in);
    ASSERT_EQ(de_rebind_begin(in, f.interact, 0, kb, 1), DE_OK);
    input_test_key(in, DE_KEY_Q, true);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ(got.code, (u32)DE_KEY_Q);
    EXPECT_EQ(got.modifier_key, (u32)DE_KEY_RCTRL);
    input_test_key(in, DE_KEY_Q, false);
    input_test_key(in, DE_KEY_RCTRL, false);
    act_frame(in);

    /* A modifier pressed and released alone binds itself, on its release. */
    ASSERT_EQ(de_rebind_begin(in, f.interact, 0, kb, 1), DE_OK);
    input_test_key(in, DE_KEY_LCTRL, true);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    input_test_key(in, DE_KEY_LCTRL, false);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ(got.code, (u32)DE_KEY_LCTRL);
    EXPECT_EQ(got.modifier_key, 0u);

    /* A modifier that took part in a chord attempt does not bind itself on release. */
    const de_binding_source pad_only[1] = {DE_BIND_PAD_BUTTON};
    ASSERT_EQ(de_rebind_begin(in, f.interact, 0, pad_only, 1), DE_OK);
    input_test_key(in, DE_KEY_LALT, true);
    input_test_key(in, DE_KEY_LALT, false);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING); /* keys not allowed */
    de_input_destroy(in);
}

TEST(rebind, cancel_keys) {
    fixture f;
    fixture_init(&f);
    de_input *in = f.in;
    pad_on(in);
    const de_binding_source kb[1] = {DE_BIND_KEY};
    const de_binding_source padb[1] = {DE_BIND_PAD_BUTTON};

    /* Escape cancels, even for a pad-only slot; no binding is returned. */
    ASSERT_EQ(de_rebind_begin(in, f.jump, 1, padb, 1), DE_OK);
    input_test_key(in, DE_KEY_ESCAPE, true);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_CANCELLED);
    de_binding got;
    EXPECT_FALSE(de_rebind_poll(in, &got, NULL));
    input_test_key(in, DE_KEY_ESCAPE, false);
    act_frame(in);

    /* Pad Start and Guide cancel a keyboard slot. */
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    input_test_pad_button(in, 1, DE_PAD_START, true);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_CANCELLED);
    input_test_pad_button(in, 1, DE_PAD_START, false);
    act_frame(in);
    ASSERT_EQ(de_rebind_begin(in, f.jump, 1, padb, 1), DE_OK);
    input_test_pad_button(in, 1, DE_PAD_GUIDE, true);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_CANCELLED);
    input_test_pad_button(in, 1, DE_PAD_GUIDE, false);
    act_frame(in);

    /* Configurable: Backspace cancels instead, and Escape can be bound. */
    const de_binding cancel[1] = {key(DE_KEY_BACKSPACE)};
    ASSERT_EQ(de_rebind_set_cancel_inputs(in, cancel, 1), DE_OK);
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    input_test_key(in, DE_KEY_ESCAPE, true);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ(got.code, (u32)DE_KEY_ESCAPE);
    input_test_key(in, DE_KEY_ESCAPE, false);
    act_frame(in);
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    tap_key(in, DE_KEY_BACKSPACE);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_CANCELLED);

    /* No cancel input at all; NULL restores the defaults. */
    ASSERT_EQ(de_rebind_set_cancel_inputs(in, cancel, 0), DE_OK);
    ASSERT_EQ(de_rebind_begin(in, f.jump, 1, padb, 1), DE_OK);
    input_test_pad_button(in, 1, DE_PAD_START, true);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    EXPECT_EQ(got.code, (u32)DE_PAD_START);
    input_test_pad_button(in, 1, DE_PAD_START, false);
    act_frame(in);
    ASSERT_EQ(de_rebind_set_cancel_inputs(in, NULL, 0), DE_OK);
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    tap_key(in, DE_KEY_ESCAPE);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_CANCELLED);

    /* Only keys, mouse buttons and pad buttons can cancel. */
    const de_binding bad[1] = {bind(DE_BIND_PAD_AXIS, DE_AXIS_LEFT_X, -1)};
    EXPECT_EQ(de_rebind_set_cancel_inputs(in, bad, 1), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de_input_destroy(in);
}

/* ---- Suppression ----------------------------------------------------------------------------- */

TEST(rebind, capture_suppresses_actions) {
    fixture f;
    fixture_init(&f);
    de_input *in = f.in;
    /* A menu map that hosts the rebinding UI keeps working while listening. */
    const de_action_map menu = de_action_map_create(in, "menu");
    de_action_map_push(in, menu);
    /* (A mouse button: the slot below listens for keys only.) */
    const de_binding confirm_b[1] = {bind(DE_BIND_MOUSE_BUTTON, DE_MOUSE_LEFT, -1)};
    const de_action confirm = add(in, menu, "confirm", DE_ACTION_BUTTON, confirm_b, 1, true);
    ASSERT_TRUE(DE_HANDLE_VALID(confirm));
    pad_on(in);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_Y, -1.0f);
    act_frame(in);
    EXPECT_TRUE(de_action_value(in, f.move) > 0.5f); /* the stick moves while not listening */
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_Y, 0.0f);
    act_frame(in);

    /* Space is held when listening starts: jump reads idle at once (no release trigger). */
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_held(in, f.jump));
    const de_binding_source kb[1] = {DE_BIND_KEY};
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    EXPECT_FALSE(de_action_held(in, f.jump));
    EXPECT_FALSE(de_action_triggered(in, f.jump));

    /* While listening the gameplay map is idle; the menu map is not. */
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_Y, -1.0f); /* move forward (pads are not listened to) */
    input_test_mouse_button(in, DE_MOUSE_LEFT, true);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_LISTENING);
    EXPECT_FALSE(de_action_held(in, f.jump));
    EXPECT_FLOAT_EQ(de_action_value(in, f.move), 0.0, 0.0);
    EXPECT_TRUE(de_action_triggered(in, confirm));
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_Y, 0.0f);
    input_test_mouse_button(in, DE_MOUSE_LEFT, false);
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);
    EXPECT_FALSE(de_action_triggered(in, f.jump));

    /* The capture frame: F fires neither interact (its old action) nor jump. */
    input_test_key(in, DE_KEY_F, true);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_CAPTURED);
    EXPECT_FALSE(de_action_triggered(in, f.interact));
    EXPECT_FALSE(de_action_held(in, f.interact));
    EXPECT_FALSE(de_action_triggered(in, f.jump));
    de__actions_fixed_tick(in, 1);
    de__action_state tick;
    ASSERT_TRUE(de__action_tick_state_get(in, f.interact, &tick));
    EXPECT_FALSE(tick.triggered);
    de_binding got;
    ASSERT_TRUE(de_rebind_poll(in, &got, NULL));
    ASSERT_EQ(de_rebind_apply(in, f.jump, 0, &got, DE_REBIND_ALLOW), DE_OK);

    /* F stays swallowed while held, and its release triggers nothing. */
    act_frame(in);
    EXPECT_FALSE(de_action_triggered(in, f.jump));
    EXPECT_FALSE(de_action_held(in, f.jump));
    EXPECT_FALSE(de_action_triggered(in, f.interact));
    input_test_key(in, DE_KEY_F, false);
    act_frame(in);
    EXPECT_FALSE(de_action_triggered(in, f.jump));
    EXPECT_FALSE(de_action_triggered(in, f.interact));

    /* The next press plays: jump (now F) and interact (ALLOW kept F) both fire. */
    input_test_key(in, DE_KEY_F, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, f.jump));
    EXPECT_TRUE(de_action_triggered(in, f.interact));
    input_test_key(in, DE_KEY_F, false);
    act_frame(in);

    /* A cancel key is swallowed too: Escape bound in the menu does not fire for it. */
    const de_binding back_b[1] = {key(DE_KEY_ESCAPE)};
    const de_action back = add(in, menu, "back", DE_ACTION_BUTTON, back_b, 1, true);
    ASSERT_TRUE(DE_HANDLE_VALID(back));
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    input_test_key(in, DE_KEY_ESCAPE, true);
    act_frame(in);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_CANCELLED);
    EXPECT_FALSE(de_action_triggered(in, back));
    input_test_key(in, DE_KEY_ESCAPE, false);
    act_frame(in);
    EXPECT_FALSE(de_action_triggered(in, back));
    tap_key(in, DE_KEY_ESCAPE);
    input_test_key(in, DE_KEY_ESCAPE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, back));
    de_input_destroy(in);
}

/* ---- Conflicts and policies ------------------------------------------------------------------ */

TEST(rebind, conflict_same_map) {
    fixture f;
    fixture_init(&f);
    de_input *in = f.in;
    pad_on(in);
    /* Another map also uses F: not a conflict while it shares no context set (below it does). */
    const de_action_map vehicle = de_action_map_create(in, "vehicle");
    const de_binding horn_b[1] = {key(DE_KEY_F)};
    ASSERT_TRUE(DE_HANDLE_VALID(add(in, vehicle, "horn", DE_ACTION_BUTTON, horn_b, 1, true)));

    const de_binding_source kb[1] = {DE_BIND_KEY};
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    input_test_key(in, DE_KEY_F, true);
    act_frame(in);
    de_binding got;
    de_action conflict;
    ASSERT_TRUE(de_rebind_poll(in, &got, &conflict));
    EXPECT_EQ(conflict.id, f.interact.id);
    input_test_key(in, DE_KEY_F, false);
    act_frame(in);

    /* A non-rebindable action is not a conflict; a free key has none. */
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    tap_key(in, DE_KEY_G);
    ASSERT_TRUE(de_rebind_poll(in, &got, &conflict));
    EXPECT_EQ(got.code, (u32)DE_KEY_G);
    EXPECT_FALSE(DE_HANDLE_VALID(conflict));
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    tap_key(in, DE_KEY_X);
    ASSERT_TRUE(de_rebind_poll(in, &got, &conflict));
    EXPECT_FALSE(DE_HANDLE_VALID(conflict));

    /* A key used by a composite slot conflicts (W is move up). */
    ASSERT_EQ(de_rebind_begin(in, f.interact, 0, kb, 1), DE_OK);
    tap_key(in, DE_KEY_W);
    ASSERT_TRUE(de_rebind_poll(in, &got, &conflict));
    EXPECT_EQ(conflict.id, f.move.id);

    /* Axes: the left stick (move) overlaps left_x either way; a trigger is its own input. */
    const de_binding_source axes[1] = {DE_BIND_PAD_AXIS};
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, axes, 1), DE_OK);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, -0.9f);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, &conflict));
    EXPECT_EQ(conflict.id, f.move.id);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.0f);
    act_frame(in);
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, axes, 1), DE_OK);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_TRIGGER, 1.0f);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, &conflict));
    EXPECT_EQ(conflict.id, f.fire.id);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_TRIGGER, 0.0f);
    act_frame(in);

    /* A map that shares a named context set (DENG-85) does conflict: gameplay and vehicle in
     * one set, whether or not the set is on the stack. */
    const de_action_map set_maps[2] = {f.gameplay, vehicle};
    ASSERT_EQ(de_input_context_define(in, "driving", set_maps, 2, 0), DE_OK);
    const de_binding hk = key(DE_KEY_H);
    ASSERT_EQ(de_rebind_apply(in, f.interact, 0, &hk, DE_REBIND_ALLOW), DE_OK);
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    tap_key(in, DE_KEY_F);
    ASSERT_TRUE(de_rebind_poll(in, &got, &conflict));
    EXPECT_EQ(conflict.id, de_action_find(in, vehicle, "horn").id);
    /* The slot's own map comes first. */
    const de_binding fk2 = key(DE_KEY_F);
    ASSERT_EQ(de_rebind_apply(in, f.interact, 0, &fk2, DE_REBIND_ALLOW), DE_OK);
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    tap_key(in, DE_KEY_F);
    ASSERT_TRUE(de_rebind_poll(in, &got, &conflict));
    EXPECT_EQ(conflict.id, f.interact.id);
    /* SWAP reaches into the other map of the set: jump takes F, interact and horn take Space. */
    ASSERT_EQ(de_rebind_apply(in, f.jump, 0, &got, DE_REBIND_SWAP), DE_OK);
    EXPECT_EQ(binding_of(in, f.interact, 0).code, (u32)DE_KEY_SPACE);
    EXPECT_EQ(binding_of(in, de_action_find(in, vehicle, "horn"), 0).code, (u32)DE_KEY_SPACE);
    const de_binding space = key(DE_KEY_SPACE);
    ASSERT_EQ(de_rebind_apply(in, f.jump, 0, &space, DE_REBIND_SWAP), DE_OK);

    /* A chord is a different input from the plain key. */
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, kb, 1), DE_OK);
    input_test_key(in, DE_KEY_LSHIFT, true);
    input_test_key(in, DE_KEY_F, true);
    act_frame(in);
    ASSERT_TRUE(de_rebind_poll(in, &got, &conflict));
    EXPECT_EQ(got.modifier_key, (u32)DE_KEY_LSHIFT);
    EXPECT_FALSE(DE_HANDLE_VALID(conflict));
    de_input_destroy(in);
}

TEST(rebind, policy_swap) {
    fixture f;
    fixture_init(&f);
    de_input *in = f.in;

    /* SWAP: jump takes F, interact takes jump's Space. */
    de_binding fk = key(DE_KEY_F);
    ASSERT_EQ(de_rebind_apply(in, f.jump, 0, &fk, DE_REBIND_SWAP), DE_OK);
    EXPECT_EQ(binding_of(in, f.jump, 0).code, (u32)DE_KEY_F);
    EXPECT_EQ(binding_of(in, f.interact, 0).code, (u32)DE_KEY_SPACE);
    EXPECT_EQ(binding_of(in, f.jump, 1).code, (u32)DE_PAD_SOUTH); /* other slot unchanged */
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, f.interact));
    EXPECT_FALSE(de_action_triggered(in, f.jump));
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);

    /* SWAP into a composite slot keeps that slot: move-up (W) and interact (Space). */
    de_binding wk = key(DE_KEY_W);
    ASSERT_EQ(de_rebind_apply(in, f.interact, 0, &wk, DE_REBIND_SWAP), DE_OK);
    const de_binding up = binding_of(in, f.move, 0);
    EXPECT_EQ(up.code, (u32)DE_KEY_SPACE);
    EXPECT_EQ((int)up.composite_slot, DE_COMPOSITE_UP);
    EXPECT_EQ(binding_of(in, f.interact, 0).code, (u32)DE_KEY_W);

    /* CLEAR_OTHER: interact takes D, move-right becomes unbound (its slot stays). */
    de_binding dk = key(DE_KEY_D);
    ASSERT_EQ(de_rebind_apply(in, f.interact, 0, &dk, DE_REBIND_CLEAR_OTHER), DE_OK);
    const de_binding right = binding_of(in, f.move, 3);
    EXPECT_TRUE(de_binding_is_unbound(&right));
    EXPECT_EQ((int)right.composite_slot, DE_COMPOSITE_RIGHT);
    EXPECT_EQ(de__action_binding_count(in, f.move), 5u);
    input_test_key(in, DE_KEY_D, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, f.interact));
    EXPECT_FLOAT_EQ(de_action_value(in, f.move), 0.0, 0.0);
    input_test_key(in, DE_KEY_D, false);
    act_frame(in);

    /* ALLOW keeps the other binding. */
    de_binding ak = key(DE_KEY_A);
    ASSERT_EQ(de_rebind_apply(in, f.jump, 0, &ak, DE_REBIND_ALLOW), DE_OK);
    EXPECT_EQ(binding_of(in, f.move, 2).code, (u32)DE_KEY_A);
    EXPECT_EQ(binding_of(in, f.jump, 0).code, (u32)DE_KEY_A);

    /* Whole sticks: move's slot 4 (left stick) takes the right stick; look (right stick)
     * conflicts and gets the left stick by SWAP, keeping its deadzone (same source). */
    const de_binding look_before = binding_of(in, f.look, 0);
    de_binding rstick = bind(DE_BIND_PAD_AXIS, DE_BIND_RIGHT_STICK, DE_COMPOSITE_NONE);
    ASSERT_EQ(de_rebind_apply(in, f.move, 4, &rstick, DE_REBIND_SWAP), DE_OK);
    EXPECT_EQ(binding_of(in, f.move, 4).code, (u32)DE_BIND_RIGHT_STICK);
    const de_binding look_after = binding_of(in, f.look, 0);
    EXPECT_EQ(look_after.code, (u32)DE_BIND_LEFT_STICK);
    EXPECT_FLOAT_EQ(look_after.deadzone, look_before.deadzone, 0.0);

    /* Bad bindings and policies are refused. */
    de_binding stick_in_slot = bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_UP);
    EXPECT_EQ(de_rebind_apply(in, f.move, 0, &stick_in_slot, DE_REBIND_SWAP),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_rebind_apply(in, f.jump, 0, &fk, (de_rebind_policy)7), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();

    /* de_rebind_clear unbinds one slot. */
    ASSERT_EQ(de_rebind_clear(in, f.jump, 1), DE_OK);
    const de_binding cleared = binding_of(in, f.jump, 1);
    EXPECT_TRUE(de_binding_is_unbound(&cleared));
    de_input_destroy(in);
}

TEST(rebind, swap_refused_when_it_does_not_fit) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = de_action_map_create(in, "gameplay");
    de_action_map_push(in, m);
    /* steer: 1D composite on Q / E; aim: 2D on the whole left stick. */
    const de_binding steer_b[2] = {bind(DE_BIND_KEY, DE_KEY_Q, DE_COMPOSITE_NEGATIVE),
                                   bind(DE_BIND_KEY, DE_KEY_E, DE_COMPOSITE_POSITIVE)};
    const de_action steer = add(in, m, "steer", DE_ACTION_AXIS1D, steer_b, 2, true);
    const de_binding aim_b[1] = {bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_NONE)};
    const de_action aim = add(in, m, "aim", DE_ACTION_AXIS2D, aim_b, 1, true);
    ASSERT_TRUE(DE_HANDLE_VALID(steer) && DE_HANDLE_VALID(aim));

    /* steer-positive takes left_x: aim (the left stick) conflicts. SWAP would give aim the key
     * E and CLEAR_OTHER an unbound slot; a whole-stick slot can hold neither, so both are
     * refused and nothing changes. */
    const de_binding lx = bind(DE_BIND_PAD_AXIS, DE_AXIS_LEFT_X, DE_COMPOSITE_POSITIVE);
    EXPECT_EQ(de_rebind_apply(in, steer, 1, &lx, DE_REBIND_SWAP), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_rebind_apply(in, steer, 1, &lx, DE_REBIND_CLEAR_OTHER), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_rebind_clear(in, aim, 0), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    EXPECT_EQ(binding_of(in, steer, 1).code, (u32)DE_KEY_E);
    EXPECT_EQ(binding_of(in, aim, 0).code, (u32)DE_BIND_LEFT_STICK);
    /* ALLOW sets steer only. */
    ASSERT_EQ(de_rebind_apply(in, steer, 1, &lx, DE_REBIND_ALLOW), DE_OK);
    EXPECT_EQ(binding_of(in, steer, 1).code, (u32)DE_AXIS_LEFT_X);
    EXPECT_EQ(binding_of(in, aim, 0).code, (u32)DE_BIND_LEFT_STICK);
    de_input_destroy(in);
}

TEST(rebind, non_rebindable_rejected) {
    fixture f;
    fixture_init(&f);
    de_input *in = f.in;
    de_error_clear();
    EXPECT_EQ(de_rebind_begin(in, f.pause, 0, NULL, 0), DE_ERR_INVALID_ARGUMENT);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_TRUE(strstr(e->message, "pause") != NULL);
    EXPECT_EQ(de_rebind_status(in), DE_REBIND_IDLE);
    de_binding fk = key(DE_KEY_F);
    EXPECT_EQ(de_rebind_apply(in, f.pause, 0, &fk, DE_REBIND_ALLOW), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_rebind_clear(in, f.pause, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(binding_of(in, f.pause, 0).code, (u32)DE_KEY_G);
    /* Bad handles and indices. */
    EXPECT_EQ(de_rebind_begin(in, (de_action){0}, 0, NULL, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_rebind_begin(in, f.jump, 2, NULL, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_rebind_apply(in, f.jump, 5, &fk, DE_REBIND_ALLOW), DE_ERR_INVALID_ARGUMENT);
    de_binding bad = key(DE_KEY_F);
    bad.deadzone = 2.0f;
    EXPECT_EQ(de_rebind_apply(in, f.jump, 0, &bad, DE_REBIND_ALLOW), DE_ERR_INVALID_ARGUMENT);
    /* A key cannot bind a whole-2D slot. */
    EXPECT_EQ(de_rebind_apply(in, f.look, 0, &fk, DE_REBIND_ALLOW), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de_input_destroy(in);
}

TEST(rebind, unbound_slot_reads_nothing) {
    fixture f;
    fixture_init(&f);
    de_input *in = f.in;
    ASSERT_EQ(de_rebind_clear(in, f.interact, 0), DE_OK);
    /* Nothing presses an unbound slot, and it never conflicts. */
    input_test_key(in, DE_KEY_F, true);
    act_frame(in);
    EXPECT_FALSE(de_action_held(in, f.interact));
    input_test_key(in, DE_KEY_F, false);
    act_frame(in);
    const de_binding u = binding_of(in, f.interact, 0);
    EXPECT_TRUE(de_binding_is_unbound(&u));
    ASSERT_EQ(de_rebind_clear(in, f.jump, 0), DE_OK);
    de_input_destroy(in);
}

TEST(rebind, no_alloc_while_listening) {
    fixture f;
    fixture_init(&f);
    de_input *in = f.in;
    pad_on(in);
    const de_binding_source all[3] = {DE_BIND_KEY, DE_BIND_PAD_AXIS, DE_BIND_GYRO};
    ASSERT_EQ(de_rebind_begin(in, f.jump, 0, all, 3), DE_OK); /* allocates the state once */
    const de_binding cancel[1] = {key(DE_KEY_BACKSPACE)};
    ASSERT_EQ(de_rebind_set_cancel_inputs(in, cancel, 1), DE_OK);

    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    for (u32 i = 0; i < 200; ++i) {
        if (de_rebind_status(in) != DE_REBIND_LISTENING)
            (void)de_rebind_begin(in, f.jump, 0, all, 3);
        input_test_key(in, DE_KEY_LSHIFT, (i % 7) < 3);
        input_test_key(in, (i & 1) ? DE_KEY_Y : DE_KEY_U, (i % 3) == 0);
        input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, (i % 4) == 0 ? 0.9f : 0.0f);
        input_test_mouse_move(in, 1.0f, 1.0f, 1.0f, 1.0f);
        act_frame(in);
        de_binding got;
        de_action conflict;
        (void)de_rebind_poll(in, &got, &conflict);
        de__actions_fixed_tick(in, i);
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    de_input_destroy(in);
}
