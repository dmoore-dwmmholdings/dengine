/* Metal requires explicit batch IFT queries between the capture and forward composite. */
#include "render/rt/rt_refraction.h"
#include "render/shaders/engine_shaders.h"
#include <string.h>
enum { K_CLEAR, K_GENERATE, K_EXIT, K_SHADE, K_SHADOW_RAY, K_SHADOW_ADD, K_COUNT };
typedef struct refraction_view {
    de_view handle;
    u32 capacity;
    de_gpu_buffer capture, result, path, rays, hits, exits, shadow_rays, shadow_hits, diagnostic;
    de__rt_refraction_info info;
    bool active;
} refraction_view;
struct de__rt_refraction {
    de_renderer *r;
    de_gpu_pipeline kernels[K_COUNT];
    refraction_view views[32];
};
struct de__rt_refraction_frame {
    de__rt_refraction *s;
    refraction_view *v;
    de__rt_context context;
    u32 width, height, frame, events, local_bound;
};
typedef struct refraction_params {
    u32 scene, slot, view, entry;
    u32 query, capture, rays, hits;
    u32 shadow_rays, shadow_hits, result, path;
    u32 width, height, frame, events;
    u32 lights, dirs, grid, list, counters, ltc1, ltc2, sampler;
    u32 light, instance, side, reserved;
} refraction_params;
DE_STATIC_ASSERT(sizeof(refraction_params) == 112, "RefractionParams shader ABI");
static void release(de__rt_refraction *s, refraction_view *v) {
    de_gpu_buffer b[] = {v->capture, v->result,      v->path,        v->rays,      v->hits,
                         v->exits,   v->shadow_rays, v->shadow_hits, v->diagnostic};
    for (u32 i = 0; i < 9; ++i)
        de_gpu_buffer_destroy(s->r->dev, b[i]);
    memset(v, 0, sizeof *v);
}
de_result de__rt_refraction_create(de_renderer *r, de__rt_refraction **out) {
    *out = NULL;
    de__rt_refraction *s = DE_ALLOC(r->alloc, sizeof *s, 16, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "RT refraction owner");
    memset(s, 0, sizeof *s);
    s->r = r;
    if (r->caps.ray_query && r->caps.max_bindless_accels) {
        const char *names[] = {"cs_clear", "cs_generate",   "cs_exit",
                               "cs_shade", "cs_shadow_ray", "cs_shadow_add"};
        de__engine_shader_code code;
        char why[512] = {0};
        de_result result = de__engine_shader_load(r->dev, r->alloc, "rt_refraction", 0, names,
                                                  K_COUNT, &code, why, sizeof why);
        if (result != DE_OK) {
            de__rt_refraction_destroy(s);
            return DE_FAIL(result, "RT refraction shaders: %s", why);
        }
        for (u32 i = 0; i < K_COUNT; ++i)
            s->kernels[i] = de_gpu_pipeline_create(
                r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                .cs = code.entries[i].blob,
                                                .push_constant_bytes = sizeof(refraction_params),
                                                .threadgroup_size = {8, 8, 1},
                                                .name = names[i]});
        de__engine_shader_code_free(&code);
        for (u32 i = 0; i < K_COUNT; ++i)
            if (!s->kernels[i].id) {
                de__rt_refraction_destroy(s);
                return DE_FAIL(DE_ERR_UNSUPPORTED, "RT refraction pipeline");
            }
    }
    *out = s;
    return DE_OK;
}
void de__rt_refraction_destroy(de__rt_refraction *s) {
    if (!s)
        return;
    for (u32 i = 0; i < 32; ++i)
        release(s, &s->views[i]);
    for (u32 i = 0; i < K_COUNT; ++i)
        de_gpu_pipeline_destroy(s->r->dev, s->kernels[i]);
    DE_FREE(s->r->alloc, s, sizeof *s);
}
void de__rt_refraction_reset(de__rt_refraction *s, de_view h) {
    u32 slot = (h.id & 255u) - 1u;
    if (s && slot < 32)
        s->views[slot].active = false;
}
de__rt_refraction_frame *de__rt_refraction_prepare(de__rt_refraction *s, de_rg *g,
                                                   de__vis_context *vc, de__lighting_context *lc) {
    if (!s || !vc || vc->view_slot >= 32)
        return NULL;
    refraction_view *v = &s->views[vc->view_slot];
    v->active = false;
    if (vc->settings->tier != DE_TIER_RT || !vc->settings->rt_refraction || !s->kernels[0].id)
        return NULL;
    de__rt_context *context = de__rt_context_get(s->r, vc->view, g, vc->frame_index);
    if (!context)
        return NULL;
    if (v->handle.id != vc->view.id) {
        release(s, v);
        v->handle = vc->view;
    }
    u64 pixels = (u64)lc->width * lc->height;
    if (!pixels || pixels > UINT32_MAX)
        return NULL;
    if (v->capacity < pixels) {
        release(s, v);
        v->handle = vc->view;
        de_gpu_buffer *b[] = {&v->capture,     &v->result,      &v->path,
                              &v->rays,        &v->hits,        &v->exits,
                              &v->shadow_rays, &v->shadow_hits, &v->diagnostic};
        for (u32 i = 0; i < 9; ++i) {
            u32 stride = i == 0 ? 80 : i < 3 || i == 8 ? 16 : 32;
            *b[i] = de_gpu_buffer_create(
                s->r->dev, &(de_gpu_buffer_desc){.size = i == 8 ? 64 : pixels * stride,
                                                 .stride = stride,
                                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                 .name = "RT refraction retained draw buffers"});
            if (!b[i]->id)
                return NULL;
        }
        v->capacity = (u32)pixels;
    }
    de__rt_refraction_frame *f = de_arena_push_zero(vc->arena, sizeof *f, 16);
    if (!f)
        return NULL;
    *f = (de__rt_refraction_frame){.s = s,
                                   .v = v,
                                   .context = *context,
                                   .width = lc->width,
                                   .height = lc->height,
                                   .frame = (u32)vc->frame_index,
                                   .events = vc->settings->reflections == DE_QUALITY_ULTRA ? 2 : 1,
                                   .local_bound = lc->view.light_slots + lc->view.proxy_capacity};
    if (f->local_bound > lc->local_capacity)
        f->local_bound = lc->local_capacity;
    v->info = (de__rt_refraction_info){.capture = v->capture,
                                       .radiance = v->result,
                                       .path = v->path,
                                       .rays = v->rays,
                                       .hits = v->hits,
                                       .diagnostic = v->diagnostic,
                                       .width = f->width,
                                       .height = f->height,
                                       .events = f->events,
                                       .frame = vc->frame_index};
    v->active = true;
    return f;
}
void de__rt_refraction_declare(de__rt_refraction_frame *f, de_rg_pass *p) {
    if (!f)
        return;
    de_rg *g = f->context.graph;
    de_rg_read(p, f->context.dependency, DE_ACCESS_SHADER_READ);
    refraction_view *v = f->v;
    de_gpu_buffer b[] = {v->capture, v->result,      v->path,        v->rays,      v->hits,
                         v->exits,   v->shadow_rays, v->shadow_hits, v->diagnostic};
    for (u32 i = 0; i < 9; ++i) {
        de_rg_resource r = de_rg_import_buffer(g, b[i], DE_ACCESS_SHADER_READ);
        de_rg_read(p, r, DE_ACCESS_SHADER_READ);
        de_rg_write(p, r, DE_ACCESS_SHADER_WRITE);
        de_rg_export(g, r, DE_ACCESS_SHADER_READ);
    }
}
static refraction_params params(de__rt_refraction_frame *f, const de__fwd_params *pc) {
    de_gpu_device *d = f->s->r->dev;
    refraction_view *v = f->v;
    return (refraction_params){.scene = pc->scene,
                               .slot = pc->view_slot,
                               .view = pc->view,
                               .entry = pc->entry,
                               .query = de_gpu_buffer_bindless(d, f->context.query),
                               .capture = de_gpu_buffer_bindless(d, v->capture),
                               .rays = de_gpu_buffer_bindless(d, v->rays),
                               .hits = de_gpu_buffer_bindless(d, v->hits),
                               .shadow_rays = de_gpu_buffer_bindless(d, v->shadow_rays),
                               .shadow_hits = de_gpu_buffer_bindless(d, v->shadow_hits),
                               .result = de_gpu_buffer_bindless(d, v->result),
                               .path = de_gpu_buffer_bindless(d, v->path),
                               .width = f->width,
                               .height = f->height,
                               .frame = f->frame,
                               .events = f->events,
                               .lights = pc->lights,
                               .dirs = pc->dir_lights,
                               .grid = pc->grid,
                               .list = pc->list,
                               .counters = pc->counters,
                               .ltc1 = pc->ltc1,
                               .ltc2 = pc->ltc2,
                               .sampler = pc->ltc_sampler,
                               .instance = pc->instance,
                               .side = pc->side | (v->info.draws == 0 ? 2u : 0u),
                               .reserved = de_gpu_buffer_bindless(d, v->diagnostic)};
}
static void dispatch(de_gpu_cmd *cmd, de__rt_refraction_frame *f, refraction_params *p,
                     u32 kernel) {
    de_gpu_cmd_set_pipeline(cmd, f->s->kernels[kernel]);
    de_gpu_cmd_push_constants(cmd, p, sizeof *p);
    de_gpu_cmd_dispatch(cmd, (f->width + 7) / 8, (f->height + 7) / 8, 1);
}
static void barrier(de_gpu_cmd *cmd, de_gpu_buffer b, u32 before, u32 after) {
    de_gpu_barrier x = {.buffer = b, .before = before, .after = after};
    de_gpu_cmd_barriers(cmd, &x, 1);
}
void de__rt_refraction_clear(de_gpu_cmd *cmd, de__rt_refraction_frame *f, de__fwd_params *pc) {
    refraction_view *v = f->v;
    de_gpu_buffer b[] = {v->capture, v->result,      v->path,        v->rays,      v->hits,
                         v->exits,   v->shadow_rays, v->shadow_hits, v->diagnostic};
    for (u32 i = 0; i < 9; ++i)
        barrier(cmd, b[i], DE_ACCESS_SHADER_READ, DE_ACCESS_SHADER_WRITE);
    pc->rt_capture = de_gpu_buffer_bindless(f->s->r->dev, f->v->capture);
    pc->rt_result = de_gpu_buffer_bindless(f->s->r->dev, f->v->result);
    pc->rt_path = de_gpu_buffer_bindless(f->s->r->dev, f->v->path);
    refraction_params p = params(f, pc);
    dispatch(cmd, f, &p, K_CLEAR);
    barrier(cmd, v->capture, DE_ACCESS_SHADER_WRITE,
            DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
    barrier(cmd, v->result, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_WRITE);
    barrier(cmd, v->path, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
    barrier(cmd, v->diagnostic, DE_ACCESS_SHADER_WRITE,
            DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
}
void de__rt_refraction_trace(de_gpu_cmd *cmd, de__rt_refraction_frame *f,
                             const de__fwd_params *pc) {
    refraction_view *v = f->v;
    refraction_params p = params(f, pc);
    u32 count = f->width * f->height;
    barrier(cmd, v->capture, DE_ACCESS_SHADER_WRITE,
            DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
    dispatch(cmd, f, &p, K_GENERATE);
    barrier(cmd, v->rays, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
    barrier(cmd, v->capture, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
    barrier(cmd, v->path, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
    if (f->events == 2) {
        de__rt_trace(f->s->r, cmd, &f->context, v->rays, v->exits, count, 0);
        barrier(cmd, v->exits, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        p.hits = de_gpu_buffer_bindless(f->s->r->dev, v->exits);
        barrier(cmd, v->rays, DE_ACCESS_SHADER_READ,
                DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
        dispatch(cmd, f, &p, K_EXIT);
        p.hits = de_gpu_buffer_bindless(f->s->r->dev, v->hits);
        barrier(cmd, v->rays, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
    }
    de__rt_trace(f->s->r, cmd, &f->context, v->rays, v->hits, count, 0);
    barrier(cmd, v->hits, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
    barrier(cmd, v->path, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
    dispatch(cmd, f, &p, K_SHADE);
    barrier(cmd, v->result, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
    for (u32 light = 0; light < 4 + f->local_bound; ++light) {
        p.light = light;
        barrier(cmd, v->shadow_rays, DE_ACCESS_SHADER_READ, DE_ACCESS_SHADER_WRITE);
        barrier(cmd, v->shadow_hits, DE_ACCESS_SHADER_READ, DE_ACCESS_SHADER_WRITE);
        dispatch(cmd, f, &p, K_SHADOW_RAY);
        barrier(cmd, v->shadow_rays, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        de__rt_trace(f->s->r, cmd, &f->context, v->shadow_rays, v->shadow_hits, count,
                     DE__RT_CAST_SHADOWS | DE__RT_ACCEPT_FIRST_HIT);
        barrier(cmd, v->shadow_hits, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        dispatch(cmd, f, &p, K_SHADOW_ADD);
        barrier(cmd, v->result, DE_ACCESS_SHADER_WRITE,
                DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
    }
    barrier(cmd, v->capture, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
    barrier(cmd, v->path, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
    barrier(cmd, v->diagnostic, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
    ++v->info.draws;
    u64 rays = (u64)count * (f->events + 4 + f->local_bound);
    v->info.rays_traced += rays;
    f->s->r->stats.rays_traced += rays;
}
bool de__rt_refraction_get(const de__rt_refraction *s, de_view handle,
                           de__rt_refraction_info *out) {
    u32 slot = (handle.id & 255u) - 1u;
    if (!s || !out || slot >= 32)
        return false;
    const refraction_view *v = &s->views[slot];
    if (v->handle.id != handle.id || !v->active || !v->info.draws)
        return false;
    *out = v->info;
    return true;
}
