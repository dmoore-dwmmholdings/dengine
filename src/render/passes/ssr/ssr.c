#include "render/passes/ssr/ssr_internal.h"
#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"
#include "render/passes/vis/gbuffer.h"
#include "render/materials/brdf_luts.h"
#include "render/world/render_world.h"
#include "platform/de_atomic.h"
#include <string.h>
#define SSR_ACCESS (DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE)
typedef struct ssr_params {
    u32 scene, slot, depth, gb0, gb1, gb2, motion, hzb;
    u32 hits, rays, history_color, history_surface, width, height, tw, th;
    u32 hzb_width, hzb_height, mips, frame, history_valid, previous_specular, previous_moments,
        output;
    u32 write_specular, write_moments, lut, max_steps;
    f32 near_z, thickness, max_roughness, previous_exposure;
} ssr_params;
DE_STATIC_ASSERT(sizeof(ssr_params) == 128, "SsrParams is 128 bytes");
DE_STATIC_ASSERT(sizeof(ssr_params) <= DE_GPU_MAX_PUSH_CONSTANT_BYTES, "SSR push constant limit");
typedef struct ssr_view {
    de_view view;
    u32 width, height, tw, th, last, access[2];
    de_gpu_texture specular[2], moments[2], retired[4];
    u64 frame, pending_frame, retired_frame;
    bool valid, pending;
    de_atomic_u32 dispatches;
    de__ssr_info info;
} ssr_view;
struct de__ssr {
    de_renderer *renderer;
    de__scene_history *history;
    de__vis_shared *vis;
    de__vis_kernel trace, resolve;
    de_cvar max_roughness, thickness, max_steps;
    ssr_view views[DE__VIEW_MAX];
};
typedef struct ssr_pass {
    de__ssr *s;
    ssr_view *view;
    bool resolve;
    ssr_params pc;
    de_rg_resource depth, gb[3], motion, hzb, hits, rays, history_color, history_surface;
    de_rg_resource previous_specular, previous_moments, write_specular, write_moments, output;
} ssr_pass;
static void common_stage(de__view_frame *vf, void *user) {
    de__ssr *s = user;
    if (vf->settings->reflections == DE_QUALITY_OFF && vf->settings->gi == DE_QUALITY_OFF)
        return;
    const de__vis_context *vc = de__vis_frame_context(s->vis, vf->view, vf->graph, vf->frame_index);
    (void)de__scene_history_prepare(s->history, vf, vc);
}
static void history_stage(de__view_frame *vf, void *user) {
    de__ssr *s = user;
    de__scene_history_capture(s->history, vf,
                              de__lighting_frame_context(de__views_lighting(s->renderer), vf->view,
                                                         vf->graph, vf->frame_index));
}
static void release_view(de__ssr *s, ssr_view *v) {
    for (u32 i = 0; i < 2; ++i) {
        de_gpu_texture_destroy(s->renderer->dev, v->specular[i]);
        de_gpu_texture_destroy(s->renderer->dev, v->moments[i]);
    }
    for (u32 i = 0; i < 4; ++i)
        de_gpu_texture_destroy(s->renderer->dev, v->retired[i]);
    memset(v, 0, sizeof *v);
}
static bool reserve(de__ssr *s, ssr_view *v, const de__view_frame *vf, u32 tw, u32 th) {
    if (v->view.id == vf->view.id && v->width == vf->width && v->height == vf->height &&
        v->tw == tw && v->th == th)
        return true;
    de_gpu_texture sp[2] = {0}, mo[2] = {0};
    de_gpu_texture_desc d = {.width = vf->width,
                             .height = vf->height,
                             .format = DE_FMT_RGBA16_FLOAT,
                             .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                             .name = "ssr.temporal_specular"};
    for (u32 i = 0; i < 2; ++i) {
        sp[i] = de_gpu_texture_create(s->renderer->dev, &d);
        d.name = "ssr.temporal_moments";
        d.format = DE_FMT_RGBA32_FLOAT;
        mo[i] = de_gpu_texture_create(s->renderer->dev, &d);
        d.name = "ssr.temporal_specular";
        d.format = DE_FMT_RGBA16_FLOAT;
        if (!DE_HANDLE_VALID(sp[i]) || !DE_HANDLE_VALID(mo[i])) {
            for (u32 j = 0; j < 2; ++j) {
                de_gpu_texture_destroy(s->renderer->dev, sp[j]);
                de_gpu_texture_destroy(s->renderer->dev, mo[j]);
            }
            (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "SSR: temporal textures");
            return false;
        }
    }
    for (u32 i = 0; i < 4; ++i)
        de_gpu_texture_destroy(s->renderer->dev, v->retired[i]);
    for (u32 i = 0; i < 2; ++i) {
        v->retired[i * 2] = v->specular[i];
        v->retired[i * 2 + 1] = v->moments[i];
    }
    v->retired_frame = vf->frame_index;
    memcpy(v->specular, sp, sizeof sp);
    memcpy(v->moments, mo, sizeof mo);
    memset(v->access, 0, sizeof v->access);
    v->view = vf->view;
    v->width = vf->width;
    v->height = vf->height;
    v->tw = tw;
    v->th = th;
    v->last = 0;
    v->valid = false;
    return true;
}
static u32 tex(const ssr_pass *p, const de_rg_pass *pass, de_rg_resource r, bool rw) {
    return de_gpu_texture_bindless(p->s->renderer->dev, de_rg_texture(pass, r), 0, rw);
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    ssr_pass *p = user;
    ssr_params pc = p->pc;
    pc.depth = tex(p, pass, p->depth, false);
    pc.gb0 = tex(p, pass, p->gb[0], false);
    pc.gb1 = tex(p, pass, p->gb[1], false);
    pc.gb2 = tex(p, pass, p->gb[2], false);
    pc.motion = tex(p, pass, p->motion, false);
    pc.hzb = tex(p, pass, p->hzb, false);
    pc.hits = tex(p, pass, p->hits, !p->resolve);
    pc.rays = tex(p, pass, p->rays, !p->resolve);
    pc.history_color = tex(p, pass, p->history_color, true);
    pc.history_surface = tex(p, pass, p->history_surface, true);
    if (p->resolve) {
        pc.previous_specular = tex(p, pass, p->previous_specular, true);
        pc.previous_moments = tex(p, pass, p->previous_moments, true);
        pc.write_specular = tex(p, pass, p->write_specular, true);
        pc.write_moments = tex(p, pass, p->write_moments, true);
        pc.output = tex(p, pass, p->output, true);
        p->view->info.output = de_rg_texture(pass, p->output);
    }
    de_gpu_cmd_set_pipeline(cmd, p->resolve ? p->s->resolve.pipeline : p->s->trace.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    u32 w = p->resolve ? pc.width : pc.tw, h = p->resolve ? pc.height : pc.th;
    de_gpu_cmd_dispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    de_atomic_fetch_add_u32(&p->view->dispatches, 1, DE_ATOMIC_RELAXED);
}
static void reads(de_rg_pass *pass, const ssr_pass *p, const de__vis_context *vc) {
    de_rg_read(pass, p->depth, DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < 3; ++i)
        de_rg_read(pass, p->gb[i], DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->motion, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->hzb, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->history_color, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, p->history_color, DE_ACCESS_SHADER_WRITE);
    de_rg_read(pass, p->history_surface, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, p->history_surface, DE_ACCESS_SHADER_WRITE);
}
static void ssr_stage(de__view_frame *vf, void *user) {
    de__ssr *s = user;
    u32 slot = (vf->view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return;
    ssr_view *v = &s->views[slot];
    v->pending = false;
    memset(&v->info, 0, sizeof v->info);
    v->info.frame = vf->frame_index;
    de_atomic_store_u32(&v->dispatches, 0, DE_ATOMIC_RELAXED);
    if (vf->probe_capture || vf->settings->reflections == DE_QUALITY_OFF || !s->trace.pipeline.id)
        return;
    de__screen_space_context *c =
        de__scene_history_context(s->history, vf->view, vf->graph, vf->frame_index);
    const de__vis_context *vc = de__vis_frame_context(s->vis, vf->view, vf->graph, vf->frame_index);
    if (!c || !vc)
        return;
    bool half = vf->settings->reflections <= DE_QUALITY_MEDIUM;
    u32 tw = half ? (vf->width + 1) / 2 : vf->width, th = half ? (vf->height + 1) / 2 : vf->height;
    if (!reserve(s, v, vf, tw, th))
        return;
    if (vf->frame_index > v->retired_frame)
        for (u32 i = 0; i < 4; ++i) {
            de_gpu_texture_destroy(s->renderer->dev, v->retired[i]);
            v->retired[i] = (de_gpu_texture){0};
        }
    de_gpu_texture_desc d = {.width = tw,
                             .height = th,
                             .format = DE_FMT_RGBA32_FLOAT,
                             .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                             .name = "ssr.ray_hits"};
    de_rg_resource hits = de_rg_create_texture(vf->graph, &d);
    d.name = "ssr.ray_direction_pdf";
    de_rg_resource rays = de_rg_create_texture(vf->graph, &d);
    d.width = vf->width;
    d.height = vf->height;
    d.format = DE_FMT_RGBA16_FLOAT;
    d.usage |= DE_USAGE_COPY_SRC;
    d.name = "ssr.radiance_confidence";
    de_rg_resource output = de_rg_create_texture(vf->graph, &d);
    u32 write = v->last ^ 1u;
    ssr_pass *a = de_arena_push_zero(vf->graph->arena, sizeof *a, 16),
             *b = de_arena_push_zero(vf->graph->arena, sizeof *b, 16);
    if (!a || !b || !DE_HANDLE_VALID(hits) || !DE_HANDLE_VALID(rays) || !DE_HANDLE_VALID(output))
        return;
    *a = (ssr_pass){
        .s = s,
        .view = v,
        .depth = vc->depth,
        .gb = {vc->gbuffer[DE__GB0], vc->gbuffer[DE__GB1], vc->gbuffer[DE__GB2]},
        .motion = vc->gbuffer[DE__GB_MOTION],
        .hzb = c->closest.resource,
        .hits = hits,
        .rays = rays,
        .history_color = c->previous_color,
        .history_surface = c->previous_surface,
        .pc = {.scene = vc->scene_views_bindless,
               .slot = slot,
               .width = vf->width,
               .height = vf->height,
               .tw = tw,
               .th = th,
               .hzb_width = c->closest.width,
               .hzb_height = c->closest.height,
               .mips = c->closest.mips,
               .frame = (u32)vf->frame_index,
               .history_valid = c->valid ? 1u : 0u,
               .lut = de__renderer_brdf_lut(s->renderer),
               .max_steps = (u32)de_cvar_int(s->max_steps),
               .near_z = vf->camera->camera.near_z > 0 ? vf->camera->camera.near_z : 0.1f,
               .thickness = (f32)de_cvar_float(s->thickness),
               .max_roughness = (f32)de_cvar_float(s->max_roughness),
               .previous_exposure = c->previous_exposure}};
    *b = *a;
    b->resolve = true;
    b->output = output;
    b->previous_specular =
        de_rg_import_texture(vf->graph, v->specular[v->last], v->access[v->last]);
    b->previous_moments = de_rg_import_texture(vf->graph, v->moments[v->last], v->access[v->last]);
    b->write_specular = de_rg_import_texture(vf->graph, v->specular[write], v->access[write]);
    b->write_moments = de_rg_import_texture(vf->graph, v->moments[write], v->access[write]);
    if (c->valid && v->valid && v->frame + 1 == vf->frame_index)
        b->pc.history_valid |= 2u;
    de_rg_pass *p = de_rg_add_pass(vf->graph, "ssr.trace", DE_QUEUE_GRAPHICS, execute, a);
    reads(p, a, vc);
    de_rg_write(p, hits, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, rays, DE_ACCESS_SHADER_WRITE);
    p = de_rg_add_pass(vf->graph, "ssr.resolve", DE_QUEUE_GRAPHICS, execute, b);
    reads(p, b, vc);
    de_rg_read(p, hits, DE_ACCESS_SHADER_READ);
    de_rg_read(p, rays, DE_ACCESS_SHADER_READ);
    de_rg_read(p, b->previous_specular, DE_ACCESS_SHADER_READ);
    de_rg_write(p, b->previous_specular, DE_ACCESS_SHADER_WRITE);
    de_rg_read(p, b->previous_moments, DE_ACCESS_SHADER_READ);
    de_rg_write(p, b->previous_moments, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, b->write_specular, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, b->write_moments, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, output, DE_ACCESS_SHADER_WRITE);
    de_rg_export(vf->graph, b->write_specular, SSR_ACCESS);
    de_rg_export(vf->graph, b->write_moments, SSR_ACCESS);
    de__lighting_inputs *in = de__lighting_view_inputs(de__views_lighting(s->renderer), slot);
    in->indirect_specular = output;
    in->indirect_specular_opaque = true;
    v->pending = true;
    v->pending_frame = vf->frame_index;
    v->info = (de__ssr_info){.frame = vf->frame_index,
                             .width = vf->width,
                             .height = vf->height,
                             .trace_width = tw,
                             .trace_height = th,
                             .history_valid = c->valid,
                             .previous_exposure = c->previous_exposure,
                             .current_exposure = c->current_exposure};
}
de_result de__ssr_create(de_renderer *r, de__ssr **out) {
    *out = NULL;
    de__ssr *s = DE_NEW(r->alloc, de__ssr, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "SSR: state");
    memset(s, 0, sizeof *s);
    s->renderer = r;
    s->vis = de__views_vis(r);
    s->max_roughness = de_cvar_register(&(de_cvar_desc){.name = "r.ssr_max_roughness",
                                                        .type = DE_CVAR_FLOAT,
                                                        .default_number = 0.6,
                                                        .min = 0.05,
                                                        .max = 1});
    s->thickness = de_cvar_register(&(de_cvar_desc){.name = "r.ssr_thickness",
                                                    .type = DE_CVAR_FLOAT,
                                                    .default_number = 0.1,
                                                    .min = 0.001,
                                                    .max = 10});
    s->max_steps = de_cvar_register(&(de_cvar_desc){.name = "r.ssr_max_steps",
                                                    .type = DE_CVAR_INT,
                                                    .default_number = 512,
                                                    .min = 16,
                                                    .max = 2048});
    de_result result = de__scene_history_create(r, &s->history);
    if (result == DE_OK && s->vis && s->vis->enabled) {
        const char *files[] = {"ssr_trace", "ssr_resolve"},
                   *entries[] = {"cs_ssr_trace", "cs_ssr_resolve"};
        const u32 group[] = {8, 8, 1};
        de__vis_kernel *kernels[] = {&s->trace, &s->resolve};
        for (u32 i = 0; i < 2 && result == DE_OK; ++i) {
            de__engine_shader_code code;
            result = de__vis_shader_load(s->vis, files[i], &entries[i], 1, &code);
            if (result == DE_OK) {
                result = de__vis_kernel_create(s->vis, &code, 0, entries[i], group,
                                               sizeof(ssr_params), kernels[i]);
                de__engine_shader_code_free(&code);
            }
        }
    }
    if (result == DE_OK)
        result = de__view_register_stage(r, DE__VIEW_SLOT_VISIBILITY, 10, "screen.closest_depth",
                                         common_stage, s);
    if (result == DE_OK)
        result = de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, -20, "ssr", ssr_stage, s);
    if (result == DE_OK)
        result =
            de__view_register_stage(r, DE__VIEW_SLOT_POST, -10, "screen.history", history_stage, s);
    if (result != DE_OK) {
        de__ssr_destroy(s);
        return result;
    }
    *out = s;
    return DE_OK;
}
void de__ssr_destroy(de__ssr *s) {
    if (!s)
        return;
    de__view_unregister_stage(s->renderer, common_stage, s);
    de__view_unregister_stage(s->renderer, ssr_stage, s);
    de__view_unregister_stage(s->renderer, history_stage, s);
    de__scene_history_destroy(s->history);
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        release_view(s, &s->views[i]);
    de__vis_kernel_destroy(s->vis, &s->trace);
    de__vis_kernel_destroy(s->vis, &s->resolve);
    DE_FREE(s->renderer->alloc, s, sizeof *s);
}
void de__ssr_view_complete(de__ssr *s, de_view view, u64 frame, bool success) {
    if (!s)
        return;
    de__scene_history_complete(s->history, view, frame, success);
    u32 slot = (view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return;
    ssr_view *v = &s->views[slot];
    if (v->view.id != view.id || !v->pending || v->pending_frame != frame)
        return;
    if (success) {
        v->last ^= 1u;
        v->access[v->last] = SSR_ACCESS;
        v->frame = frame;
        v->valid = true;
    } else {
        v->valid = false;
        memset(v->access, 0, sizeof v->access);
    }
    v->pending = false;
}
de__scene_history *de__ssr_scene_history(de__ssr *s) {
    return s ? s->history : NULL;
}
bool de__ssr_view_info(const de__ssr *s, de_view view, de__ssr_info *out) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || !view.id || slot >= DE__VIEW_MAX || !out)
        return false;
    const ssr_view *v = &s->views[slot];
    *out = v->info;
    out->dispatches = de_atomic_load_u32(&v->dispatches, DE_ATOMIC_RELAXED);
    out->temporal = v->specular[v->last];
    out->moments = v->moments[v->last];
    return true;
}
