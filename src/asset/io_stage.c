/*
 * io_stage.c: the OPEN and READ stages of the load pipeline (SPEC §11.4, §6; DENG-142): the
 * source (S07: source files; S08 installs the pack reader through de__pipeline_set_source),
 * the size query, the read buffer, and one de_io_request per job on the process I/O threads.
 * See pipeline_internal.h.
 */

#include "asset/pipeline_internal.h"

#include "asset/asset_internal.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "platform/de_atomic.h"

#include <stdint.h>
#include <string.h>

/* ---- The default source: S07 source mode -------------------------------------------------- */

static de_result source_file_open(void *user, de_asset_system *s, const char *vpath, de_asset_id id,
                                  de__source_span *out) {
    (void)user;
    (void)id;
    if (s->desc.mode == DE_ASSET_MODE_PACKED)
        return DE_FAIL(DE_ERR_UNSUPPORTED,
                       "packed mode reads packs, and the pack reader is not available yet");
    de_file *f = NULL;
    de_result r = de_file_open(vpath, DE_FILE_READ, &f);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        return DE_FAIL(r, "cannot open \"%s\": %s", vpath,
                       e != NULL && e->result == r ? e->message : de_result_str(r));
    }
    out->file = f;
    out->offset = 0;
    out->size = de_file_size(f);
    out->handle = NULL;
    return DE_OK;
}

static void source_file_close(void *user, de__source_span *span) {
    (void)user;
    de_file_close(span->file);
    span->file = NULL;
}

const de__pipeline_source *de__pipeline_default_source(void) {
    static const de__pipeline_source k_source = {source_file_open, source_file_close, NULL};
    return &k_source;
}

/* ---- I/O system ---------------------------------------------------------------------------- */

de_result de__io_stage_start(de__pipeline *pl) {
    de_io_desc d = {.thread_count = pl->s->desc.io_threads};
    de_result r = de_io_init(&d);
    if (r == DE_OK) {
        pl->owns_io = true;
        return DE_OK;
    }
    if (r == DE_ERR_ALREADY_EXISTS) { /* the host (or a tool) runs it: use it as it is */
        de_error_clear();
        return DE_OK;
    }
    return r;
}

void de__io_stage_stop(de__pipeline *pl) {
    if (pl->owns_io)
        de_io_shutdown();
    pl->owns_io = false;
}

/* ---- Stages -------------------------------------------------------------------------------- */

static void job_close_span(de__pjob *job) {
    if (job->span_open) {
        job->pl->source.close(job->pl->source.user, &job->span);
        job->span_open = false;
    }
}

static void job_free_buf(de__pjob *job) {
    if (job->buf != NULL)
        DE_FREE(job->pl->alloc, job->buf, (size_t)job->buf_size);
    job->buf = NULL;
    job->buf_size = 0;
}

/* The bytes are in: raw ones go to the loader, zstd ones to the DECODE stage. */
static void io_after_read(de__pjob *job) {
    de__payload_header h;
    de_result r = de__payload_header_load(job->buf, job->buf_size, &h);
    if (r == DE_ERR_NOT_FOUND) {
        job->data = job->buf;
        job->data_size = job->buf_size;
        de__load_stage_begin(job, false);
        return;
    }
    if (r != DE_OK) {
        de__pipeline_job_fail(job, DE_ERR_PARSE, "\"%s\" has an invalid payload header",
                              job->vpath);
        job_free_buf(job);
        de__pipeline_finish(job);
        return;
    }
    job->payload = h;
    if (h.codec == DE__PAYLOAD_RAW) {
        job->data = (const u8 *)job->buf + DE__PAYLOAD_HEADER_SIZE;
        job->data_size = h.raw_size;
        de__load_stage_begin(job, false);
        return;
    }
    de__pjob_set_stage(job, DE__PJOB_DECODE);
    de__pipeline_run(job->pl, DE_TASK_POOL_FRAME, de__decompress_stage_run, job);
}

/* READ finished (DE_IO_CALLBACK_ON_IO_THREAD: an I/O thread, or the thread that cancelled a read
 * that was still queued). */
static void io_complete(de_io_request *req, de_result r, u64 bytes_read) {
    de__pjob *job = (de__pjob *)req->user;
    de__pipeline *pl = job->pl;
    job_close_span(job);
    if (r == DE_OK)
        (void)de_atomic_fetch_add_u64(&pl->stats.reads, 1, DE_ATOMIC_RELAXED);
    else if (r == DE_ERR_CANCELLED)
        (void)de_atomic_fetch_add_u64(&pl->stats.io_cancelled, 1, DE_ATOMIC_RELAXED);
    /* The next queued job may start reading now. */
    de__pipeline_io_done(pl);

    if (r == DE_ERR_CANCELLED || de__pjob_cancelled(job)) {
        job_free_buf(job);
        de__pipeline_finish(job);
        return;
    }
    if (r != DE_OK || bytes_read != job->buf_size) {
        de__pipeline_job_fail(job, r != DE_OK ? r : DE_ERR_IO,
                              "reading \"%s\" failed (%llu of %llu bytes): %s", job->vpath,
                              (unsigned long long)bytes_read, (unsigned long long)job->buf_size,
                              de_result_str(r != DE_OK ? r : DE_ERR_IO));
        job_free_buf(job);
        de__pipeline_finish(job);
        return;
    }
    de__pipeline_progress(job, bytes_read, bytes_read);
    io_after_read(job);
}

/* OPEN: leaves the OPEN/READ window and hands the job back (failure or cancel). */
static void open_abort(de__pjob *job) {
    job_close_span(job);
    job_free_buf(job);
    de__pipeline_io_done(job->pl);
    de__pipeline_finish(job);
}

void de__io_stage_open(void *user) {
    de__pjob *job = (de__pjob *)user;
    de__pipeline *pl = job->pl;
    if (de__pjob_cancelled(job)) {
        open_abort(job);
        return;
    }
    de__source_span span;
    memset(&span, 0, sizeof span);
    de_error_clear();
    de_result r = pl->source.open(pl->source.user, job->s, job->vpath, job->id, &span);
    if (r != DE_OK) {
        const de_error *e = de_error_last();
        de__pipeline_job_fail(job, r, "%s",
                              e != NULL && e->result == r ? e->message : de_result_str(r));
        open_abort(job);
        return;
    }
    job->span = span;
    job->span_open = true;
    /* The size is known: the load API's byte-weighted progress has its total. */
    de__pipeline_progress(job, 0, span.size);

    if (span.size == 0) { /* nothing to read */
        job_close_span(job);
        de__pipeline_io_done(pl);
        io_after_read(job);
        return;
    }
    if (span.size > (u64)SIZE_MAX / 2u) {
        de__pipeline_job_fail(job, DE_ERR_OUT_OF_MEMORY, "\"%s\" is too large (%llu bytes)",
                              job->vpath, (unsigned long long)span.size);
        open_abort(job);
        return;
    }
    job->buf = DE_ALLOC(pl->alloc, (size_t)span.size, 16, DE_MEM_ASSET);
    if (job->buf == NULL) {
        de__pipeline_job_fail(job, DE_ERR_OUT_OF_MEMORY, "out of memory for %llu bytes",
                              (unsigned long long)span.size);
        open_abort(job);
        return;
    }
    job->buf_size = span.size;
    job->io.file = span.file;
    job->io.offset = span.offset;
    job->io.bytes = span.size;
    job->io.dst = job->buf;
    job->io.on_complete = io_complete;
    job->io.user = job;
    job->io.flags = DE_IO_CALLBACK_ON_IO_THREAD;

    /* Submit and enter READ under the lock: cancel (which takes it) either sees the job before
     * the submit, and the submit does not happen, or sees READ and cancels the read. The
     * callback cannot run before the lock is released (it takes the lock first). */
    de_mutex_lock(&pl->lock);
    if (de__pjob_cancelled(job)) {
        de_mutex_unlock(&pl->lock);
        open_abort(job);
        return;
    }
    r = de_io_submit(&job->io);
    if (r == DE_OK)
        de__pjob_set_stage(job, DE__PJOB_READ);
    de_mutex_unlock(&pl->lock);
    if (r != DE_OK) {
        de__pipeline_job_fail(job, r, "cannot read \"%s\": %s", job->vpath, de_result_str(r));
        open_abort(job);
    }
    /* From here the job belongs to the read and its callback. */
}
