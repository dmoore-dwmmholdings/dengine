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
    /* `emit_fn` (the event hook) is in DENG-144's block below: one copy for both tasks. The
     * first de_asset_system_update with a world sets it to the ECS sender of asset_events.c. */
    /** asset_events.c: the attached world, the event type, the DeAssetSystemRef resource, the
     * `de.asset_events` system and the queue of events not sent yet; NULL until attached. */
    struct de__asset_events *events;

    /* ---- DENG-144: budgets and eviction (append below) ----------------------------------- */
    /** Event hook shared with DENG-143 (which sets it to its ECS sender): called for every asset
     * event; NULL (the default from create) sends nothing. Main thread. Keep ONE copy of this
     * field and of de__asset_emit when DENG-143 and DENG-144 are merged. */
    void (*emit_fn)(de_asset_system *s, const de_asset_event *ev);
    /** budget.c: budgets, residency accounting, LRU lists (budget_internal.h); created by
     * de_asset_system_create. */
    struct de__budget *budget;

    /* ---- DENG-212: pack mount stack (append below) --------------------------------------- */
    /** mount.c: the mounted .dpak packs, highest priority first (mount_internal.h); NULL until
     * the first de_asset_pack_mount. */
    struct de__mount_stack *mounts;
};

/** The current locale, or NULL when none is set. */
static inline const char *de__asset_locale(const de_asset_system *s) {
    return s->locale_buf[0] != '\0' ? s->locale_buf : NULL;
}

/* DENG-143 / DENG-144: sends one asset event through `emit_fn` (if set). Main thread. */
static inline void de__asset_emit(de_asset_system *s, const de_asset_event *ev) {
    if (s->emit_fn)
        s->emit_fn(s, ev);
}

/* ---- DENG-143: ECS events (asset_events.c) ----------------------------------------------- */

/** First de_asset_system_update with a world: registers the event type and the resource and
 * sets `emit_fn` (once; a later, different world gets a warning). Main thread. */
void de__asset_events_attach(de_asset_system *s, de_world *world);
/** de_asset_system_destroy: removes the system and the resource from the world, frees the
 * queue, clears `emit_fn`. The world must still exist. */
void de__asset_events_detach(de_asset_system *s);

DE_EXTERN_C_END

#endif /* DE_ASSET_INTERNAL_H */
