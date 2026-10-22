#include "anim_internal.h"
#include "asset/slots_internal.h"
#include <string.h>
static de_result load(de_asset_load_ctx *ctx, u32 kind) {
    const de_allocator *a = de_asset_load_ctx_user(ctx);
    u64 bytes;
    const void *data = de_asset_load_ctx_data(ctx, &bytes);
    if (bytes > SIZE_MAX - sizeof(de__anim_asset))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "animation asset size");
    size_t size = sizeof(de__anim_asset) + (size_t)bytes;
    de__anim_asset *object = DE_ALLOC(a, size, 16, DE_MEM_ASSET);
    if (!object)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "animation asset");
    memset(object, 0, sizeof *object);
    object->allocator = *a;
    object->bytes = size;
    object->kind = kind;
    memcpy(object + 1, data, (size_t)bytes);
    const char *why = NULL;
    bool valid = kind == 1 ? de_cooked_skeleton_parse(object + 1, bytes, &object->skeleton, &why)
                           : de_cooked_clip_parse(object + 1, bytes, &object->clip, &why);
    if (!valid) {
        DE_FREE(a, object, size);
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "animation: %s",
                                      why ? why : "invalid cooked blob");
    }
    de_asset_load_ctx_set_object(ctx, object, size);
    return DE_OK;
}
static de_result load_skeleton(de_asset_load_ctx *ctx) {
    return load(ctx, 1);
}
static de_result load_clip(de_asset_load_ctx *ctx) {
    return load(ctx, 2);
}
static void unload(void *object, void *user) {
    (void)user;
    de__anim_asset *asset = object;
    if (asset) {
        de_allocator a = asset->allocator;
        DE_FREE(&a, asset, asset->bytes);
    }
}
de_result de__anim_register_assets(de_anim_system *s) {
    if (!s->assets)
        return DE_OK;
    if (!de__asset_type_find(s->assets, "skeleton").id &&
        !de_asset_type_register(s->assets, &(de_asset_type_desc){.name = "skeleton",
                                                                 .load = load_skeleton,
                                                                 .unload = unload,
                                                                 .user = (void *)s->alloc})
             .id)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "skeleton asset registration");
    if (!de__asset_type_find(s->assets, "anim_clip").id &&
        !de_asset_type_register(s->assets, &(de_asset_type_desc){.name = "anim_clip",
                                                                 .load = load_clip,
                                                                 .unload = unload,
                                                                 .user = (void *)s->alloc})
             .id)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "clip asset registration");
    return de__sprite_register_asset(s);
}
const de__anim_asset *de__anim_asset_get(de_anim_system *s, u32 handle, const char *type) {
    if (!s->assets || !handle)
        return NULL;
    const de__slot *slot = de__slot_get(s->assets, handle);
    if (!slot || slot->state != DE_LOAD_READY || slot->type_hash != de_hash64_str(type))
        return NULL;
    return de__slot_object(slot);
}
de_skeleton de_anim_skeleton(de_anim_system *s, de_asset_id id) {
    return (de_skeleton){s ? de__asset_handle_of(s->assets, id, "skeleton") : 0};
}
de_anim_clip de_anim_clip_asset(de_anim_system *s, de_asset_id id) {
    u32 h = s && s->assets ? de__asset_handle_of(s->assets, id, NULL) : 0;
    const de__slot *slot = h ? de__slot_get(s->assets, h) : NULL;
    if (!slot || (slot->type_hash != de_hash64_str("anim_clip") &&
                  slot->type_hash != de_hash64_str("sprite_clip")))
        return (de_anim_clip){0};
    return (de_anim_clip){h};
}
de_anim_clip_kind de_anim_clip_kind_get(de_anim_system *s, de_anim_clip clip) {
    const de__slot *slot = s && s->assets ? de__slot_get(s->assets, clip.id) : NULL;
    if (!slot)
        return DE_ANIM_CLIP_NONE;
    if (slot->type_hash == de_hash64_str("anim_clip"))
        return DE_ANIM_CLIP_SKELETAL;
    if (slot->type_hash == de_hash64_str("sprite_clip"))
        return DE_ANIM_CLIP_SPRITE;
    return DE_ANIM_CLIP_NONE;
}
