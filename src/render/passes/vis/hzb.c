/*
 * hzb.c: the hierarchical depth buffer of each view (SPEC §12.5 step 1; DENG-233). The kernels
 * are shaders/culling/hzb_build.slang (engine shader "vis_hzb_build"); vis_stages.h has the API.
 *
 * Each view slot owns one HZB texture: R32F, mip 0 the previous power of two of the view's depth
 * size in each dimension, a full mip chain, STORAGE | SAMPLED | COPY_SRC. It is made when the
 * view first culls and remade (the old one through deferred destruction) when the depth size
 * changes. Between frames it rests in DE_ACCESS_SHADER_READ (the graph exports it so).
 *
 * The hzb_build stage runs after phase-1 raster: "vis.hzb.level0" reads the depth
 * (DE_ACCESS_SHADER_READ) and writes mip 0 (it declares every mip, so all are in the
 * shader-write state from then on); then the reduce passes. Every range that changes the HZB's
 * state is exact ([0, mips)): an import's mip count is unknown to the graph, and a barrier on an
 * open range past the last mip would reach real mips through the backends' clamping. Whole-
 * texture reads in the resting state (DE_ACCESS_SHADER_READ) need no barrier there. Each reduce
 * pass reads and writes a mip range with de_rg_read_sub / de_rg_write_sub: the mip it reduces from
 * (read through its storage view, so the range is in the shader-write state) and the mips it
 * writes. The render graph orders the passes by those ranges and issues the barriers between them.
 *
 *   single pass (r.hzb_single_pass 1, default)  "vis.hzb.reduce<b>": cs_hzb_reduce6 makes up to
 *       6 mips from mip b per dispatch (groupshared memory), so 2 dispatches up to 4096 x 4096
 *   multi pass (0)                              "vis.hzb.mip<n>": cs_hzb_reduce, one per mip
 * Both give the same texels (min is exact); the multi-pass path is the fallback and the
 * reference of the test hzb.single_pass_vs_multi.
 */
#include "render/passes/vis/vis_stages.h"

#include "render/graph/rg_internal.h"

#include <stdio.h>
#include <string.h>

/** Threads per axis of cs_hzb_level0 / cs_hzb_reduce, and the block cs_hzb_reduce6 makes per
 * group in its first mip. */
#define HZB_GROUP        8u
#define HZB_REDUCE6_TILE 32u
#define HZB_REDUCE6_MIPS 6u

/** hzb_build.slang HzbParams (the push constants of every kernel). */
typedef struct hzb_params {
    u32 src, src_width, src_height, dst_width, dst_height, count, op, reserved;
    u32 dst[8];
} hzb_params;
DE_STATIC_ASSERT(sizeof(hzb_params) == 64, "HzbParams is 64 bytes");

typedef struct hzb_view {
    de_view handle;
    de_gpu_texture texture;
    u32 width, height, mips; /**< mip 0 size */
    u32 src_w, src_h;        /**< the depth size it was made for */
    u32 access;              /**< its access between frames */
    u32 sampled;             /**< sampled bindless index */
    u32 storage[DE__VIS_HZB_MAX_MIPS];
    /* A replaced HZB (new size or new view), destroyed in a later frame: see retire(). */
    de_gpu_texture retired;
    u64 retired_frame;
} hzb_view;

typedef struct hzb_state {
    de__vis_kernel level0, reduce, reduce6;
    hzb_view views[DE__VIEW_MAX];
} hzb_state;

static const char *const k_mip_names[DE__VIS_HZB_MAX_MIPS] = {
    "vis.hzb.mip0",  "vis.hzb.mip1",  "vis.hzb.mip2",  "vis.hzb.mip3",
    "vis.hzb.mip4",  "vis.hzb.mip5",  "vis.hzb.mip6",  "vis.hzb.mip7",
    "vis.hzb.mip8",  "vis.hzb.mip9",  "vis.hzb.mip10", "vis.hzb.mip11",
    "vis.hzb.mip12", "vis.hzb.mip13", "vis.hzb.mip14", "vis.hzb.mip15"};
static const char *const k_reduce_names[DE__VIS_HZB_MAX_MIPS] = {
    "vis.hzb.reduce0",  "vis.hzb.reduce1",  "vis.hzb.reduce2",  "vis.hzb.reduce3",
    "vis.hzb.reduce4",  "vis.hzb.reduce5",  "vis.hzb.reduce6",  "vis.hzb.reduce7",
    "vis.hzb.reduce8",  "vis.hzb.reduce9",  "vis.hzb.reduce10", "vis.hzb.reduce11",
    "vis.hzb.reduce12", "vis.hzb.reduce13", "vis.hzb.reduce14", "vis.hzb.reduce15"};

/* ---------- Sizes ---------- */

static u32 prev_pow2(u32 v) {
    u32 p = 1u;
    while (p <= v / 2u)
        p <<= 1u;
    return p;
}

void de__vis_hzb_size(u32 w, u32 h, u32 *hzb_w, u32 *hzb_h, u32 *mips) {
    const u32 hw = prev_pow2(w > 0 ? w : 1u), hh = prev_pow2(h > 0 ? h : 1u);
    u32 m = 1u;
    while (m < DE__VIS_HZB_MAX_MIPS && ((hw >> m) > 0u || (hh >> m) > 0u))
        ++m;
    *hzb_w = hw;
    *hzb_h = hh;
    *mips = m;
}

/* ---------- Init / shutdown ---------- */

/* Releases everything at shutdown (the GPU is idle). */
static void view_release(de__vis_shared *s, hzb_view *v) {
    if (DE_HANDLE_VALID(v->texture))
        de_gpu_texture_destroy(s->dev, v->texture); /* deferred until the GPU is done */
    if (DE_HANDLE_VALID(v->retired))
        de_gpu_texture_destroy(s->dev, v->retired);
    memset(v, 0, sizeof *v);
}

/* Drops the view's HZB while frames may still use it. It is destroyed (deferred destruction) only
 * in a later frame: right after a frame-fence wait, Metal's debug layer still counts the last
 * command buffer that used it as running, and an immediate release can trip it. */
static void retire(de__vis_shared *s, hzb_view *v, u64 frame) {
    if (DE_HANDLE_VALID(v->retired))
        de_gpu_texture_destroy(s->dev, v->retired);
    v->retired = v->texture;
    v->retired_frame = frame;
    v->texture = (de_gpu_texture){0};
}

de_result de__vis_hzb_build_init(de__vis_shared *s) {
    hzb_state *st = DE_NEW(s->alloc, hzb_state, DE_MEM_RENDER);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: HZB state");
    memset(st, 0, sizeof *st);
    static const char *const k_entries[3] = {"cs_hzb_level0", "cs_hzb_reduce", "cs_hzb_reduce6"};
    de__engine_shader_code code;
    de_result r = de__vis_shader_load(s, "vis_hzb_build", k_entries, 3, &code);
    if (r == DE_OK) {
        static const u32 g8[3] = {HZB_GROUP, HZB_GROUP, 1u}, g16[3] = {16u, 16u, 1u};
        const u32 pb = (u32)sizeof(hzb_params);
        r = de__vis_kernel_create(s, &code, 0, "vis hzb level0", g8, pb, &st->level0);
        if (r == DE_OK)
            r = de__vis_kernel_create(s, &code, 1, "vis hzb reduce", g8, pb, &st->reduce);
        if (r == DE_OK)
            r = de__vis_kernel_create(s, &code, 2, "vis hzb reduce6", g16, pb, &st->reduce6);
        de__engine_shader_code_free(&code);
    }
    if (r != DE_OK) {
        de__vis_kernel_destroy(s, &st->level0);
        de__vis_kernel_destroy(s, &st->reduce);
        de__vis_kernel_destroy(s, &st->reduce6);
        DE_FREE(s->alloc, st, sizeof *st);
        return r;
    }
    s->stage[DE__VIS_STAGE_HZB_BUILD] = st;
    return DE_OK;
}

void de__vis_hzb_build_shutdown(de__vis_shared *s) {
    hzb_state *st = (hzb_state *)s->stage[DE__VIS_STAGE_HZB_BUILD];
    if (st == NULL)
        return;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        view_release(s, &st->views[i]);
    de__vis_kernel_destroy(s, &st->level0);
    de__vis_kernel_destroy(s, &st->reduce);
    de__vis_kernel_destroy(s, &st->reduce6);
    DE_FREE(s->alloc, st, sizeof *st);
    s->stage[DE__VIS_STAGE_HZB_BUILD] = NULL;
}

/* ---------- Passes ---------- */

typedef struct hzb_pass {
    de_gpu_device *dev;
    de__vis_kernel kernel;
    hzb_params pc;
    u32 groups[2];
    de_rg_resource src; /**< level0: the source (sampled); else invalid (the HZB's mip src_mip) */
    de_rg_resource hzb; /**< the HZB: its storage views are resolved at execution (import or
                             transient) */
    u32 src_mip;        /**< reduce: the mip read through its storage view */
    u32 dst_mip;        /**< the first mip written (pc.count of them) */
} hzb_pass;

static void hzb_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const hzb_pass *p = (const hzb_pass *)user;
    hzb_params pc = p->pc;
    const de_gpu_texture hzb = de_rg_texture(pass, p->hzb);
    if (DE_HANDLE_VALID(p->src))
        pc.src = de_gpu_texture_bindless(p->dev, de_rg_texture(pass, p->src), 0, false);
    else
        pc.src = de_gpu_texture_bindless(p->dev, hzb, p->src_mip, true);
    for (u32 i = 0; i < pc.count && i < 8u; ++i)
        pc.dst[i] = de_gpu_texture_bindless(p->dev, hzb, p->dst_mip + i, true);
    de_gpu_cmd_set_pipeline(cmd, p->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, p->groups[0], p->groups[1], 1);
}

static u32 mip_dim(u32 v, u32 m) {
    const u32 d = v >> m;
    return d > 0 ? d : 1u;
}

static u32 div_up(u32 a, u32 b) {
    return (a + b - 1u) / b;
}

/* One reduce pass: from mip `b` into mips b + 1 .. b + count. */
static void add_reduce(de__vis_shared *s, hzb_state *st, de_rg *g, const de__vis_hzb_target *t,
                       u32 op, u32 b, u32 count, bool grouped) {
    hzb_pass *p = (hzb_pass *)de_arena_push_zero(g->arena, sizeof *p, 16);
    if (p == NULL) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: HZB: the frame arena is full");
        return;
    }
    p->dev = s->dev;
    p->kernel = grouped ? st->reduce6 : st->reduce;
    p->hzb = t->resource;
    p->src_mip = b;
    p->dst_mip = b + 1u;
    p->pc = (hzb_params){.src_width = mip_dim(t->width, b),
                         .src_height = mip_dim(t->height, b),
                         .dst_width = mip_dim(t->width, b + 1u),
                         .dst_height = mip_dim(t->height, b + 1u),
                         .count = count,
                         .op = op};
    const u32 tile = grouped ? HZB_REDUCE6_TILE : HZB_GROUP;
    p->groups[0] = div_up(p->pc.dst_width, tile);
    p->groups[1] = div_up(p->pc.dst_height, tile);
    de_rg_pass *rp = de_rg_add_pass(g, grouped ? k_reduce_names[b] : k_mip_names[b + 1u],
                                    DE_QUEUE_GRAPHICS, hzb_execute, p);
    /* Mip b is read through its storage view: read + write keeps it in the shader-write state. */
    const de_rg_subresource range = {.mip_first = b, .mip_count = count + 1u, .layer_count = 1};
    de_rg_read_sub(rp, t->resource, range, DE_ACCESS_SHADER_READ);
    de_rg_write_sub(rp, t->resource, range, DE_ACCESS_SHADER_WRITE);
}

void de__vis_hzb_add_passes(de__vis_shared *s, de_rg *g, de_rg_resource src, u32 src_w, u32 src_h,
                            const de__vis_hzb_target *t, u32 op, bool single_pass) {
    hzb_state *st = (hzb_state *)s->stage[DE__VIS_STAGE_HZB_BUILD];
    if (st == NULL || !DE_HANDLE_VALID(src) || !DE_HANDLE_VALID(t->resource) || t->mips == 0)
        return;
    hzb_pass *p = (hzb_pass *)de_arena_push_zero(g->arena, sizeof *p, 16);
    if (p == NULL) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: HZB: the frame arena is full");
        return;
    }
    p->dev = s->dev;
    p->kernel = st->level0;
    p->src = src;
    p->pc = (hzb_params){.src_width = src_w,
                         .src_height = src_h,
                         .dst_width = t->width,
                         .dst_height = t->height,
                         .count = 1u,
                         .op = op};
    p->hzb = t->resource;
    p->dst_mip = 0;
    p->groups[0] = div_up(t->width, HZB_GROUP);
    p->groups[1] = div_up(t->height, HZB_GROUP);
    de_rg_pass *rp = de_rg_add_pass(g, "vis.hzb.level0", DE_QUEUE_GRAPHICS, hzb_execute, p);
    de_rg_read(rp, src, DE_ACCESS_SHADER_READ);
    /* Every mip, though the kernel writes only mip 0: they are all in the shader-write state for
     * the whole build (the reduce passes rewrite each). The range is exact: the HZB is an import,
     * whose mip count the graph does not know, so an open range would also cover mips past the
     * last, and a barrier there names no real mip (the backends map it to other mips). */
    de_rg_write_sub(rp, t->resource,
                    (de_rg_subresource){.mip_first = 0, .mip_count = t->mips, .layer_count = 1},
                    DE_ACCESS_SHADER_WRITE);
    for (u32 b = 0; b + 1u < t->mips;) {
        const u32 left = t->mips - 1u - b;
        const u32 n = single_pass ? (left < HZB_REDUCE6_MIPS ? left : HZB_REDUCE6_MIPS) : 1u;
        add_reduce(s, st, g, t, op, b, n, single_pass);
        b += n;
    }
}

/* ---------- The view's HZB ---------- */

void de__vis_hzb_prepare(de_rg *g, de__vis_context *vc) {
    de__vis_shared *s = vc->shared;
    hzb_state *st = (hzb_state *)s->stage[DE__VIS_STAGE_HZB_BUILD];
    if (st == NULL || vc->view_slot >= DE__VIEW_MAX || vc->depth_width == 0 ||
        vc->depth_height == 0)
        return;
    hzb_view *v = &st->views[vc->view_slot];
    if (DE_HANDLE_VALID(v->retired) && vc->frame_index > v->retired_frame + 1u) {
        de_gpu_texture_destroy(s->dev, v->retired);
        v->retired = (de_gpu_texture){0};
    }
    if (v->handle.id != vc->view.id || vc->view_new) {
        retire(s, v, vc->frame_index);
        v->handle = vc->view;
    }
    if (!DE_HANDLE_VALID(v->texture) || v->src_w != vc->depth_width ||
        v->src_h != vc->depth_height) {
        if (DE_HANDLE_VALID(v->texture))
            retire(s, v, vc->frame_index);
        u32 w = 0, hh = 0, mips = 0;
        de__vis_hzb_size(vc->depth_width, vc->depth_height, &w, &hh, &mips);
        char name[48];
        snprintf(name, sizeof name, "vis hzb %u", vc->view_slot);
        v->texture = de_gpu_texture_create(
            s->dev,
            &(de_gpu_texture_desc){.format = DE_FMT_R32_FLOAT,
                                   .width = w,
                                   .height = hh,
                                   .mip_count = mips,
                                   .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED | DE_USAGE_COPY_SRC,
                                   .name = name});
        if (!DE_HANDLE_VALID(v->texture)) {
            const de_error *e = de_error_last();
            DE_ERROR(s->log, "vis: view slot %u: HZB %ux%u: %s", vc->view_slot, w, hh,
                     e != NULL ? e->message : "?");
            return;
        }
        v->width = w;
        v->height = hh;
        v->mips = mips;
        v->src_w = vc->depth_width;
        v->src_h = vc->depth_height;
        v->access = DE_ACCESS_NONE;
        v->sampled = de_gpu_texture_bindless(s->dev, v->texture, 0, false);
    }
    vc->hzb = de_rg_import_texture(g, v->texture, v->access);
    de_rg_set_name(g, vc->hzb, "vis.hzb");
    /* Its state between frames: every frame's last access is phase 2's read. */
    de_rg_export(g, vc->hzb, DE_ACCESS_SHADER_READ);
    v->access = DE_ACCESS_SHADER_READ;
    vc->hzb_width = v->width;
    vc->hzb_height = v->height;
    vc->hzb_mips = v->mips;
    vc->hzb_bindless = v->sampled;
}

/* ---------- The stage ---------- */

void de__vis_hzb_build(de_rg *g, de__vis_context *vc) {
    de__vis_shared *s = vc->shared;
    hzb_state *st = (hzb_state *)s->stage[DE__VIS_STAGE_HZB_BUILD];
    if (st == NULL || !DE_HANDLE_VALID(vc->depth) || !DE_HANDLE_VALID(vc->hzb))
        return;
    const hzb_view *v = &st->views[vc->view_slot];
    const de__vis_hzb_target t = {
        .resource = vc->hzb, .width = v->width, .height = v->height, .mips = v->mips};
    const bool single =
        !DE_HANDLE_VALID(s->cv_hzb_single_pass) || de_cvar_bool(s->cv_hzb_single_pass);
    de__vis_hzb_add_passes(s, g, vc->depth, vc->depth_width, vc->depth_height, &t,
                           DE__VIS_HZB_OP_MIN, single);
}
