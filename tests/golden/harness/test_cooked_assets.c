#include "de_test.h"
#include "cooked_assets.h"
#include "asset/dpak/dpak_reader.h"
#include "asset/scene/scene_format.h"
#include <dengine/de_platform.h>
#include <dengine/de_ecs.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_text.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pack_fixture {
    de_asset_system *assets;
    de_text_system *text;
    de_load_handle loads[2];
    dpak_reader *reader;
    de_type_registry *types;
    de_world *world;
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
    dpak_close(f->reader);
    for (u32 i = 0; i < 2; ++i)
        if (f->loads[i].id)
            de_asset_release(f->assets, f->loads[i]);
    de_text_destroy(f->text);
    de_asset_system_destroy(f->assets);
    de_world_destroy(f->world);
    de_type_registry_destroy(f->types);
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

typedef pack_fixture base_pack_fixture;
static void base_pack_fixture_setup(base_pack_fixture *f) {
    ASSERT_EQ(de_fs_mount("goldenroot", DE_GOLDEN_TEST_ROOT, DE_MOUNT_READ_ONLY, 0), DE_OK);
    ASSERT_EQ(de_type_registry_create(NULL, &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.types = f->types}, &f->world), DE_OK);
    ASSERT_EQ(de_render_register_components(f->world), DE_OK);
    ASSERT_EQ(
        de_asset_system_create(
            &(de_asset_system_desc){.mode = DE_ASSET_MODE_PACKED, .types = f->types}, &f->assets),
        DE_OK);
}
static void base_pack_fixture_teardown(base_pack_fixture *f) {
    pack_fixture_teardown(f);
}

TEST_F(golden_assets, packed_base_tier_subassets_scenes_and_bakes, base_pack_fixture) {
    const char *root = "goldenroot:/tests/golden/base_tier/assets";
    ASSERT_EQ(
        de_asset_manifest_load(f->assets, "goldenroot:/tests/golden/base_tier/assets/assets.toml"),
        DE_OK);
    ASSERT_EQ(de_golden_pack_assets(f->assets, root, f->pack), DE_OK);
    ASSERT_EQ(
        dpak_open(f->pack, &(dpak_open_desc){.verify = true, .check_chunks = true}, &f->reader),
        DE_OK);
    const char *names[] = {"mesh/box",
                           "mesh/box#box",
                           "mesh/sphere",
                           "mesh/sphere#sphere",
                           "mesh/card",
                           "mesh/card#card",
                           "mesh/cylinder",
                           "mesh/cylinder#cylinder",
                           "scenes/ao_gtao",
                           "scenes/reflections_ssr_probes",
                           "scenes/gi_probes_ssgi"};
    for (u32 i = 0; i < sizeof names / sizeof names[0]; ++i) {
        const u32 index = dpak_find(f->reader, de_asset_id_from(names[i]).hash);
        ASSERT_NE(index, DPAK_NOT_FOUND);
        dpak_toc_entry entry;
        dpak_entry(f->reader, index, &entry);
        const char *type = i < 8 ? (i % 2 == 0 ? "model" : "mesh") : "scene";
        EXPECT_EQ(entry.type_hash, de_asset_id_from(type).hash);
        EXPECT_GT(entry.raw_size, 8u);
        for (u32 dep = 0; dep < entry.dep_count; ++dep)
            EXPECT_NE(dpak_find(f->reader, dpak_dep(f->reader, entry.dep_first + dep)),
                      DPAK_NOT_FOUND);
    }
    u32 scenes = 0, prefabs = 0;
    for (u32 i = 0; i < dpak_entry_count(f->reader); ++i) {
        dpak_toc_entry entry;
        dpak_entry(f->reader, i, &entry);
        const bool prefab = entry.type_hash == de_asset_id_from("prefab").hash;
        if (!prefab && entry.type_hash != de_asset_id_from("scene").hash)
            continue;
        void *bytes = malloc((size_t)entry.raw_size);
        ASSERT_TRUE(bytes);
        const de_result read = dpak_read_entry(f->reader, i, NULL, bytes, entry.raw_size);
        EXPECT_EQ(read, DE_OK);
        if (read == DE_OK) {
            de__scene_view view;
            const de_result parsed = de__scene_view_parse(bytes, entry.raw_size, &view, NULL, 0);
            EXPECT_EQ(parsed, DE_OK);
            if (parsed == DE_OK) {
                EXPECT_EQ(view.kind, prefab ? DE__SCENE_KIND_PREFAB : DE__SCENE_KIND_SCENE);
                EXPECT_GT(view.entity_count, 0u);
                EXPECT_GT(view.component_count, 0u);
            }
        }
        free(bytes);
        if (prefab)
            ++prefabs;
        else
            ++scenes;
    }
    EXPECT_EQ(prefabs, 4u);
    EXPECT_EQ(scenes, 14u);
    const char *bakes[] = {"bake/reflection.deprobe", "bake/irradiance.degi"};
    const char *sources[] = {"goldenroot:/tests/golden/base_tier/assets/bakes/reflection.deprobe",
                             "goldenroot:/tests/golden/base_tier/assets/bakes/irradiance.degi"};
    for (u32 i = 0; i < sizeof bakes / sizeof bakes[0]; ++i) {
        const de_asset_id id = de_asset_id_from(bakes[i]);
        f->loads[i] = de_asset_load(f->assets, id, 0);
        ASSERT_EQ(de_asset_wait(f->assets, f->loads[i], 120ULL * 1000000000), DE_LOAD_READY);
        u64 size = 0;
        const void *data = de_blob_data(f->assets, de_asset_blob(f->assets, id), &size);
        ASSERT_TRUE(data);
        void *source = NULL;
        u64 source_size = 0;
        ASSERT_EQ(de_file_read_all(sources[i], de_heap_allocator(), &source, &source_size), DE_OK);
        EXPECT_EQ(size, source_size);
        EXPECT_GT(source_size, 1024u);
        if (size == source_size)
            EXPECT_EQ(memcmp(data, source, (size_t)size), 0);
        DE_FREE(de_heap_allocator(), source, (size_t)source_size);
    }
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
