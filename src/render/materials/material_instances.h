/*
 * material_instances.h: material instances (SPEC §12.6; DENG-302). Internal to de_render; include
 * as "render/materials/material_instances.h".
 *
 * A material instance is one parameter block in the GPU scene's material table
 * (render/materials/material_params.h): a loaded `material` asset (its record,
 * render/loaders/render_assets.h), or a runtime instance made with de_material_create_instance,
 * a copy of another material that the renderer owns. Both are de_material handles:
 *
 *   asset materials     the asset system's handles (slot index in the low 20 bits, generation
 *                       1..4095 in the high 12; asset/slots_internal.h)
 *   runtime instances   ids below 2^20: index in the low DE__MATERIAL_INSTANCE_INDEX_BITS bits,
 *                       generation 1..63 above it. The asset system never makes an id without a
 *                       generation, so the two never collide (de__material_is_instance).
 *
 * Parameters change on the main thread, between frames: de_material_set_params writes the record
 * (the asset's, in place, or the instance's) and marks its block in the GPU scene
 * (de__gpu_scene_material_changed). The GPU scene packs the marked blocks at the next kick and
 * uploads only those (render/scene/gpu_scene.c). Nothing about pipelines changes: a block's
 * features pick one of the permutations compiled at create (material_params.h).
 *
 * What else repacks blocks: the epoch (de__material_instances_epoch) moves when a material or a
 * texture is published, reloaded or unloaded (the loaders bump the uploader's), and when a game
 * model registers (a cooked material may name it); the GPU scene then re-checks every referenced
 * block once, in the frame of the change.
 *
 * Threads: everything on the main thread (the GPU scene calls the record lookup at the kick, while
 * the render thread is idle), except de__material_instances_epoch (any thread) and the pure
 * functions.
 */
#ifndef DE_RENDER_MATERIAL_INSTANCES_H
#define DE_RENDER_MATERIAL_INSTANCES_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_render.h>

#include "render/materials/material_params.h"

DE_EXTERN_C_BEGIN

struct de_render_material;
struct de_render_uploader;
struct de__material_models;

/** Runtime instance ids: index bits, generation bits (1..63), most instances alive at once. */
#define DE__MATERIAL_INSTANCE_INDEX_BITS 14u
#define DE__MATERIAL_INSTANCE_GEN_MAX    63u
#define DE__MATERIAL_INSTANCE_MAX        (1u << DE__MATERIAL_INSTANCE_INDEX_BITS)

typedef struct de__material_instances de__material_instances;

/** de__material_instances_create settings. */
typedef struct de__material_instances_desc {
    const de_allocator *allocator;            /**< NULL = the heap */
    de_gpu_device *device;                    /**< GPU texture leases (nullable for CPU tests) */
    struct de_asset_system *assets;           /**< asset materials (NULL: instances only) */
    struct de_render_uploader *uploader;      /**< its material epoch (NULL: none) */
    const struct de__material_models *models; /**< model names of DMAT model hashes (nullable) */
} de__material_instances_desc;

/** Creates the instance pool. Main thread. */
de_result de__material_instances_create(const de__material_instances_desc *d,
                                        de__material_instances **out);
/** Frees the pool (NULL ok). Main thread. */
void de__material_instances_destroy(de__material_instances *mi);

/** True for a runtime instance's handle (whether live or not). Pure. */
static inline bool de__material_is_instance(de_material m) {
    return m.id != 0u && (m.id >> 20) == 0u;
}

/**
 * The READY record of `m`: a live runtime instance's, or the asset's (de_render_material_get).
 * NULL when `m` is invalid, stale, or not loaded. An instance's record moves when the pool grows
 * (de__material_instances_create_instance): use it at once. Main thread.
 */
struct de_render_material *de__material_instances_get(de__material_instances *mi, de_material m);

/** A copy of READY material `base` (an asset or an instance) as a new runtime instance; invalid
 * on failure (error set). Main thread. */
de_material de__material_instances_create_instance(de__material_instances *mi, de_material base);
/** Destroys runtime instance `m`: its handle becomes stale. DE_ERR_INVALID_ARGUMENT for a stale
 * or non-instance handle. Main thread. */
de_result de__material_instances_destroy_instance(de__material_instances *mi, de_material m);
/** Live runtime instances. */
u32 de__material_instances_live(const de__material_instances *mi);

/** Number of live retained GPU bindings (main thread only). */
u32 de__material_instances_gpu_bindings(const de__material_instances *mi);

/** The change epoch: the uploader's (publishes, reloads, unloads) plus the pool's own (game model
 * registrations). Any thread. */
u64 de__material_instances_epoch(const de__material_instances *mi);
/** Moves the pool's own epoch: every block is re-checked at the next kick (NULL ok). */
void de__material_instances_touch(de__material_instances *mi);

/** The model id of record `m`: its game model id, its DMAT model hash resolved through `models`
 * (nullable; Standard while the model is not registered), else Unlit or Standard by its
 * parameters. */
u32 de__material_model_of(const struct de_render_material *m,
                          const struct de__material_models *models);

/** Returns the sampled bindless index of a texture, or DE_GPU_BINDLESS_INVALID while not loaded. */
typedef u32 (*de__material_texture_fn)(void *user, de_texture t);

/**
 * Packs record `m` (NULL: a block without READY, all zero) into its parameter block `out`: the
 * header (model, the features of de__material_features, flags, alpha mode), the Standard rows,
 * the textures' bindless indices through `texture` (a texture not loaded: 0) and the UV-set bits.
 * `models` as for de__material_model_of. Pure apart from the callback.
 */
void de__material_pack(const struct de_render_material *m, const struct de__material_models *models,
                       de__material_texture_fn texture, void *texture_user,
                       de__material_params *out);

/** Fills the material fields of `s` (DENG-302: permutations_per_model, the last kick's
 * permutation switches and live instances, the last frame's material uploads). Thread that
 * renders, after the frame's GPU-scene stats. */
void de__material_fill_stats(const de_renderer *r, de_render_stats *s);

DE_EXTERN_C_END

#endif /* DE_RENDER_MATERIAL_INSTANCES_H */
