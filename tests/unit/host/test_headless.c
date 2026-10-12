/*
 * test_headless.c: the host's headless mode (src/host/headless.c), the asset stubs
 * (src/host/asset_stubs.c) and examples/minimal_game.c run in-process (DENG-95).
 *
 * The executable links examples/minimal_game.c (its de_game_get_api() is the static game of the
 * minimal_game.* tests) and the de_asset_stubs target, the same way the example does.
 * Every run here is headless, through de_host_run, with game:/ = tests/unit/host/data and user:/
 * and cache:/ in the build tree.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* sigaction, raise under -std=c11 */
#endif

#include "de_test.h"

#include "host/host_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_math.h>

#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#endif

#ifndef DE_HOST_TEST_DATA_DIR
#error "DE_HOST_TEST_DATA_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif
#ifndef DE_HOST_TEST_TMP_DIR
#error "DE_HOST_TEST_TMP_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif

/* examples/minimal_game.c, compiled into this executable. */
const de_game_api *de_game_get_api(void);

/* ---- Running the host ------------------------------------------------------------------- */

static void fs_start(void) {
    de_fs_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.game_dir = DE_HOST_TEST_DATA_DIR;
    fd.user_dir = DE_HOST_TEST_TMP_DIR "/user";
    fd.cache_dir = DE_HOST_TEST_TMP_DIR "/cache";
    ASSERT_EQ(de_fs_init(&fd), DE_OK);
}

/* de_host_run(`api`) headless (or not), for `frames` frames (0: until the game quits). */
static int run_host(const de_game_api *api, bool headless, const char *frames) {
    de_host_desc d;
    memset(&d, 0, sizeof d);
    d.static_api = api;
    d.headless = headless;
    d.game_toml = "game:/host_run.toml"; /* one frame worker */
    char a0[] = "game", a1[] = "--frames";
    char a2[16];
    char *argv[3] = {a0, a1, a2};
    d.argv = argv;
    d.argc = 1;
    if (frames != NULL) {
        size_t n = strlen(frames);
        if (n >= sizeof a2)
            n = sizeof a2 - 1;
        memcpy(a2, frames, n);
        a2[n] = '\0';
        d.argc = 3;
    }
    return de_host_run(&d);
}

/* A game with no-op callbacks except those a test sets. */
static de_game_api plain_game(const char *name) {
    de_game_api api;
    memset(&api, 0, sizeof api);
    api.api_version = DE_GAME_API_VERSION;
    api.name = name;
    return api;
}

static void noop_shutdown(de_engine *e) {
    (void)e;
}

static void noop_types(de_engine *e, de_type_registry *types) {
    (void)e;
    (void)types;
}

/* ---- headless.no_window ----------------------------------------------------------------- */

static struct {
    bool ran;
    u32 window_id;
    const void *gpu, *renderer, *audio, *ui;
    bool engine_headless, platform_headless;
    u32 updates;
} g_seen;

static de_result seen_init(de_engine *e) {
    g_seen.ran = true;
    g_seen.window_id = e->window.id;
    g_seen.gpu = e->gpu;
    g_seen.renderer = e->renderer;
    g_seen.audio = e->audio;
    g_seen.ui = e->ui;
    g_seen.engine_headless = de__engine_from(e)->headless;
    g_seen.platform_headless = de_platform_headless();
    return DE_OK;
}

static void count_update(de_system_ctx *ctx) {
    (void)ctx;
    ++g_seen.updates;
}

static void seen_systems(de_engine *e, de_world *w) {
    (void)e;
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "test.count";
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = count_update;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    (void)de_system_add(w, &sd);
}

static de_game_api seen_game(void) {
    de_game_api api = plain_game("headless_test");
    api.init = seen_init;
    api.shutdown = noop_shutdown;
    api.register_types = noop_types;
    api.register_systems = seen_systems;
    return api;
}

TEST(headless, no_window) {
    fs_start();
    de_game_api api = seen_game();
    memset(&g_seen, 0, sizeof g_seen);
    EXPECT_EQ(run_host(&api, true, "3"), 0);
    EXPECT_TRUE(g_seen.ran);
    EXPECT_EQ(g_seen.window_id, 0u);
    EXPECT_TRUE(g_seen.gpu == NULL);
    EXPECT_TRUE(g_seen.renderer == NULL);
    EXPECT_TRUE(g_seen.audio == NULL);
    EXPECT_TRUE(g_seen.ui == NULL);
    EXPECT_TRUE(g_seen.engine_headless);
    EXPECT_TRUE(g_seen.platform_headless);
    EXPECT_EQ(g_seen.updates, 3u); /* the event pump and the loop still run */
    de_fs_shutdown();
}

#if defined(DE_HEADLESS)
TEST(headless, forced_by_build) {
    /* A DE_HEADLESS build is headless even when the caller does not ask for it. */
    fs_start();
    de_game_api api = seen_game();
    memset(&g_seen, 0, sizeof g_seen);
    EXPECT_EQ(run_host(&api, false, "2"), 0);
    EXPECT_TRUE(g_seen.ran);
    EXPECT_EQ(g_seen.window_id, 0u);
    EXPECT_TRUE(g_seen.engine_headless);
    EXPECT_EQ(g_seen.updates, 2u);
    de_fs_shutdown();
}
#endif

TEST(headless, check_engine) {
    de_engine e;
    memset(&e, 0, sizeof e);
    EXPECT_EQ(de__headless_check(&e), DE_OK);
    EXPECT_EQ(de__headless_check(NULL), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();

    e.window.id = 1;
    EXPECT_EQ(de__headless_check(&e), DE_ERR_INTERNAL);
    e.window.id = 0;
    int dummy = 0;
    e.gpu = (de_gpu_device *)(void *)&dummy;
    EXPECT_EQ(de__headless_check(&e), DE_ERR_INTERNAL);
    EXPECT_TRUE(strstr(de_error_last()->message, "GPU") != NULL);
    e.gpu = NULL;
    e.renderer = (de_renderer *)(void *)&dummy;
    EXPECT_EQ(de__headless_check(&e), DE_ERR_INTERNAL);
    e.renderer = NULL;
    e.audio = (de_audio *)(void *)&dummy;
    EXPECT_EQ(de__headless_check(&e), DE_ERR_INTERNAL);
    e.audio = NULL;
    e.ui = (de_ui *)(void *)&dummy;
    EXPECT_EQ(de__headless_check(&e), DE_ERR_INTERNAL);
    de_error_clear();
}

/* ---- headless.sigterm_quit / headless.console_ctrl_quit --------------------------------- */

/* The Update system asks to stop (a signal or a console event) in frame STOP_FRAME (0-based):
 * that frame completes, and no other frame runs. */
#define STOP_FRAME   3u
#define STOP_LIMIT   "50"
#define STOP_PENDING 0xffffffffu

static struct {
    u32 updates;
    u32 how; /* the signal number or console event to send; STOP_PENDING: none */
    bool shutdown_ran;
} g_stop;

static void stop_update(de_system_ctx *ctx) {
    (void)ctx;
    const u32 frame = g_stop.updates++;
    if (frame != STOP_FRAME || g_stop.how == STOP_PENDING)
        return;
#if defined(_WIN32)
    EXPECT_TRUE(de__headless_on_console_ctrl(g_stop.how));
#else
    (void)raise((int)g_stop.how); /* the handler runs before raise returns */
#endif
}

static de_result stop_init(de_engine *e) {
    (void)e;
    return DE_OK;
}

static void stop_shutdown(de_engine *e) {
    (void)e;
    g_stop.shutdown_ran = true;
}

static void stop_systems(de_engine *e, de_world *w) {
    (void)e;
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "test.stop";
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = stop_update;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    (void)de_system_add(w, &sd);
}

static de_game_api stop_game(void) {
    de_game_api api = plain_game("headless_stop");
    api.init = stop_init;
    api.shutdown = stop_shutdown;
    api.register_types = noop_types;
    api.register_systems = stop_systems;
    return api;
}

/* Runs the stop game with `how` sent in frame STOP_FRAME; returns de_host_run's exit code. */
static int run_stop(u32 how) {
    memset(&g_stop, 0, sizeof g_stop);
    g_stop.how = how;
    de_game_api api = stop_game();
    return run_host(&api, true, STOP_LIMIT);
}

#if !defined(_WIN32)

static volatile sig_atomic_t g_test_handler_hits;

static void test_handler(int sig) {
    (void)sig;
    ++g_test_handler_hits;
}

TEST(headless, sigterm_quit) {
    fs_start();
    /* The test's own handlers: the host replaces them for the loop and puts them back. */
    struct sigaction mine, old_term, old_int, now;
    memset(&mine, 0, sizeof mine);
    mine.sa_handler = test_handler;
    sigemptyset(&mine.sa_mask);
    ASSERT_EQ(sigaction(SIGTERM, &mine, &old_term), 0);
    ASSERT_EQ(sigaction(SIGINT, &mine, &old_int), 0);
    g_test_handler_hits = 0;

    /* SIGTERM in frame 3: frame 3 completes, the game shuts down, exit 0. */
    EXPECT_EQ(run_stop((u32)SIGTERM), 0);
    EXPECT_EQ(g_stop.updates, STOP_FRAME + 1u);
    EXPECT_TRUE(g_stop.shutdown_ran);

    /* SIGINT the same way. */
    EXPECT_EQ(run_stop((u32)SIGINT), 0);
    EXPECT_EQ(g_stop.updates, STOP_FRAME + 1u);
    EXPECT_TRUE(g_stop.shutdown_ran);

    /* No signal: the frame limit ends the run. */
    EXPECT_EQ(run_stop(STOP_PENDING), 0);
    EXPECT_EQ(g_stop.updates, 50u);

    /* The host's handler took both signals; ours are back in place. */
    EXPECT_EQ((int)g_test_handler_hits, 0);
    ASSERT_EQ(sigaction(SIGTERM, NULL, &now), 0);
    EXPECT_TRUE(now.sa_handler == test_handler);
    ASSERT_EQ(sigaction(SIGINT, NULL, &now), 0);
    EXPECT_TRUE(now.sa_handler == test_handler);
    (void)raise(SIGTERM);
    EXPECT_EQ((int)g_test_handler_hits, 1);

    (void)sigaction(SIGTERM, &old_term, NULL);
    (void)sigaction(SIGINT, &old_int, NULL);
    de_fs_shutdown();
}

#else /* _WIN32 */

TEST(headless, console_ctrl_quit) {
    /* A real console event would reach every process on the console (CTest too), so the test
     * calls the handler's body, as the console's handler thread would. */
    fs_start();
    EXPECT_EQ(run_stop(CTRL_C_EVENT), 0);
    EXPECT_EQ(g_stop.updates, STOP_FRAME + 1u);
    EXPECT_TRUE(g_stop.shutdown_ran);

    EXPECT_EQ(run_stop(CTRL_BREAK_EVENT), 0);
    EXPECT_EQ(g_stop.updates, STOP_FRAME + 1u);

    EXPECT_EQ(run_stop(STOP_PENDING), 0);
    EXPECT_EQ(g_stop.updates, 50u);

    /* Events the host does not handle go to the next handler. */
    EXPECT_FALSE(de__headless_on_console_ctrl(99u));
    de_fs_shutdown();
}

#endif

/* ---- headless.asset_stubs_ready --------------------------------------------------------- */

TEST(headless, asset_stubs_ready) {
    /* The stubs ignore the system pointer: the host has none (de_engine.assets is NULL). */
    de_asset_system *s = NULL;
    EXPECT_EQ(de_asset_manifest_load(s, "dev:/assets.toml"), DE_OK);
    u32 issues = 7;
    EXPECT_TRUE(de_asset_manifest_issues(s, &issues) == NULL);
    EXPECT_EQ(issues, 0u);

    de_load_handle h = de_asset_load_bundle(s, "level_01", DE_LOAD_PRIORITY_HIGH);
    EXPECT_TRUE(DE_HANDLE_VALID(h));
    de_load_info info;
    memset(&info, 0xab, sizeof info);
    EXPECT_EQ(de_asset_status(s, h, &info), DE_LOAD_READY);
    EXPECT_EQ(info.assets_total, 0u);
    EXPECT_EQ(info.assets_ready, 0u);
    EXPECT_EQ(info.assets_failed, 0u);
    EXPECT_EQ(info.bytes_total, 0u);
    EXPECT_EQ(info.bytes_done, 0u);
    EXPECT_EQ(info.first_failed.hash, 0u);
    EXPECT_EQ(info.first_error, DE_OK);
    EXPECT_STREQ(info.first_error_msg, "");
    EXPECT_FLOAT_EQ(info.progress, 1.0f, 0.0f);
    EXPECT_EQ(de_asset_status(s, h, NULL), DE_LOAD_READY);
    EXPECT_EQ(de_asset_wait(s, h, 0), DE_LOAD_READY);
    de_asset_release(s, h);

    de_asset_id id = {42};
    EXPECT_TRUE(DE_HANDLE_VALID(de_asset_load(s, id, DE_LOAD_DEFAULT)));
    EXPECT_TRUE(DE_HANDLE_VALID(de_asset_load_many(s, &id, 1, DE_LOAD_DEFAULT)));

    /* Everything else is invalid or empty. */
    de_load_handle none = {0};
    EXPECT_EQ(de_asset_status(s, none, &info), DE_LOAD_INVALID);
    EXPECT_FLOAT_EQ(info.progress, 0.0f, 0.0f);
    EXPECT_EQ(de_asset_status_of(s, id), DE_LOAD_INVALID);
    EXPECT_EQ(de_asset_status_list(s, h, NULL, 0), 0u);
    EXPECT_EQ(de_asset_id_from("player/model").hash, 0u);
    EXPECT_TRUE(de_asset_id_path(s, id) == NULL);
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_texture(s, id)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_mesh(s, id)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_model(s, id)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_material(s, id)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_sound(s, id)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_font(s, id)));
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_prefab(s, id)));
    de_blob b = de_asset_blob(s, id);
    EXPECT_FALSE(DE_HANDLE_VALID(b));
    u64 size = 9;
    EXPECT_TRUE(de_blob_data(s, b, &size) == NULL);
    EXPECT_EQ(size, 0u);
    de_asset_type_desc td;
    memset(&td, 0, sizeof td);
    td.name = "thing";
    EXPECT_FALSE(DE_HANDLE_VALID(de_asset_type_register(s, &td)));
    de_asset_stats st = de_asset_get_stats(s);
    EXPECT_EQ(st.assets_resident, 0u);

    de_asset_system *made = (de_asset_system *)(void *)&size;
    EXPECT_EQ(de_asset_system_create(NULL, &made), DE_ERR_UNSUPPORTED);
    EXPECT_TRUE(made == NULL);
    EXPECT_EQ(de_asset_pack_mount(s, "game:/a.dpak", 0), DE_ERR_UNSUPPORTED);
    de_error_clear();

    /* The type loader context (DENG-142): empty values and the fallbacks. */
    EXPECT_TRUE(de_asset_load_ctx_data(NULL, &size) == NULL);
    EXPECT_EQ(size, 0u);
    EXPECT_EQ(de_asset_load_ctx_id(NULL).hash, 0u);
    EXPECT_TRUE(de_asset_load_ctx_user(NULL) == NULL);
    EXPECT_FALSE(de_asset_load_ctx_settings_has(NULL, "k"));
    EXPECT_TRUE(de_asset_load_ctx_settings_bool(NULL, "k", true));
    EXPECT_EQ(de_asset_load_ctx_settings_int(NULL, "k", 5), 5);
    EXPECT_STREQ(de_asset_load_ctx_settings_string(NULL, "k", "fb"), "fb");
    const char *const *items = (const char *const *)(void *)&size;
    EXPECT_EQ(de_asset_load_ctx_settings_string_array(NULL, "k", &items), 0u);
    EXPECT_TRUE(items == NULL);
    EXPECT_EQ(de_asset_load_ctx_fail(NULL, DE_ERR_PARSE, "bad %d", 1), DE_ERR_PARSE);
    EXPECT_EQ(de_asset_load_ctx_add_dependency(NULL, id), DE_ERR_UNSUPPORTED);
    de_asset_load_ctx_set_object(NULL, NULL, 0);
    de_error_clear();

    /* The setters do nothing (and do not crash on the NULL system). */
    de_asset_cancel(s, h);
    de_asset_pack_unmount(s, "game:/a.dpak");
    de_asset_set_locale(s, "ja");
    de_asset_set_budgets(s, NULL);
    de_asset_flush_unused(s);
    de_asset_set_streaming_sources(s, NULL, 0);
    de_asset_system_update(s, NULL);
    de_asset_system_destroy(s);
}

/* ---- minimal_game.* : examples/minimal_game.c in-process -------------------------------- */
/*
 * The example's spin system (FixedUpdate, deterministic) sets DeSpin.speed_rad =
 * de_dsin(speed_rad + dt) for every DeSpin entity. The test wraps the game's table: after the
 * game registers its systems it spawns one DeSpin entity and adds a FixedUpdate system that
 * records each run's dt. Replaying de_dsin over the recorded dts must give the entity's final
 * value bit for bit: proof that spin_system ran exactly once per FixedUpdate run (once per fixed
 * step in fixed mode, once per frame in variable mode), in every build type.
 */

#define MG_MAX_STEPS 4096u

static struct {
    const de_game_api *game;
    bool variable;
    de_component c_spin;
    de_entity ent;
    u32 steps, frames;
    f32 dt[MG_MAX_STEPS];
    f32 final_spin;
    bool have_final;
} g_mg;

static void mg_record_step(de_system_ctx *ctx) {
    if (g_mg.steps < MG_MAX_STEPS)
        g_mg.dt[g_mg.steps] = ctx->dt;
    ++g_mg.steps;
}

static void mg_count_frame(de_system_ctx *ctx) {
    (void)ctx;
    ++g_mg.frames;
}

static de_result mg_init(de_engine *e) {
    de_result r = g_mg.game->init(e);
    if (r == DE_OK) {
        de_timing t = de_engine_get_timing(e);
        if (g_mg.variable) {
            t.mode = DE_TIMING_VARIABLE;
        } else {
            t.mode = DE_TIMING_FIXED;
            t.fixed_hz = 60;
            t.frame_cap_hz = 240; /* about 4 frames per step, so several steps run */
        }
        de_engine_set_timing(e, &t);
    }
    return r;
}

static void mg_types(de_engine *e, de_type_registry *types) {
    g_mg.game->register_types(e, types);
}

static void mg_systems(de_engine *e, de_world *w) {
    g_mg.game->register_systems(e, w);
    g_mg.c_spin = de_component_find(w, "DeSpin");
    ASSERT_TRUE(DE_HANDLE_VALID(g_mg.c_spin));
    g_mg.ent = de_entity_spawn(w);
    f32 *spin = (f32 *)de_entity_add(w, g_mg.ent, g_mg.c_spin);
    ASSERT_TRUE(spin != NULL);
    *spin = 0.0f;

    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "test.record_step";
    sd.schedule = DE_SCHEDULE_FIXED_UPDATE;
    sd.fn = mg_record_step;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    (void)de_system_add(w, &sd);
    sd.name = "test.count_frame";
    sd.schedule = DE_SCHEDULE_LAST;
    sd.fn = mg_count_frame;
    (void)de_system_add(w, &sd);
}

static void mg_shutdown(de_engine *e) {
    const f32 *spin = (const f32 *)de_entity_get_const(e->world, g_mg.ent, g_mg.c_spin);
    g_mg.have_final = spin != NULL;
    if (spin != NULL)
        g_mg.final_spin = *spin;
    g_mg.game->shutdown(e);
}

/* Runs minimal_game for `frames` frames and checks the spin replay. */
static void mg_run(bool variable, const char *frames, u32 expect_frames) {
    fs_start();
    memset(&g_mg, 0, sizeof g_mg);
    g_mg.game = de_game_get_api();
    g_mg.variable = variable;
    ASSERT_TRUE(g_mg.game != NULL);
    EXPECT_STREQ(g_mg.game->name, "minimal");

    de_game_api api = *g_mg.game;
    api.init = mg_init;
    api.register_types = mg_types;
    api.register_systems = mg_systems;
    api.shutdown = mg_shutdown;
    EXPECT_EQ(run_host(&api, true, frames), 0);
    de_fs_shutdown();

    EXPECT_EQ(g_mg.frames, expect_frames);
    ASSERT_TRUE(g_mg.have_final);
    ASSERT_GE(g_mg.steps, 1u);
    ASSERT_LE(g_mg.steps, MG_MAX_STEPS);
    if (variable)
        EXPECT_EQ(g_mg.steps, expect_frames); /* one FixedUpdate per frame */

    const f32 fixed_dt = 1.0f / 60.0f;
    f32 x = 0.0f, x_short = 0.0f;
    for (u32 i = 0; i < g_mg.steps; ++i) {
        if (!variable)
            EXPECT_MEMEQ(&g_mg.dt[i], &fixed_dt, sizeof fixed_dt); /* the constant fixed dt */
        x = de_dsin(x + g_mg.dt[i]);
        if (i + 1 < g_mg.steps)
            x_short = x;
    }
    EXPECT_MEMEQ(&g_mg.final_spin, &x, sizeof x);
    /* One run fewer gives another value, so the comparison does count the runs. */
    if (g_mg.steps > 1)
        EXPECT_TRUE(memcmp(&g_mg.final_spin, &x_short, sizeof x) != 0);
}

TEST(minimal_game, spin_fixed) {
    mg_run(false, "40", 40u);
}

TEST(minimal_game, spin_variable) {
    mg_run(true, "50", 50u);
}
