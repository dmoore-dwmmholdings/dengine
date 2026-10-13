/*
 * model_cook.c: the `model` importer (DENG-101, SPEC §11.6): source → IR → sub-assets and the
 * model bytes. See model_import.h for the settings and the output, model_ir.h for the IR.
 *
 * Steps of one import:
 *   1. settings (`scale`, ...), then the front end by source type (glTF; DENG-102 adds FBX);
 *   2. axis conversion to engine coordinates with the scale, bounds;
 *   3. sub-asset names (de_model_names_build: deterministic, collision-free);
 *   4. sub-assets in a fixed order: textures (through the registered `texture` importer),
 *      materials (DMAT), meshes (the mesh stage: DMSH v1; DENG-103 replaces it), skeletons and
 *      clips (DENG-104), the prefab (TOML through the registered `prefab` importer);
 *   5. the model bytes (DMDL) and the dependencies on every sub-asset.
 *
 * Extension points (append only, DENG-103 and DENG-104): model_mesh_stage and
 * model_emit_skeletons_and_clips below. DENG-102 adds its front end in model_load_source.
 */
#include "model_import.h"
#include "model_internal.h"
#include "model_ir.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "cook/lib/de_cooker.h"
#include "render/formats/de_cooked_mesh.h"
#include "render/formats/de_cooked_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *const de_model_import_extensions[DE_MODEL_IMPORT_EXTENSION_COUNT] = {"gltf", "glb"};

/* ---- Sub-asset names ------------------------------------------------------------------------- */

/* Length of the valid UTF-8 sequence at `s` (`n` bytes left), 0 if invalid (as asset_id.c). */
static size_t utf8_len(const unsigned char *s, size_t n) {
    unsigned char c = s[0];
    if (c < 0x80)
        return 1;
    size_t len;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        if (c == 0xE0)
            lo = 0xA0;
        else if (c == 0xED)
            hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        if (c == 0xF0)
            lo = 0x90;
        else if (c == 0xF4)
            hi = 0x8F;
    } else {
        return 0;
    }
    if (len > n || s[1] < lo || s[1] > hi)
        return 0;
    for (size_t i = 2; i < len; ++i)
        if (s[i] < 0x80 || s[i] > 0xBF)
            return 0;
    return len;
}

/* `prefix` + the source name with the characters an asset ID refuses replaced by '_', or
 * `prefix` + `fallback` + index when the name is empty. */
static char *base_name(de_model_ir *ir, const char *prefix, const char *name, const char *fallback,
                       u32 index) {
    size_t pl = strlen(prefix), nl = strlen(name);
    char *out = (char *)de_model_ir_alloc(ir, pl + nl + 32u);
    if (out == NULL)
        return NULL;
    memcpy(out, prefix, pl);
    if (nl == 0) {
        snprintf(out + pl, 32u, "%s%u", fallback, index);
        return out;
    }
    const unsigned char *s = (const unsigned char *)name;
    size_t o = pl;
    for (size_t i = 0; i < nl;) {
        unsigned char c = s[i];
        size_t l = utf8_len(s + i, nl - i);
        if (l == 0 || c < 0x20 || c == 0x7F || c == '\\' || c == '*' || c == '?' || c == '#') {
            out[o++] = '_';
            i += l != 0 ? l : 1u;
            continue;
        }
        memcpy(out + o, s + i, l);
        o += l;
        i += l;
    }
    out[o] = '\0';
    return out;
}

/* An open-addressing set of the names taken so far (probed, never iterated). */
typedef struct name_set {
    const char **slots;
    u64 *hashes;
    u32 cap;
} name_set;

static bool set_has(const name_set *s, const char *n, u64 h) {
    for (u32 i = (u32)h & (s->cap - 1u);; i = (i + 1u) & (s->cap - 1u)) {
        if (s->slots[i] == NULL)
            return false;
        if (s->hashes[i] == h && strcmp(s->slots[i], n) == 0)
            return true;
    }
}

static void set_add(name_set *s, const char *n, u64 h) {
    u32 i = (u32)h & (s->cap - 1u);
    while (s->slots[i] != NULL)
        i = (i + 1u) & (s->cap - 1u);
    s->slots[i] = n;
    s->hashes[i] = h;
}

/* `base`, or `base~2`, `base~3`, ... : the first that is free, then taken. */
static const char *unique(de_model_ir *ir, name_set *set, const char *base) {
    if (base == NULL)
        return NULL;
    u64 h = de_hash64_str(base);
    if (!set_has(set, base, h)) {
        set_add(set, base, h);
        return base;
    }
    size_t bl = strlen(base);
    char *out = (char *)de_model_ir_alloc(ir, bl + 16u);
    if (out == NULL)
        return NULL;
    for (u32 k = 2;; ++k) {
        snprintf(out, bl + 16u, "%s~%u", base, k);
        h = de_hash64_str(out);
        if (!set_has(set, out, h)) {
            set_add(set, out, h);
            return out;
        }
    }
}

de_result de_model_names_build(de_model_ir *ir, de_model_names *out) {
    memset(out, 0, sizeof *out);
    u64 total = 1u + (u64)ir->node_count + ir->material_count + ir->texture_count + ir->skin_count +
                ir->anim_count + ir->mesh_count;
    name_set set = {0};
    set.cap = 16u;
    while ((u64)set.cap < total * 2u)
        set.cap *= 2u;
    set.slots = (const char **)de_model_ir_array(ir, set.cap, sizeof(char *));
    set.hashes = (u64 *)de_model_ir_array(ir, set.cap, sizeof(u64));
    out->nodes = (const char **)de_model_ir_array(ir, ir->node_count + 1u, sizeof(char *));
    out->materials = (const char **)de_model_ir_array(ir, ir->material_count + 1u, sizeof(char *));
    out->textures = (const char **)de_model_ir_array(ir, ir->texture_count + 1u, sizeof(char *));
    out->skins = (const char **)de_model_ir_array(ir, ir->skin_count + 1u, sizeof(char *));
    out->anims = (const char **)de_model_ir_array(ir, ir->anim_count + 1u, sizeof(char *));
    out->meshes = (const char **)de_model_ir_array(ir, ir->mesh_count + 1u, sizeof(char *));
    if (set.slots == NULL || set.hashes == NULL || out->nodes == NULL || out->materials == NULL ||
        out->textures == NULL || out->skins == NULL || out->anims == NULL || out->meshes == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    bool ok = true;
    out->prefab = unique(ir, &set, "prefab");
    ok = ok && out->prefab != NULL;
    for (u32 i = 0; i < ir->node_count && ok; ++i)
        ok = (out->nodes[i] = unique(ir, &set, base_name(ir, "", ir->nodes[i].name, "node", i))) !=
             NULL;
    for (u32 i = 0; i < ir->material_count && ok; ++i)
        ok = (out->materials[i] = unique(
                  ir, &set, base_name(ir, "material:", ir->materials[i].name, "", i))) != NULL;
    for (u32 i = 0; i < ir->texture_count && ok; ++i)
        ok = (out->textures[i] =
                  unique(ir, &set, base_name(ir, "texture:", ir->textures[i].name, "", i))) != NULL;
    for (u32 i = 0; i < ir->skin_count && ok; ++i)
        ok = (out->skins[i] =
                  unique(ir, &set, base_name(ir, "skeleton:", ir->skins[i].name, "", i))) != NULL;
    for (u32 i = 0; i < ir->anim_count && ok; ++i)
        ok = (out->anims[i] = unique(ir, &set, base_name(ir, "anim:", ir->anims[i].name, "", i))) !=
             NULL;
    /* Meshes: named after the first node that uses them, else `mesh:<name>`. */
    for (u32 i = 0; i < ir->node_count && ok; ++i) {
        i32 m = ir->nodes[i].mesh;
        if (m >= 0 && out->meshes[m] == NULL)
            out->meshes[m] = out->nodes[i];
    }
    for (u32 i = 0; i < ir->mesh_count && ok; ++i)
        if (out->meshes[i] == NULL)
            ok = (out->meshes[i] =
                      unique(ir, &set, base_name(ir, "mesh:", ir->meshes[i].name, "", i))) != NULL;
    return ok ? DE_OK : DE_ERR_OUT_OF_MEMORY;
}

/* ---- Mesh and model writers ------------------------------------------------------------------ */

void de_model_mesh_write(de_model_buf *b, const de_model_mesh *m, const de_asset_id *material_ids) {
    const void *streams[DE_MODEL_ATTR_COUNT] = {m->position, m->normal, m->tangent, m->uv[0],
                                                m->uv[1],    m->color0, m->joints0, m->weights0};
    u32 stream_count = 0;
    for (u32 a = 0; a < DE_MODEL_ATTR_COUNT; ++a)
        if (streams[a] != NULL)
            ++stream_count;
    u64 start = b->size;
    de_model_buf_put(b, DE_COOKED_MESH_MAGIC, 4);
    de_model_buf_u32(b, DE_COOKED_MESH_VERSION);
    de_model_buf_u32(b, 0); /* flags */
    de_model_buf_u32(b, m->vertex_count);
    de_model_buf_u32(b, m->index_count);
    de_model_buf_u32(b, m->submesh_count);
    de_model_buf_u32(b, stream_count);
    de_model_buf_u32(b, DE_COOKED_INDEX_U32);
    u64 at_tables = b->size;
    de_model_buf_put(b, NULL, 16); /* table offsets and total size, patched below */
    for (u32 k = 0; k < 3; ++k)
        de_model_buf_f32(b, m->bounds_min[k]);
    for (u32 k = 0; k < 3; ++k)
        de_model_buf_f32(b, m->bounds_max[k]);
    de_model_buf_put(b, NULL, 8);

    u64 stream_table = b->size;
    de_model_buf_put(b, NULL, (u64)stream_count * DE_COOKED_MESH_STREAM_SIZE);
    de_model_buf_align(b, 16);
    u64 sub_table = b->size;
    for (u32 s = 0; s < m->submesh_count; ++s) {
        const de_model_submesh *sm = &m->submeshes[s];
        de_model_buf_u32(b, sm->index_first);
        de_model_buf_u32(b, sm->index_count);
        de_model_buf_u32(b, sm->vertex_first);
        de_model_buf_u32(b, sm->vertex_count);
        de_model_buf_u64(b, sm->material >= 0 ? material_ids[sm->material].hash : 0u);
        for (u32 k = 0; k < 3; ++k)
            de_model_buf_f32(b, sm->bounds_min[k]);
        for (u32 k = 0; k < 3; ++k)
            de_model_buf_f32(b, sm->bounds_max[k]);
    }
    u32 row = 0;
    for (u32 a = 0; a < DE_MODEL_ATTR_COUNT; ++a) {
        if (streams[a] == NULL)
            continue;
        de_model_buf_align(b, 16);
        u64 off = b->size - start;
        u32 fmt = de_cooked_stream_format(a);
        u64 n = (u64)m->vertex_count *
                (de_cooked_vertex_format_size(fmt) / (a == DE_MODEL_ATTR_JOINTS0 ? 2u : 4u));
        if (a == DE_MODEL_ATTR_JOINTS0) {
            for (u64 i = 0; i < n; ++i) {
                u8 x[2] = {(u8)m->joints0[i], (u8)(m->joints0[i] >> 8)};
                de_model_buf_put(b, x, 2);
            }
        } else {
            const f32 *f = (const f32 *)streams[a];
            for (u64 i = 0; i < n; ++i)
                de_model_buf_f32(b, f[i]);
        }
        u64 rec = stream_table + (u64)row * DE_COOKED_MESH_STREAM_SIZE;
        de_model_buf_patch_u32(b, rec, a);
        de_model_buf_patch_u32(b, rec + 4, fmt);
        de_model_buf_patch_u32(b, rec + 8, (u32)off);
        de_model_buf_patch_u32(b, rec + 12, (u32)(b->size - start - off));
        ++row;
    }
    de_model_buf_align(b, 16);
    u64 index_off = b->size - start;
    for (u32 i = 0; i < m->index_count; ++i)
        de_model_buf_u32(b, m->indices[i]);
    de_model_buf_patch_u32(b, at_tables, (u32)(stream_table - start));
    de_model_buf_patch_u32(b, at_tables + 4, (u32)(sub_table - start));
    de_model_buf_patch_u32(b, at_tables + 8, (u32)index_off);
    de_model_buf_patch_u32(b, at_tables + 12, (u32)(b->size - start));
}

/* The DMDL blob. `mesh_index[i]` is IR mesh i's index in `mesh_ids` (DE_COOKED_NONE if it was
 * not cooked); node skins point into `skeleton_ids` when there is one per IR skin. */
static void model_write(de_model_buf *b, const de_model_ir *ir, const de_model_names *names,
                        const u32 *mesh_index, const de_asset_id *mesh_ids, u32 mesh_count,
                        const de_asset_id *material_ids, const de_asset_id *texture_ids,
                        const de_asset_id *skeleton_ids, u32 skeleton_count,
                        const de_asset_id *clip_ids, u32 clip_count, de_asset_id prefab_id) {
    de_model_buf_put(b, DE_COOKED_MODEL_MAGIC, 4);
    de_model_buf_u32(b, DE_COOKED_MODEL_VERSION);
    de_model_buf_u32(b, 0);
    de_model_buf_u32(b, ir->node_count);
    de_model_buf_u32(b, ir->root_count);
    de_model_buf_u32(b, mesh_count);
    de_model_buf_u32(b, ir->material_count);
    de_model_buf_u32(b, ir->texture_count);
    de_model_buf_u32(b, skeleton_count);
    de_model_buf_u32(b, clip_count);
    u64 at_offsets = b->size;
    de_model_buf_put(b, NULL, 24); /* 4 offsets, string size, total: patched below */
    de_model_buf_u64(b, prefab_id.hash);
    de_model_buf_u64(b, 0);

    u64 node_off = b->size;
    u32 str = 0;
    for (u32 i = 0; i < ir->node_count; ++i) {
        const de_model_node *n = &ir->nodes[i];
        u32 len = (u32)strlen(names->nodes[i]);
        de_model_buf_u32(b, str);
        de_model_buf_u32(b, len);
        str += len + 1u;
        de_model_buf_u32(b, n->parent >= 0 ? (u32)n->parent : DE_COOKED_NONE);
        de_model_buf_u32(b, n->mesh >= 0 ? mesh_index[n->mesh] : DE_COOKED_NONE);
        de_model_buf_u32(b, n->skin >= 0 && skeleton_count == ir->skin_count ? (u32)n->skin
                                                                             : DE_COOKED_NONE);
        de_model_buf_u32(b, 0);
        for (u32 k = 0; k < 3; ++k)
            de_model_buf_f32(b, n->translation[k]);
        for (u32 k = 0; k < 4; ++k)
            de_model_buf_f32(b, n->rotation[k]);
        for (u32 k = 0; k < 3; ++k)
            de_model_buf_f32(b, n->scale[k]);
    }
    u64 root_off = b->size;
    for (u32 i = 0; i < ir->root_count; ++i)
        de_model_buf_u32(b, ir->roots[i]);
    de_model_buf_align(b, 8);
    u64 id_off = b->size;
    for (u32 i = 0; i < mesh_count; ++i)
        de_model_buf_u64(b, mesh_ids[i].hash);
    for (u32 i = 0; i < ir->material_count; ++i)
        de_model_buf_u64(b, material_ids[i].hash);
    for (u32 i = 0; i < ir->texture_count; ++i)
        de_model_buf_u64(b, texture_ids[i].hash);
    for (u32 i = 0; i < skeleton_count; ++i)
        de_model_buf_u64(b, skeleton_ids[i].hash);
    for (u32 i = 0; i < clip_count; ++i)
        de_model_buf_u64(b, clip_ids[i].hash);
    u64 str_off = b->size;
    for (u32 i = 0; i < ir->node_count; ++i)
        de_model_buf_put(b, names->nodes[i], strlen(names->nodes[i]) + 1u);
    de_model_buf_patch_u32(b, at_offsets, (u32)node_off);
    de_model_buf_patch_u32(b, at_offsets + 4, (u32)root_off);
    de_model_buf_patch_u32(b, at_offsets + 8, (u32)id_off);
    de_model_buf_patch_u32(b, at_offsets + 12, (u32)str_off);
    de_model_buf_patch_u32(b, at_offsets + 16, str);
    de_model_buf_patch_u32(b, at_offsets + 20, (u32)b->size);
}

/* ---- The importer ---------------------------------------------------------------------------- */

/** The importer's settings (all of them are in the settings hash, read or not). */
typedef struct model_settings {
    f32 scale;
    bool import_cameras;
    /* Read for DENG-103's mesh stage. */
    bool generate_lods, meshlets;
    f32 lod_error;
} model_settings;

static de_result read_file_cb(void *user, const char *rel, const void **data, u64 *size) {
    return de_asset_import_read_file((de_asset_import_ctx *)user, rel, data, size);
}

/* Front ends by source type. DENG-102: `.fbx` goes to the FBX front end here. */
static de_result model_load_source(de_asset_import_ctx *ctx, const de_model_source *src,
                                   de_model_ir *ir, de_model_axes *axes) {
    (void)ctx;
    *axes = DE_MODEL_AXES_GLTF;
    return de_model_gltf_load(src, ir);
}

/*
 * DENG-103 extension point: the mesh stage. Called once per cooked mesh, in mesh order, with the
 * IR in engine coordinates; writes the mesh sub-asset's bytes to `out`. Version 1 writes the DMSH
 * v1 blob as it is. DENG-103 adds meshlets, quantization and the LOD chain here (mesh v2), using
 * `s->meshlets`, `s->generate_lods` and `s->lod_error`.
 */
static de_result model_mesh_stage(de_asset_import_ctx *ctx, const model_settings *s,
                                  de_model_ir *ir, u32 mesh, const de_asset_id *material_ids,
                                  de_model_buf *out) {
    (void)ctx;
    (void)s;
    de_model_mesh_write(out, &ir->meshes[mesh], material_ids);
    return out->failed ? DE_ERR_OUT_OF_MEMORY : DE_OK;
}

/*
 * DENG-104 extension point: skeleton and clip sub-assets (`skeleton:<name>` and `anim:<name>`
 * from `names`), from ir->skins and ir->anims (raw, already in engine coordinates). Emit one
 * skeleton per IR skin in skin order, or none, so node skin indices stay valid; set the counts
 * and the IDs. Version 1 emits nothing.
 */
static de_result model_emit_skeletons_and_clips(de_asset_import_ctx *ctx, const model_settings *s,
                                                de_model_ir *ir, const de_model_names *names,
                                                de_asset_id *skeleton_ids, u32 *skeleton_count,
                                                de_asset_id *clip_ids, u32 *clip_count) {
    (void)ctx;
    (void)s;
    (void)ir;
    (void)names;
    (void)skeleton_ids;
    (void)clip_ids;
    *skeleton_count = 0;
    *clip_count = 0;
    return DE_OK;
}

static de_asset_id sub_id(de_model_ir *ir, const char *model_id, const char *name) {
    size_t n = strlen(model_id) + 1u + strlen(name) + 1u;
    char *full = (char *)de_model_ir_alloc(ir, n);
    if (full == NULL)
        return (de_asset_id){0};
    snprintf(full, n, "%s#%s", model_id, name);
    return de_asset_id_from(full);
}

/* The failure of a step as the import's failure (the error context's message). */
static de_result fail_from_error(de_asset_import_ctx *ctx, de_result r) {
    const de_error *e = de_error_last();
    if (e != NULL && e->result == r)
        return de_asset_import_fail(ctx, r, "%s", e->message);
    return de_asset_import_fail(ctx, r, "%s", de_result_str(r));
}

static de_result import_ir(de_asset_import_ctx *ctx, const model_settings *s, de_model_ir *ir) {
    const char *model_id = de_cooker_import_id_str(ctx);
    const char *source_path = de_asset_import_source_path(ctx);
    de_model_names names;
    DE_TRY(de_model_names_build(ir, &names));

    u32 nmax = ir->texture_count + ir->material_count + ir->mesh_count + ir->skin_count +
               ir->anim_count + 1u;
    de_asset_id *texture_ids = (de_asset_id *)de_model_ir_array(ir, nmax, sizeof(de_asset_id));
    de_asset_id *material_ids = texture_ids + ir->texture_count;
    de_asset_id *mesh_ids = material_ids + ir->material_count;
    de_asset_id *skeleton_ids = mesh_ids + ir->mesh_count;
    de_asset_id *clip_ids = skeleton_ids + ir->skin_count;
    u32 *mesh_index = (u32 *)de_model_ir_array(ir, ir->mesh_count + 1u, sizeof(u32));
    if (texture_ids == NULL || mesh_index == NULL)
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 i = 0; i < ir->texture_count; ++i)
        texture_ids[i] = sub_id(ir, model_id, names.textures[i]);
    for (u32 i = 0; i < ir->material_count; ++i)
        material_ids[i] = sub_id(ir, model_id, names.materials[i]);

    /* Textures, through the registered texture importer (pass-through until there is one). */
    bool raw_textures = false;
    for (u32 i = 0; i < ir->texture_count; ++i) {
        const de_model_texture *t = &ir->textures[i];
        de__manifest_kv kv[2];
        memset(kv, 0, sizeof kv);
        kv[0].key = "srgb";
        kv[0].value.type = DE__MANIFEST_BOOL;
        kv[0].value.u.b = de_model_tex_usage_srgb(t->usage);
        kv[1].key = "usage";
        kv[1].value.type = DE__MANIFEST_STRING;
        kv[1].value.u.s = de_model_tex_usage_name(t->usage);
        bool cooked = false;
        DE_TRY(de_cooker_import_cook_subasset(ctx, names.textures[i], "texture", t->data, t->size,
                                              kv, 2, &cooked));
        raw_textures = raw_textures || !cooked;
        DE_TRY(de_asset_import_add_dependency(ctx, texture_ids[i]));
    }
    if (raw_textures)
        de_model_ir_warn(ir, "no `texture` importer is registered: texture sub-assets hold the "
                             "image files as they are");

    /* Materials. */
    for (u32 i = 0; i < ir->material_count; ++i) {
        de_model_buf b;
        de_model_buf_init(&b, ir);
        de_model_material_write(&b, &ir->materials[i], texture_ids);
        if (b.failed)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for a material");
        DE_TRY(de_asset_import_add_subasset(ctx, names.materials[i], "material", b.data, b.size));
        DE_TRY(de_asset_import_add_dependency(ctx, material_ids[i]));
    }

    /* Meshes (those with triangles), in mesh order. */
    u32 mesh_count = 0;
    for (u32 i = 0; i < ir->mesh_count; ++i) {
        mesh_index[i] = DE_COOKED_NONE;
        if (ir->meshes[i].submesh_count == 0)
            continue;
        de_model_buf b;
        de_model_buf_init(&b, ir);
        DE_TRY(model_mesh_stage(ctx, s, ir, i, material_ids, &b));
        DE_TRY(de_asset_import_add_subasset(ctx, names.meshes[i], "mesh", b.data, b.size));
        mesh_ids[mesh_count] = sub_id(ir, model_id, names.meshes[i]);
        DE_TRY(de_asset_import_add_dependency(ctx, mesh_ids[mesh_count]));
        mesh_index[i] = mesh_count++;
    }

    u32 skeleton_count = 0, clip_count = 0;
    DE_TRY(model_emit_skeletons_and_clips(ctx, s, ir, &names, skeleton_ids, &skeleton_count,
                                          clip_ids, &clip_count));
    for (u32 i = 0; i < skeleton_count; ++i)
        DE_TRY(de_asset_import_add_dependency(ctx, skeleton_ids[i]));
    for (u32 i = 0; i < clip_count; ++i)
        DE_TRY(de_asset_import_add_dependency(ctx, clip_ids[i]));

    /* The prefab, through the registered prefab importer. */
    for (u32 i = 0; i < ir->node_count; ++i) {
        i32 m = ir->nodes[i].mesh;
        if (ir->nodes[i].in_scene && m >= 0 && ir->meshes[m].submesh_count > 8u)
            de_model_ir_warn(ir,
                             "node \"%s\": %u submeshes; the prefab's DeMeshRenderer names the "
                             "first 8 materials (the others use the mesh's defaults)",
                             names.nodes[i], ir->meshes[m].submesh_count);
    }
    de_model_buf toml;
    de_model_buf_init(&toml, ir);
    if (!de_model_prefab_toml(&toml, ir, &names, model_id, source_path))
        return toml.failed ? DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for the prefab")
                           : DE_FAIL(DE_ERR_INTERNAL,
                                     "two node names give the same prefab scene ID; rename one");
    bool cooked = false;
    DE_TRY(de_cooker_import_cook_subasset(ctx, names.prefab, "prefab", toml.data, toml.size, NULL,
                                          0, &cooked));
    if (!cooked)
        de_model_ir_warn(ir, "no `prefab` importer is registered: the prefab sub-asset holds its "
                             "TOML source");
    de_asset_id prefab_id = sub_id(ir, model_id, names.prefab);
    DE_TRY(de_asset_import_add_dependency(ctx, prefab_id));

    de_model_buf b;
    de_model_buf_init(&b, ir);
    model_write(&b, ir, &names, mesh_index, mesh_ids, mesh_count, material_ids, texture_ids,
                skeleton_ids, skeleton_count, clip_ids, clip_count, prefab_id);
    if (b.failed)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "out of memory for the model");
    return de_asset_import_write(ctx, b.data, b.size);
}

de_result de_model_import(de_asset_import_ctx *ctx) {
    model_settings s;
    memset(&s, 0, sizeof s);
    f64 scale = de_asset_import_setting_float(ctx, "scale", 1.0);
    if (!(scale > 0.0) || !(scale < 1e6) || isnan(scale))
        return de_asset_import_fail(ctx, DE_ERR_INVALID_ARGUMENT,
                                    "setting `scale` must be a number above 0 (got %g)", scale);
    s.scale = (f32)scale;
    s.import_cameras = de_asset_import_setting_bool(ctx, "import_cameras", false);
    s.generate_lods = de_asset_import_setting_bool(ctx, "generate_lods", false);
    s.meshlets = de_asset_import_setting_bool(ctx, "meshlets", false);
    s.lod_error = (f32)de_asset_import_setting_float(ctx, "lod_error", 0.01);

    de_model_ir ir;
    de_model_ir_init(&ir, de_asset_import_allocator(ctx), de_cooker_import_id_str(ctx));
    if (s.import_cameras)
        de_model_ir_warn(&ir, "import_cameras: cameras and lights are not imported yet");
    u64 size = 0;
    const void *data = de_asset_import_source(ctx, &size);
    de_model_source src = {data, size, de_asset_import_source_path(ctx), read_file_cb, ctx};
    de_model_axes axes = DE_MODEL_AXES_ENGINE;
    de_error_clear();
    de_result r = model_load_source(ctx, &src, &ir, &axes);
    if (r == DE_OK) {
        de_model_ir_convert_axes(&ir, axes, s.scale);
        de_model_ir_compute_bounds(&ir);
        r = import_ir(ctx, &s, &ir);
    }
    if (r != DE_OK)
        (void)fail_from_error(ctx, r);
    de_model_ir_free(&ir);
    return r;
}
