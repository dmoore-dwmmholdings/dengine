#include "pixel_internal.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
#include "render/shaders/engine_shaders.h"
#include "render/scene/gpu_scene.h"
#include "render/loaders/render_assets.h"
#include "render/graph/rg_internal.h"
#include <math.h>
#include <string.h>
typedef struct palette_entry {
    de_texture asset;
    de_gpu_texture source;
    de_gpu_buffer lut;
} palette_entry;
struct de__palette_runtime {
    de_renderer *r;
    de__engine_shader_code code;
    de_gpu_pipeline pipeline;
    palette_entry *entries;
    u32 count, capacity;
};
static f32 linear(f32 c) {
    return c <= .04045f ? c / 12.92f : powf((c + .055f) / 1.055f, 2.4f);
}
static f32 half(u16 h) {
    u32 exponent = (h >> 10) & 31, m = h & 1023;
    f32 value = exponent ? ldexpf(1 + (f32)m / 1024, (i32)exponent - 15) : ldexpf((f32)m, -24);
    return h & 32768 ? -value : value;
}
void de__palette_prepare(de__palette_runtime *owner, de_texture asset) {
    if (!owner || !asset.id)
        return;
    de_renderer *r = owner->r;
    const de_render_texture *texture = de__gpu_scene_texture_record(r->gpu_scene, asset);
    if (!texture || texture->height != 1 || !texture->width)
        return;
    u32 bytes = 0;
    switch (texture->format) {
    case DE_FMT_RGBA8_UNORM:
    case DE_FMT_RGBA8_SRGB:
    case DE_FMT_BGRA8_UNORM:
    case DE_FMT_BGRA8_SRGB:
        bytes = 4;
        break;
    case DE_FMT_RGBA16_FLOAT:
        bytes = 8;
        break;
    case DE_FMT_RGBA32_FLOAT:
        bytes = 16;
        break;
    default:
        return;
    }
    for (u32 i = 0; i < owner->count; ++i)
        if (owner->entries[i].asset.id == asset.id &&
            owner->entries[i].source.id == texture->texture.id)
            return;
    u32 pitch = ((texture->width * bytes + 255) / 256) * 256;
    de_gpu_buffer readback = de_gpu_buffer_create(
        r->dev, &(de_gpu_buffer_desc){
                    .size = pitch, .usage = DE_USAGE_COPY_DST, .memory = DE_MEM_READBACK});
    if (!readback.id)
        return;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(r->dev, DE_QUEUE_GRAPHICS, "Pixel.PaletteReadback");
    de_gpu_barrier barrier = {
        .texture = texture->texture, .before = DE_ACCESS_SHADER_READ, .after = DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    de_gpu_cmd_copy_texture_to_buffer(cmd, readback, 0, pitch, texture->texture, 0, 0);
    barrier.before = DE_ACCESS_COPY_SRC;
    barrier.after = DE_ACCESS_SHADER_READ;
    de_gpu_cmd_barriers(cmd, &barrier, 1);
    if (de_gpu_queue_submit(r->dev, DE_QUEUE_GRAPHICS,
                            &(de_gpu_submit){.cmds = &cmd, .cmd_count = 1}) != DE_OK) {
        de_gpu_buffer_destroy(r->dev, readback);
        return;
    }
    de_gpu_wait_idle(r->dev);
    const u8 *data = de_gpu_buffer_map(r->dev, readback);
    size_t color_bytes = (size_t)texture->width * sizeof(de_vec4),
           lut_bytes = 32768 * sizeof(de_vec4);
    de_vec4 *colors = DE_ALLOC(r->alloc, color_bytes, 16, DE_MEM_RENDER),
            *lut = DE_ALLOC(r->alloc, lut_bytes, 16, DE_MEM_RENDER);
    if (!colors || !lut)
        goto done;
    for (u32 i = 0; i < texture->width; ++i) {
        f32 c[4];
        for (u32 channel = 0; channel < 4; ++channel) {
            if (bytes == 4)
                c[channel] = data[i * 4 + channel] / 255.f;
            else if (bytes == 8) {
                u16 h;
                memcpy(&h, data + i * 8 + channel * 2, 2);
                c[channel] = half(h);
            } else
                memcpy(&c[channel], data + i * 16 + channel * 4, 4);
        }
        if (texture->format == DE_FMT_BGRA8_UNORM || texture->format == DE_FMT_BGRA8_SRGB) {
            f32 red = c[0];
            c[0] = c[2];
            c[2] = red;
        }
        if (texture->format == DE_FMT_RGBA8_SRGB || texture->format == DE_FMT_BGRA8_SRGB)
            for (u32 channel = 0; channel < 3; ++channel)
                c[channel] = linear(c[channel]);
        colors[i] = (de_vec4){c[0], c[1], c[2], c[3]};
    }
    de__palette_build(colors, texture->width, lut);
    de_gpu_buffer gpu =
        de_gpu_buffer_create(r->dev, &(de_gpu_buffer_desc){.size = lut_bytes,
                                                           .usage = DE_USAGE_STORAGE,
                                                           .memory = DE_MEM_UPLOAD,
                                                           .name = "pixel OKLab palette LUT"});
    if (!gpu.id)
        goto done;
    memcpy(de_gpu_buffer_map(r->dev, gpu), lut, lut_bytes);
    /* Reload replaces this asset's LUT; RHI retirement preserves in-flight readers. */
    for (u32 i = 0; i < owner->count; ++i)
        if (owner->entries[i].asset.id == asset.id) {
            de_gpu_buffer_destroy(r->dev, owner->entries[i].lut);
            owner->entries[i] = (palette_entry){asset, texture->texture, gpu};
            goto done;
        }
    if (owner->count == owner->capacity) {
        u32 capacity = owner->capacity ? owner->capacity * 2 : 8;
        palette_entry *next =
            DE_ALLOC(r->alloc, (size_t)capacity * sizeof *next, 16, DE_MEM_RENDER);
        if (!next) {
            de_gpu_buffer_destroy(r->dev, gpu);
            goto done;
        }
        if (owner->entries) {
            memcpy(next, owner->entries, (size_t)owner->count * sizeof *next);
            DE_FREE(r->alloc, owner->entries, (size_t)owner->capacity * sizeof *next);
        }
        owner->entries = next;
        owner->capacity = capacity;
    }
    owner->entries[owner->count++] = (palette_entry){asset, texture->texture, gpu};
done:
    if (colors)
        DE_FREE(r->alloc, colors, color_bytes);
    if (lut)
        DE_FREE(r->alloc, lut, lut_bytes);
    de_gpu_buffer_destroy(r->dev, readback);
}
typedef struct palette_push {
    u32 src, dst, lut, width, height;
    f32 strength;
    i32 phase_x, phase_y;
} palette_push;
typedef struct palette_pass {
    de__palette_runtime *owner;
    de_rg_resource src, dst;
    palette_push push;
} palette_pass;
static void execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    palette_pass *p = user;
    de_renderer *r = p->owner->r;
    p->push.src = de_gpu_texture_bindless(r->dev, de_rg_texture(pass, p->src), 0, false);
    p->push.dst = de_gpu_texture_bindless(r->dev, de_rg_texture(pass, p->dst), 0, true);
    de_gpu_cmd_set_pipeline(cmd, p->owner->pipeline);
    de_gpu_cmd_push_constants(cmd, &p->push, sizeof p->push);
    de_gpu_cmd_dispatch(cmd, (p->push.width + 7) / 8, (p->push.height + 7) / 8, 1);
}
static void stage(de__view_frame *vf, void *user) {
    de__palette_runtime *owner = user;
    if (!vf->pixel_art || !owner->pipeline.id)
        return;
    u32 lut = 0;
    for (u32 i = owner->count; i > 0; --i)
        if (owner->entries[i - 1].asset.id == vf->pixel_art->palette_lut.id) {
            lut = de_gpu_buffer_bindless(owner->r->dev, owner->entries[i - 1].lut);
            break;
        }
    if (!lut && !(vf->pixel_art->dither_strength > 0))
        return;
    palette_pass *p = de_arena_push_zero(vf->graph->arena, sizeof *p, 16);
    if (!p)
        return;
    p->owner = owner;
    p->src = vf->color;
    p->dst = de_rg_create_texture(
        vf->graph, &(de_gpu_texture_desc){.width = vf->width,
                                          .height = vf->height,
                                          .format = DE_FMT_RGBA16_FLOAT,
                                          .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                          .name = "pixel.palette"});
    p->push = (palette_push){.lut = lut,
                             .width = vf->width,
                             .height = vf->height,
                             .strength = vf->pixel_art->dither_strength};
    f64 step = (vf->camera->camera.ortho_height > 0 ? vf->camera->camera.ortho_height : 10) /
               vf->pixel_art->internal_height;
    de_dvec3 position = vf->camera->has_position
                            ? vf->camera->position
                            : (de_dvec3){vf->world->origin.x + vf->camera->offset.x,
                                         vf->world->origin.y + vf->camera->offset.y,
                                         vf->world->origin.z + vf->camera->offset.z};
    p->push.phase_x = (i32)fmod(round(position.x / step), 8);
    p->push.phase_y = -(i32)fmod(
        round((owner->r->plane_2d == DE_PLANE_SIDE_XZ ? position.z : position.y) / step), 8);
    de_rg_pass *pass =
        de_rg_add_pass(vf->graph, "Pixel.PaletteDither", DE_QUEUE_GRAPHICS, execute, p);
    if (!pass)
        return;
    de_rg_read(pass, p->src, DE_ACCESS_SHADER_READ);
    de_rg_write(pass, p->dst, DE_ACCESS_SHADER_WRITE);
    vf->color = p->dst;
    vf->color_format = DE_FMT_RGBA16_FLOAT;
}
de_result de__palette_runtime_create(de_renderer *r, de__palette_runtime **out) {
    *out = DE_NEW(r->alloc, de__palette_runtime, DE_MEM_RENDER);
    if (!*out)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "palette owner");
    de__palette_runtime *p = *out;
    memset(p, 0, sizeof *p);
    p->r = r;
    const char *entries[] = {"cs_main"};
    char why[512];
    if (de__engine_shader_load(r->dev, r->alloc, "pixel_palette", 0, entries, 1, &p->code, why,
                               sizeof why) == DE_OK)
        p->pipeline = de_gpu_pipeline_create(
            r->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_COMPUTE,
                                            .cs = p->code.entries[0].blob,
                                            .push_constant_bytes = sizeof(palette_push),
                                            .name = "pixel palette dither",
                                            .threadgroup_size = {8, 8, 1}});
    return de__view_register_stage(r, DE__VIEW_SLOT_POST, INT32_MAX - 1, "pixel.palette", stage, p);
}
void de__palette_runtime_destroy(de_renderer *r, de__palette_runtime *p) {
    if (!p)
        return;
    de__view_unregister_stage(r, stage, p);
    for (u32 i = 0; i < p->count; ++i)
        de_gpu_buffer_destroy(r->dev, p->entries[i].lut);
    if (p->entries)
        DE_FREE(r->alloc, p->entries, (size_t)p->capacity * sizeof *p->entries);
    de_gpu_pipeline_destroy(r->dev, p->pipeline);
    de__engine_shader_code_free(&p->code);
    DE_FREE(r->alloc, p, sizeof *p);
}
