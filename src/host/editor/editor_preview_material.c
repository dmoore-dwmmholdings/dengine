#include "editor_preview_material.h"
#include "asset/asset_internal.h"
#include "asset/budget_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"
#include "render/loaders/loaders_internal.h"

de_result de__ed_material_mesh(de_asset_system *assets, de__ed_material_meshes *meshes,
                               de__render_preview_shape shape, de_mesh *out) {
    if (!assets || !meshes || !out || (u32)shape >= DE__RENDER_PREVIEW_SHAPES)
        return DE_ERR_INVALID_ARGUMENT;
    const de_asset_type_desc *type =
        de__asset_type_get(assets, de__asset_type_find(assets, "mesh"));
    if (!type || !type->user || !type->ready || !type->unload)
        return DE_ERR_UNSUPPORTED;
    de_mesh *handle = &meshes->handles[shape];
    if (!handle->id) {
        const char *const names[] = {"__didePreview/sphere", "__didePreview/box",
                                     "__didePreview/plane"};
        const de_asset_id id = de_asset_id_from(names[shape]);
        /* An authored collision fails; never overwrite or impersonate a project asset. */
        if (de__resolved_asset(assets->resolved, id) || de__slot_find(assets, id))
            return DE_ERR_ALREADY_EXISTS;
        de__render_preview_blob cooked = {0};
        de_render_mesh *mesh = NULL;
        de_result result = de__render_preview_geometry(assets->alloc, shape, &cooked);
        if (result == DE_OK)
            result = de__mesh_from_cooked(type->user, id, cooked.data, cooked.size, &mesh);
        if (cooked.data)
            DE_FREE(assets->alloc, cooked.data, (size_t)cooked.size);
        /* Upload staging copied every byte before returning. */
        if (result != DE_OK)
            return result;
        u32 slot = 0;
        result = de__slot_create(assets, id, de_hash64_str("mesh"), &slot);
        if (result != DE_OK) {
            type->unload(mesh, type->user);
            return result;
        }
        de__slot *record = de__slot_get(assets, slot);
        de__slot_set_object(record, mesh);
        record->state = DE_LOAD_LOADING;
        (void)de__slot_ref_asset(assets, slot); /* the private context owns this reference */
        handle->id = slot;
    }
    de__slot *record = de__slot_get(assets, handle->id);
    if (!record || record->type_hash != de_hash64_str("mesh"))
        return DE_ERR_INTERNAL;
    if (record->state != DE_LOAD_READY) {
        de_render_mesh *mesh = de__slot_object(record);
        const de_result ready = type->ready(mesh, type->user);
        if (ready != DE_OK)
            return ready;
        record->bytes = mesh->gpu_bytes;
        record->state = DE_LOAD_READY;
        ++record->version;
        de__budget_on_publish(assets, handle->id, record->bytes);
    }
    *out = *handle;
    return DE_OK;
}
