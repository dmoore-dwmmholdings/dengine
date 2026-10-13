/*
 * pack_stage.c: the pack read path of the load pipeline (SPEC §11.4, §11.7; DENG-212): the plan
 * at submit, merged chunk-run reads on the I/O threads, and per-chunk decoding on frame workers.
 * See mount_internal.h for the stages.
 *
 * Counting. A job has `run_count` runs. `runs_left` counts the reads still out: the read that
 * takes it to 0 leaves the pipeline's OPEN/READ window (de__pipeline_io_done). `pending` counts
 * the runs not finished yet, where a run is finished when its chunks are decoded (or, for a job
 * whose chunks are all raw, when it is read, or at once when it failed or the job is cancelled).
 * The thread that takes `pending` to 0 hands the job on: to the LOAD stage, or back to the main
 * thread on a failure or cancel. The first failure wins the job's message (`failed` CAS); the
 * other threads only stop.
 */

#include "asset/mount_internal.h"

#include "asset/asset_internal.h"
#include "asset/dpak/dpak_reader.h"
#include "asset/pipeline_internal.h"
#include "asset/resolve_internal.h"
#include "asset/slots_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "platform/de_atomic.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* One de_io read: chunks [first, first + count) of the entry, `bytes` stored bytes at `offset`
 * from the entry's data_offset. */
typedef struct de__pack_run {
    de_io_request io;
    struct de__pack_job *pj;
    u32 first, count;
    u64 offset, bytes;
} de__pack_run;

struct de__pack_job {
    de__pjob *job;
    de__pack *pack; /**< referenced from attach to free (main thread) */
    dpak_toc_entry toc;
    bool compressed; /**< some chunk is a zstd frame: read into job->buf, decode into job->raw */
    de_atomic_u32 runs_left, pending, failed;
    de_atomic_u64 bytes_done;
    u32 run_count;
    size_t block_size;
    de__pack_run *runs; /**< in the same block, after this struct */
};

/* ---- Plan and attach (main thread) ----------------------------------------------------------- */

static void plan_fail(de__pack_plan *p, de_result code, const char *fmt, ...) DE_PRINTF_FMT(3, 4);
static void plan_fail(de__pack_plan *p, de_result code, const char *fmt, ...) {
    p->error = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->msg, sizeof p->msg, fmt, ap);
    va_end(ap);
}

bool de__pack_stage_plan(de_asset_system *s, u32 slot, de__pack_plan *p) {
    const de__slot *sl = de__slot_get(s, slot);
    if (sl == NULL)
        return false;
    bool packed = s->desc.mode == DE_ASSET_MODE_PACKED;
    const de__manifest_asset *ma = de__resolved_asset(s->resolved, sl->id);
    p->error = DE_OK;
    p->msg[0] = '\0';
    if (!de__mount_lookup(s, sl->id, &p->where)) {
        if (!packed)
            return false; /* SOURCE mode: the manifest source (or the cooker) serves it */
        if (ma != NULL)
            plan_fail(p, DE_ERR_NOT_FOUND, "asset \"%s\" is in no mounted pack", ma->id);
        else
            plan_fail(p, DE_ERR_NOT_FOUND, "asset %016llx is in no mounted pack",
                      (unsigned long long)sl->id.hash);
        return true;
    }
    const dpak_reader *r = p->where.pack->reader;

    /* The name for messages and de_asset_load_ctx: the manifest's, the pack's, or the hex ID. */
    const char *name = ma != NULL ? ma->id : dpak_name(r, p->where.index, NULL);
    if (name != NULL)
        snprintf(p->id_buf, sizeof p->id_buf, "%s", name);
    else
        snprintf(p->id_buf, sizeof p->id_buf, "%016llx", (unsigned long long)sl->id.hash);
    if (ma != NULL) {
        p->asset = *ma; /* SOURCE mode: the manifest settings still reach the loader */
    } else {
        memset(&p->asset, 0, sizeof p->asset);
        p->asset.type = "";
        p->asset.source = "";
        p->asset.file = "";
    }
    p->asset.id = p->id_buf;
    snprintf(p->vpath, sizeof p->vpath, "%s", p->where.pack->vpath);

    if (p->where.toc.type_hash != sl->type_hash) {
        plan_fail(p, DE_ERR_INVALID_ARGUMENT,
                  "asset \"%s\": pack \"%s\" holds it as another type (type hash %016llx, the "
                  "asset's is %016llx)",
                  p->id_buf, p->vpath, (unsigned long long)p->where.toc.type_hash,
                  (unsigned long long)sl->type_hash);
        return true;
    }
    p->type = de__asset_type_get(s, de__slot_type(s, sl));
    if (p->type == NULL) {
        plan_fail(p, DE_ERR_UNSUPPORTED,
                  "asset \"%s\": no registered asset type has the type hash %016llx", p->id_buf,
                  (unsigned long long)sl->type_hash);
        return true;
    }
    if (p->type->load == NULL) {
        plan_fail(p, DE_ERR_UNSUPPORTED, "asset \"%s\": type \"%s\" has no loader", p->id_buf,
                  p->type->name);
        return true;
    }
    if (p->where.toc.raw_size > (u64)SIZE_MAX / 2u ||
        p->where.toc.stored_size > (u64)SIZE_MAX / 2u) {
        plan_fail(p, DE_ERR_OUT_OF_MEMORY, "asset \"%s\" is too large (%llu bytes)", p->id_buf,
                  (unsigned long long)p->where.toc.raw_size);
        return true;
    }
    return true;
}

/* The runs of an entry: contiguous chunks, at most DE__PACK_RUN_MAX_BYTES each (one chunk may be
 * a run of its own when it is larger). Writes them to `runs` when not NULL; returns the count. */
static u32 plan_runs(const dpak_reader *r, const dpak_toc_entry *e, de__pack_run *runs) {
    u32 n = 0;
    u64 run_bytes = 0;
    for (u32 k = 0; k < e->chunk_count; ++k) {
        dpak_chunk_entry c;
        dpak_chunk(r, e->first_chunk + k, &c);
        if (n == 0 || run_bytes + c.stored_size > DE__PACK_RUN_MAX_BYTES) {
            if (runs != NULL) {
                de__pack_run *run = &runs[n];
                memset(run, 0, sizeof *run);
                run->first = k;
                run->offset = c.file_offset - e->data_offset;
            }
            n++;
            run_bytes = 0;
        }
        run_bytes += c.stored_size;
        if (runs != NULL) {
            runs[n - 1u].count++;
            runs[n - 1u].bytes = run_bytes;
        }
    }
    return n;
}

static size_t align_up(size_t v, size_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

de_result de__pack_stage_attach(de__pjob *job, const de__pack_plan *p) {
    de__pipeline *pl = job->pl;
    const dpak_reader *r = p->where.pack->reader;
    const dpak_toc_entry *e = &p->where.toc;
    u32 runs = plan_runs(r, e, NULL);
    size_t off_runs = align_up(sizeof(de__pack_job), DE_ALIGNOF(de__pack_run));
    size_t size = off_runs + sizeof(de__pack_run) * runs;
    u8 *block = (u8 *)DE_ALLOC(pl->alloc, size, DE_ALIGNOF(de__pack_run), DE_MEM_ASSET);
    if (block == NULL)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset \"%s\": out of memory", p->id_buf);
    de__pack_job *pj = (de__pack_job *)block;
    memset(pj, 0, sizeof *pj);
    pj->job = job;
    pj->toc = *e;
    pj->compressed = (e->flags & DPAK_ENTRY_UNCOMPRESSED) == 0;
    pj->run_count = runs;
    pj->block_size = size;
    pj->runs = runs != 0 ? (de__pack_run *)(block + off_runs) : NULL;
    (void)plan_runs(r, e, pj->runs);
    for (u32 i = 0; i < runs; ++i)
        pj->runs[i].pj = pj;
    de_atomic_init_u32(&pj->runs_left, runs);
    de_atomic_init_u32(&pj->pending, runs);
    de_atomic_init_u32(&pj->failed, 0);
    de_atomic_init_u64(&pj->bytes_done, 0);

    /* The TOC's dependencies are the job's first ones; the loader may add more (deduplicated). */
    if (e->dep_count != 0) {
        job->deps = (de_asset_id *)DE_ALLOC(pl->alloc, sizeof(de_asset_id) * e->dep_count,
                                            DE_ALIGNOF(de_asset_id), DE_MEM_ASSET);
        if (job->deps == NULL) {
            DE_FREE(pl->alloc, block, size);
            return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "asset \"%s\": out of memory", p->id_buf);
        }
        job->dep_capacity = e->dep_count;
        for (u32 i = 0; i < e->dep_count; ++i) {
            de_asset_id id = {dpak_dep(r, e->dep_first + i)};
            bool dup = id.hash == 0;
            for (u32 k = 0; k < job->dep_count && !dup; ++k)
                dup = job->deps[k].hash == id.hash;
            if (!dup)
                job->deps[job->dep_count++] = id;
        }
    }
    pj->pack = p->where.pack;
    de__mount_pack_ref(pj->pack);
    job->pack = pj;
    de__mount_count(job->s, 1, 0, 0, 0);
    return DE_OK;
}

void de__pack_stage_free(de__pjob *job) {
    de__pack_job *pj = job->pack;
    if (pj == NULL)
        return;
    job->pack = NULL;
    de__mount_pack_release(job->s, pj->pack);
    DE_FREE(job->pl->alloc, pj, pj->block_size);
}

void de__pack_stage_cancel(de__pjob *job) {
    de__pack_job *pj = job->pack;
    /* Every run was submitted before the job entered READ (under the pipeline lock); a read that
     * finished already ignores the cancel. The job and its runs live until the main thread frees
     * them, so the requests are valid here. */
    for (u32 i = 0; pj != NULL && i < pj->run_count; ++i)
        de_io_cancel(&pj->runs[i].io);
}

/* ---- Stage threads --------------------------------------------------------------------------- */

static bool job_failed(const de__pack_job *pj) {
    return de_atomic_load_u32(&pj->failed, DE_ATOMIC_ACQUIRE) != 0;
}

/* Records the first failure of the job; later ones are dropped. */
static void fail_once(de__pack_job *pj, de_result code, const char *fmt, ...) DE_PRINTF_FMT(3, 4);
static void fail_once(de__pack_job *pj, de_result code, const char *fmt, ...) {
    u32 expected = 0;
    if (!de_atomic_cas_u32(&pj->failed, &expected, 1, DE_ATOMIC_ACQ_REL, DE_ATOMIC_ACQUIRE))
        return;
    char msg[DE__LOAD_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    de__pipeline_job_fail(pj->job, code, "%s", msg);
}

static void free_bytes(de__pjob *job) {
    de__pipeline *pl = job->pl;
    if (job->buf != NULL)
        DE_FREE(pl->alloc, job->buf, (size_t)job->buf_size);
    if (job->raw != NULL)
        DE_FREE(pl->alloc, job->raw, (size_t)(job->raw_size != 0 ? job->raw_size : 1u));
    job->buf = job->raw = NULL;
    job->buf_size = job->raw_size = 0;
    job->data = NULL;
    job->data_size = 0;
}

/* Every run is finished: hand the job on. The caller does not touch it afterwards. */
static void job_done(de__pack_job *pj) {
    de__pjob *job = pj->job;
    if (job_failed(pj) || de__pjob_cancelled(job)) {
        free_bytes(job);
        de__pipeline_finish(job);
        return;
    }
    de__pipeline *pl = job->pl;
    if (job->buf != NULL) /* the stored bytes are decoded: only the raw ones go on */
        DE_FREE(pl->alloc, job->buf, (size_t)job->buf_size);
    job->buf = NULL;
    job->buf_size = 0;
    job->data = job->raw;
    job->data_size = pj->toc.raw_size;
    de__load_stage_begin(job, de_task_worker_index() != DE_TASK_WORKER_NONE);
}

static void pending_done(de__pack_job *pj) {
    if (de_atomic_fetch_sub_u32(&pj->pending, 1, DE_ATOMIC_ACQ_REL) == 1u)
        job_done(pj);
}

/* Decodes chunks [begin, end) of a run (de_range_fn: a frame worker, or the run's thread). */
static void decode_range(u32 begin, u32 end, void *user) {
    de__pack_run *run = (de__pack_run *)user;
    de__pack_job *pj = run->pj;
    de__pjob *job = pj->job;
    const dpak_reader *r = pj->pack->reader;
    u64 cs = dpak_get_header(r)->chunk_size;
    bool spare = false;
    void *dctx = de__decompress_dctx_take(job->pl, &spare);
    if (dctx == NULL) {
        fail_once(pj, DE_ERR_OUT_OF_MEMORY, "out of memory for a zstd decoder");
        return;
    }
    u32 done = 0;
    for (u32 i = begin; i < end; ++i) {
        if (job_failed(pj) || de__pjob_cancelled(job))
            break;
        u32 k = run->first + i;
        dpak_chunk_entry c;
        dpak_chunk(r, pj->toc.first_chunk + k, &c);
        const u8 *src = (const u8 *)job->buf + (c.file_offset - pj->toc.data_offset);
        u8 *dst = (u8 *)job->raw + (u64)k * cs;
        de_error_clear();
        if (dpak_decode_chunk(r, dctx, &c, src, dst) != DE_OK) {
            const de_error *e = de_error_last();
            fail_once(pj, DE_ERR_PARSE, "%s", e != NULL ? e->message : "corrupt chunk");
            break;
        }
        done++;
    }
    de__decompress_dctx_give(job->pl, dctx, spare);
    de__mount_count(job->s, 0, 0, done, 0);
}

/* DECODE of one run (frame task). */
static void run_decode(void *user) {
    de__pack_run *run = (de__pack_run *)user;
    de__pack_job *pj = run->pj;
    de__pjob *job = pj->job;
    if (!job_failed(pj) && !de__pjob_cancelled(job)) {
        if (job->pl->tasks != NULL && run->count > 1u)
            de_task_parallel_for(job->pl->tasks, run->count, 1, decode_range, run);
        else
            decode_range(0, run->count, run);
    }
    pending_done(pj);
}

/* Debug and Dev mounts check chunk hashes (dpak_open_desc.check_chunks, DENG-392). The chunks of
 * an all-raw entry are read straight into the raw buffer and never pass dpak_decode_chunk, so
 * their hashes are checked here, on the I/O thread. Returns false (and fails the job) on a
 * damaged chunk. */
static bool check_raw_run(de__pack_run *run) {
    de__pack_job *pj = run->pj;
    de__pjob *job = pj->job;
    const dpak_reader *r = pj->pack->reader;
    if (!dpak_checks_chunks(r))
        return true;
    for (u32 i = 0; i < run->count; ++i) {
        dpak_chunk_entry c;
        dpak_chunk(r, pj->toc.first_chunk + run->first + i, &c);
        const u8 *src = (const u8 *)job->raw + (c.file_offset - pj->toc.data_offset);
        de_error_clear();
        if (dpak_check_chunk(r, &c, src) != DE_OK) {
            const de_error *e = de_error_last();
            fail_once(pj, DE_ERR_PARSE, "%s", e != NULL ? e->message : "damaged chunk");
            return false;
        }
    }
    return true;
}

/* A run's read is over (read, failed, cancelled, or never submitted): leave the I/O window with
 * the last one, then decode it or count it finished. */
static void run_finished(de__pack_run *run, bool ok) {
    de__pack_job *pj = run->pj;
    de__pjob *job = pj->job;
    de__pipeline *pl = job->pl;
    if (de_atomic_fetch_sub_u32(&pj->runs_left, 1, DE_ATOMIC_ACQ_REL) == 1u) {
        de__pjob_set_stage(job, DE__PJOB_DECODE);
        de__pipeline_io_done(pl);
    }
    if (ok && pj->compressed && !job_failed(pj) && !de__pjob_cancelled(job)) {
        /* On the pipeline's counter, like every pipeline task (shutdown waits on it). */
        if (pl->tasks != NULL)
            de_task_submit(pl->tasks, DE_TASK_POOL_FRAME, run_decode, run, &pl->task_counter);
        else
            run_decode(run);
        return;
    }
    if (ok && !pj->compressed && check_raw_run(run))
        de__mount_count(job->s, 0, 0, run->count, 0); /* raw chunks need no decode */
    pending_done(pj);
}

/* READ finished (DE_IO_CALLBACK_ON_IO_THREAD). */
static void run_complete(de_io_request *req, de_result r, u64 bytes_read) {
    de__pack_run *run = (de__pack_run *)req->user;
    de__pack_job *pj = run->pj;
    de__pjob *job = pj->job;
    de__pipeline *pl = job->pl;
    /* The submitting thread holds the lock until every run is out and the job is in READ: wait
     * for it, so this job cannot be finished (and freed) before that. */
    de_mutex_lock(&pl->lock);
    de_mutex_unlock(&pl->lock);
    if (r == DE_OK)
        (void)de_atomic_fetch_add_u64(&pl->stats.reads, 1, DE_ATOMIC_RELAXED);
    else if (r == DE_ERR_CANCELLED)
        (void)de_atomic_fetch_add_u64(&pl->stats.io_cancelled, 1, DE_ATOMIC_RELAXED);
    bool ok = r == DE_OK && bytes_read == run->bytes;
    if (!ok && r != DE_ERR_CANCELLED && !de__pjob_cancelled(job))
        fail_once(pj, r != DE_OK ? r : DE_ERR_IO,
                  "reading pack \"%s\" failed (%llu of %llu bytes at %llu): %s", job->vpath,
                  (unsigned long long)bytes_read, (unsigned long long)run->bytes,
                  (unsigned long long)(pj->toc.data_offset + run->offset),
                  de_result_str(r != DE_OK ? r : DE_ERR_IO));
    if (ok) {
        de__mount_count(job->s, 0, 0, 0, bytes_read);
        u64 done =
            de_atomic_fetch_add_u64(&pj->bytes_done, bytes_read, DE_ATOMIC_ACQ_REL) + bytes_read;
        de__pipeline_progress(job, done, pj->toc.stored_size);
    }
    run_finished(run, ok);
}

/* OPEN gave up before any read: out of the I/O window, back to the main thread. */
static void open_abort(de__pjob *job) {
    free_bytes(job);
    de__pipeline_io_done(job->pl);
    de__pipeline_finish(job);
}

void de__pack_stage_open(void *user) {
    de__pjob *job = (de__pjob *)user;
    de__pack_job *pj = job->pack;
    de__pipeline *pl = job->pl;
    if (de__pjob_cancelled(job)) {
        open_abort(job);
        return;
    }
    const dpak_toc_entry *e = &pj->toc;
    de__pipeline_progress(job, 0, e->stored_size);
    if (pj->run_count == 0) { /* an empty asset: nothing to read */
        de__pipeline_io_done(pl);
        job->data = NULL;
        job->data_size = 0;
        de__load_stage_begin(job, de_task_worker_index() != DE_TASK_WORKER_NONE);
        return;
    }
    /* One raw buffer, sized once; the stored bytes go to a second one only when some chunk is
     * compressed (else they are the raw bytes). */
    job->raw = DE_ALLOC(pl->alloc, (size_t)e->raw_size, 16, DE_MEM_ASSET);
    if (job->raw != NULL)
        job->raw_size = e->raw_size;
    if (job->raw != NULL && pj->compressed) {
        job->buf = DE_ALLOC(pl->alloc, (size_t)e->stored_size, 16, DE_MEM_ASSET);
        if (job->buf != NULL)
            job->buf_size = e->stored_size;
    }
    if (job->raw == NULL || (pj->compressed && job->buf == NULL)) {
        de__pipeline_job_fail(job, DE_ERR_OUT_OF_MEMORY, "out of memory for %llu bytes",
                              (unsigned long long)(e->raw_size + e->stored_size));
        open_abort(job);
        return;
    }
    de_file *f = dpak_file(pj->pack->reader);
    u8 *dst = pj->compressed ? (u8 *)job->buf : (u8 *)job->raw;
    for (u32 i = 0; i < pj->run_count; ++i) {
        de__pack_run *run = &pj->runs[i];
        memset(&run->io, 0, sizeof run->io);
        run->io.file = f;
        run->io.offset = e->data_offset + run->offset;
        run->io.bytes = run->bytes;
        run->io.dst = dst + run->offset;
        run->io.on_complete = run_complete;
        run->io.user = run;
        run->io.priority = job->priority;
        run->io.flags = DE_IO_CALLBACK_ON_IO_THREAD;
    }

    /* Every run is submitted, and the job enters READ, under the lock: cancel (which takes it)
     * either sees the job before any submit, and nothing is submitted, or sees READ with every
     * run out, and cancels them all. Callbacks wait for the lock (run_complete). */
    de_mutex_lock(&pl->lock);
    if (de__pjob_cancelled(job)) {
        de_mutex_unlock(&pl->lock);
        open_abort(job);
        return;
    }
    /* Once every run is out and the lock is released, a callback may finish the job and the
     * main thread free it: read what is needed afterwards now, and count under the lock. */
    de__pack_run *runs = pj->runs;
    u32 n = pj->run_count;
    u32 submitted = 0;
    de_result r = DE_OK;
    for (; submitted < n; ++submitted) {
        r = de_io_submit(&runs[submitted].io);
        if (r != DE_OK)
            break;
    }
    if (submitted != 0)
        de__pjob_set_stage(job, DE__PJOB_READ);
    de__mount_count(job->s, 0, submitted, 0, 0);
    de_mutex_unlock(&pl->lock);
    if (submitted == n)
        return; /* the job belongs to the reads and their callbacks now */

    /* A submit failed: the runs that are out finish on their own; the rest finish here (the job
     * lives until the last of them). The last run_finished may hand the job on. */
    fail_once(pj, r, "cannot read pack \"%s\": %s", job->vpath, de_result_str(r));
    for (u32 i = submitted; i < n; ++i)
        run_finished(&runs[i], false);
}
