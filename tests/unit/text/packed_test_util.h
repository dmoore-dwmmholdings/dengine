#ifndef DE_TEXT_PACKED_TEST_UTIL_H
#define DE_TEXT_PACKED_TEST_UTIL_H
#include "de_test.h"
#include "asset/asset_internal.h"
#include "cook/lib/de_cooker.h"
#include "dpak_writer.h"
#include <stdio.h>
#if defined(_WIN32)
#include <process.h>
#define text_test_pid _getpid
#else
#include <unistd.h>
#define text_test_pid getpid
#endif

/* Only test executables link the tools. Release/Shipping runtime still loads
 * normal cooked pack entries, with no in-process cooker or source-file bypass. */
static inline void text_fixture_pack(de_asset_system *assets, const char *asset_root,
                                     char path[256]) {
    static u32 serial;
    ASSERT_EQ(de_fs_mount("textpacked", DE_TEXT_PACK_TMP, DE_MOUNT_WRITABLE, 0), DE_OK);
    snprintf(path, 256, "textpacked:/%lu-%llu-%u.dpak", (unsigned long)text_test_pid(),
             (unsigned long long)de_time_now_ns(), serial++);
    de_cooker *cooker = NULL;
    ASSERT_EQ(de_cooker_create(NULL, &cooker), DE_OK);
    /* Model imports cook their prefab through the same reflected registry. */
    const char *scene_types[] = {"scene", "prefab"};
    for (u32 i = 0; i < 2; ++i) {
        de_asset_type_desc type;
        ASSERT_TRUE(de_cooker_builtin_importer(scene_types[i], &type));
        type.user = assets;
        ASSERT_EQ(de_cooker_register_type(cooker, &type), DE_OK);
    }
    dpak_writer *writer = NULL;
    ASSERT_EQ(
        dpak_writer_create(&(dpak_writer_desc){.include_names = true, .zstd_level = 1}, &writer),
        DE_OK);
    for (u32 i = 0; i < assets->manifest->asset_count; ++i) {
        const de__manifest_asset *asset = &assets->manifest->assets[i];
        if (!de_cooker_has_type(cooker, asset->type)) {
            de_asset_type_desc type;
            ASSERT_TRUE(de_cooker_builtin_importer(asset->type, &type));
            ASSERT_EQ(de_cooker_register_type(cooker, &type), DE_OK);
        }
        de_cook_result cooked = {0};
        ASSERT_EQ(de_cooker_cook(cooker, &(de_cook_input){.asset = asset, .asset_root = asset_root},
                                 &cooked),
                  DE_OK);
        u64 *deps = NULL;
        size_t dep_bytes = (size_t)cooked.dependency_count * sizeof(u64);
        if (dep_bytes) {
            deps = DE_ALLOC(de_heap_allocator(), dep_bytes, DE_ALIGNOF(u64), DE_MEM_ASSET);
            ASSERT_TRUE(deps != NULL);
            for (u32 j = 0; j < cooked.dependency_count; ++j)
                deps[j] = cooked.dependencies[j].hash;
        }
        ASSERT_EQ(dpak_writer_add(writer, de_asset_id_from(asset->id).hash, asset->type,
                                  cooked.data, cooked.size, deps, cooked.dependency_count,
                                  asset->id),
                  DE_OK);
        if (deps)
            DE_FREE(de_heap_allocator(), deps, dep_bytes);
        for (u32 j = 0; j < cooked.subasset_count; ++j) {
            const de_cook_subasset *sub = &cooked.subassets[j];
            char name[1024];
            int n = snprintf(name, sizeof name, "%s#%s", asset->id, sub->name);
            ASSERT_TRUE(n > 0 && (size_t)n < sizeof name);
            ASSERT_EQ(dpak_writer_add(writer, sub->id.hash, sub->type, sub->data, sub->size, NULL,
                                      0, name),
                      DE_OK);
        }
        de_cook_result_free(&cooked);
    }
    ASSERT_EQ(dpak_writer_finish(writer, path), DE_OK);
    dpak_writer_destroy(writer);
    de_cooker_destroy(cooker);
    ASSERT_EQ(de_asset_pack_mount(assets, path, 0), DE_OK);
}
/* Call after destroying the asset system, which releases the mapped pack file. */
static inline void text_fixture_pack_cleanup(char path[256]) {
    if (!path[0])
        return;
    ASSERT_EQ(de_file_delete(path), DE_OK);
    ASSERT_EQ(de_fs_unmount("textpacked", DE_TEXT_PACK_TMP), DE_OK);
    path[0] = 0;
}
#endif
