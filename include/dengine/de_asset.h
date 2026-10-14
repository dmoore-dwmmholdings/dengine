/* de_asset.h — asset manifest, loading, status, streaming, hot reload. SPEC.md §11.
 *
 * Typical use:
 *   de_asset_manifest_load(assets, "dev:/assets.toml");
 *   de_load_handle h = de_asset_load_bundle(assets, "level_01", DE_LOAD_DEFAULT);
 *   ... each frame:
 *   de_load_info info;
 *   if (de_asset_status(assets, h, &info) == DE_LOAD_READY) start_level();
 *   else draw_progress(info.progress);
 */
#ifndef DE_ASSET_H
#define DE_ASSET_H
#include "de_base.h"
#include "de_ecs.h" /* de_event_type, de_resource_id (DENG-143) */
#include "de_memory.h"
#include "de_task.h"
DE_EXTERN_C_BEGIN

typedef struct de_asset_system de_asset_system;
typedef struct de_world de_world;

typedef struct de_asset_id {
    u64 hash;
} de_asset_id; /* hash of "player/model#Body" */
/**
 * The ID of the asset path `path`: de_hash64 (XXH3-64, seed 0) of its exact bytes, the same on
 * every platform and in every build. The hash is part of the pack format: it never changes
 * without a pack version bump. A hash of 0 is remapped to 1, so `{0}` always means "no asset".
 *
 * Rules (checked, never normalised; DENG-139): UTF-8, case-sensitive, `/` separates segments, no
 * leading or trailing `/`, no empty, `.` or `..` segment, no control character, `\`, `*` or `?`.
 * At most one `#` starts a non-empty sub-asset name (`"player/model#Body"`), which may contain
 * `/`. Allocation-free; safe on any thread.
 * @return the ID, or `{0}` with DE_ERR_INVALID_ARGUMENT in the error context for an invalid path.
 */
DE_API de_asset_id de_asset_id_from(const char *path);
/**
 * The path of `id` in the active manifest (reverse lookup for logs and tools), or NULL when no
 * asset of the manifest has that ID (sub-asset IDs are not in the manifest). Valid until the next
 * successful de_asset_manifest_load or destroy. Binary search; no allocation.
 */
DE_API const char *de_asset_id_path(const de_asset_system *s, de_asset_id id);

DE_HANDLE(de_load_handle);
DE_HANDLE(de_asset_type);

/* Typed handles. A handle points to a slot; reloads and streaming swap the data behind it. */
DE_HANDLE(de_texture);
DE_HANDLE(de_mesh);
DE_HANDLE(de_material);
DE_HANDLE(de_skeleton);
DE_HANDLE(de_anim_clip);
DE_HANDLE(de_sound);
DE_HANDLE(de_font);
DE_HANDLE(de_shader);
DE_HANDLE(de_prefab);
DE_HANDLE(de_tilemap);
DE_HANDLE(de_navmesh);
DE_HANDLE(de_video);
DE_HANDLE(de_blob);
DE_HANDLE(de_model);
DE_HANDLE(de_string_table);

/* ---------- System ---------- */
typedef enum de_asset_mode {
    DE_ASSET_MODE_SOURCE, /* dev: import sources via the in-process cooker + .decache */
    DE_ASSET_MODE_PACKED  /* shipping: cooked data from .dpak archives only */
} de_asset_mode;

typedef struct de_asset_budgets {
    u64 gpu_texture_bytes, gpu_mesh_bytes, audio_bytes, cpu_bytes; /* 0 = unlimited */
} de_asset_budgets;

typedef struct de_asset_system_desc {
    const de_allocator *allocator;
    de_task_system *tasks;
    de_asset_mode mode;
    const char *cache_dir;  /* default "cache:/decache" */
    u32 io_threads;         /* default 2 */
    u64 staging_ring_bytes; /* GPU upload ring, default 128 MiB */
    de_asset_budgets budgets;
    bool hot_reload;    /* dev builds only */
    const char *locale; /* "en", "ja", ... for locale overrides */
    /* ---- DENG-214: hot reload ---- */
    /** Hot reload (SPEC §11.9): the updates an old object stays loaded after a reload swapped
     * a new one in, before it goes to its type's `unload` (GPU frames in flight may still use
     * it). 0 = 3, the frames in flight. */
    u32 reload_retire_frames;
    /* DENG-215: the type registry the `scene` and `prefab` importer reads component types from
     * (normally the world's, de_world_types). NULL: scenes and prefabs fail to cook. Must outlive
     * the system; only read (by cooks, on cook threads), so register types before loading. */
    de_type_registry *types;
    /* DENG-215: true = do not register the built-in types `blob`, `scene` and `prefab` at create
     * (tests and tools that register their own types of those names). */
    bool no_builtin_types;
} de_asset_system_desc;

/**
 * Creates an asset system. `(de_asset_system_desc){0}` is valid: the default heap, no task
 * system, source mode, the defaults listed in the desc, and no locale. Strings in the desc are
 * copied. The allocator (and `tasks`) must outlive the system. Unless `no_builtin_types`, the
 * built-in types `blob`, `scene` and `prefab` are registered (DENG-215; see "Blobs, scenes and
 * prefabs").
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`out` is NULL), DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_asset_system_create(const de_asset_system_desc *desc, de_asset_system **out);
/** Destroys the system and everything it owns, the manifest included. NULL is ignored. */
DE_API void de_asset_system_destroy(de_asset_system *s);
/**
 * Call once per frame on the main thread (the app host does, after the platform event pump and
 * before `First`): with hot reload on, first polls the file watches and starts the reloads of
 * changed assets (DENG-214, see "Hot reload" below); then applies finished loads and swaps in
 * finished reloads, refreshes request states (DENG-141, DENG-142), and queues the asset events of
 * those changes (DENG-143), then evicts unreferenced assets of categories over budget (DENG-144;
 * see de_asset_set_budgets) and samples the I/O rate. Never blocks on I/O; allocates nothing
 * when nothing completed (and no file changed).
 *
 * The first call with a non-NULL `world` attaches the system to it (see "ECS events" below): it
 * registers the `de_asset_event` payload type and its event type, inserts the resource
 * `DeAssetSystemRef`, and adds the main-thread system `de.asset_events` to `DE_SCHEDULE_FIRST`,
 * which sends the queued events. A later call with another world logs a warning and keeps the
 * first one. With `world` NULL (tools, tests) no event is produced. The world must outlive the
 * system: de_asset_system_destroy removes the system and the resource from it.
 */
DE_API void de_asset_system_update(de_asset_system *s, de_world *world);

/* ---------- Manifest ---------- */

/** How bad a manifest issue is. */
typedef enum de_asset_issue_severity {
    DE_ASSET_ISSUE_INFO,    /**< noted, nothing to fix (e.g. the reserved `variants` key) */
    DE_ASSET_ISSUE_WARNING, /**< ignored input, e.g. an unknown key */
    DE_ASSET_ISSUE_ERROR    /**< the manifest is not usable */
} de_asset_issue_severity;

/** One problem found in a manifest. Strings stay valid until the next manifest load or destroy. */
typedef struct de_asset_manifest_issue {
    de_asset_issue_severity severity;
    de_result code;       /**< DE_ERR_PARSE, or the read error of a file that could not be read */
    const char *file;     /**< normalised virtual path of the manifest file */
    const char *key_path; /**< TOML key path, e.g. `assets."ui/logo".type`; "" for the file */
    u32 line;             /**< 1-based line, 0 when unknown */
    const char *message;
} de_asset_manifest_issue;

/**
 * Loads the project manifest `vpath` (`dev:/assets.toml`) and the files it includes
 * (`[project] include`, relative to the including file). Every problem is collected, not just the
 * first, and logged (category "asset"); de_asset_manifest_issues lists them. On success the new
 * manifest replaces the previous one; on failure the previous one stays active.
 * @return DE_OK, DE_ERR_PARSE (at least one error issue), DE_ERR_NOT_FOUND / DE_ERR_IO (the root
 *         file could not be read), DE_ERR_INVALID_ARGUMENT (bad `vpath`), DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_asset_manifest_load(de_asset_system *s, const char *vpath);
/**
 * The issues of the last de_asset_manifest_load (successful or not), in file order then key
 * order. `*count` gets their number (0 before any load). Valid until the next load or destroy.
 * For tools (`de-cook --check`) and tests.
 */
DE_API const de_asset_manifest_issue *de_asset_manifest_issues(const de_asset_system *s,
                                                               u32 *count);
/* ---------- Packs (DENG-212) ---------- */
/**
 * Mounts the .dpak pack at the virtual path `vpath` (SPEC §11.7). Packs form a stack: a lookup
 * walks it from the highest `priority` down, and among equal priorities the pack mounted last
 * wins. The first pack that holds an asset ID serves it, so a patch, DLC or mod pack mounted
 * above the base pack overrides the assets it contains (§21.3). Bundles merge the same way: the
 * topmost pack that defines a bundle name defines the whole bundle.
 *
 * DE_ASSET_MODE_PACKED loads every asset from the mounted packs and needs no manifest: typed
 * access and de_asset_load find the IDs and types in the packs, and de_asset_load_bundle takes the
 * bundles of the packs (before the manifest's, if one is loaded). In DE_ASSET_MODE_SOURCE a
 * mounted pack overrides the manifest source of the IDs it holds (patch testing in Dev) and the
 * manifest's bundles win.
 *
 * The pack is checked at mount (header, version, platform, every offset and size; Debug builds
 * also check the content hash), and its metadata and dictionaries are read; asset data is read
 * when an asset loads. Assets already loaded keep their data. Main thread.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL argument, invalid path); DE_ERR_ALREADY_EXISTS
 *         (that path is mounted); DE_ERR_NOT_FOUND / DE_ERR_IO (the file); DE_ERR_VERSION_MISMATCH
 *         (another pack format version); DE_ERR_UNSUPPORTED (cooked for another platform);
 *         DE_ERR_PARSE (not a pack, truncated or damaged); DE_ERR_OUT_OF_MEMORY. Failures set the
 *         error context and leave the stack unchanged.
 */
DE_API de_result de_asset_pack_mount(de_asset_system *s, const char *vpath, i32 priority);
/**
 * Takes the pack mounted from `vpath` off the stack: later loads of its assets go to the next
 * pack that holds them (or fail). Loads that read it now finish first; the file is closed when
 * the last of them is done. Assets already loaded keep their data. A path that is not mounted is
 * ignored with a warning. Main thread.
 */
DE_API void de_asset_pack_unmount(de_asset_system *s, const char *vpath);
/**
 * Selects the locale for `[assets."<id>".locale]` source overrides (`"ja"`, `"pt-BR"`; NULL or
 * "" = none). A tag without its own override falls back to its parent (`pt-BR` -> `pt`), then to
 * the asset's `source`. The string is copied; a tag longer than 35 bytes is refused with a log
 * warning and the previous locale stays.
 */
DE_API void de_asset_set_locale(de_asset_system *s, const char *locale);

/* ---------- Loading (the "hook") ---------- */
typedef enum de_load_flags {
    DE_LOAD_DEFAULT = 0,
    DE_LOAD_PRIORITY_CRITICAL = 1u << 0,
    DE_LOAD_PRIORITY_HIGH = 1u << 1,
    DE_LOAD_PRIORITY_LOW = 1u << 2,
    DE_LOAD_PRIORITY_BACKGROUND = 1u << 3,
    DE_LOAD_NO_EVENTS = 1u << 4,    /* suppress ECS events for this request */
    DE_LOAD_STOP_ON_ERROR = 1u << 5 /* cancel remaining assets after the first failure */
} de_load_flags;

/**
 * Load requests (DENG-141, SPEC §11.3). Each call starts one request and returns its handle; poll
 * it with de_asset_status. The request takes one reference on every asset it names and keeps it
 * until de_asset_release, also after it is final. Assets that are already loaded count at once;
 * the rest are handed to the load pipeline. Main thread.
 *
 * - de_asset_load_bundle: the bundle's members plus the members of every bundle it depends on
 *   (transitively, dependencies first), each asset once.
 * - de_asset_load_many: `ids` in order, duplicates dropped. `count` 0 is an empty request (READY).
 * - Priority: the most urgent DE_LOAD_PRIORITY_* bit in `flags` (CRITICAL > HIGH > LOW >
 *   BACKGROUND); with none, the bundle's manifest `priority`, else NORMAL. An asset that several
 *   requests wait on loads with the most urgent of their priorities.
 * - DE_LOAD_STOP_ON_ERROR: the first failed asset ends the request (PARTIAL if any asset is
 *   ready, else FAILED) and cancels its assets that no other request waits on.
 * - An unknown bundle or asset ID is not an error of the call: the request is FAILED (or the
 *   asset counts as failed) with DE_ERR_NOT_FOUND in de_load_info. A failed asset is loaded
 *   again by a later request once no waiting request still counts it as failed.
 *
 * @return The request handle (never 0 for an unknown bundle or ID). An invalid handle, with the
 *         error context set, only for a NULL system or bundle name, `ids` NULL with `count` > 0
 *         (DE_ERR_INVALID_ARGUMENT), or when out of memory or out of the 2^20 request slots
 *         (DE_ERR_OUT_OF_MEMORY).
 */
DE_API de_load_handle de_asset_load_bundle(de_asset_system *s, const char *bundle, u32 flags);
/** One asset; see de_asset_load_bundle. */
DE_API de_load_handle de_asset_load(de_asset_system *s, de_asset_id id, u32 flags);
/** Several assets; see de_asset_load_bundle. */
DE_API de_load_handle de_asset_load_many(de_asset_system *s, const de_asset_id *ids, u32 count,
                                         u32 flags);
/**
 * Ends a request that is not final yet: it becomes DE_LOAD_CANCELLED at once, and the pipeline
 * stops loading its assets that no other request waits on. The references stay until
 * de_asset_release. A final request, or a stale handle, is left alone. Main thread.
 */
DE_API void de_asset_cancel(de_asset_system *s, de_load_handle h);
/**
 * Drops the request's references (cancelling it first if it is not final) and frees it; the
 * handle is stale afterwards (status DE_LOAD_INVALID). Assets no request references any more stay
 * loaded until eviction. A stale handle is ignored. Main thread.
 */
DE_API void de_asset_release(de_asset_system *s, de_load_handle h); /* drops the reference */

/* ---------- Status ---------- */
typedef enum de_load_state {
    DE_LOAD_INVALID = 0,
    DE_LOAD_PENDING, /* queued, nothing read yet */
    DE_LOAD_LOADING,
    DE_LOAD_READY,   /* every asset loaded */
    DE_LOAD_PARTIAL, /* finished; at least one asset failed */
    DE_LOAD_FAILED,  /* finished; nothing usable */
    DE_LOAD_CANCELLED
} de_load_state;

/**
 * Aggregate of a request (de_asset_status). States: PENDING until any asset reports progress or
 * finishes, then LOADING; READY when every asset is loaded, PARTIAL when all finished (or
 * STOP_ON_ERROR ended it) with at least one failed and one ready, FAILED when nothing is usable.
 * `progress` is byte-weighted (bytes_done / bytes_total) while any size is known, else the share
 * of finished assets; it never decreases, stays below 1.0 until the request is final, and is 1.0
 * once it is (CANCELLED included). `first_failed`, `first_error` and `first_error_msg` describe the
 * first failed asset in request order ({0}, DE_OK and "" when none failed; an unknown bundle sets
 * only the error and message).
 */
typedef struct de_load_info {
    u32 assets_total, assets_ready, assets_failed;
    u64 bytes_total, bytes_done; /* bytes_total may grow as dependencies resolve */
    f32 progress;                /* 0..1, byte-weighted */
    de_asset_id first_failed;
    de_result first_error;
    char first_error_msg[256];
} de_load_info;

/**
 * The state of request `h` (DE_LOAD_INVALID for a stale or 0 handle), and its aggregate in
 * `*info` when not NULL (zeroed for a stale handle). Cached: O(1), no allocation, so games call it
 * every frame. The state moves on in de_asset_system_update, which applies finished loads.
 * Main thread.
 */
DE_API de_load_state de_asset_status(de_asset_system *s, de_load_handle h,
                                     de_load_info *info /* nullable */);
/** The state of one asset: DE_LOAD_INVALID if it was never requested (or has no slot), else
 * PENDING, LOADING, READY, FAILED or CANCELLED. Main thread. */
DE_API de_load_state de_asset_status_of(de_asset_system *s, de_asset_id id);
/**
 * Tools and tests only: blocks until request `h` is final or `timeout_ns` passed (UINT64_MAX =
 * no limit, 0 = poll once), applying finished loads meanwhile and running main-thread tasks when
 * called on the task system's main thread. Returns the state reached; on timeout also sets
 * DE_ERR_TIMEOUT in the error context. Asserts (Debug/Dev) when called from inside an ECS system
 * of a running schedule. Main thread.
 */
DE_API de_load_state de_asset_wait(de_asset_system *s, de_load_handle h, u64 timeout_ns);

/* Per-asset listing for loading screens and debug UI. */
/** One asset of a request: its state (de_asset_status_of) and its bytes (resident size when
 * READY, else the bytes read so far). */
typedef struct de_asset_status_entry {
    de_asset_id id;
    de_load_state state;
    u64 bytes;
} de_asset_status_entry;
/**
 * Writes the first min(`max`, count) assets of request `h`, in request order, to `out` (NULL with
 * `max` 0 to ask for the count) and returns the request's asset count (0 for a stale handle).
 * O(assets); no allocation. Main thread.
 */
DE_API u32 de_asset_status_list(de_asset_system *s, de_load_handle h, de_asset_status_entry *out,
                                u32 max);

/* ---------- ECS events (registered by de_asset_system_update on first use) ---------- */
/*
 * Asset events (SPEC §11.3, §7.1; DENG-143). Read them like any ECS event: declare the type from
 * de_asset_event_type() in `reads_events` and use de_event_reader_create / de_event_next. The
 * payload type is registered as "de_asset_event" in the world's type registry.
 *
 * - DE_ASSET_EV_LOADED / DE_ASSET_EV_FAILED: an asset's slot reached READY / FAILED (`id`,
 *   `state`, `error`; `handle` is 0). Sent once per load of the asset, and only when at least one
 *   request that waits on the asset was made without DE_LOAD_NO_EVENTS. An asset that is already
 *   READY when a request names it, and an ID that is not in the manifest, send no per-asset event
 *   (the request's REQUEST_DONE reports them).
 * - DE_ASSET_EV_REQUEST_DONE: load handle `handle` reached a final state (`state`: READY,
 *   PARTIAL, FAILED or CANCELLED; `error`: the first failure in request order, or DE_OK); `id` is
 *   0. Not sent for requests made with DE_LOAD_NO_EVENTS, nor for a request that
 *   de_asset_release ends (its handle is stale by then).
 * - DE_ASSET_EV_RELOADED (S08) and DE_ASSET_EV_EVICTED (DENG-144) go through the same path.
 *
 * Order: the order the changes happen on the main thread. Inside de_asset_system_update that is
 * completion order (LOADED / FAILED), then request index order (REQUEST_DONE); changes made by
 * calls between two updates (a request that is final at once, de_asset_cancel, de_asset_wait)
 * come in call order. The events are queued and sent by the `de.asset_events` system at the start
 * of `DE_SCHEDULE_FIRST`, after the event frame step, so each one is readable in that frame and
 * the next (two-frame lifetime) even though the host updates assets before `First`.
 */
typedef enum de_asset_event_kind {
    DE_ASSET_EV_LOADED,
    DE_ASSET_EV_FAILED,
    DE_ASSET_EV_RELOADED,
    DE_ASSET_EV_EVICTED,
    DE_ASSET_EV_REQUEST_DONE /* a load handle reached a final state */
} de_asset_event_kind;

typedef struct de_asset_event {
    de_asset_event_kind kind;
    de_load_handle handle; /* set for REQUEST_DONE */
    de_asset_id id;        /* set for per-asset events */
    de_load_state state;
    de_result error;
} de_asset_event;

/** Resource holding the asset system of a world (SPEC §7.1), inserted by the first
 * de_asset_system_update with that world; type name "DeAssetSystemRef". */
typedef struct DeAssetSystemRef {
    de_asset_system *ptr;
} DeAssetSystemRef;

/** The asset event type in the attached world, or an invalid type before the first
 * de_asset_system_update with a world. Main thread. */
DE_API de_event_type de_asset_event_type(const de_asset_system *s);
/** The `DeAssetSystemRef` resource in the attached world, or an invalid id before the first
 * de_asset_system_update with a world. Main thread. */
DE_API de_resource_id de_asset_resource(const de_asset_system *s);

/* ---------- Typed access (valid handle even before READY; data appears when loaded) ---------- */
/**
 * Typed access (DENG-140): the handle of the slot of asset `id`, created on first use. The handle
 * is valid before the asset is loaded and stays the same across reloads; it takes no reference
 * (load the asset with de_asset_load / de_asset_load_bundle for that). Each function checks the
 * asset's manifest `type` against its own (`de_asset_texture` wants `type = "texture"`, then
 * "mesh", "model", "material", "sound", "font", "prefab", "blob", "shader"). Main thread.
 *
 * Returns an invalid handle (id 0) and sets the error context when `id` is 0 or the asset has
 * another type (DE_ERR_INVALID_ARGUMENT), when the active manifest does not declare `id`
 * (DE_ERR_NOT_FOUND), or when all 2^20 asset slots are in use (DE_ERR_OUT_OF_MEMORY).
 *
 * A handle is a 20-bit slot index and a 12-bit generation: once a slot is freed, the old handle
 * is stale and resolves to nothing. Eviction (DENG-144) does not free the slot: the handle stays
 * valid, it resolves to no data until the asset is loaded again, then to the new data.
 */
DE_API de_texture de_asset_texture(de_asset_system *s, de_asset_id id);
DE_API de_mesh de_asset_mesh(de_asset_system *s, de_asset_id id);
DE_API de_model de_asset_model(de_asset_system *s, de_asset_id id);
DE_API de_material de_asset_material(de_asset_system *s, de_asset_id id);
DE_API de_sound de_asset_sound(de_asset_system *s, de_asset_id id);
DE_API de_font de_asset_font(de_asset_system *s, de_asset_id id);
DE_API de_prefab de_asset_prefab(de_asset_system *s, de_asset_id id);
DE_API de_blob de_asset_blob(de_asset_system *s, de_asset_id id);
/** A `shader` asset (type "shader", DENG-289). The shader type is registered by the GPU side
 * (src/gpu/shader/), which reads the cooked blobs behind the handle. */
DE_API de_shader de_asset_shader(de_asset_system *s, de_asset_id id);
/**
 * The prefab of model `m` (DENG-105). The model importer cooks the node hierarchy of a model into
 * its sub-asset `<model>#prefab` (type `prefab`, DENG-101), which is a dependency of the model and
 * so is READY with it. de_asset_prefab refuses the model's own ID (its type is `model`); pass the
 * model's handle here, or the sub-asset's ID to de_asset_prefab:
 * `de_asset_prefab(s, de_asset_id_from("props/crate#prefab"))`. Returns an invalid handle (no
 * error context) when `m` is stale, is not a READY `model`, or has no prefab. Main thread.
 */
DE_API de_prefab de_model_prefab(de_asset_system *s, de_model m);
/**
 * The bytes of a READY `blob` asset: exactly the source file's bytes, followed by a NUL that is
 * not counted, with their count in `*size` (may be NULL). NULL (and size 0) for an invalid or
 * stale handle, or a blob that is not loaded. Valid until the asset is evicted or reloaded. Any
 * thread, as long as the asset stays loaded (DENG-215).
 */
DE_API const void *de_blob_data(de_asset_system *s, de_blob b, u64 *size);

/* ---------- Custom asset types ---------- */
typedef struct de_asset_import_ctx de_asset_import_ctx; /* cooker side */
typedef struct de_asset_load_ctx de_asset_load_ctx;     /* runtime side */

/**
 * The budget an asset type's resident bytes count against (DENG-144, SPEC §11.8). The zero value
 * is CPU, so a zeroed de_asset_type_desc is a CPU type. Values outside the enum count as CPU.
 * Note the order differs from de_asset_stats.bytes_resident (see DE_ASSET_BYTES_*).
 */
typedef enum de_asset_category {
    DE_ASSET_CAT_CPU = 0,     /**< CPU-side data: de_asset_budgets.cpu_bytes (the default) */
    DE_ASSET_CAT_GPU_TEXTURE, /**< de_asset_budgets.gpu_texture_bytes */
    DE_ASSET_CAT_GPU_MESH,    /**< de_asset_budgets.gpu_mesh_bytes */
    DE_ASSET_CAT_AUDIO,       /**< de_asset_budgets.audio_bytes */
    DE_ASSET_CAT_COUNT
} de_asset_category;

typedef struct de_asset_type_desc {
    const char *name; /* manifest "type = ..." */
    const char *const *extensions;
    u32 extension_count;
    u32 importer_version; /* bump to invalidate cached cooks */
    /* Cooker: source bytes + settings (TOML table) → cooked bytes. Dev and de-cook. */
    de_result (*import)(de_asset_import_ctx *ctx);
    /* Runtime: cooked bytes → live object in the slot. Runs on a worker unless main_thread_load. */
    de_result (*load)(de_asset_load_ctx *ctx);
    void (*unload)(void *object, void *user);
    void *user;
    bool main_thread_load;
    /* DENG-144: the budget the type's resident bytes (de_asset_load_ctx_set_object) count
     * against. Default (0) CPU. */
    de_asset_category category;
    /* ---- DENG-214: hot reload ---- */
    /** Optional. Hot reload (SPEC §11.9) calls it on the main thread, inside
     * de_asset_system_update, for each loaded asset of this type whose dependency `dep` was
     * reloaded, once per reload and after the swap: `object` is the dependent's live object,
     * which can now rebind to the new data of `dep` (a material to its texture). NULL = none. */
    void (*dependency_reloaded)(void *object, de_asset_id dep, void *user);
    /* ---- DENG-289: own reload ---- */
    /** Optional. Hot reload calls it on the main thread, inside de_asset_system_update, once for
     * each reload of an asset of this type, right after the swap and the RELOADED event and
     * before the dependents' dependency_reloaded: `object` is the asset's new live object, `id`
     * the asset. The old object is still loaded (it goes to `unload` reload_retire_frames
     * updates later). Shaders use it to rebuild their pipelines. NULL = none. */
    void (*reloaded)(void *object, de_asset_id id, void *user);
    /* ---- DENG-105: publish after the GPU uploads ---- */
    /** Optional. Called on the main thread, inside de_asset_system_update, before a loaded object
     * of this type is published: after `load` returned it and the asset's dependencies are READY
     * (for a hot reload: before the swap). DE_OK publishes it now. DE_ERR_BUSY keeps the asset
     * LOADING (it counts in de_asset_stats.uploads_in_flight) and asks again at every later
     * update. Any other result fails the asset, with the error context's message when its code
     * matches. The render types use it so that a texture or mesh is READY only once the GPU has
     * completed its copies (SPEC §11.4), and to turn asset IDs into typed handles, which is
     * main-thread work. A load that is cancelled while it waits gives its object to `unload`.
     * NULL = publish at once. */
    de_result (*ready)(void *object, void *user);
} de_asset_type_desc;

/**
 * Registers an asset type (DENG-140), so manifest assets with `type = "<name>"` can be imported
 * and loaded. The desc, the name and the extension strings are copied; extensions may be written
 * with or without the leading dot (`".png"` or `"png"`) and match without ASCII case. When two
 * types claim one extension, the first keeps it (a warning is logged). `importer_version` is kept
 * for the cook cache (S08). Main thread; register types before loading assets of them. Types are
 * never unregistered; at most 256 exist.
 *
 * Returns an invalid handle (id 0) and sets the error context on failure:
 * DE_ERR_INVALID_ARGUMENT (NULL argument; a name that is empty, longer than 63 bytes, or has
 * spaces, quotes or non-ASCII bytes; a NULL or malformed extension), DE_ERR_ALREADY_EXISTS (a
 * type with this name exists), DE_ERR_OUT_OF_MEMORY (256 types, or no memory).
 */
DE_API de_asset_type de_asset_type_register(de_asset_system *s, const de_asset_type_desc *desc);

/* ---------- Load context (DENG-142) ----------
 *
 * A type's `load` gets one de_asset_load_ctx per load. It runs on a frame worker of the task
 * system (on the main thread, inside de_asset_system_update, when the type sets
 * `main_thread_load`; on an I/O thread when the system has no task system). The context and the
 * bytes are valid only during that call. These functions may be called only from the loader,
 * on the thread that runs it. The asset system's allocator must be thread-safe: loads allocate
 * from it on these threads.
 *
 * A loader returns DE_OK after de_asset_load_ctx_set_object, or an error (the asset is FAILED).
 * An object set before a failure, or before the load was cancelled, is passed to the type's
 * `unload`. Dependencies declared with de_asset_load_ctx_add_dependency are loaded at the
 * asset's priority; the asset becomes READY in the same de_asset_system_update as the last of
 * them, or FAILED if one fails (its message names the dependency and the dependency's cause).
 */

/** The bytes to load (the asset's stored bytes, decompressed) and their count in `*size`. Never
 * NULL (an empty asset gives "" and 0). */
DE_API const void *de_asset_load_ctx_data(const de_asset_load_ctx *ctx, u64 *size);
/** The asset being loaded. */
DE_API de_asset_id de_asset_load_ctx_id(const de_asset_load_ctx *ctx);
/** The type's `user` pointer (de_asset_type_desc.user). */
DE_API void *de_asset_load_ctx_user(const de_asset_load_ctx *ctx);

/**
 * The asset's settings from the manifest (`[defaults.<type>]` overlaid by the asset's own keys).
 * Each getter returns `fallback` when the key is missing or has another type; the float getter
 * also accepts an integer. Strings stay valid during the load.
 */
DE_API bool de_asset_load_ctx_settings_has(const de_asset_load_ctx *ctx, const char *key);
DE_API bool de_asset_load_ctx_settings_bool(const de_asset_load_ctx *ctx, const char *key,
                                            bool fallback);
DE_API i64 de_asset_load_ctx_settings_int(const de_asset_load_ctx *ctx, const char *key,
                                          i64 fallback);
DE_API f64 de_asset_load_ctx_settings_float(const de_asset_load_ctx *ctx, const char *key,
                                            f64 fallback);
DE_API const char *de_asset_load_ctx_settings_string(const de_asset_load_ctx *ctx, const char *key,
                                                     const char *fallback);
/** A string-array setting: its items in `*items` (NULL when missing) and their count (0). */
DE_API u32 de_asset_load_ctx_settings_string_array(const de_asset_load_ctx *ctx, const char *key,
                                                   const char *const **items);

/**
 * Sets the live object the slot gets when the asset is published, and its resident size in
 * bytes (for budgets). A second call replaces the first object, which goes to the type's
 * `unload`.
 */
DE_API void de_asset_load_ctx_set_object(de_asset_load_ctx *ctx, void *object, u64 bytes);
/**
 * Declares that the asset needs asset `id` (a texture of a material, ...). The dependency is
 * loaded at the asset's priority and held with an asset -> asset reference while the asset is
 * loaded. Duplicates count once. A dependency cycle found at run time fails the asset that
 * closes it (DE_ERR_PARSE).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`id` 0), DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_asset_load_ctx_add_dependency(de_asset_load_ctx *ctx, de_asset_id id);
/**
 * Records why the load fails: `result` (DE_OK becomes DE_ERR_INTERNAL) and a printf-style
 * message, which the asset's failure message carries after `asset "<path>": `. Returns `result`,
 * so a loader writes `return de_asset_load_ctx_fail(ctx, DE_ERR_PARSE, "bad header");`. The
 * first recorded failure wins.
 */
DE_API de_result de_asset_load_ctx_fail(de_asset_load_ctx *ctx, de_result result, const char *fmt,
                                        ...) DE_PRINTF_FMT(3, 4);

/* ---------- Import context (DENG-209) ----------
 *
 * A type's `import` (de_asset_type_desc.import) gets one de_asset_import_ctx per import. The
 * cooker library runs it: `de-cook`, and in Debug and Dev builds the runtime itself, which cooks
 * source assets in-process on a background task (SPEC §11.5). These functions exist only where
 * the cooker is linked: in builds that define DE_HAS_COOKER (Debug and Dev with tools, the
 * default) and in the tools. Release and Shipping link no cooker, so guard importer code with
 * `#if DE_HAS_COOKER` and register `.import = NULL` there.
 *
 * An importer reads the source and the settings, writes the cooked bytes, and may declare
 * dependencies, sub-assets and further source files. It must be deterministic: the same inputs
 * give the same bytes. It must not read clocks, the environment, absolute paths, pointer values
 * or uninitialised padding, and must not depend on the thread count or the thread it runs on.
 * The cooker can run each import twice and compare (de_cooker_desc.verify_determinism, on by
 * default in Debug builds). The context and everything it returns are valid only during the
 * `import` call, on the thread that runs it.
 *
 * An importer returns DE_OK, or an error (the cook fails, and so does the asset's load).
 */

/** The platform a cook targets (the values match the `.dpak` header's platform). */
typedef enum de_cook_platform {
    DE_COOK_PLATFORM_HOST = 0, /**< only in a cooker desc: the platform the cooker runs on */
    DE_COOK_PLATFORM_WINDOWS = 1,
    DE_COOK_PLATFORM_LINUX = 2,
    DE_COOK_PLATFORM_MACOS = 3
} de_cook_platform;

/** The source file's bytes and their count in `*size`. Never NULL ("" for an empty file). */
DE_API const void *de_asset_import_source(const de_asset_import_ctx *ctx, u64 *size);
/** The source path relative to the project's asset root (`textures/player.png`), normalised.
 * Never an absolute path, so it may go into cooked data. */
DE_API const char *de_asset_import_source_path(const de_asset_import_ctx *ctx);
/** The asset being imported. */
DE_API de_asset_id de_asset_import_id(const de_asset_import_ctx *ctx);
/** The platform the cook targets (never DE_COOK_PLATFORM_HOST). */
DE_API de_cook_platform de_asset_import_platform(const de_asset_import_ctx *ctx);
/** The type's `user` pointer (de_asset_type_desc.user). */
DE_API void *de_asset_import_user(const de_asset_import_ctx *ctx);

/**
 * The asset's settings from the manifest (`[defaults.<type>]` overlaid by the asset's own keys),
 * as for de_asset_load_ctx_settings_*: each getter returns `fallback` when the key is missing or
 * has another type; the float getter also accepts an integer. Every setting is part of the
 * settings hash, whether the importer reads it or not.
 */
DE_API bool de_asset_import_setting_has(const de_asset_import_ctx *ctx, const char *key);
DE_API bool de_asset_import_setting_bool(const de_asset_import_ctx *ctx, const char *key,
                                         bool fallback);
DE_API i64 de_asset_import_setting_int(const de_asset_import_ctx *ctx, const char *key,
                                       i64 fallback);
DE_API f64 de_asset_import_setting_float(const de_asset_import_ctx *ctx, const char *key,
                                         f64 fallback);
DE_API const char *de_asset_import_setting_string(const de_asset_import_ctx *ctx, const char *key,
                                                  const char *fallback);
/** A string-array setting: its items in `*items` (NULL when missing) and their count (0). */
DE_API u32 de_asset_import_setting_string_array(const de_asset_import_ctx *ctx, const char *key,
                                                const char *const **items);

/**
 * Reads another source file (a glTF buffer, an included file), `rel_path` relative to the
 * directory of the source file (`../shared/pal.png` is fine). The normalised path must stay
 * below the asset root. The file is recorded in the cook result's extra sources, so a change to
 * it invalidates the cooked asset. `*data` (NUL-terminated after `*size` bytes) stays valid until
 * the import returns.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL argument, an absolute or virtual path, a path that
 *         leaves the asset root); DE_ERR_NOT_FOUND; DE_ERR_IO; DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_asset_import_read_file(de_asset_import_ctx *ctx, const char *rel_path,
                                           const void **data, u64 *size);
/** Appends `size` bytes to the cooked output (call it as often as needed).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`data` NULL with `size` > 0), DE_ERR_OUT_OF_MEMORY. */
DE_API de_result de_asset_import_write(de_asset_import_ctx *ctx, const void *data, u64 size);
/**
 * Declares that the asset needs asset `id` at run time (a material's textures). The runtime
 * loads it with the asset; packs store it in the TOC. Duplicates count once.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`id` 0), DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_asset_import_add_dependency(de_asset_import_ctx *ctx, de_asset_id id);
/**
 * Declares a sub-asset `<asset id>#<name>` (a model's meshes) with its own type and cooked
 * bytes; the bytes are copied. The sub-asset loads with the loader of `type_name`.
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL argument, an empty name or type, a name with `#`,
 *         an ID that de_asset_id_from refuses); DE_ERR_ALREADY_EXISTS (the name was declared);
 *         DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_asset_import_add_subasset(de_asset_import_ctx *ctx, const char *name,
                                              const char *type_name, const void *data, u64 size);
/** A scratch allocator for the importer's temporaries. Everything still allocated from it is
 * freed when the import returns. Not thread-safe: use it on the importer's thread only. */
DE_API const de_allocator *de_asset_import_allocator(de_asset_import_ctx *ctx);
/**
 * Records why the import fails: `result` (DE_OK becomes DE_ERR_INTERNAL) and a printf-style
 * message, which the cook's error carries after `asset "<id>": `. Returns `result`, so an importer
 * writes `return de_asset_import_fail(ctx, DE_ERR_PARSE, "bad header");`. The first recorded
 * failure wins, and the cook fails even if the importer then returns DE_OK.
 */
DE_API de_result de_asset_import_fail(de_asset_import_ctx *ctx, de_result result, const char *fmt,
                                      ...) DE_PRINTF_FMT(3, 4);
/* ---------- End of the import context (DENG-209) ---------- */

/* ---------- Blobs, scenes and prefabs (DENG-215) ----------
 *
 * Built-in types, registered at de_asset_system_create (SPEC §11.1, §11.6, §9.2):
 *
 * - `blob`: raw bytes. Packs and the cooker copy the source unchanged (the asset setting
 *   `compress = true|false` only decides whether a pack compresses it); de_blob_data reads them.
 * - `scene` and `prefab`: TOML sources (DENG-2 Q9), cooked to a binary scene. A prefab has
 *   exactly one root entity; a scene any number. Source:
 *
 *       [[entity]]
 *       id = "0x9f3a6c0d5e2b4471"     # stable 64-bit scene ID (de_scene_id_new), or an integer
 *       parent = "0x1c2d..."          # optional: another entity of the same file
 *       [entity.Health]               # one table per component, named by its reflected type
 *       current = 75.0
 *       [entity.Sprite]
 *       texture = "ui/heart"          # ASSET fields: an asset ID path (a cook dependency)
 *       target = "0x1c2d..."          # ENTITY fields: the scene ID of an entity of the file
 *
 *   Component tables are read with the text serializer (de_deserialize_object rules) against
 *   de_asset_system_desc.types and written with the binary serializer (self-describing). An
 *   unknown component type, an unknown field and a key that is not `id`, `parent` or a table
 *   log a warning and are skipped. `DeChildOf` and `DeChildren` tables are skipped too: the
 *   hierarchy comes from `parent`. Asset references are cook dependencies and load before the
 *   scene or prefab is READY. The output depends only on the content: entities are sorted by
 *   scene ID and components by type name, so the key order of the TOML never matters.
 */

/** A `scene` asset as a de_prefab handle: a scene is a prefab with any number of roots, and
 * de_prefab_instantiate spawns both. Like the typed accessors above (type "scene"). */
DE_API de_prefab de_asset_scene(de_asset_system *s, de_asset_id id);

/**
 * Spawns a READY prefab or scene `p` into world `w` (immediate mode: main thread, not inside a
 * running system; commands are a later task). Every entity is spawned with its components; each
 * component is found in `w` by its type name (de_component_find) and read with the binary
 * serializer from the world's type registry. A stored type version older than the registered one
 * runs the type's `migrate` hook; a component whose type is not a component of `w` is skipped
 * with a warning, and so is one that `migrate` drops. ENTITY fields that name a scene ID of the
 * file point at the entity spawned for it (else DE_ENTITY_NULL); ASSET fields become typed
 * handles (typed access by the field's asset type). Then `parent` links are set with
 * de_entity_set_parent, children in scene-ID order, and the roots are parented to `parent`
 * (DE_ENTITY_NULL: they stay roots).
 *
 * `out_root` (nullable) gets the root entity: the prefab's one root, or a scene's first root in
 * scene-ID order (DE_ENTITY_NULL for an empty scene).
 * @return DE_OK; DE_ERR_INVALID_ARGUMENT (NULL system or world, a handle that is not a prefab or
 *         scene, `parent` not alive); DE_ERR_NOT_FOUND (not loaded); DE_ERR_PARSE (a component's
 *         bytes are damaged); DE_ERR_OUT_OF_MEMORY; the ECS's errors. On failure every entity
 *         spawned so far is despawned again.
 */
DE_API de_result de_prefab_instantiate(de_asset_system *s, de_world *w, de_prefab p,
                                       de_entity parent, de_entity *out_root);
/* ---------- End of blobs, scenes and prefabs (DENG-215) ---------- */

/* ---------- Streaming and residency ---------- */
/*
 * Budgets and eviction (DENG-144, SPEC §11.8). Each READY asset counts its resident bytes (from
 * de_asset_load_ctx_set_object) against the budget of its type's category. An asset that no load
 * request and no other asset references is an eviction candidate; candidates are ordered least
 * recently used first: by the frame (de_asset_system_update count) in which their last reference
 * was dropped or typed access last touched them, then by asset ID, so the order never depends on
 * thread timing. While a category is over its budget, de_asset_system_update evicts its
 * candidates in that order, at most 64 per update. Members of a bundle with `resident = true`
 * that were loaded through a bundle request are never evicted. A category that is over budget
 * with no candidate left logs a warning (at most once per second per category); referenced
 * assets are never evicted.
 *
 * Evicting an asset passes its object to the type's `unload`, drops the references it held on
 * its dependencies, and sets its state back to DE_LOAD_INVALID. Its slot stays: typed handles
 * keep working and the asset loads again on the next request. Each eviction sends one
 * DE_ASSET_EV_EVICTED event (state DE_LOAD_INVALID).
 */
/**
 * Sets the per-category budgets in bytes (0 = unlimited; NULL = all unlimited). The defaults
 * come from de_asset_system_desc.budgets. Takes effect at the next de_asset_system_update.
 * Main thread.
 */
DE_API void de_asset_set_budgets(de_asset_system *s, const de_asset_budgets *b);
/**
 * Evicts every candidate now (no per-call cap, budgets ignored), also the dependencies that an
 * eviction leaves unreferenced. Resident bundle members and referenced assets stay. Main thread.
 */
DE_API void de_asset_flush_unused(de_asset_system *s); /* evict every zero-ref asset now */
typedef struct de_streaming_source {
    f64 x, y, z;
    f32 radius;
} de_streaming_source;
DE_API void de_asset_set_streaming_sources(de_asset_system *s, const de_streaming_source *src,
                                           u32 count);

typedef struct de_asset_stats {
    u32 loads_in_flight, uploads_in_flight, assets_resident;
    u64 bytes_resident[4]; /* gpu tex, gpu mesh, audio, cpu */
    u64 io_bytes_per_sec;
} de_asset_stats;
/** Indices of de_asset_stats.bytes_resident (DENG-144). */
enum {
    DE_ASSET_BYTES_GPU_TEXTURE = 0,
    DE_ASSET_BYTES_GPU_MESH = 1,
    DE_ASSET_BYTES_AUDIO = 2,
    DE_ASSET_BYTES_CPU = 3
};
/**
 * Residency counters (DENG-144): `bytes_resident` per category and `assets_resident` count the
 * READY assets that hold an object; `loads_in_flight` the assets submitted to the load backend
 * and not finished; `uploads_in_flight` the loaded assets that wait for their GPU copies (their
 * type's `ready` hook said DE_ERR_BUSY; DENG-105);
 * `io_bytes_per_sec` the bytes the backend reported read, averaged over the last second (it is
 * sampled by de_asset_system_update). All zero for a NULL system. O(loaded slots) for
 * `loads_in_flight`; no allocation. Main thread.
 */
DE_API de_asset_stats de_asset_get_stats(const de_asset_system *s);

/* ---------- Hot reload (DENG-214) ----------
 *
 * SPEC §11.9. With de_asset_system_desc.hot_reload set, Debug and Dev builds (never Shipping)
 * watch the manifest files and the files every load read: its source (with the locale applied)
 * and the extra sources its importer read (de_asset_import_read_file). When one changes, the
 * next de_asset_system_update starts a reload of each loaded asset that read it: the asset is
 * cooked again (Dev and Debug cook in-process) and loaded into a new object while the old one
 * stays live. A later update (the same one when the work is quick) swaps the new object in
 * behind the slot: typed handles stay valid and resolve to the new data, the slot's version is
 * bumped, one DE_ASSET_EV_RELOADED event is sent (`id`, state READY, error DE_OK), and every
 * loaded asset that depends on it gets its type's `dependency_reloaded`. The old object goes to
 * `unload` reload_retire_frames updates later.
 *
 * A reload that fails keeps the old data, logs the cause at Error and sends no event;
 * de_asset_last_error gives the message until a later reload succeeds. A changed manifest is
 * loaded again: assets whose type, source or settings changed reload, added assets become
 * loadable, removed ones keep their data while referenced (a warning is logged), and bundle
 * changes apply to later loads. An asset whose type changed is not reloaded (its slot keeps the
 * type it was created with; an error is kept for it). Assets that a mounted pack serves are not
 * reloaded from source. In Release and Shipping builds `hot_reload` is ignored.
 */
/**
 * The last error of asset `id`, for the debug UI (S30): the failure of its last hot reload while
 * it still holds (the old data stays loaded), else, when the asset is FAILED, the failure of its
 * last load; DE_OK when there is none (or no slot exists for `id`). The message, with its cause
 * chain, is written to `msg` (`cap` bytes, always NUL-terminated; "" for DE_OK; `msg` may be
 * NULL with `cap` 0). Does not set the error context. Main thread.
 * @return The asset's error code, DE_OK, or DE_ERR_INVALID_ARGUMENT for a NULL system.
 */
DE_API de_result de_asset_last_error(de_asset_system *s, de_asset_id id, char *msg, u32 cap);
/* ---------- End of hot reload (DENG-214) ---------- */

DE_EXTERN_C_END
#endif
