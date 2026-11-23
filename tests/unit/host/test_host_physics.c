/*
 * test_host_physics.c: the host's own physics world (DENG-835). de__engine_attach_physics gives
 * a game without physics a 3D physics world, so DeCollider exists for the editor inspector and
 * the nav bake; game.toml `[physics] attach = true` does it before init, and an editor session
 * (DE_EDITOR_TOKEN) after register_systems. A game's own physics world is left alone.
 *
 * Every engine is headless. The de_host_run cases mount data/ as game:/ (as test_engine.c).
 */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "de_test.h"

#include "host/host_internal.h"

#include <stdlib.h>
#include <string.h>

#ifndef DE_HAS_PHYSICS
#define DE_HAS_PHYSICS 0
#endif
#ifndef DE_HOST_TEST_DATA_DIR
#error "DE_HOST_TEST_DATA_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif
#ifndef DE_HOST_TEST_TMP_DIR
#error "DE_HOST_TEST_TMP_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif

static void set_token(const char *v) {
#if defined(_WIN32)
    (void)_putenv_s("DE_EDITOR_TOKEN", v != NULL ? v : "");
#else
    if (v != NULL)
        (void)setenv("DE_EDITOR_TOKEN", v, 1);
    else
        (void)unsetenv("DE_EDITOR_TOKEN");
#endif
}

static bool has_collider(de_engine *e) {
    return e != NULL && e->world != NULL &&
           DE_HANDLE_VALID(de_component_find(e->world, "DeCollider"));
}

TEST(host_physics, attach_creates_owns_and_frees) {
    de_tracking_allocator t;
    de_allocator a = de_tracking_allocator_init(&t, NULL, DE_MEM_GENERAL);
    de__game_config c;
    de__game_config_defaults(&c);
    c.task_workers = 1;
    de__engine_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = &a;
    d.config = &c;
    d.headless = true;
    de_engine *e = NULL;
    ASSERT_EQ(de__engine_create(&d, &e), DE_OK);
    EXPECT_TRUE(e->physics == NULL); /* nothing by default */
    EXPECT_FALSE(has_collider(e));

    ASSERT_EQ(de__engine_attach_physics(e), DE_OK);
    if (DE_HAS_PHYSICS) {
        ASSERT_TRUE(e->physics != NULL);
        EXPECT_TRUE(has_collider(e));
        EXPECT_TRUE(DE_HANDLE_VALID(de_component_find(e->world, "DeRigidBody")));
        de_physics_world *first = e->physics;
        ASSERT_EQ(de__engine_attach_physics(e), DE_OK); /* again: the same world */
        EXPECT_TRUE(e->physics == first);
    } else {
        EXPECT_TRUE(e->physics == NULL);
    }
    de__engine_destroy(e);
    EXPECT_EQ(de_tracking_allocator_stats(&t).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&t).bytes_live, 0u);
    EXPECT_EQ(de__engine_attach_physics(NULL), DE_ERR_INVALID_ARGUMENT);
    de_error_clear();
}

/* ---- de_host_run ------------------------------------------------------------------------- */

static struct {
    de_engine *engine;
    bool init_physics, init_collider;       /* seen in init */
    bool shutdown_physics, shutdown_collider; /* seen in shutdown */
    de_physics_world *shutdown_world;
    bool own; /* init sets e->physics to a world of the game's own (a stand-in pointer) */
} g;

static int g_own_world; /* the stand-in: never dereferenced, the host does not own it */

static void quit_after_frame(de_system_ctx *ctx) {
    (void)ctx;
    de_engine_request_quit(g.engine);
}

static de_result game_init(de_engine *e) {
    g.engine = e;
    g.init_physics = e->physics != NULL;
    g.init_collider = has_collider(e);
    if (g.own)
        e->physics = (de_physics_world *)(void *)&g_own_world;
    return DE_OK;
}

static void game_register_types(de_engine *e, de_type_registry *types) {
    (void)e;
    (void)types;
}

static void game_register_systems(de_engine *e, de_world *w) {
    (void)e;
    de_system_desc quit;
    memset(&quit, 0, sizeof quit);
    quit.name = "test.quit";
    quit.schedule = DE_SCHEDULE_LAST;
    quit.fn = quit_after_frame;
    (void)de_system_add(w, &quit);
}

static void game_shutdown(de_engine *e) {
    g.shutdown_physics = e->physics != NULL;
    g.shutdown_world = e->physics;
    g.shutdown_collider = has_collider(e);
    if (g.own)
        e->physics = NULL; /* the game's own: it releases it */
}

static de_game_api test_api(void) {
    de_game_api api;
    memset(&api, 0, sizeof api);
    api.api_version = DE_GAME_API_VERSION;
    api.name = "host_physics_game";
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
    memset(&g, 0, sizeof g);
    set_token(NULL);
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    de_fs_desc fd;
    memset(&fd, 0, sizeof fd);
    fd.game_dir = DE_HOST_TEST_DATA_DIR;
    fd.user_dir = DE_HOST_TEST_TMP_DIR "/user";
    fd.cache_dir = DE_HOST_TEST_TMP_DIR "/cache";
    ASSERT_EQ(de_fs_init(&fd), DE_OK);
    f->fs = true;
}

static void host_fx_teardown(host_fx *f) {
    set_token(NULL);
    if (f->fs)
        de_fs_shutdown();
    de_log_shutdown();
}

static int run(const char *toml) {
    de_game_api api = test_api();
    de_host_desc d;
    memset(&d, 0, sizeof d);
    d.static_api = &api;
    d.headless = true;
    d.game_toml = toml;
    return de_host_run(&d);
}

TEST_F(host_physics, no_physics_by_default, host_fx) {
    EXPECT_EQ(run("game:/host_run.toml"), 0);
    EXPECT_FALSE(g.init_physics);
    EXPECT_FALSE(g.shutdown_physics);
    EXPECT_FALSE(g.shutdown_collider);
}

TEST_F(host_physics, game_toml_attach_before_init, host_fx) {
    EXPECT_EQ(run("game:/physics_attach.toml"), 0);
    EXPECT_EQ(g.init_physics, (bool)DE_HAS_PHYSICS);
    EXPECT_EQ(g.init_collider, (bool)DE_HAS_PHYSICS);
    EXPECT_EQ(g.shutdown_collider, (bool)DE_HAS_PHYSICS);
}

TEST_F(host_physics, editor_session_attaches_after_init, host_fx) {
    set_token("host-physics-test");
    const bool session = de_engine_editor_session(NULL); /* Dev builds with the editor service */
    EXPECT_EQ(run("game:/host_run.toml"), 0);
    EXPECT_FALSE(g.init_physics); /* the game's init ran first */
    EXPECT_EQ(g.shutdown_physics, session && DE_HAS_PHYSICS);
    EXPECT_EQ(g.shutdown_collider, session && DE_HAS_PHYSICS);
}

TEST_F(host_physics, editor_session_keeps_the_games_world, host_fx) {
    set_token("host-physics-test");
    g.own = true;
    EXPECT_EQ(run("game:/host_run.toml"), 0);
    EXPECT_TRUE(g.shutdown_world == (de_physics_world *)(void *)&g_own_world);
    EXPECT_FALSE(g.shutdown_collider); /* the host attached nothing of its own */
}

TEST(host_physics, game_toml_physics_section) {
    de__game_config c;
    const char ok[] = "[physics]\nattach = true\n";
    ASSERT_EQ(de__game_config_parse(ok, sizeof ok - 1, "t.toml", &c), DE_OK);
    EXPECT_TRUE(c.physics_attach);
    ASSERT_EQ(de__game_config_parse("", 0, "t.toml", &c), DE_OK);
    EXPECT_FALSE(c.physics_attach);
    const char bad[] = "[physics]\nattach = 1\n";
    EXPECT_EQ(de__game_config_parse(bad, sizeof bad - 1, "t.toml", &c), DE_ERR_PARSE);
    de_error_clear();
}
