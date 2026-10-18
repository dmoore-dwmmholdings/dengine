#include "render/denoise/denoise_policy.h"
#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"
#include "render/passes/vis/gbuffer.h"
#include "render/shaders/engine_shaders.h"
#include <math.h>
#include <string.h>
typedef struct policy_params {
    u32 width, height, full_width, full_height, scale, slot, kind, quality;
    u32 scene, scene_slot, raw, distance, keys, depth, normal, motion;
    u32 material, lights, directions, source, output, key, out_depth, out_normal;
    u32 out_motion, out_material, position, radius, out_position, hit_value, r0, r1;
    f32 radius_limit, p0, p1, p2;
} policy_params;
DE_STATIC_ASSERT(sizeof(policy_params) == 144, "PolicyParams layout");
struct de__signal_denoiser {
    de_renderer *r;
    de_gpu_pipeline prepare, prefilter, publish;
    de_gpu_buffer params[32];
    de_dvec3 origin[32];
    de_view handle[32];
    u64 frame[32];
};
typedef struct policy_pass {
    de__signal_denoiser *owner;
    policy_params pc;
    de_gpu_pipeline pipeline;
    de_gpu_buffer params;
    u32 entry;
    de_rg_resource scene, raw, distance, keys, depth, normal, motion, material, lights, directions,
        source, output, key, out_depth, out_normal, out_motion, out_material, position, radius,
        out_position;
} policy_pass;
f32 de__shadow_penumbra(f32 source, f32 blocker, f32 distance, f32 ppm, bool directional) {
    if (!(source > 0) || !(blocker > 0))
        return 0;
    return directional ? blocker * tanf(fminf(source, 1.55f)) * ppm
                       : source * blocker / fmaxf(distance - blocker, .0001f) * ppm;
}
void de__signal_denoiser_destroy(de__signal_denoiser *s) {
    if (!s)
        return;
    for (u32 i = 0; i < 32; ++i)
        de_gpu_buffer_destroy(s->r->dev, s->params[i]);
    de_gpu_pipeline_destroy(s->r->dev, s->prepare);
    de_gpu_pipeline_destroy(s->r->dev, s->prefilter);
    de_gpu_pipeline_destroy(s->r->dev, s->publish);
    DE_FREE(s->r->alloc, s, sizeof *s);
}
de_result de__signal_denoiser_create(de_renderer *r, de__signal_denoiser **out) {
    de__signal_denoiser *s = DE_ALLOC(r->alloc, sizeof *s, 16, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "signal denoiser");
    memset(s, 0, sizeof *s);
    s->r = r;
    *out = s;
    return DE_OK;
}
static de_result pipelines(de__signal_denoiser *s) {
    if (s->publish.id)
        return DE_OK;
    const char *names[] = {"denoise_shadow_prefilter", "denoise_shadow_publish"};
    const char *entries[] = {"cs_prepare", "cs_prefilter", "cs_publish"};
    de_gpu_pipeline *p[] = {&s->prepare, &s->prefilter, &s->publish};
    for (u32 i = 0; i < 3; ++i) {
        de__engine_shader_code code = {0};
        char why[256];
        const char *entry[] = {entries[i]};
        DE_TRY(de__engine_shader_load(s->r->dev, s->r->alloc, names[i == 2 ? 1 : 0], 0, entry, 1,
                                      &code, why, sizeof why));
        *p[i] =
            de_gpu_pipeline_create(s->r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                                      .cs = code.entries[0].blob,
                                                                      .push_constant_bytes = 8,
                                                                      .threadgroup_size = {8, 8, 1},
                                                                      .name = entries[i]});
        de__engine_shader_code_free(&code);
        if (!p[i]->id)
            return DE_FAIL(DE_ERR_UNSUPPORTED, "signal denoiser shaders");
    }
    return DE_OK;
}
static u32 sampled(policy_pass *x, const de_rg_pass *p, de_rg_resource r) {
    return r.id ? de_gpu_texture_bindless(x->owner->r->dev, de_rg_texture(p, r), 0, false) : 0;
}
static u32 storage(policy_pass *x, const de_rg_pass *p, de_rg_resource r) {
    return r.id ? de_gpu_texture_bindless(x->owner->r->dev, de_rg_texture(p, r), 0, true) : 0;
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    policy_pass *x = user;
    policy_params pc = x->pc;
    de_gpu_device *d = x->owner->r->dev;
#define R(name) pc.name = sampled(x, pass, x->name)
#define W(name) pc.name = storage(x, pass, x->name)
    R(raw);
    R(distance);
    R(depth);
    R(normal);
    R(motion);
    R(material);
    R(source);
    R(position);
    W(output);
    W(out_depth);
    W(out_normal);
    W(out_motion);
    W(out_material);
    W(out_position);
    if (x->pipeline.id == x->owner->prepare.id) {
        W(key);
        W(radius);
    } else {
        R(key);
        R(radius);
    }
#undef R
#undef W
#define B(name)                                                                                    \
    if (x->name.id)                                                                                \
    pc.name = de_gpu_buffer_bindless(d, de_rg_buffer(pass, x->name))
    B(scene);
    B(keys);
    B(lights);
    B(directions);
#undef B
    memcpy((u8 *)de_gpu_buffer_map(d, x->params) + x->entry * sizeof pc, &pc, sizeof pc);
    u32 push[] = {de_gpu_buffer_bindless(d, x->params), x->entry};
    de_gpu_cmd_set_pipeline(cmd, x->pipeline);
    de_gpu_cmd_push_constants(cmd, push, sizeof push);
    de_gpu_cmd_dispatch(cmd, (pc.width + 7) / 8, (pc.height + 7) / 8, 1);
}
static de_rg_resource temp(de_rg *g, u32 w, u32 h, de_gpu_format format, const char *name) {
    return de_rg_create_texture(
        g, &(de_gpu_texture_desc){.width = w,
                                  .height = h,
                                  .depth_or_layers = 1,
                                  .mip_count = 1,
                                  .format = format,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                  .name = name});
}
static void add(de_rg *g, policy_pass *x, const char *name) {
    de_rg_pass *p = de_rg_add_pass(g, name, DE_QUEUE_GRAPHICS, execute, x);
    if (!p) {
        g->oom = true;
        return;
    }
    const de_rg_resource reads[] = {x->scene,  x->raw,        x->distance, x->keys,
                                    x->depth,  x->normal,     x->motion,   x->material,
                                    x->lights, x->directions, x->source,   x->position};
    for (u32 i = 0; i < sizeof reads / sizeof reads[0]; ++i)
        if (reads[i].id)
            de_rg_read(p, reads[i], DE_ACCESS_SHADER_READ);
    const de_rg_resource writes[] = {x->output,     x->out_depth,    x->out_normal,
                                     x->out_motion, x->out_material, x->out_position};
    for (u32 i = 0; i < sizeof writes / sizeof writes[0]; ++i)
        if (writes[i].id)
            de_rg_write(p, writes[i], DE_ACCESS_SHADER_WRITE);
    if (x->pipeline.id == x->owner->prepare.id) {
        de_rg_write(p, x->key, DE_ACCESS_SHADER_WRITE);
        de_rg_write(p, x->radius, DE_ACCESS_SHADER_WRITE);
    } else {
        if (x->key.id)
            de_rg_read(p, x->key, DE_ACCESS_SHADER_READ);
        if (x->radius.id)
            de_rg_read(p, x->radius, DE_ACCESS_SHADER_READ);
    }
    de_rg_read(p, de_rg_import_buffer(g, x->params, DE_ACCESS_SHADER_READ), DE_ACCESS_SHADER_READ);
    de_rg_pass_side_effect(p);
}
de_result de__denoise_policy_build(de__signal_denoiser *s, de_rg *g, const de__vis_context *vc,
                                   const de__lighting_context *lc, de_rg_resource raw,
                                   de_rg_resource distance, de_rg_resource keys,
                                   de_rg_resource output, u32 width, u32 height, u32 slots,
                                   u32 scale, u32 kind, bool reference) {
    DE_TRY(pipelines(s));
    u32 view = (vc->view.id & 255u) - 1;
    if (view >= 32)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "signal view");
    if (!s->params[view].id)
        s->params[view] = de_gpu_buffer_create(
            s->r->dev, &(de_gpu_buffer_desc){.size = 8 * 16 * sizeof(policy_params),
                                             .stride = sizeof(policy_params),
                                             .usage = DE_USAGE_STORAGE,
                                             .memory = DE_MEM_UPLOAD});
    if (!s->params[view].id)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "signal parameters");
    de_rg_resource material = denoise_add_material_identity(
        s->r->denoise, g, vc->width, vc->height, vc->vis_ids_buffer, vc->visible_meshlets,
        vc->scene_views, vc->scene_view_slot, vc->vis_ids_pitch);
    if (!material.id)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "canonical denoise identities");
    de_vec3 origin_delta = {0};
    if (s->handle[view].id == vc->view.id) {
        origin_delta = (de_vec3){(f32)(s->origin[view].x - vc->origin.x),
                                 (f32)(s->origin[view].y - vc->origin.y),
                                 (f32)(s->origin[view].z - vc->origin.z)};
    }
    for (u32 slot = 0; slot < slots; ++slot) {
        policy_pass *p = de_arena_push_zero(g->arena, 3 * sizeof *p, 16);
        if (!p)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "signal graph data");
        de_rg_resource
            value = temp(g, width, height, DE_FMT_RGBA32_FLOAT, "Denoise mapped signal"),
            z = temp(g, width, height, DE_FMT_R32_FLOAT, "Denoise representative depth"),
            n = temp(g, width, height, DE_FMT_RGBA32_FLOAT, "Denoise representative GB1"),
            motion = temp(g, width, height, DE_FMT_RG32_FLOAT, "Denoise representative motion"),
            mat = temp(g, width, height, DE_FMT_RG32_FLOAT, "Denoise representative material"),
            key = temp(g, width, height, DE_FMT_RG32_FLOAT, "Denoise exact light identity"),
            position =
                temp(g, width, height, DE_FMT_RGBA32_FLOAT, "Denoise representative position"),
            radius = temp(g, width, height, DE_FMT_R32_FLOAT, "Denoise physical filter radius");
        p[0] = (policy_pass){.owner = s,
                             .pipeline = s->prepare,
                             .params = s->params[view],
                             .entry = (u32)(vc->frame_index % 8) * 16 + slot * 3,
                             .pc = {.width = width,
                                    .height = height,
                                    .full_width = vc->width,
                                    .full_height = vc->height,
                                    .scale = scale,
                                    .slot = slot,
                                    .kind = kind,
                                    .quality = kind == 0 ? vc->settings->shadows : vc->settings->ao,
                                    .scene_slot = vc->scene_view_slot},
                             .scene = vc->scene_views,
                             .raw = raw,
                             .distance = distance,
                             .keys = keys,
                             .depth = vc->depth,
                             .normal = vc->gbuffer[DE__GB1],
                             .motion = vc->gbuffer[DE__GB_MOTION],
                             .material = material,
                             .lights = lc ? lc->lights : (de_rg_resource){0},
                             .directions = lc ? lc->dir_lights : (de_rg_resource){0},
                             .output = value,
                             .key = key,
                             .out_depth = z,
                             .out_normal = n,
                             .out_motion = motion,
                             .out_material = mat,
                             .out_position = position,
                             .radius = radius};
        add(g, &p[0], kind == 0 ? "Denoise.Shadow.Guides" : "Denoise.AO.Guides");
        de_rg_resource source = value;
        if (!reference && kind == 0) {
            p[1] = (policy_pass){
                .owner = s,
                .pipeline = s->prefilter,
                .params = s->params[view],
                .entry = p[0].entry + 1,
                .pc = p[0].pc,
                .source = value,
                .key = key,
                .material = mat,
                .normal = n,
                .position = position,
                .radius = radius,
                .output = temp(g, width, height, DE_FMT_RGBA32_FLOAT, "Shadow physical prefilter")};
            add(g, &p[1], "Denoise.Shadow.Prefilter");
            source = p[1].output;
        }
        if (!reference) {
            de_denoise_ctx *context = NULL;
            de_denoise_frame frame = {
                .graph = g,
                .view = vc->view,
                .frame = vc->frame_index,
                .reset = vc->view_new || vc->occlusion_reset,
                .inputs = {.signal = source,
                           .depth = z,
                           .normal = n,
                           .motion = motion,
                           .material = mat,
                           .hit_distance = radius,
                           .width = width,
                           .height = height,
                           .inverse_view_proj = de_m4_inverse(&vc->view_proj),
                           .previous_inverse_view_proj = de_m4_inverse(&vc->prev_view_proj),
                           .camera_position = vc->camera_position,
                           .previous_origin_delta = origin_delta,
                           .validation_key = kind == 0 ? key : (de_rg_resource){0},
                           .position = position}};
            de_denoise_signal_desc desc = {
                .identity = (kind == 0 ? 0x5348445700000000ull : 0x414f000000000000ull) + slot,
                .format = DE_FMT_RGBA32_FLOAT,
                .channels = kind == 0 ? 1u : 2u,
                .alpha_min = kind == 0 ? .1f : .05f,
                .clamp = DE_DENOISE_CLAMP_LUMINANCE,
                .iterations = kind == 0 ? (vc->settings->shadows >= DE_QUALITY_HIGH ? 3u : 2u)
                                        : (vc->settings->ao >= DE_QUALITY_HIGH ? 3u : 2u),
                .hit_distance_scale = 1,
                .zero_radius_passthrough = kind == 0};
            DE_TRY(denoise_begin(s->r->denoise, &frame, &desc, &context));
            de_denoise_result result = denoise_add_reproject(context);
            result = denoise_add_temporal(context, result);
            result = denoise_add_atrous(context, result);
            if (!result.color.id)
                return DE_FAIL(DE_ERR_UNSUPPORTED, "signal denoiser blocks");
            source = result.color;
        }
        p[2] = (policy_pass){.owner = s,
                             .pipeline = s->publish,
                             .params = s->params[view],
                             .entry = p[0].entry + 2,
                             .pc = p[0].pc,
                             .source = source,
                             .output = output};
        add(g, &p[2], kind == 0 ? "Denoise.Shadow.Publish" : "Denoise.AO.Publish");
    }
    s->origin[view] = vc->origin;
    s->handle[view] = vc->view;
    s->frame[view] = vc->frame_index;
    return DE_OK;
}
de_result de__denoise_shadow(de__signal_denoiser *s, de_rg *g, const de__vis_context *vc,
                             const de__lighting_context *lc, de_rg_resource raw,
                             de_rg_resource distance, de_rg_resource keys, de_rg_resource output,
                             u32 w, u32 h, u32 slots, u32 scale, bool reference) {
    return de__denoise_policy_build(s, g, vc, lc, raw, distance, keys, output, w, h, slots, scale,
                                    0, reference);
}
