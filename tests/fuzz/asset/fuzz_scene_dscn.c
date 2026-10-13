/*
 * fuzz_scene_dscn.c: fuzz target `scene_dscn` (DENG-403, SPEC §19.1): hostile DSCN bytes into the
 * scene loader's format check, de__scene_view_parse (src/asset/scene/scene_type.c), and every
 * accepted input into de_prefab_instantiate (scene_instantiate.c).
 *
 * Each input is a whole cooked scene or prefab, as a loader gets it from the cook stage or a pack.
 *   1. de__scene_view_parse must return DE_OK, DE_ERR_PARSE or DE_ERR_VERSION_MISMATCH, with a
 *      message, and the same result for a second call (the check has no state).
 *   2. For an accepted input, fz_scene_check_view checks every promise of the parse: names inside
 *      the strings and NUL-terminated, entities found by scene ID, parents and component ranges in
 *      range, component data inside the data section, sorted asset references.
 *   3. Then the bytes are instantiated into a world with the fuzz component types (and the ECS
 *      built-ins), once without a parent and once under one: the component objects go through
 *      de_deserialize_binary with the world's types (stored schemas, migrate, entity and asset
 *      fields), the hierarchy through de_entity_set_parent. A failure must leave no entity behind
 *      (the world is destroyed afterwards; LeakSanitizer and ASan see the rest).
 *
 * Seed corpus corpus/scene_dscn/: the cooked DSCN of the corpus/scene_toml/ seeds, written and
 * checked by fuzz_scene_corpus.c (CTest scene_corpus.seeds), plus regress_* inputs of fixed bugs.
 */
#include "de_fuzz.h"
#include "fuzz_scene_harness.h"

#include <dengine/de_base.h>

#include "asset/scene/scene_format.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)fz_scene_assets(); /* setup first: it silences the log categories */
    de__scene_view v;
    char err[256];
    err[0] = '\0';
    de_result r = de__scene_view_parse(data, size, &v, err, sizeof err);
    if (r != DE_OK && r != DE_ERR_PARSE && r != DE_ERR_VERSION_MISMATCH)
        fz_scene_fail("scene_dscn", "parse returned an unexpected result");
    if (r != DE_OK && err[0] == '\0')
        fz_scene_fail("scene_dscn", "parse refused the bytes without a message");
    de__scene_view v2;
    if (de__scene_view_parse(data, size, &v2, NULL, 0) != r)
        fz_scene_fail("scene_dscn", "parse gave another result the second time");
    if (r != DE_OK)
        return 0;
    if (v.kind != v2.kind || v.type_count != v2.type_count || v.entity_count != v2.entity_count ||
        v.component_count != v2.component_count || v.asset_count != v2.asset_count ||
        v.root_count != v2.root_count || v.data != v2.data || v.data_bytes != v2.data_bytes)
        fz_scene_fail("scene_dscn", "parse gave another view the second time");
    fz_scene_check_view("scene_dscn", &v);
    (void)fz_scene_instantiate("scene_dscn", data, size);
    return 0;
}
