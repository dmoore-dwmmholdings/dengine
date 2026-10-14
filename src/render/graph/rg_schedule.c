/*
 * rg_schedule.c: the render graph schedule (SPEC §12.3, DENG-172): steps in a deterministic
 * order, the queue each pass runs on, and the batches (submissions) with the fewest cross-queue
 * waits and signals.
 *
 * Order. The live passes run in declaration order. The builder only makes edges from an earlier
 * pass to a later one, so declaration order is already a topological order, and it is the
 * stable one the issue asks for: nothing depends on a hash map or on sorting.
 *
 * Queues. A pass declared on DE_QUEUE_COMPUTE runs on the async compute queue when the device
 * has one (caps.async_compute) and the graph allows it; otherwise it folds into the graphics
 * queue at its place. Copy passes stay on the copy queue. The prologue (step 0) and the epilogue
 * (the last step) are graphics steps without a pass.
 *
 * Batches. rg_barriers.c gives the cross-queue dependencies (step `to` waits for step `from`).
 * Steps are visited in order with a vector clock per queue: clock[q][q'] = 1 + the last step of
 * q' known to be complete on q. A dependency already covered by the clock (directly or through
 * an earlier wait) needs nothing; otherwise the step waits for the latest needed step of each
 * other queue, and that step gets a signal right after it. A batch starts at a step with waits
 * and ends at a step with a signal, so a signal never waits for later work of its own queue
 * (no deadlock), and each queue transition costs one wait and one signal.
 *
 * Frame joins: the epilogue waits for the last step of every other queue the frame uses, and
 * the first batch of the compute and copy queues waits for the graphics queue's last signal
 * before the frame (wait signal 0). Transient memory and imported resources are shared across
 * frames, so frame N+1's async work must not overlap frame N's graphics work.
 */
#include "render/graph/rg_internal.h"

#include <string.h>

void *de__rg_cgrow(de__rg_cbuild *b, void *old, u32 count, u32 *cap, u32 need, size_t elem) {
    if (need <= *cap)
        return old;
    u32 nc = *cap ? *cap * 2u : 32u;
    while (nc < need)
        nc *= 2u;
    void *p = de_arena_push(b->arena, (size_t)nc * elem, 16);
    if (!p) {
        b->oom = true;
        return NULL;
    }
    if (count)
        memcpy(p, old, (size_t)count * elem);
    *cap = nc;
    return p;
}

bool de__rg_schedule_steps(de__rg_cbuild *b) {
    de_rg *g = b->g;
    u32 live = g->pass_count - g->culled_count;
    b->step_count = live + 2u;
    b->steps = DE_ARENA_PUSH(b->arena, de__rg_step, b->step_count);
    b->step_of_pass = g->pass_count ? DE_ARENA_PUSH(b->arena, u32, g->pass_count) : NULL;
    if (!b->steps || (g->pass_count && !b->step_of_pass))
        return false;
    memset(b->steps, 0, sizeof *b->steps * b->step_count);
    b->steps[0].pass = DE__RG_NONE;
    b->steps[0].queue = DE_QUEUE_GRAPHICS;
    u32 s = 1;
    for (u32 pi = 0; pi < g->pass_count; ++pi) {
        const de_rg_pass *p = g->passes[pi];
        if (p->culled) {
            b->step_of_pass[pi] = DE__RG_NONE;
            continue;
        }
        de_gpu_queue q = p->queue;
        if (q == DE_QUEUE_COMPUTE && !b->async_compute)
            q = DE_QUEUE_GRAPHICS;
        b->steps[s].pass = pi;
        b->steps[s].queue = q;
        b->step_of_pass[pi] = s++;
    }
    DE_ASSERT(s == live + 1u);
    b->steps[s].pass = DE__RG_NONE;
    b->steps[s].queue = DE_QUEUE_GRAPHICS;
    return true;
}

bool de__rg_schedule_batches(de__rg_cbuild *b) {
    const u32 n = b->step_count;
    const u32 Q = DE_QUEUE_COUNT_;
    const u32 epi = n - 1u;

    /* Epilogue joins: the last step of every other queue the frame uses. */
    u32 last_on[DE_QUEUE_COUNT_];
    for (u32 q = 0; q < Q; ++q)
        last_on[q] = DE__RG_NONE;
    for (u32 s = 0; s < epi; ++s)
        last_on[b->steps[s].queue] = s;
    for (u32 q = 0; q < Q; ++q) {
        if (q == DE_QUEUE_GRAPHICS || last_on[q] == DE__RG_NONE)
            continue;
        de__rg_dep *d =
            de__rg_cgrow(b, b->deps, b->dep_count, &b->dep_cap, b->dep_count + 1u, sizeof *d);
        if (!d)
            return false;
        b->deps = d;
        d[b->dep_count++] = (de__rg_dep){last_on[q], epi};
    }

    /* Dependencies grouped by `to` (counting sort; stable, so deterministic). */
    u32 *dep_start = DE_ARENA_PUSH(b->arena, u32, n + 1u);
    de__rg_dep *sorted = b->dep_count ? DE_ARENA_PUSH(b->arena, de__rg_dep, b->dep_count) : NULL;
    b->clock_in = DE_ARENA_PUSH(b->arena, u32, (size_t)n * Q);
    u32 *clock_out = DE_ARENA_PUSH(b->arena, u32, (size_t)n * Q);
    u32 *signal_of = DE_ARENA_PUSH(b->arena, u32, n); /* signal number after the step, 0 = none */
    bool *signal_after = DE_ARENA_PUSH(b->arena, bool, n);
    u32 *wait_first = DE_ARENA_PUSH(b->arena, u32, n); /* pending waits, into wtmp */
    u32 *wait_count = DE_ARENA_PUSH(b->arena, u32, n);
    de__rg_dep *wtmp = DE_ARENA_PUSH(b->arena, de__rg_dep, (size_t)n * Q); /* {from step, -} */
    b->batches = DE_ARENA_PUSH(b->arena, de__rg_batch, n);
    b->batch_steps = DE_ARENA_PUSH(b->arena, u32, n);
    b->waits = DE_ARENA_PUSH(b->arena, de__rg_wait, (size_t)n * Q + Q);
    if (!dep_start || (b->dep_count && !sorted) || !b->clock_in || !clock_out || !signal_of ||
        !signal_after || !wait_first || !wait_count || !wtmp || !b->batches || !b->batch_steps ||
        !b->waits)
        return false;
    memset(dep_start, 0, sizeof *dep_start * (n + 1u));
    for (u32 i = 0; i < b->dep_count; ++i)
        ++dep_start[b->deps[i].to + 1u];
    for (u32 s = 0; s < n; ++s)
        dep_start[s + 1u] += dep_start[s];
    {
        u32 *fill = DE_ARENA_PUSH(b->arena, u32, n);
        if (!fill)
            return false;
        memcpy(fill, dep_start, sizeof *fill * n);
        for (u32 i = 0; i < b->dep_count; ++i)
            sorted[fill[b->deps[i].to]++] = b->deps[i];
    }
    memset(signal_after, 0, sizeof *signal_after * n);
    memset(signal_of, 0, sizeof *signal_of * n);

    /* Vector clocks and the waits each step needs. */
    u32 cur[DE_QUEUE_COUNT_][DE_QUEUE_COUNT_];
    memset(cur, 0, sizeof cur);
    u32 wt = 0;
    for (u32 s = 0; s < n; ++s) {
        const u32 q = b->steps[s].queue;
        u32 need[DE_QUEUE_COUNT_];
        for (u32 k = 0; k < Q; ++k)
            need[k] = DE__RG_NONE;
        for (u32 i = dep_start[s]; i < dep_start[s + 1u]; ++i) {
            const u32 a = sorted[i].from;
            const u32 qa = b->steps[a].queue;
            DE_ASSERT(a < s);
            if (qa == q || cur[q][qa] >= a + 1u)
                continue;
            if (need[qa] == DE__RG_NONE || a > need[qa])
                need[qa] = a;
        }
        wait_first[s] = wt;
        wait_count[s] = 0;
        for (u32 qa = 0; qa < Q; ++qa) {
            const u32 a = need[qa];
            if (a == DE__RG_NONE || cur[q][qa] >= a + 1u) /* covered by an earlier wait */
                continue;
            signal_after[a] = true;
            wtmp[wt++] = (de__rg_dep){a, s};
            ++wait_count[s];
            for (u32 k = 0; k < Q; ++k)
                if (clock_out[(size_t)a * Q + k] > cur[q][k])
                    cur[q][k] = clock_out[(size_t)a * Q + k];
        }
        memcpy(&b->clock_in[(size_t)s * Q], cur[q], sizeof cur[q]);
        cur[q][q] = s + 1u;
        memcpy(&clock_out[(size_t)s * Q], cur[q], sizeof cur[q]);
    }

    /* Batches: a new one at a step with waits, closed by a step with a signal. */
    u32 open[DE_QUEUE_COUNT_];
    for (u32 k = 0; k < Q; ++k)
        open[k] = DE__RG_NONE;
    u32 *batch_of = DE_ARENA_PUSH(b->arena, u32, n);
    if (!batch_of)
        return false;
    b->batch_count = 0;
    memset(b->signal_count, 0, sizeof b->signal_count);
    for (u32 s = 0; s < n; ++s) {
        const u32 q = b->steps[s].queue;
        if (open[q] == DE__RG_NONE || wait_count[s]) {
            de__rg_batch *nb = &b->batches[b->batch_count];
            memset(nb, 0, sizeof *nb);
            nb->queue = (de_gpu_queue)q;
            open[q] = b->batch_count++;
        }
        batch_of[s] = open[q];
        b->steps[s].batch = open[q];
        if (signal_after[s]) {
            signal_of[s] = ++b->signal_count[q];
            b->batches[open[q]].signal = signal_of[s];
            open[q] = DE__RG_NONE;
        }
    }
    /* Steps and waits per batch, in batch order. */
    u32 *fill = DE_ARENA_PUSH(b->arena, u32, b->batch_count);
    if (!fill)
        return false;
    for (u32 s = 0; s < n; ++s)
        ++b->batches[batch_of[s]].step_count;
    u32 at = 0;
    for (u32 i = 0; i < b->batch_count; ++i) {
        b->batches[i].step_first = at;
        fill[i] = at;
        at += b->batches[i].step_count;
    }
    for (u32 s = 0; s < n; ++s)
        b->batch_steps[fill[batch_of[s]]++] = s;
    bool first_seen[DE_QUEUE_COUNT_] = {false};
    b->wait_count = 0;
    for (u32 i = 0; i < b->batch_count; ++i) {
        de__rg_batch *bt = &b->batches[i];
        const u32 s0 = b->batch_steps[bt->step_first];
        bt->wait_first = b->wait_count;
        bool waits_graphics = false;
        for (u32 w = 0; w < wait_count[s0]; ++w) {
            const u32 a = wtmp[wait_first[s0] + w].from;
            DE_ASSERT(signal_of[a] != 0);
            b->waits[b->wait_count++] = (de__rg_wait){b->steps[a].queue, signal_of[a]};
            waits_graphics |= b->steps[a].queue == DE_QUEUE_GRAPHICS;
        }
        /* Frame-entry join of the async queues (implied by an in-frame graphics wait). */
        if (bt->queue != DE_QUEUE_GRAPHICS && !first_seen[bt->queue] && !waits_graphics)
            b->waits[b->wait_count++] = (de__rg_wait){DE_QUEUE_GRAPHICS, 0};
        first_seen[bt->queue] = true;
        bt->wait_count = b->wait_count - bt->wait_first;
    }
    return true;
}
