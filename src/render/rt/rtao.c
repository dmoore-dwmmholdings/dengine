/* DENG-47: raw geometric AO; lighting owns material/min/multibounce composition. */
#include "render/rt/rtao.h"
#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/passes/vis/gbuffer.h"
#include "render/shaders/engine_shaders.h"
#include <string.h>

typedef struct ao_params {
    u32 scene, slot, depth, normal;
    u32 rays, hits, raw, source;
    u32 output, packed, width, height;
    u32 tw, th, scale, spp;
    u32 frame, pitch, distance_pitch, distance_offset;
    f32 radius;
    u32 reserved[3];
} ao_params;
DE_STATIC_ASSERT(sizeof(ao_params) == 96, "RtaoParams is96 bytes");
typedef struct ao_view {
    de_view handle;
    de_gpu_buffer rays, hits, packed;
    de_gpu_texture raw, spatial, ao, distance;
    u32 width, height, tw, th, capacity;
    const de_rg *graph;
    de__rtao_info info;
} ao_view;
struct de__rtao {
    de_renderer *r;
    de_cvar radius;
    de_gpu_pipeline generate, resolve, filter, pack;
    ao_view views[32];
};
typedef struct ao_pass {
    de__rtao *s;
    ao_view *view;
    de__rt_context context;
    ao_params pc;
    de_rg_resource depth, normal, scene;
    u32 kind;
} ao_pass;
static void release(de__rtao *s, ao_view *v) {
    de_gpu_device *d = s->r->dev;
    de_gpu_buffer_destroy(d, v->rays);
    de_gpu_buffer_destroy(d, v->hits);
    de_gpu_buffer_destroy(d, v->packed);
    de_gpu_texture_destroy(d, v->raw);
    de_gpu_texture_destroy(d, v->spatial);
    de_gpu_texture_destroy(d, v->ao);
    de_gpu_texture_destroy(d, v->distance);
    memset(v, 0, sizeof *v);
}
static void stage(de__view_frame *, void *);
void de__rtao_destroy(de__rtao *s) {
    if (!s)
        return;
    de__view_unregister_stage(s->r, stage, s);
    for (u32 i = 0; i < 32; ++i)
        release(s, &s->views[i]);
    de_gpu_pipeline_destroy(s->r->dev, s->generate);
    de_gpu_pipeline_destroy(s->r->dev, s->resolve);
    de_gpu_pipeline_destroy(s->r->dev, s->filter);
    de_gpu_pipeline_destroy(s->r->dev, s->pack);
    DE_FREE(s->r->alloc, s, sizeof *s);
}
de_result de__rtao_create(de_renderer *r, de__rtao **out) {
    *out = NULL;
    de__rtao *s = DE_ALLOC(r->alloc, sizeof *s, 16, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RTAO owner");
    memset(s, 0, sizeof *s);
    s->r = r;
    s->radius = de_cvar_register(&(de_cvar_desc){.name = "r.rt.ao.radius",
                                                 .type = DE_CVAR_FLOAT,
                                                 .default_number = 1.5,
                                                 .min = .01,
                                                 .max = 100});
    if (!s->radius.id) {
        de__rtao_destroy(s);
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "RTAO radius registration");
    }
    if (r->caps.ray_query && r->caps.max_bindless_accels) {
        const char *entries[] = {"cs_generate", "cs_resolve", "cs_filter", "cs_pack"};
        de__engine_shader_code code;
        char why[512] = {0};
        de_result result =
            de__engine_shader_load(r->dev, r->alloc, "rtao", 0, entries, 4, &code, why, sizeof why);
        if (result != DE_OK) {
            DE_WARN(r->log, "RTAO shaders unavailable; using GTAO: %s", why);
            de_error_clear();
            goto register_stage;
        }
        de_gpu_pipeline *pipelines[] = {&s->generate, &s->resolve, &s->filter, &s->pack};
        for (u32 i = 0; i < 4; ++i)
            *pipelines[i] = de_gpu_pipeline_create(
                r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                .cs = code.entries[i].blob,
                                                .push_constant_bytes = sizeof(ao_params),
                                                .threadgroup_size = {8, 8, 1},
                                                .name = entries[i]});
        de__engine_shader_code_free(&code);
        if (!s->generate.id || !s->resolve.id || !s->filter.id || !s->pack.id) {
            for (u32 i = 0; i < 4; ++i) {
                de_gpu_pipeline_destroy(r->dev, *pipelines[i]);
                *pipelines[i] = (de_gpu_pipeline){0};
            }
            DE_WARN(r->log, "RTAO pipelines unavailable; using GTAO");
            de_error_clear();
        }
    }
register_stage:;
    de_result result = de__view_register_stage(r, DE__VIEW_SLOT_LIGHTING, -31, "rtao", stage, s);
    if (result != DE_OK) {
        de__rtao_destroy(s);
        return result;
    }
    *out = s;
    return DE_OK;
}
static bool reserve(de__rtao *s, ao_view *v, u32 width, u32 height, u32 tw, u32 th, u32 spp) {
    de_gpu_device *d = s->r->dev;
    if (v->width != width || v->height != height || v->tw != tw || v->th != th) {
        release(s, v);
        v->width = width;
        v->height = height;
        v->tw = tw;
        v->th = th;
        de_gpu_texture_desc td = {.width = tw,
                                  .height = th,
                                  .format = DE_FMT_RGBA16_FLOAT,
                                  .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                  .name = "RTAO raw"};
        v->raw = de_gpu_texture_create(d, &td);
        v->spatial = de_gpu_texture_create(d, &td);
        td.width = width;
        td.height = height;
        td.usage = DE_USAGE_SAMPLED | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC;
        td.format = DE_FMT_R8_UNORM;
        td.name = "RTAO geometric visibility";
        v->ao = de_gpu_texture_create(d, &td);
        td.format = DE_FMT_R16_FLOAT;
        td.name = "RTAO hit distance";
        v->distance = de_gpu_texture_create(d, &td);
        const u64 offset = ((u64)((width + 255) & ~255u) * height + 511) & ~511ull;
        v->packed = de_gpu_buffer_create(
            d, &(de_gpu_buffer_desc){.size = offset + (u64)((width * 2 + 255) & ~255u) * height,
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                     .name = "RTAO narrow format publication"});
    }
    const u64 count = (u64)tw * th * spp;
    if (count > UINT32_MAX)
        return false;
    if (count > v->capacity) {
        de_gpu_buffer_destroy(d, v->rays);
        de_gpu_buffer_destroy(d, v->hits);
        de_gpu_buffer_desc bd = {
            .size = count * 32, .stride = 32, .usage = DE_USAGE_STORAGE, .name = "RTAO rays"};
        v->rays = de_gpu_buffer_create(d, &bd);
        bd.name = "RTAO hits";
        v->hits = de_gpu_buffer_create(d, &bd);
        v->capacity = (u32)count;
    }
    return v->rays.id && v->hits.id && v->packed.id && v->raw.id && v->spatial.id && v->ao.id &&
           v->distance.id;
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    ao_pass *p = user;
    ao_view *v = p->view;
    de_gpu_device *d = p->s->r->dev;
    ao_params pc = p->pc;
    if (p->kind == 3) {
        de_gpu_cmd_copy_buffer_to_texture(cmd, v->ao, 0, 0, v->packed, 0, pc.pitch);
        de_gpu_cmd_copy_buffer_to_texture(cmd, v->distance, 0, 0, v->packed, pc.distance_offset,
                                          pc.distance_pitch);
        return;
    }
    pc.depth = de_gpu_texture_bindless(d, de_rg_texture(pass, p->depth), 0, false);
    pc.normal = de_gpu_texture_bindless(d, de_rg_texture(pass, p->normal), 0, false);
    pc.rays = de_gpu_buffer_bindless(d, v->rays);
    pc.hits = de_gpu_buffer_bindless(d, v->hits);
    pc.raw = de_gpu_texture_bindless(d, v->raw, 0, true);
    if (p->kind == 0) {
        de_gpu_cmd_set_pipeline(cmd, p->s->generate);
        de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
        de_gpu_cmd_dispatch(cmd, (pc.tw + 7) / 8, (pc.th + 7) / 8, 1);
        de_gpu_barrier b = {
            .buffer = v->rays, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_SHADER_READ};
        de_gpu_cmd_barriers(cmd, &b, 1);
        de__rt_trace(p->s->r, cmd, &p->context, v->rays, v->hits, pc.tw * pc.th * pc.spp,
                     DE__RT_ACCEPT_FIRST_HIT);
        b.buffer = v->hits;
        de_gpu_cmd_barriers(cmd, &b, 1);
        de_gpu_cmd_set_pipeline(cmd, p->s->resolve);
        de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
        de_gpu_cmd_dispatch(cmd, (pc.tw + 7) / 8, (pc.th + 7) / 8, 1);
    } else {
        pc.source = de_gpu_texture_bindless(d, p->kind == 1 ? v->raw : v->spatial, 0, false);
        pc.output = de_gpu_texture_bindless(d, v->spatial, 0, true);
        pc.packed = de_gpu_buffer_bindless(d, v->packed);
        de_gpu_cmd_set_pipeline(cmd, p->kind == 1 ? p->s->filter : p->s->pack);
        de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
        de_gpu_cmd_dispatch(cmd, ((p->kind == 1 ? pc.tw : (pc.width + 3) / 4) + 7) / 8,
                            ((p->kind == 1 ? pc.th : pc.height) + 7) / 8, 1);
    }
}
/* DENG-69 replaces this temporary spatial filter, independently of ray generation/publication. */
static de_result spatial(de_rg *g, ao_pass *p, de_rg_resource raw, de_rg_resource output) {
    de_rg_pass *pass = de_rg_add_pass(g, "RT.AO.Spatial", DE_QUEUE_GRAPHICS, execute, p);
    if (!pass)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RTAO filter pass");
    de_rg_read(pass, p->scene, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->depth, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->normal, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, raw, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, output, DE_ACCESS_SHADER_WRITE);
    return DE_OK;
}
static de_result build(de__rtao *s, de__view_frame *vf, const de__vis_context *vc) {
    u32 slot = (vf->view.id & 255u) - 1u;
    if (slot >= 32)
        return DE_OK;
    ao_view *v = &s->views[slot];
    v->graph = NULL;
    if (!vf->settings || vf->probe_capture || vf->settings->tier != DE_TIER_RT ||
        !vf->settings->rt_ao || vf->settings->ao == DE_QUALITY_OFF || !s->generate.id || !vc ||
        !vc->gbuffer[DE__GB1].id)
        return DE_OK;
    de__rt_context *rt = de__rt_context_get(s->r, vf->view, vf->graph, vf->frame_index);
    if (!rt)
        return DE_OK;
    u32 scale = vf->settings->ao == DE_QUALITY_LOW ? 2 : 1, tw = (vf->width + scale - 1) / scale,
        th = (vf->height + scale - 1) / scale;
    u32 spp = vf->settings->ao == DE_QUALITY_ULTRA ? 2 : 1;
    if (!reserve(s, v, vf->width, vf->height, tw, th, spp)) {
        release(s, v);
        DE_WARN(s->r->log, "RTAO capacity unavailable; using GTAO");
        return DE_OK;
    }
    v->handle = vf->view;
    de_rg *g = vf->graph;
    de_rg_resource rays = de_rg_import_buffer(g, v->rays, DE_ACCESS_SHADER_READ),
                   hits = de_rg_import_buffer(g, v->hits, DE_ACCESS_SHADER_READ),
                   raw = de_rg_import_texture(g, v->raw, DE_ACCESS_SHADER_READ),
                   filtered = de_rg_import_texture(g, v->spatial, DE_ACCESS_SHADER_READ),
                   packed = de_rg_import_buffer(g, v->packed, DE_ACCESS_COPY_SRC),
                   ao = de_rg_import_texture(g, v->ao, DE_ACCESS_SHADER_READ),
                   distance = de_rg_import_texture(g, v->distance, DE_ACCESS_SHADER_READ);
    ao_pass *p = de_arena_push_zero(g->arena, 4 * sizeof *p, 16);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RTAO graph data");
    p[0] = (ao_pass){.s = s,
                     .view = v,
                     .context = *rt,
                     .scene = vc->scene_views,
                     .depth = vc->depth,
                     .normal = vc->gbuffer[DE__GB1],
                     .pc = {.scene = vc->scene_views_bindless,
                            .slot = vc->scene_view_slot,
                            .width = vf->width,
                            .height = vf->height,
                            .tw = tw,
                            .th = th,
                            .scale = scale,
                            .spp = spp,
                            .frame = (u32)vf->frame_index,
                            .pitch = (vf->width + 255) & ~255u,
                            .distance_pitch = (vf->width * 2 + 255) & ~255u,
                            .radius = (f32)de_cvar_float(s->radius)}};
    p[0].pc.distance_offset = (u32)(((u64)p[0].pc.pitch * vf->height + 511) & ~511ull);
    for (u32 i = 1; i < 4; ++i) {
        p[i] = p[0];
        p[i].kind = i;
    }
    de_rg_pass *pass = de_rg_add_pass(g, "RT.AO", DE_QUEUE_GRAPHICS, execute, p);
    if (!pass)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RTAO trace pass");
    de_rg_read(pass, rt->dependency, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, vc->depth, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->normal, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, rays, DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass, hits, DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass, raw, DE_ACCESS_SHADER_WRITE);
    DE_TRY(spatial(g, &p[1], raw, filtered));
    pass = de_rg_add_pass(g, "RT.AO.Pack", DE_QUEUE_GRAPHICS, execute, &p[2]);
    if (!pass)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RTAO packing pass");
    de_rg_read(pass, vc->depth, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, p->normal, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, filtered, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, packed, DE_ACCESS_SHADER_WRITE);
    pass = de_rg_add_pass(g, "RT.AO.Publish", DE_QUEUE_GRAPHICS, execute, &p[3]);
    if (!pass)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RTAO publication pass");
    de_rg_read(pass, packed, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, ao, DE_ACCESS_COPY_DST);
    de_rg_write(pass, distance, DE_ACCESS_COPY_DST);
    de_rg_export(g, rays, DE_ACCESS_SHADER_READ);
    de_rg_export(g, hits, DE_ACCESS_SHADER_READ);
    de_rg_export(g, raw, DE_ACCESS_SHADER_READ);
    de_rg_export(g, filtered, DE_ACCESS_SHADER_READ);
    de_rg_export(g, packed, DE_ACCESS_COPY_SRC);
    de_rg_export(g, ao, DE_ACCESS_SHADER_READ);
    de_rg_export(g, distance, DE_ACCESS_SHADER_READ);
    de__lighting_inputs *inputs = de__lighting_view_inputs(de__views_lighting(s->r), slot);
    inputs->ao = ao;
    inputs->ao_gtao = true;
    inputs->ao_bent = (de_rg_resource){0};
    v->graph = g;
    v->info =
        (de__rtao_info){v->ao, v->distance, v->raw,          vf->width,          vf->height,  tw,
                        th,    spp,         vf->frame_index, (u64)tw * th * spp, p->pc.radius};
    s->r->stats.rays_traced += (u64)tw * th * spp;
    return DE_OK;
}
static void stage(de__view_frame *vf, void *user) {
    de__rtao *s = user;
    const de__vis_context *vc =
        de__vis_frame_context(de__views_vis(s->r), vf->view, vf->graph, vf->frame_index);
    if (build(s, vf, vc) != DE_OK)
        vf->graph->oom = true;
}
bool de__rtao_published(const de__rtao *s, de_view view, const de_rg *g, u64 frame) {
    u32 slot = (view.id & 255u) - 1u;
    return s && slot < 32 && s->views[slot].handle.id == view.id && s->views[slot].graph == g &&
           s->views[slot].info.frame == frame;
}
bool de__rtao_get(const de__rtao *s, de_view view, de__rtao_info *out) {
    u32 slot = (view.id & 255u) - 1u;
    if (!s || !out || slot >= 32 || s->views[slot].handle.id != view.id || !s->views[slot].graph)
        return false;
    *out = s->views[slot].info;
    return true;
}
