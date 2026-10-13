/* test_pack.c: netcode packing of one map's quantized tick view, and the per-player tick views
 * (DENG-89). */
#include "input_test_util.h"

#include "input/record_format.h"

#include "de_alloc_guard.h"
#include "de_test.h"

#include <string.h>

static de_binding key_binding(u32 code) {
    de_binding b;
    memset(&b, 0, sizeof b);
    b.source = DE_BIND_KEY;
    b.code = code;
    b.composite_slot = DE_COMPOSITE_NONE;
    return b;
}

static de_action add_action(de_input *in, de_action_map m, const char *name, de_action_type t) {
    de_binding b;
    memset(&b, 0, sizeof b);
    b.composite_slot = DE_COMPOSITE_NONE;
    if (t == DE_ACTION_BUTTON) {
        b = key_binding(DE_KEY_A);
    } else if (t == DE_ACTION_AXIS1D) {
        b.source = DE_BIND_PAD_AXIS;
        b.code = DE_AXIS_LEFT_TRIGGER;
    } else {
        b.source = DE_BIND_PAD_AXIS;
        b.code = DE_BIND_LEFT_STICK;
    }
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = t;
    d.bindings = &b;
    d.binding_count = 1;
    return de_action_add(in, m, &d);
}

/* Map "net": 16 buttons, then 2 sticks (the acceptance bound), with a 1D axis in another map
 * between them in declaration order, so the map's own order is what counts. */
typedef struct net_fixture {
    de_input *in;
    de_action_map m, other;
    de_action buttons[16];
    de_action sticks[2];
    de_action axis1d;
} net_fixture;

static bool net_setup(net_fixture *f) {
    memset(f, 0, sizeof *f);
    f->in = input_test_create(NULL);
    if (f->in == NULL)
        return false;
    f->m = de_action_map_create(f->in, "net");
    f->other = de_action_map_create(f->in, "other");
    char name[16];
    for (u32 i = 0; i < 16; ++i) {
        name[0] = 'b';
        name[1] = (char)('a' + i);
        name[2] = '\0';
        f->buttons[i] = add_action(f->in, f->m, name, DE_ACTION_BUTTON);
        if (i == 7)
            f->axis1d = add_action(f->in, f->other, "throttle", DE_ACTION_AXIS1D);
    }
    f->sticks[0] = add_action(f->in, f->m, "move", DE_ACTION_AXIS2D);
    f->sticks[1] = add_action(f->in, f->m, "look", DE_ACTION_AXIS2D);
    return DE_HANDLE_VALID(f->sticks[1]) && DE_HANDLE_VALID(f->axis1d);
}

static void set_tick(de_input *in, de_action a, bool held, bool triggered, f32 x, f32 y) {
    de__action_state s;
    memset(&s, 0, sizeof s);
    s.held = held;
    s.triggered = triggered;
    s.value.x = x;
    s.value.y = y;
    (void)de__action_tick_state_set(in, a, &s);
}

/** Quantized state of action `a` for player `player` (0: its tick view). */
static de__qstate q_of(de_input *in, de_action a, u32 player) {
    struct de__actions *s = de__actions_state(in);
    const u32 i = de__action_index(in, a);
    const de__action_state *st = player == 0 ? &s->actions[i].tick : &s->players[player - 1u][i];
    return de__quantize_state(&s->actions[i], st);
}

static bool q_eq(de__qstate a, de__qstate b) {
    return a.flags == b.flags && a.x == b.x && a.y == b.y;
}

TEST(pack, pack_size_bound) {
    net_fixture f;
    ASSERT_TRUE(net_setup(&f));
    EXPECT_EQ(de_input_packed_size(f.in, f.m), 12u); /* 16 * 2 + 2 * 32 bits */
    EXPECT_EQ(de_input_packed_size(f.in, f.other), 2u);
    u8 buf[64];
    EXPECT_EQ(de_input_pack_actions(f.in, f.m, buf, sizeof buf), 12u);
    EXPECT_EQ(de_input_pack_actions(f.in, f.other, buf, sizeof buf), 2u);
    /* A map of 3 buttons: 6 bits in one byte. An empty map packs nothing. */
    const de_action_map small = de_action_map_create(f.in, "small");
    for (u32 i = 0; i < 3; ++i) {
        char name[4] = {'s', (char)('0' + i), '\0', '\0'};
        add_action(f.in, small, name, DE_ACTION_BUTTON);
    }
    EXPECT_EQ(de_input_packed_size(f.in, small), 1u);
    const de_action_map empty = de_action_map_create(f.in, "empty");
    EXPECT_EQ(de_input_packed_size(f.in, empty), 0u);
    EXPECT_EQ(de_input_pack_actions(f.in, empty, buf, sizeof buf), 0u);
    de_input_destroy(f.in);
}

TEST(pack, pack_roundtrip_all_types) {
    net_fixture f;
    ASSERT_TRUE(net_setup(&f));
    /* Every button flag combination, sticks at the extremes and in between, values that are not
     * multiples of 1/32767, out of range and NaN. */
    for (u32 i = 0; i < 16; ++i)
        set_tick(f.in, f.buttons[i], (i & 1) != 0, (i & 2) != 0, 0.3f, 0.0f);
    set_tick(f.in, f.sticks[0], true, true, -1.0f, 0.123456f);
    set_tick(f.in, f.sticks[1], false, false, 2.0f, -0.5f);
    set_tick(f.in, f.axis1d, true, true, -0.000015f, 0.0f);
    u8 bytes[12];
    ASSERT_EQ(de_input_pack_actions(f.in, f.m, bytes, sizeof bytes), 12u);
    /* Buttons: 2 bits each, LSB first: held, triggered. b0 = 00, b1 = 01 (held), b2 = 10, b3 = 11
     * -> byte 0 = 0b11100100. */
    EXPECT_EQ(bytes[0], 0xE4u);
    /* move.x = -32767 = 0x8001 (LE at byte 4). */
    EXPECT_EQ(bytes[4], 0x01u);
    EXPECT_EQ(bytes[5], 0x80u);
    /* look.x clamped to 1 -> 32767 = 0x7FFF, look.y = -16384 = 0xC000. */
    EXPECT_EQ(bytes[8], 0xFFu);
    EXPECT_EQ(bytes[9], 0x7Fu);
    EXPECT_EQ(bytes[10], 0x00u);
    EXPECT_EQ(bytes[11], 0xC0u);

    ASSERT_TRUE(de_input_unpack_actions(f.in, f.m, 3, bytes, sizeof bytes));
    for (u32 i = 0; i < 16; ++i)
        EXPECT_TRUE(q_eq(q_of(f.in, f.buttons[i], 0), q_of(f.in, f.buttons[i], 3)));
    for (u32 i = 0; i < 2; ++i)
        EXPECT_TRUE(q_eq(q_of(f.in, f.sticks[i], 0), q_of(f.in, f.sticks[i], 3)));
    /* Bytes round trip too: pack(unpack(bytes)) == bytes, for player 0. */
    ASSERT_TRUE(de_input_unpack_actions(f.in, f.m, 0, bytes, sizeof bytes));
    u8 again[12];
    ASSERT_EQ(de_input_pack_actions(f.in, f.m, again, sizeof again), 12u);
    EXPECT_MEMEQ(bytes, again, sizeof bytes);
    /* Axis triggered never travels; held is derived from the value (move: |(-1, 0.12)| >= 0.5). */
    EXPECT_FALSE(de_action_triggered_p(f.in, f.sticks[0], 3));
    EXPECT_TRUE(de_action_held_p(f.in, f.sticks[0], 3));
    EXPECT_TRUE(de_action_held_p(f.in, f.sticks[1], 3)); /* (1, -0.5): held */

    /* Every valid stream of the 1D map round trips exactly (all 16-bit values but -32768). */
    for (u32 v = 0; v < 0x10000u; ++v) {
        if (v == 0x8000u)
            continue;
        const u8 in2[2] = {(u8)v, (u8)(v >> 8)};
        u8 out2[2];
        if (!de_input_unpack_actions(f.in, f.other, 0, in2, 2) ||
            de_input_pack_actions(f.in, f.other, out2, 2) != 2u || memcmp(in2, out2, 2) != 0) {
            EXPECT_EQ(v, 0xFFFFFFFFu);
            break;
        }
    }
    de_input_destroy(f.in);
}

TEST(pack, unpack_per_player) {
    net_fixture f;
    ASSERT_TRUE(net_setup(&f));
    u8 streams[DE_INPUT_MAX_PLAYERS][12];
    for (u32 p = 0; p < DE_INPUT_MAX_PLAYERS; ++p) {
        for (u32 i = 0; i < 16; ++i)
            set_tick(f.in, f.buttons[i], i == p, i == p + 1u, 0.0f, 0.0f);
        set_tick(f.in, f.sticks[0], false, false, (f32)p / 8.0f, -(f32)p / 8.0f);
        set_tick(f.in, f.sticks[1], false, false, 0.0f, 0.0f);
        ASSERT_EQ(de_input_pack_actions(f.in, f.m, streams[p], 12), 12u);
    }
    for (u32 p = 1; p < DE_INPUT_MAX_PLAYERS; ++p)
        ASSERT_TRUE(de_input_unpack_actions(f.in, f.m, p, streams[p], 12));
    EXPECT_FALSE(de_input_unpack_actions(f.in, f.m, DE_INPUT_MAX_PLAYERS, streams[0], 12));
    for (u32 p = 1; p < DE_INPUT_MAX_PLAYERS; ++p) {
        for (u32 i = 0; i < 16; ++i) {
            EXPECT_EQ(de_action_held_p(f.in, f.buttons[i], p), i == p);
            EXPECT_EQ(de_action_triggered_p(f.in, f.buttons[i], p), i == p + 1u);
            EXPECT_FLOAT_EQ(de_action_value_p(f.in, f.buttons[i], p), i == p ? 1.0 : 0.0, 0.0);
        }
        const de_vec2 v = de_action_value2_p(f.in, f.sticks[0], p);
        EXPECT_EQ(de__quantize_axis(v.x), de__quantize_axis((f32)p / 8.0f));
        EXPECT_EQ(de__quantize_axis(v.y), de__quantize_axis(-(f32)p / 8.0f));
        EXPECT_FLOAT_EQ(de_action_value_p(f.in, f.sticks[0], p), (f64)p / 8.0 * 1.41421356, 1e-4);
    }
    /* Player 0 is the action's own view (frame view outside FixedUpdate); a player slot nobody
     * unpacked into, a bad handle and a bad player read idle. */
    ASSERT_TRUE(de_input_unpack_actions(f.in, f.m, 0, streams[5], 12));
    de__action_state st;
    ASSERT_TRUE(de__action_tick_state_get(f.in, f.buttons[5], &st));
    EXPECT_TRUE(st.held);
    EXPECT_EQ(de_action_held_p(f.in, f.buttons[5], 0), de_action_held(f.in, f.buttons[5]));
    EXPECT_FALSE(de_action_held_p(f.in, f.axis1d, 4));
    EXPECT_FALSE(de_action_held_p(f.in, (de_action){0}, 1));
    EXPECT_FALSE(de_action_triggered_p(f.in, f.buttons[2], 99));
    EXPECT_FLOAT_EQ(de_action_value_p(f.in, f.buttons[1], 99), 0.0, 0.0);
    const de_vec2 z = de_action_value2_p(f.in, f.sticks[0], 99);
    EXPECT_FLOAT_EQ(z.x, 0.0, 0.0);
    de_input_destroy(f.in);
}

TEST(pack, pack_buffer_too_small) {
    net_fixture f;
    ASSERT_TRUE(net_setup(&f));
    u8 buf[12];
    memset(buf, 0xAB, sizeof buf);
    EXPECT_EQ(de_input_pack_actions(f.in, f.m, buf, 11), 0u);
    EXPECT_EQ(buf[0], 0xABu); /* nothing written */
    EXPECT_EQ(de_input_pack_actions(f.in, f.m, NULL, 12), 0u);
    EXPECT_EQ(de_input_pack_actions(f.in, (de_action_map){0}, buf, 12), 0u);
    EXPECT_EQ(de_input_pack_actions(NULL, f.m, buf, 12), 0u);
    EXPECT_EQ(de_input_packed_size(f.in, (de_action_map){12345}), 0u);
    ASSERT_EQ(de_input_pack_actions(f.in, f.m, buf, 12), 12u);

    /* Unpack refuses a wrong size, set padding bits and -32768, and changes nothing then. */
    set_tick(f.in, f.buttons[0], true, false, 0.0f, 0.0f);
    EXPECT_FALSE(de_input_unpack_actions(f.in, f.m, 0, buf, 11));
    EXPECT_FALSE(de_input_unpack_actions(f.in, f.m, 0, buf, 13));
    EXPECT_FALSE(de_input_unpack_actions(f.in, f.m, 0, NULL, 12));
    u8 bad[12];
    memcpy(bad, buf, sizeof bad);
    bad[4] = 0x00;
    bad[5] = 0x80; /* move.x = -32768 */
    EXPECT_FALSE(de_input_unpack_actions(f.in, f.m, 0, bad, 12));
    EXPECT_TRUE(de_action_held_p(f.in, f.buttons[0], 0) == de_action_held(f.in, f.buttons[0]));
    de__action_state st;
    ASSERT_TRUE(de__action_tick_state_get(f.in, f.buttons[0], &st));
    EXPECT_TRUE(st.held); /* unchanged */
    const de_action_map small = de_action_map_create(f.in, "small");
    add_action(f.in, small, "x", DE_ACTION_BUTTON);
    const u8 pad_set = 0x04; /* bit 2: past the 2 bits of the map */
    EXPECT_FALSE(de_input_unpack_actions(f.in, small, 0, &pad_set, 1));
    const u8 ok = 0x03;
    EXPECT_TRUE(de_input_unpack_actions(f.in, small, 0, &ok, 1));
    de_input_destroy(f.in);
}

TEST(pack, no_alloc_per_tick) {
    net_fixture f;
    ASSERT_TRUE(net_setup(&f));
    u8 buf[12];
    (void)de_input_pack_actions(f.in, f.m, buf, sizeof buf);
    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    for (u32 i = 0; i < 1000; ++i) {
        set_tick(f.in, f.sticks[0], false, false, (f32)(i % 100) / 100.0f, 0.5f);
        (void)de_input_pack_actions(f.in, f.m, buf, sizeof buf);
        (void)de_input_unpack_actions(f.in, f.m, 1u + (i % 7u), buf, sizeof buf);
        (void)de_action_value2_p(f.in, f.sticks[0], 1u + (i % 7u));
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    de_input_destroy(f.in);
}
