/* test_action_interactions.c: press, release, hold, tap and double-tap, and the per-tick view of
 * the action state (DENG-84). Frames use an explicit dt, so the times are exact. */
#include "input_test_util.h"

#include "input/action_internal.h"

#include "de_test.h"

#include <dengine/de_reflect.h>

#include <string.h>

#define DT 0.05f

static void frame_dt(de_input *in, f32 dt) {
    de__input_begin_frame_dt(in, dt);
    de__actions_frame_update(in, dt);
}

static void frame(de_input *in) {
    frame_dt(in, DT);
}

static de_action button(de_input *in, const char *name, de_interaction it, f32 hold_seconds) {
    de_action_map m = de_action_map_create(in, "gameplay");
    de_action_map_push(in, m);
    de_binding b;
    memset(&b, 0, sizeof b);
    b.source = DE_BIND_KEY;
    b.code = DE_KEY_SPACE;
    b.composite_slot = DE_COMPOSITE_NONE;
    de_action_desc d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = DE_ACTION_BUTTON;
    d.interaction = it;
    d.hold_seconds = hold_seconds;
    d.bindings = &b;
    d.binding_count = 1;
    return de_action_add(in, m, &d);
}

static void space(de_input *in, bool down) {
    input_test_key(in, DE_KEY_SPACE, down);
}

TEST(action_interactions, release) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action a = button(in, "drop", DE_INTERACT_RELEASE, 0.0f);
    ASSERT_TRUE(DE_HANDLE_VALID(a));
    space(in, true);
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    EXPECT_TRUE(de_action_held(in, a));
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    space(in, false);
    frame(in);
    EXPECT_TRUE(de_action_triggered(in, a)); /* the up edge */
    EXPECT_FALSE(de_action_held(in, a));
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    /* Down and up in one frame: a press and a release. */
    space(in, true);
    space(in, false);
    frame(in);
    EXPECT_TRUE(de_action_triggered(in, a));
    de_input_destroy(in);
}

TEST(action_interactions, hold) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action a = button(in, "charge", DE_INTERACT_HOLD, 0.125f);
    ASSERT_TRUE(DE_HANDLE_VALID(a));

    /* Held time: 0 in the press frame, then + dt per frame: 0.05, 0.10, 0.15 >= 0.125. */
    space(in, true);
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    frame(in);
    EXPECT_TRUE(de_action_triggered(in, a));
    /* Once per press. */
    for (u32 i = 0; i < 10; ++i) {
        frame(in);
        EXPECT_FALSE(de_action_triggered(in, a));
    }
    EXPECT_TRUE(de_action_held(in, a));

    /* Released early: no trigger, and the next press starts from 0. */
    space(in, false);
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    space(in, true);
    frame(in);
    frame(in);
    space(in, false);
    frame(in);
    for (u32 i = 0; i < 5; ++i)
        frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));

    /* The default hold time (0.5 s) when hold_seconds is 0. */
    const de_action d = button(in, "use", DE_INTERACT_HOLD, 0.0f);
    space(in, true);
    frame_dt(in, 0.1f);
    u32 fired_at = 0;
    for (u32 i = 1; i <= 8; ++i) {
        frame_dt(in, 0.1f);
        if (de_action_triggered(in, d))
            fired_at = i;
    }
    EXPECT_EQ(fired_at, 5u);
    de_input_destroy(in);
}

TEST(action_interactions, tap) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action a = button(in, "tap", DE_INTERACT_TAP, 0.0f); /* tap time 0.2 s */
    ASSERT_TRUE(DE_HANDLE_VALID(a));

    /* Released after 0.10 s: a tap, in the release frame only. */
    space(in, true);
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    space(in, false);
    frame(in);
    EXPECT_TRUE(de_action_triggered(in, a));
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));

    /* Released after 0.30 s: no tap. */
    space(in, true);
    for (u32 i = 0; i < 6; ++i) {
        frame(in);
        EXPECT_FALSE(de_action_triggered(in, a));
    }
    space(in, false);
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));

    /* Down and up inside one frame: a tap. */
    space(in, true);
    space(in, false);
    frame(in);
    EXPECT_TRUE(de_action_triggered(in, a));
    de_input_destroy(in);
}

TEST(action_interactions, double_tap) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action a = button(in, "dodge", DE_INTERACT_DOUBLE_TAP, 0.0f); /* 0.2 s / 0.3 s */
    ASSERT_TRUE(DE_HANDLE_VALID(a));

    /* Tap, 0.10 s gap, tap: triggers on the second release. */
    space(in, true);
    frame(in);
    space(in, false);
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    frame(in);
    space(in, true);
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    space(in, false);
    frame(in);
    EXPECT_TRUE(de_action_triggered(in, a));

    /* A third tap right after starts a new pair: no trigger. */
    space(in, true);
    frame(in);
    space(in, false);
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    for (u32 i = 0; i < 10; ++i) /* let it expire */
        frame(in);

    /* Two taps with a 0.40 s gap: too slow. */
    space(in, true);
    frame(in);
    space(in, false);
    frame(in);
    for (u32 i = 0; i < 7; ++i)
        frame(in);
    space(in, true);
    frame(in);
    space(in, false);
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    /* ... but that second tap starts a pair, so one more quick tap triggers. */
    frame(in);
    space(in, true);
    frame(in);
    space(in, false);
    frame(in);
    EXPECT_TRUE(de_action_triggered(in, a));

    /* Two taps inside single frames each. */
    for (u32 i = 0; i < 10; ++i)
        frame(in);
    space(in, true);
    space(in, false);
    frame(in);
    EXPECT_FALSE(de_action_triggered(in, a));
    frame(in);
    space(in, true);
    space(in, false);
    frame(in);
    EXPECT_TRUE(de_action_triggered(in, a));
    de_input_destroy(in);
}

/* ---- Tick view ------------------------------------------------------------------------------- */

static bool tick_triggered(const de_input *in, de_action a) {
    de__action_state s;
    de__action_tick_state_get(in, a, &s);
    return s.triggered;
}

TEST(action_interactions, tick_view_latch_n_ticks) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action a = button(in, "jump", DE_INTERACT_PRESS, 0.0f);
    ASSERT_TRUE(DE_HANDLE_VALID(a));
    u64 tick = 0;

    /* One press in a frame with 3 fixed ticks: triggered in exactly one of them. */
    space(in, true);
    frame(in);
    u32 seen = 0;
    for (u32 i = 0; i < 3; ++i) {
        de__actions_fixed_tick(in, tick++);
        if (tick_triggered(in, a))
            ++seen;
        de__action_state s;
        de__action_tick_state_get(in, a, &s);
        EXPECT_TRUE(s.held);
        EXPECT_FLOAT_EQ(s.value.x, 1.0, 1e-6);
    }
    EXPECT_EQ(seen, 1u);
    /* The next frame, held: no trigger in its ticks. */
    frame(in);
    de__actions_fixed_tick(in, tick++);
    EXPECT_FALSE(tick_triggered(in, a));
    space(in, false);
    frame(in);
    de__actions_fixed_tick(in, tick++);
    EXPECT_FALSE(tick_triggered(in, a));
    de__action_state s;
    de__action_tick_state_get(in, a, &s);
    EXPECT_FALSE(s.held);
    de_input_destroy(in);
}

TEST(action_interactions, tick_view_latch_zero_ticks) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action a = button(in, "jump", DE_INTERACT_PRESS, 0.0f);
    ASSERT_TRUE(DE_HANDLE_VALID(a));

    /* A press in a frame with no fixed tick is reported by the next tick, in a later frame. */
    space(in, true);
    frame(in); /* 0 ticks */
    EXPECT_TRUE(de_action_triggered(in, a));
    frame(in); /* the frame view moved on ... */
    EXPECT_FALSE(de_action_triggered(in, a));
    de__actions_fixed_tick(in, 0); /* ... the tick view still gets it */
    EXPECT_TRUE(tick_triggered(in, a));
    de__actions_fixed_tick(in, 1);
    EXPECT_FALSE(tick_triggered(in, a));

    /* A tap inside one frame between two ticks: triggered in the tick, not held. */
    space(in, false);
    frame(in);
    de__actions_fixed_tick(in, 2);
    space(in, true);
    space(in, false);
    frame(in);
    frame(in);
    de__actions_fixed_tick(in, 3);
    EXPECT_TRUE(tick_triggered(in, a));
    de__action_state s;
    de__action_tick_state_get(in, a, &s);
    EXPECT_FALSE(s.held);

    de_input_destroy(in);
}

typedef struct override_ctx {
    de_action a;
    u32 calls;
    u64 tick;
} override_ctx;

static void override_fn(de_input *in, u64 tick, void *user) {
    override_ctx *o = (override_ctx *)user;
    o->calls++;
    o->tick = tick;
    de__action_state s;
    memset(&s, 0, sizeof s);
    s.triggered = true;
    s.held = true;
    s.value.x = 0.25f;
    de__action_tick_state_set(in, o->a, &s);
}

TEST(action_interactions, tick_override) {
    de_input *in = input_test_create(NULL);
    ASSERT_TRUE(in != NULL);
    const de_action a = button(in, "jump", DE_INTERACT_PRESS, 0.0f);
    override_ctx o = {a, 0, 0};
    de__action_tick_override(in, override_fn, &o);
    frame(in);
    de__actions_fixed_tick(in, 42);
    EXPECT_EQ(o.calls, 1u);
    EXPECT_EQ(o.tick, 42u);
    de__action_state s;
    ASSERT_TRUE(de__action_tick_state_get(in, a, &s));
    EXPECT_TRUE(s.triggered);
    EXPECT_FLOAT_EQ(s.value.x, 0.25, 1e-6);
    /* Removed: the tick view is the latched frame view again. */
    de__action_tick_override(in, NULL, NULL);
    de__actions_fixed_tick(in, 43);
    EXPECT_EQ(o.calls, 1u);
    ASSERT_TRUE(de__action_tick_state_get(in, a, &s));
    EXPECT_FALSE(s.triggered);
    de_input_destroy(in);
}

/* ---- Through the ECS: a FixedUpdate system reads the tick view ------------------------------- */

typedef struct fixed_probe {
    de_action a;
    u32 runs;
    u32 triggered;
} fixed_probe;

static void probe_system(de_system_ctx *ctx) {
    fixed_probe *p = (fixed_probe *)ctx->user;
    const de_type_id t = de_type_find(de_world_types(ctx->world), "de_input_resource");
    const de_input_resource *r =
        (const de_input_resource *)de_resource_get(ctx->world, de_resource_find(ctx->world, t));
    p->runs++;
    if (r != NULL && de_action_triggered(r->input, p->a))
        p->triggered++;
}

TEST(action_interactions, fixed_update_reads_tick_view) {
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
    const de_action a = button(in, "jump", DE_INTERACT_PRESS, 0.0f);
    ASSERT_TRUE(DE_HANDLE_VALID(a));

    fixed_probe probe;
    memset(&probe, 0, sizeof probe);
    probe.a = a;
    de_resource_access acc;
    acc.id = in->resource;
    acc.access = DE_READ;
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "test.probe";
    sd.schedule = DE_SCHEDULE_FIXED_UPDATE;
    sd.fn = probe_system;
    sd.user = &probe;
    sd.resources = &acc;
    sd.resource_count = 1;
    ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(w, &sd)));

    /* Frame 1: press, then FixedUpdate 3 times: the probe sees the trigger once. */
    space(in, true);
    input_test_frame(in);
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_PRE_UPDATE, DT, 0.0f), DE_OK);
    EXPECT_TRUE(de_action_triggered(in, a)); /* outside FixedUpdate: the frame view */
    for (u32 i = 0; i < 3; ++i)
        ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_FIXED_UPDATE, DT, 0.0f), DE_OK);
    EXPECT_EQ(probe.runs, 3u);
    EXPECT_EQ(probe.triggered, 1u);
    EXPECT_TRUE(de_action_triggered(in, a)); /* the frame view is unchanged by the ticks */

    /* Frames 2 and 3: release, then press again, with no FixedUpdate. Frame 4: one tick, which
     * reports the press of frame 3. */
    space(in, false);
    input_test_frame(in);
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_PRE_UPDATE, DT, 0.0f), DE_OK);
    space(in, true);
    input_test_frame(in);
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_PRE_UPDATE, DT, 0.0f), DE_OK);
    input_test_frame(in);
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_PRE_UPDATE, DT, 0.0f), DE_OK);
    EXPECT_FALSE(de_action_triggered(in, a));
    ASSERT_EQ(de_world_run_schedule(w, DE_SCHEDULE_FIXED_UPDATE, DT, 0.0f), DE_OK);
    EXPECT_EQ(probe.triggered, 2u);
    EXPECT_FALSE(de__actions_in_fixed_update());

    de_input_detach(in);
    de_world_destroy(w);
    de_input_destroy(in);
    de_type_registry_destroy(types);
}
