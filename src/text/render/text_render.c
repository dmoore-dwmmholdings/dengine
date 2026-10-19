#include "text_render_internal.h"
#if DE_HAS_RENDER
#include "text/fallback_internal.h"
#include "render/world/render_world.h"
#include "render/passes/vis/vis_stages.h"
#include <string.h>
#include <math.h>
static de_gpu_pipeline graphics(struct de__text_render *r, bool world) {
    de_gpu_pipeline_desc p = {.kind = DE_PIPE_GRAPHICS,
                              .vs = r->code.entries[0].blob,
                              .ps = r->code.entries[world ? 2 : 1].blob,
                              .color_formats = {DE_FMT_RGBA16_FLOAT},
                              .color_count = 1,
                              .blend = {DE_BLEND_PREMUL},
                              .cull = DE_CULL_NONE,
                              .push_constant_bytes = sizeof(de__text_push),
                              .name = world ? "world text" : "screen text"};
    if (world) {
        p.depth_format = DE_FMT_D32_FLOAT;
        p.depth_test = true;
        p.depth_write = false;
    }
    return de_gpu_pipeline_create(r->device, &p);
}
void de_text__render_destroy(de_text_system *t) {
    struct de__text_render *r = t->render;
    if (!r)
        return;
    de__render_world_wait_idle(r->renderer);
    de__view_unregister_stage(r->renderer, de__text_ui_stage, r);
    de__view_unregister_stage(r->renderer, de__text_world_stage, r);
    de__text_world_detach(r);
    de_gpu_wait_idle(r->device);
    for (u32 i = 0; i < r->page_count; ++i)
        de_gpu_texture_destroy(r->device, r->pages[i].texture);
    for (u32 i = 0; i < 3; ++i) {
        if (r->frames[i].id)
            de_gpu_buffer_destroy(r->device, r->frames[i]);
        if (r->queues[i].screen)
            DE_FREE(t->alloc, r->queues[i].screen, (u64)DE_TEXT_DRAW_CAP * sizeof(de__text_item));
        if (r->queues[i].world)
            DE_FREE(t->alloc, r->queues[i].world, (u64)DE_TEXT_DRAW_CAP * sizeof(de__text_item));
    }
    if (r->ui_pipeline.id)
        de_gpu_pipeline_destroy(r->device, r->ui_pipeline);
    if (r->world_pipeline.id)
        de_gpu_pipeline_destroy(r->device, r->world_pipeline);
    if (r->upload_pipeline.id)
        de_gpu_pipeline_destroy(r->device, r->upload_pipeline);
    if (r->linear.id)
        de_gpu_sampler_destroy(r->device, r->linear);
    if (r->nearest.id)
        de_gpu_sampler_destroy(r->device, r->nearest);
    de__engine_shader_code_free(&r->code);
    de__engine_shader_code_free(&r->upload_code);
    de_mutex_destroy(&r->mutex);
    DE_FREE(t->alloc, r, sizeof *r);
    t->render = NULL;
}
de_result de_text_attach(de_text_system *t, de_renderer *renderer, de_world *world) {
    if (!t)
        return DE_ERR_INVALID_ARGUMENT;
    de_text__render_destroy(t);
    if (!renderer)
        return DE_OK;
    struct de__text_render *r = DE_NEW(t->alloc, struct de__text_render, DE_MEM_UI);
    if (!r)
        return DE_ERR_OUT_OF_MEMORY;
    memset(r, 0, sizeof *r);
    t->render = r;
    r->text = t;
    r->renderer = renderer;
    r->device = renderer->dev;
    de_mutex_init(&r->mutex);
    r->next_frame = renderer->frame_index;
    r->current_frame = r->next_frame;
    r->white_nits = de_cvar_register(&(de_cvar_desc){.name = "r.ui_white_nits",
                                                     .type = DE_CVAR_FLOAT,
                                                     .default_number = 200,
                                                     .min = 1,
                                                     .max = 10000});
    static const char *const entries[] = {"vs_main", "ps_main", "ps_world"};
    char why[768];
    de_result result = de__engine_shader_load(r->device, t->alloc, "text_msdf", 0, entries, 3,
                                              &r->code, why, sizeof why);
    if (result != DE_OK)
        goto fail;
    static const char *const upload[] = {"cs_main"};
    result = de__engine_shader_load(r->device, t->alloc, "text_upload", 0, upload, 1,
                                    &r->upload_code, why, sizeof why);
    if (result != DE_OK)
        goto fail;
    r->ui_pipeline = graphics(r, false);
    r->world_pipeline = graphics(r, true);
    de_gpu_pipeline_desc cp = {.kind = DE_PIPE_COMPUTE,
                               .cs = r->upload_code.entries[0].blob,
                               .push_constant_bytes = 32,
                               .threadgroup_size = {8, 8, 1},
                               .name = "text dirty upload"};
    r->upload_pipeline = de_gpu_pipeline_create(r->device, &cp);
    if (!r->ui_pipeline.id || !r->world_pipeline.id || !r->upload_pipeline.id) {
        result = DE_ERR_INTERNAL;
        goto fail;
    }
    de_gpu_sampler_desc smp = {.min = DE_FILTER_LINEAR,
                               .mag = DE_FILTER_LINEAR,
                               .u = DE_ADDR_CLAMP,
                               .v = DE_ADDR_CLAMP,
                               .w = DE_ADDR_CLAMP};
    r->linear = de_gpu_sampler_create(r->device, &smp);
    smp.min = smp.mag = DE_FILTER_NEAREST;
    r->nearest = de_gpu_sampler_create(r->device, &smp);
    if (!r->linear.id || !r->nearest.id) {
        result = DE_ERR_INTERNAL;
        goto fail;
    }
    r->linear_index = de_gpu_sampler_bindless(r->device, r->linear);
    r->nearest_index = de_gpu_sampler_bindless(r->device, r->nearest);
    for (u32 i = 0; i < 3; ++i) {
        r->gpu_frame[i] = UINT64_MAX;
        r->queues[i].screen = DE_ALLOC(t->alloc, (u64)DE_TEXT_DRAW_CAP * sizeof(de__text_item),
                                       _Alignof(de__text_item), DE_MEM_UI);
        r->queues[i].world = DE_ALLOC(t->alloc, (u64)DE_TEXT_DRAW_CAP * sizeof(de__text_item),
                                      _Alignof(de__text_item), DE_MEM_UI);
        r->frames[i] = de_gpu_buffer_create(
            r->device, &(de_gpu_buffer_desc){.size = DE_TEXT_FRAME_BYTES,
                                             .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                             .memory = DE_MEM_UPLOAD,
                                             .name = "text frame instances"});
        if (!r->queues[i].screen || !r->queues[i].world || !r->frames[i].id) {
            result = DE_ERR_OUT_OF_MEMORY;
            goto fail;
        }
        r->mapped[i] = de_gpu_buffer_map(r->device, r->frames[i]);
        if (!r->mapped[i]) {
            result = DE_ERR_INTERNAL;
            goto fail;
        }
    }
    r->queues[r->current_frame % 3].valid = true;
    r->queues[r->current_frame % 3].frame = r->current_frame;
    result = de__view_register_stage(renderer, DE__VIEW_SLOT_TRANSPARENT, 100, "text.world",
                                     de__text_world_stage, r);
    if (result != DE_OK)
        goto fail;
    result = de__view_register_stage(renderer, DE__VIEW_SLOT_OUTPUT, DE__VIEW_ORDER_UI + 1,
                                     "text.screen", de__text_ui_stage, r);
    if (result != DE_OK)
        goto fail;
    result = de__text_world_attach(r, world);
    if (result != DE_OK)
        goto fail;
    return DE_OK;
fail:
    de_text__render_destroy(t);
    return DE_FAIL_WRAP(result, "attach text rendering");
}
void de_text__render_begin(de_text_system *t) {
    struct de__text_render *r = t->render;
    if (!r)
        return;
    de_mutex_lock(&r->mutex);
    r->current_frame = r->next_frame++;
    de__text_queue *q = &r->queues[r->current_frame % 3];
    q->frame = r->current_frame;
    q->valid = true;
    q->screen_count = q->world_count = 0;
    r->stats = (de_text_render_stats){0};
    de_mutex_unlock(&r->mutex);
}
de_result de_text_draw(de_text_system *t, de_view view, const de_glyph_quad *quads, u32 count,
                       de_vec2 origin, const de_text_draw_opts *options) {
    if (!t || (!quads && count) || !options)
        return DE_ERR_INVALID_ARGUMENT;
    if (!t->render)
        return DE_OK;
    struct de__text_render *r = t->render;
    de_mutex_lock(&r->mutex);
    de__text_queue *q = &r->queues[r->current_frame % 3];
    if (count > DE_TEXT_DRAW_CAP - q->screen_count) {
        de_mutex_unlock(&r->mutex);
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "text frame glyph capacity");
    }
    if (!view.id)
        view = de_renderer_main_view(r->renderer);
    for (u32 i = 0; i < count; ++i) {
        de__text_item *item = &q->screen[q->screen_count++];
        memset(item, 0, sizeof *item);
        item->font = quads[i].font.id ? quads[i].font : options->font;
        item->page = quads[i].atlas_page;
        item->view = view;
        item->gpu.rect = quads[i].pos;
        item->gpu.rect.x += origin.x;
        item->gpu.rect.y += origin.y;
        if (options->pixel_snap) {
            item->gpu.rect.x = roundf(item->gpu.rect.x);
            item->gpu.rect.y = roundf(item->gpu.rect.y);
            item->gpu.rect.z = roundf(item->gpu.rect.z);
            item->gpu.rect.w = roundf(item->gpu.rect.w);
        }
        item->gpu.uv = quads[i].uv;
        item->gpu.color = quads[i].color;
        item->gpu.outline = options->outline_color;
        item->gpu.shadow = options->shadow_color;
        item->gpu.clip = options->clip_px;
        item->gpu.outline_width = options->outline_width;
        item->gpu.shadow_x = options->shadow_offset.x;
        item->gpu.shadow_y = options->shadow_offset.y;
    }
    de_mutex_unlock(&r->mutex);
    return DE_OK;
}
de_gpu_texture de_text_page_texture(de_text_system *t, de_font font, u32 page) {
    if (!t || !t->render)
        return (de_gpu_texture){0};
    struct de__text_render *r = t->render;
    de__text_lock(t->runtime);
    de_mutex_lock(&r->mutex);
    de__text_page *p = de__text_page_ensure(r, font, page);
    de_gpu_texture texture = p ? p->texture : (de_gpu_texture){0};
    de_mutex_unlock(&r->mutex);
    de__text_unlock(t->runtime);
    return texture;
}
de_text_render_stats de_text_get_render_stats(const de_text_system *t) {
    de_text_render_stats result = {0};
    if (t && t->render) {
        de_mutex_lock(&t->render->mutex);
        result = t->render->stats;
        de_mutex_unlock(&t->render->mutex);
    }
    return result;
}
typedef struct text_pass {
    struct de__text_render *r;
    de_rg_resource target, depth, data;
    de_gpu_pipeline pipeline;
    de__text_push pc;
    u32 count, width, height;
    de_rg_resource lighting[7];
} text_pass;
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    text_pass *p = user;
    de_gpu_attachment color = {.texture = de_rg_texture(pass, p->target), .store = true},
                      depth = {0};
    if (p->depth.id)
        depth = (de_gpu_attachment){.texture = de_rg_texture(pass, p->depth), .store = true};
    de_gpu_cmd_begin_render(cmd, &color, 1, p->depth.id ? &depth : NULL);
    de_gpu_cmd_set_pipeline(cmd, p->pipeline);
    de_gpu_cmd_viewport(cmd, 0, 0, (f32)p->width, (f32)p->height);
    de_gpu_cmd_scissor(cmd, 0, 0, p->width, p->height);
    de__text_push pc = p->pc;
    if (p->lighting[0].id) {
        pc.lights = de_gpu_buffer_bindless(p->r->device, de_rg_buffer(pass, p->lighting[0]));
        pc.dir_lights = de_gpu_buffer_bindless(p->r->device, de_rg_buffer(pass, p->lighting[1]));
        pc.grid = de_gpu_buffer_bindless(p->r->device, de_rg_buffer(pass, p->lighting[2]));
        pc.list = de_gpu_buffer_bindless(p->r->device, de_rg_buffer(pass, p->lighting[3]));
        pc.counters = de_gpu_buffer_bindless(p->r->device, de_rg_buffer(pass, p->lighting[4]));
        if (p->lighting[5].id)
            pc.ltc1 = de_gpu_texture_bindless(p->r->device, de_rg_texture(pass, p->lighting[5]), 0,
                                              false);
        if (p->lighting[6].id)
            pc.ltc2 = de_gpu_texture_bindless(p->r->device, de_rg_texture(pass, p->lighting[6]), 0,
                                              false);
    }
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_draw(cmd, 6, p->count, 0, 0);
    de_gpu_cmd_end_render(cmd);
}
static void lighting_reads(de_rg_pass *p, de__lighting_context *lc) {
    de_rg_resource reads[] = {lc->lights,        lc->dir_lights,    lc->grid_buf,
                              lc->list,          lc->counters,      lc->ltc1,
                              lc->ltc2,          lc->strip,         lc->shadow_records,
                              lc->atlas_records, lc->atlas_texture, lc->contact_records,
                              lc->gi_records,    lc->probe_records, lc->probe_list};
    for (u32 i = 0; i < sizeof reads / sizeof reads[0]; ++i)
        if (reads[i].id)
            de_rg_read(p, reads[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->shadow_texture_count; ++i)
        de_rg_read(p, lc->shadow_textures[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->gi_texture_count; ++i) {
        de_rg_read(p, lc->gi_textures[i], DE_ACCESS_SHADER_READ);
        de_rg_write(p, lc->gi_textures[i], DE_ACCESS_SHADER_WRITE);
    }
    for (u32 i = 0; i < lc->gi_buffer_count; ++i)
        de_rg_read(p, lc->gi_buffers[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->probe_texture_count; ++i)
        de_rg_read(p, lc->probe_textures[i], DE_ACCESS_SHADER_READ);
}
void de__text_draw_stage(struct de__text_render *r, de__view_frame *vf, bool world) {
    de_text_system *t = r->text;
    de__text_lock(t->runtime);
    de_mutex_lock(&r->mutex);
    u32 slot = (u32)(vf->frame_index % 3);
    if (r->gpu_frame[slot] != vf->frame_index) {
        r->gpu_frame[slot] = vf->frame_index;
        r->cursor[slot] = 0;
    }
    de__text_queue *q = &r->queues[slot];
    if (!q->valid || q->frame != vf->frame_index) {
        de_mutex_unlock(&r->mutex);
        de__text_unlock(t->runtime);
        return;
    }
    de__text_item *items = world ? q->world : q->screen;
    u32 n = world ? q->world_count : q->screen_count;
    for (u32 i = 0; i < n; ++i)
        if (world || items[i].view.id == vf->view.id)
            (void)de__text_page_ensure(r, items[i].font, items[i].page);
    de__text_atlas_sync(r, vf);
    u32 start = (r->cursor[slot] + 255) & ~255u;
    u32 offset = start + sizeof(de__text_view);
    offset = (offset + 15) & ~15u;
    u32 first = offset, count = 0;
    u32 width = world ? vf->width : vf->output_width;
    u32 height = world ? vf->height : vf->output_height;
    de__text_view view = {.view_proj = vf->view_proj,
                          .view = vf->view_matrix,
                          .viewport = {(f32)width, (f32)height, 0, 0}};
    de__vis_context *vc =
        de__vis_frame_context(de__views_vis(r->renderer), vf->view, vf->graph, vf->frame_index);
    if (vc)
        view.camera =
            (de_vec4){vc->camera_position.x, vc->camera_position.y, vc->camera_position.z, 1};
    if (first >= DE_TEXT_FRAME_BYTES) {
        de_mutex_unlock(&r->mutex);
        de__text_unlock(t->runtime);
        return;
    }
    memcpy(r->mapped[slot] + start, &view, sizeof view);
    for (u32 i = 0; i < n; ++i) {
        de__text_item *item = &items[i];
        if (!world && item->view.id != vf->view.id)
            continue;
        de__text_page *page = de__text_page_ensure(r, item->font, item->page);
        de_text_font *font = de_text__font_get(t, item->font);
        if (!page || !page->initialized || !font ||
            sizeof(de__text_gpu_glyph) > DE_TEXT_FRAME_BYTES - offset)
            continue;
        de__text_gpu_glyph gpu = item->gpu;
        gpu.texture = page->sampled;
        gpu.mode = font->mode != DE_FONT_MSDF;
        gpu.sampler = gpu.mode ? r->nearest_index : r->linear_index;
        gpu.range = font->range;
        if (world) {
            de_dvec3 origin = vc ? vc->origin : (de_dvec3){0};
            gpu.origin =
                (de_vec4){(f32)(item->position.x - origin.x), (f32)(item->position.y - origin.y),
                          (f32)(item->position.z - origin.z), 0};
            if (gpu.flags & 1) {
                gpu.right =
                    (de_vec4){vf->view_matrix.m[0], vf->view_matrix.m[4], vf->view_matrix.m[8], 0};
                gpu.up =
                    (de_vec4){vf->view_matrix.m[1], vf->view_matrix.m[5], vf->view_matrix.m[9], 0};
            }
        } else if (r->renderer->hdr_output) {
            f32 scale = (f32)de_cvar_float(r->white_nits) / 80;
            gpu.color.x *= scale;
            gpu.color.y *= scale;
            gpu.color.z *= scale;
        }
        memcpy(r->mapped[slot] + offset, &gpu, sizeof gpu);
        offset += sizeof gpu;
        ++count;
    }
    r->cursor[slot] = offset;
    if (count) {
        text_pass *p = de_arena_push_zero(vf->graph->arena, sizeof *p, 16);
        if (p) {
            p->r = r;
            de__lighting_context *lc =
                world ? de__lighting_frame_context(de__views_lighting(r->renderer), vf->view,
                                                   vf->graph, vf->frame_index)
                      : NULL;
            p->target = lc && lc->target.id ? lc->target : vf->color;
            p->pipeline = world ? r->world_pipeline : r->ui_pipeline;
            p->count = count;
            p->width = width;
            p->height = height;
            p->data = de_rg_import_buffer(vf->graph, r->frames[slot], DE_ACCESS_SHADER_READ);
            p->pc = (de__text_push){.data = de_gpu_buffer_bindless(r->device, r->frames[slot]),
                                    .first = first,
                                    .view_offset = start,
                                    .world = world};
            if (world && vc)
                p->depth = vc->depth;
            de_rg_pass *pass = de_rg_add_pass(vf->graph, world ? "text.world" : "text.screen",
                                              DE_QUEUE_GRAPHICS, execute, p);
            if (pass) {
                de_rg_read(pass, p->data, DE_ACCESS_SHADER_READ);
                de_rg_read(pass, p->target, DE_ACCESS_RENDER_TARGET);
                de_rg_write(pass, p->target, DE_ACCESS_RENDER_TARGET);
                if (p->depth.id)
                    de_rg_read(pass, p->depth, DE_ACCESS_DEPTH_READ);
                for (u32 i = 0; i < r->page_count; ++i)
                    if (r->pages[i].initialized) {
                        de_rg_resource texture = de_rg_import_texture(
                            vf->graph, r->pages[i].texture, DE_ACCESS_SHADER_READ);
                        de_rg_read(pass, texture, DE_ACCESS_SHADER_READ);
                    }
                if (lc) {
                    p->pc.lighting_view = lc->constants_bindless;
                    p->pc.lighting_entry = lc->constants_entry;
                    p->pc.ltc_sampler = lc->sampler_bindless;
                    p->lighting[0] = lc->lights;
                    p->lighting[1] = lc->dir_lights;
                    p->lighting[2] = lc->grid_buf;
                    p->lighting[3] = lc->list;
                    p->lighting[4] = lc->counters;
                    p->lighting[5] = lc->ltc1;
                    p->lighting[6] = lc->ltc2;
                    lighting_reads(pass, lc);
                }
                ++r->stats.draw_calls;
                r->stats.glyphs += count;
            }
        }
    }
    de_mutex_unlock(&r->mutex);
    de__text_unlock(t->runtime);
}
void de__text_ui_stage(de__view_frame *vf, void *user) {
    de__text_draw_stage(user, vf, false);
}
void de__text_world_stage(de__view_frame *vf, void *user) {
    de__text_draw_stage(user, vf, true);
}
#else
#include <string.h>
de_result de_text_attach(de_text_system *t, de_renderer *r, de_world *w) {
    (void)t;
    (void)r;
    (void)w;
    return DE_OK;
}
de_result de_text_draw(de_text_system *t, de_view v, const de_glyph_quad *q, u32 n, de_vec2 o,
                       const de_text_draw_opts *opts) {
    (void)t;
    (void)v;
    (void)q;
    (void)n;
    (void)o;
    (void)opts;
    return DE_OK;
}
de_gpu_texture de_text_page_texture(de_text_system *t, de_font f, u32 p) {
    (void)t;
    (void)f;
    (void)p;
    return (de_gpu_texture){0};
}
de_text_render_stats de_text_get_render_stats(const de_text_system *t) {
    (void)t;
    return (de_text_render_stats){0};
}
#endif
