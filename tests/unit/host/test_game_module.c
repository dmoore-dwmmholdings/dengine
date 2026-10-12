/*
 * test_game_module.c: loading the game (DENG-92, SPEC §10.1): de__game_attach / detach and the
 * de__game_module functions under them.
 *
 * The libraries are the plain fixtures of fixtures/CMakeLists.txt (DE_HOST_PLAIN_GAME_DIR),
 * which call nothing in the engine, so they load into this test executable. The full fixture
 * `test_game`, which calls the engine, runs in the `dengine` executable instead (the
 * host_game_* CTest entries).
 */
#include "de_test.h"

#include "host/host_internal.h"

#include <stdio.h>
#include <string.h>

#ifndef DE_HOST_PLAIN_GAME_DIR
#error "DE_HOST_PLAIN_GAME_DIR must be defined by tests/unit/host/CMakeLists.txt"
#endif

/* The native path of a plain fixture: "standalone" → <dir>/libtest_game_standalone.dylib. */
static void plain_path(char *buf, size_t cap, const char *variant) {
    char base[DE_FS_PATH_MAX];
    snprintf(base, sizeof base, "%s/test_game_%s", DE_HOST_PLAIN_GAME_DIR, variant);
    ASSERT_EQ(de_dylib_platform_name(base, buf, cap), DE_OK);
}

/* True when the calling thread's last error, or an error it wraps, contains `needle`. */
static bool error_mentions(const char *needle) {
    for (const de_error *e = de_error_last(); e != NULL; e = e->cause) {
        if (strstr(e->message, needle) != NULL)
            return true;
    }
    return false;
}

/* A complete static game whose callbacks count their calls. */
static int g_inits, g_shutdowns;

static de_result static_init(de_engine *e) {
    (void)e;
    g_inits++;
    return DE_OK;
}
static void static_shutdown(de_engine *e) {
    (void)e;
    g_shutdowns++;
}
static void static_types(de_engine *e, de_type_registry *t) {
    (void)e;
    (void)t;
}
static void static_systems(de_engine *e, de_world *w) {
    (void)e;
    (void)w;
}

static de_game_api static_api(void) {
    de_game_api api;
    memset(&api, 0, sizeof api);
    api.api_version = DE_GAME_API_VERSION;
    api.name = "static_game";
    api.init = static_init;
    api.shutdown = static_shutdown;
    api.register_types = static_types;
    api.register_systems = static_systems;
    return api;
}

/* A host as de_host_run has it at attach time: config defaults, a log category. */
typedef struct gm_fx {
    de__host h;
    de_host_desc d;
} gm_fx;

static void gm_fx_setup(gm_fx *f) {
    ASSERT_EQ(de_log_init(NULL), DE_OK);
    de__game_config_defaults(&f->h.config);
    f->h.log = de_log_register_category("host", DE_LOG_INFO);
    f->h.game_toml = "game:/game.toml";
    g_inits = 0;
    g_shutdowns = 0;
    de_error_clear();
}

static void gm_fx_teardown(gm_fx *f) {
    de__game_detach(&f->h);
    de_log_shutdown();
}

/* ---- Loading -------------------------------------------------------------------------------- */

TEST(game_module, load_dylib) {
    char path[DE_FS_PATH_MAX];
    plain_path(path, sizeof path, "standalone");

    de__game_module m;
    memset(&m, 0, sizeof m);
    ASSERT_EQ(de__game_module_load(&m, path), DE_OK);
    EXPECT_TRUE(m.lib != NULL);
    ASSERT_TRUE(m.api != NULL);
    EXPECT_STREQ(m.api->name, "test_game_standalone");
    EXPECT_STREQ(m.table.name, "test_game_standalone");
    EXPECT_STREQ(m.path, path);
    EXPECT_EQ(m.generation, 1u);
    EXPECT_EQ(m.table.api_version, (u32)DE_GAME_API_VERSION);

    /* The game left on_unload / on_reload NULL: the copy has no-ops, the game's table not. */
    EXPECT_TRUE(m.api->on_unload == NULL);
    ASSERT_TRUE(m.table.on_unload != NULL);
    ASSERT_TRUE(m.table.on_reload != NULL);
    m.table.on_unload(NULL);
    m.table.on_reload(NULL);
    /* The library's own code runs (the standalone game never touches the engine). */
    EXPECT_EQ(m.table.init(NULL), DE_OK);
    m.table.register_types(NULL, NULL);
    m.table.register_systems(NULL, NULL);
    m.table.shutdown(NULL);

    de__game_module_unload(&m);
    EXPECT_TRUE(m.lib == NULL);
    EXPECT_TRUE(m.api == NULL);
    EXPECT_STREQ(m.path, "");
    EXPECT_EQ(m.generation, 1u); /* kept: DENG-93 counts reloads */

    ASSERT_EQ(de__game_module_load(&m, path), DE_OK);
    EXPECT_EQ(m.generation, 2u);
    de__game_module_unload(&m);
    de__game_module_unload(NULL); /* ignored */
}

TEST_F(game_module, attach_dylib, gm_fx) {
    char path[DE_FS_PATH_MAX];
    plain_path(path, sizeof path, "standalone");
    f->d.game_library = path;
    ASSERT_EQ(de__game_attach(&f->h, &f->d), DE_OK);
    EXPECT_TRUE(f->h.api == &f->h.game.table); /* the host calls the copy */
    EXPECT_TRUE(f->h.game.lib != NULL);
    EXPECT_STREQ(f->h.api->name, "test_game_standalone");

    de__game_detach(&f->h);
    EXPECT_TRUE(f->h.api == NULL);
    EXPECT_TRUE(f->h.game.lib == NULL);
    de__game_detach(&f->h); /* nothing attached: fine */
}

TEST_F(game_module, static, gm_fx) {
    de_game_api api = static_api();
    f->d.static_api = &api;
    ASSERT_TRUE(f->d.game_library == NULL);
    ASSERT_EQ(de__game_attach(&f->h, &f->d), DE_OK);
    EXPECT_TRUE(f->h.game.lib == NULL); /* nothing loaded */
    EXPECT_STREQ(f->h.game.path, "");
    EXPECT_TRUE(f->h.game.api == &api);
    EXPECT_TRUE(f->h.api == &f->h.game.table);
    EXPECT_EQ(f->h.game.generation, 1u);
    EXPECT_EQ(f->h.api->init(NULL), DE_OK);
    EXPECT_EQ(g_inits, 1);
    f->h.api->shutdown(NULL);
    EXPECT_EQ(g_shutdowns, 1);
    EXPECT_TRUE(f->h.api->on_unload != NULL);
    de__game_detach(&f->h);
    EXPECT_TRUE(f->h.api == NULL);

    /* A library wins over the static table. */
    char path[DE_FS_PATH_MAX];
    plain_path(path, sizeof path, "standalone");
    f->d.game_library = path;
    ASSERT_EQ(de__game_attach(&f->h, &f->d), DE_OK);
    EXPECT_TRUE(f->h.game.lib != NULL);
    EXPECT_STREQ(f->h.api->name, "test_game_standalone");
}

/* ---- Failures ------------------------------------------------------------------------------- */

TEST_F(game_module, missing_lib, gm_fx) {
    char path[DE_FS_PATH_MAX];
    plain_path(path, sizeof path, "does_not_exist");
    de__game_module m;
    memset(&m, 0, sizeof m);
    EXPECT_EQ(de__game_module_load(&m, path), DE_ERR_NOT_FOUND);
    EXPECT_TRUE(m.lib == NULL);
    EXPECT_EQ(m.generation, 0u);
    EXPECT_EQ(de__game_module_load(&m, ""), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__game_module_load(NULL, path), DE_ERR_INVALID_ARGUMENT);

    f->d.game_library = path;
    EXPECT_EQ(de__game_attach(&f->h, &f->d), DE_ERR_NOT_FOUND);
    EXPECT_TRUE(f->h.api == NULL);
    EXPECT_TRUE(error_mentions("does_not_exist"));

    /* A bare file name is a file in the current directory, never a library search. */
    f->d.game_library = "no_such_game_library_xyz.so";
    EXPECT_EQ(de__game_attach(&f->h, &f->d), DE_ERR_NOT_FOUND);

    /* No game anywhere. */
    f->d.game_library = NULL;
    EXPECT_EQ(de__game_attach(&f->h, &f->d), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_mentions("no game"));
}

TEST_F(game_module, missing_symbol, gm_fx) {
    char path[DE_FS_PATH_MAX];
    plain_path(path, sizeof path, "no_symbol");
    f->d.game_library = path;
    EXPECT_EQ(de__game_attach(&f->h, &f->d), DE_ERR_NOT_FOUND);
    EXPECT_TRUE(error_mentions("de_game_get_api"));
    EXPECT_TRUE(f->h.api == NULL);
    EXPECT_TRUE(f->h.game.lib == NULL); /* closed again */
}

TEST_F(game_module, bad_version, gm_fx) {
    char path[DE_FS_PATH_MAX];
    plain_path(path, sizeof path, "bad_version");
    f->d.game_library = path;
    EXPECT_EQ(de__game_attach(&f->h, &f->d), DE_ERR_VERSION_MISMATCH);
    EXPECT_TRUE(error_mentions("test_game_bad_version"));
    EXPECT_TRUE(f->h.api == NULL);
    EXPECT_TRUE(f->h.game.lib == NULL);

    de_game_api api = static_api();
    api.api_version = DE_GAME_API_VERSION + 1;
    f->d.game_library = NULL;
    f->d.static_api = &api;
    EXPECT_EQ(de__game_attach(&f->h, &f->d), DE_ERR_VERSION_MISMATCH);
    EXPECT_EQ(g_inits, 0);
}

TEST_F(game_module, missing_callback, gm_fx) {
    char path[DE_FS_PATH_MAX];
    plain_path(path, sizeof path, "no_callback");
    f->d.game_library = path;
    EXPECT_EQ(de__game_attach(&f->h, &f->d), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(error_mentions("de_game_api.register_systems"));
    EXPECT_TRUE(f->h.game.lib == NULL);

    /* Every required field, one at a time, on a static table. */
    static const char *const fields[] = {"name", "init", "shutdown", "register_types",
                                         "register_systems"};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        de_game_api api = static_api();
        switch (i) {
        case 0:
            api.name = NULL;
            break;
        case 1:
            api.init = NULL;
            break;
        case 2:
            api.shutdown = NULL;
            break;
        case 3:
            api.register_types = NULL;
            break;
        default:
            api.register_systems = NULL;
            break;
        }
        EXPECT_EQ(de__game_api_validate(&api, "static"), DE_ERR_INVALID_ARGUMENT);
        EXPECT_TRUE(error_mentions(fields[i]));
    }
    EXPECT_EQ(de__game_api_validate(NULL, "static"), DE_ERR_INVALID_ARGUMENT);

    /* on_unload and on_reload are optional. */
    de_game_api api = static_api();
    EXPECT_EQ(de__game_api_validate(&api, "static"), DE_OK);
    f->d.game_library = NULL;
    f->d.static_api = &api;
    api.register_types = NULL;
    EXPECT_EQ(de__game_attach(&f->h, &f->d), DE_ERR_INVALID_ARGUMENT);
    EXPECT_TRUE(f->h.api == NULL);
}

/* ---- game.toml [game] library --------------------------------------------------------------- */

TEST_F(game_module, toml_library, gm_fx) {
    ASSERT_EQ(de_fs_mount("game", DE_HOST_PLAIN_GAME_DIR, DE_MOUNT_READ_ONLY, 0), DE_OK);

    /* No extension: the platform name, next to game.toml (game:/). */
    snprintf(f->h.config.library, sizeof f->h.config.library, "test_game_standalone");
    EXPECT_EQ(de__game_attach(&f->h, &f->d), DE_OK);
    char want[DE_FS_PATH_MAX]; /* the file name only: the separators differ per OS */
    ASSERT_EQ(de_dylib_platform_name("test_game_standalone", want, sizeof want), DE_OK);
    EXPECT_TRUE(strstr(f->h.game.path, want) != NULL);
    EXPECT_TRUE(f->h.game.lib != NULL);
    de__game_detach(&f->h);

    /* A static table wins over game.toml. */
    de_game_api api = static_api();
    f->d.static_api = &api;
    EXPECT_EQ(de__game_attach(&f->h, &f->d), DE_OK);
    EXPECT_TRUE(f->h.game.lib == NULL);
    de__game_detach(&f->h);
    f->d.static_api = NULL;

    /* A library game.toml names that is not there. */
    snprintf(f->h.config.library, sizeof f->h.config.library, "no_such_game");
    EXPECT_EQ(de__game_attach(&f->h, &f->d), DE_ERR_NOT_FOUND);
    EXPECT_TRUE(error_mentions("[game] library"));

    /* Resolution rules. */
    char buf[DE_FS_PATH_MAX];
    char expect[DE_FS_PATH_MAX];
#if defined(_WIN32)
    const char *absolute = "C:/games/mygame.dll";
#else
    const char *absolute = "/games/libmygame.so";
#endif
    EXPECT_EQ(de__game_library_resolve(absolute, buf, sizeof buf), DE_OK);
    EXPECT_STREQ(buf, absolute);
    EXPECT_EQ(de__game_library_resolve("sub/mygame", buf, sizeof buf), DE_OK);
    ASSERT_EQ(de_dylib_platform_name("mygame", expect, sizeof expect), DE_OK);
    EXPECT_TRUE(strstr(buf, expect) != NULL); /* <game dir>/sub/libmygame.<ext> */
    EXPECT_TRUE(strstr(buf, "sub") != NULL);
    EXPECT_EQ(de__game_library_resolve("game:/mygame.bin", buf, sizeof buf), DE_OK);
    EXPECT_TRUE(strstr(buf, "mygame.bin") != NULL); /* has an extension: kept */
    EXPECT_EQ(de__game_library_resolve("nowhere:/mygame.bin", buf, sizeof buf), DE_ERR_NOT_FOUND);
    EXPECT_EQ(de__game_library_resolve("", buf, sizeof buf), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de__game_library_resolve("mygame.bin", buf, 4), DE_ERR_INVALID_ARGUMENT);

    EXPECT_EQ(de_fs_unmount("game", DE_HOST_PLAIN_GAME_DIR), DE_OK);
}
