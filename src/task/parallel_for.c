/*
 * parallel_for.c: de_task_parallel_for, fixed-chunk parallel loops (SPEC §8, §5.7 rule 5).
 *
 * Chunk k covers [k * grain, min((k + 1) * grain, count)). The boundaries depend only on
 * (count, grain), so a deterministic fn that writes per-chunk results (indexed by
 * begin / grain) and a fixed-order reduction over them give the same answer on every machine
 * and worker count. Which thread runs which chunk is not deterministic.
 *
 * The job lives on the caller's stack. Chunks are claimed through one atomic u64 index (u64 so
 * the extra claims past the end can never wrap back into range). The caller submits at most one
 * helper task per other frame worker, then claims chunks itself; each helper claims chunks until
 * none are left. The caller then waits on the helpers' counter (de_task_wait helps meanwhile;
 * a helper that starts after the last claim returns at once). So the job outlives every helper,
 * nothing is allocated per call, and a nested parallel_for inside a chunk or a graph node is just
 * a wait that helps (see the wait-and-help note in graph.c).
 */
#include "task/task_internal.h"

typedef struct de__pfor_job {
    de_range_fn fn;
    void *user;
    u64 count;
    u64 grain;
    u64 chunks;
    de_atomic_u64 next; /* next unclaimed chunk */
} de__pfor_job;

static void de__pfor_drain(de__pfor_job *j) {
    for (;;) {
        /* Relaxed: the chunks' writes are published by the helpers' counter (acq_rel). */
        const u64 k = de_atomic_fetch_add_u64(&j->next, 1, DE_ATOMIC_RELAXED);
        if (k >= j->chunks)
            return;
        const u64 begin = k * j->grain;
        const u64 end = begin + j->grain < j->count ? begin + j->grain : j->count;
        j->fn((u32)begin, (u32)end, j->user);
    }
}

static void de__pfor_helper(void *p) {
    de__pfor_drain((de__pfor_job *)p);
}

u32 de__task_parallel_for_auto_grain(const de_task_system *ts, u32 count) {
    const u32 g = count / (ts->frame_count * 4u);
    return g > 0 ? g : 1u;
}

void de_task_parallel_for(de_task_system *ts, u32 count, u32 grain, de_range_fn fn, void *user) {
    DE_ASSERT(ts != NULL && fn != NULL);
    if (count == 0)
        return;
    if (grain == 0)
        grain = de__task_parallel_for_auto_grain(ts, count);
    const u64 chunks = ((u64)count - 1u) / grain + 1u;
    if (chunks == 1) {
        fn(0, count, user);
        return;
    }
    /* One helper per frame worker other than the caller (all of them when the caller is not a
     * frame worker of `ts`), and never more helpers than the chunks left for them. */
    const de__task_slot *self = de__task_current_slot(ts);
    const u32 others =
        (self != NULL && self->kind == DE__SLOT_FRAME) ? ts->frame_count - 1 : ts->frame_count;
    const u32 helpers = chunks - 1 < others ? (u32)(chunks - 1) : others;

    de__pfor_job job;
    job.fn = fn;
    job.user = user;
    job.count = count;
    job.grain = grain;
    job.chunks = chunks;
    de_atomic_init_u64(&job.next, 0);
    de_task_counter done = {0};
    for (u32 i = 0; i < helpers; ++i)
        de_task_submit(ts, DE_TASK_POOL_FRAME, de__pfor_helper, &job, &done);
    de__pfor_drain(&job);
    de_task_wait(ts, &done);
}
