/*
 * cull_meshlets.c: GPU meshlet culling, the second stage of the visibility-buffer frame (SPEC
 * §12.5 step 1; DENG-231). shaders/culling/meshlet_cull.slang has the kernels and the tests;
 * vis_stages.h the stage contract, the C mirrors and the CPU reference functions defined here.
 *
 * Per view and frame, on the graphics queue:
 *
 *   vis.cull_meshlets.clear         first culling phase of the frame    counters: SHADER_WRITE
 *   vis.cull_meshlets[.phase2]      one group per work item, indirect   work items: READ; work
 *                                   (instance culling's arguments)      args: INDIRECT + READ;
 *                                                                       tables: READ; counters,
 *                                                                       visible list: READ + WRITE
 *   vis.cull_meshlets.args[.phase2] the phase's range and raster args   counters: READ + WRITE;
 *                                                                       raster args: SHADER_WRITE
 *   vis.cull_meshlets.stats         copies the counters into the view's counters: COPY_SRC;
 *                                   readback buffer (side effect)       readback: COPY_DST
 *
 * The visible list (de__vis_context.visible_meshlets), its counters and each phase's raster
 * arguments are transients of the view's graph. Both phases append to the one list: phase 1 from
 * entry 0, phase 2 after phase 1's last entry, so the packed vis IDs of both phases are unique.
 * A phase without work items (phase 2 until DENG-233 adds it) adds no culling pass. The stats
 * pass runs at phase 2, after both phases.
 *
 * Capacity ("clamp, count and grow", Decision page of DENG-229): the kernel stores entries below
 * the capacity and counts all. The list starts at MC_MIN_CAPACITY entries, grows to the meshlet
 * count of the visible instances' LODs that instance culling read back (an upper bound of phase
 * 1), and, when a frame read back asked for more than its list held, to the next power of two of
 * 1.25x that count, with a warning. It never shrinks (one graph key) and stops at the vis ID's
 * limit, DE__VIS_ID_MAX_VISIBLE_MESHLETS.
 *
 * Stats: each frame copies the counters into the view's READBACK ring (DE__VIS_FRAME_RING
 * entries); a later frame reads the newest completed one into de__vis_meshlet_stats.
 * de_render_stats.meshlets_visible is the sum over the frame's views, one to three frames late.
 */
#include "render/passes/vis/vis_stages.h"

#include "render/formats/de_cooked_mesh.h"
#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/** Threads per group of cs_meshlet_cull: DE__VIS_CHUNK_MESHLETS, one per meshlet of a work item. */
#define MC_GROUP DE__VIS_CHUNK_MESHLETS
/** Bytes between the frames' counters in the readback buffer. */
#define MC_STATS_STRIDE 256u
/** Visible-list capacity: at least this many entries, at most the vis ID's limit. */
#define MC_MIN_CAPACITY (1u << 16)
#define MC_MAX_CAPACITY DE__VIS_ID_MAX_VISIBLE_MESHLETS

/** The kernels' push constants (meshlet_cull.slang MeshletCullParams, 64 bytes). */
typedef struct mc_params {
    u32 cull_view, cull_view_index, instances, meshes, materials, material_count;
    u32 work_items, work_args, visible, counters, raster_args, capacity, phase, flags;
    f32 width, height;
} mc_params;
DE_STATIC_ASSERT(sizeof(mc_params) == 64, "MeshletCullParams is 64 bytes");

/** One view slot's persistent objects. */
typedef struct mc_view {
    de_view handle;
    de_gpu_buffer stats_buf; /**< READBACK: the counters of each ring frame */
    const u8 *stats_map;
    u32 stats_access;
    u64 slot_frame[DE__VIS_FRAME_RING];
    u32 slot_capacity[DE__VIS_FRAME_RING];
    bool slot_live[DE__VIS_FRAME_RING];
    de__vis_meshlet_stats stats; /**< the newest read back */
    u32 capacity;                /**< visible-list entries (grows, never shrinks) */
    bool warned_max;
} mc_view;

typedef struct mc_state {
    de__vis_kernel clear, cull, args;
    mc_view views[DE__VIEW_MAX];
    de_cvar cv_cone, cv_small;
    de__vis_meshlet_debug debug;
    /** de_render_stats.meshlets_visible: the frame the sum belongs to. */
    u64 stats_frame;
    bool stats_any;
} mc_state;

/* =================================================================================================
 * Layout constants (meshlet_cull.slang)
 * ============================================================================================== */

static const de__vis_layout_const k_consts[] = {
    {"DE_VIS_MESHLET_COUNTER_VISIBLE", DE__VIS_MESHLET_COUNTER_VISIBLE},
    {"DE_VIS_MESHLET_COUNTER_TESTED", DE__VIS_MESHLET_COUNTER_TESTED},
    {"DE_VIS_MESHLET_COUNTER_PHASE1", DE__VIS_MESHLET_COUNTER_PHASE1},
    {"DE_VIS_MESHLET_COUNTER_BYTES", DE__VIS_MESHLET_COUNTER_BYTES},
    {"DE_VIS_MESHLET_CONE", DE__VIS_MESHLET_CONE},
    {"DE_VIS_MESHLET_SMALL", DE__VIS_MESHLET_SMALL},
    {"DE_VIS_MESHLET_CASTERS_ONLY", DE__VIS_MESHLET_CASTERS_ONLY},
    {"DE_VIS_BUCKET_OPAQUE", DE__VIS_BUCKET_OPAQUE},
    {"DE_VIS_BUCKET_NONE", DE__VIS_BUCKET_NONE},
    {"DE_VIS_MESHLET_RECORD_BYTES", DE_COOKED_MESHLET_SIZE},
};

const de__vis_layout_const *de__vis_meshlet_layout_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

/* =================================================================================================
 * CPU reference (meshlet_cull.slang)
 * ============================================================================================== */

de__vis_meshlet_bounds de__vis_meshlet_bounds_load(const u8 *records, u32 meshlet) {
    const u8 *r = records + (size_t)meshlet * DE_COOKED_MESHLET_SIZE;
    de__vis_meshlet_bounds m;
    m.submesh = de_cooked_get_u32(r + 8) >> 16;
    for (u32 k = 0; k < 4; ++k) {
        m.sphere[k] = de_cooked_get_f32(r + 16 + 4 * k);
        m.apex[k] = de_cooked_get_f32(r + 32 + 4 * k);
    }
    for (u32 k = 0; k < 3; ++k)
        m.axis[k] = de_cooked_get_f32(r + 48 + 4 * k);
    return m;
}

static f32 dot3(const f32 a[3], const f32 b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static void cross3(const f32 a[3], const f32 b[3], f32 o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static f32 absf(f32 x) {
    return x < 0.0f ? -x : x;
}

void de__vis_meshlet_world_sphere(const de__gs_instance *inst, const de__vis_meshlet_bounds *m,
                                  f32 c[3], f32 *r) {
    for (u32 k = 0; k < 3; ++k) {
        const f32 *row = inst->transform[k].v;
        c[k] = row[0] * m->sphere[0] + row[1] * m->sphere[1] + row[2] * m->sphere[2] + row[3];
    }
    *r = m->sphere[3] * de__vis_max_axis_scale(inst);
}

bool de__vis_meshlet_cone_allowed(const de__gs_instance *inst, u32 submesh,
                                  const de__vis_meshlet_params *p) {
    if (submesh >= DE__GS_INSTANCE_MATERIALS || p->materials == NULL)
        return false;
    const u32 slot = inst->materials[submesh >> 2].v[submesh & 3u];
    if (slot == DE__GS_NONE || slot >= p->material_count)
        return false;
    const u32 flags = p->materials[slot].flags;
    return (flags & DE__GS_MATERIAL_READY) != 0 && (flags & DE__GS_MATERIAL_DOUBLE_SIDED) == 0;
}

/* The cone test; `slack` gets (dot(dir, axis) - cutoff * |dir|) / |dir| when it was evaluated
 * (else it is left alone). */
static bool cone_culled(const de__gs_instance *inst, const de__vis_meshlet_bounds *m,
                        const de__vis_cull_view *v, f32 *slack) {
    if (!(m->apex[3] < 1.0f))
        return false;
    const f32 r0[3] = {inst->transform[0].v[0], inst->transform[0].v[1], inst->transform[0].v[2]};
    const f32 r1[3] = {inst->transform[1].v[0], inst->transform[1].v[1], inst->transform[1].v[2]};
    const f32 r2[3] = {inst->transform[2].v[0], inst->transform[2].v[1], inst->transform[2].v[2]};
    f32 c0[3], c1[3], c2[3];
    cross3(r1, r2, c0);
    cross3(r2, r0, c1);
    cross3(r0, r1, c2);
    const f32 det = dot3(r0, c0);
    if (!(det > 1e-30f))
        return false; /* singular or mirrored */
    f32 dir[3];
    if (v->lod[1] != 0.0f) {
        const f32 *f = v->planes[4];
        for (u32 k = 0; k < 3; ++k)
            dir[k] = (c0[k] * f[0] + c1[k] * f[1] + c2[k] * f[2]) / det;
    } else {
        const f32 q[3] = {v->camera[0] - inst->transform[0].v[3],
                          v->camera[1] - inst->transform[1].v[3],
                          v->camera[2] - inst->transform[2].v[3]};
        for (u32 k = 0; k < 3; ++k)
            dir[k] = m->apex[k] - (c0[k] * q[0] + c1[k] * q[1] + c2[k] * q[2]) / det;
    }
    const f32 len2 = dot3(dir, dir);
    if (!(len2 > 0.0f))
        return false;
    const f32 len = sqrtf(len2);
    const f32 lhs = dot3(dir, m->axis), rhs = m->apex[3] * len;
    if (slack != NULL)
        *slack = (lhs - rhs) / len;
    return lhs >= rhs;
}

bool de__vis_meshlet_cone_culled(const de__gs_instance *inst, const de__vis_meshlet_bounds *m,
                                 const de__vis_cull_view *v) {
    return cone_culled(inst, m, v, NULL);
}

/* sphere_extent of meshlet_cull.slang: the sphere's extent along row `a` over w row `b`. */
static void sphere_extent(const f32 a[4], const f32 b[4], const f32 c[3], f32 r, f32 w,
                          f32 out[2]) {
    const f32 A = dot3(a, c) + a[3];
    const f32 beta = dot3(b, b), k = dot3(a, b);
    const f32 r2 = r * r;
    const f32 m[3] = {w * a[0] - A * b[0], w * a[1] - A * b[1], w * a[2] - A * b[2]};
    f32 x[3];
    cross3(a, b, x);
    const f32 dd = dot3(m, m) - r2 * dot3(x, x);
    const f32 d = r2 * (dd > 0.0f ? dd : 0.0f);
    const f32 s = sqrtf(d);
    const f32 den = w * w - r2 * beta;
    const f32 mid = A * w - r2 * k;
    out[0] = (mid - s) / den;
    out[1] = (mid + s) / den;
}

/* The near-plane condition's slack (relative) goes to `near_slack`. */
static bool screen_rect(const de__vis_cull_view *v, const f32 c[3], f32 r, f32 width, f32 height,
                        f32 rect[4], f32 *near_slack) {
    const f32 *rw = v->view_proj[3];
    const f32 w = dot3(rw, c) + rw[3];
    const f32 lhs = w - r * sqrtf(dot3(rw, rw));
    if (near_slack != NULL)
        *near_slack = (lhs - v->camera[3]) / (1.0f + absf(w) + r);
    if (!(lhs > v->camera[3]))
        return false;
    f32 ex[2], ey[2];
    sphere_extent(v->view_proj[0], rw, c, r, w, ex);
    sphere_extent(v->view_proj[1], rw, c, r, w, ey);
    rect[0] = (ex[0] * 0.5f + 0.5f) * width;
    rect[1] = (ex[1] * 0.5f + 0.5f) * width;
    rect[2] = (ey[0] * 0.5f + 0.5f) * height;
    rect[3] = (ey[1] * 0.5f + 0.5f) * height;
    return true;
}

bool de__vis_meshlet_screen_rect(const de__vis_cull_view *v, const f32 c[3], f32 r, f32 width,
                                 f32 height, f32 rect[4]) {
    return screen_rect(v, c, r, width, height, rect, NULL);
}

/* covers_centre of meshlet_cull.slang, with the rounded bounds moved by `grow` pixels (outwards
 * when positive): 0 is the shader's test, +-tol its float tolerance band. */
static bool covers_centre(f32 lo, f32 hi, f32 grow) {
    const f32 eps = DE__VIS_SMALL_EPS_PX + 1e-5f * (absf(lo) + absf(hi));
    return floorf(hi - 0.5f + eps + grow) >= ceilf(lo - 0.5f - eps - grow);
}

static bool small_culled(const de__vis_cull_view *v, const f32 c[3], f32 r, f32 width, f32 height,
                         bool *marginal) {
    f32 rect[4], near_slack = 1.0f;
    const bool in_front = screen_rect(v, c, r, width, height, rect, &near_slack);
    if (marginal != NULL && absf(near_slack) < 1e-5f)
        *marginal = true;
    if (!in_front)
        return false;
    const bool cx = covers_centre(rect[0], rect[1], 0.0f);
    const bool cy = covers_centre(rect[2], rect[3], 0.0f);
    if (marginal != NULL) {
        /* Marginal when moving the bounds by the float tolerance could change the decision. */
        const f32 tx = 2e-3f + 2e-6f * (absf(rect[0]) + absf(rect[1]));
        const f32 ty = 2e-3f + 2e-6f * (absf(rect[2]) + absf(rect[3]));
        const bool lo =
            covers_centre(rect[0], rect[1], -tx) && covers_centre(rect[2], rect[3], -ty);
        const bool hi = covers_centre(rect[0], rect[1], tx) && covers_centre(rect[2], rect[3], ty);
        if (lo != hi)
            *marginal = true;
    }
    return !cx || !cy;
}

bool de__vis_meshlet_small_culled(const de__vis_cull_view *v, const f32 c[3], f32 r, f32 width,
                                  f32 height) {
    return small_culled(v, c, r, width, height, NULL);
}

/* The frustum test; `marg` is set when the sphere is within a float tolerance of a plane it was
 * tested against. */
static bool frustum_visible(const de__vis_cull_view *v, const f32 c[3], f32 r, bool *marg) {
    for (u32 i = 0; i < DE__VIS_CULL_PLANES; ++i) {
        const f32 *pl = v->planes[i];
        const f32 dc = pl[0] * c[0] + pl[1] * c[1] + pl[2] * c[2] + pl[3];
        if (absf(dc + r) < 1e-5f * (1.0f + absf(pl[3]) + absf(dc) + r))
            *marg = true;
        if (dc < -r) /* de__vis_cull_sphere's (de_cull_sphere's) test, in the same order */
            return false;
    }
    return true;
}

static de__vis_meshlet_result meshlet_test(const de__vis_cull_view *v, const de__gs_instance *inst,
                                           const de__vis_meshlet_bounds *m,
                                           const de__vis_meshlet_params *p, bool *marg) {
    if ((inst->layer_mask & v->layer_mask) == 0 ||
        ((p->flags & DE__VIS_MESHLET_CASTERS_ONLY) != 0 &&
         (inst->flags & DE__GS_INSTANCE_CAST_SHADOWS) == 0))
        return DE__VIS_MESHLET_CULLED_INSTANCE;
    f32 c[3], r;
    de__vis_meshlet_world_sphere(inst, m, c, &r);
    if ((v->flags & DE__VIS_CULL_FRUSTUM) && !frustum_visible(v, c, r, marg))
        return DE__VIS_MESHLET_CULLED_FRUSTUM;
    if ((p->flags & DE__VIS_MESHLET_CONE) && de__vis_meshlet_cone_allowed(inst, m->submesh, p)) {
        f32 slack = 1.0f;
        const bool culled = cone_culled(inst, m, v, &slack);
        if (absf(slack) < 1e-4f)
            *marg = true;
        if (culled)
            return DE__VIS_MESHLET_CULLED_CONE;
    }
    if ((p->flags & DE__VIS_MESHLET_SMALL) && small_culled(v, c, r, p->width, p->height, marg))
        return DE__VIS_MESHLET_CULLED_SMALL;
    return DE__VIS_MESHLET_VISIBLE;
}

de__vis_meshlet_result de__vis_meshlet_test(const de__vis_cull_view *v, const de__gs_instance *inst,
                                            const de__vis_meshlet_bounds *m,
                                            const de__vis_meshlet_params *p, bool *marginal) {
    bool marg = false;
    const de__vis_meshlet_result res = meshlet_test(v, inst, m, p, &marg);
    if (marginal != NULL)
        *marginal = marg;
    return res;
}

/* =================================================================================================
 * Init / shutdown
 * ============================================================================================== */

static void view_release(de__vis_shared *s, mc_view *pv) {
    if (DE_HANDLE_VALID(pv->stats_buf))
        de_gpu_buffer_destroy(s->dev, pv->stats_buf); /* deferred until the GPU is done */
    memset(pv, 0, sizeof *pv);
}

de_result de__vis_cull_meshlets_init(de__vis_shared *s) {
    mc_state *st = DE_NEW(s->alloc, mc_state, DE_MEM_RENDER);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: meshlet culling state");
    memset(st, 0, sizeof *st);
    /* Debug toggles (not in Shipping, where both tests are on). */
    st->cv_cone = de_cvar_register(&(de_cvar_desc){
        .name = "r.cull_meshlet_cone",
        .help = "GPU culling: the backface-cone test of meshlets (meshlets whose every triangle "
                "faces away are not drawn)",
        .type = DE_CVAR_BOOL,
        .flags = DE_CVAR_DEV_ONLY,
        .default_number = 1.0});
    st->cv_small = de_cvar_register(&(de_cvar_desc){
        .name = "r.cull_small_tri",
        .help = "GPU culling: the small-meshlet test (meshlets whose screen bounds hold no pixel "
                "centre are not drawn)",
        .type = DE_CVAR_BOOL,
        .flags = DE_CVAR_DEV_ONLY,
        .default_number = 1.0});
    de_error_clear(); /* a DEV_ONLY cvar in Shipping is no error */
    static const char *const k_entries[3] = {"cs_meshlet_cull_clear", "cs_meshlet_cull",
                                             "cs_meshlet_cull_args"};
    de__engine_shader_code code;
    de_result r = de__vis_shader_load(s, "vis_meshlet_cull", k_entries, 3, &code);
    if (r == DE_OK) {
        static const u32 one[3] = {1u, 1u, 1u}, group[3] = {MC_GROUP, 1u, 1u};
        const u32 pb = (u32)sizeof(mc_params);
        r = de__vis_kernel_create(s, &code, 0, "vis cull meshlets clear", one, pb, &st->clear);
        if (r == DE_OK)
            r = de__vis_kernel_create(s, &code, 1, "vis cull meshlets", group, pb, &st->cull);
        if (r == DE_OK)
            r = de__vis_kernel_create(s, &code, 2, "vis cull meshlets args", one, pb, &st->args);
        de__engine_shader_code_free(&code);
    }
    if (r != DE_OK) {
        de__vis_kernel_destroy(s, &st->clear);
        de__vis_kernel_destroy(s, &st->cull);
        de__vis_kernel_destroy(s, &st->args);
        DE_FREE(s->alloc, st, sizeof *st);
        return r;
    }
    s->stage[DE__VIS_STAGE_CULL_MESHLETS] = st;
    return DE_OK;
}

void de__vis_cull_meshlets_shutdown(de__vis_shared *s) {
    mc_state *st = (mc_state *)s->stage[DE__VIS_STAGE_CULL_MESHLETS];
    if (st == NULL)
        return;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        view_release(s, &st->views[i]);
    de__vis_kernel_destroy(s, &st->clear);
    de__vis_kernel_destroy(s, &st->cull);
    de__vis_kernel_destroy(s, &st->args);
    DE_FREE(s->alloc, st, sizeof *st);
    s->stage[DE__VIS_STAGE_CULL_MESHLETS] = NULL;
}

de__vis_meshlet_stats de__vis_view_meshlet_stats(const de__vis_shared *s, u32 slot) {
    const mc_state *st = s != NULL ? (const mc_state *)s->stage[DE__VIS_STAGE_CULL_MESHLETS] : NULL;
    if (st == NULL || slot >= DE__VIEW_MAX)
        return (de__vis_meshlet_stats){0};
    return st->views[slot].stats;
}

void de__vis_meshlet_set_debug(de__vis_shared *s, const de__vis_meshlet_debug *d) {
    mc_state *st = s != NULL ? (mc_state *)s->stage[DE__VIS_STAGE_CULL_MESHLETS] : NULL;
    if (st != NULL)
        st->debug = d != NULL ? *d : (de__vis_meshlet_debug){0};
}

/* =================================================================================================
 * One view
 * ============================================================================================== */

static bool view_ensure(de__vis_shared *s, mc_view *pv, u32 slot) {
    if (DE_HANDLE_VALID(pv->stats_buf))
        return true;
    char name[48];
    snprintf(name, sizeof name, "vis meshlet stats %u", slot);
    pv->stats_buf = de_gpu_buffer_create(
        s->dev, &(de_gpu_buffer_desc){.size = (u64)DE__VIS_FRAME_RING * MC_STATS_STRIDE,
                                      .usage = DE_USAGE_COPY_DST,
                                      .memory = DE_MEM_READBACK,
                                      .name = name});
    pv->stats_map = DE_HANDLE_VALID(pv->stats_buf)
                        ? (const u8 *)de_gpu_buffer_map(s->dev, pv->stats_buf)
                        : NULL;
    if (pv->stats_map == NULL) {
        const de_error *e = de_error_last();
        DE_ERROR(s->log, "vis: view slot %u: meshlet stats buffer: %s", slot,
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
static void read_stats(mc_view *pv, u64 completed) {
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
    memcpy(c, pv->stats_map + (size_t)best * MC_STATS_STRIDE, sizeof c);
    pv->stats = (de__vis_meshlet_stats){.valid = true,
                                        .frame = pv->slot_frame[best],
                                        .visible = c[DE__VIS_MESHLET_COUNTER_VISIBLE / 4u],
                                        .tested = c[DE__VIS_MESHLET_COUNTER_TESTED / 4u],
                                        .phase1 = c[DE__VIS_MESHLET_COUNTER_PHASE1 / 4u],
                                        .capacity = pv->slot_capacity[best]};
}

static u32 pow2_at_least(u64 v) {
    u64 p = 1;
    while (p < v && p < MC_MAX_CAPACITY)
        p <<= 1;
    return (u32)(p < MC_MAX_CAPACITY ? p : MC_MAX_CAPACITY);
}

/* The visible list's capacity this frame (header). */
static u32 visible_capacity(de__vis_shared *s, mc_state *st, mc_view *pv,
                            const de__vis_context *vc) {
    if (st->debug.capacity_fixed != 0)
        return st->debug.capacity_fixed < MC_MAX_CAPACITY ? st->debug.capacity_fixed
                                                          : MC_MAX_CAPACITY;
    u32 cap = pv->capacity > MC_MIN_CAPACITY ? pv->capacity : MC_MIN_CAPACITY;
    if (vc->stats.valid && vc->stats.meshlets > cap)
        cap = pow2_at_least(vc->stats.meshlets);
    const de__vis_meshlet_stats *ms = &pv->stats;
    if (ms->valid && ms->visible > ms->capacity) {
        const u32 want = pow2_at_least((u64)ms->visible + ms->visible / 4u);
        if (want > cap) {
            DE_WARN(s->log,
                    "vis: view slot %u: the visible-meshlet list overflowed in frame %llu (%u "
                    "visible meshlets for %u entries; the rest were not drawn); it grows to %u",
                    vc->view_slot, (unsigned long long)ms->frame, ms->visible, ms->capacity, want);
            cap = want;
        } else if (cap == MC_MAX_CAPACITY && !pv->warned_max) {
            DE_WARN(s->log,
                    "vis: view slot %u: %u visible meshlets do not fit the largest list (%u): "
                    "the rest are not drawn",
                    vc->view_slot, ms->visible, MC_MAX_CAPACITY);
            pv->warned_max = true;
        }
    }
    pv->capacity = cap;
    return cap;
}

/* de_render_stats.meshlets_visible: the sum over this frame's views of what each read back. */
static void publish_stats(de__vis_shared *s, mc_state *st, const de__vis_context *vc) {
    if (s->renderer == NULL)
        return;
    de_render_stats *rs = &s->renderer->stats;
    if (!st->stats_any || st->stats_frame != vc->frame_index) {
        st->stats_any = true;
        st->stats_frame = vc->frame_index;
        rs->meshlets_visible = 0;
    }
    if (vc->meshlet_stats.valid)
        rs->meshlets_visible += vc->meshlet_stats.visible;
}

/* ---------- Passes ---------- */

typedef struct mc_pass {
    de_gpu_device *dev;
    de__vis_kernel kernel;
    mc_params pc;
    de_rg_resource instances, meshes, materials, work_items, work_args, visible, counters,
        raster_args;
    de_gpu_buffer stats;
    u64 stats_offset;
} mc_pass;

static u32 bindless_of(const mc_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return DE_HANDLE_VALID(r) ? de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, r)) : 0u;
}

static void set_kernel(de_gpu_cmd *cmd, const mc_pass *p, const mc_params *pc) {
    de_gpu_cmd_set_pipeline(cmd, p->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, pc, (u32)sizeof *pc);
}

static void clear_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const mc_pass *p = (const mc_pass *)user;
    mc_params pc = p->pc;
    pc.counters = bindless_of(p, pass, p->counters);
    set_kernel(cmd, p, &pc);
    de_gpu_cmd_dispatch(cmd, 1, 1, 1);
}

static void cull_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const mc_pass *p = (const mc_pass *)user;
    mc_params pc = p->pc;
    pc.instances = bindless_of(p, pass, p->instances);
    pc.meshes = bindless_of(p, pass, p->meshes);
    pc.materials = bindless_of(p, pass, p->materials);
    pc.work_items = bindless_of(p, pass, p->work_items);
    pc.work_args = bindless_of(p, pass, p->work_args);
    pc.visible = bindless_of(p, pass, p->visible);
    pc.counters = bindless_of(p, pass, p->counters);
    set_kernel(cmd, p, &pc);
    de_gpu_cmd_dispatch_indirect(cmd, de_rg_buffer(pass, p->work_args), 0);
}

static void args_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const mc_pass *p = (const mc_pass *)user;
    mc_params pc = p->pc;
    pc.counters = bindless_of(p, pass, p->counters);
    pc.raster_args = bindless_of(p, pass, p->raster_args);
    set_kernel(cmd, p, &pc);
    de_gpu_cmd_dispatch(cmd, 1, 1, 1);
}

static void stats_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const mc_pass *p = (const mc_pass *)user;
    de_gpu_cmd_copy_buffer(cmd, p->stats, p->stats_offset, de_rg_buffer(pass, p->counters), 0,
                           DE__VIS_MESHLET_COUNTER_BYTES);
}

static mc_pass *pass_data(de__vis_context *vc, const de__vis_kernel *k, const mc_pass *base) {
    mc_pass *p = (mc_pass *)de_arena_push(vc->arena, sizeof *p, 16);
    if (p != NULL) {
        *p = *base;
        if (k != NULL)
            p->kernel = *k;
    }
    return p;
}

static bool arena_full(void) {
    (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: meshlet culling: the frame arena is full");
    return false;
}

/* The copy of the counters into the view's readback ring (after both phases). */
static void add_stats_pass(de_rg *g, de__vis_context *vc, mc_view *pv) {
    if (!DE_HANDLE_VALID(vc->meshlet_counters) || !DE_HANDLE_VALID(pv->stats_buf))
        return;
    const u32 slot = (u32)(vc->frame_index % DE__VIS_FRAME_RING);
    const mc_pass base = {.dev = vc->shared->dev,
                          .counters = vc->meshlet_counters,
                          .stats = pv->stats_buf,
                          .stats_offset = (u64)slot * MC_STATS_STRIDE};
    mc_pass *ps = pass_data(vc, NULL, &base);
    if (ps == NULL) {
        arena_full();
        return;
    }
    de_rg_pass *p =
        de_rg_add_pass(g, "vis.cull_meshlets.stats", DE_QUEUE_GRAPHICS, stats_execute, ps);
    de_rg_pass_side_effect(p);
    de_rg_read(p, vc->meshlet_counters, DE_ACCESS_COPY_SRC);
    const de_rg_resource rb = de_rg_import_buffer(g, pv->stats_buf, pv->stats_access);
    de_rg_set_name(g, rb, "vis.meshlet_stats");
    de_rg_write(p, rb, DE_ACCESS_COPY_DST);
    pv->stats_access = DE_ACCESS_COPY_DST; /* the epilogue leaves it in its last access */
    pv->slot_frame[slot] = vc->frame_index;
    pv->slot_capacity[slot] = vc->visible_capacity;
    pv->slot_live[slot] = true;
}

void de__vis_cull_meshlets(de_rg *g, de__vis_context *vc, de__vis_phase phase) {
    de__vis_shared *s = vc->shared;
    mc_state *st = (mc_state *)s->stage[DE__VIS_STAGE_CULL_MESHLETS];
    if (st == NULL || vc->view_slot >= DE__VIEW_MAX)
        return;
    mc_view *pv = &st->views[vc->view_slot];
    const u32 pi = DE__VIS_PHASE_INDEX(phase);
    if (phase == DE__VIS_PHASE_1) {
        if (vc->view_new || pv->handle.id != vc->view.id) {
            view_release(s, pv);
            pv->handle = vc->view;
        }
        if (DE_HANDLE_VALID(pv->stats_buf))
            read_stats(pv, vc->frames_completed);
        vc->meshlet_stats = pv->stats;
        vc->casters_only = vc->casters_only || st->debug.casters_only;
        publish_stats(s, st, vc);
    }
    const bool cull = DE_HANDLE_VALID(vc->work_items[pi]) && DE_HANDLE_VALID(vc->cull_args[pi]) &&
                      DE_HANDLE_VALID(vc->scene_instances) && DE_HANDLE_VALID(vc->scene_meshes) &&
                      vc->cull_view_bindless != 0;
    if (!cull || !view_ensure(s, pv, vc->view_slot)) {
        if (phase == DE__VIS_PHASE_2)
            add_stats_pass(g, vc, pv);
        return;
    }

    /* The first culling phase of the frame makes the list and the counters and clears them. */
    const bool first = !DE_HANDLE_VALID(vc->meshlet_counters);
    if (first) {
        const u32 cap = visible_capacity(s, st, pv, vc);
        const de_rg_resource list = de_rg_create_buffer(
            g, &(de_gpu_buffer_desc){.size = (u64)cap * sizeof(de__vis_meshlet),
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                     .stride = (u32)sizeof(de__vis_meshlet),
                                     .name = "vis.visible_meshlets"});
        const de_rg_resource counters = de_rg_create_buffer(
            g, &(de_gpu_buffer_desc){.size = DE__VIS_MESHLET_COUNTER_BYTES,
                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                     .name = "vis.meshlet_counters"});
        if (!DE_HANDLE_VALID(list) || !DE_HANDLE_VALID(counters))
            return; /* error set; finalize reports it */
        vc->visible_capacity = cap;
        vc->visible_meshlets = list;
        vc->meshlet_counters = counters;
    }
    const de_rg_resource raster_args = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = sizeof(de__vis_raster_args),
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT | DE_USAGE_COPY_SRC,
                                 .stride = (u32)sizeof(de__vis_raster_args),
                                 .name = phase == DE__VIS_PHASE_1 ? "vis.raster_args"
                                                                  : "vis.raster_args.phase2"});
    if (!DE_HANDLE_VALID(raster_args))
        return;

    u32 flags = 0;
    if (!vc->cull_disable) {
        if (!DE_HANDLE_VALID(st->cv_cone) || de_cvar_bool(st->cv_cone))
            flags |= DE__VIS_MESHLET_CONE;
        if (!DE_HANDLE_VALID(st->cv_small) || de_cvar_bool(st->cv_small))
            flags |= DE__VIS_MESHLET_SMALL;
    }
    if (vc->casters_only)
        flags |= DE__VIS_MESHLET_CASTERS_ONLY;
    const bool materials = DE_HANDLE_VALID(vc->scene_materials) && vc->gpu_scene != NULL;
    const mc_pass base = {
        .dev = s->dev,
        .pc = {.cull_view = vc->cull_view_bindless,
               .cull_view_index = vc->cull_view_index,
               .material_count =
                   materials ? de__gpu_scene_table(vc->gpu_scene, DE__GS_MATERIALS).count : 0u,
               .capacity = vc->visible_capacity,
               .phase = (u32)phase,
               .flags = flags,
               .width = (f32)vc->width,
               .height = (f32)vc->height},
        .instances = vc->scene_instances,
        .meshes = vc->scene_meshes,
        .materials = materials ? vc->scene_materials : (de_rg_resource){0},
        .work_items = vc->work_items[pi],
        .work_args = vc->cull_args[pi],
        .visible = vc->visible_meshlets,
        .counters = vc->meshlet_counters,
        .raster_args = raster_args};
    mc_pass *pc = first ? pass_data(vc, &st->clear, &base) : NULL;
    mc_pass *pu = pass_data(vc, &st->cull, &base), *pa = pass_data(vc, &st->args, &base);
    if ((first && pc == NULL) || pu == NULL || pa == NULL) {
        arena_full();
        return;
    }
    de_rg_pass *p;
    if (first) {
        p = de_rg_add_pass(g, "vis.cull_meshlets.clear", DE_QUEUE_GRAPHICS, clear_execute, pc);
        de_rg_write(p, vc->meshlet_counters, DE_ACCESS_SHADER_WRITE);
    }

    const bool p1 = phase == DE__VIS_PHASE_1;
    p = de_rg_add_pass(g, p1 ? "vis.cull_meshlets" : "vis.cull_meshlets.phase2", DE_QUEUE_GRAPHICS,
                       cull_execute, pu);
    de_rg_read(p, vc->scene_instances, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_meshes, DE_ACCESS_SHADER_READ);
    if (materials)
        de_rg_read(p, vc->scene_materials, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->work_items[pi], DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->cull_args[pi], DE_ACCESS_INDIRECT | DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->meshlet_counters, DE_ACCESS_SHADER_READ); /* atomics */
    de_rg_write(p, vc->meshlet_counters, DE_ACCESS_SHADER_WRITE);
    if (!first) /* keep the earlier phase's entries */
        de_rg_read(p, vc->visible_meshlets, DE_ACCESS_SHADER_READ);
    de_rg_write(p, vc->visible_meshlets, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, p1 ? "vis.cull_meshlets.args" : "vis.cull_meshlets.args.phase2",
                       DE_QUEUE_GRAPHICS, args_execute, pa);
    de_rg_read(p, vc->meshlet_counters, DE_ACCESS_SHADER_READ);
    de_rg_write(p, vc->meshlet_counters, DE_ACCESS_SHADER_WRITE); /* phase 1 stores its end */
    de_rg_write(p, raster_args, DE_ACCESS_SHADER_WRITE);
    vc->raster_args[pi] = raster_args;

    if (phase == DE__VIS_PHASE_2)
        add_stats_pass(g, vc, pv);
}
