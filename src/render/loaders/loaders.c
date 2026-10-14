/*
 * loaders.c: registration of the render asset types, the `model` type, and the record accessors
 * (SPEC §11.1, §11.4, §11.6; DENG-105). render_assets.h describes the whole path.
 *
 * Types and their importers (DE_HAS_COOKER builds only; Release and Shipping register none):
 *   texture   de_texture_import (PNG, JPEG, EXR, KTX2 -> DTEX; environments -> DTEX + ENVS)
 *   model     de_model_import (glTF, GLB, FBX -> DMDL plus texture, material, mesh, skeleton,
 *             clip and prefab sub-assets, which the model depends on)
 *   mesh      none: sub-assets of models (DMSH v2)
 *   material  none: sub-assets of models (DMAT)
 *
 * The model loader keeps the node table and, in `ready` (main thread, every sub-asset READY),
 * resolves the mesh, material and prefab IDs to handles. A model's sub-asset dependencies come
 * from the importer (cook stage or pack TOC), not from the loader.
 */

#include "render/loaders/loaders_internal.h"

#include "render/formats/de_cooked_model.h"
#include "render/loaders/render_assets.h"
#include "render/loaders/upload_ring.h"

#include "asset/asset_internal.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>

#if DE_HAS_COOKER
#include "cook/importers/model/model_import.h"
#include "cook/importers/texture/texture_import.h"
#endif

#include <string.h>

static size_t align16(size_t v) {
    return (v + 15u) & ~(size_t)15u;
}

/* ---- model --------------------------------------------------------------------------------- */

/* The record and everything it points to, in one block: nodes, roots, meshes, materials, the
 * mesh and material IDs (for `ready`), the node names. */
typedef struct model_obj {
    de_render_model pub; /* first: the slot's object is a de_render_model * */
    de_asset_id *mesh_ids, *material_ids;
    u32 *node_mesh; /* per node: index into mesh_ids, DE_COOKED_NONE */
    u64 prefab_id;
} model_obj;

de_result de__model_load(de_asset_load_ctx *ctx) {
    de_render_uploader *u = de__loader_uploader(ctx);
    if (u == NULL)
        return DE_ERR_INTERNAL;
    u64 size = 0;
    const void *p = de_asset_load_ctx_data(ctx, &size);
    de_cooked_model_view v;
    const char *why = NULL;
    if (!de_cooked_model_parse(p, size, &v, &why))
        return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "not a cooked model: %s", why);

    const size_t o_nodes = align16(sizeof(model_obj));
    const size_t o_roots = o_nodes + align16(sizeof(de_render_model_node) * v.node_count);
    const size_t o_meshes = o_roots + align16(sizeof(u32) * v.root_count);
    const size_t o_mats = o_meshes + align16(sizeof(de_mesh) * v.mesh_count);
    const size_t o_mesh_ids = o_mats + align16(sizeof(de_material) * v.material_count);
    const size_t o_mat_ids = o_mesh_ids + align16(sizeof(de_asset_id) * v.mesh_count);
    const size_t o_node_mesh = o_mat_ids + align16(sizeof(de_asset_id) * v.material_count);
    const size_t o_strings = o_node_mesh + align16(sizeof(u32) * v.node_count);
    const size_t block = o_strings + (size_t)v.string_size + 1u;
    u8 *b = (u8 *)DE_ALLOC(u->alloc, block, 16, DE_MEM_RENDER);
    if (b == NULL)
        return de_asset_load_ctx_fail(ctx, DE_ERR_OUT_OF_MEMORY, "no memory for the model");
    memset(b, 0, block);
    model_obj *m = (model_obj *)b;
    de_render_model_node *nodes = (de_render_model_node *)(b + o_nodes);
    u32 *roots = (u32 *)(b + o_roots);
    char *strings = (char *)(b + o_strings);
    memcpy(strings, v.strings, v.string_size);
    m->pub.block_size_ = block;
    m->pub.node_count = v.node_count;
    m->pub.root_count = v.root_count;
    m->pub.mesh_count = v.mesh_count;
    m->pub.material_count = v.material_count;
    m->pub.nodes = nodes;
    m->pub.roots = roots;
    m->pub.meshes = (const de_mesh *)(b + o_meshes);
    m->pub.materials = (const de_material *)(b + o_mats);
    m->mesh_ids = (de_asset_id *)(b + o_mesh_ids);
    m->material_ids = (de_asset_id *)(b + o_mat_ids);
    m->node_mesh = (u32 *)(b + o_node_mesh);
    m->prefab_id = v.prefab_id;
    for (u32 i = 0; i < v.mesh_count; ++i)
        m->mesh_ids[i].hash = de_cooked_model_mesh_id(&v, i);
    for (u32 i = 0; i < v.material_count; ++i)
        m->material_ids[i].hash = de_cooked_model_material_id(&v, i);
    for (u32 i = 0; i < v.root_count; ++i)
        roots[i] = de_cooked_model_root(&v, i);
    for (u32 i = 0; i < v.node_count; ++i) {
        de_cooked_model_node n = de_cooked_model_get_node(&v, i);
        de_render_model_node *d = &nodes[i];
        d->name = strings + (n.name - (const char *)v.strings);
        d->parent = n.parent;
        d->skin = n.skin;
        m->node_mesh[i] = n.mesh;
        memcpy(d->translation, n.translation, sizeof d->translation);
        memcpy(d->rotation, n.rotation, sizeof d->rotation);
        memcpy(d->scale, n.scale, sizeof d->scale);
    }
    de_asset_load_ctx_set_object(ctx, m, block);
    return DE_OK;
}

void de__model_unload(void *object, void *user) {
    de_render_uploader *u = (de_render_uploader *)user;
    model_obj *m = (model_obj *)object;
    if (m != NULL)
        DE_FREE(u->alloc, m, m->pub.block_size_);
}

/* Main thread, every sub-asset READY: IDs to handles. A sub-asset this runtime cannot name (its
 * type is not registered) leaves its handle invalid; the model still loads. */
de_result de__model_ready(void *object, void *user) {
    de_render_uploader *u = (de_render_uploader *)user;
    model_obj *m = (model_obj *)object;
    de_mesh *meshes = (de_mesh *)m->pub.meshes;
    de_material *mats = (de_material *)m->pub.materials;
    for (u32 i = 0; i < m->pub.mesh_count; ++i)
        meshes[i] = de_asset_mesh(u->assets, m->mesh_ids[i]);
    for (u32 i = 0; i < m->pub.material_count; ++i)
        mats[i] = de_asset_material(u->assets, m->material_ids[i]);
    de_render_model_node *nodes = (de_render_model_node *)m->pub.nodes;
    for (u32 i = 0; i < m->pub.node_count; ++i)
        nodes[i].mesh =
            m->node_mesh[i] < m->pub.mesh_count ? meshes[m->node_mesh[i]] : (de_mesh){0};
    m->pub.prefab = m->prefab_id != 0 ? de_asset_prefab(u->assets, (de_asset_id){m->prefab_id})
                                      : (de_prefab){0};
    de_error_clear(); /* a handle that could not be made is not the model's failure */
    return DE_OK;
}

/* ---- Registration -------------------------------------------------------------------------- */

#if !DE_HAS_COOKER
static const char *const k_texture_exts[] = {".png", ".jpg", ".jpeg", ".exr", ".ktx2"};
static const char *const k_model_exts[] = {".gltf", ".glb", ".fbx"};
#endif

de_result de__render_register_asset_types_ex(de_asset_system *s, const de__render_assets_desc *d,
                                             de_render_uploader **out) {
    if (out != NULL)
        *out = NULL;
    if (s == NULL || d == NULL || d->device == NULL || out == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "de_render_register_asset_types: NULL system, device or out");
    static const char *const names[] = {"texture", "mesh", "material", "model"};
    for (u32 i = 0; i < 4u; ++i)
        if (DE_HANDLE_VALID(de__asset_type_find(s, names[i])))
            return DE_FAIL(DE_ERR_ALREADY_EXISTS,
                           "de_render_register_asset_types: the type \"%s\" is registered already",
                           names[i]);

    const de_allocator *a = d->allocator != NULL ? d->allocator : de_heap_allocator();
    de_render_uploader *u = DE_NEW(a, de_render_uploader, DE_MEM_RENDER);
    if (u == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "render assets: out of memory");
    memset(u, 0, sizeof *u);
    u->alloc = a;
    u->dev = d->device;
    u->assets = s;
    de_result r = de__uploader_init(
        u, d->ring_bytes != 0 ? d->ring_bytes : s->desc.staging_ring_bytes, d->frame_cap_bytes);
    if (r != DE_OK) {
        de__uploader_fini(u);
        DE_FREE(a, u, sizeof *u);
        return r;
    }

    de_asset_type_desc td[4];
    memset(td, 0, sizeof td);
    td[0].name = "texture";
    td[0].load = de__texture_load;
    td[0].unload = de__texture_unload;
    td[0].ready = de__texture_ready;
    td[0].category = DE_ASSET_CAT_GPU_TEXTURE;
    td[1].name = "mesh";
    td[1].load = de__mesh_load;
    td[1].unload = de__mesh_unload;
    td[1].ready = de__mesh_ready;
    td[1].category = DE_ASSET_CAT_GPU_MESH;
    td[2].name = "material";
    td[2].load = de__material_load;
    td[2].unload = de__material_unload;
    td[2].ready = de__material_ready;
    td[3].name = "model";
    td[3].load = de__model_load;
    td[3].unload = de__model_unload;
    td[3].ready = de__model_ready;
    td[0].importer_version = td[3].importer_version = 1u;
#if DE_HAS_COOKER
    td[0].extensions = de_texture_import_extensions;
    td[0].extension_count = DE_TEXTURE_IMPORT_EXTENSION_COUNT;
    td[3].extensions = de_model_import_extensions;
    td[3].extension_count = DE_MODEL_IMPORT_EXTENSION_COUNT;
    if (!d->no_import) {
        td[0].import = de_texture_import;
        td[0].importer_version = DE_TEXTURE_IMPORTER_VERSION;
        td[3].import = de_model_import;
        td[3].importer_version = DE_MODEL_IMPORTER_VERSION;
    }
#else
    td[0].extensions = k_texture_exts;
    td[0].extension_count = (u32)(sizeof k_texture_exts / sizeof k_texture_exts[0]);
    td[3].extensions = k_model_exts;
    td[3].extension_count = (u32)(sizeof k_model_exts / sizeof k_model_exts[0]);
#endif
    for (u32 i = 0; i < 4u; ++i) {
        td[i].user = u;
        if (!DE_HANDLE_VALID(de_asset_type_register(s, &td[i]))) {
            r = de__loader_rhi_error();
            if (i == 0) {
                de__uploader_fini(u);
                DE_FREE(a, u, sizeof *u);
                return r;
            }
            /* Registered types keep `u` as their user: it must outlive the system. */
            u->broken = true;
            *out = u;
            return r;
        }
    }
    *out = u;
    return DE_OK;
}

de_result de_render_register_asset_types(de_asset_system *s, de_gpu_device *d,
                                         const de_allocator *a, de_render_uploader **out) {
    de__render_assets_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.device = d;
    desc.allocator = a;
    return de__render_register_asset_types_ex(s, &desc, out);
}

void de_render_uploader_destroy(de_render_uploader *u) {
    if (u == NULL)
        return;
    const de_allocator *a = u->alloc;
    de__uploader_fini(u);
    DE_FREE(a, u, sizeof *u);
}

/* ---- Accessors ------------------------------------------------------------------------------ */

/* The slot's object: set only when the asset is published (READY), so no state check, which
 * would read a main-thread field. The type hash is fixed when the slot is made. */
static void *object_of(de_asset_system *s, u32 handle, const char *type) {
    const de__slot *sl = s != NULL ? de__slot_get(s, handle) : NULL;
    if (sl == NULL || sl->type_hash != de_hash64_str(type))
        return NULL;
    return de__slot_object(sl);
}

const de_render_texture *de_render_texture_get(de_asset_system *s, de_texture t) {
    return (const de_render_texture *)object_of(s, t.id, "texture");
}
const de_render_mesh *de_render_mesh_get(de_asset_system *s, de_mesh m) {
    return (const de_render_mesh *)object_of(s, m.id, "mesh");
}
const de_render_material *de_render_material_get(de_asset_system *s, de_material m) {
    return (const de_render_material *)object_of(s, m.id, "material");
}
const de_render_model *de_render_model_get(de_asset_system *s, de_model m) {
    return (const de_render_model *)object_of(s, m.id, "model");
}
