/*
 * engine_importers.c: the cooker library's table of built-in importers (DENG-409): the engine
 * asset types whose importers are compiled into de_cooker. See de_cooker_builtin_importer in
 * tools/cook/lib/de_cooker.h.
 *
 * It sits next to the importers, not in tools/cook/lib/, because it names them: the library's own
 * files never include tools/cook/importers/ (tools/check_layers). Add a row when an importer of an
 * engine type joins de_cooker (sound S25, font, ...).
 */
#include "cook/lib/de_cooker.h"
#include "text/import/font_import.h"
#include "text/loc/string_table_format.h"
#include "cook/importers/sound/sound_import.h"

#include "cook/importers/material/mat_toml_import.h"
#include "cook/importers/model/model_import.h"
#include "cook/importers/scene/scene_import.h"
#include "cook/importers/texture/texture_import.h"
#include "cook/importers/sprite_clip/sprite_clip_import.h"
#include "cook/importers/aseprite/aseprite_import.h"
#include "cook/importers/tilemap/tilemap_import.h"
#if DE_COOKER_HAS_SHADER_IMPORTER /* tools/cook/importers/shader/CMakeLists.txt */
#include "cook/importers/shader/shader_importer.h"
#endif

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include <string.h>

static de_asset_type_desc model_desc(void) {
    de_asset_type_desc t;
    memset(&t, 0, sizeof t);
    t.name = "model";
    t.extensions = de_model_import_extensions;
    t.extension_count = DE_MODEL_IMPORT_EXTENSION_COUNT;
    t.importer_version = DE_MODEL_IMPORTER_VERSION;
    t.import = de_model_import;
    return t;
}

#if DE_COOKER_HAS_SHADER_IMPORTER
static de_asset_type_desc shader_desc(void) {
    de_asset_type_desc t;
    memset(&t, 0, sizeof t);
    t.name = "shader";
    t.extensions = de_shader_import_extensions;
    t.extension_count = DE_SHADER_IMPORT_EXTENSION_COUNT;
    t.importer_version = de_shader_importer_version();
    t.import = de_shader_import;
    return t;
}
#endif

static const struct {
    const char *name;
    de_asset_type_desc (*desc)(void);
} k_builtin[] = {
    {"scene", de_scene_import_type_desc},
    {"prefab", de_prefab_import_type_desc},
    {"texture", de_texture_import_type_desc},
    {"font", de_font_import_type_desc},
    {"string_table", de_string_table_import_type_desc},
    {"sound", de_sound_import_type_desc},
    {"model", model_desc},
    {"material", de_mat_toml_import_type_desc}, /* DENG-302: `.mat.toml` */
    {"sprite_clip", de_sprite_clip_import_type_desc},
    {"aseprite", de_aseprite_import_type_desc},
    {"tilemap", de_tilemap_import_type_desc},
#if DE_COOKER_HAS_SHADER_IMPORTER
    {"shader", shader_desc},
#endif
};

bool de_cooker_builtin_importer(const char *name, de_asset_type_desc *out) {
    if (out != NULL)
        memset(out, 0, sizeof *out);
    if (name == NULL || out == NULL)
        return false;
    for (size_t i = 0; i < sizeof k_builtin / sizeof k_builtin[0]; ++i) {
        if (strcmp(k_builtin[i].name, name) == 0) {
            *out = k_builtin[i].desc();
            return true;
        }
    }
    return false;
}
