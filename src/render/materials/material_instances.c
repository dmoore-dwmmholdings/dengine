/*
 * material_instances.c: material parameter blocks, their feature bits and resolve permutations
 * (material_params.h), the runtime instance pool and the material parameter API of de_render.h
 * (material_instances.h; SPEC §12.6, §12.2; DENG-302).
 */
#include "render/materials/material_instances.h"

#include "platform/de_atomic.h"
#include "render/formats/de_cooked_material.h"
#include "render/loaders/render_assets.h"
#include "render/loaders/upload_ring.h"
#include "render/materials/material_models.h"
#include "render/renderer_internal.h"
#include "render/scene/gpu_scene.h"

#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>

#include <string.h>

DE_STATIC_ASSERT(DE__MATERIAL_TEXTURES == DE_COOKED_TEX_COUNT, "one block word per texture slot");
DE_STATIC_ASSERT(DE__MATERIAL_INSTANCE_INDEX_BITS + 6u <= 20u,
                 "runtime instance ids stay below the asset system's generation bits");

/* =================================================================================================
 * Layout tables (the test material_params_layout_match)
 * ============================================================================================== */

#define ROW(ctype, stype, name, dims)                                                              \
    {#name, #stype, (u32)offsetof(de__material_params, name),                                      \
     (u32)sizeof(((de__material_params *)0)->name)},

static const de__material_layout_field k_fields[] = {DE__MATERIAL_PARAMS_FIELDS(ROW)};
#undef ROW

const de__material_layout_field *de__material_params_layout(u32 *count) {
    *count = (u32)(sizeof k_fields / sizeof k_fields[0]);
    return k_fields;
}

static const de__material_layout_const k_consts[] = {
    {"DE_GPU_MATERIAL_READY", DE__MATERIAL_FLAG_READY},
    {"DE_GPU_MATERIAL_DOUBLE_SIDED", DE__MATERIAL_FLAG_DOUBLE_SIDED},
    {"DE_GPU_MATERIAL_UNLIT", DE__MATERIAL_FLAG_UNLIT},
    {"DE_GPU_MATERIAL_ALPHA_OPAQUE", DE_COOKED_ALPHA_OPAQUE},
    {"DE_GPU_MATERIAL_ALPHA_MASK", DE_COOKED_ALPHA_MASK},
    {"DE_GPU_MATERIAL_ALPHA_BLEND", DE_COOKED_ALPHA_BLEND},
    {"DE_GPU_MATERIAL_TEXTURES", DE__MATERIAL_TEXTURES},
    {"DE_GPU_MATERIAL_TEXTURE_UV1", DE__MATERIAL_TEXTURE_UV1},
};

const de__material_layout_const *de__material_params_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

/* =================================================================================================
 * Features and permutations
 * ============================================================================================== */

u32 de__material_features(const de_standard_material *s, u32 alpha_mode, u32 model) {
    if (s == NULL)
        return 0u;
    u32 f = 0u;
    /* `x > 0.0f` is false for NaN, as in the shaders. */
    if (s->clearcoat > 0.0f)
        f |= DE__MATERIAL_FEATURE_CLEARCOAT;
    if (s->sheen_color.x > 0.0f || s->sheen_color.y > 0.0f || s->sheen_color.z > 0.0f)
        f |= DE__MATERIAL_FEATURE_SHEEN;
    if (s->anisotropy > 0.0f)
        f |= DE__MATERIAL_FEATURE_ANISOTROPY;
    if (s->iridescence > 0.0f &&
        (s->iridescence_thickness_max > 0.0f || s->iridescence_thickness_min > 0.0f))
        f |= DE__MATERIAL_FEATURE_IRIDESCENCE;
    if (s->transmission > 0.0f)
        f |= DE__MATERIAL_FEATURE_TRANSMISSION;
    if (s->thickness > 0.0f && (s->transmission > 0.0f || s->diffuse_transmission > 0.0f))
        f |= DE__MATERIAL_FEATURE_VOLUME;
    if (s->diffuse_transmission > 0.0f)
        f |= DE__MATERIAL_FEATURE_DIFFUSE_TRANSMISSION;
    if (s->subsurface > 0.0f)
        f |= DE__MATERIAL_FEATURE_SUBSURFACE;
    if (model == DE__GS_MATERIAL_MODEL_UNLIT)
        f |= DE__MATERIAL_FEATURE_UNLIT;
    if (alpha_mode == DE_COOKED_ALPHA_BLEND || s->transmission > 0.0f)
        f |= DE__MATERIAL_FEATURE_FORWARD;
    if (s->double_sided)
        f |= DE__MATERIAL_FEATURE_DOUBLE_SIDED;
    if (alpha_mode == DE_COOKED_ALPHA_MASK)
        f |= DE__MATERIAL_FEATURE_ALPHA_MASK;
    return f;
}

/* The Standard resolve kernels of shaders/visbuffer/resolve_standard.slang (StandardModelT<F>):
 * a chain, smallest first. Sprite, Terrain and Water resolve as Standard until their sprints. */
static const de__material_permutation k_standard[] = {
    {0u, "cs_resolve_standard_base"},
    {DE__MATERIAL_FEATURE_CLEARCOAT, "cs_resolve_standard_clearcoat"},
    {UINT32_MAX, "cs_resolve_standard"},
};
static const de__material_permutation k_unlit[] = {
    {UINT32_MAX, "cs_resolve_unlit"},
};

const de__material_permutation *de__material_permutations(u32 model, u32 *count) {
    const de__material_permutation *p = NULL;
    u32 n = 0;
    switch (model) {
    case DE__GS_MATERIAL_MODEL_STANDARD:
    case DE__GS_MATERIAL_MODEL_SPRITE:
    case DE__GS_MATERIAL_MODEL_TERRAIN:
    case DE__GS_MATERIAL_MODEL_WATER:
        p = k_standard;
        n = (u32)(sizeof k_standard / sizeof k_standard[0]);
        break;
    case DE__GS_MATERIAL_MODEL_UNLIT:
        p = k_unlit;
        n = 1u;
        break;
    default:
        break;
    }
    if (count != NULL)
        *count = n;
    return p;
}

u32 de__material_permutations_compiled(u32 model, bool registered) {
    if (model == DE__GS_MATERIAL_MODEL_SPRITE || model == DE__GS_MATERIAL_MODEL_TERRAIN ||
        model == DE__GS_MATERIAL_MODEL_WATER)
        return 0u;
    u32 n = 0;
    (void)de__material_permutations(model, &n);
    if (n != 0)
        return n;
    return model < DE__GS_MATERIAL_MODELS && registered ? 1u : 0u;
}

u32 de__material_permutation_of(u32 model, u32 features) {
    u32 n = 0;
    const de__material_permutation *p = de__material_permutations(model, &n);
    features &= DE__MATERIAL_PERMUTATION_FEATURES; /* the bits that compile lobes in or out */
    for (u32 i = 0; i < n; ++i)
        if ((features & ~p[i].features) == 0u)
            return i;
    return n != 0 ? n - 1u : 0u;
}

DE_STATIC_ASSERT(sizeof k_standard / sizeof k_standard[0] <= DE__MATERIAL_PERMUTATIONS_MAX,
                 "Standard permutations");

/* =================================================================================================
 * Packing
 * ============================================================================================== */

static void f4(de__gs_float4 *o, f32 a, f32 b, f32 c, f32 d) {
    o->v[0] = a;
    o->v[1] = b;
    o->v[2] = c;
    o->v[3] = d;
}

u32 de__material_model_of(const de_render_material *m, const de__material_models *models) {
    if (m == NULL)
        return DE__GS_MATERIAL_MODEL_STANDARD;
    if (m->model > DE__GS_MATERIAL_MODEL_WATER && m->model < DE__GS_MATERIAL_MODELS)
        return m->model; /* a game model's id (DENG-298) */
    if (m->model_hash != 0u && models != NULL) {
        const u32 id = de__material_models_find_hash(models, m->model_hash);
        if (id < DE__GS_MATERIAL_MODELS && id != DE__GS_MATERIAL_MODEL_STANDARD &&
            id != DE__GS_MATERIAL_MODEL_UNLIT)
            return id; /* named: sprite, terrain, water or a game model */
        if (id == DE__GS_MATERIAL_MODEL_UNLIT)
            return id;
        /* "standard", or a model not registered (yet): the parameters decide */
    }
    return m->standard.unlit ? DE__GS_MATERIAL_MODEL_UNLIT : DE__GS_MATERIAL_MODEL_STANDARD;
}

void de__material_pack(const de_render_material *m, const de__material_models *models,
                       de__material_texture_fn texture, void *texture_user,
                       de__material_params *o) {
    memset(o, 0, sizeof *o);
    if (m == NULL)
        return; /* not loaded: no READY flag */
    const de_standard_material *s = &m->standard;
    o->model = de__material_model_of(m, models);
    /* The record's alpha mode (the loader and de_material_set_params keep standard.alpha_mode
     * equal to it). */
    o->alpha_mode = m->alpha_mode <= DE_COOKED_ALPHA_BLEND ? m->alpha_mode : DE_COOKED_ALPHA_OPAQUE;
    o->features = de__material_features(s, o->alpha_mode, o->model);
    o->flags = DE__MATERIAL_FLAG_READY | (s->double_sided ? DE__MATERIAL_FLAG_DOUBLE_SIDED : 0u) |
               (o->model == DE__GS_MATERIAL_MODEL_UNLIT ? DE__MATERIAL_FLAG_UNLIT : 0u);
    f4(&o->base_color, s->base_color.x, s->base_color.y, s->base_color.z, s->base_color.w);
    f4(&o->emissive, s->emissive.x, s->emissive.y, s->emissive.z, s->emissive_strength);
    f4(&o->pbr, s->metallic, s->roughness, s->normal_scale, s->alpha_cutoff);
    f4(&o->specular, s->specular_color.x, s->specular_color.y, s->specular_color.z, s->specular);
    f4(&o->clearcoat, s->clearcoat, s->clearcoat_roughness, s->ior, s->transmission);
    f4(&o->sheen, s->sheen_color.x, s->sheen_color.y, s->sheen_color.z, s->sheen_roughness);
    f4(&o->volume, s->attenuation_color.x, s->attenuation_color.y, s->attenuation_color.z,
       s->attenuation_distance);
    f4(&o->extra, s->thickness, s->diffuse_transmission, s->subsurface, s->anisotropy);
    f4(&o->iridescence, s->iridescence, s->iridescence_ior, s->iridescence_thickness_min,
       s->iridescence_thickness_max);
    f4(&o->subsurface, s->subsurface_radius.x, s->subsurface_radius.y, s->subsurface_radius.z,
       s->anisotropy_rotation);
    for (u32 t = 0; t < DE__MATERIAL_TEXTURES; ++t) {
        u32 b = 0u;
        if (DE_HANDLE_VALID(m->textures[t]) && texture != NULL) {
            b = texture(texture_user, m->textures[t]);
            if (b == DE_GPU_BINDLESS_INVALID || (b & DE__MATERIAL_TEXTURE_UV1) != 0u)
                b = 0u; /* not loaded: the shader's fallback */
        }
        o->textures[t / 4u].v[t % 4u] = b | (m->texcoord[t] != 0 ? DE__MATERIAL_TEXTURE_UV1 : 0u);
    }
    o->textures[DE__MATERIAL_DT_COLOR_WORD / 4u].v[DE__MATERIAL_DT_COLOR_WORD % 4u] =
        de__material_dt_color_pack(s->diffuse_transmission_color.x, s->diffuse_transmission_color.y,
                                   s->diffuse_transmission_color.z);
}

/* =================================================================================================
 * The instance pool
 * ============================================================================================== */

typedef struct mi_entry {
    de_render_material rec;
    u32 gen; /**< 1..DE__MATERIAL_INSTANCE_GEN_MAX: the generation of the live (or next) id */
    bool live;
    u32 free_next; /**< next free index + 1; 0 = end */
} mi_entry;

struct de__material_instances {
    const de_allocator *alloc;
    de_asset_system *assets;
    de_render_uploader *uploader;
    const de__material_models *models;
    mi_entry *entries;
    u32 cap, count; /**< entries allocated / used (high-water mark) */
    u32 free_head;  /**< free index + 1; 0 = none */
    u32 live;
    de_atomic_u64 epoch; /**< the pool's own (game model registrations) */
};

de_result de__material_instances_create(const de__material_instances_desc *d,
                                        de__material_instances **out) {
    if (out != NULL)
        *out = NULL;
    if (d == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__material_instances_create: NULL argument");
    const de_allocator *a = d->allocator != NULL ? d->allocator : de_heap_allocator();
    de__material_instances *mi = DE_NEW(a, de__material_instances, DE_MEM_RENDER);
    if (mi == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "material instances: no memory");
    memset(mi, 0, sizeof *mi);
    mi->alloc = a;
    mi->assets = d->assets;
    mi->uploader = d->uploader;
    mi->models = d->models;
    de_atomic_init_u64(&mi->epoch, 0);
    *out = mi;
    return DE_OK;
}

void de__material_instances_destroy(de__material_instances *mi) {
    if (mi == NULL)
        return;
    if (mi->entries != NULL)
        DE_FREE(mi->alloc, mi->entries, (size_t)mi->cap * sizeof(mi_entry));
    DE_FREE(mi->alloc, mi, sizeof *mi);
}

static mi_entry *instance_entry(de__material_instances *mi, de_material m) {
    if (mi == NULL || !de__material_is_instance(m))
        return NULL;
    const u32 index = m.id & (DE__MATERIAL_INSTANCE_MAX - 1u);
    const u32 gen = m.id >> DE__MATERIAL_INSTANCE_INDEX_BITS;
    if (index >= mi->count)
        return NULL;
    mi_entry *e = &mi->entries[index];
    return e->live && e->gen == gen ? e : NULL;
}

de_render_material *de__material_instances_get(de__material_instances *mi, de_material m) {
    if (mi == NULL || !DE_HANDLE_VALID(m))
        return NULL;
    if (de__material_is_instance(m)) {
        mi_entry *e = instance_entry(mi, m);
        return e != NULL ? &e->rec : NULL;
    }
    /* An asset's record: main-thread data (DENG-422), changed in place by set_params. */
    return mi->assets != NULL ? (de_render_material *)de_render_material_get(mi->assets, m) : NULL;
}

static bool pool_grow(de__material_instances *mi) {
    if (mi->cap >= DE__MATERIAL_INSTANCE_MAX)
        return false;
    const u32 cap = mi->cap != 0 ? mi->cap * 2u : 64u;
    mi_entry *n =
        (mi_entry *)DE_ALLOC(mi->alloc, (size_t)cap * sizeof(mi_entry), 16, DE_MEM_RENDER);
    if (n == NULL)
        return false;
    memset(n, 0, (size_t)cap * sizeof(mi_entry));
    if (mi->entries != NULL) {
        memcpy(n, mi->entries, (size_t)mi->count * sizeof(mi_entry));
        DE_FREE(mi->alloc, mi->entries, (size_t)mi->cap * sizeof(mi_entry));
    }
    mi->entries = n;
    mi->cap = cap;
    return true;
}

de_material de__material_instances_create_instance(de__material_instances *mi, de_material base) {
    const de_material none = {0};
    if (mi == NULL || !DE_HANDLE_VALID(base)) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_material_create_instance: %s",
                      mi == NULL ? "no renderer" : "invalid base material");
        return none;
    }
    const de_render_material *src = de__material_instances_get(mi, base);
    if (src == NULL) {
        if (de__material_is_instance(base))
            (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                          "de_material_create_instance: base instance 0x%05x is stale", base.id);
        else
            (void)DE_FAIL(DE_ERR_BUSY,
                          "de_material_create_instance: base material 0x%08x is not loaded",
                          base.id);
        return none;
    }
    const de_render_material copy = *src; /* `src` may move when the pool grows */
    u32 index;
    if (mi->free_head != 0) {
        index = mi->free_head - 1u;
        mi->free_head = mi->entries[index].free_next;
    } else {
        if (mi->count == mi->cap && !pool_grow(mi)) {
            (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                          "de_material_create_instance: %u runtime instances are alive (at most "
                          "%u), or no memory",
                          mi->live, DE__MATERIAL_INSTANCE_MAX);
            return none;
        }
        index = mi->count++;
        mi->entries[index].gen = 1u;
    }
    mi_entry *e = &mi->entries[index];
    e->rec = copy;
    memset(&e->rec.retire_, 0, sizeof e->rec.retire_);
    e->live = true;
    e->free_next = 0;
    mi->live++;
    return (de_material){(e->gen << DE__MATERIAL_INSTANCE_INDEX_BITS) | index};
}

de_result de__material_instances_destroy_instance(de__material_instances *mi, de_material m) {
    mi_entry *e = instance_entry(mi, m);
    if (e == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_material_destroy_instance: 0x%08x is not a live runtime instance", m.id);
    const u32 index = (u32)(e - mi->entries);
    e->live = false;
    e->gen = e->gen >= DE__MATERIAL_INSTANCE_GEN_MAX ? 1u : e->gen + 1u;
    e->free_next = mi->free_head;
    mi->free_head = index + 1u;
    mi->live--;
    return DE_OK;
}

u32 de__material_instances_live(const de__material_instances *mi) {
    return mi != NULL ? mi->live : 0u;
}

u64 de__material_instances_epoch(const de__material_instances *mi) {
    if (mi == NULL)
        return 0u;
    u64 e = de_atomic_load_u64(&mi->epoch, DE_ATOMIC_ACQUIRE);
    if (mi->uploader != NULL)
        e += de_atomic_load_u64(&mi->uploader->material_epoch, DE_ATOMIC_ACQUIRE);
    return e;
}

void de__material_instances_touch(de__material_instances *mi) {
    if (mi != NULL)
        (void)de_atomic_fetch_add_u64(&mi->epoch, 1u, DE_ATOMIC_RELEASE);
}

/* =================================================================================================
 * de_render.h
 * ============================================================================================== */

/* The record of `m` for the API calls, or NULL with the error set. */
static de_render_material *api_record(de_renderer *r, de_material m, const char *fn) {
    if (r == NULL || r->material_instances == NULL) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: %s", fn,
                      r == NULL ? "NULL renderer" : "the renderer has no materials");
        return NULL;
    }
    if (!DE_HANDLE_VALID(m)) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: invalid material", fn);
        return NULL;
    }
    de_render_material *rec = de__material_instances_get(r->material_instances, m);
    if (rec != NULL)
        return rec;
    if (de__material_is_instance(m))
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "%s: runtime instance 0x%05x is stale", fn, m.id);
    else /* a stale asset handle reads the same: the asset system cannot tell here */
        (void)DE_FAIL(DE_ERR_BUSY, "%s: material 0x%08x is not loaded", fn, m.id);
    return NULL;
}

de_result de_material_set_params(de_renderer *r, de_material m,
                                 const de_standard_material *params) {
    if (params == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_material_set_params: NULL params");
    if (params->alpha_mode > DE_COOKED_ALPHA_BLEND)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_material_set_params: alpha_mode %u (0 opaque, 1 mask, 2 blend)",
                       params->alpha_mode);
    de_render_material *rec = api_record(r, m, "de_material_set_params");
    if (rec == NULL) {
        const de_error *e = de_error_last();
        return e != NULL && e->result != DE_OK ? e->result : DE_ERR_INVALID_ARGUMENT;
    }
    rec->standard = *params;
    rec->alpha_mode = params->alpha_mode;
    rec->flags = (rec->flags & ~(u32)(DE_COOKED_MAT_DOUBLE_SIDED | DE_COOKED_MAT_UNLIT)) |
                 (params->double_sided ? (u32)DE_COOKED_MAT_DOUBLE_SIDED : 0u) |
                 (params->unlit ? (u32)DE_COOKED_MAT_UNLIT : 0u);
    /* The six slots de_standard_material names. */
    rec->textures[DE_COOKED_TEX_BASE_COLOR] = params->base_color_tex;
    rec->textures[DE_COOKED_TEX_METAL_ROUGH] = params->metal_rough_tex;
    rec->textures[DE_COOKED_TEX_NORMAL] = params->normal_tex;
    rec->textures[DE_COOKED_TEX_OCCLUSION] = params->occlusion_tex;
    rec->textures[DE_COOKED_TEX_EMISSIVE] = params->emissive_tex;
    rec->textures[DE_COOKED_TEX_CLEARCOAT_NORMAL] = params->clearcoat_normal_tex;
    de__gpu_scene_material_changed(r->gpu_scene, m);
    return DE_OK;
}

de_result de_material_get_params(de_renderer *r, de_material m, de_standard_material *out) {
    if (out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_material_get_params: NULL out");
    const de_render_material *rec = api_record(r, m, "de_material_get_params");
    if (rec == NULL) {
        memset(out, 0, sizeof *out);
        const de_error *e = de_error_last();
        return e != NULL && e->result != DE_OK ? e->result : DE_ERR_INVALID_ARGUMENT;
    }
    *out = rec->standard;
    return DE_OK;
}

de_material de_material_create_instance(de_renderer *r, de_material base) {
    if (r == NULL) {
        (void)DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_material_create_instance: NULL renderer");
        return (de_material){0};
    }
    return de__material_instances_create_instance(r->material_instances, base);
}

void de_material_destroy_instance(de_renderer *r, de_material m) {
    if (r == NULL || !DE_HANDLE_VALID(m))
        return;
    /* Its block loses READY at the next kick (the slot stays while instances name it). */
    if (de__material_instances_destroy_instance(r->material_instances, m) == DE_OK)
        de__gpu_scene_material_changed(r->gpu_scene, m);
    else
        DE_WARN(r->log, "de_material_destroy_instance: 0x%08x is not a live runtime instance",
                m.id);
}

void de__material_fill_stats(const de_renderer *r, de_render_stats *s) {
    if (r == NULL || s == NULL)
        return;
    const u32 models = de__material_models_count(r->material_models);
    u32 pipelines = 0;
    for (u32 m = 0; m < DE__GS_MATERIAL_MODELS; ++m) {
        s->permutations_per_model[m] = de__material_permutations_compiled(m, m < models);
        pipelines += s->permutations_per_model[m];
    }
    s->material_pipelines = pipelines;
    const de__gpu_scene_stats *gs = de__gpu_scene_stats_get(r->gpu_scene);
    s->material_permutation_switches = gs != NULL ? gs->permutation_switches : 0u;
    s->material_uploads = gs != NULL ? gs->frame_entries[DE__GS_MATERIALS] : 0u;
    s->material_instances = r->material_instances_live;
}
