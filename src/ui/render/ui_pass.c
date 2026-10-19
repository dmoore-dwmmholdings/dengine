#include "draw_list.h"
#if DE_HAS_RENDER
#include <math.h>
#include <string.h>
typedef struct ui_run {
    u32 first, count;
    de_vec4 clip;
} ui_run;
typedef struct ui_push {
    u32 data, first;
    f32 width, height, white;
} ui_push;
typedef struct ui_frame_pass {
    struct de_ui_render *owner;
    de_rg_resource target, buffer;
    de_render_hook_info info;
    ui_run *runs;
    u32 count;
} ui_frame_pass;
struct de_ui_pass_state {
    ui_frame_pass frames[DE_UI_RENDER_GPU_SLOTS];
};
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ui_frame_pass *p = user;
    struct de_ui_render *r = p->owner;
    de_gpu_attachment color = {
        .texture = de_rg_texture(pass, p->target), .clear = false, .store = true};
    de_gpu_cmd_begin_render(cmd, &color, 1, NULL);
    de_gpu_cmd_set_pipeline(cmd, r->pipeline);
    de_gpu_cmd_viewport(cmd, 0, 0, (f32)p->info.width, (f32)p->info.height);
    ui_push push = {.data = de_gpu_buffer_bindless(r->device, de_rg_buffer(pass, p->buffer)),
                    .width = (f32)p->info.width,
                    .height = (f32)p->info.height,
                    .white = p->info.ui_white_multiplier};
    for (u32 i = 0; i < p->count; ++i) {
        const ui_run *run = &p->runs[i];
        i32 left = (i32)fmaxf(0, floorf(run->clip.x)), top = (i32)fmaxf(0, floorf(run->clip.y));
        i32 right = (i32)fminf((f32)p->info.width, ceilf(run->clip.x + run->clip.z));
        i32 bottom = (i32)fminf((f32)p->info.height, ceilf(run->clip.y + run->clip.w));
        if (right <= left || bottom <= top)
            continue;
        de_gpu_cmd_scissor(cmd, (u32)left, (u32)top, (u32)(right - left), (u32)(bottom - top));
        push.first = run->first;
        de_gpu_cmd_push_constants(cmd, &push, sizeof push);
        de_gpu_cmd_draw(cmd, 6, run->count, 0, 0);
    }
    de_gpu_cmd_end_render(cmd);
}
void de_ui__render_hook(de_rg *graph, de_view view, void *user) {
    struct de_ui_render *r = user;
    de_render_hook_info info;
    if (!de_view_hook_info(r->ui->desc.renderer, view, &info) ||
        info.ring_index >= DE_UI_RENDER_GPU_SLOTS || !info.width || !info.height ||
        info.color_format != r->pipeline_format)
        return;
    de_mutex_lock(&r->mutex);
    const de_ui_draw_list *packet = NULL;
    for (u32 i = 0; i < DE_UI_RENDER_SNAPSHOTS; ++i)
        if (r->snapshots[i].valid && r->snapshots[i].generation == info.extract_frame) {
            packet = &r->snapshots[i];
            r->snapshots[i].last_gpu_fence = info.frame_index + 1;
            break;
        }
    if (!packet) {
        de_mutex_unlock(&r->mutex);
        return;
    }
    ui_frame_pass *p = &r->pass->frames[info.ring_index];
    p->info = info;
    p->count = 0;
    p->owner = r;
    de_ui_gpu_quad *mapped = de_gpu_buffer_map(r->device, r->buffers[info.ring_index]);
    if (!mapped) {
        de_mutex_unlock(&r->mutex);
        return;
    }
    u32 count = 0;
    for (u32 i = 0; i < packet->count; ++i) {
        const de_ui_draw_cmd *c = &packet->commands[i];
        if (packet->roots[c->root].world ||
            (c->gpu.kind != DE_UI_QUAD_RECT && c->gpu.texture == UINT32_MAX))
            continue;
        /* Identical packet data is written only once per frame, even with several view hooks. */
        if (r->upload_frame[info.ring_index] != info.frame_index)
            mapped[count] = c->gpu;
        if (!p->count || memcmp(&p->runs[p->count - 1].clip, &c->clip, sizeof c->clip) != 0)
            p->runs[p->count++] = (ui_run){count, 0, c->clip};
        ++p->runs[p->count - 1].count;
        ++count;
    }
    if (r->upload_frame[info.ring_index] != info.frame_index)
        r->stats.draw_calls = 0;
    r->upload_frame[info.ring_index] = info.frame_index;
    r->stats.draw_calls += p->count;
    de_mutex_unlock(&r->mutex);
    if (!count)
        return;
    p->target = de_view_color(r->ui->desc.renderer, view);
    p->buffer = de_rg_import_buffer(graph, r->buffers[info.ring_index], DE_ACCESS_SHADER_READ);
    de_rg_pass *draw = de_rg_add_pass(graph, "UI.Retained", DE_QUEUE_GRAPHICS, execute, p);
    if (!draw)
        return;
    de_rg_read(draw, p->target, DE_ACCESS_RENDER_TARGET);
    de_rg_write(draw, p->target, DE_ACCESS_RENDER_TARGET);
    de_rg_read(draw, p->buffer, DE_ACCESS_SHADER_READ);
}
de_result de_ui__pass_init(struct de_ui_render *r) {
    if (!r->ui->desc.renderer)
        return DE_OK;
    r->device = de_renderer_device(r->ui->desc.renderer);
    r->pipeline_format = DE_FMT_RGBA16_FLOAT;
    de_gpu_pipeline_desc pipeline = {.kind = DE_PIPE_GRAPHICS,
                                     .color_count = 1,
                                     .color_formats = {DE_FMT_RGBA16_FLOAT},
                                     .blend = {DE_BLEND_PREMUL},
                                     .cull = DE_CULL_NONE,
                                     .name = "retained UI bindless quads"};
    DE_TRY(de_render_builtin_graphics_pipeline(r->ui->desc.renderer, "ui", 0, "vs_main", "ps_main",
                                               &pipeline, &r->pipeline));
    r->linear = de_gpu_sampler_create(r->device, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                        .mag = DE_FILTER_LINEAR,
                                                                        .mip = DE_FILTER_NEAREST,
                                                                        .u = DE_ADDR_CLAMP,
                                                                        .v = DE_ADDR_CLAMP,
                                                                        .w = DE_ADDR_CLAMP});
    r->nearest = de_gpu_sampler_create(r->device, &(de_gpu_sampler_desc){.min = DE_FILTER_NEAREST,
                                                                         .mag = DE_FILTER_NEAREST,
                                                                         .mip = DE_FILTER_NEAREST,
                                                                         .u = DE_ADDR_CLAMP,
                                                                         .v = DE_ADDR_CLAMP,
                                                                         .w = DE_ADDR_CLAMP});
    r->pass = DE_NEW(r->ui->alloc, struct de_ui_pass_state, DE_MEM_UI);
    if (!r->pass)
        return DE_ERR_OUT_OF_MEMORY;
    memset(r->pass, 0, sizeof *r->pass);
    for (u32 i = 0; i < DE_UI_RENDER_GPU_SLOTS; ++i) {
        r->upload_frame[i] = UINT64_MAX;
        r->buffers[i] = de_gpu_buffer_create(
            r->device, &(de_gpu_buffer_desc){.size = (u64)r->capacity * sizeof(de_ui_gpu_quad),
                                             .usage = DE_USAGE_STORAGE,
                                             .memory = DE_MEM_UPLOAD,
                                             .name = "retained UI frame quads"});
        r->pass->frames[i].runs =
            DE_ALLOC(r->ui->alloc, (u64)r->capacity * sizeof(ui_run), _Alignof(ui_run), DE_MEM_UI);
        if (!r->buffers[i].id || !r->pass->frames[i].runs)
            return DE_ERR_OUT_OF_MEMORY;
    }
    if (!r->linear.id || !r->nearest.id)
        return DE_ERR_OUT_OF_MEMORY;
    de_error_clear();
    de_render_add_hook(r->ui->desc.renderer, DE_HOOK_UI, de_ui__render_hook, r);
    if (de_error_last() && de_error_last()->result != DE_OK)
        return de_error_last()->result;
    r->hook_registered = true;
    return DE_OK;
}
void de_ui__pass_destroy(struct de_ui_render *r) {
    if (r->hook_registered) {
        (void)de_render_remove_hook(r->ui->desc.renderer, DE_HOOK_UI, de_ui__render_hook, r);
        r->hook_registered = false;
    }
    if (r->device) {
        de_gpu_wait_idle(r->device);
        for (u32 i = 0; i < DE_UI_RENDER_GPU_SLOTS; ++i)
            de_gpu_buffer_destroy(r->device, r->buffers[i]);
        de_gpu_pipeline_destroy(r->device, r->pipeline);
        de_gpu_sampler_destroy(r->device, r->linear);
        de_gpu_sampler_destroy(r->device, r->nearest);
    }
    if (r->pass) {
        for (u32 i = 0; i < DE_UI_RENDER_GPU_SLOTS; ++i)
            if (r->pass->frames[i].runs)
                DE_FREE(r->ui->alloc, r->pass->frames[i].runs, (u64)r->capacity * sizeof(ui_run));
        DE_FREE(r->ui->alloc, r->pass, sizeof *r->pass);
        r->pass = NULL;
    }
}
#else
de_result de_ui__pass_init(struct de_ui_render *r) {
    (void)r;
    return DE_OK;
}
void de_ui__pass_destroy(struct de_ui_render *r) {
    (void)r;
}
void de_ui__render_hook(de_rg *g, de_view v, void *user) {
    (void)g;
    (void)v;
    (void)user;
}
#endif
