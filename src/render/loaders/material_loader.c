/*
 * material_loader.c: the runtime `material` type (SPEC §12.6; DENG-105). Parses DMAT
 * (render/formats/de_cooked_material.h, DENG-101) into a CPU de_standard_material. Each texture
 * the material names is a dependency (de_asset_load_ctx_add_dependency), so the material is READY
 * only once its textures are, and fails with the first texture that fails (S07 fan-out). The
 * `ready` hook turns the texture IDs into de_texture handles: typed access is main-thread work,
 * and by then every texture has its slot. The GPU material tables are S12/S14.
 */

#include "render/loaders/loaders_internal.h"

#include "render/formats/de_cooked_material.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_math.h>
#include <dengine/de_render.h>

#include <string.h>

static de_vec3 vec3(const f32 v[3]) {
    de_vec3 r = {v[0], v[1], v[2]};
    return r;
}

/* The cooked parameters into de_standard_material (the textures come in `ready`). */
static void standard_from(de_standard_material *s, const de_cooked_material_view *v) {
    const de_cooked_material_params *p = &v->params;
    memset(s, 0, sizeof *s);
    s->base_color.x = p->base_color[0];
    s->base_color.y = p->base_color[1];
    s->base_color.z = p->base_color[2];
    s->base_color.w = p->base_color[3];
    s->metallic = p->metallic;
    s->roughness = p->roughness;
    s->normal_scale = p->normal_scale;
    s->emissive = vec3(p->emissive);
    s->emissive_strength = p->emissive_strength;
    s->ior = p->ior;
    s->specular = p->specular;
    s->specular_color = vec3(p->specular_color);
    s->clearcoat = p->clearcoat;
    s->clearcoat_roughness = p->clearcoat_roughness;
    s->sheen_color = vec3(p->sheen_color);
    s->sheen_roughness = p->sheen_roughness;
    s->transmission = p->transmission;
    s->thickness = p->thickness;
    s->attenuation_color = vec3(p->attenuation_color);
    s->attenuation_distance = p->attenuation_distance;
    s->diffuse_transmission = p->diffuse_transmission;
    s->anisotropy = p->anisotropy;
    s->anisotropy_rotation = p->anisotropy_rotation;
    s->iridescence = p->iridescence;
    s->iridescence_ior = p->iridescence_ior;
    s->iridescence_thickness_min = p->iridescence_thickness_min;
    s->iridescence_thickness_max = p->iridescence_thickness_max;
    s->alpha_cutoff = p->alpha_cutoff;
    s->alpha_mode = v->alpha_mode;
    s->double_sided = (v->flags & DE_COOKED_MAT_DOUBLE_SIDED) != 0;
    s->unlit = (v->flags & DE_COOKED_MAT_UNLIT) != 0;
}

de_result de__material_load(de_asset_load_ctx *ctx) {
    de_render_uploader *u = de__loader_uploader(ctx);
    if (u == NULL)
        return DE_ERR_INTERNAL;
    u64 size = 0;
    const void *p = de_asset_load_ctx_data(ctx, &size);
    de_cooked_material_view v;
    const char *why = NULL;
    if (!de_cooked_material_parse(p, size, &v, &why))
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "not a cooked material: %s", why);
    for (u32 i = 0; i < v.texture_count; ++i) {
        de_result r = de_asset_load_ctx_add_dependency(ctx, (de_asset_id){v.textures[i].asset_id});
        if (r != DE_OK)
            return de_asset_load_ctx_fail(ctx, r, "cannot declare its textures");
    }
    de_render_material *m = (de_render_material *)DE_ALLOC(
        u->alloc, sizeof *m, DE_ALIGNOF(de_render_material), DE_MEM_RENDER);
    if (m == NULL)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no memory for the material");
    memset(m, 0, sizeof *m);
    standard_from(&m->standard, &v);
    m->flags = v.flags;
    m->alpha_mode = v.alpha_mode;
    m->params = v.params;
    for (u32 i = 0; i < v.texture_count; ++i) {
        const de_cooked_material_tex *t = &v.textures[i];
        m->texture_ids[t->slot].hash = t->asset_id;
        m->texcoord[t->slot] = (u8)t->texcoord;
    }
    de_asset_load_ctx_set_object(ctx, m, sizeof *m);
    return DE_OK;
}

void de__material_unload(void *object, void *user) {
    de_render_uploader *u = (de_render_uploader *)user;
    if (object != NULL)
        DE_FREE(u->alloc, object, sizeof(de_render_material));
}

/* Main thread, with every texture READY: IDs to handles. */
de_result de__material_ready(void *object, void *user) {
    de_render_uploader *u = (de_render_uploader *)user;
    de_render_material *m = (de_render_material *)object;
    for (u32 s = 0; s < DE_COOKED_TEX_COUNT; ++s)
        m->textures[s] = m->texture_ids[s].hash != 0
                             ? de_asset_texture(u->assets, m->texture_ids[s])
                             : (de_texture){0};
    de_standard_material *st = &m->standard;
    st->base_color_tex = m->textures[DE_COOKED_TEX_BASE_COLOR];
    st->metal_rough_tex = m->textures[DE_COOKED_TEX_METAL_ROUGH];
    st->normal_tex = m->textures[DE_COOKED_TEX_NORMAL];
    st->occlusion_tex = m->textures[DE_COOKED_TEX_OCCLUSION];
    st->emissive_tex = m->textures[DE_COOKED_TEX_EMISSIVE];
    st->clearcoat_normal_tex = m->textures[DE_COOKED_TEX_CLEARCOAT_NORMAL];
    for (u32 s = 0; s < DE_COOKED_TEX_COUNT; ++s)
        if (m->texture_ids[s].hash != 0 && !DE_HANDLE_VALID(m->textures[s]))
            return DE_FAIL_WRAP(DE_ERR_NOT_FOUND, "texture %016llx of the material has no handle",
                                (unsigned long long)m->texture_ids[s].hash);
    return DE_OK;
}
