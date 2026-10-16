/*
 * raster_alpha.c: the alpha-tested raster of the visibility buffer (SPEC §12.5 step 2, §12.6;
 * DENG-236). shaders/visbuffer/vis_alpha.slang has the shaders (engine shader "vis_alpha");
 * vis_stages.h the stage contract.
 *
 * Meshlet culling (cull_meshlets.c, meshlet_cull.slang) sends meshlets whose submesh material is
 * alpha-tested (READY, alpha_mode mask) to the alpha bucket: the region [visible_capacity,
 * visible_capacity + visible_alpha_capacity) of the view's one visible-meshlet list, with its own
 * raster arguments per phase (de__vis_context.raster_args_alpha). This stage draws them into the
 * opaque raster's targets (vis ID and depth) right after each phase's opaque raster (vis_frame.c
 * calls it per phase), so phase 1's depth, and the HZB built from it, holds the cutout occluders.
 * The pixel shader discards where the material's base-colour alpha is below alpha_cutoff; the
 * opaque pipelines (raster_mesh.c, raster_compute.c) never do, so they keep early depth.
 *
 * Per view and phase, on the graphics queue, on the frame's raster path (vc->raster_path):
 *
 *   mesh path
 *     vis.raster_alpha.mesh[.phase2]   one mesh-shader group per alpha    tables, list: READ; alpha
 *                                      meshlet (ms_alpha + ps_alpha_mesh) args: INDIRECT + READ;
 *                                                                         vis ID: RENDER_TARGET;
 *                                                                         depth: DEPTH R + W
 *   compute path (vis_expand.slang's kernels, unchanged, on the alpha bucket)
 *     vis.raster_alpha.clear           first phase: the counters          counters: WRITE
 *     vis.raster_alpha.expand[.phase2] the alpha meshlets into triangle   as raster_compute.c
 *                                      slots
 *     vis.raster_alpha.args[.phase2]   the phase's indexed indirect draw
 *     vis.raster_alpha.draw[.phase2]   vs_alpha + ps_alpha_draw           as raster_compute.c
 *
 * Zero work without alpha-tested materials: when the GPU scene references no READY mask material
 * (de__gpu_scene_stats.mask_materials, counted where the materials are packed), the stage adds no
 * pass. With some referenced but none in view, the bucket is empty and the GPU-written arguments
 * make the passes one empty mesh-shader group, or one empty expansion group and an indexed draw of
 * zero indices.
 *
 * Capacity: the mesh path dispatches once (the alpha region is at most DE__VIS_MESH_GROUPS_MAX
 * entries, cull_meshlets.c). The compute path's slot buffers hold every triangle the region can
 * hold (124 per meshlet, at most r.raster_compute_max_tris), so they never overflow; they follow
 * the region's capacity, which grows from what meshlet culling read back.
 *
 * Sampler: the pass's own (linear, wrap): DeGpuMaterial has no sampler before S14's material
 * instances. One sample per pixel, so no alpha to coverage.
 */
#include "render/passes/vis/vis_stages.h"

#include "render/formats/de_cooked_mesh.h"
#include "render/scene/gpu_scene.h"

#include <string.h>

/** Threads per mesh-shader group (vis_alpha.slang ms_alpha). */
#define RA_GROUP 64u
/** AlphaParams.flags */
#define RA_TRI_CULL 1u

/** The push constants of every vis_alpha entry (vis_alpha.slang AlphaParams, 32 bytes). */
typedef struct ra_params {
    u32 scene, view_slot, visible, args, tris, flags, sampler, reserved;
} ra_params;
DE_STATIC_ASSERT(sizeof(ra_params) == 32, "AlphaParams is 32 bytes");

/** vis_expand.slang's ExpandParams (80 bytes; raster_compute.c has the same mirror). */
typedef struct ra_expand_params {
    u32 instances, meshes, materials, material_count, scene, view_slot, visible, raster_args;
    u32 counters, tris, indices, draw_args, capacity, phase, flags;
    f32 width, height;
    u32 reserved[3];
} ra_expand_params;
DE_STATIC_ASSERT(sizeof(ra_expand_params) == 80, "ExpandParams is 80 bytes");

/** One view slot's buffers of this frame (compute path). */
typedef struct ra_view {
    const de_rg *frame_graph;
    u64 frame;
    de_rg_resource counters, tris, indices;
    u32 capacity;
} ra_view;

typedef struct ra_state {
    de_gpu_pipeline mesh; /**< ms_alpha + ps_alpha_mesh (invalid without mesh shaders) */
    de_gpu_pipeline draw; /**< vs_alpha + ps_alpha_draw */
    u32 mesh_push, draw_push;
    de__vis_kernel clear, expand, args; /**< vis_raster_expand's kernels */
    de_gpu_sampler sampler;
    u32 sampler_bindless;
    de_cvar cv_tri_cull, cv_max_tris;
    ra_view views[DE__VIEW_MAX];
} ra_state;

static const de__vis_layout_const k_consts[] = {
    {"DE_VIS_ALPHA_TEX_BASE_COLOR", 0u /* DE_COOKED_TEX_BASE_COLOR */},
    {"DE_VIS_ALPHA_TRI_CULL", RA_TRI_CULL},
};

const de__vis_layout_const *de__vis_raster_alpha_layout_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

/* =================================================================================================
 * Init / shutdown
 * ============================================================================================== */

static ra_state *state_of(const de__vis_shared *s) {
    return s != NULL ? (ra_state *)s->stage[DE__VIS_STAGE_RASTER_ALPHA] : NULL;
}

static de_result make_pipeline(de__vis_shared *s, const de_gpu_pipeline_desc *d,
                               de_gpu_pipeline *out) {
    *out = de_gpu_pipeline_create(s->dev, d);
    if (DE_HANDLE_VALID(*out))
        return DE_OK;
    const de_error *e = de_error_last();
    return DE_FAIL_WRAP(e != NULL && e->result != DE_OK ? e->result : DE_ERR_INTERNAL,
                        "vis: the pipeline %s", d->name);
}

static void state_free(de__vis_shared *s, ra_state *st) {
    if (DE_HANDLE_VALID(st->mesh))
        de_gpu_pipeline_destroy(s->dev, st->mesh);
    if (DE_HANDLE_VALID(st->draw))
        de_gpu_pipeline_destroy(s->dev, st->draw);
    de__vis_kernel_destroy(s, &st->clear);
    de__vis_kernel_destroy(s, &st->expand);
    de__vis_kernel_destroy(s, &st->args);
    if (DE_HANDLE_VALID(st->sampler))
        de_gpu_sampler_destroy(s->dev, st->sampler);
    DE_FREE(s->alloc, st, sizeof *st);
}

static u32 push_of(u32 reflected, u32 own) {
    const u32 b = reflected > own ? reflected : own;
    return (b + 3u) & ~3u;
}

de_result de__vis_raster_alpha_init(de__vis_shared *s) {
    ra_state *st = DE_NEW(s->alloc, ra_state, DE_MEM_RENDER);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: alpha raster state");
    memset(st, 0, sizeof *st);
    /* The raster paths registered these (registering again returns the handle). */
    st->cv_tri_cull = de_cvar_find("r.raster_tri_cull");
    st->cv_max_tris = de_cvar_find("r.raster_compute_max_tris");
    de_error_clear(); /* r.raster_tri_cull is Dev only: absent in Shipping */
    st->sampler = de_gpu_sampler_create(s->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                       .mag = DE_FILTER_LINEAR,
                                                                       .mip = DE_FILTER_LINEAR});
    de_result r = DE_HANDLE_VALID(st->sampler)
                      ? DE_OK
                      : DE_FAIL_WRAP(DE_ERR_INTERNAL, "vis: the alpha-test sampler");
    if (r == DE_OK)
        st->sampler_bindless = de_gpu_sampler_bindless(s->dev, st->sampler);

    /* The compute path: vis_expand.slang's kernels, and the alpha draw. */
    de__engine_shader_code code;
    if (r == DE_OK) {
        static const char *const k_kernels[3] = {"cs_expand_clear", "cs_expand", "cs_expand_args"};
        r = de__vis_shader_load(s, "vis_raster_expand", k_kernels, 3, &code);
        if (r == DE_OK) {
            static const u32 one[3] = {1u, 1u, 1u}, group[3] = {DE__VIS_EXPAND_GROUP, 1u, 1u};
            const u32 pb = (u32)sizeof(ra_expand_params);
            r = de__vis_kernel_create(s, &code, 0, "vis alpha expand clear", one, pb, &st->clear);
            if (r == DE_OK)
                r = de__vis_kernel_create(s, &code, 1, "vis alpha expand", group, pb, &st->expand);
            if (r == DE_OK)
                r = de__vis_kernel_create(s, &code, 2, "vis alpha expand args", one, pb, &st->args);
            de__engine_shader_code_free(&code);
        }
    }
    if (r == DE_OK) {
        const char *const k_entries[4] = {
            "ms_alpha", s->depth_only ? "ps_shadow_alpha_mesh" : "ps_alpha_mesh", "vs_alpha",
            s->depth_only ? "ps_shadow_alpha_draw" : "ps_alpha_draw"};
        r = de__vis_shader_load(s, s->depth_only ? "shadow_depth" : "vis_alpha", k_entries, 4,
                                &code);
        if (r == DE_OK) {
            st->draw_push = push_of(code.entries[2].push_constant_bytes, (u32)sizeof(ra_params));
            r = make_pipeline(s,
                              &(de_gpu_pipeline_desc){.kind = DE_PIPE_GRAPHICS,
                                                      .vs = code.entries[2].blob,
                                                      .ps = code.entries[3].blob,
                                                      .color_formats = {DE_FMT_R32_UINT},
                                                      .color_count = s->depth_only ? 0u : 1u,
                                                      .depth_format = DE_FMT_D32_FLOAT,
                                                      .cull = DE_CULL_NONE,
                                                      .depth_test = true,
                                                      .depth_write = true,
                                                      .push_constant_bytes = st->draw_push,
                                                      .name = "vis alpha draw"},
                              &st->draw);
            if (r == DE_OK && de_gpu_get_caps(s->dev)->mesh_shaders) {
                st->mesh_push =
                    push_of(code.entries[0].push_constant_bytes, (u32)sizeof(ra_params));
                r = make_pipeline(s,
                                  &(de_gpu_pipeline_desc){.kind = DE_PIPE_MESH,
                                                          .ms = code.entries[0].blob,
                                                          .ps = code.entries[1].blob,
                                                          .color_formats = {DE_FMT_R32_UINT},
                                                          .color_count = s->depth_only ? 0u : 1u,
                                                          .depth_format = DE_FMT_D32_FLOAT,
                                                          .cull = DE_CULL_NONE,
                                                          .depth_test = true,
                                                          .depth_write = true,
                                                          .push_constant_bytes = st->mesh_push,
                                                          .threadgroup_size = {RA_GROUP, 1u, 1u},
                                                          .name = "vis alpha mesh"},
                                  &st->mesh);
            }
            de__engine_shader_code_free(&code);
        }
    }
    if (r != DE_OK) {
        state_free(s, st);
        return r;
    }
    s->stage[DE__VIS_STAGE_RASTER_ALPHA] = st;
    return DE_OK;
}

void de__vis_raster_alpha_shutdown(de__vis_shared *s) {
    ra_state *st = state_of(s);
    if (st == NULL)
        return;
    state_free(s, st);
    s->stage[DE__VIS_STAGE_RASTER_ALPHA] = NULL;
}

de__vis_raster_alpha_info de__vis_raster_alpha_get_info(const de__vis_shared *s) {
    const ra_state *st = state_of(s);
    if (st == NULL)
        return (de__vis_raster_alpha_info){0};
    return (de__vis_raster_alpha_info){.mesh = DE_HANDLE_VALID(st->mesh),
                                       .compute = DE_HANDLE_VALID(st->draw),
                                       .sampler = st->sampler_bindless};
}

/* =================================================================================================
 * Passes
 * ============================================================================================== */

typedef struct ra_pass {
    de_gpu_device *dev;
    de_gpu_pipeline pipeline;
    de__vis_kernel kernel;
    u32 push_bytes;
    ra_params pc;
    ra_expand_params ec;
    de_rg_resource instances, meshes, materials, scene, visible, args, counters, tris, indices,
        draw_args, vis_ids, depth;
    bool depth_only;
    u32 layer, rect[4];
} ra_pass;

static u32 bindless_of(const ra_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return DE_HANDLE_VALID(r) ? de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, r)) : 0u;
}

static ra_params draw_params(const ra_pass *p, const de_rg_pass *pass) {
    ra_params pc = p->pc;
    pc.scene = bindless_of(p, pass, p->scene);
    pc.visible = bindless_of(p, pass, p->visible);
    pc.args = bindless_of(p, pass, p->args);
    pc.tris = bindless_of(p, pass, p->tris);
    return pc;
}

static void begin_targets(de_gpu_cmd *cmd, const ra_pass *p, const de_rg_pass *pass) {
    const de_gpu_attachment color = {.texture = p->depth_only ? (de_gpu_texture){0}
                                                              : de_rg_texture(pass, p->vis_ids),
                                     .store = true};
    const de_gpu_attachment depth = {
        .texture = de_rg_texture(pass, p->depth), .layer = p->layer, .store = true};
    de_gpu_cmd_begin_render(cmd, p->depth_only ? NULL : &color, p->depth_only ? 0u : 1u, &depth);
    if (p->rect[2]) {
        de_gpu_cmd_viewport(cmd, (f32)p->rect[0], (f32)p->rect[1], (f32)p->rect[2],
                            (f32)p->rect[3]);
        de_gpu_cmd_scissor(cmd, (i32)p->rect[0], (i32)p->rect[1], p->rect[2], p->rect[3]);
    }
}

static void mesh_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ra_pass *p = (const ra_pass *)user;
    const ra_params pc = draw_params(p, pass);
    begin_targets(cmd, p, pass);
    de_gpu_cmd_set_pipeline(cmd, p->pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, p->push_bytes);
    de_gpu_cmd_dispatch_mesh_indirect(cmd, de_rg_buffer(pass, p->args), DE__VIS_RASTER_ARGS_MESH,
                                      (de_gpu_buffer){0}, 0, 1);
    de_gpu_cmd_end_render(cmd);
}

static void kernel_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ra_pass *p = (const ra_pass *)user;
    ra_expand_params ec = p->ec;
    ec.instances = bindless_of(p, pass, p->instances);
    ec.meshes = bindless_of(p, pass, p->meshes);
    ec.materials = bindless_of(p, pass, p->materials);
    ec.scene = bindless_of(p, pass, p->scene);
    ec.visible = bindless_of(p, pass, p->visible);
    ec.raster_args = bindless_of(p, pass, p->args);
    ec.counters = bindless_of(p, pass, p->counters);
    ec.tris = bindless_of(p, pass, p->tris);
    ec.indices = bindless_of(p, pass, p->indices);
    ec.draw_args = bindless_of(p, pass, p->draw_args);
    de_gpu_cmd_set_pipeline(cmd, p->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &ec, (u32)sizeof ec);
    if (DE_HANDLE_VALID(p->args)) /* the expansion; clear and args: one thread */
        de_gpu_cmd_dispatch_indirect(cmd, de_rg_buffer(pass, p->args), DE__VIS_RASTER_ARGS_EXPAND);
    else
        de_gpu_cmd_dispatch(cmd, 1, 1, 1);
}

static void draw_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ra_pass *p = (const ra_pass *)user;
    const ra_params pc = draw_params(p, pass);
    begin_targets(cmd, p, pass);
    de_gpu_cmd_set_pipeline(cmd, p->pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, p->push_bytes);
    de_gpu_cmd_set_index_buffer(cmd, de_rg_buffer(pass, p->indices), 0, false);
    de_gpu_cmd_draw_indexed_indirect(cmd, de_rg_buffer(pass, p->draw_args), 0, (de_gpu_buffer){0},
                                     0, 1);
    de_gpu_cmd_end_render(cmd);
}

static ra_pass *pass_data(de__vis_context *vc, const ra_pass *base) {
    ra_pass *p = (ra_pass *)de_arena_push(vc->arena, sizeof *p, 16);
    if (p != NULL)
        *p = *base;
    else
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: alpha raster: the frame arena is full");
    return p;
}

/* The reads every alpha draw declares (tables, list, targets). */
static void declare_draw(de_rg_pass *p, const de__vis_context *vc) {
    de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_instances, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_meshes, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(vc->scene_materials))
        de_rg_read(p, vc->scene_materials, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->visible_meshlets, DE_ACCESS_SHADER_READ);
    if (!vc->depth_only) {
        de_rg_read(p, vc->vis_ids, DE_ACCESS_RENDER_TARGET);
        de_rg_write(p, vc->vis_ids, DE_ACCESS_RENDER_TARGET);
    }
    de_rg_read(p, vc->depth, DE_ACCESS_DEPTH_READ);
    de_rg_write(p, vc->depth, DE_ACCESS_DEPTH_READ | DE_ACCESS_DEPTH_WRITE);
}

static void mesh_phase(de_rg *g, de__vis_context *vc, ra_state *st, const ra_pass *base, bool p1,
                       u32 pi) {
    ra_pass b = *base;
    b.pipeline = st->mesh;
    b.push_bytes = st->mesh_push;
    b.args = vc->raster_args_alpha[pi];
    ra_pass *pm = pass_data(vc, &b);
    if (pm == NULL)
        return;
    de_rg_pass *p = de_rg_add_pass(g, p1 ? "vis.raster_alpha.mesh" : "vis.raster_alpha.mesh.phase2",
                                   DE_QUEUE_GRAPHICS, mesh_execute, pm);
    declare_draw(p, vc);
    de_rg_read(p, vc->raster_args_alpha[pi], DE_ACCESS_INDIRECT | DE_ACCESS_SHADER_READ);
    vc->alpha_recorded = true;
}

static void compute_phase(de_rg *g, de__vis_context *vc, ra_state *st, const ra_pass *base, bool p1,
                          u32 pi) {
    ra_view *pv = &st->views[vc->view_slot];
    const bool first =
        !(pv->frame_graph == g && pv->frame == vc->frame_index && DE_HANDLE_VALID(pv->counters));
    if (first) {
        i64 max = DE_HANDLE_VALID(st->cv_max_tris) ? de_cvar_int(st->cv_max_tris)
                                                   : (i64)DE__VIS_RASTER_COMPUTE_MAX_TRIS_DEFAULT;
        if (max < 1)
            max = 1;
        u64 cap = (u64)vc->visible_alpha_capacity * DE_COOKED_MESHLET_MAX_TRIANGLES;
        if (cap > (u64)max)
            cap = (u64)max;
        if (cap == 0)
            cap = 1;
        const de_rg_resource tris = de_rg_create_buffer(
            g, &(de_gpu_buffer_desc){.size = cap * 4u,
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                     .name = "vis.raster_alpha_tris"});
        const de_rg_resource indices = de_rg_create_buffer(
            g, &(de_gpu_buffer_desc){.size = cap * 12u,
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_INDEX | DE_USAGE_COPY_SRC,
                                     .name = "vis.raster_alpha_indices"});
        const de_rg_resource counters = de_rg_create_buffer(
            g, &(de_gpu_buffer_desc){.size = DE__VIS_EXPAND_COUNTER_BYTES,
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                     .name = "vis.raster_alpha_counters"});
        if (!DE_HANDLE_VALID(tris) || !DE_HANDLE_VALID(indices) || !DE_HANDLE_VALID(counters))
            return; /* error set; finalize reports it */
        pv->frame_graph = g;
        pv->frame = vc->frame_index;
        pv->tris = tris;
        pv->indices = indices;
        pv->counters = counters;
        pv->capacity = (u32)cap;
    }
    const de_rg_resource draw_args = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = sizeof(de__vis_draw_indexed_args),
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT | DE_USAGE_COPY_SRC,
                                 .name = p1 ? "vis.raster_alpha_draw_args"
                                            : "vis.raster_alpha_draw_args.phase2"});
    if (!DE_HANDLE_VALID(draw_args))
        return;
    const de_rg_resource args = vc->raster_args_alpha[pi];
    ra_pass b = *base;
    b.ec.capacity = pv->capacity;
    b.ec.phase = p1 ? 1u : 2u;
    /* Each pass's data names only what it declares (de_rg_buffer needs a declared access). */
    ra_pass b_clear = {.dev = b.dev, .kernel = st->clear, .ec = b.ec, .counters = pv->counters};
    ra_pass b_expand = b;
    b_expand.kernel = st->expand;
    b_expand.args = args;
    b_expand.counters = pv->counters;
    b_expand.tris = pv->tris;
    b_expand.indices = pv->indices;
    b_expand.vis_ids = (de_rg_resource){0};
    b_expand.depth = (de_rg_resource){0};
    ra_pass b_args = {.dev = b.dev,
                      .kernel = st->args,
                      .ec = b.ec,
                      .counters = pv->counters,
                      .draw_args = draw_args};
    ra_pass b_draw = b;
    b_draw.pipeline = st->draw;
    b_draw.push_bytes = st->draw_push;
    b_draw.materials = (de_rg_resource){0};
    b_draw.tris = pv->tris;
    b_draw.indices = pv->indices;
    b_draw.draw_args = draw_args;
    ra_pass *pc = first ? pass_data(vc, &b_clear) : NULL;
    ra_pass *pe = pass_data(vc, &b_expand), *pa = pass_data(vc, &b_args),
            *pd = pass_data(vc, &b_draw);
    if ((first && pc == NULL) || pe == NULL || pa == NULL || pd == NULL)
        return;
    de_rg_pass *p;
    if (first) {
        p = de_rg_add_pass(g, "vis.raster_alpha.clear", DE_QUEUE_GRAPHICS, kernel_execute, pc);
        de_rg_write(p, pv->counters, DE_ACCESS_SHADER_WRITE);
    }
    p = de_rg_add_pass(g, p1 ? "vis.raster_alpha.expand" : "vis.raster_alpha.expand.phase2",
                       DE_QUEUE_GRAPHICS, kernel_execute, pe);
    de_rg_read(p, vc->scene_instances, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_meshes, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(b.materials))
        de_rg_read(p, b.materials, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->visible_meshlets, DE_ACCESS_SHADER_READ);
    de_rg_read(p, args, DE_ACCESS_INDIRECT | DE_ACCESS_SHADER_READ);
    de_rg_read(p, pv->counters, DE_ACCESS_SHADER_READ); /* atomics */
    de_rg_write(p, pv->counters, DE_ACCESS_SHADER_WRITE);
    if (!first) { /* keep phase 1's slots */
        de_rg_read(p, pv->tris, DE_ACCESS_SHADER_READ);
        de_rg_read(p, pv->indices, DE_ACCESS_SHADER_READ);
    }
    de_rg_write(p, pv->tris, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, pv->indices, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, p1 ? "vis.raster_alpha.args" : "vis.raster_alpha.args.phase2",
                       DE_QUEUE_GRAPHICS, kernel_execute, pa);
    de_rg_read(p, pv->counters, DE_ACCESS_SHADER_READ);
    de_rg_write(p, pv->counters, DE_ACCESS_SHADER_WRITE); /* phase 1 stores its end */
    de_rg_write(p, draw_args, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, p1 ? "vis.raster_alpha.draw" : "vis.raster_alpha.draw.phase2",
                       DE_QUEUE_GRAPHICS, draw_execute, pd);
    declare_draw(p, vc);
    de_rg_read(p, draw_args, DE_ACCESS_INDIRECT);
    de_rg_read(p, pv->indices, DE_ACCESS_SHADER_READ); /* the index buffer */
    de_rg_read(p, pv->tris, DE_ACCESS_SHADER_READ);
    vc->alpha_recorded = true;
}

void de__vis_raster_alpha(de_rg *g, de__vis_context *vc, de__vis_phase phase) {
    ra_state *st = state_of(vc->shared);
    const u32 pi = DE__VIS_PHASE_INDEX(phase);
    if (st == NULL || vc->view_slot >= DE__VIEW_MAX ||
        !DE_HANDLE_VALID(vc->raster_args_alpha[pi]) || !DE_HANDLE_VALID(vc->visible_meshlets) ||
        !DE_HANDLE_VALID(vc->scene_instances) || !DE_HANDLE_VALID(vc->scene_meshes) ||
        !DE_HANDLE_VALID(vc->scene_views) || !vc->depth_target || !DE_HANDLE_VALID(vc->depth))
        return;
    /* No alpha-tested material referenced: no alpha pass at all (and no alpha pipeline bound). */
    if (vc->gpu_scene == NULL || de__gpu_scene_stats_get(vc->gpu_scene)->mask_materials == 0)
        return;
    const bool mesh = vc->raster_path == (u32)DE__VIS_RASTER_MESH && DE_HANDLE_VALID(st->mesh);
    const bool compute = vc->raster_path == (u32)DE__VIS_RASTER_COMPUTE;
    if (!mesh && !compute)
        return;
    if (!DE_HANDLE_VALID(de__vis_raster_targets(g, vc)))
        return;
    const bool materials = DE_HANDLE_VALID(vc->scene_materials) && vc->gpu_scene != NULL;
    const de_cvar tc = st->cv_tri_cull;
    const bool tri_cull = !DE_HANDLE_VALID(tc) || de_cvar_bool(tc);
    const ra_pass base = {
        .dev = vc->shared->dev,
        .pc = {.view_slot = vc->scene_view_slot,
               .flags = tri_cull ? RA_TRI_CULL : 0u,
               .sampler = st->sampler_bindless},
        .ec = {.material_count =
                   materials ? de__gpu_scene_table(vc->gpu_scene, DE__GS_MATERIALS).count : 0u,
               .view_slot = vc->scene_view_slot,
               .flags = tri_cull ? DE__VIS_EXPAND_TRI_CULL : 0u,
               .width = (f32)vc->width,
               .height = (f32)vc->height},
        .instances = vc->scene_instances,
        .meshes = vc->scene_meshes,
        .materials = materials ? vc->scene_materials : (de_rg_resource){0},
        .scene = vc->scene_views,
        .visible = vc->visible_meshlets,
        .vis_ids = vc->vis_ids,
        .depth = vc->depth,
        .depth_only = vc->depth_only,
        .layer = vc->depth_layer,
        .rect = {vc->raster_rect[0], vc->raster_rect[1], vc->raster_rect[2], vc->raster_rect[3]}};
    const bool p1 = phase == DE__VIS_PHASE_1;
    if (mesh)
        mesh_phase(g, vc, st, &base, p1, pi);
    else
        compute_phase(g, vc, st, &base, p1, pi);
}

void de__vis_raster_alpha_abort(de__vis_shared *s, u32 slot, u64 frame) {
    (void)frame;
    ra_state *st = state_of(s);
    if (st && slot < DE__VIEW_MAX)
        st->views[slot].frame_graph = NULL;
}
