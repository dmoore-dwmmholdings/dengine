#include "render/denoise/reblur_internal.h"
#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"
#include "render/passes/vis/gbuffer.h"
#include "render/shaders/engine_shaders.h"
#include "render/rt/rt_internal.h"
#include <string.h>
typedef struct reblur_params {
    u32 width, height, scene, slot, view, entry, ids, ids_pitch;
    u32 visible, visible_count, sampler, lut, raw, distance, depth, gb0;
    u32 gb1, gb2, motion, material, input, output, factor, position;
    u32 out_motion, radius, alpha, length, kind, mip, quality, reserved;
    u32 rays, scale, out_hit, pad1;
} reblur_params;
DE_STATIC_ASSERT(sizeof(reblur_params) == 144, "ReblurParams layout");
struct de__radiance_denoiser {
    de_renderer *r;
    de_gpu_pipeline prepare, preblur, mip, fix, remodulate, anisotropic;
    de_gpu_buffer params[32];
    de_view view[32];
    u64 frame[32];
    de_dvec3 origin[32];
    de_vec3 delta[32];
};
typedef struct reblur_pass {
    de__radiance_denoiser *s;
    de_gpu_pipeline pipeline;
    de_gpu_buffer params;
    u32 entry;
    reblur_params pc;
    de_rg_resource scene, view, ids, visible, raw, distance, depth, gb0, gb1, gb2, motion, material,
        input, output, factor, position, out_motion, radius, alpha, length, rays, out_hit;
} reblur_pass;
void de__radiance_denoiser_destroy(de__radiance_denoiser *s) {
    if (!s)
        return;
    for (u32 i = 0; i < 32; ++i)
        de_gpu_buffer_destroy(s->r->dev, s->params[i]);
    de_gpu_pipeline p[] = {s->prepare, s->preblur, s->mip, s->fix, s->remodulate, s->anisotropic};
    for (u32 i = 0; i < 6; ++i)
        de_gpu_pipeline_destroy(s->r->dev, p[i]);
    DE_FREE(s->r->alloc, s, sizeof *s);
}
de_result de__radiance_denoiser_create(de_renderer *r, de__radiance_denoiser **out) {
    de__radiance_denoiser *s = DE_ALLOC(r->alloc, sizeof *s, 16, DE_MEM_RENDER);
    if (!s)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "radiance denoiser");
    memset(s, 0, sizeof *s);
    s->r = r;
    *out = s;
    return DE_OK;
}
static de_result pipelines(de__radiance_denoiser *s) {
    if (s->anisotropic.id)
        return DE_OK;
    const char *entries[] = {"cs_prepare",     "cs_preblur",    "cs_mip",
                             "cs_history_fix", "cs_remodulate", "cs_anisotropic"};
    de_gpu_pipeline *p[] = {&s->prepare, &s->preblur,    &s->mip,
                            &s->fix,     &s->remodulate, &s->anisotropic};
    for (u32 i = 0; i < 6; ++i) {
        const char *entry[] = {entries[i]};
        de__engine_shader_code code = {0};
        char why[256];
        DE_TRY(de__engine_shader_load(s->r->dev, s->r->alloc,
                                      i == 5 ? "denoise_reblur_specular" : "denoise_reblur_diffuse",
                                      0, entry, 1, &code, why, sizeof why));
        *p[i] =
            de_gpu_pipeline_create(s->r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                                                      .cs = code.entries[0].blob,
                                                                      .push_constant_bytes = 8,
                                                                      .threadgroup_size = {8, 8, 1},
                                                                      .name = entries[i]});
        de__engine_shader_code_free(&code);
        if (!p[i]->id)
            return DE_FAIL(DE_ERR_UNSUPPORTED, "radiance denoiser pipeline");
    }
    return DE_OK;
}
static u32 tex(reblur_pass *x, const de_rg_pass *p, de_rg_resource r, bool write) {
    return r.id ? de_gpu_texture_bindless(x->s->r->dev, de_rg_texture(p, r), 0, write) : 0;
}
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    reblur_pass *x = user;
    reblur_params pc = x->pc;
    de_gpu_device *d = x->s->r->dev;
#define R(n) pc.n = tex(x, pass, x->n, false)
#define W(n) pc.n = tex(x, pass, x->n, true)
    R(raw);
    R(distance);
    R(depth);
    R(gb0);
    R(gb1);
    R(gb2);
    R(motion);
    R(material);
    R(input);
    R(length);
    W(output);
    if (x->pipeline.id == x->s->prepare.id) {
        W(factor);
        W(position);
        W(out_motion);
        W(radius);
        W(alpha);
        W(out_hit);
    } else {
        R(factor);
        R(position);
        R(radius);
    }
#undef R
#undef W
#define B(n)                                                                                       \
    if (x->n.id)                                                                                   \
    pc.n = de_gpu_buffer_bindless(d, de_rg_buffer(pass, x->n))
    B(scene);
    B(view);
    B(ids);
    B(visible);
    B(rays);
#undef B
    memcpy((u8 *)de_gpu_buffer_map(d, x->params) + x->entry * sizeof pc, &pc, sizeof pc);
    u32 push[] = {de_gpu_buffer_bindless(d, x->params), x->entry};
    de_gpu_cmd_set_pipeline(cmd, x->pipeline);
    de_gpu_cmd_push_constants(cmd, push, sizeof push);
    u32 w = pc.width, h = pc.height;
    if (x->pipeline.id == x->s->mip.id) {
        w = (w + (1u << pc.mip) - 1) >> pc.mip;
        h = (h + (1u << pc.mip) - 1) >> pc.mip;
    }
    de_gpu_cmd_dispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
}
static de_rg_resource temporary(de_rg *g, u32 w, u32 h, de_gpu_format format, const char *name) {
    return de_rg_create_texture(
        g, &(de_gpu_texture_desc){.width = w,
                                  .height = h,
                                  .depth_or_layers = 1,
                                  .mip_count = 1,
                                  .format = format,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                  .name = name});
}
static void add(de_rg *g, reblur_pass *p, const char *name) {
    de_rg_pass *pass = de_rg_add_pass(g, name, DE_QUEUE_GRAPHICS, execute, p);
    if (!pass) {
        g->oom = true;
        return;
    }
    de_rg_resource reads[] = {p->scene,    p->view,     p->ids,   p->visible, p->raw,
                              p->distance, p->depth,    p->gb0,   p->gb1,     p->gb2,
                              p->motion,   p->material, p->input, p->length,  p->rays};
    for (u32 i = 0; i < sizeof reads / sizeof reads[0]; ++i)
        if (reads[i].id)
            de_rg_read(pass, reads[i], DE_ACCESS_SHADER_READ);
    de_rg_write(pass, p->output, DE_ACCESS_SHADER_WRITE);
    de_rg_resource extra[] = {p->factor, p->position, p->out_motion,
                              p->radius, p->alpha,    p->out_hit};
    for (u32 i = 0; i < 6; ++i)
        if (extra[i].id) {
            if (p->pipeline.id == p->s->prepare.id)
                de_rg_write(pass, extra[i], DE_ACCESS_SHADER_WRITE);
            else
                de_rg_read(pass, extra[i], DE_ACCESS_SHADER_READ);
        }
    de_rg_read(pass, de_rg_import_buffer(g, p->params, DE_ACCESS_SHADER_READ),
               DE_ACCESS_SHADER_READ);
    de_rg_pass_side_effect(pass);
}
de_result de__reblur_build(de__radiance_denoiser *s, de_rg *g, const de__vis_context *vc,
                           const de__lighting_context *lc, de_rg_resource radiance,
                           de_rg_resource distance, u32 kind, bool reference, de_rg_resource rays,
                           u32 scale) {
    if (reference)
        return DE_OK;
    DE_TRY(pipelines(s));
    u32 slot = (vc->view.id & 255u) - 1;
    if (slot >= 32)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "radiance view");
    if (!s->params[slot].id)
        s->params[slot] = de_gpu_buffer_create(
            s->r->dev, &(de_gpu_buffer_desc){.size = 8 * 32 * sizeof(reblur_params),
                                             .stride = sizeof(reblur_params),
                                             .usage = DE_USAGE_STORAGE,
                                             .memory = DE_MEM_UPLOAD});
    if (!s->params[slot].id)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "radiance parameters");
    if (s->view[slot].id != vc->view.id || s->frame[slot] != vc->frame_index) {
        s->delta[slot] = (de_vec3){0};
        if (s->view[slot].id == vc->view.id)
            s->delta[slot] = (de_vec3){(f32)(s->origin[slot].x - vc->origin.x),
                                       (f32)(s->origin[slot].y - vc->origin.y),
                                       (f32)(s->origin[slot].z - vc->origin.z)};
        s->origin[slot] = vc->origin;
        s->frame[slot] = vc->frame_index;
        s->view[slot] = vc->view;
    }
    de_rg_resource material = denoise_add_material_identity(
        s->r->denoise, g, vc->width, vc->height, vc->vis_ids_buffer, vc->visible_meshlets,
        vc->scene_views, vc->scene_view_slot, vc->vis_ids_pitch);
    u32 w = vc->width, h = vc->height;
    de_rg_resource demod = temporary(g, w, h, DE_FMT_RGBA32_FLOAT, "Reblur demodulated radiance"),
                   factor =
                       temporary(g, w, h, DE_FMT_RGBA32_FLOAT, "Reblur canonical material factor"),
                   position = temporary(g, w, h, DE_FMT_RGBA32_FLOAT, "Reblur receiver position"),
                   motion = temporary(g, w, h, DE_FMT_RG32_FLOAT, "Reblur virtual/surface motion"),
                   radius = temporary(g, w, h, DE_FMT_R32_FLOAT, "Reblur hit radius"),
                   alpha = temporary(g, w, h, DE_FMT_R32_FLOAT, "Reblur roughness alpha");
    de_rg_resource hit_guide =
        temporary(g, w, h, DE_FMT_R32_FLOAT, "Reblur representative hit distance");
    reblur_pass *p = de_arena_push_zero(g->arena, 9 * sizeof *p, 16);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "Reblur graph data");
    p[0] =
        (reblur_pass){.s = s,
                      .pipeline = s->prepare,
                      .params = s->params[slot],
                      .entry = (u32)(vc->frame_index % 8) * 32 + kind * 10,
                      .pc = {.width = w,
                             .height = h,
                             .slot = vc->scene_view_slot,
                             .entry = lc->constants_entry,
                             .ids_pitch = vc->vis_ids_pitch,
                             .visible_count = vc->visible_capacity + vc->visible_alpha_capacity,
                             .sampler = de_gpu_sampler_bindless(s->r->dev, s->r->rt_scene->sampler),
                             .lut = lc->view.brdf_lut,
                             .kind = kind,
                             .quality = kind == 2 ? vc->settings->reflections : vc->settings->gi,
                             .scale = scale},
                      .scene = vc->scene_views,
                      .ids = vc->vis_ids_buffer,
                      .visible = vc->visible_meshlets,
                      .raw = radiance,
                      .distance = distance,
                      .depth = vc->depth,
                      .gb0 = vc->gbuffer[DE__GB0],
                      .gb1 = vc->gbuffer[DE__GB1],
                      .gb2 = vc->gbuffer[DE__GB2],
                      .motion = vc->gbuffer[DE__GB_MOTION],
                      .material = material,
                      .output = demod,
                      .factor = factor,
                      .position = position,
                      .out_motion = motion,
                      .radius = radius,
                      .alpha = alpha,
                      .rays = rays,
                      .out_hit = hit_guide};
    add(g, &p[0], kind == 0 ? "Denoise.Diffuse.Demodulate" : "Denoise.Specular.Demodulate");
    reblur_pass base = p[0];
    base.raw = (de_rg_resource){0};
    base.out_hit = (de_rg_resource){0};
    base.distance = hit_guide;
    base.out_motion = base.alpha = (de_rg_resource){0};
    base.factor = (de_rg_resource){0};
    base.pipeline = kind == 0 ? s->preblur : s->anisotropic;
    base.input = demod;
    base.output = temporary(g, w, h, DE_FMT_RGBA32_FLOAT, "Reblur preblur");
    base.entry++;
    p[1] = base;
    add(g, &p[1], kind == 0 ? "Denoise.Diffuse.Poisson" : "Denoise.Specular.LobePreblur");
    de_denoise_frame frame = {
        .graph = g,
        .view = vc->view,
        .frame = vc->frame_index,
        .reset = vc->view_new || vc->occlusion_reset,
        .inputs = {.signal = p[1].output,
                   .depth = vc->depth,
                   .normal = vc->gbuffer[DE__GB1],
                   .motion = motion,
                   .material = material,
                   .hit_distance = radius,
                   .width = w,
                   .height = h,
                   .inverse_view_proj = de_m4_inverse(&vc->view_proj),
                   .previous_inverse_view_proj = de_m4_inverse(&vc->prev_view_proj),
                   .camera_position = vc->camera_position,
                   .previous_origin_delta = s->delta[slot],
                   .position = position,
                   .temporal_alpha = alpha}};
    de_denoise_signal_desc desc = {.identity = 0x5245424c55520000ull + kind,
                                   .format = DE_FMT_RGBA32_FLOAT,
                                   .channels = 3,
                                   .alpha_min = .05f,
                                   .clamp = DE_DENOISE_CLAMP_YCOCG,
                                   .iterations = base.pc.quality >= DE_QUALITY_HIGH ? 3u : 2u,
                                   .hit_distance_scale = 1,
                                   .roughness_threshold = kind ? .1f : 1,
                                   .zero_radius_passthrough = kind != 0};
    de_denoise_ctx *ctx = NULL;
    DE_TRY(denoise_begin(s->r->denoise, &frame, &desc, &ctx));
    de_denoise_result result = denoise_add_reproject(ctx);
    result = denoise_add_temporal(ctx, result);
    if (!result.color.id)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "Reblur temporal");
    de_rg_resource low = result.color;
    u32 levels = base.pc.quality >= DE_QUALITY_HIGH ? 3 : 2;
    for (u32 level = 1; level <= levels; ++level) {
        p[1 + level] = base;
        p[1 + level].entry = base.entry + level;
        p[1 + level].pc.mip = level;
        p[1 + level].pipeline = s->mip;
        p[1 + level].input = low;
        p[1 + level].output =
            temporary(g, (w + (1u << level) - 1) >> level, (h + (1u << level) - 1) >> level,
                      DE_FMT_RGBA32_FLOAT, "Reblur bilateral history mip");
        add(g, &p[1 + level], "Denoise.HistoryMip");
        low = p[1 + level].output;
    }
    u32 next = 2 + levels;
    p[next] = base;
    p[next].entry = base.entry + levels + 1;
    p[next].pipeline = s->fix;
    p[next].pc.mip = levels;
    p[next].raw = result.color;
    p[next].input = low;
    p[next].length = result.history_length;
    p[next].output = temporary(g, w, h, DE_FMT_RGBA32_FLOAT, "Reblur short-history reconstruction");
    add(g, &p[next], "Denoise.HistoryFix");
    result.color = p[next].output;
    result = denoise_add_atrous(ctx, result);
    if (!result.color.id)
        return DE_FAIL(DE_ERR_UNSUPPORTED, "Reblur postblur");
    if (kind != 0) {
        ++next;
        p[next] = base;
        p[next].entry = base.entry + levels + 2;
        p[next].pipeline = s->anisotropic;
        p[next].input = result.color;
        p[next].output = temporary(g, w, h, DE_FMT_RGBA32_FLOAT, "Reblur anisotropic specular");
        add(g, &p[next], "Denoise.Specular.LobePostblur");
        result.color = p[next].output;
    }
    ++next;
    p[next] = base;
    p[next].entry = base.entry + levels + 3;
    p[next].pipeline = s->remodulate;
    p[next].input = result.color;
    p[next].factor = factor;
    p[next].output = radiance;
    add(g, &p[next], kind == 0 ? "Denoise.Diffuse.Publish" : "Denoise.Specular.Publish");
    return DE_OK;
}
de_result de__denoise_diffuse(de__radiance_denoiser *s, de_rg *g, const de__vis_context *vc,
                              const de__lighting_context *lc, de_rg_resource radiance,
                              de_rg_resource distance, bool reference) {
    return de__reblur_build(s, g, vc, lc, radiance, distance, 0, reference, (de_rg_resource){0}, 1);
}
