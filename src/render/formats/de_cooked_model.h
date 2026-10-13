/*
 * de_cooked_model.h: the cooked model format DMDL, version 1 (SPEC §11.6; DENG-101). Internal;
 * include as "render/formats/de_cooked_model.h".
 *
 * The bytes of a `model` asset: the node table of the source and the sub-asset IDs the import
 * produced (meshes, materials, textures, skeletons, clips, the prefab). The sub-assets carry the
 * data; the runtime model loader (DENG-105) declares them as dependencies. Node transforms are in
 * engine coordinates (Z-up, right-handed, metres, SPEC §5.5) with the import `scale` applied.
 *
 * Every field is little-endian (de_cooked_le.h). Layout:
 *
 *   header      DE_COOKED_MODEL_HEADER_SIZE (80) bytes:
 *                 0  u8[4] magic "DMDL"
 *                 4  u32   format version (DE_COOKED_MODEL_VERSION)
 *                 8  u32   flags (0 in version 1)
 *                12  u32   node count N
 *                16  u32   root count R (the imported scene's root nodes)
 *                20  u32   mesh count
 *                24  u32   material count
 *                28  u32   texture count
 *                32  u32   skeleton count
 *                36  u32   clip count
 *                40  u32   node table offset (80)
 *                44  u32   root list offset
 *                48  u32   ID table offset (8-byte aligned)
 *                52  u32   string offset
 *                56  u32   string bytes S
 *                60  u32   total size of the blob
 *                64  u64   prefab asset ID (`<model>#prefab`; 0 = none)
 *                72  u64   reserved, 0
 *   nodes       N x 64: u32 name offset (into strings), u32 name length, u32 parent (node index,
 *               DE_COOKED_NONE for a root), u32 mesh (index into the mesh IDs, DE_COOKED_NONE),
 *               u32 skin (index into the skeleton IDs, DE_COOKED_NONE), u32 reserved 0,
 *               f32[3] translation, f32[4] rotation (x, y, z, w), f32[3] scale. Parent links have
 *               no cycle. The name is the node's unique sub-asset name (`<model>#<name>`).
 *   roots       R x u32: node indices without a parent, in scene order.
 *   IDs         u64 asset IDs: the meshes, then the materials, textures, skeletons and clips.
 *   strings     S bytes: node names, each followed by a NUL.
 *
 * Bump DE_COOKED_MODEL_VERSION on any change.
 */
#ifndef DE_RENDER_FORMATS_COOKED_MODEL_H
#define DE_RENDER_FORMATS_COOKED_MODEL_H

#include "render/formats/de_cooked_le.h"

#include <dengine/de_base.h>

DE_EXTERN_C_BEGIN

#define DE_COOKED_MODEL_MAGIC       "DMDL"
#define DE_COOKED_MODEL_VERSION     1u
#define DE_COOKED_MODEL_HEADER_SIZE 80u
#define DE_COOKED_MODEL_NODE_SIZE   64u
/** "No parent / no mesh / no skin" in a node record. */
#define DE_COOKED_NONE 0xFFFFFFFFu

/** One decoded node record. */
typedef struct de_cooked_model_node {
    const char *name; /**< into the blob, NUL-terminated */
    u32 parent, mesh, skin;
    f32 translation[3], rotation[4], scale[3];
} de_cooked_model_node;

/** A checked view of a DMDL blob (pointers into it). */
typedef struct de_cooked_model_view {
    const u8 *base;
    u64 size;
    u32 node_count, root_count;
    u32 mesh_count, material_count, texture_count, skeleton_count, clip_count;
    u64 prefab_id;
    const u8 *nodes, *roots, *ids, *strings;
    u32 string_size;
} de_cooked_model_view;

/**
 * Checks a DMDL blob and fills `out`: magic, version, sizes, offsets, node records (name inside
 * the strings and NUL-terminated, parent/mesh/skin indices in range, no parent cycle) and roots
 * (in range, without a parent). Allocation-free. False on any error, with a short reason in `*why`
 * (nullable).
 */
static inline bool de_cooked_model_parse(const void *data, u64 size, de_cooked_model_view *out,
                                         const char **why) {
    const char *w = NULL;
    const u8 *p = (const u8 *)data;
    memset(out, 0, sizeof *out);
    if (p == NULL || size < DE_COOKED_MODEL_HEADER_SIZE) {
        w = "shorter than the header";
        goto fail;
    }
    if (memcmp(p, DE_COOKED_MODEL_MAGIC, 4) != 0) {
        w = "not a cooked model (magic)";
        goto fail;
    }
    if (de_cooked_get_u32(p + 4) != DE_COOKED_MODEL_VERSION) {
        w = "unsupported format version";
        goto fail;
    }
    out->base = p;
    out->size = size;
    out->node_count = de_cooked_get_u32(p + 12);
    out->root_count = de_cooked_get_u32(p + 16);
    out->mesh_count = de_cooked_get_u32(p + 20);
    out->material_count = de_cooked_get_u32(p + 24);
    out->texture_count = de_cooked_get_u32(p + 28);
    out->skeleton_count = de_cooked_get_u32(p + 32);
    out->clip_count = de_cooked_get_u32(p + 36);
    u32 node_off = de_cooked_get_u32(p + 40);
    u32 root_off = de_cooked_get_u32(p + 44);
    u32 id_off = de_cooked_get_u32(p + 48);
    u32 str_off = de_cooked_get_u32(p + 52);
    out->string_size = de_cooked_get_u32(p + 56);
    u32 total = de_cooked_get_u32(p + 60);
    out->prefab_id = de_cooked_get_u64(p + 64);
    u64 ids = (u64)out->mesh_count + out->material_count + out->texture_count +
              out->skeleton_count + out->clip_count;
    if (total != size || out->root_count > out->node_count ||
        !de_cooked_range_ok(size, node_off, (u64)out->node_count * DE_COOKED_MODEL_NODE_SIZE) ||
        !de_cooked_range_ok(size, root_off, (u64)out->root_count * 4u) ||
        !de_cooked_range_ok(size, id_off, ids * 8u) ||
        !de_cooked_range_ok(size, str_off, out->string_size) ||
        node_off < DE_COOKED_MODEL_HEADER_SIZE) {
        w = "bad sizes or offsets";
        goto fail;
    }
    out->nodes = p + node_off;
    out->roots = p + root_off;
    out->ids = p + id_off;
    out->strings = p + str_off;
    for (u32 i = 0; i < out->node_count; ++i) {
        const u8 *r = out->nodes + (u64)i * DE_COOKED_MODEL_NODE_SIZE;
        u32 name = de_cooked_get_u32(r), len = de_cooked_get_u32(r + 4);
        u32 parent = de_cooked_get_u32(r + 8), mesh = de_cooked_get_u32(r + 12);
        u32 skin = de_cooked_get_u32(r + 16);
        if (!de_cooked_range_ok(out->string_size, name, (u64)len + 1u) ||
            out->strings[name + len] != 0 ||
            (parent != DE_COOKED_NONE && parent >= out->node_count) ||
            (mesh != DE_COOKED_NONE && mesh >= out->mesh_count) ||
            (skin != DE_COOKED_NONE && skin >= out->skeleton_count)) {
            w = "bad node record";
            goto fail;
        }
        /* No cycle: walking up from any node ends within N steps. A parent of a later node is
         * not checked yet: stop at one out of range (DENG-106: it was read past the table). */
        u32 at = parent, steps = 0;
        while (at != DE_COOKED_NONE && at < out->node_count && steps <= out->node_count) {
            at = de_cooked_get_u32(out->nodes + (u64)at * DE_COOKED_MODEL_NODE_SIZE + 8);
            ++steps;
        }
        if (at != DE_COOKED_NONE) {
            w = at < out->node_count ? "node parent cycle" : "bad node record";
            goto fail;
        }
    }
    for (u32 i = 0; i < out->root_count; ++i) {
        u32 n = de_cooked_get_u32(out->roots + 4u * i);
        if (n >= out->node_count ||
            de_cooked_get_u32(out->nodes + (u64)n * DE_COOKED_MODEL_NODE_SIZE + 8) !=
                DE_COOKED_NONE) {
            w = "bad root";
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

/** Node `i` (< node_count) of a parsed model. */
static inline de_cooked_model_node de_cooked_model_get_node(const de_cooked_model_view *v, u32 i) {
    const u8 *r = v->nodes + (u64)i * DE_COOKED_MODEL_NODE_SIZE;
    de_cooked_model_node n;
    n.name = (const char *)(v->strings + de_cooked_get_u32(r));
    n.parent = de_cooked_get_u32(r + 8);
    n.mesh = de_cooked_get_u32(r + 12);
    n.skin = de_cooked_get_u32(r + 16);
    for (u32 k = 0; k < 3; ++k) {
        n.translation[k] = de_cooked_get_f32(r + 24 + 4 * k);
        n.scale[k] = de_cooked_get_f32(r + 52 + 4 * k);
    }
    for (u32 k = 0; k < 4; ++k)
        n.rotation[k] = de_cooked_get_f32(r + 36 + 4 * k);
    return n;
}

/** Root `i` (< root_count): a node index. */
static inline u32 de_cooked_model_root(const de_cooked_model_view *v, u32 i) {
    return de_cooked_get_u32(v->roots + 4u * i);
}

/** The ID lists, in table order. */
static inline u64 de_cooked_model_mesh_id(const de_cooked_model_view *v, u32 i) {
    return de_cooked_get_u64(v->ids + 8u * (u64)i);
}
static inline u64 de_cooked_model_material_id(const de_cooked_model_view *v, u32 i) {
    return de_cooked_get_u64(v->ids + 8u * ((u64)v->mesh_count + i));
}
static inline u64 de_cooked_model_texture_id(const de_cooked_model_view *v, u32 i) {
    return de_cooked_get_u64(v->ids + 8u * ((u64)v->mesh_count + v->material_count + i));
}
static inline u64 de_cooked_model_skeleton_id(const de_cooked_model_view *v, u32 i) {
    return de_cooked_get_u64(v->ids +
                             8u * ((u64)v->mesh_count + v->material_count + v->texture_count + i));
}
static inline u64 de_cooked_model_clip_id(const de_cooked_model_view *v, u32 i) {
    return de_cooked_get_u64(v->ids + 8u * ((u64)v->mesh_count + v->material_count +
                                            v->texture_count + v->skeleton_count + i));
}

DE_EXTERN_C_END

#endif /* DE_RENDER_FORMATS_COOKED_MODEL_H */
