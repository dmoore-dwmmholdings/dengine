#include "render/upscale/upscaler.h"
#include <math.h>
#include "render/post/jitter.h"
#include "render/renderer_internal.h"
#include "render/views_internal.h"
#include "render/world/render_world.h"
#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/passes/vis/vis_stages.h"
#include "render/passes/vis/gbuffer.h"
#include <string.h>
de_upscaler de__upscaler_select(de_upscaler request, de_gpu_backend backend, u32 available) {
    if (request == DE_UPSCALE_NONE)
        return DE_UPSCALE_NONE;
    const bool fsr = (backend == DE_GPU_VULKAN || backend == DE_GPU_D3D12) &&
                     (available & (1u << DE_UPSCALE_FSR));
    const bool metalfx = backend == DE_GPU_METAL && (available & (1u << DE_UPSCALE_METALFX));
    if ((request == DE_UPSCALE_AUTO || request == DE_UPSCALE_METALFX) && metalfx)
        return DE_UPSCALE_METALFX;
    if ((request == DE_UPSCALE_AUTO || request == DE_UPSCALE_FSR) && fsr)
        return DE_UPSCALE_FSR;
    return DE_UPSCALE_TAA;
}
f32 de__upscaler_mip_bias(de__upscale_size s, f32 extra) {
    if (!s.input_width || !s.input_height || !s.output_width || !s.output_height)
        return 0;
    const f32 scale =
        fminf((f32)s.input_width / s.output_width, (f32)s.input_height / s.output_height);
    if (scale >= 1)
        return 0;
    if (!isfinite(extra))
        extra = 0;
    return log2f(scale) - fminf(4, fmaxf(0, extra));
}

typedef struct temporal_view {
    de_view view;
    void *backend;
    const de__upscaler_backend *implementation;
    de__upscale_history history;
    de__upscale_size size;
    de_render_settings settings;
    de_mat4 matrix;
    u64 camera, frame, epoch;
    u32 last, access[2];
    u32 layer_mask, excluded_light_mask;
    f32 exposure, mip_bias;
    bool initialized, valid, reset, pending;
} temporal_view;
struct de__upscalers {
    de_renderer *renderer;
    de_cvar sharpen, extra_bias;
    const de__upscaler_backend *backends[4];
    temporal_view views[DE__VIEW_MAX];
};
typedef struct temporal_pass {
    temporal_view *view;
    de__upscale_inputs inputs;
    de__upscale_outputs outputs;
    de_rg_resource color, depth, motion, previous_color, previous_depth, output, next_color,
        next_depth;
} temporal_pass;
bool de__upscaler_policy(const de_render_settings *s, const de_view_display *d, bool pixel,
                         bool capture) {
    return s && d && s->upscaler != DE_UPSCALE_NONE && !s->path_tracer && !pixel && !capture &&
           d->mode == DE_VIEW_RENDER_LIT && !(d->disabled_features & DE_VIEW_DISABLE_POST_EFFECTS);
}
static temporal_view *view_of(de_renderer *r, de_view view) {
    const u32 index = (view.id & 255u) - 1u;
    return r->upscalers && index < DE__VIEW_MAX ? &r->upscalers->views[index] : NULL;
}
void de__upscalers_forget(de_renderer *r, de_view view) {
    temporal_view *v = view_of(r, view);
    if (!v || v->view.id != view.id)
        return;
    if (v->initialized)
        v->implementation->destroy(v->backend);
    memset(v, 0, sizeof *v);
}
void de__upscalers_prepare(de__view_frame *f) {
    de_renderer *r = f->renderer;
    temporal_view *v = view_of(r, f->view);
    if (!v)
        return;
    v->pending = false;
    if (!f->world ||
        !de__upscaler_policy(f->settings, &f->display, f->pixel_art != NULL, f->probe_capture)) {
        v->valid = false;
        return;
    }
    u32 available = 0;
    for (u32 i = 1; i < 4; ++i)
        if (r->upscalers->backends[i])
            available |= 1u << i;
    const de_upscaler selected =
        de__upscaler_select(f->settings->upscaler, r->caps.backend, available);
    const de__upscaler_backend *implementation = r->upscalers->backends[selected];
    if (v->view.id != f->view.id || v->implementation != implementation) {
        if (v->initialized)
            v->implementation->destroy(v->backend);
        *v = (temporal_view){.view = f->view, .implementation = implementation};
    }
    const de__upscale_size size = {f->width, f->height, f->output_width, f->output_height};
    const bool resized = memcmp(&size, &v->size, sizeof size) != 0;
    const f32 mip_bias = de__upscaler_mip_bias(size, (f32)de_cvar_float(r->upscalers->extra_bias));
    f32 difference = 0;
    for (u32 i = 0; i < 16; ++i)
        difference = fmaxf(difference, fabsf(v->matrix.m[i] - f->view_proj.m[i]));
    const u64 camera = f->camera ? f->camera->entity : 0;
    v->reset = !v->valid || resized || v->frame + 1 != f->frame_index || v->camera != camera ||
               f->world->origin_changed || difference > .25f || v->layer_mask != f->layer_mask ||
               v->excluded_light_mask != f->excluded_light_mask || v->mip_bias != mip_bias ||
               !de__render_settings_equal(&v->settings, f->settings);
    if (v->reset)
        v->epoch = f->frame_index;
    v->size = size;
    v->settings = *f->settings;
    v->matrix = f->view_proj;
    v->camera = camera;
    v->layer_mask = f->layer_mask;
    v->excluded_light_mask = f->excluded_light_mask;
    v->mip_bias = mip_bias;
    f->temporal_enabled = true;
    f->jitter = de__jitter_sample(f->frame_index - v->epoch,
                                  de__jitter_phase_count(size.input_width, size.input_height,
                                                         size.output_width, size.output_height));
    f->mip_bias = mip_bias;
    f->proj = de__jitter_projection(f->proj, f->jitter, f->width, f->height);
    f->view_proj = de_m4_mul(&f->proj, &f->view_matrix);
}
static bool ensure_backend(de__view_frame *f, temporal_view *v) {
    de__upscalers *s = f->renderer->upscalers;
    if (!v->initialized) {
        de_result result = v->implementation->init(f->renderer, &v->backend);
        if (result != DE_OK && v->implementation->kind != DE_UPSCALE_TAA) {
            /* Device-specific native initialization can fail after registration. */
            s->backends[v->implementation->kind] = NULL;
            de_error_clear();
            v->implementation = de__taa_backend();
            result = v->implementation->init(f->renderer, &v->backend);
        }
        if (result != DE_OK) {
            f->graph->oom = f->graph->oom || result == DE_ERR_OUT_OF_MEMORY;
            ++f->graph->error_count;
            return false;
        }
        v->initialized = true;
    }
    const de__upscale_history previous = v->history;
    const de_result result = v->implementation->resize(v->backend, v->size, &v->history);
    if (result != DE_OK) {
        f->graph->oom = f->graph->oom || result == DE_ERR_OUT_OF_MEMORY;
        ++f->graph->error_count;
        return false;
    }
    if (memcmp(&previous, &v->history, sizeof previous))
        memset(v->access, 0, sizeof v->access);
    return true;
}
static void clear_input(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const de_rg_resource *resource = user;
    const de_gpu_attachment color = {
        .texture = de_rg_texture(pass, *resource), .clear = true, .store = true};
    de_gpu_cmd_begin_render(cmd, &color, 1, NULL);
    de_gpu_cmd_end_render(cmd);
}
static de_rg_resource stationary_input(de__view_frame *f, de_gpu_format format, const char *name) {
    de_rg *g = f->graph;
    de_rg_resource resource = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.width = f->width,
                                  .height = f->height,
                                  .format = format,
                                  .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                  .name = name});
    de_rg_resource *data = de_arena_push(g->arena, sizeof *data, 8);
    if (!resource.id || !data) {
        g->oom = true;
        return (de_rg_resource){0};
    }
    *data = resource;
    de_rg_pass *pass = de_rg_add_pass(g, name, DE_QUEUE_GRAPHICS, clear_input, data);
    if (!pass)
        return (de_rg_resource){0};
    de_rg_write(pass, resource, DE_ACCESS_RENDER_TARGET);
    return resource;
}
static de_gpu_texture graph_texture(const de_rg_pass *pass, de_rg_resource resource) {
    return resource.id ? de_rg_texture(pass, resource) : (de_gpu_texture){0};
}
static de_rg_resource import_history(de_rg *graph, de_gpu_texture texture, u32 access) {
    return texture.id ? de_rg_import_texture(graph, texture, access) : (de_rg_resource){0};
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    temporal_pass *p = user;
    de__upscale_inputs in = p->inputs;
    de__upscale_outputs out = p->outputs;
    in.color = de_rg_texture(pass, p->color);
    in.depth = de_rg_texture(pass, p->depth);
    in.motion = de_rg_texture(pass, p->motion);
    in.history_color = graph_texture(pass, p->previous_color);
    in.history_depth = graph_texture(pass, p->previous_depth);
    out.color = de_rg_texture(pass, p->output);
    out.history_color = graph_texture(pass, p->next_color);
    out.history_depth = graph_texture(pass, p->next_depth);
    p->view->implementation->dispatch(p->view->backend, cmd, &in, &out);
}
static void stage(de__view_frame *f, void *user) {
    de__upscalers *s = user;
    temporal_view *v = view_of(f->renderer, f->view);
    if (!f->temporal_enabled || !v ||
        (f->color.id != f->initial_color.id && !f->color_is_scene_linear))
        return;
    const de__vis_context *vc =
        de__vis_frame_context(de__views_vis(f->renderer), f->view, f->graph, f->frame_index);
    const de__lighting_context *lc = de__lighting_frame_context(de__views_lighting(f->renderer),
                                                                f->view, f->graph, f->frame_index);
    if (!f->color_is_scene_linear && (!lc || !lc->lit || !lc->target.id))
        return;
    /* Blank views never initialize an upscaler. Forward/custom HDR still needs reconstruction
     * without mesh instances. Missing per-object geometry uses far depth and stationary motion;
     * neighborhood rejection bounds unsupported motion, as for transparent mesh coverage. */
    if (!ensure_backend(f, v))
        return;
    de_rg_resource depth = vc ? vc->depth : (de_rg_resource){0};
    de_rg_resource motion = vc ? vc->gbuffer[DE__GB_MOTION] : (de_rg_resource){0};
    if (!depth.id)
        depth = stationary_input(f, DE_FMT_R32_FLOAT, "taa.far_depth");
    if (!motion.id)
        motion = stationary_input(f, DE_FMT_RG16_FLOAT, "taa.stationary_motion");
    if (!depth.id || !motion.id)
        return;
    de_rg *g = f->graph;
    temporal_pass *p = de_arena_push(g->arena, sizeof *p, 16);
    if (!p) {
        g->oom = true;
        return;
    }
    const u32 write = 1u - v->last;
    *p = (temporal_pass){
        .view = v,
        .color = f->color_is_scene_linear ? f->color : lc->target,
        .depth = depth,
        .motion = motion,
        .previous_color = import_history(g, v->history.color[v->last], v->access[v->last]),
        .previous_depth = import_history(g, v->history.depth[v->last], v->access[v->last]),
        .next_color = import_history(g, v->history.color[write], v->access[write]),
        .next_depth = import_history(g, v->history.depth[write], v->access[write]),
        .inputs = {.size = v->size,
                   .jitter = f->jitter,
                   .projection = f->proj,
                   .pre_exposure = lc ? lc->view.pre_exposure : 1,
                   .previous_pre_exposure = v->valid ? v->exposure
                                            : lc     ? lc->view.pre_exposure
                                                     : 1,
                   .reset = v->reset,
                   .delta_seconds = f->world ? f->world->delta_seconds : 0,
                   .sharpen = (f32)de_cvar_float(s->sharpen)}};
    p->output = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.width = f->output_width,
                                  .height = f->output_height,
                                  .format = DE_FMT_RGBA16_FLOAT,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                  .name = "post.taa"});
    if (!p->output.id)
        return;
    de_rg_pass *pass = de_rg_add_pass(g, "post.taa", DE_QUEUE_GRAPHICS, execute, p);
    if (!pass)
        return;
    const de_rg_resource reads[] = {p->color, p->depth, p->motion, p->previous_color,
                                    p->previous_depth};
    for (u32 i = 0; i < 5; ++i)
        if (reads[i].id)
            de_rg_read(pass, reads[i], DE_ACCESS_SHADER_READ);
    de_rg_write(pass, p->output, DE_ACCESS_SHADER_WRITE);
    const de_rg_resource writes[] = {p->next_color, p->next_depth};
    for (u32 i = 0; i < 2; ++i)
        if (writes[i].id) {
            de_rg_write(pass, writes[i], DE_ACCESS_SHADER_WRITE);
            de_rg_export(g, writes[i], DE_ACCESS_SHADER_READ);
        }
    if (p->previous_color.id)
        de_rg_export(g, p->previous_color, DE_ACCESS_SHADER_READ);
    if (p->previous_depth.id)
        de_rg_export(g, p->previous_depth, DE_ACCESS_SHADER_READ);
    v->pending = true;
    v->exposure = lc ? lc->view.pre_exposure : 1;
    f->color = p->output;
    f->color_is_scene_linear = true;
    f->width = f->output_width;
    f->height = f->output_height;
}
void de__upscalers_complete(de_renderer *r, de_view view, u64 frame, bool submitted) {
    temporal_view *v = view_of(r, view);
    if (!v || v->view.id != view.id)
        return;
    v->valid = v->pending && submitted;
    if (v->valid) {
        v->last = 1u - v->last;
        v->access[0] = v->access[1] = DE_ACCESS_SHADER_READ;
        v->frame = frame;
    } else {
        v->access[0] = v->access[1] = DE_ACCESS_NONE;
    }
    v->pending = false;
}
de_result de__upscalers_create(de_renderer *r) {
    de__upscalers *s = DE_NEW(r->alloc, de__upscalers, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "upscalers: state");
    *s = (de__upscalers){.renderer = r};
    s->backends[DE_UPSCALE_TAA] = de__taa_backend();
    r->upscalers = s;
    s->sharpen = de_cvar_register(&(de_cvar_desc){.name = "r.taa.sharpen", .type = DE_CVAR_FLOAT});
    s->extra_bias =
        de_cvar_register(&(de_cvar_desc){.name = "r.taa.mip_bias", .type = DE_CVAR_FLOAT});
    return de__view_register_stage(r, DE__VIEW_SLOT_POST, 10, "post.taa", stage, s);
}
de_result de__upscalers_register(de_renderer *r, const de__upscaler_backend *backend) {
    if (!r || !r->upscalers || !backend || !backend->name || !backend->init || !backend->resize ||
        !backend->dispatch || !backend->destroy ||
        (backend->kind != DE_UPSCALE_FSR && backend->kind != DE_UPSCALE_METALFX))
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "upscalers: invalid vendor implementation");
    if (r->upscalers->backends[backend->kind])
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "upscalers: backend is already registered");
    r->upscalers->backends[backend->kind] = backend;
    return DE_OK;
}
void de__upscalers_destroy(de_renderer *r) {
    de__upscalers *s = r->upscalers;
    if (!s)
        return;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        if (s->views[i].initialized)
            s->views[i].implementation->destroy(s->views[i].backend);
    DE_FREE(r->alloc, s, sizeof *s);
    r->upscalers = NULL;
}
