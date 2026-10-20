#ifndef DE_RUNTIME_TEST_UTIL_H
#define DE_RUNTIME_TEST_UTIL_H
#include "font_test_util.h"
#include "text/font_runtime_internal.h"
#include "asset/slots_internal.h"
#include "asset/hot_reload_internal.h"
#include "packed_test_util.h"
typedef struct runtime_fx {
    de_asset_system *assets;
    de_text_system *t;
    de_font latin, cjk, emoji;
    de_load_handle loads[3];
    char pack[256];
} runtime_fx;
static inline void runtime_fx_setup_with_types(runtime_fx *f, de_type_registry *types) {
    ASSERT_EQ(de_fs_mount("fontall", DE_FONT_DATA "/..", DE_MOUNT_READ_ONLY, 0), DE_OK);
    de_asset_system_desc desc = {.hot_reload = true, .types = types};
#if !DE_HAS_COOKER
    desc.mode = DE_ASSET_MODE_PACKED;
#endif
    ASSERT_EQ(de_asset_system_create(&desc, &f->assets), DE_OK);
    de__asset_reload_set_watching(f->assets, false);
    ASSERT_EQ(de_text_create(NULL, f->assets, &f->t), DE_OK);
    ASSERT_EQ(de_asset_manifest_load(f->assets, "fontall:/runtime-assets.toml"), DE_OK);
#if !DE_HAS_COOKER
    text_fixture_pack(f->assets, "fontall:/", f->pack);
#endif
    const char *names[] = {"font/latin", "font/cjk", "font/emoji"};
    de_font *handles[] = {&f->latin, &f->cjk, &f->emoji};
    for (u32 i = 0; i < 3; ++i) {
        de_asset_id id = de_asset_id_from(names[i]);
        *handles[i] = de_asset_font(f->assets, id);
        f->loads[i] = de_asset_load(f->assets, id, 0);
        de_load_state state = de_asset_wait(f->assets, f->loads[i], 60ULL * 1000000000);
        if (state != DE_LOAD_READY) {
            de_load_info info;
            de_asset_status(f->assets, f->loads[i], &info);
            fprintf(stderr, "%s: %s\n", names[i], info.first_error_msg);
        }
        ASSERT_EQ(state, DE_LOAD_READY);
    }
    de_font chain[] = {f->cjk, f->emoji};
    de_text_set_fallbacks(f->t, f->latin, chain, 2);
    de_text_update(f->t);
}
static inline void runtime_fx_setup(runtime_fx *f) {
    runtime_fx_setup_with_types(f, NULL);
}
static inline void runtime_fx_teardown(runtime_fx *f) {
    /* Localization requests and glyph jobs still refer to the asset system. */
    de_text_destroy(f->t);
    f->t = NULL;
    if (f->assets) {
        for (u32 i = 0; i < 3; ++i)
            if (f->loads[i].id)
                de_asset_release(f->assets, f->loads[i]);
        de_asset_system_destroy(f->assets);
        f->assets = NULL;
    }
    text_fixture_pack_cleanup(f->pack);
    de_fs_unmount("fontall", DE_FONT_DATA "/..");
}
#endif
