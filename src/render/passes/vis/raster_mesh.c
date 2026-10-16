/*
 * raster_mesh.c: the mesh-shader vis-buffer raster, the third stage of the visibility-buffer frame
 * (SPEC §12.5 step 2, §3.3; DENG-234). shaders/visbuffer/vis_mesh.slang has the shaders (engine
 * shader "vis_mesh"), shaders/visbuffer/vis_decode.slang the geometry decode every raster path
 * shares; vis_stages.h the stage contract. vis_stages_stub.c's raster stage picks the path
 * (de__vis_raster_choose) and calls de__vis_raster_mesh for the mesh path.
 *
 * Per view and frame, on the graphics queue:
 *
 *   vis.vis_ids_clear       phase 1: a full-screen triangle writes  vis_ids: RENDER_TARGET
 *                           DE__VIS_ID_EMPTY (de__vis_raster_targets)
 *   vis.raster.mesh_split   only when a phase may hold more than    raster args: SHADER_READ;
 *   [.phase2]               DE__VIS_MESH_GROUPS_MAX meshlets: one   split args: SHADER_WRITE
 *                           mesh dispatch per 2^22 meshlets
 *   vis.raster.mesh         one mesh-shader group per visible       scene constants, instances,
 *   [.phase2]               meshlet of the phase                    meshes, materials, visible
 *                           (de_gpu_cmd_dispatch_mesh_indirect on   list: SHADER_READ; raster
 *                           the phase's DeVisRasterArgs)            args: INDIRECT + READ;
 *                                                                   vis_ids: RENDER_TARGET;
 *                                                                   depth: DEPTH read + write
 *
 * Targets: the vis buffer (de__vis_context.vis_ids, R32_UINT, cleared to DE__VIS_ID_EMPTY) is a
 * transient of the view's graph; the depth is DENG-233's (cleared to 0 by "vis.depth_clear",
 * reverse-Z, GREATER_EQUAL). Phase 1 draws, the HZB is built from the depth, phase 2 draws on
 * top: its entries follow phase 1's in the visible list, so every vis ID of the frame names one
 * entry.
 *
 * Pipeline (made once, at renderer create): DE_PIPE_MESH without an amplification stage, cull mode
 * NONE (the mesh shader culls back faces of single-sided materials and zero-area triangles,
 * r.raster_tri_cull), depth test and write. 64 threads per group: every backend runs 64-thread
 * groups (Apple and AMD waves of 32/64, NVIDIA 32), and a meshlet has at most 64 vertices; no
 * per-backend specialization is needed.
 */
#include "render/passes/vis/vis_stages.h"

#include "render/formats/de_cooked_mesh.h"
#include "render/graph/rg_internal.h"

#include <stdio.h>
#include <string.h>

/** Threads per mesh-shader group (vis_mesh.slang ms_vis). */
#define RM_GROUP 64u
/** RasterMeshParams.flags */
#define RM_TRI_CULL 1u

/** The push constants of every vis_mesh entry (vis_mesh.slang RasterMeshParams, 32 bytes). */
typedef struct rm_params {
    u32 scene, view_slot, visible, args, base, flags, split, split_count;
} rm_params;
DE_STATIC_ASSERT(sizeof(rm_params) == 32, "RasterMeshParams is 32 bytes");

typedef struct rm_state {
    de_gpu_pipeline mesh;  /**< ms_vis + ps_vis (invalid without mesh shaders) */
    de_gpu_pipeline clear; /**< vs_vis_clear + ps_vis_clear */
    de__vis_kernel split;  /**< cs_raster_mesh_split */
    u32 mesh_push;
    de_cvar cv_path, cv_tri_cull;
} rm_state;

static const de__vis_layout_const k_consts[] = {
    {"DE_VIS_DECODE_RECORD_BYTES", DE_COOKED_MESHLET_SIZE},
    {"DE_VIS_MESHLET_MAX_VERTICES", DE_COOKED_MESHLET_MAX_VERTICES},
    {"DE_VIS_MESHLET_MAX_TRIANGLES", DE_COOKED_MESHLET_MAX_TRIANGLES},
    {"DE_VIS_STREAM_POSITION", DE_COOKED_STREAM_POSITION},
    {"DE_VIS_VF_F32X3", DE_COOKED_VF_F32X3},
    {"DE_VIS_VF_UNORM16X4", DE_COOKED_VF_UNORM16X4},
    /* DENG-236: UV0 (vis_decode.slang) */
    {"DE_VIS_STREAM_UV0", DE_COOKED_STREAM_UV0},
    {"DE_VIS_VF_F32X2", DE_COOKED_VF_F32X2},
    {"DE_VIS_VF_UNORM16X2", DE_COOKED_VF_UNORM16X2},
    /* DENG-238: the other vertex streams (resolve) */
    {"DE_VIS_STREAM_NORMAL", DE_COOKED_STREAM_NORMAL},
    {"DE_VIS_STREAM_TANGENT", DE_COOKED_STREAM_TANGENT},
    {"DE_VIS_STREAM_UV1", DE_COOKED_STREAM_UV1},
    {"DE_VIS_STREAM_COLOR0", DE_COOKED_STREAM_COLOR0},
    {"DE_VIS_VF_OCT16", DE_COOKED_VF_OCT16},
    {"DE_VIS_VF_OCT16_SIGN", DE_COOKED_VF_OCT16_SIGN},
    {"DE_VIS_VF_UNORM8X4", DE_COOKED_VF_UNORM8X4},
    {"DE_VIS_MESH_GROUPS_MAX", DE__VIS_MESH_GROUPS_MAX},
};

const de__vis_layout_const *de__vis_raster_layout_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

u32 de__vis_mesh_dispatch_count(u32 capacity) {
    const u32 n = (capacity + DE__VIS_MESH_GROUPS_MAX - 1u) / DE__VIS_MESH_GROUPS_MAX;
    return n > 0u ? n : 1u;
}

void de__vis_mesh_dispatch_groups(u32 count, u32 k, u32 *x, u32 *y) {
    const u64 start = (u64)k * DE__VIS_MESH_GROUPS_MAX;
    u32 n = 0;
    if (count > start) {
        const u64 left = count - start;
        n = left < DE__VIS_MESH_GROUPS_MAX ? (u32)left : DE__VIS_MESH_GROUPS_MAX;
    }
    const de__vis_dispatch_args a = de__vis_dispatch_args_for(n);
    *x = a.x;
    *y = a.y;
}

/* =================================================================================================
 * Init / shutdown
 * ============================================================================================== */

static rm_state *state_of(const de__vis_shared *s) {
    return s != NULL ? (rm_state *)s->stage[DE__VIS_STAGE_RASTER] : NULL;
}

static de_result make_pipeline(de__vis_shared *s, de_gpu_pipeline_desc *d, de_gpu_pipeline *out) {
    *out = de_gpu_pipeline_create(s->dev, d);
    if (DE_HANDLE_VALID(*out))
        return DE_OK;
    const de_error *e = de_error_last();
    return DE_FAIL_WRAP(e != NULL && e->result != DE_OK ? e->result : DE_ERR_INTERNAL,
                        "vis: the pipeline %s", d->name);
}

static void destroy_state(de__vis_shared *s, rm_state *st) {
    if (DE_HANDLE_VALID(st->mesh))
        de_gpu_pipeline_destroy(s->dev, st->mesh);
    if (DE_HANDLE_VALID(st->clear))
        de_gpu_pipeline_destroy(s->dev, st->clear);
    de__vis_kernel_destroy(s, &st->split);
    DE_FREE(s->alloc, st, sizeof *st);
}

static u32 push_size(const de__engine_shader_entry *e) {
    const u32 b = e->push_constant_bytes > sizeof(rm_params) ? e->push_constant_bytes
                                                             : (u32)sizeof(rm_params);
    return (b + 3u) & ~3u;
}

de_result de__vis_raster_mesh_init(de__vis_shared *s) {
    rm_state *st = DE_NEW(s->alloc, rm_state, DE_MEM_RENDER);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: raster state");
    memset(st, 0, sizeof *st);
    static const char *const k_paths[3] = {"auto", "mesh", "compute"};
    st->cv_path = de_cvar_register(&(de_cvar_desc){
        .name = "r.raster_path",
        .help = "Vis-buffer raster: auto (mesh shaders when the GPU has them, else compute "
                "expansion), mesh or compute",
        .type = DE_CVAR_ENUM,
        .enum_values = k_paths,
        .enum_count = 3,
        .default_number = 0});
    st->cv_tri_cull = de_cvar_register(&(de_cvar_desc){
        .name = "r.raster_tri_cull",
        .help = "Vis-buffer raster: cull back faces of single-sided materials and zero-area "
                "triangles in the mesh shader",
        .type = DE_CVAR_BOOL,
        .flags = DE_CVAR_DEV_ONLY,
        .default_number = 1});
    de_error_clear(); /* a DEV_ONLY cvar in Shipping is no error */

    static const char *const k_entries[5] = {"ms_vis", "ps_vis", "vs_vis_clear", "ps_vis_clear",
                                             "cs_raster_mesh_split"};
    de__engine_shader_code code;
    de_result r = de__vis_shader_load(s, "vis_mesh", k_entries, 5, &code);
    if (r != DE_OK) {
        DE_FREE(s->alloc, st, sizeof *st);
        return r;
    }
    de_gpu_pipeline_desc cd = {.kind = DE_PIPE_GRAPHICS,
                               .vs = code.entries[2].blob,
                               .ps = code.entries[3].blob,
                               .color_formats = {DE_FMT_R32_UINT},
                               .color_count = 1,
                               .cull = DE_CULL_NONE,
                               .name = "vis ids clear"};
    r = make_pipeline(s, &cd, &st->clear);
    if (r == DE_OK) {
        static const u32 one[3] = {1u, 1u, 1u};
        r = de__vis_kernel_create(s, &code, 4, "vis raster mesh split", one, (u32)sizeof(rm_params),
                                  &st->split);
    }
    if (r == DE_OK && de_gpu_get_caps(s->dev)->mesh_shaders) {
        const de__engine_shader_entry *ms = &code.entries[0];
        st->mesh_push = push_size(ms);
        de_gpu_pipeline_desc md = {.kind = DE_PIPE_MESH,
                                   .ms = ms->blob,
                                   .ps = s->depth_only ? (de_gpu_shader_blob){0}
                                                       : code.entries[1].blob,
                                   .color_formats = {DE_FMT_R32_UINT},
                                   .color_count = s->depth_only ? 0u : 1u,
                                   .depth_format = DE_FMT_D32_FLOAT,
                                   .cull = DE_CULL_NONE,
                                   .depth_test = true,
                                   .depth_write = true,
                                   .push_constant_bytes = st->mesh_push,
                                   .threadgroup_size = {RM_GROUP, 1u, 1u},
                                   .name = "vis raster mesh"};
        r = make_pipeline(s, &md, &st->mesh);
    }
    de__engine_shader_code_free(&code);
    if (r != DE_OK) {
        destroy_state(s, st);
        return r;
    }
    s->stage[DE__VIS_STAGE_RASTER] = st;
    return DE_OK;
}

void de__vis_raster_mesh_shutdown(de__vis_shared *s) {
    rm_state *st = state_of(s);
    if (st == NULL)
        return;
    destroy_state(s, st);
    s->stage[DE__VIS_STAGE_RASTER] = NULL;
}

bool de__vis_raster_mesh_ready(const de__vis_shared *s) {
    const rm_state *st = state_of(s);
    return st != NULL && DE_HANDLE_VALID(st->mesh);
}

de__vis_raster_path de__vis_raster_choose(const de__vis_shared *s) {
    const rm_state *st = state_of(s);
    if (st == NULL)
        return DE__VIS_RASTER_NONE;
    const i64 want = DE_HANDLE_VALID(st->cv_path) ? de_cvar_int(st->cv_path) : 0;
    if (want != 2 && de__vis_raster_mesh_ready(s))
        return DE__VIS_RASTER_MESH;
    return DE__VIS_RASTER_COMPUTE;
}

/* =================================================================================================
 * Targets
 * ============================================================================================== */

typedef struct clear_pass {
    de_gpu_pipeline pipeline;
    de_rg_resource target;
} clear_pass;

static void clear_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const clear_pass *p = (const clear_pass *)user;
    const de_gpu_attachment a = {.texture = de_rg_texture(pass, p->target), .store = true};
    de_gpu_cmd_begin_render(cmd, &a, 1, NULL);
    de_gpu_cmd_set_pipeline(cmd, p->pipeline);
    de_gpu_cmd_draw(cmd, 3, 1, 0, 0);
    de_gpu_cmd_end_render(cmd);
}

de_rg_resource de__vis_raster_targets(de_rg *g, de__vis_context *vc) {
    if (vc->depth_only)
        return vc->depth; /* Target validity only; no ID image is created. */
    if (DE_HANDLE_VALID(vc->vis_ids))
        return vc->vis_ids;
    rm_state *st = state_of(vc->shared);
    if (st == NULL || !vc->depth_target || !DE_HANDLE_VALID(vc->depth) || vc->width == 0 ||
        vc->height == 0)
        return (de_rg_resource){0};
    clear_pass *p = (clear_pass *)de_arena_push_zero(vc->arena, sizeof *p, 16);
    if (p == NULL) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: raster: the frame arena is full");
        return (de_rg_resource){0};
    }
    /* No SAMPLED usage: a uint texture must not take a sampled bindless index (de_tex2d reads
     * float4, and GPU-assisted validation checks every index a shader ever read against the
     * format there now). A reader adds the view it needs (DENG-237, DENG-238). */
    const de_rg_resource t = de_rg_create_texture(
        g, &(de_gpu_texture_desc){.format = DE_FMT_R32_UINT,
                                  .width = vc->width,
                                  .height = vc->height,
                                  .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_COPY_SRC,
                                  .name = "vis.vis_ids"});
    if (!DE_HANDLE_VALID(t))
        return t; /* error set; finalize reports it */
    p->pipeline = st->clear;
    p->target = t;
    de_rg_pass *rp = de_rg_add_pass(g, "vis.vis_ids_clear", DE_QUEUE_GRAPHICS, clear_execute, p);
    de_rg_write(rp, t, DE_ACCESS_RENDER_TARGET);
    vc->vis_ids = t;
    return t;
}

/* =================================================================================================
 * One phase
 * ============================================================================================== */

typedef struct mesh_pass {
    de_gpu_device *dev;
    de_gpu_pipeline pipeline;
    rm_params pc;
    u32 push_bytes;
    de_rg_resource vis_ids, depth, args, split_args, visible;
    bool depth_only;
    u32 layer, rect[4];
    u32 dispatches; /**< > 1: split_args holds one DeVisMeshArgs per dispatch */
} mesh_pass;

typedef struct split_pass {
    de_gpu_device *dev;
    const de__vis_kernel *k;
    rm_params pc;
    de_rg_resource args, split_args;
} split_pass;

static void split_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const split_pass *p = (const split_pass *)user;
    rm_params pc = p->pc;
    pc.args = de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, p->args));
    pc.split = de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, p->split_args));
    de_gpu_cmd_set_pipeline(cmd, p->k->pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, p->k->push_bytes);
    de_gpu_cmd_dispatch(cmd, 1, 1, 1);
}

static void mesh_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const mesh_pass *p = (const mesh_pass *)user;
    const de_gpu_attachment color = {.texture = p->depth_only ? (de_gpu_texture){0}
                                                              : de_rg_texture(pass, p->vis_ids),
                                     .store = true};
    const de_gpu_attachment depth = {
        .texture = de_rg_texture(pass, p->depth), .layer = p->layer, .store = true};
    const de_gpu_buffer args = de_rg_buffer(pass, p->args);
    rm_params pc = p->pc;
    pc.args = de_gpu_buffer_bindless(p->dev, args);
    pc.visible = de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, p->visible));
    de_gpu_cmd_begin_render(cmd, p->depth_only ? NULL : &color, p->depth_only ? 0u : 1u, &depth);
    if (p->rect[2]) {
        de_gpu_cmd_viewport(cmd, (f32)p->rect[0], (f32)p->rect[1], (f32)p->rect[2],
                            (f32)p->rect[3]);
        de_gpu_cmd_scissor(cmd, (i32)p->rect[0], (i32)p->rect[1], p->rect[2], p->rect[3]);
    }
    de_gpu_cmd_set_pipeline(cmd, p->pipeline);
    if (p->dispatches <= 1u) {
        pc.base = 0;
        de_gpu_cmd_push_constants(cmd, &pc, p->push_bytes);
        de_gpu_cmd_dispatch_mesh_indirect(cmd, args, DE__VIS_RASTER_ARGS_MESH, (de_gpu_buffer){0},
                                          0, 1);
    } else {
        const de_gpu_buffer split = de_rg_buffer(pass, p->split_args);
        for (u32 k = 0; k < p->dispatches; ++k) {
            pc.base = k * DE__VIS_MESH_GROUPS_MAX;
            de_gpu_cmd_push_constants(cmd, &pc, p->push_bytes);
            de_gpu_cmd_dispatch_mesh_indirect(cmd, split, (u64)k * 12u, (de_gpu_buffer){0}, 0, 1);
        }
    }
    de_gpu_cmd_end_render(cmd);
}

void de__vis_raster_mesh(de_rg *g, de__vis_context *vc, de__vis_phase phase) {
    rm_state *st = state_of(vc->shared);
    const u32 pi = DE__VIS_PHASE_INDEX(phase);
    if (st == NULL || !DE_HANDLE_VALID(st->mesh) || !DE_HANDLE_VALID(vc->raster_args[pi]) ||
        !DE_HANDLE_VALID(vc->visible_meshlets) || !DE_HANDLE_VALID(vc->scene_instances) ||
        !DE_HANDLE_VALID(vc->scene_meshes) || vc->scene_views_bindless == 0)
        return;
    const de_rg_resource ids = de__vis_raster_targets(g, vc);
    if (!DE_HANDLE_VALID(ids))
        return;
    const bool p1 = phase == DE__VIS_PHASE_1;
    const u32 dispatches = de__vis_mesh_dispatch_count(vc->visible_capacity);
    mesh_pass *mp = (mesh_pass *)de_arena_push_zero(vc->arena, sizeof *mp, 16);
    split_pass *sp =
        dispatches > 1u ? (split_pass *)de_arena_push_zero(vc->arena, sizeof *sp, 16) : NULL;
    if (mp == NULL || (dispatches > 1u && sp == NULL)) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: raster: the frame arena is full");
        return;
    }
    const de_cvar tc = st->cv_tri_cull;
    const rm_params pc = {.scene = vc->scene_views_bindless,
                          .view_slot = vc->scene_view_slot,
                          .visible = 0, /* the transients' indices: at execute */
                          .flags = !DE_HANDLE_VALID(tc) || de_cvar_bool(tc) ? RM_TRI_CULL : 0u,
                          .split_count = dispatches};
    mp->dev = vc->shared->dev;
    mp->pipeline = st->mesh;
    mp->visible = vc->visible_meshlets;
    mp->pc = pc;
    mp->push_bytes = st->mesh_push;
    mp->depth_only = vc->depth_only;
    mp->layer = vc->depth_layer;
    memcpy(mp->rect, vc->raster_rect, sizeof mp->rect);
    mp->vis_ids = vc->vis_ids;
    mp->depth = vc->depth;
    mp->args = vc->raster_args[pi];
    mp->dispatches = dispatches;

    if (dispatches > 1u) {
        mp->split_args = de_rg_create_buffer(
            g,
            &(de_gpu_buffer_desc){.size = (u64)dispatches * 12u,
                                  .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT,
                                  .stride = 12u,
                                  .name = p1 ? "vis.raster_mesh_args" : "vis.raster_mesh_args2"});
        if (!DE_HANDLE_VALID(mp->split_args))
            return; /* error set; finalize reports it */
        sp->dev = vc->shared->dev;
        sp->k = &st->split;
        sp->pc = pc;
        sp->args = vc->raster_args[pi];
        sp->split_args = mp->split_args;
        de_rg_pass *p =
            de_rg_add_pass(g, p1 ? "vis.raster.mesh_split" : "vis.raster.mesh_split.phase2",
                           DE_QUEUE_GRAPHICS, split_execute, sp);
        de_rg_read(p, vc->raster_args[pi], DE_ACCESS_SHADER_READ);
        de_rg_write(p, mp->split_args, DE_ACCESS_SHADER_WRITE);
    }

    de_rg_pass *p = de_rg_add_pass(g, p1 ? "vis.raster.mesh" : "vis.raster.mesh.phase2",
                                   DE_QUEUE_GRAPHICS, mesh_execute, mp);
    if (DE_HANDLE_VALID(vc->scene_views))
        de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_instances, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_meshes, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(vc->scene_materials))
        de_rg_read(p, vc->scene_materials, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->visible_meshlets, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->raster_args[pi], DE_ACCESS_INDIRECT | DE_ACCESS_SHADER_READ);
    if (dispatches > 1u)
        de_rg_read(p, mp->split_args, DE_ACCESS_INDIRECT);
    if (!vc->depth_only) {
        de_rg_read(p, ids, DE_ACCESS_RENDER_TARGET);
        de_rg_write(p, ids, DE_ACCESS_RENDER_TARGET);
    }
    de_rg_read(p, vc->depth, DE_ACCESS_DEPTH_READ);
    de_rg_write(p, vc->depth, DE_ACCESS_DEPTH_READ | DE_ACCESS_DEPTH_WRITE);
    vc->raster_recorded = true;
}
