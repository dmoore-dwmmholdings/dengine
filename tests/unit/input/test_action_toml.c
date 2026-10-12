/* test_action_toml.c: action-map files (de_action_maps_load, DENG-84): a full example file, the
 * name tables, unknown keys and the error messages. */
#include "input_test_util.h"

#include "input/action_internal.h"

#include "de_test.h"

#include <string.h>

#ifndef DE_INPUT_TEST_DATA_DIR
#error "DE_INPUT_TEST_DATA_DIR must be defined by tests/unit/input/CMakeLists.txt"
#endif

static void act_frame(de_input *in) {
    input_test_frame(in);
    de__actions_frame_update(in, INPUT_TEST_DT);
}

/* Parses `text`; returns the result and leaves the error message in `msg`. */
static de_result parse(de_input *in, const char *text, char *msg, size_t cap, u32 *unknown) {
    de_error_clear();
    const de_result r = de__action_maps_parse(in, text, strlen(text), "test.toml", unknown);
    const de_error *e = de_error_last();
    msg[0] = '\0';
    if (r != DE_OK && e != NULL) {
        size_t n = strlen(e->message);
        if (n >= cap)
            n = cap - 1;
        memcpy(msg, e->message, n);
        msg[n] = '\0';
    }
    de_error_clear();
    return r;
}

static bool contains(const char *s, const char *sub) {
    return strstr(s, sub) != NULL;
}

TEST(action_toml, load_basic_file) {
    ASSERT_EQ(de_fs_mount("game", DE_INPUT_TEST_DATA_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    ASSERT_EQ(de_action_maps_load(in, "game:/actions_basic.toml"), DE_OK);

    const de_action_map gp = de_action_map_find(in, "gameplay");
    const de_action_map menu = de_action_map_find(in, "menu");
    ASSERT_TRUE(DE_HANDLE_VALID(gp));
    ASSERT_TRUE(DE_HANDLE_VALID(menu));
    EXPECT_EQ(de__action_map_count(in), 2u);

    /* Actions in file order. */
    de_action list[16];
    ASSERT_EQ(de__action_map_actions(in, gp, list, 16), 8u);
    static const char *const names[8] = {"move",     "look",  "jump",     "super_jump",
                                         "interact", "dodge", "throttle", "fire"};
    for (u32 i = 0; i < 8; ++i)
        EXPECT_STREQ(de_action_name(in, list[i]), names[i]);
    EXPECT_EQ(de__action_map_actions(in, menu, NULL, 0), 2u);

    /* Fields. */
    const de__action *move = de__action_get_const(in, list[0]);
    ASSERT_TRUE(move != NULL);
    EXPECT_EQ((int)move->type, (int)DE_ACTION_AXIS2D);
    EXPECT_TRUE(move->rebindable);
    EXPECT_EQ(move->binding_count, 5u);
    de_binding b;
    ASSERT_TRUE(de__action_get_binding(in, list[0], 0, &b));
    EXPECT_EQ((int)b.source, (int)DE_BIND_KEY);
    EXPECT_EQ(b.code, (u32)DE_KEY_W);
    EXPECT_EQ((int)b.composite_slot, DE_COMPOSITE_UP);
    ASSERT_TRUE(de__action_get_binding(in, list[0], 4, &b));
    EXPECT_EQ(b.code, DE_BIND_LEFT_STICK);
    EXPECT_EQ((int)b.composite_slot, DE_COMPOSITE_NONE);
    EXPECT_FLOAT_EQ(b.deadzone, 0.2, 1e-6);

    ASSERT_TRUE(de__action_get_binding(in, list[1], 1, &b));
    EXPECT_EQ((int)b.curve.type, (int)DE_CURVE_POWER);
    EXPECT_FLOAT_EQ(b.curve.exponent, 2.0, 1e-6);
    EXPECT_TRUE(b.invert);
    ASSERT_TRUE(de__action_get_binding(in, list[1], 2, &b));
    EXPECT_EQ((int)b.source, (int)DE_BIND_GYRO);
    EXPECT_EQ(b.code, DE_BIND_AXIS_XY);

    ASSERT_TRUE(de__action_get_binding(in, list[3], 0, &b));
    EXPECT_EQ(b.modifier_key, (u32)DE_KEY_LSHIFT);

    const de__action *interact = de__action_get_const(in, list[4]);
    EXPECT_EQ((int)interact->interaction, (int)DE_INTERACT_HOLD);
    EXPECT_FLOAT_EQ(interact->hold_seconds, 0.4, 1e-6);
    const de__action *dodge = de__action_get_const(in, list[5]);
    EXPECT_EQ((int)dodge->interaction, (int)DE_INTERACT_DOUBLE_TAP);
    EXPECT_FLOAT_EQ(dodge->double_tap_seconds, 0.25, 1e-6);
    EXPECT_FLOAT_EQ(dodge->tap_seconds, DE_ACTION_DEFAULT_TAP_SECONDS, 1e-6);

    const de__action *throttle = de__action_get_const(in, list[6]);
    EXPECT_FLOAT_EQ(throttle->press_threshold, 0.1, 1e-6);
    ASSERT_TRUE(de__action_get_binding(in, list[6], 0, &b));
    EXPECT_EQ((int)b.curve.type, (int)DE_CURVE_CUSTOM);
    EXPECT_EQ(b.curve.point_count, 3u);
    EXPECT_FLOAT_EQ(b.curve.points[1].y, 0.2, 1e-6);
    ASSERT_TRUE(de__action_get_binding(in, list[6], 2, &b));
    EXPECT_EQ((int)b.composite_slot, DE_COMPOSITE_NEGATIVE);

    /* The maps work. */
    de_action_map_push(in, gp);
    input_test_key(in, DE_KEY_W, true);
    input_test_key(in, DE_KEY_D, true);
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    const de_vec2 v = de_action_value2(in, list[0]);
    EXPECT_FLOAT_EQ(v.x, 0.70710678, 1e-5);
    EXPECT_FLOAT_EQ(v.y, 0.70710678, 1e-5);
    EXPECT_TRUE(de_action_triggered(in, list[2]));

    /* Loading again updates in place: same handles, no new maps or actions. */
    const de_action jump = list[2];
    ASSERT_EQ(de_action_maps_load(in, "game:/actions_basic.toml"), DE_OK);
    EXPECT_EQ(de__action_map_count(in), 2u);
    EXPECT_EQ(de_action_find(in, gp, "jump").id, jump.id);
    EXPECT_EQ(in->actions->action_count, 10u);

    /* A missing file is the VFS error. */
    EXPECT_EQ(de_action_maps_load(in, "game:/missing.toml"), DE_ERR_NOT_FOUND);
    de_error_clear();
    de_input_destroy(in);
    EXPECT_EQ(de_fs_unmount("game", DE_INPUT_TEST_DATA_DIR), DE_OK);
}

TEST(action_toml, unknown_keys_warn) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    char msg[DE_ERROR_MESSAGE_MAX];
    u32 unknown = 0;
    const char *text = "colour = 1\n"
                       "[maps.ui]\n"
                       "priority = 3\n"
                       "[maps.ui.actions.ok]\n"
                       "sound = \"click\"\n"
                       "bindings = [{ key = \"return\", haptic = true }]\n";
    EXPECT_EQ(parse(in, text, msg, sizeof msg, &unknown), DE_OK);
    EXPECT_EQ(unknown, 4u);
    const de_action_map ui = de_action_map_find(in, "ui");
    EXPECT_TRUE(DE_HANDLE_VALID(de_action_find(in, ui, "ok")));
    de_input_destroy(in);
}

TEST(action_toml, errors_name_file_and_line) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    char msg[DE_ERROR_MESSAGE_MAX];

    /* A bad enum value. */
    EXPECT_EQ(parse(in,
                    "[maps.g.actions.jump]\n"
                    "type = \"trigger\"\n",
                    msg, sizeof msg, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(contains(msg, "test.toml:2: maps.g.actions.jump.type"));

    /* An unknown key name, on line 3. */
    EXPECT_EQ(parse(in,
                    "[maps.g.actions.jump]\n"
                    "interaction = \"press\"\n"
                    "bindings = [{ key = \"spacebar\" }]\n",
                    msg, sizeof msg, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(contains(msg, "test.toml:3:"));
    EXPECT_TRUE(contains(msg, "bindings[0].key"));
    EXPECT_TRUE(contains(msg, "spacebar"));

    /* A key in a 2D action without a composite. */
    EXPECT_EQ(parse(in,
                    "[maps.g.actions.move]\n"
                    "type = \"axis2d\"\n"
                    "bindings = [\n"
                    "  { key = \"w\" },\n"
                    "]\n",
                    msg, sizeof msg, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(contains(msg, "test.toml:4:"));
    EXPECT_TRUE(contains(msg, "composite"));

    /* Out of range, wrong type, two sources, no source, bad composite, custom without points. */
    static const char *const bad[] = {
        "[maps.g.actions.a]\nbindings = [{ pad_axis = \"left_x\", deadzone = 1.5 }]\n",
        "[maps.g.actions.a]\nhold_seconds = \"long\"\n",
        "[maps.g.actions.a]\nbindings = [{ key = \"a\", pad_button = \"south\" }]\n",
        "[maps.g.actions.a]\nbindings = [{ scale = 2.0 }]\n",
        "[maps.g.actions.a]\ntype = \"axis1d\"\nbindings = [{ key = \"a\", composite = \"up\" }]\n",
        ("[maps.g.actions.a]\ntype = \"axis1d\"\n"
         "bindings = [{ pad_axis = \"left_x\", curve = \"custom\" }]\n"),
        "[maps.g.actions.a]\nbindings = [{ key = \"a\", composite = \"up\" }]\n",
        "[maps.g]\nactions = 3\n",
        "maps = []\n",
        "[maps.g.actions.a]\nbindings = [{ key = \"a\", scale = 0 }]\n",
        "[maps.g.actions.a]\ntype = \"axis1d\"\nbindings = [{ pad_axis = \"left_stick\" }]\n",
    };
    for (u32 i = 0; i < (u32)(sizeof bad / sizeof bad[0]); ++i) {
        EXPECT_EQ(parse(in, bad[i], msg, sizeof msg, NULL), DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(contains(msg, "test.toml"));
    }

    /* Invalid TOML is a parse error. */
    EXPECT_EQ(parse(in, "[maps.g.actions.a\n", msg, sizeof msg, NULL), DE_ERR_PARSE);
    EXPECT_TRUE(contains(msg, "test.toml"));

    /* None of the bad files added anything. */
    EXPECT_EQ(de__action_map_count(in), 0u);

    /* A file whose second action is bad adds nothing either (checked before applying). */
    EXPECT_EQ(parse(in,
                    "[maps.g.actions.ok]\nbindings = [{ key = \"a\" }]\n"
                    "[maps.g.actions.bad]\nbindings = [{ key = \"nope\" }]\n",
                    msg, sizeof msg, NULL),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__action_map_count(in), 0u);
    de_input_destroy(in);
}

TEST(action_toml, name_tables) {
    /* Every code with a name maps back to itself (DENG-86 saves by name). */
    for (u32 s = 0; s < (u32)DE_BIND_SOURCE_COUNT_; ++s) {
        const char *sn = de__binding_source_name((de_binding_source)s);
        ASSERT_TRUE(sn != NULL);
        de_binding_source back;
        ASSERT_TRUE(de__binding_source_from_name(sn, strlen(sn), &back));
        EXPECT_EQ((u32)back, s);
        u32 named = 0;
        for (u32 c = 0; c < (u32)DE_KEY_COUNT_; ++c) {
            const char *cn = de__binding_code_name((de_binding_source)s, c);
            if (cn == NULL)
                continue;
            ++named;
            u32 code = 0xFFFFFFFFu;
            EXPECT_TRUE(de__binding_code_from_name((de_binding_source)s, cn, strlen(cn), &code));
            EXPECT_EQ(code, c);
        }
        EXPECT_GT(named, 0u);
    }
    EXPECT_STREQ(de__binding_code_name(DE_BIND_PAD_BUTTON, DE_PAD_SOUTH), "south");
    EXPECT_STREQ(de__binding_code_name(DE_BIND_PAD_AXIS, DE_BIND_RIGHT_STICK), "right_stick");
    EXPECT_STREQ(de__binding_code_name(DE_BIND_MOUSE_BUTTON, DE_MOUSE_X2), "x2");
    EXPECT_TRUE(de__binding_code_name(DE_BIND_MOUSE_DELTA, DE_BIND_AXIS_Z) == NULL);
    u32 code = 0;
    EXPECT_TRUE(de__binding_code_from_name(DE_BIND_KEY, "LShift", 6, &code));
    EXPECT_EQ(code, (u32)DE_KEY_LSHIFT);
    EXPECT_FALSE(de__binding_code_from_name(DE_BIND_MOUSE_DELTA, "z", 1, &code));
    EXPECT_STREQ(de__composite_name(DE_ACTION_AXIS2D, DE_COMPOSITE_RIGHT), "right");
    EXPECT_STREQ(de__composite_name(DE_ACTION_AXIS1D, DE_COMPOSITE_NEGATIVE), "negative");
    EXPECT_TRUE(de__composite_name(DE_ACTION_BUTTON, 0) == NULL);
    i8 slot = DE_COMPOSITE_NONE;
    EXPECT_TRUE(de__composite_from_name("left", 4, DE_ACTION_AXIS2D, &slot));
    EXPECT_EQ((int)slot, DE_COMPOSITE_LEFT);
    EXPECT_FALSE(de__composite_from_name("left", 4, DE_ACTION_AXIS1D, &slot));
}
