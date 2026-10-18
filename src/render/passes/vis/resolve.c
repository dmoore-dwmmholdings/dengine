/*
 * resolve.c: the material resolve, the last stage of the visibility-buffer frame (SPEC §12.5 step
 * 4, §11.8; DENG-238, DENG-298). shaders/visbuffer/resolve_standard.slang (engine shader
 * `vis_resolve`) has the kernels, resolve_model.slang the kernel body generic over the material
 * model (materials/material_interface.slang), resolve_common.slang the reconstruction,
 * gbuffer.slang / gbuffer.h the G-buffer contract, feedback.slang the texture-streaming feedback.
 *
 * Per view and frame, on the graphics queue, when classification ran (DENG-237):
 *
 *   vis.resolve.clear_lighting  DENG-305: the lighting target to 0 as a  lighting: RENDER_TARGET
 *                          render target (the forward pass blends into it; D3D12 needs a render
 *                          target's first use in a frame to be a clear or discard, not a UAV write)
 *   vis.resolve.clear      the other G-buffer targets to 0            targets: SHADER_WRITE
 *   vis.resolve.feedback   feedback to "none", tile counters to 0     feedback, counters: WRITE
 *   vis.resolve            one indirect dispatch per material model   ID buffer, tile lists,
 *                          on its tile list (classify_args, byte      tables, visible list: READ;
 *                          16 m): a registered game model         args: INDIRECT + READ;
 *                          (DENG-298) through its own kernel,         targets, feedback, counters:
 *                          Unlit (model 1) through cs_resolve_unlit,  READ + WRITE
 *                          every other model through the Standard
 *                          one
 *   vis.resolve.readback   the counters and the feedback into the     COPY_SRC; readback ring:
 *                          view's readback ring (side effect; only    COPY_DST
 *                          with r.texture_feedback 1)
 *
 * Nothing reads the G-buffer before lighting (S14), so without r.texture_feedback the graph culls
 * the resolve (and classification) in the renderer.
 *
 * The targets (de__vis_context.gbuffer, gbuffer.h) are transients of the view's graph at the
 * internal size, written through storage views (GB0 holds sRGB-encoded values in RGBA8_UNORM:
 * storage images cannot be sRGB). Material textures are sampled through the bindless table with
 * the stage's sampler (linear, wrap; DeGpuMaterial has no sampler before S14). The feedback buffer
 * holds a u32 per sampled-texture index (up to DE__VIS_FEEDBACK_MAX); each frame's copy goes into
 * a per-view READBACK ring of DE__VIS_FRAME_RING entries made when the view first resolves, and
 * de__vis_view_resolve_stats returns the newest completed one. A steady frame allocates nothing.
 * The render thread reads only GPU tables, never asset records.
 */
#include "render/passes/vis/vis_stages.h"

#include "render/materials/material_models.h"
#include "render/materials/material_params.h"
#include "render/passes/vis/gbuffer.h"

#include <stdio.h>
#include <string.h>

DE_STATIC_ASSERT(DE__GB_COUNT == 5, "de__vis_context.gbuffer holds the gbuffer.h targets");

/** The kernels' push constants (resolve_model.slang ResolveParams, 112 bytes). */
typedef struct rs_params {
    u32 ids, pitch, width, height, tile, tiles_x, tile_count, lists, args, model, scene, view_slot;
    u32 visible, visible_count, sampler, frame;
    u32 targets[DE__GB_COUNT];
    u32 feedback, feedback_capacity, counters, debug_pos, debug_uv, flags, reserved;
} rs_params;
DE_STATIC_ASSERT(sizeof(rs_params) == 112, "ResolveParams is 112 bytes");
DE_STATIC_ASSERT(sizeof(rs_params) == DE__MATERIAL_MODEL_RESOLVE_PUSH,
                 "a game model's cs_resolve_model takes ResolveParams");

/** ResolveParams.flags */
#define RS_DEBUG 1u
/** Bytes of the counters at the start of each readback entry. */
#define RS_COUNTER_BYTES (DE__GS_MATERIAL_MODELS * 4u)

typedef struct rs_view {
    de_view handle;
    de_gpu_buffer rb; /**< READBACK ring: DE__VIS_FRAME_RING x (counters, feedback) */
    const u8 *rb_map;
    u32 rb_access;
    u64 slot_frame[DE__VIS_FRAME_RING];
    bool slot_live[DE__VIS_FRAME_RING];
    de__vis_resolve_stats stats;
} rs_view;

typedef struct rs_state {
    de__vis_kernel clear, feedback_clear, unlit;
    /* DENG-302: the Standard resolve permutations (de__material_permutations), smallest first;
     * the last one has every feature. */
    de__vis_kernel standard[DE__MATERIAL_PERMUTATIONS_MAX];
    u32 standard_count;
    de_gpu_sampler sampler;
    u32 sampler_bindless;
    u32 feedback_capacity;
    u64 entry_bytes; /**< one readback entry */
    rs_view views[DE__VIEW_MAX];
    de__vis_resolve_debug debug;
    de_cvar cv_feedback; /**< r.texture_feedback */
} rs_state;

static const de__vis_layout_const k_consts[] = {
    {"DE_RESOLVE_DEBUG", RS_DEBUG},
    {"DE_RESOLVE_GROUP", 64u},
    {"DE_RESOLVE_TEX_BASE_COLOR", 0u},
    {"DE_RESOLVE_TEX_METAL_ROUGH", 1u},
    {"DE_RESOLVE_TEX_NORMAL", 2u},
    {"DE_RESOLVE_TEX_OCCLUSION", 3u},
    {"DE_RESOLVE_TEX_EMISSIVE", 4u},
    {"DE_GB_FLAG_DOUBLE_SIDED", DE__GB_FLAG_DOUBLE_SIDED},
    {"DE_GB_FLAG_TRANSMISSION", DE__GB_FLAG_TRANSMISSION},
    {"DE_GB_FLAG_SUBSURFACE", DE__GB_FLAG_SUBSURFACE},
    {"DE_GB_FLAG_CLEARCOAT", DE__GB_FLAG_CLEARCOAT},
    {"DE_GB_FLAG_UNLIT", DE__GB_FLAG_UNLIT},
    {"DE_GB_NORMAL_BITS", DE__GB_NORMAL_BITS},
    {"DE_FEEDBACK_NONE", DE__VIS_FEEDBACK_NONE},
};

const de__vis_layout_const *de__vis_resolve_layout_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

/* =================================================================================================
 * Per renderer
 * ============================================================================================== */

static void view_release(de__vis_shared *s, rs_view *pv) {
    if (DE_HANDLE_VALID(pv->rb))
        de_gpu_buffer_destroy(s->dev, pv->rb); /* deferred until the GPU is done */
    memset(pv, 0, sizeof *pv);
}

static void state_free(de__vis_shared *s, rs_state *st) {
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        view_release(s, &st->views[i]);
    de__vis_kernel_destroy(s, &st->clear);
    de__vis_kernel_destroy(s, &st->feedback_clear);
    for (u32 i = 0; i < DE__MATERIAL_PERMUTATIONS_MAX; ++i)
        de__vis_kernel_destroy(s, &st->standard[i]);
    de__vis_kernel_destroy(s, &st->unlit);
    if (DE_HANDLE_VALID(st->sampler))
        de_gpu_sampler_destroy(s->dev, st->sampler);
    DE_FREE(s->alloc, st, sizeof *st);
}

de_result de__vis_resolve_init(de__vis_shared *s) {
    rs_state *st = DE_NEW(s->alloc, rs_state, DE_MEM_RENDER);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: resolve state");
    memset(st, 0, sizeof *st);
    st->cv_feedback = de_cvar_register(&(de_cvar_desc){
        .name = "r.texture_feedback",
        .help = "Material resolve: read the texture-streaming feedback (and the per-model tile "
                "counts) back every frame (on for the residency manager, S20)",
        .type = DE_CVAR_BOOL,
        .default_number = 0});
    de_error_clear();
    const u32 max_tex = de_gpu_get_caps(s->dev)->max_bindless_textures;
    st->feedback_capacity =
        max_tex > 0 && max_tex < DE__VIS_FEEDBACK_MAX ? max_tex : DE__VIS_FEEDBACK_MAX;
    st->entry_bytes = (RS_COUNTER_BYTES + (u64)st->feedback_capacity * 4u + 255u) & ~(u64)255u;
    st->sampler = de_gpu_sampler_create(s->dev, &(de_gpu_sampler_desc){.min = DE_FILTER_LINEAR,
                                                                       .mag = DE_FILTER_LINEAR,
                                                                       .mip = DE_FILTER_LINEAR,
                                                                       .max_aniso = 1.0f});
    de_result r = DE_HANDLE_VALID(st->sampler)
                      ? DE_OK
                      : DE_FAIL_WRAP(DE_ERR_INTERNAL, "vis: resolve sampler");
    if (r == DE_OK) {
        st->sampler_bindless = de_gpu_sampler_bindless(s->dev, st->sampler);
        /* DENG-302: every Standard permutation is made now (pre-warmed), so a material's
         * features crossing to another permutation never creates a pipeline. */
        u32 nperm = 0;
        const de__material_permutation *perm =
            de__material_permutations(DE__GS_MATERIAL_MODEL_STANDARD, &nperm);
        const char *entries[3u + DE__MATERIAL_PERMUTATIONS_MAX] = {
            "cs_resolve_clear", "cs_feedback_clear", "cs_resolve_unlit"};
        for (u32 i = 0; i < nperm; ++i)
            entries[3u + i] = perm[i].entry;
        de__engine_shader_code code;
        r = de__vis_shader_load(s, "vis_resolve", entries, 3u + nperm, &code);
        if (r == DE_OK) {
            static const u32 g8[3] = {8u, 8u, 1u}, g64[3] = {64u, 1u, 1u};
            const u32 pb = (u32)sizeof(rs_params);
            r = de__vis_kernel_create(s, &code, 0, "vis resolve clear", g8, pb, &st->clear);
            if (r == DE_OK)
                r = de__vis_kernel_create(s, &code, 1, "vis feedback clear", g64, pb,
                                          &st->feedback_clear);
            if (r == DE_OK)
                r = de__vis_kernel_create(s, &code, 2, "vis resolve unlit", g64, pb, &st->unlit);
            for (u32 i = 0; i < nperm && r == DE_OK; ++i) {
                char name[64];
                snprintf(name, sizeof name, "vis resolve standard %u", i);
                r = de__vis_kernel_create(s, &code, 3u + i, name, g64, pb, &st->standard[i]);
            }
            st->standard_count = r == DE_OK ? nperm : 0u;
            de__engine_shader_code_free(&code);
        }
    }
    if (r != DE_OK) {
        state_free(s, st);
        return r;
    }
    s->stage[DE__VIS_STAGE_RESOLVE] = st;
    return DE_OK;
}

void de__vis_resolve_shutdown(de__vis_shared *s) {
    rs_state *st = (rs_state *)s->stage[DE__VIS_STAGE_RESOLVE];
    if (st == NULL)
        return;
    state_free(s, st);
    s->stage[DE__VIS_STAGE_RESOLVE] = NULL;
}

de__vis_resolve_stats de__vis_view_resolve_stats(const de__vis_shared *s, u32 slot) {
    const rs_state *st = s != NULL ? (const rs_state *)s->stage[DE__VIS_STAGE_RESOLVE] : NULL;
    if (st == NULL || slot >= DE__VIEW_MAX)
        return (de__vis_resolve_stats){0};
    return st->views[slot].stats;
}

void de__vis_resolve_set_debug(de__vis_shared *s, const de__vis_resolve_debug *d) {
    rs_state *st = s != NULL ? (rs_state *)s->stage[DE__VIS_STAGE_RESOLVE] : NULL;
    if (st != NULL)
        st->debug = d != NULL ? *d : (de__vis_resolve_debug){0};
}

/* =================================================================================================
 * One view
 * ============================================================================================== */

static bool view_ensure(de__vis_shared *s, rs_state *st, rs_view *pv, u32 slot) {
    if (DE_HANDLE_VALID(pv->rb))
        return true;
    char name[48];
    snprintf(name, sizeof name, "vis resolve readback %u", slot);
    pv->rb = de_gpu_buffer_create(
        s->dev, &(de_gpu_buffer_desc){.size = (u64)DE__VIS_FRAME_RING * st->entry_bytes,
                                      .usage = DE_USAGE_COPY_DST,
                                      .memory = DE_MEM_READBACK,
                                      .name = name});
    pv->rb_map = DE_HANDLE_VALID(pv->rb) ? (const u8 *)de_gpu_buffer_map(s->dev, pv->rb) : NULL;
    if (pv->rb_map == NULL) {
        const de_error *e = de_error_last();
        DE_ERROR(s->log, "vis: view slot %u: resolve readback buffer: %s", slot,
                 e != NULL ? e->message : "?");
        const de_view h = pv->handle;
        view_release(s, pv);
        pv->handle = h;
        return false;
    }
    pv->rb_access = DE_ACCESS_NONE;
    return true;
}

/* Takes the newest completed frame's counters and feedback (frames before `completed` are done). */
static void read_stats(const rs_state *st, rs_view *pv, u64 completed) {
    i32 best = -1;
    for (u32 k = 0; k < DE__VIS_FRAME_RING; ++k) {
        if (!pv->slot_live[k] || pv->slot_frame[k] >= completed)
            continue;
        if (pv->stats.valid && pv->slot_frame[k] <= pv->stats.frame)
            continue;
        if (best < 0 || pv->slot_frame[k] > pv->slot_frame[best])
            best = (i32)k;
    }
    if (best < 0)
        return;
    const u8 *e = pv->rb_map + (size_t)best * st->entry_bytes;
    pv->stats.valid = true;
    pv->stats.frame = pv->slot_frame[best];
    memcpy(pv->stats.tiles, e, RS_COUNTER_BYTES);
    pv->stats.feedback = (const u32 *)(const void *)(e + RS_COUNTER_BYTES);
    pv->stats.feedback_count = st->feedback_capacity;
}

typedef struct rs_pass {
    de_gpu_device *dev;
    rs_state *st;
    const de__material_models
        *models;                     /**< game models' resolve pipelines (DENG-298); may be NULL */
    u8 perm[DE__GS_MATERIAL_MODELS]; /**< DENG-302: each built-in model's Standard permutation */
    rs_params pc;
    u32 groups_x, groups_y;
    de_rg_resource ids, lists, args, scene, visible, feedback, counters, debug_pos, debug_uv;
    de_rg_resource targets[DE__GB_COUNT];
    de_gpu_buffer rb;
    u64 rb_offset;
} rs_pass;

static u32 buf_index(const rs_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return DE_HANDLE_VALID(r) ? de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, r)) : 0u;
}
static u32 tex_index(const rs_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return DE_HANDLE_VALID(r) ? de_gpu_texture_bindless(p->dev, de_rg_texture(pass, r), 0, true)
                              : 0u;
}

static rs_params params(const rs_pass *p, const de_rg_pass *pass) {
    rs_params pc = p->pc;
    pc.ids = buf_index(p, pass, p->ids);
    pc.lists = buf_index(p, pass, p->lists);
    pc.args = buf_index(p, pass, p->args);
    pc.scene = buf_index(p, pass, p->scene);
    pc.visible = buf_index(p, pass, p->visible);
    pc.feedback = buf_index(p, pass, p->feedback);
    pc.counters = buf_index(p, pass, p->counters);
    for (u32 t = 0; t < DE__GB_COUNT; ++t)
        pc.targets[t] = tex_index(p, pass, p->targets[t]);
    pc.debug_pos = tex_index(p, pass, p->debug_pos);
    pc.debug_uv = tex_index(p, pass, p->debug_uv);
    return pc;
}

static void clear_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rs_pass *p = (const rs_pass *)user;
    const rs_params pc = params(p, pass);
    de_gpu_cmd_set_pipeline(cmd, p->st->clear.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.width + 7u) / 8u, (pc.height + 7u) / 8u, 1);
}

/* DENG-305: the lighting target is also a render target (the forward pass blends into it), so on
 * D3D12 its first use in a frame must be a render-target clear or discard, not a UAV write. */
static void clear_lighting_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rs_pass *p = (const rs_pass *)user;
    const de_gpu_attachment color = {
        .texture = de_rg_texture(pass, p->targets[DE__GB_LIGHTING]), .clear = true, .store = true};
    de_gpu_cmd_begin_render(cmd, &color, 1, NULL);
    de_gpu_cmd_end_render(cmd);
}

static void feedback_clear_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rs_pass *p = (const rs_pass *)user;
    const rs_params pc = params(p, pass);
    de_gpu_cmd_set_pipeline(cmd, p->st->feedback_clear.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, (pc.feedback_capacity + 63u) / 64u, 1, 1);
}

static void resolve_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rs_pass *p = (const rs_pass *)user;
    rs_params pc = params(p, pass);
    const de_gpu_buffer args = de_rg_buffer(pass, p->args);
    for (u32 m = 0; m < DE__GS_MATERIAL_MODELS; ++m) {
        /* DENG-298: a game model's own kernel (de_material_model_register), Unlit's, else the
         * Standard one (Standard and the built-ins not implemented yet). */
        de_gpu_pipeline pipe = de__material_models_resolve_pipeline(p->models, m);
        if (!DE_HANDLE_VALID(pipe)) {
            /* DENG-302: the smallest Standard permutation covering this model's materials. */
            const u32 k =
                p->perm[m] < p->st->standard_count ? p->perm[m] : p->st->standard_count - 1u;
            pipe = m == DE__GS_MATERIAL_MODEL_UNLIT ? p->st->unlit.pipeline
                                                    : p->st->standard[k].pipeline;
        }
        pc.model = m;
        de_gpu_cmd_set_pipeline(cmd, pipe);
        de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
        de_gpu_cmd_dispatch_indirect(cmd, args, (u64)m * 16u);
    }
}

static void readback_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rs_pass *p = (const rs_pass *)user;
    de_gpu_cmd_copy_buffer(cmd, p->rb, p->rb_offset, de_rg_buffer(pass, p->counters), 0,
                           RS_COUNTER_BYTES);
    de_gpu_cmd_copy_buffer(cmd, p->rb, p->rb_offset + RS_COUNTER_BYTES,
                           de_rg_buffer(pass, p->feedback), 0, (u64)p->pc.feedback_capacity * 4u);
}

static rs_pass *pass_data(de__vis_context *vc, const rs_pass *base) {
    rs_pass *p = (rs_pass *)de_arena_push(vc->arena, sizeof *p, 16);
    if (p != NULL)
        *p = *base;
    else
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: resolve: the frame arena is full");
    return p;
}

void de__vis_resolve(de_rg *g, de__vis_context *vc) {
    de__vis_shared *s = vc->shared;
    rs_state *st = (rs_state *)s->stage[DE__VIS_STAGE_RESOLVE];
    if (st == NULL || vc->view_slot >= DE__VIEW_MAX)
        return;
    rs_view *pv = &st->views[vc->view_slot];
    if (vc->view_new || pv->handle.id != vc->view.id) {
        view_release(s, pv);
        pv->handle = vc->view;
    }
    if (DE_HANDLE_VALID(pv->rb))
        read_stats(st, pv, vc->frames_completed);
    if (!DE_HANDLE_VALID(vc->classify_tiles) || !DE_HANDLE_VALID(vc->vis_ids_buffer) ||
        !DE_HANDLE_VALID(vc->scene_views) || !DE_HANDLE_VALID(vc->scene_instances) ||
        !DE_HANDLE_VALID(vc->scene_meshes) || vc->width == 0 || vc->height == 0 ||
        !view_ensure(s, st, pv, vc->view_slot))
        return;

    /* AO/SSR/SSGI and RT reservoir reprojection sample motion. Do not publish an unused sampled
     * descriptor for a storage-written image; NULL settings retain the legacy standalone resolve
     * contract. */
    const de_render_settings *settings = vc->settings;
    const bool sampled_motion = !settings || settings->ao != DE_QUALITY_OFF ||
                                settings->reflections != DE_QUALITY_OFF ||
                                settings->gi != DE_QUALITY_OFF || settings->tier == DE_TIER_RT;
    for (u32 t = 0; t < DE__GB_COUNT; ++t) {
        static const char *const k_names[DE__GB_COUNT] = {"vis.gb0", "vis.gb1", "vis.gb2",
                                                          "vis.motion", "vis.lighting"};
        vc->gbuffer[t] = de_rg_create_texture(
            g, &(de_gpu_texture_desc){
                   .format = de__gb_format(t),
                   .width = vc->width,
                   .height = vc->height,
                   .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC |
                            (t != DE__GB_MOTION || sampled_motion ? DE_USAGE_SAMPLED : 0u) |
                            /* DENG-305: the forward pass blends into it */
                            (t == DE__GB_LIGHTING ? DE_USAGE_RENDER_TARGET : 0u),
                   .name = k_names[t]});
        if (!DE_HANDLE_VALID(vc->gbuffer[t]))
            return; /* error set; finalize reports it */
    }
    const bool debug = st->debug.targets;
    if (debug) {
        vc->resolve_debug_pos = de_rg_create_texture(
            g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA32_FLOAT,
                                      .width = vc->width,
                                      .height = vc->height,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                      .name = "vis.resolve_debug_pos"});
        vc->resolve_debug_uv = de_rg_create_texture(
            g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA32_FLOAT,
                                      .width = vc->width,
                                      .height = vc->height,
                                      .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                      .name = "vis.resolve_debug_uv"});
        if (!DE_HANDLE_VALID(vc->resolve_debug_pos) || !DE_HANDLE_VALID(vc->resolve_debug_uv))
            return;
    }
    const de_rg_resource feedback =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = (u64)st->feedback_capacity * 4u,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .name = "vis.feedback"});
    const de_rg_resource counters =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = RS_COUNTER_BYTES,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .name = "vis.resolve_counters"});
    if (!DE_HANDLE_VALID(feedback) || !DE_HANDLE_VALID(counters))
        return;

    const u32 ring = (u32)(vc->frame_index % DE__VIS_FRAME_RING);
    rs_pass base = {.dev = s->dev,
                    .st = st,
                    .models = s->material_models,
                    .pc = {.pitch = vc->vis_ids_pitch,
                           .width = vc->width,
                           .height = vc->height,
                           .tile = vc->classify_tile,
                           .tiles_x = vc->classify_tiles_x,
                           .tile_count = vc->classify_tile_count,
                           .view_slot = vc->view_slot,
                           .visible_count = vc->visible_capacity + vc->visible_alpha_capacity,
                           .sampler = st->sampler_bindless,
                           .frame = (u32)vc->frame_index,
                           .feedback_capacity = st->feedback_capacity,
                           .flags = debug ? RS_DEBUG : 0u},
                    .rb = pv->rb,
                    .rb_offset = (u64)ring * st->entry_bytes};
    /* DENG-302: this frame's permutations (chosen at the kick; no GPU scene: every feature). */
    for (u32 m = 0; m < DE__GS_MATERIAL_MODELS; ++m)
        base.perm[m] =
            (u8)(vc->gpu_scene != NULL ? de__gpu_scene_material_permutation(vc->gpu_scene, m)
                                       : (st->standard_count != 0 ? st->standard_count - 1u : 0u));
    /* Each pass names only the resources it declares (de_rg_* need a declared access). */
    rs_pass b_clear = base;
    memcpy(b_clear.targets, vc->gbuffer, sizeof b_clear.targets);
    b_clear.debug_pos = debug ? vc->resolve_debug_pos : (de_rg_resource){0};
    b_clear.debug_uv = debug ? vc->resolve_debug_uv : (de_rg_resource){0};
    rs_pass b_fb = base;
    b_fb.feedback = feedback;
    b_fb.counters = counters;
    rs_pass b_res = b_clear;
    b_res.ids = vc->vis_ids_buffer;
    b_res.lists = vc->classify_tiles;
    b_res.args = vc->classify_args;
    b_res.scene = vc->scene_views;
    b_res.visible = vc->visible_meshlets;
    b_res.feedback = feedback;
    b_res.counters = counters;
    rs_pass b_rb = base;
    b_rb.feedback = feedback;
    b_rb.counters = counters;
    /* DENG-305: the compute clear leaves the lighting target to vis.resolve.clear_lighting. */
    rs_pass b_clear_cs = b_clear;
    b_clear_cs.targets[DE__GB_LIGHTING] = (de_rg_resource){0};
    rs_pass *pl = pass_data(vc, &b_clear);
    rs_pass *pc = pass_data(vc, &b_clear_cs), *pf = pass_data(vc, &b_fb),
            *pr = pass_data(vc, &b_res), *pb = pass_data(vc, &b_rb);
    if (pl == NULL || pc == NULL || pf == NULL || pr == NULL || pb == NULL)
        return;

    de_rg_pass *p = de_rg_add_pass(g, "vis.resolve.clear_lighting", DE_QUEUE_GRAPHICS,
                                   clear_lighting_execute, pl); /* DENG-305 */
    de_rg_write(p, vc->gbuffer[DE__GB_LIGHTING], DE_ACCESS_RENDER_TARGET);
    p = de_rg_add_pass(g, "vis.resolve.clear", DE_QUEUE_GRAPHICS, clear_execute, pc);
    for (u32 t = 0; t < DE__GB_COUNT; ++t)
        if (t != DE__GB_LIGHTING)
            de_rg_write(p, vc->gbuffer[t], DE_ACCESS_SHADER_WRITE);
    if (debug) {
        de_rg_write(p, vc->resolve_debug_pos, DE_ACCESS_SHADER_WRITE);
        de_rg_write(p, vc->resolve_debug_uv, DE_ACCESS_SHADER_WRITE);
    }

    p = de_rg_add_pass(g, "vis.resolve.feedback", DE_QUEUE_GRAPHICS, feedback_clear_execute, pf);
    de_rg_write(p, feedback, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, counters, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, "vis.resolve", DE_QUEUE_GRAPHICS, resolve_execute, pr);
    de_rg_read(p, vc->vis_ids_buffer, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->classify_tiles, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->classify_args, DE_ACCESS_INDIRECT | DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_instances, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_meshes, DE_ACCESS_SHADER_READ);
    if (DE_HANDLE_VALID(vc->scene_materials))
        de_rg_read(p, vc->scene_materials, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->visible_meshlets, DE_ACCESS_SHADER_READ);
    for (u32 t = 0; t < DE__GB_COUNT; ++t) {
        de_rg_read(p, vc->gbuffer[t], DE_ACCESS_SHADER_READ);
        de_rg_write(p, vc->gbuffer[t], DE_ACCESS_SHADER_WRITE);
    }
    if (debug) {
        de_rg_read(p, vc->resolve_debug_pos, DE_ACCESS_SHADER_READ);
        de_rg_write(p, vc->resolve_debug_pos, DE_ACCESS_SHADER_WRITE);
        de_rg_read(p, vc->resolve_debug_uv, DE_ACCESS_SHADER_READ);
        de_rg_write(p, vc->resolve_debug_uv, DE_ACCESS_SHADER_WRITE);
    }
    de_rg_read(p, feedback, DE_ACCESS_SHADER_READ); /* atomics */
    de_rg_write(p, feedback, DE_ACCESS_SHADER_WRITE);
    de_rg_read(p, counters, DE_ACCESS_SHADER_READ);
    de_rg_write(p, counters, DE_ACCESS_SHADER_WRITE);

    /* The readback is the only side effect: without it (r.texture_feedback 0) the resolve runs
     * only when a later pass reads the G-buffer, and the graph culls it (and classification)
     * otherwise. */
    if (!st->debug.readback && !(DE_HANDLE_VALID(st->cv_feedback) && de_cvar_bool(st->cv_feedback)))
        return;
    p = de_rg_add_pass(g, "vis.resolve.readback", DE_QUEUE_GRAPHICS, readback_execute, pb);
    de_rg_pass_side_effect(p);
    de_rg_read(p, feedback, DE_ACCESS_COPY_SRC);
    de_rg_read(p, counters, DE_ACCESS_COPY_SRC);
    const de_rg_resource rb = de_rg_import_buffer(g, pv->rb, pv->rb_access);
    de_rg_set_name(g, rb, "vis.resolve_readback");
    de_rg_write(p, rb, DE_ACCESS_COPY_DST);
    pv->rb_access = DE_ACCESS_COPY_DST;
    pv->slot_frame[ring] = vc->frame_index;
    pv->slot_live[ring] = true;
}
