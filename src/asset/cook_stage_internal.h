/*
 * cook_stage_internal.h: the COOK stage of the load pipeline (SPEC §11.5; DENG-209). Internal to
 * de_asset; include as "asset/cook_stage_internal.h".
 *
 * In SOURCE mode, an asset whose type has an `import` callback is cooked in-process before it is
 * loaded: pl_submit (pipeline.c) asks de__cook_stage_plan, creates the job, and
 * de__cook_stage_begin queues the cook for the stage's cook threads, which run the cooker
 * library (tools/cook/lib/, "cook/lib/de_cooker.h"). The cooked bytes go to the type's `load`
 * like read bytes, and the dependencies the importer declared become the job's dependencies.
 *
 * Cook threads (DENG-387): with a task system the stage starts 1 to 4 threads `de-cook-<n>` (one
 * per 4 logical cores) at DE_THREAD_NORMAL on the first cook, and joins them at shutdown. They
 * take the most urgent load priority first (the job's priority at submit), FIFO within it. Not
 * the background pool: its workers run below normal priority and Windows starves them under a
 * busy loop (Trama "Decision: Asset loads never wait on below-normal threads"). Not the frame
 * pool: a cold cook may take long. Without a task system the cook runs inline.
 *
 * Sub-assets: each cook records the sub-assets it declared (`<id>#<name>`, type) in a table of
 * the stage. Typed access and the load API find their slots through it
 * (de__cook_stage_subasset_type), and a job for a sub-asset cooks its parent and loads the
 * sub-asset's bytes with the sub-asset type's loader. A sub-asset is known once its parent was
 * cooked in this process.
 *
 * Only builds that link the cooker (DE_HAS_COOKER: Debug and Dev with tools) cook; everywhere
 * else an importer type fails at submit with DE_ERR_UNSUPPORTED "cooker not available", and no
 * cooker symbol is referenced. PACKED mode never cooks: packs hold cooked bytes (DENG-212).
 */
#ifndef DE_ASSET_COOK_STAGE_INTERNAL_H
#define DE_ASSET_COOK_STAGE_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_platform.h>

#include "asset/load_internal.h"
#include "asset/manifest_internal.h"
#include "asset/pipeline_internal.h"

DE_EXTERN_C_BEGIN

/** The stage's state (cook_stage.c): the cooker and the sub-asset table; de__pipeline.cook. */
typedef struct de__cook_stage de__cook_stage;

/** What pl_submit needs to create a cooked job (de__cook_stage_plan fills it; main thread). */
typedef struct de__cook_plan {
    /** Not DE_OK: fail the submission with this code and `msg`. */
    de_result error;
    char msg[DE__LOAD_MSG_MAX];
    /** What the job copies (ID, settings): the asset; for a sub-asset, its parent's entry under
     * the sub-asset's ID. */
    de__manifest_asset asset;
    /** The type whose loader gets the cooked bytes: the asset's, or the sub-asset's. */
    const de_asset_type_desc *load_type;
    /** The source path, normalised (the job's `vpath`, for messages). */
    char vpath[DE_FS_PATH_MAX];

    /* For the cook task (copied by de__cook_stage_begin). */
    const de_asset_type_desc *import_type; /**< the cooked asset's type (it has `import`) */
    const char *parent_id;                 /**< the cooked asset's ID string */
    const char *subasset;                  /**< NULL, or the sub-asset name */
    char asset_root[DE_FS_PATH_MAX];       /**< the asset root, a normalised virtual dir */
    char source[DE_FS_PATH_MAX];           /**< relative to asset_root (locale applied) */
    char id_buf[DE_FS_PATH_MAX];           /**< "<parent>#<name>" for a sub-asset */
} de__cook_plan;

/**
 * Decides whether the submission of `slot` is cooked: false = load it from its source as it is
 * (not an importer type, not a known sub-asset, or PACKED mode). True = `*p` is filled: either
 * `p->error` (fail the submission) or a plan for de__cook_stage_begin. Main thread.
 */
bool de__cook_stage_plan(de_asset_system *s, u32 slot, de__cook_plan *p);
/** Starts the cook of a new job made from `p` (job_new copied the asset). Main thread. */
void de__cook_stage_begin(de__pjob *job, const de__cook_plan *p);
/** Frees the stage (the cooker, the sub-asset table). Pipeline shutdown, nothing in flight. */
void de__cook_stage_shutdown(de__pipeline *pl);
/** True if `id` is a sub-asset an in-process cook declared; `*type_hash` gets de_hash64_str of
 * its type name. Main thread. */
bool de__cook_stage_subasset_type(de_asset_system *s, de_asset_id id, u64 *type_hash);

/** DENG-387: counters for tests. */
typedef struct de__cook_stage_stats {
    u32 threads;          /**< cook threads running (0: none yet, no task system, or no cooker) */
    u64 cooks;            /**< cooks started (a cancelled job that never cooked is not counted) */
    u64 cooks_on_threads; /**< of those, the ones that ran on a cook thread */
} de__cook_stage_stats;
/** The counters; zeroes before the first cook and in builds without the cooker. Main thread. */
void de__cook_stage_get_stats(const de_asset_system *s, de__cook_stage_stats *out);

DE_EXTERN_C_END

#endif /* DE_ASSET_COOK_STAGE_INTERNAL_H */
