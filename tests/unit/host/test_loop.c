/*
 * test_loop.c: the main loop (src/host/loop.c, DENG-91, SPEC §10.2).
 *
 * Every engine here is headless. The loop runs on a fake clock (de__host.loop_clock) that a
 * frame-end hook moves on, so the number of FixedUpdate steps per frame is exact; frame pacing
 * still uses the real clock, and is uncapped except in the frame-cap test.
 *
 * Built twice: `loop` (the default) and, with DE_LOOP_TIMING_TESTS, `loop_timing`, which holds
 * the wall-clock frame-cap test only (label `timing`, run serially; see "Decision: Timing tests
 * run serially and loosen their bounds on CI").
 */
#define _CRT_SECURE_NO_WARNINGS /* getenv */
#include "de_test.h"

#include "host/host_internal.h"

#include <dengine/de_debug.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DE_HOST_TEST_DATA_DIR
#error "DE_HOST_TEST_DATA_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif
#ifndef DE_HOST_TEST_TMP_DIR
#error "DE_HOST_TEST_TMP_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif

#define MS 1000000ull

/* A headless config with one frame worker. */
static de__game_config small_config(void) {
    de__game_config c;
    de__game_config_defaults(&c);
    c.task_workers = 1;
    return c;
}

/* Creates a headless engine into a zeroed host. */
static void host_start(de__host *h, const de__game_config *c, const de_allocator *a) {
    memset(h, 0, sizeof *h);
    de__engine_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = a;
    d.config = c;
    d.headless = true;
    ASSERT_EQ(de__engine_create(&d, &h->engine), DE_OK);
    h->log = de_log_register_category("host", DE_LOG_INFO);
}

static void host_stop(de__host *h) {
    de__engine_destroy(h->engine);
    h->engine = NULL;
}

#ifndef DE_LOOP_TIMING_TESTS

#include "de_alloc_guard.h"

#include "platform/de_atomic.h"

/* ---- Recording -------------------------------------------------------------------------- */

/* What ran, in order. Schedules record their de_schedule value; hooks record HOOK_* codes. */
enum {
    HOOK_BEGIN = 100, /* the frame-begin hook */
    HOOK_END_A = 200, /* frame-end hooks A, B, C */
    HOOK_END_B,
    HOOK_END_C,
};

typedef struct rec_event {
    int what;
    f32 dt, alpha;
    u64 tick;
} rec_event;

#define REC_MAX 4096
static struct {
    rec_event ev[REC_MAX];
    u32 n;
    u64 clock_ns;      /* the fake clock */
    const u64 *deltas; /* frame N's end hook moves the clock on by deltas[N] */
    u32 delta_count;
    u64 frame; /* frames whose end hook ran */
    de_engine *engine;
    int quit_on_update; /* request quit in the Update run with this number (1-based) */
    int update_runs;
    u64 set_variable_at; /* frame-end hook of this frame switches to variable mode (or ~0) */
    u64 set_fixed_at;    /* ... back to fixed (or ~0) */
} g_rec;

static void rec(int what, f32 dt, f32 alpha, u64 tick) {
    if (g_rec.n < REC_MAX) {
        g_rec.ev[g_rec.n].what = what;
        g_rec.ev[g_rec.n].dt = dt;
        g_rec.ev[g_rec.n].alpha = alpha;
        g_rec.ev[g_rec.n].tick = tick;
    }
    g_rec.n++;
}

static void rec_reset(void) {
    memset(&g_rec, 0, sizeof g_rec);
    g_rec.set_variable_at = ~0ull;
    g_rec.set_fixed_at = ~0ull;
}

static u64 fake_clock(void *user) {
    (void)user;
    return g_rec.clock_ns;
}

static void sys_record(de_system_ctx *ctx) {
    const int s = (int)(intptr_t)ctx->user;
    rec(s, ctx->dt, ctx->alpha, ctx->tick);
    if (s == DE_SCHEDULE_UPDATE) {
        ++g_rec.update_runs;
        if (g_rec.quit_on_update != 0 && g_rec.update_runs == g_rec.quit_on_update)
            de_engine_request_quit(g_rec.engine);
    }
}

/* One recording system in every schedule, Startup included. */
static void add_recorders(de_world *w) {
    for (int s = DE_SCHEDULE_STARTUP; s < DE_SCHEDULE_COUNT_; ++s) {
        de_system_desc sd;
        memset(&sd, 0, sizeof sd);
        sd.schedule = (de_schedule)s;
        sd.fn = sys_record;
        sd.user = (void *)(intptr_t)s;
        ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(w, &sd)));
    }
}

/* Frame-end hook: records, applies the scripted timing switches, moves the fake clock on. */
static de_result hook_clock(de__host *h, void *user) {
    (void)user;
    const u64 f = g_rec.frame++;
    rec(HOOK_END_A, 0.0f, 0.0f, 0);
    if (f == g_rec.set_variable_at || f == g_rec.set_fixed_at) {
        de_timing t = de_engine_get_timing(h->engine);
        t.mode = f == g_rec.set_variable_at ? DE_TIMING_VARIABLE : DE_TIMING_FIXED;
        de_engine_set_timing(h->engine, &t);
    }
    if (f < g_rec.delta_count)
        g_rec.clock_ns += g_rec.deltas[f];
    return DE_OK;
}

/* Sets up a host with the recorders, the fake clock and the clock hook. */
static void loop_host(de__host *h, const de__game_config *c) {
    rec_reset();
    g_rec.clock_ns = 1000 * MS;
    host_start(h, c, NULL);
    g_rec.engine = h->engine;
    add_recorders(h->engine->world);
    h->loop_clock = fake_clock;
    ASSERT_EQ(de__loop_add_frame_end_hook(h, hook_clock, NULL), DE_OK);
}

/* The schedules of frame `f` in g_rec, from its First to its end hook: returns how many
 * FixedUpdate runs it had and checks the order around them. `*pos` moves past the frame. */
static u32 expect_frame(u32 *pos, u32 extra_end_hooks) {
    u32 i = *pos;
    u32 fixed = 0;
    EXPECT_EQ(g_rec.ev[i++].what, (int)DE_SCHEDULE_FIRST);
    EXPECT_EQ(g_rec.ev[i++].what, (int)DE_SCHEDULE_PRE_UPDATE);
    while (i < g_rec.n && g_rec.ev[i].what == (int)DE_SCHEDULE_FIXED_UPDATE) {
        ++fixed;
        ++i;
    }
    EXPECT_EQ(g_rec.ev[i++].what, (int)DE_SCHEDULE_UPDATE);
    EXPECT_EQ(g_rec.ev[i++].what, (int)DE_SCHEDULE_POST_UPDATE);
    EXPECT_EQ(g_rec.ev[i++].what, (int)DE_SCHEDULE_EXTRACT);
    EXPECT_EQ(g_rec.ev[i++].what, (int)DE_SCHEDULE_LAST);
    EXPECT_EQ(g_rec.ev[i++].what, HOOK_END_A);
    i += extra_end_hooks;
    *pos = i;
    return fixed;
}

/* Index of the n-th (0-based) event `what` in g_rec, or ~0u. */
static u32 find_nth(int what, u32 n) {
    for (u32 i = 0; i < g_rec.n && i < REC_MAX; ++i)
        if (g_rec.ev[i].what == what && n-- == 0)
            return i;
    return ~0u;
}

/* ---- Tests ------------------------------------------------------------------------------ */

TEST(loop, schedule_order) {
    /* Fixed 60 Hz on the fake clock. Frame 0 counts as one step; then 1 step, 2.5 steps, 0. */
    static const u64 deltas[] = {16666667, 41666667, 0, 0};
    de__game_config c = small_config();
    de__host h;
    loop_host(&h, &c);
    g_rec.deltas = deltas;
    g_rec.delta_count = 4;
    h.loop_max_frames = 4;

    ASSERT_EQ(de__loop_run(&h), DE_OK);
    EXPECT_EQ(h.loop_frames, 4u);
    ASSERT_LE(g_rec.n, (u32)REC_MAX);

    u32 pos = 0;
    EXPECT_EQ(g_rec.ev[pos++].what, (int)DE_SCHEDULE_STARTUP); /* once, before frame 0 */
    EXPECT_EQ(expect_frame(&pos, 0), 1u); /* frame 0: the first frame is one step long */
    EXPECT_EQ(expect_frame(&pos, 0), 1u); /* frame 1: 16666667 ns */
    EXPECT_EQ(expect_frame(&pos, 0), 2u); /* frame 2: 2.5 steps, 0.5 left */
    EXPECT_EQ(expect_frame(&pos, 0), 0u); /* frame 3: nothing new */
    EXPECT_EQ(pos, g_rec.n);
    EXPECT_EQ(find_nth(DE_SCHEDULE_STARTUP, 1), ~0u); /* Startup ran once */
    EXPECT_EQ(de_world_tick(h.engine->world), 4u);    /* one tick per FixedUpdate */

    /* dt and alpha: FixedUpdate gets 1/60 as f32 every step and alpha 0; the schedules after it
     * get the frame delta and the interpolation alpha; Startup gets 0, 0. */
    const rec_event *st = &g_rec.ev[find_nth(DE_SCHEDULE_STARTUP, 0)];
    EXPECT_TRUE(st->dt == 0.0f && st->alpha == 0.0f);
    for (u32 k = 0; k < 4; ++k) {
        const rec_event *fx = &g_rec.ev[find_nth(DE_SCHEDULE_FIXED_UPDATE, k)];
        EXPECT_TRUE(fx->dt == 1.0f / 60.0f);
        EXPECT_TRUE(fx->alpha == 0.0f);
        EXPECT_EQ(fx->tick, (u64)k); /* the world tick moves on after each FixedUpdate */
    }
    const rec_event *u2 = &g_rec.ev[find_nth(DE_SCHEDULE_UPDATE, 2)];
    EXPECT_FLOAT_EQ(u2->dt, 0.041666667, 1e-7);
    EXPECT_FLOAT_EQ(u2->alpha, 0.5, 1e-6);
    const rec_event *l2 = &g_rec.ev[find_nth(DE_SCHEDULE_LAST, 2)];
    EXPECT_FLOAT_EQ(l2->alpha, 0.5, 1e-6);
    const rec_event *f2 = &g_rec.ev[find_nth(DE_SCHEDULE_FIRST, 2)];
    EXPECT_FLOAT_EQ(f2->dt, 0.041666667, 1e-7);
    EXPECT_TRUE(f2->alpha == 0.0f); /* before FixedUpdate: no interpolation alpha */
    const rec_event *u3 = &g_rec.ev[find_nth(DE_SCHEDULE_UPDATE, 3)];
    EXPECT_TRUE(u3->dt == 0.0f);
    EXPECT_FLOAT_EQ(u3->alpha, 0.5, 1e-6);
    host_stop(&h);
}

TEST(loop, variable_mode_dt) {
    /* Variable mode at time scale 0.5: one FixedUpdate per frame, dt = the scaled delta. */
    static const u64 deltas[] = {10 * MS, 20 * MS, 30 * MS, 0};
    de__game_config c = small_config();
    c.timing.mode = DE_TIMING_VARIABLE;
    c.timing.time_scale = 0.5f;
    de__host h;
    loop_host(&h, &c);
    g_rec.deltas = deltas;
    g_rec.delta_count = 4;
    h.loop_max_frames = 4;
    ASSERT_EQ(de__loop_run(&h), DE_OK);

    u32 pos = 1;
    for (int f = 0; f < 4; ++f)
        EXPECT_EQ(expect_frame(&pos, 0), 1u);
    const f32 want[4] = {(f32)(16666667.0 * 0.5e-9), 0.005f, 0.010f, 0.015f};
    for (u32 k = 0; k < 4; ++k) {
        const rec_event *fx = &g_rec.ev[find_nth(DE_SCHEDULE_FIXED_UPDATE, k)];
        EXPECT_FLOAT_EQ(fx->dt, want[k], 1e-9);
        const rec_event *up = &g_rec.ev[find_nth(DE_SCHEDULE_UPDATE, k)];
        EXPECT_TRUE(up->dt == fx->dt);
        EXPECT_TRUE(up->alpha == 1.0f);
    }
    host_stop(&h);
}

TEST(loop, mode_switch_next_frame) {
    /* 10 ms frames (0.6 step at 60 Hz). Frame 1's end switches to variable, frame 2's end back
     * to fixed. */
    static const u64 deltas[] = {10 * MS, 10 * MS, 10 * MS, 10 * MS, 10 * MS};
    de__game_config c = small_config();
    de__host h;
    loop_host(&h, &c);
    g_rec.deltas = deltas;
    g_rec.delta_count = 5;
    g_rec.set_variable_at = 1;
    g_rec.set_fixed_at = 2;
    h.loop_max_frames = 5;
    ASSERT_EQ(de__loop_run(&h), DE_OK);

    u32 pos = 1;
    EXPECT_EQ(expect_frame(&pos, 0), 1u); /* frame 0: fixed, the first step */
    EXPECT_EQ(expect_frame(&pos, 0), 0u); /* frame 1: fixed, 0.6 step pending */
    EXPECT_EQ(expect_frame(&pos, 0), 1u); /* frame 2: variable, one step of 10 ms */
    /* frame 3: fixed again with an empty accumulator: 0.6 step, nothing runs (had the 0.6 of
     * frame 1 survived the switches, 1.2 steps would run one) */
    EXPECT_EQ(expect_frame(&pos, 0), 0u);
    EXPECT_EQ(expect_frame(&pos, 0), 1u); /* frame 4: 1.2 steps */
    const rec_event *fx = &g_rec.ev[find_nth(DE_SCHEDULE_FIXED_UPDATE, 1)];
    EXPECT_FLOAT_EQ(fx->dt, 0.010, 1e-9); /* frame 2's variable step */
    host_stop(&h);
}

TEST(loop, quit_request) {
    de__game_config c = small_config();
    de__host h;

    /* Asked in frame 2's Update: frame 2 completes (Last and the end hook run), then it stops. */
    loop_host(&h, &c);
    g_rec.quit_on_update = 3;
    ASSERT_EQ(de__loop_run(&h), DE_OK);
    EXPECT_EQ(h.loop_frames, 3u);
    EXPECT_NE(find_nth(DE_SCHEDULE_LAST, 2), ~0u);
    EXPECT_EQ(find_nth(DE_SCHEDULE_LAST, 3), ~0u);
    EXPECT_EQ(g_rec.frame, 3u); /* the end hook ran in every frame, the last included */
    host_stop(&h);

    /* Asked before the first frame (the game's init or a Startup system): no frame runs. */
    loop_host(&h, &c);
    de_engine_request_quit(h.engine);
    ASSERT_EQ(de__loop_run(&h), DE_OK);
    EXPECT_EQ(h.loop_frames, 0u);
    EXPECT_EQ(find_nth(DE_SCHEDULE_STARTUP, 0), 0u);
    EXPECT_EQ(find_nth(DE_SCHEDULE_FIRST, 0), ~0u);
    host_stop(&h);

    /* The frame limit, from de__host and from the command line (which wins). */
    loop_host(&h, &c);
    h.loop_max_frames = 3;
    ASSERT_EQ(de__loop_run(&h), DE_OK);
    EXPECT_EQ(h.loop_frames, 3u);
    char a0[] = "game", a1[] = "--frames", a2[] = "5", a3[] = "--frames=2", a4[] = "+x.y";
    char *argv1[] = {a0, a1, a2};
    h.desc.argc = 3;
    h.desc.argv = argv1;
    ASSERT_EQ(de__loop_run(&h), DE_OK);
    EXPECT_EQ(h.loop_frames, 5u);
    char *argv2[] = {a0, a1, a2, a4, a3}; /* the last one wins */
    h.desc.argc = 5;
    h.desc.argv = argv2;
    ASSERT_EQ(de__loop_run(&h), DE_OK);
    EXPECT_EQ(h.loop_frames, 2u);

    /* Bad values: nothing runs, not even Startup. */
    char b0[] = "0", b1[] = "x7", b2[] = "7x", b3[] = "-1", b4[] = "--frames=";
    char *bad[][3] = {{a0, a1, b0}, {a0, a1, b1}, {a0, a1, b2}, {a0, a1, b3}, {a0, b4, NULL}};
    const int bad_argc[] = {3, 3, 3, 3, 2};
    for (int i = 0; i < 5; ++i) {
        h.desc.argc = bad_argc[i];
        h.desc.argv = bad[i];
        const u32 before = g_rec.n;
        de_error_clear();
        EXPECT_EQ(de__loop_run(&h), DE_ERR_INVALID_ARGUMENT);
        EXPECT_EQ(g_rec.n, before);
        ASSERT_TRUE(de_error_last() != NULL);
        EXPECT_TRUE(strstr(de_error_last()->message, "--frames") != NULL);
    }
    char *missing[] = {a0, a1};
    h.desc.argc = 2;
    h.desc.argv = missing;
    EXPECT_EQ(de__loop_run(&h), DE_ERR_INVALID_ARGUMENT);
    host_stop(&h);

    EXPECT_EQ(de__loop_run(NULL), DE_ERR_INVALID_ARGUMENT);
    de__host empty;
    memset(&empty, 0, sizeof empty);
    EXPECT_EQ(de__loop_run(&empty), DE_ERR_INVALID_ARGUMENT);
}

/* ---- Frame arenas ----------------------------------------------------------------------- */

#define ARENA_FRAMES 8
static struct {
    de_arena *arena[ARENA_FRAMES]; /* de_frame_arena() at frame begin */
    size_t used_at_begin[ARENA_FRAMES];
    u8 *block[ARENA_FRAMES]; /* pushed by frame N's end hook */
    de_arena *arena_at_end[ARENA_FRAMES];
    bool marker_ok_next_frame[ARENA_FRAMES]; /* frame N's block intact in frame N + 1 */
    u64 frame;
} g_arena;

static de_result arena_begin_hook(de__host *h, void *user) {
    (void)h;
    (void)user;
    const u64 f = g_arena.frame;
    if (f >= ARENA_FRAMES)
        return DE_OK;
    de_arena *a = de_frame_arena();
    g_arena.arena[f] = a;
    g_arena.used_at_begin[f] = a != NULL ? de_arena_get_info(a).used : 0;
    if (f > 0 && g_arena.block[f - 1] != NULL) {
        bool ok = true;
        for (int i = 0; i < 64; ++i)
            ok = ok && g_arena.block[f - 1][i] == (u8)(f - 1 + 1);
        g_arena.marker_ok_next_frame[f - 1] = ok;
    }
    return DE_OK;
}

static de_result arena_end_hook(de__host *h, void *user) {
    (void)h;
    (void)user;
    const u64 f = g_arena.frame++;
    if (f >= ARENA_FRAMES)
        return DE_OK;
    de_arena *a = de_frame_arena();
    g_arena.arena_at_end[f] = a;
    u8 *p = a != NULL ? (u8 *)de_arena_push(a, 64, 16) : NULL;
    if (p != NULL)
        memset(p, (int)(f + 1), 64);
    g_arena.block[f] = p;
    return DE_OK;
}

TEST(loop, frame_arena_swap) {
    de__game_config c = small_config();
    de__host h;
    host_start(&h, &c, NULL);
    ASSERT_TRUE(de_frame_arena() == NULL); /* nobody created the frame arenas yet */
    memset(&g_arena, 0, sizeof g_arena);
    ASSERT_EQ(de__loop_add_frame_begin_hook(&h, arena_begin_hook, NULL), DE_OK);
    ASSERT_EQ(de__loop_add_frame_end_hook(&h, arena_end_hook, NULL), DE_OK);
    h.loop_max_frames = ARENA_FRAMES;
    ASSERT_EQ(de__loop_run(&h), DE_OK);
    EXPECT_TRUE(de_frame_arena() == NULL); /* the loop created them, so it destroyed them */

    for (u32 f = 0; f < ARENA_FRAMES; ++f) {
        ASSERT_TRUE(g_arena.arena[f] != NULL);
        ASSERT_TRUE(g_arena.block[f] != NULL);
        /* Reset at every frame start: frame N - 2's block (same arena) is gone. */
        EXPECT_EQ(g_arena.used_at_begin[f], g_arena.used_at_begin[0]);
        /* The end hooks run before the swap: still the frame's own arena. */
        EXPECT_TRUE(g_arena.arena_at_end[f] == g_arena.arena[f]);
        if (f >= 1) {
            EXPECT_TRUE(g_arena.arena[f] != g_arena.arena[f - 1]); /* swapped every frame */
            EXPECT_TRUE(g_arena.marker_ok_next_frame[f - 1]); /* N's data lives through N + 1 */
        }
        if (f >= 2) {
            EXPECT_TRUE(g_arena.arena[f] == g_arena.arena[f - 2]);
            EXPECT_TRUE(g_arena.block[f] == g_arena.block[f - 2]); /* the same memory again */
        }
    }

    /* Frame arenas the caller created are used and left alone. */
    ASSERT_EQ(de_frame_arenas_init(0), DE_OK);
    de_frame_arena_begin(0);
    memset(&g_arena, 0, sizeof g_arena);
    h.loop_max_frames = 3;
    ASSERT_EQ(de__loop_run(&h), DE_OK);
    EXPECT_TRUE(de_frame_arena() != NULL);
    de_frame_arenas_shutdown();
    host_stop(&h);
}

/* ---- Frame-end hooks -------------------------------------------------------------------- */

static de_result hook_rec(de__host *h, void *user) {
    (void)h;
    rec((int)(intptr_t)user, 0.0f, 0.0f, 0);
    return DE_OK;
}

static de_result hook_begin_rec(de__host *h, void *user) {
    (void)h;
    (void)user;
    rec(HOOK_BEGIN, 0.0f, 0.0f, 0);
    return DE_OK;
}

static de_result hook_fail_at_2(de__host *h, void *user) {
    (void)h;
    (void)user;
    rec(HOOK_END_B, 0.0f, 0.0f, 0);
    if (g_rec.frame == 3) /* hook_clock (first in the list) counted frame 2 already */
        return DE_FAIL(DE_ERR_IO, "test hook: the tape ran out");
    return DE_OK;
}

static de_result hook_quit(de__host *h, void *user) {
    (void)user;
    if (g_rec.frame == 2)
        de_engine_request_quit(h->engine);
    return DE_OK;
}

TEST(loop, frame_end_hooks) {
    de__game_config c = small_config();
    de__host h;
    loop_host(&h, &c); /* hook_clock records HOOK_END_A */
    ASSERT_EQ(de__loop_add_frame_end_hook(&h, hook_rec, (void *)(intptr_t)HOOK_END_B), DE_OK);
    ASSERT_EQ(de__loop_add_frame_end_hook(&h, hook_rec, (void *)(intptr_t)HOOK_END_C), DE_OK);
    ASSERT_EQ(de__loop_add_frame_begin_hook(&h, hook_begin_rec, NULL), DE_OK);
    h.loop_max_frames = 3;
    ASSERT_EQ(de__loop_run(&h), DE_OK);

    /* Per frame: begin hook, First .. Last, then A, B, C in registration order, once each. */
    u32 pos = 1;
    for (int f = 0; f < 3; ++f) {
        EXPECT_EQ(g_rec.ev[pos++].what, HOOK_BEGIN);
        expect_frame(&pos, 0);
        EXPECT_EQ(g_rec.ev[pos++].what, HOOK_END_B);
        EXPECT_EQ(g_rec.ev[pos++].what, HOOK_END_C);
    }
    EXPECT_EQ(pos, g_rec.n);
    host_stop(&h);

    /* A failing hook stops the loop at once with its error; the hooks after it do not run. */
    loop_host(&h, &c);
    ASSERT_EQ(de__loop_add_frame_end_hook(&h, hook_fail_at_2, NULL), DE_OK);
    ASSERT_EQ(de__loop_add_frame_end_hook(&h, hook_rec, (void *)(intptr_t)HOOK_END_C), DE_OK);
    de_error_clear();
    EXPECT_EQ(de__loop_run(&h), DE_ERR_IO);
    EXPECT_EQ(h.loop_frames, 2u); /* frames 0 and 1 completed */
    ASSERT_TRUE(de_error_last() != NULL);
    EXPECT_TRUE(strstr(de_error_last()->message, "frame-end hook 1") != NULL);
    ASSERT_TRUE(de_error_last()->cause != NULL);
    EXPECT_TRUE(strstr(de_error_last()->cause->message, "tape ran out") != NULL);
    EXPECT_EQ(find_nth(HOOK_END_C, 2), ~0u); /* C ran in frames 0 and 1 only */
    EXPECT_NE(find_nth(HOOK_END_C, 1), ~0u);
    EXPECT_TRUE(de_frame_arena() == NULL); /* cleaned up on the error path too */
    host_stop(&h);

    /* A hook may ask to quit: the loop stops after that frame. */
    loop_host(&h, &c);
    ASSERT_EQ(de__loop_add_frame_end_hook(&h, hook_quit, NULL), DE_OK);
    ASSERT_EQ(de__loop_run(&h), DE_OK);
    EXPECT_EQ(h.loop_frames, 2u);

    /* Arguments and capacity: DE__LOOP_MAX_HOOKS per list, no allocation. */
    EXPECT_EQ(de__loop_add_frame_end_hook(NULL, hook_quit, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__loop_add_frame_end_hook(&h, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__loop_add_frame_begin_hook(NULL, hook_quit, NULL), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__loop_add_frame_begin_hook(&h, NULL, NULL), DE_ERR_INVALID_ARGUMENT);
    while (h.loop_hooks.frame_end_count < DE__LOOP_MAX_HOOKS)
        ASSERT_EQ(de__loop_add_frame_end_hook(&h, hook_rec, NULL), DE_OK);
    EXPECT_EQ(de__loop_add_frame_end_hook(&h, hook_rec, NULL), DE_ERR_BUSY);
    while (h.loop_hooks.frame_begin_count < DE__LOOP_MAX_HOOKS)
        ASSERT_EQ(de__loop_add_frame_begin_hook(&h, hook_begin_rec, NULL), DE_OK);
    EXPECT_EQ(de__loop_add_frame_begin_hook(&h, hook_begin_rec, NULL), DE_ERR_BUSY);
    host_stop(&h);
}

/* ---- No heap allocation per frame -------------------------------------------------------- */

static struct {
    de_alloc_guard guard;
    u64 frame;
    u64 begin_at, end_at; /* the guard spans the end hooks of these frames */
    u64 tracking_allocs_at_begin, tracking_allocs_at_end;
    de_tracking_allocator *tracking;
    u64 rows;
} g_alloc;

/* A clock that moves on by one 60 Hz period per call: the loop reads it once per frame, so every
 * frame runs exactly one FixedUpdate. */
static u64 g_step_clock_ns;
static u64 step_clock(void *user) {
    (void)user;
    g_step_clock_ns += 16666667;
    return g_step_clock_ns;
}

static void count_rows(const de_query_chunk *chunk, void *user) {
    (void)user;
    g_alloc.rows += chunk->count;
}

static void sys_query(de_system_ctx *ctx) {
    de_query_each(ctx->world, ctx->queries[0], count_rows, NULL);
}

static de_result alloc_guard_hook(de__host *h, void *user) {
    (void)h;
    (void)user;
    const u64 f = g_alloc.frame++;
    if (f == g_alloc.end_at) {
        de_alloc_guard_end(&g_alloc.guard);
        g_alloc.tracking_allocs_at_end = de_tracking_allocator_stats(g_alloc.tracking).alloc_count;
    }
    if (f == g_alloc.begin_at) {
        g_alloc.tracking_allocs_at_begin =
            de_tracking_allocator_stats(g_alloc.tracking).alloc_count;
        de_alloc_guard_begin(&g_alloc.guard);
    }
    return DE_OK;
}

/* Barrier tasks: each one waits (up to 5 s) until `n` of them run at once, so `n` distinct
 * threads have run a task. */
typedef struct start_barrier {
    de_atomic_u32 arrived;
    u32 n;
} start_barrier;

static void barrier_task(void *user) {
    start_barrier *b = (start_barrier *)user;
    de_atomic_fetch_add_u32(&b->arrived, 1u, DE_ATOMIC_ACQ_REL);
    const u64 deadline = de_time_now_ns() + 5000u * MS;
    while (de_atomic_load_u32(&b->arrived, DE_ATOMIC_ACQUIRE) < b->n && de_time_now_ns() < deadline)
        de_cpu_pause();
}

static void barrier_run(de_task_system *ts, de_task_pool pool, u32 n) {
    static start_barrier b;
    de_atomic_init_u32(&b.arrived, 0);
    b.n = n;
    de_task_counter c;
    memset(&c, 0, sizeof c);
    for (u32 i = 0; i < n; ++i)
        de_task_submit(ts, pool, barrier_task, &b, &c);
    de_task_wait(ts, &c);
    EXPECT_GE(de_atomic_load_u32(&b.arrived, DE_ATOMIC_ACQUIRE), n);
}

/* Every task thread has started and run a task. A worker thread's start-up (its thread-local
 * blocks, which dyld allocates on macOS) is not per-frame work, but on a loaded host a thread can
 * start late, inside the guarded frames. */
static void start_all_task_threads(de_task_system *ts) {
    barrier_run(ts, DE_TASK_POOL_FRAME, de_task_worker_count(ts));
    const u32 cores = de_cpu_logical_cores();
    barrier_run(ts, DE_TASK_POOL_BACKGROUND, cores / 4u > 1u ? cores / 4u : 1u);
}

TEST(loop, no_heap_alloc_per_frame) {
    /* Everything the engine allocates goes through a tracking allocator (counted by the guard in
     * every build), and the guard counts the heap and raw malloc where the build allows it. The
     * guard spans 100 whole frames after 4 warm-up frames: pump, timing, every schedule with a
     * query system in it, transform propagation, hooks, the arena swap and pacing. */
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_GENERAL);
    de__game_config c = small_config();
    de__host h;
    host_start(&h, &c, &a);
    de_world *w = h.engine->world;

    const de_builtin_component_ids ids = de_builtin_components(w);
    de_entity root = de_entity_spawn(w);
    (void)de_entity_add(w, root, ids.transform);
    for (int i = 0; i < 64; ++i) {
        de_entity e = de_entity_spawn(w);
        (void)de_entity_add(w, e, ids.transform);
        ASSERT_EQ(de_entity_set_parent(w, e, root), DE_OK);
    }
    de_query_term term = {ids.transform, DE_READ};
    de_query_desc qd;
    memset(&qd, 0, sizeof qd);
    qd.terms = &term;
    qd.term_count = 1;
    for (int s = DE_SCHEDULE_FIRST; s < DE_SCHEDULE_COUNT_; ++s) {
        de_system_desc sd;
        memset(&sd, 0, sizeof sd);
        sd.schedule = (de_schedule)s;
        sd.fn = sys_query;
        sd.queries = &qd;
        sd.query_count = 1;
        ASSERT_TRUE(DE_HANDLE_VALID(de_system_add(w, &sd)));
    }

    start_all_task_threads(h.engine->tasks);

    memset(&g_alloc, 0, sizeof g_alloc);
    g_alloc.tracking = &t;
    g_alloc.begin_at = 3;
    g_alloc.end_at = 103;
    ASSERT_EQ(de__loop_add_frame_end_hook(&h, alloc_guard_hook, NULL), DE_OK);
    h.loop_max_frames = 105;
    h.loop_clock = step_clock;
    ASSERT_EQ(de__loop_run(&h), DE_OK);
    EXPECT_EQ(g_alloc.rows, 105u * 7u * 65u); /* every system saw every entity each frame */

    printf("guarded 100 frames: %llu engine allocs, %llu raw mallocs (raw counted: %s), "
           "tracking allocator %llu -> %llu\n",
           (unsigned long long)g_alloc.guard.engine_allocs,
           (unsigned long long)g_alloc.guard.raw_mallocs,
           de_alloc_guard_counts_raw() ? "yes" : "no",
           (unsigned long long)g_alloc.tracking_allocs_at_begin,
           (unsigned long long)g_alloc.tracking_allocs_at_end);
    DE_ALLOC_GUARD_ASSERT_ZERO(&g_alloc.guard);
    EXPECT_EQ(g_alloc.tracking_allocs_at_end, g_alloc.tracking_allocs_at_begin);
    host_stop(&h);
    EXPECT_EQ(de_tracking_allocator_stats(&t).blocks_live, 0u);
}

/* ---- Through de_host_run ------------------------------------------------------------------ */

static int g_host_updates;

static void count_update(de_system_ctx *ctx) {
    (void)ctx;
    ++g_host_updates;
}

static de_result host_game_init(de_engine *e) {
    (void)e;
    return DE_OK;
}

static void host_game_shutdown(de_engine *e) {
    (void)e;
}

static void host_game_types(de_engine *e, de_type_registry *types) {
    (void)e;
    (void)types;
}

static void host_game_systems(de_engine *e, de_world *w) {
    (void)e;
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = count_update;
    (void)de_system_add(w, &sd);
}

TEST(loop, host_run_frames_arg) {
    /* `--frames N` reaches the loop through de_host_desc.argv (the dengine exe forwards it). */
    de_fs_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.game_dir = DE_HOST_TEST_DATA_DIR;
    fd.user_dir = DE_HOST_TEST_TMP_DIR "/user";
    fd.cache_dir = DE_HOST_TEST_TMP_DIR "/cache";
    ASSERT_EQ(de_fs_init(&fd), DE_OK);

    de_game_api api;
    memset(&api, 0, sizeof api);
    api.api_version = DE_GAME_API_VERSION;
    api.name = "loop_test_game";
    api.init = host_game_init;
    api.shutdown = host_game_shutdown;
    api.register_types = host_game_types;
    api.register_systems = host_game_systems;
    de_host_desc d;
    memset(&d, 0, sizeof d);
    d.static_api = &api;
    d.headless = true;
    d.game_toml = "game:/host_run.toml";
    char a0[] = "game", a1[] = "--frames", a2[] = "5", b2[] = "five";
    char *argv[] = {a0, a1, a2};
    d.argc = 3;
    d.argv = argv;
    g_host_updates = 0;
    EXPECT_EQ(de_host_run(&d), 0);
    EXPECT_EQ(g_host_updates, 5);

    argv[2] = b2;
    g_host_updates = 0;
    EXPECT_EQ(de_host_run(&d), 1); /* a bad value fails the run before any frame */
    EXPECT_EQ(g_host_updates, 0);
    de_fs_shutdown();
}

#else /* DE_LOOP_TIMING_TESTS */

/* ---- Frame cap (wall clock) ------------------------------------------------------------- */

static bool env_flag(const char *name) {
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' && strcmp(v, "0") != 0 && strcmp(v, "false") != 0;
}

static bool on_ci(void) {
    return env_flag("DE_CI") || env_flag("CI");
}

#define CAP_FRAMES     60
/* At most this many stall frames are excused (see check_cap). More means the pacer is wrong: a
 * pacer at a third of the rate or less makes every frame a "stall". */
#define CAP_MAX_STALLS (CAP_FRAMES / 2)
static struct {
    u64 t[CAP_FRAMES + 1];
    u32 n;
} g_cap;

static de_result cap_begin_hook(de__host *h, void *user) {
    (void)h;
    (void)user;
    if (g_cap.n <= CAP_FRAMES)
        g_cap.t[g_cap.n++] = de_time_now_ns();
    return DE_OK;
}

/*
 * Checks the CAP_FRAMES frame-begin times in g_cap against a cap of `hz`.
 *
 * Bounds on the average frame time, period P = 1000 / hz ms:
 * - Host (CI unset), every build, ASan included: P ± 1 ms. This is DENG-91's acceptance
 *   criterion; the local ci-macos-asan build measures P ± 0.1 ms, also beside a parallel suite.
 * - CI (`CI` / `DE_CI` set), every build: P ± 2 ms ("Decision: Timing tests run serially and
 *   loosen their bounds on CI"), and (DENG-381) the upper bound excuses stall frames. A stall
 *   frame is longer than 2 P. The shared GitHub macOS runners stop the process for 100 ms and
 *   more, with or without a sanitizer: one stall frame of 137.3 ms gave an average of 35.456 ms
 *   (ASan+UBSan, dev @ 902b684) and 35.660 ms (Debug, DENG-382's branch); Release on DENG-381's
 *   branch had 16 stall frames of up to 141.6 ms and 36.836 ms. After a deadline missed by more
 *   than 2 P, de_frame_pacer restarts its schedule instead of running a burst of short frames
 *   (its documented resync), so a stall is never caught up. The upper bound therefore counts
 *   each stall frame as one period: the run, with every stall frame of d ms shortened to P,
 *   must still average P + 2 ms or less. The excuse is off when more than CAP_MAX_STALLS
 *   frames stall. The lower bound (a capped loop is never faster than the cap) gets no excuse.
 *   The pacer's own wake-up accuracy is checked by the platform `pacing.*` tests.
 */
static void check_cap(u32 hz) {
    const f64 period_ms = 1000.0 / hz;
    const f64 avg_ms = (f64)(g_cap.t[CAP_FRAMES] - g_cap.t[0]) / CAP_FRAMES * 1e-6;
    f64 min_ms = 1e9, max_ms = 0.0, stall_ms = 0.0;
    u32 stalls = 0;
    for (u32 i = 1; i <= CAP_FRAMES; ++i) {
        const f64 d = (f64)(g_cap.t[i] - g_cap.t[i - 1]) * 1e-6;
        min_ms = d < min_ms ? d : min_ms;
        max_ms = d > max_ms ? d : max_ms;
        if (d > 2.0 * period_ms) {
            ++stalls;
            stall_ms += d - period_ms;
        }
    }
    const bool ci = on_ci();
    const f64 tol_ms = ci ? 2.0 : 1.0;
    const f64 excuse_ms = ci && stalls <= CAP_MAX_STALLS ? stall_ms / CAP_FRAMES : 0.0;
    const f64 lo_ms = period_ms - tol_ms;
    const f64 hi_ms = period_ms + tol_ms + excuse_ms;
    printf("frame cap %u Hz: average %.3f ms over %d frames (min %.3f, max %.3f), %u stall "
           "frame(s) > %.1f ms; bounds [%.3f, %.3f] ms%s\n",
           hz, avg_ms, CAP_FRAMES, min_ms, max_ms, stalls, 2.0 * period_ms, lo_ms, hi_ms,
           ci ? " (CI)" : "");
    EXPECT_TRUE(avg_ms >= lo_ms);
    EXPECT_TRUE(avg_ms <= hi_ms);
}

TEST(loop_timing, frame_cap) {
    /* frame_cap_hz = 30 holds the average frame time at 33.3 ms ± 1 ms over 60 frames on the
     * host; check_cap has the CI bounds. The real clock throughout. */
    de__game_config c = small_config();
    c.timing.frame_cap_hz = 30;
    de__host h;
    host_start(&h, &c, NULL);
    memset(&g_cap, 0, sizeof g_cap);
    ASSERT_EQ(de__loop_add_frame_begin_hook(&h, cap_begin_hook, NULL), DE_OK);
    h.loop_max_frames = CAP_FRAMES + 1;
    ASSERT_EQ(de__loop_run(&h), DE_OK);
    ASSERT_EQ(g_cap.n, (u32)CAP_FRAMES + 1);
    check_cap(30);

    /* A cap changed at run time applies from the next frame: 60 Hz now. */
    de_timing t = de_engine_get_timing(h.engine);
    t.frame_cap_hz = 60;
    de_engine_set_timing(h.engine, &t);
    memset(&g_cap, 0, sizeof g_cap);
    ASSERT_EQ(de__loop_run(&h), DE_OK);
    ASSERT_EQ(g_cap.n, (u32)CAP_FRAMES + 1);
    check_cap(60);
    host_stop(&h);
}

#endif /* DE_LOOP_TIMING_TESTS */
