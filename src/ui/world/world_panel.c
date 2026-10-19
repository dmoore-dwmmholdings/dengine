#include "world_internal.h"
#include "ui/render/draw_list.h"
#include <math.h>
#include <string.h>
static void panel_construct(void *ptr) {
    *(DeUiWorldPanel *)ptr =
        (DeUiWorldPanel){.size_m = {1, 1}, .pixels_per_meter = 256, .depth_test = true};
}
static de_result panel_register(de_ui *ui) {
    de_type_registry *registry = de_world_types(ui->world);
    de_type_id id = de_type_find(registry, "DeUiWorldPanel");
    if (!id.id) {
        static const de_field_info fields[] = {
            DE_FIELD_EX(DeUiWorldPanel, size_m, DE_FK_VEC2, DE_FIELD_DEFAULT,
                        {0.001f, 10000, "m", "Panel width and height", NULL, 0}),
            DE_FIELD_EX(DeUiWorldPanel, pixels_per_meter, DE_FK_F32, DE_FIELD_DEFAULT,
                        {1, 16384, "px/m", "Layout density", NULL, 0}),
            DE_FIELD_EX(DeUiWorldPanel, lit, DE_FK_BOOL, DE_FIELD_DEFAULT,
                        {0, 1, NULL, "Use clustered forward lighting", NULL, 0}),
            DE_FIELD_EX(DeUiWorldPanel, depth_test, DE_FK_BOOL, DE_FIELD_DEFAULT,
                        {0, 1, NULL, "Test opaque depth; false overlays transparents", NULL, 0})};
        DE_TRY(de_type_register_ex(registry,
                                   &(de_type_desc){.name = "DeUiWorldPanel",
                                                   .size = sizeof(DeUiWorldPanel),
                                                   .align = _Alignof(DeUiWorldPanel),
                                                   .version = 1,
                                                   .is_pod = true,
                                                   .fields = fields,
                                                   .field_count = 4,
                                                   .hooks = {.construct = panel_construct}},
                                   &id, NULL));
    }
    return de_component_register(ui->world, id, DE_COMP_SERIALIZE | DE_COMP_SNAPSHOT).id
               ? DE_OK
               : DE_FAIL(DE_ERR_INTERNAL, "world UI panel component registration");
}
bool de_ui__world_root_extent(de_ui *ui, de_entity root, de_vec4 *rect) {
    de_component component = de_component_find(ui->world, "DeUiWorldPanel");
    const DeUiWorldPanel *panel =
        component.id ? de_entity_get_const(ui->world, root, component) : NULL;
    if (!panel)
        return false;
    f32 width = panel->size_m.x * panel->pixels_per_meter;
    f32 height = panel->size_m.y * panel->pixels_per_meter;
    if (!isfinite(width) || !isfinite(height) || width <= 0 || height <= 0)
        width = height = 0;
    *rect = (de_vec4){0, 0, roundf(width), roundf(height)};
    return true;
}

typedef struct world_gpu_quad {
    de_ui_gpu_quad quad;
    de_vec4 clip;
} world_gpu_quad;
typedef struct world_gpu_root {
    de_vec4 rows[3], panel;
} world_gpu_root;
typedef struct world_push {
    de_forward_shader_bindings bindings;
    u32 data, first, roots, root;
} world_push;
_Static_assert(sizeof(world_gpu_quad) == 160, "world quad shader stride");
_Static_assert(sizeof(world_gpu_root) == 64, "world root shader stride");
_Static_assert(sizeof(world_push) == 64, "world shader push ABI");
struct de_ui_world_state {
    struct de_ui_render *render;
    de_view_frame_info camera;
    bool camera_valid, registered;
#if DE_HAS_RENDER
    de_gpu_buffer quads[DE_UI_RENDER_GPU_SLOTS], roots[DE_UI_RENDER_GPU_SLOTS];
    de_gpu_pipeline pipeline[2];
    u64 uploaded[DE_UI_RENDER_GPU_SLOTS];
#endif
};
bool de_ui__world_camera(de_ui *ui, de_view_frame_info *out) {
    struct de_ui_render *r = ui ? ui->render : NULL;
    if (!r || !r->world || !r->world->camera_valid)
        return false;
    *out = r->world->camera;
    return true;
}
void de_ui__world_extract(struct de_ui_render *r, de_ui_draw_list *packet) {
    de_ui *ui = r->ui;
    de_component panel = de_component_find(ui->world, "DeUiWorldPanel");
    for (u32 i = 0; i < packet->root_count; ++i) {
        de_ui_root_list *root = &packet->roots[i];
        if (!root->world)
            continue;
        const DeUiWorldPanel *p = de_entity_get_const(ui->world, root->entity, panel);
        const DeGlobalTransform *transform =
            de_entity_get_const(ui->world, root->entity, ui->hierarchy.global_transform);
        root->transform_valid = p && transform;
        if (p)
            root->panel = *p;
        if (transform)
            root->transform = *transform;
    }
    packet->camera_valid = false;
#if DE_HAS_RENDER
    if (ui->desc.renderer) {
        de_view view = ui->desc.view.id ? ui->desc.view : de_renderer_main_view(ui->desc.renderer);
        packet->camera_valid =
            de_renderer_extract_view_info(ui->desc.renderer, view, &packet->camera);
    }
#endif
    if (r->world) {
        r->world->camera_valid = packet->camera_valid;
        if (packet->camera_valid)
            r->world->camera = packet->camera;
    }
}
#if DE_HAS_RENDER
typedef struct world_pass {
    struct de_ui_world_state *owner;
    const de_forward_context *context;
    de_rg_resource quads, roots;
    de_view_frame_info info;
    u32 first, count, root;
    bool depth;
} world_pass;
static bool world_upload(struct de_ui_world_state *s, const de_ui_draw_list *packet,
                         const de_view_frame_info *info) {
    struct de_ui_render *r = s->render;
    const u32 ring = info->ring_index;
    if (s->uploaded[ring] == info->frame_index)
        return true;
    world_gpu_quad *quads = de_gpu_buffer_map(r->device, s->quads[ring]);
    world_gpu_root *roots = de_gpu_buffer_map(r->device, s->roots[ring]);
    if (!quads || !roots)
        return false;
    for (u32 i = 0; i < packet->count; ++i)
        quads[i] = (world_gpu_quad){packet->commands[i].gpu, packet->commands[i].clip};
    for (u32 i = 0; i < packet->root_count; ++i) {
        const de_ui_root_list *root = &packet->roots[i];
        if (!root->world || !root->transform_valid)
            continue;
        const de_affine m = de_transform_to_affine_relative(&root->transform, info->render_origin);
        roots[i] =
            (world_gpu_root){.rows = {{m.m[0], m.m[3], m.m[6], m.m[9]},
                                      {m.m[1], m.m[4], m.m[7], m.m[10]},
                                      {m.m[2], m.m[5], m.m[8], m.m[11]}},
                             .panel = {root->panel.size_m.x, root->panel.size_m.y,
                                       root->panel.pixels_per_meter, root->panel.lit ? 1 : 0}};
    }
    s->uploaded[ring] = info->frame_index;
    return true;
}
static u32 world_gather(const de_forward_context *context, de_forward_item *out, u32 capacity,
                        void *user) {
    struct de_ui_world_state *s = user;
    struct de_ui_render *r = s->render;
    de_view_frame_info info;
    if (!de_render_forward_view_info(context, &info) || info.ring_index >= DE_UI_RENDER_GPU_SLOTS ||
        (r->ui->desc.view.id && info.view.id != r->ui->desc.view.id))
        return 0;
    de_mutex_lock(&r->mutex);
    if (r->stats_frame != info.frame_index) {
        r->stats.draw_calls = 0;
        r->stats_frame = info.frame_index;
    }
    de_ui_draw_list *packet = (de_ui_draw_list *)de_ui__packet_for_frame(r, info.extract_frame);
    if (!packet) {
        de_mutex_unlock(&r->mutex);
        return 0;
    }
    u32 count = 0;
    for (u32 i = 0; i < packet->root_count; ++i) {
        const de_ui_root_list *root = &packet->roots[i];
        if (!root->world || !root->transform_valid || !root->count ||
            !(root->panel.pixels_per_meter > 0) || !(root->panel.size_m.x > 0) ||
            !(root->panel.size_m.y > 0))
            continue;
        if (count == capacity)
            break;
        out[count++] = (de_forward_item){
            .center_relative = de_dv3_relative(root->transform.position, info.render_origin),
            .entity = root->entity.bits,
            .item_index = i,
            .always_on_top = !root->panel.depth_test};
    }
    if (count && !world_upload(s, packet, &info))
        count = 0;
    if (count)
        packet->last_gpu_fence = info.frame_index + 1;
    de_mutex_unlock(&r->mutex);
    return count;
}
static void world_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    world_pass *p = user;
    struct de_ui_render *r = p->owner->render;
    const de_gpu_attachment color = {
        .texture = de_rg_texture(pass, de_render_forward_color(p->context)), .store = true};
    de_gpu_attachment depth = {0};
    if (p->depth)
        depth = (de_gpu_attachment){
            .texture = de_rg_texture(pass, de_render_forward_depth(p->context)), .store = true};
    world_push push = {.data = de_gpu_buffer_bindless(r->device, de_rg_buffer(pass, p->quads)),
                       .roots = de_gpu_buffer_bindless(r->device, de_rg_buffer(pass, p->roots)),
                       .first = p->first,
                       .root = p->root};
    if (!de_render_forward_bindings(p->context, pass, &push.bindings))
        return;
    de_gpu_cmd_begin_render(cmd, &color, 1, p->depth ? &depth : NULL);
    de_gpu_cmd_set_pipeline(cmd, p->owner->pipeline[p->depth ? 1 : 0]);
    de_gpu_cmd_viewport(cmd, 0, 0, (f32)p->info.internal_width, (f32)p->info.internal_height);
    de_gpu_cmd_scissor(cmd, 0, 0, p->info.internal_width, p->info.internal_height);
    de_gpu_cmd_push_constants(cmd, &push, sizeof push);
    de_gpu_cmd_draw(cmd, 6, p->count, 0, 0);
    de_gpu_cmd_end_render(cmd);
}
static de_result world_build(de_rg *graph, const de_forward_context *context,
                             const de_forward_item *item, void *user) {
    struct de_ui_world_state *s = user;
    struct de_ui_render *r = s->render;
    de_view_frame_info info;
    if (!de_render_forward_view_info(context, &info))
        return DE_ERR_INVALID_ARGUMENT;
    world_pass *data = de_render_forward_alloc(context, sizeof *data, _Alignof(world_pass));
    if (!data)
        return DE_ERR_OUT_OF_MEMORY;
    de_mutex_lock(&r->mutex);
    const de_ui_draw_list *packet = de_ui__packet_for_frame(r, info.extract_frame);
    if (!packet || item->item_index >= packet->root_count) {
        de_mutex_unlock(&r->mutex);
        return DE_OK;
    }
    const de_ui_root_list *root = &packet->roots[item->item_index];
    *data = (world_pass){.owner = s,
                         .context = context,
                         .info = info,
                         .first = root->first,
                         .count = root->count,
                         .root = item->item_index,
                         .depth = root->panel.depth_test};
    de_mutex_unlock(&r->mutex);
    data->quads = de_rg_import_buffer(graph, s->quads[info.ring_index], DE_ACCESS_SHADER_READ);
    data->roots = de_rg_import_buffer(graph, s->roots[info.ring_index], DE_ACCESS_SHADER_READ);
    de_rg_pass *pass =
        de_rg_add_pass(graph, "UI.WorldPanel", DE_QUEUE_GRAPHICS, world_execute, data);
    if (!pass)
        return DE_ERR_OUT_OF_MEMORY;
    de_render_forward_declare(pass, context, data->depth);
    de_rg_read(pass, data->quads, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, data->roots, DE_ACCESS_SHADER_READ);
    de_mutex_lock(&r->mutex);
    ++r->stats.draw_calls;
    de_mutex_unlock(&r->mutex);
    return DE_OK;
}
#endif

de_result de_ui__world_init(struct de_ui_render *r) {
    struct de_ui_world_state *s = DE_NEW(r->ui->alloc, struct de_ui_world_state, DE_MEM_UI);
    if (!s)
        return DE_ERR_OUT_OF_MEMORY;
    memset(s, 0, sizeof *s);
    s->render = r;
    r->world = s;
#if DE_HAS_RENDER
    for (u32 i = 0; i < DE_UI_RENDER_GPU_SLOTS; ++i)
        s->uploaded[i] = UINT64_MAX;
    if (!r->device || !r->ui->desc.renderer)
        return DE_OK;
    for (u32 i = 0; i < 2; ++i) {
        de_gpu_pipeline_desc d = {.kind = DE_PIPE_GRAPHICS,
                                  .color_formats = {DE_FMT_RGBA16_FLOAT},
                                  .color_count = 1,
                                  .blend = {DE_BLEND_PREMUL},
                                  .depth_format = i ? DE_FMT_D32_FLOAT : DE_FMT_UNKNOWN,
                                  .depth_test = i != 0,
                                  .depth_write = false,
                                  .cull = DE_CULL_NONE,
                                  .name = "world UI panel"};
        DE_TRY(de_render_builtin_graphics_pipeline(r->ui->desc.renderer, "ui_world", 0, "vs_world",
                                                   "ps_world", &d, &s->pipeline[i]));
    }
    for (u32 i = 0; i < DE_UI_RENDER_GPU_SLOTS; ++i) {
        s->quads[i] = de_gpu_buffer_create(
            r->device, &(de_gpu_buffer_desc){.size = (u64)r->capacity * sizeof(world_gpu_quad),
                                             .usage = DE_USAGE_STORAGE,
                                             .memory = DE_MEM_UPLOAD,
                                             .name = "world UI quads"});
        s->roots[i] = de_gpu_buffer_create(
            r->device, &(de_gpu_buffer_desc){.size = (u64)r->ui->capacity * sizeof(world_gpu_root),
                                             .usage = DE_USAGE_STORAGE,
                                             .memory = DE_MEM_UPLOAD,
                                             .name = "world UI roots"});
        if (!s->quads[i].id || !s->roots[i].id)
            return DE_ERR_OUT_OF_MEMORY;
    }
#endif
    return DE_OK;
}
de_result de_ui__world_attach(struct de_ui_render *r) {
    DE_TRY(panel_register(r->ui));
#if DE_HAS_RENDER
    if (r->device && r->ui->desc.renderer && !r->world->registered) {
        de_forward_participant_desc d = {.max_items =
                                             r->ui->capacity < 65536 ? r->ui->capacity : 65536,
                                         .gather = world_gather,
                                         .build = world_build,
                                         .user = r->world};
        DE_TRY(de_render_add_forward_participant(r->ui->desc.renderer, &d));
        r->world->registered = true;
    }
#endif
    return DE_OK;
}
void de_ui__world_destroy(struct de_ui_render *r) {
    struct de_ui_world_state *s = r->world;
    if (!s)
        return;
#if DE_HAS_RENDER
    if (s->registered)
        (void)de_render_remove_forward_participant(r->ui->desc.renderer, world_gather, s);
    if (r->device) {
        de_gpu_wait_idle(r->device);
        for (u32 i = 0; i < DE_UI_RENDER_GPU_SLOTS; ++i) {
            de_gpu_buffer_destroy(r->device, s->quads[i]);
            de_gpu_buffer_destroy(r->device, s->roots[i]);
        }
        de_gpu_pipeline_destroy(r->device, s->pipeline[0]);
        de_gpu_pipeline_destroy(r->device, s->pipeline[1]);
    }
#endif
    DE_FREE(r->ui->alloc, s, sizeof *s);
    r->world = NULL;
}
