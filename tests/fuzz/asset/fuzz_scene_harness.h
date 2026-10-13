/*
 * fuzz_scene_harness.h: the shared setup of the scene fuzz targets (DENG-403, SPEC §19.1):
 * fuzz_scene_dscn.c (hostile DSCN bytes into de__scene_view_parse, then de_prefab_instantiate) and
 * fuzz_scene_toml.c (hostile TOML through the DENG-215 `scene` and `prefab` importers), and the
 * corpus check fuzz_scene_corpus.c.
 *
 * The runtime part (fuzz_scene_harness.c, every build): one type registry with the fuzz component
 * types below and the ECS built-ins (a world registers them, as de-cook does), and one asset
 * system whose built-in `scene` and `prefab` types hold the loader and, with DE_HAS_COOKER, the
 * importer. A slot for the blob "ui/heart" exists, so asset fields of the fixtures map to a handle.
 * All of it is built lazily on the first input (the standalone runner has no
 * LLVMFuzzerInitialize) and never destroyed; it is reachable from statics, so LeakSanitizer does
 * not report it. Each instantiation gets a world of its own, destroyed before the call returns.
 *
 * The cook part (fuzz_scene_cook.c, tools builds): a cooker holding the asset system's `scene` and
 * `prefab` types, as de-cook sets it up, and the same again over a registry where THealth is
 * version 1 ({ hp }), which only the corpus generator uses (a seed for the migrate path).
 *
 * Fuzz component types (the names of the DENG-215 unit fixtures, so their TOML cooks unchanged):
 *   THealth  { f32 current, max }, version 2, migrate hook from version 1 { f32 hp }
 *   TTarget  { de_entity target, u32 texture (blob asset ref), f32 speed }
 *   TMisc    { bool, i8, i16, i32, i64, u8, u16, u32, u64, f64, vec3, quat, name, enum TMood,
 *              flags TLayers, TItem items[3] (u32 id, i32 count), f32 samples[4] }
 *   TMarker  a tag (registered with DE_COMP_TAG: no data)
 */
#ifndef DE_FUZZ_SCENE_HARNESS_H
#define DE_FUZZ_SCENE_HARNESS_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_reflect.h>

#include "asset/scene/scene_format.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DE_HAS_COOKER
#define DE_HAS_COOKER 0
#endif

/** The asset ID of the one blob slot (the fixtures' `texture = "ui/heart"`). */
#define FZ_SCENE_BLOB_ID "ui/heart"

/** Prints `what` (and the last error) to stderr and aborts: a broken promise. */
void fz_scene_fail(const char *target, const char *what);

/** The shared registry (current versions) and asset system; built on first use. */
de_type_registry *fz_scene_types(void);
de_asset_system *fz_scene_assets(void);

/**
 * Checks what de__scene_view_parse promised for a view it accepted: every type name is
 * NUL-terminated inside the strings; every entity is found by its scene ID, its parent is a valid
 * index or none, its components are the next contiguous range, each with a valid type index and
 * data inside the data section; the root count; the asset references are sorted, unique, non-zero.
 * Aborts (naming `target`) on a broken promise.
 */
void fz_scene_check_view(const char *target, const de__scene_view *v);

/**
 * What the importer promises on top of the format (DENG-400's prefab check): every type is a
 * struct type of `types`, and every non-tag component's binary object decodes into its type with
 * de_deserialize_binary and uses exactly its bytes. Aborts on a broken promise.
 */
void fz_scene_check_cooked(const char *target, const de__scene_view *v,
                           const de_type_registry *types);

/**
 * Instantiates `size` bytes (which de__scene_view_parse accepted) into a new world: a slot of the
 * asset system holds them as the loader would (one block: object, then a copy of the bytes), and
 * de_prefab_instantiate runs twice, once without a parent and once under a holder entity. On
 * success the root is alive (if the scene has entities) and the holder has the second root as a
 * child; on failure the root is null. Returns the result of the first call. Everything is freed.
 */
de_result fz_scene_instantiate(const char *target, const u8 *data, size_t size);

#if DE_HAS_COOKER
/** Which registry a cook reads component types from. */
typedef enum fz_scene_reg {
    FZ_SCENE_REG_CURRENT = 0, /**< fz_scene_types(): THealth version 2 */
    FZ_SCENE_REG_HEALTH_V1,   /**< THealth version 1 { hp } (the corpus's migrate seed) */
} fz_scene_reg;

/** One cook output: the DSCN bytes and the cook dependencies (asset IDs, in the order given). */
typedef struct fz_scene_cooked {
    u8 *data;
    u64 size;
    u64 *deps;
    u32 dep_count;
} fz_scene_cooked;

/**
 * Cooks TOML `src` (`size` bytes; a NUL is added after a copy) with the `prefab` importer when
 * `prefab`, else the `scene` importer, through a cooker import context in memory. On DE_OK,
 * `*out` holds the output (free it with fz_scene_cooked_free); otherwise `msg` gets the message.
 */
de_result fz_scene_cook(const u8 *src, size_t size, bool prefab, fz_scene_reg reg,
                        fz_scene_cooked *out, char *msg, size_t msg_size);
void fz_scene_cooked_free(fz_scene_cooked *c);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DE_FUZZ_SCENE_HARNESS_H */
