/*
 * decompress_stage.c: the DECODE stage of the load pipeline (SPEC §11.4; DENG-142): payloads
 * tagged zstd (de__payload header, pipeline_internal.h) are decompressed on a frame worker.
 *
 * Decoder contexts: one ZSTD_DCtx per frame worker index (de_task_worker_index), created the
 * first time that worker decodes and kept until shutdown, so a decode allocates only its output.
 * A thread that is not a frame worker (no task system: the I/O thread runs the stage inline)
 * borrows a context from a small spare list under `dctx_lock`. zstd allocates through the
 * system allocator (ZSTD_customMem), so tracking allocators see its memory.
 */

#include "asset/pipeline_internal.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "platform/de_atomic.h"

#define ZSTD_STATIC_LINKING_ONLY /* ZSTD_customMem, ZSTD_createDCtx_advanced */
#include <zstd.h>

#include <string.h>

/* zstd's allocations: a 16-byte prefix keeps the size for DE_FREE. */
#define ZS_PREFIX 16u

static void *zs_alloc(void *opaque, size_t size) {
    const de__pipeline *pl = (const de__pipeline *)opaque;
    u8 *p = (u8 *)DE_ALLOC(pl->alloc, size + ZS_PREFIX, 16, DE_MEM_ASSET);
    if (p == NULL)
        return NULL;
    memcpy(p, &size, sizeof size);
    return p + ZS_PREFIX;
}

static void zs_free(void *opaque, void *address) {
    if (address == NULL)
        return;
    const de__pipeline *pl = (const de__pipeline *)opaque;
    u8 *p = (u8 *)address - ZS_PREFIX;
    size_t size = 0;
    memcpy(&size, p, sizeof size);
    DE_FREE(pl->alloc, p, size + ZS_PREFIX);
}

static ZSTD_DCtx *dctx_create(de__pipeline *pl) {
    ZSTD_customMem mem = {zs_alloc, zs_free, pl};
    return ZSTD_createDCtx_advanced(mem);
}

/* A decoder context for the calling thread; *spare tells dctx_give where it goes back. */
static ZSTD_DCtx *dctx_take(de__pipeline *pl, bool *spare) {
    u32 w = de_task_worker_index();
    if (pl->dctx != NULL && w != DE_TASK_WORKER_NONE && w < pl->dctx_count) {
        *spare = false;
        if (pl->dctx[w] == NULL)
            pl->dctx[w] = dctx_create(pl); /* only worker w touches entry w */
        return (ZSTD_DCtx *)pl->dctx[w];
    }
    *spare = true;
    ZSTD_DCtx *d = NULL;
    de_mutex_lock(&pl->dctx_lock);
    if (pl->dctx_spare_count != 0)
        d = (ZSTD_DCtx *)pl->dctx_spare[--pl->dctx_spare_count];
    de_mutex_unlock(&pl->dctx_lock);
    return d != NULL ? d : dctx_create(pl);
}

static void dctx_give(de__pipeline *pl, ZSTD_DCtx *d, bool spare) {
    if (!spare || d == NULL)
        return;
    de_mutex_lock(&pl->dctx_lock);
    if (pl->dctx_spare_count == pl->dctx_spare_capacity) {
        u32 cap = pl->dctx_spare_capacity != 0 ? pl->dctx_spare_capacity * 2u : 4u;
        void *p = DE_REALLOC(pl->alloc, pl->dctx_spare, sizeof(void *) * pl->dctx_spare_capacity,
                             sizeof(void *) * cap, DE_ALIGNOF(void *), DE_MEM_ASSET);
        if (p == NULL) {
            de_mutex_unlock(&pl->dctx_lock);
            ZSTD_freeDCtx(d);
            return;
        }
        pl->dctx_spare = (void **)p;
        pl->dctx_spare_capacity = cap;
    }
    pl->dctx_spare[pl->dctx_spare_count++] = d;
    de_mutex_unlock(&pl->dctx_lock);
}

static void decode_fail(de__pjob *job, const char *what) {
    de__pipeline_job_fail(job, DE_ERR_PARSE, "corrupt zstd payload in \"%s\": %s", job->vpath,
                          what);
    if (job->buf != NULL)
        DE_FREE(job->pl->alloc, job->buf, (size_t)job->buf_size);
    job->buf = NULL;
    job->buf_size = 0;
    de__pipeline_finish(job);
}

void de__decompress_stage_run(void *user) {
    de__pjob *job = (de__pjob *)user;
    de__pipeline *pl = job->pl;
    if (de__pjob_cancelled(job)) {
        if (job->buf != NULL)
            DE_FREE(pl->alloc, job->buf, (size_t)job->buf_size);
        job->buf = NULL;
        de__pipeline_finish(job);
        return;
    }
    const u8 *src = (const u8 *)job->buf + DE__PAYLOAD_HEADER_SIZE;
    size_t src_size = (size_t)job->payload.stored_size;
    u64 raw_size = job->payload.raw_size;

    /* The frame header must agree with the payload header before the output is allocated. */
    unsigned long long fcs = ZSTD_getFrameContentSize(src, src_size);
    if (fcs == ZSTD_CONTENTSIZE_ERROR) {
        decode_fail(job, "not a zstd frame");
        return;
    }
    if (fcs != ZSTD_CONTENTSIZE_UNKNOWN && fcs != raw_size) {
        decode_fail(job, "frame size does not match the payload header");
        return;
    }
    size_t alloc_size = raw_size != 0 ? (size_t)raw_size : 1u;
    void *raw = DE_ALLOC(pl->alloc, alloc_size, 16, DE_MEM_ASSET);
    if (raw == NULL) {
        de__pipeline_job_fail(job, DE_ERR_OUT_OF_MEMORY, "out of memory for %llu bytes",
                              (unsigned long long)raw_size);
        decode_fail(job, "out of memory");
        return;
    }
    bool spare = false;
    ZSTD_DCtx *d = dctx_take(pl, &spare);
    size_t got =
        d != NULL ? ZSTD_decompressDCtx(d, raw, (size_t)raw_size, src, src_size) : (size_t)-1;
    dctx_give(pl, d, spare);
    if (d == NULL || ZSTD_isError(got) || got != raw_size) {
        DE_FREE(pl->alloc, raw, alloc_size);
        decode_fail(job, d == NULL           ? "out of memory for a decoder"
                         : ZSTD_isError(got) ? ZSTD_getErrorName(got)
                                             : "decompressed size does not match");
        return;
    }
    DE_FREE(pl->alloc, job->buf, (size_t)job->buf_size);
    job->buf = NULL;
    job->buf_size = 0;
    job->raw = raw;
    job->raw_size = raw_size;
    job->data = raw;
    job->data_size = raw_size;
    (void)de_atomic_fetch_add_u64(&pl->stats.decoded, 1, DE_ATOMIC_RELAXED);
    de__load_stage_begin(job, true);
}

void de__decompress_stage_shutdown(de__pipeline *pl) {
    for (u32 i = 0; i < pl->dctx_count; ++i)
        if (pl->dctx[i] != NULL)
            ZSTD_freeDCtx((ZSTD_DCtx *)pl->dctx[i]);
    if (pl->dctx != NULL)
        DE_FREE(pl->alloc, pl->dctx, sizeof(void *) * pl->dctx_count);
    pl->dctx = NULL;
    pl->dctx_count = 0;
    for (u32 i = 0; i < pl->dctx_spare_count; ++i)
        ZSTD_freeDCtx((ZSTD_DCtx *)pl->dctx_spare[i]);
    if (pl->dctx_spare != NULL)
        DE_FREE(pl->alloc, pl->dctx_spare, sizeof(void *) * pl->dctx_spare_capacity);
    pl->dctx_spare = NULL;
    pl->dctx_spare_count = pl->dctx_spare_capacity = 0;
}
