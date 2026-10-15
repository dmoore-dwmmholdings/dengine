/*
 * material_params.h: the material parameter block, the C mirror of
 * shaders/materials/material_params.slang (SPEC §12.6; DENG-302), the material feature derivation
 * and the resolve permutations. Internal to de_render; include as
 * "render/materials/material_params.h".
 *
 * One block per material instance (an asset material, or a runtime instance made with
 * de_material_create_instance) in the GPU scene's material table: one bindless storage buffer of
 * DE__MATERIAL_PARAMS_SIZE-byte entries (render/scene/gpu_scene.h, table DE__GS_MATERIALS).
 *
 *   offset   0  uint   model       the material model id (classification and the resolve read it
 *                                  here: DENG-237, DENG-298)
 *            4  uint   features    DE__MATERIAL_FEATURE_* the parameter values use
 *                                  (de__material_features); they pick the resolve permutation
 *            8  uint   flags       DE__MATERIAL_FLAG_*
 *           12  uint   alpha_mode  de_cooked_alpha_mode
 *           16  float4 x 10        the Standard parameters (de_standard_material, rows as named)
 *          176  uint4 x 5          per de_cooked_tex_slot (19 words): its sampled bindless index
 *                                  (0 = none) | DE__MATERIAL_TEXTURE_UV1 when it samples UV1; the
 *                                  20th word (textures[4].v[3]) the diffuse transmission colour as
 *                                  1 - colour in unorm 10:10:10 (r in bits 0-9), so a zeroed block
 *                                  has the KHR default, white
 *
 * The single definition is the X-macro DE__MATERIAL_PARAMS_FIELDS. It declares the C struct and the
 * field table (de__material_params_layout) that the test material_params_layout_match compares
 * with the struct it parses from the Slang file.
 */
#ifndef DE_RENDER_MATERIAL_PARAMS_H
#define DE_RENDER_MATERIAL_PARAMS_H

#include <dengine/de_base.h>

#include "render/materials/material_models.h" /* DE__MATERIAL_FEATURE_* */

#include <stddef.h>

DE_EXTERN_C_BEGIN

/* The 4-byte field types of the GPU tables (render/scene/gpu_scene.h uses them too). Each is
 * 4-byte aligned, so the C layout is the packed one; 16-byte types sit at multiples of 16
 * (checked below), where std430, Metal and D3D12 put them. */
typedef u32 de__gs_uint;
typedef f32 de__gs_float;
typedef struct de__gs_uint4 {
    u32 v[4];
} de__gs_uint4;
typedef struct de__gs_float4 {
    f32 v[4];
} de__gs_float4;

/** DeGpuMaterial.flags (DE_GPU_MATERIAL_*). */
enum {
    DE__MATERIAL_FLAG_READY = 1u, /**< the material is loaded; the block is valid */
    DE__MATERIAL_FLAG_DOUBLE_SIDED = 2u,
    DE__MATERIAL_FLAG_UNLIT = 4u
};
/** Texture slots of a block (DE_GPU_MATERIAL_TEXTURES = DE_COOKED_TEX_COUNT). */
#define DE__MATERIAL_TEXTURES 19u
/** The bit of a texture word set when the slot samples UV1 (DE_GPU_MATERIAL_TEXTURE_UV1). */
#define DE__MATERIAL_TEXTURE_UV1 0x80000000u
/** The word of `textures` that holds the diffuse transmission colour (1 - colour, unorm 10:10:10).
 */
#define DE__MATERIAL_DT_COLOR_WORD 19u
/** Bytes per block (the table stride). */
#define DE__MATERIAL_PARAMS_SIZE 256u

/* F(c_type, slang_type, name, array_dims) */
#define DE__MATERIAL_PARAMS_FIELDS(F)                                                              \
    F(de__gs_uint, uint, model, )                                                                  \
    F(de__gs_uint, uint, features, )                                                               \
    F(de__gs_uint, uint, flags, )                                                                  \
    F(de__gs_uint, uint, alpha_mode, )                                                             \
    F(de__gs_float4, float4, base_color, )                                                         \
    F(de__gs_float4, float4, emissive, )                                                           \
    F(de__gs_float4, float4, pbr, )                                                                \
    F(de__gs_float4, float4, specular, )                                                           \
    F(de__gs_float4, float4, clearcoat, )                                                          \
    F(de__gs_float4, float4, sheen, )                                                              \
    F(de__gs_float4, float4, volume, )                                                             \
    F(de__gs_float4, float4, extra, )                                                              \
    F(de__gs_float4, float4, iridescence, )                                                        \
    F(de__gs_float4, float4, subsurface, )                                                         \
    F(de__gs_uint4, uint4, textures, [5])

#define DE__MATERIAL_DECLARE_FIELD(ctype, stype, name, dims) ctype name dims;

/** DeGpuMaterial (256 bytes). */
typedef struct de__material_params {
    DE__MATERIAL_PARAMS_FIELDS(DE__MATERIAL_DECLARE_FIELD)
} de__material_params;

DE_STATIC_ASSERT(sizeof(de__material_params) == DE__MATERIAL_PARAMS_SIZE,
                 "DeGpuMaterial is 256 bytes");
DE_STATIC_ASSERT(offsetof(de__material_params, model) == 0 &&
                     offsetof(de__material_params, features) == 4 &&
                     offsetof(de__material_params, flags) == 8 &&
                     offsetof(de__material_params, alpha_mode) == 12 &&
                     offsetof(de__material_params, base_color) == 16 &&
                     offsetof(de__material_params, subsurface) == 160 &&
                     offsetof(de__material_params, textures) == 176,
                 "DeGpuMaterial layout");
DE_STATIC_ASSERT(DE__MATERIAL_DT_COLOR_WORD < 20u &&
                     DE__MATERIAL_TEXTURES <= DE__MATERIAL_DT_COLOR_WORD,
                 "texture words");

/** The bindless index of texture slot `s` of `p` (0 = none). */
static inline u32 de__material_params_texture(const de__material_params *p, u32 s) {
    return p->textures[s / 4u].v[s % 4u] & ~DE__MATERIAL_TEXTURE_UV1;
}
/** The UV set (0 or 1) texture slot `s` of `p` samples. */
static inline u32 de__material_params_texcoord(const de__material_params *p, u32 s) {
    return p->textures[s / 4u].v[s % 4u] >> 31;
}
/** The diffuse transmission colour word of `p`. */
static inline u32 de__material_params_dt_color(const de__material_params *p) {
    return p->textures[DE__MATERIAL_DT_COLOR_WORD / 4u].v[DE__MATERIAL_DT_COLOR_WORD % 4u];
}
/** 1 - colour in unorm 10:10:10, the encoding of the diffuse transmission colour word (each
 * channel clamped to [0, 1]; NaN reads as 1). */
static inline u32 de__material_dt_color_pack(f32 r, f32 g, f32 b) {
    const f32 c[3] = {r, g, b};
    u32 w = 0;
    for (u32 i = 0; i < 3; ++i) {
        f32 v = c[i] >= 0.0f ? (c[i] <= 1.0f ? c[i] : 1.0f) : (c[i] < 0.0f ? 0.0f : 1.0f);
        w |= (u32)((1.0f - v) * 1023.0f + 0.5f) << (10u * i);
    }
    return w;
}

/** One field of the block, for the layout check. */
typedef struct de__material_layout_field {
    const char *name;
    const char *slang_type; /**< "uint", "float", "uint4", "float4" */
    u32 offset, size;       /**< bytes; size covers the whole array */
} de__material_layout_field;
/** The fields of DeGpuMaterial in order. */
const de__material_layout_field *de__material_params_layout(u32 *count);

/** A `public static const uint` of material_params.slang and its C value. */
typedef struct de__material_layout_const {
    const char *name;
    u32 value;
} de__material_layout_const;
const de__material_layout_const *de__material_params_consts(u32 *count);

/* =================================================================================================
 * Features and permutations
 * ============================================================================================== */

struct de_standard_material;

/**
 * The DE__MATERIAL_FEATURE_* (render/materials/material_models.h) of a material with parameters
 * `s`, alpha mode `alpha_mode` (de_cooked_alpha_mode) and model `model`. The rules match the
 * shader's (standard.slang and the layers): CLEARCOAT clearcoat > 0; SHEEN a sheen colour channel
 * > 0; ANISOTROPY anisotropy > 0; IRIDESCENCE iridescence > 0 with a thickness > 0;
 * TRANSMISSION transmission > 0; DIFFUSE_TRANSMISSION > 0; SUBSURFACE > 0;
 * UNLIT the Unlit model; FORWARD alpha blend or transmission > 0; DOUBLE_SIDED; ALPHA_MASK alpha
 * mask. TRANSMISSION, VOLUME (thickness > 0 with transmission or diffuse transmission > 0),
 * DIFFUSE_TRANSMISSION and FORWARD follow de_transmission_set_features (DENG-301,
 * lobe_transmission.slang). A NaN never sets a bit. Pure; any thread.
 */
u32 de__material_features(const struct de_standard_material *s, u32 alpha_mode, u32 model);

/** The feature bits a permutation compiles in or out (StandardModelT<FEATURES>: the layers and
 * lobes). The others (UNLIT, FORWARD, DOUBLE_SIDED, ALPHA_MASK) never pick a permutation. */
#define DE__MATERIAL_PERMUTATION_FEATURES                                                          \
    (DE__MATERIAL_FEATURE_CLEARCOAT | DE__MATERIAL_FEATURE_SHEEN |                                 \
     DE__MATERIAL_FEATURE_ANISOTROPY | DE__MATERIAL_FEATURE_IRIDESCENCE |                          \
     DE__MATERIAL_FEATURE_TRANSMISSION | DE__MATERIAL_FEATURE_VOLUME |                             \
     DE__MATERIAL_FEATURE_DIFFUSE_TRANSMISSION | DE__MATERIAL_FEATURE_SUBSURFACE)

/** Most resolve permutations of one material model. */
#define DE__MATERIAL_PERMUTATIONS_MAX 4u

/** One compiled permutation of a model's resolve kernel. */
typedef struct de__material_permutation {
    u32 features;      /**< the DE__MATERIAL_FEATURE_* its lobes cover (UINT32_MAX: all) */
    const char *entry; /**< the entry point of the engine shader vis_resolve */
} de__material_permutation;

/**
 * The resolve permutations of built-in model `model`, smallest first; each one's feature set
 * contains the one before it (a chain), so a set of materials needs the largest of their
 * permutations. The Standard model (and Sprite, Terrain and Water, which resolve as Standard until
 * their sprints) has base (no layer), CLEARCOAT and all features; Unlit has one. NULL (and 0) for
 * a game model's id: a game model brings one kernel (DE__MATERIAL_MODEL_RESOLVE_ENTRY). Pure.
 */
const de__material_permutation *de__material_permutations(u32 model, u32 *count);

/** How many permutations model `model` compiles itself: the built-in table's count, except that
 * Sprite, Terrain and Water share Standard's kernels (0); a game model 1 when registered. */
u32 de__material_permutations_compiled(u32 model, bool registered);

/** The index of the smallest permutation of `model` whose features contain `features` (the last
 * one when none does); 0 for a game model. Pure. */
u32 de__material_permutation_of(u32 model, u32 features);

DE_EXTERN_C_END

#endif /* DE_RENDER_MATERIAL_PARAMS_H */
