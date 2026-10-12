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
} de_asset_system_desc;

/**
 * Creates an asset system. `(de_asset_system_desc){0}` is valid: the default heap, no task
 * system, source mode, the defaults listed in the desc, and no locale. Strings in the desc are
 * copied. The allocator (and `tasks`) must outlive the system.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`out` is NULL), DE_ERR_OUT_OF_MEMORY.
 */
DE_API de_result de_asset_system_create(const de_asset_system_desc *desc, de_asset_system **out);
/** Destroys the system and everything it owns, the manifest included. NULL is ignored. */
DE_API void de_asset_system_destroy(de_asset_system *s);
/* Call once per frame on the main thread: publishes finished loads, sends ECS events,
 * applies hot reloads, runs eviction. DENG-141: applies finished loads and updates request
 * states; `world` may be NULL (events come with DENG-143). */
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
DE_API de_result de_asset_pack_mount(de_asset_system *s, const char *vpath, i32 priority);
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

/* ---------- Typed access (valid handle even before READY; data appears when loaded) ---------- */
/**
 * Typed access (DENG-140): the handle of the slot of asset `id`, created on first use. The handle
 * is valid before the asset is loaded and stays the same across reloads; it takes no reference
 * (load the asset with de_asset_load / de_asset_load_bundle for that). Each function checks the
 * asset's manifest `type` against its own (`de_asset_texture` wants `type = "texture"`, then
 * "mesh", "model", "material", "sound", "font", "prefab", "blob"). Main thread.
 *
 * Returns an invalid handle (id 0) and sets the error context when `id` is 0 or the asset has
 * another type (DE_ERR_INVALID_ARGUMENT), when the active manifest does not declare `id`
 * (DE_ERR_NOT_FOUND), or when all 2^20 asset slots are in use (DE_ERR_OUT_OF_MEMORY).
 *
 * A handle is a 20-bit slot index and a 12-bit generation: once the asset is evicted and its slot
 * freed, the old handle is stale and resolves to nothing.
 */
DE_API de_texture de_asset_texture(de_asset_system *s, de_asset_id id);
DE_API de_mesh de_asset_mesh(de_asset_system *s, de_asset_id id);
DE_API de_model de_asset_model(de_asset_system *s, de_asset_id id);
DE_API de_material de_asset_material(de_asset_system *s, de_asset_id id);
DE_API de_sound de_asset_sound(de_asset_system *s, de_asset_id id);
DE_API de_font de_asset_font(de_asset_system *s, de_asset_id id);
DE_API de_prefab de_asset_prefab(de_asset_system *s, de_asset_id id);
DE_API de_blob de_asset_blob(de_asset_system *s, de_asset_id id);
DE_API const void *de_blob_data(de_asset_system *s, de_blob b, u64 *size);

/* ---------- Custom asset types ---------- */
typedef struct de_asset_import_ctx de_asset_import_ctx; /* cooker side */
typedef struct de_asset_load_ctx de_asset_load_ctx;     /* runtime side */

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

/* ---------- Streaming and residency ---------- */
DE_API void de_asset_set_budgets(de_asset_system *s, const de_asset_budgets *b);
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
DE_API de_asset_stats de_asset_get_stats(const de_asset_system *s);

DE_EXTERN_C_END
#endif
