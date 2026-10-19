/* Shared, deterministic render-graph denoising. Signal policy belongs to the callers. */
#include "render/denoise/denoise_internal.h"
#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"
#include "render/shaders/engine_shaders.h"
#include <string.h>
#include <math.h>

typedef struct dn_params {
    u32 width, height, input, depth, normal, motion, material, hit;
    u32 previous, previous_moments, previous_length, previous_depth, previous_normal,
        previous_material;
    u32 output, moments, length, validity, variance, out_depth, out_normal, out_material;
    u32 reset, linear_depth, clamp_mode, step, scene, slot, visible, reserved;
    f32 alpha_min, depth_relative, normal_cos, normal_power, luminance_scale, hit_scale,
        roughness_threshold, unused;
    f32 inverse[4][4], previous_inverse[4][4];
    f32 camera[4], origin_delta[4];
    u32 key, previous_key, out_key, zero_radius_passthrough;
    u32 position, previous_position, out_position, rgb_variance;
    u32 temporal_alpha, normal_world, channels, out_variance;
} dn_params;
DE_STATIC_ASSERT(sizeof(dn_params) == 360, "DenoiseParams structured-buffer layout");
DE_STATIC_ASSERT(offsetof(dn_params, channels) == 352, "DenoiseParams channel semantics offset");
struct de_denoise_ctx {
    struct de_denoise_ctx *next;
    de__denoise *owner;
    de_view view;
    de_denoise_signal_desc desc;
    de_denoise_frame frame;
    de_gpu_texture history[2], moments[2], length[2], depth[2], normal[2], material[2], key[2],
        position[2];
    de_gpu_buffer params;
    u32 width, height, ping, passes;
    u64 last_frame, reset_version;
    bool valid, reset;
};
struct de__denoise {
    de_renderer *r;
    de_denoise_ctx *views[32];
    de_gpu_pipeline reproject, temporal, variance, atrous, material;
    de_cvar debug, reset;
    de_gpu_buffer material_params;
    const de_rg *material_graph[32];
    u32 material_generation[32];
    de_rg_resource material_identity[32];
};
typedef struct dn_pass {
    de__denoise *owner;
    de_gpu_pipeline pipeline;
    de_gpu_buffer params;
    dn_params pc;
    u32 entry;
    de_rg_resource input, depth, normal, motion, material, hit;
    de_rg_resource previous, previous_moments, previous_length, previous_depth, previous_normal,
        previous_material;
    de_rg_resource output, moments, length, validity, variance, out_depth, out_normal, out_material;
    de_rg_resource scene, visible;
    de_rg_resource key, previous_key, out_key, position, previous_position, out_position;
    de_rg_resource temporal_alpha, out_variance;
} dn_pass;
static void release(de_denoise_ctx *c) {
    de_gpu_device *d = c->owner->r->dev;
    for (u32 i = 0; i < 2; ++i) {
        de_gpu_texture_destroy(d, c->history[i]);
        de_gpu_texture_destroy(d, c->moments[i]);
        de_gpu_texture_destroy(d, c->length[i]);
        de_gpu_texture_destroy(d, c->depth[i]);
        de_gpu_texture_destroy(d, c->normal[i]);
        de_gpu_texture_destroy(d, c->material[i]);
        de_gpu_texture_destroy(d, c->key[i]);
        de_gpu_texture_destroy(d, c->position[i]);
    }
    de_gpu_buffer_destroy(d, c->params);
}
void de__denoise_forget(de__denoise *s, de_view view) {
    if (!s)
        return;
    /* Graph pointers may be recycled when any view is retired. */
    for (u32 i = 0; i < 32; ++i)
        s->material_graph[i] = NULL;
    u32 slot = (view.id & 255u) - 1;
    if (slot >= 32)
        return;
    de_denoise_ctx **next = &s->views[slot];
    while (*next) {
        de_denoise_ctx *c = *next;
        if (c->view.id == view.id) {
            *next = c->next;
            release(c);
            DE_FREE(s->r->alloc, c, sizeof *c);
        } else
            next = &c->next;
    }
}
void de__denoise_destroy(de__denoise *s) {
    if (!s)
        return;
    for (u32 i = 0; i < 32; ++i)
        while (s->views[i])
            de__denoise_forget(s, s->views[i]->view);
    de_gpu_pipeline_destroy(s->r->dev, s->reproject);
    de_gpu_pipeline_destroy(s->r->dev, s->temporal);
    de_gpu_pipeline_destroy(s->r->dev, s->variance);
    de_gpu_pipeline_destroy(s->r->dev, s->atrous);
    de_gpu_pipeline_destroy(s->r->dev, s->material);
    de_gpu_buffer_destroy(s->r->dev, s->material_params);
    DE_FREE(s->r->alloc, s, sizeof *s);
}
de_result de__denoise_create(de_renderer *r, de__denoise **out) {
    de__denoise *s = DE_ALLOC(r->alloc, sizeof *s, 16, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "denoiser owner");
    memset(s, 0, sizeof *s);
    s->r = r;
    *out = s;
    s->debug = de_cvar_register(
        &(de_cvar_desc){.name = "r.denoise.debug", .type = DE_CVAR_INT, .min = 0, .max = 2});
    s->reset = de_cvar_register(&(de_cvar_desc){
        .name = "r.denoise.reset", .type = DE_CVAR_INT, .min = 0, .max = 2147483647});
    return DE_OK;
}
static de_result pipeline(de__denoise *s, de_gpu_pipeline *out, const char *name,
                          const char *entry) {
    if (out->id)
        return DE_OK;
    de__engine_shader_code code = {0};
    char why[256];
    const char *entries[] = {entry};
    DE_TRY(de__engine_shader_load(s->r->dev, s->r->alloc, name, 0, entries, 1, &code, why,
                                  sizeof why));
    de_gpu_shader_blob shader = code.entries[0].blob;
    *out = de_gpu_pipeline_create(s->r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                                     .cs = shader,
                                                                     .push_constant_bytes = 8,
                                                                     .threadgroup_size = {8, 8, 1},
                                                                     .name = name});
    de__engine_shader_code_free(&code);
    return out->id ? DE_OK : DE_FAIL(DE_ERR_UNSUPPORTED, "denoiser pipeline %s", name);
}
static de_gpu_texture texture(de__denoise *s, u32 width, u32 height, de_gpu_format format,
                              const char *name) {
    return de_gpu_texture_create(
        s->r->dev,
        &(de_gpu_texture_desc){.width = width,
                               .height = height,
                               .depth_or_layers = 1,
                               .mip_count = 1,
                               .format = format,
                               .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                               .name = name});
}
static de_result allocate(de_denoise_ctx *c, u32 w, u32 h) {
    if (c->width == w && c->height == h && c->params.id)
        return DE_OK;
    release(c);
    memset(c->history, 0, sizeof c->history);
    memset(c->moments, 0, sizeof c->moments);
    memset(c->length, 0, sizeof c->length);
    memset(c->depth, 0, sizeof c->depth);
    memset(c->normal, 0, sizeof c->normal);
    memset(c->material, 0, sizeof c->material);
    memset(c->key, 0, sizeof c->key);
    memset(c->position, 0, sizeof c->position);
    c->params = (de_gpu_buffer){0};
    c->valid = false;
    c->width = w;
    c->height = h;
    for (u32 i = 0; i < 2; ++i) {
        c->history[i] = texture(c->owner, w, h, DE_FMT_RGBA32_FLOAT, "Denoise history");
        c->moments[i] = texture(c->owner, w, h, DE_FMT_RG32_FLOAT, "Denoise moments");
        c->length[i] = texture(c->owner, w, h, DE_FMT_R32_FLOAT, "Denoise age");
        c->depth[i] = texture(c->owner, w, h, DE_FMT_R32_FLOAT, "Denoise previous depth");
        c->normal[i] = texture(c->owner, w, h, DE_FMT_RGBA32_FLOAT, "Denoise previous encoded GB1");
        c->material[i] =
            texture(c->owner, w, h, DE_FMT_RG32_FLOAT, "Denoise previous material identity");
        c->key[i] = texture(c->owner, w, h, DE_FMT_RG32_FLOAT, "Denoise previous validation key");
        c->position[i] = texture(c->owner, w, h, DE_FMT_RGBA32_FLOAT,
                                 "Denoise previous representative position");
        if (!c->key[i].id || !c->position[i].id || !c->history[i].id || !c->moments[i].id ||
            !c->length[i].id || !c->depth[i].id || !c->normal[i].id || !c->material[i].id)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "denoiser histories");
    }
    c->params = de_gpu_buffer_create(
        c->owner->r->dev, &(de_gpu_buffer_desc){.size = 8 * 16 * sizeof(dn_params),
                                                .stride = sizeof(dn_params),
                                                .usage = DE_USAGE_STORAGE,
                                                .memory = DE_MEM_UPLOAD,
                                                .name = "Immutable denoiser pass parameters"});
    return c->params.id ? DE_OK : DE_FAIL(DE_ERR_OUT_OF_MEMORY, "denoiser parameters");
}
de_vec2 de__denoise_previous_uv(de_vec2 uv, de_vec2 motion) {
    return (de_vec2){uv.x - .5f * motion.x, uv.y + .5f * motion.y};
}
de_result denoise_begin(de__denoise *s, const de_denoise_frame *f,
                        const de_denoise_signal_desc *desc, de_denoise_ctx **out) {
    if (!s || !f || !desc || !out || !f->graph || !f->inputs.width || !f->inputs.height ||
        !f->inputs.material.id)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "denoise_begin inputs");
    u32 slot = (f->view.id & 255u) - 1;
    if (slot >= 32)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "denoiser view");
    de_denoise_ctx *c = s->views[slot];
    while (c && c->view.id == f->view.id && c->desc.identity != desc->identity)
        c = c->next;
    if (c && c->view.id != f->view.id) {
        while (s->views[slot])
            de__denoise_forget(s, s->views[slot]->view);
        c = NULL;
    }
    if (!c) {
        c = DE_ALLOC(s->r->alloc, sizeof *c, 16, DE_MEM_RENDER);
        if (!c)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "denoiser signal");
        memset(c, 0, sizeof *c);
        c->owner = s;
        c->view = f->view;
        c->next = s->views[slot];
        s->views[slot] = c;
    }
    bool channel_change = c->valid && c->desc.channels != desc->channels;
    c->desc = *desc;
    if (!c->desc.format)
        c->desc.format = DE_FMT_RGBA16_FLOAT;
    if (!c->desc.depth_relative)
        c->desc.depth_relative = .01f;
    if (!c->desc.normal_cos)
        c->desc.normal_cos = .906307787f;
    if (!c->desc.normal_power)
        c->desc.normal_power = 32;
    if (!c->desc.luminance_scale)
        c->desc.luminance_scale = 4;
    if (!c->desc.roughness_threshold)
        c->desc.roughness_threshold = 1;
    c->desc.iterations = c->desc.iterations < 1   ? 1
                         : c->desc.iterations > 5 ? 5
                                                  : c->desc.iterations;
    DE_TRY(allocate(c, f->inputs.width, f->inputs.height));
    c->reset = f->reset || channel_change || !c->valid || f->frame != c->last_frame + 1 ||
               c->reset_version != (u64)de_cvar_int(s->reset);
    c->reset_version = (u64)de_cvar_int(s->reset);
    c->frame = *f;
    c->passes = 0;
    *out = c;
    return DE_OK;
}
static u32 tex(dn_pass *x, const de_rg_pass *pass, de_rg_resource resource, bool storage) {
    return resource.id ? de_gpu_texture_bindless(x->owner->r->dev, de_rg_texture(pass, resource), 0,
                                                 storage)
                       : 0;
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    dn_pass *x = user;
    dn_params p = x->pc;
#define READ(name)  p.name = tex(x, pass, x->name, false)
#define WRITE(name) p.name = tex(x, pass, x->name, true)
    if (x->pipeline.id == x->owner->material.id) {
        p.input = de_gpu_buffer_bindless(x->owner->r->dev, de_rg_buffer(pass, x->input));
    } else {
        READ(input);
    }
    READ(depth);
    READ(normal);
    READ(motion);
    READ(material);
    READ(hit);
    READ(previous);
    READ(previous_moments);
    READ(previous_length);
    READ(previous_depth);
    READ(previous_normal);
    READ(previous_material);
    READ(key);
    READ(previous_key);
    READ(position);
    READ(previous_position);
    READ(temporal_alpha);
    WRITE(output);
    if (x->pipeline.id == x->owner->variance.id) {
        READ(moments);
        READ(length);
    } else {
        WRITE(moments);
        WRITE(length);
    }
    WRITE(validity);
    if (x->pipeline.id == x->owner->atrous.id)
        READ(variance);
    else
        WRITE(variance);
    WRITE(out_depth);
    WRITE(out_normal);
    WRITE(out_material);
    WRITE(out_key);
    WRITE(out_position);
    WRITE(out_variance);
#undef READ
#undef WRITE
    if (x->scene.id)
        p.scene = de_gpu_buffer_bindless(x->owner->r->dev, de_rg_buffer(pass, x->scene));
    if (x->visible.id)
        p.visible = de_gpu_buffer_bindless(x->owner->r->dev, de_rg_buffer(pass, x->visible));
    memcpy((u8 *)de_gpu_buffer_map(x->owner->r->dev, x->params) + x->entry * sizeof p, &p,
           sizeof p);
    u32 push[2] = {de_gpu_buffer_bindless(x->owner->r->dev, x->params), x->entry};
    de_gpu_cmd_set_pipeline(cmd, x->pipeline);
    de_gpu_cmd_push_constants(cmd, push, sizeof push);
    de_gpu_cmd_dispatch(cmd, (p.width + 7) / 8, (p.height + 7) / 8, 1);
}
static void declarations(de_rg_pass *pass, dn_pass *x) {
    const de_rg_resource reads[] = {x->input,           x->depth,
                                    x->normal,          x->motion,
                                    x->material,        x->hit,
                                    x->previous,        x->previous_moments,
                                    x->previous_length, x->previous_depth,
                                    x->previous_normal, x->previous_material,
                                    x->scene,           x->visible};
    const de_rg_resource writes[] = {x->output,   x->moments,   x->length,     x->validity,
                                     x->variance, x->out_depth, x->out_normal, x->out_material};
    for (u32 i = 0; i < sizeof(reads) / sizeof(reads[0]); ++i)
        if (reads[i].id)
            de_rg_read(pass, reads[i], DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < sizeof(writes) / sizeof(writes[0]); ++i)
        if (writes[i].id) {
            bool read = (x->pipeline.id == x->owner->variance.id && (i == 1 || i == 2)) ||
                        (x->pipeline.id == x->owner->atrous.id && i == 4);
            if (read)
                de_rg_read(pass, writes[i], DE_ACCESS_SHADER_READ);
            else
                de_rg_write(pass, writes[i], DE_ACCESS_SHADER_WRITE);
        }
    if (x->out_variance.id)
        de_rg_write(pass, x->out_variance, DE_ACCESS_SHADER_WRITE);
    if (x->temporal_alpha.id)
        de_rg_read(pass, x->temporal_alpha, DE_ACCESS_SHADER_READ);
    if (x->key.id)
        de_rg_read(pass, x->key, DE_ACCESS_SHADER_READ);
    if (x->previous_key.id)
        de_rg_read(pass, x->previous_key, DE_ACCESS_SHADER_READ);
    if (x->position.id)
        de_rg_read(pass, x->position, DE_ACCESS_SHADER_READ);
    if (x->previous_position.id)
        de_rg_read(pass, x->previous_position, DE_ACCESS_SHADER_READ);
    if (x->out_key.id)
        de_rg_write(pass, x->out_key, DE_ACCESS_SHADER_WRITE);
    if (x->out_position.id)
        de_rg_write(pass, x->out_position, DE_ACCESS_SHADER_WRITE);
    de_rg_pass_side_effect(pass);
}
static de_rg_resource transient(de_denoise_ctx *c, de_gpu_format format, const char *name) {
    return de_rg_create_texture(
        c->frame.graph,
        &(de_gpu_texture_desc){.width = c->width,
                               .height = c->height,
                               .depth_or_layers = 1,
                               .mip_count = 1,
                               .format = format,
                               .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                               .name = name});
}
static de_rg_resource imported(de_denoise_ctx *c, de_gpu_texture t) {
    return de_rg_import_texture(c->frame.graph, t, DE_ACCESS_SHADER_READ);
}
static dn_pass *add(de_denoise_ctx *c, de_gpu_pipeline pipeline, const char *name) {
    de_rg *g = c->frame.graph;
    dn_pass *x = de_arena_push_zero(g->arena, sizeof *x, 16);
    if (!x || c->passes >= 16) {
        g->oom = true;
        return NULL;
    }
    x->owner = c->owner;
    x->pipeline = pipeline;
    x->params = c->params;
    x->entry = (u32)(c->frame.frame % 8) * 16 + c->passes++;
    x->pc.width = c->width;
    x->pc.height = c->height;
    x->pc.alpha_min = c->desc.alpha_min;
    x->pc.depth_relative = c->desc.depth_relative;
    x->pc.normal_cos = c->desc.normal_cos;
    x->pc.normal_power = c->desc.normal_power;
    x->pc.luminance_scale = c->desc.luminance_scale;
    x->pc.hit_scale = c->desc.hit_distance_scale;
    x->pc.roughness_threshold = c->desc.roughness_threshold;
    x->pc.clamp_mode = c->desc.clamp;
    x->pc.channels = c->desc.channels;
    x->pc.rgb_variance = c->desc.rgb_variance;
    x->pc.reset = c->reset;
    x->pc.linear_depth = c->frame.inputs.linear_depth;
    x->pc.normal_world = c->frame.inputs.normal_is_world;
    x->pc.zero_radius_passthrough = c->desc.zero_radius_passthrough;
    x->key = c->frame.inputs.validation_key;
    x->position = c->frame.inputs.position;
    x->temporal_alpha = c->frame.inputs.temporal_alpha;
    for (u32 r = 0; r < 4; ++r)
        for (u32 col = 0; col < 4; ++col) {
            x->pc.inverse[r][col] = c->frame.inputs.inverse_view_proj.m[col * 4 + r];
            x->pc.previous_inverse[r][col] =
                c->frame.inputs.previous_inverse_view_proj.m[col * 4 + r];
        }
    memcpy(x->pc.camera, &c->frame.inputs.camera_position, 12);
    memcpy(x->pc.origin_delta, &c->frame.inputs.previous_origin_delta, 12);
    x->depth = c->frame.inputs.depth;
    x->normal = c->frame.inputs.normal;
    x->motion = c->frame.inputs.motion;
    x->material = c->frame.inputs.material;
    x->hit = c->frame.inputs.hit_distance;
    de_rg_pass *pass = de_rg_add_pass(g, name, DE_QUEUE_GRAPHICS, execute, x);
    if (!pass) {
        g->oom = true;
        return NULL;
    }
    return x;
}
static void finish(de_denoise_ctx *c, dn_pass *x) {
    de_rg_pass *pass = c->frame.graph->passes[c->frame.graph->pass_count - 1];
    declarations(pass, x);
    de_rg_read(pass, de_rg_import_buffer(c->frame.graph, c->params, DE_ACCESS_SHADER_READ),
               DE_ACCESS_SHADER_READ);
}
de_denoise_result denoise_add_reproject(de_denoise_ctx *c) {
    de_denoise_result r = {0};
    if (!c ||
        pipeline(c->owner, &c->owner->reproject, "denoise_reproject", "cs_reproject") != DE_OK)
        return r;
    r.color = transient(c, DE_FMT_RGBA32_FLOAT, "Denoise reprojected color");
    r.moments = transient(c, DE_FMT_RG32_FLOAT, "Denoise reprojected moments");
    r.history_length = transient(c, DE_FMT_R32_FLOAT, "Denoise reprojected age");
    r.validity = transient(c, DE_FMT_R8_UNORM, "Denoise validity");
    dn_pass *x = add(c, c->owner->reproject, "Denoise.Reproject");
    if (!x)
        return (de_denoise_result){0};
    x->previous = imported(c, c->history[c->ping]);
    x->previous_moments = imported(c, c->moments[c->ping]);
    x->previous_length = imported(c, c->length[c->ping]);
    x->previous_depth = imported(c, c->depth[c->ping]);
    x->previous_normal = imported(c, c->normal[c->ping]);
    x->previous_material = imported(c, c->material[c->ping]);
    if (c->frame.inputs.validation_key.id)
        x->previous_key = imported(c, c->key[c->ping]);
    if (c->frame.inputs.position.id)
        x->previous_position = imported(c, c->position[c->ping]);
    x->output = r.color;
    x->moments = r.moments;
    x->length = r.history_length;
    x->validity = r.validity;
    finish(c, x);
    return r;
}
de_denoise_result denoise_add_temporal(de_denoise_ctx *c, de_denoise_result previous) {
    if (!c || !previous.color.id ||
        pipeline(c->owner, &c->owner->temporal, "denoise_temporal", "cs_temporal") != DE_OK ||
        pipeline(c->owner, &c->owner->variance, "denoise_variance", "cs_variance") != DE_OK)
        return (de_denoise_result){0};
    u32 next = c->ping ^ 1;
    de_denoise_result out = previous;
    out.color = imported(c, c->history[next]);
    out.moments = imported(c, c->moments[next]);
    out.variance = transient(c, DE_FMT_R32_FLOAT, "Denoise variance");
    dn_pass *x = add(c, c->owner->temporal, "Denoise.Temporal");
    if (!x)
        return (de_denoise_result){0};
    x->input = c->frame.inputs.signal;
    x->previous = previous.color;
    x->previous_moments = previous.moments;
    x->previous_length = previous.history_length;
    x->output = out.color;
    x->moments = out.moments;
    x->length = imported(c, c->length[next]);
    x->out_depth = imported(c, c->depth[next]);
    x->out_normal = imported(c, c->normal[next]);
    x->out_material = imported(c, c->material[next]);
    if (c->frame.inputs.validation_key.id)
        x->out_key = imported(c, c->key[next]);
    if (c->frame.inputs.position.id)
        x->out_position = imported(c, c->position[next]);
    finish(c, x);
    x = add(c, c->owner->variance, "Denoise.Variance");
    if (!x)
        return (de_denoise_result){0};
    x->input = out.color;
    x->moments = out.moments;
    x->length = imported(c, c->length[next]);
    x->variance = out.variance;
    finish(c, x);
    de_rg_pass *ready =
        de_rg_add_pass(c->frame.graph, "Denoise.HistoryReady", DE_QUEUE_GRAPHICS, NULL, NULL);
    if (ready) {
        de_rg_read(ready, imported(c, c->history[next]), DE_ACCESS_SHADER_READ);
        de_rg_read(ready, imported(c, c->moments[next]), DE_ACCESS_SHADER_READ);
        de_rg_read(ready, imported(c, c->length[next]), DE_ACCESS_SHADER_READ);
        de_rg_read(ready, imported(c, c->depth[next]), DE_ACCESS_SHADER_READ);
        de_rg_read(ready, imported(c, c->normal[next]), DE_ACCESS_SHADER_READ);
        de_rg_read(ready, imported(c, c->material[next]), DE_ACCESS_SHADER_READ);
        if (c->frame.inputs.validation_key.id)
            de_rg_read(ready, imported(c, c->key[next]), DE_ACCESS_SHADER_READ);
        if (c->frame.inputs.position.id)
            de_rg_read(ready, imported(c, c->position[next]), DE_ACCESS_SHADER_READ);
        de_rg_pass_side_effect(ready);
    } else
        c->frame.graph->oom = true;
    c->ping = next;
    c->last_frame = c->frame.frame;
    c->valid = true;
    return out;
}
de_denoise_result denoise_add_atrous(de_denoise_ctx *c, de_denoise_result r) {
    if (!c || !r.color.id ||
        pipeline(c->owner, &c->owner->atrous, "denoise_atrous", "cs_atrous") != DE_OK)
        return (de_denoise_result){0};
    for (u32 i = 0; i < c->desc.iterations; ++i) {
        dn_pass *x = add(c, c->owner->atrous, "Denoise.Atrous");
        if (!x)
            return (de_denoise_result){0};
        x->pc.step = 1u << i;
        x->input = r.color;
        x->variance = r.variance;
        x->output = transient(c, i + 1 == c->desc.iterations ? c->desc.format : DE_FMT_RGBA32_FLOAT,
                              "Denoise filtered signal");
        if (c->desc.propagate_variance) {
            x->out_variance =
                transient(c, c->desc.rgb_variance ? DE_FMT_RGBA32_FLOAT : DE_FMT_R32_FLOAT,
                          "Denoise filtered variance");
            r.variance = x->out_variance;
        }
        r.color = x->output;
        finish(c, x);
    }
    i64 debug = de_cvar_int(c->owner->debug);
    if (debug == 1)
        r.color = r.history_length;
    else if (debug == 2)
        r.color = r.validity;
    return r;
}
de_rg_resource denoise_add_material_identity(de__denoise *s, de_rg *g, u32 w, u32 h,
                                             de_rg_resource visibility, de_rg_resource visible,
                                             de_rg_resource scene, u32 slot, u32 pitch) {
    if (s && g && slot < 32 && s->material_graph[slot] == g &&
        s->material_generation[slot] == g->gen && s->material_identity[slot].id)
        return s->material_identity[slot];
    if (slot >= 32)
        return (de_rg_resource){0};
    if (!s || !g || pipeline(s, &s->material, "denoise_reproject", "cs_material") != DE_OK)
        return (de_rg_resource){0};
    if (!s->material_params.id)
        s->material_params = de_gpu_buffer_create(
            s->r->dev, &(de_gpu_buffer_desc){.size = 8 * 32 * sizeof(dn_params),
                                             .stride = sizeof(dn_params),
                                             .usage = DE_USAGE_STORAGE,
                                             .memory = DE_MEM_UPLOAD});
    dn_pass *x = de_arena_push_zero(g->arena, sizeof *x, 16);
    if (!x || !s->material_params.id) {
        g->oom = true;
        return (de_rg_resource){0};
    }
    x->owner = s;
    x->pipeline = s->material;
    x->params = s->material_params;
    x->entry = (u32)(s->r->frame_index % 8) * 32 + slot;
    x->pc.width = w;
    x->pc.height = h;
    x->pc.slot = slot;
    x->pc.reserved = pitch;
    x->scene = scene;
    x->visible = visible;
    x->input = visibility;
    x->output = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.width = w,
                                  .height = h,
                                  .depth_or_layers = 1,
                                  .mip_count = 1,
                                  .format = DE_FMT_RG32_FLOAT,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                  .name = "Canonical denoiser material identity"});
    de_rg_pass *pass = de_rg_add_pass(g, "Denoise.MaterialIdentity", DE_QUEUE_GRAPHICS, execute, x);
    if (!pass) {
        g->oom = true;
        return (de_rg_resource){0};
    }
    declarations(pass, x);
    de_rg_read(pass, de_rg_import_buffer(g, s->material_params, DE_ACCESS_SHADER_READ),
               DE_ACCESS_SHADER_READ);
    s->material_graph[slot] = g;
    s->material_generation[slot] = g->gen;
    s->material_identity[slot] = x->output;
    return x->output;
}
