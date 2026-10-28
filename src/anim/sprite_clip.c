#include "anim_internal.h"
#include "asset/asset_internal.h"
#include "asset/slots_internal.h"
de_result de__sprite_asset_load(de_asset_load_ctx *ctx) {
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
void de__sprite_asset_unload(void *object, void *user) {
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
    d.load = de__sprite_asset_load;
    d.unload = de__sprite_asset_unload;
    d.user = (void *)s->alloc;
    if (!de_asset_type_register(s->assets, &d).id)
        return DE_FAIL(DE_ERR_INTERNAL, "sprite clip registration");
    return DE_OK;
}

static const de__anim_asset *sprite_asset(de_anim_system *s, de_anim_clip clip) {
    const de__slot *slot = s && s->assets ? de__slot_get(s->assets, clip.id) : NULL;
    if (!slot || slot->state != DE_LOAD_READY ||
        de_anim_clip_kind_get(s, clip) != DE_ANIM_CLIP_SPRITE)
        return NULL;
    const de__anim_asset *asset = de__slot_object(slot);
    return asset && asset->kind == 3 ? asset : NULL;
}
u32 de_anim_sprite_frame_count(de_anim_system *s, de_anim_clip clip) {
    const de__anim_asset *asset = sprite_asset(s, clip);
    return asset ? asset->sprite.frame_count : 0;
}
bool de_anim_sprite_frame(de_anim_system *s, de_anim_clip clip, u32 frame,
                          de_sprite_frame_info *out) {
    const de__anim_asset *asset = sprite_asset(s, clip);
    if (!asset || !out || frame >= asset->sprite.frame_count)
        return false;
    const de__sprite_clip_view *view = &asset->sprite;
    de__sprite_frame f = de__sprite_frame_at(view, frame);
    *out =
        (de_sprite_frame_info){.uv_rect = {f.rect[0], f.rect[1], f.rect[2], f.rect[3]},
                               .pivot = {f.pivot[0], f.pivot[1]},
                               .size = {f.size[0], f.size[1]},
                               .source_pivot = {f.source_pivot[0], f.source_pivot[1]},
                               .trim = {f.trim[0], f.trim[1], f.trim[2], f.trim[3]},
                               .canvas_width = view->canvas_width,
                               .canvas_height = view->canvas_height,
                               .duration = (f32)f.ms / 1000,
                               .has_geometry = view->version == DE__SPRITE_CLIP_GEOMETRY_VERSION};
    return true;
}
bool de_anim_sprite_slice(de_anim_system *s, de_anim_clip clip, u32 frame, de_name name,
                          de_sprite_slice_info *out) {
    const de__anim_asset *asset = sprite_asset(s, clip);
    if (!asset || !out || !name.hash || frame >= asset->sprite.frame_count)
        return false;
    const de__sprite_clip_view *v = &asset->sprite;
    u32 lo = 0, hi = v->slice_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        de__sprite_slice key = de__sprite_slice_at(v, mid);
        if (key.name < name.hash || (key.name == name.hash && key.frame <= frame))
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return false;
    de__sprite_slice key = de__sprite_slice_at(v, lo - 1);
    if (key.name != name.hash || !key.width || !key.height)
        return false;
    *out = (de_sprite_slice_info){.origin = {key.x, key.y},
                                  .width = key.width,
                                  .height = key.height,
                                  .center_origin = {key.center_x, key.center_y},
                                  .center_width = key.center_width,
                                  .center_height = key.center_height,
                                  .pivot = {key.pivot_x, key.pivot_y},
                                  .has_nine_slice = (key.flags & 1u) != 0,
                                  .has_pivot = (key.flags & 2u) != 0};
    if (out->has_nine_slice)
        out->nine_slice = (de_vec4){(f32)key.center_x, (f32)key.center_y,
                                    (f32)((i64)key.width - key.center_x - key.center_width),
                                    (f32)((i64)key.height - key.center_y - key.center_height)};
    return true;
}
