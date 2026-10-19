#ifndef DE_LOC_TEST_UTIL_H
#define DE_LOC_TEST_UTIL_H
#include "de_test.h"
#include "text/loc/loc_internal.h"
#include "text/loc/string_table_format.h"
#include "text/text_internal.h"
#include "asset/asset_internal.h"
#include "cook/lib/de_cooker.h"
#include <stdio.h>
#include <string.h>
typedef struct loc_fixture {
    char root[256];
    de_asset_system *assets;
    de_task_system *tasks;
    de__loc *loc;
} loc_fixture;
static inline void loc_write(const char *path, const char *text) {
    ASSERT_EQ(de_file_write_all_atomic(path, text, strlen(text)), DE_OK);
}
static inline void loc_file(loc_fixture *f, const char *locale, const char *text) {
    char path[512];
    snprintf(path, sizeof path, "%s/strings/main/%s.toml", f->root, locale);
    loc_write(path, text);
}
static inline void loc_setup(loc_fixture *f, const char *name) {
    static bool mounted;
    de_fp_env_init_thread();
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("loc264", DE_LOC_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        mounted = true;
    }
    memset(f, 0, sizeof *f);
    snprintf(f->root, sizeof f->root, "loc264:/%s", name);
    char path[512];
    snprintf(path, sizeof path, "%s/strings/main", f->root);
    ASSERT_EQ(de_dir_create(path), DE_OK);
    loc_file(f, "en",
             "[strings]\n\"menu.play\"=\"Play\"\n\"menu.quit\"=\"Quit\"\n\"items\"=\"{count, "
             "plural, one{# item} other{# items}}\"\n");
    loc_file(f, "pt", "[strings]\n\"menu.play\"=\"Jogar\"\n");
    loc_file(f, "pt-BR", "[strings]\n\"greeting\"=\"Olá\"\n");
    snprintf(path, sizeof path, "%s/assets.toml", f->root);
    loc_write(path, "[assets]\n\"strings/main\"={type=\"string_table\",source=\"strings/main\"}\n");
    de_task_system_desc td = {0};
    td.worker_count = 1;
    td.background_worker_count = 1;
    ASSERT_EQ(de_task_system_create(&td, &f->tasks), DE_OK);
    de_asset_system_desc d = {0};
    d.tasks = f->tasks;
    char cache[512];
    snprintf(cache, sizeof cache, "%s/cache", f->root);
    d.cache_dir = cache;
    ASSERT_EQ(de_asset_system_create(&d, &f->assets), DE_OK);
    ASSERT_EQ(de__loc_create(NULL, f->assets, &f->loc), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, path), DE_OK);
}
static inline bool loc_ready(loc_fixture *f, u64 generation) {
    u64 end = de_time_now_ns() + 10000000000ull;
    while (de__loc_generation(f->loc) < generation && de_time_now_ns() < end) {
        de_asset_system_update(f->assets, NULL);
        de__loc_update(f->loc);
        de_sleep_ns(1000000);
    }
    return de__loc_generation(f->loc) >= generation;
}
static inline void loc_destroy(loc_fixture *f) {
    de__loc_destroy(f->loc);
    de_asset_system_destroy(f->assets);
    de_task_system_destroy(f->tasks);
    de_io_shutdown();
}
static inline de_cook_result loc_cook(loc_fixture *f, de_cooker *c) {
    de_cook_input in = {0};
    in.asset = &f->assets->manifest->assets[0];
    in.asset_root = f->root;
    de_cook_result r = {0};
    ASSERT_EQ(de_cooker_cook(c, &in, &r), DE_OK);
    return r;
}
#endif
