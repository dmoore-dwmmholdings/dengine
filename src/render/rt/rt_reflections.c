#include "render/denoise/reblur_internal.h"
/* DENG-48: shared closest-hit shading through the production alpha-aware batch query. */
#include "render/rt/rt_reflections.h"
#include "render/graph/rg_internal.h"
#include "render/passes/vis/gbuffer.h"
#include "render/shaders/engine_shaders.h"
#include <string.h>
typedef struct reflection_params {
    u32 scene, slot, depth, gb0;
    u32 gb1, gb2, ids, visible;
    u32 visible_count, ids_pitch, query, view;
    u32 entry, lights, dirs, grid;
    u32 list, counters, ltc1, ltc2;
    u32 sampler, rays, hits, sun_rays;
    u32 sun_hits, output, distance, width;
    u32 height, frame, scale, samples;
    f32 max_roughness;
    u32 resolved, source, rt_locals;
} reflection_params;
DE_STATIC_ASSERT(sizeof(reflection_params) == 144, "ReflectionParams layout");
typedef struct reflection_view {
    de_view handle;
    de_gpu_buffer rays, hits, sun_rays, sun_hits, params;
    void *map;
    de_gpu_texture raw, output, distance;
    u32 width, height;
    bool valid;
    de__rt_reflections_info info;
} reflection_view;
struct de__rt_reflections {
    de__radiance_denoiser *denoiser;
    de_renderer *r;
    de_gpu_pipeline kernels[5];
    de_cvar max_roughness;
    u32 reference;
    reflection_view views[32];
};
typedef struct reflection_pass {
    de__rt_reflections *s;
    reflection_view *v;
    de__rt_context context;
    reflection_params params;
    de_rg_resource reads[14];
    u32 kind, local_lights;
} reflection_pass;
static void release(de__rt_reflections *s, reflection_view *v) {
    de_gpu_buffer buffers[] = {v->rays, v->hits, v->sun_rays, v->sun_hits, v->params};
    for (u32 i = 0; i < 5; ++i)
        de_gpu_buffer_destroy(s->r->dev, buffers[i]);
    de_gpu_texture_destroy(s->r->dev, v->raw);
    de_gpu_texture_destroy(s->r->dev, v->output);
    de_gpu_texture_destroy(s->r->dev, v->distance);
    memset(v, 0, sizeof *v);
}
void de__rt_reflections_destroy(de__rt_reflections *s) {
    if (!s)
        return;
    de__radiance_denoiser_destroy(s->denoiser);
    for (u32 i = 0; i < 32; ++i)
        release(s, &s->views[i]);
    for (u32 i = 0; i < 5; ++i)
        de_gpu_pipeline_destroy(s->r->dev, s->kernels[i]);
    DE_FREE(s->r->alloc, s, sizeof *s);
}
de_result de__rt_reflections_create(de_renderer *r, de__rt_reflections **out) {
    *out = NULL;
    de__rt_reflections *s = DE_ALLOC(r->alloc, sizeof *s, 16, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT reflections owner");
    memset(s, 0, sizeof *s);
    s->r = r;
    de_result denoiser_result = de__radiance_denoiser_create(r, &s->denoiser);
    if (denoiser_result != DE_OK) {
        DE_FREE(r->alloc, s, sizeof *s);
        return denoiser_result;
    }
    s->max_roughness = de_cvar_register(&(de_cvar_desc){.name = "r.rt.reflections.max_roughness",
                                                        .type = DE_CVAR_FLOAT,
                                                        .default_number = .6,
                                                        .min = .05,
                                                        .max = 1});
    if (!s->max_roughness.id) {
        de__rt_reflections_destroy(s);
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "RT reflections cvar");
    }
    if (r->caps.ray_query && r->caps.max_bindless_accels) {
        const char *names[] = {"cs_generate", "cs_shade", "cs_sun_ray", "cs_sun_add", "cs_resolve"};
        de__engine_shader_code code;
        char why[512] = {0};
        de_result res = de__engine_shader_load(r->dev, r->alloc, "rt_reflections", 0, names, 5,
                                               &code, why, sizeof why);
        if (res != DE_OK) {
            de__rt_reflections_destroy(s);
            return DE_FAIL(res, "RT reflection shaders: %s", why);
        }
        for (u32 i = 0; i < 5; ++i)
            s->kernels[i] = de_gpu_pipeline_create(
                r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                .cs = code.entries[i].blob,
                                                .push_constant_bytes = 16,
                                                .threadgroup_size = {8, 8, 1},
                                                .name = names[i]});
        de__engine_shader_code_free(&code);
        for (u32 i = 0; i < 5; ++i)
            if (!s->kernels[i].id) {
                de__rt_reflections_destroy(s);
                return DE_FAIL(DE_ERR_UNSUPPORTED, "RT reflection pipeline");
            }
    }
    *out = s;
    return DE_OK;
}
static bool reserve(de__rt_reflections *s, reflection_view *v, u32 width, u32 height) {
    if (v->width == width && v->height == height)
        return true;
    release(s, v);
    de_gpu_device *d = s->r->dev;
    u64 count = (u64)width * height;
    if (count > UINT32_MAX)
        return false;
    de_gpu_buffer *buffers[] = {&v->rays, &v->hits, &v->sun_rays, &v->sun_hits};
    for (u32 i = 0; i < 4; ++i)
        *buffers[i] = de_gpu_buffer_create(
            d, &(de_gpu_buffer_desc){.size = count * 32,
                                     .stride = 32,
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                     .name = "RT reflection typed query buffer"});
    v->params = de_gpu_buffer_create(
        d, &(de_gpu_buffer_desc){.size = 8 * sizeof(reflection_params),
                                 .stride = sizeof(reflection_params),
                                 .usage = DE_USAGE_STORAGE,
                                 .memory = DE_MEM_UPLOAD,
                                 .name = "RT reflection immutable parameters"});
    v->map = de_gpu_buffer_map(d, v->params);
    de_gpu_texture_desc td = {.width = width,
                              .height = height,
                              .format = DE_FMT_RGBA16_FLOAT,
                              .usage = DE_USAGE_SAMPLED | DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                              .name = "RT reflection outgoing specular"};
    td.format = DE_FMT_RGBA32_FLOAT;
    v->raw = de_gpu_texture_create(d, &td);
    td.format = DE_FMT_RGBA16_FLOAT;
    v->output = de_gpu_texture_create(d, &td);
    td.name = "RT reflection hit distance";
    td.format = DE_FMT_RGBA16_FLOAT;
    v->distance = de_gpu_texture_create(d, &td);
    if (!v->map || !v->rays.id || !v->hits.id || !v->sun_rays.id || !v->sun_hits.id || !v->raw.id ||
        !v->output.id || !v->distance.id) {
        release(s, v);
        return false;
    }
    v->width = width;
    v->height = height;
    return true;
}
static void dispatch(de_gpu_cmd *cmd, reflection_pass *p, u32 kernel, u32 *push) {
    de_gpu_cmd_set_pipeline(cmd, p->s->kernels[kernel]);
    de_gpu_cmd_push_constants(cmd, push, 16);
    de_gpu_cmd_dispatch(cmd, (p->params.width + 7) / 8, (p->params.height + 7) / 8, 1);
}
static void barrier(de_gpu_cmd *cmd, de_gpu_buffer buffer, u32 before, u32 after) {
    de_gpu_barrier b = {.buffer = buffer, .before = before, .after = after};
    de_gpu_cmd_barriers(cmd, &b, 1);
}
static void image_barrier(de_gpu_cmd *cmd, de_gpu_texture texture) {
    de_gpu_barrier b = {.texture = texture,
                        .before = DE_ACCESS_SHADER_WRITE,
                        .after = DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE};
    de_gpu_cmd_barriers(cmd, &b, 1);
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    reflection_pass *p = user;
    reflection_view *v = p->v;
    de_gpu_device *d = p->s->r->dev;
    reflection_params pc = p->params;
    if (p->kind) {
        u32 push[] = {de_gpu_buffer_bindless(d, v->params), (u32)(p->context.frame % 8), 0, 0};
        dispatch(cmd, p, 4, push);
        return;
    }
    u32 *tex[] = {&pc.depth, &pc.gb0, &pc.gb1, &pc.gb2};
    for (u32 i = 0; i < 4; ++i)
        *tex[i] = de_gpu_texture_bindless(d, de_rg_texture(pass, p->reads[i]), 0, false);
    u32 *buf[] = {&pc.ids, &pc.visible, &pc.lights, &pc.dirs, &pc.grid, &pc.list, &pc.counters};
    for (u32 i = 0; i < 7; ++i)
        *buf[i] = de_gpu_buffer_bindless(d, de_rg_buffer(pass, p->reads[i + 4]));
    pc.ltc1 = de_gpu_texture_bindless(d, de_rg_texture(pass, p->reads[11]), 0, false);
    pc.ltc2 = de_gpu_texture_bindless(d, de_rg_texture(pass, p->reads[12]), 0, false);
    pc.query = de_gpu_buffer_bindless(d, p->context.query);
    pc.rays = de_gpu_buffer_bindless(d, v->rays);
    pc.hits = de_gpu_buffer_bindless(d, v->hits);
    pc.sun_rays = de_gpu_buffer_bindless(d, v->sun_rays);
    pc.sun_hits = de_gpu_buffer_bindless(d, v->sun_hits);
    pc.output = de_gpu_texture_bindless(d, v->raw, 0, true);
    pc.distance = de_gpu_texture_bindless(d, v->distance, 0, true);
    pc.resolved = de_gpu_texture_bindless(d, v->output, 0, true);
    pc.source = de_gpu_texture_bindless(d, v->raw, 0, false);
    u32 entry = (u32)(p->context.frame % 8);
    memcpy((u8 *)v->map + entry * sizeof pc, &pc, sizeof pc);
    u32 push[] = {de_gpu_buffer_bindless(d, v->params), entry, 0, 0};
    u32 count = pc.width * pc.height;
    for (u32 sample = 0; sample < pc.samples; ++sample) {
        push[3] = sample;
        dispatch(cmd, p, 0, push);
        barrier(cmd, v->rays, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        de__rt_trace(p->s->r, cmd, &p->context, v->rays, v->hits, count, 0);
        barrier(cmd, v->hits, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        dispatch(cmd, p, 1, push);
        image_barrier(cmd, v->raw);
        for (u32 sun = 0; sun < 4 + p->local_lights; ++sun) {
            push[2] = sun;
            dispatch(cmd, p, 2, push);
            barrier(cmd, v->sun_rays, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
            de__rt_trace(p->s->r, cmd, &p->context, v->sun_rays, v->sun_hits, count,
                         DE__RT_CAST_SHADOWS | DE__RT_ACCEPT_FIRST_HIT);
            barrier(cmd, v->sun_hits, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
            dispatch(cmd, p, 3, push);
            image_barrier(cmd, v->raw);
            barrier(cmd, v->sun_rays, DE_ACCESS_SHADER_READ, DE_ACCESS_SHADER_WRITE);
            barrier(cmd, v->sun_hits, DE_ACCESS_SHADER_READ, DE_ACCESS_SHADER_WRITE);
        }
        barrier(cmd, v->rays, DE_ACCESS_SHADER_READ, DE_ACCESS_SHADER_WRITE);
        barrier(cmd, v->hits, DE_ACCESS_SHADER_READ, DE_ACCESS_SHADER_WRITE);
    }
}
de_result de__rt_reflections_build(de__rt_reflections *s, de_rg *g, de__vis_context *vc,
                                   de__lighting_context *lc, de__lighting_inputs *inputs) {
    if (!s || !vc || !vc->settings)
        return DE_OK;
    u32 slot = vc->view_slot;
    if (slot >= 32)
        return DE_OK;
    reflection_view *v = &s->views[slot];
    v->valid = false;
    if (vc->settings->tier != DE_TIER_RT || !vc->settings->rt_reflections ||
        vc->settings->reflections == DE_QUALITY_OFF || !s->kernels[0].id)
        return DE_OK;
    de__rt_context *rt = de__rt_context_get(s->r, vc->view, g, vc->frame_index);
    if (!rt)
        return DE_OK;
    if (!reserve(s, v, vc->width, vc->height))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT reflection extent");
    v->handle = vc->view;
    reflection_pass *p = de_arena_push_zero(g->arena, 2 * sizeof *p, 16);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT reflection pass");
    *p = (reflection_pass){
        .s = s,
        .v = v,
        .context = *rt,
        .params = {.scene = vc->scene_views_bindless,
                   .slot = vc->scene_view_slot,
                   .visible_count = vc->visible_capacity + vc->visible_alpha_capacity,
                   .ids_pitch = vc->vis_ids_pitch,
                   .view = lc->constants_bindless,
                   .entry = lc->constants_entry,
                   .sampler = lc->sampler_bindless,
                   .width = vc->width,
                   .height = vc->height,
                   .frame = (u32)de__rt_sample_frame(s->r, vc->frame_index),
                   .scale = vc->settings->reflections <= DE_QUALITY_MEDIUM ? 2 : 1,
                   .samples = s->reference ? s->reference : 1,
                   .rt_locals = vc->settings->reflections >= DE_QUALITY_HIGH ? 1 : 0,
                   .max_roughness = (f32)de_cvar_float(s->max_roughness)},
        .local_lights = vc->settings->reflections >= DE_QUALITY_HIGH
                            ? lc->view.light_slots + lc->view.proxy_capacity
                            : 0,
        .reads = {vc->depth, vc->gbuffer[0], vc->gbuffer[1], vc->gbuffer[2], vc->vis_ids_buffer,
                  vc->visible_meshlets, lc->lights, lc->dir_lights, lc->grid_buf, lc->list,
                  lc->counters, lc->ltc1, lc->ltc2, vc->scene_views}};
    if (p->local_lights > lc->local_capacity)
        p->local_lights = lc->local_capacity;
    de_rg_resource raw = de_rg_import_texture(g, v->raw, DE_ACCESS_SHADER_READ),
                   output = de_rg_import_texture(g, v->output, DE_ACCESS_SHADER_READ),
                   distance = de_rg_import_texture(g, v->distance, DE_ACCESS_SHADER_READ);
    de_rg_pass *pass = de_rg_add_pass(g, "RT.Reflections", DE_QUEUE_GRAPHICS, execute, p);
    if (!pass)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT reflections graph");
    de_rg_read(pass, rt->dependency, DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < 14; ++i)
        if (p->reads[i].id)
            de_rg_read(pass, p->reads[i], DE_ACCESS_SHADER_READ);
    de_rg_resource optional[] = {lc->strip,      lc->probe_records, lc->probe_list,
                                 lc->gi_records, lc->atlas_records, lc->atlas_texture};
    for (u32 i = 0; i < 6; ++i)
        if (optional[i].id)
            de_rg_read(pass, optional[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->probe_texture_count; ++i)
        de_rg_read(pass, lc->probe_textures[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->gi_texture_count; ++i)
        de_rg_read(pass, lc->gi_textures[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->gi_buffer_count; ++i)
        de_rg_read(pass, lc->gi_buffers[i], DE_ACCESS_SHADER_READ);
    de_gpu_buffer buffers[] = {v->rays, v->hits, v->sun_rays, v->sun_hits};
    for (u32 i = 0; i < 4; ++i) {
        de_rg_resource b = de_rg_import_buffer(g, buffers[i], DE_ACCESS_SHADER_READ);
        de_rg_write(pass, b, DE_ACCESS_SHADER_WRITE);
        de_rg_export(g, b, DE_ACCESS_SHADER_READ);
    }
    de_rg_write(pass, raw, DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass, distance, DE_ACCESS_SHADER_WRITE);
    p[1] = p[0];
    p[1].kind = 1;
    pass = de_rg_add_pass(g, "RT.Reflections.Resolve", DE_QUEUE_GRAPHICS, execute, &p[1]);
    if (!pass)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT reflection resolve");
    de_rg_read(pass, raw, DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < 6; ++i)
        de_rg_read(pass, p->reads[i], DE_ACCESS_SHADER_READ);
    de_rg_read(pass, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, output, DE_ACCESS_SHADER_WRITE);
    de_rg_export(g, raw, DE_ACCESS_SHADER_READ);
    de_rg_export(g, output, DE_ACCESS_SHADER_READ);
    de_rg_export(g, distance, DE_ACCESS_SHADER_READ);
    DE_TRY(de__denoise_specular(s->denoiser, g, vc, lc, output, distance, true, s->reference != 0,
                                de_rg_import_buffer(g, v->rays, DE_ACCESS_SHADER_READ),
                                p->params.scale));
    inputs->indirect_specular = output;
    inputs->indirect_specular_opaque = true;
    inputs->indirect_specular_layers = true;
    v->valid = true;
    v->info = (de__rt_reflections_info){
        v->output,       v->distance,
        v->rays,         v->hits,
        vc->frame_index, (u64)vc->width * vc->height * p->params.samples * (5 + p->local_lights),
        vc->width,       vc->height,
        p->params.scale, p->params.samples};
    s->r->stats.rays_traced += v->info.rays_traced;
    return DE_OK;
}
bool de__rt_reflections_get(const de__rt_reflections *s, de_view view,
                            de__rt_reflections_info *out) {
    u32 slot = (view.id & 255) - 1;
    if (!s || !out || slot >= 32 || s->views[slot].handle.id != view.id || !s->views[slot].valid)
        return false;
    *out = s->views[slot].info;
    return true;
}
void de__rt_reflections_reference(de__rt_reflections *s, u32 samples) {
    if (s)
        s->reference = samples > 4096 ? 4096 : samples;
}
