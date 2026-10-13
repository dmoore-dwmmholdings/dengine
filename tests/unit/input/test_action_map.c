/* test_action_map.c: action maps, bindings, composites, chords, combination and modifiers
 * (DENG-84). Input is injected with de__input_inject (input_test_util.h); each frame runs
 * de_input's frame step and then the action evaluation hook, as the input.actions system does. */
#include "input_test_util.h"

#include "input/action_internal.h"

#include "de_alloc_guard.h"
#include "de_test.h"

#include <string.h>

/* ---- Helpers --------------------------------------------------------------------------------- */

/* One frame: apply the queued events, then evaluate the actions (input.actions, PreUpdate). */
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
                     de_interaction it, const de_binding *b, u32 n) {
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = type;
    d.interaction = it;
    d.bindings = b;
    d.binding_count = n;
    return de_action_add(in, m, &d);
}

static de_action_map pushed_map(de_input *in, const char *name) {
    const de_action_map m = de_action_map_create(in, name);
    de_action_map_push(in, m);
    return m;
}

/* Connects pad 1 (an Xbox pad). */
static void pad_on(de_input *in) {
    input_test_pad_add(in, 1, DE_PLATFORM_PAD_XBOX, DE_PLATFORM_PAD_CAP_GYRO, "Xbox");
    act_frame(in);
}

#define EPS 1.0e-5

/* ---- Buttons and axes ------------------------------------------------------------------------ */

TEST(action_map, button_from_key) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = de_action_map_create(in, "gameplay");
    ASSERT_TRUE(DE_HANDLE_VALID(m));
    const de_binding b[2] = {key(DE_KEY_SPACE), bind(DE_BIND_PAD_BUTTON, DE_PAD_SOUTH, 0)};
    const de_action jump = add(in, m, "jump", DE_ACTION_BUTTON, DE_INTERACT_PRESS, b, 2);
    ASSERT_TRUE(DE_HANDLE_VALID(jump));

    /* An inactive map reads idle. */
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_FALSE(de_action_triggered(in, jump));
    EXPECT_FALSE(de_action_held(in, jump));
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);

    de_action_map_push(in, m);
    act_frame(in);
    EXPECT_FALSE(de_action_triggered(in, jump));
    EXPECT_FALSE(de_action_held(in, jump));
    EXPECT_FLOAT_EQ(de_action_value(in, jump), 0.0, EPS);

    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, jump));
    EXPECT_TRUE(de_action_held(in, jump));
    EXPECT_FLOAT_EQ(de_action_value(in, jump), 1.0, EPS);

    act_frame(in); /* held: no new trigger */
    EXPECT_FALSE(de_action_triggered(in, jump));
    EXPECT_TRUE(de_action_held(in, jump));

    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);
    EXPECT_FALSE(de_action_triggered(in, jump));
    EXPECT_FALSE(de_action_held(in, jump));

    /* Down and up inside one frame still presses (and is not held afterwards). */
    input_test_key(in, DE_KEY_SPACE, true);
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, jump));
    EXPECT_FALSE(de_action_held(in, jump));

    /* A pad button binds too. */
    pad_on(in);
    input_test_pad_button(in, 1, DE_PAD_SOUTH, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, jump));
    input_test_pad_button(in, 1, DE_PAD_SOUTH, false);
    act_frame(in);

    /* Pop releases the actions of the map and makes them idle (DENG-85: at the next frame
     * update, not at the call). */
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_held(in, jump));
    de_action_map_pop(in, m);
    act_frame(in);
    EXPECT_FALSE(de_action_held(in, jump));
    EXPECT_FALSE(de_action_triggered(in, jump));

    /* Lookups and names. */
    EXPECT_EQ(de_action_map_find(in, "gameplay").id, m.id);
    EXPECT_EQ(de_action_find(in, m, "jump").id, jump.id);
    EXPECT_FALSE(DE_HANDLE_VALID(de_action_find(in, m, "fly")));
    EXPECT_FALSE(DE_HANDLE_VALID(de_action_map_find(in, "menu")));
    EXPECT_STREQ(de_action_name(in, jump), "jump");
    EXPECT_STREQ(de_action_map_name(in, m), "gameplay");
    EXPECT_EQ(de_action_map_create(in, "gameplay").id, m.id); /* same name: same map */

    /* Bad handles read as idle. */
    const de_action bogus = {jump.id + 1u};
    EXPECT_FALSE(de_action_triggered(in, bogus));
    EXPECT_FLOAT_EQ(de_action_value(in, (de_action){0}), 0.0, EPS);
    EXPECT_STREQ(de_action_name(in, bogus), "");
    EXPECT_FALSE(de_action_held(NULL, jump));
    de_input_destroy(in);
}

TEST(action_map, axis1d_from_trigger) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = pushed_map(in, "driving");
    const de_binding tb = bind(DE_BIND_PAD_AXIS, DE_AXIS_RIGHT_TRIGGER, DE_COMPOSITE_NONE);
    const de_action gas = add(in, m, "gas", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, &tb, 1);
    const de_binding sb[2] = {bind(DE_BIND_KEY, DE_KEY_E, DE_COMPOSITE_POSITIVE),
                              bind(DE_BIND_KEY, DE_KEY_Q, DE_COMPOSITE_NEGATIVE)};
    const de_action steer = add(in, m, "steer", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, sb, 2);
    ASSERT_TRUE(DE_HANDLE_VALID(gas));
    ASSERT_TRUE(DE_HANDLE_VALID(steer));
    pad_on(in);

    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_TRIGGER, 0.25f);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, gas), 0.25, EPS);
    EXPECT_FALSE(de_action_held(in, gas)); /* below the 0.5 press threshold */
    EXPECT_FALSE(de_action_triggered(in, gas));
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_TRIGGER, 0.75f);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, gas), 0.75, EPS);
    EXPECT_TRUE(de_action_held(in, gas));
    EXPECT_TRUE(de_action_triggered(in, gas));
    const de_vec2 v2 = de_action_value2(in, gas);
    EXPECT_FLOAT_EQ(v2.x, 0.75, EPS);
    EXPECT_FLOAT_EQ(v2.y, 0.0, EPS);

    /* 1D composite: positive - negative. */
    input_test_key(in, DE_KEY_E, true);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, steer), 1.0, EPS);
    input_test_key(in, DE_KEY_Q, true);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, steer), 0.0, EPS);
    input_test_key(in, DE_KEY_E, false);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, steer), -1.0, EPS);
    de_input_destroy(in);
}

TEST(action_map, wasd_composite) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = pushed_map(in, "gameplay");
    const de_binding b[5] = {
        bind(DE_BIND_KEY, DE_KEY_W, DE_COMPOSITE_UP),
        bind(DE_BIND_KEY, DE_KEY_S, DE_COMPOSITE_DOWN),
        bind(DE_BIND_KEY, DE_KEY_A, DE_COMPOSITE_LEFT),
        bind(DE_BIND_KEY, DE_KEY_D, DE_COMPOSITE_RIGHT),
        bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_NONE),
    };
    const de_action move = add(in, m, "move", DE_ACTION_AXIS2D, DE_INTERACT_PRESS, b, 5);
    ASSERT_TRUE(DE_HANDLE_VALID(move));

    input_test_key(in, DE_KEY_W, true);
    act_frame(in);
    de_vec2 v = de_action_value2(in, move);
    EXPECT_FLOAT_EQ(v.x, 0.0, EPS);
    EXPECT_FLOAT_EQ(v.y, 1.0, EPS);

    input_test_key(in, DE_KEY_D, true);
    act_frame(in);
    v = de_action_value2(in, move);
    EXPECT_FLOAT_EQ(v.x, 0.70710678, EPS);
    EXPECT_FLOAT_EQ(v.y, 0.70710678, EPS);
    EXPECT_FLOAT_EQ(de_action_value(in, move), 1.0, EPS);

    input_test_key(in, DE_KEY_D, false);
    input_test_key(in, DE_KEY_S, true);
    act_frame(in);
    v = de_action_value2(in, move);
    EXPECT_FLOAT_EQ(v.x, 0.0, EPS);
    EXPECT_FLOAT_EQ(v.y, 0.0, EPS);
    EXPECT_FALSE(de_action_held(in, move));

    input_test_key(in, DE_KEY_W, false);
    input_test_key(in, DE_KEY_S, false);
    input_test_key(in, DE_KEY_A, true);
    act_frame(in);
    v = de_action_value2(in, move);
    EXPECT_FLOAT_EQ(v.x, -1.0, EPS);
    EXPECT_FLOAT_EQ(v.y, 0.0, EPS);
    input_test_key(in, DE_KEY_A, false);

    /* The stick, when it is larger than the (idle) composite: raw +y down is "back", so the
     * platform's -0.6 is forward (+y up in action space). */
    pad_on(in);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.0f);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_Y, -0.6f);
    act_frame(in);
    v = de_action_value2(in, move);
    EXPECT_FLOAT_EQ(v.x, 0.0, EPS);
    EXPECT_FLOAT_EQ(v.y, 0.6, EPS);
    EXPECT_EQ(in->actions->actions[0].winning_binding, 4u);

    /* A key now (magnitude 1) beats the stick (0.6). */
    input_test_key(in, DE_KEY_D, true);
    act_frame(in);
    v = de_action_value2(in, move);
    EXPECT_FLOAT_EQ(v.x, 1.0, EPS);
    EXPECT_FLOAT_EQ(v.y, 0.0, EPS);
    EXPECT_EQ(in->actions->actions[0].winning_binding, 0u); /* the composite: its first index */
    de_input_destroy(in);
}

TEST(action_map, chord_suppresses_plain) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = pushed_map(in, "gameplay");
    const de_binding plain = key(DE_KEY_E);
    de_binding chord = key(DE_KEY_E);
    chord.modifier_key = DE_KEY_LSHIFT;
    const de_action use = add(in, m, "use", DE_ACTION_BUTTON, DE_INTERACT_PRESS, &plain, 1);
    const de_action special = add(in, m, "special", DE_ACTION_BUTTON, DE_INTERACT_PRESS, &chord, 1);
    /* Another map with plain E: a chord only suppresses inside its own map. */
    const de_action_map other = pushed_map(in, "ui");
    /* DENG-85: a map above blocks the inputs it uses from the maps below; passthrough keeps this
     * test about chords only. */
    de_action_map_set_flags(in, other, DE_INPUT_CONTEXT_PASSTHROUGH);
    const de_action ui_use =
        add(in, other, "ui_use", DE_ACTION_BUTTON, DE_INTERACT_PRESS, &plain, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(use) && DE_HANDLE_VALID(special) && DE_HANDLE_VALID(ui_use));

    input_test_key(in, DE_KEY_E, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, use));
    EXPECT_FALSE(de_action_triggered(in, special));
    EXPECT_TRUE(de_action_triggered(in, ui_use));
    input_test_key(in, DE_KEY_E, false);
    act_frame(in);

    input_test_key(in, DE_KEY_LSHIFT, true);
    act_frame(in);
    input_test_key(in, DE_KEY_E, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, special));
    EXPECT_FALSE(de_action_triggered(in, use));
    EXPECT_FALSE(de_action_held(in, use));
    EXPECT_TRUE(de_action_triggered(in, ui_use));

    /* Shift released while E is held: the chord stops, the plain binding takes over. */
    input_test_key(in, DE_KEY_LSHIFT, false);
    act_frame(in);
    EXPECT_FALSE(de_action_held(in, special));
    EXPECT_TRUE(de_action_held(in, use));
    de_input_destroy(in);
}

TEST(action_map, multi_binding_max_magnitude) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = pushed_map(in, "gameplay");
    const de_binding lr[2] = {bind(DE_BIND_PAD_AXIS, DE_AXIS_LEFT_X, DE_COMPOSITE_NONE),
                              bind(DE_BIND_PAD_AXIS, DE_AXIS_RIGHT_X, DE_COMPOSITE_NONE)};
    const de_binding rl[2] = {lr[1], lr[0]};
    const de_action a_lr = add(in, m, "lr", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, lr, 2);
    const de_action a_rl = add(in, m, "rl", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, rl, 2);
    ASSERT_TRUE(DE_HANDLE_VALID(a_lr) && DE_HANDLE_VALID(a_rl));
    pad_on(in);

    /* The largest magnitude wins, whatever its sign. */
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.3f);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_X, -0.8f);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, a_lr), -0.8, EPS);
    EXPECT_FLOAT_EQ(de_action_value(in, a_rl), -0.8, EPS);

    /* A tie goes to the lower binding index, so the order of the bindings decides. */
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.5f);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_X, -0.5f);
    for (u32 i = 0; i < 3; ++i) { /* and the same every frame */
        act_frame(in);
        EXPECT_FLOAT_EQ(de_action_value(in, a_lr), 0.5, EPS);
        EXPECT_FLOAT_EQ(de_action_value(in, a_rl), -0.5, EPS);
    }

    /* Two pads on one binding: the largest magnitude wins, a tie goes to the first pad. */
    input_test_pad_add(in, 2, DE_PLATFORM_PAD_PS5, 0, "DualSense");
    input_test_pad_axis(in, 2, DE_AXIS_LEFT_X, -0.5f);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, a_lr), 0.5, EPS);
    input_test_pad_axis(in, 2, DE_AXIS_LEFT_X, -0.9f);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, a_lr), -0.9, EPS);
    de_input_destroy(in);
}

/* ---- Modifiers ------------------------------------------------------------------------------- */

TEST(action_map, modifier_order) {
    /* deadzone 0.2 -> power 2 -> scale 2 -> invert, on 0.6:
     * (0.6 - 0.2) / 0.8 = 0.5 -> 0.25 -> 0.5 -> -0.5. Any other order gives another value. */
    de_binding b = bind(DE_BIND_PAD_AXIS, DE_AXIS_LEFT_X, DE_COMPOSITE_NONE);
    b.deadzone = 0.2f;
    b.curve.type = DE_CURVE_POWER;
    b.curve.exponent = 2.0f;
    b.scale = 2.0f;
    b.invert = true;
    EXPECT_FLOAT_EQ(de__modifiers_apply_1d(&b, 0.6f, true), -0.5, EPS);
    EXPECT_FLOAT_EQ(de__modifiers_apply_1d(&b, -0.6f, true), 0.5, EPS); /* sign kept */
    EXPECT_FLOAT_EQ(de__modifiers_apply_1d(&b, 0.1f, true), 0.0, EPS);

    /* Each modifier alone. */
    de_binding s = bind(DE_BIND_PAD_AXIS, DE_AXIS_LEFT_X, DE_COMPOSITE_NONE);
    EXPECT_FLOAT_EQ(de__modifiers_apply_1d(&s, 0.6f, true), 0.6, EPS); /* scale 0 means 1 */
    s.scale = 3.0f;
    EXPECT_FLOAT_EQ(de__modifiers_apply_1d(&s, 0.5f, true), 1.5, EPS);
    s.scale = 0.0f;
    s.invert = true;
    EXPECT_FLOAT_EQ(de__modifiers_apply_1d(&s, 0.5f, true), -0.5, EPS);

    /* Through an action. */
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = pushed_map(in, "gameplay");
    const de_action a = add(in, m, "steer", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, &b, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(a));
    pad_on(in);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.6f);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, a), -0.5, EPS);

    /* 2D invert flips y only ("invert look"). */
    de_binding look = bind(DE_BIND_PAD_AXIS, DE_BIND_RIGHT_STICK, DE_COMPOSITE_NONE);
    look.invert = true;
    const de_vec2 in2 = {0.5f, 0.5f};
    const de_vec2 out2 = de__modifiers_apply_2d(&look, in2, true);
    EXPECT_FLOAT_EQ(out2.x, 0.5, EPS);
    EXPECT_FLOAT_EQ(out2.y, -0.5, EPS);
    de_input_destroy(in);
}

TEST(action_map, radial_deadzone) {
    de_binding b = bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_NONE);
    b.deadzone = 0.2f;
    /* Along a diagonal, so the deadzone is radial, not per axis. */
    const f32 d = 0.70710678f;
    de_vec2 v = de__modifiers_apply_2d(&b, (de_vec2){0.2f * d, 0.2f * d}, true);
    EXPECT_FLOAT_EQ(v.x, 0.0, EPS);
    EXPECT_FLOAT_EQ(v.y, 0.0, EPS);
    v = de__modifiers_apply_2d(&b, (de_vec2){d, d}, true);
    EXPECT_FLOAT_EQ(v.x * v.x + v.y * v.y, 1.0, 1.0e-4);
    EXPECT_FLOAT_EQ(v.x, d, 1.0e-4);
    v = de__modifiers_apply_2d(&b, (de_vec2){0.6f, 0.0f}, true);
    EXPECT_FLOAT_EQ(v.x, 0.5, EPS);
    /* A square gate corner (length 1.41) is clamped to 1. */
    v = de__modifiers_apply_2d(&b, (de_vec2){1.0f, 1.0f}, true);
    EXPECT_FLOAT_EQ(v.x * v.x + v.y * v.y, 1.0, 1.0e-4);

    /* Continuous: from 0.2 to 1.0 the output length rises from 0 to 1 without a jump. */
    f32 prev = 0.0f;
    for (u32 i = 0; i <= 800; ++i) {
        const f32 mag = 0.2f + (f32)i * 0.001f;
        const de_vec2 o = de__modifiers_apply_2d(&b, (de_vec2){0.0f, mag}, true);
        EXPECT_TRUE(o.y >= prev - 1.0e-6f);
        EXPECT_TRUE(o.y - prev <= 0.0013f);
        prev = o.y;
    }
    EXPECT_FLOAT_EQ(prev, 1.0, 1.0e-4);

    /* Through an action: a stick inside the deadzone reads (0, 0). */
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = pushed_map(in, "gameplay");
    const de_action a = add(in, m, "move", DE_ACTION_AXIS2D, DE_INTERACT_PRESS, &b, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(a));
    pad_on(in);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.1f);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_Y, -0.1f);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, a), 0.0, EPS);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 1.0f);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_Y, 0.0f);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, a), 1.0, EPS);
    de_input_destroy(in);
}

TEST(action_map, curve_piecewise) {
    de_response_curve c;
    memset(&c, 0, sizeof c);
    c.type = DE_CURVE_CUSTOM;
    c.point_count = 3;
    c.points[0] = (de_vec2){0.0f, 0.0f};
    c.points[1] = (de_vec2){0.5f, 0.2f};
    c.points[2] = (de_vec2){1.0f, 1.0f};
    EXPECT_EQ(de__curve_validate(&c, "test"), DE_OK);
    EXPECT_FLOAT_EQ(de__curve_eval(&c, 0.0f), 0.0, EPS);
    EXPECT_FLOAT_EQ(de__curve_eval(&c, 0.25f), 0.1, EPS);
    EXPECT_FLOAT_EQ(de__curve_eval(&c, 0.5f), 0.2, EPS);
    EXPECT_FLOAT_EQ(de__curve_eval(&c, 0.75f), 0.6, EPS);
    EXPECT_FLOAT_EQ(de__curve_eval(&c, 1.0f), 1.0, EPS);
    EXPECT_FLOAT_EQ(de__curve_eval(&c, 2.0f), 1.0, EPS); /* clamped to the last point */

    /* Linear and power. */
    de_response_curve lin;
    memset(&lin, 0, sizeof lin);
    EXPECT_FLOAT_EQ(de__curve_eval(&lin, 0.3f), 0.3, EPS);
    de_response_curve pw = lin;
    pw.type = DE_CURVE_POWER;
    pw.exponent = 3.0f;
    EXPECT_FLOAT_EQ(de__curve_eval(&pw, 0.5f), 0.125, EPS);

    /* Through a binding: the curve shapes the magnitude and keeps the sign. */
    de_binding b = bind(DE_BIND_PAD_AXIS, DE_AXIS_LEFT_X, DE_COMPOSITE_NONE);
    b.curve = c;
    EXPECT_FLOAT_EQ(de__modifiers_apply_1d(&b, -0.75f, true), -0.6, EPS);

    /* Validation. */
    de_response_curve bad = c;
    bad.points[1].x = 0.0f; /* not increasing */
    EXPECT_EQ(de__curve_validate(&bad, "test"), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    bad = c;
    bad.point_count = 1;
    EXPECT_EQ(de__curve_validate(&bad, "test"), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    bad = pw;
    bad.exponent = 0.0f;
    EXPECT_EQ(de__curve_validate(&bad, "test"), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
}

/* ---- Other sources --------------------------------------------------------------------------- */

TEST(action_map, mouse_and_gyro_sources) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = pushed_map(in, "gameplay");
    const de_binding look_b[2] = {bind(DE_BIND_MOUSE_DELTA, DE_BIND_AXIS_XY, DE_COMPOSITE_NONE),
                                  bind(DE_BIND_GYRO, DE_BIND_AXIS_XY, DE_COMPOSITE_NONE)};
    const de_action look = add(in, m, "look", DE_ACTION_AXIS2D, DE_INTERACT_PRESS, look_b, 2);
    const de_binding wheel_b = bind(DE_BIND_MOUSE_WHEEL, DE_BIND_AXIS_Y, DE_COMPOSITE_NONE);
    const de_action zoom = add(in, m, "zoom", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, &wheel_b, 1);
    const de_binding click_b = bind(DE_BIND_MOUSE_BUTTON, DE_MOUSE_LEFT, DE_COMPOSITE_NONE);
    const de_action fire = add(in, m, "fire", DE_ACTION_BUTTON, DE_INTERACT_PRESS, &click_b, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(look) && DE_HANDLE_VALID(zoom) && DE_HANDLE_VALID(fire));

    /* Mouse: action space is +y up, so moving the mouse up (dy < 0) gives +y. */
    input_test_mouse_move(in, 10.0f, 10.0f, 3.0f, -4.0f);
    input_test_mouse_wheel(in, 0.0f, 2.0f);
    input_test_mouse_button(in, DE_MOUSE_LEFT, true);
    act_frame(in);
    de_vec2 v = de_action_value2(in, look);
    EXPECT_FLOAT_EQ(v.x, 3.0, EPS);
    EXPECT_FLOAT_EQ(v.y, 4.0, EPS);
    EXPECT_FLOAT_EQ(de_action_value(in, zoom), 2.0, EPS);
    EXPECT_TRUE(de_action_triggered(in, fire));

    /* Gyro aim: x = -gyro.y (turn right), y = gyro.x (tilt up). */
    pad_on(in);
    input_test_pad_sensor(in, 1, DE_PLATFORM_SENSOR_GYRO, 0.5f, -2.0f, 0.0f, 1000u);
    act_frame(in);
    v = de_action_value2(in, look);
    EXPECT_FLOAT_EQ(v.x, 2.0, EPS);
    EXPECT_FLOAT_EQ(v.y, 0.5, EPS);
    de_input_destroy(in);
}

/* ---- Validation, replace, storage ------------------------------------------------------------ */

TEST(action_map, add_validation_and_replace) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    EXPECT_FALSE(DE_HANDLE_VALID(de_action_map_create(in, "")));
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(de_action_map_create(NULL, "x")));
    de_error_clear();
    const de_action_map m = pushed_map(in, "gameplay");

    /* A key in a 2D action needs a composite slot; a stick needs none; a stick is not 1D. */
    const de_binding k = key(DE_KEY_W);
    EXPECT_FALSE(DE_HANDLE_VALID(add(in, m, "a", DE_ACTION_AXIS2D, DE_INTERACT_PRESS, &k, 1)));
    EXPECT_EQ(de_error_last()->result, DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    const de_binding stick_up = bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_UP);
    EXPECT_FALSE(
        DE_HANDLE_VALID(add(in, m, "b", DE_ACTION_AXIS2D, DE_INTERACT_PRESS, &stick_up, 1)));
    de_error_clear();
    const de_binding stick = bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_NONE);
    EXPECT_FALSE(DE_HANDLE_VALID(add(in, m, "c", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, &stick, 1)));
    de_error_clear();
    const de_binding left = bind(DE_BIND_KEY, DE_KEY_A, DE_COMPOSITE_LEFT);
    EXPECT_FALSE(DE_HANDLE_VALID(add(in, m, "d", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, &left, 1)));
    de_error_clear();
    de_binding dz = stick;
    dz.deadzone = 1.0f;
    EXPECT_FALSE(DE_HANDLE_VALID(add(in, m, "e", DE_ACTION_AXIS2D, DE_INTERACT_PRESS, &dz, 1)));
    de_error_clear();
    const de_binding bad_code = bind(DE_BIND_PAD_BUTTON, 99, DE_COMPOSITE_NONE);
    EXPECT_FALSE(
        DE_HANDLE_VALID(add(in, m, "f", DE_ACTION_BUTTON, DE_INTERACT_PRESS, &bad_code, 1)));
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(add(in, m, "g", (de_action_type)7, DE_INTERACT_PRESS, &k, 1)));
    de_error_clear();
    EXPECT_FALSE(DE_HANDLE_VALID(add(in, m, NULL, DE_ACTION_BUTTON, DE_INTERACT_PRESS, &k, 1)));
    de_error_clear();
    EXPECT_EQ(de__action_map_actions(in, m, NULL, 0), 0u); /* nothing was added */

    /* A zeroed binding in a button action: slot 0 is ignored there. */
    de_binding zero = key(DE_KEY_J);
    zero.composite_slot = 0;
    const de_action j = add(in, m, "j", DE_ACTION_BUTTON, DE_INTERACT_PRESS, &zero, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(j));

    /* Replace by name keeps the handle and the order; bindings, get and set. */
    const de_binding kb[2] = {key(DE_KEY_K), key(DE_KEY_L)};
    const de_action k1 = add(in, m, "k", DE_ACTION_BUTTON, DE_INTERACT_PRESS, kb, 1);
    const de_action k2 = add(in, m, "k", DE_ACTION_BUTTON, DE_INTERACT_TAP, kb, 2);
    EXPECT_EQ(k1.id, k2.id);
    de_action list[4];
    ASSERT_EQ(de__action_map_actions(in, m, list, 4), 2u);
    EXPECT_EQ(list[0].id, j.id);
    EXPECT_EQ(list[1].id, k1.id);
    EXPECT_EQ(de__action_binding_count(in, k1), 2u);
    de_binding got;
    ASSERT_TRUE(de__action_get_binding(in, k1, 1, &got));
    EXPECT_EQ(got.code, (u32)DE_KEY_L);
    EXPECT_EQ(de__action_set_binding(in, k1, 1, &kb[0]), DE_OK);
    ASSERT_TRUE(de__action_get_binding(in, k1, 1, &got));
    EXPECT_EQ(got.code, (u32)DE_KEY_K);
    EXPECT_EQ(de__action_set_binding(in, k1, 5, &kb[0]), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    EXPECT_EQ(de__action_set_binding(in, k1, 0, &stick_up), DE_OK); /* a stick in a button */
    EXPECT_EQ(de__action_map_of(in, k1).id, m.id);
    EXPECT_EQ(de__action_map_count(in), 1u);
    de_input_destroy(in);
}

TEST(action_map, no_alloc_per_frame) {
    de_tracking_allocator tracker;
    const de_allocator ta = de_tracking_allocator_init(&tracker, de_heap_allocator(), 0);
    de_input *in = input_test_create(&ta);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = pushed_map(in, "gameplay");
    const de_binding move_b[5] = {
        bind(DE_BIND_KEY, DE_KEY_W, DE_COMPOSITE_UP),
        bind(DE_BIND_KEY, DE_KEY_S, DE_COMPOSITE_DOWN),
        bind(DE_BIND_KEY, DE_KEY_A, DE_COMPOSITE_LEFT),
        bind(DE_BIND_KEY, DE_KEY_D, DE_COMPOSITE_RIGHT),
        bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_NONE),
    };
    ASSERT_TRUE(
        DE_HANDLE_VALID(add(in, m, "move", DE_ACTION_AXIS2D, DE_INTERACT_PRESS, move_b, 5)));
    de_binding jb[2] = {key(DE_KEY_SPACE), key(DE_KEY_SPACE)};
    jb[1].modifier_key = DE_KEY_LSHIFT;
    ASSERT_TRUE(DE_HANDLE_VALID(add(in, m, "jump", DE_ACTION_BUTTON, DE_INTERACT_HOLD, jb, 2)));
    ASSERT_TRUE(
        DE_HANDLE_VALID(add(in, m, "dash", DE_ACTION_BUTTON, DE_INTERACT_DOUBLE_TAP, jb, 1)));
    const u64 allocs = de_tracking_allocator_stats(&tracker).alloc_count;
    EXPECT_EQ(allocs, 4u); /* the input state, the gyro and haptics state (DENG-87), the actions */
    pad_on(in);

    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    for (u32 i = 0; i < 200; ++i) {
        input_test_key(in, DE_KEY_W, (i & 1) == 0);
        input_test_key(in, DE_KEY_SPACE, (i % 3) == 0);
        input_test_key(in, DE_KEY_LSHIFT, (i % 5) == 0);
        input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.5f);
        input_test_mouse_move(in, 1.0f, 1.0f, 1.0f, 1.0f);
        act_frame(in);
        de__actions_fixed_tick(in, i);
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_EQ(de_tracking_allocator_stats(&tracker).alloc_count, allocs);
    de_input_destroy(in);
    const de_tracking_stats st = de_tracking_allocator_stats(&tracker);
    EXPECT_EQ(st.free_count, st.alloc_count);
}
