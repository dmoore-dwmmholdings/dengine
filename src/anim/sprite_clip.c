#include "anim_internal.h"
#include "asset/asset_internal.h"
#include "asset/slots_internal.h"
static de_result load_sprite(de_asset_load_ctx *ctx) {
    const de_allocator *a = de_asset_load_ctx_user(ctx);
    u64 bytes;
    const void *data = de_asset_load_ctx_data(ctx, &bytes);
    de__sprite_clip_view view;
    const char *why;
    if (!de__sprite_clip_parse(data, bytes, &view, &why))
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "sprite clip: %s", why);
    size_t payload = ((size_t)bytes + 7) & ~(size_t)7;
    size_t total = sizeof(de__anim_asset) + payload + ((size_t)view.frame_count + 1) * sizeof(u64);
    de__anim_asset *asset = DE_ALLOC(a, total, 16, DE_MEM_ASSET);
    if (!asset)
        return DE_ERR_OUT_OF_MEMORY;
    memset(asset, 0, sizeof *asset);
    asset->allocator = *a;
    asset->bytes = total;
    asset->kind = 3;
    memcpy(asset + 1, data, (size_t)bytes);
    (void)de__sprite_clip_parse(asset + 1, bytes, &asset->sprite, NULL);
    asset->sprite_prefix = (u64 *)((u8 *)(asset + 1) + payload);
    asset->sprite_prefix[0] = 0;
    for (u32 i = 0; i < view.frame_count; ++i)
        asset->sprite_prefix[i + 1] =
            asset->sprite_prefix[i] + de__sprite_frame_at(&asset->sprite, i).ms;
    de_result r = de_asset_load_ctx_add_dependency(ctx, (de_asset_id){view.atlas});
    if (r != DE_OK) {
        DE_FREE(a, asset, total);
        return r;
    }
    de_asset_load_ctx_set_object(ctx, asset, total);
    return DE_OK;
}
static void unload_sprite(void *object, void *user) {
    (void)user;
    de__anim_asset *a = object;
    if (a) {
        de_allocator allocator = a->allocator;
        DE_FREE(&allocator, a, a->bytes);
    }
}
de_result de__sprite_register_asset(de_anim_system *s) {
    if (de__asset_type_find(s->assets, "sprite_clip").id)
        return DE_OK;
    de_asset_type_desc d = {.name = "sprite_clip"};
    (void)de__asset_builtin_importer("sprite_clip", &d);
    /* Runtime manifests name the type explicitly; their registry permits single suffixes only. */
    d.extensions = NULL;
    d.extension_count = 0;
    d.load = load_sprite;
    d.unload = unload_sprite;
    d.user = (void *)s->alloc;
    if (!de_asset_type_register(s->assets, &d).id)
        return DE_FAIL(DE_ERR_INTERNAL, "sprite clip registration");
    return DE_OK;
}
