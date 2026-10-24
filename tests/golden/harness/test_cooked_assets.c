#include "de_test.h"
#include "cooked_assets.h"
#include <dengine/de_platform.h>
#include <dengine/de_text.h>
#include <stdio.h>
#include <string.h>

typedef struct pack_fixture {
    de_asset_system *assets;
    de_text_system *text;
    de_load_handle loads[2];
    char pack[256];
} pack_fixture;

static void pack_fixture_setup(pack_fixture *f) {
    ASSERT_EQ(de_fs_mount("goldenroot", DE_GOLDEN_TEST_ROOT, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(
        de_asset_system_create(&(de_asset_system_desc){.mode = DE_ASSET_MODE_PACKED}, &f->assets),
        DE_OK);
    ASSERT_EQ(de_text_create(NULL, f->assets, &f->text), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "goldenroot:/tests/golden/text/assets.toml"),
              DE_OK);
}

static void pack_fixture_teardown(pack_fixture *f) {
    for (u32 i = 0; i < 2; ++i)
        if (f->loads[i].id)
            de_asset_release(f->assets, f->loads[i]);
    de_text_destroy(f->text);
    de_asset_system_destroy(f->assets);
    char original[256];
    memcpy(original, f->pack, sizeof original);
    ASSERT_EQ(de_golden_pack_cleanup(f->pack), DE_OK);
    EXPECT_EQ(f->pack[0], 0);
    if (original[0]) {
        char physical[4096], check[256];
        EXPECT_NE(de_fs_resolve(original, physical, sizeof physical), DE_OK);
        ASSERT_EQ(de_fs_mount("packcheck", DE_GOLDEN_PACK_TMP, DE_MOUNT_READ_ONLY, 0), DE_OK);
        const char *colon = strchr(original, ':');
        ASSERT_TRUE(colon);
        const char *file = colon + 1;
        snprintf(check, sizeof check, "packcheck:%s", file);
        EXPECT_FALSE(de_path_exists(check));
        ASSERT_EQ(de_fs_unmount("packcheck", DE_GOLDEN_PACK_TMP), DE_OK);
    }
    ASSERT_EQ(de_fs_unmount("goldenroot", DE_GOLDEN_TEST_ROOT), DE_OK);
}

TEST_F(golden_assets, packed_fonts_and_cleanup, pack_fixture) {
    ASSERT_EQ(de_golden_pack_assets(f->assets, "goldenroot:/tests/data/fonts", f->pack), DE_OK);
    ASSERT_TRUE(de_path_exists(f->pack));
    const char *names[] = {"font/latin", "font/bitmap"};
    for (u32 i = 0; i < 2; ++i) {
        de_asset_id id = de_asset_id_from(names[i]);
        EXPECT_NE(de_asset_font(f->assets, id).id, 0u);
        f->loads[i] = de_asset_load(f->assets, id, 0);
        ASSERT_EQ(de_asset_wait(f->assets, f->loads[i], 120ULL * 1000000000), DE_LOAD_READY);
    }
}

TEST_F(golden_assets, failed_cook_cleans_mount, pack_fixture) {
    EXPECT_NE(de_golden_pack_assets(f->assets, "goldenroot:/missing-source-root", f->pack), DE_OK);
    EXPECT_NE(f->pack[0], 0); /* Failure happened after the temporary mount was created. */
}

TEST(golden_assets, source_mode_unchanged) {
    de_asset_system *assets = NULL;
    de_text_system *text = NULL;
    ASSERT_EQ(de_fs_mount("goldenroot", DE_GOLDEN_TEST_ROOT, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(
        de_asset_system_create(&(de_asset_system_desc){.mode = DE_ASSET_MODE_SOURCE}, &assets),
        DE_OK);
    ASSERT_EQ(de_text_create(NULL, assets, &text), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(assets, "goldenroot:/tests/golden/text/assets.toml"), DE_OK);
    char path[256] = {0};
    EXPECT_EQ(de_golden_pack_assets(assets, "goldenroot:/tests/data/fonts", path), DE_OK);
    EXPECT_EQ(path[0], 0);
    de_asset_id id = de_asset_id_from("font/latin");
    ASSERT_NE(de_asset_font(assets, id).id, 0u);
    de_load_handle load = de_asset_load(assets, id, 0);
#if DE_HAS_COOKER
    const de_load_state expected = DE_LOAD_READY;
#else
    const de_load_state expected = DE_LOAD_FAILED;
#endif
    /* Linking test-side tools must not turn on source cooking in a packed runtime. */
    EXPECT_EQ(de_asset_wait(assets, load, 120ULL * 1000000000), expected);
    de_asset_release(assets, load);
    de_text_destroy(text);
    de_asset_system_destroy(assets);
    ASSERT_EQ(de_fs_unmount("goldenroot", DE_GOLDEN_TEST_ROOT), DE_OK);
}
