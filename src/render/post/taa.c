#include "render/upscale/upscaler.h"
#include "render/renderer_internal.h"
#include "render/shaders/engine_shaders.h"
#include <math.h>
#include <string.h>

typedef struct taa_state {
    de_renderer *renderer;
    de_gpu_pipeline pipeline;
    de__upscale_size size;
    de__upscale_history history;
} taa_state;
typedef struct taa_push {
    u32 color, depth, motion, exposure;
    u32 reactive, previous_color, previous_depth, output;
    u32 history_color, history_depth, iw, ih;
    u32 ow, oh, flags, padding;
    f32 jitter[2], exposure_ratio, gamma;
    f32 history_weight, sharpen, depth_relative, reserved;
    f32 depth_coeff[4];
} taa_push;
DE_STATIC_ASSERT(sizeof(taa_push) == 112, "TaaPush ABI");

static void release_history(taa_state *s, de__upscale_history h) {
    for (u32 i = 0; i < 2; ++i) {
        de_gpu_texture_destroy(s->renderer->dev, h.color[i]);
        de_gpu_texture_destroy(s->renderer->dev, h.depth[i]);
    }
}
static de_result init(de_renderer *r, void **out) {
    *out = NULL;
    taa_state *s = DE_NEW(r->alloc, taa_state, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "TAA: backend state");
    *s = (taa_state){.renderer = r};
    de__engine_shader_code code;
    const char *entry = "cs_taa";
    char why[256];
    de_result result =
        de__engine_shader_load(r->dev, r->alloc, "post_taa", 0, &entry, 1, &code, why, sizeof why);
    if (result == DE_OK) {
        s->pipeline = de_gpu_pipeline_create(
            r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                            .cs = code.entries[0].blob,
                                            .push_constant_bytes = sizeof(taa_push),
                                            .threadgroup_size = {8, 8, 1},
                                            .name = "post.taa"});
        de__engine_shader_code_free(&code);
        if (!s->pipeline.id)
            result = DE_FAIL_WRAP(DE_ERR_INTERNAL, "TAA: compute pipeline");
    }
    if (result != DE_OK) {
        DE_FREE(r->alloc, s, sizeof *s);
        return result;
    }
    *out = s;
    return DE_OK;
}
static de_result resize(void *state, de__upscale_size size, de__upscale_history *out) {
    taa_state *s = state;
    if (!size.input_width || !size.input_height || !size.output_width || !size.output_height ||
        size.input_width > 16384 || size.input_height > 16384 || size.output_width > 16384 ||
        size.output_height > 16384)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "TAA: invalid dimensions");
    /* Input-scale changes reset accumulation, but output-sized history can be reused. */
    if (s->history.color[0].id && s->size.output_width == size.output_width &&
        s->size.output_height == size.output_height) {
        s->size = size;
        *out = s->history;
        return DE_OK;
    }
    de__upscale_history h = {0};
    de_gpu_texture_desc desc = {.width = size.output_width,
                                .height = size.output_height,
                                .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC};
    for (u32 i = 0; i < 2; ++i) {
        desc.format = DE_FMT_RGBA32_FLOAT;
        desc.name = "taa.history_color";
        h.color[i] = de_gpu_texture_create(s->renderer->dev, &desc);
        desc.format = DE_FMT_RG32_FLOAT;
        desc.name = "taa.history_depth";
        h.depth[i] = de_gpu_texture_create(s->renderer->dev, &desc);
        if (!h.color[i].id || !h.depth[i].id) {
            release_history(s, h);
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "TAA: history images");
        }
    }
    release_history(s, s->history);
    s->history = h;
    s->size = size;
    *out = h;
    return DE_OK;
}
static u32 tex(taa_state *s, de_gpu_texture t, bool write) {
    return t.id ? de_gpu_texture_bindless(s->renderer->dev, t, 0, write) : 0;
}
static void dispatch(void *state, de_gpu_cmd *cmd, const de__upscale_inputs *in,
                     const de__upscale_outputs *out) {
    taa_state *s = state;
    const f32 current = in->pre_exposure > 0 && isfinite(in->pre_exposure) ? in->pre_exposure : 1;
    const f32 previous = in->previous_pre_exposure > 0 && isfinite(in->previous_pre_exposure)
                             ? in->previous_pre_exposure
                             : current;
    de_mat4 inverse = de_m4_inverse(&in->projection);
    taa_push p = {.color = tex(s, in->color, false),
                  .depth = tex(s, in->depth, false),
                  .motion = tex(s, in->motion, false),
                  .exposure = tex(s, in->exposure, false),
                  .reactive = tex(s, in->reactive, false),
                  .previous_color = tex(s, in->history_color, false),
                  .previous_depth = tex(s, in->history_depth, false),
                  .output = tex(s, out->color, true),
                  .history_color = tex(s, out->history_color, true),
                  .history_depth = tex(s, out->history_depth, true),
                  .iw = in->size.input_width,
                  .ih = in->size.input_height,
                  .ow = in->size.output_width,
                  .oh = in->size.output_height,
                  .flags = (in->reset ? 1u : 0u) | (in->linear_depth ? 2u : 0u),
                  .jitter = {in->jitter.x, in->jitter.y},
                  .exposure_ratio = current / previous,
                  .gamma = 1.25f,
                  .history_weight = 63.f / 64,
                  .sharpen = isfinite(in->sharpen) ? fminf(1, fmaxf(0, in->sharpen)) : 0,
                  .depth_relative = .02f,
                  .depth_coeff = {inverse.m[10], inverse.m[14], inverse.m[11], inverse.m[15]}};
    de_gpu_cmd_set_pipeline(cmd, s->pipeline);
    de_gpu_cmd_push_constants(cmd, &p, sizeof p);
    de_gpu_cmd_dispatch(cmd, (p.ow + 7) / 8, (p.oh + 7) / 8, 1);
}
static void destroy(void *state) {
    taa_state *s = state;
    if (!s)
        return;
    release_history(s, s->history);
    de_gpu_pipeline_destroy(s->renderer->dev, s->pipeline);
    DE_FREE(s->renderer->alloc, s, sizeof *s);
}
const de__upscaler_backend *de__taa_backend(void) {
    static const de__upscaler_backend backend = {"TAA",  DE_UPSCALE_TAA, init,
                                                 resize, dispatch,       destroy};
    return &backend;
}
