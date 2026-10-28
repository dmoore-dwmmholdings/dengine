/*
 * tilemap_import.c: the cooker glue of the tilemap importer (DENG-80): settings, external files
 * through de_asset_import_read_file, warnings to the log, the cooked bytes and the dependencies.
 * The work is de__tm_import (tilemap_ir.c).
 */
#include "cook/importers/tilemap/tilemap_import.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>

#include <string.h>

static const char *const k_extensions[] = {".ldtk", ".tmj", ".tmx"};

static de_result read_file(void *user, const char *rel, const void **data, u64 *size) {
    return de_asset_import_read_file((de_asset_import_ctx *)user, rel, data, size);
}

static void warn(void *user, const char *msg) {
    (void)user;
    /* Registering returns the existing category; warnings are rare, so no cached static. */
    de_log_category cat = de_log_register_category("tilemap", DE_LOG_INFO);
    DE_WARN(cat, "%s", msg);
}

de_result de_tilemap_import(de_asset_import_ctx *ctx) {
    u64 size = 0;
    const void *src = de_asset_import_source(ctx, &size);
    de__tm_import_desc d;
    memset(&d, 0, sizeof d);
    d.allocator = de_asset_import_allocator(ctx);
    d.source = src;
    d.size = size;
    d.source_path = de_asset_import_source_path(ctx);
    d.read = read_file;
    d.read_user = ctx;
    d.warn = warn;
    d.pixels_per_unit = de_asset_import_setting_float(ctx, "pixels_per_unit", 0.0);
    d.tileset_count = de_asset_import_setting_string_array(ctx, "tilesets", &d.tilesets);
    d.entity_count = de_asset_import_setting_string_array(ctx, "entities", &d.entities);
    d.solid_layer_count =
        de_asset_import_setting_string_array(ctx, "solid_layers", &d.solid_layers);
    de__tm_import_result r;
    de_result res = de__tm_import(&d, &r);
    if (res != DE_OK)
        return de_asset_import_fail(ctx, res, "%s", r.message);
    res = de_asset_import_write(ctx, r.data, r.size);
    de__tm_import_free(d.allocator, &r);
    if (res != DE_OK)
        return de_asset_import_fail(ctx, res, "tilemap: writing the cooked map");
    /* Dependencies are declared by the runtime loader (it skips textures when no renderer
     * registered the `texture` type), so a headless host can load maps. */
    return DE_OK;
}

de_asset_type_desc de_tilemap_import_type_desc(void) {
    de_asset_type_desc t;
    memset(&t, 0, sizeof t);
    t.name = "tilemap";
    t.extensions = k_extensions;
    t.extension_count = (u32)(sizeof k_extensions / sizeof k_extensions[0]);
    t.importer_version = DE_TILEMAP_IMPORTER_VERSION;
    t.import = de_tilemap_import;
    return t;
}
