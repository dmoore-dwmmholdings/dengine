/*
 * known_types.c: the asset type names `de-cook --check` accepts (DENG-145). The table is the
 * cooker library's list of engine types (DE_COOKER_ENGINE_TYPE_NAMES in
 * tools/cook/lib/de_cooker.h, DENG-209), whose importers later sprints register. `--types a,b,c`
 * adds names for one run.
 */
#include "check.h"

#include "cook/lib/de_cooker.h"

const char *const de_cook_known_types[] = {DE_COOKER_ENGINE_TYPE_NAMES};

const u32 de_cook_known_type_count =
    (u32)(sizeof de_cook_known_types / sizeof de_cook_known_types[0]);
