/*
 * asset_internal.h: the de_asset_system object, shared by every file of src/asset/. Internal;
 * include as "asset/asset_internal.h".
 *
 * APPEND-ONLY (S07). Each S07 task adds its own block of fields at the end of the struct, under a
 * comment naming the task, and its helpers after the struct. Do not reorder or rewrite another
 * task's block; on a merge conflict keep both sides.
 */
#ifndef DE_ASSET_INTERNAL_H
#define DE_ASSET_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h" /* DENG-139 */

DE_EXTERN_C_BEGIN

/** Longest locale tag de_asset_set_locale keeps, without the NUL. */
#define DE__ASSET_LOCALE_MAX 35

struct de_asset_system {
    /* ---- DENG-138: system, manifest, locale ---------------------------------------------- */
    const de_allocator *alloc; /**< desc.allocator, or de_heap_allocator() */
    de_task_system *tasks;     /**< desc.tasks; may be NULL */
    /** The desc with defaults applied. `cache_dir` points at `cache_dir_buf`, `locale` at
     * `locale_buf`; every other pointer field is as given. */
    de_asset_system_desc desc;
    char *cache_dir_buf; /**< owned copy, from `alloc` */
    size_t cache_dir_size;
    char locale_buf[DE__ASSET_LOCALE_MAX + 1]; /**< current locale; "" = none */
    de__manifest *manifest;        /**< the active manifest (last successful load); may be NULL */
    de__manifest *failed_manifest; /**< the last load if it failed (for its issues); else NULL */
    de_log_category log;           /**< "asset" */

    /* ---- DENG-139: asset IDs, globs, bundle closure (append below) ----------------------- */
    /** The resolved view of `manifest` (IDs, bundle members and closures); NULL when
     * `manifest` is NULL. Destroyed before its manifest. */
    de__resolved *resolved;
    /** The resolved view of `failed_manifest` when the load failed in resolution (its issues
     * are the full list); else NULL. */
    de__resolved *failed_resolved;

    /* ---- DENG-140: slots, typed handles, type registry (append below) -------------------- */
    struct de__slot_table *slots; /**< slots.c; NULL until the first slot (slots_internal.h) */
    struct de__asset_type_registry *types; /**< asset_types.c; NULL until the first register */

    /* ---- DENG-141: load requests and status (append below) ------------------------------- */
    /** load.c: requests, per-slot load state, backend, completion queue (load_internal.h);
     * NULL until the first request or backend. */
    struct de__load_state *load;

    /* ---- DENG-142: load pipeline (append below) ------------------------------------------ */
    /** pipeline.c: the real load backend (pipeline_internal.h); created by
     * de_asset_system_create, started on the first submit. */
    struct de__pipeline *pipeline;

    /* ---- DENG-143: ECS events, host integration (append below) --------------------------- */

    /* ---- DENG-144: budgets and eviction (append below) ----------------------------------- */
};

/** The current locale, or NULL when none is set. */
static inline const char *de__asset_locale(const de_asset_system *s) {
    return s->locale_buf[0] != '\0' ? s->locale_buf : NULL;
}

DE_EXTERN_C_END

#endif /* DE_ASSET_INTERNAL_H */
