/*
 * known_types.c: the asset type names `de-cook --check` accepts (DENG-145). Later sprints
 * register loaders and importers for them; S08's cooker importer registry replaces this table.
 * `--types a,b,c` adds names for one run.
 */
#include "check.h"

const char *const de_cook_known_types[] = {
    "texture", "model",   "material",   "shader", "sound", "font",   "aseprite",
    "tilemap", "strings", "stylesheet", "video",  "scene", "prefab", "blob",
};

const u32 de_cook_known_type_count =
    (u32)(sizeof de_cook_known_types / sizeof de_cook_known_types[0]);
