/*
 * import_subcook.c: de_cooker_import_cook_subasset (DENG-101): a sub-asset whose bytes come from
 * the importer registered for its type, run on in-memory source bytes inside the outer import.
 * The model importer cooks its texture and prefab sub-assets "through the registry" with it, so
 * the texture importer (DENG-98) and the prefab importer (DENG-215) stay the only code that knows
 * their formats. See de_cooker.h.
 */
#include "cooker_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <stdio.h>
#include <string.h>

/* Records `rel` as an extra source of `ctx` (deduplicated, in first-read order), as
 * de_asset_import_read_file does. */
static bool add_extra(de_asset_import_ctx *ctx, const char *rel) {
    for (u32 i = 0; i < ctx->extra_count; ++i)
        if (strcmp(ctx->extra[i], rel) == 0)
            return true;
    const de_allocator *a = ctx->c->alloc;
    if (ctx->extra_count == ctx->extra_cap) {
        u32 ncap = ctx->extra_cap != 0 ? ctx->extra_cap * 2u : 8u;
        char **n = (char **)DE_REALLOC(a, ctx->extra, sizeof(char *) * ctx->extra_cap,
                                       sizeof(char *) * ncap, DE_ALIGNOF(char *), DE_MEM_ASSET);
        if (n == NULL)
            return false;
        ctx->extra = n;
        ctx->extra_cap = ncap;
    }
    size_t len = strlen(rel) + 1u;
    char *copy = (char *)DE_ALLOC(a, len, 1, DE_MEM_ASSET);
    if (copy == NULL)
        return false;
    memcpy(copy, rel, len);
    ctx->extra[ctx->extra_count++] = copy;
    return true;
}

de_result de_cooker_import_cook_subasset(de_asset_import_ctx *ctx, const char *name,
                                         const char *type_name, const void *source, u64 size,
                                         const de__manifest_kv *settings, u32 setting_count,
                                         bool *cooked) {
    if (cooked != NULL)
        *cooked = false;
    if (ctx == NULL || name == NULL || type_name == NULL || (source == NULL && size != 0) ||
        (settings == NULL && setting_count != 0))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_cooker_import_cook_subasset: NULL argument");
    for (u32 i = 0; i < ctx->sub_count; ++i)
        if (strcmp(ctx->subs[i].name, name) == 0)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS, "asset \"%s\": sub-asset \"%s\" declared twice",
                           ctx->id_str, name);

    const de__cooker_type *type = de__cooker_find_type(ctx->c, type_name);
    if (type == NULL || type->import == NULL) /* pass-through: the bytes as they are */
        return de_asset_import_add_subasset(ctx, name, type_name, source, size);

    char id_str[DE_FS_PATH_MAX];
    int n = snprintf(id_str, sizeof id_str, "%s#%s", ctx->id_str, name);
    if (n < 0 || (size_t)n >= sizeof id_str || strchr(name, '#') != NULL || name[0] == '\0')
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "asset \"%s\": sub-asset name \"%s\" is not valid",
                       ctx->id_str, name);
    de_asset_id id = de_asset_id_from(id_str);
    if (id.hash == 0)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "asset \"%s\": sub-asset \"%s\": invalid ID",
                       ctx->id_str, name);

    de_asset_import_ctx sub;
    de__import_ctx_init(&sub, ctx->c, type);
    sub.id = id;
    sub.id_str = id_str;
    sub.asset_root = ctx->asset_root;
    sub.source_rel = ctx->source_rel;
    sub.source_dir = ctx->source_dir;
    sub.source = source;
    sub.source_size = size;
    sub.settings = settings;
    sub.setting_count = setting_count;

    de_result r = de__cooker_run_import(&sub);
    if (r != DE_OK) {
        de__import_ctx_fail(ctx, r, "sub-asset \"%s\" (type \"%s\"): %s", name, type_name, sub.msg);
        r = DE_FAIL(r, "asset \"%s\": sub-asset \"%s\" (type \"%s\"): %s", ctx->id_str, name,
                    type_name, sub.msg);
    } else if (sub.sub_count != 0) {
        de__import_ctx_fail(ctx, DE_ERR_UNSUPPORTED,
                            "sub-asset \"%s\" (type \"%s\"): its importer declared sub-assets of "
                            "its own, which a nested import cannot have",
                            name, type_name);
        r = DE_FAIL(DE_ERR_UNSUPPORTED, "asset \"%s\": sub-asset \"%s\": nested sub-assets",
                    ctx->id_str, name);
    }
    for (u32 i = 0; i < sub.extra_count && r == DE_OK; ++i)
        if (!add_extra(ctx, sub.extra[i])) {
            de__import_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "out of memory (extra source list)");
            r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset \"%s\": out of memory", ctx->id_str);
        }
    if (r == DE_OK)
        r = de_asset_import_add_subasset(ctx, name, type_name, sub.out, sub.out_size);
    if (r == DE_OK && cooked != NULL)
        *cooked = true;
    de__import_ctx_release(&sub);
    return r;
}

const char *de_cooker_import_id_str(const de_asset_import_ctx *ctx) {
    return ctx != NULL && ctx->id_str != NULL ? ctx->id_str : "";
}
