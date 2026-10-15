/*
 * mat_toml_import.c: `.mat.toml` materials to DMAT (mat_toml_import.h; DENG-302).
 */
#include "cook/importers/material/mat_toml_import.h"

#include "core/de_toml_internal.h"
#include "render/formats/de_cooked_material.h"

#include <dengine/de_debug.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

de_asset_type_desc de_mat_toml_import_type_desc(void) {
    de_asset_type_desc t;
    memset(&t, 0, sizeof t);
    t.name = "material";
    t.importer_version = DE_MAT_TOML_IMPORTER_VERSION;
    t.import = de_mat_toml_import;
    return t;
}

/* ---- Keys ------------------------------------------------------------------------------------ */

typedef enum mt_kind { MT_FLOAT, MT_VEC3, MT_COLOR4 } mt_kind;

/* A parameter key: where it goes in de_cooked_material_params, and the KHR extension it uses. */
typedef struct mt_param {
    const char *key;
    mt_kind kind;
    size_t offset;
    u32 ext; /* DE_COOKED_MAT_* feature bit, 0 = core glTF */
} mt_param;

#define P(key, kind, field, ext) {key, kind, offsetof(de_cooked_material_params, field), ext}
static const mt_param k_params[] = {
    P("base_color", MT_COLOR4, base_color, 0),
    P("metallic", MT_FLOAT, metallic, 0),
    P("roughness", MT_FLOAT, roughness, 0),
    P("normal_scale", MT_FLOAT, normal_scale, 0),
    P("occlusion_strength", MT_FLOAT, occlusion_strength, 0),
    P("emissive", MT_VEC3, emissive, 0),
    P("emissive_strength", MT_FLOAT, emissive_strength, DE_COOKED_MAT_EMISSIVE_STRENGTH),
    P("ior", MT_FLOAT, ior, DE_COOKED_MAT_IOR),
    P("specular", MT_FLOAT, specular, DE_COOKED_MAT_SPECULAR),
    P("specular_color", MT_VEC3, specular_color, DE_COOKED_MAT_SPECULAR),
    P("clearcoat", MT_FLOAT, clearcoat, DE_COOKED_MAT_CLEARCOAT),
    P("clearcoat_roughness", MT_FLOAT, clearcoat_roughness, DE_COOKED_MAT_CLEARCOAT),
    P("clearcoat_normal_scale", MT_FLOAT, clearcoat_normal_scale, DE_COOKED_MAT_CLEARCOAT),
    P("sheen_color", MT_VEC3, sheen_color, DE_COOKED_MAT_SHEEN),
    P("sheen_roughness", MT_FLOAT, sheen_roughness, DE_COOKED_MAT_SHEEN),
    P("transmission", MT_FLOAT, transmission, DE_COOKED_MAT_TRANSMISSION),
    P("thickness", MT_FLOAT, thickness, DE_COOKED_MAT_VOLUME),
    P("attenuation_color", MT_VEC3, attenuation_color, DE_COOKED_MAT_VOLUME),
    P("attenuation_distance", MT_FLOAT, attenuation_distance, DE_COOKED_MAT_VOLUME),
    P("diffuse_transmission", MT_FLOAT, diffuse_transmission, DE_COOKED_MAT_DIFFUSE_TRANSMISSION),
    P("diffuse_transmission_color", MT_VEC3, diffuse_transmission_color,
      DE_COOKED_MAT_DIFFUSE_TRANSMISSION),
    P("anisotropy", MT_FLOAT, anisotropy, DE_COOKED_MAT_ANISOTROPY),
    P("anisotropy_rotation", MT_FLOAT, anisotropy_rotation, DE_COOKED_MAT_ANISOTROPY),
    P("iridescence", MT_FLOAT, iridescence, DE_COOKED_MAT_IRIDESCENCE),
    P("iridescence_ior", MT_FLOAT, iridescence_ior, DE_COOKED_MAT_IRIDESCENCE),
    P("iridescence_thickness_min", MT_FLOAT, iridescence_thickness_min, DE_COOKED_MAT_IRIDESCENCE),
    P("iridescence_thickness_max", MT_FLOAT, iridescence_thickness_max, DE_COOKED_MAT_IRIDESCENCE),
    P("alpha_cutoff", MT_FLOAT, alpha_cutoff, 0),
    P("subsurface", MT_FLOAT, subsurface, 0),
    P("subsurface_radius", MT_VEC3, subsurface_radius, 0),
};
#undef P

/* The texture slots by name (de_cooked_tex_slot order): keys `<name>_tex`, `<name>_texcoord`. */
static const char *const k_slots[DE_COOKED_TEX_COUNT] = {"base_color",
                                                         "metal_rough",
                                                         "normal",
                                                         "occlusion",
                                                         "emissive",
                                                         "clearcoat",
                                                         "clearcoat_roughness",
                                                         "clearcoat_normal",
                                                         "sheen_color",
                                                         "sheen_roughness",
                                                         "specular",
                                                         "specular_color",
                                                         "transmission",
                                                         "thickness",
                                                         "diffuse_transmission",
                                                         "diffuse_transmission_color",
                                                         "anisotropy",
                                                         "iridescence",
                                                         "iridescence_thickness"};

static const char *const k_alpha_modes[3] = {"opaque", "mask", "blend"};

/* ---- Import ---------------------------------------------------------------------------------- */

typedef struct mt_state {
    de_asset_import_ctx *ctx;
    const char *source;
    de_cooked_material_params params;
    u32 flags, alpha_mode;
    const char *model; /* NULL = "standard" */
    bool unlit_key, unlit;
    const char *tex[DE_COOKED_TEX_COUNT];
    u32 texcoord[DE_COOKED_TEX_COUNT];
    char unknown[512];
    u32 unknown_count;
} mt_state;

static bool is_number(const toml_datum_t *v) {
    return v->type == TOML_FP64 || v->type == TOML_INT64;
}
static f32 number(const toml_datum_t *v) {
    return v->type == TOML_FP64 ? (f32)v->u.fp64 : (f32)v->u.int64;
}

static de_result bad_value(mt_state *m, const char *key, int line, const char *want) {
    return de_asset_import_fail(m->ctx, DE_ERR_PARSE, "%s:%d: `%s` must be %s", m->source, line,
                                key, want);
}

/* A parameter key; false when `key` is not one. */
static bool param_key(mt_state *m, const char *key, const toml_datum_t *v, de_result *r) {
    for (size_t i = 0; i < sizeof k_params / sizeof k_params[0]; ++i) {
        const mt_param *p = &k_params[i];
        if (strcmp(p->key, key) != 0)
            continue;
        f32 *dst = (f32 *)(void *)((u8 *)&m->params + p->offset);
        if (p->kind == MT_FLOAT) {
            if (!is_number(v)) {
                *r = bad_value(m, key, v->lineno, "a number");
                return true;
            }
            dst[0] = number(v);
        } else {
            const int32_t n = v->type == TOML_ARRAY ? v->u.arr.size : -1;
            bool ok = n == 3 || (p->kind == MT_COLOR4 && n == 4);
            for (int32_t k = 0; ok && k < n; ++k)
                ok = is_number(&v->u.arr.elem[k]);
            if (!ok) {
                *r = bad_value(m, key, v->lineno,
                               p->kind == MT_COLOR4 ? "an array of 3 or 4 numbers"
                                                    : "an array of 3 numbers");
                return true;
            }
            for (int32_t k = 0; k < n; ++k)
                dst[k] = number(&v->u.arr.elem[k]);
            if (p->kind == MT_COLOR4 && n == 3)
                dst[3] = 1.0f;
        }
        m->flags |= p->ext;
        return true;
    }
    return false;
}

/* A texture key (`<slot>_tex`, `<slot>_texcoord`); false when `key` is not one. */
static bool texture_key(mt_state *m, const char *key, const toml_datum_t *v, de_result *r) {
    const size_t n = strlen(key);
    for (u32 s = 0; s < DE_COOKED_TEX_COUNT; ++s) {
        const size_t sl = strlen(k_slots[s]);
        if (n <= sl || strncmp(key, k_slots[s], sl) != 0)
            continue;
        const char *rest = key + sl;
        if (strcmp(rest, "_tex") == 0) {
            if (v->type != TOML_STRING) {
                *r = bad_value(m, key, v->lineno, "a texture asset ID (a string)");
                return true;
            }
            de_error_clear();
            if (de_asset_id_from(v->u.s).hash == 0) {
                *r = de_asset_import_fail(m->ctx, DE_ERR_PARSE,
                                          "%s:%d: `%s`: \"%s\" is not a valid asset ID", m->source,
                                          v->lineno, key, v->u.s);
                return true;
            }
            m->tex[s] = v->u.s;
            return true;
        }
        if (strcmp(rest, "_texcoord") == 0) {
            if (v->type != TOML_INT64 || v->u.int64 < 0 || v->u.int64 > 1) {
                *r = bad_value(m, key, v->lineno, "0 or 1");
                return true;
            }
            m->texcoord[s] = (u32)v->u.int64;
            return true;
        }
    }
    return false;
}

/* The other keys: model, alpha_mode, double_sided, unlit; false when `key` is not one. */
static bool other_key(mt_state *m, const char *key, const toml_datum_t *v, de_result *r) {
    if (strcmp(key, "model") == 0) {
        if (v->type != TOML_STRING || v->u.str.len == 0 || v->u.str.len > 31) {
            *r = bad_value(m, key, v->lineno, "a model name (1 to 31 bytes)");
            return true;
        }
        m->model = v->u.s;
        return true;
    }
    if (strcmp(key, "alpha_mode") == 0) {
        for (u32 i = 0; v->type == TOML_STRING && i < 3u; ++i)
            if (strcmp(v->u.s, k_alpha_modes[i]) == 0) {
                m->alpha_mode = i;
                return true;
            }
        *r = bad_value(m, key, v->lineno, "\"opaque\", \"mask\" or \"blend\"");
        return true;
    }
    if (strcmp(key, "double_sided") == 0 || strcmp(key, "unlit") == 0) {
        if (v->type != TOML_BOOLEAN) {
            *r = bad_value(m, key, v->lineno, "true or false");
            return true;
        }
        if (key[0] == 'd') {
            m->flags = v->u.boolean ? m->flags | DE_COOKED_MAT_DOUBLE_SIDED
                                    : m->flags & ~(u32)DE_COOKED_MAT_DOUBLE_SIDED;
        } else {
            m->unlit_key = true;
            m->unlit = v->u.boolean;
        }
        return true;
    }
    return false;
}

static void put_u32(u8 *p, u32 v) {
    de_cooked_put_u32(p, v);
}

/* A source that is already a cooked material (DMAT, any version the runtime reads): checked and
 * passed through, its textures declared as dependencies. */
static de_result pass_through(de_asset_import_ctx *ctx, const char *source, const void *data,
                              u64 size) {
    de_cooked_material_view v;
    const char *why = NULL;
    if (!de_cooked_material_parse(data, size, &v, &why))
        return de_asset_import_fail(ctx, DE_ERR_PARSE, "%s: not a valid cooked material: %s",
                                    source, why);
    for (u32 i = 0; i < v.texture_count; ++i) {
        const de_result r =
            de_asset_import_add_dependency(ctx, (de_asset_id){v.textures[i].asset_id});
        if (r != DE_OK)
            return de_asset_import_fail(ctx, r, "%s: texture dependency", source);
    }
    const de_result r = de_asset_import_write(ctx, data, size);
    return r == DE_OK ? DE_OK : de_asset_import_fail(ctx, r, "%s: writing", source);
}

de_result de_mat_toml_import(de_asset_import_ctx *ctx) {
    mt_state m;
    memset(&m, 0, sizeof m);
    m.ctx = ctx;
    m.source = de_asset_import_source_path(ctx);
    m.params = de_cooked_material_params_default();
    u64 len = 0;
    const char *src = (const char *)de_asset_import_source(ctx, &len);
    if (len >= 4u && memcmp(src, DE_COOKED_MATERIAL_MAGIC, 4) == 0)
        return pass_through(ctx, m.source, src, len);
    if (len > (u64)INT32_MAX)
        return de_asset_import_fail(ctx, DE_ERR_PARSE, "%s is too large", m.source);
    toml_result_t tr = de__toml_parse(de_asset_import_allocator(ctx), src, (int)len);
    if (!tr.ok) {
        const de_result fr = de_asset_import_fail(ctx, DE_ERR_PARSE, "%s: %s", m.source, tr.errmsg);
        de__toml_free(tr);
        return fr;
    }

    /* Every key once, in any order: the output depends only on the values. */
    de_result r = DE_OK;
    const toml_datum_t *top = &tr.toptab;
    for (int32_t k = 0; k < top->u.tab.size && r == DE_OK; ++k) {
        const char *key = top->u.tab.key[k];
        const toml_datum_t *v = &top->u.tab.value[k];
        if (param_key(&m, key, v, &r) || texture_key(&m, key, v, &r) || other_key(&m, key, v, &r))
            continue;
        /* Unknown: collected, so the error names them all. */
        const size_t used = strlen(m.unknown);
        snprintf(m.unknown + used, sizeof m.unknown - used, "%s`%s` (line %d)",
                 m.unknown_count != 0 ? ", " : "", key, v->lineno);
        m.unknown_count++;
    }
    if (r == DE_OK && m.unknown_count != 0)
        r = de_asset_import_fail(ctx, DE_ERR_PARSE,
                                 "%s: unknown key%s %s (keys are the de_standard_material fields, "
                                 "`model`, `<slot>_tex` and `<slot>_texcoord`)",
                                 m.source, m.unknown_count == 1 ? "" : "s", m.unknown);

    /* The model: "standard" and "unlit" by the flags (readable by every DMAT reader), the others
     * by name hash, resolved at run time against the registered models. */
    u32 model_hash = 0;
    if (r == DE_OK) {
        const bool unlit_model = m.model != NULL && strcmp(m.model, "unlit") == 0;
        const bool standard_model = m.model == NULL || strcmp(m.model, "standard") == 0;
        if (m.unlit_key && m.unlit && !unlit_model && !standard_model)
            r = de_asset_import_fail(ctx, DE_ERR_PARSE,
                                     "%s: `unlit = true` with `model = \"%s\"`; use one of them",
                                     m.source, m.model);
        else if (m.unlit_key && m.unlit && m.model != NULL && standard_model)
            r = de_asset_import_fail(ctx, DE_ERR_PARSE,
                                     "%s: `unlit = true` with `model = \"standard\"`", m.source);
        else if (unlit_model && m.unlit_key && !m.unlit)
            r = de_asset_import_fail(ctx, DE_ERR_PARSE,
                                     "%s: `unlit = false` with `model = \"unlit\"`", m.source);
        if (unlit_model || (m.unlit_key && m.unlit))
            m.flags |= DE_COOKED_MAT_UNLIT;
        if (!unlit_model && !standard_model)
            model_hash = de_cooked_material_model_hash(m.model);
    }

    /* Textures: dependencies, then the DMAT bytes. */
    u32 tex_count = 0;
    for (u32 s = 0; s < DE_COOKED_TEX_COUNT && r == DE_OK; ++s) {
        if (m.tex[s] == NULL)
            continue;
        ++tex_count;
        r = de_asset_import_add_dependency(ctx, de_asset_id_from(m.tex[s]));
        if (r != DE_OK)
            r = de_asset_import_fail(ctx, r, "%s: texture \"%s\"", m.source, m.tex[s]);
    }
    if (r == DE_OK) {
        const u32 total = DE_COOKED_MATERIAL_HEADER_SIZE + DE_COOKED_MATERIAL_PARAMS_SIZE +
                          tex_count * DE_COOKED_MATERIAL_TEX_SIZE;
        u8 head[DE_COOKED_MATERIAL_HEADER_SIZE];
        memcpy(head, DE_COOKED_MATERIAL_MAGIC, 4);
        put_u32(head + 4, DE_COOKED_MATERIAL_VERSION);
        put_u32(head + 8, m.flags);
        put_u32(head + 12, m.alpha_mode);
        put_u32(head + 16, DE_COOKED_MATERIAL_PARAMS_SIZE);
        put_u32(head + 20, tex_count);
        put_u32(head + 24, total);
        put_u32(head + 28, model_hash);
        r = de_asset_import_write(ctx, head, sizeof head);
        f32 f[DE_COOKED_MATERIAL_PARAM_FLOATS];
        memcpy(f, &m.params, sizeof f);
        u8 pb[DE_COOKED_MATERIAL_PARAMS_SIZE];
        for (u32 i = 0; i < DE_COOKED_MATERIAL_PARAM_FLOATS; ++i)
            de_cooked_put_f32(pb + 4u * i, f[i]);
        if (r == DE_OK)
            r = de_asset_import_write(ctx, pb, sizeof pb);
        for (u32 s = 0; s < DE_COOKED_TEX_COUNT && r == DE_OK; ++s) {
            if (m.tex[s] == NULL)
                continue;
            u8 t[DE_COOKED_MATERIAL_TEX_SIZE];
            put_u32(t, s);
            put_u32(t + 4, m.texcoord[s]);
            de_cooked_put_u64(t + 8, de_asset_id_from(m.tex[s]).hash);
            r = de_asset_import_write(ctx, t, sizeof t);
        }
        if (r != DE_OK)
            r = de_asset_import_fail(ctx, r, "%s: writing the cooked material", m.source);
    }
    de__toml_free(tr);
    return r;
}
