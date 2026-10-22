#include "de_fuzz.h"
#include "anim/sprite_clip_format.h"
#include "cook/importers/sprite_clip/sprite_clip_import.h"
#include "cook/lib/cooker_internal.h"
#include <dengine/de_platform.h>
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 1024 * 1024)
        return 0;
    de_fp_env_init_thread();
    de__sprite_clip_view view;
    (void)de__sprite_clip_parse(data, size, &view, NULL);
    de_cooker *cooker = NULL;
    if (de_cooker_create(NULL, &cooker) != DE_OK)
        abort();
    de__cooker_type type = {.name = "sprite_clip", .import = de_sprite_clip_import};
    de_asset_import_ctx ctx;
    de__import_ctx_init(&ctx, cooker, &type);
    ctx.source = data;
    ctx.source_size = size;
    ctx.source_rel = "fuzz.sprite.toml";
    ctx.id_str = "fuzz/sprite";
    ctx.id = de_asset_id_from(ctx.id_str);
    if (de__cooker_run_import(&ctx) == DE_OK &&
        !de__sprite_clip_parse(ctx.out, ctx.out_size, &view, NULL))
        abort();
    de__import_ctx_release(&ctx);
    de_cooker_destroy(cooker);
    return 0;
}
