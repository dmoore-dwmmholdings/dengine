/* test_context_stack.c: context stacks and named context sets (DENG-85): stack order and when
 * changes apply, blocking of lower maps, passthrough maps, the transitions of maps that leave or
 * join the stack, named sets, the fixed stack depth, and [contexts] in action-map files. Input is
 * injected with de__input_inject (input_test_util.h); each frame runs de_input's frame step and
 * then the action evaluation hook, as the input.actions system does. */
#include "input_test_util.h"

#include "input/action_internal.h"

#include "de_alloc_guard.h"
#include "de_test.h"

#include <string.h>

#ifndef DE_INPUT_TEST_DATA_DIR
#error "DE_INPUT_TEST_DATA_DIR must be defined by tests/unit/input/CMakeLists.txt"
#endif

#define EPS 1.0e-5

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

static de_action button(de_input *in, de_action_map m, const char *name, de_key k) {
    const de_binding b = key(k);
    return add(in, m, name, DE_ACTION_BUTTON, DE_INTERACT_PRESS, &b, 1);
}

static de_action wasd(de_input *in, de_action_map m, const char *name) {
    const de_binding b[4] = {
        bind(DE_BIND_KEY, DE_KEY_W, DE_COMPOSITE_UP),
        bind(DE_BIND_KEY, DE_KEY_S, DE_COMPOSITE_DOWN),
        bind(DE_BIND_KEY, DE_KEY_A, DE_COMPOSITE_LEFT),
        bind(DE_BIND_KEY, DE_KEY_D, DE_COMPOSITE_RIGHT),
    };
    return add(in, m, name, DE_ACTION_AXIS2D, DE_INTERACT_PRESS, b, 4);
}

/* The maps de__context_active_maps returns, by name, joined with ',' into `buf`. */
static const char *active_names(de_input *in, char *buf, size_t cap) {
    u32 maps[DE_ACTION_MAX_MAPS];
    const u32 n = de__context_active_maps(in, maps, DE_ACTION_MAX_MAPS);
    buf[0] = '\0';
    size_t len = 0;
    for (u32 i = 0; i < n; ++i) {
        const char *name = in->actions->maps[maps[i]].name;
        const size_t nl = strlen(name);
        if (len + nl + 2u > cap)
            break;
        if (i != 0)
            buf[len++] = ',';
        memcpy(buf + len, name, nl);
        len += nl;
        buf[len] = '\0';
    }
    return buf;
}

static de_result last_error(void) {
    const de_error *e = de_error_last();
    return e != NULL ? e->result : DE_OK;
}

/* ---- Stack order, and when changes apply ------------------------------------------------------
 */

TEST(context_stack, push_pop_order) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map a = de_action_map_create(in, "a");
    const de_action_map b = de_action_map_create(in, "b");
    const de_action_map c = de_action_map_create(in, "c");
    char buf[128];

    de_action_map_push(in, a);
    de_action_map_push(in, b);
    de_action_map_push(in, c);
    de_action_map_push(in, a); /* already on the stack: nothing */
    EXPECT_EQ(de__context_requested_depth(in), 3u);
    /* Requested, not applied: nothing is active before the next frame update. */
    EXPECT_FALSE(de_action_map_active(in, a));
    EXPECT_FALSE(de_action_map_active(in, c));
    act_frame(in);
    EXPECT_TRUE(de_action_map_active(in, a));
    EXPECT_TRUE(de_action_map_active(in, b));
    EXPECT_TRUE(de_action_map_active(in, c));
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "c,b,a"); /* top first */
    EXPECT_EQ(in->actions->active_count, 3u);                 /* DENG-84 fields: bottom to top */
    EXPECT_EQ(in->actions->active[0], 0u);
    EXPECT_EQ(in->actions->active[2], 2u);

    /* A pop below the top removes the map in place. */
    de_action_map_pop(in, b);
    EXPECT_TRUE(de_action_map_active(in, b)); /* until the next frame update */
    act_frame(in);
    EXPECT_FALSE(de_action_map_active(in, b));
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "c,a");

    /* Changes apply in call order: push b then pop it nets to nothing; pop c then push c moves
     * nothing either (c is on top again). */
    const u64 applies = in->actions->context.applies;
    de_action_map_push(in, b);
    de_action_map_pop(in, b);
    de_action_map_pop(in, c);
    de_action_map_push(in, c);
    act_frame(in);
    EXPECT_EQ(in->actions->context.applies, applies);
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "c,a");

    /* pop c, push b, push c: b then c above a. */
    de_action_map_pop(in, c);
    de_action_map_push(in, b);
    de_action_map_push(in, c);
    act_frame(in);
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "c,b,a");

    /* Popping a map that is not on the stack, or a bad handle, does nothing. */
    de_action_map_pop(in, b);
    de_action_map_pop(in, b);
    de_action_map_pop(in, (de_action_map){0});
    de_action_map_push(in, (de_action_map){12345});
    EXPECT_EQ(de__context_requested_depth(in), 2u);
    act_frame(in);
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "c,a");
    de_input_destroy(in);
}

/* ---- Blocking ---------------------------------------------------------------------------------
 */

TEST(context_stack, higher_map_consumes_input) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map gp = de_action_map_create(in, "gameplay");
    const de_action_map menu = de_action_map_create(in, "menu");
    const de_action jump = button(in, gp, "jump", DE_KEY_SPACE);
    const de_action move = wasd(in, gp, "move");
    const de_action confirm = button(in, menu, "confirm", DE_KEY_SPACE);
    ASSERT_TRUE(DE_HANDLE_VALID(jump) && DE_HANDLE_VALID(move) && DE_HANDLE_VALID(confirm));
    de_action_map_push(in, gp);
    act_frame(in);

    /* Gameplay alone: Space jumps. */
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, jump));
    EXPECT_FALSE(de_action_triggered(in, confirm));
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);

    /* Menu over gameplay: Space confirms only; W (not bound by the menu) passes through. */
    de_action_map_push(in, menu);
    act_frame(in);
    input_test_key(in, DE_KEY_SPACE, true);
    input_test_key(in, DE_KEY_W, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, confirm));
    EXPECT_FALSE(de_action_triggered(in, jump));
    EXPECT_FALSE(de_action_held(in, jump));
    EXPECT_FLOAT_EQ(de_action_value2(in, move).y, 1.0, EPS);
    act_frame(in);
    EXPECT_TRUE(de_action_held(in, confirm));
    EXPECT_FALSE(de_action_held(in, jump));
    input_test_key(in, DE_KEY_SPACE, false);
    input_test_key(in, DE_KEY_W, false);
    act_frame(in);

    /* After the pop: Space jumps only. */
    de_action_map_pop(in, menu);
    act_frame(in);
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, jump));
    EXPECT_FALSE(de_action_triggered(in, confirm));
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);

    /* The button that closes the menu does not jump: Confirm pops the menu in its press frame;
     * Space is still held when gameplay gets it back. */
    de_action_map_push(in, menu);
    act_frame(in);
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    ASSERT_TRUE(de_action_triggered(in, confirm));
    de_action_map_pop(in, menu); /* as a system would, in the same frame */
    act_frame(in);
    EXPECT_FALSE(de_action_triggered(in, jump));
    EXPECT_FALSE(de_action_held(in, jump));
    act_frame(in);
    EXPECT_FALSE(de_action_held(in, jump));
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, jump)); /* a new press */
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);
    de_input_destroy(in);
}

TEST(context_stack, overlapping_codes_consume) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map gp = de_action_map_create(in, "gameplay");
    const de_action_map menu = de_action_map_create(in, "menu");
    const de_binding steer_b = bind(DE_BIND_PAD_AXIS, DE_AXIS_LEFT_X, DE_COMPOSITE_NONE);
    const de_action steer = add(in, gp, "steer", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, &steer_b, 1);
    const de_binding look_b = bind(DE_BIND_MOUSE_DELTA, DE_BIND_AXIS_XY, DE_COMPOSITE_NONE);
    const de_action look = add(in, gp, "look", DE_ACTION_AXIS2D, DE_INTERACT_PRESS, &look_b, 1);
    const de_binding nav_b = bind(DE_BIND_PAD_AXIS, DE_BIND_LEFT_STICK, DE_COMPOSITE_NONE);
    const de_action nav = add(in, menu, "navigate", DE_ACTION_AXIS2D, DE_INTERACT_PRESS, &nav_b, 1);
    const de_binding cur_b = bind(DE_BIND_MOUSE_DELTA, DE_BIND_AXIS_X, DE_COMPOSITE_NONE);
    const de_action cur = add(in, menu, "cursor_x", DE_ACTION_AXIS1D, DE_INTERACT_PRESS, &cur_b, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(steer) && DE_HANDLE_VALID(look) && DE_HANDLE_VALID(nav) &&
                DE_HANDLE_VALID(cur));
    input_test_pad_add(in, 1, DE_PLATFORM_PAD_XBOX, 0, "Xbox");
    de_action_map_push(in, gp);
    de_action_map_push(in, menu);
    act_frame(in);

    /* The menu's whole stick consumes the gameplay's left X; its mouse X consumes the XY look. */
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 1.0f);
    input_test_mouse_move(in, 10.0f, 0.0f, 4.0f, 0.0f);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value2(in, nav).x, 1.0, EPS);
    EXPECT_FLOAT_EQ(de_action_value(in, cur), 4.0, EPS);
    EXPECT_FLOAT_EQ(de_action_value(in, steer), 0.0, EPS);
    EXPECT_FLOAT_EQ(de_action_value(in, look), 0.0, EPS);

    /* Without the menu, the gameplay reads both again once the stick was released (it was
     * consumed while held); the mouse never latches. */
    de_action_map_pop(in, menu);
    input_test_mouse_move(in, 10.0f, 0.0f, 2.0f, 0.0f);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, steer), 0.0, EPS);
    EXPECT_FLOAT_EQ(de_action_value2(in, look).x, 2.0, EPS);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 0.0f);
    act_frame(in);
    input_test_pad_axis(in, 1, DE_AXIS_LEFT_X, 1.0f);
    act_frame(in);
    EXPECT_FLOAT_EQ(de_action_value(in, steer), 1.0, EPS);
    de_input_destroy(in);
}

TEST(context_stack, passthrough_map) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map gp = de_action_map_create(in, "gameplay");
    const de_action_map photo = de_action_map_create(in, "photo");
    const de_action jump = button(in, gp, "jump", DE_KEY_SPACE);
    const de_action snap = button(in, photo, "snap", DE_KEY_SPACE);
    de_action_map_set_flags(in, photo, DE_INPUT_CONTEXT_PASSTHROUGH);
    EXPECT_EQ(de_action_map_flags(in, photo), DE_INPUT_CONTEXT_PASSTHROUGH);
    EXPECT_EQ(de_action_map_flags(in, gp), 0u);
    de_action_map_push(in, gp);
    de_action_map_push(in, photo);
    act_frame(in);

    /* The overlay sees Space and so does the map below. */
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, snap));
    EXPECT_TRUE(de_action_triggered(in, jump));
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);

    /* Flags apply from the next frame update: blocking again. */
    de_action_map_set_flags(in, photo, DE_INPUT_CONTEXT_BLOCKS_LOWER);
    act_frame(in);
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, snap));
    EXPECT_FALSE(de_action_triggered(in, jump));
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);

    /* A passthrough named context makes its maps passthrough. */
    de_action_map_pop(in, photo);
    ASSERT_EQ(de_input_context_define(in, "photo_mode", &photo, 1, DE_INPUT_CONTEXT_PASSTHROUGH),
              DE_OK);
    ASSERT_EQ(de_input_context_push(in, de_name_from("photo_mode")), DE_OK);
    act_frame(in);
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, snap));
    EXPECT_TRUE(de_action_triggered(in, jump));
    de_input_destroy(in);
}

/* ---- Transitions ------------------------------------------------------------------------------
 */

TEST(context_stack, release_on_deactivate) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map gp = de_action_map_create(in, "gameplay");
    const de_action_map menu = de_action_map_create(in, "menu");
    const de_action jump = button(in, gp, "jump", DE_KEY_SPACE);
    const de_action move = wasd(in, gp, "move");
    const de_binding sb = key(DE_KEY_SPACE);
    const de_action drop = add(in, gp, "drop", DE_ACTION_BUTTON, DE_INTERACT_RELEASE, &sb, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(button(in, menu, "confirm", DE_KEY_SPACE)));
    de_action_map_push(in, gp);
    act_frame(in);
    input_test_key(in, DE_KEY_SPACE, true);
    input_test_key(in, DE_KEY_W, true);
    act_frame(in);
    de__actions_fixed_tick(in, 1);
    act_frame(in);
    ASSERT_TRUE(de_action_held(in, jump));
    ASSERT_FLOAT_EQ(de_action_value2(in, move).y, 1.0, EPS);

    /* Pop: in the next frame update, the held actions release (held false, axes 0, the release
     * edge fires a RELEASE interaction), though the keys are still down. */
    de_action_map_pop(in, gp);
    act_frame(in);
    EXPECT_FALSE(de_action_map_active(in, gp));
    EXPECT_FALSE(de_action_held(in, jump));
    EXPECT_FALSE(de_action_triggered(in, jump));
    EXPECT_FLOAT_EQ(de_action_value2(in, move).x, 0.0, EPS);
    EXPECT_FLOAT_EQ(de_action_value2(in, move).y, 0.0, EPS);
    EXPECT_TRUE(de_action_triggered(in, drop));

    /* No fixed tick ran in that frame: the map keeps releasing, so the tick view still gets the
     * release trigger, once. */
    act_frame(in);
    EXPECT_FALSE(de_action_triggered(in, drop)); /* frame view: one frame only */
    EXPECT_TRUE(in->actions->context.releasing[0]);
    de__actions_fixed_tick(in, 2);
    de__action_state st;
    ASSERT_TRUE(de__action_tick_state_get(in, drop, &st));
    EXPECT_TRUE(st.triggered);
    EXPECT_FALSE(st.held);
    de__actions_fixed_tick(in, 3);
    ASSERT_TRUE(de__action_tick_state_get(in, drop, &st));
    EXPECT_FALSE(st.triggered);
    act_frame(in);
    EXPECT_FALSE(in->actions->context.releasing[0]); /* drained: an ordinary inactive map */
    EXPECT_FALSE(de_action_held(in, jump));

    /* A blocking map pushed over held gameplay input releases it in the same frame too. */
    input_test_key(in, DE_KEY_SPACE, false);
    input_test_key(in, DE_KEY_W, false);
    act_frame(in);
    de_action_map_push(in, gp);
    act_frame(in);
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    ASSERT_TRUE(de_action_held(in, jump));
    de_action_map_push(in, menu);
    act_frame(in);
    EXPECT_FALSE(de_action_held(in, jump));
    EXPECT_TRUE(de_action_triggered(in, drop));
    de_input_destroy(in);
}

TEST(context_stack, no_trigger_on_activate_while_held) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map menu = de_action_map_create(in, "menu");
    const de_binding cb[2] = {key(DE_KEY_SPACE), bind(DE_BIND_PAD_BUTTON, DE_PAD_SOUTH, 0)};
    const de_action confirm = add(in, menu, "confirm", DE_ACTION_BUTTON, DE_INTERACT_PRESS, cb, 2);
    const de_binding tb = bind(DE_BIND_PAD_AXIS, DE_AXIS_RIGHT_TRIGGER, DE_COMPOSITE_NONE);
    const de_action fire = add(in, menu, "fire", DE_ACTION_BUTTON, DE_INTERACT_PRESS, &tb, 1);
    ASSERT_TRUE(DE_HANDLE_VALID(confirm) && DE_HANDLE_VALID(fire));
    input_test_pad_add(in, 1, DE_PLATFORM_PAD_XBOX, 0, "Xbox");
    act_frame(in);

    /* Space, pad South and the right trigger are held before the menu is pushed. */
    input_test_key(in, DE_KEY_SPACE, true);
    input_test_pad_button(in, 1, DE_PAD_SOUTH, true);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_TRIGGER, 1.0f);
    act_frame(in);
    de_action_map_push(in, menu);
    act_frame(in);
    EXPECT_TRUE(de_action_map_active(in, menu));
    EXPECT_FALSE(de_action_triggered(in, confirm));
    EXPECT_FALSE(de_action_held(in, confirm));
    EXPECT_FALSE(de_action_triggered(in, fire));
    for (u32 i = 0; i < 3; ++i) {
        act_frame(in);
        EXPECT_FALSE(de_action_held(in, confirm));
        EXPECT_FALSE(de_action_held(in, fire));
    }

    /* Space released but South still held: still ignored. Then South released. */
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);
    EXPECT_FALSE(de_action_held(in, confirm));
    input_test_pad_button(in, 1, DE_PAD_SOUTH, false);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_TRIGGER, 0.0f);
    act_frame(in);

    /* A new press fires. */
    input_test_key(in, DE_KEY_SPACE, true);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_TRIGGER, 1.0f);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, confirm));
    EXPECT_TRUE(de_action_triggered(in, fire));
    input_test_key(in, DE_KEY_SPACE, false);
    input_test_pad_axis(in, 1, DE_AXIS_RIGHT_TRIGGER, 0.0f);
    act_frame(in);

    /* A press in the frame the push is applied is a new press (it came after the request). */
    de_action_map_pop(in, menu);
    act_frame(in);
    act_frame(in);
    de_action_map_push(in, menu);
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, confirm));

    /* Released and pressed again inside one frame while latched: a new press. */
    input_test_key(in, DE_KEY_SPACE, false);
    act_frame(in);
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    de_action_map_pop(in, menu);
    act_frame(in);
    act_frame(in);
    de_action_map_push(in, menu);
    act_frame(in);
    EXPECT_FALSE(de_action_held(in, confirm)); /* latched */
    input_test_key(in, DE_KEY_SPACE, false);
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, confirm));
    de_input_destroy(in);
}

/* ---- Named context sets -----------------------------------------------------------------------
 */

/* A frame with one fixed tick, so the triggers of maps that leave reach a tick and they drain at
 * once. */
static void frame_tick(de_input *in) {
    static u64 tick;
    act_frame(in);
    de__actions_fixed_tick(in, ++tick);
}

TEST(context_stack, named_context_set) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action_map gp = de_action_map_create(in, "gameplay");
    const de_action_map veh = de_action_map_create(in, "vehicle");
    const de_action_map menu = de_action_map_create(in, "menu");
    const de_action move = wasd(in, gp, "move");
    const de_action accel = button(in, veh, "accelerate", DE_KEY_W);
    const de_action horn = button(in, veh, "horn", DE_KEY_H);
    ASSERT_TRUE(DE_HANDLE_VALID(move) && DE_HANDLE_VALID(accel) && DE_HANDLE_VALID(horn));
    char buf[128];

    const de_action_map driving[2] = {gp, veh};
    ASSERT_EQ(de_input_context_define(in, "driving", driving, 2, 0), DE_OK);
    ASSERT_EQ(de_input_context_define(in, "pause", &menu, 1, 0), DE_OK);
    const de_name n_driving = de_name_from("driving");
    const de_name n_pause = de_name_from("pause");

    /* Set membership (DENG-86 rebinding conflicts). */
    const u32 gi = de__action_map_index(in, gp), vi = de__action_map_index(in, veh);
    const u32 mi = de__action_map_index(in, menu);
    EXPECT_TRUE(de__context_maps_share_set(in, gi, vi));
    EXPECT_TRUE(de__context_maps_share_set(in, vi, gi));
    EXPECT_TRUE(de__context_maps_share_set(in, mi, mi));
    EXPECT_FALSE(de__context_maps_share_set(in, gi, mi));
    EXPECT_FALSE(de__context_maps_share_set(in, gi, 31u));

    /* Push pushes the whole set, atomically, bottom to top; the vehicle blocks W below it. */
    ASSERT_EQ(de_input_context_push(in, n_driving), DE_OK);
    EXPECT_EQ(de__context_requested_depth(in), 2u);
    ASSERT_EQ(de_input_context_push(in, n_driving), DE_OK); /* on the stack: nothing */
    EXPECT_EQ(de__context_requested_depth(in), 2u);
    frame_tick(in);
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "vehicle,gameplay");
    input_test_key(in, DE_KEY_W, true);
    input_test_key(in, DE_KEY_D, true);
    frame_tick(in);
    EXPECT_TRUE(de_action_triggered(in, accel));
    EXPECT_FLOAT_EQ(de_action_value2(in, move).x, 1.0, EPS); /* D passes through */
    EXPECT_FLOAT_EQ(de_action_value2(in, move).y, 0.0, EPS); /* W is the vehicle's */
    input_test_key(in, DE_KEY_W, false);
    input_test_key(in, DE_KEY_D, false);
    frame_tick(in);

    /* Pause on top, then pop driving below it: removed in place, pause stays. */
    ASSERT_EQ(de_input_context_push(in, n_pause), DE_OK);
    frame_tick(in);
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "menu,vehicle,gameplay");
    ASSERT_EQ(de_input_context_pop(in, n_driving), DE_OK);
    frame_tick(in);
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "menu");
    ASSERT_EQ(de_input_context_pop(in, n_driving), DE_OK); /* not on the stack: nothing */
    ASSERT_EQ(de_input_context_pop(in, n_pause), DE_OK);
    frame_tick(in);
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "");

    /* A map pushed on its own stays when a set that also has it is popped. */
    de_action_map_push(in, gp);
    ASSERT_EQ(de_input_context_push(in, n_driving), DE_OK);
    EXPECT_EQ(de__context_requested_depth(in), 3u);
    frame_tick(in);
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "vehicle,gameplay");
    ASSERT_EQ(de_input_context_pop(in, n_driving), DE_OK);
    frame_tick(in);
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "gameplay");
    de_action_map_pop(in, gp);
    frame_tick(in);

    /* Errors. */
    de_error_clear();
    EXPECT_EQ(de_input_context_push(in, de_name_from("nope")), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de_input_context_pop(in, de_name_from("nope")), DE_ERR_NOT_FOUND);
    const de_action_map twice[2] = {gp, gp};
    EXPECT_EQ(de_input_context_define(in, "bad", twice, 2, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_input_context_define(in, "bad", driving, 0, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_input_context_define(in, "", driving, 2, 0), DE_ERR_INVALID_ARGUMENT);
    const de_action_map stale[1] = {{999u}};
    EXPECT_EQ(de_input_context_define(in, "bad", stale, 1, 0), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_input_context_define(NULL, "bad", driving, 2, 0), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
    EXPECT_EQ(de__context_find(in, de_name_from("bad")), DE__CONTEXT_NONE);

    /* Defining a name again replaces its maps. */
    ASSERT_EQ(de_input_context_define(in, "driving", &veh, 1, 0), DE_OK);
    EXPECT_FALSE(de__context_maps_share_set(in, gi, vi));
    EXPECT_EQ(in->actions->context.set_count, 2u);
    de_input_destroy(in);
}

/* ---- Fixed depth ------------------------------------------------------------------------------
 */

TEST(context_stack, stack_overflow_error) {
    de_tracking_allocator tracker;
    const de_allocator ta = de_tracking_allocator_init(&tracker, de_heap_allocator(), 0);
    de_input *in = input_test_create(&ta);
    ASSERT_TRUE(in != NULL);
    de_action_map maps[DE_INPUT_CONTEXT_STACK_MAX + 1];
    for (u32 i = 0; i < DE_INPUT_CONTEXT_STACK_MAX + 1; ++i) {
        char name[16];
        name[0] = 'm';
        name[1] = (char)('a' + i);
        name[2] = '\0';
        maps[i] = de_action_map_create(in, name);
        ASSERT_TRUE(DE_HANDLE_VALID(maps[i]));
        ASSERT_TRUE(DE_HANDLE_VALID(button(in, maps[i], "act", (de_key)(DE_KEY_A + i))));
    }
    ASSERT_EQ(de_input_context_define(in, "pair", maps, 2, 0), DE_OK);
    const de_name pair = de_name_from("pair");
    const u64 allocs = de_tracking_allocator_stats(&tracker).alloc_count;

    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    for (u32 i = 0; i < DE_INPUT_CONTEXT_STACK_MAX; ++i)
        de_action_map_push(in, maps[i]);
    EXPECT_EQ(de__context_requested_depth(in), (u32)DE_INPUT_CONTEXT_STACK_MAX);
    de_error_clear();
    de_action_map_push(in, maps[DE_INPUT_CONTEXT_STACK_MAX]); /* one too many */
    EXPECT_EQ(last_error(), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(in->actions->context.overflows, 1u);
    EXPECT_EQ(de__context_requested_depth(in), (u32)DE_INPUT_CONTEXT_STACK_MAX);

    /* A set that does not fit is refused whole. */
    de_action_map_pop(in, maps[0]);
    EXPECT_EQ(de_input_context_push(in, pair), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(in->actions->context.overflows, 2u);
    EXPECT_EQ(de__context_requested_depth(in), (u32)DE_INPUT_CONTEXT_STACK_MAX - 1u);
    de_error_clear();

    /* Busy frames with stack changes allocate nothing. */
    for (u32 i = 0; i < 100; ++i) {
        input_test_key(in, (de_key)(DE_KEY_A + (i % 17u)), (i & 1u) == 0);
        if (i % 7u == 0)
            de_action_map_pop(in, maps[i % 16u]);
        if (i % 5u == 0)
            de_action_map_push(in, maps[(i * 3u) % 17u]);
        act_frame(in);
        de__actions_fixed_tick(in, i);
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    EXPECT_EQ(de_tracking_allocator_stats(&tracker).alloc_count, allocs);
    de_error_clear();
    de_input_destroy(in);
}

/* ---- Files ------------------------------------------------------------------------------------
 */

static de_result parse(de_input *in, const char *text, char *msg, size_t cap) {
    de_error_clear();
    const de_result r = de__action_maps_parse(in, text, strlen(text), "test.toml", NULL);
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

TEST(context_stack, contexts_toml) {
    ASSERT_EQ(de_fs_mount("game", DE_INPUT_TEST_DATA_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    ASSERT_EQ(de_action_maps_load(in, "game:/contexts.toml"), DE_OK);
    char buf[128];

    const de__context_state *c = &in->actions->context;
    EXPECT_EQ(c->set_count, 3u);
    const u32 di = de__context_find(in, de_name_from("driving"));
    const u32 pi = de__context_find(in, de_name_from("photo"));
    ASSERT_TRUE(di != DE__CONTEXT_NONE && pi != DE__CONTEXT_NONE);
    EXPECT_STREQ(c->sets[di].name, "driving");
    EXPECT_EQ(c->sets[di].map_count, 2u);
    EXPECT_EQ(c->sets[di].flags, 0u);
    EXPECT_EQ(c->sets[pi].flags, DE_INPUT_CONTEXT_PASSTHROUGH);
    const de_action_map gp = de_action_map_find(in, "gameplay");
    const de_action_map veh = de_action_map_find(in, "vehicle");
    EXPECT_TRUE(de__context_maps_share_set(in, de__action_map_index(in, gp),
                                           de__action_map_index(in, veh)));
    /* The maps the sets named first still got their actions. */
    const de_action jump = de_action_find(in, gp, "jump");
    const de_action accel = de_action_find(in, veh, "accelerate");
    const de_action snap = de_action_find(in, de_action_map_find(in, "photo"), "snap");
    ASSERT_TRUE(DE_HANDLE_VALID(jump) && DE_HANDLE_VALID(accel) && DE_HANDLE_VALID(snap));

    /* The sets work. */
    ASSERT_EQ(de_input_context_push(in, de_name_from("driving")), DE_OK);
    ASSERT_EQ(de_input_context_push(in, de_name_from("photo")), DE_OK);
    act_frame(in);
    EXPECT_STREQ(active_names(in, buf, sizeof buf), "photo,vehicle,gameplay");
    input_test_key(in, DE_KEY_SPACE, true);
    act_frame(in);
    EXPECT_TRUE(de_action_triggered(in, snap));
    EXPECT_TRUE(de_action_triggered(in, jump)); /* photo is passthrough */

    /* Loading again updates in place. */
    ASSERT_EQ(de_action_maps_load(in, "game:/contexts.toml"), DE_OK);
    EXPECT_EQ(in->actions->context.set_count, 3u);
    EXPECT_EQ(de__action_map_count(in), 4u);

    /* Errors name the file, line and key; a bad file defines nothing. */
    char msg[512];
    EXPECT_EQ(parse(in, "[contexts.x]\nmaps = [\"nowhere\"]\n", msg, sizeof msg),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(msg, "test.toml:2: contexts.x.maps: unknown map 'nowhere'") != NULL);
    EXPECT_EQ(parse(in, "[contexts.x]\nmaps = [\"menu\", \"menu\"]\n", msg, sizeof msg),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(msg, "listed twice") != NULL);
    EXPECT_EQ(parse(in, "[contexts.x]\nblocks_lower = true\n", msg, sizeof msg),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(msg, "contexts.x: missing 'maps'") != NULL);
    EXPECT_EQ(parse(in, "[contexts.x]\nmaps = [\"menu\"]\nblocks_lower = 1\n", msg, sizeof msg),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(strstr(msg, "test.toml:3: contexts.x.blocks_lower") != NULL);
    EXPECT_EQ(parse(in, "[contexts.x]\nmaps = []\n", msg, sizeof msg), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(parse(in, "[contexts.x]\nmaps = [3]\n", msg, sizeof msg), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(parse(in, "contexts = 3\n", msg, sizeof msg), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(parse(in, "[contexts]\nx = 3\n", msg, sizeof msg), DE_ERR_INVALID_ARGUMENT);
    /* The first context of a bad file is not defined either, nor its new map created. */
    EXPECT_EQ(parse(in,
                    "[contexts.fine]\nmaps = [\"menu\", \"fresh\"]\n[contexts.bad]\nmaps = [1]\n"
                    "[maps.fresh.actions.a]\nbindings = [{ key = \"x\" }]\n",
                    msg, sizeof msg),
              DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__context_find(in, de_name_from("fine")), DE__CONTEXT_NONE);
    EXPECT_FALSE(DE_HANDLE_VALID(de_action_map_find(in, "fresh")));
    EXPECT_EQ(in->actions->context.set_count, 3u);

    /* Unknown keys warn and are skipped. */
    u32 unknown = 0;
    const char *ok = "[contexts.extra]\nmaps = [\"menu\"]\npriority = 3\n";
    EXPECT_EQ(de__action_maps_parse(in, ok, strlen(ok), "test.toml", &unknown), DE_OK);
    EXPECT_EQ(unknown, 1u);
    EXPECT_NE(de__context_find(in, de_name_from("extra")), DE__CONTEXT_NONE);
    de_error_clear();
    de_input_destroy(in);
    EXPECT_EQ(de_fs_unmount("game", DE_INPUT_TEST_DATA_DIR), DE_OK);
}
