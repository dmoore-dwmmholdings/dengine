#include "render/rt/rt_internal.h"
#include "render/passes/ao/gtao_internal.h"
#include "render/renderer_internal.h"
#include "render/rt/rtao.h"
#include "render/graph/rg_internal.h"
#include "render/passes/vis/gbuffer.h"
#include "platform/de_atomic.h"
#include <string.h>
#include <math.h>

#define GTAO_ACCESS (DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE)
typedef struct gtao_view {
    de_view view;
    u32 width, height, tw, th, last, access[2];
    de_quality quality;
    de__gtao_config config;
    de__gtao_config pending_config;
    de_quality pending_quality;
    de_mat4 view_proj, pending_view_proj;
    de_gpu_texture history[2], surface[2];
    de_rg *pending;
    u64 frame, pending_frame;
    bool valid;
    de_atomic_u32 dispatches;
    de__gtao_info info;
} gtao_view;
struct de__gtao {
    de_gpu_device *dev;
    const de_allocator *alloc;
    de__vis_shared *vis;
    de_renderer *renderer;
    de__vis_kernel kernels[4];
    de_cvar radius, falloff, thickness, bent;
    gtao_view views[DE__VIEW_MAX];
};
typedef struct gtao_pass {
    de__gtao *s;
    gtao_view *v;
    de__gtao_params pc;
    u32 kind;
    de_rg_resource depth, normal, motion, raw, spatial, previous, previous_surface;
    de_rg_resource history, history_surface, packed, output, bent;
} gtao_pass;

static void release_view(de__gtao *s, gtao_view *v) {
    for (u32 i = 0; i < 2; ++i) {
        de_gpu_texture_destroy(s->dev, v->history[i]);
        de_gpu_texture_destroy(s->dev, v->surface[i]);
    }
    memset(v, 0, sizeof *v);
}
static de_result reserve(de__gtao *s, gtao_view *v, const de__view_frame *vf, u32 tw, u32 th) {
    if (v->view.id == vf->view.id && v->width == vf->width && v->height == vf->height &&
        v->tw == tw && v->th == th)
        return DE_OK;
    de_gpu_texture history[2] = {0}, surface[2] = {0};
    for (u32 i = 0; i < 2; ++i) {
        de_gpu_texture_desc d = {.width = tw,
                                 .height = th,
                                 .format = DE_FMT_RGBA16_FLOAT,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                 .name = "gtao.history"};
        history[i] = de_gpu_texture_create(s->dev, &d);
        d.format = DE_FMT_RGBA32_FLOAT;
        d.name = "gtao.surface";
        surface[i] = de_gpu_texture_create(s->dev, &d);
        if (!history[i].id || !surface[i].id) {
            for (u32 j = 0; j < 2; ++j) {
                de_gpu_texture_destroy(s->dev, history[j]);
                de_gpu_texture_destroy(s->dev, surface[j]);
            }
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GTAO: history textures");
        }
    }
    release_view(s, v);
    memcpy(v->history, history, sizeof history);
    memcpy(v->surface, surface, sizeof surface);
    v->view = vf->view;
    v->width = vf->width;
    v->height = vf->height;
    v->tw = tw;
    v->th = th;
    return DE_OK;
}
static u32 texture(const gtao_pass *p, const de_rg_pass *pass, de_rg_resource r, bool rw) {
    return r.id ? de_gpu_texture_bindless(p->s->dev, de_rg_texture(pass, r), 0, rw) : 0;
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    gtao_pass *p = user;
    de__gtao_params pc = p->pc;
    if (p->kind == 4) {
        de_gpu_buffer buffer = de_rg_buffer(pass, p->packed);
        de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, p->output), 0, 0, buffer, 0,
                                          pc.pitch_ao);
        if (p->bent.id)
            de_gpu_cmd_copy_buffer_to_texture(cmd, de_rg_texture(pass, p->bent), 0, 0, buffer,
                                              pc.bent_offset, pc.pitch_bent);
        p->v->info.output = de_rg_texture(pass, p->output);
        p->v->info.bent = p->bent.id ? de_rg_texture(pass, p->bent) : (de_gpu_texture){0};
        return;
    }
    pc.depth = texture(p, pass, p->depth, false);
    pc.normal = texture(p, pass, p->normal, false);
    if (p->kind == 0)
        pc.raw = texture(p, pass, p->raw, true);
    if (p->kind == 1) {
        pc.raw = texture(p, pass, p->raw, true);
        pc.spatial = texture(p, pass, p->spatial, true);
    }
    if (p->kind == 2) {
        pc.motion = texture(p, pass, p->motion, false);
        pc.spatial = texture(p, pass, p->spatial, true);
        pc.previous = texture(p, pass, p->previous, true);
        pc.previous_surface = texture(p, pass, p->previous_surface, true);
    }
    if (p->kind >= 2) {
        pc.history = texture(p, pass, p->history, true);
        pc.history_surface = texture(p, pass, p->history_surface, true);
    }
    if (p->kind == 3)
        pc.packed = de_gpu_buffer_bindless(p->s->dev, de_rg_buffer(pass, p->packed));
    de_gpu_cmd_set_pipeline(cmd, p->s->kernels[p->kind].pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    u32 w = p->kind == 3 ? pc.pitch_ao / 4 : pc.tw, h = p->kind == 3 ? pc.height : pc.th;
    de_gpu_cmd_dispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    de_atomic_fetch_add_u32(&p->v->dispatches, 1, DE_ATOMIC_RELAXED);
}

de_result de__gtao_build(de__gtao *s, const de__view_frame *vf, const de__vis_context *vc,
                         const de__gtao_config *config, de__lighting_inputs *inputs) {
    if (!s || !vf || !vf->graph || !vf->settings || !inputs || !vf->view.id || !vf->width ||
        !vf->height)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GTAO: build arguments");
    u32 slot = (vf->view.id & 255u) - 1u;
    if (slot >= DE__VIEW_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GTAO: view slot");
    gtao_view *v = &s->views[slot];
    if (v->pending)
        return DE_FAIL(DE_ERR_BUSY, "GTAO: graph already pending");
    if (v->view.id && v->view.id != vf->view.id)
        release_view(s, v);
    memset(&v->info, 0, sizeof v->info);
    v->info.frame = vf->frame_index;
    de_atomic_store_u32(&v->dispatches, 0, DE_ATOMIC_RELAXED);
    if (vf->probe_capture || vf->settings->ao == DE_QUALITY_OFF) {
        v->valid = false;
        return DE_OK;
    }
    if (!s->kernels[0].pipeline.id)
        return DE_FAIL(DE_ERR_INTERNAL, "GTAO: visibility unavailable");
    if (!vc || !vc->depth.id || !vc->gbuffer[DE__GB1].id || !vc->gbuffer[DE__GB_MOTION].id ||
        !vc->scene_views.id) {
        v->valid = false;
        if (vc && vc->instance_count)
            return DE_FAIL(DE_ERR_INTERNAL, "GTAO: incomplete nonempty visibility");
        return DE_OK; /* Empty views have no thin G-buffer and need no AO work. */
    }
    de__gtao_config cfg =
        config ? *config : (de__gtao_config){.radius = 1, .falloff = .8f, .thickness = .2f};
    if (!isfinite(cfg.radius) || !isfinite(cfg.falloff) || !isfinite(cfg.thickness) ||
        cfg.radius <= 0 || cfg.falloff < 0 || cfg.falloff >= 1 || cfg.thickness < 0 ||
        cfg.thickness > 1)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GTAO: configuration");
    u32 tw, th, slices, steps;
    de__gtao_quality(vf->settings->ao, vf->width, vf->height, &tw, &th, &slices, &steps);
    de_result result = reserve(s, v, vf, tw, th);
    if (result != DE_OK)
        return result;
    bool history = v->valid && v->frame + 1 == vf->frame_index && v->quality == vf->settings->ao &&
                   v->config.radius == cfg.radius && v->config.falloff == cfg.falloff &&
                   v->config.thickness == cfg.thickness && v->config.bent == cfg.bent;
    history = history && !vc->occlusion_reset &&
              memcmp(vf->prev_view_proj.m, v->view_proj.m, sizeof v->view_proj.m) == 0;
    de_rg *g = vf->graph;
    gtao_pass *p = de_arena_push_zero(g->arena, 5 * sizeof *p, 16);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GTAO: graph parameters");
    const u32 write = v->last ^ 1u, pitch_ao = (vf->width + 255u) & ~255u,
              pitch_bent = (vf->width * 2u + 255u) & ~255u;
    de_gpu_texture_desc td = {.width = tw,
                              .height = th,
                              .format = DE_FMT_RGBA16_FLOAT,
                              .usage = DE_USAGE_STORAGE,
                              .name = "gtao.raw"};
    de_rg_resource raw = de_rg_create_texture(g, &td);
    td.name = "gtao.spatial";
    de_rg_resource spatial = de_rg_create_texture(g, &td);
    td.width = vf->width;
    td.height = vf->height;
    td.format = DE_FMT_R8_UNORM;
    td.usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC;
    td.name = "gtao.ao";
    de_rg_resource output = de_rg_create_texture(g, &td), bent = {0};
    if (cfg.bent) {
        td.format = DE_FMT_RG8_UNORM;
        td.name = "gtao.bent";
        bent = de_rg_create_texture(g, &td);
    }
    u64 bent_offset = ((u64)pitch_ao * vf->height + 511u) & ~(u64)511u;
    de_rg_resource packed = de_rg_create_buffer(
        g,
        &(de_gpu_buffer_desc){.size = bent_offset + (cfg.bent ? (u64)pitch_bent * vf->height : 0),
                              .stride = 4,
                              .memory = DE_MEM_GPU_ONLY,
                              .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                              .name = "gtao.packed"});
    if (!raw.id || !spatial.id || !output.id || !packed.id || (cfg.bent && !bent.id) ||
        bent_offset > UINT32_MAX)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GTAO: graph resources");
    p[0] = (gtao_pass){
        .s = s,
        .v = v,
        .depth = vc->depth,
        .normal = vc->gbuffer[DE__GB1],
        .motion = vc->gbuffer[DE__GB_MOTION],
        .raw = raw,
        .spatial = spatial,
        .packed = packed,
        .output = output,
        .bent = bent,
        .previous = de_rg_import_texture(g, v->history[v->last], v->access[v->last]),
        .previous_surface = de_rg_import_texture(g, v->surface[v->last], v->access[v->last]),
        .history = de_rg_import_texture(g, v->history[write], v->access[write]),
        .history_surface = de_rg_import_texture(g, v->surface[write], v->access[write]),
        .pc = {.scene = vc->scene_views_bindless,
               .slot = vc->scene_view_slot,
               .width = vf->width,
               .height = vf->height,
               .tw = tw,
               .th = th,
               .slices = slices,
               .steps = steps,
               .frame = (u32)de__rt_sample_frame(s->renderer, vf->frame_index),
               .pitch_ao = pitch_ao,
               .pitch_bent = pitch_bent,
               .bent_offset = (u32)bent_offset,
               .bent = cfg.bent,
               .history_valid = history,
               .radius = cfg.radius,
               .falloff = cfg.falloff,
               .thickness = cfg.thickness,
               .history_weight = .96875f}};
    const char *names[] = {"gtao.raw", "gtao.spatial", "gtao.temporal", "gtao.pack",
                           "gtao.publish"};
    for (u32 i = 0; i < 5; ++i) {
        if (i)
            p[i] = p[0];
        p[i].kind = i;
        de_rg_pass *pass = de_rg_add_pass(g, names[i], DE_QUEUE_GRAPHICS, execute, &p[i]);
        if (!pass)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GTAO: graph pass");
        if (i < 4) {
            de_rg_read(pass, vc->scene_views, DE_ACCESS_SHADER_READ);
            de_rg_read(pass, p[i].depth, DE_ACCESS_SHADER_READ);
            de_rg_read(pass, p[i].normal, DE_ACCESS_SHADER_READ);
        }
        if (i == 0)
            de_rg_write(pass, raw, DE_ACCESS_SHADER_WRITE);
        if (i == 1) {
            de_rg_read(pass, raw, DE_ACCESS_SHADER_READ);
            de_rg_write(pass, raw, DE_ACCESS_SHADER_WRITE);
            de_rg_write(pass, spatial, DE_ACCESS_SHADER_WRITE);
        }
        if (i == 2) {
            de_rg_read(pass, spatial, DE_ACCESS_SHADER_READ);
            de_rg_write(pass, spatial, DE_ACCESS_SHADER_WRITE);
            de_rg_read(pass, p[i].motion, DE_ACCESS_SHADER_READ);
            de_rg_read(pass, p[i].previous, DE_ACCESS_SHADER_READ);
            de_rg_write(pass, p[i].previous, DE_ACCESS_SHADER_WRITE);
            de_rg_read(pass, p[i].previous_surface, DE_ACCESS_SHADER_READ);
            de_rg_write(pass, p[i].previous_surface, DE_ACCESS_SHADER_WRITE);
            de_rg_write(pass, p[i].history, DE_ACCESS_SHADER_WRITE);
            de_rg_write(pass, p[i].history_surface, DE_ACCESS_SHADER_WRITE);
        }
        if (i == 3) {
            de_rg_read(pass, p[i].history, DE_ACCESS_SHADER_READ);
            de_rg_write(pass, p[i].history, DE_ACCESS_SHADER_WRITE);
            de_rg_read(pass, p[i].history_surface, DE_ACCESS_SHADER_READ);
            de_rg_write(pass, p[i].history_surface, DE_ACCESS_SHADER_WRITE);
            de_rg_write(pass, packed, DE_ACCESS_SHADER_WRITE);
        }
        if (i == 4) {
            de_rg_read(pass, packed, DE_ACCESS_COPY_SRC);
            de_rg_write(pass, output, DE_ACCESS_COPY_DST);
            if (bent.id)
                de_rg_write(pass, bent, DE_ACCESS_COPY_DST);
        }
    }
    if (g->oom || g->error_count)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GTAO: incomplete graph");
    de_rg_export(g, p[0].history, GTAO_ACCESS);
    de_rg_export(g, p[0].history_surface, GTAO_ACCESS);
    de_rg_export(g, p[0].previous, GTAO_ACCESS);
    de_rg_export(g, p[0].previous_surface, GTAO_ACCESS);
    inputs->ao = output;
    inputs->ao_gtao = true;
    inputs->ao_bent = bent;
    v->pending = g;
    v->pending_frame = vf->frame_index;
    v->pending_config = cfg;
    v->pending_quality = vf->settings->ao;
    v->pending_view_proj = vf->view_proj;
    v->info = (de__gtao_info){.frame = vf->frame_index,
                              .width = vf->width,
                              .height = vf->height,
                              .trace_width = tw,
                              .trace_height = th,
                              .slices = slices,
                              .steps = steps,
                              .history_valid = history};
    return DE_OK;
}

static void stage(de__view_frame *vf, void *user) {
    de__gtao *s = user;
    if (de__rtao_published(s->renderer->rtao, vf->view, vf->graph, vf->frame_index))
        return;
    if (!s->vis->enabled || !s->kernels[0].pipeline.id)
        return;
    const de__vis_context *vc = de__vis_frame_context(s->vis, vf->view, vf->graph, vf->frame_index);
    de__gtao_config config = {.radius = (f32)de_cvar_float(s->radius),
                              .falloff = (f32)de_cvar_float(s->falloff),
                              .thickness = (f32)de_cvar_float(s->thickness),
                              .bent = de_cvar_int(s->bent) != 0};
    u32 slot = (vf->view.id & 255u) - 1u;
    de_result result = de__gtao_build(
        s, vf, vc, &config, de__lighting_view_inputs(de__views_lighting(s->renderer), slot));
    if (result != DE_OK)
        vf->graph->oom = true;
}
de_result de__gtao_create_ex(de_gpu_device *dev, const de_allocator *alloc, de__vis_shared *vis,
                             de__gtao **out) {
    if (!dev || !alloc || !vis || !out)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "GTAO: create arguments");
    *out = NULL;
    de__gtao *s = DE_NEW(alloc, de__gtao, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "GTAO: state");
    memset(s, 0, sizeof *s);
    s->dev = dev;
    s->alloc = alloc;
    s->vis = vis;
    const char *files[] = {"gtao", "gtao_filter", "gtao_filter", "gtao_filter"};
    const char *entries[] = {"cs_gtao", "cs_gtao_spatial", "cs_gtao_temporal", "cs_gtao_pack"};
    const u32 group[] = {8, 8, 1};
    de_result result = DE_OK;
    for (u32 i = 0; i < 4 && vis->enabled && result == DE_OK; ++i) {
        de__engine_shader_code code;
        result = de__vis_shader_load(vis, files[i], &entries[i], 1, &code);
        if (result == DE_OK) {
            result = de__vis_kernel_create(vis, &code, 0, entries[i], group,
                                           sizeof(de__gtao_params), &s->kernels[i]);
            de__engine_shader_code_free(&code);
        }
    }
    if (result != DE_OK) {
        de__gtao_destroy(s);
        return result;
    }
    *out = s;
    return DE_OK;
}
de_result de__gtao_create(de_renderer *r, de__gtao **out) {
    de_result result = de__gtao_create_ex(r->dev, r->alloc, de__views_vis(r), out);
    if (result != DE_OK)
        return result;
    de__gtao *s = *out;
    s->renderer = r;
    s->radius = de_cvar_register(&(de_cvar_desc){
        .name = "r.ao_radius", .type = DE_CVAR_FLOAT, .default_number = 1, .min = .01, .max = 100});
    s->falloff = de_cvar_register(&(de_cvar_desc){
        .name = "r.ao_falloff", .type = DE_CVAR_FLOAT, .default_number = .8, .min = 0, .max = .99});
    s->thickness = de_cvar_register(&(de_cvar_desc){
        .name = "r.ao_thickness", .type = DE_CVAR_FLOAT, .default_number = .2, .min = 0, .max = 1});
    s->bent = de_cvar_register(&(de_cvar_desc){
        .name = "r.ao_bent_normals", .type = DE_CVAR_INT, .default_number = 0, .min = 0, .max = 1});
    result = de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, -30, "gtao", stage, s);
    if (result != DE_OK) {
        de__gtao_destroy(s);
        *out = NULL;
    }
    return result;
}
void de__gtao_destroy(de__gtao *s) {
    if (!s)
        return;
    if (s->renderer)
        de__view_unregister_stage(s->renderer, stage, s);
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        release_view(s, &s->views[i]);
    for (u32 i = 0; i < 4; ++i)
        de__vis_kernel_destroy(s->vis, &s->kernels[i]);
    DE_FREE(s->alloc, s, sizeof *s);
}
void de__gtao_complete(de__gtao *s, de_view view, const de_rg *graph, bool attempted,
                       bool success) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || !view.id || slot >= DE__VIEW_MAX)
        return;
    gtao_view *v = &s->views[slot];
    if (v->view.id != view.id || v->pending != graph)
        return;
    if (success) {
        v->last ^= 1u;
        v->frame = v->pending_frame;
        v->valid = true;
        v->access[0] = v->access[1] = GTAO_ACCESS;
        v->config = v->pending_config;
        v->quality = v->pending_quality;
        v->view_proj = v->pending_view_proj;
    } else if (attempted) {
        v->valid = false;
        v->access[0] = v->access[1] = 0;
    }
    v->pending = NULL;
}
bool de__gtao_get(const de__gtao *s, de_view view, de__gtao_info *out) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || !view.id || slot >= DE__VIEW_MAX || !out)
        return false;
    const gtao_view *v = &s->views[slot];
    if (v->view.id != view.id)
        return false;
    *out = v->info;
    out->dispatches = de_atomic_load_u32(&v->dispatches, DE_ATOMIC_RELAXED);
    out->history = v->history[v->last];
    out->surface = v->surface[v->last];
    return true;
}

void de__gtao_quality(de_quality quality, u32 width, u32 height, u32 *tw, u32 *th, u32 *slices,
                      u32 *steps) {
    *tw = quality <= DE_QUALITY_MEDIUM ? (width + 1u) / 2u : width;
    *th = quality <= DE_QUALITY_MEDIUM ? (height + 1u) / 2u : height;
    *slices = quality <= DE_QUALITY_MEDIUM ? 2u : quality == DE_QUALITY_HIGH ? 3u : 4u;
    *steps = quality == DE_QUALITY_LOW ? 4u : quality == DE_QUALITY_ULTRA ? 12u : 8u;
    if (quality == DE_QUALITY_OFF)
        *slices = *steps = 0;
}
