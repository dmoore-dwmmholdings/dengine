/* test_bindings_io.c: the user bindings file (de_bindings_save / de_bindings_load, DENG-86) and the
 * reset helpers. Each test mounts its own directory under DE_INPUT_TEST_TMP_DIR as user:/. */
#include "input_test_util.h"

#include "input/action_internal.h"
#include "platform/vfs_internal.h"

#include "de_test.h"

#include <stdio.h>
#include <string.h>

#ifndef DE_INPUT_TEST_TMP_DIR
#error "DE_INPUT_TEST_TMP_DIR must be defined by tests/unit/input/CMakeLists.txt"
#endif

/* ---- Helpers --------------------------------------------------------------------------------- */

static void mount_user(const char *test) {
    char dir[DE_FS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%s", DE_INPUT_TEST_TMP_DIR, test);
    ASSERT_EQ(de__native_mkdirs(dir), DE_OK);
    ASSERT_EQ(de_fs_mount("user", dir, DE_MOUNT_WRITABLE, 0), DE_OK);
    (void)de_file_delete("user:/bindings.toml");
    de_error_clear();
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
    d.bindings = b;
    d.binding_count = n;
    d.rebindable = rebindable;
    return de_action_add(in, m, &d);
}

/* The default maps: "gameplay" (jump, interact, move, look, throttle, pause) and "menu ui" (a
 * name that needs quoting) with confirm. The same calls give the same defaults every time. */
static de_input *make_input(void) {
    de_input *in = input_test_create(NULL);
    if (in == NULL)
        return NULL;
    const de_action_map gp = de_action_map_create(in, "gameplay");
    const de_binding jump[2] = {key(DE_KEY_SPACE), bind(DE_BIND_PAD_BUTTON, DE_PAD_SOUTH, -1)};
    add(in, gp, "jump", DE_ACTION_BUTTON, jump, 2, true);
    const de_binding interact[1] = {key(DE_KEY_F)};
    add(in, gp, "interact", DE_ACTION_BUTTON, interact, 1, true);
    const de_binding move[5] = {
        bind(DE_BIND_KEY, DE_KEY_W, DE_COMPOSITE_UP),
        bind(DE_BIND_KEY, DE_KEY_S, DE_COMPOSITE_DOWN),
        bind(DE_BIND_KEY, DE_KEY_A, DE_COMPOSITE_LEFT),
        bind(DE_BIND_KEY, DE_KEY_D, DE_COMPOSITE_RIGHT),
        bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_NONE),
    };
    add(in, gp, "move", DE_ACTION_AXIS2D, move, 5, true);
    de_binding look = bind(DE_BIND_PAD_AXIS, DE_BIND_RIGHT_STICK, DE_COMPOSITE_NONE);
    look.deadzone = 0.15f;
    add(in, gp, "look", DE_ACTION_AXIS2D, &look, 1, true);
    de_binding throttle = bind(DE_BIND_PAD_AXIS, DE_AXIS_RIGHT_TRIGGER, DE_COMPOSITE_NONE);
    throttle.curve.type = DE_CURVE_POWER;
    throttle.curve.exponent = 2.0f;
    add(in, gp, "throttle", DE_ACTION_AXIS1D, &throttle, 1, true);
    const de_binding pause[1] = {key(DE_KEY_P)};
    add(in, gp, "pause", DE_ACTION_BUTTON, pause, 1, false);
    const de_action_map menu = de_action_map_create(in, "menu ui");
    const de_binding confirm[2] = {key(DE_KEY_RETURN), bind(DE_BIND_PAD_BUTTON, DE_PAD_SOUTH, -1)};
    add(in, menu, "confirm", DE_ACTION_BUTTON, confirm, 2, true);
    return in;
}

static de_action find(const de_input *in, const char *map, const char *action) {
    return de_action_find(in, de_action_map_find(in, map), action);
}

static de_binding binding_of(const de_input *in, const char *map, const char *action, u32 i) {
    de_binding b;
    memset(&b, 0, sizeof b);
    EXPECT_TRUE(de__action_get_binding(in, find(in, map, action), i, &b));
    return b;
}

/* Every binding of every action of `a` equals the one of `b` (same maps and actions). */
static bool same_bindings(const de_input *a, const de_input *b) {
    const struct de__actions *sa = a->actions;
    const struct de__actions *sb = b->actions;
    if (sa->action_count != sb->action_count)
        return false;
    for (u32 i = 0; i < sa->action_count; ++i) {
        const de__action *x = &sa->actions[i];
        const de__action *y = &sb->actions[i];
        if (x->binding_count != y->binding_count || strcmp(x->name, y->name) != 0)
            return false;
        for (u32 j = 0; j < x->binding_count; ++j)
            if (!de__binding_same(x->type, &sa->bindings[x->first_binding + j],
                                  &sb->bindings[y->first_binding + j]))
                return false;
    }
    return true;
}

/* Reads user:/bindings.toml into `buf` (NUL-terminated). */
static void read_file(char *buf, size_t cap) {
    void *data = NULL;
    u64 size = 0;
    ASSERT_EQ(de_file_read_all("user:/bindings.toml", NULL, &data, &size), DE_OK);
    ASSERT_TRUE(size < cap);
    memcpy(buf, data, (size_t)size);
    buf[size] = '\0';
    DE_FREE(de_heap_allocator(), data, (size_t)size + 1);
}

static de_result parse(de_input *in, const char *text, u32 *warnings, char *msg, size_t cap) {
    de_error_clear();
    const de_result r = de__bindings_parse(in, text, strlen(text), "user:/bindings.toml", warnings);
    msg[0] = '\0';
    const de_error *e = de_error_last();
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

/* ---- Tests ----------------------------------------------------------------------------------- */

TEST(bindings_io, roundtrip_overrides_only) {
    mount_user("roundtrip");
    de_input *in = make_input();
    ASSERT_TRUE(in != NULL);

    /* No override yet: the file has only the header. */
    ASSERT_EQ(de_bindings_save(in, "user:/bindings.toml"), DE_OK);
    char text[8192];
    read_file(text, sizeof text);
    EXPECT_TRUE(strstr(text, "version = 1") != NULL);
    EXPECT_TRUE(strstr(text, "[maps") == NULL);

    /* Overrides of several kinds. */
    de_binding fk = key(DE_KEY_F);
    fk.modifier_key = DE_KEY_LSHIFT;
    ASSERT_EQ(de_rebind_apply(in, find(in, "gameplay", "jump"), 0, &fk, DE_REBIND_ALLOW), DE_OK);
    const de_binding east = bind(DE_BIND_PAD_BUTTON, DE_PAD_EAST, DE_COMPOSITE_NONE);
    ASSERT_EQ(de_rebind_apply(in, find(in, "gameplay", "jump"), 1, &east, DE_REBIND_ALLOW), DE_OK);
    const de_binding up = bind(DE_BIND_KEY, DE_KEY_I, DE_COMPOSITE_UP);
    ASSERT_EQ(de_rebind_apply(in, find(in, "gameplay", "move"), 0, &up, DE_REBIND_ALLOW), DE_OK);
    ASSERT_EQ(de_rebind_clear(in, find(in, "gameplay", "move"), 3), DE_OK);
    de_binding lstick = bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_NONE);
    lstick.deadzone = 0.2f;
    lstick.invert = true;
    lstick.scale = 1.75f;
    ASSERT_EQ(de_rebind_apply(in, find(in, "gameplay", "look"), 0, &lstick, DE_REBIND_ALLOW),
              DE_OK);
    de_binding lt = bind(DE_BIND_PAD_AXIS, DE_AXIS_LEFT_TRIGGER, DE_COMPOSITE_NONE);
    lt.curve.type = DE_CURVE_CUSTOM;
    lt.curve.point_count = 3;
    lt.curve.points[0] = (de_vec2){0.0f, 0.0f};
    lt.curve.points[1] = (de_vec2){0.3f, 0.1f};
    lt.curve.points[2] = (de_vec2){1.0f, 1.0f};
    ASSERT_EQ(de_rebind_apply(in, find(in, "gameplay", "throttle"), 0, &lt, DE_REBIND_ALLOW),
              DE_OK);
    const de_binding space = key(DE_KEY_SPACE);
    ASSERT_EQ(de_rebind_apply(in, find(in, "menu ui", "confirm"), 0, &space, DE_REBIND_ALLOW),
              DE_OK);
    /* Rebound and back again: equal to the default, so not written. */
    const de_binding x = key(DE_KEY_X);
    ASSERT_EQ(de_rebind_apply(in, find(in, "gameplay", "interact"), 0, &x, DE_REBIND_ALLOW), DE_OK);
    const de_binding f_again = key(DE_KEY_F);
    ASSERT_EQ(de_rebind_apply(in, find(in, "gameplay", "interact"), 0, &f_again, DE_REBIND_ALLOW),
              DE_OK);

    ASSERT_EQ(de_bindings_save(in, "user:/bindings.toml"), DE_OK);
    read_file(text, sizeof text);
    /* Only overrides: jump 0 and 1, move 0 and 3, look, throttle, confirm 0. */
    EXPECT_TRUE(strstr(text, "{ index = 0, key = \"f\", modifier = \"lshift\" }") != NULL);
    EXPECT_TRUE(strstr(text, "{ index = 1, pad_button = \"east\" }") != NULL);
    EXPECT_TRUE(strstr(text, "{ index = 0, key = \"i\", composite = \"up\" }") != NULL);
    EXPECT_TRUE(strstr(text, "{ index = 3, unbound = true, composite = \"right\" }") != NULL);
    EXPECT_TRUE(strstr(text, "[maps.\"menu ui\".actions.confirm]") != NULL);
    EXPECT_TRUE(strstr(text, "interact") == NULL);
    EXPECT_TRUE(strstr(text, "pause") == NULL);
    EXPECT_TRUE(strstr(text, "index = 4") == NULL); /* move's stick is unchanged */
    EXPECT_TRUE(strstr(text, "\"return\"") == NULL);

    /* A fresh input state with the same defaults reads it back exactly. */
    de_input *in2 = make_input();
    ASSERT_TRUE(in2 != NULL);
    EXPECT_FALSE(same_bindings(in, in2));
    ASSERT_EQ(de_bindings_load(in2, "user:/bindings.toml"), DE_OK);
    EXPECT_TRUE(same_bindings(in, in2));
    const de_binding m3 = binding_of(in2, "gameplay", "move", 3);
    EXPECT_TRUE(de_binding_is_unbound(&m3));
    EXPECT_EQ((int)m3.composite_slot, DE_COMPOSITE_RIGHT);

    /* Saving the loaded state gives the same file. */
    char text2[8192];
    ASSERT_EQ(de_bindings_save(in2, "user:/bindings.toml"), DE_OK);
    read_file(text2, sizeof text2);
    EXPECT_STREQ(text2, text);

    /* Loading replaces the user layer: a later override not in the file goes back to default. */
    const de_binding z = key(DE_KEY_Z);
    ASSERT_EQ(de_rebind_apply(in2, find(in2, "gameplay", "interact"), 0, &z, DE_REBIND_ALLOW),
              DE_OK);
    ASSERT_EQ(de_bindings_load(in2, "user:/bindings.toml"), DE_OK);
    EXPECT_EQ(binding_of(in2, "gameplay", "interact", 0).code, (u32)DE_KEY_F);
    EXPECT_TRUE(same_bindings(in, in2));

    /* A missing file is DE_ERR_NOT_FOUND and changes nothing. */
    EXPECT_EQ(de_bindings_load(in2, "user:/none.toml"), DE_ERR_NOT_FOUND);
    de_error_clear();
    EXPECT_TRUE(same_bindings(in, in2));
    de_input_destroy(in2);
    de_input_destroy(in);
}

TEST(bindings_io, unknown_action_skipped) {
    de_input *in = make_input();
    ASSERT_TRUE(in != NULL);
    const char *text = "version = 1\n"
                       "[maps.gameplay.actions.jump]\n"
                       "bindings = [ { index = 0, key = \"g\" },\n"
                       "             { index = 7, key = \"h\" },\n"            /* past the end */
                       "             { index = 1, key = \"no_such_key\" } ]\n" /* unknown name */
                       "[maps.gameplay.actions.removed_action]\n"
                       "bindings = [ { index = 0, key = \"k\" } ]\n"
                       "[maps.gameplay.actions.pause]\n" /* not rebindable */
                       "bindings = [ { index = 0, key = \"l\" } ]\n"
                       "[maps.gameplay.actions.move]\n" /* the composite does not fit */
                       "bindings = [ { index = 0, key = \"m\", composite = \"positive\" } ]\n"
                       "[maps.removed_map.actions.x]\n"
                       "bindings = [ { index = 0, key = \"n\" } ]\n"
                       "[maps.\"menu ui\".actions.confirm]\n"
                       "bindings = [ { index = 1, pad_button = \"east\", colour = \"red\" } ]\n";
    u32 warnings = 0;
    char msg[512];
    ASSERT_EQ(parse(in, text, &warnings, msg, sizeof msg), DE_OK);
    EXPECT_EQ(warnings, 7u);
    /* The valid entries are applied. */
    EXPECT_EQ(binding_of(in, "gameplay", "jump", 0).code, (u32)DE_KEY_G);
    EXPECT_EQ(binding_of(in, "gameplay", "jump", 1).code, (u32)DE_PAD_SOUTH);
    EXPECT_EQ(binding_of(in, "menu ui", "confirm", 1).code, (u32)DE_PAD_EAST);
    /* The skipped ones changed nothing. */
    EXPECT_EQ(binding_of(in, "gameplay", "pause", 0).code, (u32)DE_KEY_P);
    EXPECT_EQ(binding_of(in, "gameplay", "move", 0).code, (u32)DE_KEY_W);
    /* A key code without a name is written and read as a number. */
    const de_binding code = key((de_key)300);
    ASSERT_EQ(de_rebind_apply(in, find(in, "gameplay", "interact"), 0, &code, DE_REBIND_ALLOW),
              DE_OK);
    ASSERT_EQ(parse(in, "[maps.gameplay.actions.interact]\nbindings = [{ index = 0, key = 300 }]\n",
                    &warnings, msg, sizeof msg),
              DE_OK);
    EXPECT_EQ(warnings, 0u);
    EXPECT_EQ(binding_of(in, "gameplay", "interact", 0).code, 300u);
    de_input_destroy(in);
}

TEST(bindings_io, corrupt_file_keeps_defaults) {
    mount_user("corrupt");
    de_input *in = make_input();
    ASSERT_TRUE(in != NULL);
    de_input *ref = make_input();
    ASSERT_TRUE(ref != NULL);

    /* Invalid TOML. */
    const char *broken =
        "version = 1\n[maps.gameplay.actions.jump\nbindings = [ { index = 0, key = \"g\" ]\n";
    ASSERT_EQ(de_file_write_all_atomic("user:/bindings.toml", broken, strlen(broken)), DE_OK);
    de_error_clear();
    EXPECT_EQ(de_bindings_load(in, "user:/bindings.toml"), DE_ERR_PARSE);
    const de_error *e = de_error_last();
    ASSERT_TRUE(e != NULL);
    EXPECT_TRUE(strstr(e->message, "user:/bindings.toml") != NULL);
    de_error_clear();
    EXPECT_TRUE(same_bindings(in, ref));

    /* Bad values: the message names the file, the line and the key; nothing changes, also not
     * the valid entry before the bad one. */
    char msg[512];
    u32 warnings = 0;
    const char *bad_index = "version = 1\n"
                            "[maps.gameplay.actions.interact]\n"
                            "bindings = [ { index = 0, key = \"g\" } ]\n"
                            "[maps.gameplay.actions.jump]\n"
                            "bindings = [ { index = \"zero\", key = \"g\" } ]\n";
    EXPECT_EQ(parse(in, bad_index, &warnings, msg, sizeof msg), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(
        strstr(msg, "user:/bindings.toml:5: maps.gameplay.actions.jump.bindings[0].index") != NULL);
    EXPECT_TRUE(same_bindings(in, ref));

    static const char *const bad[] = {
        "version = 2\n",
        "maps = 3\n",
        "[maps.gameplay.actions.jump]\nbindings = { index = 0 }\n",
        "[maps.gameplay.actions.jump]\nbindings = [ { key = \"g\" } ]\n",
        "[maps.gameplay.actions.jump]\nbindings = [ { index = 0 } ]\n",
        "[maps.gameplay.actions.jump]\nbindings = [ { index = 0, key = \"g\", pad_button = "
        "\"east\" } ]\n",
        "[maps.gameplay.actions.jump]\nbindings = [ { index = 0, key = \"g\", deadzone = 1.5 } ]\n",
        "[maps.gameplay.actions.jump]\nbindings = [ { index = 0, key = 9999 } ]\n",
        "[maps.gameplay.actions.jump]\nbindings = [ { index = 0, unbound = false } ]\n",
        "[maps.gameplay.actions.jump]\nbindings = [ { index = 99, key = \"g\" } ]\n",
        "[maps.gameplay.actions.jump]\nbindings = [ { index = 0, key = \"g\", curve = \"custom\" "
        "} ]\n",
    };
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        EXPECT_EQ(parse(in, bad[i], &warnings, msg, sizeof msg), DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(strstr(msg, "user:/bindings.toml") != NULL);
        EXPECT_TRUE(same_bindings(in, ref));
    }

    /* A rebound state is kept too (the file never got applied). */
    const de_binding g = key(DE_KEY_G);
    ASSERT_EQ(de_rebind_apply(in, find(in, "gameplay", "jump"), 0, &g, DE_REBIND_ALLOW), DE_OK);
    EXPECT_EQ(parse(in, bad_index, &warnings, msg, sizeof msg), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(binding_of(in, "gameplay", "jump", 0).code, (u32)DE_KEY_G);
    de_input_destroy(ref);
    de_input_destroy(in);
}

TEST(bindings_io, reset_to_defaults) {
    mount_user("reset");
    de_input *in = make_input();
    ASSERT_TRUE(in != NULL);
    de_input *ref = make_input();
    ASSERT_TRUE(ref != NULL);
    const de_action jump = find(in, "gameplay", "jump");
    const de_action interact = find(in, "gameplay", "interact");
    const de_action confirm = find(in, "menu ui", "confirm");

    /* Resetting before any change does nothing. */
    EXPECT_EQ(de_bindings_reset_action(in, jump), DE_OK);
    de_bindings_reset_all(in);
    EXPECT_TRUE(same_bindings(in, ref));

    const de_binding g = key(DE_KEY_G);
    const de_binding h = key(DE_KEY_H);
    const de_binding k = key(DE_KEY_K);
    ASSERT_EQ(de_rebind_apply(in, jump, 0, &g, DE_REBIND_ALLOW), DE_OK);
    ASSERT_EQ(de_rebind_apply(in, interact, 0, &h, DE_REBIND_ALLOW), DE_OK);
    ASSERT_EQ(de_rebind_apply(in, confirm, 0, &k, DE_REBIND_ALLOW), DE_OK);
    ASSERT_EQ(de_rebind_clear(in, jump, 1), DE_OK);

    /* One action. */
    ASSERT_EQ(de_bindings_reset_action(in, jump), DE_OK);
    EXPECT_EQ(binding_of(in, "gameplay", "jump", 0).code, (u32)DE_KEY_SPACE);
    EXPECT_EQ(binding_of(in, "gameplay", "jump", 1).code, (u32)DE_PAD_SOUTH);
    EXPECT_EQ(binding_of(in, "gameplay", "interact", 0).code, (u32)DE_KEY_H);

    /* One map. */
    ASSERT_EQ(de_bindings_reset_map(in, de_action_map_find(in, "gameplay")), DE_OK);
    EXPECT_EQ(binding_of(in, "gameplay", "interact", 0).code, (u32)DE_KEY_F);
    EXPECT_EQ(binding_of(in, "menu ui", "confirm", 0).code, (u32)DE_KEY_K);

    /* All, after a SWAP that touched two actions. */
    ASSERT_EQ(de_rebind_apply(in, jump, 0,
                              &(de_binding){.source = DE_BIND_KEY,
                                            .code = DE_KEY_F,
                                            .composite_slot = DE_COMPOSITE_NONE},
                              DE_REBIND_SWAP),
              DE_OK);
    EXPECT_EQ(binding_of(in, "gameplay", "interact", 0).code, (u32)DE_KEY_SPACE);
    de_bindings_reset_all(in);
    EXPECT_TRUE(same_bindings(in, ref));

    /* After a reset, nothing is written. */
    ASSERT_EQ(de_bindings_save(in, "user:/bindings.toml"), DE_OK);
    char text[4096];
    read_file(text, sizeof text);
    EXPECT_TRUE(strstr(text, "[maps") == NULL);

    /* Adding an action again (reloading maps) makes its new bindings the defaults. */
    ASSERT_EQ(de_rebind_apply(in, interact, 0, &h, DE_REBIND_ALLOW), DE_OK);
    const de_binding new_default[1] = {key(DE_KEY_E)};
    ASSERT_TRUE(DE_HANDLE_VALID(add(in, de_action_map_find(in, "gameplay"), "interact",
                                    DE_ACTION_BUTTON, new_default, 1, true)));
    ASSERT_EQ(de_bindings_reset_action(in, interact), DE_OK);
    EXPECT_EQ(binding_of(in, "gameplay", "interact", 0).code, (u32)DE_KEY_E);
    ASSERT_EQ(de_rebind_apply(in, interact, 0, &h, DE_REBIND_ALLOW), DE_OK);
    ASSERT_EQ(de_bindings_reset_action(in, interact), DE_OK);
    EXPECT_EQ(binding_of(in, "gameplay", "interact", 0).code, (u32)DE_KEY_E);

    /* Bad handles. */
    EXPECT_EQ(de_bindings_reset_action(in, (de_action){0}), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_bindings_reset_map(in, (de_action_map){0}), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    de_input_destroy(ref);
    de_input_destroy(in);
}

TEST(bindings_io, save_errors) {
    de_input *in = make_input();
    ASSERT_TRUE(in != NULL);
    EXPECT_EQ(de_bindings_save(NULL, "user:/bindings.toml"), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_bindings_save(in, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_bindings_load(in, NULL), DE_ERR_INVALID_ARGUMENT);
    /* A root without a writable mount. */
    EXPECT_NE(de_bindings_save(in, "nomount:/bindings.toml"), DE_OK);
    de_error_clear();
    /* An input state without action maps saves an empty file and loads one. */
    de_input *empty = input_test_create(NULL);
    ASSERT_TRUE(empty != NULL);
    u32 warnings = 0;
    char msg[256];
    EXPECT_EQ(parse(empty, "version = 1\n[maps.gameplay.actions.jump]\nbindings = []\n", &warnings,
                    msg, sizeof msg),
              DE_OK);
    EXPECT_EQ(warnings, 1u);
    de_input_destroy(empty);
    de_input_destroy(in);
}
