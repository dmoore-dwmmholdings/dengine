#include "pixel_internal.h"
#include "render/renderer_internal.h"
#include "render/graph/rg_internal.h"
#include "render/shaders/engine_shaders.h"
#include <math.h>
#include <string.h>
de__pixel_layout de__pixel_layout_make(u32 iw, u32 ih, u32 ow, u32 oh, bool integer) {
    if (!iw || !ih)
        return (de__pixel_layout){0};
    f32 scale = fminf((f32)ow / iw, (f32)oh / ih);
    if (integer)
        scale = fmaxf(1, floorf(scale));
    f32 w = iw * scale, h = ih * scale;
    return (de__pixel_layout){scale, ((f32)ow - w) * .5f, ((f32)oh - h) * .5f, w, h};
}
de_dvec3 de__pixel_snap(de_dvec3 position, de_plane_2d plane, f64 texel, de_vec2 *residual) {
    *residual = (de_vec2){0};
    if (!(texel > 0) || !isfinite(texel))
        return position;
    f64 x = round(position.x / texel) * texel;
    f64 *vertical = plane == DE_PLANE_SIDE_XZ ? &position.z : &position.y;
    f64 y = round(*vertical / texel) * texel;
    residual->x = (f32)((position.x - x) / texel);
    residual->y = (f32)((*vertical - y) / texel);
    position.x = x;
    *vertical = y;
    return position;
}
struct de__pixel {
    de__pixel_snapshots *snapshots;
    de__palette_runtime *palette;
    de_renderer *r;
    de__engine_shader_code code;
    de_gpu_sampler sampler;
    de_gpu_pipeline pipeline;
};
typedef struct pixel_push {
    u32 src, smp, iw, ih;
    f32 rect[4], residual[2], border, integer_only, clear[4];
} pixel_push;
typedef struct pixel_pass {
    de__pixel *owner;
    de_rg_resource src, dst;
    pixel_push push;
    u32 width, height;
} pixel_pass;
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    pixel_pass *p = user;
    de_gpu_attachment a = {.texture = de_rg_texture(pass, p->dst), .clear = true, .store = true};
    memcpy(a.clear_color, p->push.clear, sizeof a.clear_color);
    de_gpu_cmd_begin_render(cmd, &a, 1, NULL);
    if (p->owner->pipeline.id) {
        p->push.src =
            de_gpu_texture_bindless(p->owner->r->dev, de_rg_texture(pass, p->src), 0, false);
        de_gpu_cmd_set_pipeline(cmd, p->owner->pipeline);
        de_gpu_cmd_viewport(cmd, 0, 0, (f32)p->width, (f32)p->height);
        de_gpu_cmd_scissor(cmd, 0, 0, p->width, p->height);
        de_gpu_cmd_push_constants(cmd, &p->push, sizeof p->push);
        de_gpu_cmd_draw(cmd, 3, 1, 0, 0);
    }
    de_gpu_cmd_end_render(cmd);
}
static void stage(de__view_frame *vf, void *user) {
    if (!vf->pixel_art)
        return;
    de__pixel *owner = user;
    pixel_pass *p = de_arena_push_zero(vf->graph->arena, sizeof *p, 16);
    if (!p)
        return;
    de__pixel_layout layout = de__pixel_layout_make(
        vf->pixel_art->internal_width, vf->pixel_art->internal_height, vf->output_width,
        vf->output_height, vf->pixel_art->integer_scale_only);
    p->owner = owner;
    p->src = vf->color;
    p->dst = vf->output;
    p->width = vf->output_width;
    p->height = vf->output_height;
    p->push = (pixel_push){.smp = de_gpu_sampler_bindless(owner->r->dev, owner->sampler),
                           .iw = vf->pixel_art->internal_width,
                           .ih = vf->pixel_art->internal_height,
                           .rect = {layout.x, layout.y, layout.width, layout.height},
                           .residual = {vf->pixel_residual.x, -vf->pixel_residual.y},
                           .border = vf->pixel_art->smooth_subpixel_scroll ? 1.f : 0.f,
                           .integer_only = vf->pixel_art->integer_scale_only};
    memcpy(p->push.clear, &vf->pixel_art->letterbox_color, sizeof p->push.clear);
    if (!vf->pixel_art->smooth_subpixel_scroll)
        p->push.residual[0] = p->push.residual[1] = 0;
    de_rg_pass *pass = de_rg_add_pass(vf->graph, "Pixel.Upscale", DE_QUEUE_GRAPHICS, execute, p);
    if (!pass)
        return;
    de_rg_read(pass, p->src, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, p->dst, DE_ACCESS_RENDER_TARGET);
    vf->color = vf->output;
    vf->color_format = DE__VIEW_OUTPUT_FORMAT;
    vf->width = vf->output_width;
    vf->height = vf->output_height;
}
de_result de__pixel_create(de_renderer *r, de__pixel **out) {
    *out = DE_NEW(r->alloc, de__pixel, DE_MEM_RENDER);
    if (!*out)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "pixel owner");
    de__pixel *p = *out;
    memset(p, 0, sizeof *p);
    p->r = r;
    p->sampler = de_gpu_sampler_create(r->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                      .mag = DE_FILTER_LINEAR,
                                                                      .mip = DE_FILTER_NEAREST,
                                                                      .u = DE_ADDR_CLAMP,
                                                                      .v = DE_ADDR_CLAMP,
                                                                      .w = DE_ADDR_CLAMP});
    const char *entries[] = {"vs_main", "ps_main"};
    char why[512];
    if (de__engine_shader_load(r->dev, r->alloc, "pixel_upscale", 0, entries, 2, &p->code, why,
                               sizeof why) == DE_OK) {
        de_gpu_pipeline_desc d = {.kind = DE_PIPE_GRAPHICS,
                                  .vs = p->code.entries[0].blob,
                                  .ps = p->code.entries[1].blob,
                                  .color_formats = {DE__VIEW_OUTPUT_FORMAT},
                                  .color_count = 1,
                                  .blend = {DE_BLEND_OPAQUE},
                                  .cull = DE_CULL_NONE,
                                  .push_constant_bytes = sizeof(pixel_push),
                                  .name = "pixel upscale"};
        p->pipeline = de_gpu_pipeline_create(r->dev, &d);
    }
    de_result result = de__pixel_snapshots_create(r, &p->snapshots);
    if (result != DE_OK)
        return result;
    result = de__palette_runtime_create(r, &p->palette);
    if (result != DE_OK)
        return result;
    return de__view_register_stage(r, DE__VIEW_SLOT_POST, INT32_MAX, "pixel.upscale", stage, p);
}
void de__pixel_destroy(de_renderer *r, de__pixel *p) {
    if (!p)
        return;
    de__view_unregister_stage(r, stage, p);
    de__pixel_snapshots_destroy(r, p->snapshots);
    de__palette_runtime_destroy(r, p->palette);
    de_gpu_pipeline_destroy(r->dev, p->pipeline);
    de_gpu_sampler_destroy(r->dev, p->sampler);
    de__engine_shader_code_free(&p->code);
    DE_FREE(r->alloc, p, sizeof *p);
}

void de__pixel_prepare_palette(de__pixel *p, de_texture texture) {
    if (p)
        de__palette_prepare(p->palette, texture);
}
