/*
 * pipeline_internal.h: the asset load pipeline, the real backend behind the load API seam
 * (backend_internal.h) (SPEC §11.4, §11.5; DENG-142). Internal to de_asset; include as
 * "asset/pipeline_internal.h".
 *
 * One JOB per submission of a slot (de__load_backend.submit). A job walks these stages:
 *
 *   QUEUED     in the pipeline's dispatch queue: 5 FIFO lists, one per de__load_priority. At most
 *              `io_limit` jobs (2 x io_threads) are in OPEN or READ at once, so a more urgent job
 *              queued later overtakes the less urgent ones still queued (main thread enqueues;
 *              dispatch pops under `lock`).
 *   OPEN       the source is opened and its size queried (bytes_total is known from here on):
 *              a DE_TASK_POOL_FRAME task, or inline without a task system. io_stage.c. Not the
 *              background pool: its workers run below normal priority, and on Windows a busy
 *              main thread and frame workers starved them for seconds (DENG-386). No stage of a
 *              load runs on a below-normal thread.
 *   READ       one de_io_request on the process I/O threads (de_io_submit, S03); the callback
 *              runs on the I/O thread (DE_IO_CALLBACK_ON_IO_THREAD).
 *   DECODE     payloads tagged zstd (de__payload header) decompress on a DE_TASK_POOL_FRAME
 *              worker with that worker's ZSTD_DCtx. Raw payloads skip it. decompress_stage.c.
 *   LOAD       the type's `load` (de_asset_load_ctx) on a frame worker, or
 *   MAIN_LOAD  queued for the main thread (`main_thread_load`), run by the update hook.
 *   FINISHED   pushed on the lock-free finished list; the main thread takes the whole list in
 *              de_asset_system_update (the backend's `update` hook) and publishes.
 *   WAIT_DEPS  loaded, with dependencies (de_asset_load_ctx_add_dependency): they are loaded by
 *              an internal request (DE_LOAD_NO_EVENTS, the job's priority) and held with
 *              asset -> asset refs. The job is published READY in the same update as its last
 *              dependency, or FAILED (cause chain) when one fails or closes a cycle.
 *   WAIT_UPLOAD (DENG-105) loaded and its dependencies READY, but its type's `ready` hook
 *              returned DE_ERR_BUSY (the GPU copies of a texture or mesh are in flight). It stays
 *              on the waiting list and the hook is asked again by every update; DE_OK publishes
 *              it, any other result fails it. Types without the hook never get here.
 *
 * Every slot state change happens on the main thread, in the update hook, through
 * de__slot_complete_now (load_internal.h). A cancelled job reports nothing more: it is detached
 * from its slot at once, stops at its next stage boundary (in-flight I/O is cancelled with
 * de_io_cancel), and when it reaches the main thread its partial object goes to the type's
 * `unload`.
 *
 * Threads: `lock` guards the dispatch queue, `io_active`, the main-thread load list, job `stage`
 * changes that cancel cares about (QUEUED -> OPEN, OPEN -> READ) and progress reports. Never held
 * around a loader, de_io_* or de_task_* call. Everything else marked (main) is main-thread only.
 */
#ifndef DE_ASSET_PIPELINE_INTERNAL_H
#define DE_ASSET_PIPELINE_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_task.h>

#include "asset/backend_internal.h"
#include "asset/load_internal.h"
#include "asset/manifest_internal.h"
#include "core/de_hashmap.h"
#include "platform/de_atomic.h"

#include <string.h>

DE_EXTERN_C_BEGIN

/* ---- Payload header (shared with the S08 pack writer) ------------------------------------ */

/*
 * de__payload: an optional 24-byte header in front of stored asset bytes. Little-endian:
 *   [0..3]   magic 'D' 'E' 'P' 'L'
 *   [4..5]   version (1)
 *   [6..7]   codec: 0 raw, 1 zstd (one zstd frame)
 *   [8..15]  raw_size: bytes the loader gets
 *   [16..23] stored_size: bytes after the header (the file or pack span is header + stored)
 * Bytes without the magic are raw and go to the loader unchanged (S07 source mode).
 */
#define DE__PAYLOAD_HEADER_SIZE 24u
#define DE__PAYLOAD_VERSION     1u
/** Largest raw_size accepted (a corrupt header must not ask for a huge allocation). */
#define DE__PAYLOAD_MAX_RAW ((u64)1 << 32)

typedef enum de__payload_codec {
    DE__PAYLOAD_RAW = 0,
    DE__PAYLOAD_ZSTD = 1,
    DE__PAYLOAD_CODEC_COUNT
} de__payload_codec;

typedef struct de__payload_header {
    u32 version;
    u32 codec; /**< de__payload_codec */
    u64 raw_size;
    u64 stored_size;
} de__payload_header;

static inline void de__payload_store_u64(u8 *p, u64 v) {
    for (u32 i = 0; i < 8; ++i)
        p[i] = (u8)(v >> (8u * i));
}
static inline u64 de__payload_load_u64(const u8 *p) {
    u64 v = 0;
    for (u32 i = 0; i < 8; ++i)
        v |= (u64)p[i] << (8u * i);
    return v;
}

/** Writes the header of a payload into `out` (DE__PAYLOAD_HEADER_SIZE bytes). */
static inline void de__payload_header_store(u8 *out, de__payload_codec codec, u64 raw_size,
                                            u64 stored_size) {
    out[0] = 'D';
    out[1] = 'E';
    out[2] = 'P';
    out[3] = 'L';
    out[4] = (u8)DE__PAYLOAD_VERSION;
    out[5] = 0;
    out[6] = (u8)codec;
    out[7] = 0;
    de__payload_store_u64(out + 8, raw_size);
    de__payload_store_u64(out + 16, stored_size);
}

/**
 * Reads the header at the start of `size` bytes. DE_ERR_NOT_FOUND: no magic (raw bytes).
 * DE_ERR_PARSE: the magic is there but the header is not valid (version, codec, sizes that do not
 * match `size`, raw_size over DE__PAYLOAD_MAX_RAW, a raw payload whose sizes differ). Sets no
 * error context.
 */
static inline de_result de__payload_header_load(const void *data, u64 size,
                                                de__payload_header *out) {
    const u8 *p = (const u8 *)data;
    if (size < DE__PAYLOAD_HEADER_SIZE || p[0] != 'D' || p[1] != 'E' || p[2] != 'P' || p[3] != 'L')
        return DE_ERR_NOT_FOUND;
    out->version = (u32)p[4] | ((u32)p[5] << 8);
    out->codec = (u32)p[6] | ((u32)p[7] << 8);
    out->raw_size = de__payload_load_u64(p + 8);
    out->stored_size = de__payload_load_u64(p + 16);
    if (out->version != DE__PAYLOAD_VERSION || out->codec >= DE__PAYLOAD_CODEC_COUNT ||
        out->stored_size != size - DE__PAYLOAD_HEADER_SIZE || out->raw_size > DE__PAYLOAD_MAX_RAW)
        return DE_ERR_PARSE;
    if (out->codec == DE__PAYLOAD_RAW && out->raw_size != out->stored_size)
        return DE_ERR_PARSE;
    return DE_OK;
}

/* ---- Source (what the I/O stage reads; S08 packs replace it) ----------------------------- */

/** Where the stored bytes of one asset are: `size` bytes at `offset` in `file`. */
typedef struct de__source_span {
    de_file *file;
    u64 offset;
    u64 size;
    void *handle; /**< for the source's own use (close) */
} de__source_span;

/**
 * The source the I/O stage reads from. S07 reads source files (`asset_root/source`, with the
 * locale override); S08's pack reader installs its own (file = the pack, offset and size of the
 * asset's data). Functions run on a pipeline thread (OPEN stage) and may block.
 */
typedef struct de__pipeline_source {
    /** Opens the stored bytes of asset `id`, whose source path is `vpath`. Sets the error
     * context on failure (its message becomes the asset's failure message). */
    de_result (*open)(void *user, de_asset_system *s, const char *vpath, de_asset_id id,
                      de__source_span *out);
    /** Closes what `open` returned, once its bytes are read (or the read was abandoned). */
    void (*close)(void *user, de__source_span *span);
    void *user;
} de__pipeline_source;

/* ---- Jobs ---------------------------------------------------------------------------------- */

typedef enum de__pjob_stage {
    DE__PJOB_QUEUED,
    DE__PJOB_OPEN,
    DE__PJOB_READ,
    DE__PJOB_DECODE,
    DE__PJOB_LOAD,
    DE__PJOB_MAIN_LOAD,
    DE__PJOB_FINISHED,
    DE__PJOB_WAIT_DEPS,
    DE__PJOB_WAIT_UPLOAD /**< DENG-105: loaded, dependencies READY; its type's `ready` hook said
                            DE_ERR_BUSY (GPU copies in flight). On the waiting list. */
} de__pjob_stage;

/** de__pipeline_debug_stage: no live job for the slot. */
#define DE__PJOB_NONE 0xFFFFFFFFu

typedef struct de__pipeline de__pipeline;

/**
 * One job. It is also the de_asset_load_ctx a loader receives (load_ctx.c has the accessors).
 * One allocation holds the job, the asset path, the source path and a deep copy of the asset's
 * settings, so a manifest reload cannot free what a running job reads.
 */
struct de_asset_load_ctx {
    /* Set at submit, read-only afterwards. */
    de__pipeline *pl;
    de_asset_system *s;
    u32 slot;
    de_asset_id id;
    const de_asset_type_desc *type; /**< registry entries never move */
    const char *asset;              /**< the asset ID string (copy) */
    const char *vpath;              /**< the source path, normalised (copy) */
    const de__manifest_kv *settings;
    u32 setting_count;
    size_t block_size; /**< bytes of the job's allocation */

    de_atomic_u32 stage;     /**< de__pjob_stage */
    de_atomic_u32 cancelled; /**< set (under `lock`) by cancel; the job reports nothing more */
    u8 priority;             /**< (lock) de__load_priority */

    struct de_asset_load_ctx *q_prev, *q_next;       /**< (lock) dispatch queue */
    struct de_asset_load_ctx *m_next;                /**< (lock) main-thread load list */
    struct de_asset_load_ctx *done_next;             /**< finished list (published by the CAS) */
    struct de_asset_load_ctx *all_prev, *all_next;   /**< (main) every live job */
    struct de_asset_load_ctx *wait_prev, *wait_next; /**< (main) WAIT_DEPS jobs */
    bool in_table; /**< (main) the slot -> job map points at this job */
    bool waiting;  /**< (main) on the WAIT_DEPS list */

    /* I/O and decode (the stage thread that owns the job). */
    de__source_span span;
    bool span_open;
    de_io_request io;
    void *buf; /**< stored bytes as read */
    u64 buf_size;
    void *raw; /**< decompressed bytes */
    u64 raw_size;
    const void *data; /**< what the loader gets: into buf or raw */
    u64 data_size;
    de__payload_header payload;

    /* Results (the stage thread, then the main thread). */
    de_result result;
    bool msg_set;
    char msg[DE__LOAD_MSG_MAX];
    void *object;
    u64 object_bytes;
    de_asset_id *deps; /**< declared dependencies, no duplicates */
    u32 dep_count, dep_capacity;

    /* Dependencies (main). */
    de_load_handle dep_req; /**< the internal request that loads `deps` */
    u32 *dep_slots;         /**< slots this job holds an asset ref on */
    u32 dep_slot_count;
    u32 visit_epoch; /**< cycle search marks */
    struct de_asset_load_ctx *visit_parent;

    /* DENG-212: the pack read state of a job whose asset a mounted pack serves
     * (mount_internal.h, pack_stage.c); NULL for every other job. Set at submit. */
    struct de__pack_job *pack;

    /* DENG-214: hot reload (hot_reload_internal.h). */
    /** (main) A reload job: the slot is READY and keeps its object until this job is swapped in
     * (de__asset_reload_swap); a failure goes to de__asset_reload_failed, not to the slot. Not in
     * the slot -> job map, so the load API never sees it. Set at submit. */
    bool reload;
    /** The cooker's extra sources as normalised virtual paths, NUL-separated (`extra_count` of
     * them in `extra_size` bytes), written by the cook thread before it hands the job on; NULL
     * without hot reload or extra sources. Freed with the job. */
    char *extra_paths;
    u32 extra_count;
    size_t extra_size;
};

typedef struct de_asset_load_ctx de__pjob;

/** Counters for tests and debugging (relaxed atomics; any thread). */
typedef struct de__pipeline_stats {
    u64 submits;      /**< jobs created */
    u64 reads;        /**< de_io reads that completed with DE_OK */
    u64 io_cancelled; /**< de_io reads that completed with DE_ERR_CANCELLED */
    u64 decoded;      /**< zstd payloads decompressed */
    u64 loads;        /**< loader calls (worker and main thread) */
    u64 main_loads;   /**< loader calls on the main thread */
    u64 published;    /**< slots made READY */
    u64 failed;       /**< slots made FAILED */
    u64 discarded;    /**< cancelled jobs cleaned up */
    u64 unloads;      /**< objects of cancelled or failed jobs passed to `unload` */
    u32 live_jobs;    /**< jobs not freed yet (main thread's view) */
} de__pipeline_stats;

/* ---- The pipeline -------------------------------------------------------------------------- */

struct de__pipeline {
    de_asset_system *s;
    const de_allocator *alloc;
    de_task_system *tasks; /**< NULL: every stage runs inline on the thread that reaches it */
    bool started;          /**< (main) lazy start on the first submit */
    de_result start_error; /**< (main) why the start failed; DE_OK otherwise */
    bool owns_io;          /**< (main) this pipeline started the I/O system (de_io_init) */
    u32 io_limit;          /**< jobs in OPEN or READ at once */
    de__pipeline_source source;

    /* Main thread. */
    de_hashmap jobs; /**< slot handle id -> de__pjob * (current job of the slot; probed only) */
    de__pjob *all_head;
    de__pjob *wait_head, *wait_tail;
    u32 live_jobs;
    u32 epoch;      /**< cycle search */
    de__pjob **dfs; /**< cycle search stack (grown on demand) */
    u32 dfs_capacity;
    bool in_update; /**< the update hook is running */

    /* Guarded by `lock`. */
    de_mutex lock;
    de__pjob *q_head[DE__LOAD_PRIORITY_COUNT], *q_tail[DE__LOAD_PRIORITY_COUNT];
    u32 queued;
    u32 io_active;
    de__pjob *main_head, *main_tail;

    /* Finished jobs: a lock-free LIFO stack; the main thread takes it whole and reverses it. */
    de_atomic_ptr done_head;

    /* Every task the pipeline submits; shutdown waits on it. */
    de_task_counter task_counter;

    /* zstd decoder contexts (decompress_stage.c). */
    void **dctx; /**< one per frame worker index (created on first use by that index) */
    u32 dctx_count;
    de_mutex dctx_lock;
    void **dctx_spare; /**< for threads that are not frame workers (guarded by dctx_lock) */
    u32 dctx_spare_count, dctx_spare_capacity;

    struct {
        de_atomic_u64 submits, reads, io_cancelled, decoded, loads, main_loads, published, failed,
            discarded, unloads;
    } stats;

    /* DENG-209: the in-process cooker and its sub-asset table (cook_stage.c); NULL until the
     * first cook. */
    struct de__cook_stage *cook;
};

/* ---- Lifetime (pipeline.c) ------------------------------------------------------------------ */

/** de_asset_system_create: creates the pipeline (nothing starts yet) and installs it as the
 * backend. DE_ERR_OUT_OF_MEMORY. */
de_result de__pipeline_install(de_asset_system *s);
/** de_asset_system_destroy, after de__load_shutdown (which cancels every submission): waits for
 * every job to come back, frees them, stops the I/O system if the pipeline started it. */
void de__pipeline_shutdown(de_asset_system *s);

/** Replaces the source the I/O stage reads (S08 packs, tests); NULL = the default (source
 * files). Only while no job is in flight. Main thread. */
void de__pipeline_set_source(de_asset_system *s, const de__pipeline_source *source);
/** The default source: S07 source mode (de_file_open on the source path). */
const de__pipeline_source *de__pipeline_default_source(void);

/** Counters (see de__pipeline_stats). Zeroes when the pipeline does not exist. */
void de__pipeline_get_stats(const de_asset_system *s, de__pipeline_stats *out);
/** The stage of the live job of `slot`, or DE__PJOB_NONE. For tests. Main thread. */
u32 de__pipeline_debug_stage(de_asset_system *s, u32 slot);
/** DENG-105: jobs in WAIT_UPLOAD (de_asset_stats.uploads_in_flight). O(waiting jobs). Main
 * thread. */
u32 de__pipeline_uploads_waiting(const de_asset_system *s);

/* ---- DENG-214: hot reload jobs (pipeline.c) ----------------------------------------------- */

/**
 * Starts a reload job for the READY slot `slot` (de__asset_reload_update): the same stages as a
 * load (pack, cook or source path, chosen at submit), with `reload` set. Returns the job, or
 * NULL with the error context set when it cannot start (the asset is no longer in the manifest,
 * no loader, out of memory, ...). The job may already be finished when this returns (no task
 * system); it is published by the next pipeline update. Main thread.
 */
struct de_asset_load_ctx *de__pipeline_submit_reload(de_asset_system *s, u32 slot,
                                                     de__load_priority priority);
/** Cancels a reload job a newer change replaces: it stops at its next stage boundary and is
 * discarded on the main thread. Main thread. */
void de__pipeline_cancel_reload(de_asset_system *s, struct de_asset_load_ctx *job);

/* ---- Between the stages (pipeline.c) --------------------------------------------------------- */

/** Runs `fn(job)` on `pool` (counted on the pipeline's task counter), or inline without a task
 * system. */
void de__pipeline_run(de__pipeline *pl, de_task_pool pool, de_task_fn fn, de__pjob *job);
/** Hands a job to the main thread (lock-free push). Any thread. The caller must not touch the
 * job afterwards. */
void de__pipeline_finish(de__pjob *job);
/** Sets the job's failure (code and message, prefixed with the asset path) unless one is set.
 * Stage thread. */
void de__pipeline_job_fail(de__pjob *job, de_result code, const char *fmt, ...) DE_PRINTF_FMT(3, 4);
/** Reports read progress to the load API unless the job is cancelled (under `lock`). */
void de__pipeline_progress(de__pjob *job, u64 bytes_done, u64 bytes_total);
/** Starts queued jobs while fewer than `io_limit` are in OPEN or READ. Any thread. */
void de__pipeline_dispatch(de__pipeline *pl);
/** A job left the OPEN/READ window: frees its place and dispatches the next. Any thread. */
void de__pipeline_io_done(de__pipeline *pl);
/** True once cancel was called for the job. */
static inline bool de__pjob_cancelled(const de__pjob *job) {
    return de_atomic_load_u32(&job->cancelled, DE_ATOMIC_ACQUIRE) != 0;
}
static inline void de__pjob_set_stage(de__pjob *job, de__pjob_stage stage) {
    de_atomic_store_u32(&job->stage, (u32)stage, DE_ATOMIC_RELEASE);
}

/* ---- Stages ------------------------------------------------------------------------------- */

/** io_stage.c: OPEN (task fn, `user` = job): opens the source, allocates the buffer, submits
 * the read. */
void de__io_stage_open(void *job);
/** io_stage.c: starts the process I/O system when it is not running. Main thread. */
de_result de__io_stage_start(de__pipeline *pl);
/** io_stage.c: stops it if this pipeline started it. Main thread. */
void de__io_stage_stop(de__pipeline *pl);

/** decompress_stage.c: DECODE (task fn, `user` = job). */
void de__decompress_stage_run(void *job);
/** decompress_stage.c: frees the decoder contexts. Main thread, nothing in flight. */
void de__decompress_stage_shutdown(de__pipeline *pl);
/** DENG-212: a ZSTD_DCtx for the calling thread (the frame worker's own, or a spare); NULL when
 * out of memory. Give it back with de__decompress_dctx_give and the same `*spare`. */
void *de__decompress_dctx_take(de__pipeline *pl, bool *spare);
void de__decompress_dctx_give(de__pipeline *pl, void *dctx, bool spare);

/** load_ctx.c: hands a job with its bytes to the loader: the main-thread list, a frame task,
 * or inline when already on a frame worker (or without a task system). */
void de__load_stage_begin(de__pjob *job, bool on_frame_worker);
/** load_ctx.c: runs the loader of a job (any thread; never under a lock). */
void de__load_stage_call(de__pjob *job);
/** load_ctx.c: the task fn of the LOAD stage. */
void de__load_stage_run(void *job);

/**
 * GPU upload stage: called after a successful `load`, on the loader's thread, before the job goes
 * back to the main thread. A no-op: DENG-105's GPU types stage their bytes in their loader and
 * wait for the copies on the main thread through `ready` (WAIT_UPLOAD). Returning an error fails
 * the asset.
 */
de_result de__pipeline_upload_stage(de_asset_system *s, de_asset_load_ctx *ctx);

DE_EXTERN_C_END

#endif /* DE_ASSET_PIPELINE_INTERNAL_H */
