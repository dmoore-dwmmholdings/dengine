/*
 * rg_timing.c: per-pass GPU timers, the breadcrumb names and the published timings of the render
 * graph (SPEC §12.3, §18.2, §18.5; DENG-173). See rg_internal.h ("DENG-173") for the frame flow.
 *
 * Each frame uses one of DE__RG_TIMING_SLOTS slots (round robin). A slot owns two timestamp
 * heaps (graphics + compute share the main heap; the copy queue has its own, DENG-326), a
 * READBACK buffer per heap, and a copy of the frame's pass names and query indices, so its
 * results are labelled by the frame that produced them even when a later frame has another pass
 * set. Main-heap queries 0 and 1 are the frame span (prologue start, epilogue end); every timed
 * pass takes two consecutive queries (begin, end) of its queue's heap.
 *
 * The resolves are recorded at the end of the frame (rg_execute.c) and the values are read when
 * the frame's end value is reached on the graph's graphics timeline: usually one or two frames
 * later. The newest completed frame is published (de__rg_timings): copies of its names and times
 * that stay valid until the next publish.
 *
 * Metal (DENG-326): a timestamp is a small blit encoder of its own and a full barrier. The
 * timestamps of a pass sit outside its barrier lists (begin before PRE, end after POST), so the
 * plan's barriers are unchanged; what timing costs on Metal is that passes of one list no longer
 * overlap. r.rg_gpu_timers 0 turns the timers off (no queries, no resolve; times read 0).
 *
 * Heaps and buffers grow to the largest graph seen (rounded up to a power of two, at least 64
 * queries, at most DE__RG_MAX_QUERIES; passes past that are not timed). Nothing allocates once
 * every slot has seen the largest graph.
 */
#include "render/graph/rg_internal.h"

#include <stdio.h>
#include <string.h>

/* Grows a heap array of `elem`-byte elements to hold `need` (keeps the first `keep`). */
static bool grow(de_rg *g, void **arr, u32 *cap, u32 need, size_t elem, u32 keep) {
    if (need <= *cap)
        return true;
    u32 nc = *cap ? *cap : 16u;
    while (nc < need)
        nc = nc > UINT32_MAX / 2u ? need : nc * 2u;
    void *p = DE_ALLOC(g->alloc, (size_t)nc * elem, 16, DE_MEM_RENDER);
    if (!p)
        return false;
    if (keep && *arr)
        memcpy(p, *arr, (size_t)keep * elem);
    if (*arr)
        DE_FREE(g->alloc, *arr, (size_t)*cap * elem);
    *arr = p;
    *cap = nc;
    return true;
}

static void free_array(de_rg *g, void **arr, u32 *cap, size_t elem) {
    if (*arr)
        DE_FREE(g->alloc, *arr, (size_t)*cap * elem);
    *arr = NULL;
    *cap = 0;
}

static u32 query_capacity(u32 need) {
    u32 c = 64u;
    while (c < need && c < DE__RG_MAX_QUERIES)
        c *= 2u;
    return c;
}

static void destroy_heap(de__rg_exec *x, de__rg_tslot *ts, u32 h) {
    if (DE_HANDLE_VALID(ts->heap[h]))
        de_gpu_query_heap_destroy(x->device, ts->heap[h]);
    if (DE_HANDLE_VALID(ts->readback[h]))
        de_gpu_buffer_destroy(x->device, ts->readback[h]);
    ts->heap[h].id = 0;
    ts->readback[h].id = 0;
    ts->heap_cap[h] = 0;
}

/* Makes heap `h` of the slot hold `need` queries (at most DE__RG_MAX_QUERIES). Destroying the
 * old heap is deferred by the RHI until the GPU is done with it; the slot is not in flight here
 * (de__rg_timing_begin waited for it). False when the device has no such heap. */
static bool ensure_heap(de_rg *g, de__rg_exec *x, de__rg_tslot *ts, u32 h, u32 need) {
    bool *unsupported = h == DE__RG_HEAP_COPY ? &x->copy_unsupported : &x->main_unsupported;
    if (*unsupported)
        return false;
    if (need > DE__RG_MAX_QUERIES)
        need = DE__RG_MAX_QUERIES;
    if (DE_HANDLE_VALID(ts->heap[h]) && ts->heap_cap[h] >= need)
        return true;
    destroy_heap(x, ts, h);
    const u32 cap = query_capacity(need);
    const de_gpu_queue q = h == DE__RG_HEAP_COPY ? DE_QUEUE_COPY : DE_QUEUE_GRAPHICS;
    const char *what = h == DE__RG_HEAP_COPY ? "copy" : "graphics";
    char name[96];
    snprintf(name, sizeof name, "rg %.24s %s times", g->name, what);
    ts->heap[h] = de_gpu_query_heap_create(
        x->device, &(de_gpu_query_heap_desc){
                       .type = DE_QUERY_TIMESTAMP, .count = cap, .name = name, .queue = q});
    if (!DE_HANDLE_VALID(ts->heap[h])) {
        const de_error *e = de_error_last();
        if (e && e->result == DE_ERR_UNSUPPORTED) {
            /* No timestamps on this queue (CI's paravirtual Metal GPU, a transfer-only Vulkan
             * family): its passes are not timed, from now on. */
            *unsupported = true;
            DE_WARN(g->log,
                    "render graph \"%s\": no %s-queue timestamps on this device (%s); "
                    "those passes are not timed",
                    g->name, what, e->message);
            de_error_clear();
        }
        return false;
    }
    snprintf(name, sizeof name, "rg %.24s %s readback", g->name, what);
    ts->readback[h] =
        de_gpu_buffer_create(x->device, &(de_gpu_buffer_desc){.size = (u64)cap * 8u,
                                                              .usage = DE_USAGE_COPY_DST,
                                                              .memory = DE_MEM_READBACK,
                                                              .name = name});
    if (!DE_HANDLE_VALID(ts->readback[h]) || !de_gpu_buffer_map(x->device, ts->readback[h])) {
        destroy_heap(x, ts, h);
        return false;
    }
    ts->heap_cap[h] = cap;
    return true;
}

static u32 heap_of(de_gpu_queue q) {
    return q == DE_QUEUE_COPY ? DE__RG_HEAP_COPY : DE__RG_HEAP_MAIN;
}

static bool timers_on(de_rg *g, de__rg_exec *x) {
    (void)g;
    if (!DE_HANDLE_VALID(x->timers_cvar))
        x->timers_cvar = de_cvar_register(&(de_cvar_desc){
            .name = "r.rg_gpu_timers",
            .help = "Per-pass GPU timers of the render graph (de_render_stats.pass_gpu_ms). On "
                    "Metal every timestamp is a full barrier, so passes of one command list stop "
                    "overlapping while timers are on",
            .type = DE_CVAR_BOOL,
            .default_number = 1,
        });
    return DE_HANDLE_VALID(x->timers_cvar) ? de_cvar_bool(x->timers_cvar) : true;
}

/* ---------- Read back and publish ---------- */

static f32 ticks_ms(de_gpu_device *d, de_gpu_queue q, const u64 *t, u32 begin) {
    const u64 a = t[begin], b = t[begin + 1u];
    if (a == 0 || b == 0 || a == UINT64_MAX || b == UINT64_MAX || b < a)
        return 0.0f; /* not written (Metal reads UINT64_MAX) or wrapped */
    return (f32)de_gpu_timestamp_to_ms(d, q, b - a);
}

/* Size of the published block for `n` passes: name pointers, then times, then queues. */
static size_t pub_block_size(u32 n) {
    return (size_t)n * (sizeof(const char *) + sizeof(f32) + sizeof(de_gpu_queue));
}

static void publish(de_rg *g, de__rg_exec *x, de__rg_tslot *ts) {
    const u64 *ticks[DE__RG_HEAP_COUNT] = {NULL, NULL};
    for (u32 h = 0; h < DE__RG_HEAP_COUNT; ++h)
        if (ts->used[h] && DE_HANDLE_VALID(ts->readback[h]))
            ticks[h] = (const u64 *)de_gpu_buffer_map(x->device, ts->readback[h]);
    const u32 n = ts->pass_count;
    if (n > x->pub_cap) {
        u32 cap = x->pub_cap ? x->pub_cap : 16u;
        while (cap < n)
            cap *= 2u;
        void *b = DE_ALLOC(g->alloc, pub_block_size(cap), 16, DE_MEM_RENDER);
        if (!b) {
            DE_ERROR(g->log, "render graph \"%s\": no memory for the GPU timings", g->name);
            return;
        }
        if (x->pub_block)
            DE_FREE(g->alloc, x->pub_block, pub_block_size(x->pub_cap));
        x->pub_block = b;
        x->pub_cap = cap;
    }
    if (!grow(g, (void **)&x->pub_text, &x->pub_text_cap, ts->names_used ? ts->names_used : 1u, 1,
              0)) {
        DE_ERROR(g->log, "render graph \"%s\": no memory for the GPU timings", g->name);
        return;
    }
    const char **names = (const char **)x->pub_block;
    f32 *ms = (f32 *)(void *)(names + x->pub_cap);
    de_gpu_queue *queues = (de_gpu_queue *)(void *)(ms + x->pub_cap);
    if (ts->names_used)
        memcpy(x->pub_text, ts->names, ts->names_used);
    for (u32 i = 0; i < n; ++i) {
        const de__rg_tpass *tp = &ts->passes[i];
        const u32 hq = heap_of(tp->queue);
        names[i] = x->pub_text + tp->name_off;
        queues[i] = tp->queue;
        ms[i] = tp->query != DE__RG_NONE && ticks[hq]
                    ? ticks_ms(x->device, tp->queue, ticks[hq], tp->query)
                    : 0.0f;
    }
    x->pub.valid = true;
    x->pub.frame_index = ts->frame_index;
    x->pub.pass_count = n;
    x->pub.pass_names = names;
    x->pub.pass_gpu_ms = ms;
    x->pub.pass_queue = queues;
    x->pub.gpu_ms = ticks[DE__RG_HEAP_MAIN]
                        ? ticks_ms(x->device, DE_QUEUE_GRAPHICS, ticks[DE__RG_HEAP_MAIN],
                                   DE__RG_QUERY_FRAME_BEGIN)
                        : 0.0f;
    DE_PROFILE_PLOT("render.rg.gpu_ms", x->pub.gpu_ms);
}

void de__rg_timing_collect(de_rg *g, de__rg_exec *x, bool wait) {
    if (!x || !DE_HANDLE_VALID(x->timeline[DE_QUEUE_GRAPHICS]))
        return;
    const de_gpu_fence tl = x->timeline[DE_QUEUE_GRAPHICS];
    for (;;) {
        /* The oldest pending slot (end values grow with the frames). */
        de__rg_tslot *ts = NULL;
        for (u32 i = 0; i < DE__RG_TIMING_SLOTS; ++i)
            if (x->slots[i].pending && (!ts || x->slots[i].end_value < ts->end_value))
                ts = &x->slots[i];
        if (!ts)
            return;
        if (de_gpu_fence_value(x->device, tl) < ts->end_value) {
            if (!wait)
                return;
            /* A device that stopped (lost) fails the wait: drop the results, never hang. */
            if (de_gpu_fence_wait(x->device, tl, ts->end_value, 10ull * 1000000000ull) != DE_OK) {
                ts->pending = false;
                continue;
            }
        }
        publish(g, x, ts);
        ts->pending = false;
    }
}

/* ---------- Frame start ---------- */

/* Sizes the slot's pass records and name text for the plan. */
static bool size_passes(de_rg *g, de__rg_tslot *ts, const de__rg_plan *p) {
    u32 n = 0, bytes = 0;
    for (u32 s = 0; s < p->step_count; ++s)
        if (p->steps[s].pass != DE__RG_NONE) {
            ++n;
            bytes += (u32)strlen(g->passes[p->steps[s].pass]->name) + 1u;
        }
    if (!grow(g, (void **)&ts->passes, &ts->pass_cap, n, sizeof *ts->passes, 0) ||
        !grow(g, (void **)&ts->names, &ts->names_cap, bytes ? bytes : 1u, 1, 0))
        return false;
    ts->pass_count = n;
    ts->names_used = 0;
    return true;
}

de_result de__rg_timing_begin(de_rg *g, de__rg_exec *x, u64 frame_index) {
    const de__rg_plan *p = g->plan;
    de__rg_timing_collect(g, x, false);
    x->slot = (u32)(x->frames % DE__RG_TIMING_SLOTS);
    de__rg_tslot *ts = &x->slots[x->slot];
    if (ts->pending) {
        /* Still in flight (more frames in flight than slots - 1): wait for this frame only (the
         * newer ones keep running), then read back what has completed. */
        if (de_gpu_fence_wait(x->device, x->timeline[DE_QUEUE_GRAPHICS], ts->end_value,
                              10ull * 1000000000ull) != DE_OK)
            ts->pending = false; /* lost or stuck: drop its results rather than hang */
        de__rg_timing_collect(g, x, false);
        ts->pending = false;
    }
    ts->frame_index = frame_index;
    ts->used[DE__RG_HEAP_MAIN] = ts->used[DE__RG_HEAP_COPY] = 0;
    if (!grow(g, (void **)&x->step_query, &x->step_cap, p->step_count, sizeof *x->step_query, 0) ||
        !size_passes(g, ts, p))
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                       "de__rg_execute: render graph \"%s\": no memory for the GPU timers",
                       g->name);

    /* Queries: two per pass on its queue's heap, plus the frame span on the main heap. */
    const bool on = timers_on(g, x);
    u32 want[DE__RG_HEAP_COUNT] = {2u, 0u};
    for (u32 s = 0; s < p->step_count; ++s)
        if (p->steps[s].pass != DE__RG_NONE)
            want[heap_of(p->steps[s].queue)] += 2u;
    bool have[DE__RG_HEAP_COUNT] = {false, false};
    for (u32 h = 0; h < DE__RG_HEAP_COUNT; ++h)
        have[h] = on && (h == DE__RG_HEAP_MAIN || want[h]) && ensure_heap(g, x, ts, h, want[h]);
    if (have[DE__RG_HEAP_MAIN])
        ts->used[DE__RG_HEAP_MAIN] = 2u; /* the frame span */

    u32 pi = 0;
    for (u32 s = 0; s < p->step_count; ++s) {
        x->step_query[s] = DE__RG_NONE;
        const de__rg_step *st = &p->steps[s];
        if (st->pass == DE__RG_NONE)
            continue;
        const u32 h = heap_of(st->queue);
        if (have[h] && ts->used[h] + 2u <= ts->heap_cap[h]) {
            x->step_query[s] = ts->used[h];
            ts->used[h] += 2u;
        }
        const char *name = g->passes[st->pass]->name;
        const u32 len = (u32)strlen(name) + 1u;
        ts->passes[pi].query = x->step_query[s];
        ts->passes[pi].queue = st->queue;
        ts->passes[pi].name_off = ts->names_used;
        memcpy(ts->names + ts->names_used, name, len);
        ts->names_used += len;
        ++pi;
    }

    /* Breadcrumb names (SPEC §18.5): "<graph>/<pass>" per step of this slot, registered when the
     * slot's plan changes (the device's name table only allocates while it grows). */
    if (ts->bc_steps != p->step_count || ts->bc_hash != p->hash) {
        for (u32 s = 0; s < p->step_count; ++s) {
            if (p->steps[s].pass == DE__RG_NONE)
                continue;
            char name[96];
            snprintf(name, sizeof name, "%.24s/%s", g->name, g->passes[p->steps[s].pass]->name);
            (void)de_gpu_breadcrumb_name(x->device, de__rg_breadcrumb_id(g, x->slot, s), name);
        }
        ts->bc_hash = p->hash;
        ts->bc_steps = p->step_count;
    }
    return DE_OK;
}

bool de__rg_timing_needs_resolve(const de__rg_exec *x, u32 heap) {
    return x && heap < DE__RG_HEAP_COUNT && x->slots[x->slot].used[heap] > 0;
}

de_gpu_query_heap de__rg_timing_heap(const de__rg_exec *x, de_gpu_queue q) {
    const de__rg_tslot *ts = &x->slots[x->slot];
    const u32 h = heap_of(q);
    return ts->used[h] ? ts->heap[h] : (de_gpu_query_heap){0};
}

void de__rg_timing_resolve(de_rg *g, de__rg_exec *x, u32 heap, de_gpu_cmd *cmd) {
    (void)g;
    if (!de__rg_timing_needs_resolve(x, heap) || !cmd)
        return;
    de__rg_tslot *ts = &x->slots[x->slot];
    const de_gpu_queue q = heap == DE__RG_HEAP_COPY ? DE_QUEUE_COPY : DE_QUEUE_GRAPHICS;
    de_gpu_barrier b;
    memset(&b, 0, sizeof b);
    b.buffer = ts->readback[heap];
    b.before = DE_ACCESS_NONE; /* last read by the CPU after an earlier frame completed */
    b.after = DE_ACCESS_COPY_DST;
    b.src_queue = b.dst_queue = q;
    de_gpu_cmd_barriers(cmd, &b, 1);
    de_gpu_cmd_resolve_queries(cmd, ts->heap[heap], 0, ts->used[heap], ts->readback[heap], 0);
}

void de__rg_timing_end(de__rg_exec *x, u64 end_value, f32 cpu_record_ms) {
    de__rg_tslot *ts = &x->slots[x->slot];
    ts->pending = true;
    ts->end_value = end_value;
    x->pub.cpu_record_ms = cpu_record_ms;
    DE_PROFILE_PLOT("render.rg.cpu_record_ms", cpu_record_ms);
}

void de__rg_timing_release(de_rg *g, de__rg_exec *x) {
    for (u32 i = 0; i < DE__RG_TIMING_SLOTS; ++i) {
        de__rg_tslot *ts = &x->slots[i];
        for (u32 h = 0; h < DE__RG_HEAP_COUNT; ++h)
            destroy_heap(x, ts, h);
        free_array(g, (void **)&ts->passes, &ts->pass_cap, sizeof *ts->passes);
        free_array(g, (void **)&ts->names, &ts->names_cap, 1);
        ts->pending = false;
    }
    if (x->pub_block)
        DE_FREE(g->alloc, x->pub_block, pub_block_size(x->pub_cap));
    x->pub_block = NULL;
    x->pub_cap = 0;
    free_array(g, (void **)&x->pub_text, &x->pub_text_cap, 1);
    memset(&x->pub, 0, sizeof x->pub);
}
