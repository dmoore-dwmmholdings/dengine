/* test_haptics.c: rumble, trigger rumble, adaptive triggers and the haptics cvars (DENG-87). Pads
 * are injected (input_test_util.h) and haptics go to a mock device sink (haptics_internal.h), so
 * no device or SDL is needed. */
#include "input_test_util.h"

#include "input/haptics_internal.h"

#include "de_alloc_guard.h"
#include "de_test.h"

#include <dengine/de_debug.h>

#include <string.h>

/* ---- Mock sink ------------------------------------------------------------------------------- */

enum { CALL_RUMBLE, CALL_TRIGGER_RUMBLE, CALL_EFFECT };

typedef struct mock_call {
    int kind;
    u32 device;
    f32 a, b;
    u32 ms;
    u8 data[64];
    u32 size;
} mock_call;

typedef struct mock_sink {
    mock_call calls[128];
    u32 count;
    de_result fail; /* returned (with the error context set) when not DE_OK */
} mock_sink;

static de_result mock_result(mock_sink *m) {
    if (m->fail == DE_OK)
        return DE_OK;
    return DE_FAIL(m->fail, "mock sink refuses");
}

static mock_call *mock_push(mock_sink *m, int kind, u32 device) {
    static mock_call overflow;
    mock_call *c = m->count < 128u ? &m->calls[m->count++] : &overflow;
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->device = device;
    return c;
}

static de_result mock_rumble(void *user, u32 device, f32 low, f32 high, u32 ms) {
    mock_sink *m = (mock_sink *)user;
    mock_call *c = mock_push(m, CALL_RUMBLE, device);
    c->a = low;
    c->b = high;
    c->ms = ms;
    return mock_result(m);
}

static de_result mock_trigger_rumble(void *user, u32 device, f32 left, f32 right, u32 ms) {
    mock_sink *m = (mock_sink *)user;
    mock_call *c = mock_push(m, CALL_TRIGGER_RUMBLE, device);
    c->a = left;
    c->b = right;
    c->ms = ms;
    return mock_result(m);
}

static de_result mock_send_effect(void *user, u32 device, const void *data, u32 size) {
    mock_sink *m = (mock_sink *)user;
    mock_call *c = mock_push(m, CALL_EFFECT, device);
    c->size = size;
    memcpy(c->data, data, size < sizeof c->data ? size : sizeof c->data);
    return mock_result(m);
}

static void mock_install(de_input *in, mock_sink *m) {
    memset(m, 0, sizeof *m);
    de__haptics_sink s;
    s.rumble = mock_rumble;
    s.trigger_rumble = mock_trigger_rumble;
    s.send_effect = mock_send_effect;
    s.user = m;
    de__haptics_set_sink(in, &s);
}

/* Pads: 1 = Xbox (rumble + trigger rumble), 2 = DualSense (rumble, adaptive triggers), 3 = a
 * generic pad without haptics. */
#define PAD_XBOX 1u
#define PAD_PS5  2u
#define PAD_BARE 3u

static de_input *setup(mock_sink *m) {
    de_input *in = input_test_create(NULL);
    if (in == NULL)
        return NULL;
    mock_install(in, m);
    input_test_pad_add(in, PAD_XBOX, DE_PLATFORM_PAD_XBOX,
                       DE_PLATFORM_PAD_CAP_RUMBLE | DE_PLATFORM_PAD_CAP_TRIGGER_RUMBLE, "Xbox");
    input_test_pad_add(in, PAD_PS5, DE_PLATFORM_PAD_PS5,
                       DE_PLATFORM_PAD_CAP_RUMBLE | DE_PLATFORM_PAD_CAP_GYRO, "DualSense");
    input_test_pad_add(in, PAD_BARE, DE_PLATFORM_PAD_GENERIC, 0, "Bare");
    input_test_frame(in);
    return in;
}

/* Destroy stops running effects through the mock, which is still alive here. */
static void teardown(de_input *in) {
    de_input_destroy(in);
}

static bool is_stop(const mock_call *c) {
    return c->a == 0.0f && c->b == 0.0f && c->ms == 0u;
}

/* ---- Rumble ---------------------------------------------------------------------------------- */

TEST(haptics, rumble_duration) {
    mock_sink m;
    de_input *in = setup(&m);
    ASSERT_TRUE(in != NULL);
    const de_gamepad p = input_test_pad(in, PAD_XBOX);

    de_pad_rumble(in, p, 0.5f, 1.0f, 0.5f);
    ASSERT_EQ(m.count, 1u);
    EXPECT_EQ(m.calls[0].kind, CALL_RUMBLE);
    EXPECT_EQ(m.calls[0].device, PAD_XBOX);
    EXPECT_FLOAT_EQ(m.calls[0].a, 0.5f, 0.0f);
    EXPECT_FLOAT_EQ(m.calls[0].b, 1.0f, 0.0f);
    /* The device runs it a margin longer; the engine stops it. */
    EXPECT_EQ(m.calls[0].ms, 500u + DE__HAPTICS_DEVICE_MARGIN_MS);

    /* 0.5 s at 60 Hz is 30 frames: the stop comes in frame 30 (+- one frame), never earlier. */
    u32 stop_frame = 0;
    for (u32 f = 1; f <= 40 && stop_frame == 0; ++f) {
        input_test_frame(in);
        if (m.count > 1u)
            stop_frame = f;
    }
    EXPECT_TRUE(stop_frame >= 29u && stop_frame <= 31u);
    ASSERT_EQ(m.count, 2u);
    EXPECT_EQ(m.calls[1].kind, CALL_RUMBLE);
    EXPECT_TRUE(is_stop(&m.calls[1]));

    /* Nothing more once stopped. */
    for (u32 f = 0; f < 10; ++f)
        input_test_frame(in);
    EXPECT_EQ(m.count, 2u);
    teardown(in);
}

TEST(haptics, rumble_replace) {
    mock_sink m;
    de_input *in = setup(&m);
    ASSERT_TRUE(in != NULL);
    const de_gamepad p = input_test_pad(in, PAD_XBOX);

    de_pad_rumble(in, p, 1.0f, 1.0f, 0.5f);
    for (u32 f = 0; f < 10; ++f)
        input_test_frame(in);
    EXPECT_EQ(m.count, 1u); /* no resend while nothing changes */

    /* A newer call replaces the effect at once, with its own duration. */
    de_pad_rumble(in, p, 0.2f, 0.3f, 0.2f);
    ASSERT_EQ(m.count, 2u);
    EXPECT_FLOAT_EQ(m.calls[1].a, 0.2f, 0.0f);
    EXPECT_FLOAT_EQ(m.calls[1].b, 0.3f, 0.0f);
    EXPECT_EQ(m.calls[1].ms, 200u + DE__HAPTICS_DEVICE_MARGIN_MS);
    u32 stop_frame = 0;
    for (u32 f = 1; f <= 30 && stop_frame == 0; ++f) {
        input_test_frame(in);
        if (m.count > 2u)
            stop_frame = f;
    }
    EXPECT_TRUE(stop_frame >= 11u && stop_frame <= 13u); /* 0.2 s = 12 frames, not the 0.5 s */
    ASSERT_EQ(m.count, 3u);
    EXPECT_TRUE(is_stop(&m.calls[2]));

    /* The same values again still go out (the device timer is refreshed). */
    de_pad_rumble(in, p, 0.4f, 0.4f, 1.0f);
    de_pad_rumble(in, p, 0.4f, 0.4f, 1.0f);
    EXPECT_EQ(m.count, 5u);

    /* seconds <= 0 stops at once. */
    de_pad_rumble(in, p, 1.0f, 1.0f, 0.0f);
    ASSERT_EQ(m.count, 6u);
    EXPECT_TRUE(is_stop(&m.calls[5]));
    input_test_frame(in);
    EXPECT_EQ(m.count, 6u);

    /* Trigger rumble is a separate effect with its own timer. */
    de_pad_trigger_rumble(in, p, 0.6f, 0.7f, 0.1f);
    ASSERT_EQ(m.count, 7u);
    EXPECT_EQ(m.calls[6].kind, CALL_TRIGGER_RUMBLE);
    EXPECT_FLOAT_EQ(m.calls[6].a, 0.6f, 0.0f);
    EXPECT_FLOAT_EQ(m.calls[6].b, 0.7f, 0.0f);
    for (u32 f = 0; f < 8; ++f)
        input_test_frame(in);
    ASSERT_EQ(m.count, 8u);
    EXPECT_EQ(m.calls[7].kind, CALL_TRIGGER_RUMBLE);
    EXPECT_TRUE(is_stop(&m.calls[7]));
    teardown(in);
}

TEST(haptics, rumble_clamp) {
    mock_sink m;
    de_input *in = setup(&m);
    ASSERT_TRUE(in != NULL);
    const de_gamepad p = input_test_pad(in, PAD_XBOX);

    de_pad_rumble(in, p, 2.0f, -1.0f, 1.0f);
    ASSERT_EQ(m.count, 1u);
    EXPECT_FLOAT_EQ(m.calls[0].a, 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(m.calls[0].b, 0.0f, 0.0f);

    volatile f32 zero = 0.0f; /* not a constant: MSVC rejects a constant 0 / 0 */
    const f32 nan = zero / zero;
    de_pad_rumble(in, p, nan, 0.5f, 1.0f);
    ASSERT_EQ(m.count, 2u);
    EXPECT_FLOAT_EQ(m.calls[1].a, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(m.calls[1].b, 0.5f, 0.0f);

    /* Durations are cut to DE_PAD_RUMBLE_MAX_SECONDS; NaN stops. */
    de_pad_rumble(in, p, 0.5f, 0.5f, 1.0e6f);
    ASSERT_EQ(m.count, 3u);
    EXPECT_EQ(m.calls[2].ms,
              (u32)(DE_PAD_RUMBLE_MAX_SECONDS * 1000.0f) + DE__HAPTICS_DEVICE_MARGIN_MS);
    de_pad_rumble(in, p, 0.5f, 0.5f, nan);
    ASSERT_EQ(m.count, 4u);
    EXPECT_TRUE(is_stop(&m.calls[3]));

    de_pad_trigger_rumble(in, p, 1.5f, -0.5f, 1.0f);
    ASSERT_EQ(m.count, 5u);
    EXPECT_FLOAT_EQ(m.calls[4].a, 1.0f, 0.0f);
    EXPECT_FLOAT_EQ(m.calls[4].b, 0.0f, 0.0f);
    teardown(in);
}

TEST(haptics, unsupported_noop) {
    mock_sink m;
    de_input *in = setup(&m);
    ASSERT_TRUE(in != NULL);
    const de_gamepad bare = input_test_pad(in, PAD_BARE);
    const de_gamepad xbox = input_test_pad(in, PAD_XBOX);
    const de_gamepad ps5 = input_test_pad(in, PAD_PS5);
    de_trigger_effect fx;
    memset(&fx, 0, sizeof fx);
    fx.mode = DE_TRIGGER_FEEDBACK;
    fx.strength = 4;

    /* No capability: nothing reaches the device, twice (the log is once). */
    for (u32 k = 0; k < 2; ++k) {
        de_pad_rumble(in, bare, 1.0f, 1.0f, 1.0f);
        de_pad_trigger_rumble(in, bare, 1.0f, 1.0f, 1.0f);
        de_pad_adaptive_trigger(in, bare, true, &fx);
        de_pad_adaptive_trigger(in, xbox, true, &fx);     /* Xbox: no adaptive triggers */
        de_pad_trigger_rumble(in, ps5, 1.0f, 1.0f, 1.0f); /* DualSense: no impulse triggers */
    }
    input_test_frame(in);
    EXPECT_EQ(m.count, 0u);
    EXPECT_TRUE(de_error_last() == NULL || de_error_last()->result == DE_OK);

    /* Stale handles, invalid handles and a NULL input do nothing. */
    input_test_pad_remove(in, PAD_XBOX);
    input_test_frame(in);
    de_pad_rumble(in, xbox, 1.0f, 1.0f, 1.0f);
    de_pad_trigger_rumble(in, xbox, 1.0f, 1.0f, 1.0f);
    de_pad_rumble(in, (de_gamepad){0}, 1.0f, 1.0f, 1.0f);
    de_pad_rumble(NULL, ps5, 1.0f, 1.0f, 1.0f);
    de_pad_adaptive_trigger(NULL, ps5, false, &fx);
    EXPECT_EQ(m.count, 0u);

    /* A device that refuses: the call is made once, the error does not leak, the state goes on. */
    m.fail = DE_ERR_IO;
    de_pad_rumble(in, ps5, 1.0f, 1.0f, 0.05f);
    EXPECT_EQ(m.count, 1u);
    EXPECT_TRUE(de_error_last() == NULL || de_error_last()->result == DE_OK);
    for (u32 f = 0; f < 5; ++f)
        input_test_frame(in);
    EXPECT_EQ(m.count, 2u); /* the stop */
    m.fail = DE_OK;
    teardown(in);
}

/* ---- Adaptive triggers ----------------------------------------------------------------------- */

typedef struct trigger_case {
    de_trigger_effect fx;
    u8 block[DE__DS5_TRIGGER_BYTES];
} trigger_case;

/* The documented table (de_input.h, de_trigger_mode; Trama "Module: input"). */
static const trigger_case k_trigger_cases[] = {
    {{DE_TRIGGER_OFF, 0, 0, 0, 0}, {0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    /* Feedback from zone 3, strength 5: zones 3..9 active, force 4 (3 bits each). */
    {{DE_TRIGGER_FEEDBACK, 3, 0, 5, 0}, {0x21, 0xF8, 0x03, 0x00, 0x48, 0x92, 0x24, 0, 0, 0, 0}},
    /* Feedback from zone 0, strength 1: all zones, force 0. */
    {{DE_TRIGGER_FEEDBACK, 0, 0, 1, 0}, {0x21, 0xFF, 0x03, 0, 0, 0, 0, 0, 0, 0, 0}},
    /* Weapon from zone 2 to 5, strength 8: start and stop zone bits, strength - 1. */
    {{DE_TRIGGER_WEAPON, 2, 5, 8, 0}, {0x25, 0x24, 0x00, 0x07, 0, 0, 0, 0, 0, 0, 0}},
    {{DE_TRIGGER_WEAPON, 7, 8, 1, 0}, {0x25, 0x80, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0}},
    /* Vibration from zone 0, amplitude 3, 30 Hz: all zones with amplitude 2, frequency byte 9. */
    {{DE_TRIGGER_VIBRATION, 0, 0, 3, 30}, {0x26, 0xFF, 0x03, 0x92, 0x24, 0x49, 0x12, 0, 0, 30, 0}},
    /* Strength 0, and vibration at 0 Hz, are off. */
    {{DE_TRIGGER_FEEDBACK, 4, 0, 0, 0}, {0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {{DE_TRIGGER_VIBRATION, 4, 0, 5, 0}, {0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
};

TEST(haptics, adaptive_trigger_report_bytes) {
    mock_sink m;
    de_input *in = setup(&m);
    ASSERT_TRUE(in != NULL);
    const de_gamepad p = input_test_pad(in, PAD_PS5);
    de_gamepad_info info;
    ASSERT_TRUE(de_pad_info(in, p, &info));
    EXPECT_TRUE((info.caps & DE_PAD_CAP_ADAPTIVE_TRIGGERS) != 0);
    EXPECT_TRUE((info.caps & DE_PAD_CAP_HD_HAPTICS) == 0); /* SDL3 has no HD haptics */

    const u32 n = (u32)(sizeof k_trigger_cases / sizeof k_trigger_cases[0]);
    for (u32 i = 0; i < n; ++i) {
        const trigger_case *tc = &k_trigger_cases[i];
        u8 block[DE__DS5_TRIGGER_BYTES];
        ASSERT_TRUE(de__haptics_encode_trigger(&tc->fx, 1.0f, block));
        EXPECT_TRUE(memcmp(block, tc->block, sizeof block) == 0);

        /* Through the API, on both triggers: a 47-byte packet with one enable bit and the block
         * at that trigger's offset. */
        for (u32 side = 0; side < 2; ++side) {
            const bool right = side == 1u;
            m.count = 0;
            de_pad_adaptive_trigger(in, p, right, &tc->fx);
            ASSERT_EQ(m.count, 1u);
            const mock_call *c = &m.calls[0];
            EXPECT_EQ(c->kind, CALL_EFFECT);
            EXPECT_EQ(c->device, PAD_PS5);
            ASSERT_EQ(c->size, DE__DS5_EFFECT_PACKET_BYTES);
            u8 want[DE__DS5_EFFECT_PACKET_BYTES];
            memset(want, 0, sizeof want);
            want[0] = right ? 0x04 : 0x08;
            memcpy(want + (right ? 10 : 21), tc->block, DE__DS5_TRIGGER_BYTES);
            EXPECT_TRUE(memcmp(c->data, want, sizeof want) == 0);
        }
    }

    /* The effect stays: frames send nothing more. NULL turns it off. */
    m.count = 0;
    for (u32 f = 0; f < 5; ++f)
        input_test_frame(in);
    EXPECT_EQ(m.count, 0u);
    de_pad_adaptive_trigger(in, p, true, NULL);
    ASSERT_EQ(m.count, 1u);
    EXPECT_EQ(m.calls[0].data[10], 0x05);

    /* Fields out of range: ignored, nothing sent. */
    static const de_trigger_effect bad[] = {
        {DE_TRIGGER_FEEDBACK, 10, 0, 4, 0}, /* zone > 9 */
        {DE_TRIGGER_FEEDBACK, 2, 0, 9, 0},  /* strength > 8 */
        {DE_TRIGGER_WEAPON, 1, 5, 4, 0},    /* start < 2 */
        {DE_TRIGGER_WEAPON, 8, 9, 4, 0},    /* start > 7 */
        {DE_TRIGGER_WEAPON, 4, 4, 4, 0},    /* end <= start */
        {DE_TRIGGER_WEAPON, 4, 9, 4, 0},    /* end > 8 */
        {DE_TRIGGER_VIBRATION, 10, 0, 4, 9},  {DE_TRIGGER_VIBRATION, 0, 0, 9, 9},
        {DE_TRIGGER_MODE_COUNT_, 0, 0, 0, 0}, {200, 0, 0, 0, 0},
    };
    m.count = 0;
    for (u32 i = 0; i < (u32)(sizeof bad / sizeof bad[0]); ++i) {
        u8 block[DE__DS5_TRIGGER_BYTES];
        EXPECT_FALSE(de__haptics_encode_trigger(&bad[i], 1.0f, block));
        de_pad_adaptive_trigger(in, p, false, &bad[i]);
    }
    EXPECT_EQ(m.count, 0u);
    teardown(in);
}

/* ---- Cvars ----------------------------------------------------------------------------------- */

TEST(haptics, haptics_scale_cvar) {
    mock_sink m;
    de_input *in = setup(&m);
    ASSERT_TRUE(in != NULL);
    const de_cvar scale = de_cvar_find("input.haptics_scale");
    const de_cvar enabled = de_cvar_find("input.haptics_enabled");
    ASSERT_TRUE(DE_HANDLE_VALID(scale));
    ASSERT_TRUE(DE_HANDLE_VALID(enabled));
    EXPECT_TRUE((de_cvar_get_flags(scale) & DE_CVAR_SAVED) != 0);
    EXPECT_TRUE((de_cvar_get_flags(enabled) & DE_CVAR_SAVED) != 0);
    EXPECT_FLOAT_EQ(de__haptics_effective_scale(in), 1.0f, 0.0f);

    const de_gamepad xbox = input_test_pad(in, PAD_XBOX);
    const de_gamepad ps5 = input_test_pad(in, PAD_PS5);

    /* The scale applies to new effects ... */
    ASSERT_EQ(de_cvar_set(scale, "0.5"), DE_OK);
    de_pad_rumble(in, xbox, 1.0f, 0.8f, 1.0f);
    ASSERT_EQ(m.count, 1u);
    EXPECT_FLOAT_EQ(m.calls[0].a, 0.5f, 1e-6f);
    EXPECT_FLOAT_EQ(m.calls[0].b, 0.4f, 1e-6f);
    input_test_frame(in);
    EXPECT_EQ(m.count, 1u);

    /* ... and to running ones, from the next frame. */
    ASSERT_EQ(de_cvar_set(scale, "0.25"), DE_OK);
    input_test_frame(in);
    ASSERT_EQ(m.count, 2u);
    EXPECT_FLOAT_EQ(m.calls[1].a, 0.25f, 1e-6f);
    EXPECT_FLOAT_EQ(m.calls[1].b, 0.2f, 1e-6f);
    /* The remaining time (about 0.97 s) plus the margin. */
    EXPECT_TRUE(m.calls[1].ms > 900u + DE__HAPTICS_DEVICE_MARGIN_MS &&
                m.calls[1].ms <= 1000u + DE__HAPTICS_DEVICE_MARGIN_MS);

    /* Adaptive trigger strength scales too: strength 8 at 0.25 is 2 (force value 1). */
    de_trigger_effect fx = {DE_TRIGGER_FEEDBACK, 3, 0, 8, 0};
    de_pad_adaptive_trigger(in, ps5, true, &fx);
    ASSERT_EQ(m.count, 3u);
    EXPECT_EQ(m.calls[2].data[10], 0x21);
    EXPECT_EQ(m.calls[2].data[10 + 3], 0x00);
    EXPECT_EQ(m.calls[2].data[10 + 4], 0x92); /* 0x09249200: value 1 in zones 3..9 */
    EXPECT_EQ(m.calls[2].data[10 + 6], 0x09);
    ASSERT_EQ(de_cvar_set(scale, "0.5"), DE_OK);
    m.count = 0;
    input_test_frame(in); /* rumble resent at 0.5, the trigger at strength 4 (value 3) */
    ASSERT_EQ(m.count, 2u);
    EXPECT_EQ(m.calls[1].kind, CALL_EFFECT);
    EXPECT_EQ(m.calls[1].data[10 + 4], 0xB6);
    EXPECT_EQ(m.calls[1].data[10 + 5], 0x6D);
    EXPECT_EQ(m.calls[1].data[10 + 6], 0x1B);

    /* Disabled: running effects stop, the trigger goes off, new calls send nothing. */
    ASSERT_EQ(de_cvar_set(enabled, "false"), DE_OK);
    EXPECT_FLOAT_EQ(de__haptics_effective_scale(in), 0.0f, 0.0f);
    m.count = 0;
    input_test_frame(in);
    ASSERT_EQ(m.count, 2u);
    EXPECT_EQ(m.calls[0].kind, CALL_RUMBLE);
    EXPECT_TRUE(is_stop(&m.calls[0]));
    EXPECT_EQ(m.calls[1].kind, CALL_EFFECT);
    EXPECT_EQ(m.calls[1].data[10], 0x05);
    de_pad_rumble(in, xbox, 1.0f, 1.0f, 2.0f);
    input_test_frame(in);
    EXPECT_EQ(m.count, 2u);

    /* Enabled again: what still runs comes back. */
    ASSERT_EQ(de_cvar_set(enabled, "true"), DE_OK);
    input_test_frame(in);
    ASSERT_EQ(m.count, 4u);
    EXPECT_EQ(m.calls[2].kind, CALL_RUMBLE);
    EXPECT_FLOAT_EQ(m.calls[2].a, 0.5f, 1e-6f);
    EXPECT_EQ(m.calls[3].kind, CALL_EFFECT);
    EXPECT_EQ(m.calls[3].data[10], 0x21);

    ASSERT_EQ(de_cvar_set(scale, "1"), DE_OK);
    teardown(in);
}

/* ---- Lifetime -------------------------------------------------------------------------------- */

TEST(haptics, shutdown_and_reconnect) {
    mock_sink m;
    de_input *in = setup(&m);
    ASSERT_TRUE(in != NULL);
    de_gamepad xbox = input_test_pad(in, PAD_XBOX);
    const de_gamepad ps5 = input_test_pad(in, PAD_PS5);
    de_pad_rumble(in, xbox, 1.0f, 1.0f, 5.0f);

    /* A pad that reconnects into the slot starts with nothing running (and nothing is sent to
     * the old device). */
    input_test_pad_remove(in, PAD_XBOX);
    input_test_frame(in);
    input_test_pad_add(in, 9, DE_PLATFORM_PAD_XBOX, DE_PLATFORM_PAD_CAP_RUMBLE, "Xbox 2");
    input_test_frame(in);
    m.count = 0;
    for (u32 f = 0; f < 3; ++f)
        input_test_frame(in);
    EXPECT_EQ(m.count, 0u);

    /* Destroy stops what runs: rumble and a trigger effect. */
    xbox = input_test_pad(in, 9);
    de_pad_rumble(in, xbox, 1.0f, 1.0f, 5.0f);
    de_trigger_effect fx = {DE_TRIGGER_WEAPON, 2, 5, 8, 0};
    de_pad_adaptive_trigger(in, ps5, false, &fx);
    m.count = 0;
    de_input_destroy(in);
    ASSERT_EQ(m.count, 2u);
    EXPECT_EQ(m.calls[0].device, 9u);
    EXPECT_TRUE(is_stop(&m.calls[0]));
    EXPECT_EQ(m.calls[1].kind, CALL_EFFECT);
    EXPECT_EQ(m.calls[1].data[0], 0x08);
    EXPECT_EQ(m.calls[1].data[21], 0x05);
}

TEST(haptics, no_alloc_per_frame) {
    mock_sink m;
    de_input *in = setup(&m);
    ASSERT_TRUE(in != NULL);
    const de_gamepad xbox = input_test_pad(in, PAD_XBOX);
    const de_gamepad ps5 = input_test_pad(in, PAD_PS5);
    const de_gamepad bare = input_test_pad(in, PAD_BARE);
    de_trigger_effect fx = {DE_TRIGGER_VIBRATION, 0, 0, 3, 30};
    /* Warm up the log paths (missing capability, refused call). */
    de_pad_rumble(in, bare, 1.0f, 1.0f, 1.0f);
    m.fail = DE_ERR_IO;
    de_pad_rumble(in, xbox, 1.0f, 1.0f, 0.01f);
    m.fail = DE_OK;
    input_test_frame(in);

    de_alloc_guard g;
    memset(&g, 0, sizeof g);
    g.this_thread_only = true;
    de_alloc_guard_begin(&g);
    for (u32 i = 0; i < 200; ++i) {
        m.count = 0;
        if (i % 20u == 0u) {
            de_pad_rumble(in, xbox, 0.5f, 0.5f, 0.1f);
            de_pad_trigger_rumble(in, xbox, 0.5f, 0.5f, 0.1f);
            fx.frequency = (u8)(10u + i / 20u);
            de_pad_adaptive_trigger(in, ps5, (i / 20u) % 2u == 0u, &fx);
            de_pad_rumble(in, bare, 1.0f, 1.0f, 1.0f);
        }
        input_test_frame(in);
    }
    de_alloc_guard_end(&g);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g);
    teardown(in);
}
