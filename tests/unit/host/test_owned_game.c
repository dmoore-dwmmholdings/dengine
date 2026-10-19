#include "de_test.h"
#include "fixtures/owned_game.h"
#include <dengine/de_platform.h>
#include <dengine/de_debug.h>
#include <string.h>
static void run_owned(u32 mode) {
    owned_game_reset(mode);
    de_fs_desc fs = {.game_dir = DE_HOST_TEST_DATA_DIR,
                     .user_dir = DE_HOST_TEST_TMP_DIR "/user",
                     .cache_dir = DE_HOST_TEST_TMP_DIR "/cache"};
    ASSERT_EQ(de_fs_init(&fs), DE_OK);
    const de_host_desc desc = {
        .static_api = de_game_get_api(), .headless = true, .game_toml = "game:/owned_game.toml"};
    int code = de_host_run(&desc);
    owned_game_result r = owned_game_get_result();
    de_fs_shutdown();
    if (r.unavailable)
        SKIP("native GPU unavailable or GPU/render module disabled");
    EXPECT_EQ(r.allocations.blocks_live, 0u);
    EXPECT_EQ(r.allocations.bytes_live, 0u);
    bool success = mode == 0 || mode == 4;
    EXPECT_EQ(code, success ? 0 : 1);
    EXPECT_EQ(r.shutdowns, success ? 1u : 0u);
    if (success) {
        EXPECT_EQ(r.frames, 3u);
        EXPECT_EQ(r.unloaded, 1u);
        EXPECT_TRUE(r.dependencies_alive);
        EXPECT_EQ(r.destructed, 1u);
        EXPECT_TRUE(r.destructor_dependencies_alive);
    }
}
TEST(host_owned_game, public_game_runs_and_host_releases_transferred_modules) {
    run_owned(0);
}
TEST(host_owned_game, init_failure_after_gpu_unwinds) {
    run_owned(1);
}
TEST(host_owned_game, init_failure_after_renderer_unwinds) {
    run_owned(2);
}
TEST(host_owned_game, init_failure_after_ui_text_attach_unwinds) {
    run_owned(3);
}
TEST(host_owned_game, self_destroyed_ui_is_not_destroyed_twice) {
    run_owned(4);
}
TEST(host_owned_game, self_destroyed_gpu_is_not_destroyed_twice) {
    run_owned(5);
}

#if defined(DE_HOST_OWNED_MODULE) && DE_HAS_GPU && DE_HAS_RENDER
static bool module_unloaded_assets, module_destructed_component, module_unavailable;
static void module_sink(const de_log_record *record, void *user) {
    (void)user;
    char text[512];
    size_t n = record->message.len < sizeof text - 1 ? record->message.len : sizeof text - 1;
    memcpy(text, record->message.ptr, n);
    text[n] = 0;
    if (strstr(text, "owned_game: asset unload, dependencies alive"))
        module_unloaded_assets = true;
    if (strstr(text, "owned_game: component destruct, dependencies alive"))
        module_destructed_component = true;
    if (strstr(text, "owned_game: native GPU unavailable"))
        module_unavailable = true;
}
#endif
TEST(host_owned_game, dynamic_game_callbacks_remain_loaded_through_owned_teardown) {
#if defined(DE_HOST_OWNED_MODULE) && DE_HAS_GPU && DE_HAS_RENDER
    module_unloaded_assets = module_destructed_component = module_unavailable = false;
    ASSERT_EQ(de_log_init(&(de_log_desc){0}), DE_OK);
    de_log_add_sink(module_sink, NULL);
    de_fs_desc fs = {.game_dir = DE_HOST_OWNED_MODULE_DIR,
                     .user_dir = DE_HOST_TEST_TMP_DIR "/module_user",
                     .cache_dir = DE_HOST_TEST_TMP_DIR "/module_cache"};
    ASSERT_EQ(de_fs_init(&fs), DE_OK);
    int code = de_host_run(&(de_host_desc){.game_library = DE_HOST_OWNED_MODULE, .headless = true});
    de_fs_shutdown();
    de_log_shutdown();
    if (module_unavailable)
        SKIP("native GPU unavailable");
    EXPECT_EQ(code, 0);
    EXPECT_TRUE(module_unloaded_assets);
    EXPECT_TRUE(module_destructed_component);
#else
    SKIP("dynamic games or GPU/render unavailable in this build");
#endif
}
