/*
 * de_cooker.h: the cooker library (SPEC §11.5, §11.6; DENG-209). Tools-internal: `de-cook`
 * (tools/cook/) and, in Debug and Dev builds, the runtime asset pipeline (src/asset/cook_stage.c,
 * DE_HAS_COOKER) include it as "cook/lib/de_cooker.h". Release and Shipping builds do not link it.
 *
 * A cooker holds a table of asset types with their importers (de_cooker_register_type, which
 * mirrors de_asset_type_register) and turns one manifest asset into cooked bytes:
 *
 *   de_cooker *c = NULL;
 *   DE_TRY(de_cooker_create(&(de_cooker_desc){.platform = DE_COOK_PLATFORM_LINUX}, &c));
 *   de_cooker_register_type(c, &my_type_desc);
 *   de_cook_result r;
 *   DE_TRY(de_cooker_cook(c, &(de_cook_input){.asset = a, .asset_root = "cook:/assets"}, &r));
 *   ... r.data, r.dependencies, r.subassets, r.extra_sources, r.settings_hash ...
 *   de_cook_result_free(&r);
 *
 * Determinism (SPEC §11.5): the same inputs (source bytes, files read through
 * de_asset_import_read_file, settings, importer version, platform) give byte-identical outputs.
 * Importers must not read clocks, the environment, absolute paths, pointer values or
 * uninitialised padding, and must not depend on the thread count or on the thread they run on.
 * With `verify_determinism` the cooker runs every import twice and fails with DE_ERR_INTERNAL when
 * the two outputs differ.
 *
 * Threads: register types from one thread at a time; de_cooker_cook may run on any number of
 * threads at once, also while types are registered (the type table is published with release
 * stores). The allocator must be thread-safe when cooks run in parallel.
 */
#ifndef DE_COOKER_H
#define DE_COOKER_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_task.h>

#include "asset/manifest_internal.h"

DE_EXTERN_C_BEGIN

/* ---- Engine asset types ---------------------------------------------------------------------- */

/**
 * The asset type names the engine defines (SPEC §11.2, §11.6). The modules that own them register
 * their importers in later sprints (texture and model S11, shader S09, sound S25, ...); until
 * then a manifest may name them and `de-cook --check` accepts them (tools/cook/known_types.c
 * builds its table from this list). Append new names at the end.
 */
#define DE_COOKER_ENGINE_TYPE_NAMES                                                                \
    "texture", "model", "material", "shader", "sound", "font", "aseprite", "tilemap", "strings",   \
        "stylesheet", "video", "scene", "prefab", "blob"

/** The engine type names (DE_COOKER_ENGINE_TYPE_NAMES) and their count in `*count`. */
const char *const *de_cooker_engine_type_names(u32 *count);
/** True if `name` is one of the engine type names. */
bool de_cooker_is_engine_type(const char *name);

/** "windows", "linux", "macos"; "host" for DE_COOK_PLATFORM_HOST; "unknown" otherwise. */
const char *de_cook_platform_name(de_cook_platform p);
/** The platform this program was built for (never DE_COOK_PLATFORM_HOST). */
de_cook_platform de_cook_platform_host(void);

/* ---- Cooker ---------------------------------------------------------------------------------- */

typedef struct de_cooker de_cooker;

/** Whether de_cooker_cook runs each import twice and compares the outputs. */
typedef enum de_cook_verify {
    DE_COOK_VERIFY_DEFAULT = 0, /**< on in Debug builds (DE_DEBUG), off otherwise */
    DE_COOK_VERIFY_ON,
    DE_COOK_VERIFY_OFF
} de_cook_verify;

/** `(de_cooker_desc){0}` is valid: the default heap, no tasks, the host platform. */
typedef struct de_cooker_desc {
    const de_allocator *allocator; /**< NULL = de_heap_allocator(); thread-safe if cooks run in
                                        parallel */
    de_task_system *tasks;         /**< kept for parallel cooks (DENG-213); may be NULL */
    de_cook_platform platform;     /**< the target; DE_COOK_PLATFORM_HOST = this program's */
    const char *cache_dir;         /**< `.decache` directory (DENG-210); copied; NULL = none */
    de_cook_verify verify_determinism;
} de_cooker_desc;

/**
 * Creates a cooker with an empty type table.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`out` NULL, platform out of range),
 *         DE_ERR_OUT_OF_MEMORY.
 */
de_result de_cooker_create(const de_cooker_desc *desc, de_cooker **out);
/** Destroys a cooker. No cook may be running. NULL is ignored. */
void de_cooker_destroy(de_cooker *c);

/** The platform cooks target (resolved: never DE_COOK_PLATFORM_HOST). */
de_cook_platform de_cooker_platform(const de_cooker *c);
/** True when the cooker runs each import twice (the desc's verify_determinism, resolved). */
bool de_cooker_verifies(const de_cooker *c);
/** The cache directory of the desc, or NULL. */
const char *de_cooker_cache_dir(const de_cooker *c);

/* ---- Importer registry (importer_registry.c) ------------------------------------------------- */

/**
 * Registers an asset type, mirroring de_asset_type_register: the cooker copies `name` (1 to 63
 * printable ASCII bytes, no space or quote), `importer_version`, `import` and `user`. A type with
 * no `import` is cooked by copying its source bytes unchanged. At most 256 types. One thread at a
 * time.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL argument, bad name), DE_ERR_ALREADY_EXISTS (a type
 *         with this name), DE_ERR_OUT_OF_MEMORY (256 types).
 */
de_result de_cooker_register_type(de_cooker *c, const de_asset_type_desc *desc);
/** True if a type `name` is registered. Any thread. */
bool de_cooker_has_type(const de_cooker *c, const char *name);
/**
 * Writes the registered type names, in registration order, to `out` (at most `max`; NULL with
 * `max` 0 asks for the count) and returns the number of registered types. Any thread.
 */
u32 de_cooker_type_names(const de_cooker *c, const char **out, u32 max);
/** The importer version of type `name`, or 0 when it is not registered. Any thread. */
u32 de_cooker_importer_version(const de_cooker *c, const char *name);

/* ---- Cooking (cooker.c) ---------------------------------------------------------------------- */

/** One asset to cook. */
typedef struct de_cook_input {
    /** The manifest entry: `id`, `type`, `source` and the effective settings are read. */
    const de__manifest_asset *asset;
    /** The asset root as a virtual directory (`cook:/assets`; `asset_root` next to the root
     * manifest). Sources and files read by the importer must stay below it. */
    const char *asset_root;
    /** The source to import, relative to `asset_root`; NULL = `asset->source` (a locale
     * override passes its own). */
    const char *source;
} de_cook_input;

/** A sub-asset an importer declared (de_asset_import_add_subasset). */
typedef struct de_cook_subasset {
    const char *name; /**< the part after `#` */
    const char *type; /**< the asset type of its bytes */
    de_asset_id id;   /**< de_asset_id_from("<asset id>#<name>") */
    const void *data; /**< never NULL ("" when empty) */
    u64 size;
} de_cook_subasset;

/**
 * What a cook produced. Every pointer stays valid until de_cook_result_free. Lists keep the order
 * in which the importer declared their items (no duplicates).
 */
typedef struct de_cook_result {
    const void *data; /**< the cooked bytes (de_asset_import_write); never NULL */
    u64 size;
    const de_asset_id *dependencies; /**< de_asset_import_add_dependency */
    u32 dependency_count;
    const de_cook_subasset *subassets;
    u32 subasset_count;
    /** Files read through de_asset_import_read_file, relative to `asset_root` (normalised). The
     * source itself is not listed. */
    const char *const *extra_sources;
    u32 extra_source_count;
    /** The source path that was imported, relative to `asset_root` (normalised). */
    const char *source;
    u64 settings_hash;    /**< de_cook_settings_hash of the asset's settings */
    u32 importer_version; /**< of the type */
    de_cook_platform platform;
    /** XXH3-64 over every output (bytes, dependencies, sub-assets, extra sources): what
     * verify_determinism compares. */
    u64 output_hash;

    /* Private. */
    const de_allocator *alloc_;
    void *block_;
    size_t block_size_;
} de_cook_result;

/**
 * Cooks one asset: reads `asset_root/source`, runs the importer of the asset's type (twice with
 * verify_determinism) and collects its outputs into `*out`. On failure `*out` is zeroed and the
 * error context names the asset.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL argument, a source path that is not valid or
 *         leaves `asset_root`); DE_ERR_NOT_FOUND (the type is not registered, or the source is
 *         missing); DE_ERR_INTERNAL (verify_determinism found two different outputs); the
 *         importer's own error; DE_ERR_IO, DE_ERR_OUT_OF_MEMORY.
 */
de_result de_cooker_cook(de_cooker *c, const de_cook_input *in, de_cook_result *out);
/** Frees what a successful cook returned and zeroes `*r`. A zeroed result is ignored. */
void de_cook_result_free(de_cook_result *r);
/** The sub-asset `name` of a result, or NULL. */
const de_cook_subasset *de_cook_result_subasset(const de_cook_result *r, const char *name);

/* ---- Settings hash (settings_hash.c) --------------------------------------------------------- */

/**
 * The settings hash (part of the `.decache` key, DENG-210): XXH3-64 (seed 0) of a canonical
 * encoding of `settings`, so the order of the keys and the formatting of the TOML never change
 * it, and every value and type does (`1` and `1.0` differ). Encoding, little-endian:
 *
 *   "DESH" u32 version (1) u32 count
 *   per setting, keys sorted bytewise (strcmp):
 *     u32 key length, key bytes, u8 type tag (1 bool, 2 integer, 3 float, 4 string,
 *     5 string array), value:
 *       bool    u8 0 or 1
 *       integer i64
 *       float   u64 IEEE-754 bit pattern of the f64
 *       string  u32 length, bytes
 *       array   u32 count, then each item as a string, in order
 *
 * `settings` need not be sorted. Two settings with the same key hash as written (the manifest
 * never has duplicates). Allocation-free; any thread.
 */
u64 de_cook_settings_hash(const de__manifest_kv *settings, u32 count);

/* ---- Sub-assets cooked by another importer (import_subcook.c, DENG-101) ---------------------- */

/** The ID string of the asset an import context imports (`env/castle`); for building the IDs
 * of its sub-assets as text (`env/castle#Body`). "" for NULL. */
const char *de_cooker_import_id_str(const de_asset_import_ctx *ctx);

/**
 * Declares a sub-asset `<asset id>#<name>` of type `type_name` whose bytes come from running the
 * importer registered for `type_name` on `source` (instead of bytes the caller cooked itself).
 * The model importer cooks its textures (`texture`) and its prefab (`prefab`, TOML) this way.
 * Call it from inside an import, on the import's thread.
 *
 * - The nested import gets `source`, the `settings` given here (only these; the manifest's
 *   `[defaults.<type>]` do not apply), the ID `<asset id>#<name>`, and the outer import's source
 *   path and directory: its de_asset_import_read_file reads relative to the outer source, and the
 *   files it reads become extra sources of the outer cook.
 * - Its dependencies are not recorded (a cook result has no per-sub-asset dependency list): the
 *   sub-asset's loader declares what it needs (the scene loader does). Its own sub-assets are
 *   refused (DE_ERR_UNSUPPORTED).
 * - When `type_name` is not registered with the cooker, or is registered without an importer,
 *   the sub-asset holds `source` unchanged (the cooker's pass-through rule) and `*cooked` is false.
 *   Note: the outer asset's cook-cache key does not cover the nested type's registration or
 *   importer version; bump the outer importer's version when that matters.
 * - A failure of the nested import fails the outer import too (de_asset_import_fail with the
 *   nested message) and is returned.
 *
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL argument; a name de_asset_import_add_subasset
 *         refuses); DE_ERR_ALREADY_EXISTS (the name was declared); DE_ERR_UNSUPPORTED (the nested
 *         import declared sub-assets); the nested importer's error; DE_ERR_OUT_OF_MEMORY.
 */
de_result de_cooker_import_cook_subasset(de_asset_import_ctx *ctx, const char *name,
                                         const char *type_name, const void *source, u64 size,
                                         const de__manifest_kv *settings, u32 setting_count,
                                         bool *cooked);

DE_EXTERN_C_END

#endif /* DE_COOKER_H */
