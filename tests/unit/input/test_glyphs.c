/* test_glyphs.c: prompt glyph lookup (DENG-88): the family from the last used device, positional
 * face buttons, the fallback to the other device class, unbound actions, composite IDs, the
 * one-frame family change flag, the provider override, key labels and key IDs, and that every ID
 * of glyph_table.inc is listed in docs/input/glyph-ids.md (and the other way round). Input is
 * injected with de__input_inject (input_test_util.h). */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* fopen */
#endif
#include "input_test_util.h"

#include "input/action_internal.h"

#include "de_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_GLYPH_DOC
#error "DE_GLYPH_DOC must be defined by tests/unit/input/CMakeLists.txt"
#endif

/* ---- Helpers --------------------------------------------------------------------------------- */

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

static de_binding pad(de_pad_button btn) {
    return bind(DE_BIND_PAD_BUTTON, (u32)btn, DE_COMPOSITE_NONE);
}

static de_action add(de_input *in, de_action_map m, const char *name, de_action_type type,
                     const de_binding *b, u32 n) {
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = type;
    d.interaction = DE_INTERACT_PRESS;
    d.bindings = b;
    d.binding_count = n;
    return de_action_add(in, m, &d);
}

/* "jump": Space and Pad South, the acceptance criterion's action. */
static de_action jump_action(de_input *in, de_action_map m) {
    const de_binding b[2] = {key(DE_KEY_SPACE), pad(DE_PAD_SOUTH)};
    return add(in, m, "jump", DE_ACTION_BUTTON, b, 2);
}

/* Connects pad `device` and presses and releases its south button, so it is the last used
 * device. */
static void use_pad(de_input *in, u32 device, de_platform_pad_type type, const char *name) {
    input_test_pad_add(in, device, type, 0, name);
    input_test_pad_button(in, device, DE_PAD_SOUTH, true);
    input_test_frame(in);
    input_test_pad_button(in, device, DE_PAD_SOUTH, false);
    input_test_frame(in);
}

static void tap_key(de_input *in, de_key k) {
    input_test_key(in, k, true);
    input_test_frame(in);
    input_test_key(in, k, false);
    input_test_frame(in);
}

/* ---- Family from the last used device -------------------------------------------------------- */

TEST(glyphs, family_from_last_device) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = de_action_map_create(in, "gameplay");
    const de_action jump = jump_action(in, m);
    ASSERT_TRUE(DE_HANDLE_VALID(jump));

    /* Nothing used, no pad: keyboard and mouse. */
    input_test_frame(in);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_KBM);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "kbm/key_space");

    /* Keyboard last. */
    tap_key(in, DE_KEY_SPACE);
    EXPECT_EQ(de_input_last_device(in, NULL), DE_INPUT_DEVICE_KEYBOARD_MOUSE);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "kbm/key_space");

    /* A PS5 pad last. */
    use_pad(in, 1, DE_PLATFORM_PAD_PS5, "DualSense");
    EXPECT_EQ(de_input_last_device(in, NULL), DE_INPUT_DEVICE_GAMEPAD);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_PS5);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "ps/cross");

    /* A Switch Pro controller last. */
    use_pad(in, 2, DE_PLATFORM_PAD_SWITCH, "Pro Controller");
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_SWITCH);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "switch/b");

    /* The other pad types. */
    use_pad(in, 3, DE_PLATFORM_PAD_XBOX, "Xbox Series");
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_XBOX);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "xbox/a");
    use_pad(in, 4, DE_PLATFORM_PAD_PS4, "DualShock 4");
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_PS);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "ps/cross");
    use_pad(in, 5, DE_PLATFORM_PAD_STEAM, "Steam Deck");
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_DECK);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "deck/a");
    use_pad(in, 6, DE_PLATFORM_PAD_GENERIC, "Some pad");
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_GENERIC);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "generic/south");

    /* Back to the keyboard. */
    tap_key(in, DE_KEY_SPACE);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_KBM);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "kbm/key_space");

    /* The last used pad disconnects: the first connected pad (the PS5 pad) gives the family. */
    input_test_pad_button(in, 5, DE_PAD_EAST, true);
    input_test_frame(in);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_DECK);
    input_test_pad_remove(in, 5);
    input_test_frame(in);
    EXPECT_EQ(de_input_last_device(in, NULL), DE_INPUT_DEVICE_NONE);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_PS5);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "ps/cross");

    /* Every pad gone: keyboard and mouse. */
    for (u32 d = 1; d <= 6; ++d)
        input_test_pad_remove(in, d);
    input_test_frame(in);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_KBM);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "kbm/key_space");

    /* Family names. */
    EXPECT_STREQ(de_glyph_family_name(DE_GLYPH_FAMILY_KBM), "kbm");
    EXPECT_STREQ(de_glyph_family_name(DE_GLYPH_FAMILY_PS5), "ps5");
    EXPECT_STREQ(de_glyph_family_name(DE_GLYPH_FAMILY_GENERIC), "generic");
    EXPECT_TRUE(de_glyph_family_name(DE_GLYPH_FAMILY_COUNT_) == NULL);
    EXPECT_TRUE(de_glyph_family_name((de_glyph_family)-1) == NULL);
    de_input_destroy(in);
}

/* ---- Positional face buttons ------------------------------------------------------------------
 */

TEST(glyphs, face_button_positional) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = de_action_map_create(in, "gameplay");
    static const de_pad_button k_face[4] = {DE_PAD_SOUTH, DE_PAD_EAST, DE_PAD_WEST, DE_PAD_NORTH};
    static const char *const k_names[4] = {"south", "east", "west", "north"};
    /* [family][south, east, west, north] */
    static const char *const k_expect[DE_GLYPH_FAMILY_COUNT_][4] = {
        [DE_GLYPH_FAMILY_KBM] = {"generic/south", "generic/east", "generic/west", "generic/north"},
        [DE_GLYPH_FAMILY_XBOX] = {"xbox/a", "xbox/b", "xbox/x", "xbox/y"},
        [DE_GLYPH_FAMILY_PS] = {"ps/cross", "ps/circle", "ps/square", "ps/triangle"},
        [DE_GLYPH_FAMILY_PS5] = {"ps/cross", "ps/circle", "ps/square", "ps/triangle"},
        [DE_GLYPH_FAMILY_SWITCH] = {"switch/b", "switch/a", "switch/y", "switch/x"},
        [DE_GLYPH_FAMILY_DECK] = {"deck/a", "deck/b", "deck/x", "deck/y"},
        [DE_GLYPH_FAMILY_GENERIC] = {"generic/south", "generic/east", "generic/west",
                                     "generic/north"},
    };
    de_action face[4];
    for (u32 i = 0; i < 4; ++i) {
        const de_binding b = pad(k_face[i]);
        face[i] = add(in, m, k_names[i], DE_ACTION_BUTTON, &b, 1);
        ASSERT_TRUE(DE_HANDLE_VALID(face[i]));
    }
    /* KBM with no pad connected falls back to the pad binding in the generic family. */
    for (u32 f = 0; f < (u32)DE_GLYPH_FAMILY_COUNT_; ++f)
        for (u32 i = 0; i < 4; ++i)
            EXPECT_STREQ(de_action_glyph_id_family(in, face[i], (de_glyph_family)f),
                         k_expect[f][i]);

    /* PS5-only controls, and their PS4 names. */
    const de_binding b_back = pad(DE_PAD_BACK), b_touch = pad(DE_PAD_TOUCHPAD);
    const de_binding b_misc = pad(DE_PAD_MISC1);
    const de_action back = add(in, m, "back", DE_ACTION_BUTTON, &b_back, 1);
    const de_action touch = add(in, m, "touch", DE_ACTION_BUTTON, &b_touch, 1);
    const de_action misc = add(in, m, "misc", DE_ACTION_BUTTON, &b_misc, 1);
    EXPECT_STREQ(de_action_glyph_id_family(in, back, DE_GLYPH_FAMILY_PS5), "ps5/create");
    EXPECT_STREQ(de_action_glyph_id_family(in, back, DE_GLYPH_FAMILY_PS), "ps/share");
    EXPECT_STREQ(de_action_glyph_id_family(in, touch, DE_GLYPH_FAMILY_PS5), "ps5/touchpad");
    EXPECT_STREQ(de_action_glyph_id_family(in, touch, DE_GLYPH_FAMILY_PS), "ps/touchpad");
    EXPECT_STREQ(de_action_glyph_id_family(in, misc, DE_GLYPH_FAMILY_PS5), "ps5/mute");
    /* A control the family does not have uses the generic ID. */
    EXPECT_STREQ(de_action_glyph_id_family(in, touch, DE_GLYPH_FAMILY_XBOX), "generic/touchpad");
    EXPECT_STREQ(de_action_glyph_id_family(in, misc, DE_GLYPH_FAMILY_PS), "generic/misc1");
    /* An out-of-range family is treated as generic. */
    EXPECT_STREQ(de_action_glyph_id_family(in, face[0], DE_GLYPH_FAMILY_COUNT_), "generic/south");
    de_input_destroy(in);
}

/* ---- Fallback to the other device class -------------------------------------------------------
 */

TEST(glyphs, fallback_other_device) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = de_action_map_create(in, "gameplay");
    const de_binding b_key = key(DE_KEY_E);
    const de_binding b_pad = pad(DE_PAD_WEST);
    const de_action use = add(in, m, "use", DE_ACTION_BUTTON, &b_key, 1);
    const de_action reload = add(in, m, "reload", DE_ACTION_BUTTON, &b_pad, 1);
    /* Pad North, then Q, then Pad South: the first binding of the class wins. */
    const de_binding mixed[3] = {pad(DE_PAD_NORTH), key(DE_KEY_Q), pad(DE_PAD_SOUTH)};
    const de_action swap = add(in, m, "swap", DE_ACTION_BUTTON, mixed, 3);
    ASSERT_TRUE(DE_HANDLE_VALID(use) && DE_HANDLE_VALID(reload) && DE_HANDLE_VALID(swap));

    /* Keyboard-only action on a pad family: the key. */
    EXPECT_STREQ(de_action_glyph_id_family(in, use, DE_GLYPH_FAMILY_PS5), "kbm/key_e");
    EXPECT_STREQ(de_action_glyph_id_family(in, use, DE_GLYPH_FAMILY_XBOX), "kbm/key_e");
    /* Pad-only action on KBM: the generic family when no pad is connected... */
    EXPECT_STREQ(de_action_glyph_id_family(in, reload, DE_GLYPH_FAMILY_KBM), "generic/west");
    /* ...else the family of the first connected pad. */
    input_test_pad_add(in, 9, DE_PLATFORM_PAD_SWITCH, 0, "Pro Controller");
    input_test_pad_add(in, 10, DE_PLATFORM_PAD_XBOX, 0, "Xbox");
    input_test_frame(in);
    EXPECT_STREQ(de_action_glyph_id_family(in, reload, DE_GLYPH_FAMILY_KBM), "switch/y");

    EXPECT_STREQ(de_action_glyph_id_family(in, swap, DE_GLYPH_FAMILY_XBOX), "xbox/y");
    EXPECT_STREQ(de_action_glyph_id_family(in, swap, DE_GLYPH_FAMILY_KBM), "kbm/key_q");

    /* Through the current family: Xbox used last. */
    input_test_pad_button(in, 10, DE_PAD_EAST, true);
    input_test_frame(in);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_XBOX);
    EXPECT_STREQ(de_action_glyph_id(in, use), "kbm/key_e");
    EXPECT_STREQ(de_action_glyph_id(in, reload), "xbox/x");

    /* A rebound binding (DENG-86 sets it through action_internal.h) changes the glyph. */
    const de_binding rebound = pad(DE_PAD_RSHOULDER);
    EXPECT_EQ(de__action_set_binding(in, reload, 0, &rebound), DE_OK);
    EXPECT_STREQ(de_action_glyph_id(in, reload), "xbox/rb");
    de_input_destroy(in);
}

/* ---- Unbound ----------------------------------------------------------------------------------
 */

TEST(glyphs, unbound) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    EXPECT_STREQ(DE_GLYPH_ID_UNBOUND, "generic/unbound");
    const de_action_map m = de_action_map_create(in, "gameplay");
    const de_action none = add(in, m, "nothing", DE_ACTION_BUTTON, NULL, 0);
    ASSERT_TRUE(DE_HANDLE_VALID(none));
    input_test_frame(in);
    EXPECT_STREQ(de_action_glyph_id(in, none), DE_GLYPH_ID_UNBOUND);
    for (u32 f = 0; f < (u32)DE_GLYPH_FAMILY_COUNT_; ++f)
        EXPECT_STREQ(de_action_glyph_id_family(in, none, (de_glyph_family)f), "generic/unbound");

    /* Invalid and stale handles, and no input state. */
    EXPECT_STREQ(de_action_glyph_id(in, (de_action){0}), "generic/unbound");
    EXPECT_STREQ(de_action_glyph_id(in, (de_action){0xFFFFFFu}), "generic/unbound");
    EXPECT_STREQ(de_action_glyph_id(NULL, none), "generic/unbound");
    EXPECT_EQ(de_input_glyph_family(NULL), DE_GLYPH_FAMILY_KBM);
    EXPECT_FALSE(de_input_glyph_family_changed(NULL));

    /* A key code without a name is still a key: kbm/key_unknown, not unbound. */
    const de_binding b = key((de_key)(DE_KEY_COUNT_ - 1));
    const de_action odd = add(in, m, "odd", DE_ACTION_BUTTON, &b, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(odd));
    EXPECT_TRUE(de_key_name((de_key)(DE_KEY_COUNT_ - 1)) == NULL);
    EXPECT_STREQ(de_action_glyph_id_family(in, odd, DE_GLYPH_FAMILY_KBM), "kbm/key_unknown");
    de_input_destroy(in);
}

/* ---- Composites -------------------------------------------------------------------------------
 */

TEST(glyphs, composite_ids) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = de_action_map_create(in, "gameplay");

    /* WASD and a whole left stick: kbm/wasd on KBM, the stick on a pad. */
    const de_binding move_b[5] = {
        bind(DE_BIND_KEY, DE_KEY_W, DE_COMPOSITE_UP),
        bind(DE_BIND_KEY, DE_KEY_S, DE_COMPOSITE_DOWN),
        bind(DE_BIND_KEY, DE_KEY_A, DE_COMPOSITE_LEFT),
        bind(DE_BIND_KEY, DE_KEY_D, DE_COMPOSITE_RIGHT),
        bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_NONE),
    };
    const de_action move = add(in, m, "move", DE_ACTION_AXIS2D, move_b, 5);
    ASSERT_TRUE(DE_HANDLE_VALID(move));
    EXPECT_STREQ(de_action_glyph_id_family(in, move, DE_GLYPH_FAMILY_KBM), "kbm/wasd");
    EXPECT_STREQ(de_action_glyph_id_family(in, move, DE_GLYPH_FAMILY_XBOX), "xbox/stick_left");
    EXPECT_STREQ(de_action_glyph_id_family(in, move, DE_GLYPH_FAMILY_PS5), "ps/stick_left");

    /* Arrow keys, in another slot order: the set counts, not the binding order. */
    const de_binding arrows_b[4] = {
        bind(DE_BIND_KEY, DE_KEY_RIGHT, DE_COMPOSITE_RIGHT),
        bind(DE_BIND_KEY, DE_KEY_LEFT, DE_COMPOSITE_LEFT),
        bind(DE_BIND_KEY, DE_KEY_UP, DE_COMPOSITE_UP),
        bind(DE_BIND_KEY, DE_KEY_DOWN, DE_COMPOSITE_DOWN),
    };
    const de_action steer = add(in, m, "steer", DE_ACTION_AXIS2D, arrows_b, 4);
    EXPECT_STREQ(de_action_glyph_id_family(in, steer, DE_GLYPH_FAMILY_KBM), "kbm/arrows");

    /* Not a known set (I/K/J/L): the first binding's own ID. */
    const de_binding ijkl_b[4] = {
        bind(DE_BIND_KEY, DE_KEY_I, DE_COMPOSITE_UP),
        bind(DE_BIND_KEY, DE_KEY_K, DE_COMPOSITE_DOWN),
        bind(DE_BIND_KEY, DE_KEY_J, DE_COMPOSITE_LEFT),
        bind(DE_BIND_KEY, DE_KEY_L, DE_COMPOSITE_RIGHT),
    };
    const de_action look = add(in, m, "look_keys", DE_ACTION_AXIS2D, ijkl_b, 4);
    EXPECT_STREQ(de_action_glyph_id_family(in, look, DE_GLYPH_FAMILY_KBM), "kbm/key_i");

    /* The D-pad as a 2D composite. */
    const de_binding dpad_b[4] = {
        bind(DE_BIND_PAD_BUTTON, DE_PAD_DPAD_UP, DE_COMPOSITE_UP),
        bind(DE_BIND_PAD_BUTTON, DE_PAD_DPAD_DOWN, DE_COMPOSITE_DOWN),
        bind(DE_BIND_PAD_BUTTON, DE_PAD_DPAD_LEFT, DE_COMPOSITE_LEFT),
        bind(DE_BIND_PAD_BUTTON, DE_PAD_DPAD_RIGHT, DE_COMPOSITE_RIGHT),
    };
    const de_action menu = add(in, m, "menu_nav", DE_ACTION_AXIS2D, dpad_b, 4);
    EXPECT_STREQ(de_action_glyph_id_family(in, menu, DE_GLYPH_FAMILY_XBOX), "xbox/dpad");
    EXPECT_STREQ(de_action_glyph_id_family(in, menu, DE_GLYPH_FAMILY_PS5), "ps/dpad");
    EXPECT_STREQ(de_action_glyph_id_family(in, menu, DE_GLYPH_FAMILY_SWITCH), "switch/dpad");
    /* Three D-pad buttons are not the set: the first binding's ID. */
    const de_action part = add(in, m, "part", DE_ACTION_AXIS2D, dpad_b + 1, 3);
    EXPECT_STREQ(de_action_glyph_id_family(in, part, DE_GLYPH_FAMILY_XBOX), "xbox/dpad_down");

    /* Stick axes in composite slots: the direction of the side that feeds the slot. */
    de_binding stick_b[4] = {
        bind(DE_BIND_PAD_AXIS, DE_AXIS_RIGHT_Y, DE_COMPOSITE_UP),
        bind(DE_BIND_PAD_AXIS, DE_AXIS_RIGHT_Y, DE_COMPOSITE_DOWN),
        bind(DE_BIND_PAD_AXIS, DE_AXIS_RIGHT_X, DE_COMPOSITE_LEFT),
        bind(DE_BIND_PAD_AXIS, DE_AXIS_RIGHT_X, DE_COMPOSITE_RIGHT),
    };
    stick_b[1].invert = true;
    stick_b[2].scale = -1.0f;
    const de_action up = add(in, m, "aim_up", DE_ACTION_AXIS2D, stick_b, 4);
    const de_action down = add(in, m, "aim_down", DE_ACTION_AXIS2D, stick_b + 1, 3);
    const de_action left = add(in, m, "aim_left", DE_ACTION_AXIS2D, stick_b + 2, 2);
    const de_action right = add(in, m, "aim_right", DE_ACTION_AXIS2D, stick_b + 3, 1);
    EXPECT_STREQ(de_action_glyph_id_family(in, up, DE_GLYPH_FAMILY_XBOX), "xbox/stick_right_up");
    EXPECT_STREQ(de_action_glyph_id_family(in, down, DE_GLYPH_FAMILY_DECK),
                 "deck/stick_right_down");
    EXPECT_STREQ(de_action_glyph_id_family(in, left, DE_GLYPH_FAMILY_PS5), "ps/stick_right_left");
    EXPECT_STREQ(de_action_glyph_id_family(in, right, DE_GLYPH_FAMILY_SWITCH),
                 "switch/stick_right_right");
    /* The same axis outside a composite: the axis. */
    const de_binding axis_b = bind(DE_BIND_PAD_AXIS, DE_AXIS_LEFT_X, DE_COMPOSITE_NONE);
    const de_binding trig_b = bind(DE_BIND_PAD_AXIS, DE_AXIS_RIGHT_TRIGGER, DE_COMPOSITE_NONE);
    const de_action strafe = add(in, m, "strafe", DE_ACTION_AXIS1D, &axis_b, 1);
    const de_action fire = add(in, m, "fire", DE_ACTION_AXIS1D, &trig_b, 1);
    EXPECT_STREQ(de_action_glyph_id_family(in, strafe, DE_GLYPH_FAMILY_XBOX), "xbox/stick_left_x");
    EXPECT_STREQ(de_action_glyph_id_family(in, fire, DE_GLYPH_FAMILY_SWITCH), "switch/zr");
    EXPECT_STREQ(de_action_glyph_id_family(in, fire, DE_GLYPH_FAMILY_PS), "ps/r2");

    /* Mouse, wheel and gyro. */
    const de_binding look_b[2] = {
        bind(DE_BIND_MOUSE_DELTA, DE_BIND_AXIS_XY, DE_COMPOSITE_NONE),
        bind(DE_BIND_GYRO, DE_BIND_AXIS_XY, DE_COMPOSITE_NONE),
    };
    const de_action aim = add(in, m, "aim", DE_ACTION_AXIS2D, look_b, 2);
    EXPECT_STREQ(de_action_glyph_id_family(in, aim, DE_GLYPH_FAMILY_KBM), "kbm/mouse_move");
    EXPECT_STREQ(de_action_glyph_id_family(in, aim, DE_GLYPH_FAMILY_PS5), "generic/gyro");
    de_binding wheel_b[2] = {
        bind(DE_BIND_MOUSE_WHEEL, DE_BIND_AXIS_Y, DE_COMPOSITE_UP),
        bind(DE_BIND_MOUSE_WHEEL, DE_BIND_AXIS_Y, DE_COMPOSITE_DOWN),
    };
    wheel_b[1].invert = true;
    const de_action zoom_in = add(in, m, "zoom_in", DE_ACTION_AXIS2D, wheel_b, 2);
    const de_action zoom_out = add(in, m, "zoom_out", DE_ACTION_AXIS2D, wheel_b + 1, 1);
    EXPECT_STREQ(de_action_glyph_id_family(in, zoom_in, DE_GLYPH_FAMILY_KBM), "kbm/mouse_wheel_up");
    EXPECT_STREQ(de_action_glyph_id_family(in, zoom_out, DE_GLYPH_FAMILY_KBM),
                 "kbm/mouse_wheel_down");
    const de_binding wheel1 = bind(DE_BIND_MOUSE_WHEEL, DE_BIND_AXIS_Y, DE_COMPOSITE_NONE);
    const de_action scroll = add(in, m, "scroll", DE_ACTION_AXIS1D, &wheel1, 1);
    EXPECT_STREQ(de_action_glyph_id_family(in, scroll, DE_GLYPH_FAMILY_KBM), "kbm/mouse_wheel");
    const de_binding click = bind(DE_BIND_MOUSE_BUTTON, DE_MOUSE_RIGHT, DE_COMPOSITE_NONE);
    const de_action zoom = add(in, m, "zoom", DE_ACTION_BUTTON, &click, 1);
    EXPECT_STREQ(de_action_glyph_id_family(in, zoom, DE_GLYPH_FAMILY_KBM), "kbm/mouse_right");

    /* A chord (Shift+E): the modifier is not part of the ID. */
    de_binding chord = key(DE_KEY_E);
    chord.modifier_key = DE_KEY_LSHIFT;
    const de_action sprint_use = add(in, m, "sprint_use", DE_ACTION_BUTTON, &chord, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(sprint_use));
    EXPECT_STREQ(de_action_glyph_id_family(in, sprint_use, DE_GLYPH_FAMILY_KBM), "kbm/key_e");
    de_input_destroy(in);
}

/* ---- Change flag ------------------------------------------------------------------------------
 */

TEST(glyphs, family_changed_flag) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    input_test_frame(in);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_KBM);
    EXPECT_FALSE(de_input_glyph_family_changed(in)); /* KBM from the start: no change */

    input_test_key(in, DE_KEY_W, true);
    input_test_frame(in);
    EXPECT_FALSE(de_input_glyph_family_changed(in));

    /* Switch to a pad: set for exactly one frame. */
    input_test_pad_add(in, 3, DE_PLATFORM_PAD_XBOX, 0, "Xbox");
    input_test_pad_button(in, 3, DE_PAD_SOUTH, true);
    input_test_frame(in);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_XBOX);
    EXPECT_TRUE(de_input_glyph_family_changed(in));
    input_test_frame(in);
    EXPECT_FALSE(de_input_glyph_family_changed(in));
    input_test_pad_button(in, 3, DE_PAD_SOUTH, false);
    input_test_pad_button(in, 3, DE_PAD_EAST, true); /* same family: no change */
    input_test_frame(in);
    EXPECT_FALSE(de_input_glyph_family_changed(in));
    input_test_frame(in);
    EXPECT_FALSE(de_input_glyph_family_changed(in));

    /* Back to the keyboard. */
    input_test_key(in, DE_KEY_W, false);
    input_test_key(in, DE_KEY_SPACE, true);
    input_test_frame(in);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_KBM);
    EXPECT_TRUE(de_input_glyph_family_changed(in));
    input_test_frame(in);
    EXPECT_FALSE(de_input_glyph_family_changed(in));

    /* Pad, then keyboard and pad again inside one frame: the last event decides, and the family
     * that the frame ends with is compared with the previous frame's. */
    input_test_pad_button(in, 3, DE_PAD_EAST, false);
    input_test_pad_button(in, 3, DE_PAD_WEST, true);
    input_test_key(in, DE_KEY_SPACE, false);
    input_test_key(in, DE_KEY_E, true);
    input_test_pad_button(in, 3, DE_PAD_NORTH, true);
    input_test_frame(in);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_XBOX);
    EXPECT_TRUE(de_input_glyph_family_changed(in));
    input_test_frame(in);
    EXPECT_FALSE(de_input_glyph_family_changed(in));

    /* The last used pad disconnects, none left: back to KBM, for one frame. */
    input_test_pad_remove(in, 3);
    input_test_frame(in);
    EXPECT_EQ(de_input_glyph_family(in), DE_GLYPH_FAMILY_KBM);
    EXPECT_TRUE(de_input_glyph_family_changed(in));
    input_test_frame(in);
    EXPECT_FALSE(de_input_glyph_family_changed(in));
    de_input_destroy(in);
}

/* ---- Provider override ------------------------------------------------------------------------
 */

typedef struct provider_log {
    de_action jump;
    u32 calls;
    de_glyph_family last_family;
} provider_log;

static const char *steam_provider(const de_input *in, de_action a, de_glyph_family f, void *user) {
    (void)in;
    provider_log *log = (provider_log *)user;
    log->calls++;
    log->last_family = f;
    return a.id == log->jump.id ? "steam/button_a" : NULL;
}

TEST(glyphs, provider_override) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map m = de_action_map_create(in, "gameplay");
    const de_action jump = jump_action(in, m);
    const de_binding b = key(DE_KEY_F);
    const de_action use = add(in, m, "use", DE_ACTION_BUTTON, &b, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(jump) && DE_HANDLE_VALID(use));
    use_pad(in, 4, DE_PLATFORM_PAD_STEAM, "Steam Deck");
    EXPECT_STREQ(de_action_glyph_id(in, jump), "deck/a");

    provider_log log;
    memset(&log, 0, sizeof log);
    log.jump = jump;
    de_input_set_glyph_provider(in, steam_provider, &log);
    /* The provider comes first... */
    EXPECT_STREQ(de_action_glyph_id(in, jump), "steam/button_a");
    EXPECT_EQ(log.last_family, DE_GLYPH_FAMILY_DECK);
    EXPECT_STREQ(de_action_glyph_id_family(in, jump, DE_GLYPH_FAMILY_KBM), "steam/button_a");
    EXPECT_EQ(log.last_family, DE_GLYPH_FAMILY_KBM);
    /* ...and NULL falls back to the engine's table. */
    EXPECT_STREQ(de_action_glyph_id(in, use), "kbm/key_f");
    EXPECT_EQ(log.calls, 3u);

    /* Removed: the table again, and the provider is not called. */
    de_input_set_glyph_provider(in, NULL, &log);
    EXPECT_STREQ(de_action_glyph_id(in, jump), "deck/a");
    EXPECT_EQ(log.calls, 3u);
    de_input_set_glyph_provider(NULL, steam_provider, &log); /* no input: nothing */
    de_input_destroy(in);
}

/* ---- Key labels and key IDs -------------------------------------------------------------------
 */

static const char *german_labels(de_key k, void *user) {
    (*(u32 *)user)++;
    if (k == DE_KEY_SPACE)
        return "Leertaste";
    if (k == DE_KEY_Y)
        return "Z"; /* QWERTZ: the key at the Y position prints Z */
    return NULL;
}

TEST(glyphs, key_labels_and_key_ids) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    EXPECT_STREQ(de_input_key_label(in, DE_KEY_SPACE), "space");
    u32 calls = 0;
    de_input_set_key_label_fn(in, german_labels, &calls);
    EXPECT_STREQ(de_input_key_label(in, DE_KEY_SPACE), "Leertaste");
    EXPECT_STREQ(de_input_key_label(in, DE_KEY_Y), "Z");
    EXPECT_STREQ(de_input_key_label(in, DE_KEY_W), "w"); /* NULL: the stable name */
    EXPECT_EQ(calls, 3u);
    /* The label does not change the glyph ID: it names the key position. */
    const de_action_map m = de_action_map_create(in, "gameplay");
    const de_binding b = key(DE_KEY_Y);
    const de_action yes = add(in, m, "yes", DE_ACTION_BUTTON, &b, 1);
    EXPECT_STREQ(de_action_glyph_id_family(in, yes, DE_GLYPH_FAMILY_KBM), "kbm/key_y");
    de_input_set_key_label_fn(in, NULL, NULL);
    EXPECT_STREQ(de_input_key_label(in, DE_KEY_SPACE), "space");
    EXPECT_TRUE(de_input_key_label(in, (de_key)(DE_KEY_COUNT_ - 1)) == NULL);

    EXPECT_EQ(de_glyph_id_key("kbm/key_space"), DE_KEY_SPACE);
    EXPECT_EQ(de_glyph_id_key("kbm/key_kp_enter"), DE_KEY_KP_ENTER);
    EXPECT_EQ(de_glyph_id_key("kbm/key_SPACE"), DE_KEY_UNKNOWN); /* IDs are exact */
    EXPECT_EQ(de_glyph_id_key("kbm/key_"), DE_KEY_UNKNOWN);
    EXPECT_EQ(de_glyph_id_key("kbm/key_unknown"), DE_KEY_UNKNOWN);
    EXPECT_EQ(de_glyph_id_key("kbm/mouse_left"), DE_KEY_UNKNOWN);
    EXPECT_EQ(de_glyph_id_key("xbox/a"), DE_KEY_UNKNOWN);
    EXPECT_EQ(de_glyph_id_key(NULL), DE_KEY_UNKNOWN);
    de_input_destroy(in);
}

/* ---- Every ID is documented -------------------------------------------------------------------
 */

static bool id_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/* True when s[0..n) is "<family>/<control>" with a known family and a non-empty control. */
static bool is_glyph_id(const char *s, size_t n) {
    for (u32 f = 0; f < (u32)DE_GLYPH_FAMILY_COUNT_; ++f) {
        const char *fam = de_glyph_family_name((de_glyph_family)f);
        const size_t fl = strlen(fam);
        if (n <= fl + 1u || memcmp(s, fam, fl) != 0 || s[fl] != '/')
            continue;
        for (size_t i = fl + 1u; i < n; ++i)
            if (!id_char(s[i]))
                return false;
        return true;
    }
    return false;
}

static bool in_list(const char *const *list, u32 n, const char *s, size_t len) {
    for (u32 i = 0; i < n; ++i)
        if (strlen(list[i]) == len && memcmp(list[i], s, len) == 0)
            return true;
    return false;
}

TEST(glyphs, ids_documented) {
    /* The doc, whole. */
    FILE *fp = fopen(DE_GLYPH_DOC, "rb");
    ASSERT_TRUE(fp != NULL);
    ASSERT_EQ(fseek(fp, 0, SEEK_END), 0);
    const long size = ftell(fp);
    ASSERT_TRUE(size > 0);
    ASSERT_EQ(fseek(fp, 0, SEEK_SET), 0);
    char *doc = (char *)malloc((size_t)size + 1u);
    ASSERT_TRUE(doc != NULL);
    ASSERT_EQ(fread(doc, 1, (size_t)size, fp), (size_t)size);
    doc[size] = '\0';
    fclose(fp);

    /* The table's IDs. */
    const u32 n = de__glyph_table_ids(NULL, 0);
    ASSERT_TRUE(n > 300u);
    const char **ids = (const char **)malloc(sizeof(const char *) * n);
    ASSERT_TRUE(ids != NULL);
    ASSERT_EQ(de__glyph_table_ids(ids, n), n);

    /* Doc -> table: every backticked glyph ID in the doc is in the table. */
    u32 doc_ids = 0, missing_in_table = 0;
    const char **doc_list = (const char **)malloc(sizeof(const char *) * (size_t)size);
    size_t *doc_len = (size_t *)malloc(sizeof(size_t) * (size_t)size);
    ASSERT_TRUE(doc_list != NULL && doc_len != NULL);
    for (const char *p = doc; (p = strchr(p, '`')) != NULL;) {
        const char *start = p + 1;
        const char *end = strchr(start, '`');
        if (end == NULL)
            break;
        const size_t len = (size_t)(end - start);
        if (is_glyph_id(start, len)) {
            doc_list[doc_ids] = start;
            doc_len[doc_ids] = len;
            doc_ids++;
            if (!in_list(ids, n, start, len)) {
                fprintf(stderr, "glyph-ids.md lists `%.*s`, which glyph_table.inc does not have\n",
                        (int)len, start);
                missing_in_table++;
            }
        }
        p = end + 1;
    }
    EXPECT_GT(doc_ids, 300u);
    EXPECT_EQ(missing_in_table, 0u);

    /* Table -> doc: every ID of the table is in the doc, and is well formed. */
    u32 missing_in_doc = 0, malformed = 0, key_ids = 0;
    for (u32 i = 0; i < n; ++i) {
        const size_t len = strlen(ids[i]);
        if (!is_glyph_id(ids[i], len)) {
            fprintf(stderr, "glyph_table.inc: malformed ID \"%s\"\n", ids[i]);
            malformed++;
        }
        bool found = false;
        for (u32 j = 0; j < doc_ids && !found; ++j)
            found = doc_len[j] == len && memcmp(doc_list[j], ids[i], len) == 0;
        if (!found) {
            fprintf(stderr, "glyph_table.inc has \"%s\", which glyph-ids.md does not list\n",
                    ids[i]);
            missing_in_doc++;
        }
        if (strncmp(ids[i], "kbm/key_", 8) == 0 && strcmp(ids[i], "kbm/key_unknown") != 0)
            key_ids++;
    }
    EXPECT_EQ(malformed, 0u);
    EXPECT_EQ(missing_in_doc, 0u);
    /* The IDs of the acceptance criteria are in the table. */
    EXPECT_TRUE(in_list(ids, n, "kbm/key_space", 13));
    EXPECT_TRUE(in_list(ids, n, "ps/cross", 8));
    EXPECT_TRUE(in_list(ids, n, "switch/b", 8));
    EXPECT_TRUE(in_list(ids, n, "kbm/wasd", 8));
    EXPECT_TRUE(in_list(ids, n, "generic/unbound", 15));

    /* Keys: the ID of every named key is "kbm/key_" + de_key_name(), and only named keys have
     * one. de_glyph_id_key() compares with the table's entry for the key. */
    u32 named = 0, wrong = 0;
    for (u32 k = 0; k < (u32)DE_KEY_COUNT_; ++k) {
        const char *name = de_key_name((de_key)k);
        if (name == NULL)
            continue;
        named++;
        char want[64];
        snprintf(want, sizeof want, "kbm/key_%s", name);
        if (de_glyph_id_key(want) != (de_key)k || !in_list(ids, n, want, strlen(want))) {
            fprintf(stderr, "key %u: no glyph ID \"%s\"\n", k, want);
            wrong++;
        }
    }
    EXPECT_EQ(wrong, 0u);
    EXPECT_EQ(key_ids, named);

    free(doc_len);
    free(doc_list);
    free(ids);
    free(doc);
}
