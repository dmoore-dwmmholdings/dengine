#include "asset/cook_preview_internal.h"
#include "asset/asset_internal.h"
#include <dengine/de_platform.h>
#include <stdio.h>
#include <string.h>

#if DE_HAS_COOKER
#include "cook/lib/de_cooker.h"
typedef struct preview_owner {
    const de_allocator *alloc;
    de_cooker *cooker;
    de_cook_result results[2];
} preview_owner;

static const de__manifest_value *find(const de__manifest_kv *items, u32 count, const char *key) {
    for (u32 i = 0; i < count; ++i)
        if (!strcmp(items[i].key, key))
            return &items[i].value;
    return NULL;
}
static bool reserved(const char *key) {
    return !strcmp(key, "source") || !strcmp(key, "type") || !strcmp(key, "locale") ||
           !strcmp(key, "variants");
}
static de_result proposed(const de__manifest *manifest, const de__manifest_asset *saved,
                          const de__manifest_kv *entry, u32 count, de__manifest_asset *out,
                          de__manifest_kv settings[512], de__manifest_locale locales[64]) {
    if (count > 256 || (count && !entry))
        return DE_ERR_INVALID_ARGUMENT;
    for (u32 i = 0; i < count; ++i) {
        if (!entry[i].key || !entry[i].key[0])
            return DE_ERR_INVALID_ARGUMENT;
        for (u32 j = 0; j < i; ++j)
            if (!strcmp(entry[i].key, entry[j].key))
                return DE_ERR_INVALID_ARGUMENT;
    }
    *out = *saved;
    out->settings = settings;
    out->setting_count = 0;
    const de__manifest_defaults *defaults = de__manifest_find_defaults(manifest, saved->type);
    if (defaults) {
        if (defaults->setting_count > 256)
            return DE_ERR_UNSUPPORTED;
        for (u32 i = 0; i < defaults->setting_count; ++i)
            if (!find(entry, count, defaults->settings[i].key))
                settings[out->setting_count++] = defaults->settings[i];
    }
    for (u32 i = 0; i < count; ++i)
        if (!reserved(entry[i].key))
            settings[out->setting_count++] = entry[i];
    /* Same sorted representation as the ordinary immutable manifest. */
    for (u32 i = 1; i < out->setting_count; ++i) {
        const de__manifest_kv value = settings[i];
        u32 j = i;
        while (j && strcmp(settings[j - 1].key, value.key) > 0) {
            settings[j] = settings[j - 1];
            --j;
        }
        settings[j] = value;
    }
    const char *names[] = {"source", "type"};
    const char *values[] = {saved->source, saved->type};
    for (u32 i = 0; i < 2; ++i) {
        const de__manifest_value *value = find(entry, count, names[i]);
        if (value &&
            (value->type != DE__MANIFEST_STRING || !value->u.s || strcmp(value->u.s, values[i])))
            return DE_ERR_INVALID_ARGUMENT;
    }
    out->locales = locales;
    out->locale_count = 0;
    const de__manifest_value *locale = find(entry, count, "locale");
    if (locale) {
        if (locale->type != DE__MANIFEST_TABLE || locale->u.table.count > 64)
            return DE_ERR_INVALID_ARGUMENT;
        for (u32 i = 0; i < locale->u.table.count; ++i) {
            if (locale->u.table.items[i].type != DE__MANIFEST_STRING)
                return DE_ERR_INVALID_ARGUMENT;
            locales[out->locale_count++] =
                (de__manifest_locale){locale->u.table.keys[i], locale->u.table.items[i].u.s};
        }
    }
    return DE_OK;
}
static de_result describe(const de_cook_result *result, de__cook_preview_side *side) {
    *side = (de__cook_preview_side){.data = result->data,
                                    .size = result->size,
                                    .total_bytes = result->size,
                                    .child_count = result->subasset_count,
                                    .private_ = result};
    for (u32 i = 0; i < result->subasset_count; ++i) {
        if (result->subassets[i].size > UINT64_MAX - side->total_bytes)
            return DE_ERR_UNSUPPORTED;
        side->total_bytes += result->subassets[i].size;
    }
    return DE_OK;
}
de_result de__asset_cook_preview(de_asset_system *snapshot, const char *id,
                                 const de__manifest_kv *entry, u32 count, de_cook_platform platform,
                                 de__cook_preview *out) {
    if (!snapshot || !snapshot->manifest || !id || !out ||
        snapshot->desc.mode == DE_ASSET_MODE_PACKED)
        return DE_ERR_INVALID_ARGUMENT;
    const de__manifest *manifest = snapshot->manifest;
    const de__manifest_asset *saved = de__manifest_find_asset(manifest, id);
    if (!saved)
        return DE_ERR_NOT_FOUND;
    if (strcmp(saved->type, "model") && strcmp(saved->type, "texture") &&
        strcmp(saved->type, "sound"))
        return DE_ERR_UNSUPPORTED;
    de__manifest_asset after;
    de__manifest_kv settings[512];
    de__manifest_locale locales[64];
    DE_TRY(proposed(manifest, saved, entry, count, &after, settings, locales));
    char joined[DE_FS_PATH_MAX], root[DE_FS_PATH_MAX];
    const char *slash = strrchr(manifest->root, '/');
    const int directory = slash ? (int)(slash - manifest->root) : 0;
    const int n =
        snprintf(joined, sizeof joined, "%.*s/%s", directory, manifest->root, manifest->asset_root);
    if (n < 0 || (size_t)n >= sizeof joined)
        return DE_ERR_UNSUPPORTED;
    DE_TRY(de_path_normalize(joined, root, sizeof root));
    preview_owner *owner = DE_NEW(snapshot->alloc, preview_owner, DE_MEM_ASSET);
    if (!owner)
        return DE_ERR_OUT_OF_MEMORY;
    memset(owner, 0, sizeof *owner);
    owner->alloc = snapshot->alloc;
    de__cook_preview preview = {.private_ = owner};
    de_result result = de_cooker_create(&(de_cooker_desc){.allocator = owner->alloc,
                                                          .platform = platform,
                                                          .verify_determinism = DE_COOK_VERIFY_OFF},
                                        &owner->cooker);
    u32 type_count = 0;
    const char *const *types = de_cooker_engine_type_names(&type_count);
    for (u32 i = 0; result == DE_OK && i < type_count; ++i) {
        de_asset_type_desc type;
        if (!de_cooker_builtin_importer(types[i], &type))
            continue;
        if (!strcmp(type.name, "prefab") || !strcmp(type.name, "scene"))
            type.user = snapshot;
        result = de_cooker_register_type(owner->cooker, &type);
    }
    const de__manifest_asset *assets[] = {saved, &after};
    for (u32 i = 0; result == DE_OK && i < 2; ++i) {
        const de_cook_input input = {
            .asset = assets[i],
            .asset_root = root,
            .source = de__manifest_source_for(manifest, assets[i], de__asset_locale(snapshot))};
        result = de_cooker_cook(owner->cooker, &input, &owner->results[i]);
    }
    if (result == DE_OK)
        result = describe(&owner->results[0], &preview.before);
    if (result == DE_OK)
        result = describe(&owner->results[1], &preview.after);
    if (result != DE_OK) {
        de__asset_cook_preview_free(&preview);
        return result;
    }
    *out = preview;
    return DE_OK;
}
bool de__asset_cook_preview_child(const de__cook_preview_side *side, u32 index,
                                  de__cook_preview_child *out) {
    if (!side || !side->private_ || !out || index >= side->child_count)
        return false;
    const de_cook_result *result = side->private_;
    const de_cook_subasset *child = &result->subassets[index];
    *out = (de__cook_preview_child){child->name, child->type, child->data, child->size};
    return true;
}
void de__asset_cook_preview_free(de__cook_preview *preview) {
    if (!preview || !preview->private_)
        return;
    preview_owner *owner = preview->private_;
    de_cook_result_free(&owner->results[0]);
    de_cook_result_free(&owner->results[1]);
    de_cooker_destroy(owner->cooker);
    DE_FREE(owner->alloc, owner, sizeof *owner);
    memset(preview, 0, sizeof *preview);
}
#else
de_result de__asset_cook_preview(de_asset_system *snapshot, const char *id,
                                 const de__manifest_kv *entry, u32 count, de_cook_platform platform,
                                 de__cook_preview *out) {
    (void)snapshot;
    (void)id;
    (void)entry;
    (void)count;
    (void)platform;
    (void)out;
    return DE_ERR_UNSUPPORTED;
}
bool de__asset_cook_preview_child(const de__cook_preview_side *side, u32 index,
                                  de__cook_preview_child *out) {
    (void)side;
    (void)index;
    (void)out;
    return false;
}
void de__asset_cook_preview_free(de__cook_preview *preview) {
    (void)preview;
}
#endif
