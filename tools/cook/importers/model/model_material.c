/*
 * model_material.c: materials of the model importer (DENG-101, SPEC §12.6): how each texture slot
 * is sampled (the settings of its texture sub-asset) and the DMAT writer
 * (render/formats/de_cooked_material.h).
 *
 * Texture usage per slot (the issue's rule):
 *   - colour, sRGB: base colour, emissive, sheen colour, specular colour, diffuse transmission
 *     colour → `srgb = true`, `usage = "color"`;
 *   - normal maps: normal, clearcoat normal → `srgb = false`, `usage = "normal"`;
 *   - everything else (metallic-roughness, occlusion, clearcoat, transmission, thickness, ...) is
 *     linear data → `srgb = false`, `usage = "mask"`.
 */
#include "model_internal.h"

#include "render/formats/de_cooked_material.h"

#include <string.h>

de_model_tex_usage de_model_tex_usage_of_slot(u32 slot) {
    switch (slot) {
    case DE_COOKED_TEX_BASE_COLOR:
    case DE_COOKED_TEX_EMISSIVE:
    case DE_COOKED_TEX_SHEEN_COLOR:
    case DE_COOKED_TEX_SPECULAR_COLOR:
    case DE_COOKED_TEX_DIFFUSE_TRANSMISSION_COLOR:
        return DE_MODEL_TEX_COLOR;
    case DE_COOKED_TEX_NORMAL:
    case DE_COOKED_TEX_CLEARCOAT_NORMAL:
        return DE_MODEL_TEX_NORMAL;
    default:
        return DE_MODEL_TEX_LINEAR;
    }
}

bool de_model_tex_usage_srgb(de_model_tex_usage u) {
    return u == DE_MODEL_TEX_COLOR;
}

const char *de_model_tex_usage_name(de_model_tex_usage u) {
    switch (u) {
    case DE_MODEL_TEX_COLOR:
        return "color";
    case DE_MODEL_TEX_NORMAL:
        return "normal";
    case DE_MODEL_TEX_LINEAR:
        return "mask";
    }
    return "mask";
}

void de_model_material_write(de_model_buf *b, const de_model_material *m,
                             const de_asset_id *texture_ids) {
    u32 tex_count = 0;
    for (u32 s = 0; s < DE_COOKED_TEX_COUNT; ++s)
        if (m->tex[s].texture >= 0)
            ++tex_count;
    de_model_buf_put(b, DE_COOKED_MATERIAL_MAGIC, 4);
    de_model_buf_u32(b, DE_COOKED_MATERIAL_VERSION);
    de_model_buf_u32(b, m->flags);
    de_model_buf_u32(b, m->alpha_mode);
    de_model_buf_u32(b, DE_COOKED_MATERIAL_PARAMS_SIZE);
    de_model_buf_u32(b, tex_count);
    de_model_buf_u32(b, DE_COOKED_MATERIAL_HEADER_SIZE + DE_COOKED_MATERIAL_PARAMS_SIZE +
                            tex_count * DE_COOKED_MATERIAL_TEX_SIZE);
    de_model_buf_u32(b, 0);
    f32 f[DE_COOKED_MATERIAL_PARAM_FLOATS];
    memcpy(f, &m->params, sizeof f);
    for (u32 i = 0; i < DE_COOKED_MATERIAL_PARAM_FLOATS; ++i)
        de_model_buf_f32(b, f[i]);
    for (u32 s = 0; s < DE_COOKED_TEX_COUNT; ++s) {
        if (m->tex[s].texture < 0)
            continue;
        de_model_buf_u32(b, s);
        de_model_buf_u32(b, m->tex[s].texcoord);
        de_model_buf_u64(b, texture_ids[m->tex[s].texture].hash);
    }
}
