#include "de_fuzz.h"
#include <dengine/de_text.h>
#include <dengine/de_asset.h>
#include <dengine/de_platform.h>
#include "text/text_internal.h"
#include <stdlib.h>
#include <string.h>
static de_asset_system *assets;
static de_font font;
static de_load_handle load;
static void cleanup(void) {
    if (assets) {
        de_asset_release(assets, load);
        de_asset_system_destroy(assets);
        assets = NULL;
    }
    de_fs_unmount("layoutfuzz", DE_LAYOUT_FONT_ROOT);
}
static void setup(void) {
    de_fp_env_init_thread();
    if (de_fs_mount("layoutfuzz", DE_LAYOUT_FONT_ROOT, DE_MOUNT_READ_ONLY, 0) != DE_OK)
        abort();
    if (de_asset_system_create(NULL, &assets) != DE_OK)
        abort();
    de_text_system *registration = NULL;
    if (de_text_create(NULL, assets, &registration) != DE_OK)
        abort();
    if (de_asset_manifest_load(assets, "layoutfuzz:/runtime-assets.toml") != DE_OK)
        abort();
    de_asset_id id = de_asset_id_from("font/latin");
    font = de_asset_font(assets, id);
    load = de_asset_load(assets, id, 0);
    if (de_asset_wait(assets, load, 120ULL * 1000000000) != DE_LOAD_READY)
        abort();
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
