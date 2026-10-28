/*
 * tilemap_import.h: the `tilemap` importer (DENG-80, SPEC §21.4, §11.6): LDtk 1.x projects
 * (`.ldtk`, with external `.ldtkl` levels) and Tiled maps (`.tmj` JSON, `.tmx` XML, with external
 * `.tsj` / `.tsx` tilesets) to the cooked DTMP format (src/gameplay/tilemap/tilemap_cooked.h).
 * Cooker-library code (de_cooker): the tools, and Debug and Dev builds through
 * de_cooker_builtin_importer("tilemap").
 *
 * Settings (manifest keys of the asset):
 *   pixels_per_unit = 16        source pixels per map unit (meter); default: the map's tile width
 *                               (Tiled `tilewidth`, LDtk `defaultGridSize`), so one tile is 1 m
 *   tilesets = ["terrain=tex/terrain"]   tileset name -> texture asset ID. Without an entry the ID
 *                               is the tileset image's path below the asset root without its
 *                               extension (`../tiles/terrain.png` next to `maps/a.tmj` ->
 *                               `tiles/terrain`)
 *   entities = ["Player=prefabs/player"] LDtk entity identifier / Tiled object type (class) ->
 *                               prefab asset ID; objects of other types are kept unmapped
 *   solid_layers = ["ground"]   tile layers whose every tile collides (as the Tiled layer property
 *                               `collision = true`); LDtk IntGrid values other than 0 always do
 * Manifest settings cannot be tables, so the two maps are `"key=value"` string arrays.
 *
 * Spawn fields: a Tiled object property, or an LDtk entity field, becomes a field record whose
 * path is the property name (`THealth.current`); LDtk identifiers cannot contain `.`, so `__`
 * stands for it there (`THealth__current`). Booleans, integers, floats and strings are kept.
 *
 * The readers (tilemap_json.c, tilemap_xml.c) and the front ends allocate only from the import
 * allocator, bound their recursion, and fail with a message instead of crashing (fuzzed:
 * tests/fuzz/tilemap/).
 */
#ifndef DE_COOK_IMPORTERS_TILEMAP_IMPORT_H
#define DE_COOK_IMPORTERS_TILEMAP_IMPORT_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

DE_EXTERN_C_BEGIN

#define DE_TILEMAP_IMPORTER_VERSION 1u

/** Reads `rel_path` relative to the source file's directory (de_asset_import_read_file). */
typedef de_result (*de__tm_read_fn)(void *user, const char *rel_path, const void **data,
                                    u64 *size);
/** Called once per warning (the importer also counts them). */
typedef void (*de__tm_warn_fn)(void *user, const char *message);

/** The source format; AUTO picks by the source path's extension, then by the content. */
typedef enum de__tm_format {
    DE__TM_FORMAT_AUTO = 0,
    DE__TM_FORMAT_TMJ,
    DE__TM_FORMAT_TMX,
    DE__TM_FORMAT_LDTK
} de__tm_format;

/** One import, without the cooker (tests and fuzz targets call it directly). */
typedef struct de__tm_import_desc {
    const de_allocator *allocator; /**< temporaries and the output; NULL = the heap */
    const void *source;
    u64 size;
    /** The source path relative to the asset root (`maps/level01.tmj`); "" when unknown. */
    const char *source_path;
    de__tm_format format;
    de__tm_read_fn read; /**< NULL: external files cannot be read (fail) */
    void *read_user;
    de__tm_warn_fn warn; /**< nullable */
    void *warn_user;
    f64 pixels_per_unit; /**< 0 = the map's tile width */
    const char *const *tilesets;
    u32 tileset_count;
    const char *const *entities;
    u32 entity_count;
    const char *const *solid_layers;
    u32 solid_layer_count;
    /** Memory the import may use for temporaries (0 = 512 MiB); exceeding it fails. */
    u64 memory_limit;
} de__tm_import_desc;

/** What an import produced. `data` comes from desc->allocator; free with de__tm_import_free. */
typedef struct de__tm_import_result {
    void *data;
    u64 size;
    u32 warnings;
    /** Asset IDs the map references: tileset textures, then prefabs (deduplicated, in order). */
    u32 dependency_count;
    u64 dependencies[64];
    char message[256]; /**< the failure message ("" on success) */
} de__tm_import_result;

/**
 * Imports one map. DE_OK with the cooked bytes in `*out`, or a failure with `out->message`:
 * DE_ERR_PARSE (malformed JSON/XML, an unknown layer compression, inconsistent data),
 * DE_ERR_UNSUPPORTED (Tiled infinite maps, image-collection tilesets, non-orthogonal maps),
 * DE_ERR_NOT_FOUND / DE_ERR_IO (an external file), DE_ERR_OUT_OF_MEMORY (also the memory limit),
 * DE_ERR_INVALID_ARGUMENT (bad settings).
 */
de_result de__tm_import(const de__tm_import_desc *desc, de__tm_import_result *out);
/** Frees the bytes of a successful import. */
void de__tm_import_free(const de_allocator *a, de__tm_import_result *r);

/** The cooker entry point (de_asset_type_desc.import). */
de_result de_tilemap_import(de_asset_import_ctx *ctx);
/** The importer-only type desc of `tilemap` (engine_importers.c). */
de_asset_type_desc de_tilemap_import_type_desc(void);

DE_EXTERN_C_END

#endif /* DE_COOK_IMPORTERS_TILEMAP_IMPORT_H */
