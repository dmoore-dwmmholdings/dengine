#include "de_fuzz.h"
#include <dengine/de_text.h>
#include <dengine/de_asset.h>
#include <dengine/de_platform.h>
#include "text/text_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static de_asset_system *assets;
static de_font font;
static de_load_handle load;
#if !DE_HAS_COOKER
#include "asset/asset_internal.h"
#include "cook/lib/de_cooker.h"
#include "dpak_writer.h"
#if defined(_WIN32)
#include <process.h>
#define layout_pid _getpid
#else
#include <unistd.h>
#define layout_pid getpid
#endif
static char pack_path[256];
/* Tool libraries are fixture-owned. The tested runtime loads genuine cooked bytes. */
static de_result prepare_font_pack(void) {
    DE_TRY(de_fs_mount("layoutpacked", DE_LAYOUT_PACK_TMP, DE_MOUNT_WRITABLE, 0));
    snprintf(pack_path, sizeof pack_path, "layoutpacked:/%lu-%llu.dpak",
             (unsigned long)layout_pid(), (unsigned long long)de_time_now_ns());
    const de__manifest_asset *source = NULL;
    for (u32 i = 0; i < assets->manifest->asset_count; ++i)
        if (!strcmp(assets->manifest->assets[i].id, "font/latin"))
            source = &assets->manifest->assets[i];
    if (!source)
        return DE_FAIL(DE_ERR_NOT_FOUND, "layout fuzz font manifest entry");
    de_cooker *cooker = NULL;
    dpak_writer *writer = NULL;
    de_cook_result cooked = {0};
    DE_TRY(de_cooker_create(NULL, &cooker));
    de_asset_type_desc type;
    de_result r = de_cooker_builtin_importer(source->type, &type)
                      ? de_cooker_register_type(cooker, &type)
                      : DE_ERR_NOT_FOUND;
    if (r == DE_OK)
        r = de_cooker_cook(cooker, &(de_cook_input){.asset = source, .asset_root = "layoutfuzz:/"},
                           &cooked);
    if (r == DE_OK)
        r = dpak_writer_create(&(dpak_writer_desc){.include_names = true, .zstd_level = 1},
                               &writer);
    if (r == DE_OK)
        r = dpak_writer_add(writer, de_asset_id_from(source->id).hash, source->type, cooked.data,
                            cooked.size, NULL, 0, source->id);
    for (u32 i = 0; i < cooked.subasset_count && r == DE_OK; ++i) {
        const de_cook_subasset *sub = &cooked.subassets[i];
        char name[1024];
        int n = snprintf(name, sizeof name, "%s#%s", source->id, sub->name);
        r = n > 0 && (size_t)n < sizeof name ? dpak_writer_add(writer, sub->id.hash, sub->type,
                                                               sub->data, sub->size, NULL, 0, name)
                                             : DE_ERR_INVALID_ARGUMENT;
    }
    if (r == DE_OK)
        r = dpak_writer_finish(writer, pack_path);
    if (writer)
        dpak_writer_destroy(writer);
    de_cook_result_free(&cooked);
    de_cooker_destroy(cooker);
    DE_TRY(r);
    return de_asset_pack_mount(assets, pack_path, 0);
}
#endif
static void cleanup(void) {
    if (assets) {
        de_asset_release(assets, load);
        de_asset_system_destroy(assets);
        assets = NULL;
    }
#if !DE_HAS_COOKER
    /* Close the asset system's mapped file before removing its backing pack. */
    if (de_file_delete(pack_path) != DE_OK ||
        de_fs_unmount("layoutpacked", DE_LAYOUT_PACK_TMP) != DE_OK)
        abort();
#endif
    de_fs_unmount("layoutfuzz", DE_LAYOUT_FONT_ROOT);
}
static void setup(void) {
    de_fp_env_init_thread();
    if (de_fs_mount("layoutfuzz", DE_LAYOUT_FONT_ROOT, DE_MOUNT_READ_ONLY, 0) != DE_OK)
        abort();
    de_asset_system_desc desc = {0};
#if !DE_HAS_COOKER
    desc.mode = DE_ASSET_MODE_PACKED;
#endif
    if (de_asset_system_create(&desc, &assets) != DE_OK)
        abort();
    de_text_system *registration = NULL;
    if (de_text_create(NULL, assets, &registration) != DE_OK)
        abort();
    if (de_asset_manifest_load(assets, "layoutfuzz:/runtime-assets.toml") != DE_OK)
        abort();
#if !DE_HAS_COOKER
    if (prepare_font_pack() != DE_OK)
        abort();
#endif
    de_asset_id id = de_asset_id_from("font/latin");
    font = de_asset_font(assets, id);
    load = de_asset_load(assets, id, 0);
    if (de_asset_wait(assets, load, 120ULL * 1000000000) != DE_LOAD_READY) {
        de_load_info info = {0};
        de_asset_status(assets, load, &info);
        fprintf(stderr, "layout fuzz font setup: %s\n", info.first_error_msg);
        abort();
    }
    de_text_destroy(registration);
    atexit(cleanup);
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 3 || size > 1027)
        return 0;
    if (!assets)
        setup();
    /* Fresh text state per input: cache/background glyph work cannot leak across cases.
     * Loader registration belongs to the persistent read-only asset fixture. */
    de_text_system *text = NULL;
    if (de_text_create(NULL, NULL, &text) != DE_OK)
        abort();
    text->assets = assets;
    de_text_style style = {.font = font,
                           .size_px = (f32)(8 + data[0] % 89),
                           .color = {1, 1, 1, 1},
                           .letter_spacing = (f32)(data[0] % 5) * .1f,
                           .line_height = 1};
    de_text_layout_desc desc = {.utf8 = {(const char *)data + 3, size - 3},
                                .style = &style,
                                .max_width = data[1] % 3 ? ((f32)data[1] * 4) : 0,
                                .direction = (de_text_dir)(data[2] % 3),
                                .align = (de_text_align)((data[2] >> 2) % 4),
                                .pixel_snap = (data[2] & 32) != 0};
    de_text_layout *layout = NULL, *repeat = NULL;
    de_result result = de_text_layout_build(text, &desc, NULL, &layout);
    if (result == DE_OK) {
        de_glyph_quad quads[32];
        (void)de_text_layout_quads(layout, quads, 32);
        de_vec2 point = {(f32)data[0], (f32)data[1]};
        u32 offset = de_text_layout_hit_test(layout, point);
        if (offset > size - 3)
            abort();
        (void)de_text_layout_caret(layout, offset);
        if (de_text_layout_build(text, &desc, NULL, &repeat) != DE_OK)
            abort();
        de_vec2 a = de_text_layout_size(layout), b = de_text_layout_size(repeat);
        if (a.x != b.x || a.y != b.y)
            abort();
    }
    de_text_layout_free(repeat);
    de_text_layout_free(layout);
    de_text_destroy(text);
    return 0;
}
