#include "editor_model_inspection.h"
#include "render/formats/de_cooked_model.h"
#include <string.h>

static void *allocate(de_ep_arena *arena, size_t count, size_t stride) {
    if (!count)
        return NULL;
    if (count > SIZE_MAX / stride || arena->used > SIZE_MAX - 7)
        return NULL;
    size_t at = (arena->used + 7) & ~(size_t)7;
    size_t bytes = count * stride;
    if (!arena->base || at > arena->size || bytes > arena->size - at)
        return NULL;
    void *result = arena->base + at;
    memset(result, 0, bytes);
    arena->used = at + bytes;
    return result;
}
static de_ep_list_f32 vector(const f32 value[3]) {
    return (de_ep_list_f32){(f32 *)value, 3};
}
static const char *format_name(u32 format) {
    static const char *const names[] = {NULL,        "f32x2",     "f32x3", "f32x4",
                                        "u16x4",     "unorm16x4", "oct16", "oct16_sign",
                                        "unorm16x2", "unorm8x4",  "u8x4"};
    return format < sizeof names / sizeof names[0] ? names[format] : NULL;
}
de_result de__ed_model_inspect(const de_render_model *model, const de_render_mesh *mesh,
                               u32 selected, const de__ed_model_names *names, de_ep_arena *arena,
                               de_ep_model_inspection *out) {
    if (!model || !names || !names->handle || !names->asset || !arena || !out)
        return DE_ERR_INVALID_ARGUMENT;
    if (model->node_count > 8192 || model->mesh_count > 1024)
        return DE_ERR_UNSUPPORTED;
    if ((model->node_count && !model->nodes) || (model->mesh_count && !model->meshes))
        return DE_ERR_PARSE;
    if ((model->mesh_count && selected >= model->mesh_count) || (!model->mesh_count && selected))
        return DE_ERR_INVALID_ARGUMENT;
    if (model->mesh_count && !mesh)
        return DE_ERR_NOT_FOUND;
    if (mesh && (mesh->submesh_count > 4096 || mesh->lod_count > DE_COOKED_MESH_MAX_LODS))
        return DE_ERR_UNSUPPORTED;
    if (mesh &&
        (!mesh->lod_count || (mesh->submesh_count && (!mesh->submeshes || !mesh->lod_submeshes))))
        return DE_ERR_PARSE;
    de_ep_model_inspection result = {0};
    result.nodes.items = allocate(arena, model->node_count, sizeof *result.nodes.items);
    result.meshes.items = allocate(arena, model->mesh_count, sizeof *result.meshes.items);
    if ((model->node_count && !result.nodes.items) || (model->mesh_count && !result.meshes.items))
        return DE_ERR_OUT_OF_MEMORY;
    result.nodes.count = model->node_count;
    result.meshes.count = model->mesh_count;
    for (u32 i = 0; i < model->mesh_count; ++i) {
        const char *id = names->handle(names->user, model->meshes[i].id);
        if (!id || !*id)
            return DE_ERR_NOT_FOUND;
        result.meshes.items[i] = de_ep_cstr(id);
    }
    for (u32 i = 0; i < model->node_count; ++i) {
        const de_render_model_node *source = &model->nodes[i];
        de_ep_model_node *node = &result.nodes.items[i];
        node->name = de_ep_cstr(source->name ? source->name : "");
        if (source->parent != DE_COOKED_NONE) {
            if (source->parent >= model->node_count)
                return DE_ERR_PARSE;
            node->has_parent = true;
            node->parent = source->parent;
        }
        if (source->mesh.id) {
            const char *id = names->handle(names->user, source->mesh.id);
            if (!id)
                return DE_ERR_NOT_FOUND;
            node->has_mesh = true;
            node->mesh = de_ep_cstr(id);
            bool found = false;
            for (u32 m = 0; m < model->mesh_count; ++m)
                found |= model->meshes[m].id == source->mesh.id;
            if (!found)
                return DE_ERR_PARSE;
        }
    }
    if (!model->mesh_count) {
        *out = result;
        return DE_OK;
    }
    result.has_mesh = true;
    result.mesh = selected;
    result.vertices = mesh->vertex_count;
    result.bounds_min = vector(mesh->bounds_min);
    result.bounds_max = vector(mesh->bounds_max);
    result.position_offset = vector(mesh->position_offset);
    result.position_scale = vector(mesh->position_scale);
    result.max_position_error = mesh->max_position_error;
    result.lods.items = allocate(arena, mesh->lod_count, sizeof *result.lods.items);
    result.attributes.items =
        allocate(arena, DE_COOKED_STREAM_COUNT, sizeof *result.attributes.items);
    result.uv_channels.items = allocate(arena, 2, sizeof *result.uv_channels.items);
    result.submeshes.items = allocate(arena, mesh->submesh_count, sizeof *result.submeshes.items);
    if (!result.lods.items || !result.attributes.items || !result.uv_channels.items ||
        (mesh->submesh_count && !result.submeshes.items))
        return DE_ERR_OUT_OF_MEMORY;
    result.lods.count = mesh->lod_count;
    for (u32 l = 0; l < mesh->lod_count; ++l) {
        const de_cooked_mesh_lod *lod = &mesh->lods[l];
        if (lod->index_count % 3 || lod->triangle_count != lod->index_count / 3)
            return DE_ERR_PARSE;
        result.lods.items[l] = (de_ep_model_mesh_lod){.triangles = lod->triangle_count,
                                                      .meshlets = lod->meshlet_count,
                                                      .vertices = lod->vertex_count,
                                                      .error_metres = lod->error};
    }
    static const char *const semantics[] = {"position", "normal", "tangent", "uv0",
                                            "uv1",      "color",  "joints",  "weights"};
    for (u32 i = 0; i < DE_COOKED_STREAM_COUNT; ++i) {
        if (!mesh->stream_format[i])
            continue;
        const char *name = format_name(mesh->stream_format[i]);
        if (!name || !de_cooked_stream_format_ok(i, mesh->stream_format[i]))
            return DE_ERR_PARSE;
        result.attributes.items[result.attributes.count++] =
            (de_ep_model_attribute){.name = de_ep_cstr(semantics[i]), .format = de_ep_cstr(name)};
        if (i == DE_COOKED_STREAM_UV0 || i == DE_COOKED_STREAM_UV1)
            result.uv_channels.items[result.uv_channels.count++] = i - DE_COOKED_STREAM_UV0;
    }
    result.submeshes.count = mesh->submesh_count;
    for (u32 s = 0; s < mesh->submesh_count; ++s) {
        const de_render_submesh *source = &mesh->submeshes[s];
        de_ep_model_submesh *submesh = &result.submeshes.items[s];
        const char *material =
            source->material.hash ? names->asset(names->user, source->material) : "";
        if (!material)
            return DE_ERR_NOT_FOUND;
        submesh->material = de_ep_cstr(material);
        submesh->bounds_min = vector(source->bounds_min);
        submesh->bounds_max = vector(source->bounds_max);
        submesh->lods.items = allocate(arena, mesh->lod_count, sizeof *submesh->lods.items);
        if (!submesh->lods.items)
            return DE_ERR_OUT_OF_MEMORY;
        submesh->lods.count = mesh->lod_count;
        for (u32 l = 0; l < mesh->lod_count; ++l) {
            const de_cooked_mesh_range *range = &mesh->lod_submeshes[l * mesh->submesh_count + s];
            const de_cooked_mesh_lod *lod = &mesh->lods[l];
            if (range->index_count % 3 || range->index_first < lod->index_first ||
                range->index_count > lod->index_count ||
                range->index_first - lod->index_first > lod->index_count - range->index_count ||
                range->meshlet_first < lod->meshlet_first ||
                range->meshlet_count > lod->meshlet_count ||
                range->meshlet_first - lod->meshlet_first >
                    lod->meshlet_count - range->meshlet_count)
                return DE_ERR_PARSE;
            submesh->lods.items[l] =
                (de_ep_model_submesh_lod){.index_first = range->index_first,
                                          .index_count = range->index_count,
                                          .meshlet_first = range->meshlet_first,
                                          .triangles = range->index_count / 3,
                                          .meshlets = range->meshlet_count};
        }
    }
    *out = result;
    return DE_OK;
}
