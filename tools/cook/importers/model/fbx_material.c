/*
 * fbx_material.c: FBX materials and textures for the model IR (DENG-102, SPEC §12.6).
 *
 * Two paths, chosen by ufbx's shader type:
 *   - PBR shader models (3ds Max Physical, OpenPBR, glTF and PBR materials, Arnold and OSL
 *     standard surface, Blender's Principled BSDF as Blender writes it, ShaderFX): the values of
 *     ufbx_material.pbr. The base colour is multiplied by the base factor, roughness given as
 *     glossiness is inverted, an emission above 1 becomes emissive_strength.
 *   - FBX Lambert and Phong (and unknown models without a shader): a legacy conversion from
 *     ufbx_material.fbx. Diffuse × factor is the base colour, metallic is 0, the Phong exponent n
 *     becomes roughness (2 / (n + 2))^(1/4) (Blinn-Phong to GGX alpha, alpha = roughness²), the
 *     specular colour × factor becomes KHR_materials_specular (Lambert: specular 0). Opacity is the
 *     `Opacity` property when there is one, else 1 - TransparencyFactor × mean(TransparentColor).
 *
 * Textures: a texture is used when ufbx says it is connected and enabled. Its bytes are the
 * embedded content, else the file at its relative path (or the file name of its absolute path)
 * read through the source's reader, relative to the FBX file. A texture that is neither is
 * skipped with a warning. One IR texture per (file, usage), in order of first use, so two
 * materials that use one image as colour share one texture sub-asset. Texture coordinates are
 * always UV set 0 (FBX names the set per texture; the IR has no name to match it with).
 */
#include "fbx_internal.h"

#include "model_internal.h"
#include "model_ir.h"

#include <dengine/de_base.h>

#include "render/formats/de_cooked_material.h"

#include <math.h>
#include <string.h>

/* ---- Values ---------------------------------------------------------------------------------- */

static f64 map_real(const ufbx_material_map *m, f64 def) {
    return m->has_value ? (f64)m->value_real : def;
}

/* An RGB value; a scalar map gives grey. */
static void map_rgb(const ufbx_material_map *m, f64 def, f64 out[3]) {
    if (!m->has_value) {
        out[0] = out[1] = out[2] = def;
        return;
    }
    if (m->value_components >= 3) {
        out[0] = m->value_vec3.x;
        out[1] = m->value_vec3.y;
        out[2] = m->value_vec3.z;
    } else {
        out[0] = out[1] = out[2] = m->value_real;
    }
}

static f32 clamp01(f64 v) {
    if (!(v > 0.0)) /* also NaN */
        return 0.0f;
    return v < 1.0 ? (f32)v : 1.0f;
}

static f32 finite_or(f64 v, f32 def) {
    return isfinite(v) ? (f32)v : def;
}

/* Colour and strength: an emission above 1 is split into a unit colour and a strength. */
static void set_emission(de_model_material *out, const f64 rgb[3], f64 factor) {
    f64 e[3], mx = 0.0;
    for (u32 k = 0; k < 3; ++k) {
        e[k] = rgb[k] * factor;
        if (!(e[k] > 0.0) || !isfinite(e[k]))
            e[k] = 0.0;
        if (e[k] > mx)
            mx = e[k];
    }
    f64 strength = mx > 1.0 ? mx : 1.0;
    for (u32 k = 0; k < 3; ++k)
        out->params.emissive[k] = (f32)(e[k] / strength);
    if (strength != 1.0) {
        out->params.emissive_strength = (f32)strength;
        out->flags |= DE_COOKED_MAT_EMISSIVE_STRENGTH;
    }
}

static void set_alpha(de_model_material *out, f64 alpha) {
    out->params.base_color[3] = clamp01(alpha);
    if (out->params.base_color[3] < 1.0f)
        out->alpha_mode = DE_COOKED_ALPHA_BLEND;
}

/* A feature the file switched off explicitly (a glTF material's "enableClearCoat" = false). */
static bool feature_off(const ufbx_material_feature_info *fi) {
    return fi->is_explicit && !fi->enabled;
}

/* ---- Textures -------------------------------------------------------------------------------- */

static const char *path_base(const char *p) {
    const char *b = p;
    for (const char *s = p; *s != '\0'; ++s)
        if (*s == '/' || *s == '\\')
            b = s + 1;
    return b;
}

static bool path_absolute(const char *p) {
    return p[0] == '/' || p[0] == '\\' ||
           (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':');
}

static bool ext_is(const char *path, const char *ext) {
    const char *dot = strrchr(path_base(path), '.');
    if (dot == NULL)
        return false;
    ++dot;
    size_t n = strlen(ext);
    if (strlen(dot) != n)
        return false;
    for (size_t i = 0; i < n; ++i) {
        char c = dot[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != ext[i])
            return false;
    }
    return true;
}

static const char *mime_of(const char *path) {
    if (ext_is(path, "png"))
        return "image/png";
    if (ext_is(path, "jpg") || ext_is(path, "jpeg"))
        return "image/jpeg";
    if (ext_is(path, "ktx2"))
        return "image/ktx2";
    if (ext_is(path, "exr"))
        return "image/x-exr";
    if (ext_is(path, "tga"))
        return "image/x-tga";
    return "";
}

/* The file name of `t` with '\\' turned into '/': the relative path when it is one, else the
 * file name of the absolute path (FBX files often keep the artist's absolute paths). */
static const char *texture_rel_path(de__fbx_ctx *f, const ufbx_texture *t) {
    const char *rel = t->relative_filename.data;
    const char *use = NULL;
    if (rel != NULL && rel[0] != '\0' && !path_absolute(rel))
        use = rel;
    else if (t->absolute_filename.length != 0)
        use = path_base(t->absolute_filename.data);
    else if (t->filename.length != 0)
        use = path_base(t->filename.data);
    if (use == NULL || use[0] == '\0')
        return NULL;
    char *p = (char *)de_model_ir_strdup(f->ir, use);
    if (p == NULL)
        return NULL;
    for (char *s = p; *s != '\0'; ++s)
        if (*s == '\\')
            *s = '/';
    return p;
}

/* The IR texture for (file of `t`, usage), created on first use. -1 when the texture has no
 * readable bytes (a warning) or on out of memory (`*err` set). */
static i32 texture_for(de__fbx_ctx *f, const ufbx_texture *t, de_model_tex_usage usage,
                       de_result *err) {
    if (t->type != UFBX_TEXTURE_FILE) {
        if (t->file_textures.count == 0) {
            de_model_ir_warn(f->ir, "texture \"%s\" is not a file texture; ignored", t->name.data);
            return -1;
        }
        if (t->file_textures.count > 1)
            de_model_ir_warn(f->ir, "texture \"%s\" has %u layers; only the first is imported",
                             t->name.data, (unsigned)t->file_textures.count);
        t = t->file_textures.data[0];
    }
    de_model_ir *ir = f->ir;
    u32 image = t->has_file ? t->file_index : (u32)f->scene->texture_files.count + (u32)t->typed_id;
    for (u32 i = 0; i < ir->texture_count; ++i)
        if (ir->textures[i].image == image && ir->textures[i].usage == usage)
            return (i32)i;
    if (ir->texture_count >= f->texture_cap) {
        *err = DE_FAIL(DE_ERR_INTERNAL, "FBX: texture table full");
        return -1;
    }
    const char *rel = texture_rel_path(f, t);
    const void *data = NULL;
    u64 size = 0;
    if (t->content.size != 0) {
        void *copy = de_model_ir_alloc(ir, (u64)t->content.size);
        if (copy == NULL) {
            *err = DE_ERR_OUT_OF_MEMORY;
            return -1;
        }
        memcpy(copy, t->content.data, t->content.size);
        data = copy;
        size = (u64)t->content.size;
    } else {
        de_result r = DE_ERR_NOT_FOUND;
        if (rel != NULL && f->src->read_file != NULL)
            r = f->src->read_file(f->src->read_user, rel, &data, &size);
        if (r != DE_OK) {
            const de_error *e = de_error_last();
            de_model_ir_warn(ir,
                             "texture \"%s\": \"%s\" is not embedded and cannot be read (%s); "
                             "ignored",
                             t->name.data, rel != NULL ? rel : "",
                             e != NULL && e->result == r ? e->message : de_result_str(r));
            de_error_clear();
            return -1;
        }
    }
    de_model_texture *out = &ir->textures[ir->texture_count];
    const char *name = t->name.data;
    if (name[0] == '\0' && rel != NULL) {
        /* The file name without its extension. */
        char *stem = (char *)de_model_ir_strdup(ir, path_base(rel));
        if (stem == NULL) {
            *err = DE_ERR_OUT_OF_MEMORY;
            return -1;
        }
        char *dot = strrchr(stem, '.');
        if (dot != NULL && dot != stem)
            *dot = '\0';
        name = stem;
    }
    out->name = de_model_ir_strdup(ir, name);
    out->mime = de_model_ir_strdup(ir, rel != NULL ? mime_of(rel) : "");
    if (out->name == NULL || out->mime == NULL) {
        *err = DE_ERR_OUT_OF_MEMORY;
        return -1;
    }
    out->image = image;
    out->usage = usage;
    out->data = data;
    out->size = size;
    return (i32)ir->texture_count++;
}

/* Puts the texture of `map` (when connected and enabled) in `slot`. */
static de_result set_slot(de__fbx_ctx *f, de_model_material *out, u32 slot,
                          const ufbx_material_map *map) {
    if (map->texture == NULL || !map->texture_enabled || out->tex[slot].texture >= 0)
        return DE_OK;
    de_result err = DE_OK;
    i32 t = texture_for(f, map->texture, de_model_tex_usage_of_slot(slot), &err);
    if (err != DE_OK)
        return err;
    out->tex[slot].texture = t;
    out->tex[slot].texcoord = 0;
    return DE_OK;
}

static bool has_texture(const ufbx_material_map *map) {
    return map->texture != NULL && map->texture_enabled;
}

/* ---- PBR shader models ----------------------------------------------------------------------- */

static de_result pbr_material(de__fbx_ctx *f, const ufbx_material *m, de_model_material *out) {
    const ufbx_material_pbr_maps *p = &m->pbr;
    const ufbx_material_features *ft = &m->features;
    de_cooked_material_params *q = &out->params;

    f64 base[3];
    map_rgb(&p->base_color, 1.0, base);
    f64 base_factor = map_real(&p->base_factor, 1.0);
    for (u32 k = 0; k < 3; ++k)
        q->base_color[k] = clamp01(base[k] * base_factor);
    set_alpha(out, map_real(&p->opacity, 1.0));

    q->metallic = clamp01(map_real(&p->metalness, 0.0));
    f64 rough = map_real(&p->roughness, 0.5);
    q->roughness = clamp01(ft->roughness_as_glossiness.enabled ? 1.0 - rough : rough);

    if (!feature_off(&ft->specular) &&
        (p->specular_factor.has_value || p->specular_color.has_value)) {
        f64 sc[3];
        map_rgb(&p->specular_color, 1.0, sc);
        q->specular = clamp01(map_real(&p->specular_factor, 1.0));
        for (u32 k = 0; k < 3; ++k)
            q->specular_color[k] = finite_or(sc[k] > 0.0 ? sc[k] : 0.0, 1.0f);
        out->flags |= DE_COOKED_MAT_SPECULAR;
    }
    if (p->specular_ior.has_value && !feature_off(&ft->ior)) {
        f64 ior = p->specular_ior.value_real;
        if (ior >= 1.0 && isfinite(ior)) {
            q->ior = (f32)ior;
            out->flags |= DE_COOKED_MAT_IOR;
        }
    }

    f64 em[3];
    map_rgb(&p->emission_color, 0.0, em);
    if (!feature_off(&ft->emission))
        set_emission(out, em, map_real(&p->emission_factor, 1.0));

    f64 coat = map_real(&p->coat_factor, 0.0);
    if (coat > 0.0 && !feature_off(&ft->coat)) {
        q->clearcoat = clamp01(coat);
        f64 cr = map_real(&p->coat_roughness, 0.0);
        q->clearcoat_roughness = clamp01(ft->coat_roughness_as_glossiness.enabled ? 1.0 - cr : cr);
        out->flags |= DE_COOKED_MAT_CLEARCOAT;
    }

    f64 sheen_rgb[3];
    map_rgb(&p->sheen_color, 1.0, sheen_rgb);
    f64 sheen = map_real(&p->sheen_factor, p->sheen_color.has_value ? 1.0 : 0.0);
    if (sheen > 0.0 && !feature_off(&ft->sheen)) {
        for (u32 k = 0; k < 3; ++k)
            q->sheen_color[k] = clamp01(sheen_rgb[k] * sheen);
        q->sheen_roughness = clamp01(map_real(&p->sheen_roughness, 0.3));
        out->flags |= DE_COOKED_MAT_SHEEN;
    }

    f64 trans = map_real(&p->transmission_factor, 0.0);
    if (trans > 0.0 && !feature_off(&ft->transmission)) {
        q->transmission = clamp01(trans);
        out->flags |= DE_COOKED_MAT_TRANSMISSION;
    }
    if (ft->double_sided.enabled)
        out->flags |= DE_COOKED_MAT_DOUBLE_SIDED;
    if (ft->unlit.enabled)
        out->flags |= DE_COOKED_MAT_UNLIT;

    DE_TRY(set_slot(f, out, DE_COOKED_TEX_BASE_COLOR, &p->base_color));
    DE_TRY(set_slot(f, out, DE_COOKED_TEX_NORMAL, &p->normal_map));
    DE_TRY(set_slot(f, out, DE_COOKED_TEX_EMISSIVE, &p->emission_color));
    DE_TRY(set_slot(f, out, DE_COOKED_TEX_OCCLUSION, &p->ambient_occlusion));
    if (out->flags & DE_COOKED_MAT_CLEARCOAT) {
        DE_TRY(set_slot(f, out, DE_COOKED_TEX_CLEARCOAT, &p->coat_factor));
        DE_TRY(set_slot(f, out, DE_COOKED_TEX_CLEARCOAT_ROUGHNESS, &p->coat_roughness));
        DE_TRY(set_slot(f, out, DE_COOKED_TEX_CLEARCOAT_NORMAL, &p->coat_normal));
    }
    if (out->flags & DE_COOKED_MAT_SHEEN) {
        DE_TRY(set_slot(f, out, DE_COOKED_TEX_SHEEN_COLOR, &p->sheen_color));
        DE_TRY(set_slot(f, out, DE_COOKED_TEX_SHEEN_ROUGHNESS, &p->sheen_roughness));
    }
    if (out->flags & DE_COOKED_MAT_SPECULAR) {
        DE_TRY(set_slot(f, out, DE_COOKED_TEX_SPECULAR, &p->specular_factor));
        DE_TRY(set_slot(f, out, DE_COOKED_TEX_SPECULAR_COLOR, &p->specular_color));
    }
    if (out->flags & DE_COOKED_MAT_TRANSMISSION)
        DE_TRY(set_slot(f, out, DE_COOKED_TEX_TRANSMISSION, &p->transmission_factor));
    /* glTF packs roughness (G) and metalness (B) in one texture: only a shared file maps. */
    bool rt = has_texture(&p->roughness), mt = has_texture(&p->metalness);
    if (rt && mt && p->roughness.texture->has_file && p->metalness.texture->has_file &&
        p->roughness.texture->file_index == p->metalness.texture->file_index)
        DE_TRY(set_slot(f, out, DE_COOKED_TEX_METAL_ROUGH, &p->roughness));
    else if (rt || mt)
        de_model_ir_warn(f->ir,
                         "material \"%s\": separate roughness and metalness textures are not "
                         "packed into one; ignored",
                         m->name.data);
    if (has_texture(&p->opacity))
        de_model_ir_warn(f->ir,
                         "material \"%s\": an opacity texture is not imported (use the base "
                         "colour's alpha)",
                         m->name.data);
    return DE_OK;
}

/* ---- FBX Lambert and Phong ------------------------------------------------------------------- */

static de_result legacy_material(de__fbx_ctx *f, const ufbx_material *m, de_model_material *out) {
    const ufbx_material_fbx_maps *x = &m->fbx;
    de_cooked_material_params *q = &out->params;

    f64 diff[3];
    map_rgb(&x->diffuse_color, 1.0, diff);
    f64 diff_factor = map_real(&x->diffuse_factor, 1.0);
    for (u32 k = 0; k < 3; ++k)
        q->base_color[k] = clamp01(diff[k] * diff_factor);

    const ufbx_prop *op = ufbx_find_prop(&m->props, "Opacity");
    f64 alpha = 1.0;
    if (op != NULL) {
        alpha = op->value_real;
    } else if (x->transparency_factor.has_value || x->transparency_color.has_value) {
        f64 tc[3];
        map_rgb(&x->transparency_color, 1.0, tc);
        alpha = 1.0 - map_real(&x->transparency_factor, 1.0) * (tc[0] + tc[1] + tc[2]) / 3.0;
    }
    set_alpha(out, alpha);

    f64 em[3];
    map_rgb(&x->emission_color, 0.0, em);
    set_emission(out, em, map_real(&x->emission_factor, 1.0));

    q->metallic = 0.0f;
    bool phong = m->shader_type == UFBX_SHADER_FBX_PHONG ||
                 (m->shader_type != UFBX_SHADER_FBX_LAMBERT && x->specular_exponent.has_value);
    if (phong) {
        f64 n = map_real(&x->specular_exponent, 20.0);
        if (!(n >= 0.0) || !isfinite(n))
            n = 0.0;
        q->roughness = clamp01(pow(2.0 / (n + 2.0), 0.25));
        if (x->specular_color.has_value || x->specular_factor.has_value) {
            f64 sc[3], mx = 0.0;
            map_rgb(&x->specular_color, 1.0, sc);
            for (u32 k = 0; k < 3; ++k) {
                if (!(sc[k] > 0.0) || !isfinite(sc[k]))
                    sc[k] = 0.0;
                if (sc[k] > mx)
                    mx = sc[k];
            }
            q->specular = clamp01(map_real(&x->specular_factor, 1.0) * mx);
            for (u32 k = 0; k < 3; ++k)
                q->specular_color[k] = mx > 0.0 ? (f32)(sc[k] / mx) : 1.0f;
            out->flags |= DE_COOKED_MAT_SPECULAR;
        }
    } else {
        q->roughness = 1.0f;
        q->specular = 0.0f;
        out->flags |= DE_COOKED_MAT_SPECULAR;
    }

    DE_TRY(set_slot(f, out, DE_COOKED_TEX_BASE_COLOR, &x->diffuse_color));
    DE_TRY(set_slot(f, out, DE_COOKED_TEX_NORMAL, &x->normal_map));
    DE_TRY(set_slot(f, out, DE_COOKED_TEX_EMISSIVE, &x->emission_color));
    for (u32 i = 0; i < UFBX_MATERIAL_FBX_MAP_COUNT; ++i) {
        const ufbx_material_map *map = &x->maps[i];
        if (map != &x->diffuse_color && map != &x->normal_map && map != &x->emission_color &&
            has_texture(map)) {
            de_model_ir_warn(f->ir,
                             "material \"%s\": only diffuse, normal and emissive textures of FBX "
                             "Lambert and Phong materials are imported",
                             m->name.data);
            break;
        }
    }
    return DE_OK;
}

de_result de__fbx_material(de__fbx_ctx *f, const ufbx_material *m, de_model_material *out) {
    out->name = de_model_ir_strdup(f->ir, m->name.data);
    if (out->name == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    out->params = de_cooked_material_params_default();
    out->flags = 0;
    out->alpha_mode = DE_COOKED_ALPHA_OPAQUE;
    for (u32 s = 0; s < DE_COOKED_TEX_COUNT; ++s) {
        out->tex[s].texture = -1;
        out->tex[s].texcoord = 0;
    }
    switch (m->shader_type) {
    case UFBX_SHADER_UNKNOWN:
    case UFBX_SHADER_FBX_LAMBERT:
    case UFBX_SHADER_FBX_PHONG:
        return legacy_material(f, m, out);
    default:
        return pbr_material(f, m, out);
    }
}
