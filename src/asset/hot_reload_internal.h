/*
 * hot_reload_internal.h: asset hot reload (SPEC §11.9, §11.3 RELOADED; DENG-214). Internal to
 * de_asset; include as "asset/hot_reload_internal.h".
 *
 * Dev builds (Debug and Dev, never Shipping) watch every source path that a load read, the
 * cooker's extra sources and every manifest file. A change goes through these steps:
 *
 *   1. Seen: a file watch delivers the path in de_asset_system_update (de_file_watch_poll, main
 *      thread), or a test calls de__asset_reload_on_change. The reverse index (reverse_index.c)
 *      gives the asset IDs that read the path; each one is queued once per frame. A manifest
 *      file sets `manifest_dirty`.
 *   2. Started, in the same update: the manifest is loaded again and diffed against the old one
 *      (manifest_diff.c); each queued asset whose slot is READY gets a RELOAD JOB in the load
 *      pipeline (de__pipeline_submit_reload): the COOK stage on the normal-priority cook threads
 *      for importer types, else OPEN and READ, then LOAD on the frame pool (or the main thread
 *      for main_thread_load types). Nothing of a reload runs on DE_TASK_POOL_BACKGROUND (Trama
 *      "Decision: Asset loads never wait on below-normal threads"). The old object stays live.
 *   3. Swapped, on the main thread inside de_asset_system_update (the pipeline's update hook
 *      publishes the job): de__asset_reload_swap replaces the slot's object, bytes and
 *      dependency list, bumps `version` and keeps the generation, so every typed handle stays
 *      valid and now resolves to the new object. Then the DE_ASSET_EV_RELOADED event, then the
 *      `dependency_reloaded` callback of every READY asset that lists the reloaded one in its
 *      dependencies. The old object and its dependency refs are retired: unloaded after
 *      `reload_retire_frames` more updates (default 3, the frames in flight).
 *   4. Failed: the old object stays, the error is kept for de_asset_last_error and logged at
 *      Error with its cause chain. A FAILED event reports recook timing and location while
 *      state stays READY. The next successful reload clears the error.
 *
 * Skipped: assets a mounted pack serves (logged at Info once per asset), PACKED mode (packs do
 * not change at run time), slots that are not READY (a load in flight is retried next frame; a
 * slot that is INVALID, FAILED or CANCELLED loads the new file on its next request).
 *
 * Threads: everything here runs on the main thread. The cook threads only write the job's
 * extra-source list (pipeline_internal.h) before the job is finished.
 */
#ifndef DE_ASSET_HOT_RELOAD_INTERNAL_H
#define DE_ASSET_HOT_RELOAD_INTERNAL_H

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "asset/manifest_internal.h"
#include "asset/resolve_internal.h"
#include "core/de_hashmap.h"

DE_EXTERN_C_BEGIN

/** 1 where hot reload is compiled: Debug and Dev, never Shipping (SPEC §6, §11.9). Release and
 * Shipping keep only de_asset_last_error and no-op hooks. */
#if (defined(DE_DEBUG) || defined(DE_DEV)) && !defined(DE_SHIPPING)
#define DE__ASSET_RELOAD_ENABLED 1
#else
#define DE__ASSET_RELOAD_ENABLED 0
#endif

/** reload_retire_frames when the desc leaves it 0: the frames in flight. */
#define DE__ASSET_RELOAD_RETIRE_DEFAULT 3u

struct de_asset_load_ctx; /* the pipeline job (pipeline_internal.h) */

/* ---- Reverse index (reverse_index.c) ------------------------------------------------------- */

/** One (path, asset) pair, linked into the list of its path and the list of its asset. */
typedef struct de__rindex_link {
    u64 path_hash; /**< de__rindex_path_hash of the normalised virtual path */
    de_asset_id id;
    u32 path_prev, path_next; /**< links of the same path (index + 1; 0 = none) */
    u32 id_next;              /**< links of the same asset, or the free list (index + 1) */
    u32 live;
} de__rindex_link;

/**
 * Source path -> asset IDs. Each asset has the set of paths its last successful load read: the
 * source (with the locale applied) and the extra sources the cooker recorded. Two hash maps give
 * the first link of a path and of an asset, so setting, removing and looking up cost O(paths of
 * that asset or that path), whatever the number of assets. The maps are only probed, never
 * iterated. Paths are compared by their 64-bit hash: a collision only reloads one asset more.
 * Main thread.
 */
typedef struct de__rindex {
    const de_allocator *alloc;
    de__rindex_link *links;
    u32 link_count, link_cap; /**< links used (live or free) / allocated */
    u32 free_head;            /**< free links (index + 1), through id_next */
    u32 count;                /**< live links */
    de_hashmap by_path;       /**< u64 path hash -> u32 first link + 1 */
    de_hashmap by_id;         /**< u64 asset ID -> u32 first link + 1 */
} de__rindex;

/** DE_ERR_OUT_OF_MEMORY never happens here (the maps allocate on first use); returns DE_OK. */
de_result de__rindex_init(de__rindex *r, const de_allocator *alloc);
void de__rindex_free(de__rindex *r);
/** The hash a path is indexed under: de_hash64 of the normalised path's bytes. Paths that do not
 * normalise are hashed as given. */
u64 de__rindex_path_hash(const char *vpath);
/**
 * Replaces the paths of `id` with `main_path` (NULL = none) and the `extra_count` NUL-separated
 * paths in `extra_block` (virtual paths; duplicates count once). DE_ERR_OUT_OF_MEMORY leaves
 * `id` with no paths.
 */
de_result de__rindex_set(de__rindex *r, de_asset_id id, const char *main_path,
                         const char *extra_block, u32 extra_count);
/** Forgets every path of `id`. */
void de__rindex_remove(de__rindex *r, de_asset_id id);
/** Calls `fn` for each asset that read `vpath` (newest first; the reload queue sorts by ID).
 * Returns the number of calls. */
u32 de__rindex_lookup(const de__rindex *r, const char *vpath,
                      void (*fn)(de_asset_id id, void *user), void *user);
/** Calls `fn` once per (path, asset) link: every indexed asset at least once, in link order.
 * Returns the number of calls. */
u32 de__rindex_each_id(const de__rindex *r, void (*fn)(de_asset_id id, void *user), void *user);

/* ---- Manifest diff (manifest_diff.c) ------------------------------------------------------- */

/** What decides whether an asset must reload, per asset of a manifest. */
typedef struct de__manifest_digest_entry {
    de_asset_id id;
    u64 type_hash;     /**< de_hash64_str of `type` */
    u64 source_hash;   /**< asset_root, source and every locale override, in file order */
    u64 settings_hash; /**< the effective settings: keys, value types and values */
} de__manifest_digest_entry;

/** The digest of one manifest: one entry per asset with a valid ID, sorted by ID. */
typedef struct de__manifest_digest {
    const de_allocator *alloc;
    de__manifest_digest_entry *entries;
    u32 count;
    u32 capacity; /**< entries allocated */
} de__manifest_digest;

/** Builds the digest of `m` with the IDs of `res` (both may be NULL: an empty digest).
 * DE_ERR_OUT_OF_MEMORY. */
de_result de__manifest_digest_build(const de__manifest *m, const de__resolved *res,
                                    const de_allocator *alloc, de__manifest_digest *out);
void de__manifest_digest_free(de__manifest_digest *d);

typedef enum de__manifest_change_kind {
    DE__MANIFEST_ADDED,   /**< only in the new manifest */
    DE__MANIFEST_REMOVED, /**< only in the old manifest */
    DE__MANIFEST_CHANGED  /**< in both, with another type, source or settings */
} de__manifest_change_kind;

/** de__manifest_change.what bits (CHANGED only). */
enum {
    DE__MANIFEST_CHANGED_TYPE = 1u << 0,
    DE__MANIFEST_CHANGED_SOURCE = 1u << 1,
    DE__MANIFEST_CHANGED_SETTINGS = 1u << 2
};

typedef struct de__manifest_change {
    de_asset_id id;
    de__manifest_change_kind kind;
    u32 what;
} de__manifest_change;

/**
 * The changes from `old_d` to `new_d`, sorted by ID, in `*out` (allocated from `alloc`, free
 * with DE_FREE(alloc, *out, sizeof(de__manifest_change) * *count); NULL when there is none).
 * Bundles are not compared: the new manifest's bundles apply to the loads that follow.
 * DE_ERR_OUT_OF_MEMORY.
 */
de_result de__manifest_diff(const de__manifest_digest *old_d, const de__manifest_digest *new_d,
                            const de_allocator *alloc, de__manifest_change **out, u32 *count);

/* ---- Slots (slots.c, DENG-214 block) -------------------------------------------------------- */

/** What de__slot_swap took out of a slot. */
typedef struct de__slot_swapped {
    void *object;
    u64 bytes;
    de_asset_id *deps; /**< owned by the caller now (system allocator) */
    u32 dep_count;
} de__slot_swapped;

/**
 * The version swap of a hot reload: puts `object` (`bytes` resident) and the dependency list
 * `deps` (owned by the slot from now on) into the live slot `handle_id`, bumps `version`, clears
 * `error`, and returns the previous object and dependency list in `*old`. The generation, the
 * state and the refcounts do not change, so every handle stays valid. Main thread.
 */
void de__slot_swap(de_asset_system *s, u32 handle_id, void *object, u64 bytes, de_asset_id *deps,
                   u32 dep_count, de__slot_swapped *old);

/** Calls `fn` for every live slot in slot index order. Main thread; `fn` must not free slots. */
void de__slot_for_each_live(de_asset_system *s,
                            void (*fn)(de_asset_system *s, u32 handle_id, void *user), void *user);

/* ---- The hot reload state (hot_reload.c) ---------------------------------------------------- */

/** Counters for tests. */
typedef struct de__asset_reload_stats {
    u64 changes;          /**< paths seen (watch events and de__asset_reload_on_change) */
    u64 queued;           /**< assets queued (once per frame each) */
    u64 started;          /**< reload jobs submitted */
    u64 superseded;       /**< reload jobs cancelled by a newer change */
    u64 swapped;          /**< reloads published (one RELOADED event each) */
    u64 failed;           /**< reloads that failed (old data kept) */
    u64 discarded;        /**< results dropped because the slot was no longer READY */
    u64 retired;          /**< old objects unloaded after reload_retire_frames */
    u64 dependents;       /**< dependency_reloaded calls */
    u64 skipped_pack;     /**< changes to assets a mounted pack serves */
    u64 manifest_reloads; /**< manifest reloads that succeeded */
    u32 watches;          /**< directory watches running */
} de__asset_reload_stats;

/** de_asset_system_create, when desc.hot_reload is set: creates `s->reload` (Debug and Dev
 * builds; elsewhere it logs at Info that hot reload is compiled out). DE_ERR_OUT_OF_MEMORY. */
de_result de__asset_reload_init(de_asset_system *s);
/** de_asset_system_destroy, after the pipeline shutdown (every reload job is gone) and before
 * the slots: unloads every retired object now, stops the watches, frees the state. */
void de__asset_reload_shutdown(de_asset_system *s);
/**
 * de_asset_system_update, before the load update: retires old objects whose frames ran out,
 * (re)creates the watches for the active manifest, polls them, reloads a changed manifest and
 * starts the reload jobs of the queued assets. The load update that follows publishes the jobs
 * that finished (inline ones in this same update).
 */
void de__asset_reload_update(de_asset_system *s);

/**
 * The watch-change entry point: `vpath` (a virtual path) changed. CREATED, MODIFIED, RENAMED
 * (`vpath` is the new name) and DELETED all count as a change. A directory (`is_dir`) that is
 * MODIFIED (the OS lost events), DELETED or RENAMED means "rescan": every indexed asset reloads
 * and the manifest is loaded again; a new directory changes nothing. Main thread. A no-op when
 * hot reload is off.
 */
void de__asset_reload_on_change(de_asset_system *s, const char *vpath, de_watch_change change,
                                bool is_dir);

/** Queues asset `id` for a reload at the next update, as if its source changed (the editor
 * service's asset.reimport, DENG-629). False when hot reload is off. Main thread. */
bool de__asset_reload_queue(de_asset_system *s, de_asset_id id);

/** Loads the manifest again now, as a change to it would at the next update: an editor that just
 * added an entry (DIDE's Create > 3D Object, DENG-650) need not wait for the file watcher. False
 * when hot reload is off. Main thread. */
bool de__asset_reload_manifest_now(de_asset_system *s);

/* Pipeline hooks (pipeline.c). `job` is a de__pjob. Main thread. */

/** A normal load published `job`: index the paths it read (no-op for pack jobs). */
void de__asset_reload_record_paths(de_asset_system *s, struct de_asset_load_ctx *job);
/**
 * A reload job loaded with its dependencies READY: swap it into its slot (see the top of the
 * file). Takes `deps` (dep_slot_count IDs), the job's object and its dependency refs (it zeroes
 * job->object and job->dep_slot_count). If the slot is no longer READY the new object is
 * unloaded and `deps` freed instead.
 */
void de__asset_reload_swap(de_asset_system *s, struct de_asset_load_ctx *job, de_asset_id *deps);
/** A reload job failed with `code` and `msg` (cause chain included): keep the old data. */
void de__asset_reload_failed(de_asset_system *s, struct de_asset_load_ctx *job, de_result code,
                             const char *msg);
/** job_free of a reload job: forgets it. */
void de__asset_reload_job_gone(de_asset_system *s, struct de_asset_load_ctx *job);

/* Tests. */

/** False: no directory watch is created (tests drive de__asset_reload_on_change themselves, so
 * the OS watcher cannot add a reload of its own). Default true. Call it right after create. */
void de__asset_reload_set_watching(de_asset_system *s, bool watching);
/** The counters (zeroes when hot reload is off). */
void de__asset_reload_get_stats(const de_asset_system *s, de__asset_reload_stats *out);

DE_EXTERN_C_END

#endif /* DE_ASSET_HOT_RELOAD_INTERNAL_H */
