/*
 * picking.c: vis-buffer picking (SPEC §20.1; DENG-239): de_view_request_pick and
 * de_view_pick_result read the entity and the triangle under a pixel of a view back from that
 * view's visibility buffer. shaders/visbuffer/pick.slang has the kernel (engine shader
 * "vis_pick"); vis_stages.h the stage contract.
 *
 * Main thread: de_view_request_pick queues a request in its view slot's queue (at most
 * DE__VIS_PICK_MAX per view and frame; more are dropped, with a warning in Debug and Dev).
 *
 * Render thread, in the view's vis frame (vis_frame.c calls de__vis_pick after phase 2's alpha
 * raster, before classify, so the vis IDs and the visible-meshlet list are this frame's): the
 * stage takes the queue, maps each request from output pixels to internal pixels
 * (floor((x + 0.5) * internal / output)) and, when the frame has a vis buffer, adds
 *
 *   vis.pick.copy       the vis IDs into a raw buffer        vis_ids: COPY_SRC; ids: COPY_DST
 *                       (the R32_UINT target has no sampled
 *                       or storage view)
 *   vis.pick            cs_pick, one 16-thread group          ids, visible list, instances:
 *                                                             SHADER_READ; results: SHADER_WRITE
 *   vis.pick.readback   the results into the view's           results: COPY_SRC; readback:
 *                       readback ring entry of this frame     COPY_DST (side effect)
 *
 * A frame without requests adds no pass. The requests of a frame form a batch in entry
 * frame % DE__VIS_FRAME_RING of the view slot's ring (the renderer keeps at most 3 frames in
 * flight, so the entry's previous frame has completed). A batch is collected once its frame has
 * completed on the GPU (frames_completed, or the renderer's frame fence): each request's result
 * goes to the results table, where de_view_pick_result finds it for DE__VIS_PICK_KEEP_FRAMES
 * frames. Requests outside the view, and every request of a frame without a vis buffer, are
 * background (entity 0, DE_PICK_TRIANGLE_NONE), still delivered once their frame completed.
 *
 * Collection runs in the stage (render thread) and in de_view_pick_result (main thread), under
 * the state's lock. A view slot that changes its view (destroyed, a new view in the slot) drops
 * its queue and its batches in flight (their results never arrive: de_view_pick_result returns
 * false). All state is allocated once, at renderer create: requests and results allocate nothing.
 */
#include "render/passes/vis/vis_stages.h"

#include "render/graph/rg_internal.h"
#include "render/renderer_internal.h"

#include <dengine/de_platform.h>

#include <stdio.h>
#include <string.h>

/** Bytes of one view slot's readback entry (one frame's results). */
#define PK_ENTRY_BYTES (DE__VIS_PICK_MAX * (u32)sizeof(de__vis_pick_gpu_result))

/** The push constants of cs_pick (pick.slang PickParams, 112 bytes). */
typedef struct pk_params {
    u32 ids, pitch, width, height;
    u32 visible, visible_count, instances, instance_count;
    u32 results, count, reserved0, reserved1;
    u32 coords[DE__VIS_PICK_MAX]; /**< x | (y << 16), internal pixels */
} pk_params;
DE_STATIC_ASSERT(sizeof(pk_params) == 112, "PickParams is 112 bytes");
DE_STATIC_ASSERT(sizeof(pk_params) <= DE_GPU_MAX_PUSH_CONSTANT_BYTES, "PickParams fits");

typedef struct pk_request {
    u64 id;
    u32 x, y; /**< output pixels */
} pk_request;

/** One frame's requests of one view. */
typedef struct pk_batch {
    bool live; /**< waits for its frame to complete */
    bool gpu;  /**< the GPU writes its results (else every request is background) */
    u64 frame;
    u32 count;
    u64 ids[DE__VIS_PICK_MAX];
    bool outside[DE__VIS_PICK_MAX]; /**< outside the view: background without the GPU */
} pk_batch;

typedef struct pk_view {
    /* Main side: the queue of the next frame (lock). */
    de_view queue_view;
    pk_request queue[DE__VIS_PICK_MAX];
    u32 queue_count;
    u64 warned; /**< frame_now + 1 of the last drop warning (0: none) */
    /* Render side. */
    de_view handle;         /**< the view the batches belong to */
    de_gpu_buffer readback; /**< READBACK: DE__VIS_FRAME_RING x PK_ENTRY_BYTES */
    const u8 *map;
    u32 rb_access;
    pk_batch batch[DE__VIS_FRAME_RING]; /**< lock: both threads collect */
} pk_view;

typedef struct pk_result {
    bool used;
    u64 id;
    u64 entity;
    u32 triangle;
    u64 frame; /**< frame_now when it became available */
} pk_result;

typedef struct pk_state {
    de_mutex lock;
    de__vis_kernel kernel; /**< cs_pick (invalid when the frame is off) */
    pk_view views[DE__VIEW_MAX];
    pk_result results[DE__VIS_PICK_RESULTS];
    u64 frame_now;      /**< the newest frame the stage saw */
    u64 completed_seen; /**< its frames_completed */
    de__vis_pick_stats stats;
} pk_state;

static pk_state *state_of(const de__vis_shared *s) {
    return s != NULL ? (pk_state *)s->pick : NULL;
}

static const de__vis_layout_const k_consts[] = {
    {"DE_VIS_PICK_MAX", DE__VIS_PICK_MAX},
    {"DE_VIS_PICK_RESULT_BYTES", (u32)sizeof(de__vis_pick_gpu_result)},
    {"DE_VIS_PICK_TRIANGLE_NONE", DE_PICK_TRIANGLE_NONE},
};

const de__vis_layout_const *de__vis_pick_layout_consts(u32 *count) {
    *count = (u32)(sizeof k_consts / sizeof k_consts[0]);
    return k_consts;
}

/* =================================================================================================
 * Init / shutdown
 * ============================================================================================== */

de_result de__vis_pick_init(de__vis_shared *s) {
    pk_state *st = DE_NEW(s->alloc, pk_state, DE_MEM_RENDER);
    if (st == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "vis: picking state");
    memset(st, 0, sizeof *st);
    de_mutex_init(&st->lock);
    s->pick = st;
    if (!s->enabled)
        return DE_OK; /* requests are taken but never rendered */
    static const char *const k_entries[1] = {"cs_pick"};
    de__engine_shader_code code;
    de_result r = de__vis_shader_load(s, "vis_pick", k_entries, 1, &code);
    if (r == DE_OK) {
        static const u32 group[3] = {DE__VIS_PICK_MAX, 1u, 1u};
        r = de__vis_kernel_create(s, &code, 0, "vis pick", group, (u32)sizeof(pk_params),
                                  &st->kernel);
        de__engine_shader_code_free(&code);
    }
    if (r != DE_OK) {
        /* Picking without its kernel: every request comes back as background. */
        const de_error *e = de_error_last();
        DE_ERROR(s->log, "vis: picking is off: %s", e != NULL ? e->message : de_result_str(r));
        de_error_clear();
    }
    return DE_OK;
}

void de__vis_pick_shutdown(de__vis_shared *s) {
    pk_state *st = state_of(s);
    if (st == NULL)
        return;
    for (u32 i = 0; i < DE__VIEW_MAX; ++i)
        if (DE_HANDLE_VALID(st->views[i].readback))
            de_gpu_buffer_destroy(s->dev, st->views[i].readback);
    de__vis_kernel_destroy(s, &st->kernel);
    de_mutex_destroy(&st->lock);
    DE_FREE(s->alloc, st, sizeof *st);
    s->pick = NULL;
}

/* =================================================================================================
 * Results (lock held)
 * ============================================================================================== */

static bool result_live(const pk_state *st, const pk_result *r) {
    return r->used && st->frame_now <= r->frame + DE__VIS_PICK_KEEP_FRAMES;
}

static void result_store(pk_state *st, u64 id, u64 entity, u32 triangle) {
    pk_result *slot = NULL;
    pk_result *oldest = &st->results[0];
    for (u32 i = 0; i < DE__VIS_PICK_RESULTS; ++i) {
        pk_result *r = &st->results[i];
        if (r->used && r->id == id) {
            slot = r;
            break;
        }
        if (slot == NULL && !result_live(st, r))
            slot = r;
        if (r->frame < oldest->frame)
            oldest = r;
    }
    if (slot == NULL)
        slot = oldest; /* every entry is live: more than the table holds in 8 frames */
    *slot = (pk_result){
        .used = true, .id = id, .entity = entity, .triangle = triangle, .frame = st->frame_now};
    ++st->stats.completed;
}

static void result_forget(pk_state *st, u64 id) {
    for (u32 i = 0; i < DE__VIS_PICK_RESULTS; ++i)
        if (st->results[i].used && st->results[i].id == id)
            st->results[i].used = false;
}

/* Frames [0, value) have completed on the GPU. */
static u64 completed_now(const de__vis_shared *s, const pk_state *st) {
    u64 c = st->completed_seen;
    const de_renderer *r = s->renderer;
    if (r != NULL && DE_HANDLE_VALID(r->frame_fence)) {
        const u64 f = de_gpu_fence_value(r->dev, r->frame_fence); /* frame N signals N + 1 */
        if (f > c)
            c = f;
    }
    return c;
}

/* Moves the results of every batch whose frame has completed to the table. */
static void collect(pk_state *st, u64 completed) {
    for (u32 v = 0; v < DE__VIEW_MAX; ++v) {
        pk_view *pv = &st->views[v];
        for (u32 k = 0; k < DE__VIS_FRAME_RING; ++k) {
            pk_batch *b = &pv->batch[k];
            if (!b->live || b->frame >= completed)
                continue;
            for (u32 i = 0; i < b->count; ++i) {
                de__vis_pick_gpu_result g = {.triangle = DE_PICK_TRIANGLE_NONE};
                if (b->gpu && !b->outside[i] && pv->map != NULL)
                    memcpy(&g, pv->map + (size_t)k * PK_ENTRY_BYTES + (size_t)i * sizeof g,
                           sizeof g);
                if (g.hit != 0)
                    result_store(st, b->ids[i], ((u64)g.entity_hi << 32) | g.entity_lo, g.triangle);
                else
                    result_store(st, b->ids[i], 0u, DE_PICK_TRIANGLE_NONE);
            }
            b->live = false;
        }
    }
}

/* =================================================================================================
 * Main thread
 * ============================================================================================== */

bool de__vis_pick_request(de__vis_shared *s, de_view v, u32 x, u32 y, u64 id) {
    pk_state *st = state_of(s);
    const u32 slot = (v.id & 0xFFu) - 1u; /* views.c: (generation << 8) | (slot + 1) */
    if (st == NULL || !DE_HANDLE_VALID(v) || slot >= DE__VIEW_MAX)
        return false;
    de_mutex_lock(&st->lock);
    pk_view *pv = &st->views[slot];
    if (pv->queue_view.id != v.id) { /* another view took the slot: its requests are gone */
        st->stats.dropped += pv->queue_count;
        pv->queue_count = 0;
        pv->queue_view = v;
    }
    bool ok = pv->queue_count < DE__VIS_PICK_MAX;
    if (ok) {
        pv->queue[pv->queue_count++] = (pk_request){.id = id, .x = x, .y = y};
        result_forget(st, id);
        ++st->stats.requested;
    } else {
        ++st->stats.dropped;
#if defined(DE_DEBUG) || defined(DE_DEV)
        if (pv->warned != st->frame_now + 1u) { /* once per view and frame */
            pv->warned = st->frame_now + 1u;
            ++st->stats.warnings;
            DE_WARN(s->log,
                    "de_view_request_pick: more than %u requests for view %u this frame; "
                    "request %llu dropped",
                    DE__VIS_PICK_MAX, slot, (unsigned long long)id);
        }
#endif
    }
    de_mutex_unlock(&st->lock);
    return ok;
}

bool de__vis_pick_result(de__vis_shared *s, u64 id, u64 *entity_bits, u32 *triangle) {
    pk_state *st = state_of(s);
    if (st == NULL)
        return false;
    de_mutex_lock(&st->lock);
    collect(st, completed_now(s, st));
    bool found = false;
    for (u32 i = 0; i < DE__VIS_PICK_RESULTS; ++i) {
        const pk_result *r = &st->results[i];
        if (r->id != id || !result_live(st, r))
            continue;
        if (entity_bits != NULL)
            *entity_bits = r->entity;
        if (triangle != NULL)
            *triangle = r->triangle;
        found = true;
        break;
    }
    de_mutex_unlock(&st->lock);
    return found;
}

de__vis_pick_stats de__vis_pick_get_stats(const de__vis_shared *s) {
    pk_state *st = state_of(s);
    if (st == NULL)
        return (de__vis_pick_stats){0};
    de_mutex_lock(&st->lock);
    const de__vis_pick_stats out = st->stats;
    de_mutex_unlock(&st->lock);
    return out;
}

/* The public API (de_render.h). */

void de_view_request_pick(de_renderer *r, de_view v, u32 x, u32 y, u64 request_id) {
    if (r == NULL || !de__view_valid(r, v))
        return;
    (void)de__vis_pick_request(de__views_vis(r), v, x, y, request_id);
}

bool de_view_pick_result(de_renderer *r, u64 request_id, u64 *entity_bits, u32 *triangle) {
    return r != NULL && de__vis_pick_result(de__views_vis(r), request_id, entity_bits, triangle);
}

/* =================================================================================================
 * Render thread: the stage
 * ============================================================================================== */

typedef struct pk_pass {
    de_gpu_device *dev;
    de__vis_kernel kernel;
    pk_params pc;
    de_rg_resource vis_ids, ids, visible, instances, results, readback;
    u64 rb_offset;
    u32 count;
} pk_pass;

static u32 bindless_of(const pk_pass *p, const de_rg_pass *pass, de_rg_resource r) {
    return de_gpu_buffer_bindless(p->dev, de_rg_buffer(pass, r));
}

static void copy_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const pk_pass *p = (const pk_pass *)user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, p->ids), 0, p->pc.pitch,
                                      de_rg_texture(pass, p->vis_ids), 0, 0);
}

static void pick_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const pk_pass *p = (const pk_pass *)user;
    pk_params pc = p->pc;
    pc.ids = bindless_of(p, pass, p->ids);
    pc.visible = bindless_of(p, pass, p->visible);
    pc.instances = bindless_of(p, pass, p->instances);
    pc.results = bindless_of(p, pass, p->results);
    de_gpu_cmd_set_pipeline(cmd, p->kernel.pipeline);
    de_gpu_cmd_push_constants(cmd, &pc, (u32)sizeof pc);
    de_gpu_cmd_dispatch(cmd, 1, 1, 1);
}

static void readback_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const pk_pass *p = (const pk_pass *)user;
    de_gpu_cmd_copy_buffer(cmd, de_rg_buffer(pass, p->readback), p->rb_offset,
                           de_rg_buffer(pass, p->results), 0,
                           (u64)p->count * sizeof(de__vis_pick_gpu_result));
}

/* The view slot's readback ring (made at its first pick). */
static bool readback_ensure(de__vis_shared *s, pk_view *pv, u32 slot) {
    if (pv->map != NULL)
        return true;
    char name[40];
    snprintf(name, sizeof name, "vis pick readback %u", slot);
    pv->readback = de_gpu_buffer_create(
        s->dev, &(de_gpu_buffer_desc){.size = (u64)DE__VIS_FRAME_RING * PK_ENTRY_BYTES,
                                      .usage = DE_USAGE_COPY_DST,
                                      .memory = DE_MEM_READBACK,
                                      .name = name});
    pv->map =
        DE_HANDLE_VALID(pv->readback) ? (const u8 *)de_gpu_buffer_map(s->dev, pv->readback) : NULL;
    if (pv->map == NULL) {
        const de_error *e = de_error_last();
        DE_ERROR(s->log, "vis: view slot %u: pick readback: %s", slot,
                 e != NULL ? e->message : "?");
        if (DE_HANDLE_VALID(pv->readback))
            de_gpu_buffer_destroy(s->dev, pv->readback);
        pv->readback = (de_gpu_buffer){0};
        return false;
    }
    pv->rb_access = DE_ACCESS_NONE;
    return true;
}

/* Output pixel `v` of `out` pixels to the internal pixel of `in` pixels: floor((v + 0.5) * in /
 * out), clamped. */
static u32 to_internal(u32 v, u32 out, u32 in) {
    const u64 p = ((u64)v * 2u + 1u) * in / ((u64)out * 2u);
    return p < in ? (u32)p : in - 1u;
}

/* Adds the three passes; false when the frame cannot pick (the batch is then background). */
static bool add_passes(de_rg *g, de__vis_context *vc, pk_state *st, pk_view *pv,
                       const pk_params *base, u32 count, u32 ring) {
    de__vis_shared *s = vc->shared;
    if (!DE_HANDLE_VALID(st->kernel.pipeline) || !DE_HANDLE_VALID(vc->vis_ids) ||
        !DE_HANDLE_VALID(vc->visible_meshlets) || !DE_HANDLE_VALID(vc->scene_instances) ||
        vc->width == 0 || vc->height == 0 || vc->width > 0xFFFFu || vc->height > 0xFFFFu)
        return false;
    if (!readback_ensure(s, pv, vc->view_slot))
        return false;
    pk_pass *p = (pk_pass *)de_arena_push_zero(vc->arena, sizeof *p, 16);
    if (p == NULL) {
        DE_ERROR(s->log, "vis: picking: the frame arena is full");
        return false;
    }
    const u32 pitch = (vc->width * 4u + 255u) & ~255u; /* copy_texture_to_buffer's row rule */
    const de_rg_resource ids =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = (u64)pitch * vc->height,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_DST,
                                                     .name = "vis.pick_ids"});
    const de_rg_resource results =
        de_rg_create_buffer(g, &(de_gpu_buffer_desc){.size = PK_ENTRY_BYTES,
                                                     .usage = DE_USAGE_STORAGE | DE_USAGE_COPY_SRC,
                                                     .name = "vis.pick_results"});
    if (!DE_HANDLE_VALID(ids) || !DE_HANDLE_VALID(results))
        return false; /* error set; finalize reports it */
    p->dev = s->dev;
    p->kernel = st->kernel;
    p->pc = *base;
    p->pc.pitch = pitch;
    p->pc.width = vc->width;
    p->pc.height = vc->height;
    p->pc.visible_count = vc->visible_capacity + vc->visible_alpha_capacity;
    p->pc.instance_count = vc->instance_count;
    p->pc.count = count;
    p->vis_ids = vc->vis_ids;
    p->ids = ids;
    p->visible = vc->visible_meshlets;
    p->instances = vc->scene_instances;
    p->results = results;
    p->rb_offset = (u64)ring * PK_ENTRY_BYTES;
    p->count = count;

    de_rg_pass *rp = de_rg_add_pass(g, "vis.pick.copy", DE_QUEUE_GRAPHICS, copy_execute, p);
    de_rg_read(rp, vc->vis_ids, DE_ACCESS_COPY_SRC);
    de_rg_write(rp, ids, DE_ACCESS_COPY_DST);

    rp = de_rg_add_pass(g, "vis.pick", DE_QUEUE_GRAPHICS, pick_execute, p);
    de_rg_read(rp, ids, DE_ACCESS_SHADER_READ);
    de_rg_read(rp, vc->visible_meshlets, DE_ACCESS_SHADER_READ);
    de_rg_read(rp, vc->scene_instances, DE_ACCESS_SHADER_READ);
    de_rg_write(rp, results, DE_ACCESS_SHADER_WRITE);

    rp = de_rg_add_pass(g, "vis.pick.readback", DE_QUEUE_GRAPHICS, readback_execute, p);
    de_rg_pass_side_effect(rp);
    de_rg_read(rp, results, DE_ACCESS_COPY_SRC);
    p->readback = de_rg_import_buffer(g, pv->readback, pv->rb_access);
    de_rg_set_name(g, p->readback, "vis.pick_readback");
    de_rg_write(rp, p->readback, DE_ACCESS_COPY_DST);
    pv->rb_access = DE_ACCESS_COPY_DST; /* the epilogue leaves it in its last access */
    return true;
}

void de__vis_pick(de_rg *g, de__vis_context *vc) {
    de__vis_shared *s = vc->shared;
    pk_state *st = state_of(s);
    if (st == NULL || vc->view_slot >= DE__VIEW_MAX)
        return;
    pk_view *pv = &st->views[vc->view_slot];
    pk_request reqs[DE__VIS_PICK_MAX];
    u32 n = 0;

    de_mutex_lock(&st->lock);
    if (vc->frame_index > st->frame_now)
        st->frame_now = vc->frame_index;
    if (vc->frames_completed > st->completed_seen)
        st->completed_seen = vc->frames_completed;
    if (vc->view_new || pv->handle.id != vc->view.id) {
        /* A new view in the slot: the old one's frames in flight are lost. */
        for (u32 k = 0; k < DE__VIS_FRAME_RING; ++k) {
            if (pv->batch[k].live)
                st->stats.lost += pv->batch[k].count;
            pv->batch[k].live = false;
        }
        pv->handle = vc->view;
    }
    collect(st, completed_now(s, st));
    if (pv->queue_count > 0) {
        if (pv->queue_view.id == vc->view.id) {
            n = pv->queue_count;
            memcpy(reqs, pv->queue, sizeof(pk_request) * n);
        } else {
            st->stats.dropped += pv->queue_count; /* for a view no longer in the slot */
        }
        pv->queue_count = 0;
    }
    de_mutex_unlock(&st->lock);
    if (n == 0)
        return;

    pk_batch b = {.live = true, .frame = vc->frame_index, .count = n};
    pk_params pc;
    memset(&pc, 0, sizeof pc);
    const u32 ow = vc->output_width != 0 ? vc->output_width : vc->width;
    const u32 oh = vc->output_height != 0 ? vc->output_height : vc->height;
    for (u32 i = 0; i < n; ++i) {
        b.ids[i] = reqs[i].id;
        b.outside[i] = reqs[i].x >= ow || reqs[i].y >= oh || vc->width == 0 || vc->height == 0;
        if (!b.outside[i])
            pc.coords[i] = to_internal(reqs[i].x, ow, vc->width) |
                           (to_internal(reqs[i].y, oh, vc->height) << 16);
        else
            pc.coords[i] = 0xFFFFFFFFu; /* never inside: the kernel writes background */
    }
    const u32 ring = (u32)(vc->frame_index % DE__VIS_FRAME_RING);
    de_mutex_lock(&st->lock);
    pk_batch *slot = &pv->batch[ring];
    if (slot->live) { /* its frame never completed (more frames in flight than the ring) */
        st->stats.lost += slot->count;
        slot->live = false;
    }
    de_mutex_unlock(&st->lock);
    b.gpu = add_passes(g, vc, st, pv, &pc, n, ring);
    de_mutex_lock(&st->lock);
    *slot = b;
    st->stats.dispatched += n;
    de_mutex_unlock(&st->lock);
}
