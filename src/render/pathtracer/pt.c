/* Progressive per-view path tracing. The graph owns submission, pipelines own immutable SBTs. */
#include "render/pathtracer/pt_internal.h"
#include "render/rt/rt_internal.h"
#include "render/rt/restir_di.h"
#include "render/graph/rg_internal.h"
#include "render/passes/vis/vis_stages.h"
#include "render/shaders/engine_shaders.h"
#include "render/materials/brdf_luts.h"
#include "render/materials/material_instances.h"
#include "render/world/render_world.h"
#include <math.h>
#include <string.h>
#define PT_RING 8u
/* PtParams in pt_closest_hit.slang; float4 rows avoid target matrix packing differences. */
typedef struct pt_params {
    u32 query, width, height, sample, max_bounces, sources, source_count, env_cdf;
    u32 accumulation, output, brdf_lut, seed;
    f32 inv_view_proj[4][4], camera[4];
    de__ibl_gpu ibl;
    u32 environment_revision, flags, reserved0, reserved1;
    f32 exposure;
    u32 light_count, unused0, unused1;
    de__pt_media_params media;
    u32 aov_albedo, aov_normal, aov_depth, aov_position, aov_material, aov_pad[3];
} pt_params;
DE_STATIC_ASSERT(sizeof(pt_params) == 416, "PtParams shader layout");
typedef struct pt_view {
    de_view handle;
    de_gpu_texture accumulation, output;
    de_gpu_texture albedo, normal, depth, position, material;
    de_gpu_buffer params, cdf;
    u32 width, height, samples, reset_serial, seen_reset, strategy, seed;
    u64 signature, frame, pending_frame;
    de_mat4 matrix, projection;
    de_render_settings settings;
    de__ibl_gpu ibl;
    bool active, pending, cdf_valid;
} pt_view;
struct de__pt {
    de_renderer *r;
    de_gpu_pipeline pipeline, env;
    de__pt_media *media;
    de_cvar max_samples, seed, strategy, final_denoise, golden;
    de__pt_denoise *denoise;
    bool warned;
    pt_view views[32];
};
f32 de__pt_power(f32 a, f32 b) {
    return a * a / fmaxf(a * a + b * b, 1e-30f);
}
void de__pt_env_cdf(const f32 *weights, u32 count, f32 *cdf, f32 *pdf) {
    f64 total = 0;
    for (u32 i = 0; i < count; ++i)
        if (isfinite(weights[i]) && weights[i] > 0)
            total += weights[i];
    f64 sum = 0;
    for (u32 i = 0; i < count; ++i) {
        pdf[i] = total > 0 ? (f32)(fmaxf(isfinite(weights[i]) ? weights[i] : 0, 0) / total)
                           : 1.f / count;
        sum += pdf[i];
        cdf[i] = i + 1 == count ? 1.f : (f32)sum;
    }
}
static void release(de__pt *p, pt_view *v) {
    de_gpu_texture_destroy(p->r->dev, v->accumulation);
    de_gpu_texture_destroy(p->r->dev, v->output);
    de_gpu_texture_destroy(p->r->dev, v->albedo);
    de_gpu_texture_destroy(p->r->dev, v->normal);
    de_gpu_texture_destroy(p->r->dev, v->depth);
    de_gpu_texture_destroy(p->r->dev, v->position);
    de_gpu_texture_destroy(p->r->dev, v->material);
    de_gpu_buffer_destroy(p->r->dev, v->params);
    de_gpu_buffer_destroy(p->r->dev, v->cdf);
    memset(v, 0, sizeof *v);
}
void de__pt_destroy(de__pt *p) {
    if (!p)
        return;
    for (u32 i = 0; i < 32; ++i)
        release(p, &p->views[i]);
    de__pt_media_destroy(p->media);
    de__pt_denoise_destroy(p->denoise);
    de_gpu_pipeline_destroy(p->r->dev, p->pipeline);
    de_gpu_pipeline_destroy(p->r->dev, p->env);
    DE_FREE(p->r->alloc, p, sizeof *p);
}
bool de__pt_enabled(const de_renderer *r, const de_render_settings *s) {
    return r && r->pt && s->path_tracer && s->tier == DE_TIER_RT && r->caps.rt_pipelines &&
           r->pt->pipeline.id;
}
de_result de__pt_create(de_renderer *r, de__pt **out) {
    de__pt *p = DE_ALLOC(r->alloc, sizeof *p, 16, DE_MEM_RENDER);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "PT owner");
    memset(p, 0, sizeof *p);
    p->r = r;
    *out = p;
    DE_TRY(de__pt_media_create(r, &p->media));
    DE_TRY(de__pt_denoise_create(r, &p->denoise));
    p->final_denoise = de_cvar_register(
        &(de_cvar_desc){.name = "r.pt.final_denoise", .type = DE_CVAR_BOOL, .default_number = 0});
    p->golden = de_cvar_register(
        &(de_cvar_desc){.name = "r.pt.golden", .type = DE_CVAR_BOOL, .default_number = 0});
    p->max_samples = de_cvar_register(&(de_cvar_desc){.name = "r.pt.max_samples",
                                                      .type = DE_CVAR_INT,
                                                      .default_number = 0,
                                                      .min = 0,
                                                      .max = 16777216});
    p->seed = de_cvar_register(&(de_cvar_desc){.name = "r.pt.seed",
                                               .type = DE_CVAR_INT,
                                               .default_number = 0,
                                               .min = 0,
                                               .max = 2147483647});
    p->strategy = de_cvar_register(&(de_cvar_desc){.name = "r.pt.sampling_strategy",
                                                   .type = DE_CVAR_INT,
                                                   .default_number = 0,
                                                   .min = 0,
                                                   .max = 2});
    if (!r->caps.rt_pipelines)
        return DE_OK;
    de_gpu_shader_blob ray = {0}, miss = {0}, hit = {0}, alpha = {0}, env = {0};
#if DE_HAS_GPU_METAL
    extern const unsigned char de__pt_metal_library[];
    extern const unsigned long long de__pt_metal_library_bytes;
    if (r->caps.backend == DE_GPU_METAL) {
        ray = (de_gpu_shader_blob){de__pt_metal_library, de__pt_metal_library_bytes, "pt_raygen"};
        miss = ray;
        miss.entry = "pt_miss";
        hit = ray;
        hit.entry = "pt_closest";
        alpha = ray;
        alpha.entry = "pt_alpha";
        env = ray;
        env.entry = "pt_env_cdf";
    }
#endif
    de__engine_shader_code code = {0}, env_code = {0};
    if (!ray.code) {
        const char *entries[] = {"pt_raygen", "pt_miss", "pt_closest", "pt_alpha"};
        char why[512] = {0};
        de_result result = de__engine_shader_load(r->dev, r->alloc, "pt_pipeline", 0, entries, 4,
                                                  &code, why, sizeof why);
        if (result != DE_OK) {
            DE_WARN(r->log, "path tracer RT stages unavailable: %s", why);
            return DE_OK;
        }
        const char *env_entry[] = {"pt_env_cdf"};
        result = de__engine_shader_load(r->dev, r->alloc, "pt_environment", 0, env_entry, 1,
                                        &env_code, why, sizeof why);
        if (result != DE_OK) {
            de__engine_shader_code_free(&code);
            return result;
        }
        ray = code.entries[0].blob;
        miss = code.entries[1].blob;
        hit = code.entries[2].blob;
        alpha = code.entries[3].blob;
        env = env_code.entries[0].blob;
    }
    de_gpu_rt_hit_group group = {.closest_hit = hit, .any_hit = alpha};
    de_gpu_rt_desc rt = {.raygen = ray,
                         .miss = &miss,
                         .miss_count = 1,
                         .hit_groups = &group,
                         .hit_group_count = 1,
                         .max_recursion = 1,
                         .payload_bytes = 64,
                         .attribute_bytes = 8};
    p->pipeline =
        de_gpu_pipeline_create(r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_RAYTRACING,
                                                               .rt = &rt,
                                                               .push_constant_bytes = 8,
                                                               .name = "PT iterative raygen"});
    p->env = de_gpu_pipeline_create(r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                                    .cs = env,
                                                                    .push_constant_bytes = 8,
                                                                    .threadgroup_size = {1, 1, 1},
                                                                    .name = "PT environment CDF"});
    de__engine_shader_code_free(&code);
    de__engine_shader_code_free(&env_code);
    if (!p->pipeline.id || !p->env.id)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "PT pipelines");
    return DE_OK;
}
static bool capacity(de__pt *p, pt_view *v, de__view_frame *f) {
    if (v->handle.id == f->view.id && v->width == f->width && v->height == f->height)
        return true;
    release(p, v);
    v->handle = f->view;
    v->width = f->width;
    v->height = f->height;
    de_gpu_device *d = p->r->dev;
    v->accumulation = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.width = v->width,
                                  .height = v->height,
                                  .depth_or_layers = 1,
                                  .mip_count = 1,
                                  .format = DE_FMT_RGBA32_FLOAT,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                  .name = "PT linear accumulation"});
    v->output = de_gpu_texture_create(
        d, &(de_gpu_texture_desc){.width = v->width,
                                  .height = v->height,
                                  .depth_or_layers = 1,
                                  .mip_count = 1,
                                  .format = DE_FMT_RGBA16_FLOAT,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                  .name = "PT exposed HDR"});
    de_gpu_texture *aovs[] = {&v->albedo, &v->normal, &v->depth, &v->position, &v->material};
    const de_gpu_format formats[] = {DE_FMT_RGBA32_FLOAT, DE_FMT_RGBA32_FLOAT, DE_FMT_R32_FLOAT,
                                     DE_FMT_RGBA32_FLOAT, DE_FMT_RG32_FLOAT};
    const char *names[] = {"PT first-hit albedo", "PT first-hit world normal",
                           "PT first-hit distance", "PT first-hit position",
                           "PT actual material identity"};
    for (u32 i = 0; i < 5; ++i)
        *aovs[i] = de_gpu_texture_create(
            d,
            &(de_gpu_texture_desc){.width = v->width,
                                   .height = v->height,
                                   .depth_or_layers = 1,
                                   .mip_count = 1,
                                   .format = formats[i],
                                   .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                   .name = names[i]});
    v->params =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = sizeof(pt_params) * PT_RING,
                                                      .stride = sizeof(pt_params),
                                                      .usage = DE_USAGE_STORAGE,
                                                      .memory = DE_MEM_UPLOAD,
                                                      .name = "PT immutable frame parameters"});
    v->cdf =
        de_gpu_buffer_create(d, &(de_gpu_buffer_desc){.size = 512 * 8,
                                                      .stride = 8,
                                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                      .name = "PT environment CDF"});
    return v->accumulation.id && v->output.id && v->params.id && v->cdf.id && v->albedo.id &&
           v->normal.id && v->depth.id && v->position.id && v->material.id;
}
typedef struct pt_pass {
    de__pt *p;
    pt_view *v;
    u32 pc[2];
    bool env, trace;
    de__pt_media_params media;
} pt_pass;
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    (void)pass;
    pt_pass *x = user;
    de__pt *p = x->p;
    if (x->env) {
        de_gpu_cmd_set_pipeline(cmd, p->env);
        de_gpu_cmd_push_constants(cmd, x->pc, sizeof x->pc);
        de_gpu_cmd_dispatch(cmd, 1, 1, 1);
        de_gpu_barrier b = {
            .buffer = x->v->cdf, .before = DE_ACCESS_SHADER_WRITE, .after = DE_ACCESS_SHADER_READ};
        de_gpu_cmd_barriers(cmd, &b, 1);
    }
    if (x->trace) {
        de__pt_media_begin(p->media, cmd, &x->media);
        de_gpu_cmd_set_pipeline(cmd, p->pipeline);
        de_gpu_cmd_push_constants(cmd, x->pc, sizeof x->pc);
        de_gpu_cmd_dispatch_rays(cmd, x->v->width, x->v->height, 1);
        de__pt_media_end(p->media, cmd, &x->media);
    }
}
void de__pt_build(de__view_frame *f, void *user) {
    de__pt *p = user;
    pt_view *v = &p->views[(f->view.id & 255u) - 1u];
    if (!de__pt_enabled(p->r, f->settings) || f->probe_capture)
        return;
    de__rt_context *rt = de__rt_context_get(p->r, f->view, f->graph, f->frame_index);
    if (!rt)
        return;
    if (!capacity(p, v, f)) {
        f->graph->oom = true;
        return;
    }
    de__lighting_context lighting = {0};
    de__ibl_build(de__views_lighting(p->r), f->graph, &lighting);
    de__pt_media_params media;
    u64 media_signature = 0;
    if (de__pt_media_frame(p->media, f, &media, &media_signature) != DE_OK) {
        f->graph->oom = true;
        return;
    }
    const u64 signature = de__gpu_scene_source_signature(p->r->gpu_scene) ^
                          de__material_instances_epoch(p->r->material_instances) ^ media_signature;
    u32 strategy = (u32)de_cvar_int(p->strategy), seed = (u32)de_cvar_int(p->seed);
    bool golden = de_cvar_bool(p->golden);
    if (golden) {
        strategy = 0;
        seed = 0;
    }
    bool env = !v->cdf_valid || memcmp(&v->ibl, &lighting.view.ibl, sizeof v->ibl) != 0;
    if (v->strategy != strategy || v->seed != seed || !v->active ||
        v->seen_reset != v->reset_serial || v->signature != signature ||
        memcmp(&v->matrix, &f->view_matrix, sizeof v->matrix) ||
        memcmp(&v->projection, &f->proj, sizeof v->projection) ||
        !de__render_settings_equal(&v->settings, f->settings) || env)
        v->samples = 0;
    v->strategy = strategy;
    v->seed = seed;
    v->active = true;
    v->seen_reset = v->reset_serial;
    v->matrix = f->view_matrix;
    v->projection = f->proj;
    v->settings = *f->settings;
    v->signature = signature;
    v->ibl = lighting.view.ibl;
    de_gpu_buffer sources = {0};
    u32 count = 0;
    de_rg_resource source_dependency = {0};
    if (de__restir_di_sources(p->r->restir_di, f, rt, &sources, &count, &source_dependency) !=
        DE_OK) {
        f->graph->oom = true;
        return;
    }
    const u32 ring = (u32)(f->frame_index % PT_RING);
    pt_params params = {
        .query = de_gpu_buffer_bindless(p->r->dev, rt->query),
        .width = f->width,
        .height = f->height,
        .sample = v->samples,
        .max_bounces = f->settings->path_max_bounces,
        .sources = sources.id ? de_gpu_buffer_bindless(p->r->dev, sources) : 0,
        .source_count = count,
        .env_cdf = de_gpu_buffer_bindless(p->r->dev, v->cdf),
        .accumulation = de_gpu_texture_bindless(p->r->dev, v->accumulation, 0, true),
        .output = de_gpu_texture_bindless(p->r->dev, v->output, 0, true),
        .brdf_lut = de__renderer_brdf_lut(p->r),
        .seed = seed,
        .flags = strategy,
        .ibl = lighting.view.ibl,
        .exposure =
            de__lighting_view_exposure(de__views_lighting(p->r), (f->view.id & 255u) - 1u, f->view,
                                       f->frame_index, f->camera->camera.exposure_ev100, NULL),
        .light_count = de__gpu_scene_table(p->r->gpu_scene, DE__GS_LIGHTS).count};
    params.media = media;
    params.aov_albedo = de_gpu_texture_bindless(p->r->dev, v->albedo, 0, true);
    params.aov_normal = de_gpu_texture_bindless(p->r->dev, v->normal, 0, true);
    params.aov_depth = de_gpu_texture_bindless(p->r->dev, v->depth, 0, true);
    params.aov_position = de_gpu_texture_bindless(p->r->dev, v->position, 0, true);
    params.aov_material = de_gpu_texture_bindless(p->r->dev, v->material, 0, true);
    if (golden)
        params.exposure = 1;
    de_mat4 inverse = de_m4_inverse(&f->view_proj);
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            params.inv_view_proj[r][c] = inverse.m[c * 4 + r];
    params.camera[0] = f->camera->offset.x;
    params.camera[1] = f->camera->offset.y;
    params.camera[2] = f->camera->offset.z;
    memcpy((u8 *)de_gpu_buffer_map(p->r->dev, v->params) + ring * sizeof params, &params,
           sizeof params);
    de_rg *g = f->graph;
    de_rg_resource accumulation = de_rg_import_texture(g, v->accumulation, DE_ACCESS_SHADER_READ),
                   output = de_rg_import_texture(g, v->output, DE_ACCESS_SHADER_READ);
    de_rg_resource cdf = de_rg_import_buffer(g, v->cdf, DE_ACCESS_SHADER_READ);
    pt_pass *x = de_arena_push_zero(g->arena, sizeof *x, 16);
    if (!x) {
        g->oom = true;
        return;
    }
    i64 cap = de_cvar_int(p->max_samples);
    bool trace = cap <= 0 || v->samples < (u32)cap;
    *x = (pt_pass){.p = p,
                   .v = v,
                   .pc = {de_gpu_buffer_bindless(p->r->dev, v->params), ring},
                   .env = env,
                   .trace = trace,
                   .media = media};
    de_rg_pass *pass = de_rg_add_pass(g, "PT.TraceAccumulate", DE_QUEUE_GRAPHICS, execute, x);
    if (!pass) {
        g->oom = true;
        return;
    }
    if (media.features)
        de__pt_media_graph(p->media, g, pass);
    de_rg_read(pass, rt->dependency, DE_ACCESS_SHADER_READ);
    de_rg_read(pass, de_rg_import_buffer(g, rt->query, DE_ACCESS_SHADER_READ),
               DE_ACCESS_SHADER_READ);
    if (source_dependency.id)
        de_rg_read(pass, source_dependency, DE_ACCESS_SHADER_READ);
    if (lighting.strip.id)
        de_rg_read(pass, lighting.strip, DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < DE__GS_TABLE_COUNT; ++i)
        de_rg_read(pass, de__gpu_scene_import(p->r->gpu_scene, g, i), DE_ACCESS_SHADER_READ);
    if (env)
        de_rg_write(pass, cdf, DE_ACCESS_SHADER_WRITE);
    else
        de_rg_read(pass, cdf, DE_ACCESS_SHADER_READ);
    if (v->samples)
        de_rg_read(pass, accumulation, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, accumulation, DE_ACCESS_SHADER_WRITE);
    de_rg_write(pass, output, DE_ACCESS_SHADER_WRITE);
    de_rg_export(g, accumulation, DE_ACCESS_SHADER_READ);
    de_rg_export(g, output, DE_ACCESS_SHADER_READ);
    de_rg_export(g, cdf, DE_ACCESS_SHADER_READ);
    de_gpu_texture aov_textures[] = {v->albedo, v->normal, v->depth, v->position, v->material};
    for (u32 i = 0; i < 5; ++i) {
        de_rg_resource a = de_rg_import_texture(g, aov_textures[i], DE_ACCESS_SHADER_READ);
        de_rg_write(pass, a, DE_ACCESS_SHADER_WRITE);
        de_rg_export(g, a, DE_ACCESS_SHADER_READ);
    }
    f->color = output;
    if (de_cvar_bool(p->final_denoise) && !golden) {
        de__pt_info info = {.accumulation = v->accumulation,
                            .output = v->output,
                            .samples = v->samples + (trace ? 1u : 0u),
                            .width = v->width,
                            .height = v->height,
                            .albedo = v->albedo,
                            .normal = v->normal,
                            .depth = v->depth,
                            .position = v->position,
                            .material = v->material};
        de_rg_resource denoised = de__pt_denoise_build(p->denoise, f, &info, params.exposure);
        if (denoised.id)
            f->color = denoised;
    }
    f->color_format = DE_FMT_RGBA16_FLOAT;
    v->pending = true;
    v->pending_frame = f->frame_index;
    v->frame = trace ? 1 : 0;
    p->r->stats.rays_traced += (u64)f->width * f->height * f->settings->path_max_bounces;
}
void de__pt_complete(de__pt *p, de_view view, u64 frame, bool success) {
    if (!p)
        return;
    pt_view *v = &p->views[(view.id & 255u) - 1u];
    if (v->handle.id != view.id)
        return;
    if (v->pending && v->pending_frame == frame) {
        if (success) {
            v->samples += (u32)v->frame;
            v->cdf_valid = true;
        } else {
            v->samples = 0;
            v->cdf_valid = false;
        }
        v->pending = false;
    } else
        v->active = false;
}
u32 de_renderer_path_samples(const de_renderer *r, de_view view) {
    de__pt_info info;
    return de__pt_get(r, view, &info) ? info.samples : 0;
}
void de_renderer_path_reset(de_renderer *r, de_view view) {
    u32 slot = (view.id & 255u) - 1u;
    if (r && r->pt && slot < 32 && r->pt->views[slot].handle.id == view.id)
        ++r->pt->views[slot].reset_serial;
}
bool de__pt_get(const de_renderer *r, de_view view, de__pt_info *out) {
    u32 slot = (view.id & 255u) - 1u;
    if (!r || !r->pt || slot >= 32 || !de__view_valid(r, view) ||
        r->pt->views[slot].handle.id != view.id)
        return false;
    pt_view *v = &r->pt->views[slot];
    *out = (de__pt_info){.accumulation = v->accumulation,
                         .output = v->output,
                         .samples = v->samples,
                         .width = v->width,
                         .height = v->height,
                         .albedo = v->albedo,
                         .normal = v->normal,
                         .depth = v->depth,
                         .position = v->position,
                         .material = v->material};
    return true;
}

void de__pt_forget(de__pt *p, de_view view) {
    u32 slot = (view.id & 255u) - 1u;
    if (p && slot < 32 && p->views[slot].handle.id == view.id) {
        release(p, &p->views[slot]);
        de__pt_denoise_forget(p->denoise, view);
    }
}
