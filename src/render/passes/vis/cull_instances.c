/*
 * cull_instances.c: GPU instance culling, the first stage of the visibility-buffer frame (SPEC
 * §12.5 step 1, §11.8 LOD; DENG-229). shaders/culling/instance_cull.slang has the kernels and
 * the LOD rule; vis_stages.h the stage contract and the CPU reference functions defined here.
 *
 * Per view and phase, three passes on the graphics queue, then one stats pass after phase 2:
 *
 *   vis.cull_instances[2].clear  zeroes the counters                counters: SHADER_WRITE
 *   vis.cull_instances[2]        one thread per instance slot       instances, meshes: READ;
 *                                                                   counters: READ + WRITE;
 *                                                                   work items: SHADER_WRITE;
 *                                                                   visibility: READ (phase 1),
 *                                                                   READ + WRITE (phase 2);
 *                                                                   HZB: READ (phase 2)
 *   vis.cull_instances[2].args   meshlet culling's dispatch args    counters: READ; args: WRITE
 *   vis.cull_instances.stats     both phases' counters into the     counters: COPY_SRC;
 *                                view's readback buffer (side       readback: COPY_DST
 *                                effect)
 *
 * Two-phase occlusion (DENG-233, instance_cull.slang; occlusion.c has the per-view visibility
 * flags): phase 1 draws what was visible after the last frame, phase 2 (after the HZB) tests
 * every frustum-visible instance, draws the newly visible ones and rewrites the flags. Both
 * phases use the same cull constants (written in phase 1). Each phase has its own work list,
 * counters and arguments (de__vis_context.work_items / instance_counters / cull_args, indexed by
 * DE__VIS_PHASE_INDEX). With every later stage stubbed nothing reads the arguments and the graph
 * culls the args passes; the stats pass and the visibility flags keep the clears and the culls.
 *
 * Per view slot: an UPLOAD buffer with DE__VIS_FRAME_RING de__vis_cull_view entries (the CPU
 * writes frame N's entry N % ring while building the graph; the GPU reads it with de_robuffer)
 * and a READBACK buffer with the counters of the same frames (phase 1's, then phase 2's, 16 bytes
 * each). A frame reads back the newest
 * completed frame's counters (de__vis_stats; de_render_stats.instances_visible), and grows the
 * work list when that frame asked for more items than it held: items past the capacity are
 * dropped by the kernel and counted, so the next frames recover, with a warning.
 *
 * Culling queue: graphics. The tables are graphics-owned between graphs, so async compute would
 * add a queue transfer for each; it can move to DE_QUEUE_COMPUTE once there is work to overlap.
 */
#include "render/passes/vis/vis_stages.h"

#include "render/graph/rg_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/** Threads per group of cs_instance_cull. */
#define CI_GROUP 64u
/** Bytes between the frames' counters in the readback buffer. */
#define CI_STATS_STRIDE 256u
/** Work-list capacity: at least this many items, at most CI_MAX_CAPACITY (64 MiB). */
#define CI_MIN_CAPACITY 4096u
#define CI_MAX_CAPACITY (1u << 22)
/** Instance slots one dispatch covers. */
#define CI_MAX_INSTANCES (DE__VIS_MAX_GROUPS_X * CI_GROUP)

/** The kernels' push constants (instance_cull.slang InstanceCullParams). */
typedef struct ci_params {
    u32 cull_view, cull_view_index, instances, meshes, work_items, counters, args, phase;
} ci_params;

/** One view slot's persistent objects. */
typedef struct ci_view {
    de_view handle;
    de_gpu_buffer cull_view; /**< UPLOAD: DE__VIS_FRAME_RING de__vis_cull_view entries */
    u8 *cull_map;
    u32 cull_bindless;
    de_gpu_buffer stats_buf; /**< READBACK: the counters of each ring frame */
    const u8 *stats_map;
    u32 stats_access; /**< its access between frames */
    u64 slot_frame[DE__VIS_FRAME_RING];
    u32 slot_capacity[DE__VIS_FRAME_RING];
    bool slot_live[DE__VIS_FRAME_RING];
    bool slot_single_phase[DE__VIS_FRAME_RING];
    de__vis_stats stats; /**< the newest read back */
    u32 capacity;        /**< work items (grows, never shrinks) */
    bool warned_max;
    /* DENG-233: phase 1 of this frame, for phase 2 */
    u32 frame_slot, frame_count, frame_capacity;
    bool frame_ready;
    /* r.cull_freeze */
    bool frozen;
    de__vis_cull_view frozen_view;
    de_dvec3 frozen_origin;
} ci_view;

typedef struct ci_state {
    de__vis_kernel clear, cull, args;
    ci_view views[DE__VIEW_MAX];
} ci_state;

/* =================================================================================================
 * CPU reference (instance_cull.slang, cull_common.slang)
 * ============================================================================================== */

void de__vis_cull_view_init(de__vis_cull_view *v, const de__vis_cull_camera *cam) {
    memset(v, 0, sizeof *v);
    const de_frustum f = de_frustum_from_m4(&cam->view_proj);
    for (u32 i = 0; i < DE__VIS_CULL_PLANES; ++i) {
        v->planes[i][0] = f.planes[i].n.x;
        v->planes[i][1] = f.planes[i].n.y;
        v->planes[i][2] = f.planes[i].n.z;
        v->planes[i][3] = f.planes[i].d;
    }
    /* de_m4_perspective_rz: element (3, 3) is 0 and (2, 3) the near distance; orthographic
     * projections have (3, 3) = 1. Element (row r, column c) is m[c * 4 + r]. */
    const bool ortho = cam->proj.m[15] != 0.0f;
    v->camera[0] = cam->position.x;
    v->camera[1] = cam->position.y;
    v->camera[2] = cam->position.z;
    v->camera[3] = ortho ? 0.0f : cam->proj.m[14];
    const f32 err = cam->lod_error_px > 0.0f ? cam->lod_error_px : 1.0f;
    v->lod[0] = 0.5f * (f32)cam->height * cam->proj.m[5] / err;
    v->lod[1] = ortho ? 1.0f : 0.0f;
    v->lod[2] = (f32)cam->lod_bias;
    for (u32 r = 0; r < 4; ++r)
        for (u32 c = 0; c < 4; ++c)
            v->view_proj[r][c] = cam->view_proj.m[c * 4u + r];
    v->flags = DE__VIS_CULL_FRUSTUM;
}

void de__vis_cull_view_rebase(de__vis_cull_view *v, de_dvec3 from, de_dvec3 to) {
    /* A point p relative to `to` is p + (to - from) relative to `from`. */
    const f32 d[3] = {(f32)(to.x - from.x), (f32)(to.y - from.y), (f32)(to.z - from.z)};
    for (u32 i = 0; i < DE__VIS_CULL_PLANES; ++i)
        v->planes[i][3] += v->planes[i][0] * d[0] + v->planes[i][1] * d[1] + v->planes[i][2] * d[2];
    for (u32 k = 0; k < 3; ++k)
        v->camera[k] -= d[k];
    for (u32 r = 0; r < 4; ++r)
        v->view_proj[r][3] +=
            v->view_proj[r][0] * d[0] + v->view_proj[r][1] * d[1] + v->view_proj[r][2] * d[2];
}

bool de__vis_cull_sphere(const de__vis_cull_view *v, const f32 c[3], f32 r) {
    for (u32 i = 0; i < DE__VIS_CULL_PLANES; ++i) {
        const f32 *p = v->planes[i];
        if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -r)
            return false;
    }
    return true;
}

bool de__vis_cull_aabb(const de__vis_cull_view *v, const f32 mn[3], const f32 mx[3]) {
    for (u32 i = 0; i < DE__VIS_CULL_PLANES; ++i) {
        const f32 *p = v->planes[i];
        const f32 x = p[0] >= 0.0f ? mx[0] : mn[0];
        const f32 y = p[1] >= 0.0f ? mx[1] : mn[1];
        const f32 z = p[2] >= 0.0f ? mx[2] : mn[2];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f)
            return false;
    }
    return true;
}

f32 de__vis_max_axis_scale(const de__gs_instance *inst) {
    const f32 *r0 = inst->transform[0].v, *r1 = inst->transform[1].v, *r2 = inst->transform[2].v;
    const f32 sx = r0[0] * r0[0] + r1[0] * r1[0] + r2[0] * r2[0];
    const f32 sy = r0[1] * r0[1] + r1[1] * r1[1] + r2[1] * r2[1];
    const f32 sz = r0[2] * r0[2] + r1[2] * r1[2] + r2[2] * r2[2];
    const f32 m = sx > sy ? sx : sy;
    return sqrtf(m > sz ? m : sz);
}

u32 de__vis_select_lod(const de__vis_cull_view *v, const de__gs_instance *inst,
                       const de__gs_mesh *m) {
    const u32 levels = m->lod_count < DE__GS_MESH_LODS ? m->lod_count : DE__GS_MESH_LODS;
    if (!levels)
        return 0;
    if (v->lod[3] > 0.0f) {
        const u32 forced = (u32)v->lod[3] - 1u;
        return forced < levels ? forced : levels - 1u;
    }
    const f32 *c = inst->sphere.v;
    f32 dist = 1.0f;
    if (v->lod[1] == 0.0f) {
        const f32 dx = c[0] - v->camera[0], dy = c[1] - v->camera[1], dz = c[2] - v->camera[2];
        const f32 d = sqrtf(dx * dx + dy * dy + dz * dz) - c[3];
        dist = d > v->camera[3] ? d : v->camera[3];
    }
    const f32 scale = de__vis_max_axis_scale(inst);
    const i32 count = (i32)levels;
    i32 lod = 0;
    for (i32 l = count - 1; l > 0; --l) {
        if (m->lods[l].error * scale * v->lod[0] <= dist) {
            lod = l;
            break;
        }
    }
    i32 b = lod + (i32)v->lod[2];
    b = b < 0 ? 0 : b > count - 1 ? count - 1 : b;
    return (u32)(b < 0 ? 0 : b);
}

i32 de__vis_instance_test(const de__vis_cull_view *v, const de__gs_instance *inst,
                          const de__gs_mesh *m, u32 *lod) {
    const u32 need = DE__GS_INSTANCE_LIVE | DE__GS_INSTANCE_HAS_BOUNDS;
    if ((inst->flags & need) != need || inst->mesh == DE__GS_NONE)
        return -1;
    if ((inst->layer_mask & v->layer_mask) == 0)
        return -1;
    const u32 mesh_need = DE__GS_MESH_READY | DE__GS_MESH_MESHLETS;
    if (m == NULL || (m->flags & mesh_need) != mesh_need || m->lod_count == 0)
        return -1;
    if (v->flags & DE__VIS_CULL_FRUSTUM) {
        if (!de__vis_cull_sphere(v, inst->sphere.v, inst->sphere.v[3]))
            return 0;
        if (!de__vis_cull_aabb(v, inst->aabb_min.v, inst->aabb_max.v))
            return 0;
    }
    const u32 l = de__vis_select_lod(v, inst, m);
    if (lod != NULL)
        *lod = l;
    return 1;
}

/* =================================================================================================
 * Init / shutdown
 * ============================================================================================== */

static void view_release(de__vis_shared *s, ci_view *pv) {
    if (DE_HANDLE_VALID(pv->cull_view))
        de_gpu_buffer_destroy(s->dev, pv->cull_view); /* deferred until the GPU is done */
    if (DE_HANDLE_VALID(pv->stats_buf))
        de_gpu_buffer_destroy(s->dev, pv->stats_buf);
    memset(pv, 0, sizeof *pv);
}

de_result de__vis_cull_instances_init(de__vis_shared *s) {
    ci_state *st = DE_NEW(s->alloc, ci_state, DE_MEM_RENDER);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: instance culling state");
    memset(st, 0, sizeof *st);
    static const char *const k_entries[3] = {"cs_instance_cull_clear", "cs_instance_cull",
                                             "cs_instance_cull_args"};
    de__engine_shader_code code;
    de_result r = de__vis_shader_load(s, "vis_instance_cull", k_entries, 3, &code);
    if (r == DE_OK) {
        static const u32 one[3] = {1u, 1u, 1u}, group[3] = {CI_GROUP, 1u, 1u};
        const u32 pb = (u32)sizeof(ci_params);
        r = de__vis_kernel_create(s, &code, 0, "vis cull instances clear", one, pb, &st->clear);
        if (r == DE_OK)
            r = de__vis_kernel_create(s, &code, 1, "vis cull instances", group, pb, &st->cull);
        if (r == DE_OK)
            r = de__vis_kernel_create(s, &code, 2, "vis cull instances args", one, pb, &st->args);
        de__engine_shader_code_free(&code); /* the pipelines keep what they need */
    }
    if (r != DE_OK) {
        de__vis_kernel_destroy(s, &st->clear);
        de__vis_kernel_destroy(s, &st->cull);
        de__vis_kernel_destroy(s, &st->args);
        DE_FREE(s->alloc, st, sizeof *st);
        return r;
    }
    s->stage[DE__VIS_STAGE_CULL_INSTANCES] = st;
    return DE_OK;
}

void de__vis_cull_instances_shutdown(de__vis_shared *s) {
    ci_state *st = (ci_state *)s->stage[DE__VIS_STAGE_CULL_INSTANCES];
    if (st == NULL)
        return;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        view_release(s, &st->views[i]);
    de__vis_kernel_destroy(s, &st->clear);
    de__vis_kernel_destroy(s, &st->cull);
    de__vis_kernel_destroy(s, &st->args);
    DE_FREE(s->alloc, st, sizeof *st);
    s->stage[DE__VIS_STAGE_CULL_INSTANCES] = NULL;
}

de__vis_stats de__vis_view_stats(const de__vis_shared *s, u32 slot) {
    const ci_state *st =
        s != NULL ? (const ci_state *)s->stage[DE__VIS_STAGE_CULL_INSTANCES] : NULL;
    if (st == NULL || slot >= DE__VIEW_MAX)
        return (de__vis_stats){0};
    return st->views[slot].stats;
}

/* =================================================================================================
 * One view
 * ============================================================================================== */

static bool view_ensure(de__vis_shared *s, ci_view *pv, u32 slot) {
    if (DE_HANDLE_VALID(pv->cull_view))
        return true;
    char name[48];
    snprintf(name, sizeof name, "vis cull view %u", slot);
    pv->cull_view = de_gpu_buffer_create(
        s->dev, &(de_gpu_buffer_desc){.size = DE__VIS_FRAME_RING * sizeof(de__vis_cull_view),
                                      .usage = DE_USAGE_STORAGE,
                                      .memory = DE_MEM_UPLOAD,
                                      .stride = (u32)sizeof(de__vis_cull_view),
                                      .name = name});
    snprintf(name, sizeof name, "vis cull stats %u", slot);
    pv->stats_buf = de_gpu_buffer_create(
        s->dev, &(de_gpu_buffer_desc){.size = (u64)DE__VIS_FRAME_RING * CI_STATS_STRIDE,
                                      .usage = DE_USAGE_COPY_DST,
                                      .memory = DE_MEM_READBACK,
                                      .name = name});
    pv->cull_map =
        DE_HANDLE_VALID(pv->cull_view) ? (u8 *)de_gpu_buffer_map(s->dev, pv->cull_view) : NULL;
    pv->stats_map = DE_HANDLE_VALID(pv->stats_buf)
                        ? (const u8 *)de_gpu_buffer_map(s->dev, pv->stats_buf)
                        : NULL;
    if (pv->cull_map == NULL || pv->stats_map == NULL) {
        const de_error *e = de_error_last();
        DE_ERROR(s->log, "vis: view slot %u: cull buffers: %s", slot, e != NULL ? e->message : "?");
        const de_view h = pv->handle;
        view_release(s, pv);
        pv->handle = h;
        return false;
    }
    pv->cull_bindless = de_gpu_buffer_bindless(s->dev, pv->cull_view);
    pv->stats_access = DE_ACCESS_NONE;
    return true;
}

/* Takes the newest completed frame's counters (frames before `completed` are done). */
static void read_stats(ci_view *pv, u64 completed) {
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
    u32 c[8]; /* phase 1's counters, then phase 2's (DENG-233) */
    memcpy(c, pv->stats_map + (size_t)best * CI_STATS_STRIDE, sizeof c);
    if (pv->slot_single_phase[best])
        memset(c + 4, 0, 4 * sizeof(u32));
    const u32 v1 = c[DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u];
    const u32 v2 = c[4u + DE__VIS_COUNTER_INSTANCES_VISIBLE / 4u];
    pv->stats = (de__vis_stats){.valid = true,
                                .frame = pv->slot_frame[best],
                                .work_items = c[DE__VIS_COUNTER_WORK_ITEMS / 4u],
                                .instances_visible = v1 + v2,
                                .instances_tested = c[DE__VIS_COUNTER_INSTANCES_TESTED / 4u],
                                .meshlets = c[DE__VIS_COUNTER_MESHLETS / 4u] +
                                            c[4u + DE__VIS_COUNTER_MESHLETS / 4u],
                                .work_capacity = pv->slot_capacity[best],
                                .instances_visible_phase1 = v1,
                                .instances_visible_phase2 = v2,
                                .work_items_phase2 = c[4u + DE__VIS_COUNTER_WORK_ITEMS / 4u]};
}

static u32 pow2_at_least(u64 v) {
    u64 p = 1;
    while (p < v && p < CI_MAX_CAPACITY)
        p <<= 1;
    return (u32)(p < CI_MAX_CAPACITY ? p : CI_MAX_CAPACITY);
}

/* The work list's capacity this frame: enough for two chunks per instance, grown when a frame
 * read back asked for more than its list held. */
static u32 work_capacity(de__vis_shared *s, ci_view *pv, const de__vis_context *vc) {
    if (vc->work_capacity_fixed != 0)
        return vc->work_capacity_fixed;
    const u64 base = (u64)vc->instance_count * 2u;
    u32 cap = pow2_at_least(base > CI_MIN_CAPACITY ? base : CI_MIN_CAPACITY);
    if (pv->capacity > cap)
        cap = pv->capacity;
    const de__vis_stats *st = &pv->stats;
    const u32 asked =
        st->work_items > st->work_items_phase2 ? st->work_items : st->work_items_phase2;
    if (st->valid && asked > st->work_capacity) {
        const u32 want = pow2_at_least((u64)asked + asked / 4u);
        if (want > cap) {
            DE_WARN(s->log,
                    "vis: view slot %u: the work list overflowed in frame %llu (%u work items "
                    "for %u entries; the rest were not drawn); it grows to %u",
                    vc->view_slot, (unsigned long long)st->frame, asked, st->work_capacity, want);
            cap = want;
        } else if (cap == CI_MAX_CAPACITY && !pv->warned_max) {
            DE_WARN(s->log,
                    "vis: view slot %u: %u work items do not fit the largest work list (%u): "
                    "the rest are not drawn",
                    vc->view_slot, asked, CI_MAX_CAPACITY);
            pv->warned_max = true;
        }
    }
    pv->capacity = cap;
    return cap;
}

/* ---------- Passes ---------- */

typedef struct ci_pass {
    de_gpu_device *dev;
    de__vis_kernel kernel;
    ci_params pc;
    u32 groups;
    de_rg_resource instances, meshes, work_items, counters, args;
    de_rg_resource counters1; /**< phase 2's stats pass: phase 1's counters */
    de_gpu_buffer stats;
    u64 stats_offset;
} ci_pass;

static u32 bindless_of(const ci_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, r));
}

static void run(de_gpu_cmd *cmd, const ci_pass *p, const ci_params *pc, u32 groups) {
    de_gpu_cmd_set_pipeline(cmd, p->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, pc, (u32)sizeof *pc);
    de_gpu_cmd_dispatch(cmd, groups, 1, 1);
}

static void clear_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ci_pass *p = (const ci_pass *)user;
    ci_params pc = p->pc;
    pc.counters = bindless_of(p, pass, p->counters);
    run(cmd, p, &pc, 1);
}

static void cull_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ci_pass *p = (const ci_pass *)user;
    ci_params pc = p->pc;
    pc.instances = bindless_of(p, pass, p->instances);
    pc.meshes = bindless_of(p, pass, p->meshes);
    pc.work_items = bindless_of(p, pass, p->work_items);
    pc.counters = bindless_of(p, pass, p->counters);
    run(cmd, p, &pc, p->groups);
}

static void args_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ci_pass *p = (const ci_pass *)user;
    ci_params pc = p->pc;
    pc.counters = bindless_of(p, pass, p->counters);
    pc.args = bindless_of(p, pass, p->args);
    run(cmd, p, &pc, 1);
}

static void stats_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const ci_pass *p = (const ci_pass *)user;
    if (DE_HANDLE_VALID(p->counters1))
        de_gpu_cmd_copy_buffer(cmd, p->stats, p->stats_offset, de_rg_buffer(pass, p->counters1), 0,
                               DE__VIS_COUNTER_BYTES);
    if (DE_HANDLE_VALID(p->counters))
        de_gpu_cmd_copy_buffer(cmd, p->stats, p->stats_offset + DE__VIS_COUNTER_BYTES,
                               de_rg_buffer(pass, p->counters), 0, DE__VIS_COUNTER_BYTES);
}

static ci_pass *pass_data(de__vis_context *vc, const de__vis_kernel *k, const ci_pass *base) {
    ci_pass *p = (ci_pass *)de_arena_push(vc->arena, sizeof *p, 16);
    if (p != NULL) {
        *p = *base;
        if (k != NULL)
            p->kernel = *k;
    }
    return p;
}

/* The transients and the clear, cull and args passes of one phase (DENG-233: phase 1 reads last
 * frame's visibility flags; phase 2 reads the HZB and rewrites them). */
static bool add_phase(de_rg *g, de__vis_context *vc, ci_state *st, ci_view *pv,
                      de__vis_phase phase) {
    const u32 cap = pv->frame_capacity, count = pv->frame_count, slot = pv->frame_slot;
    const bool p2 = phase == DE__VIS_PHASE_2;
    const de_rg_resource items = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = (u64)cap * sizeof(de__vis_work_item),
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                 .stride = (u32)sizeof(de__vis_work_item),
                                 .name = p2 ? "vis.work_items2" : "vis.work_items"});
    const de_rg_resource counters = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = DE__VIS_COUNTER_BYTES,
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                 .name = p2 ? "vis.instance_counters2" : "vis.instance_counters"});
    const de_rg_resource args = de_rg_create_buffer(
        g, &(de_gpu_buffer_desc){.size = sizeof(de__vis_dispatch_args),
                                 .usage = DE_USAGE_STORAGE | DE_USAGE_INDIRECT | DE_USAGE_COPY_SRC,
                                 .stride = (u32)sizeof(de__vis_dispatch_args),
                                 .name = p2 ? "vis.cull_args2" : "vis.cull_args"});
    if (!DE_HANDLE_VALID(items) || !DE_HANDLE_VALID(counters) || !DE_HANDLE_VALID(args))
        return false; /* error set; finalize reports it */
    const ci_pass base = {
        .dev = vc->shared->dev,
        .pc = {.cull_view = pv->cull_bindless, .cull_view_index = slot, .phase = (u32)phase},
        .groups = (count + CI_GROUP - 1u) / CI_GROUP,
        .instances = vc->scene_instances,
        .meshes = vc->scene_meshes,
        .work_items = items,
        .counters = counters,
        .args = args,
        .stats = pv->stats_buf,
        .stats_offset = (u64)slot * CI_STATS_STRIDE};
    ci_pass *pc = pass_data(vc, &st->clear, &base), *pu = pass_data(vc, &st->cull, &base),
            *pa = pass_data(vc, &st->args, &base);
    if (pc == NULL || pu == NULL || pa == NULL) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: instance culling: the frame arena is full");
        return false;
    }
    de_rg_pass *p = de_rg_add_pass(g, p2 ? "vis.cull_instances2.clear" : "vis.cull_instances.clear",
                                   DE_QUEUE_GRAPHICS, clear_execute, pc);
    de_rg_write(p, counters, DE_ACCESS_SHADER_WRITE);

    p = de_rg_add_pass(g, p2 ? "vis.cull_instances2" : "vis.cull_instances", DE_QUEUE_GRAPHICS,
                       cull_execute, pu);
    de_rg_read(p, vc->scene_instances, DE_ACCESS_SHADER_READ);
    if (vc->hidden_entities.id)
        de_rg_read(p, vc->hidden_entities, DE_ACCESS_SHADER_READ);
    de_rg_read(p, vc->scene_meshes, DE_ACCESS_SHADER_READ);
    de_rg_read(p, counters, DE_ACCESS_SHADER_READ); /* atomics: read-modify-write */
    de_rg_write(p, counters, DE_ACCESS_SHADER_WRITE);
    de_rg_write(p, items, DE_ACCESS_SHADER_WRITE);
    if (DE_HANDLE_VALID(vc->visibility)) {
        de_rg_read(p, vc->visibility, DE_ACCESS_SHADER_READ);
        if (p2)
            de_rg_write(p, vc->visibility, DE_ACCESS_SHADER_WRITE);
    }
    if (p2 && DE_HANDLE_VALID(vc->hzb))
        de_rg_read(p, vc->hzb, DE_ACCESS_SHADER_READ);

    p = de_rg_add_pass(g, p2 ? "vis.cull_instances2.args" : "vis.cull_instances.args",
                       DE_QUEUE_GRAPHICS, args_execute, pa);
    de_rg_read(p, counters, DE_ACCESS_SHADER_READ);
    de_rg_write(p, args, DE_ACCESS_SHADER_WRITE);

    const u32 i = DE__VIS_PHASE_INDEX(phase);
    vc->work_items[i] = items;
    vc->instance_counters[i] = counters;
    vc->cull_args[i] = args;
    return true;
}

/* After phase 2: both phases' counters into the view's readback entry of this frame. */
static void add_stats(de_rg *g, de__vis_context *vc, ci_view *pv) {
    ci_pass *ps = (ci_pass *)de_arena_push_zero(vc->arena, sizeof *ps, 16);
    if (ps == NULL)
        return;
    ps->counters1 = vc->instance_counters[0];
    ps->counters = vc->instance_counters[1];
    ps->stats = pv->stats_buf;
    ps->stats_offset = (u64)pv->frame_slot * CI_STATS_STRIDE;
    de_rg_pass *p =
        de_rg_add_pass(g, "vis.cull_instances.stats", DE_QUEUE_GRAPHICS, stats_execute, ps);
    de_rg_pass_side_effect(p);
    if (DE_HANDLE_VALID(ps->counters1))
        de_rg_read(p, ps->counters1, DE_ACCESS_COPY_SRC);
    if (DE_HANDLE_VALID(ps->counters))
        de_rg_read(p, ps->counters, DE_ACCESS_COPY_SRC);
    const de_rg_resource rb = de_rg_import_buffer(g, pv->stats_buf, pv->stats_access);
    de_rg_set_name(g, rb, "vis.cull_stats");
    de_rg_write(p, rb, DE_ACCESS_COPY_DST);
    pv->stats_access = DE_ACCESS_COPY_DST; /* the epilogue leaves it in its last access */
    pv->slot_frame[pv->frame_slot] = vc->frame_index;
    pv->slot_capacity[pv->frame_slot] = pv->frame_capacity;
    pv->slot_live[pv->frame_slot] = true;
    pv->slot_single_phase[pv->frame_slot] = vc->depth_only;
}

void de__vis_cull_instances(de_rg *g, de__vis_context *vc, de__vis_phase phase) {
    de__vis_shared *s = vc->shared;
    ci_state *st = (ci_state *)s->stage[DE__VIS_STAGE_CULL_INSTANCES];
    if (st == NULL || vc->view_slot >= DE__VIEW_MAX)
        return;
    ci_view *pv = &st->views[vc->view_slot];
    if (phase == DE__VIS_PHASE_2) {
        /* DENG-233: the same constants, after the HZB was built; then the stats of both. */
        if (pv->frame_ready && add_phase(g, vc, st, pv, phase))
            add_stats(g, vc, pv);
        pv->frame_ready = false;
        return;
    }
    pv->frame_ready = false;
    if (vc->view_new || pv->handle.id != vc->view.id) {
        view_release(s, pv);
        pv->handle = vc->view;
    }
    if (DE_HANDLE_VALID(pv->stats_buf))
        read_stats(pv, vc->frames_completed);
    vc->stats = pv->stats;
    if (vc->instance_count == 0 || !DE_HANDLE_VALID(vc->scene_instances) ||
        !DE_HANDLE_VALID(vc->scene_meshes))
        return;
    if (!view_ensure(s, pv, vc->view_slot))
        return;

    /* This frame's cull constants: the camera's, or the frozen one rebased onto this origin. */
    const u32 slot = (u32)(vc->frame_index % DE__VIS_FRAME_RING);
    const u32 cap = work_capacity(s, pv, vc);
    de__vis_cull_view cv;
    de__vis_cull_view_init(&cv, &(de__vis_cull_camera){.view_proj = vc->view_proj,
                                                       .proj = vc->proj,
                                                       .position = vc->camera_position,
                                                       .height = vc->height,
                                                       .lod_error_px = vc->lod_error_px,
                                                       .lod_bias = vc->lod_bias});
    if (vc->cull_freeze) {
        if (!pv->frozen) {
            pv->frozen = true;
            pv->frozen_view = cv;
            pv->frozen_origin = vc->origin;
        }
        de__vis_cull_view fz = pv->frozen_view;
        de__vis_cull_view_rebase(&fz, pv->frozen_origin, vc->origin);
        memcpy(cv.planes, fz.planes, sizeof cv.planes);
        memcpy(cv.camera, fz.camera, sizeof cv.camera);
        memcpy(cv.lod, fz.lod, sizeof cv.lod);
        memcpy(cv.view_proj, fz.view_proj, sizeof cv.view_proj);
        cv.flags |= DE__VIS_CULL_FROZEN;
    } else {
        pv->frozen = false;
    }
    /* Freeze only the camera's automatic policy; an explicit per-view choice remains live. */
    cv.lod[3] = (f32)vc->forced_lod;
    if (vc->cull_disable)
        cv.flags &= ~(u32)DE__VIS_CULL_FRUSTUM;
    u32 count = vc->instance_count;
    if (count > CI_MAX_INSTANCES) {
        DE_ERROR(s->log, "vis: %u instance slots; one cull dispatch covers %u", count,
                 CI_MAX_INSTANCES);
        count = CI_MAX_INSTANCES;
    }
    cv.instance_count = count;
    cv.layer_mask = vc->layer_mask;
    cv.hidden_entities = vc->hidden_bindless;
    cv.hidden_count = vc->hidden_count;
    cv.work_capacity = cap;
    /* DENG-233: the HZB phase 2 tests against, and both phases' visibility flags. r.cull_disable
     * also turns occlusion off. */
    if (vc->occlusion && DE_HANDLE_VALID(vc->visibility) && !vc->cull_disable)
        cv.flags |= DE__VIS_CULL_OCCLUSION;
    cv.visibility = DE_HANDLE_VALID(vc->visibility) ? vc->visibility_bindless : 0u;
    cv.visibility_count = vc->visibility_count;
    if (DE_HANDLE_VALID(vc->hzb)) {
        cv.hzb = vc->hzb_bindless;
        cv.hzb_width = vc->hzb_width;
        cv.hzb_height = vc->hzb_height;
        cv.hzb_mips = vc->hzb_mips;
    }
    memcpy(pv->cull_map + (size_t)slot * sizeof cv, &cv, sizeof cv);
    vc->cull = cv;
    vc->cull_view_bindless = pv->cull_bindless;
    vc->cull_view_index = slot;
    vc->work_capacity = cap;
    pv->frame_slot = slot;
    pv->frame_count = count;
    pv->frame_capacity = cap;
    pv->frame_ready = add_phase(g, vc, st, pv, phase);
    if (vc->depth_only && pv->frame_ready)
        add_stats(g, vc, pv);
}

void de__vis_cull_instances_abort(de__vis_shared *s, u32 slot, u64 frame) {
    ci_state *st = (ci_state *)s->stage[DE__VIS_STAGE_CULL_INSTANCES];
    if (!st || slot >= DE__VIEW_MAX)
        return;
    u32 ring = (u32)(frame % DE__VIS_FRAME_RING);
    if (st->views[slot].slot_frame[ring] == frame)
        st->views[slot].slot_live[ring] = false;
    st->views[slot].frame_ready = false;
}
