#include "draw_list.h"
#include "gpu/texture_lease_internal.h"
#include "ui/world/world_internal.h"
#include <string.h>
void de_ui__packet_unpin(struct de_ui_render *r, de_ui_draw_list *packet) {
    for (u32 i = 0; i < packet->count; ++i) {
        de_ui_draw_cmd *c = &packet->commands[i];
#if DE_HAS_RENDER
        de__gpu_texture_unpin(r->device, c->texture_lease);
#endif
        c->texture_lease = (de_gpu_texture){0};
#if DE_HAS_RENDER
        if (c->pins & 1u)
            de_render_unpin_asset(r->ui->desc.renderer, c->image.id);
#endif
        if (c->pins & 2u)
            de_text_unpin_font(r->ui->desc.text, c->font);
        c->pins = 0;
    }
    packet->valid = false;
    packet->count = packet->root_count = 0;
    packet->last_gpu_fence = 0;
}
void de_ui__extract_draw_list(de_system_ctx *ctx) {
    de_ui *ui = ctx->user;
    struct de_ui_render *r = ui->render;
    if (!r || !de_ui__pixel_extract(ui))
        return;
    u64 gpu_done = UINT64_MAX, cpu_done = UINT64_MAX, token = r->build.generation;
#if DE_HAS_RENDER
    if (ui->desc.renderer) {
        gpu_done = de_renderer_completed_frame(ui->desc.renderer);
        cpu_done = de_renderer_completed_extract_frame(ui->desc.renderer);
        token = de_renderer_extract_frame(ui->desc.renderer);
        if (!token)
            return; /* No immutable renderer Extract frame exists yet. */
    }
#endif
    const u64 start = de_time_now_ns();
    de_mutex_lock(&r->mutex);
    u32 selected = UINT32_MAX;
    for (u32 i = 0; i < DE_UI_RENDER_SNAPSHOTS; ++i) {
        de_ui_draw_list *p = &r->snapshots[i];
        if (i == r->published)
            continue;
        bool consumed = p->last_gpu_fence != 0;
        if (!p->valid || (consumed && p->last_gpu_fence <= gpu_done) ||
            (!consumed && p->generation && p->generation <= cpu_done)) {
            selected = i;
            break;
        }
    }
    if (selected == UINT32_MAX) {
        /* A slow or not-yet-submitted render frame owns every packet. Preserve all of them;
         * dropping this newest UI update applies backpressure without exposing mutable data. */
        ++r->stats.truncated;
        de_mutex_unlock(&r->mutex);
        return;
    }
    de_ui_draw_list *packet = &r->snapshots[selected];
    de_ui__packet_unpin(r, packet);
    memcpy(packet->commands, r->build.commands, (size_t)r->build.count * sizeof *packet->commands);
    memcpy(packet->roots, r->build.roots, (size_t)r->build.root_count * sizeof *packet->roots);
    packet->count = r->build.count;
    packet->root_count = r->build.root_count;
    packet->generation = token;
    packet->last_gpu_fence = 0;
    packet->valid = true;
    for (u32 i = 0; i < packet->count; ++i) {
        de_ui_draw_cmd *c = &packet->commands[i];
        if (c->gpu.kind != DE_UI_QUAD_RECT)
            c->gpu.texture = UINT32_MAX;
#if DE_HAS_RENDER
        if (c->image.id && de_render_pin_asset(ui->desc.renderer, c->image.id)) {
            c->pins |= 1u;
            de_gpu_texture texture = de_render_texture_gpu(ui->desc.renderer, c->image);
            if (de__gpu_texture_pin(r->device, texture))
                c->texture_lease = texture;
            else
                texture = (de_gpu_texture){0};
            c->gpu.texture =
                texture.id ? de_gpu_texture_bindless(r->device, texture, 0, false) : UINT32_MAX;
            c->gpu.sampler =
                de_gpu_sampler_bindless(r->device, ui->desc.pixel_art ? r->nearest : r->linear);
        }
#endif
        if (c->font.id && de_text_pin_font(ui->desc.text, c->font)) {
            c->pins |= 2u;
            de_text_page_info info = {0};
            if (de_text_get_page_info(ui->desc.text, c->font, c->page, &info) == DE_OK) {
                c->gpu.params.z = info.distance_range;
                c->gpu.flags = info.bitmap ? 1u : 0;
#if DE_HAS_RENDER
                if (r->device) {
                    if (de__gpu_texture_pin(r->device, info.texture))
                        c->texture_lease = info.texture;
                    else
                        info.texture = (de_gpu_texture){0};
                    c->gpu.texture =
                        info.texture.id ? de_gpu_texture_bindless(r->device, info.texture, 0, false)
                                        : UINT32_MAX;
                    c->gpu.sampler = de_gpu_sampler_bindless(
                        r->device, (info.pixel || ui->desc.pixel_art) ? r->nearest : r->linear);
                    if (ui->desc.pixel_art && !info.bitmap)
                        c->gpu.texture = UINT32_MAX;
                }
#endif
            }
        }
    }
    de_ui__world_extract(r, packet);
    r->published = selected;
    r->stats.build_extract_ms += (f32)(de_time_now_ns() - start) / 1e6f;
    de_mutex_unlock(&r->mutex);
}
static bool list_alloc(struct de_ui_render *r, de_ui_draw_list *list) {
    de_ui *ui = r->ui;
    list->commands = DE_ALLOC(ui->alloc, (u64)r->capacity * sizeof *list->commands,
                              _Alignof(de_ui_draw_cmd), DE_MEM_UI);
    list->roots = DE_ALLOC(ui->alloc, (u64)ui->capacity * sizeof *list->roots,
                           _Alignof(de_ui_root_list), DE_MEM_UI);
    return list->commands && list->roots;
}
static void list_free(struct de_ui_render *r, de_ui_draw_list *list) {
    if (list->commands)
        DE_FREE(r->ui->alloc, list->commands, (u64)r->capacity * sizeof *list->commands);
    if (list->roots)
        DE_FREE(r->ui->alloc, list->roots, (u64)r->ui->capacity * sizeof *list->roots);
}
de_result de_ui__render_init(de_ui *ui) {
    struct de_ui_render *r = DE_NEW(ui->alloc, struct de_ui_render, DE_MEM_UI);
    if (!r)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "UI draw-list state");
    memset(r, 0, sizeof *r);
    r->ui = ui;
    ui->render = r;
    r->published = UINT32_MAX;
    r->stats_frame = UINT64_MAX;
    r->log = de_log_register_category("ui", DE_LOG_INFO);
    r->max_quads = de_cvar_register(
        &(de_cvar_desc){.name = "ui.render.max_quads",
                        .type = DE_CVAR_INT,
                        .default_number = 65536,
                        .min = 1,
                        .max = 1048576,
                        .help = "UI quad capacity; set before UI creation to increase storage"});
    r->capacity = (u32)de_cvar_int(r->max_quads);
    de_mutex_init(&r->mutex);
    r->mutex_ready = true;
    r->nodes = DE_ALLOC(ui->alloc, (u64)ui->capacity * sizeof *r->nodes, _Alignof(de_ui_draw_node),
                        DE_MEM_UI);
    r->sort = DE_ALLOC(ui->alloc, (u64)ui->capacity * sizeof *r->sort, _Alignof(de_ui_draw_node),
                       DE_MEM_UI);
    r->inherited = DE_ALLOC(ui->alloc, (u64)ui->capacity * sizeof *r->inherited,
                            _Alignof(de_ui_inherited), DE_MEM_UI);
    r->text_cache = DE_ALLOC(ui->alloc, (u64)ui->capacity * sizeof *r->text_cache,
                             _Alignof(de_ui_text_cache), DE_MEM_UI);
    r->glyphs = DE_ALLOC(ui->alloc, (u64)r->capacity * sizeof *r->glyphs, _Alignof(de_glyph_quad),
                         DE_MEM_UI);
    if (r->text_cache)
        memset(r->text_cache, 0, (size_t)ui->capacity * sizeof *r->text_cache);
    if (!r->nodes || !r->sort || !r->inherited || !r->text_cache || !r->glyphs)
        return DE_ERR_OUT_OF_MEMORY;
    if (!list_alloc(r, &r->build))
        return DE_ERR_OUT_OF_MEMORY;
    for (u32 i = 0; i < DE_UI_RENDER_SNAPSHOTS; ++i)
        if (!list_alloc(r, &r->snapshots[i]))
            return DE_ERR_OUT_OF_MEMORY;
    DE_TRY(de_ui__pass_init(r));
    return de_ui__world_init(r);
}
de_result de_ui__render_attach(de_ui *ui) {
    struct de_ui_render *r = ui->render;
    if (!r || !ui->world)
        return DE_ERR_INVALID_ARGUMENT;
    if (r->build_system.id)
        return DE_OK;
    DE_TRY(de_ui__world_attach(r));
    const char *after_layout[] = {DE_UI_LAYOUT_SYSTEM};
    r->build_system = de_system_add(
        ui->world, &(de_system_desc){.name = "ui.draw_list",
                                     .schedule = DE_SCHEDULE_POST_UPDATE,
                                     .fn = de_ui__build_draw_list,
                                     .user = ui,
                                     .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE,
                                     .after = after_layout,
                                     .after_count = 1});
    const char *after_render[] = {"de.render.extract_views"};
    r->extract_system = de_system_add(
        ui->world, &(de_system_desc){.name = DE_UI_EXTRACT_SYSTEM,
                                     .schedule = DE_SCHEDULE_EXTRACT,
                                     .fn = de_ui__extract_draw_list,
                                     .user = ui,
                                     .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE,
                                     .after = ui->desc.renderer ? after_render : NULL,
                                     .after_count = ui->desc.renderer ? 1 : 0});
    if (!r->build_system.id || !r->extract_system.id)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "UI draw systems");
    return DE_OK;
}
void de_ui__render_shutdown(de_ui *ui) {
    struct de_ui_render *r = ui->render;
    if (!r)
        return;
    if (ui->world) {
        if (r->build_system.id)
            de_system_remove(ui->world, r->build_system);
        if (r->extract_system.id)
            de_system_remove(ui->world, r->extract_system);
    }
    de_ui__world_destroy(r);
    de_ui__pass_destroy(r); /* Drains the hook before packet references or state are freed. */
    for (u32 i = 0; i < DE_UI_RENDER_SNAPSHOTS; ++i) {
        de_ui__packet_unpin(r, &r->snapshots[i]);
        list_free(r, &r->snapshots[i]);
    }
    list_free(r, &r->build);
    if (r->text_cache) {
        for (u32 i = 0; i < ui->capacity; ++i)
            de_text_layout_free(r->text_cache[i].layout);
        DE_FREE(ui->alloc, r->text_cache, (u64)ui->capacity * sizeof *r->text_cache);
    }
    if (r->nodes)
        DE_FREE(ui->alloc, r->nodes, (u64)ui->capacity * sizeof *r->nodes);
    if (r->sort)
        DE_FREE(ui->alloc, r->sort, (u64)ui->capacity * sizeof *r->sort);
    if (r->inherited)
        DE_FREE(ui->alloc, r->inherited, (u64)ui->capacity * sizeof *r->inherited);
    if (r->glyphs)
        DE_FREE(ui->alloc, r->glyphs, (u64)r->capacity * sizeof *r->glyphs);
    if (r->mutex_ready)
        de_mutex_destroy(&r->mutex);
    DE_FREE(ui->alloc, r, sizeof *r);
    ui->render = NULL;
}
de_ui_render_stats de_ui_get_render_stats(const de_ui *ui) {
    de_ui_render_stats stats = {0};
    if (!ui || !ui->render)
        return stats;
    struct de_ui_render *r = ui->render;
    de_mutex_lock(&r->mutex);
    stats = r->stats;
    de_mutex_unlock(&r->mutex);
    return stats;
}
