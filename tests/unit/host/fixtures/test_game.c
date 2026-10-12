/*
 * test_game.c: the game fixture of the host tests (DENG-92, SPEC §10.1).
 *
 * One source, several libraries (tests/unit/host/fixtures/CMakeLists.txt):
 *   (no define)               `test_game`, built with de_add_game(): a library in Debug, Dev and
 *                             Release, a static executable in Shipping. It calls the engine (log,
 *                             systems, quit), so it only works when the engine binds to it.
 *   TEST_GAME_STANDALONE      a valid game that calls nothing in the engine, for the in-process
 *                             tests (test_game_module.c), which do not export the engine.
 *   TEST_GAME_BAD_VERSION     api_version is DE_GAME_API_VERSION + 1.
 *   TEST_GAME_NO_CALLBACK     register_systems is NULL.
 *   TEST_GAME_NO_SYMBOL       exports test_game_get_api, not de_game_get_api.
 *
 * The full game records the order of its callbacks and logs it at shutdown, as
 *   test_game: calls init,register_types,register_systems,update,shutdown
 * which the CTest scripts (check_run.cmake) look for. `update` is an Update system that asks
 * the main loop to quit, so the run ends after one frame.
 */
#include <dengine/de_game.h>

#include <string.h>

#if defined(TEST_GAME_STANDALONE) || defined(TEST_GAME_BAD_VERSION) ||                             \
    defined(TEST_GAME_NO_CALLBACK) || defined(TEST_GAME_NO_SYMBOL)
#define TEST_GAME_USES_ENGINE 0
#else
#define TEST_GAME_USES_ENGINE 1
#endif

#if defined(TEST_GAME_STANDALONE)
#define TEST_GAME_NAME "test_game_standalone"
#elif defined(TEST_GAME_BAD_VERSION)
#define TEST_GAME_NAME "test_game_bad_version"
#elif defined(TEST_GAME_NO_CALLBACK)
#define TEST_GAME_NAME "test_game_no_callback"
#elif defined(TEST_GAME_NO_SYMBOL)
#define TEST_GAME_NAME "test_game_no_symbol"
#else
#define TEST_GAME_NAME "test_game"
#endif

/* The callbacks so far, comma-separated. Fixed size: a game library has no allocator of its
 * own before init. */
static char g_calls[256];

static void record(const char *call) {
    size_t used = strlen(g_calls);
    size_t n = strlen(call);
    if (used + n + 2 > sizeof g_calls)
        return;
    if (used > 0)
        g_calls[used++] = ',';
    memcpy(g_calls + used, call, n + 1);
}

#if TEST_GAME_USES_ENGINE
static de_log_category g_log;

static void game_update(de_system_ctx *ctx) {
    /* Once: the loop may run another frame before it sees the request. */
    if (strstr(g_calls, "update") == NULL)
        record("update");
    de_engine_request_quit((de_engine *)ctx->user);
}
#endif

static de_result game_init(de_engine *e) {
    record("init");
#if TEST_GAME_USES_ENGINE
    g_log = de_log_register_category("game", DE_LOG_INFO);
    if (e == NULL || e->world == NULL || e->types == NULL || e->tasks == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "test_game: init got an incomplete engine");
    DE_INFO(g_log, "test_game: init");
#else
    (void)e;
#endif
    return DE_OK;
}

static void game_register_types(de_engine *e, de_type_registry *types) {
    (void)e;
    (void)types;
    record("register_types");
}

static void game_register_systems(de_engine *e, de_world *w) {
    record("register_systems");
#if TEST_GAME_USES_ENGINE
    de_system_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.name = "test_game.update";
    sd.schedule = DE_SCHEDULE_UPDATE;
    sd.fn = game_update;
    sd.user = e;
    sd.flags = DE_SYSTEM_MAIN_THREAD;
    if (!DE_HANDLE_VALID(de_system_add(w, &sd)))
        DE_ERROR(g_log, "test_game: de_system_add failed");
#else
    (void)e;
    (void)w;
#endif
}

static void game_shutdown(de_engine *e) {
    (void)e;
    record("shutdown");
#if TEST_GAME_USES_ENGINE
    DE_INFO(g_log, "test_game: calls %s", g_calls);
#endif
}

static const de_game_api g_api = {
#if defined(TEST_GAME_BAD_VERSION)
    .api_version = DE_GAME_API_VERSION + 1,
#else
    .api_version = DE_GAME_API_VERSION,
#endif
    .name = TEST_GAME_NAME,
    .init = game_init,
    .shutdown = game_shutdown,
    .register_types = game_register_types,
#if !defined(TEST_GAME_NO_CALLBACK)
    .register_systems = game_register_systems,
#endif
};

#if defined(TEST_GAME_NO_CALLBACK)
/* Referenced, so the unused function does not warn. */
void (*const test_game_unused)(de_engine *, de_world *) = game_register_systems;
#endif

#if defined(TEST_GAME_NO_SYMBOL)
DE_GAME_EXPORT const de_game_api *test_game_get_api(void);
DE_GAME_EXPORT const de_game_api *test_game_get_api(void) {
    return &g_api;
}
#else
DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    return &g_api;
}
#endif
