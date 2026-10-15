/*
 * raster_compute.c: the compute-expansion raster path of the visibility buffer (SPEC §12.5 step
 * 2, §2; DENG-235): the fallback where mesh shaders are missing, and the path the CI software
 * rasterizers (lavapipe, WARP) run. Its output is the mesh path's (DENG-234): the packed vis ID
 * (R32_UINT, DE__VIS_ID_EMPTY where nothing is drawn) and the reverse-Z depth.
 *
 * Shaders (engine shaders, DENG-421): vis_raster_expand (shaders/visbuffer/vis_expand.slang) and
 * vis_raster_draw (shaders/visbuffer/vis_vs_ps.slang); both decode through DENG-234's
 * vis_decode.slang, so the culling rule and the positions are the mesh path's.
 * The vis-ID target is de__vis_raster_targets' (made and cleared once per frame).
 *
 * Per view and frame, on the graphics queue, for each phase whose raster arguments exist:
 *
 *   vis.raster_compute.clear           first raster phase of the frame    counters: SHADER_WRITE
 *   vis.raster_compute.expand[.phase2] one 128-thread group per visible   tables, visible list:
 *                                      meshlet of the phase, indirect on  READ; raster args:
 *                                      DeVisRasterArgs (byte 16)          INDIRECT + READ;
 *                                                                         counters: READ + WRITE;
 *                                                                         triangles, indices:
 *                                                                         WRITE (phase 2: + READ)
 *   vis.raster_compute.args[.phase2]   the phase's DeVisDrawIndexedArgs   counters: READ + WRITE;
 *                                                                         draw args: WRITE
 *   vis.raster_compute.draw[.phase2]   one indexed indirect draw          draw args: INDIRECT;
 *                                                                         indices, triangles,
 *                                                                         tables, list: READ;
 *                                                                         vis ID: RENDER_TARGET;
 *                                                                         depth: DEPTH R + W
 *   vis.raster_compute.stats           copies the counters into the       counters: COPY_SRC
 *                                      view's readback ring (at phase 2)
 *
 * Triangle slots: phase 1 fills slots [0, n1), phase 2 [n1, n). A slot holds the packed vis ID
 * (de__vis_id_pack(visible-list entry, triangle)) and the indices 3 * slot + 0..2, so the draw's
 * vertex 3 * slot + corner finds its triangle without a primitive ID (vis_expand.slang says why).
 *
 * Capacity ("clamp, count and grow", as the other S13 lists): the kernel stores slots below the
 * capacity and counts every kept triangle. The buffers start at RC_MIN_CAPACITY slots; when a
 * frame read back asked for more than its buffers held, the next frame grows them to the next
 * power of two of 1.25x that count, with a warning. They never grow past r.raster_compute_max_tris
 * (default 16M slots, 16 bytes each); a frame that needs more draws the first slots only and warns
 * once. Nothing reads or writes past the capacity, so an overflow never hangs the GPU.
 *
 * The triangle and index buffers are transients of the view's graph (one key while the capacity
 * holds); the per-view readback ring is made when the view first rasters. A steady frame
 * allocates no heap. The render thread reads only GPU tables, never asset records.
 */
#include "render/passes/vis/vis_stages.h"

#include <stdio.h>
#include <string.h>

/** Triangle slots at least, and the ceiling of the buffers. */
#define RC_MIN_CAPACITY (1u << 18)
/** Bytes per triangle slot: the packed ID (4) and three u32 indices (12). */
#define RC_SLOT_BYTES 16u
/** Bytes between the frames' counters in the readback buffer. */
#define RC_STATS_STRIDE 256u

/** The expansion kernels' push constants (vis_expand.slang ExpandParams, 80 bytes). */
typedef struct rc_params {
    u32 instances, meshes, materials, material_count, scene, view_slot, visible, raster_args;
    u32 counters, tris, indices, draw_args, capacity, phase, flags;
    f32 width, height;
    u32 reserved[3];
} rc_params;
DE_STATIC_ASSERT(sizeof(rc_params) == 80, "ExpandParams is 80 bytes");

/** The draw's push constants (vis_vs_ps.slang DrawParams, 32 bytes). */
typedef struct rc_draw_params {
    u32 instances, meshes, scene, view_slot, visible, tris, reserved[2];
} rc_draw_params;
DE_STATIC_ASSERT(sizeof(rc_draw_params) == 32, "DrawParams is 32 bytes");

/** One view slot's persistent objects and this frame's buffers. */
typedef struct rc_view {
    de_view handle;
    de_gpu_buffer stats_buf; /**< READBACK: the counters of each ring frame */
    const u8 *stats_map;
    u32 stats_access;
    u64 slot_frame[DE__VIS_FRAME_RING];
    u32 slot_capacity[DE__VIS_FRAME_RING];
    bool slot_live[DE__VIS_FRAME_RING];
    de__vis_raster_compute_stats stats; /**< the newest read back */
    u32 capacity;                       /**< triangle slots (grows; at most the cvar) */
    u32 warned_max;                     /**< the r.raster_compute_max_tris value warned about */
    /* This frame (valid while frame_graph == the graph and frame == its index). */
    const de_rg *frame_graph;
    u64 frame;
    de_rg_resource counters, tris, indices;
    u32 frame_capacity;
} rc_view;

typedef struct rc_state {
    de__vis_kernel clear, expand, args;
    de_gpu_pipeline draw; /**< the triangles and the full-screen DE__VIS_ID_EMPTY clear */
    u32 draw_push_bytes;
    rc_view views[DE__VIEW_MAX];
    de_cvar cv_tri_cull, cv_max_tris;
    de__vis_raster_compute_debug debug;
} rc_state;

static const de__vis_layout_const k_consts[] = {
    {"DE_VIS_EXPAND_COUNTER_TRIANGLES", DE__VIS_EXPAND_COUNTER_TRIANGLES},
    {"DE_VIS_EXPAND_COUNTER_PHASE1", DE__VIS_EXPAND_COUNTER_PHASE1},
    {"DE_VIS_EXPAND_COUNTER_TESTED", DE__VIS_EXPAND_COUNTER_TESTED},
    {"DE_VIS_EXPAND_COUNTER_MESHLETS", DE__VIS_EXPAND_COUNTER_MESHLETS},
    {"DE_VIS_EXPAND_COUNTER_BYTES", DE__VIS_EXPAND_COUNTER_BYTES},
    {"DE_VIS_EXPAND_TRI_CULL", DE__VIS_EXPAND_TRI_CULL},
    {"DE_VIS_EXPAND_GROUP", DE__VIS_EXPAND_GROUP},
};

const de__vis_layout_const *de__vis_raster_compute_layout_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

/* =================================================================================================
 * Per renderer
 * ============================================================================================== */

static void view_release(de__vis_shared *s, rc_view *pv) {
    if (DE_HANDLE_VALID(pv->stats_buf))
        de_gpu_buffer_destroy(s->dev, pv->stats_buf); /* deferred until the GPU is done */
    memset(pv, 0, sizeof *pv);
}

static void state_free(de__vis_shared *s, rc_state *st) {
    de__vis_kernel_destroy(s, &st->clear);
    de__vis_kernel_destroy(s, &st->expand);
    de__vis_kernel_destroy(s, &st->args);
    if (DE_HANDLE_VALID(st->draw))
        de_gpu_pipeline_destroy(s->dev, st->draw);
    DE_FREE(s->alloc, st, sizeof *st);
}

/* The draw pipeline: the vis-ID target and the D32 reverse-Z depth (test and write). */
static de_result make_pipeline(de__vis_shared *s, const de__engine_shader_code *code,
                               u32 push_bytes, de_gpu_pipeline *out) {
    *out =
        de_gpu_pipeline_create(s->dev, &(de_gpu_pipeline_desc){.kind = DE_PIPE_GRAPHICS,
                                                               .vs = code->entries[0].blob,
                                                               .ps = code->entries[1].blob,
                                                               .color_formats = {DE_FMT_R32_UINT},
                                                               .color_count = 1,
                                                               .depth_format = DE_FMT_D32_FLOAT,
                                                               .cull = DE_CULL_NONE,
                                                               .depth_test = true,
                                                               .depth_write = true,
                                                               .push_constant_bytes = push_bytes,
                                                               .name = "vis raster draw"});
    if (DE_HANDLE_VALID(*out))
        return DE_OK;
    const de_error *er = de_error_last();
    return DE_FAIL_WRAP(er != NULL && er->result != DE_OK ? er->result : DE_ERR_INTERNAL,
                        "vis: the pipeline vis raster draw");
}

de_result de__vis_raster_compute_init(de__vis_shared *s) {
    rc_state *st = DE_NEW(s->alloc, rc_state, DE_MEM_RENDER);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: compute raster state");
    memset(st, 0, sizeof *st);
    /* The same cvar as the mesh path (DENG-234): registering it again returns the handle. */
    st->cv_tri_cull = de_cvar_register(&(de_cvar_desc){
        .name = "r.raster_tri_cull",
        .help = "Vis-buffer raster: cull triangles per material (back faces unless the material "
                "is double-sided, zero area) and, on the compute path, outside the frustum or "
                "between pixel centres (0: draw every triangle of the visible meshlets)",
        .type = DE_CVAR_BOOL,
        .flags = DE_CVAR_DEV_ONLY,
        .default_number = 1.0});
    st->cv_max_tris = de_cvar_register(&(de_cvar_desc){
        .name = "r.raster_compute_max_tris",
        .help = "Vis-buffer compute raster: the most triangles one view draws per frame (16 bytes "
                "of buffer each); more are not drawn, with a warning",
        .type = DE_CVAR_INT,
        .default_number = (f64)DE__VIS_RASTER_COMPUTE_MAX_TRIS_DEFAULT,
        .min = 1.0,
        .max = (f64)DE__VIS_RASTER_COMPUTE_MAX_TRIS_LIMIT});
    de_error_clear(); /* a DEV_ONLY cvar in Shipping is no error */

    static const char *const k_kernels[3] = {"cs_expand_clear", "cs_expand", "cs_expand_args"};
    de__engine_shader_code code;
    de_result r = de__vis_shader_load(s, "vis_raster_expand", k_kernels, 3, &code);
    if (r == DE_OK) {
        static const u32 one[3] = {1u, 1u, 1u}, group[3] = {DE__VIS_EXPAND_GROUP, 1u, 1u};
        const u32 pb = (u32)sizeof(rc_params);
        r = de__vis_kernel_create(s, &code, 0, "vis raster expand clear", one, pb, &st->clear);
        if (r == DE_OK)
            r = de__vis_kernel_create(s, &code, 1, "vis raster expand", group, pb, &st->expand);
        if (r == DE_OK)
            r = de__vis_kernel_create(s, &code, 2, "vis raster expand args", one, pb, &st->args);
        de__engine_shader_code_free(&code);
    }
    if (r == DE_OK) {
        static const char *const k_draw[2] = {"vs_draw", "ps_draw"};
        r = de__vis_shader_load(s, "vis_raster_draw", k_draw, 2, &code);
        if (r == DE_OK) {
            u32 pb = (u32)sizeof(rc_draw_params);
            if (code.push_constant_bytes > pb)
                pb = code.push_constant_bytes;
            st->draw_push_bytes = (pb + 3u) & ~3u;
            r = make_pipeline(s, &code, st->draw_push_bytes, &st->draw);
            de__engine_shader_code_free(&code);
        }
    }
    if (r != DE_OK) {
        state_free(s, st);
        return r;
    }
    s->raster_compute = st;
    return DE_OK;
}

void de__vis_raster_compute_shutdown(de__vis_shared *s) {
    rc_state *st = (rc_state *)s->raster_compute;
    if (st == NULL)
        return;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        view_release(s, &st->views[i]);
    state_free(s, st);
    s->raster_compute = NULL;
}

de__vis_raster_compute_stats de__vis_view_raster_compute_stats(const de__vis_shared *s, u32 slot) {
    const rc_state *st = s != NULL ? (const rc_state *)s->raster_compute : NULL;
    if (st == NULL || slot >= DE__VIEW_MAX)
        return (de__vis_raster_compute_stats){0};
    return st->views[slot].stats;
}

void de__vis_raster_compute_set_debug(de__vis_shared *s, const de__vis_raster_compute_debug *d) {
    rc_state *st = s != NULL ? (rc_state *)s->raster_compute : NULL;
    if (st != NULL)
        st->debug = d != NULL ? *d : (de__vis_raster_compute_debug){0};
}

/* =================================================================================================
 * One view
 * ============================================================================================== */

static bool view_ensure(de__vis_shared *s, rc_view *pv, u32 slot) {
    if (DE_HANDLE_VALID(pv->stats_buf))
        return true;
    char name[48];
    snprintf(name, sizeof name, "vis raster stats %u", slot);
    pv->stats_buf = de_gpu_buffer_create(
        s->dev, &(de_gpu_buffer_desc){.size = (u64)DE__VIS_FRAME_RING * RC_STATS_STRIDE,
                                      .usage = DE_USAGE_COPY_DST,
                                      .memory = DE_MEM_READBACK,
                                      .name = name});
    pv->stats_map = DE_HANDLE_VALID(pv->stats_buf)
                        ? (const u8 *)de_gpu_buffer_map(s->dev, pv->stats_buf)
                        : NULL;
    if (pv->stats_map == NULL) {
        const de_error *e = de_error_last();
        DE_ERROR(s->log, "vis: view slot %u: raster stats buffer: %s", slot,
                 e != NULL ? e->message : "?");
        const de_view h = pv->handle;
        view_release(s, pv);
        pv->handle = h;
        return false;
    }
    pv->stats_access = DE_ACCESS_NONE;
    return true;
}

/* Takes the newest completed frame's counters (frames before `completed` are done). */
static void read_stats(rc_view *pv, u64 completed) {
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
    u32 c[4];
    memcpy(c, pv->stats_map + (size_t)best * RC_STATS_STRIDE, sizeof c);
    pv->stats =
        (de__vis_raster_compute_stats){.valid = true,
                                       .frame = pv->slot_frame[best],
                                       .triangles = c[DE__VIS_EXPAND_COUNTER_TRIANGLES / 4u],
                                       .phase1 = c[DE__VIS_EXPAND_COUNTER_PHASE1 / 4u],
                                       .tested = c[DE__VIS_EXPAND_COUNTER_TESTED / 4u],
                                       .meshlets = c[DE__VIS_EXPAND_COUNTER_MESHLETS / 4u],
                                       .capacity = pv->slot_capacity[best]};
}

static u32 pow2_at_least(u64 v, u32 cap) {
    u64 p = 1;
    while (p < v && p < cap)
        p <<= 1;
    return (u32)(p < cap ? p : cap);
}

/* The triangle capacity this frame (header). */
static u32 tri_capacity(de__vis_shared *s, rc_state *st, rc_view *pv, const de__vis_context *vc) {
    i64 m = DE_HANDLE_VALID(st->cv_max_tris) ? de_cvar_int(st->cv_max_tris)
                                             : (i64)DE__VIS_RASTER_COMPUTE_MAX_TRIS_DEFAULT;
    if (m < 1)
        m = 1;
    if (m > (i64)DE__VIS_RASTER_COMPUTE_MAX_TRIS_LIMIT)
        m = (i64)DE__VIS_RASTER_COMPUTE_MAX_TRIS_LIMIT;
    const u32 max = (u32)m;
    u32 cap;
    if (st->debug.capacity_fixed != 0) {
        cap = st->debug.capacity_fixed < max ? st->debug.capacity_fixed : max;
    } else {
        cap = pv->capacity > RC_MIN_CAPACITY ? pv->capacity : RC_MIN_CAPACITY;
        const de__vis_raster_compute_stats *rs = &pv->stats;
        if (rs->valid && rs->triangles > rs->capacity) {
            const u32 want = pow2_at_least((u64)rs->triangles + rs->triangles / 4u,
                                           DE__VIS_RASTER_COMPUTE_MAX_TRIS_LIMIT);
            if (want > cap && cap < max) {
                const u32 to = want < max ? want : max;
                DE_WARN(s->log,
                        "vis: view slot %u: the compute raster's triangle buffer overflowed in "
                        "frame %llu (%u triangles for %u slots; the rest were not drawn); it "
                        "grows to %u",
                        vc->view_slot, (unsigned long long)rs->frame, rs->triangles, rs->capacity,
                        to);
                cap = to;
            }
        }
        if (cap > max)
            cap = max;
    }
    /* At the ceiling: say once per ceiling value that triangles are not drawn. */
    const de__vis_raster_compute_stats *rs = &pv->stats;
    if (rs->valid && rs->triangles > rs->capacity && rs->capacity >= cap && pv->warned_max != max) {
        DE_WARN(s->log,
                "vis: view slot %u: %u triangles do not fit the compute raster's buffers (%u "
                "slots, r.raster_compute_max_tris %u): the rest are not drawn",
                vc->view_slot, rs->triangles, rs->capacity, max);
        pv->warned_max = max;
    }
    pv->capacity = cap;
    return cap;
}

/* ---------- Passes ---------- */

typedef struct rc_pass {
    de_gpu_device *dev;
    de__vis_kernel kernel;
    rc_params pc;
    de_rg_resource instances, meshes, materials, scene, visible, raster_args, counters, tris,
        indices, draw_args, vis_id, depth;
    de_gpu_pipeline draw;
    de_gpu_buffer stats;
    u64 stats_offset;
} rc_pass;

static u32 bindless_of(const rc_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return DE_HANDLE_VALID(r) ? de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, r)) : 0u;
}

static rc_params kernel_params(const rc_pass *p, const de_rg_pass *pass) {
    rc_params pc = p->pc;
    pc.instances = bindless_of(p, pass, p->instances);
    pc.meshes = bindless_of(p, pass, p->meshes);
    pc.materials = bindless_of(p, pass, p->materials);
    pc.scene = bindless_of(p, pass, p->scene);
    pc.visible = bindless_of(p, pass, p->visible);
    pc.raster_args = bindless_of(p, pass, p->raster_args);
    pc.counters = bindless_of(p, pass, p->counters);
    pc.tris = bindless_of(p, pass, p->tris);
    pc.indices = bindless_of(p, pass, p->indices);
    pc.draw_args = bindless_of(p, pass, p->draw_args);
    return pc;
}

static void kernel_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rc_pass *p = (const rc_pass *)user;
    const rc_params pc = kernel_params(p, pass);
    de_gpu_cmd_set_pipeline(cmd, p->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    if (DE_HANDLE_VALID(p->raster_args)) /* the expansion; clear and args: one thread */
        de_gpu_cmd_dispatch_indirect(cmd, de_rg_buffer(pass, p->raster_args),
                                     DE__VIS_RASTER_ARGS_EXPAND);
    else
        de_gpu_cmd_dispatch(cmd, 1, 1, 1);
}

static void draw_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rc_pass *p = (const rc_pass *)user;
    const de_gpu_attachment color = {.texture = de_rg_texture(pass, p->vis_id), .store = true};
    const de_gpu_attachment depth = {.texture = de_rg_texture(pass, p->depth), .store = true};
    const rc_draw_params dp = {.instances = bindless_of(p, pass, p->instances),
                               .meshes = bindless_of(p, pass, p->meshes),
                               .scene = bindless_of(p, pass, p->scene),
                               .view_slot = p->pc.view_slot,
                               .visible = bindless_of(p, pass, p->visible),
                               .tris = bindless_of(p, pass, p->tris)};
    de_gpu_cmd_begin_render(cmd, &color, 1, &depth);
    de_gpu_cmd_set_pipeline(cmd, p->draw);
    de_gpu_cmd_push_constants(cmd, &dp, (u32)sizeof dp);
    de_gpu_cmd_set_index_buffer(cmd, de_rg_buffer(pass, p->indices), 0, false);
    de_gpu_cmd_draw_indexed_indirect(cmd, de_rg_buffer(pass, p->draw_args), 0, (de_gpu_buffer){0},
                                     0, 1);
    de_gpu_cmd_end_render(cmd);
}

static void stats_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rc_pass *p = (const rc_pass *)user;
    de_gpu_cmd_copy_buffer(cmd, p->stats, p->stats_offset, de_rg_buffer(pass, p->counters), 0,
                           DE__VIS_EXPAND_COUNTER_BYTES);
}

static rc_pass *pass_data(de__vis_context *vc, const rc_pass *base, const de__vis_kernel *k) {
    rc_pass *p = (rc_pass *)de_arena_push(vc->arena, sizeof *p, 16);
    if (p != NULL) {
        *p = *base;
        if (k != NULL)
            p->kernel = *k;
    }
    return p;
}

static void arena_full(void) {
    (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: compute raster: the frame arena is full");
}

static bool frame_current(const rc_view *pv, const de_rg *g, const de__vis_context *vc) {
    return pv->frame_graph == g && pv->frame == vc->frame_index && DE_HANDLE_VALID(pv->counters);
}

/* The copy of the counters into the view's readback ring (after both phases). */
static void add_stats_pass(de_rg *g, de__vis_context *vc, rc_view *pv) {
    if (!frame_current(pv, g, vc) || !DE_HANDLE_VALID(pv->stats_buf))
        return;
    const u32 slot = (u32)(vc->frame_index % DE__VIS_FRAME_RING);
    const rc_pass base = {.dev = vc->shared->dev,
                          .counters = pv->counters,
                          .stats = pv->stats_buf,
                          .stats_offset = (u64)slot * RC_STATS_STRIDE};
    rc_pass *ps = pass_data(vc, &base, NULL);
    if (ps == NULL) {
        arena_full();
        return;
    }
    de_rg_pass *p =
        de_rg_add_pass(g, "vis.raster_compute.stats", DE_QUEUE_GRAPHICS, stats_execute, ps);
    de_rg_pass_side_effect(p);
    de_rg_read(p, pv->counters, DE_ACCESS_COPY_SRC);
    const de_rg_resource rb = de_rg_import_buffer(g, pv->stats_buf, pv->stats_access);
    de_rg_set_name(g, rb, "vis.raster_compute_stats");
    de_rg_write(p, rb, DE_ACCESS_COPY_DST);
    pv->stats_access = DE_ACCESS_COPY_DST; /* the epilogue leaves it in its last access */
    pv->slot_frame[slot] = vc->frame_index;
    pv->slot_capacity[slot] = pv->frame_capacity;
    pv->slot_live[slot] = true;
}

/* The frame's triangle buffers, counters and (when no raster pass made it) the vis-ID target. */
static bool frame_begin(de__vis_shared *s, rc_state *st, rc_view *pv, de_rg *g,
                        de__vis_context *vc) {
    const u32 cap = tri_capacity(s, st, pv, vc);
    const de_rg_resource tris =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = (u64)cap * 4u,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .name = "vis.raster_tris"});
    const de_rg_resource indices = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = (u64)cap * 12u,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_INDEX | DE_USAGE_COPY_SRC,
                                 .name = "vis.raster_indices"});
    const de_rg_resource counters =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = DE__VIS_EXPAND_COUNTER_BYTES,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .name = "vis.raster_counters"});
    if (!DE_HANDLE_VALID(tris) || !DE_HANDLE_VALID(indices) || !DE_HANDLE_VALID(counters))
        return false; /* error set; finalize reports it */
    pv->frame_graph = g;
    pv->frame = vc->frame_index;
    pv->tris = tris;
    pv->indices = indices;
    pv->counters = counters;
    pv->frame_capacity = cap;
    return true;
}

void de__vis_raster_compute(de_rg *g, de__vis_context *vc, de__vis_phase phase) {
    de__vis_shared *s = vc->shared;
    rc_state *st = (rc_state *)s->raster_compute;
    if (st == NULL || vc->view_slot >= DE__VIEW_MAX)
        return;
    rc_view *pv = &st->views[vc->view_slot];
    const u32 pi = DE__VIS_PHASE_INDEX(phase);
    const bool p1 = phase == DE__VIS_PHASE_1;
    if (p1) {
        if (vc->view_new || pv->handle.id != vc->view.id) {
            view_release(s, pv);
            pv->handle = vc->view;
        }
        if (DE_HANDLE_VALID(pv->stats_buf))
            read_stats(pv, vc->frames_completed);
    }
    const bool run = DE_HANDLE_VALID(vc->raster_args[pi]) &&
                     DE_HANDLE_VALID(vc->visible_meshlets) &&
                     DE_HANDLE_VALID(vc->scene_instances) && DE_HANDLE_VALID(vc->scene_meshes) &&
                     DE_HANDLE_VALID(vc->scene_views) && vc->depth_target &&
                     DE_HANDLE_VALID(vc->depth) && vc->width > 0 && vc->height > 0;
    if (!run || !view_ensure(s, pv, vc->view_slot)) {
        if (!p1)
            add_stats_pass(g, vc, pv);
        return;
    }

    /* The first raster phase of the frame makes the buffers and clears the counters. */
    const bool first = !frame_current(pv, g, vc);
    if (first && !frame_begin(s, st, pv, g, vc))
        return;
    /* The vis-ID target: made and cleared once per frame (DENG-234's "vis.vis_ids_clear"). */
    if (!DE_HANDLE_VALID(de__vis_raster_targets(g, vc)))
        return;
    const de_rg_resource draw_args = de_rg_create_buffer(
        g,
        &(de_gpu_buffer_desc){.size = sizeof(de__vis_draw_indexed_args),
                              .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT | DE_USAGE_COPY_SRC,
                              .name = p1 ? "vis.raster_draw_args" : "vis.raster_draw_args.phase2"});
    if (!DE_HANDLE_VALID(draw_args))
        return;

    const bool materials = DE_HANDLE_VALID(vc->scene_materials) && vc->gpu_scene != NULL;
    const u32 flags = !DE_HANDLE_VALID(st->cv_tri_cull) || de_cvar_bool(st->cv_tri_cull)
                          ? DE__VIS_EXPAND_TRI_CULL
                          : 0u;
    const rc_pass base = {
        .dev = s->dev,
        .pc = {.material_count =
                   materials ? de__gpu_scene_table(vc->gpu_scene, DE__GS_MATERIALS).count : 0u,
               .view_slot = vc->view_slot,
               .capacity = pv->frame_capacity,
               .phase = (u32)phase,
               .flags = flags,
               .width = (f32)vc->width,
               .height = (f32)vc->height},
        .instances = vc->scene_instances,
        .meshes = vc->scene_meshes,
        .materials = materials ? vc->scene_materials : (de_rg_resource){0},
        .scene = vc->scene_views,
        .visible = vc->visible_meshlets,
        .raster_args = vc->raster_args[pi],
        .counters = pv->counters,
        .tris = pv->tris,
        .indices = pv->indices,
        .draw_args = draw_args,
        .vis_id = vc->vis_ids,
        .depth = vc->depth,
        .draw = st->draw};
    /* Each pass's data names only the resources it declares (de_rg_buffer needs a declared
     * access). */
    rc_pass b_clear = {.dev = s->dev, .pc = base.pc, .counters = pv->counters};
    rc_pass b_args = {
        .dev = s->dev, .pc = base.pc, .counters = pv->counters, .draw_args = draw_args};
    rc_pass b_expand = base;
    b_expand.draw_args = (de_rg_resource){0};
    b_expand.vis_id = (de_rg_resource){0};
    b_expand.depth = (de_rg_resource){0};
    rc_pass b_draw = base;
    b_draw.materials = (de_rg_resource){0};
    b_draw.raster_args = (de_rg_resource){0};
    b_draw.counters = (de_rg_resource){0};
    rc_pass *pc = first ? pass_data(vc, &b_clear, &st->clear) : NULL;
    rc_pass *pe = pass_data(vc, &b_expand, &st->expand);
    rc_pass *pa = pass_data(vc, &b_args, &st->args);
    rc_pass *pd = pass_data(vc, &b_draw, NULL);
    if ((first && pc == NULL) || pe == NULL || pa == NULL || pd == NULL) {
        arena_full();
        return;
    }
    de_rg_pass *p;
    if (first) {
        p = de_rg_add_pass(g, "vis.raster_compute.clear", DE_QUEUE_GRAPHICS, kernel_execute, pc);
        de_rg_write(p, pv->counters, DE_ACCESS_SHADER_WRITE);
    }

    p = de_rg_add_pass(g, p1 ? "vis.raster_compute.expand" : "vis.raster_compute.expand.phase2",
                       DE_QUEUE_GRAPHICS, kernel_execute, pe);
    de_rg_read(p, vc->scene_instances, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_meshes, DE_ACCESS_SHADER_READ);
    if (materials)
        de_rg_read(p, vc->scene_materials, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->visible_meshlets, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->raster_args[pi], DE_ACCESS_INDIRECT | DE_ACCESS_SHADER_READ);
    de_rg_read(p, pv->counters, DE_ACCESS_SHADER_READ); /* atomics */
    de_rg_write(p, pv->counters, DE_ACCESS_SHADER_WRITE);
    if (!first) { /* keep the earlier phase's slots */
        de_rg_read(p, pv->tris, DE_ACCESS_SHADER_READ);
        de_rg_read(p, pv->indices, DE_ACCESS_SHADER_READ);
    }
    de_rg_write(p, pv->tris, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, pv->indices, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, p1 ? "vis.raster_compute.args" : "vis.raster_compute.args.phase2",
                       DE_QUEUE_GRAPHICS, kernel_execute, pa);
    de_rg_read(p, pv->counters, DE_ACCESS_SHADER_READ);
    de_rg_write(p, pv->counters, DE_ACCESS_SHADER_WRITE); /* phase 1 stores its end */
    de_rg_write(p, draw_args, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, p1 ? "vis.raster_compute.draw" : "vis.raster_compute.draw.phase2",
                       DE_QUEUE_GRAPHICS, draw_execute, pd);
    de_rg_read(p, draw_args, DE_ACCESS_INDIRECT);
    de_rg_read(p, pv->indices, DE_ACCESS_SHADER_READ); /* index input */
    de_rg_read(p, pv->tris, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->visible_meshlets, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_instances, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_meshes, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_views, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->vis_ids, DE_ACCESS_RENDER_TARGET);
    de_rg_write(p, vc->vis_ids, DE_ACCESS_RENDER_TARGET);
    de_rg_read(p, vc->depth, DE_ACCESS_DEPTH_READ);
    de_rg_write(p, vc->depth, DE_ACCESS_DEPTH_READ | DE_ACCESS_DEPTH_WRITE);

    if (!p1)
        add_stats_pass(g, vc, pv);
}
