#include "render/pathtracer/pt_internal.h"
#include "render/denoise/denoise_internal.h"
#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"
#include "render/shaders/engine_shaders.h"
#include <string.h>
typedef struct pt_final_params {
    u32 width, height, accumulation, albedo, normal, depth, position, material, input, output,
        variance, moments;
    f32 exposure;
    u32 pad[3];
} pt_final_params;
DE_STATIC_ASSERT(sizeof(pt_final_params) == 64, "PtFinalParams layout");
struct de__pt_denoise {
    de_renderer *r;
    de_gpu_pipeline prepare, publish;
    de_gpu_buffer params[32];
};
typedef struct pt_final_pass {
    de__pt_denoise *s;
    de_gpu_pipeline pipeline;
    de_gpu_buffer params;
    u32 entry;
    pt_final_params pc;
    de_rg_resource accumulation, albedo, normal, depth, position, material, input, output, variance,
        moments;
} pt_final_pass;
de_result de__pt_denoise_create(de_renderer *r, de__pt_denoise **out) {
    de__pt_denoise *s = DE_ALLOC(r->alloc, sizeof *s, 16, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "PT final denoise");
    memset(s, 0, sizeof *s);
    s->r = r;
    *out = s;
    return DE_OK;
}
void de__pt_denoise_destroy(de__pt_denoise *s) {
    if (!s)
        return;
    for (u32 i = 0; i < 32; ++i)
        de_gpu_buffer_destroy(s->r->dev, s->params[i]);
    de_gpu_pipeline_destroy(s->r->dev, s->prepare);
    de_gpu_pipeline_destroy(s->r->dev, s->publish);
    DE_FREE(s->r->alloc, s, sizeof *s);
}
static de_result pipelines(de__pt_denoise *s) {
    if (s->publish.id)
        return DE_OK;
    const char *entries[] = {"cs_pt_final_prepare", "cs_pt_final_publish"};
    de__engine_shader_code code = {0};
    char why[256];
    DE_TRY(de__engine_shader_load(s->r->dev, s->r->alloc, "pt_final_denoise", 0, entries, 2, &code,
                                  why, sizeof why));
    de_gpu_pipeline *handles[] = {&s->prepare, &s->publish};
    for (u32 i = 0; i < 2; ++i)
        *handles[i] =
            de_gpu_pipeline_create(s->r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                                      .cs = code.entries[i].blob,
                                                                      .push_constant_bytes = 8,
                                                                      .threadgroup_size = {8, 8, 1},
                                                                      .name = entries[i]});
    de__engine_shader_code_free(&code);
    return s->prepare.id && s->publish.id ? DE_OK
                                          : DE_FAIL(DE_ERR_UNSUPPORTED, "PT final denoise shaders");
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    pt_final_pass *x = user;
    pt_final_params p = x->pc;
#define R(n)                                                                                       \
    if (x->n.id)                                                                                   \
    p.n = de_gpu_texture_bindless(x->s->r->dev, de_rg_texture(pass, x->n), 0, false)
    R(accumulation);
    R(albedo);
    R(normal);
    R(depth);
    R(position);
    R(material);
    R(moments);
    R(input);
#undef R
    p.output = de_gpu_texture_bindless(x->s->r->dev, de_rg_texture(pass, x->output), 0, true);
    if (x->variance.id)
        p.variance =
            de_gpu_texture_bindless(x->s->r->dev, de_rg_texture(pass, x->variance), 0, true);
    memcpy((u8 *)de_gpu_buffer_map(x->s->r->dev, x->params) + x->entry * sizeof p, &p, sizeof p);
    u32 push[] = {de_gpu_buffer_bindless(x->s->r->dev, x->params), x->entry};
    de_gpu_cmd_set_pipeline(cmd, x->pipeline);
    de_gpu_cmd_push_constants(cmd, push, sizeof push);
    de_gpu_cmd_dispatch(cmd, (p.width + 7) / 8, (p.height + 7) / 8, 1);
}
static void add(de_rg *g, pt_final_pass *x, const char *name) {
    de_rg_pass *pass = de_rg_add_pass(g, name, DE_QUEUE_GRAPHICS, execute, x);
    de_rg_resource reads[] = {x->accumulation, x->albedo,   x->normal, x->depth,
                              x->position,     x->material, x->input,  x->moments};
    for (u32 i = 0; i < sizeof reads / sizeof reads[0]; ++i)
        if (reads[i].id)
            de_rg_read(pass, reads[i], DE_ACCESS_SHADER_READ);
    de_rg_write(pass, x->output, DE_ACCESS_SHADER_WRITE);
    if (x->variance.id)
        de_rg_write(pass, x->variance, DE_ACCESS_SHADER_WRITE);
    de_rg_read(pass, de_rg_import_buffer(g, x->params, DE_ACCESS_SHADER_READ),
               DE_ACCESS_SHADER_READ);
    de_rg_pass_side_effect(pass);
}
static de_rg_resource temporary(de_rg *g, u32 w, u32 h, de_gpu_format fmt, const char *name) {
    return de_rg_create_texture(
        g, &(de_gpu_texture_desc){.width = w,
                                  .height = h,
                                  .depth_or_layers = 1,
                                  .mip_count = 1,
                                  .format = fmt,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                  .name = name});
}
de_rg_resource de__pt_denoise_build(de__pt_denoise *s, de__view_frame *f, const de__pt_info *info,
                                    f32 exposure) {
    de_rg *g = f->graph;
    if (pipelines(s) != DE_OK)
        return (de_rg_resource){0};
    u32 slot = (f->view.id & 255u) - 1u;
    if (slot >= 32)
        return (de_rg_resource){0};
    if (!s->params[slot].id)
        s->params[slot] = de_gpu_buffer_create(
            s->r->dev, &(de_gpu_buffer_desc){.size = 16 * sizeof(pt_final_params),
                                             .stride = sizeof(pt_final_params),
                                             .usage = DE_USAGE_STORAGE,
                                             .memory = DE_MEM_UPLOAD});
    if (!s->params[slot].id)
        return (de_rg_resource){0};
    pt_final_pass *x = de_arena_push_zero(g->arena, 2 * sizeof *x, 16);
    if (!x)
        return (de_rg_resource){0};
    x[0] = (pt_final_pass){
        .s = s,
        .pipeline = s->prepare,
        .params = s->params[slot],
        .entry = (u32)(f->frame_index % 8) * 2,
        .pc = {.width = info->width, .height = info->height, .exposure = exposure},
        .accumulation = de_rg_import_texture(g, info->accumulation, DE_ACCESS_SHADER_READ),
        .albedo = de_rg_import_texture(g, info->albedo, DE_ACCESS_SHADER_READ),
        .normal = de_rg_import_texture(g, info->normal, DE_ACCESS_SHADER_READ),
        .depth = de_rg_import_texture(g, info->depth, DE_ACCESS_SHADER_READ),
        .position = de_rg_import_texture(g, info->position, DE_ACCESS_SHADER_READ),
        .material = de_rg_import_texture(g, info->material, DE_ACCESS_SHADER_READ),
        .moments = de_rg_import_texture(g, info->moments, DE_ACCESS_SHADER_READ),
        .output =
            temporary(g, info->width, info->height, DE_FMT_RGBA32_FLOAT, "PT final demodulated"),
        .variance =
            temporary(g, info->width, info->height, DE_FMT_RGBA32_FLOAT, "PT final variance")};
    add(g, &x[0], "PT.FinalDenoise.Prepare");
    de_denoise_frame frame = {.graph = g,
                              .view = f->view,
                              .frame = f->frame_index,
                              .reset = true,
                              .inputs = {.signal = x[0].output,
                                         .depth = x[0].depth,
                                         .normal = x[0].normal,
                                         .material = x[0].material,
                                         .position = x[0].position,
                                         .width = info->width,
                                         .height = info->height,
                                         .linear_depth = true,
                                         .normal_is_world = true}};
    de_denoise_signal_desc desc = {.identity = 0x505446494e414cull,
                                   .format = DE_FMT_RGBA32_FLOAT,
                                   .channels = 3,
                                   .iterations = 3,
                                   .propagate_variance = true,
                                   .rgb_variance = true,
                                   .depth_relative = .02f,
                                   .normal_power = 64,
                                   .luminance_scale = 4,
                                   .roughness_threshold = 1};
    de_denoise_ctx *ctx = NULL;
    if (denoise_begin(s->r->denoise, &frame, &desc, &ctx) != DE_OK)
        return (de_rg_resource){0};
    de_denoise_result filtered = denoise_add_atrous(
        ctx, (de_denoise_result){.color = x[0].output, .variance = x[0].variance});
    if (!filtered.color.id)
        return (de_rg_resource){0};
    x[1] = x[0];
    x[1].entry++;
    x[1].pipeline = s->publish;
    x[1].input = filtered.color;
    x[1].variance = (de_rg_resource){0};
    x[1].output = temporary(g, info->width, info->height, DE_FMT_RGBA16_FLOAT,
                            "PT final exposed presentation");
    add(g, &x[1], "PT.FinalDenoise.Publish");
    return x[1].output;
}

void de__pt_denoise_forget(de__pt_denoise *s, de_view view) {
    u32 slot = (view.id & 255u) - 1u;
    if (s && slot < 32) {
        de_gpu_buffer_destroy(s->r->dev, s->params[slot]);
        s->params[slot] = (de_gpu_buffer){0};
    }
}
