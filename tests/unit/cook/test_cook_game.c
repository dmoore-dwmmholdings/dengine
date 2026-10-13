/*
 * test_cook_game.c: `de-cook cook --game <library>` (DENG-394). CTest names cook_game.<test>.
 *
 * The tests run de_cook_cook_run in-process, without de_cook_hooks: the fixture project's types
 * come from the game library alone. Libraries (fixtures/cook_game.c):
 *   COOK_GAME_LIB            cook_game, which registers the project's `cooktext` asset type and
 *                            `CookSpin` component. Defined only where de_add_game() builds
 *                            libraries with a static engine (macOS, Linux; not Shipping, not
 *                            DE_BUILD_SHARED); this executable then links the whole engine and
 *                            exports it, like de-cook, so the library binds to it.
 *   COOK_GAME_PLAIN_LIB      registers nothing and needs nothing from the engine (every build).
 *   COOK_GAME_NO_SYMBOL_LIB  does not export de_game_get_api (every build).
 *
 * Outputs go to the scratch directory DE_COOK_CLI_TMP_DIR, one subdirectory per test. Cooking
 * needs the runtime importers (DE_HAS_COOKER: Debug and Dev); elsewhere the tests skip, except
 * `cook_game.usage`.
 */
#include "cook_project_util.h"

#include <dengine/de_game.h>
#include <dengine/de_task.h>

#include "asset/slots_internal.h"
#include "cook/lib/de_cooker.h"
#include "host/game_module.h"

#define NO_COOKER_MSG "this build has no runtime importers (DE_HAS_COOKER: Debug and Dev)"
#define NO_GAME_LIB   "no game library in this build (de_add_game builds executables here)"
#define WAIT_NS       ((u64)60 * 1000000000u)

/* Runs `de-cook cook` in-process without hooks (NULL-terminated arguments). */
static int cg_cook(const char *const *args) {
    cct_reset();
    de_cook_io io = {cct_write, NULL};
    return de_cook_cook_run(cct_argc(args), args, &io, NULL);
}

#if DE_HAS_COOKER
/* Cooks `manifest` into CCT_TMP/<sub>/<name>.dpak for `platform` without a cache, with the game
 * library `game` (NULL = none) and up to 4 more arguments (NULL-terminated, may be NULL). */
static int cg_cook_to(const char *manifest, const char *sub, const char *name, const char *platform,
                      const char *game, const char *const *extra) {
    char out[DE_FS_PATH_MAX];
    cct_path(out, sizeof out, sub);
    const char *args[24] = {manifest,      "--platform", platform,  "--out",     out,
                            "--pack-name", name,         "--quiet", "--no-cache"};
    int n = 9;
    if (game != NULL) {
        args[n++] = "--game";
        args[n++] = game;
    }
    for (int i = 0; extra != NULL && extra[i] != NULL && i < 4; ++i)
        args[n++] = extra[i];
    args[n] = NULL;
    return cg_cook(args);
}

static u8 *cg_read_pack(const char *sub, const char *name, size_t *size) {
    char rel[256], path[DE_FS_PATH_MAX];
    snprintf(rel, sizeof rel, "%s/%s.dpak", sub, name);
    return cct_read(cct_path(path, sizeof path, rel), size);
}

static bool cg_pack_exists(const char *sub, const char *name) {
    size_t n = 0;
    u8 *p = cg_read_pack(sub, name, &n);
    free(p);
    return p != NULL;
}

/* The two packs CCT_TMP/<sub>/<a>.dpak and <b>.dpak exist and hold the same bytes. */
static void cg_expect_same_pack(const char *sub, const char *a, const char *b) {
    size_t na = 0, nb = 0;
    u8 *pa = cg_read_pack(sub, a, &na);
    u8 *pb = cg_read_pack(sub, b, &nb);
    ASSERT_TRUE(pa != NULL);
    ASSERT_TRUE(pb != NULL);
    EXPECT_EQ(na, nb);
    EXPECT_TRUE(na == nb && memcmp(pa, pb, na) == 0);
    free(pa);
    free(pb);
}
#endif

/* ---- test_cook_game_custom_type_cooks_and_loads ------------------------------------------- */

/* The full fixture project, whose `cooktext` assets and `CookSpin` prefab need game types,
 * cooks with --game alone into the same bytes as with the in-process hooks. A runtime asset
 * system with the game's types (registered by the same library) loads the custom asset and its
 * sub-asset from that pack through the game's loader. */
TEST(cook_game, custom_type_cooks_and_loads) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#elif !defined(COOK_GAME_LIB)
    SKIP(NO_GAME_LIB);
#else
    cct_fresh("custom");
    const char *host = de_cook_platform_name(de_cook_platform_host());
    ASSERT_EQ(
        cg_cook_to(CCT_PROJECT "/assets.toml", "custom", "with_game", host, COOK_GAME_LIB, NULL),
        DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "assets cooked: 7 (+2 sub-assets), bundles: 2") != NULL);
    EXPECT_TRUE(strstr(g_cct.out.p, "game: cook_game (1 asset types added)") != NULL);

    /* The same project through the hooks, which register the same types in-process. */
    char out[DE_FS_PATH_MAX];
    cct_path(out, sizeof out, "custom");
    const char *hooked[] = {CCT_PROJECT "/assets.toml",
                            "--platform",
                            host,
                            "--out",
                            out,
                            "--pack-name",
                            "with_hooks",
                            "--quiet",
                            "--no-cache",
                            NULL};
    ASSERT_EQ(cct_cook(hooked), DE_COOK_EXIT_OK);
    cg_expect_same_pack("custom", "with_game", "with_hooks");

    /* The runtime side: the game's register_types on a runtime asset system, then the pack. */
    de__game_module game;
    memset(&game, 0, sizeof game);
    ASSERT_EQ(de__game_module_load(&game, COOK_GAME_LIB), DE_OK);
    de_fp_env_init_thread();
    de_task_system *ts = NULL;
    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &ts), DE_OK);
    de_type_registry *types = NULL;
    ASSERT_EQ(de_type_registry_create(NULL, &types), DE_OK);
    de_asset_system *s = NULL;
    ASSERT_EQ(de_asset_system_create(
                  &(de_asset_system_desc){
                      .tasks = ts, .mode = DE_ASSET_MODE_PACKED, .io_threads = 1, .types = types},
                  &s),
              DE_OK);
    de_engine e;
    memset(&e, 0, sizeof e);
    e.allocator = de_heap_allocator();
    e.tasks = ts;
    e.types = types;
    e.assets = s;
    game.table.register_types(&e, types);
    ASSERT_EQ(de_asset_pack_mount(s, "cct:/custom/with_game.dpak", 0), DE_OK);

    const de_asset_id greeting = de_asset_id_from("text/greeting");
    const de_asset_id lower = de_asset_id_from("text/greeting#lower");
    de_load_handle h = de_asset_load(s, greeting, 0);
    ASSERT_EQ((int)de_asset_wait(s, h, WAIT_NS), (int)DE_LOAD_READY);
    de_load_handle hl = de_asset_load(s, lower, 0);
    ASSERT_EQ((int)de_asset_wait(s, hl, WAIT_NS), (int)DE_LOAD_READY);
    static const char k_text[] = "Good morning, traveller.";
    const size_t k_len = sizeof k_text - 1;
    const cct_text *t = (const cct_text *)de__asset_object(s, de__slot_find(s, greeting));
    ASSERT_TRUE(t != NULL);
    ASSERT_GE(t->size, (u64)k_len);
    EXPECT_MEMEQ((const char *)(t + 1), "GOOD MORNING, TRAVELLER.", k_len);
    const cct_text *tl = (const cct_text *)de__asset_object(s, de__slot_find(s, lower));
    ASSERT_TRUE(tl != NULL);
    EXPECT_EQ(tl->size, t->size);
    EXPECT_MEMEQ((const char *)(tl + 1), "good morning, traveller.", k_len);
    de_asset_release(s, hl);
    de_asset_release(s, h);

    /* The prefab with the game's component, and its dependency. */
    h = de_asset_load(s, de_asset_id_from("props/crate"), 0);
    EXPECT_EQ((int)de_asset_wait(s, h, WAIT_NS), (int)DE_LOAD_READY);
    de_asset_release(s, h);

    /* Everything that points into the library goes before the library. */
    de_asset_system_destroy(s);
    de_type_registry_destroy(types);
    de_task_system_destroy(ts);
    de__game_module_unload(&game);
#endif
}

/* ---- test_cook_game_deterministic ------------------------------------------------------------ */

/* Cooking twice with the game gives the same bytes: a cold and a warm cache, and no cache on one
 * thread. */
TEST(cook_game, deterministic) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#elif !defined(COOK_GAME_LIB)
    SKIP(NO_GAME_LIB);
#else
    cct_fresh("twice");
    char out[DE_FS_PATH_MAX], cache[DE_FS_PATH_MAX];
    cct_path(out, sizeof out, "twice");
    cct_path(cache, sizeof cache, "twice/cache");
    const char *cold[] = {CCT_PROJECT "/assets.toml",
                          "--platform",
                          "linux",
                          "--out",
                          out,
                          "--cache",
                          cache,
                          "--pack-name",
                          "cold",
                          "--quiet",
                          "--game",
                          COOK_GAME_LIB,
                          NULL};
    ASSERT_EQ(cg_cook(cold), DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "cache hits: 0, misses: 7") != NULL);
    cold[8] = "warm";
    ASSERT_EQ(cg_cook(cold), DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "cache hits: 7, misses: 0") != NULL);
    ASSERT_EQ(cg_cook_to(CCT_PROJECT "/assets.toml", "twice", "nocache", "linux", COOK_GAME_LIB,
                         (const char *const[]){"--jobs", "1", NULL}),
              DE_COOK_EXIT_OK);
    cg_expect_same_pack("twice", "cold", "warm");
    cg_expect_same_pack("twice", "cold", "nocache");
#endif
}

/* ---- test_cook_game_no_types_same_pack --------------------------------------------------------
 */

/* A project that uses none of the game's types cooks into the same bytes with and without a
 * game library: engine.toml without --game, with the plain library (every build) and with
 * cook_game (where it exists). The full project with the hooks and the plain library equals the
 * full project with the hooks alone. */
TEST(cook_game, no_types_same_pack) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    cct_fresh("same");
    const char *engine = CCT_PROJECT "/engine.toml";
    ASSERT_EQ(cg_cook_to(engine, "same", "none", "windows", NULL, NULL), DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "game:") == NULL);
    ASSERT_EQ(cg_cook_to(engine, "same", "plain", "windows", COOK_GAME_PLAIN_LIB, NULL),
              DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "game: cook_game_plain (0 asset types added)") != NULL);
    cg_expect_same_pack("same", "none", "plain");
#if defined(COOK_GAME_LIB)
    ASSERT_EQ(cg_cook_to(engine, "same", "game", "windows", COOK_GAME_LIB, NULL), DE_COOK_EXIT_OK);
    cg_expect_same_pack("same", "none", "game");
#endif

    char out[DE_FS_PATH_MAX];
    cct_path(out, sizeof out, "same");
    const char *hooks_only[] = {CCT_PROJECT "/assets.toml",
                                "--platform",
                                "windows",
                                "--out",
                                out,
                                "--pack-name",
                                "hooks",
                                "--quiet",
                                "--no-cache",
                                NULL,
                                NULL,
                                NULL};
    ASSERT_EQ(cct_cook(hooks_only), DE_COOK_EXIT_OK);
    hooks_only[6] = "hooks_plain";
    hooks_only[9] = "--game";
    hooks_only[10] = COOK_GAME_PLAIN_LIB;
    ASSERT_EQ(cct_cook(hooks_only), DE_COOK_EXIT_OK);
    cg_expect_same_pack("same", "hooks", "hooks_plain");
#endif
}

/* ---- test_cook_game_refused -------------------------------------------------------------------
 */

/* A library that cannot be used stops the command with exit 2 before anything cooks, and a
 * library that registers nothing leaves the project's own types unknown (exit 1). No pack in
 * either case. */
TEST(cook_game, refused) {
#if !DE_HAS_COOKER
    SKIP(NO_COOKER_MSG);
#else
    cct_fresh("refused");
    const char *engine = CCT_PROJECT "/engine.toml";
    char missing[DE_FS_PATH_MAX];
    cct_path(missing, sizeof missing, "refused/no_such_game.lib");

    ASSERT_EQ(cg_cook_to(engine, "refused", "missing", "linux", missing, NULL), DE_COOK_EXIT_USAGE);
    EXPECT_TRUE(strstr(g_cct.err.p, "de-cook: cannot load the game library '") != NULL);
    EXPECT_TRUE(strstr(g_cct.err.p, "no_such_game.lib") != NULL);
    EXPECT_FALSE(cg_pack_exists("refused", "missing"));

    ASSERT_EQ(cg_cook_to(engine, "refused", "nosym", "linux", COOK_GAME_NO_SYMBOL_LIB, NULL),
              DE_COOK_EXIT_USAGE);
    EXPECT_TRUE(strstr(g_cct.err.p, "does not export de_game_get_api()") != NULL);
    EXPECT_FALSE(cg_pack_exists("refused", "nosym"));

    /* The plain library adds no type: cooktext is unknown, as without --game. */
    ASSERT_EQ(cg_cook_to(CCT_PROJECT "/assets.toml", "refused", "plain", "linux",
                         COOK_GAME_PLAIN_LIB, NULL),
              DE_COOK_EXIT_ERRORS);
    EXPECT_TRUE(strstr(g_cct.out.p, "unknown asset type \"cooktext\"") != NULL);
    EXPECT_FALSE(cg_pack_exists("refused", "plain"));

    /* Every mount is gone afterwards, so the next run in this process works. */
    ASSERT_EQ(cg_cook_to(engine, "refused", "after", "linux", COOK_GAME_PLAIN_LIB, NULL),
              DE_COOK_EXIT_OK);
    EXPECT_TRUE(cg_pack_exists("refused", "after"));
#endif
}

/* ---- test_cook_game_usage ---------------------------------------------------------------------
 */

TEST(cook_game, usage) {
    const char *no_value[] = {"x.toml", "--platform", "linux", "--out", "o", "--game", NULL};
    EXPECT_EQ(cg_cook(no_value), DE_COOK_EXIT_USAGE);
    EXPECT_TRUE(strstr(g_cct.err.p, "missing value for '--game'") != NULL);
    const char *empty[] = {"x.toml", "--platform", "linux", "--out", "o", "--game=", NULL};
    EXPECT_EQ(cg_cook(empty), DE_COOK_EXIT_USAGE);
    EXPECT_TRUE(strstr(g_cct.err.p, "missing value for '--game'") != NULL);
    const char *twice[] = {"x.toml", "--platform", "linux",    "--out", "o",
                           "--game", "a",          "--game=b", NULL};
    EXPECT_EQ(cg_cook(twice), DE_COOK_EXIT_USAGE);
    EXPECT_TRUE(strstr(g_cct.err.p, "more than one game library given: 'b'") != NULL);
    const char *help[] = {"--help", NULL};
    EXPECT_EQ(cg_cook(help), DE_COOK_EXIT_OK);
    EXPECT_TRUE(strstr(g_cct.out.p, "--game <library>") != NULL);
}
