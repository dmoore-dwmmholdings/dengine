#include "cooked_assets.h"
#include <dengine/de_platform.h>
#include <stdio.h>
#include <string.h>
#if defined(DE_GOLDEN_COOK)
#include "asset/asset_internal.h"
#include "asset/slots_internal.h"
#include "cook/lib/de_cooker.h"
#include "dpak_writer.h"
#if defined(_WIN32)
#include <process.h>
#define golden_pid _getpid
#else
#include <unistd.h>
#define golden_pid getpid
#endif

static de_result register_cook_types(de_cooker *cooker, de_asset_system *assets) {
    u32 count = 0;
    const char *const *names = de_cooker_engine_type_names(&count);
    for (u32 i = 0; i < count; ++i) {
        de_asset_type_desc type;
        if (de_cooker_builtin_importer(names[i], &type)) {
            /* Scene and glTF-prefab cooking use the runtime's reflected components. */
            if (strcmp(names[i], "scene") == 0 || strcmp(names[i], "prefab") == 0)
                type.user = assets;
        } else {
            /* Built-in blobs deliberately have no importer: cook their bytes unchanged. */
            const de_asset_type_desc *runtime =
                de__asset_type_get(assets, de__asset_type_find(assets, names[i]));
            if (!runtime)
                continue;
            type = *runtime;
        }
        DE_TRY(de_cooker_register_type(cooker, &type));
    }
    return DE_OK;
}

static de_result add_cooked(dpak_writer *writer, const de__manifest_asset *asset,
                            const de_cook_result *cooked) {
    u64 *deps = NULL;
    const size_t bytes = (size_t)cooked->dependency_count * sizeof *deps;
    if (bytes) {
        deps = DE_ALLOC(de_heap_allocator(), bytes, DE_ALIGNOF(u64), DE_MEM_ASSET);
        if (!deps)
            return DE_ERR_OUT_OF_MEMORY;
        for (u32 i = 0; i < cooked->dependency_count; ++i)
            deps[i] = cooked->dependencies[i].hash;
    }
    de_result r =
        dpak_writer_add(writer, de_asset_id_from(asset->id).hash, asset->type, cooked->data,
                        cooked->size, deps, cooked->dependency_count, asset->id);
    if (deps)
        DE_FREE(de_heap_allocator(), deps, bytes);
    for (u32 i = 0; r == DE_OK && i < cooked->subasset_count; ++i) {
        const de_cook_subasset *sub = &cooked->subassets[i];
        char name[1024];
        int n = snprintf(name, sizeof name, "%s#%s", asset->id, sub->name);
        if (n < 0 || (size_t)n >= sizeof name)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "golden subasset name exceeds %zu bytes",
                           sizeof name - 1u);
        r = dpak_writer_add(writer, sub->id.hash, sub->type, sub->data, sub->size, NULL, 0, name);
    }
    return r;
}
#endif

de_result de_golden_pack_assets(de_asset_system *assets, const char *asset_root, char path[256]) {
#if defined(DE_GOLDEN_COOK)
    if (assets->desc.mode == DE_ASSET_MODE_SOURCE)
        return DE_OK;
    static u32 serial;
    DE_TRY(de_fs_mount("goldenpacked", DE_GOLDEN_PACK_TMP, DE_MOUNT_WRITABLE, 0));
    snprintf(path, 256, "goldenpacked:/%lu-%llu-%u.dpak", (unsigned long)golden_pid(),
             (unsigned long long)de_time_now_ns(), serial++);
    de_cooker *cooker = NULL;
    dpak_writer *writer = NULL;
    de_result r = de_cooker_create(NULL, &cooker);
    if (r == DE_OK)
        r = register_cook_types(cooker, assets);
    if (r == DE_OK)
        r = dpak_writer_create(&(dpak_writer_desc){.include_names = true, .zstd_level = 1},
                               &writer);
    for (u32 i = 0; r == DE_OK && i < assets->manifest->asset_count; ++i) {
        const de__manifest_asset *asset = &assets->manifest->assets[i];
        if (!de_cooker_has_type(cooker, asset->type)) {
            de_asset_type_desc type;
            if (!de_cooker_builtin_importer(asset->type, &type)) {
                r = DE_ERR_UNSUPPORTED;
                break;
            }
            r = de_cooker_register_type(cooker, &type);
        }
        de_cook_result cooked = {0};
        if (r == DE_OK)
            r = de_cooker_cook(cooker, &(de_cook_input){.asset = asset, .asset_root = asset_root},
                               &cooked);
        if (r == DE_OK)
            r = add_cooked(writer, asset, &cooked);
        if (r != DE_OK)
            r = DE_FAIL_WRAP(r, "preparing golden asset %s", asset->id);
        de_cook_result_free(&cooked);
    }
    if (r == DE_OK)
        r = dpak_writer_finish(writer, path);
    dpak_writer_destroy(writer);
    de_cooker_destroy(cooker);
    if (r == DE_OK)
        r = de_asset_pack_mount(assets, path, 0);
    return r;
#else
    (void)assets;
    (void)asset_root;
    (void)path;
#if DE_HAS_COOKER
    return DE_OK;
#else
    return DE_ERR_UNSUPPORTED;
#endif
#endif
}

de_result de_golden_pack_cleanup(char path[256]) {
#if defined(DE_GOLDEN_COOK)
    if (path[0]) {
        de_result r = de_path_exists(path) ? de_file_delete(path) : DE_OK;
        const de_result unmount = de_fs_unmount("goldenpacked", DE_GOLDEN_PACK_TMP);
        path[0] = 0;
        return r == DE_OK ? unmount : r;
    }
#else
    (void)path;
#endif
    return DE_OK;
}
