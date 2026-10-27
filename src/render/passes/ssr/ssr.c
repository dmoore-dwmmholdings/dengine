#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "render/rt/rt_internal.h"
#include "render/passes/ssr/ssr_internal.h"
#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"
#include "render/passes/vis/gbuffer.h"
#include "render/materials/brdf_luts.h"
#include "render/world/render_world.h"
#include "platform/de_atomic.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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
    de__vis_kernel trace, resolve, diag_query;
    de_cvar max_roughness, thickness, max_steps;
    ssr_view views[DE__VIEW_MAX];
    de_gpu_buffer diag_rb;
    de_rg_resource diag_reads[13];
    u64 diag_offsets[13], diag_sizes[13];
    u32 diag_pitch[12];
    ssr_params diag_pc;
    de_view diag_view;
};
typedef struct ssr_pass {
    de__ssr *s;
    ssr_view *view;
    bool resolve;
    ssr_params pc;
    de_rg_resource depth, gb[3], motion, hzb, hits, rays, history_color, history_surface;
    de_rg_resource previous_specular, previous_moments, write_specular, write_moments, output;
} ssr_pass;
/* Only used by the diagnostic pass; all product shaders and inputs are unchanged. */
static void diagnostic_query(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    ssr_pass *b = user;
    ssr_params pc = b->pc;
    de_gpu_device *dev = b->s->renderer->dev;
    pc.depth = de_gpu_texture_bindless(dev, de_rg_texture(pass, b->depth), 0, false);
    pc.gb1 = de_gpu_texture_bindless(dev, de_rg_texture(pass, b->gb[1]), 0, false);
    pc.motion = de_gpu_texture_bindless(dev, de_rg_texture(pass, b->motion), 0, false);
    pc.hits = de_gpu_texture_bindless(dev, de_rg_texture(pass, b->hits), 0, false);
    pc.history_color = de_gpu_texture_bindless(dev, de_rg_texture(pass, b->history_color), 0, true);
    pc.history_surface =
        de_gpu_texture_bindless(dev, de_rg_texture(pass, b->history_surface), 0, true);
    pc.output = de_gpu_texture_bindless(dev, de_rg_texture(pass, b->output), 0, true);
    de_gpu_cmd_set_pipeline(cmd, b->s->diag_query.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.tw + 7) / 8, (pc.th + 7) / 8, 1);
}
/* CI-only frame95 buffer evidence; never part of the product checkpoint. */
static void diagnostic_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    de__ssr *s = user;
    for (u32 i = 0; i < 12; ++i)
        de_gpu_cmd_copy_texture_to_buffer(cmd, s->diag_rb, s->diag_offsets[i], s->diag_pitch[i],
                                          de_rg_texture(pass, s->diag_reads[i]), 0, 0);
    de_gpu_cmd_copy_buffer(cmd, s->diag_rb, s->diag_offsets[12],
                           de_rg_buffer(pass, s->diag_reads[12]),
                           (u64)s->diag_pc.slot * sizeof(de__gs_view), sizeof(de__gs_view));
}
static void diagnostic_capture(de__ssr *s, const de__view_frame *vf, const ssr_pass *b,
                               const de__vis_context *vc) {
    if (!getenv("DE_DENG604_READBACK") || vf->frame_index != 95 || s->diag_rb.id)
        return;
    de_gpu_texture_desc query_desc = {.width = b->pc.tw,
                                      .height = b->pc.th,
                                      .format = DE_FMT_RGBA32_FLOAT,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                      .name = "test.ssr.hit_footprints"};
    de_rg_resource query_output = de_rg_create_texture(vf->graph, &query_desc);
    if (!DE_HANDLE_VALID(query_output))
        return;
    ssr_pass *q = de_arena_push_zero(vf->graph->arena, sizeof *q, 16);
    if (!q)
        return;
    *q = *b;
    q->output = query_output;
    de_rg_pass *query =
        de_rg_add_pass(vf->graph, "test.ssr.hit_queries", DE_QUEUE_GRAPHICS, diagnostic_query, q);
    de_rg_read(query, b->depth, DE_ACCESS_SHADER_READ);
    de_rg_read(query, b->gb[1], DE_ACCESS_SHADER_READ);
    de_rg_read(query, b->motion, DE_ACCESS_SHADER_READ);
    de_rg_read(query, b->hits, DE_ACCESS_SHADER_READ);
    de_rg_read(query, b->history_color, DE_ACCESS_SHADER_READ);
    de_rg_write(query, b->history_color, DE_ACCESS_SHADER_WRITE);
    de_rg_read(query, b->history_surface, DE_ACCESS_SHADER_READ);
    de_rg_write(query, b->history_surface, DE_ACCESS_SHADER_WRITE);
    de_rg_read(query, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_write(query, query_output, DE_ACCESS_SHADER_WRITE);
    const de_rg_resource r[13] = {b->depth,
                                  b->gb[1],
                                  b->motion,
                                  b->hits,
                                  b->rays,
                                  b->history_color,
                                  b->history_surface,
                                  b->previous_specular,
                                  b->previous_moments,
                                  b->output,
                                  b->write_moments,
                                  query_output,
                                  vc->scene_views};
    /* Temporal moments are RGBA32_FLOAT; radiance/specular are RGBA16_FLOAT. */
    const u32 bytes[12] = {4, 4, 4, 16, 16, 8, 16, 8, 16, 8, 16, 16};
    u64 total = 0;
    for (u32 i = 0; i < 13; ++i) {
        s->diag_reads[i] = r[i];
        s->diag_offsets[i] = total;
        if (i < 12) {
            bool trace = i == 3 || i == 4 || i == 11;
            u32 w = trace ? b->pc.tw : b->pc.width;
            u32 h = trace ? b->pc.th : b->pc.height;
            s->diag_pitch[i] = (w * bytes[i] + 511u) & ~511u;
            s->diag_sizes[i] = (u64)s->diag_pitch[i] * h;
        } else {
            s->diag_sizes[i] = sizeof(de__gs_view);
        }
        total = (total + s->diag_sizes[i] + 511u) & ~511ull;
    }
    s->diag_pc = b->pc;
    s->diag_view = vf->view;
    s->diag_rb =
        de_gpu_buffer_create(s->renderer->dev, &(de_gpu_buffer_desc){.size = total,
                                                                     .usage = DE_USAGE_COPY_DST,
                                                                     .memory = DE_MEM_READBACK});
    if (!s->diag_rb.id)
        return;
    de_rg_pass *p =
        de_rg_add_pass(vf->graph, "test.ssr.frame95", DE_QUEUE_GRAPHICS, diagnostic_copy, s);
    for (u32 i = 0; i < 13; ++i)
        de_rg_read(p, r[i], DE_ACCESS_COPY_SRC);
    de_rg_write(p, de_rg_import_buffer(vf->graph, s->diag_rb, DE_ACCESS_COPY_DST),
                DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(p);
}
bool de__ssr_diagnostic_dump(de__ssr *s, de_view view, const char *dir) {
    if (!s || !s->diag_rb.id || s->diag_view.id != view.id)
        return false;
    const u8 *data = de_gpu_buffer_map(s->renderer->dev, s->diag_rb);
    if (!data)
        return false;
    const char *names[13] = {"depth",
                             "gbuffer1",
                             "motion",
                             "hits",
                             "rays",
                             "history_color",
                             "history_surface",
                             "previous_specular",
                             "previous_moments",
                             "output",
                             "write_moments",
                             "hit_footprints",
                             "scene"};
    char path[2048];
    bool ok = true;
    for (u32 i = 0; i < 14; ++i) {
        snprintf(path, sizeof path, "%s/%s.bin", dir, i < 13 ? names[i] : "params");
        FILE *file = fopen(path, "wb");
        if (!file) {
            ok = false;
            break;
        }
        const void *src = i < 13 ? (const void *)(data + s->diag_offsets[i]) : &s->diag_pc;
        size_t n = i < 13 ? (size_t)s->diag_sizes[i] : sizeof s->diag_pc;
        bool written = fwrite(src, 1, n, file) == n;
        if (fclose(file) != 0 || !written) {
            ok = false;
            break;
        }
    }
    if (!ok)
        return false;
    snprintf(path, sizeof path, "%s/metadata.json", dir);
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    fprintf(file,
            "{\"frame\":95,\"width\":%u,\"height\":%u,\"trace_width\":%u,\"trace_height\":%u,\"row_"
            "pitches\":[",
            s->diag_pc.width, s->diag_pc.height, s->diag_pc.tw, s->diag_pc.th);
    for (u32 i = 0; i < 12; ++i)
        fprintf(file, "%s%u", i ? "," : "", s->diag_pitch[i]);
    fprintf(file, "]}\n");
    return fclose(file) == 0;
}

static void common_stage(de__view_frame *vf, void *user) {
    de__ssr *s = user;
    if (vf->settings->reflections == DE_QUALITY_OFF &&
        (vf->settings->gi == DE_QUALITY_OFF || vf->settings->gi_mode == DE_GI_STATIC_PROBES))
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
    if (p->resolve && p->s->diag_rb.id && p->s->diag_view.id == p->view->view.id)
        p->s->diag_pc = pc;
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
                             .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED |
                                      (getenv("DE_DENG604_READBACK") ? DE_USAGE_COPY_SRC : 0),
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
               .frame = (u32)de__rt_sample_frame(s->renderer, vf->frame_index),
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
    diagnostic_capture(s, vf, b, vc);
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
    if (result == DE_OK && getenv("DE_DENG604_READBACK")) {
        const char *entry = "cs_ssr_scene_hit_queries";
        const u32 group[] = {8, 8, 1};
        de__engine_shader_code code;
        result = de__vis_shader_load(s->vis, "ssr_resolve", &entry, 1, &code);
        if (result == DE_OK) {
            result = de__vis_kernel_create(s->vis, &code, 0, entry, group, sizeof(ssr_params),
                                           &s->diag_query);
            de__engine_shader_code_free(&code);
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
    de_gpu_buffer_destroy(s->renderer->dev, s->diag_rb);
    de__scene_history_destroy(s->history);
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        release_view(s, &s->views[i]);
    de__vis_kernel_destroy(s->vis, &s->trace);
    de__vis_kernel_destroy(s->vis, &s->resolve);
    de__vis_kernel_destroy(s->vis, &s->diag_query);
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
