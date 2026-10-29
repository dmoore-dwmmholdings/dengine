/* DENG-695: one isolated, reusable model-preview world. Never inserts game-world entities. */
#include "editor_internal.h"
#include "editor_preview_image.h"
#include "asset/asset_internal.h"
#include "platform/de_atomic.h"
#include "render/loaders/render_assets.h"
#include "render/formats/de_cooked_model.h"
#include "render/world/render_world.h"
#include <dengine/de_anim.h>
#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PREVIEW_NODES      8192u
#define PREVIEW_TIMEOUT_NS (30ull * 1000000000ull)
#define PREVIEW_IDLE_NS    (5ull * 1000000000ull)

typedef struct preview_context {
    de_gpu_device *device;
    de_type_registry *types;
    de_world *world;
    de_asset_system *assets;
    de_anim_system *anim;
    de_renderer *renderer;
    de_load_handle load;
    char id[1024], root[DE_FS_PATH_MAX];
    u64 revision, last_use;
    de_view view;
    de_gpu_texture target;
    de_gpu_buffer readback;
    const void *mapped;
    u32 size, pitch;
    u8 *rgba;
    size_t rgba_bytes;
    de_entity camera;
    de_entity nodes[PREVIEW_NODES];
    u32 node_count, selection;
    bool scene_ready;
    de_dvec3 center;
    f32 radius;
    de_rg_resource copy_color, copy_buffer;
    de_atomic_u64 captured_frame;
} preview_context;

static struct {
    preview_context *context;
    char id[1024], root[DE_FS_PATH_MAX];
    de_ep_model_render_params query;
    u64 request, deadline, revision;
    bool pending, rendering;
} g_preview;

static const de_allocator *allocator(void) {
    return de_heap_allocator();
}

static void context_destroy(preview_context *c) {
    if (!c)
        return;
    if (c->renderer) {
        de__render_world_wait_idle(c->renderer);
    }
    de_anim_destroy(c->anim);
    de_asset_system_destroy(c->assets); /* unloads require the uploader */
    de_renderer_destroy(c->renderer);   /* removes its Extract systems before the world */
    de_world_destroy(c->world);
    de_type_registry_destroy(c->types);
    if (c->target.id)
        de_gpu_texture_destroy(c->device, c->target);
    if (c->readback.id)
        de_gpu_buffer_destroy(c->device, c->readback);
    if (c->rgba)
        DE_FREE(allocator(), c->rgba, c->rgba_bytes);
    DE_FREE(allocator(), c, sizeof *c);
}
static void clear_context(void) {
    preview_context *old = g_preview.context;
    g_preview.context = NULL;
    context_destroy(old);
}
static void reset(de__editor *ed) {
    (void)ed;
    g_preview.pending = g_preview.rendering = false;
    clear_context();
}
void de__ed_preview_invalidate(void) {
    ++g_preview.revision;
    de__ed_settings_invalidate();
}
static de_result stats(int argc, const char *const *argv, void *user) {
    (void)argv;
    (void)user;
    if (argc != 1)
        return DE_ERR_INVALID_ARGUMENT;
    const preview_context *c = g_preview.context;
    const de_log_category log = de_log_register_category("editor.preview", DE_LOG_INFO);
    DE_INFO(log, "contexts=%u pending=%u rgba_bytes=%zu readback_bytes=%llu", c ? 1u : 0u,
            g_preview.pending ? 1u : 0u, c ? c->rgba_bytes : 0u,
            c ? (unsigned long long)c->pitch * c->size : 0ull);
    return DE_OK;
}
void de__ed_preview_shutdown(de__editor *ed) {
    reset(ed);
    (void)de_command_unregister("editor.previewStats");
    de_error_clear();
}

static void execute_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const preview_context *c = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, c->copy_buffer), 0, c->pitch,
                                      de_rg_texture(pass, c->copy_color), 0, 0);
}
static void capture(de_rg *graph, de_view view, void *user) {
    preview_context *c = user;
    if (view.id != c->view.id)
        return;
    de_view_frame_info info;
    if (!de_view_frame_info_get(c->renderer, view, &info))
        return;
    c->copy_color = de_view_color(c->renderer, view);
    c->copy_buffer = de_rg_import_buffer(graph, c->readback, DE_ACCESS_NONE);
    de_rg_pass *pass =
        de_rg_add_pass(graph, "editor.model.readback", DE_QUEUE_GRAPHICS, execute_copy, c);
    de_rg_read(pass, c->copy_color, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, c->copy_buffer, DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(pass);
    de_atomic_store_u64(&c->captured_frame, info.frame_index + 1u, DE_ATOMIC_RELEASE);
}
static de_result create_context(de__editor *ed, preview_context **out) {
    preview_context *c = DE_NEW(allocator(), preview_context, DE_MEM_RENDER);
    if (!c)
        return DE_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof *c);
    c->device = ed->e->gpu;
    c->revision = g_preview.revision;
    snprintf(c->id, sizeof c->id, "%s", g_preview.id);
    snprintf(c->root, sizeof c->root, "%s", g_preview.root);
    de_result result = de_type_registry_create(allocator(), &c->types);
    if (result == DE_OK)
        result = de_world_create(&(de_world_desc){.types = c->types}, &c->world);
    if (result == DE_OK)
        result = de_asset_system_create(&(de_asset_system_desc){.tasks = ed->e->tasks,
                                                                .types = c->types,
                                                                .locale = ed->e->assets->locale_buf,
                                                                .staging_ring_bytes = 16u << 20},
                                        &c->assets);
    if (result == DE_OK)
        result = de_anim_create(allocator(), c->assets, &c->anim);
    if (result == DE_OK)
        result = de_renderer_create(
            &(de_renderer_desc){
                .device = c->device, .assets = c->assets, .types = c->types, .frames_in_flight = 2},
            &c->renderer);
    if (result == DE_OK) {
        de_renderer_attach(c->renderer, c->world);
        de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
        settings.ao = settings.gi = settings.reflections = settings.shadows = settings.volumetrics =
            DE_QUALITY_OFF;
        settings.upscaler = DE_UPSCALE_NONE;
        settings.render_scale = 1;
        settings.dynamic_res_target_ms = 0;
        de_renderer_set_settings(c->renderer, &settings);
        de_view_set_paused(c->renderer, de_renderer_main_view(c->renderer), true);
        de_render_add_hook(c->renderer, DE_HOOK_UI, capture, c);
        result = de_asset_manifest_load(c->assets, c->root);
    }
    if (result == DE_OK) {
        c->load = de_asset_load(c->assets, de_asset_id_from(c->id), DE_LOAD_NO_EVENTS);
        if (!c->load.id)
            result = DE_ERR_OUT_OF_MEMORY;
    }
    if (result != DE_OK) {
        context_destroy(c);
        return result;
    }
    *out = c;
    return DE_OK;
}
static de_result configure_image(preview_context *c, u32 size) {
    if (c->size == size && c->view.id)
        return DE_OK;
    de__render_world_wait_idle(c->renderer);
    if (c->view.id)
        de_view_destroy(c->renderer, c->view);
    if (c->target.id)
        de_gpu_texture_destroy(c->device, c->target);
    if (c->readback.id)
        de_gpu_buffer_destroy(c->device, c->readback);
    if (c->rgba)
        DE_FREE(allocator(), c->rgba, c->rgba_bytes);
    c->view.id = c->target.id = c->readback.id = 0;
    c->rgba = NULL;
    c->size = size;
    c->pitch = (size * 8u + 255u) & ~255u;
    c->rgba_bytes = (size_t)size * size * 4u;
    c->target = de_gpu_texture_create(
        c->device, &(de_gpu_texture_desc){.width = size,
                                          .height = size,
                                          .format = DE_FMT_RGBA16_FLOAT,
                                          .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED});
    c->readback =
        de_gpu_buffer_create(c->device, &(de_gpu_buffer_desc){.size = (u64)c->pitch * size,
                                                              .usage = DE_USAGE_COPY_DST,
                                                              .memory = DE_MEM_READBACK});
    c->rgba = DE_ALLOC(allocator(), c->rgba_bytes, 16, DE_MEM_RENDER);
    if (!c->target.id || !c->readback.id || !c->rgba)
        return DE_ERR_OUT_OF_MEMORY;
    c->mapped = de_gpu_buffer_map(c->device, c->readback);
    c->view = de_view_create(
        c->renderer,
        &(de_view_desc){.target = c->target, .order = -1, .name = "editor.model.preview"});
    return c->view.id && c->mapped ? DE_OK : DE_ERR_INTERNAL;
}
static de_result spawn(preview_context *c, de_component component, de_entity *out) {
    const de_component components[] = {de_builtin_components(c->world).transform, component};
    return de_entity_spawn_with(c->world, components, 2, out);
}
static de_result build_scene(preview_context *c, const de_render_model *model,
                             const de_ep_model_render_params *query) {
    const u32 selection = query->has_mesh ? query->mesh : UINT32_MAX;
    if (!model->mesh_count || model->mesh_count > 1024 || model->node_count > PREVIEW_NODES)
        return DE_ERR_UNSUPPORTED;
    if (query->has_mesh && query->mesh >= model->mesh_count)
        return DE_ERR_INVALID_ARGUMENT;
    if (c->scene_ready && c->selection == selection)
        return DE_OK;
    de__render_world_wait_idle(c->renderer);
    for (u32 i = 0; i < c->node_count; ++i)
        de_entity_despawn(c->world, c->nodes[i]);
    c->node_count = 0;
    c->scene_ready = false;
    const de_component mesh_type = de_component_find(c->world, "DeMeshRenderer");
    const de_builtin_component_ids builtin = de_builtin_components(c->world);
    const u32 count = query->has_mesh ? 1u : model->node_count;
    for (u32 i = 0; i < count; ++i) {
        de_entity entity;
        DE_TRY(spawn(c, mesh_type, &entity));
        c->nodes[c->node_count++] = entity;
        DeTransform *pose = de_entity_get(c->world, entity, builtin.transform);
        DeMeshRenderer *mesh = de_entity_get(c->world, entity, mesh_type);
        *pose = de_transform_identity();
        mesh->layer_mask = ~0u;
        if (query->has_mesh)
            mesh->mesh = model->meshes[query->mesh];
        else {
            const de_render_model_node *node = &model->nodes[i];
            pose->position =
                (de_dvec3){node->translation[0], node->translation[1], node->translation[2]};
            memcpy(&pose->rotation, node->rotation, sizeof node->rotation);
            memcpy(&pose->scale, node->scale, sizeof node->scale);
            mesh->mesh = node->mesh;
        }
    }
    if (!query->has_mesh)
        for (u32 i = 0; i < count; ++i)
            if (model->nodes[i].parent != DE_COOKED_NONE) {
                if (model->nodes[i].parent >= count)
                    return DE_ERR_PARSE;
                DE_TRY(
                    de_entity_set_parent(c->world, c->nodes[i], c->nodes[model->nodes[i].parent]));
            }
    DE_TRY(de_world_run_schedule(c->world, DE_SCHEDULE_POST_UPDATE, 1.0f / 60, 0));
    bool has_bounds = false;
    de_aabb bounds = {0};
    de_dvec3 base = {0};
    for (u32 i = 0; i < c->node_count; ++i) {
        const DeMeshRenderer *instance = de_entity_get_const(c->world, c->nodes[i], mesh_type);
        if (!instance->mesh.id)
            continue;
        const de_render_mesh *mesh = de_render_mesh_get(c->assets, instance->mesh);
        const DeGlobalTransform *global =
            de_entity_get_const(c->world, c->nodes[i], builtin.global_transform);
        if (!mesh || !global || !mesh->lod_count)
            return DE_ERR_PARSE;
        if (mesh->submesh_count > 8)
            return DE_ERR_UNSUPPORTED;
        if (query->has_mesh && query->lod >= mesh->lod_count)
            return DE_ERR_INVALID_ARGUMENT;
        if (!has_bounds)
            base = global->position;
        const de_affine transform = de_affine_from_trs(
            (de_vec3){(f32)(global->position.x - base.x), (f32)(global->position.y - base.y),
                      (f32)(global->position.z - base.z)},
            global->rotation, global->scale);
        const de_aabb object = {{mesh->bounds_min[0], mesh->bounds_min[1], mesh->bounds_min[2]},
                                {mesh->bounds_max[0], mesh->bounds_max[1], mesh->bounds_max[2]}};
        const de_aabb world = de_aabb_transform(object, &transform);
        if (!has_bounds)
            bounds = world;
        else {
            bounds.min = de_v3_min(bounds.min, world.min);
            bounds.max = de_v3_max(bounds.max, world.max);
        }
        has_bounds = true;
    }
    if (!has_bounds)
        return DE_ERR_UNSUPPORTED;
    c->center = (de_dvec3){base.x + (f64)bounds.min.x + .5 * ((f64)bounds.max.x - bounds.min.x),
                           base.y + (f64)bounds.min.y + .5 * ((f64)bounds.max.y - bounds.min.y),
                           base.z + (f64)bounds.min.z + .5 * ((f64)bounds.max.z - bounds.min.z)};
    const f32 x = bounds.max.x - bounds.min.x, y = bounds.max.y - bounds.min.y,
              z = bounds.max.z - bounds.min.z;
    c->radius = .5f * sqrtf(x * x + y * y + z * z);
    if (!isfinite(c->radius) || c->radius <= 0)
        return DE_ERR_UNSUPPORTED;
    if (!c->camera.bits) {
        DE_TRY(spawn(c, de_component_find(c->world, "DeCamera"), &c->camera));
        for (u32 i = 0; i < 2; ++i) {
            de_entity entity;
            const de_component light_type = de_component_find(c->world, "DeLight");
            DE_TRY(spawn(c, light_type, &entity));
            DeLight *light = de_entity_get(c->world, entity, light_type);
            light->kind = DE_LIGHT_DIRECTIONAL;
            light->color = (de_vec3){1, 1, 1};
            light->intensity = i ? 1.5f : 4.0f;
            DeTransform *pose = de_entity_get(c->world, entity, builtin.transform);
            *pose = de_transform_identity();
            pose->rotation = de_quat_axis_angle((de_vec3){1, 0, 0}, i ? .8f : -.4f);
        }
    }
    c->selection = selection;
    c->scene_ready = true;
    return DE_OK;
}
static de_result render(preview_context *c, const de_ep_model_render_params *query) {
    const de_render_model *model =
        de_render_model_get(c->assets, de_asset_model(c->assets, de_asset_id_from(c->id)));
    if (!model)
        return DE_ERR_PARSE;
    DE_TRY(build_scene(c, model, query));
    /* Validate even when a previous request reuses the scene. */
    if (query->has_mesh) {
        const de_render_mesh *mesh = de_render_mesh_get(c->assets, model->meshes[query->mesh]);
        if (!mesh || query->lod >= mesh->lod_count)
            return DE_ERR_INVALID_ARGUMENT;
    }
    DE_TRY(configure_image(c, query->size));
    const de_component camera_type = de_component_find(c->world, "DeCamera");
    DeCamera *camera = de_entity_get(c->world, c->camera, camera_type);
    DeTransform *pose =
        de_entity_get(c->world, c->camera, de_builtin_components(c->world).transform);
    const f32 yaw = fmodf(query->yaw, 360) * (DE_PI / 180.0f);
    const f32 distance = c->radius * 2.8f;
    pose->position =
        (de_dvec3){c->center.x + sinf(yaw) * distance * .8660254f,
                   c->center.y - cosf(yaw) * distance * .8660254f, c->center.z + distance * .5f};
    pose->rotation = de_quat_mul(de_quat_axis_angle((de_vec3){0, 0, 1}, yaw),
                                 de_quat_axis_angle((de_vec3){1, 0, 0}, DE_PI / 3.0f));
    pose->scale = (de_vec3){1, 1, 1};
    camera->view = c->view;
    camera->projection = DE_PROJ_PERSPECTIVE;
    camera->fov_y = 1.0f;
    camera->near_z = c->radius * .001f;
    camera->exposure_ev100 = 0;
    const de_view_display display = {.disabled_features =
                                         DE_VIEW_HIDE_UI | DE_VIEW_DISABLE_POST_EFFECTS,
                                     .force_mesh_lod = true,
                                     .mesh_lod = query->lod};
    DE_TRY(de_view_set_display(c->renderer, c->view, &display));
    de_atomic_store_u64(&c->captured_frame, 0, DE_ATOMIC_RELEASE);
    DE_TRY(de_world_run_schedule(c->world, DE_SCHEDULE_POST_UPDATE, 1.0f / 60, 0));
    DE_TRY(de_world_run_schedule(c->world, DE_SCHEDULE_EXTRACT, 1.0f / 60, 0));
    return de__render_extract_and_kick(c->renderer, c->world);
}
static void fail(de__editor *ed, de_ep_error code, const char *message) {
    const u64 request = g_preview.request;
    g_preview.pending = g_preview.rendering = false;
    clear_context();
    mpack_writer_t writer;
    de__ed_respond_begin(ed, &writer);
    de__ed_error(&writer, request, code, "model preview: %s", message);
    (void)de__ed_send(ed, &writer, DE_EP_KIND_RESPONSE);
}
static void tick(de__editor *ed) {
    preview_context *c = g_preview.context;
    const u64 now = de_time_now_ns();
    if (!g_preview.pending) {
        if (c && now - c->last_use >= PREVIEW_IDLE_NS)
            clear_context();
        return;
    }
    if (now >= g_preview.deadline) {
        fail(ed, DE_EP_ERR_INTERNAL, "deadline expired");
        return;
    }
    if (!g_preview.rendering) {
        if (c && (strcmp(c->id, g_preview.id) || strcmp(c->root, g_preview.root) ||
                  c->revision != g_preview.revision)) {
            clear_context();
            c = NULL;
        }
        if (!c) {
            const de_result result = create_context(ed, &g_preview.context);
            if (result != DE_OK) {
                fail(ed, DE_EP_ERR_INTERNAL, de_result_str(result));
                return;
            }
            c = g_preview.context;
        }
        de_asset_system_update(c->assets, NULL);
        de_renderer_pump_uploads(c->renderer);
        de_load_info info;
        const de_load_state state = de_asset_status(c->assets, c->load, &info);
        if (state == DE_LOAD_PENDING || state == DE_LOAD_LOADING)
            return;
        if (state != DE_LOAD_READY) {
            fail(ed, DE_EP_ERR_INTERNAL, info.first_error_msg);
            return;
        }
        const de_result result = render(c, &g_preview.query);
        if (result != DE_OK) {
            fail(ed,
                 result == DE_ERR_INVALID_ARGUMENT ? DE_EP_ERR_INVALID_PARAMS
                 : result == DE_ERR_UNSUPPORTED    ? DE_EP_ERR_UNSUPPORTED
                                                   : DE_EP_ERR_INTERNAL,
                 de_result_str(result));
            return;
        }
        g_preview.rendering = true;
        return;
    }
    const u64 frame = de_atomic_load_u64(&c->captured_frame, DE_ATOMIC_ACQUIRE);
    if (!frame || de_renderer_completed_frame(c->renderer) < frame)
        return;
    if (c->revision != g_preview.revision) {
        fail(ed, DE_EP_ERR_BUSY, "asset changed; retry");
        return;
    }
    const size_t capacity = de__ed_preview_png_size(c->size, c->size);
    u8 *png = DE_ALLOC(allocator(), capacity, 16, DE_MEM_RENDER);
    if (!png) {
        fail(ed, DE_EP_ERR_INTERNAL, "out of memory");
        return;
    }
    de__ed_preview_rgba(c->size, c->size, c->mapped, c->pitch, c->rgba);
    size_t size = 0;
    const de_result encoded = de__ed_preview_png(c->size, c->size, c->rgba, png, capacity, &size);
    const u64 request = g_preview.request;
    if (encoded != DE_OK) {
        DE_FREE(allocator(), png, capacity);
        fail(ed, DE_EP_ERR_INTERNAL, "PNG encoding failed");
        return;
    }
    g_preview.pending = g_preview.rendering = false;
    c->last_use = now;
    {
        /* Sending can reset/destroy the context on disconnect. PNG ownership stays local. */
        const u64 blob = de__ed_send_blob(ed, png, size);
        if (blob && ed->connected) {
            const de_ep_blob_ref reference = {.blob_id = blob};
            mpack_writer_t writer;
            de__ed_respond_begin(ed, &writer);
            de_ep_write_response_asset_render_model(&writer, request, &reference);
            (void)de__ed_send(ed, &writer, DE_EP_KIND_RESPONSE);
        }
    }
    DE_FREE(allocator(), png, capacity);
}
static void on_render(de__editor *ed, const de_ep_request *request, de_ep_arena *arena,
                      mpack_writer_t *writer) {
    const de_ep_request *req = request;
    de_ep_arena *a = arena;
    mpack_writer_t *w = writer;
    DE__ED_PARAMS(asset_render_model, de_ep_model_render_params, query);
    if (!ed->e->gpu || !ed->e->renderer || !ed->e->assets || !ed->e->assets->manifest) {
        de__ed_error(writer, request->id, DE_EP_ERR_UNSUPPORTED,
                     "model previews need a source-mode renderer");
        return;
    }
    if (!query.id.len || query.id.len >= sizeof g_preview.id ||
        memchr(query.id.ptr, 0, query.id.len) || !query.size || query.size > 1024 ||
        query.lod >= 8 || (query.has_mesh && query.mesh >= 1024) || !isfinite(query.yaw)) {
        de__ed_error(writer, request->id, DE_EP_ERR_INVALID_PARAMS,
                     "invalid bounded model preview parameters");
        return;
    }
    if (!de_ep_str_eq(query.overlay, "none")) {
        de__ed_error(writer, request->id, DE_EP_ERR_UNSUPPORTED,
                     "model preview overlay is not implemented");
        return;
    }
    if (g_preview.pending) {
        de__ed_error(writer, request->id, DE_EP_ERR_BUSY, "one model preview is already rendering");
        return;
    }
    char id[sizeof g_preview.id];
    memcpy(id, query.id.ptr, query.id.len);
    id[query.id.len] = 0;
    const de__manifest_asset *entry = de__manifest_find_asset(ed->e->assets->manifest, id);
    if (!entry) {
        de__ed_error(writer, request->id, DE_EP_ERR_NOT_FOUND, "model asset not found");
        return;
    }
    if (strcmp(entry->type, "model")) {
        de__ed_error(writer, request->id, DE_EP_ERR_INVALID_PARAMS,
                     "preview requires a model asset");
        return;
    }
    const char *root = ed->e->assets->manifest->root;
    if (strlen(root) >= sizeof g_preview.root) {
        de__ed_error(writer, request->id, DE_EP_ERR_UNSUPPORTED, "manifest path is too long");
        return;
    }
    memcpy(g_preview.id, id, query.id.len + 1u);
    snprintf(g_preview.root, sizeof g_preview.root, "%s", root);
    g_preview.query = query;
    g_preview.query.id = de_ep_cstr(g_preview.id);
    g_preview.query.overlay = de_ep_cstr("none");
    g_preview.request = request->id;
    g_preview.deadline = de_time_now_ns() + PREVIEW_TIMEOUT_NS;
    g_preview.pending = true;
    g_preview.rendering = false;
    de__ed_defer(ed);
}
void de__ed_preview_register(de__editor *ed) {
    memset(&g_preview, 0, sizeof g_preview);
    de__ed_on(ed, DE_EP_METHOD_ASSET_RENDER_MODEL, on_render);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
    (void)de_command_register("editor.previewStats",
                              "Report model preview context and image-buffer ownership", stats,
                              NULL);
    de_error_clear();
}
