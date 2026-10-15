/*
 * material_models.h: the material model registry and the material feature flags (SPEC §12.6;
 * DENG-298). Internal to de_render; include as "render/materials/material_models.h".
 *
 * A material model is a Slang type implementing IMaterialModel
 * (shaders/materials/material_interface.slang). Its id is what DeGpuMaterial.model holds and what
 * material classification bins tiles by (DENG-237): at most DE__MATERIAL_MODEL_MAX ids, because a
 * tile's model set is one u32.
 *
 *   ids 0..4   the built-in models (DENG-2 Q12): Standard 0, Unlit 1, Sprite 2, Terrain 3, Water 4
 *              (DE__GS_MATERIAL_MODEL_*). The engine shader vis_resolve has their resolve kernels
 *              (cs_resolve_standard, cs_resolve_unlit); Sprite, Terrain and Water resolve as
 *              Standard until their sprints.
 *   ids 5..31  game models, in registration order (de_material_model_register). Each brings a
 *              `shader` asset with the compute entry point `cs_resolve_model`: the generic resolve
 *              kernel de_resolve_tiles<Model> (shaders/visbuffer/resolve_model.slang), 64 threads,
 *              the ResolveParams push constants. The registry makes its pipeline at registration;
 *              the resolve pass (passes/vis/resolve.c) dispatches it on the model's tile list.
 *
 * Threads: registration (de__material_models_add) on the main thread; the lookups for the resolve
 * pass from the render thread at any time. An entry is filled before the count that publishes it
 * is stored (release), and entries are never removed until destroy, so the readers need no lock.
 */
#ifndef DE_RENDER_MATERIAL_MODELS_H
#define DE_RENDER_MATERIAL_MODELS_H

#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_shader.h>

DE_EXTERN_C_BEGIN

/** Material model ids (DE__GS_MATERIAL_MODELS: one bit each in a u32 tile mask). */
#define DE__MATERIAL_MODEL_MAX 32u
/** Built-in ids (DENG-2 Q12); game models start at DE__MATERIAL_MODEL_FIRST_GAME. */
#define DE__MATERIAL_MODEL_BUILTIN_COUNT 5u
#define DE__MATERIAL_MODEL_FIRST_GAME    DE__MATERIAL_MODEL_BUILTIN_COUNT
/** Longest model name in bytes, without the NUL. */
#define DE__MATERIAL_MODEL_NAME_MAX 31u
/** The resolve kernel a game model's shader must have, and its push-constant block. */
#define DE__MATERIAL_MODEL_RESOLVE_ENTRY "cs_resolve_model"
#define DE__MATERIAL_MODEL_RESOLVE_GROUP 64u
#define DE__MATERIAL_MODEL_RESOLVE_PUSH  112u

/**
 * Per-material feature bits (shaders/materials/material_interface.slang DE_MATERIAL_FEATURE_*;
 * the same values, checked by a test). The one set every S14 task uses: append, never renumber.
 */
typedef enum de__material_feature {
    DE__MATERIAL_FEATURE_CLEARCOAT = 0x1u,
    DE__MATERIAL_FEATURE_SHEEN = 0x2u,
    DE__MATERIAL_FEATURE_ANISOTROPY = 0x4u,
    DE__MATERIAL_FEATURE_IRIDESCENCE = 0x8u,
    DE__MATERIAL_FEATURE_TRANSMISSION = 0x10u,
    DE__MATERIAL_FEATURE_VOLUME = 0x20u,
    DE__MATERIAL_FEATURE_DIFFUSE_TRANSMISSION = 0x40u,
    DE__MATERIAL_FEATURE_SUBSURFACE = 0x80u,
    DE__MATERIAL_FEATURE_UNLIT = 0x100u,
    DE__MATERIAL_FEATURE_FORWARD = 0x200u, /**< blend or transmissive: the forward pass */
    DE__MATERIAL_FEATURE_DOUBLE_SIDED = 0x400u,
    DE__MATERIAL_FEATURE_ALPHA_MASK = 0x800u
} de__material_feature;

/** A named constant shared with Slang (tests compare the tables with the sources). */
typedef struct de__material_const {
    const char *name;
    u32 value;
} de__material_const;

/** The DE_MATERIAL_FEATURE_* values of material_interface.slang, by Slang name. */
const de__material_const *de__material_feature_consts(u32 *count);

typedef struct de__material_models de__material_models;

/** Creates the registry with the built-in models. `alloc` NULL = the heap. Main thread. */
de_result de__material_models_create(de_gpu_device *dev, const de_allocator *alloc,
                                     de__material_models **out);
/** Destroys the game models' pipelines (deferred until the GPU is done) and frees `m`. NULL ok. */
void de__material_models_destroy(de__material_models *m);

/**
 * Registers game model `name` with the code of its `cs_resolve_model` (for `m`'s device) and that
 * entry's reflection: checks the contract (a compute kernel of 64 x 1 x 1 threads whose push
 * constants fit ResolveParams), makes the pipeline and gives the next free id. Main thread.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (a NULL argument, an empty or too long name, code that
 *         is not the resolve kernel); DE_ERR_ALREADY_EXISTS (the name is taken); DE_ERR_BUSY (all
 *         DE__MATERIAL_MODEL_MAX ids are taken); DE_ERR_INTERNAL (the pipeline failed).
 */
de_result de__material_models_add(de__material_models *m, const char *name,
                                  const de_gpu_shader_blob *code, const de_shader_reflection *refl,
                                  u32 *out_id);

/** Models registered, built-ins included (ids 0 .. count-1). Any thread. */
u32 de__material_models_count(const de__material_models *m);
/** The name of model `id` ("standard", ...), NULL for a free id. Any thread. */
const char *de__material_models_name(const de__material_models *m, u32 id);
/** The id of the model called `name`, or DE__MATERIAL_MODEL_MAX. Any thread. */
u32 de__material_models_find(const de__material_models *m, const char *name);
/** DENG-302: the id of the model whose name has DMAT model hash `hash`
 * (de_cooked_material_model_hash), or DE__MATERIAL_MODEL_MAX (also for 0). Any thread. */
u32 de__material_models_find_hash(const de__material_models *m, u32 hash);
/** The resolve pipeline of game model `id`; invalid for a built-in or free id (NULL `m` ok). Any
 * thread. */
de_gpu_pipeline de__material_models_resolve_pipeline(const de__material_models *m, u32 id);

DE_EXTERN_C_END

#endif /* DE_RENDER_MATERIAL_MODELS_H */
