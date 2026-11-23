/*
 * components.c: reflection and ECS registration of the render components of de_render.h
 * (DENG-178; SPEC §5.1, §9.1, §12.4, §12.7, §20.1).
 *
 * One register function per component (fields, construct hook, type description) and one row per
 * component in k_components, so later sprints append components without touching the others.
 * Every function first looks its type up by name: a registry that several worlds share (or a
 * world registered again) reuses the type and does not grow (the registry frees nothing before it
 * is destroyed). A type of the same name with another layout is someone else's: refused.
 *
 * Asset handles are DE_FK_ASSET (nested_type = the asset type name, for de_ser_context and typed
 * access) or fixed arrays of "asset" (the reflection has no asset type per element, so an element
 * resolves as "any type"). Runtime handles (de_view, de_tonemapper) are u32 fields that are
 * snapshotted but never serialized and hidden from the editor.
 */
#include "render/components_internal.h"

#include "render/renderer_internal.h" /* de_renderer.log */

#include <dengine/de_reflect.h>

#include <math.h>
#include <stddef.h>
#include <string.h>

DE_STATIC_ASSERT(sizeof(de_projection) == 4 && sizeof(de_light_kind) == 4 &&
                     sizeof(de_water_kind) == 4 && sizeof(de_wave_spectrum) == 4,
                 "the render enums are reflected as 4-byte DE_FK_ENUM fields");
DE_STATIC_ASSERT(sizeof(de_view) == 4 && sizeof(de_tonemapper) == 4 && sizeof(de_mesh) == 4 &&
                     sizeof(de_material) == 4 && sizeof(de_texture) == 4 && sizeof(de_blob) == 4,
                 "handles are reflected as 4-byte fields");
/* DENG-822: the tilt lives in the instance's padding; a DeFoliage row still fits a chunk. */
DE_STATIC_ASSERT(sizeof(de_foliage_instance) == 28, "de_foliage_instance is 28 bytes");
DE_STATIC_ASSERT(sizeof(bool) == 1, "bool fields are DE_FK_BOOL (1 byte)");

#define AUTHORED   DE_FIELD_DEFAULT                        /* serialize + snapshot */
#define ASSET_REF  (DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF) /* written as the asset ID */
#define RUNTIME    (DE_FIELD_SNAPSHOT | DE_FIELD_EDITOR_HIDDEN)
#define COMP_FLAGS (DE_COMP_SNAPSHOT | DE_COMP_SERIALIZE)

#define DEG_TO_RAD (DE_PI / 180.0f)
#define HALF_PI    1.5707964f

static de_result last_error_or(de_result fallback) {
    const de_error *e = de_error_last();
    return e != NULL && e->result != DE_OK ? e->result : fallback;
}

/* ---- Registry helpers --------------------------------------------------------------------- */

/* The id of `d->name`: the registered type when its layout matches, or a new registration. */
static de_result ensure_type(de_type_registry *reg, const de_type_desc *d, de_type_id *out) {
    *out = de_type_find(reg, d->name);
    if (DE_HANDLE_VALID(*out)) {
        const de_type_desc *cur = de_type_get(reg, *out);
        if (cur == NULL || cur->size != d->size || cur->align != d->align ||
            cur->version != d->version || cur->field_count != d->field_count)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                           "de_render_register_components: the type registry has another "
                           "type called %s",
                           d->name);
        return DE_OK;
    }
    const de_result r = de_type_register_ex(reg, d, out, NULL);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de_render_register_components: registering %s", d->name);
    return DE_OK;
}

static de_result ensure_enum(de_type_registry *reg, const char *name, const de_enum_value *v,
                             u32 n) {
    const de_type_id id = de_type_find(reg, name);
    if (DE_HANDLE_VALID(id)) {
        const de_enum_info *e = de_enum_get(reg, id);
        if (e == NULL || e->count != n || e->is_flags)
            return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                           "de_render_register_components: the type registry has another "
                           "type called %s",
                           name);
        return DE_OK;
    }
    const de_result r = de_enum_register(reg, name, v, n, false);
    if (r != DE_OK)
        return DE_FAIL_WRAP(r, "de_render_register_components: registering %s", name);
    return DE_OK;
}

/* Registers `d` (with `construct`) and makes it a component of `w`. */
static de_result register_component(de_world *w, de_type_desc d, void (*construct)(void *),
                                    de_component *out) {
    d.hooks.construct = construct;
    de_type_id t;
    DE_TRY(ensure_type(de_world_types(w), &d, &t));
    *out = de_component_register(w, t, COMP_FLAGS);
    if (!DE_HANDLE_VALID(*out))
        return DE_FAIL_WRAP(last_error_or(DE_ERR_INTERNAL),
                            "de_render_register_components: component %s", d.name);
    return DE_OK;
}

static de_result register_enums(de_type_registry *reg) {
    static const de_enum_value k_projection[] = {
        {"DE_PROJ_PERSPECTIVE", DE_PROJ_PERSPECTIVE},
        {"DE_PROJ_ORTHOGRAPHIC", DE_PROJ_ORTHOGRAPHIC},
    };
    static const de_enum_value k_light_kind[] = {
        {"DE_LIGHT_DIRECTIONAL", DE_LIGHT_DIRECTIONAL},
        {"DE_LIGHT_POINT", DE_LIGHT_POINT},
        {"DE_LIGHT_SPOT", DE_LIGHT_SPOT},
        {"DE_LIGHT_RECT", DE_LIGHT_RECT},
        {"DE_LIGHT_DISK", DE_LIGHT_DISK},
    };
    DE_TRY(ensure_enum(reg, "de_projection", k_projection, 2));
    DE_TRY(ensure_enum(reg, "de_light_kind", k_light_kind, 5));
    /* DENG-776: the names the DIDE editor IPC uses (world.water, protocol 1.13). */
    static const de_enum_value k_water_kind[] = {
        {"ocean", DE_WATER_OCEAN},
        {"river", DE_WATER_RIVER},
        {"lake", DE_WATER_LAKE},
    };
    static const de_enum_value k_spectrum[] = {
        {"phillips", DE_WAVE_PHILLIPS},
        {"jonswap", DE_WAVE_JONSWAP},
        {"pierson_moskowitz", DE_WAVE_PIERSON_MOSKOWITZ},
    };
    DE_TRY(ensure_enum(reg, "de_water_kind", k_water_kind, 3));
    DE_TRY(ensure_enum(reg, "de_wave_spectrum", k_spectrum, 3));
    return DE_OK;
}

/* ---- Light intensities ------------------------------------------------------------------- */

f32 de_light_default_intensity(de_light_kind kind) {
    switch (kind) {
    case DE_LIGHT_DIRECTIONAL:
        return 100000.0f; /* lux: direct sunlight */
    case DE_LIGHT_RECT:
    case DE_LIGHT_DISK:
        return 1000.0f; /* nit: a bright diffuse panel */
    case DE_LIGHT_POINT:
    case DE_LIGHT_SPOT:
    default:
        return 800.0f; /* lm: a 60 W incandescent bulb */
    }
}

/* ---- DeCamera ---------------------------------------------------------------------------- */

static void construct_camera(void *obj) {
    DeCamera *c = (DeCamera *)obj;
    memset(c, 0, sizeof *c);
    c->projection = DE_PROJ_PERSPECTIVE;
    c->fov_y = 60.0f * DEG_TO_RAD;
    c->ortho_height = 10.0f;
    c->near_z = 0.1f;
    c->exposure_ev100 = (f32)NAN;
}

static de_result reg_camera(de_world *w, de_component *out) {
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeCamera, projection, DE_FK_ENUM, AUTHORED, {0, 0, 0, 0, "de_projection", 0}),
        DE_FIELD_EX(DeCamera, fov_y, DE_FK_F32, AUTHORED,
                    {0.01, 3.13, "rad", "Vertical field of view (perspective)", 0, 0}),
        DE_FIELD_EX(DeCamera, ortho_height, DE_FK_F32, AUTHORED,
                    {0.001, 100000.0, "m", "Height of the view volume (orthographic)", 0, 0}),
        DE_FIELD_EX(DeCamera, near_z, DE_FK_F32, AUTHORED,
                    {0.001, 1000.0, "m", "Distance to the near plane", 0, 0}),
        DE_FIELD_EX(DeCamera, view, DE_FK_U32, RUNTIME,
                    {0, 0, 0, "Runtime view handle; 0 = the main view", 0, 0}),
        DE_FIELD_EX(DeCamera, exposure_ev100, DE_FK_F32, AUTHORED,
                    {0, 0, "EV100", "Physical exposure; NaN = auto exposure", 0, 0}),
        DE_FIELD_EX(DeCamera, tonemapper, DE_FK_U32, RUNTIME,
                    {0, 0, 0, "Runtime tonemapper handle; 0 = the default", 0, 0}),
        DE_FIELD_EX(
            DeCamera, tonemap_params, DE_FK_FIXED_ARRAY, AUTHORED,
            {0, 0, 0, "Parameters of the camera's tonemapper (meaning per tonemapper)", "f32", 8}),
    };
    return register_component(w, DE_TYPE_DESC(DeCamera, 1, k_fields), construct_camera, out);
}

/* ---- DeMeshRenderer ---------------------------------------------------------------------- */

static void construct_mesh_renderer(void *obj) {
    DeMeshRenderer *m = (DeMeshRenderer *)obj;
    memset(m, 0, sizeof *m);
    m->layer_mask = ~0u;
    m->cast_shadows = true;
    m->visible_in_rt = true;
}

static de_result reg_mesh_renderer(de_world *w, de_component *out) {
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeMeshRenderer, mesh, DE_FK_ASSET, ASSET_REF, {0, 0, 0, 0, "mesh", 0}),
        DE_FIELD_EX(DeMeshRenderer, materials, DE_FK_FIXED_ARRAY, ASSET_REF,
                    {0, 0, 0, "Material per submesh; 0 = the mesh's own material", "asset", 8}),
        DE_FIELD_EX(DeMeshRenderer, layer_mask, DE_FK_U32, AUTHORED,
                    {0, 0, 0, "Render layers (bit set) the mesh is drawn in; ~0 = all", 0, 0}),
        DE_FIELD(DeMeshRenderer, cast_shadows, DE_FK_BOOL, AUTHORED),
        DE_FIELD_EX(DeMeshRenderer, visible_in_rt, DE_FK_BOOL, AUTHORED,
                    {0, 0, 0, "Seen by ray-traced effects (RT tier)", 0, 0}),
    };
    return register_component(w, DE_TYPE_DESC(DeMeshRenderer, 1, k_fields), construct_mesh_renderer,
                              out);
}

/* ---- DeLight ----------------------------------------------------------------------------- */

static void construct_light(void *obj) {
    DeLight *l = (DeLight *)obj;
    memset(l, 0, sizeof *l);
    l->kind = DE_LIGHT_POINT;
    l->color = (de_vec3){1.0f, 1.0f, 1.0f};
    l->intensity = de_light_default_intensity(DE_LIGHT_POINT);
    l->spot_outer = 45.0f * DEG_TO_RAD;
    l->area_size = (de_vec2){1.0f, 1.0f};
    l->source_radius = 0.05f;
    l->cast_shadows = true;
}

static de_result reg_light(de_world *w, de_component *out) {
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeLight, kind, DE_FK_ENUM, AUTHORED, {0, 0, 0, 0, "de_light_kind", 0}),
        DE_FIELD_EX(DeLight, color, DE_FK_VEC3, AUTHORED, {0, 0, 0, "Linear RGB", 0, 0}),
        DE_FIELD_EX(DeLight, intensity, DE_FK_F32, AUTHORED,
                    {0, 0, "lux|lm|nit",
                     "Directional: illuminance in lux. Point and spot: luminous power in lumens. "
                     "Rect and disk: luminance in nits",
                     0, 0}),
        DE_FIELD_EX(DeLight, range, DE_FK_F32, AUTHORED,
                    {0, 0, "m", "Cutoff distance; 0 = where the physical falloff ends", 0, 0}),
        DE_FIELD_EX(DeLight, spot_inner, DE_FK_F32, AUTHORED,
                    {0, HALF_PI, "rad", "Half angle of the full-intensity cone (spot)", 0, 0}),
        DE_FIELD_EX(DeLight, spot_outer, DE_FK_F32, AUTHORED,
                    {0, HALF_PI, "rad", "Half angle where the spot falls to zero", 0, 0}),
        DE_FIELD_EX(DeLight, area_size, DE_FK_VEC2, AUTHORED,
                    {0, 0, "m", "Size of the emitter (rect and disk)", 0, 0}),
        DE_FIELD_EX(DeLight, source_radius, DE_FK_F32, AUTHORED,
                    {0, 0, "m", "Emitter radius; sets the soft-shadow size", 0, 0}),
        DE_FIELD(DeLight, cast_shadows, DE_FK_BOOL, AUTHORED),
        DE_FIELD_EX(
            DeLight, view_mask, DE_FK_U32, AUTHORED,
            {0, 0, 0, "Per-view analytic-light selection; 0 or all bits = every view", 0, 0}),
    };
    return register_component(w, DE_TYPE_DESC(DeLight, 2, k_fields), construct_light, out);
}

/* ---- DeEnvironment ----------------------------------------------------------------------- */

static void construct_environment(void *obj) {
    DeEnvironment *e = (DeEnvironment *)obj;
    memset(e, 0, sizeof *e);
    e->intensity = 1.0f;
}

static de_result reg_environment(de_world *w, de_component *out) {
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeEnvironment, hdri, DE_FK_ASSET, ASSET_REF,
                    {0, 0, 0, "HDR environment; 0 = the procedural sky (DeSky)", "texture", 0}),
        DE_FIELD_EX(DeEnvironment, intensity, DE_FK_F32, AUTHORED,
                    {0, 0, 0, "Multiplier on the environment's radiance", 0, 0}),
        DE_FIELD_EX(DeEnvironment, rotation, DE_FK_F32, AUTHORED,
                    {0, 0, "rad", "Rotation about the up axis (+Z)", 0, 0}),
    };
    return register_component(w, DE_TYPE_DESC(DeEnvironment, 1, k_fields), construct_environment,
                              out);
}

/* ---- DeSprite ---------------------------------------------------------------------------- */

static void construct_sprite(void *obj) {
    DeSprite *s = (DeSprite *)obj;
    memset(s, 0, sizeof *s);
    s->uv_rect = (de_vec4){0.0f, 0.0f, 1.0f, 1.0f};
    s->size = (de_vec2){1.0f, 1.0f};
    s->pivot = (de_vec2){0.5f, 0.5f};
    s->color = (de_vec4){1.0f, 1.0f, 1.0f, 1.0f};
    s->cast_shadows = true;
}

static de_result reg_sprite(de_world *w, de_component *out) {
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeSprite, texture, DE_FK_ASSET, ASSET_REF,
                    {0, 0, 0, "Texture or atlas", "texture", 0}),
        DE_FIELD_EX(DeSprite, uv_rect, DE_FK_VEC4, AUTHORED,
                    {0, 0, 0, "Atlas region in UV space", 0, 0}),
        DE_FIELD_EX(DeSprite, size, DE_FK_VEC2, AUTHORED, {0, 0, "m", 0, 0, 0}),
        DE_FIELD_EX(DeSprite, pivot, DE_FK_VEC2, AUTHORED,
                    {0, 1, 0, "Origin in the sprite, 0..1 on each axis", 0, 0}),
        DE_FIELD_EX(DeSprite, color, DE_FK_COLOR, AUTHORED, {0, 0, 0, "Linear RGBA tint", 0, 0}),
        DE_FIELD_EX(DeSprite, normal_tex, DE_FK_ASSET, ASSET_REF, {0, 0, 0, 0, "texture", 0}),
        DE_FIELD_EX(DeSprite, rough_metal_tex, DE_FK_ASSET, ASSET_REF, {0, 0, 0, 0, "texture", 0}),
        DE_FIELD_EX(DeSprite, emissive_tex, DE_FK_ASSET, ASSET_REF, {0, 0, 0, 0, "texture", 0}),
        DE_FIELD_EX(DeSprite, height_tex, DE_FK_ASSET, ASSET_REF, {0, 0, 0, 0, "texture", 0}),
        DE_FIELD_EX(DeSprite, thickness, DE_FK_F32, AUTHORED,
                    {0, 0, "m", "Depth of the sprite for ray-traced shadows", 0, 0}),
        DE_FIELD_EX(DeSprite, layer, DE_FK_I16, AUTHORED,
                    {0, 0, 0, "Sort layer of translucent sprites", 0, 0}),
        DE_FIELD_EX(DeSprite, order, DE_FK_I16, AUTHORED,
                    {0, 0, 0, "Sort order inside the layer", 0, 0}),
        DE_FIELD(DeSprite, flip_x, DE_FK_BOOL, AUTHORED),
        DE_FIELD(DeSprite, flip_y, DE_FK_BOOL, AUTHORED),
        DE_FIELD(DeSprite, translucent, DE_FK_BOOL, AUTHORED),
        DE_FIELD(DeSprite, cast_shadows, DE_FK_BOOL, AUTHORED),
        DE_FIELD_EX(DeSprite, unlit, DE_FK_BOOL, AUTHORED,
                    {0, 0, 0, "Exact texel colour, no lighting", 0, 0}),
    };
    return register_component(w, DE_TYPE_DESC(DeSprite, 1, k_fields), construct_sprite, out);
}

/* ---- DeSky ------------------------------------------------------------------------------- */

static void construct_sky(void *obj) {
    DeSky *s = (DeSky *)obj;
    memset(s, 0, sizeof *s);
    s->time_of_day_hours = 12.0f;
    s->latitude_deg = 45.0f;
    s->turbidity = 2.0f;
    s->cloud_coverage = 0.5f;
}

static de_result reg_sky(de_world *w, de_component *out) {
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeSky, time_of_day_hours, DE_FK_F32, AUTHORED,
                    {0, 24, "h", "Local solar time", 0, 0}),
        DE_FIELD_EX(DeSky, latitude_deg, DE_FK_F32, AUTHORED, {-90, 90, "deg", 0, 0, 0}),
        DE_FIELD_EX(DeSky, turbidity, DE_FK_F32, AUTHORED,
                    {1, 10, 0, "Haze in the air: 1 = clear, 10 = hazy", 0, 0}),
        DE_FIELD(DeSky, clouds, DE_FK_BOOL, AUTHORED),
        DE_FIELD_EX(DeSky, cloud_coverage, DE_FK_F32, AUTHORED, {0, 1, 0, 0, 0, 0}),
    };
    return register_component(w, DE_TYPE_DESC(DeSky, 1, k_fields), construct_sky, out);
}

/* ---- DeFogVolume ------------------------------------------------------------------------- */

static void construct_fog_volume(void *obj) {
    DeFogVolume *f = (DeFogVolume *)obj;
    memset(f, 0, sizeof *f);
    f->extent = (de_vec3){10.0f, 10.0f, 10.0f};
    f->density = 0.02f;
    f->albedo = (de_vec3){1.0f, 1.0f, 1.0f};
}

static de_result reg_fog_volume(de_world *w, de_component *out) {
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeFogVolume, extent, DE_FK_VEC3, AUTHORED,
                    {0, 0, "m", "Size of the fog box", 0, 0}),
        DE_FIELD_EX(DeFogVolume, density, DE_FK_F32, AUTHORED,
                    {0, 0, "1/m", "Extinction coefficient", 0, 0}),
        DE_FIELD_EX(DeFogVolume, albedo, DE_FK_VEC3, AUTHORED,
                    {0, 0, 0, "Scattering colour (linear RGB)", 0, 0}),
        DE_FIELD_EX(DeFogVolume, anisotropy, DE_FK_F32, AUTHORED,
                    {-1, 1, 0, "Phase function g: < 0 back scattering, > 0 forward", 0, 0}),
    };
    return register_component(w, DE_TYPE_DESC(DeFogVolume, 1, k_fields), construct_fog_volume, out);
}

/* ---- DeDecal ----------------------------------------------------------------------------- */

static void construct_decal(void *obj) {
    DeDecal *d = (DeDecal *)obj;
    memset(d, 0, sizeof *d);
    d->extent = (de_vec3){1.0f, 1.0f, 1.0f};
    d->angle_fade = 1.0f;
}

static de_result reg_decal(de_world *w, de_component *out) {
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeDecal, material, DE_FK_ASSET, ASSET_REF, {0, 0, 0, 0, "material", 0}),
        DE_FIELD_EX(DeDecal, extent, DE_FK_VEC3, AUTHORED,
                    {0, 0, "m", "Size of the projection box", 0, 0}),
        DE_FIELD_EX(DeDecal, angle_fade, DE_FK_F32, AUTHORED,
                    {0, 0, 0, "Fades the decal on surfaces steep to its projection axis", 0, 0}),
        DE_FIELD_EX(DeDecal, sort, DE_FK_I32, AUTHORED,
                    {0, 0, 0, "Order of overlapping decals; higher draws on top", 0, 0}),
    };
    return register_component(w, DE_TYPE_DESC(DeDecal, 1, k_fields), construct_decal, out);
}

/* ---- DeParticleSystem -------------------------------------------------------------------- */

static void construct_particle_system(void *obj) {
    DeParticleSystem *p = (DeParticleSystem *)obj;
    memset(p, 0, sizeof *p);
    p->playing = true;
    p->time_scale = 1.0f;
}

static de_result reg_particle_system(de_world *w, de_component *out) {
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeParticleSystem, effect, DE_FK_ASSET, ASSET_REF,
                    {0, 0, 0, "Effect data (TOML, DENG-2 Q18)", "blob", 0}),
        DE_FIELD(DeParticleSystem, playing, DE_FK_BOOL, AUTHORED),
        DE_FIELD_EX(DeParticleSystem, time_scale, DE_FK_F32, AUTHORED,
                    {0, 0, 0, "Simulation speed; 1 = real time", 0, 0}),
        DE_FIELD(DeParticleSystem, seed, DE_FK_U32, AUTHORED),
    };
    return register_component(w, DE_TYPE_DESC(DeParticleSystem, 1, k_fields),
                              construct_particle_system, out);
}

/* ---- DeTerrain --------------------------------------------------------------------------- */

static void construct_terrain(void *obj) {
    DeTerrain *t = (DeTerrain *)obj;
    memset(t, 0, sizeof *t);
    /* As the DIDE mock engine (mock-engine/src/terrain.rs, protocol 1.11). */
    t->resolution = DE_TERRAIN_RES;
    t->size = 32.0f;
    t->height = 32.0f;
    for (u32 i = 0; i < DE_TERRAIN_RES * DE_TERRAIN_RES; ++i)
        t->splat[i * DE_TERRAIN_LAYERS] = 255;
    memcpy(t->layers[0].name, "Ground", sizeof "Ground");
    for (u32 l = 0; l < DE_TERRAIN_LAYERS; ++l) {
        t->layers[l].tiling = 4.0f;
        t->layers[l].sharpness = 0.5f;
        t->order[l] = (u8)l;
    }
}

/* Version 1 had cell_size and height_scale beside an external heightmap blob that nothing read:
 * they set the size (cell_size per sample step) and the height; the rest takes the defaults. */
static bool migrate_terrain(void *obj, u32 from_version, const void *old_data,
                            const de_field_info *old_fields, u32 old_field_count) {
    DeTerrain *t = (DeTerrain *)obj;
    if (from_version >= 2)
        return true;
    for (u32 i = 0; i < old_field_count; ++i) {
        if (old_fields[i].kind != DE_FK_F32)
            continue;
        f32 v;
        memcpy(&v, (const u8 *)old_data + old_fields[i].offset, sizeof v);
        if (!(v > 0.0f) || v > 1e6f)
            continue;
        if (strcmp(old_fields[i].name, "cell_size") == 0)
            t->size = v * (f32)(DE_TERRAIN_RES - 1);
        else if (strcmp(old_fields[i].name, "height_scale") == 0)
            t->height = v;
    }
    return true;
}

static de_result reg_terrain(de_world *w, de_component *out) {
    static const de_field_info k_layer[] = {
        DE_FIELD_EX(de_terrain_layer, name, DE_FK_STRING, AUTHORED, {0, 0, 0, 0, 0, 0}),
        DE_FIELD_EX(de_terrain_layer, material, DE_FK_ASSET, ASSET_REF,
                    {0, 0, 0, 0, "material", 0}),
        DE_FIELD_EX(de_terrain_layer, tiling, DE_FK_F32, AUTHORED,
                    {0.1, 100, "m", "Metres per texture repeat", 0, 0}),
        DE_FIELD_EX(de_terrain_layer, sharpness, DE_FK_F32, AUTHORED,
                    {0, 1, 0, "Height-blend sharpness", 0, 0}),
    };
    static const de_field_info k_rule[] = {
        DE_FIELD_EX(de_terrain_rule, enabled, DE_FK_BOOL, AUTHORED, {0, 0, 0, 0, 0, 0}),
        DE_FIELD_EX(de_terrain_rule, layer, DE_FK_U8, AUTHORED,
                    {0, DE_TERRAIN_LAYERS - 1, 0, "Layer slot", 0, 0}),
        DE_FIELD_EX(de_terrain_rule, min_slope, DE_FK_F32, AUTHORED, {0, 90, "deg", 0, 0, 0}),
        DE_FIELD_EX(de_terrain_rule, max_slope, DE_FK_F32, AUTHORED, {0, 90, "deg", 0, 0, 0}),
        DE_FIELD_EX(de_terrain_rule, min_height, DE_FK_F32, AUTHORED, {0, 0, "m", 0, 0, 0}),
        DE_FIELD_EX(de_terrain_rule, max_height, DE_FK_F32, AUTHORED, {0, 0, "m", 0, 0, 0}),
        DE_FIELD_EX(de_terrain_rule, min_curvature, DE_FK_F32, AUTHORED, {0, 0, "1/m", 0, 0, 0}),
        DE_FIELD_EX(de_terrain_rule, max_curvature, DE_FK_F32, AUTHORED, {0, 0, "1/m", 0, 0, 0}),
    };
    de_type_id t;
    de_type_desc layer = DE_TYPE_DESC(de_terrain_layer, 1, k_layer);
    de_type_desc rule = DE_TYPE_DESC(de_terrain_rule, 1, k_rule);
    DE_TRY(ensure_type(de_world_types(w), &layer, &t));
    DE_TRY(ensure_type(de_world_types(w), &rule, &t));
#define RO (AUTHORED | DE_FIELD_READONLY)
#define HIDDEN (AUTHORED | DE_FIELD_EDITOR_HIDDEN)
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeTerrain, resolution, DE_FK_U32, RO, {0, 0, 0, "Samples per side", 0, 0}),
        DE_FIELD_EX(DeTerrain, size, DE_FK_F32, RO, {0, 0, "m", "Width and depth", 0, 0}),
        DE_FIELD_EX(DeTerrain, height, DE_FK_F32, RO, {0, 0, "m", "Height of the largest sample",
                                                       0, 0}),
        DE_FIELD_EX(DeTerrain, heights, DE_FK_FIXED_ARRAY, HIDDEN,
                    {0, 0, 0, 0, "u16", DE_TERRAIN_RES * DE_TERRAIN_RES}),
        DE_FIELD_EX(DeTerrain, splat, DE_FK_FIXED_ARRAY, HIDDEN,
                    {0, 0, 0, 0, "u8", DE_TERRAIN_RES * DE_TERRAIN_RES * DE_TERRAIN_LAYERS}),
        DE_FIELD_EX(DeTerrain, holes, DE_FK_FIXED_ARRAY, HIDDEN,
                    {0, 0, 0, 0, "u8", DE_TERRAIN_RES * DE_TERRAIN_RES}),
        DE_FIELD_EX(DeTerrain, layers, DE_FK_FIXED_ARRAY, AUTHORED,
                    {0, 0, 0, "Splat layer slots", "de_terrain_layer", DE_TERRAIN_LAYERS}),
        DE_FIELD_EX(DeTerrain, order, DE_FK_FIXED_ARRAY, AUTHORED,
                    {0, DE_TERRAIN_LAYERS - 1, 0, "Layer slots in blend order, bottom first",
                     "u8", DE_TERRAIN_LAYERS}),
        DE_FIELD_EX(DeTerrain, rules, DE_FK_FIXED_ARRAY, AUTHORED,
                    {0, 0, 0, "Procedural splat rules; later rules win", "de_terrain_rule",
                     DE_TERRAIN_RULES}),
    };
#undef RO
#undef HIDDEN
    de_type_desc d = DE_TYPE_DESC(DeTerrain, 2, k_fields);
    d.hooks.migrate = migrate_terrain;
    return register_component(w, d, construct_terrain, out);
}

/* ---- DeFoliage -------------------------------------------------------------------------- */

static void construct_foliage(void *obj) {
    DeFoliage *fo = (DeFoliage *)obj;
    memset(fo, 0, sizeof *fo);
    /* As the DIDE mock engine (mock-engine/src/foliage.rs, protocol 1.22). */
    for (u32 r = 0; r < DE_FOLIAGE_RULES; ++r) {
        fo->rules[r].layer = -1;
        fo->rules[r].min_weight = 0.5f;
        fo->rules[r].noise_threshold = 0.5f;
        fo->rules[r].density = 0.1f;
    }
}

static de_result reg_foliage(de_world *w, de_component *out) {
    static const de_field_info k_rule[] = {
        DE_FIELD_EX(de_foliage_rule, id, DE_FK_U32, AUTHORED,
                    {0, 0, 0, "Rule id; 0 is an empty slot", 0, 0}),
        DE_FIELD_EX(de_foliage_rule, enabled, DE_FK_BOOL, AUTHORED, {0, 0, 0, 0, 0, 0}),
        DE_FIELD_EX(de_foliage_rule, type, DE_FK_U8, AUTHORED,
                    {0, DE_FOLIAGE_TYPES - 1, 0, "Palette slot", 0, 0}),
        DE_FIELD_EX(de_foliage_rule, terrain, DE_FK_ENTITY, AUTHORED,
                    {0, 0, 0, "Terrain entity to scatter on", 0, 0}),
        DE_FIELD_EX(de_foliage_rule, layer, DE_FK_I32, AUTHORED,
                    {-1, DE_TERRAIN_LAYERS - 1, 0, "Splat layer that gates the rule; -1: any", 0,
                     0}),
        DE_FIELD_EX(de_foliage_rule, min_weight, DE_FK_F32, AUTHORED, {0, 1, 0, 0, 0, 0}),
        DE_FIELD_EX(de_foliage_rule, min_slope, DE_FK_F32, AUTHORED, {0, 90, "deg", 0, 0, 0}),
        DE_FIELD_EX(de_foliage_rule, max_slope, DE_FK_F32, AUTHORED, {0, 90, "deg", 0, 0, 0}),
        DE_FIELD_EX(de_foliage_rule, min_height, DE_FK_F32, AUTHORED, {0, 0, "m", 0, 0, 0}),
        DE_FIELD_EX(de_foliage_rule, max_height, DE_FK_F32, AUTHORED, {0, 0, "m", 0, 0, 0}),
        DE_FIELD_EX(de_foliage_rule, noise_scale, DE_FK_F32, AUTHORED,
                    {0, 0, "m", "Noise feature size; 0: no noise", 0, 0}),
        DE_FIELD_EX(de_foliage_rule, noise_threshold, DE_FK_F32, AUTHORED, {0, 1, 0, 0, 0, 0}),
        DE_FIELD_EX(de_foliage_rule, density, DE_FK_F32, AUTHORED,
                    {0, 100, "1/m2", "Candidates per square metre", 0, 0}),
        DE_FIELD_EX(de_foliage_rule, seed, DE_FK_U32, AUTHORED, {0, 0, 0, 0, 0, 0}),
        DE_FIELD_EX(de_foliage_rule, area_min, DE_FK_VEC2, AUTHORED,
                    {0, 0, "m", "World x and z", 0, 0}),
        DE_FIELD_EX(de_foliage_rule, area_max, DE_FK_VEC2, AUTHORED,
                    {0, 0, "m", "World x and z", 0, 0}),
    };
    static const de_field_info k_cell[] = {
        DE_FIELD(de_foliage_cell, x, DE_FK_I32, AUTHORED),
        DE_FIELD(de_foliage_cell, z, DE_FK_I32, AUTHORED),
        DE_FIELD(de_foliage_cell, count, DE_FK_U32, AUTHORED),
        DE_FIELD(de_foliage_cell, revision, DE_FK_U64, AUTHORED),
    };
    static const de_field_info k_inst[] = {
        DE_FIELD(de_foliage_instance, x, DE_FK_F32, AUTHORED),
        DE_FIELD(de_foliage_instance, y, DE_FK_F32, AUTHORED),
        DE_FIELD(de_foliage_instance, z, DE_FK_F32, AUTHORED),
        DE_FIELD(de_foliage_instance, yaw, DE_FK_F32, AUTHORED),
        DE_FIELD(de_foliage_instance, scale, DE_FK_F32, AUTHORED),
        DE_FIELD(de_foliage_instance, rule, DE_FK_U32, AUTHORED),
        DE_FIELD(de_foliage_instance, type, DE_FK_U8, AUTHORED),
        /* DENG-822: alignToNormal's tilt. Missing in older data: 0, upright. */
        DE_FIELD(de_foliage_instance, nx, DE_FK_I8, AUTHORED),
        DE_FIELD(de_foliage_instance, nz, DE_FK_I8, AUTHORED),
    };
    de_type_id t;
    de_type_desc rule = DE_TYPE_DESC(de_foliage_rule, 1, k_rule);
    de_type_desc cell = DE_TYPE_DESC(de_foliage_cell, 1, k_cell);
    de_type_desc inst = DE_TYPE_DESC(de_foliage_instance, 1, k_inst);
    DE_TRY(ensure_type(de_world_types(w), &rule, &t));
    DE_TRY(ensure_type(de_world_types(w), &cell, &t));
    DE_TRY(ensure_type(de_world_types(w), &inst, &t));
#define HIDDEN (AUTHORED | DE_FIELD_EDITOR_HIDDEN)
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeFoliage, types, DE_FK_FIXED_ARRAY, ASSET_REF,
                    {0, 0, 0, "Foliage type assets; instances name them by index", "asset",
                     DE_FOLIAGE_TYPES}),
        DE_FIELD_EX(DeFoliage, rules, DE_FK_FIXED_ARRAY, AUTHORED,
                    {0, 0, 0, "Scatter rules", "de_foliage_rule", DE_FOLIAGE_RULES}),
        DE_FIELD(DeFoliage, cell_count, DE_FK_U32, HIDDEN),
        DE_FIELD(DeFoliage, instance_count, DE_FK_U32, HIDDEN),
        DE_FIELD_EX(DeFoliage, cells, DE_FK_FIXED_ARRAY, HIDDEN,
                    {0, 0, 0, 0, "de_foliage_cell", DE_FOLIAGE_CELLS}),
        DE_FIELD_EX(DeFoliage, instances, DE_FK_FIXED_ARRAY, HIDDEN,
                    {0, 0, 0, 0, "de_foliage_instance", DE_FOLIAGE_INSTANCES}),
    };
#undef HIDDEN
    return register_component(w, DE_TYPE_DESC(DeFoliage, 1, k_fields), construct_foliage, out);
}

/* ---- DeWater ----------------------------------------------------------------------------- */

static void construct_water(void *obj) {
    DeWater *wt = (DeWater *)obj;
    memset(wt, 0, sizeof *wt);
    /* As the DIDE mock engine (mock-engine/src/water.rs, protocol 1.13). */
    wt->kind = DE_WATER_OCEAN;
    wt->spectrum = DE_WAVE_PIERSON_MOSKOWITZ;
    wt->wind = (de_vec2){5.0f, 0.0f};
    wt->choppiness = 1.0f;
    wt->extent = (de_vec3){64.0f, 8.0f, 64.0f};
    wt->width = 4.0f;
    wt->flow_speed = 1.0f;
    wt->fog_color = (de_vec4){0.05f, 0.2f, 0.25f, 1.0f};
    wt->fog_density = 0.1f;
}

/* Version 1 had `bool ocean` in place of `kind`: false (a river or a lake) loads as a lake, the
 * kind that needs no spline. The other version 1 fields keep their names and are already read. */
static bool migrate_water(void *obj, u32 from_version, const void *old_data,
                          const de_field_info *old_fields, u32 old_field_count) {
    DeWater *wt = (DeWater *)obj;
    if (from_version >= 2)
        return true;
    for (u32 i = 0; i < old_field_count; ++i)
        if (old_fields[i].kind == DE_FK_BOOL && strcmp(old_fields[i].name, "ocean") == 0)
            wt->kind = ((const u8 *)old_data)[old_fields[i].offset] != 0 ? DE_WATER_OCEAN
                                                                         : DE_WATER_LAKE;
    return true;
}

static de_result reg_water(de_world *w, de_component *out) {
    static const de_field_info k_fields[] = {
        DE_FIELD_EX(DeWater, kind, DE_FK_ENUM, AUTHORED, {0, 0, 0, 0, "de_water_kind", 0}),
        DE_FIELD_EX(DeWater, material, DE_FK_ASSET, ASSET_REF, {0, 0, 0, 0, "material", 0}),
        DE_FIELD_EX(DeWater, spectrum, DE_FK_ENUM, AUTHORED,
                    {0, 0, 0, "Ocean wave spectrum", "de_wave_spectrum", 0}),
        DE_FIELD_EX(DeWater, wind, DE_FK_VEC2, AUTHORED,
                    {0, 0, "m/s", "Wind over the water (x, z); its length is the wind speed", 0,
                     0}),
        DE_FIELD_EX(DeWater, choppiness, DE_FK_F32, AUTHORED, {0, 2, 0, 0, 0, 0}),
        DE_FIELD_EX(DeWater, surface_height, DE_FK_F32, AUTHORED,
                    {0, 0, "m", "Surface above the entity", 0, 0}),
        DE_FIELD_EX(DeWater, extent, DE_FK_VEC3, AUTHORED,
                    {0, 0, "m", "A lake's volume, centred on the entity", 0, 0}),
        DE_FIELD_EX(DeWater, spline, DE_FK_ENTITY, AUTHORED,
                    {0, 0, 0, "A river's centre line (a DeSpline entity)", 0, 0}),
        DE_FIELD_EX(DeWater, width, DE_FK_F32, AUTHORED, {0.1, 1000, "m", "A river's width", 0, 0}),
        DE_FIELD_EX(DeWater, flow_speed, DE_FK_F32, AUTHORED,
                    {0, 50, "m/s", "A river's speed at its centre", 0, 0}),
        DE_FIELD_EX(DeWater, fog_color, DE_FK_COLOR, AUTHORED,
                    {0, 0, 0, "Linear RGBA, underwater", 0, 0}),
        DE_FIELD_EX(DeWater, fog_density, DE_FK_F32, AUTHORED,
                    {0, 10, "1/m", "Underwater fog", 0, 0}),
    };
    de_type_desc d = DE_TYPE_DESC(DeWater, 2, k_fields);
    d.hooks.migrate = migrate_water;
    return register_component(w, d, construct_water, out);
}

/* ---- DeReflectionProbe (DENG-340) ------------------------------------------------------- */
static void construct_reflection_probe(void *obj) {
    DeReflectionProbe *p = (DeReflectionProbe *)obj;
    memset(p, 0, sizeof *p);
    p->extent = (de_vec3){5, 5, 5};
    p->blend_distance = 1;
    p->box_projection = true;
    p->resolution = 128;
    p->update_interval = 60;
    p->intensity = 1;
}
static de_result reg_reflection_probe(de_world *w, de_component *out) {
    static const de_enum_value shapes[] = {{"DE_PROBE_BOX", DE_PROBE_BOX},
                                           {"DE_PROBE_SPHERE", DE_PROBE_SPHERE}};
    static const de_enum_value updates[] = {{"DE_PROBE_ONCE", DE_PROBE_ONCE},
                                            {"DE_PROBE_ON_REQUEST", DE_PROBE_ON_REQUEST},
                                            {"DE_PROBE_EVERY_N_FRAMES", DE_PROBE_EVERY_N_FRAMES}};
    DE_TRY(ensure_enum(de_world_types(w), "de_probe_shape", shapes, 2));
    DE_TRY(ensure_enum(de_world_types(w), "de_probe_update", updates, 3));
    static const de_field_info fields[] = {
        DE_FIELD_EX(DeReflectionProbe, shape, DE_FK_ENUM, AUTHORED,
                    {0, 0, 0, 0, "de_probe_shape", 0}),
        DE_FIELD(DeReflectionProbe, extent, DE_FK_VEC3, AUTHORED),
        DE_FIELD(DeReflectionProbe, blend_distance, DE_FK_F32, AUTHORED),
        DE_FIELD(DeReflectionProbe, box_projection, DE_FK_BOOL, AUTHORED),
        DE_FIELD(DeReflectionProbe, resolution, DE_FK_U32, AUTHORED),
        DE_FIELD_EX(DeReflectionProbe, update, DE_FK_ENUM, AUTHORED,
                    {0, 0, 0, 0, "de_probe_update", 0}),
        DE_FIELD(DeReflectionProbe, update_interval, DE_FK_U32, AUTHORED),
        DE_FIELD(DeReflectionProbe, priority, DE_FK_I32, AUTHORED),
        DE_FIELD(DeReflectionProbe, intensity, DE_FK_F32, AUTHORED),
        DE_FIELD_EX(DeReflectionProbe, bake, DE_FK_ASSET, ASSET_REF, {0, 0, 0, 0, "blob", 0}),
    };
    return register_component(w, DE_TYPE_DESC(DeReflectionProbe, 1, fields),
                              construct_reflection_probe, out);
}

static void construct_irradiance_volume(void *obj) {
    DeIrradianceVolume *v = obj;
    memset(v, 0, sizeof *v);
    v->extent = (de_vec3){5, 5, 5};
    v->spacing = 2;
    v->intensity = 1;
    v->blend_distance = 1;
    v->bounces = 2;
}
static de_result reg_irradiance_volume(de_world *w, de_component *out) {
    static const de_field_info fields[] = {
        DE_FIELD(DeIrradianceVolume, extent, DE_FK_VEC3, AUTHORED),
        DE_FIELD(DeIrradianceVolume, spacing, DE_FK_F32, AUTHORED),
        DE_FIELD_EX(DeIrradianceVolume, counts, DE_FK_FIXED_ARRAY, AUTHORED,
                    {0, 0, 0, "Probe counts; all zero derives from spacing", "u32", 3}),
        DE_FIELD(DeIrradianceVolume, intensity, DE_FK_F32, AUTHORED),
        DE_FIELD(DeIrradianceVolume, blend_distance, DE_FK_F32, AUTHORED),
        DE_FIELD(DeIrradianceVolume, priority, DE_FK_I32, AUTHORED),
        DE_FIELD(DeIrradianceVolume, bounces, DE_FK_U32, AUTHORED),
        DE_FIELD_EX(DeIrradianceVolume, bake, DE_FK_ASSET, ASSET_REF, {0, 0, 0, 0, "blob", 0}),
    };
    return register_component(w, DE_TYPE_DESC(DeIrradianceVolume, 1, fields),
                              construct_irradiance_volume, out);
}

/* ---- Registration ------------------------------------------------------------------------ */

/* Registration order = component id order. Append new components at the end. */
static const struct {
    de_result (*fn)(de_world *w, de_component *out);
    size_t offset; /* in DeRenderComponents */
} k_components[] = {
    {reg_camera, offsetof(DeRenderComponents, camera)},
    {reg_mesh_renderer, offsetof(DeRenderComponents, mesh_renderer)},
    {reg_light, offsetof(DeRenderComponents, light)},
    {reg_environment, offsetof(DeRenderComponents, environment)},
    {reg_sprite, offsetof(DeRenderComponents, sprite)},
    {reg_sky, offsetof(DeRenderComponents, sky)},
    {reg_fog_volume, offsetof(DeRenderComponents, fog_volume)},
    {reg_decal, offsetof(DeRenderComponents, decal)},
    {reg_particle_system, offsetof(DeRenderComponents, particle_system)},
    {reg_terrain, offsetof(DeRenderComponents, terrain)},
    {reg_water, offsetof(DeRenderComponents, water)},
    {reg_reflection_probe, offsetof(DeRenderComponents, reflection_probe)},
    {reg_irradiance_volume, offsetof(DeRenderComponents, irradiance_volume)},
    {reg_foliage, offsetof(DeRenderComponents, foliage)},
};

/* The resource type: component ids only, no reflected fields (never serialized). */
static de_result resource_type(de_type_registry *reg, de_type_id *out) {
    de_type_desc d;
    memset(&d, 0, sizeof d);
    d.name = DE__RENDER_COMPONENTS_TYPE_NAME;
    d.size = (u32)sizeof(DeRenderComponents);
    d.align = (u32)DE_ALIGNOF(DeRenderComponents);
    d.version = 1;
    d.is_pod = true;
    return ensure_type(reg, &d, out);
}

const DeRenderComponents *de__render_components(de_world *w) {
    de_type_registry *reg = de_world_types(w);
    if (reg == NULL)
        return NULL;
    const de_type_id t = de_type_find(reg, DE__RENDER_COMPONENTS_TYPE_NAME);
    if (!DE_HANDLE_VALID(t))
        return NULL;
    const de_resource_id id = de_resource_find(w, t);
    return DE_HANDLE_VALID(id) ? (const DeRenderComponents *)de_resource_get(w, id) : NULL;
}

de_result de_render_register_components(de_world *w) {
    if (w == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_render_register_components: world is NULL");
    if (de__render_components(w) != NULL)
        return DE_OK; /* this world has them: nothing changes */
    /* Every rendered entity has a transform, and Extract reads DeGlobalTransform: the built-ins
     * are registered first (they are lazy in the ECS), so prefabs keep their DeTransform too. */
    if (!DE_HANDLE_VALID(de_builtin_components(w).transform))
        return DE_FAIL_WRAP(last_error_or(DE_ERR_INTERNAL),
                            "de_render_register_components: the ECS built-in components");
    de_type_registry *reg = de_world_types(w);
    DE_TRY(register_enums(reg));
    DeRenderComponents ids;
    memset(&ids, 0, sizeof ids);
    for (size_t i = 0; i < sizeof k_components / sizeof k_components[0]; ++i) {
        de_component c = {0};
        DE_TRY(k_components[i].fn(w, &c));
        memcpy((u8 *)&ids + k_components[i].offset, &c, sizeof c);
    }
    de_type_id t;
    DE_TRY(resource_type(reg, &t));
    if (!DE_HANDLE_VALID(de_resource_insert(w, t, &ids)))
        return DE_FAIL_WRAP(
            last_error_or(DE_ERR_INTERNAL),
            "de_render_register_components: inserting " DE__RENDER_COMPONENTS_TYPE_NAME);
    return DE_OK;
}

void de__render_components_attach(de_renderer *r, de_world *w) {
    if (de_render_register_components(w) == DE_OK)
        return;
    const de_error *e = de_error_last();
    DE_ERROR(r->log, "de_renderer_attach: the render components are not registered: %s",
             e != NULL ? e->message : "unknown error");
}
