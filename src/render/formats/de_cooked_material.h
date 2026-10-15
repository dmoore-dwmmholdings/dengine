/*
 * de_cooked_material.h: the cooked standard material format DMAT, version 2 (SPEC
 * §12.6; DENG-101, DENG-302).
 * Internal; include as "render/formats/de_cooked_material.h".
 *
 * A `material` asset holding the parameters of de_standard_material (de_render.h) with texture
 * asset IDs instead of handles. The model importer writes one per source material
 * (`<model>#material:<name>`), the `.mat.toml` importer one per file (DENG-302); the runtime
 * material loader (DENG-105) turns the IDs into handles. Version 1 covers glTF 2.0
 * metallic-roughness and the KHR_materials_* extensions of SPEC §12.6; version 2 (DENG-302) adds
 * the subsurface parameters and the material model.
 *
 * Every field is little-endian (de_cooked_le.h). Layout:
 *
 *   header      DE_COOKED_MATERIAL_HEADER_SIZE (32) bytes:
 *                 0  u8[4] magic "DMAT"
 *                 4  u32   format version (DE_COOKED_MATERIAL_VERSION)
 *                 8  u32   flags (DE_COOKED_MAT_*)
 *                12  u32   alpha mode (de_cooked_alpha_mode)
 *                16  u32   parameter bytes P (DE_COOKED_MATERIAL_PARAMS_SIZE; version 1: 164)
 *                20  u32   texture count T (at most DE_COOKED_TEX_COUNT)
 *                24  u32   total size of the blob (32 + P + 16 T)
 *                28  u32   material model (version 2): 0 = the built-in model the flags select
 *                           (Standard, or Unlit with DE_COOKED_MAT_UNLIT); otherwise
 *                           de_cooked_material_model_hash of the model's name ("standard",
 *                           "unlit", ... or a game model's, de_material_model_register).
 *                           Version 1: reserved, 0.
 *   parameters  P bytes: the f32 fields of de_cooked_material_params, in declaration order
 *               (version 1: the first DE_COOKED_MATERIAL_PARAM_FLOATS_V1 of them; the parser fills
 *               the rest with their defaults).
 *   textures    T x 16: u32 slot (de_cooked_tex_slot), u32 texcoord set (0 or 1), u64 texture
 *               asset ID (never 0). Sorted by slot, each slot once.
 *
 * Bump DE_COOKED_MATERIAL_VERSION on any change. The parser reads versions 1 and 2.
 */
#ifndef DE_RENDER_FORMATS_COOKED_MATERIAL_H
#define DE_RENDER_FORMATS_COOKED_MATERIAL_H

#include "render/formats/de_cooked_le.h"

#include <dengine/de_base.h>

#include <math.h>

DE_EXTERN_C_BEGIN

#define DE_COOKED_MATERIAL_MAGIC       "DMAT"
#define DE_COOKED_MATERIAL_VERSION     2u
#define DE_COOKED_MATERIAL_HEADER_SIZE 32u
#define DE_COOKED_MATERIAL_TEX_SIZE    16u

/** Material flags. The feature bits say which KHR_materials_* extension the source used. */
enum {
    DE_COOKED_MAT_DOUBLE_SIDED = 1u << 0,
    DE_COOKED_MAT_UNLIT = 1u << 1, /**< KHR_materials_unlit */
    DE_COOKED_MAT_EMISSIVE_STRENGTH = 1u << 8,
    DE_COOKED_MAT_IOR = 1u << 9,
    DE_COOKED_MAT_SPECULAR = 1u << 10,
    DE_COOKED_MAT_CLEARCOAT = 1u << 11,
    DE_COOKED_MAT_SHEEN = 1u << 12,
    DE_COOKED_MAT_TRANSMISSION = 1u << 13,
    DE_COOKED_MAT_VOLUME = 1u << 14,
    DE_COOKED_MAT_DIFFUSE_TRANSMISSION = 1u << 15,
    DE_COOKED_MAT_ANISOTROPY = 1u << 16,
    DE_COOKED_MAT_IRIDESCENCE = 1u << 17
};

/** de_standard_material.alpha_mode values. */
typedef enum de_cooked_alpha_mode {
    DE_COOKED_ALPHA_OPAQUE = 0,
    DE_COOKED_ALPHA_MASK = 1,
    DE_COOKED_ALPHA_BLEND = 2
} de_cooked_alpha_mode;

/** Texture slots, in table order. */
typedef enum de_cooked_tex_slot {
    DE_COOKED_TEX_BASE_COLOR = 0,             /**< sRGB colour */
    DE_COOKED_TEX_METAL_ROUGH,                /**< linear: G roughness, B metallic */
    DE_COOKED_TEX_NORMAL,                     /**< tangent-space normal map */
    DE_COOKED_TEX_OCCLUSION,                  /**< linear: R */
    DE_COOKED_TEX_EMISSIVE,                   /**< sRGB colour */
    DE_COOKED_TEX_CLEARCOAT,                  /**< linear: R */
    DE_COOKED_TEX_CLEARCOAT_ROUGHNESS,        /**< linear: G */
    DE_COOKED_TEX_CLEARCOAT_NORMAL,           /**< normal map */
    DE_COOKED_TEX_SHEEN_COLOR,                /**< sRGB colour */
    DE_COOKED_TEX_SHEEN_ROUGHNESS,            /**< linear: A */
    DE_COOKED_TEX_SPECULAR,                   /**< linear: A */
    DE_COOKED_TEX_SPECULAR_COLOR,             /**< sRGB colour */
    DE_COOKED_TEX_TRANSMISSION,               /**< linear: R */
    DE_COOKED_TEX_THICKNESS,                  /**< linear: G */
    DE_COOKED_TEX_DIFFUSE_TRANSMISSION,       /**< linear: A */
    DE_COOKED_TEX_DIFFUSE_TRANSMISSION_COLOR, /**< sRGB colour */
    DE_COOKED_TEX_ANISOTROPY,                 /**< linear: RG direction, B strength */
    DE_COOKED_TEX_IRIDESCENCE,                /**< linear: R */
    DE_COOKED_TEX_IRIDESCENCE_THICKNESS,      /**< linear: G */
    DE_COOKED_TEX_COUNT
} de_cooked_tex_slot;

/**
 * The scalar parameters (glTF factors). Only f32 fields, so there is no padding; the encoding is
 * the fields in this order. de_standard_material has no field yet for occlusion_strength,
 * clearcoat_normal_scale and diffuse_transmission_color (S12 maps them). Version 2 appends the
 * subsurface fields (DENG-302); version 1 ends at alpha_cutoff.
 */
typedef struct de_cooked_material_params {
    f32 base_color[4]; /**< linear RGBA */
    f32 metallic, roughness;
    f32 normal_scale;
    f32 occlusion_strength;
    f32 emissive[3]; /**< linear RGB */
    f32 emissive_strength;
    f32 ior;
    f32 specular;
    f32 specular_color[3];
    f32 clearcoat, clearcoat_roughness, clearcoat_normal_scale;
    f32 sheen_color[3];
    f32 sheen_roughness;
    f32 transmission;
    f32 thickness;
    f32 attenuation_color[3];
    f32 attenuation_distance; /**< +inf = no attenuation */
    f32 diffuse_transmission;
    f32 diffuse_transmission_color[3];
    f32 anisotropy, anisotropy_rotation;
    f32 iridescence, iridescence_ior, iridescence_thickness_min, iridescence_thickness_max;
    f32 alpha_cutoff;
    /* Version 2 (DENG-302): */
    f32 subsurface;
    f32 subsurface_radius[3]; /**< metres per channel */
} de_cooked_material_params;

#define DE_COOKED_MATERIAL_PARAM_FLOATS    45u
#define DE_COOKED_MATERIAL_PARAM_FLOATS_V1 41u
#define DE_COOKED_MATERIAL_PARAMS_SIZE     (DE_COOKED_MATERIAL_PARAM_FLOATS * 4u)
DE_STATIC_ASSERT(sizeof(de_cooked_material_params) == DE_COOKED_MATERIAL_PARAMS_SIZE,
                 "de_cooked_material_params must be packed f32 fields");

/** The glTF 2.0 defaults (core spec and each extension's defaults). */
static inline de_cooked_material_params de_cooked_material_params_default(void) {
    de_cooked_material_params m;
    memset(&m, 0, sizeof m);
    for (u32 i = 0; i < 4; ++i)
        m.base_color[i] = 1.0f;
    m.metallic = 1.0f;
    m.roughness = 1.0f;
    m.normal_scale = 1.0f;
    m.occlusion_strength = 1.0f;
    m.emissive_strength = 1.0f;
    m.ior = 1.5f;
    m.specular = 1.0f;
    m.clearcoat_normal_scale = 1.0f;
    for (u32 i = 0; i < 3; ++i) {
        m.specular_color[i] = 1.0f;
        m.attenuation_color[i] = 1.0f;
        m.diffuse_transmission_color[i] = 1.0f;
    }
    m.attenuation_distance = INFINITY;
    m.iridescence_ior = 1.3f;
    m.iridescence_thickness_min = 100.0f;
    m.iridescence_thickness_max = 400.0f;
    m.alpha_cutoff = 0.5f;
    for (u32 i = 0; i < 3; ++i)
        m.subsurface_radius[i] = 1.0f;
    return m;
}

/** The value of header word 28 for model `name` (DENG-302): the low 32 bits of FNV-1a 64 over
 * the name's bytes, never 0 (0 becomes 1). Pure. */
static inline u32 de_cooked_material_model_hash(const char *name) {
    u64 h = 0xcbf29ce484222325ull;
    for (const unsigned char *p = (const unsigned char *)name; p != NULL && *p != 0; ++p) {
        h ^= (u64)*p;
        h *= 0x100000001b3ull;
    }
    const u32 v = (u32)h;
    return v != 0 ? v : 1u;
}

/** One texture reference of a material. */
typedef struct de_cooked_material_tex {
    u32 slot;     /**< de_cooked_tex_slot */
    u32 texcoord; /**< 0 or 1 */
    u64 asset_id;
} de_cooked_material_tex;

/** A checked, decoded DMAT blob. */
typedef struct de_cooked_material_view {
    u32 version; /**< 1 or 2 */
    u32 flags;
    u32 alpha_mode;
    u32 model_hash; /**< header word 28: 0, or de_cooked_material_model_hash of the model name */
    de_cooked_material_params params;
    u32 texture_count;
    de_cooked_material_tex textures[DE_COOKED_TEX_COUNT];
} de_cooked_material_view;

/**
 * Checks and decodes a DMAT blob: magic, version, sizes, alpha mode, texture slots (known,
 * sorted, unique), texcoord sets, IDs. Allocation-free. False on any error, with a short reason in
 * `*why` (nullable).
 */
static inline bool de_cooked_material_parse(const void *data, u64 size,
                                            de_cooked_material_view *out, const char **why) {
    const char *w = NULL;
    const u8 *p = (const u8 *)data;
    memset(out, 0, sizeof *out);
    if (p == NULL || size < DE_COOKED_MATERIAL_HEADER_SIZE) {
        w = "shorter than the header";
        goto fail;
    }
    if (memcmp(p, DE_COOKED_MATERIAL_MAGIC, 4) != 0) {
        w = "not a cooked material (magic)";
        goto fail;
    }
    out->version = de_cooked_get_u32(p + 4);
    if (out->version != 1u && out->version != DE_COOKED_MATERIAL_VERSION) {
        w = "unsupported format version";
        goto fail;
    }
    const u32 nfloats =
        out->version == 1u ? DE_COOKED_MATERIAL_PARAM_FLOATS_V1 : DE_COOKED_MATERIAL_PARAM_FLOATS;
    out->model_hash = out->version == 1u ? 0u : de_cooked_get_u32(p + 28);
    out->flags = de_cooked_get_u32(p + 8);
    out->alpha_mode = de_cooked_get_u32(p + 12);
    u32 psize = de_cooked_get_u32(p + 16);
    out->texture_count = de_cooked_get_u32(p + 20);
    u32 total = de_cooked_get_u32(p + 24);
    if (out->alpha_mode > DE_COOKED_ALPHA_BLEND || psize != nfloats * 4u ||
        out->texture_count > DE_COOKED_TEX_COUNT ||
        (u64)total != (u64)DE_COOKED_MATERIAL_HEADER_SIZE + psize +
                          (u64)out->texture_count * DE_COOKED_MATERIAL_TEX_SIZE ||
        (u64)total != size) {
        w = "bad sizes or alpha mode";
        goto fail;
    }
    out->params = de_cooked_material_params_default(); /* version 1: the v2 fields' defaults */
    f32 f[DE_COOKED_MATERIAL_PARAM_FLOATS];
    memcpy(f, &out->params, sizeof f);
    for (u32 i = 0; i < nfloats; ++i)
        f[i] = de_cooked_get_f32(p + DE_COOKED_MATERIAL_HEADER_SIZE + 4u * i);
    memcpy(&out->params, f, sizeof f);
    const u8 *t = p + DE_COOKED_MATERIAL_HEADER_SIZE + psize;
    for (u32 i = 0; i < out->texture_count; ++i, t += DE_COOKED_MATERIAL_TEX_SIZE) {
        de_cooked_material_tex *x = &out->textures[i];
        x->slot = de_cooked_get_u32(t);
        x->texcoord = de_cooked_get_u32(t + 4);
        x->asset_id = de_cooked_get_u64(t + 8);
        if (x->slot >= DE_COOKED_TEX_COUNT || (i > 0 && x->slot <= out->textures[i - 1].slot) ||
            x->texcoord > 1u || x->asset_id == 0) {
            w = "bad texture record";
            goto fail;
        }
    }
    return true;
fail:
    memset(out, 0, sizeof *out);
    if (why != NULL)
        *why = w;
    return false;
}

/** The texture of `slot` in a decoded material, or NULL. */
static inline const de_cooked_material_tex *
de_cooked_material_texture(const de_cooked_material_view *v, u32 slot) {
    for (u32 i = 0; i < v->texture_count; ++i)
        if (v->textures[i].slot == slot)
            return &v->textures[i];
    return NULL;
}

DE_EXTERN_C_END

#endif /* DE_RENDER_FORMATS_COOKED_MATERIAL_H */
