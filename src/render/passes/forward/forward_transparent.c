/*
 * forward_transparent.c: the forward transparent pass (SPEC §12.5 step 6; DENG-305). forward.h
 * describes the module; shaders/forward/forward_transparent.slang (engine shader
 * `forward_transparent`) has the vertex and pixel shaders.
 *
 * Per view and frame, in DE__VIEW_SLOT_TRANSPARENT, after lighting (the context
 * de__lighting_build published for the view) and before output.basic reads the lighting target:
 *
 * transparent_sort.c gathers and sorts the visible list back to front.
 * forward.scene_color snapshots current opaque HDR and depth to transient packed buffers;
 * forward.scene_color.mipN builds Gaussian colour levels. r.refraction=0 copies mip0 alone.
 * forward.transparent draws each instance and side in list order (double-sided back faces
 * first), blending into the HDR target while testing reverse-Z depth without writing it.
 *
 * No pass at all when nothing transparent is in view (the list is empty, or no material in the
 * scene is forward). The pipeline is made at create (pre-warmed): a transparent object appearing
 * never creates one. The pass reads the cluster lists, the LTC tables, the environment strip and
 * the lighting constants of the view's lighting context (lighting.h), as the deferred pass does.
 *
 * Limits: picking reads the vis buffer only, so transparent surfaces cannot be picked; the
 * opaque AO/SSR/SSGI are cleared for transparent receivers; CSM and IBL use the surface's own
 * position. Sorting is per instance (intersecting or self-overlapping transparent meshes can
 * composite out of order). Screen-space refraction uses a bounded volume exit proxy, at most
 * eight opaque-depth steps, and direction-correct raw probe/sky fallback beyond the viewport.
 */
#include "render/passes/forward/forward.h"
#include "render/passes/forward/scene_color_pyramid.h"
#include "render/rt/rt_refraction.h"

#include "render/graph/rg_internal.h"
#include "render/lighting/lighting.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"

#include <stdio.h>
#include <string.h>

struct de__forward {
    de__vis_shared *vis;
    de__lighting *lighting;
    de__scene_color_pyramid *pyramid;
    de_gpu_device *dev;
    const de_allocator *alloc;
    bool enabled;
    de_cvar cv_refraction;
    char why[256];
    de_gpu_pipeline pipeline; /**< vs_forward + ps_forward */
    de_gpu_pipeline capture;
    u32 push_bytes;
    de_gpu_sampler sampler; /**< the material sampler (as the resolve's) */
    u32 sampler_bindless;
    de_gpu_buffer indices;
    de_gpu_buffer indirect[DE__VIEW_MAX][DE__VIS_FRAME_RING];
    u32 indirect_capacity[DE__VIEW_MAX][DE__VIS_FRAME_RING];
};

typedef struct fw_pass {
    de__forward *fw;
    de__fwd_params pc;
    de_rg_resource target, depth, scene_color, scene_depth, views;
    de_rg_resource lights, dir_lights, grid, list, counters, ltc1, ltc2;
    const de__forward_draw *draws;
    de_rg_resource indirect, indices;
    u32 draw_count;
    de__rt_refraction_frame *rt;
} fw_pass;

/* =================================================================================================
 * Create / destroy
 * ============================================================================================== */

static u32 push_of(u32 reflected, u32 own) {
    const u32 b = reflected > own ? reflected : own;
    return (b + 3u) & ~3u;
}

de_result de__forward_create_ex(de__vis_shared *vis, de__lighting *lighting, de__forward **out) {
    *out = NULL;
    if (vis == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__forward_create_ex: no vis frame");
    de__forward *fw = DE_NEW(vis->alloc, de__forward, DE_MEM_RENDER);
    if (fw == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de__forward_create_ex: no memory");
    memset(fw, 0, sizeof *fw);
    fw->vis = vis;
    fw->lighting = lighting;
    fw->dev = vis->dev;
    fw->alloc = vis->alloc;
    fw->cv_refraction = de_cvar_register(&(de_cvar_desc){
        .name = "r.refraction",
        .help =
            "Current-frame screen-space refraction; off preserves legacy same-pixel transmission",
        .type = DE_CVAR_BOOL,
        .flags = DE_CVAR_SAVED,
        .default_number = 1});
    if (!vis->enabled || !de__lighting_enabled(lighting)) {
        snprintf(fw->why, sizeof fw->why, "%s",
                 !vis->enabled ? "the vis frame is off" : "lighting is off");
        *out = fw;
        return DE_OK;
    }
    fw->sampler = de_gpu_sampler_create(fw->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                        .mag = DE_FILTER_LINEAR,
                                                                        .mip = DE_FILTER_LINEAR,
                                                                        .max_aniso = 1.0f});
    de_result r = DE_HANDLE_VALID(fw->sampler)
                      ? DE_OK
                      : DE_FAIL_WRAP(DE_ERR_INTERNAL, "forward: the material sampler");
    if (r == DE_OK) {
        fw->sampler_bindless = de_gpu_sampler_bindless(fw->dev, fw->sampler);
        static const char *const k_entries[3] = {"vs_forward", "ps_forward",
                                                 "ps_capture_refraction"};
        de__engine_shader_code code;
        r = de__vis_shader_load(vis, "forward_transparent", k_entries, 3, &code);
        if (r == DE_OK) {
            u32 reflected = code.entries[0].push_constant_bytes;
            if (code.entries[1].push_constant_bytes > reflected)
                reflected = code.entries[1].push_constant_bytes;
            fw->push_bytes = push_of(reflected, (u32)sizeof(de__fwd_params));
            fw->pipeline = de_gpu_pipeline_create(
                fw->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_GRAPHICS,
                                                 .vs = code.entries[0].blob,
                                                 .ps = code.entries[1].blob,
                                                 .color_formats = {DE_FMT_RGBA16_FLOAT},
                                                 .color_count = 1,
                                                 .blend = {DE_BLEND_PREMUL},
                                                 .depth_format = DE_FMT_D32_FLOAT,
                                                 .cull = DE_CULL_NONE,
                                                 .depth_test = true,
                                                 .depth_write = false,
                                                 .push_constant_bytes = fw->push_bytes,
                                                 .name = "forward transparent"});
            if (vis->renderer && vis->renderer->caps.ray_query)
                fw->capture = de_gpu_pipeline_create(
                    fw->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_GRAPHICS,
                                                     .vs = code.entries[0].blob,
                                                     .ps = code.entries[2].blob,
                                                     .color_formats = {DE_FMT_RGBA16_FLOAT},
                                                     .color_count = 1,
                                                     .blend = {DE_BLEND_PREMUL},
                                                     .depth_format = DE_FMT_D32_FLOAT,
                                                     .cull = DE_CULL_NONE,
                                                     .depth_test = true,
                                                     .depth_write = false,
                                                     .push_constant_bytes = fw->push_bytes,
                                                     .name = "RT refraction surface capture"});
            de__engine_shader_code_free(&code);
            if (r == DE_OK && vis->renderer && vis->renderer->caps.ray_query && !fw->capture.id)
                r = DE_FAIL(DE_ERR_INTERNAL, "forward: RT capture pipeline");
            if (!DE_HANDLE_VALID(fw->pipeline)) {
                const de_error *e = de_error_last();
                r = DE_FAIL_WRAP(e != NULL && e->result != DE_OK ? e->result : DE_ERR_INTERNAL,
                                 "forward: the pipeline");
            }
        }
    }
    if (r == DE_OK)
        r = de__scene_color_create(vis, &fw->pyramid);
    if (r == DE_OK) {
        fw->enabled = true;
    } else if (r == DE_ERR_UNSUPPORTED) {
        const de_error *e = de_error_last();
        snprintf(fw->why, sizeof fw->why, "%.200s", e != NULL ? e->message : "no engine shaders");
        DE_WARN(vis->log, "forward: the transparent pass is off: %s", fw->why);
        de_error_clear();
    } else {
        de__forward_destroy(fw);
        return r;
    }
    *out = fw;
    return DE_OK;
}

void de__forward_destroy(de__forward *fw) {
    if (fw == NULL)
        return;
    de__scene_color_destroy(fw->pyramid);
    de_gpu_buffer_destroy(fw->dev, fw->indices);
    for (u32 v = 0; v < DE__VIEW_MAX; ++v)
        for (u32 f = 0; f < DE__VIS_FRAME_RING; ++f)
            de_gpu_buffer_destroy(fw->dev, fw->indirect[v][f]);
    de_gpu_pipeline_destroy(fw->dev, fw->capture);
    if (DE_HANDLE_VALID(fw->pipeline))
        de_gpu_pipeline_destroy(fw->dev, fw->pipeline);
    if (DE_HANDLE_VALID(fw->sampler))
        de_gpu_sampler_destroy(fw->dev, fw->sampler);
    DE_FREE(fw->alloc, fw, sizeof *fw);
}

bool de__forward_enabled(const de__forward *fw) {
    return fw != NULL && fw->enabled;
}

u32 de__forward_pipeline_count(const de__forward *fw) {
    return de__forward_enabled(fw) ? 1u : 0u;
}

/* =================================================================================================
 * The passes
 * ============================================================================================== */

static u32 buf_index(const fw_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return DE_HANDLE_VALID(r) ? de_gpu_buffer_bindless(p->fw->dev, de_rg_buffer(pass, r)) : 0u;
}

static void legacy_copy_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const fw_pass *p = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, p->scene_color), 0,
                                      p->pc.scene_color_pitch, de_rg_texture(pass, p->target), 0,
                                      0);
}

static void draw_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const fw_pass *p = (const fw_pass *)user;
    de__fwd_params pc = p->pc;
    pc.scene = buf_index(p, pass, p->views);
    pc.scene_color = buf_index(p, pass, p->scene_color);
    pc.scene_depth = buf_index(p, pass, p->scene_depth);
    pc.lights = buf_index(p, pass, p->lights);
    pc.dir_lights = buf_index(p, pass, p->dir_lights);
    pc.grid = buf_index(p, pass, p->grid);
    pc.list = buf_index(p, pass, p->list);
    pc.counters = buf_index(p, pass, p->counters);
    pc.ltc1 = DE_HANDLE_VALID(p->ltc1)
                  ? de_gpu_texture_bindless(p->fw->dev, de_rg_texture(pass, p->ltc1), 0, false)
                  : 0u;
    pc.ltc2 = DE_HANDLE_VALID(p->ltc2)
                  ? de_gpu_texture_bindless(p->fw->dev, de_rg_texture(pass, p->ltc2), 0, false)
                  : 0u;
    const de_gpu_attachment color = {.texture = de_rg_texture(pass, p->target), .store = true};
    const de_gpu_attachment depth = {.texture = de_rg_texture(pass, p->depth), .store = true};
    if (!p->rt) {
        de_gpu_cmd_begin_render(cmd, &color, 1, &depth);
        de_gpu_cmd_set_pipeline(cmd, p->fw->pipeline);
    }
    de_gpu_cmd_set_index_buffer(cmd, de_rg_buffer(pass, p->indices), 0, false);
    u8 push[128];
    memset(push, 0, sizeof push);
    for (u32 i = 0; i < p->draw_count; ++i) {
        const de__forward_draw *d = &p->draws[i];
        pc.instance = d->instance;
        pc.meshlet_first = d->meshlet_first;
        pc.side = d->side;
        if (p->rt) {
            de__rt_refraction_clear(cmd, p->rt, &pc);
            de_gpu_cmd_begin_render(cmd, &color, 1, &depth);
            de_gpu_cmd_set_pipeline(cmd, p->fw->capture);
            memcpy(push, &pc, sizeof pc);
            de_gpu_cmd_push_constants(cmd, push, p->fw->push_bytes);
            de_gpu_cmd_draw_indexed_indirect(cmd, de_rg_buffer(pass, p->indirect), (u64)i * 20u,
                                             (de_gpu_buffer){0}, 0, 1);
            de_gpu_cmd_end_render(cmd);
            de__rt_refraction_trace(cmd, p->rt, &pc);
            de_gpu_cmd_begin_render(cmd, &color, 1, &depth);
            de_gpu_cmd_set_pipeline(cmd, p->fw->pipeline);
        }
        memcpy(push, &pc, sizeof pc);
        de_gpu_cmd_push_constants(cmd, push, p->fw->push_bytes);
        de_gpu_cmd_draw_indexed_indirect(cmd, de_rg_buffer(pass, p->indirect), (u64)i * 20u,
                                         (de_gpu_buffer){0}, 0, 1);
        if (p->rt)
            de_gpu_cmd_end_render(cmd);
    }
    if (!p->rt)
        de_gpu_cmd_end_render(cmd);
}

de_result de__forward_build(de__forward *fw, de_rg *g, de__vis_context *vc,
                            de__lighting_context *lc, const de__render_world_frame *world,
                            de__forward_context **out) {
    if (out != NULL)
        *out = NULL;
    if (fw && vc && fw->vis->renderer)
        de__rt_refraction_reset(fw->vis->renderer->rt_refraction, vc->view);
    if (fw == NULL || g == NULL || vc == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__forward_build: NULL argument");
    if (!fw->enabled || lc == NULL || !lc->lit || world == NULL || !DE_HANDLE_VALID(lc->target) ||
        !vc->depth_target || !DE_HANDLE_VALID(vc->depth) || !DE_HANDLE_VALID(vc->scene_views) ||
        vc->gpu_scene == NULL)
        return DE_OK;

    /* The list. */
    const de__transparent_view tv = {.gs = vc->gpu_scene,
                                     .world = world,
                                     .cull = &vc->cull,
                                     .layer_mask = vc->layer_mask,
                                     .view_matrix = vc->view_matrix};
    de__transparent_item *items = NULL;
    de__transparent_counts counts = {0};
    const u32 n = de__transparent_gather(&tv, vc->arena, &items, &counts);
    if (counts.allocation_failed)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "forward: transparent gather");
    if (n == 0)
        return DE_OK; /* nothing transparent in view */
    de__transparent_item *scratch =
        (de__transparent_item *)de_arena_push(vc->arena, (size_t)n * sizeof *scratch, 16);
    de__forward_draw *draws =
        (de__forward_draw *)de_arena_push(vc->arena, (size_t)n * 2u * sizeof *draws, 16);
    de__forward_context *fc = (de__forward_context *)de_arena_push_zero(vc->arena, sizeof *fc, 16);
    fw_pass *pd = (fw_pass *)de_arena_push_zero(vc->arena, sizeof *pd, 16);
    if (scratch == NULL || draws == NULL || fc == NULL || pd == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "forward: the frame arena is full");
    de__transparent_sort(items, n, scratch);
    u32 nd = 0;
    bool transmissive = false;
    for (u32 i = 0; i < n; ++i) {
        const de__transparent_item *it = &items[i];
        if (it->flags & DE__FWD_ITEM_DOUBLE_SIDED)
            draws[nd++] = (de__forward_draw){.instance = it->instance,
                                             .side = DE__FWD_SIDE_BACK,
                                             .meshlet_first = it->meshlet_first,
                                             .meshlet_count = it->meshlet_count};
        draws[nd++] = (de__forward_draw){.instance = it->instance,
                                         .side = DE__FWD_SIDE_FRONT,
                                         .meshlet_first = it->meshlet_first,
                                         .meshlet_count = it->meshlet_count};
        transmissive |= (it->flags & DE__FWD_ITEM_TRANSMISSIVE) != 0u;
    }

    *pd = (fw_pass){.fw = fw,
                    .pc = {.view = lc->constants_bindless,
                           .entry = lc->constants_entry,
                           .view_slot = vc->scene_view_slot,
                           .sampler = fw->sampler_bindless,
                           .refraction = !fw->cv_refraction.id || de_cvar_bool(fw->cv_refraction),
                           .width = lc->width,
                           .height = lc->height,
                           .ltc_sampler = lc->sampler_bindless},
                    .target = lc->target,
                    .depth = vc->depth,
                    .views = vc->scene_views,
                    .lights = lc->lights,
                    .dir_lights = lc->dir_lights,
                    .grid = lc->grid_buf,
                    .list = lc->list,
                    .counters = lc->counters,
                    .ltc1 = lc->ltc1,
                    .ltc2 = lc->ltc2,
                    .draws = draws,
                    .draw_count = nd};

    /* Sorted indirect arguments persist in a per-view frame ring; capacity changes are
     * the only allocation points. A shared identity index buffer preserves SV_VertexID. */
    if (!fw->indices.id) {
        fw->indices = de_gpu_buffer_create(
            fw->dev, &(de_gpu_buffer_desc){.size = DE__FWD_VERTICES * sizeof(u32),
                                           .usage = DE_USAGE_INDEX,
                                           .memory = DE_MEM_UPLOAD,
                                           .name = "forward identity indices"});
        u32 *indices = de_gpu_buffer_map(fw->dev, fw->indices);
        if (!indices)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "forward identity indices");
        for (u32 i = 0; i < DE__FWD_VERTICES; ++i)
            indices[i] = i;
    }
    u32 slot = vc->view_slot, ring = (u32)(vc->frame_index % DE__VIS_FRAME_RING);
    if (slot >= DE__VIEW_MAX)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "forward view slot");
    if (fw->indirect_capacity[slot][ring] < nd) {
        u32 capacity = 64;
        while (capacity < nd)
            capacity *= 2;
        de_gpu_buffer args = de_gpu_buffer_create(
            fw->dev, &(de_gpu_buffer_desc){.size = (u64)capacity * 20,
                                           .usage = DE_USAGE_INDIRECT,
                                           .memory = DE_MEM_UPLOAD,
                                           .name = "forward sorted indirect draws"});
        if (!args.id)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "forward indirect arguments");
        de_gpu_buffer_destroy(fw->dev, fw->indirect[slot][ring]);
        fw->indirect[slot][ring] = args;
        fw->indirect_capacity[slot][ring] = capacity;
    }
    u32 *arguments = de_gpu_buffer_map(fw->dev, fw->indirect[slot][ring]);
    if (!arguments)
        return DE_FAIL(DE_ERR_INTERNAL, "forward indirect map");
    for (u32 i = 0; i < nd; ++i) {
        arguments[i * 5] = DE__FWD_VERTICES;
        arguments[i * 5 + 1] = draws[i].meshlet_count;
        arguments[i * 5 + 2] = arguments[i * 5 + 3] = arguments[i * 5 + 4] = 0;
    }
    pd->indirect = de_rg_import_buffer(g, fw->indirect[slot][ring], DE_ACCESS_INDIRECT);
    pd->indices = de_rg_import_buffer(g, fw->indices, DE_ACCESS_SHADER_READ);

    /* The scene colour behind the transmissive surfaces (the lighting target as lighting left
     * it), copied only when one is listed. */
    if (transmissive && !pd->pc.refraction) {
        pd->pc.scene_color_pitch = (lc->width * 8u + 255u) & ~255u;
        pd->pc.scene_color_levels = 1;
        pd->scene_color = de_rg_create_buffer(
            g, &(de_gpu_buffer_desc){.size = (u64)pd->pc.scene_color_pitch * lc->height,
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST,
                                     .name = "forward.scene_color"});
        if (!pd->scene_color.id)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "forward: legacy scene copy");
        de_rg_pass *copy =
            de_rg_add_pass(g, "forward.scene_color", DE_QUEUE_GRAPHICS, legacy_copy_execute, pd);
        if (!copy)
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "forward: legacy copy pass");
        de_rg_read(copy, lc->target, DE_ACCESS_COPY_SRC);
        de_rg_write(copy, pd->scene_color, DE_ACCESS_COPY_DST);
    } else if (transmissive) {
        de__scene_color_context context;
        de_result result = de__scene_color_build(fw->pyramid, g, vc->arena, lc->target, vc->depth,
                                                 lc->width, lc->height, &context);
        if (result != DE_OK)
            return result;
        pd->scene_color = context.color;
        pd->scene_depth = context.depth;
        pd->pc.scene_color_pitch = context.layout.level[0].pitch;
        pd->pc.scene_depth_pitch = context.layout.depth_pitch;
        pd->pc.scene_color_levels = context.layout.count;
    }

    if (fw->capture.id && fw->vis->renderer && transmissive && pd->pc.refraction)
        pd->rt = de__rt_refraction_prepare(fw->vis->renderer->rt_refraction, g, vc, lc);
    de_rg_pass *p = de_rg_add_pass(g, "forward.transparent", DE_QUEUE_GRAPHICS, draw_execute, pd);
    if (!p)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "forward: draw pass");
    de__rt_refraction_declare(pd->rt, p);
    de_rg_read(p, pd->indirect, DE_ACCESS_INDIRECT);
    de_rg_read(p, pd->indices, DE_ACCESS_SHADER_READ);
    de_rg_read(p, lc->target, DE_ACCESS_RENDER_TARGET);
    de_rg_write(p, lc->target, DE_ACCESS_RENDER_TARGET);
    /* The pipeline writes no depth, but the attachment is bound in its write layout. */
    de_rg_read(p, vc->depth, DE_ACCESS_DEPTH_READ);
    de_rg_write(p, vc->depth, DE_ACCESS_DEPTH_READ | DE_ACCESS_DEPTH_WRITE);
    de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
    const de_rg_resource tables[3] = {vc->scene_instances, vc->scene_meshes, vc->scene_materials};
    for (u32 i = 0; i < 3; ++i)
        if (DE_HANDLE_VALID(tables[i]))
            de_rg_read(p, tables[i], DE_ACCESS_SHADER_READ);
    const de_rg_resource reads[8] = {lc->lights,   lc->dir_lights, lc->grid_buf, lc->list,
                                     lc->counters, lc->ltc1,       lc->ltc2,     lc->strip};
    for (u32 i = 0; i < 8; ++i)
        if (DE_HANDLE_VALID(reads[i]))
            de_rg_read(p, reads[i], DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(lc->shadow_records))
        de_rg_read(p, lc->shadow_records, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(lc->atlas_records))
        de_rg_read(p, lc->atlas_records, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(lc->atlas_texture))
        de_rg_read(p, lc->atlas_texture, DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->shadow_texture_count; ++i)
        de_rg_read(p, lc->shadow_textures[i], DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(lc->gi_records))
        de_rg_read(p, lc->gi_records, DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->gi_texture_count; ++i) {
        de_rg_read(p, lc->gi_textures[i], DE_ACCESS_SHADER_READ);
        de_rg_write(p, lc->gi_textures[i], DE_ACCESS_SHADER_WRITE);
    }
    for (u32 i = 0; i < lc->gi_buffer_count; ++i)
        de_rg_read(p, lc->gi_buffers[i], DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(lc->probe_records))
        de_rg_read(p, lc->probe_records, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(lc->probe_list))
        de_rg_read(p, lc->probe_list, DE_ACCESS_SHADER_READ);
    for (u32 i = 0; i < lc->probe_texture_count; ++i)
        de_rg_read(p, lc->probe_textures[i], DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(pd->scene_color)) {
        de_rg_read(p, pd->scene_color, DE_ACCESS_SHADER_READ);
        if (pd->scene_depth.id)
            de_rg_read(p, pd->scene_depth, DE_ACCESS_SHADER_READ);
    }

    if (g->oom || g->error_count)
        return DE_FAIL(g->oom ? DE_ERR_OUT_OF_MEMORY : DE_ERR_INTERNAL,
                       "forward: incomplete graph declarations");
    *fc = (de__forward_context){.items = items,
                                .count = n,
                                .draws = draws,
                                .draw_count = nd,
                                .counts = counts,
                                .target = lc->target,
                                .scene_color = pd->scene_color,
                                .scene_color_pitch = pd->pc.scene_color_pitch,
                                .scene_depth = pd->scene_depth,
                                .scene_depth_pitch = pd->pc.scene_depth_pitch,
                                .scene_color_levels = pd->pc.scene_color_levels};
    if (out != NULL)
        *out = fc;
    return DE_OK;
}

/* =================================================================================================
 * The renderer's view stage
 * ============================================================================================== */

static void forward_stage(de__view_frame *vf, void *user) {
    de__forward *fw = (de__forward *)user;
    if (!de__forward_enabled(fw))
        return;
    de__vis_context *vc = de__vis_frame_context(fw->vis, vf->view, vf->graph, vf->frame_index);
    de__lighting_context *lc =
        de__lighting_frame_context(fw->lighting, vf->view, vf->graph, vf->frame_index);
    if (vc == NULL || lc == NULL)
        return;
    const de_result res = de__forward_build(fw, vf->graph, vc, lc, vf->world, NULL);
    if (res != DE_OK) {
        vf->graph->oom = true;
        const de_error *e = de_error_last();
        DE_ERROR(fw->vis->log, "forward: view \"%s\": %s", vf->name,
                 e != NULL ? e->message : de_result_str(res));
    }
}

de_result de__forward_create(de_renderer *r, de__vis_shared *vis, de__lighting *lighting,
                             de__forward **out) {
    *out = NULL;
    if (vis == NULL || lighting == NULL)
        return DE_OK; /* no vis frame or lighting: nothing to draw into */
    de__forward *fw = NULL;
    de_result res = de__forward_create_ex(vis, lighting, &fw);
    if (res != DE_OK)
        return res;
    res = de__view_register_stage(r, DE__VIEW_SLOT_TRANSPARENT, 0, "forward.transparent",
                                  forward_stage, fw);
    if (res != DE_OK) {
        de__forward_destroy(fw);
        return res;
    }
    *out = fw;
    return DE_OK;
}
