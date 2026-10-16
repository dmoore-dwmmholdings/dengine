#include "render/passes/forward/scene_color_pyramid.h"
#include "render/graph/rg_internal.h"
#include <stdio.h>
#include <string.h>
struct de__scene_color_pyramid {
    de__vis_shared *vis;
    de__vis_kernel kernel;
};
typedef struct scene_pass {
    de__scene_color_pyramid *p;
    de_rg_resource source, source_depth, color, depth;
    de__scene_color_params pc;
    u32 depth_pitch;
} scene_pass;
de_result de__scene_color_layout_make(u32 w, u32 h, de__scene_color_layout *out) {
    if (!out || !w || !h || w > 32768 || h > 32768)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "scene color: dimensions");
    memset(out, 0, sizeof *out);
    out->depth_pitch = (w * 4u + 255u) & ~255u;
    out->depth_bytes = (u64)out->depth_pitch * h;
    for (u32 i = 0; i < DE__SCENE_COLOR_LEVELS; ++i) {
        u32 pitch = (w * 8u + 255u) & ~255u;
        out->bytes = (out->bytes + 511u) & ~(u64)511u;
        if (out->bytes + (u64)pitch * h > UINT32_MAX)
            return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "scene color: buffer exceeds shader offsets");
        out->level[i] = (de__scene_color_level){w, h, pitch, (u32)out->bytes};
        out->bytes += (u64)pitch * h;
        out->count = i + 1;
        if (w == 1 && h == 1)
            break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return DE_OK;
}
de_result de__scene_color_create(de__vis_shared *vis, de__scene_color_pyramid **out) {
    if (!out || !vis)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "scene color: NULL state argument");
    *out = NULL;
    de__scene_color_pyramid *p = DE_NEW(vis->alloc, de__scene_color_pyramid, DE_MEM_RENDER);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene color: state");
    memset(p, 0, sizeof *p);
    p->vis = vis;
    const char *entry = "cs_scene_color_pyramid";
    const u32 group[] = {8, 8, 1};
    de__engine_shader_code code;
    de_result r = de__vis_shader_load(vis, "scene_color_pyramid", &entry, 1, &code);
    if (r == DE_OK) {
        r = de__vis_kernel_create(vis, &code, 0, entry, group, sizeof(de__scene_color_params),
                                  &p->kernel);
        de__engine_shader_code_free(&code);
    }
    if (r != DE_OK) {
        de__scene_color_destroy(p);
        return r;
    }
    *out = p;
    return DE_OK;
}
void de__scene_color_destroy(de__scene_color_pyramid *p) {
    if (!p)
        return;
    de__vis_kernel_destroy(p->vis, &p->kernel);
    DE_FREE(p->vis->alloc, p, sizeof *p);
}
static void scene_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    scene_pass *p = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, p->color), 0, p->pc.pitch,
                                      de_rg_texture(pass, p->source), 0, 0);
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, p->depth), 0, p->depth_pitch,
                                      de_rg_texture(pass, p->source_depth), 0, 0);
}
static void scene_downsample(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    scene_pass *p = user;
    de__scene_color_params pc = p->pc;
    pc.color = de_gpu_buffer_bindless(p->p->vis->dev, de_rg_buffer(pass, p->color));
    de_gpu_cmd_set_pipeline(cmd, p->p->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.width + 7) / 8, (pc.height + 7) / 8, 1);
}
de_result de__scene_color_build(de__scene_color_pyramid *p, de_rg *g, de_arena *arena,
                                de_rg_resource source, de_rg_resource source_depth, u32 w, u32 h,
                                de__scene_color_context *out) {
    if (!out || !p || !g || !arena || !source.id || !source_depth.id)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "scene color: NULL build argument");
    memset(out, 0, sizeof *out);
    de__scene_color_layout layout;
    de_result r = de__scene_color_layout_make(w, h, &layout);
    if (r != DE_OK)
        return r;
    scene_pass *data = de_arena_push_zero(arena, layout.count * sizeof *data, 16);
    if (!data)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene color: frame parameters");
    de_rg_resource color = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = layout.bytes,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                                 .name = "forward.scene_color"});
    de_rg_resource depth = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = layout.depth_bytes,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST | DE_USAGE_COPY_SRC,
                                 .name = "forward.scene_depth"});
    if (!color.id || !depth.id)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene color: graph buffers");
    data[0] = (scene_pass){.p = p,
                           .source = source,
                           .source_depth = source_depth,
                           .color = color,
                           .depth = depth,
                           .depth_pitch = layout.depth_pitch,
                           .pc = {.pitch = layout.level[0].pitch}};
    de_rg_pass *pass =
        de_rg_add_pass(g, "forward.scene_color", DE_QUEUE_GRAPHICS, scene_copy, &data[0]);
    if (!pass)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene color: copy pass");
    de_rg_read(pass, source, DE_ACCESS_COPY_SRC);
    de_rg_read(pass, source_depth, DE_ACCESS_COPY_SRC);
    de_rg_write(pass, color, DE_ACCESS_COPY_DST);
    de_rg_write(pass, depth, DE_ACCESS_COPY_DST);
    for (u32 i = 1; i < layout.count; ++i) {
        de__scene_color_level a = layout.level[i - 1], b = layout.level[i];
        data[i] = (scene_pass){.p = p,
                               .color = color,
                               .pc = {.src_width = a.width,
                                      .src_height = a.height,
                                      .src_pitch = a.pitch,
                                      .src_offset = a.offset,
                                      .width = b.width,
                                      .height = b.height,
                                      .pitch = b.pitch,
                                      .offset = b.offset}};
        char name[48];
        snprintf(name, sizeof name, "forward.scene_color.mip%u", i);
        pass = de_rg_add_pass(g, name, DE_QUEUE_GRAPHICS, scene_downsample, &data[i]);
        if (!pass)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "scene color: mip pass");
        /* Levels share one packed buffer: retain all earlier level producers. */
        de_rg_read(pass, color, DE_ACCESS_SHADER_READ);
        de_rg_write(pass, color, DE_ACCESS_SHADER_WRITE);
    }
    if (g->oom || g->error_count)
        return DE_FAIL(g->oom ? DE_ERR_OUT_OF_MEMORY : DE_ERR_INTERNAL,
                       "scene color: incomplete graph declarations");
    *out = (de__scene_color_context){.color = color, .depth = depth, .layout = layout};
    return DE_OK;
}
