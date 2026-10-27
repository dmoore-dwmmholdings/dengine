#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "render/passes/ssr/ssr_internal.h"
#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"
#include "render/passes/vis/gbuffer.h"
#include "render/world/render_world.h"
#include <string.h>
#include <stdlib.h>

#define HISTORY_ACCESS (DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE)
typedef struct history_view {
    de_view view;
    u32 width, height, last, access[2];
    de_gpu_texture color[2], surface[2], retired[4];
    u64 retired_frame, frame;
    f32 exposure;
    bool valid;
    de__screen_space_context context;
} history_view;
struct de__scene_history {
    de_renderer *renderer;
    de__vis_shared *vis;
    de__vis_kernel copy;
    history_view views[DE__VIEW_MAX];
};
typedef struct history_params {
    u32 color, depth, normal, dst_color, dst_surface, width, height, reserved;
} history_params;
DE_STATIC_ASSERT(sizeof(history_params) == 32, "HistoryParams is 32 bytes");
typedef struct history_pass {
    de__scene_history *h;
    history_params pc;
    de_rg_resource color, depth, normal, dst_color, dst_surface;
} history_pass;
static void release_view(de__scene_history *h, history_view *v) {
    for (u32 i = 0; i < 2; ++i) {
        de_gpu_texture_destroy(h->renderer->dev, v->color[i]);
        de_gpu_texture_destroy(h->renderer->dev, v->surface[i]);
    }
    for (u32 i = 0; i < 4; ++i)
        de_gpu_texture_destroy(h->renderer->dev, v->retired[i]);
    memset(v, 0, sizeof *v);
}
de_result de__scene_history_create(de_renderer *r, de__scene_history **out) {
    *out = NULL;
    de__scene_history *h = DE_NEW(r->alloc, de__scene_history, DE_MEM_RENDER);
    if (!h)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene history: state");
    memset(h, 0, sizeof *h);
    h->renderer = r;
    h->vis = de__views_vis(r);
    if (h->vis && h->vis->enabled) {
        const char *entries[] = {"cs_scene_history"};
        const u32 group[] = {8, 8, 1};
        de__engine_shader_code code;
        de_result result = de__vis_shader_load(h->vis, "ssr_resolve", entries, 1, &code);
        if (result == DE_OK) {
            result = de__vis_kernel_create(h->vis, &code, 0, entries[0], group,
                                           sizeof(history_params), &h->copy);
            de__engine_shader_code_free(&code);
        }
        if (result != DE_OK) {
            de__scene_history_destroy(h);
            return result;
        }
    }
    *out = h;
    return DE_OK;
}
void de__scene_history_destroy(de__scene_history *h) {
    if (!h)
        return;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        release_view(h, &h->views[i]);
    de__vis_kernel_destroy(h->vis, &h->copy);
    DE_FREE(h->renderer->alloc, h, sizeof *h);
}
static bool reserve(de__scene_history *h, history_view *v, const de__view_frame *vf) {
    if (v->view.id == vf->view.id && v->width == vf->width && v->height == vf->height)
        return true;
    de_gpu_texture color[2] = {0}, surface[2] = {0};
    de_gpu_texture_desc desc = {.width = vf->width,
                                .height = vf->height,
                                .format = DE_FMT_RGBA16_FLOAT,
                                .usage = DE_USAGE_STORAGE |
                                         (getenv("DE_DENG604_READBACK") ? DE_USAGE_COPY_SRC : 0),
                                .name = "history.color"};
    for (u32 i = 0; i < 2; ++i) {
        color[i] = de_gpu_texture_create(h->renderer->dev, &desc);
        desc.format = DE_FMT_RGBA32_FLOAT;
        desc.name = "history.surface";
        surface[i] = de_gpu_texture_create(h->renderer->dev, &desc);
        desc.format = DE_FMT_RGBA16_FLOAT;
        desc.name = "history.color";
        if (!DE_HANDLE_VALID(color[i]) || !DE_HANDLE_VALID(surface[i])) {
            for (u32 j = 0; j < 2; ++j) {
                de_gpu_texture_destroy(h->renderer->dev, color[j]);
                de_gpu_texture_destroy(h->renderer->dev, surface[j]);
            }
            (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene history: textures");
            return false;
        }
    }
    for (u32 i = 0; i < 4; ++i)
        de_gpu_texture_destroy(h->renderer->dev, v->retired[i]);
    for (u32 i = 0; i < 2; ++i) {
        v->retired[i * 2] = v->color[i];
        v->retired[i * 2 + 1] = v->surface[i];
    }
    v->retired_frame = vf->frame_index;
    memcpy(v->color, color, sizeof color);
    memcpy(v->surface, surface, sizeof surface);
    memset(v->access, 0, sizeof v->access);
    v->view = vf->view;
    v->width = vf->width;
    v->height = vf->height;
    v->last = 0;
    v->valid = false;
    return true;
}
de__screen_space_context *de__scene_history_prepare(de__scene_history *h, const de__view_frame *vf,
                                                    const de__vis_context *vc) {
    if (!h || !h->copy.pipeline.id || vf->probe_capture || !vc ||
        !DE_HANDLE_VALID(vc->gbuffer[DE__GB1]))
        return NULL;
    u32 slot = (vf->view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return NULL;
    history_view *v = &h->views[slot];
    if (!reserve(h, v, vf))
        return NULL;
    if (vf->frame_index > v->retired_frame)
        for (u32 i = 0; i < 4; ++i) {
            de_gpu_texture_destroy(h->renderer->dev, v->retired[i]);
            v->retired[i] = (de_gpu_texture){0};
        }
    de__screen_space_context *c = &v->context;
    memset(c, 0, sizeof *c);
    c->view = vf->view;
    c->graph = vf->graph;
    c->frame = vf->frame_index;
    c->width = vf->width;
    c->height = vf->height;
    c->valid = v->valid && v->frame + 1 == vf->frame_index && !vc->occlusion_reset;
    c->previous_exposure = v->exposure > 0 ? v->exposure : 1;
    c->current_exposure = de__exposure_from_ev100(vf->camera->camera.exposure_ev100);
    u32 write = v->last ^ 1u;
    c->previous_color = de_rg_import_texture(vf->graph, v->color[v->last], v->access[v->last]);
    c->previous_surface = de_rg_import_texture(vf->graph, v->surface[v->last], v->access[v->last]);
    c->write_color = de_rg_import_texture(vf->graph, v->color[write], v->access[write]);
    c->write_surface = de_rg_import_texture(vf->graph, v->surface[write], v->access[write]);
    de__vis_hzb_size(vf->width, vf->height, &c->closest.width, &c->closest.height,
                     &c->closest.mips);
    c->closest.resource = de_rg_create_texture(
        vf->graph, &(de_gpu_texture_desc){.width = c->closest.width,
                                          .height = c->closest.height,
                                          .mip_count = c->closest.mips,
                                          .format = DE_FMT_R32_FLOAT,
                                          .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                          .name = "screen.closest_depth"});
    de__vis_hzb_add_passes(h->vis, vf->graph, vc->depth, vf->width, vf->height, &c->closest,
                           DE__VIS_HZB_OP_MAX, false);
    return c;
}
de__screen_space_context *de__scene_history_context(de__scene_history *h, de_view view, de_rg *g,
                                                    u64 frame) {
    if (!h || !(view.id & 255u))
        return NULL;
    u32 slot = (view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return NULL;
    de__screen_space_context *c = &h->views[slot].context;
    return c->view.id == view.id && c->graph == g && c->frame == frame ? c : NULL;
}
static void capture_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    history_pass *p = user;
    de_gpu_device *dev = p->h->renderer->dev;
    history_params pc = p->pc;
    pc.color = de_gpu_texture_bindless(dev, de_rg_texture(pass, p->color), 0, false);
    pc.depth = de_gpu_texture_bindless(dev, de_rg_texture(pass, p->depth), 0, false);
    pc.normal = de_gpu_texture_bindless(dev, de_rg_texture(pass, p->normal), 0, false);
    pc.dst_color = de_gpu_texture_bindless(dev, de_rg_texture(pass, p->dst_color), 0, true);
    pc.dst_surface = de_gpu_texture_bindless(dev, de_rg_texture(pass, p->dst_surface), 0, true);
    de_gpu_cmd_set_pipeline(cmd, p->h->copy.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.width + 7) / 8, (pc.height + 7) / 8, 1);
}
void de__scene_history_capture(de__scene_history *h, const de__view_frame *vf,
                               const de__lighting_context *lc) {
    de__screen_space_context *c =
        de__scene_history_context(h, vf->view, vf->graph, vf->frame_index);
    if (!c || !lc || !lc->lit)
        return;
    const de__vis_context *vc = de__vis_frame_context(h->vis, vf->view, vf->graph, vf->frame_index);
    history_pass *p = de_arena_push_zero(vf->graph->arena, sizeof *p, 16);
    if (!p)
        return;
    *p = (history_pass){.h = h,
                        .color = lc->target,
                        .depth = vc->depth,
                        .normal = vc->gbuffer[DE__GB1],
                        .dst_color = c->write_color,
                        .dst_surface = c->write_surface,
                        .pc = {.width = vf->width, .height = vf->height}};
    de_rg_pass *pass =
        de_rg_add_pass(vf->graph, "screen.history_capture", DE_QUEUE_GRAPHICS, capture_execute, p);
    de_rg_read(pass, p->color, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->depth, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->normal, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, p->dst_color, DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass, p->dst_surface, DE_ACCESS_SHADER_WRITE);
    de_rg_export(vf->graph, p->dst_color, HISTORY_ACCESS);
    de_rg_export(vf->graph, p->dst_surface, HISTORY_ACCESS);
    c->capture_added = true;
}
void de__scene_history_complete(de__scene_history *h, de_view view, u64 frame, bool success) {
    if (!h || !(view.id & 255u))
        return;
    u32 slot = (view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return;
    history_view *v = &h->views[slot];
    de__screen_space_context *c = &v->context;
    if (c->view.id != view.id || c->frame != frame)
        return;
    if (success && c->capture_added) {
        v->last ^= 1u;
        v->access[v->last] = HISTORY_ACCESS;
        v->exposure = c->current_exposure;
        v->frame = frame;
        v->valid = true;
    } else {
        v->valid = false;
        memset(v->access, 0, sizeof v->access);
    }
}
