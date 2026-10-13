/*
 * cook_game.c: the game libraries of the `de-cook cook --game` tests (DENG-394).
 *
 * One source, three libraries (fixtures/CMakeLists.txt):
 *   (no define)           `cook_game`, built with de_add_game() where it builds libraries. Its
 *                         register_types registers the fixture project's `CookSpin` component and
 *                         `cooktext` asset type (cook_project_types.h), the types the in-process
 *                         tests register through de_cook_hooks. It calls the engine, so it loads
 *                         only into a process that exports it (de-cook, test_cook_game).
 *   COOK_GAME_PLAIN       `cook_game_plain`: registers nothing and calls nothing in the engine,
 *                         so it loads into any process (also on Windows without DE_BUILD_SHARED).
 *   COOK_GAME_NO_SYMBOL   `cook_game_no_symbol`: exports cook_game_get_api, not de_game_get_api.
 *
 * de-cook calls register_types and nothing else. The other callbacks abort, so a cook that calls
 * one fails its test.
 */
#include <dengine/de_game.h>

#include <stdlib.h>

#if !defined(COOK_GAME_PLAIN) && !defined(COOK_GAME_NO_SYMBOL)
#include "cook_project_types.h"
#define COOK_GAME_USES_ENGINE 1
#define COOK_GAME_NAME        "cook_game"
#elif defined(COOK_GAME_PLAIN)
#define COOK_GAME_USES_ENGINE 0
#define COOK_GAME_NAME        "cook_game_plain"
#else
#define COOK_GAME_USES_ENGINE 0
#define COOK_GAME_NAME        "cook_game_no_symbol"
#endif

static de_result game_init(de_engine *e) {
    (void)e;
    abort();
}

static void game_shutdown(de_engine *e) {
    (void)e;
    abort();
}

static void game_register_types(de_engine *e, de_type_registry *types) {
#if COOK_GAME_USES_ENGINE
    /* The tool's engine holds its own registry and asset system; a runtime that loads the pack
     * passes its own (test_cook_game.c). */
    if (e == NULL || e->assets == NULL || types == NULL || e->types != types)
        return;
    (void)cct_register_types(NULL, e->assets, types);
#else
    (void)e;
    (void)types;
#endif
}

static void game_register_systems(de_engine *e, de_world *w) {
    (void)e;
    (void)w;
    abort();
}

static const de_game_api g_api = {
    .api_version = DE_GAME_API_VERSION,
    .name = COOK_GAME_NAME,
    .init = game_init,
    .shutdown = game_shutdown,
    .register_types = game_register_types,
    .register_systems = game_register_systems,
};

#if defined(COOK_GAME_NO_SYMBOL)
DE_GAME_EXPORT const de_game_api *cook_game_get_api(void);
DE_GAME_EXPORT const de_game_api *cook_game_get_api(void) {
    return &g_api;
}
#else
DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
DE_GAME_EXPORT const de_game_api *de_game_get_api(void) {
    return &g_api;
}
#endif
