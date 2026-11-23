/*
 * de_cooked_material_graph.h: the cooked material graph (DMGR, DENG-770). The `material_graph`
 * importer (tools/cook/importers/material_graph/mg_import.c) writes it; the render loader
 * (src/render/loaders/material_graph_loader.c) reads it and registers the material model.
 *
 * Layout (little-endian, version 1):
 *
 *   offset  size  field
 *   0       4     magic "DMGR"
 *   4       4     version (1)
 *   8       4     total size in bytes
 *   12      4     features: DE_MATERIAL_FEATURE_* bits the graph's layer outputs set
 *   16      4     param_count
 *   20      4     shader_offset (16-aligned)
 *   24      4     shader_size
 *   28      4     reserved (0)
 *   32      32    model name, NUL padded (1 to 31 bytes)
 *   64      168 x param_count   parameters, in the generator's order:
 *                 kind u32 (de__mg_param_kind: 0 float, 1 colour, 2 texture), row u32,
 *                 component u32, slot u32, value f32[3], min f32, max f32, reserved u32 (0),
 *                 name char[64], node char[64] (both NUL terminated)
 *   ...           zero padding to shader_offset
 *   shader_offset shader_size   the cooked shader (DSHD, gpu/shader/shader_format.h): the entry
 *                 point cs_resolve_model for each target the cook produced
 *
 * The asset also has the sub-asset "<id>#material" (type `material`, DMAT): the graph's default
 * parameters at their DeGpuMaterial row positions and model_hash = the model name's hash, so
 * materials made from it resolve to the graph's model.
 *
 * Header-only, allocation-free; the parse checks every offset and size.
 */
#ifndef DE_RENDER_FORMATS_COOKED_MATERIAL_GRAPH_H
#define DE_RENDER_FORMATS_COOKED_MATERIAL_GRAPH_H

#include "render/formats/de_cooked_le.h"

#include <dengine/de_base.h>

#include <string.h>

DE_EXTERN_C_BEGIN

#define DE_COOKED_MATERIAL_GRAPH_MAGIC       "DMGR"
#define DE_COOKED_MATERIAL_GRAPH_VERSION     1u
#define DE_COOKED_MATERIAL_GRAPH_HEADER_SIZE 64u /**< header and model name */
#define DE_COOKED_MATERIAL_GRAPH_PARAM_SIZE  168u
#define DE_COOKED_MATERIAL_GRAPH_NAME_SIZE   32u
#define DE_COOKED_MATERIAL_GRAPH_MAX_PARAMS  64u
#define DE_COOKED_MATERIAL_GRAPH_ROWS        10u
#define DE_COOKED_MATERIAL_GRAPH_SLOTS       19u

/** The sub-asset name of the default material ("<id>#material"). */
#define DE_COOKED_MATERIAL_GRAPH_MATERIAL "material"

typedef enum de_cooked_mg_param_kind {
    DE_COOKED_MG_PARAM_FLOAT = 0,
    DE_COOKED_MG_PARAM_COLOR = 1,
    DE_COOKED_MG_PARAM_TEXTURE = 2
} de_cooked_mg_param_kind;

typedef struct de_cooked_mg_param {
    u32 kind; /**< de_cooked_mg_param_kind */
    u32 row, component, slot;
    f32 value[3];
    f32 min, max;
    char name[64];
    char node[64];
} de_cooked_mg_param;

typedef struct de_cooked_material_graph_view {
    u32 version;
    u32 features;
    u32 param_count;
    char model[DE_COOKED_MATERIAL_GRAPH_NAME_SIZE];
    const u8 *params; /**< param_count records of DE_COOKED_MATERIAL_GRAPH_PARAM_SIZE bytes */
    const u8 *shader;
    u64 shader_size;
} de_cooked_material_graph_view;

/**
 * The DMAT parameter float (de_cooked_material_params, in declaration order) that DeGpuMaterial
 * row `row`, component `component` is packed from (material_instances.c de__material_pack), or
 * -1 when out of range. Rows: base_color, emissive, pbr, specular, clearcoat, sheen, volume, extra,
 * iridescence, subsurface.
 */
static inline i32 de_cooked_material_graph_param_float(u32 row, u32 component) {
    static const i8 k_map[DE_COOKED_MATERIAL_GRAPH_ROWS][4] = {
        {0, 1, 2, 3},     /* base_color */
        {8, 9, 10, 11},   /* emissive xyz, emissive_strength */
        {4, 5, 6, 40},    /* metallic, roughness, normal_scale, alpha_cutoff */
        {14, 15, 16, 13}, /* specular_color xyz, specular */
        {17, 18, 12, 24}, /* clearcoat, clearcoat_roughness, ior, transmission */
        {20, 21, 22, 23}, /* sheen_color xyz, sheen_roughness */
        {26, 27, 28, 29}, /* attenuation_color xyz, attenuation_distance */
        {25, 30, 41, 34}, /* thickness, diffuse_transmission, subsurface, anisotropy */
        {36, 37, 38, 39}, /* iridescence, iridescence_ior, thickness min, max */
        {42, 43, 44, 35}, /* subsurface_radius xyz, anisotropy_rotation */
    };
    if (row >= DE_COOKED_MATERIAL_GRAPH_ROWS || component > 3u)
        return -1;
    return k_map[row][component];
}

/** Parameter `i` of a checked view. */
static inline de_cooked_mg_param
de_cooked_material_graph_param(const de_cooked_material_graph_view *v, u32 i) {
    de_cooked_mg_param p;
    memset(&p, 0, sizeof p);
    if (i >= v->param_count)
        return p;
    const u8 *b = v->params + (u64)i * DE_COOKED_MATERIAL_GRAPH_PARAM_SIZE;
    p.kind = de_cooked_get_u32(b);
    p.row = de_cooked_get_u32(b + 4);
    p.component = de_cooked_get_u32(b + 8);
    p.slot = de_cooked_get_u32(b + 12);
    for (u32 k = 0; k < 3u; ++k)
        p.value[k] = de_cooked_get_f32(b + 16 + 4u * k);
    p.min = de_cooked_get_f32(b + 28);
    p.max = de_cooked_get_f32(b + 32);
    memcpy(p.name, b + 40, sizeof p.name);
    memcpy(p.node, b + 104, sizeof p.node);
    p.name[sizeof p.name - 1] = '\0';
    p.node[sizeof p.node - 1] = '\0';
    return p;
}

/** True when `s` (`n` bytes) holds a NUL and only printable bytes before it. */
static inline bool de_cooked_mg_cstr_ok(const u8 *s, u32 n, bool allow_empty) {
    u32 len = 0;
    while (len < n && s[len] != 0) {
        if (s[len] < 0x20 || s[len] >= 0x7f)
            return false;
        ++len;
    }
    return len < n && (allow_empty || len > 0);
}

/**
 * Checks a DMGR blob and fills `out`: magic, version, sizes, the model name, each parameter
 * (kind, row and component or slot in range, names terminated) and the shader range. `*why` gets a
 * static reason on failure (may be NULL). Any thread.
 */
static inline bool de_cooked_material_graph_parse(const void *data, u64 size,
                                                  de_cooked_material_graph_view *out,
                                                  const char **why) {
    const char *w = NULL;
    const u8 *p = (const u8 *)data;
    memset(out, 0, sizeof *out);
    if (p == NULL || size < DE_COOKED_MATERIAL_GRAPH_HEADER_SIZE) {
        w = "shorter than the header";
        goto fail;
    }
    if (memcmp(p, DE_COOKED_MATERIAL_GRAPH_MAGIC, 4) != 0) {
        w = "not a cooked material graph (magic)";
        goto fail;
    }
    out->version = de_cooked_get_u32(p + 4);
    if (out->version != DE_COOKED_MATERIAL_GRAPH_VERSION) {
        w = "unsupported format version";
        goto fail;
    }
    const u32 total = de_cooked_get_u32(p + 8);
    out->features = de_cooked_get_u32(p + 12);
    out->param_count = de_cooked_get_u32(p + 16);
    const u32 shader_offset = de_cooked_get_u32(p + 20);
    const u32 shader_size = de_cooked_get_u32(p + 24);
    if ((u64)total != size || de_cooked_get_u32(p + 28) != 0u) {
        w = "bad total size or reserved field";
        goto fail;
    }
    if (out->param_count > DE_COOKED_MATERIAL_GRAPH_MAX_PARAMS) {
        w = "too many parameters";
        goto fail;
    }
    const u64 params_end = (u64)DE_COOKED_MATERIAL_GRAPH_HEADER_SIZE +
                           (u64)out->param_count * DE_COOKED_MATERIAL_GRAPH_PARAM_SIZE;
    if ((shader_offset & 15u) != 0u || shader_offset < params_end || shader_size == 0u ||
        !de_cooked_range_ok(size, shader_offset, shader_size) ||
        (u64)shader_offset + shader_size != size) {
        w = "bad shader range";
        goto fail;
    }
    if (!de_cooked_mg_cstr_ok(p + 32, DE_COOKED_MATERIAL_GRAPH_NAME_SIZE, false)) {
        w = "bad model name";
        goto fail;
    }
    memcpy(out->model, p + 32, DE_COOKED_MATERIAL_GRAPH_NAME_SIZE);
    out->params = p + DE_COOKED_MATERIAL_GRAPH_HEADER_SIZE;
    for (u32 i = 0; i < out->param_count; ++i) {
        const u8 *b = out->params + (u64)i * DE_COOKED_MATERIAL_GRAPH_PARAM_SIZE;
        const u32 kind = de_cooked_get_u32(b), row = de_cooked_get_u32(b + 4);
        const u32 comp = de_cooked_get_u32(b + 8), slot = de_cooked_get_u32(b + 12);
        bool ok = de_cooked_get_u32(b + 36) == 0u && de_cooked_mg_cstr_ok(b + 40, 64, false) &&
                  de_cooked_mg_cstr_ok(b + 104, 64, true);
        if (kind == DE_COOKED_MG_PARAM_FLOAT)
            ok = ok && row < DE_COOKED_MATERIAL_GRAPH_ROWS && comp <= 3u;
        else if (kind == DE_COOKED_MG_PARAM_COLOR)
            ok = ok && row < DE_COOKED_MATERIAL_GRAPH_ROWS && comp <= 1u;
        else if (kind == DE_COOKED_MG_PARAM_TEXTURE)
            ok = ok && slot < DE_COOKED_MATERIAL_GRAPH_SLOTS;
        else
            ok = false;
        if (!ok) {
            w = "bad parameter record";
            goto fail;
        }
    }
    out->shader = p + shader_offset;
    out->shader_size = shader_size;
    return true;
fail:
    memset(out, 0, sizeof *out);
    if (why != NULL)
        *why = w;
    return false;
}

DE_EXTERN_C_END

#endif /* DE_RENDER_FORMATS_COOKED_MATERIAL_GRAPH_H */
