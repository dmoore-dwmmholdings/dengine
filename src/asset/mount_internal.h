/*
 * mount_internal.h: the pack mount stack and the pack read path of the load pipeline (SPEC §11.7,
 * §11.5, §21.3; DENG-212). Internal to de_asset; include as "asset/mount_internal.h".
 *
 * Mount stack (mount.c). de_asset_pack_mount opens a .dpak (dpak_reader.h) and puts it on the
 * stack of the asset system. Lookups walk the stack from the top: the highest priority first,
 * and among equal priorities the pack mounted last. The first pack that holds an asset ID serves
 * it, so a patch, DLC or mod pack overrides the base pack for the IDs it contains. Bundles merge
 * the same way: the topmost pack that defines a bundle name defines the whole bundle.
 *
 * Who reads the stack:
 *   - PACKED mode: every asset comes from the packs; no assets.toml is needed. Typed access
 *     creates slots for pack IDs with the type from the TOC, and de_asset_load_bundle reads the
 *     merged bundle tables.
 *   - SOURCE mode: a mounted pack overrides the manifest source of the IDs it holds (patch
 *     testing in Dev); the cooker is skipped for them. Manifest bundles win over pack bundles.
 *   - DENG-214 (hot reload) skips IDs that a pack overrides (de__mount_lookup).
 *
 * Lifetime: a pack is referenced by the stack and by every pipeline job that reads it. Unmount
 * takes it off the stack at once (later loads of its IDs go to the next pack), and the pack is
 * closed when its last job is freed. Already loaded assets keep their data.
 *
 * Pack read path (pack_stage.c), for a job whose asset a pack serves:
 *   OPEN    (frame worker) one buffer of the asset's stored size and one of its raw size, then one
 *           de_io_submit per RUN: contiguous chunks merged, at most DE__PACK_RUN_MAX_BYTES each
 *           (an asset's chunks are contiguous in a pack, so a run ends only at that limit). An
 *           asset whose chunks are all raw is read straight into the raw buffer.
 *   READ    the process I/O threads, DE_IO_CALLBACK_ON_IO_THREAD.
 *   DECODE  per run, as soon as it is read: a frame task that decodes the run's chunks in
 *           parallel (de_task_parallel_for, grain 1), each into the raw buffer at its raw offset,
 *           with the frame worker's ZSTD_DCtx and the chunk's ZSTD_DDict.
 *   then the LOAD stage, as for any job. The TOC's dependencies are the job's first
 *   dependencies; a loader may add more.
 * No stage runs on the background pool ("Decision: Asset loads never wait on below-normal
 * threads").
 *
 * Threads: the stack, the packs' refcounts and the plan are main thread only. A pack's reader is
 * read-only after open, so the stage threads use it freely while their job holds a reference.
 */
#ifndef DE_ASSET_MOUNT_INTERNAL_H
#define DE_ASSET_MOUNT_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_platform.h>

#include "asset/dpak/dpak_format.h"
#include "asset/dpak/dpak_reader.h"
#include "asset/load_internal.h"
#include "asset/manifest_internal.h"
#include "asset/pipeline_internal.h"
#include "platform/de_atomic.h"

DE_EXTERN_C_BEGIN

/** Largest single de_io read of a pack job (a run of contiguous chunks). */
#define DE__PACK_RUN_MAX_BYTES ((u64)4 << 20)

/* ---- Mount stack (mount.c) ------------------------------------------------------------------ */

/** The stack of one asset system (de_asset_system.mounts); NULL until the first mount. */
typedef struct de__mount_stack de__mount_stack;

/** One mounted (or unmounted, still read) pack. */
typedef struct de__pack {
    char *vpath; /**< normalised virtual path (owned) */
    size_t vpath_size;
    i32 priority;
    u32 seq;                       /**< mount order: a later mount of the same priority wins */
    dpak_reader *reader;           /**< read-only after open; any thread while referenced */
    u32 refs;                      /**< (main) 1 while on the stack, plus 1 per job */
    bool mounted;                  /**< (main) on the stack */
    struct de__pack *retired_next; /**< (main) list of unmounted packs still referenced */
} de__pack;

/** Where an asset is: the pack that serves it, its TOC index and entry. */
typedef struct de__mount_entry {
    de__pack *pack;
    u32 index;
    dpak_toc_entry toc;
} de__mount_entry;

/**
 * The topmost mounted pack that holds `id`, in `*out` (may be NULL). False when no mounted pack
 * holds it (or nothing is mounted). Binary search per pack; no allocation. Main thread.
 */
bool de__mount_lookup(de_asset_system *s, de_asset_id id, de__mount_entry *out);

/** True when at least one pack is mounted. Main thread. */
bool de__mount_any(const de_asset_system *s);

/** Takes a reference on a pack (a job reads it). Main thread. */
void de__mount_pack_ref(de__pack *p);
/** Drops a reference; the last one of an unmounted pack closes and frees it. Main thread. */
void de__mount_pack_release(de_asset_system *s, de__pack *p);

/** de_asset_system_destroy, after the pipeline (no job left): closes every pack. */
void de__mount_shutdown(de_asset_system *s);

/** The closure of a bundle over the merged pack bundle tables (de__mount_bundle_closure). */
typedef struct de__mount_bundle {
    de_asset_id *ids; /**< the assets: depended-on bundles' members first, then the bundle's */
    u32 count, capacity;
    de_asset_id *pins; /**< the members of the RESIDENT bundles among them */
    u32 pin_count, pin_capacity;
    u32 priority; /**< the named bundle's priority (a de__load_priority value) */
} de__mount_bundle;

/**
 * The bundle `name` of the topmost pack that defines it, closed over its depends (each resolved
 * the same way; a depend in no mounted pack is skipped with a warning, a cycle is cut). The IDs
 * may repeat; the load request drops the duplicates. Free with de__mount_bundle_free.
 * @return DE_OK; DE_ERR_NOT_FOUND (no mounted pack defines it; sets no error context);
 *         DE_ERR_OUT_OF_MEMORY. Main thread.
 */
de_result de__mount_bundle_closure(de_asset_system *s, const char *name, de__mount_bundle *out);
/** Pins the resident members of `b` in the budget (after the request was created). */
void de__mount_bundle_pin(de_asset_system *s, const de__mount_bundle *b);
void de__mount_bundle_free(de_asset_system *s, de__mount_bundle *b);

/** Counters for tests and debugging. */
typedef struct de__mount_stats {
    u32 mounted;    /**< packs on the stack */
    u32 retired;    /**< unmounted packs that jobs still read */
    u64 jobs;       /**< pipeline jobs served by a pack */
    u64 runs;       /**< de_io reads submitted for pack runs */
    u64 chunks;     /**< chunks decoded (zstd or raw copy) */
    u64 bytes_read; /**< stored bytes read */
} de__mount_stats;

/** Zeroes when nothing was ever mounted. Any thread (the counters are atomics). */
void de__mount_get_stats(const de_asset_system *s, de__mount_stats *out);
/** pack_stage.c: adds to the counters. Any thread. */
void de__mount_count(de_asset_system *s, u64 jobs, u64 runs, u64 chunks, u64 bytes);

/* ---- Pack read path (pack_stage.c) ----------------------------------------------------------- */

typedef struct de__pack_job de__pack_job;

/** What pl_submit needs for a pack job (de__pack_stage_plan fills it; main thread). */
typedef struct de__pack_plan {
    /** Not DE_OK: fail the submission with this code and `msg`. */
    de_result error;
    char msg[DE__LOAD_MSG_MAX];
    de__mount_entry where;
    /** What job_new copies: the manifest asset (SOURCE mode, for its settings), or an asset with
     * no settings named after the pack's name for the ID (or its hex). */
    de__manifest_asset asset;
    const de_asset_type_desc *type;
    char vpath[DE_FS_PATH_MAX]; /**< the pack's path (the job's `vpath`, for messages) */
    char id_buf[DE_FS_PATH_MAX];
} de__pack_plan;

/**
 * Decides whether the submission of `slot` reads a pack: false = it does not (no mounted pack has
 * the asset, in SOURCE mode). True = `*p` is filled: either `p->error` (fail the submission; in
 * PACKED mode an asset in no mounted pack is DE_ERR_NOT_FOUND) or a plan. Main thread.
 */
bool de__pack_stage_plan(de_asset_system *s, u32 slot, de__pack_plan *p);
/**
 * Makes `job` (just made from the plan) a pack job: allocates its run table, references the pack
 * and fills the job's dependencies from the TOC. DE_ERR_OUT_OF_MEMORY (nothing attached). Main
 * thread.
 */
de_result de__pack_stage_attach(de__pjob *job, const de__pack_plan *p);
/** OPEN of a pack job (task fn, `user` = job); io_stage.c's OPEN branches here. */
void de__pack_stage_open(void *job);
/** Cancels the job's reads (stage READ). Main thread, under the rules of job_cancel. */
void de__pack_stage_cancel(de__pjob *job);
/** job_free: frees the run table and drops the pack reference. Main thread. */
void de__pack_stage_free(de__pjob *job);

DE_EXTERN_C_END

#endif /* DE_ASSET_MOUNT_INTERNAL_H */
