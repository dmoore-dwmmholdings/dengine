/*
 * cook_test_util.h: helpers shared by the cooker tests (DENG-209): a scratch directory of the
 * build tree mounted as ckt:/ (DE_COOK_TEST_TMP_DIR), files written into it, hand-made manifest
 * entries, and the test importers.
 */
#ifndef DE_COOK_TEST_UTIL_H
#define DE_COOK_TEST_UTIL_H

#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/manifest_internal.h"
#include "cook/lib/de_cooker.h"

#include <stdio.h>
#include <string.h>

/** Mounts the scratch directory as ckt:/ once per process. */
static inline void ckt_mount(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("ckt", DE_COOK_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
    mounted = true;
}

/** Writes `text` to ckt:/<path>, creating the directories on the way. */
static inline void ckt_put(const char *path, const char *text) {
    ckt_mount();
    char vpath[512];
    snprintf(vpath, sizeof vpath, "ckt:/%s", path);
    for (char *p = strchr(vpath + 5, '/'); p != NULL; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (!de_path_exists(vpath))
            ASSERT_EQ(de_dir_create(vpath), DE_OK);
        *p = '/';
    }
    ASSERT_EQ(de_file_write_all_atomic(vpath, text, strlen(text)), DE_OK);
}

/** A manifest entry made by hand (settings must outlive it). */
static inline de__manifest_asset ckt_asset(const char *id, const char *type, const char *source,
                                           const de__manifest_kv *settings, u32 setting_count) {
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.type = type;
    a.source = source;
    a.file = "ckt:/assets.toml";
    a.settings = settings;
    a.setting_count = setting_count;
    return a;
}

/* ---- The `upper` test importer --------------------------------------------------------------
 * Writes the source upper-cased. Setting `dep` (string): declares that asset as a dependency.
 * Setting `sub` (bool, default true): declares the sub-asset `lower` (type `txt`) with the source
 * bytes as they are. Setting `read` (string array): reads those files (relative to the source)
 * and appends them, upper-cased too. */
static inline de_result ckt_upper_import(de_asset_import_ctx *ctx) {
    u64 n = 0;
    const char *src = (const char *)de_asset_import_source(ctx, &n);
    const de_allocator *a = de_asset_import_allocator(ctx);
    char *buf = (char *)DE_ALLOC(a, (size_t)n + 1u, 1, DE_MEM_SCRATCH);
    if (buf == NULL)
        return de_asset_import_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no scratch");
    for (u64 i = 0; i < n; ++i)
        buf[i] = (char)(src[i] >= 'a' && src[i] <= 'z' ? src[i] - 'a' + 'A' : src[i]);
    DE_TRY(de_asset_import_write(ctx, buf, n));
    /* Left allocated on purpose: the cooker frees the scratch blocks. */
    const char *const *reads = NULL;
    u32 rc = de_asset_import_setting_string_array(ctx, "read", &reads);
    for (u32 i = 0; i < rc; ++i) {
        const void *d = NULL;
        u64 dn = 0;
        DE_TRY(de_asset_import_read_file(ctx, reads[i], &d, &dn));
        for (u64 k = 0; k < dn; ++k) {
            char ch = ((const char *)d)[k];
            ch = (char)(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
            DE_TRY(de_asset_import_write(ctx, &ch, 1));
        }
    }
    const char *dep = de_asset_import_setting_string(ctx, "dep", NULL);
    if (dep != NULL)
        DE_TRY(de_asset_import_add_dependency(ctx, de_asset_id_from(dep)));
    if (de_asset_import_setting_bool(ctx, "sub", true))
        DE_TRY(de_asset_import_add_subasset(ctx, "lower", "txt", src, n));
    return DE_OK;
}

static inline de_asset_type_desc ckt_upper_type(void *user) {
    de_asset_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "upper";
    d.importer_version = 7;
    d.import = ckt_upper_import;
    d.user = user;
    return d;
}

#endif /* DE_COOK_TEST_UTIL_H */
