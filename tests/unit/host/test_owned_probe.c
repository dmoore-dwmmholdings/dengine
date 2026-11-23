#include "de_test.h"
#include "host/host_packs.h"
#include <dengine/de_platform.h>
#include <string.h>

typedef struct probe_cpu_result {
    u32 loads, unloads;
} probe_cpu_result;
static de_result probe_cpu_load(de_asset_load_ctx *ctx) {
    static const char bytes[] = "host-owned GPU probe\n";
    u64 size = 0;
    const void *data = de_asset_load_ctx_data(ctx, &size);
    if (size != sizeof bytes - 1 || memcmp(data, bytes, sizeof bytes - 1))
        return DE_FAIL(DE_ERR_PARSE, "owned probe fixture payload differs");
    probe_cpu_result *r = de_asset_load_ctx_user(ctx);
    ++r->loads;
    de_asset_load_ctx_set_object(ctx, r, sizeof *r);
    return DE_OK;
}
static void probe_cpu_unload(void *object, void *user) {
    EXPECT_TRUE(object == user);
    ++((probe_cpu_result *)user)->unloads;
}
static void check_probe_fixture(de_asset_mode mode) {
    de_fs_desc fs = {.game_dir = DE_HOST_TEST_DATA_DIR,
                     .user_dir = DE_HOST_TEST_TMP_DIR "/probe_user",
                     .cache_dir = DE_HOST_TEST_TMP_DIR "/probe_cache"};
    ASSERT_EQ(de_fs_init(&fs), DE_OK);
    de_tracking_allocator memory;
    de_allocator alloc = de_tracking_allocator_init(&memory, NULL, DE_MEM_ASSET);
    de_fp_env_init_thread();
    de_task_system *tasks = NULL;
    ASSERT_EQ(de_task_system_create(
                  &(de_task_system_desc){.worker_count = 1, .background_worker_count = 1}, &tasks),
              DE_OK);
    de_asset_system *assets = NULL;
    ASSERT_EQ(de_asset_system_create(
                  &(de_asset_system_desc){
                      .allocator = &alloc, .tasks = tasks, .mode = mode, .io_threads = 1},
                  &assets),
              DE_OK);
    if (mode == DE_ASSET_MODE_PACKED) {
        u32 packs = 0;
        EXPECT_EQ(de__host_mount_packs(assets, DE__HOST_PACK_DIR, &alloc, &packs), DE_OK);
        EXPECT_EQ(packs, 1u);
    }
    probe_cpu_result r = {0};
    ASSERT_TRUE(de_asset_type_register(assets, &(de_asset_type_desc){.name = "host_owned_probe",
                                                                     .load = probe_cpu_load,
                                                                     .unload = probe_cpu_unload,
                                                                     .user = &r,
                                                                     .main_thread_load = true})
                    .id);
    EXPECT_EQ(de_asset_manifest_load(assets, "game:/owned_assets.toml"), DE_OK);
    de_load_handle load = de_asset_load(assets, de_asset_id_from("probe"), 0);
    EXPECT_EQ(de_asset_wait(assets, load, 30ULL * 1000000000), DE_LOAD_READY);
    EXPECT_EQ(r.loads, 1u);
    de_asset_system_destroy(assets);
    de_task_system_destroy(tasks);
    de_fs_shutdown();
    EXPECT_EQ(r.unloads, 1u);
    EXPECT_EQ(de_tracking_allocator_stats(&memory).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&memory).bytes_live, 0u);
}
TEST(host_owned_probe, source_probe_fixture_loads_exact_bytes_without_gpu) {
    check_probe_fixture(DE_ASSET_MODE_SOURCE);
}
TEST(host_owned_probe, packed_probe_fixture_loads_exact_bytes_without_gpu) {
    check_probe_fixture(DE_ASSET_MODE_PACKED);
}
