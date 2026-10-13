/*
 * cook_project_types.h: the types of the fixture project tests/data/cook_project/ (DENG-213): the
 * `CookSpin` component and the `cooktext` asset type. Two places register them with
 * cct_register_types: the in-process `de-cook cook` and `de-pack` tests through de_cook_hooks
 * (cook_project_util.h), and the game library of the `--game` tests (fixtures/cook_game.c,
 * DENG-394) from its register_types. Both get the same importer and the same component layout,
 * so both cook the project into the same bytes.
 *
 * No test framework here: the game library includes it too. The importer exists only where the
 * cooker is linked (DE_HAS_COOKER: Debug and Dev).
 *
 * `cooktext` imports text upper-cased, declares the sub-asset `lower` (type `cooktext`, the text
 * lower-cased) and, with the setting `dep = "<asset id>"`, a dependency. `fail = true` makes the
 * import fail. Its loader keeps a copy of the bytes (a cct_text followed by them).
 */
#ifndef DE_COOK_PROJECT_TYPES_H
#define DE_COOK_PROJECT_TYPES_H

#include <stdlib.h>
#include <string.h>

#include <dengine/de_asset.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#ifndef DE_HAS_COOKER
#define DE_HAS_COOKER 0
#endif

typedef struct CookSpin {
    f32 speed;
    u32 icon; /* a de_blob handle */
} CookSpin;

static const de_field_info k_cookspin_fields[] = {
    DE_FIELD(CookSpin, speed, DE_FK_F32, DE_FIELD_DEFAULT),
    DE_FIELD_EX(CookSpin, icon, DE_FK_ASSET, DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF,
                {0, 0, 0, 0, "blob", 0}),
};

#if DE_HAS_COOKER
static inline de_result cct_cooktext_import(de_asset_import_ctx *ctx) {
    if (de_asset_import_setting_bool(ctx, "fail", false))
        return de_asset_import_fail(ctx, DE_ERR_PARSE, "cooktext: asked to fail");
    u64 n = 0;
    const char *src = (const char *)de_asset_import_source(ctx, &n);
    const de_allocator *a = de_asset_import_allocator(ctx);
    char *up = (char *)DE_ALLOC(a, (size_t)n + 1u, 1, DE_MEM_SCRATCH);
    char *low = (char *)DE_ALLOC(a, (size_t)n + 1u, 1, DE_MEM_SCRATCH);
    if (up == NULL || low == NULL)
        return de_asset_import_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no scratch");
    for (u64 i = 0; i < n; ++i) {
        char c = src[i];
        up[i] = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        low[i] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    DE_TRY(de_asset_import_write(ctx, up, n));
    DE_TRY(de_asset_import_add_subasset(ctx, "lower", "cooktext", low, n));
    const char *dep = de_asset_import_setting_string(ctx, "dep", NULL);
    if (dep != NULL)
        DE_TRY(de_asset_import_add_dependency(ctx, de_asset_id_from(dep)));
    return DE_OK;
}
#endif

typedef struct cct_text {
    u64 size;
} cct_text;

static inline de_result cct_cooktext_load(de_asset_load_ctx *ctx) {
    u64 n = 0;
    const void *src = de_asset_load_ctx_data(ctx, &n);
    cct_text *t = (cct_text *)malloc(sizeof *t + (size_t)n + 1u);
    if (t == NULL)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no memory");
    t->size = n;
    if (n != 0)
        memcpy(t + 1, src, (size_t)n);
    de_asset_load_ctx_set_object(ctx, t, n);
    return DE_OK;
}

static inline void cct_cooktext_unload(void *object, void *user) {
    (void)user;
    free(object);
}

/** Registers the fixture project's types: CookSpin into `types`, cooktext into `s`. */
static inline de_result cct_register_types(void *user, de_asset_system *s,
                                           de_type_registry *types) {
    (void)user;
    if (!DE_HANDLE_VALID(de_type_register(types, &DE_TYPE_DESC(CookSpin, 1, k_cookspin_fields))))
        return DE_ERR_INTERNAL;
    de_asset_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = "cooktext";
    d.importer_version = 1;
#if DE_HAS_COOKER
    d.import = cct_cooktext_import;
#endif
    d.load = cct_cooktext_load;
    d.unload = cct_cooktext_unload;
    return DE_HANDLE_VALID(de_asset_type_register(s, &d)) ? DE_OK : DE_ERR_INTERNAL;
}

#endif /* DE_COOK_PROJECT_TYPES_H */
