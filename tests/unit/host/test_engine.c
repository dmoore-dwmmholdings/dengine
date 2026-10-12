/*
 * test_engine.c: de__engine_create / de__engine_destroy and de_host_run (DENG-90, SPEC §10).
 *
 * Every engine here is headless: CI has no display. The zeroed-desc case that opens a window is
 * in test_engine_window.c (label `window`). The de_host_run tests mount the fixtures in data/ as
 * game:/ and a build-tree directory as user:/ and cache:/ (de_fs_init before de_host_run, so the
 * host uses those mounts and never touches the real user profile), and capture the log.
 */
#include "de_test.h"

#include "host/host_internal.h"

#include <dengine/de_debug.h>

#include <stdio.h>
#include <string.h>

#ifndef DE_HOST_TEST_DATA_DIR
#error "DE_HOST_TEST_DATA_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif
#ifndef DE_HOST_TEST_TMP_DIR
#error "DE_HOST_TEST_TMP_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif

/* A headless config with one frame worker, so the tests start few threads. */
static de__game_config small_config(void) {
    de__game_config c;
    de__game_config_defaults(&c);
    c.task_workers = 1;
    return c;
}

/* ---- Engine create / destroy ------------------------------------------------------------ */

TEST(engine, create_destroy_no_leak) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_GENERAL);
    de__game_config c = small_config();
    de__engine_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &a;
    d.config = &c;
    d.headless = true;

    de_engine *e = NULL;
    ASSERT_EQ(de__engine_create(&d, &e), DE_OK);
    ASSERT_TRUE(e != NULL);
    EXPECT_TRUE(e->allocator == &a);
    EXPECT_TRUE(e->tasks != NULL);
    EXPECT_TRUE(e->types != NULL);
    EXPECT_TRUE(e->world != NULL);
    EXPECT_TRUE(de_world_types(e->world) == e->types);
    EXPECT_FALSE(DE_HANDLE_VALID(e->window)); /* headless: no window */
    EXPECT_TRUE(e->assets == NULL);
    EXPECT_TRUE(e->gpu == NULL);
    EXPECT_TRUE(e->renderer == NULL);
    EXPECT_TRUE(e->input == NULL);
    EXPECT_TRUE(e->audio == NULL);
    EXPECT_TRUE(e->physics == NULL);
    EXPECT_TRUE(e->ui == NULL);
    EXPECT_TRUE(e->game_state == NULL);
    EXPECT_EQ(de_task_worker_count(e->tasks), 2u); /* one worker + the main thread */
    EXPECT_EQ(de_task_worker_index(), 0u);         /* this thread is frame worker 0 */
    EXPECT_TRUE(de_platform_headless());

    de_tracking_stats s = de_tracking_allocator_stats(&t);
    EXPECT_GT(s.alloc_count, 0u); /* everything went through the engine's allocator */

    de__engine_destroy(e);
    s = de_tracking_allocator_stats(&t);
    EXPECT_EQ(s.blocks_live, 0u);
    EXPECT_EQ(s.bytes_live, 0u);
    EXPECT_EQ(de_task_worker_index(), DE_TASK_WORKER_NONE); /* the task system is gone */
    de__engine_destroy(NULL);                               /* ignored */
}

TEST(engine, create_failure_unwinds) {
    de__game_config c = small_config();
    bool created = false;
    i64 n = 0;
    for (; n < 100000 && !created; ++n) {
        de_tracking_allocator t;
        de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_GENERAL);
        de_tracking_allocator_fail_after(&t, n);
        de__engine_desc d;
        memset(&d, 0, sizeof d);
        d.allocator = &a;
        d.config = &c;
        d.headless = true;

        de_error_clear();
        de_engine sentinel;
        de_engine *e = &sentinel; /* must be reset to NULL on failure */
        de_result r = de__engine_create(&d, &e);
        if (r == DE_OK) {
            created = true;
            ASSERT_GT(n, 0); /* the engine allocates something */
            de__engine_destroy(e);
        } else {
            EXPECT_TRUE(e == NULL);
            ASSERT_TRUE(de_error_last() != NULL);
            EXPECT_GE(de_tracking_allocator_stats(&t).fail_count, 1u);
        }
        de_tracking_stats s = de_tracking_allocator_stats(&t);
        if (s.blocks_live != 0 || s.bytes_live != 0) {
            fprintf(stderr, "fail_after(%lld): %llu blocks / %llu bytes leaked (%s)\n",
                    (long long)n, (unsigned long long)s.blocks_live,
                    (unsigned long long)s.bytes_live, de_result_str(r));
        }
        ASSERT_EQ(s.blocks_live, 0u);
        ASSERT_EQ(s.bytes_live, 0u);
        ASSERT_EQ(de_task_worker_index(), DE_TASK_WORKER_NONE);
        ASSERT_TRUE(de_platform_headless()); /* the platform was shut down (or never started) */
    }
    ASSERT_TRUE(created);
    printf("engine create: failure injected at allocations 0..%lld, all unwound\n",
           (long long)(n - 2));
}

TEST(engine, zero_desc_headless_defaults) {
    de__engine_desc d;
    memset(&d, 0, sizeof d);
    d.headless = true; /* everything else zero: heap, default config */
    de_engine *e = NULL;
    ASSERT_EQ(de__engine_create(&d, &e), DE_OK);
    EXPECT_TRUE(e->allocator == de_heap_allocator());
    EXPECT_GE(de_task_worker_count(e->tasks), 2u); /* default: cores - 1 workers, at least 1 */
    de_timing t = de_engine_get_timing(e);
    EXPECT_EQ(t.mode, DE_TIMING_FIXED);
    EXPECT_EQ(t.fixed_hz, 60u);
    EXPECT_EQ(t.max_catchup_steps, 4u);
    EXPECT_FLOAT_EQ(t.time_scale, 1.0, 0.0);
    EXPECT_EQ(t.frame_cap_hz, 0u);
    EXPECT_FALSE(DE_HANDLE_VALID(e->window));
    de__engine_destroy(e);
}

TEST(engine, create_bad_arguments) {
    EXPECT_EQ(de__engine_create(NULL, NULL), DE_ERR_INVALID_ARGUMENT);
}

TEST(engine, timing_and_quit) {
    de__game_config c = small_config();
    c.timing.fixed_hz = 30;
    de__engine_desc d;
    memset(&d, 0, sizeof d);
    d.config = &c;
    d.headless = true;
    de_engine *e = NULL;
    ASSERT_EQ(de__engine_create(&d, &e), DE_OK);

    EXPECT_EQ(de_engine_get_timing(e).fixed_hz, 30u); /* from the config */
    de_timing t = {DE_TIMING_VARIABLE, 0, 2, -1.0f, 75};
    de_engine_set_timing(e, &t);
    de_timing got = de_engine_get_timing(e);
    EXPECT_EQ(got.mode, DE_TIMING_VARIABLE);
    EXPECT_EQ(got.fixed_hz, DE_TIMING_DEFAULT_FIXED_HZ); /* 0 means the default */
    EXPECT_EQ(got.max_catchup_steps, 2u);
    EXPECT_FLOAT_EQ(got.time_scale, 1.0, 0.0); /* negative means 1.0 */
    EXPECT_EQ(got.frame_cap_hz, 75u);
    de_engine_set_timing(e, NULL); /* ignored */
    de_engine_set_timing(NULL, &t);
    EXPECT_EQ(de_engine_get_timing(e).frame_cap_hz, 75u);
    EXPECT_EQ(de_engine_get_timing(NULL).fixed_hz, 0u);

    EXPECT_FALSE(de_engine_quit_requested(e));
    de_engine_request_quit(e);
    EXPECT_TRUE(de_engine_quit_requested(e));
    de_engine_request_quit(NULL);
    EXPECT_FALSE(de_engine_quit_requested(NULL));
    de__engine_destroy(e);
}

/* ---- de_host_run ------------------------------------------------------------------------ */

#define CAP_MAX 64
#define CAP_LEN 600

static struct {
    u32 count;
    char msgs[CAP_MAX][CAP_LEN];
} g_cap;

static void capture_sink(const de_log_record *rec, void *user) {
    (void)user;
    if (rec->level < DE_LOG_WARN || g_cap.count >= CAP_MAX)
        return;
    size_t n = rec->message.len < CAP_LEN - 1 ? rec->message.len : CAP_LEN - 1;
    memcpy(g_cap.msgs[g_cap.count], rec->message.ptr, n);
    g_cap.msgs[g_cap.count][n] = '\0';
    g_cap.count++;
}

static bool logged(const char *needle) {
    de_log_flush();
    for (u32 i = 0; i < g_cap.count; ++i)
        if (strstr(g_cap.msgs[i], needle) != NULL)
            return true;
    return false;
}

/* What the test game saw. */
static struct {
    int seq;
    int init_at, types_at, systems_at, shutdown_at;
    int init_n, types_n, systems_n, shutdown_n;
    de_result init_result;
    bool init_saw_modules, types_arg_ok, systems_arg_ok;
    int startup_runs, fixed_runs, update_runs, last_runs;
} g_game;

static void count_run(de_system_ctx *ctx) {
    (*(int *)ctx->user)++;
}

static de_result game_init(de_engine *e) {
    g_game.init_at = ++g_game.seq;
    g_game.init_n++;
    g_game.init_saw_modules = e != NULL && e->world != NULL && e->types != NULL &&
                              e->tasks != NULL && e->allocator != NULL;
    if (g_game.init_result != DE_OK)
        return DE_FAIL(g_game.init_result, "test game: init refuses (disk on fire)");
    return DE_OK;
}

static void game_register_types(de_engine *e, de_type_registry *types) {
    g_game.types_at = ++g_game.seq;
    g_game.types_n++;
    g_game.types_arg_ok = types != NULL && types == e->types;
}

static void game_register_systems(de_engine *e, de_world *w) {
    g_game.systems_at = ++g_game.seq;
    g_game.systems_n++;
    g_game.systems_arg_ok = w != NULL && w == e->world;
    static const struct {
        const char *name;
        de_schedule schedule;
        int *counter;
    } systems[] = {
        {"test.startup", DE_SCHEDULE_STARTUP, &g_game.startup_runs},
        {"test.fixed", DE_SCHEDULE_FIXED_UPDATE, &g_game.fixed_runs},
        {"test.update", DE_SCHEDULE_UPDATE, &g_game.update_runs},
        {"test.last", DE_SCHEDULE_LAST, &g_game.last_runs},
    };
    for (size_t i = 0; i < sizeof systems / sizeof systems[0]; ++i) {
        de_system_desc sd;
        memset(&sd, 0, sizeof sd);
        sd.name = systems[i].name;
        sd.schedule = systems[i].schedule;
        sd.fn = count_run;
        sd.user = systems[i].counter;
        (void)de_system_add(w, &sd);
    }
}

static void game_shutdown(de_engine *e) {
    (void)e;
    g_game.shutdown_at = ++g_game.seq;
    g_game.shutdown_n++;
}

static de_game_api test_api(void) {
    de_game_api api;
    memset(&api, 0, sizeof api);
    api.api_version = DE_GAME_API_VERSION;
    api.name = "test_game";
    api.init = game_init;
    api.shutdown = game_shutdown;
    api.register_types = game_register_types;
    api.register_systems = game_register_systems;
    return api;
}

typedef struct host_fx {
    bool fs;
} host_fx;

static void host_fx_setup(host_fx *f) {
    memset(&g_cap, 0, sizeof g_cap);
    memset(&g_game, 0, sizeof g_game);
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_log_add_sink(capture_sink, NULL);
    de_fs_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.game_dir = DE_HOST_TEST_DATA_DIR;
    fd.user_dir = DE_HOST_TEST_TMP_DIR "/user";
    fd.cache_dir = DE_HOST_TEST_TMP_DIR "/cache";
    ASSERT_EQ(de_fs_init(&fd), DE_OK);
    f->fs = true;
}

static void host_fx_teardown(host_fx *f) {
    if (f->fs)
        de_fs_shutdown();
    de_log_shutdown();
}

static de_host_desc host_desc(const de_game_api *api, const char *game_toml) {
    de_host_desc d;
    memset(&d, 0, sizeof d);
    d.static_api = api;
    d.headless = true;
    d.game_toml = game_toml;
    return d;
}

TEST_F(engine, host_run_call_order, host_fx) {
    de_game_api api = test_api();
    de_host_desc d = host_desc(&api, "game:/host_run.toml");
    EXPECT_EQ(de_host_run(&d), 0);

    EXPECT_EQ(g_game.init_n, 1);
    EXPECT_EQ(g_game.types_n, 1);
    EXPECT_EQ(g_game.systems_n, 1);
    EXPECT_EQ(g_game.shutdown_n, 1);
    EXPECT_EQ(g_game.init_at, 1);
    EXPECT_EQ(g_game.types_at, 2);
    EXPECT_EQ(g_game.systems_at, 3);
    EXPECT_EQ(g_game.shutdown_at, 4);
    EXPECT_TRUE(g_game.init_saw_modules);
    EXPECT_TRUE(g_game.types_arg_ok);
    EXPECT_TRUE(g_game.systems_arg_ok);
    /* The loop stub: Startup once, then one frame of every schedule. */
    EXPECT_EQ(g_game.startup_runs, 1);
    EXPECT_EQ(g_game.fixed_runs, 1);
    EXPECT_EQ(g_game.update_runs, 1);
    EXPECT_EQ(g_game.last_runs, 1);
    EXPECT_EQ(de_task_worker_index(), DE_TASK_WORKER_NONE); /* engine destroyed */
    EXPECT_FALSE(logged("de_host_run failed"));
}

TEST_F(engine, host_run_init_failure, host_fx) {
    de_game_api api = test_api();
    g_game.init_result = DE_ERR_IO;
    de_host_desc d = host_desc(&api, "game:/host_run.toml");
    EXPECT_EQ(de_host_run(&d), 1);

    EXPECT_EQ(g_game.init_n, 1);
    EXPECT_EQ(g_game.types_n, 0);
    EXPECT_EQ(g_game.systems_n, 0);
    EXPECT_EQ(g_game.shutdown_n, 0); /* init failed: no shutdown */
    EXPECT_EQ(g_game.startup_runs, 0);
    EXPECT_EQ(de_task_worker_index(), DE_TASK_WORKER_NONE); /* engine destroyed */
    EXPECT_TRUE(logged("de_host_run failed"));
    EXPECT_TRUE(logged("init failed"));
    EXPECT_TRUE(logged("disk on fire")); /* the game's own message, from the cause chain */
}

TEST_F(engine, host_run_api_version_mismatch, host_fx) {
    de_game_api api = test_api();
    api.api_version = DE_GAME_API_VERSION + 1;
    EXPECT_EQ(de__game_api_check(&api), DE_ERR_VERSION_MISMATCH);
    EXPECT_EQ(de__game_api_check(NULL), DE_ERR_INVALID_ARGUMENT);

    de_host_desc d = host_desc(&api, "game:/host_run.toml");
    EXPECT_EQ(de_host_run(&d), 1);
    EXPECT_EQ(g_game.init_n, 0); /* rejected before init */
    EXPECT_EQ(g_game.shutdown_n, 0);
    EXPECT_EQ(de_task_worker_index(), DE_TASK_WORKER_NONE);
    EXPECT_TRUE(logged("DE_ERR_VERSION_MISMATCH"));
}

TEST_F(engine, host_run_no_game, host_fx) {
    de_host_desc d = host_desc(NULL, "game:/host_run.toml");
    EXPECT_EQ(de_host_run(&d), 1);
    d.game_library = "libnothing.dylib"; /* DENG-92 loads libraries */
    EXPECT_EQ(de_host_run(&d), 1);
    EXPECT_TRUE(logged("DE_ERR_UNSUPPORTED"));
}

TEST_F(engine, host_run_game_toml_arg, host_fx) {
    de_cvar from_toml =
        de_cvar_register(&(const de_cvar_desc){.name = "test.host.from_toml", .type = DE_CVAR_INT});
    de_cvar over =
        de_cvar_register(&(const de_cvar_desc){.name = "test.host.override", .type = DE_CVAR_INT});
    ASSERT_TRUE(DE_HANDLE_VALID(from_toml));
    ASSERT_TRUE(DE_HANDLE_VALID(over));

    de_game_api api = test_api();
    /* desc.game_toml names a file that does not exist; --game-toml wins. */
    de_host_desc d = host_desc(&api, "game:/does_not_exist.toml");
    char a0[] = "game", a1[] = "--game-toml", a2[] = "game:/host_run.toml";
    char a3[] = "+test.host.override", a4[] = "2", a5[] = "+test.host.from_args", a6[] = "9";
    char *argv[] = {a0, a1, a2, a3, a4, a5, a6};
    d.argc = 7;
    d.argv = argv;
    EXPECT_EQ(de_host_run(&d), 0);
    EXPECT_EQ(g_game.init_n, 1);
    EXPECT_EQ(de_cvar_int(from_toml), 5); /* game.toml [cvars] */
    EXPECT_EQ(de_cvar_int(over), 2);      /* the command line beats game.toml */
    /* Registered after the run: the command-line value was kept pending. */
    de_cvar from_args =
        de_cvar_register(&(const de_cvar_desc){.name = "test.host.from_args", .type = DE_CVAR_INT});
    EXPECT_EQ(de_cvar_int(from_args), 9);

    char b2[] = "--game-toml";
    char *bad[] = {a0, b2};
    d.argc = 2;
    d.argv = bad;
    EXPECT_EQ(de_host_run(&d), 1); /* --game-toml without a value */
    EXPECT_TRUE(logged("--game-toml needs a value"));
}

TEST_F(engine, host_run_missing_game_toml_uses_defaults, host_fx) {
    de_game_api api = test_api();
    de_host_desc d = host_desc(&api, "game:/does_not_exist.toml");
    EXPECT_EQ(de_host_run(&d), 0);
    EXPECT_EQ(g_game.init_n, 1);
    EXPECT_EQ(g_game.shutdown_n, 1);
}

TEST_F(engine, host_run_bad_game_toml, host_fx) {
    de_game_api api = test_api();
    de_host_desc d = host_desc(&api, "game:/bad_timing_mode.toml");
    EXPECT_EQ(de_host_run(&d), 1);
    EXPECT_EQ(g_game.init_n, 0);
    EXPECT_TRUE(logged("timing.mode"));
}
