/*
 * material_models.c: the material model registry (material_models.h) and de_material_model_register
 * (SPEC §12.6; DENG-298).
 */
#include "render/materials/material_models.h"

#include "platform/de_atomic.h"
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"

#include <dengine/de_asset.h>
#include <dengine/de_render.h>

#include <stdio.h>
#include <string.h>

DE_STATIC_ASSERT(DE__MATERIAL_MODEL_MAX == DE__GS_MATERIAL_MODELS,
                 "one id per bit of classification's tile mask");
DE_STATIC_ASSERT(DE__GS_MATERIAL_MODEL_WATER + 1u == DE__MATERIAL_MODEL_FIRST_GAME,
                 "game models follow the built-in ids");

static const de__material_const k_features[] = {
    {"DE_MATERIAL_FEATURE_CLEARCOAT", DE__MATERIAL_FEATURE_CLEARCOAT},
    {"DE_MATERIAL_FEATURE_SHEEN", DE__MATERIAL_FEATURE_SHEEN},
    {"DE_MATERIAL_FEATURE_ANISOTROPY", DE__MATERIAL_FEATURE_ANISOTROPY},
    {"DE_MATERIAL_FEATURE_IRIDESCENCE", DE__MATERIAL_FEATURE_IRIDESCENCE},
    {"DE_MATERIAL_FEATURE_TRANSMISSION", DE__MATERIAL_FEATURE_TRANSMISSION},
    {"DE_MATERIAL_FEATURE_VOLUME", DE__MATERIAL_FEATURE_VOLUME},
    {"DE_MATERIAL_FEATURE_DIFFUSE_TRANSMISSION", DE__MATERIAL_FEATURE_DIFFUSE_TRANSMISSION},
    {"DE_MATERIAL_FEATURE_SUBSURFACE", DE__MATERIAL_FEATURE_SUBSURFACE},
    {"DE_MATERIAL_FEATURE_UNLIT", DE__MATERIAL_FEATURE_UNLIT},
    {"DE_MATERIAL_FEATURE_FORWARD", DE__MATERIAL_FEATURE_FORWARD},
    {"DE_MATERIAL_FEATURE_DOUBLE_SIDED", DE__MATERIAL_FEATURE_DOUBLE_SIDED},
    {"DE_MATERIAL_FEATURE_ALPHA_MASK", DE__MATERIAL_FEATURE_ALPHA_MASK},
};

const de__material_const *de__material_feature_consts(u32 *count) {
    *count = (u32)(sizeof k_features / sizeof k_features[0]);
    return k_features;
}

typedef struct mm_entry {
    char name[DE__MATERIAL_MODEL_NAME_MAX + 1u];
    de_gpu_pipeline resolve; /**< game models; invalid for the built-ins */
} mm_entry;

struct de__material_models {
    de_gpu_device *dev;
    const de_allocator *alloc;
    de_atomic_u32 count; /**< entries [0, count) are filled (release / acquire) */
    mm_entry entries[DE__MATERIAL_MODEL_MAX];
};

static const char *const k_builtin[DE__MATERIAL_MODEL_BUILTIN_COUNT] = {
    "standard", "unlit", "sprite", "terrain", "water"};

de_result de__material_models_create(de_gpu_device *dev, const de_allocator *alloc,
                                     de__material_models **out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__material_models_create: NULL argument");
    *out = NULL;
    if (alloc == NULL)
        alloc = de_heap_allocator();
    de__material_models *m = DE_NEW(alloc, de__material_models, DE_MEM_RENDER);
    if (m == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "material models: no memory");
    memset(m, 0, sizeof *m);
    m->dev = dev;
    m->alloc = alloc;
    for (u32 i = 0; i < DE__MATERIAL_MODEL_BUILTIN_COUNT; ++i)
        snprintf(m->entries[i].name, sizeof m->entries[i].name, "%s", k_builtin[i]);
    de_atomic_init_u32(&m->count, DE__MATERIAL_MODEL_BUILTIN_COUNT);
    *out = m;
    return DE_OK;
}

void de__material_models_destroy(de__material_models *m) {
    if (m == NULL)
        return;
    const u32 n = de_atomic_load_u32(&m->count, DE_ATOMIC_ACQUIRE);
    for (u32 i = 0; i < n; ++i)
        if (DE_HANDLE_VALID(m->entries[i].resolve))
            de_gpu_pipeline_destroy(m->dev, m->entries[i].resolve);
    DE_FREE(m->alloc, m, sizeof *m);
}

u32 de__material_models_count(const de__material_models *m) {
    return m != NULL ? de_atomic_load_u32(&m->count, DE_ATOMIC_ACQUIRE) : 0u;
}

const char *de__material_models_name(const de__material_models *m, u32 id) {
    return id < de__material_models_count(m) ? m->entries[id].name : NULL;
}

u32 de__material_models_find(const de__material_models *m, const char *name) {
    if (name == NULL)
        return DE__MATERIAL_MODEL_MAX;
    const u32 n = de__material_models_count(m);
    for (u32 i = 0; i < n; ++i)
        if (strcmp(m->entries[i].name, name) == 0)
            return i;
    return DE__MATERIAL_MODEL_MAX;
}

de_gpu_pipeline de__material_models_resolve_pipeline(const de__material_models *m, u32 id) {
    if (id < DE__MATERIAL_MODEL_FIRST_GAME || id >= de__material_models_count(m))
        return (de_gpu_pipeline){0};
    return m->entries[id].resolve;
}

/* The contract of a game model's resolve kernel, or why its reflection breaks it. */
static bool resolve_contract(const de_shader_reflection *refl, char *why, size_t cap) {
    if (refl->stage != DE_SHADER_STAGE_COMPUTE) {
        snprintf(why, cap, "`%s` is not a compute entry point", DE__MATERIAL_MODEL_RESOLVE_ENTRY);
        return false;
    }
    if (refl->threadgroup[0] != DE__MATERIAL_MODEL_RESOLVE_GROUP || refl->threadgroup[1] != 1u ||
        refl->threadgroup[2] != 1u) {
        snprintf(why, cap, "`%s` has %u x %u x %u threads, not [numthreads(%u, 1, 1)]",
                 DE__MATERIAL_MODEL_RESOLVE_ENTRY, refl->threadgroup[0], refl->threadgroup[1],
                 refl->threadgroup[2], DE__MATERIAL_MODEL_RESOLVE_GROUP);
        return false;
    }
    if (refl->push_constant_bytes > DE__MATERIAL_MODEL_RESOLVE_PUSH) {
        snprintf(why, cap, "`%s` has %u bytes of push constants, not ResolveParams (%u)",
                 DE__MATERIAL_MODEL_RESOLVE_ENTRY, refl->push_constant_bytes,
                 DE__MATERIAL_MODEL_RESOLVE_PUSH);
        return false;
    }
    return true;
}

de_result de__material_models_add(de__material_models *m, const char *name,
                                  const de_gpu_shader_blob *code, const de_shader_reflection *refl,
                                  u32 *out_id) {
    if (m == NULL || name == NULL || code == NULL || refl == NULL || out_id == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "material model: NULL argument");
    const size_t len = strlen(name);
    if (len == 0 || len > DE__MATERIAL_MODEL_NAME_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "material model \"%.40s\": the name must be 1 to %u bytes", name,
                       DE__MATERIAL_MODEL_NAME_MAX);
    if (de__material_models_find(m, name) != DE__MATERIAL_MODEL_MAX)
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "material model \"%s\" is already registered", name);
    const u32 id = de_atomic_load_u32(&m->count, DE_ATOMIC_RELAXED);
    if (id >= DE__MATERIAL_MODEL_MAX)
        return DE_FAIL(DE_ERR_BUSY,
                       "material model \"%s\": all %u material model ids are taken (classification "
                       "keeps a tile's models in one 32-bit mask)",
                       name, DE__MATERIAL_MODEL_MAX);
    char why[160];
    if (!resolve_contract(refl, why, sizeof why))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "material model \"%s\": the shader does not implement IMaterialModel's "
                       "resolve: %s (it must be de_resolve_tiles<Model> of "
                       "shaders/visbuffer/resolve_model.slang)",
                       name, why);
    char label[64];
    snprintf(label, sizeof label, "material model %s resolve", name);
    const de_gpu_pipeline p = de_gpu_pipeline_create(
        m->dev,
        &(de_gpu_pipeline_desc){
            .kind = DE_PIPE_COMPUTE,
            .cs = *code,
            .push_constant_bytes = DE__MATERIAL_MODEL_RESOLVE_PUSH,
            .threadgroup_size = {refl->threadgroup[0], refl->threadgroup[1], refl->threadgroup[2]},
            .name = label});
    if (!DE_HANDLE_VALID(p))
        return DE_FAIL_WRAP(DE_ERR_INTERNAL, "material model \"%s\": the resolve pipeline", name);
    mm_entry *e = &m->entries[id];
    memcpy(e->name, name, len + 1u);
    e->resolve = p;
    de_atomic_store_u32(&m->count, id + 1u, DE_ATOMIC_RELEASE);
    *out_id = id;
    return DE_OK;
}

/* ---------- Public API (de_render.h) ---------- */

de_material_model de_material_model_register(de_renderer *r, const char *name, de_shader module) {
    de_material_model none = {0};
    if (r == NULL || name == NULL) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_material_model_register: NULL argument");
        return none;
    }
    if (r->material_models == NULL || r->assets == NULL) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                      "material model \"%s\": the renderer has no asset system to load the shader "
                      "from",
                      name);
        return none;
    }
    if (de__material_models_find(r->material_models, name) != DE__MATERIAL_MODEL_MAX) {
        (void)DE_FAIL(DE_ERR_ALREADY_EXISTS, "material model \"%s\" is already registered", name);
        return none;
    }
    de_gpu_shader_blob code = {0};
    de_shader_reflection refl;
    memset(&refl, 0, sizeof refl);
    const de_result res = de_shader_get_code(r->assets, module, r->caps.backend, 0,
                                             DE__MATERIAL_MODEL_RESOLVE_ENTRY, &code, &refl);
    switch (res) {
    case DE_OK:
        break;
    case DE_ERR_BUSY:
        (void)DE_FAIL(DE_ERR_BUSY,
                      "material model \"%s\": the shader is not loaded (still loading, or its "
                      "load failed): register the model once the shader is READY",
                      name);
        return none;
    case DE_ERR_NOT_FOUND:
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                      "material model \"%s\": the shader does not implement IMaterialModel: it has "
                      "no `%s` entry point for the %s backend (a model's shader defines "
                      "cs_resolve_model as de_resolve_tiles<Model>, "
                      "shaders/visbuffer/resolve_model.slang)",
                      name, DE__MATERIAL_MODEL_RESOLVE_ENTRY, de_gpu_backend_name(r->caps.backend));
        return none;
    default:
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                      "material model \"%s\": not a loaded shader asset (%s)", name,
                      de_result_str(res));
        return none;
    }
    u32 id = 0;
    if (de__material_models_add(r->material_models, name, &code, &refl, &id) != DE_OK)
        return none;
    DE_INFO(r->log, "material model \"%s\" registered with id %u", name, id);
    return (de_material_model){id + 1u};
}

de_material_model de_material_model_find(de_renderer *r, const char *name) {
    const u32 id =
        r != NULL ? de__material_models_find(r->material_models, name) : DE__MATERIAL_MODEL_MAX;
    return id < DE__MATERIAL_MODEL_MAX ? (de_material_model){id + 1u} : (de_material_model){0};
}

u32 de_material_model_id(de_material_model model) {
    return DE_HANDLE_VALID(model) ? model.id - 1u : 0u;
}
