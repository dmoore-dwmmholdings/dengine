/* DENG-49: live DDGI source; S15 owns the sampling/layout and authored/static resources. */
#include "render/rt/rt_gi_probes.h"
#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/shaders/engine_shaders.h"
#include <math.h>
#include <string.h>
#define DDGI_RING       8u
#define DDGI_VOLUMES    8u
#define DDGI_BUDGET_MAX 1024u
#define DDGI_RAYS_MAX   512u

de_vec2 de__ddgi_oct_encode(de_vec3 n) {
    f32 length = fabsf(n.x) + fabsf(n.y) + fabsf(n.z);
    if (!(length > 0))
        return (de_vec2){.5f, .5f};
    n = de_v3_scale(n, 1 / length);
    f32 x = n.x, y = n.y;
    if (n.z < 0) {
        x = (1 - fabsf(n.y)) * (n.x >= 0 ? 1 : -1);
        y = (1 - fabsf(n.x)) * (n.y >= 0 ? 1 : -1);
    }
    return (de_vec2){x * .5f + .5f, y * .5f + .5f};
}
de_vec3 de__ddgi_oct_decode(de_vec2 uv) {
    de_vec3 n = {uv.x * 2 - 1, uv.y * 2 - 1, 1 - fabsf(uv.x * 2 - 1) - fabsf(uv.y * 2 - 1)};
    if (n.z < 0) {
        f32 x = n.x;
        n.x = (1 - fabsf(n.y)) * (x >= 0 ? 1 : -1);
        n.y = (1 - fabsf(x)) * (n.y >= 0 ? 1 : -1);
    }
    return de_v3_normalize(n);
}
f32 de__ddgi_hysteresis(f32 previous, f32 current, f32 h, bool valid) {
    return valid ? current * (1 - h) + previous * h : current;
}
typedef struct ddgi_params {
    u32 scene, scene_slot, query, view;
    u32 entry, previous_records, current_records, volume;
    u32 rays, hits, shadow_rays, shadow_hits;
    u32 radiance, ray_info, state_previous, state_current;
    u32 irradiance_previous, moments_previous, payload, selected_first;
    u32 probes, budget, rays_per_probe, frame;
    u32 irradiance_width, irradiance_height, moments_width, moments_height;
    u32 irradiance_pitch, moments_pitch, moments_offset, initialized;
    u32 lights, dirs, grid, list;
    u32 counters, ltc1, ltc2, sampler;
    i32 shift_x, shift_y, shift_z;
    f32 hysteresis;
    u32 history_previous, history_current, padding0, padding1;
} ddgi_params;
DE_STATIC_ASSERT(sizeof(ddgi_params) == 192, "DdgiParams shader layout");
enum {
    K_GENERATE,
    K_SHADE,
    K_SHADOW_RAY,
    K_SHADOW_ADD,
    K_SCROLL,
    K_UPDATE,
    K_BORDER,
    K_RELOCATE,
    K_COUNT
};
typedef struct ddgi_volume {
    u64 entity, settings;
    u32 cascade, last, pending_write;
    de__gi_layout layout;
    de_gpu_texture irradiance[2], moments[2];
    de_gpu_buffer state[2], history[2], payload;
    u32 irradiance_pitch, moments_pitch, moments_offset;
    de_dvec3 center, pending_center;
    bool initialized;
    de__rt_gi_info info;
} ddgi_volume;
typedef struct ddgi_view {
    de_view handle;
    ddgi_volume volumes[DDGI_VOLUMES];
    u32 count, total, cursor, capacity;
    de_gpu_buffer rays, hits, shadow_rays, shadow_hits, radiance, ray_info, params;
    ddgi_params *params_map;
    de_gpu_buffer metadata[DDGI_RING][2];
    de__gi_gpu *metadata_map[DDGI_RING][2];
    de_rg *pending;
    u64 frame;
    bool recorded, published;
} ddgi_view;
struct de__rt_gi {
    de_renderer *r;
    de_gpu_pipeline kernels[K_COUNT];
    de_cvar budget, rays, hysteresis;
    de__irradiance_snapshot snapshots[8];
    u32 count;
    de_dvec3 origin;
    ddgi_view views[32];
};
typedef struct ddgi_pass {
    de__rt_gi *s;
    ddgi_view *v;
    ddgi_volume *volume;
    de__rt_context context;
    ddgi_params pc;
    de_rg_resource reads[8];
    u32 entry, kind, local_bound;
} ddgi_pass;
static bool enabled(const de_render_settings *s) {
    return s && s->tier == DE_TIER_RT && s->rt_gi && s->gi != DE_QUALITY_OFF &&
           s->gi_mode == DE_GI_RT;
}
static bool owns_frame(void *user, const de__render_world_frame *world) {
    de__rt_gi *s = user;
    if (!world || !s->kernels[0].id || !world->view_count)
        return false;
    for (u32 i = 0; i < world->view_count; ++i) {
        de_render_settings settings = de_view_get_settings(s->r, world->views[i].camera.view);
        if (!enabled(&settings))
            return false;
    }
    return true;
}
static void release_volume(de__rt_gi *s, ddgi_volume *v) {
    for (u32 i = 0; i < 2; ++i) {
        de_gpu_texture_destroy(s->r->dev, v->irradiance[i]);
        de_gpu_texture_destroy(s->r->dev, v->moments[i]);
        de_gpu_buffer_destroy(s->r->dev, v->state[i]);
        de_gpu_buffer_destroy(s->r->dev, v->history[i]);
    }
    de_gpu_buffer_destroy(s->r->dev, v->payload);
    memset(v, 0, sizeof *v);
}
static void release_view(de__rt_gi *s, ddgi_view *v) {
    for (u32 i = 0; i < 8; ++i)
        release_volume(s, &v->volumes[i]);
    de_gpu_buffer buffers[] = {v->rays,     v->hits,     v->shadow_rays, v->shadow_hits,
                               v->radiance, v->ray_info, v->params};
    for (u32 i = 0; i < 7; ++i)
        de_gpu_buffer_destroy(s->r->dev, buffers[i]);
    for (u32 ring = 0; ring < 8; ++ring)
        for (u32 set = 0; set < 2; ++set)
            de_gpu_buffer_destroy(s->r->dev, v->metadata[ring][set]);
    memset(v, 0, sizeof *v);
}
void de__rt_gi_destroy(de__rt_gi *s) {
    if (!s)
        return;
    de__irradiance_set_update_source(s->r->irradiance, NULL, NULL, NULL);
    for (u32 i = 0; i < 32; ++i)
        release_view(s, &s->views[i]);
    for (u32 i = 0; i < K_COUNT; ++i)
        de_gpu_pipeline_destroy(s->r->dev, s->kernels[i]);
    DE_FREE(s->r->alloc, s, sizeof *s);
}
static de_result build(void *, de_rg *, de__vis_context *, de__lighting_context *,
                       de__lighting_inputs *);
de_result de__rt_gi_create(de_renderer *r, de__rt_gi **out) {
    *out = NULL;
    de__rt_gi *s = DE_ALLOC(r->alloc, sizeof *s, 16, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DDGI owner");
    memset(s, 0, sizeof *s);
    s->r = r;
    s->budget = de_cvar_register(&(de_cvar_desc){.name = "r.rt.gi.probes_per_frame",
                                                 .type = DE_CVAR_INT,
                                                 .default_number = 128,
                                                 .min = 1,
                                                 .max = DDGI_BUDGET_MAX});
    s->rays = de_cvar_register(&(de_cvar_desc){.name = "r.rt.gi.rays_per_probe",
                                               .type = DE_CVAR_INT,
                                               .default_number = 128,
                                               .min = 32,
                                               .max = DDGI_RAYS_MAX});
    s->hysteresis = de_cvar_register(&(de_cvar_desc){.name = "r.rt.gi.hysteresis",
                                                     .type = DE_CVAR_FLOAT,
                                                     .default_number = .97,
                                                     .min = 0,
                                                     .max = .999});
    if (!s->budget.id || !s->rays.id || !s->hysteresis.id) {
        de__rt_gi_destroy(s);
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "DDGI cvars");
    }
    if (r->caps.ray_query && r->caps.max_bindless_accels) {
        const char *assets[] = {"ddgi_trace", "ddgi_update", "ddgi_relocate"};
        const char *names[] = {"cs_generate", "cs_shade",  "cs_shadow_ray", "cs_shadow_add",
                               "cs_scroll",   "cs_update", "cs_border",     "cs_relocate"};
        for (u32 group = 0; group < 3; ++group) {
            u32 first = group == 0   ? 0
                        : group == 1 ? 4
                                     : 7,
                count = group == 0   ? 4
                        : group == 1 ? 3
                                     : 1;
            de__engine_shader_code code;
            char why[512] = {0};
            de_result result = de__engine_shader_load(r->dev, r->alloc, assets[group], 0,
                                                      names + first, count, &code, why, sizeof why);
            if (result != DE_OK) {
                de__rt_gi_destroy(s);
                return DE_FAIL(result, "DDGI shaders: %s", why);
            }
            for (u32 i = 0; i < count; ++i) {
                u32 kernel = first + i, threads = group == 1 ? 8 : 64;
                s->kernels[kernel] = de_gpu_pipeline_create(
                    r->dev,
                    &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                            .cs = code.entries[i].blob,
                                            .push_constant_bytes = 16,
                                            .threadgroup_size = {threads, group == 1 ? 8 : 1, 1},
                                            .name = names[kernel]});
            }
            de__engine_shader_code_free(&code);
        }
        for (u32 i = 0; i < K_COUNT; ++i)
            if (!s->kernels[i].id) {
                de__rt_gi_destroy(s);
                return DE_FAIL(DE_ERR_UNSUPPORTED, "DDGI pipeline");
            }
    }
    de__irradiance_set_update_source(r->irradiance, build, owns_frame, s);
    *out = s;
    return DE_OK;
}
void de__rt_gi_prepare(de__rt_gi *s, const de__render_world_frame *world) {
    if (!s)
        return;
    s->count = 0;
    if (!world)
        return;
    s->origin = world->origin;
    s->count = world->irradiance_count < 8 ? world->irradiance_count : 8;
    memcpy(s->snapshots, world->irradiance, s->count * sizeof s->snapshots[0]);
}
static de_gpu_buffer buffer(de__rt_gi *s, u64 bytes, u32 stride, de_gpu_memory memory,
                            const char *name) {
    return de_gpu_buffer_create(s->r->dev,
                                &(de_gpu_buffer_desc){.size = bytes,
                                                      .stride = stride,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                      .memory = memory,
                                                      .name = name});
}
static bool reserve_view(de__rt_gi *s, ddgi_view *v, u32 rays, u32 ring) {
    if (v->capacity < rays) {
        de_gpu_buffer *b[] = {&v->rays,        &v->hits,     &v->shadow_rays,
                              &v->shadow_hits, &v->radiance, &v->ray_info};
        for (u32 i = 0; i < 6; ++i) {
            de_gpu_buffer_destroy(s->r->dev, *b[i]);
            *b[i] = buffer(s, (u64)rays * (i < 4 ? 32 : 16), i < 4 ? 32 : 16, DE_MEM_GPU_ONLY,
                           "DDGI reusable rays/shading");
        }
        v->capacity = rays;
    }
    if (!v->params.id) {
        v->params = buffer(s, 8 * 8 * sizeof(ddgi_params), sizeof(ddgi_params), DE_MEM_UPLOAD,
                           "DDGI immutable parameters");
        v->params_map = de_gpu_buffer_map(s->r->dev, v->params);
    }
    for (u32 set = 0; set < 2; ++set)
        if (!v->metadata[ring][set].id) {
            v->metadata[ring][set] = buffer(s, 9 * sizeof(de__gi_gpu), sizeof(de__gi_gpu),
                                            DE_MEM_UPLOAD, "DDGI immutable volume metadata");
            v->metadata_map[ring][set] = de_gpu_buffer_map(s->r->dev, v->metadata[ring][set]);
        }
    return v->rays.id && v->hits.id && v->shadow_rays.id && v->shadow_hits.id && v->radiance.id &&
           v->ray_info.id && v->params_map && v->metadata_map[ring][0] && v->metadata_map[ring][1];
}
static bool reserve_volume(de__rt_gi *s, ddgi_volume *v, const de__irradiance_snapshot *snapshot,
                           u32 cascade, u32 excluded_light_mask) {
    de__gi_layout layout;
    if (de__gi_layout_make(&snapshot->volume, &layout) != DE_OK)
        return false;
    u64 hash =
        de__gi_settings_hash(snapshot) ^ ((u64)excluded_light_mask * UINT64_C(0x9e3779b97f4a7c15));
    if (v->entity == snapshot->entity && v->settings == hash && v->cascade == cascade &&
        v->payload.id && v->irradiance[0].id && v->irradiance[1].id && v->moments[0].id &&
        v->moments[1].id && v->state[0].id && v->state[1].id && v->history[0].id &&
        v->history[1].id)
        return true;
    release_volume(s, v);
    v->entity = snapshot->entity;
    v->settings = hash;
    v->cascade = cascade;
    v->layout = layout;
    de_gpu_texture_desc td = {.width = layout.iw,
                              .height = layout.ih,
                              .format = DE_FMT_R11G11B10_FLOAT,
                              .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                              .name = "DDGI S15 irradiance"};
    for (u32 i = 0; i < 2; ++i) {
        td.width = layout.iw;
        td.height = layout.ih;
        td.format = DE_FMT_R11G11B10_FLOAT;
        v->irradiance[i] = de_gpu_texture_create(s->r->dev, &td);
        td.width = layout.dw;
        td.height = layout.dh;
        td.format = DE_FMT_RG16_FLOAT;
        v->moments[i] = de_gpu_texture_create(s->r->dev, &td);
        v->state[i] =
            buffer(s, (u64)layout.probes * 16, 16, DE_MEM_GPU_ONLY, "DDGI offset/classification");
        v->history[i] = buffer(s, (u64)layout.probes * (64 * 16 + 256 * 8), 0, DE_MEM_GPU_ONLY,
                               "DDGI float32 hysteresis feedback");
    }
    v->irradiance_pitch = ((layout.iw * 4 + 255) & ~255u) / 4;
    v->moments_pitch = ((layout.dw * 4 + 255) & ~255u) / 4;
    v->moments_offset = (u32)(((u64)v->irradiance_pitch * layout.ih + 127) & ~127ull);
    v->payload = de_gpu_buffer_create(
        s->r->dev, &(de_gpu_buffer_desc){
                       .size = ((u64)v->moments_offset + (u64)v->moments_pitch * layout.dh) * 4,
                       .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                       .name = "DDGI exact S15 packed atlas publication"});
    return v->irradiance[0].id && v->irradiance[1].id && v->moments[0].id && v->moments[1].id &&
           v->state[0].id && v->state[1].id && v->payload.id && v->history[0].id &&
           v->history[1].id;
}
static de__gi_gpu volume_gpu(const de__irradiance_snapshot *snapshot, const ddgi_volume *v,
                             de_dvec3 center, de_dvec3 origin, bool ready) {
    de__gi_gpu g = {0};
    de_quat q = de_quat_normalize(snapshot->rotation);
    de_vec3 axes[] = {de_quat_rotate(q, (de_vec3){1, 0, 0}), de_quat_rotate(q, (de_vec3){0, 1, 0}),
                      de_quat_rotate(q, (de_vec3){0, 0, 1})};
    de_vec3 relative = de_dv3_relative(center, origin);
    const f32 extent[] = {snapshot->volume.extent.x, snapshot->volume.extent.y,
                          snapshot->volume.extent.z};
    f32 minimum = INFINITY;
    for (u32 i = 0; i < 3; ++i) {
        g.local[i][0] = axes[i].x;
        g.local[i][1] = axes[i].y;
        g.local[i][2] = axes[i].z;
        g.local[i][3] = -de_v3_dot(axes[i], relative);
        g.extent[i] = extent[i];
        g.counts[i] = v->layout.counts[i];
        g.step[i] = 2 * extent[i] / g.counts[i];
        minimum = fminf(minimum, g.step[i]);
    }
    g.step[3] = fminf(.02f, minimum * .01f);
    g.extent[3] = g.step[3] * .5f;
    memcpy(g.rotation, &q, sizeof q);
    g.counts[3] = v->layout.columns;
    g.textures[3] = 1;
    g.params[0] = snapshot->volume.intensity;
    g.params[1] = snapshot->volume.blend_distance;
    g.params[2] = v->layout.range;
    g.params[3] = ready ? 1.f : 0.f;
    return g;
}
static void barrier(de_gpu_cmd *cmd, de_gpu_buffer buffer, u32 before, u32 after) {
    de_gpu_barrier b = {.buffer = buffer, .before = before, .after = after};
    de_gpu_cmd_barriers(cmd, &b, 1);
}
static void dispatch(de_gpu_cmd *cmd, ddgi_pass *p, u32 kernel, u32 light) {
    const ddgi_params *pc = &p->pc;
    u32 push[] = {de_gpu_buffer_bindless(p->s->r->dev, p->v->params), p->entry, light, 0};
    de_gpu_cmd_set_pipeline(cmd, p->s->kernels[kernel]);
    de_gpu_cmd_push_constants(cmd, push, sizeof push);
    if (kernel == K_SCROLL)
        de_gpu_cmd_dispatch(cmd, (pc->moments_width + 7) / 8, (pc->moments_height + 7) / 8, 1);
    else if (kernel == K_UPDATE || kernel == K_BORDER)
        de_gpu_cmd_dispatch(cmd, kernel == K_UPDATE ? 2 : 3, kernel == K_UPDATE ? 2 : 3,
                            pc->budget);
    else {
        u32 count = kernel == K_RELOCATE ? pc->budget : pc->budget * pc->rays_per_probe,
            groups = (count + 63) / 64;
        de_gpu_cmd_dispatch(cmd, groups < 65535 ? groups : 65535, (groups + 65534) / 65535, 1);
    }
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    ddgi_pass *p = user;
    ddgi_view *v = p->v;
    ddgi_volume *volume = p->volume;
    de_gpu_device *d = p->s->r->dev;
    ddgi_params pc = p->pc;
    u32 read = volume->last, write = volume->pending_write;
    if (p->kind) {
        de_gpu_cmd_copy_buffer_to_texture(cmd, volume->irradiance[write], 0, 0, volume->payload, 0,
                                          volume->irradiance_pitch * 4);
        de_gpu_cmd_copy_buffer_to_texture(cmd, volume->moments[write], 0, 0, volume->payload,
                                          (u64)volume->moments_offset * 4,
                                          volume->moments_pitch * 4);
        v->recorded = true;
        return;
    }
    pc.rays = de_gpu_buffer_bindless(d, v->rays);
    pc.hits = de_gpu_buffer_bindless(d, v->hits);
    pc.shadow_rays = de_gpu_buffer_bindless(d, v->shadow_rays);
    pc.shadow_hits = de_gpu_buffer_bindless(d, v->shadow_hits);
    pc.radiance = de_gpu_buffer_bindless(d, v->radiance);
    pc.ray_info = de_gpu_buffer_bindless(d, v->ray_info);
    pc.state_previous = de_gpu_buffer_bindless(d, volume->state[read]);
    pc.state_current = de_gpu_buffer_bindless(d, volume->state[write]);
    pc.payload = de_gpu_buffer_bindless(d, volume->payload);
    pc.history_previous = de_gpu_buffer_bindless(d, volume->history[read]);
    pc.history_current = de_gpu_buffer_bindless(d, volume->history[write]);
    pc.irradiance_previous = de_gpu_texture_bindless(d, volume->irradiance[read], 0, true);
    pc.moments_previous = de_gpu_texture_bindless(d, volume->moments[read], 0, true);
    u32 *indices[] = {&pc.lights, &pc.dirs, &pc.grid, &pc.list, &pc.counters};
    for (u32 i = 0; i < 5; ++i)
        *indices[i] = de_gpu_buffer_bindless(d, de_rg_buffer(pass, p->reads[i]));
    pc.ltc1 = de_gpu_texture_bindless(d, de_rg_texture(pass, p->reads[5]), 0, false);
    pc.ltc2 = de_gpu_texture_bindless(d, de_rg_texture(pass, p->reads[6]), 0, false);
    pc.query = de_gpu_buffer_bindless(d, p->context.query);
    v->params_map[p->entry] = pc;
    dispatch(cmd, p, K_SCROLL, 0);
    barrier(cmd, volume->history[write], DE_ACCESS_SHADER_WRITE,
            DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
    barrier(cmd, volume->state[write], DE_ACCESS_SHADER_WRITE,
            DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
    barrier(cmd, volume->payload, DE_ACCESS_SHADER_WRITE,
            DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
    if (pc.budget) {
        dispatch(cmd, p, K_GENERATE, 0);
        barrier(cmd, v->rays, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        u32 count = pc.budget * pc.rays_per_probe;
        de__rt_trace(p->s->r, cmd, &p->context, v->rays, v->hits, count, 0);
        barrier(cmd, v->hits, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        dispatch(cmd, p, K_SHADE, 0);
        barrier(cmd, v->radiance, DE_ACCESS_SHADER_WRITE,
                DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
        barrier(cmd, v->ray_info, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        for (u32 light = 0; light < 4 + p->local_bound; ++light) {
            dispatch(cmd, p, K_SHADOW_RAY, light);
            barrier(cmd, v->shadow_rays, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
            de__rt_trace(p->s->r, cmd, &p->context, v->shadow_rays, v->shadow_hits, count,
                         DE__RT_CAST_SHADOWS | DE__RT_ACCEPT_FIRST_HIT);
            barrier(cmd, v->shadow_hits, DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
            dispatch(cmd, p, K_SHADOW_ADD, light);
            barrier(cmd, v->radiance, DE_ACCESS_SHADER_WRITE,
                    DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
            barrier(cmd, v->shadow_rays, DE_ACCESS_SHADER_READ, DE_ACCESS_SHADER_WRITE);
            barrier(cmd, v->shadow_hits, DE_ACCESS_SHADER_READ, DE_ACCESS_SHADER_WRITE);
        }
        dispatch(cmd, p, K_RELOCATE, 0);
        barrier(cmd, volume->state[write], DE_ACCESS_SHADER_WRITE, DE_ACCESS_SHADER_READ);
        dispatch(cmd, p, K_UPDATE, 0);
        barrier(cmd, volume->payload, DE_ACCESS_SHADER_WRITE,
                DE_ACCESS_SHADER_READ | DE_ACCESS_SHADER_WRITE);
        dispatch(cmd, p, K_BORDER, 0);
    }
}
static de_result build(void *user, de_rg *g, de__vis_context *vc, de__lighting_context *lc,
                       de__lighting_inputs *inputs) {
    de__rt_gi *s = user;
    if (!s || !vc || vc->view_slot >= 32)
        return DE_OK;
    ddgi_view *view = &s->views[vc->view_slot];
    view->published = false;
    view->pending = NULL;
    if (!enabled(vc->settings) || !s->kernels[0].id || !s->count)
        return DE_OK;
    de__rt_context *rt = de__rt_context_get(s->r, vc->view, g, vc->frame_index);
    if (!rt)
        return DE_OK;
    if (view->handle.id != vc->view.id) {
        release_view(s, view);
        view->handle = vc->view;
    }
    u32 cascades = vc->settings->gi == DE_QUALITY_LOW      ? 1
                   : vc->settings->gi == DE_QUALITY_MEDIUM ? 2
                   : vc->settings->gi == DE_QUALITY_HIGH   ? 3
                                                           : 4;
    de__irradiance_snapshot snapshots[8];
    u32 count = 0, total = 0;
    u64 resident = 0;
    for (u32 source = 0; source < s->count && count < 8; ++source) {
        de__gi_layout base;
        if (de__gi_layout_make(&s->snapshots[source].volume, &base) != DE_OK) {
            de_error_clear();
            continue;
        }
        for (u32 cascade = 0; cascade < cascades && count < 8; ++cascade) {
            de__irradiance_snapshot snapshot = s->snapshots[source];
            memcpy(snapshot.volume.counts, base.counts, sizeof base.counts);
            f32 scale = (f32)(1u << cascade);
            snapshot.volume.extent = de_v3_scale(snapshot.volume.extent, scale);
            snapshot.volume.spacing *= scale;
            de__gi_layout layout;
            if (de__gi_layout_make(&snapshot.volume, &layout) != DE_OK) {
                de_error_clear();
                continue;
            }
            resident += layout.payload_bytes * 2;
            if (resident > 64ull * 1024 * 1024)
                break;
            if (!reserve_volume(s, &view->volumes[count], &snapshot, cascade,
                                vc->excluded_light_mask))
                return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DDGI volume capacity");
            snapshots[count] = snapshot;
            total += view->volumes[count].layout.probes;
            ++count;
        }
    }
    for (u32 i = count; i < view->count; ++i)
        release_volume(s, &view->volumes[i]);
    view->count = count;
    view->total = total;
    if (!count || !total)
        return DE_OK;
    u32 budget = (u32)de_cvar_int(s->budget);
    if (budget > total)
        budget = total;
    u32 rays = (u32)de_cvar_int(s->rays), ring = (u32)(vc->frame_index % 8);
    if (!reserve_view(s, view, budget * rays, ring))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DDGI shared capacity");
    de_gpu_device *d = s->r->dev;
    de__gi_gpu *current = view->metadata_map[ring][0], *previous = view->metadata_map[ring][1];
    memset(current, 0, 9 * sizeof *current);
    memset(previous, 0, 9 * sizeof *previous);
    current[0].counts[0] = previous[0].counts[0] = count;
    de_rg_resource current_meta =
                       de_rg_import_buffer(g, view->metadata[ring][0], DE_ACCESS_SHADER_READ),
                   previous_meta =
                       de_rg_import_buffer(g, view->metadata[ring][1], DE_ACCESS_SHADER_READ);
    u32 current_index = de_gpu_buffer_bindless(d, view->metadata[ring][0]),
        previous_index = de_gpu_buffer_bindless(d, view->metadata[ring][1]);
    inputs->gi_records = current_meta;
    inputs->gi_texture_count = 0;
    inputs->gi_buffer_count = 0;
    u32 start = (u32)((de__rt_sample_frame(s->r, vc->frame_index) * (u64)budget) % total),
        end = start + budget, base = 0;
    de_vec3 camera = vc->camera_position;
    de_dvec3 absolute_camera = {s->origin.x + camera.x, s->origin.y + camera.y,
                                s->origin.z + camera.z};
    de_rg_pass *previous_work = NULL;
    de_rg_resource previous_payload = {0};
    for (u32 i = 0; i < count; ++i) {
        ddgi_volume *volume = &view->volumes[i];
        de__irradiance_snapshot *snapshot = &snapshots[i];
        u32 n = volume->layout.probes, read = volume->last, write = read ^ 1u;
        volume->pending_write = write;
        de_quat q = de_quat_normalize(snapshot->rotation), inverse = {-q.x, -q.y, -q.z, q.w};
        de_vec3 local_camera =
            de_quat_rotate(inverse, de_dv3_relative(absolute_camera, snapshot->position));
        de_vec3 step = {2 * snapshot->volume.extent.x / volume->layout.counts[0],
                        2 * snapshot->volume.extent.y / volume->layout.counts[1],
                        2 * snapshot->volume.extent.z / volume->layout.counts[2]};
        de_vec3 shift_local = {roundf(local_camera.x / step.x) * step.x,
                               roundf(local_camera.y / step.y) * step.y,
                               roundf(local_camera.z / step.z) * step.z},
                offset = de_quat_rotate(q, shift_local);
        volume->pending_center =
            (de_dvec3){snapshot->position.x + offset.x, snapshot->position.y + offset.y,
                       snapshot->position.z + offset.z};
        de_vec3 delta =
            volume->initialized
                ? de_quat_rotate(inverse, de_dv3_relative(volume->pending_center, volume->center))
                : (de_vec3){0};
        i32 shift[3] = {(i32)lrintf(delta.x / step.x), (i32)lrintf(delta.y / step.y),
                        (i32)lrintf(delta.z / step.z)};
        u32 a = start > base ? start : base, b = end < base + n ? end : base + n, first = 0,
            selected = 0;
        if (b > a) {
            first = a - base;
            selected = b - a;
        }
        if (end > total) {
            u32 wrapped = end - total, wb = wrapped < base + n ? wrapped : base + n;
            if (wb > base) {
                if (!selected)
                    first = 0;
                selected += wb - base;
            }
        }
        current[i + 1] = volume_gpu(snapshot, volume, volume->pending_center, s->origin,
                                    volume->initialized || selected > 0);
        previous[i + 1] = volume_gpu(snapshot, volume,
                                     volume->initialized ? volume->center : volume->pending_center,
                                     s->origin, volume->initialized);
        de__gi_gpu *records[] = {&current[i + 1], &previous[i + 1]};
        for (u32 set = 0; set < 2; ++set) {
            u32 index = set ? read : write;
            records[set]->textures[0] =
                de_gpu_texture_bindless(d, volume->irradiance[index], 0, true);
            records[set]->textures[1] = de_gpu_texture_bindless(d, volume->moments[index], 0, true);
            records[set]->textures[2] = de_gpu_buffer_bindless(d, volume->state[index]);
        }
        de_rg_resource
            old_irr = de_rg_import_texture(g, volume->irradiance[read],
                                           volume->initialized ? DE__GI_ACCESS : DE_ACCESS_NONE),
            old_mom = de_rg_import_texture(g, volume->moments[read],
                                           volume->initialized ? DE__GI_ACCESS : DE_ACCESS_NONE),
            old_state =
                de_rg_import_buffer(g, volume->state[read],
                                    volume->initialized ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE),
            irr = de_rg_import_texture(g, volume->irradiance[write],
                                       volume->initialized ? DE__GI_ACCESS : DE_ACCESS_NONE),
            mom = de_rg_import_texture(g, volume->moments[write],
                                       volume->initialized ? DE__GI_ACCESS : DE_ACCESS_NONE),
            state =
                de_rg_import_buffer(g, volume->state[write],
                                    volume->initialized ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE),
            payload = de_rg_import_buffer(
                g, volume->payload, volume->initialized ? DE_ACCESS_COPY_SRC : DE_ACCESS_NONE);
        ddgi_pass *p = de_arena_push_zero(g->arena, 2 * sizeof *p, 16);
        if (!p)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DDGI graph data");
        *p = (ddgi_pass){.s = s,
                         .v = view,
                         .volume = volume,
                         .context = *rt,
                         .entry = ring * 8 + i,
                         .pc = {.scene = vc->scene_views_bindless,
                                .scene_slot = vc->scene_view_slot,
                                .view = lc->constants_bindless,
                                .entry = lc->constants_entry,
                                .previous_records = previous_index,
                                .current_records = current_index,
                                .volume = i,
                                .selected_first = first,
                                .probes = n,
                                .budget = selected,
                                .rays_per_probe = rays,
                                .frame = (u32)de__rt_sample_frame(s->r, vc->frame_index),
                                .irradiance_width = volume->layout.iw,
                                .irradiance_height = volume->layout.ih,
                                .moments_width = volume->layout.dw,
                                .moments_height = volume->layout.dh,
                                .irradiance_pitch = volume->irradiance_pitch,
                                .moments_pitch = volume->moments_pitch,
                                .moments_offset = volume->moments_offset,
                                .initialized = volume->initialized ? 1 : 0,
                                .sampler = lc->sampler_bindless,
                                .shift_x = shift[0],
                                .shift_y = shift[1],
                                .shift_z = shift[2],
                                .hysteresis = (f32)de_cvar_float(s->hysteresis)},
                         .reads = {lc->lights, lc->dir_lights, lc->grid_buf, lc->list, lc->counters,
                                   lc->ltc1, lc->ltc2, vc->scene_views},
                         .local_bound = lc->view.light_slots + lc->view.proxy_capacity};
        if (p->local_bound > lc->local_capacity)
            p->local_bound = lc->local_capacity;
        de_rg_pass *pass = de_rg_add_pass(g, "RT.GI.TraceUpdate", DE_QUEUE_GRAPHICS, execute, p);
        if (!pass)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DDGI trace/update pass");
        de_rg_read(pass, rt->dependency, DE_ACCESS_SHADER_READ);
        de_rg_read(pass, current_meta, DE_ACCESS_SHADER_READ);
        de_rg_read(pass, previous_meta, DE_ACCESS_SHADER_READ);
        for (u32 j = 0; j < 8; ++j)
            de_rg_read(pass, p->reads[j], DE_ACCESS_SHADER_READ);
        de_rg_read(pass, old_irr, DE_ACCESS_SHADER_READ);
        de_rg_read(pass, old_mom, DE_ACCESS_SHADER_READ);
        de_rg_read(pass, old_state, DE_ACCESS_SHADER_READ);
        if (previous_work)
            de_rg_read(pass, previous_payload, DE_ACCESS_COPY_SRC);
        de_rg_resource history_previous = de_rg_import_buffer(
                           g, volume->history[read],
                           volume->initialized ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE),
                       history_current = de_rg_import_buffer(
                           g, volume->history[write],
                           volume->initialized ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE);
        de_rg_read(pass, history_previous, DE_ACCESS_SHADER_READ);
        de_rg_write(pass, history_current, DE_ACCESS_SHADER_WRITE);
        de_rg_export(g, history_previous, DE_ACCESS_SHADER_READ);
        de_rg_export(g, history_current, DE_ACCESS_SHADER_READ);
        de_rg_write(pass, state, DE_ACCESS_SHADER_WRITE);
        de_rg_write(pass, payload, DE_ACCESS_SHADER_WRITE);
        de_rg_resource optional[] = {lc->strip, lc->probe_records, lc->probe_list,
                                     lc->atlas_records, lc->atlas_texture};
        for (u32 j = 0; j < 5; ++j)
            if (optional[j].id)
                de_rg_read(pass, optional[j], DE_ACCESS_SHADER_READ);
        for (u32 j = 0; j < lc->probe_texture_count; ++j)
            de_rg_read(pass, lc->probe_textures[j], DE_ACCESS_SHADER_READ);
        de_gpu_buffer buffers[] = {view->rays,        view->hits,     view->shadow_rays,
                                   view->shadow_hits, view->radiance, view->ray_info};
        for (u32 j = 0; j < 6; ++j) {
            de_rg_resource resource = de_rg_import_buffer(g, buffers[j], DE_ACCESS_SHADER_READ);
            de_rg_write(pass, resource, DE_ACCESS_SHADER_WRITE);
            de_rg_export(g, resource, DE_ACCESS_SHADER_READ);
        }
        p[1] = p[0];
        p[1].kind = 1;
        pass = de_rg_add_pass(g, "RT.GI.Publish", DE_QUEUE_GRAPHICS, execute, &p[1]);
        if (!pass)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "DDGI publication pass");
        de_rg_read(pass, payload, DE_ACCESS_COPY_SRC);
        de_rg_write(pass, irr, DE_ACCESS_COPY_DST);
        de_rg_write(pass, mom, DE_ACCESS_COPY_DST);
        de_rg_export(g, payload, DE_ACCESS_COPY_SRC);
        de_rg_export(g, irr, DE__GI_ACCESS);
        de_rg_export(g, mom, DE__GI_ACCESS);
        de_rg_export(g, old_irr, DE__GI_ACCESS);
        de_rg_export(g, old_mom, DE__GI_ACCESS);
        de_rg_export(g, state, DE_ACCESS_SHADER_READ);
        de_rg_export(g, old_state, DE_ACCESS_SHADER_READ);
        previous_work = pass;
        previous_payload = payload;
        inputs->gi_textures[inputs->gi_texture_count++] = irr;
        inputs->gi_textures[inputs->gi_texture_count++] = mom;
        inputs->gi_buffers[inputs->gi_buffer_count++] = state;
        u64 retained = 1;
        for (u32 axis = 0; axis < 3; ++axis) {
            u32 amount = (u32)(shift[axis] < 0 ? -shift[axis] : shift[axis]);
            retained *=
                amount < volume->layout.counts[axis] ? volume->layout.counts[axis] - amount : 0;
        }
        volume->info = (de__rt_gi_info){.layout = volume->layout,
                                        .gpu = current[i + 1],
                                        .irradiance = volume->irradiance[write],
                                        .distance = volume->moments[write],
                                        .classification = volume->state[write],
                                        .rays = view->rays,
                                        .hits = view->hits,
                                        .metadata = view->metadata[ring][0],
                                        .cascades = cascades,
                                        .volumes = count,
                                        .probes_updated = selected,
                                        .rays_per_probe = rays,
                                        .cursor = first,
                                        .frame = vc->frame_index,
                                        .rays_traced = (u64)selected * rays * (5 + p->local_bound),
                                        .invalidated = volume->initialized ? n - retained : n};
        s->r->stats.rays_traced += volume->info.rays_traced;
        base += n;
    }
    /* Every update reads the complete previous snapshot, never current partially updated tiles. */
    for (u32 i = 0; i < count; ++i) {
        ddgi_volume *volume = &view->volumes[i];
        de_rg_resource old[] = {
            de_rg_import_texture(g, volume->irradiance[volume->last],
                                 volume->initialized ? DE__GI_ACCESS : DE_ACCESS_NONE),
            de_rg_import_texture(g, volume->moments[volume->last],
                                 volume->initialized ? DE__GI_ACCESS : DE_ACCESS_NONE),
            de_rg_import_buffer(g, volume->state[volume->last],
                                volume->initialized ? DE_ACCESS_SHADER_READ : DE_ACCESS_NONE)};
        for (u32 j = 0; j < g->pass_count; ++j) {
            de_rg_pass *pass = g->passes[j];
            if (strcmp(pass->name, "RT.GI.TraceUpdate") == 0)
                for (u32 k = 0; k < 3; ++k)
                    de_rg_read(pass, old[k], DE_ACCESS_SHADER_READ);
        }
    }
    lc->gi_records = inputs->gi_records;
    lc->gi_texture_count = inputs->gi_texture_count;
    lc->gi_buffer_count = inputs->gi_buffer_count;
    memcpy(lc->gi_textures, inputs->gi_textures, sizeof lc->gi_textures);
    memcpy(lc->gi_buffers, inputs->gi_buffers, sizeof lc->gi_buffers);
    view->pending = g;
    view->frame = vc->frame_index;
    view->cursor = start;
    view->recorded = false;
    view->published = true;
    return DE_OK;
}
void de__rt_gi_complete(de__rt_gi *s, de_view handle, de_rg *g, u64 frame, bool success) {
    u32 slot = (handle.id & 255u) - 1u;
    if (!s || slot >= 32)
        return;
    ddgi_view *view = &s->views[slot];
    if (view->handle.id != handle.id || view->pending != g || view->frame != frame)
        return;
    success = success && view->recorded;
    if (success)
        for (u32 i = 0; i < view->count; ++i) {
            ddgi_volume *v = &view->volumes[i];
            v->last = v->pending_write;
            v->center = v->pending_center;
            v->initialized = true;
        }
    view->published = success;
    view->pending = NULL;
}
bool de__rt_gi_get(const de__rt_gi *s, de_view handle, u32 volume, de__rt_gi_info *out) {
    u32 slot = (handle.id & 255u) - 1u;
    if (!s || !out || slot >= 32)
        return false;
    const ddgi_view *view = &s->views[slot];
    if (!view->published || view->handle.id != handle.id || volume >= view->count)
        return false;
    *out = view->volumes[volume].info;
    return true;
}
