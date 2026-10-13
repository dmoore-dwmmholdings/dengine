/*
 * fuzz_scene_cook.c: the cook part of the scene fuzz harness (DENG-403; tools builds). See
 * fuzz_scene_harness.h.
 *
 * A cooker holds the asset system's own `scene` and `prefab` type descs (importer included), as
 * de-cook copies them (cc_setup_runtime), and runs one import in memory through a cooker import
 * context, as DENG-106's fuzz_import_harness.c does for models: no file system, no cache. Where
 * de_asset has no cooker (Release with tools), the types have no importer and nothing here is
 * built (DE_HAS_COOKER is 0).
 */
#include "fuzz_scene_harness.h"

#if DE_HAS_COOKER

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_memory.h>
#include <dengine/de_reflect.h>

#include "asset/slots_internal.h"
#include "cook/lib/cooker_internal.h"
#include "cook/lib/de_cooker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One cook runtime: a cooker with the scene and prefab types of an asset system over `types`. */
typedef struct fz_cook_rt {
    de_cooker *cooker;
    de_type_registry *types;
    de_world *world; /* registered the ECS built-ins into `types` */
    de_asset_system *assets;
} fz_cook_rt;

static fz_cook_rt g_rt[2];

static void fz_copy_type(fz_cook_rt *rt, const char *name) {
    const de_asset_type_desc *td =
        de__asset_type_get(rt->assets, de__asset_type_find(rt->assets, name));
    if (td == NULL || td->import == NULL || de_cooker_register_type(rt->cooker, td) != DE_OK)
        fz_scene_fail("scene_toml", "setup: the scene types did not copy into the cooker");
}

/* The registry of FZ_SCENE_REG_HEALTH_V1: the harness types with THealth version 1 { hp }. */
typedef struct THealthV1 {
    f32 hp;
} THealthV1;
static const de_field_info k_health_v1_fields[] = {
    DE_FIELD(THealthV1, hp, DE_FK_F32, DE_FIELD_DEFAULT),
};

static fz_cook_rt *fz_rt(fz_scene_reg reg) {
    fz_cook_rt *rt = &g_rt[reg == FZ_SCENE_REG_HEALTH_V1 ? 1 : 0];
    if (rt->cooker != NULL)
        return rt;
    de_log_set_level(de_log_register_category("cook", DE_LOG_INFO), DE_LOG_FATAL);
    if (reg == FZ_SCENE_REG_CURRENT) {
        rt->types = fz_scene_types();
        rt->assets = fz_scene_assets();
    } else {
        /* Copy every struct type of the current registry but THealth, which is version 1 here.
         * Only the corpus generator cooks with it, so it is built at most once per process. */
        if (de_type_registry_create(NULL, &rt->types) != DE_OK)
            fz_scene_fail("scene_toml", "setup: the v1 registry");
        static const de_enum_value mood[] = {{"calm", 0}, {"angry", 1}, {"afraid", -2}};
        static const de_enum_value layers[] = {{"ground", 1}, {"water", 2}, {"air", 4}};
        if (de_enum_register(rt->types, "TMood", mood, 3, false) != DE_OK ||
            de_enum_register(rt->types, "TLayers", layers, 3, true) != DE_OK)
            fz_scene_fail("scene_toml", "setup: the v1 enums");
        const de_type_registry *cur = fz_scene_types();
        static const char *const copy[] = {"TItem", "TTarget", "TMisc", "TMarker"};
        for (u32 i = 0; i < 4u; ++i) {
            const de_type_desc *d = de_type_get(cur, de_type_find(cur, copy[i]));
            if (d == NULL || !DE_HANDLE_VALID(de_type_register(rt->types, d)))
                fz_scene_fail("scene_toml", "setup: a v1 type");
        }
        de_type_desc h = DE_TYPE_DESC(THealthV1, 1, k_health_v1_fields);
        h.name = "THealth";
        if (!DE_HANDLE_VALID(de_type_register(rt->types, &h)))
            fz_scene_fail("scene_toml", "setup: THealth v1");
        de_world_desc wd;
        memset(&wd, 0, sizeof wd);
        wd.types = rt->types;
        if (de_world_create(&wd, &rt->world) != DE_OK ||
            !DE_HANDLE_VALID(de_builtin_components(rt->world).transform))
            fz_scene_fail("scene_toml", "setup: the v1 world");
        de_asset_system_desc ad;
        memset(&ad, 0, sizeof ad);
        ad.types = rt->types;
        ad.io_threads = 1;
        ad.cache_dir = "fuzzmem:/decache";
        if (de_asset_system_create(&ad, &rt->assets) != DE_OK)
            fz_scene_fail("scene_toml", "setup: the v1 asset system");
    }
    de_cooker_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.verify_determinism = DE_COOK_VERIFY_OFF;
    if (de_cooker_create(&cd, &rt->cooker) != DE_OK)
        fz_scene_fail("scene_toml", "setup: the cooker");
    fz_copy_type(rt, DE__SCENE_TYPE_NAME);
    fz_copy_type(rt, DE__PREFAB_TYPE_NAME);
    return rt;
}

de_result fz_scene_cook(const u8 *src, size_t size, bool prefab, fz_scene_reg reg,
                        fz_scene_cooked *out, char *msg, size_t msg_size) {
    memset(out, 0, sizeof *out);
    if (msg != NULL && msg_size != 0)
        msg[0] = '\0';
    fz_cook_rt *rt = fz_rt(reg);
    const de__cooker_type *type =
        de__cooker_find_type(rt->cooker, prefab ? DE__PREFAB_TYPE_NAME : DE__SCENE_TYPE_NAME);
    if (type == NULL)
        fz_scene_fail("scene_toml", "the cooker lost a type");

    /* The cooker hands importers the source with a NUL after it (de_file_read_all); so does this
     * copy, and nothing more, so ASan sees a read past it. */
    u8 *source = (u8 *)malloc(size + 1u);
    if (source == NULL)
        abort();
    if (size != 0)
        memcpy(source, src, size);
    source[size] = 0;

    de_asset_import_ctx ctx;
    de__import_ctx_init(&ctx, rt->cooker, type);
    ctx.id_str = prefab ? "fuzz/prefab" : "fuzz/scene";
    ctx.id = de_asset_id_from(ctx.id_str);
    ctx.asset_root = "fuzzmem:/";
    ctx.source_rel = prefab ? "fuzz.prefab.toml" : "fuzz.scene.toml";
    ctx.source_dir = "fuzzmem:/";
    ctx.source = source;
    ctx.source_size = size;
    de_result r = de__cooker_run_import(&ctx);
    if (r == DE_OK) {
        if (ctx.sub_count != 0 || ctx.extra_count != 0)
            fz_scene_fail("scene_toml", "the scene importer declared a sub-asset or a source");
        out->size = ctx.out_size;
        out->data = (u8 *)malloc(ctx.out_size != 0 ? (size_t)ctx.out_size : 1u);
        out->deps = (u64 *)malloc(sizeof(u64) * (ctx.dep_count != 0 ? ctx.dep_count : 1u));
        if (out->data == NULL || out->deps == NULL)
            abort();
        if (ctx.out_size != 0)
            memcpy(out->data, ctx.out, (size_t)ctx.out_size);
        for (u32 i = 0; i < ctx.dep_count; ++i)
            out->deps[i] = ctx.deps[i].hash;
        out->dep_count = ctx.dep_count;
    } else if (msg != NULL && msg_size != 0) {
        snprintf(msg, msg_size, "%s", ctx.msg);
    }
    de__import_ctx_release(&ctx);
    free(source);
    de_error_clear();
    return r;
}

void fz_scene_cooked_free(fz_scene_cooked *c) {
    free(c->data);
    free(c->deps);
    memset(c, 0, sizeof *c);
}

#else
typedef int fz_scene_cook_unused; /* ISO C: a translation unit needs a declaration */
#endif /* DE_HAS_COOKER */
