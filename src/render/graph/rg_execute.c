/*
 * rg_execute.c: records and submits a compiled render graph (SPEC §12.3, §8; DENG-173). See
 * rg_internal.h ("DENG-173") for the partition, the per-pass instrumentation and the fences;
 * rg_timing.c has the GPU timers.
 *
 * de__rg_execute, after de__rg_compile:
 *   1. reads back earlier frames' timings and prepares this frame's slot (rg_timing.c);
 *   2. cuts every batch of the plan into chunks (one command list each) by a fixed step cost;
 *   3. records the chunks with de_task_parallel_for (grain 1), so each list is recorded by one
 *      thread and the lists' contents never depend on the worker count;
 *   4. on the calling thread, submits the batches in plan order with their lists, waits and
 *      signals mapped onto the graph's timeline fences, then the caller's frame fence.
 * Per-frame memory is on the frame arena (the list pointers, the waits); the partition and the
 * step queries live in the execute state, which only grows.
 */
#include "render/graph/rg_internal.h"

#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "platform/de_atomic.h"

#include <stdio.h>
#include <string.h>

/* Marker colours by queue (0xRRGGBB): graphics blue, compute orange, copy green. */
static const u32 k_queue_color[DE_QUEUE_COUNT_] = {0x3D7DD8u, 0xE08A2Eu, 0x3FA95Bu};

/* Breadcrumb graph tags: one per graph, 1..255 (ids of different graphs on one device differ). */
static de_atomic_u32 g_next_tag;

u32 de__rg_breadcrumb_id(const de_rg *g, u32 slot, u32 step) {
    const u32 tag = g && g->exec ? g->exec->tag : 0u;
    return (tag << 24) | ((slot & 3u) << 22) | (step & 0x3FFFFFu);
}

/* ---------- State ---------- */

static de__rg_exec *exec_get(de_rg *g) {
    if (g->exec)
        return g->exec;
    de__rg_exec *x = (de__rg_exec *)DE_ALLOC(g->alloc, sizeof *x, 16, DE_MEM_RENDER);
    if (!x)
        return NULL;
    memset(x, 0, sizeof *x);
    x->device = g->device;
    x->tag = (de_atomic_fetch_add_u32(&g_next_tag, 1u, DE_ATOMIC_RELAXED) % 255u) + 1u;
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        x->timeline[q] = de_gpu_fence_create(g->device, 0);
        if (!DE_HANDLE_VALID(x->timeline[q])) {
            for (u32 k = 0; k < q; ++k)
                de_gpu_fence_destroy(g->device, x->timeline[k]);
            DE_FREE(g->alloc, x, sizeof *x);
            return NULL;
        }
    }
    g->exec = x;
    return x;
}

static u32 record_cost_target(de__rg_exec *x) {
    if (!DE_HANDLE_VALID(x->cost_cvar))
        x->cost_cvar = de_cvar_register(&(de_cvar_desc){
            .name = "r.rg_record_cost",
            .help = "Render graph recording: the estimated cost one command list holds (a pass "
                    "costs 4, each barrier call 1); smaller values give more lists to record in "
                    "parallel",
            .type = DE_CVAR_INT,
            .default_number = DE__RG_RECORD_COST_DEF,
            .min = 1,
            .max = 1 << 20,
        });
    const i64 v =
        DE_HANDLE_VALID(x->cost_cvar) ? de_cvar_int(x->cost_cvar) : (i64)DE__RG_RECORD_COST_DEF;
    return v < 1 ? 1u : (u32)v;
}

/* ---------- Partition ---------- */

static u32 step_cost(const de__rg_step *st) {
    u32 c = st->pass != DE__RG_NONE ? DE__RG_COST_PASS : DE__RG_COST_EDGE;
    c += st->barrier_count[DE__RG_PRE] ? 1u : 0u;
    c += st->barrier_count[DE__RG_POST] ? 1u : 0u;
    return c;
}

/* Cuts every batch into chunks of consecutive steps (x->chunks). Depends only on the plan and
 * the target, so it is the same on any number of workers. */
static bool partition(de_rg *g, de__rg_exec *x, u32 target) {
    const de__rg_plan *p = g->plan;
    /* At most one chunk per step. */
    if (p->step_count > x->chunk_cap) {
        u32 cap = x->chunk_cap ? x->chunk_cap : 16u;
        while (cap < p->step_count)
            cap *= 2u;
        de__rg_chunk *c = (de__rg_chunk *)DE_ALLOC(g->alloc, sizeof *c * cap, 16, DE_MEM_RENDER);
        if (!c)
            return false;
        if (x->chunks)
            DE_FREE(g->alloc, x->chunks, sizeof *x->chunks * x->chunk_cap);
        x->chunks = c;
        x->chunk_cap = cap;
    }
    u32 n = 0;
    for (u32 bi = 0; bi < p->batch_count; ++bi) {
        const de__rg_batch *b = &p->batches[bi];
        u64 acc = 0;
        for (u32 k = 0; k < b->step_count; ++k) {
            const u32 cost = step_cost(&p->steps[p->batch_steps[b->step_first + k]]);
            if (k == 0 || acc + cost > target) {
                x->chunks[n++] = (de__rg_chunk){
                    .batch = bi, .first = b->step_first + k, .count = 0, .queue = b->queue};
                acc = 0;
            }
            x->chunks[n - 1u].count++;
            acc += cost;
        }
    }
    x->chunk_count = n;
    return true;
}

de_result de__rg_partition_recording(de_rg *g, u32 target, const u32 limits[DE_QUEUE_COUNT_],
                                     const u32 reserved[DE_QUEUE_COUNT_]) {
    if (!g || !g->plan || !target)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "render graph recording partition");
    de__rg_exec *x = exec_get(g);
    if (!x)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "render graph recording state");
    if (limits) {
        u32 batches[DE_QUEUE_COUNT_] = {0};
        for (u32 i = 0; i < g->plan->batch_count; ++i)
            if (g->plan->batches[i].step_count)
                ++batches[g->plan->batches[i].queue];
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
            const u32 extra = reserved ? reserved[q] : 0u;
            if ((u64)batches[q] + extra > limits[q])
                return DE_FAIL(DE_ERR_UNSUPPORTED,
                               "render graph \"%s\": %u irreducible %s recording batches plus %u "
                               "reserved lists exceed native queue capacity %u",
                               g->name, batches[q], de__rg_queue_name((de_gpu_queue)q), extra,
                               limits[q]);
        }
    }
    for (;;) {
        if (!partition(g, x, target))
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "render graph recording partition storage");
        if (!limits)
            return DE_OK;
        u32 lists[DE_QUEUE_COUNT_] = {0};
        for (u32 i = 0; i < x->chunk_count; ++i)
            ++lists[x->chunks[i].queue];
        bool fits = true;
        for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
            fits = fits && (u64)lists[q] + (reserved ? reserved[q] : 0u) <= limits[q];
        if (fits)
            return DE_OK;
        /* Coarsen only inside batches: every step/barrier and all batch waits/signals retain
         * their original order. At UINT32_MAX one chunk holds each nonempty batch. */
        if (target == UINT32_MAX)
            return DE_FAIL(DE_ERR_INTERNAL, "render graph recording capacity did not converge");
        target = target > UINT32_MAX / 2u ? UINT32_MAX : target * 2u;
    }
}

/* ---------- Command-list pool (DENG-415) ---------- */

/* The RHI recycles lists only after observing completion at submit/wait_idle. Reserve
 * enough reusable handles for the timing ring plus one completed-but-uncollected frame
 * (DENG-415). Reserve does not begin backend command buffers: opening the entire warm-up
 * population before submitting would exhaust Metal's 64 non-completed-buffer queue even
 * when the actual frame records far fewer lists. */
static de_result prime_cmd_pool(de_rg *g, de__rg_exec *x, const u32 lists[DE_QUEUE_COUNT_]) {
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        const u32 need = lists[q] * (DE__RG_TIMING_SLOTS + 1u);
        if (need <= x->primed[q])
            continue;
        const de_result r = de_gpu_cmd_reserve(x->device, (de_gpu_queue)q, need);
        if (r != DE_OK)
            return DE_FAIL_WRAP(r,
                                "de__rg_execute: render graph \"%s\": reserving %u command "
                                "lists on the %s queue",
                                g->name, need, de__rg_queue_name((de_gpu_queue)q));
        x->primed[q] = need;
    }
    return DE_OK;
}

/* ---------- Recording ---------- */

typedef struct rec_ctx {
    de_rg *g;
    de__rg_exec *x;
    de_gpu_cmd **cmds;  /* [chunk_count] */
    de_result *results; /* [chunk_count] */
} rec_ctx;

static void record_step(rec_ctx *r, de_gpu_cmd *cmd, u32 s) {
    de_rg *g = r->g;
    de__rg_exec *x = r->x;
    const de__rg_plan *p = g->plan;
    const de__rg_step *st = &p->steps[s];
    if (st->pass == DE__RG_NONE) {
        /* Prologue (step 0) and epilogue (the last step): barriers and the frame span. */
        const de_gpu_query_heap h = de__rg_timing_heap(x, DE_QUEUE_GRAPHICS);
        if (s == 0 && DE_HANDLE_VALID(h))
            de_gpu_cmd_timestamp(cmd, h, DE__RG_QUERY_FRAME_BEGIN);
        de__rg_record_barriers(g, s, DE__RG_PRE, cmd);
        de__rg_record_barriers(g, s, DE__RG_POST, cmd);
        if (s + 1u == p->step_count && DE_HANDLE_VALID(h))
            de_gpu_cmd_timestamp(cmd, h, DE__RG_QUERY_FRAME_END);
        return;
    }
    const de_rg_pass *pass = g->passes[st->pass];
    const u32 q = x->step_query[s];
    const de_gpu_query_heap h =
        q != DE__RG_NONE ? de__rg_timing_heap(x, st->queue) : (de_gpu_query_heap){0};
    de_gpu_cmd_breadcrumb(cmd, de__rg_breadcrumb_id(g, x->slot, s));
    de_gpu_cmd_marker_push(cmd, pass->name, k_queue_color[st->queue]);
    if (DE_HANDLE_VALID(h))
        de_gpu_cmd_timestamp(cmd, h, q);
    de__rg_record_barriers(g, s, DE__RG_PRE, cmd);
    de__rg_execute_pass(g, st->pass, cmd);
    de__rg_record_barriers(g, s, DE__RG_POST, cmd);
    if (DE_HANDLE_VALID(h))
        de_gpu_cmd_timestamp(cmd, h, q + 1u);
    de_gpu_cmd_marker_pop(cmd);
}

static void record_chunks(u32 begin, u32 end, void *user) {
    rec_ctx *r = (rec_ctx *)user;
    const de__rg_plan *p = r->g->plan;
    for (u32 k = begin; k < end; ++k) {
        const de__rg_chunk *c = &r->x->chunks[k];
        /* The chunk's number within its batch. */
        u32 sub = 0;
        while (sub < k && r->x->chunks[k - sub - 1u].batch == c->batch)
            ++sub;
        char name[96];
        snprintf(name, sizeof name, "%.24s b%u.%u", r->g->name, c->batch, sub);
        de_gpu_cmd *cmd = de_gpu_cmd_begin(r->x->device, c->queue, name);
        if (!cmd) {
            const de_error *e = de_error_last();
            r->results[k] = e ? e->result : DE_ERR_INTERNAL;
            r->cmds[k] = NULL;
            continue;
        }
        for (u32 i = 0; i < c->count; ++i)
            record_step(r, cmd, p->batch_steps[c->first + i]);
        r->cmds[k] = cmd;
        r->results[k] = DE_OK;
    }
}

/* ---------- Submission ---------- */

/* The last batch of queue `q` in the plan, or DE__RG_NONE. */
static u32 last_batch_of(const de__rg_plan *p, de_gpu_queue q) {
    u32 last = DE__RG_NONE;
    for (u32 bi = 0; bi < p->batch_count; ++bi)
        if (p->batches[bi].queue == q)
            last = bi;
    return last;
}

/* Begins and records a resolve list on `q` for timing heap `heap`; NULL when nothing to resolve
 * or the list cannot begin (the timings of this frame then read 0). */
static de_gpu_cmd *resolve_list(de_rg *g, de__rg_exec *x, u32 heap, de_gpu_queue q) {
    if (!de__rg_timing_needs_resolve(x, heap))
        return NULL;
    char name[96];
    snprintf(name, sizeof name, "%.24s resolve", g->name);
    de_gpu_cmd *cmd = de_gpu_cmd_begin(x->device, q, name);
    if (cmd)
        de__rg_timing_resolve(g, x, heap, cmd);
    else
        x->slots[x->slot].used[heap] = 0; /* nothing resolved: never read stale values */
    return cmd;
}

de_result de__rg_execute(de_rg *g, const de__rg_execute_desc *desc) {
    if (!g)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__rg_execute: the graph is NULL");
    const de__rg_execute_desc dd = desc ? *desc : (de__rg_execute_desc){0};
    if (!g->plan)
        return DE_FAIL(DE_ERR_BUSY, "de__rg_execute: render graph \"%s\" is not compiled", g->name);
    if (!g->device)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__rg_execute: render graph \"%s\" has no device",
                       g->name);
    const u64 t0 = de_time_now_ns();
    de__rg_exec *x = exec_get(g);
    if (!x)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                       "de__rg_execute: render graph \"%s\": no memory for the execute state",
                       g->name);
    const de__rg_plan *p = g->plan;
    de_result r = de__rg_timing_begin(g, x, dd.frame_index);
    if (r != DE_OK)
        return r;
    if (de_gpu_get_caps(x->device)->backend == DE_GPU_METAL) {
        /* Metal's default native queues admit 64 uncompleted buffers. Record-all-before-submit
         * must leave one slot for the backend's wait/signal-only head and include the resolve
         * lists opened before submission. Reserved front-end handles do not consume slots. */
        const u32 limits[DE_QUEUE_COUNT_] = {64, 64, 64};
        u32 reserved[DE_QUEUE_COUNT_] = {1, 1, 1};
        reserved[DE_QUEUE_GRAPHICS] += de__rg_timing_needs_resolve(x, DE__RG_HEAP_MAIN) ? 1u : 0u;
        reserved[DE_QUEUE_COPY] += de__rg_timing_needs_resolve(x, DE__RG_HEAP_COPY) ? 1u : 0u;
        DE_TRY(de__rg_partition_recording(g, record_cost_target(x), limits, reserved));
    } else {
        DE_TRY(de__rg_partition_recording(g, record_cost_target(x), NULL, NULL));
    }
    const u32 nc = x->chunk_count;
    {
        u32 lists[DE_QUEUE_COUNT_] = {0};
        for (u32 k = 0; k < nc; ++k)
            ++lists[x->chunks[k].queue];
        lists[DE_QUEUE_GRAPHICS] += de__rg_timing_needs_resolve(x, DE__RG_HEAP_MAIN) ? 1u : 0u;
        lists[DE_QUEUE_COPY] += de__rg_timing_needs_resolve(x, DE__RG_HEAP_COPY) ? 1u : 0u;
        r = prime_cmd_pool(g, x, lists);
        if (r != DE_OK)
            return r;
    }
    de_gpu_cmd **cmds = DE_ARENA_PUSH(g->arena, de_gpu_cmd *, nc + 2u);
    de_result *results = DE_ARENA_PUSH(g->arena, de_result, nc);
    u32 max_waits = 1;
    for (u32 bi = 0; bi < p->batch_count; ++bi)
        if (p->batches[bi].wait_count > max_waits)
            max_waits = p->batches[bi].wait_count;
    de_gpu_wait *waits = DE_ARENA_PUSH(g->arena, de_gpu_wait, max_waits);
    de_gpu_cmd **sub = DE_ARENA_PUSH(g->arena, de_gpu_cmd *, nc + 1u);
    if (!cmds || !results || !waits || !sub)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY,
                       "de__rg_execute: render graph \"%s\": the frame arena is full", g->name);

    /* Record. Each chunk is one list on one thread; the chunks of a batch are consecutive. */
    rec_ctx rc = {g, x, cmds, results};
    if (dd.tasks && nc > 1u)
        de_task_parallel_for(dd.tasks, nc, 1u, record_chunks, &rc);
    else
        record_chunks(0, nc, &rc);
    de_gpu_cmd *resolve[DE__RG_HEAP_COUNT] = {
        resolve_list(g, x, DE__RG_HEAP_MAIN, DE_QUEUE_GRAPHICS),
        resolve_list(g, x, DE__RG_HEAP_COPY, DE_QUEUE_COPY),
    };
    const u32 last_graphics = last_batch_of(p, DE_QUEUE_GRAPHICS);
    const u32 last_copy = last_batch_of(p, DE_QUEUE_COPY);

    /* Submit in plan order. Signal n of queue q is base[q] + n; graphics also ends the frame. */
    u64 base[DE_QUEUE_COUNT_];
    memcpy(base, x->value, sizeof base);
    const u64 frame_end = base[DE_QUEUE_GRAPHICS] + p->signal_count[DE_QUEUE_GRAPHICS] + 1u;
    de_result first_error = DE_OK;
    for (u32 k = 0; k < nc; ++k)
        if (results[k] != DE_OK && first_error == DE_OK)
            first_error = DE_FAIL_WRAP(results[k],
                                       "de__rg_execute: render graph \"%s\": command list %u of "
                                       "batch %u did not begin",
                                       g->name, k, x->chunks[k].batch);
    u32 k = 0;
    for (u32 bi = 0; bi < p->batch_count; ++bi) {
        const de__rg_batch *b = &p->batches[bi];
        u32 n = 0;
        while (k < nc && x->chunks[k].batch == bi) {
            if (cmds[k])
                sub[n++] = cmds[k];
            ++k;
        }
        if (bi == last_graphics && resolve[DE__RG_HEAP_MAIN])
            sub[n++] = resolve[DE__RG_HEAP_MAIN];
        if (bi == last_copy && resolve[DE__RG_HEAP_COPY])
            sub[n++] = resolve[DE__RG_HEAP_COPY];
        for (u32 w = 0; w < b->wait_count; ++w) {
            const de__rg_wait *pw = &p->waits[b->wait_first + w];
            waits[w] = (de_gpu_wait){x->timeline[pw->queue], base[pw->queue] + pw->signal};
        }
        de_gpu_submit s = {
            .cmds = sub, .cmd_count = n, .waits = waits, .wait_count = b->wait_count};
        if (b->signal) {
            s.signal = x->timeline[b->queue];
            s.signal_value = base[b->queue] + b->signal;
        } else if (bi == last_graphics) {
            s.signal = x->timeline[DE_QUEUE_GRAPHICS];
            s.signal_value = frame_end;
        }
        DE_ASSERT(!(b->signal && bi == last_graphics)); /* the epilogue never signals in-frame */
        const de_result sr = de_gpu_queue_submit(x->device, b->queue, &s);
        if (sr != DE_OK && first_error == DE_OK)
            first_error = DE_FAIL_WRAP(
                sr, "de__rg_execute: render graph \"%s\": submit of batch %u", g->name, bi);
    }
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
        x->value[q] = base[q] + p->signal_count[q];
    x->value[DE_QUEUE_GRAPHICS] = frame_end;
    if (DE_HANDLE_VALID(dd.frame_fence)) {
        const de_gpu_submit s = {.signal = dd.frame_fence, .signal_value = dd.frame_value};
        const de_result sr = de_gpu_queue_submit(x->device, DE_QUEUE_GRAPHICS, &s);
        if (sr != DE_OK && first_error == DE_OK)
            first_error =
                DE_FAIL_WRAP(sr, "de__rg_execute: render graph \"%s\": frame fence", g->name);
    }
    ++x->frames;
    de__rg_timing_end(x, frame_end, (f32)((f64)(de_time_now_ns() - t0) / 1e6));
    return first_error;
}

/* ---------- Queries and shutdown ---------- */

bool de__rg_frame_end(const de_rg *g, de_gpu_fence *fence, u64 *value) {
    if (!g || !g->exec || !g->exec->frames)
        return false;
    if (fence)
        *fence = g->exec->timeline[DE_QUEUE_GRAPHICS];
    if (value)
        *value = g->exec->value[DE_QUEUE_GRAPHICS];
    return true;
}

const de__rg_timings *de__rg_timings_get(const de_rg *g) {
    static const de__rg_timings none = {0};
    return g && g->exec ? &g->exec->pub : &none;
}

void de__rg_fill_stats(const de_rg *g, de_render_stats *s) {
    if (!s)
        return;
    const de__rg_timings *t = de__rg_timings_get(g);
    s->gpu_ms = t->gpu_ms;
    s->cpu_record_ms = t->cpu_record_ms;
    s->pass_count = t->pass_count;
    s->pass_names = t->pass_names;
    s->pass_gpu_ms = t->pass_gpu_ms;
}

const de__rg_chunk *de__rg_exec_chunks(const de_rg *g, u32 *count) {
    if (count)
        *count = g && g->exec ? g->exec->chunk_count : 0u;
    return g && g->exec ? g->exec->chunks : NULL;
}

void de__rg_execute_shutdown(de_rg *g) {
    if (!g || !g->exec)
        return;
    de__rg_exec *x = g->exec;
    /* The graph's lists may still run: wait for the last frame before the heaps go. */
    if (x->frames)
        (void)de_gpu_fence_wait(x->device, x->timeline[DE_QUEUE_GRAPHICS],
                                x->value[DE_QUEUE_GRAPHICS], 10ull * 1000000000ull);
    de__rg_timing_release(g, x);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q)
        if (DE_HANDLE_VALID(x->timeline[q]))
            de_gpu_fence_destroy(x->device, x->timeline[q]);
    if (x->chunks)
        DE_FREE(g->alloc, x->chunks, sizeof *x->chunks * x->chunk_cap);
    if (x->step_query)
        DE_FREE(g->alloc, x->step_query, sizeof *x->step_query * x->step_cap);
    DE_FREE(g->alloc, x, sizeof *x);
    g->exec = NULL;
}
