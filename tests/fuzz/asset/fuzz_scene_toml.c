/*
 * fuzz_scene_toml.c: fuzz target `scene_toml` (DENG-403, SPEC §19.1; tools builds): hostile TOML
 * through the DENG-215 `scene` and `prefab` importers (src/asset/scene/scene_type.c), in memory
 * through a cooker import context (fuzz_scene_cook.c), as de-cook runs them.
 *
 * Each input is cooked as a scene and as a prefab. For every cook that succeeds:
 *   - the output parses with de__scene_view_parse, as the kind it was cooked as, and keeps every
 *     promise of the parse (fz_scene_check_view) and of the importer (fz_scene_check_cooked: every
 *     type is registered, every component object decodes into its type and uses all its bytes);
 *   - the cook dependencies are exactly the asset references of the output;
 *   - a second cook gives the same bytes (cooks are byte-identical, SPEC §11.5);
 *   - the output instantiates into a world (fz_scene_instantiate).
 * And between the two: the prefab cook succeeds exactly when the scene cook succeeds with one
 * root, and then the two outputs differ only in the kind field.
 *
 * Where de_asset has no cooker (Release with tools: DE_HAS_COOKER 0) the importers do not exist
 * and every input returns at once.
 *
 * Seed corpus corpus/scene_toml/: the DENG-215 unit fixtures (tests/unit/asset/data/scenes/assets/)
 * and hand-written scenes for the other field kinds, the tag and the built-in components. The
 * dictionary dict/scene_toml.dict holds the TOML syntax, the keys and the type and field names.
 */
#include "de_fuzz.h"
#include "fuzz_scene_harness.h"

#include <dengine/de_base.h>

#include "asset/scene/scene_format.h"

#include <string.h>

#if DE_HAS_COOKER
#define FZ "scene_toml"

/* Cooks `data` as `prefab` and checks the output; false when the importer refused it. */
static bool fz_cook_checked(const uint8_t *data, size_t size, bool prefab, fz_scene_cooked *out) {
    char msg[256];
    de_result r = fz_scene_cook(data, size, prefab, FZ_SCENE_REG_CURRENT, out, msg, sizeof msg);
    if (r != DE_OK) {
        if (msg[0] == '\0')
            fz_scene_fail(FZ, "the importer failed without a message");
        return false;
    }
    de__scene_view v;
    char err[256];
    err[0] = '\0';
    if (de__scene_view_parse(out->data, out->size, &v, err, sizeof err) != DE_OK) {
        fz_scene_fail(FZ, err);
    }
    if (v.kind != (prefab ? DE__SCENE_KIND_PREFAB : DE__SCENE_KIND_SCENE))
        fz_scene_fail(FZ, "the importer wrote another kind");
    fz_scene_check_view(FZ, &v);
    fz_scene_check_cooked(FZ, &v, fz_scene_types());
    if (out->dep_count != v.asset_count)
        fz_scene_fail(FZ, "the cook dependencies are not the asset references");
    for (u32 i = 0; i < v.asset_count; ++i)
        if (out->deps[i] != de__scene_asset_at(&v, i))
            fz_scene_fail(FZ, "the cook dependencies are not the asset references");

    fz_scene_cooked again;
    if (fz_scene_cook(data, size, prefab, FZ_SCENE_REG_CURRENT, &again, NULL, 0) != DE_OK)
        fz_scene_fail(FZ, "a second cook failed");
    if (again.size != out->size || memcmp(again.data, out->data, (size_t)out->size) != 0)
        fz_scene_fail(FZ, "a second cook gave other bytes");
    fz_scene_cooked_free(&again);

    (void)fz_scene_instantiate(FZ, out->data, (size_t)out->size);
    return true;
}
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
#if DE_HAS_COOKER
    fz_scene_cooked scene, prefab;
    bool scene_ok = fz_cook_checked(data, size, false, &scene);
    bool prefab_ok = fz_cook_checked(data, size, true, &prefab);
    if (prefab_ok && !scene_ok)
        fz_scene_fail(FZ, "the prefab importer took a file the scene importer refused");
    if (scene_ok) {
        u32 roots = de__scene_get_u32(scene.data + 40);
        if (prefab_ok != (roots == 1u))
            fz_scene_fail(FZ, "the prefab importer disagrees with the scene's root count");
    }
    if (scene_ok && prefab_ok) {
        if (scene.size != prefab.size || memcmp(scene.data, prefab.data, 8) != 0 ||
            memcmp(scene.data + 12, prefab.data + 12, (size_t)scene.size - 12u) != 0)
            fz_scene_fail(FZ, "the scene and prefab outputs differ beyond the kind");
    }
    fz_scene_cooked_free(&scene);
    fz_scene_cooked_free(&prefab);
#else
    (void)data;
    (void)size;
#endif
    return 0;
}
