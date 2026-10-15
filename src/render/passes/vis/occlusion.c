/*
 * occlusion.c: two-phase occlusion culling per view (SPEC §12.5 step 1; DENG-233). The test is
 * cull_occlusion_test in shaders/culling/cull_common.slang (shaders/culling/occlusion.slang has
 * the HZB lookup); the phases are in instance_cull.slang; the HZB is hzb.c's.
 *
 * Per view slot (split-screen views never share history):
 *   - the visibility flags: a GPU_ONLY raw buffer, one u32 per instance slot, 1 when the instance
 *     was visible after the last frame's phase 2. Phase 1 draws those (frustum-tested with the
 *     current camera); phase 2 tests every frustum-visible instance against this frame's HZB,
 *     draws what phase 1 did not and rewrites the flags of slots [0, instance_count). The buffer
 *     follows the GPU scene's instance capacity: when it grows, a new buffer takes the old one's
 *     written flags ("vis.visibility_grow", a copy) and the old one is destroyed in the next frame
 *     (after this frame's submit).
 *   - `written`: the slots the last frame's phase 2 wrote. Slots past it (instances added since)
 *     count as not visible last frame, so a new instance is tested in phase 2.
 *   - cut detection: a new view in the slot, r.occlusion_reset (one-shot: it clears itself, and
 *     applies to every view of that frame), or a
 *     camera move of more than r.occlusion_cut_m metres (world space) or a turn of more than 45
 *     degrees since the last frame drops the history for one frame: phase 1 draws nothing (the
 *     HZB is then built from the cleared depth, every test passes) and phase 2 draws everything
 *     in the frustum.
 * r.occlusion 0: phase 1 draws every frustum-visible instance and phase 2 none (the flags are
 * still written, so turning it on again starts from a valid history).
 *
 * r.hzb_debug N >= 0: de__vis_hzb_debug draws HZB mip N into the view's colour (vis_frame.c).
 */
#include "render/passes/vis/vis_stages.h"

#include "render/graph/rg_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/** Smallest visibility buffer, in instance slots. */
#define OCC_MIN_SLOTS 1024u
/** A turn of more than this (cosine of 45 degrees) between frames is a camera cut. */
#define OCC_CUT_COS 0.70710678f

typedef struct occ_view {
    de_view handle;
    de_gpu_buffer flags; /**< u32 per slot */
    u32 capacity;        /**< slots */
    u32 bindless;
    u32 access;            /**< its access between frames */
    u32 written;           /**< slots the last frame's phase 2 wrote */
    de_gpu_buffer retired; /**< replaced (growth, new view): destroyed in a later frame */
    u64 retired_frame;
    bool has_camera;
    de_dvec3 camera; /**< world position */
    de_vec3 forward; /**< world direction */
} occ_view;

typedef struct occ_state {
    occ_view views[DE__VIEW_MAX];
    de__vis_kernel debug;
    /* r.occlusion_reset applies to every view of the frame it was seen in. */
    bool reset_pending;
    u64 reset_frame;
} occ_state;

/** occlusion.slang HzbDebugParams. */
typedef struct occ_debug_params {
    u32 hzb, mip, dst, width, height, hzb_width, hzb_height;
} occ_debug_params;

static void view_release(de__vis_shared *s, occ_view *v) {
    if (DE_HANDLE_VALID(v->flags))
        de_gpu_buffer_destroy(s->dev, v->flags); /* deferred until the GPU is done */
    if (DE_HANDLE_VALID(v->retired))
        de_gpu_buffer_destroy(s->dev, v->retired);
    memset(v, 0, sizeof *v);
}

de_result de__vis_occlusion_init(de__vis_shared *s) {
    occ_state *st = DE_NEW(s->alloc, occ_state, DE_MEM_RENDER);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: occlusion state");
    memset(st, 0, sizeof *st);
    static const char *const k_entries[1] = {"cs_hzb_debug"};
    de__engine_shader_code code;
    de_result r = de__vis_shader_load(s, "vis_occlusion", k_entries, 1, &code);
    if (r == DE_OK) {
        static const u32 g8[3] = {8u, 8u, 1u};
        r = de__vis_kernel_create(s, &code, 0, "vis hzb debug", g8, (u32)sizeof(occ_debug_params),
                                  &st->debug);
        de__engine_shader_code_free(&code);
    }
    if (r != DE_OK) {
        DE_FREE(s->alloc, st, sizeof *st);
        return r;
    }
    s->occlusion = st;
    return DE_OK;
}

void de__vis_occlusion_shutdown(de__vis_shared *s) {
    occ_state *st = (occ_state *)s->occlusion;
    if (st == NULL)
        return;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        view_release(s, &st->views[i]);
    de__vis_kernel_destroy(s, &st->debug);
    DE_FREE(s->alloc, st, sizeof *st);
    s->occlusion = NULL;
}

/* ---------- Cut detection ---------- */

/* The camera's world forward: view space looks down -Z, so it is minus row 2 of the view matrix
 * (element (row r, column c) is m[c * 4 + r]). */
static de_vec3 forward_of(const de_mat4 *view) {
    de_vec3 f = {-view->m[2], -view->m[6], -view->m[10]};
    const f32 len = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
    if (len > 0.0f) {
        f.x /= len;
        f.y /= len;
        f.z /= len;
    }
    return f;
}

static bool camera_cut(de__vis_shared *s, occ_view *v, const de__vis_context *vc) {
    const de_dvec3 cam = {vc->origin.x + (f64)vc->camera_position.x,
                          vc->origin.y + (f64)vc->camera_position.y,
                          vc->origin.z + (f64)vc->camera_position.z};
    const de_vec3 fwd = forward_of(&vc->view_matrix);
    bool cut = !v->has_camera;
    if (v->has_camera) {
        const f64 dx = cam.x - v->camera.x, dy = cam.y - v->camera.y, dz = cam.z - v->camera.z;
        const f64 limit =
            DE_HANDLE_VALID(s->cv_occlusion_cut_m) ? de_cvar_float(s->cv_occlusion_cut_m) : 10.0;
        if (dx * dx + dy * dy + dz * dz > limit * limit)
            cut = true;
        if (fwd.x * v->forward.x + fwd.y * v->forward.y + fwd.z * v->forward.z < OCC_CUT_COS)
            cut = true;
    }
    v->has_camera = true;
    v->camera = cam;
    v->forward = fwd;
    return cut;
}

/* ---------- Visibility flags ---------- */

typedef struct grow_pass {
    de_rg_resource from, to;
    u64 bytes;
} grow_pass;

static void grow_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const grow_pass *p = (const grow_pass *)user;
    de_gpu_cmd_copy_buffer(cmd, de_rg_buffer(pass, p->to), 0, de_rg_buffer(pass, p->from), 0,
                           p->bytes);
}

static u32 pow2_at_least(u32 v) {
    u32 p = OCC_MIN_SLOTS;
    while (p < v && p < (1u << 30))
        p <<= 1u;
    return p;
}

/* Makes the flags hold `slots` (the GPU scene's instance capacity); copies the history over. */
static bool ensure_flags(de__vis_shared *s, de_rg *g, occ_view *v, u32 slots, u32 slot, u64 frame) {
    if (DE_HANDLE_VALID(v->retired) && frame > v->retired_frame) { /* its last use submitted */
        de_gpu_buffer_destroy(s->dev, v->retired);
        v->retired = (de_gpu_buffer){0};
    }
    if (DE_HANDLE_VALID(v->flags) && v->capacity >= slots)
        return true;
    const u32 cap = pow2_at_least(slots);
    char name[48];
    snprintf(name, sizeof name, "vis visibility %u", slot);
    const de_gpu_buffer b = de_gpu_buffer_create(
        s->dev,
        &(de_gpu_buffer_desc){.size = (u64)cap * 4u,
                              .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC | DE_USAGE_COPY_DST,
                              .name = name});
    if (!DE_HANDLE_VALID(b)) {
        const de_error *e = de_error_last();
        DE_ERROR(s->log, "vis: view slot %u: visibility flags (%u slots): %s", slot, cap,
                 e != NULL ? e->message : "?");
        return false;
    }
    const de_gpu_buffer old = v->flags;
    grow_pass *p = NULL;
    if (DE_HANDLE_VALID(old) && v->written > 0)
        p = (grow_pass *)de_arena_push_zero(g->arena, sizeof *p, 16);
    if (p != NULL) {
        /* The history moves over; the new buffer is imported again (same handle) by the caller. */
        p->from = de_rg_import_buffer(g, old, v->access);
        p->to = de_rg_import_buffer(g, b, DE_ACCESS_NONE);
        p->bytes = (u64)v->written * 4u;
        de_rg_pass *rp =
            de_rg_add_pass(g, "vis.visibility_grow", DE_QUEUE_GRAPHICS, grow_execute, p);
        de_rg_read(rp, p->from, DE_ACCESS_COPY_SRC);
        de_rg_write(rp, p->to, DE_ACCESS_COPY_DST);
    } else {
        v->written = 0; /* nothing to keep (or no room for the copy): no history */
    }
    if (DE_HANDLE_VALID(old)) {
        /* Destroyed in a later frame, after this frame's copy was submitted. One retired buffer
         * is kept: an older one (two growths in a row) goes now; its frames were submitted. */
        if (DE_HANDLE_VALID(v->retired))
            de_gpu_buffer_destroy(s->dev, v->retired);
        v->retired = old;
        v->retired_frame = frame;
    }
    v->access = DE_ACCESS_NONE;
    v->flags = b;
    v->capacity = cap;
    v->bindless = de_gpu_buffer_bindless(s->dev, b);
    return true;
}

void de__vis_occlusion_prepare(de_rg *g, de__vis_context *vc) {
    de__vis_shared *s = vc->shared;
    occ_state *st = (occ_state *)s->occlusion;
    vc->occlusion = !DE_HANDLE_VALID(s->cv_occlusion) || de_cvar_bool(s->cv_occlusion);
    if (st == NULL || vc->view_slot >= DE__VIEW_MAX || vc->instance_count == 0)
        return;
    occ_view *v = &st->views[vc->view_slot];
    if (v->handle.id != vc->view.id || vc->view_new) {
        /* A new view: no history. Its flags buffer is destroyed in the next frame, as after a
         * growth (frames in flight may still use it). */
        if (DE_HANDLE_VALID(v->retired))
            de_gpu_buffer_destroy(s->dev, v->retired);
        const de_gpu_buffer old = v->flags;
        memset(v, 0, sizeof *v);
        v->retired = old;
        v->retired_frame = vc->frame_index;
        v->handle = vc->view;
    }
    bool reset = camera_cut(s, v, vc);
    if (DE_HANDLE_VALID(s->cv_occlusion_reset) && de_cvar_bool(s->cv_occlusion_reset)) {
        (void)de_cvar_set(s->cv_occlusion_reset, "0"); /* one-shot ... */
        st->reset_pending = true;                      /* ... for every view of this frame */
        st->reset_frame = vc->frame_index;
    }
    if (st->reset_pending && st->reset_frame == vc->frame_index)
        reset = true;
    const de__gpu_scene_table_info ti = de__gpu_scene_table(vc->gpu_scene, DE__GS_INSTANCES);
    const u32 slots = ti.capacity > vc->instance_count ? ti.capacity : vc->instance_count;
    if (!ensure_flags(s, g, v, slots, vc->view_slot, vc->frame_index))
        return;
    vc->visibility = de_rg_import_buffer(g, v->flags, v->access);
    de_rg_set_name(g, vc->visibility, "vis.visibility");
    de_rg_export(g, vc->visibility, DE_ACCESS_SHADER_READ);
    v->access = DE_ACCESS_SHADER_READ;
    vc->visibility_bindless = v->bindless;
    vc->occlusion_reset = reset;
    vc->visibility_count = reset ? 0u : v->written;
    /* Phase 2 writes [0, instance_count) this frame. */
    v->written = vc->instance_count;
}

/* ---------- r.hzb_debug ---------- */

typedef struct debug_pass {
    de_gpu_device *dev;
    de__vis_kernel kernel;
    occ_debug_params pc;
    de_rg_resource dst;
    u32 groups[2];
} debug_pass;

static void debug_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const debug_pass *p = (const debug_pass *)user;
    occ_debug_params pc = p->pc;
    pc.dst = de_gpu_texture_bindless(p->dev, de_rg_texture(pass, p->dst), 0, true);
    de_gpu_cmd_set_pipeline(cmd, p->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, p->groups[0], p->groups[1], 1);
}

de_rg_resource de__vis_hzb_debug(de_rg *g, de__vis_context *vc, u32 mip) {
    de__vis_shared *s = vc->shared;
    occ_state *st = (occ_state *)s->occlusion;
    if (st == NULL || !DE_HANDLE_VALID(vc->hzb) || vc->width == 0 || vc->height == 0)
        return (de_rg_resource){0};
    const de_rg_resource dst =
        de_rg_create_texture(g, &(de_gpu_texture_desc){.format = DE_FMT_RGBA16_FLOAT,
                                                       .width = vc->width,
                                                       .height = vc->height,
                                                       .usage = DE_USAGE_STORAGE | DE_USAGE_SAMPLED,
                                                       .name = "vis.hzb_debug"});
    debug_pass *p = (debug_pass *)de_arena_push_zero(g->arena, sizeof *p, 16);
    if (!DE_HANDLE_VALID(dst) || p == NULL)
        return (de_rg_resource){0};
    p->dev = s->dev;
    p->kernel = st->debug;
    p->dst = dst;
    p->pc = (occ_debug_params){.hzb = vc->hzb_bindless,
                               .mip = mip < vc->hzb_mips ? mip : vc->hzb_mips - 1u,
                               .width = vc->width,
                               .height = vc->height,
                               .hzb_width = vc->hzb_width,
                               .hzb_height = vc->hzb_height};
    p->groups[0] = (vc->width + 7u) / 8u;
    p->groups[1] = (vc->height + 7u) / 8u;
    de_rg_pass *rp = de_rg_add_pass(g, "vis.hzb_debug", DE_QUEUE_GRAPHICS, debug_execute, p);
    de_rg_read(rp, vc->hzb, DE_ACCESS_SHADER_READ);
    de_rg_write(rp, dst, DE_ACCESS_SHADER_WRITE);
    return dst;
}
