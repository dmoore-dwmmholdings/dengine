/*
 * engine_internal.h: the de_engine behind de_game.h, and the game.toml project config
 * (internal to src/host/, not installed). SPEC §10.1, §4.2, §5.8.
 *
 * Include as "host/engine_internal.h". Owner: DENG-90. Later S06 tasks (DENG-91 main loop,
 * DENG-92 game loading, DENG-93 hot reload, DENG-95 headless, DENG-96 console) read these
 * declarations; add to them at the end of a section, do not reorder.
 */
#ifndef DE_HOST_ENGINE_INTERNAL_H
#define DE_HOST_ENGINE_INTERNAL_H

#include <dengine/de_game.h>

#include "platform/de_atomic.h"

DE_EXTERN_C_BEGIN

/* ---------- game.toml (game_toml.c) ----------
 *
 * The project file. Every key is optional; an empty or missing file gives the defaults below.
 *
 *     [game]    name = "My Game"            library = "libmygame.dylib"
 *     [window]  title = "My Game"  width = 1280  height = 720
 *               mode = "windowed" | "borderless" | "fullscreen"   high_dpi = true
 *               surface = "auto" | "none" | "metal" | "vulkan"
 *     [timing]  mode = "fixed" | "variable"  fixed_hz = 60  max_catchup_steps = 4
 *               time_scale = 1.0  frame_cap_hz = 0
 *     [tasks]   workers = 0                  (frame workers besides the main thread; 0 = auto)
 *     [memory]  frame_arena_mib = 64
 *     [assets]  io_threads = 2  hot_reload = true  locale = ""   (DENG-143)
 *     [cvars]   r.vsync = true               (any cvar, dotted keys or nested tables)
 *
 * Unknown keys and tables log a warning that names the key path ("window.foo"). A known key
 * with the wrong type, an unknown enum string or a value out of range fails with DE_ERR_PARSE,
 * and the message names the key ("timing.mode"). Integers are accepted where a float is
 * expected. `[cvars]` values (bool, integer, float or string) are applied as startup values,
 * like the command line: a cvar nobody has registered yet keeps the value pending until it is
 * registered, and a DE_CVAR_READONLY cvar may be set until de_cvar_lock_startup(). A value the
 * cvar refuses logs a warning and the others are still applied; an array, date or inline table
 * array as a cvar value fails with DE_ERR_PARSE.
 */

#define DE__GAME_NAME_MAX  128u /**< bytes of de__game_config.name, NUL included */
#define DE__GAME_TITLE_MAX 256u /**< bytes of de__game_config.window_title, NUL included */

#define DE__GAME_DEFAULT_NAME            "DEngine"
#define DE__GAME_DEFAULT_WIDTH           1280u
#define DE__GAME_DEFAULT_HEIGHT          720u
#define DE__GAME_DEFAULT_FRAME_ARENA_MIB 64u

/* Accepted ranges (inclusive). Values outside fail with DE_ERR_PARSE. */
#define DE__GAME_MAX_WINDOW_DIM      16384u
#define DE__GAME_MAX_FIXED_HZ        10000u
#define DE__GAME_MAX_CATCHUP_STEPS   1000u
#define DE__GAME_MAX_FRAME_CAP_HZ    10000u
#define DE__GAME_MAX_WORKERS         256u
#define DE__GAME_MAX_FRAME_ARENA_MIB 65536u
#define DE__GAME_MAX_IO_THREADS      64u /* DENG-143: [assets] io_threads */

/* DENG-143: [assets] defaults and the locale buffer (de_asset_set_locale keeps 35 bytes). */
#define DE__GAME_DEFAULT_IO_THREADS 2u
#define DE__GAME_LOCALE_MAX         36u /**< bytes of de__game_config.asset_locale, NUL included */

/** The parsed project file. Plain data: copy it freely. */
typedef struct de__game_config {
    char name[DE__GAME_NAME_MAX];          /**< [game] name; DE__GAME_DEFAULT_NAME */
    char library[DE_FS_PATH_MAX];          /**< [game] library; "" = none (static game) */
    char window_title[DE__GAME_TITLE_MAX]; /**< [window] title; "" = use `name` */
    u32 window_width, window_height;       /**< [window] width, height; 1280 x 720 */
    de_window_mode window_mode;            /**< [window] mode; DE_WINDOW_WINDOWED */
    bool window_high_dpi;                  /**< [window] high_dpi; true */
    de_window_surface window_surface;      /**< [window] surface when explicitly selected */
    bool window_surface_auto;              /**< Default: match the platform GPU backend */
    de_timing timing;                      /**< [timing]; the DE_TIMING_DEFAULT_* values, fixed */
    u32 task_workers;                      /**< [tasks] workers; 0 = de_task_system_desc default */
    u32 frame_arena_mib;                   /**< [memory] frame_arena_mib; 64 (read by DENG-91) */
    u32 unknown_keys;                      /**< keys that were warned about as unknown */
    u32 cvars_applied; /**< [cvars] values that were applied (or left pending) */
    /* DENG-143: [assets], read by de__engine_create for the asset system. */
    u32 asset_io_threads;                   /**< [assets] io_threads; 2 (1 to 64) */
    bool asset_hot_reload;                  /**< [assets] hot_reload; true (Debug and Dev only) */
    char asset_locale[DE__GAME_LOCALE_MAX]; /**< [assets] locale; "" = none */
} de__game_config;

/** Fills `*out` with the defaults above. */
void de__game_config_defaults(de__game_config *out);

/**
 * Parses `len` bytes of game.toml text (no NUL needed) into `*out`, which starts from the
 * defaults, and applies its `[cvars]`. `source` names the text in messages ("game:/game.toml");
 * NULL gives "game.toml". Warnings go to log category "host".
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL `out`, NULL `text` with a non-zero `len`, or
 *         `len` > INT_MAX) or DE_ERR_PARSE (bad TOML with its line, or a bad key as described
 *         above). On failure `*out` holds the defaults and no cvar was set.
 */
de_result de__game_config_parse(const char *text, size_t len, const char *source,
                                de__game_config *out);

/**
 * Reads the file at virtual path `vpath` and parses it (de__game_config_parse). A file that does
 * not exist (DE_ERR_NOT_FOUND from the VFS) gives the defaults, logs an Info message and returns
 * DE_OK. Other read errors are returned wrapped (DE_ERR_IO, DE_ERR_INVALID_ARGUMENT, ...).
 */
de_result de__game_config_load(const char *vpath, de__game_config *out);

/* ---------- Engine (engine.c) ---------- */

/**
 * de__engine_create() parameters. `(de__engine_desc){0}` is valid: the default heap, the
 * game.toml defaults and a window.
 */
typedef struct de__engine_desc {
    /** Every engine allocation: the engine itself, the platform state, the task system, the type
     * registry and the world. NULL = de_heap_allocator(). Must outlive the engine. */
    const de_allocator *allocator;
    /** The project config, copied. NULL = de__game_config_defaults(). */
    const de__game_config *config;
    /** No window, GPU, audio or UI; the platform starts with DE_PLATFORM_HEADLESS. */
    bool headless;
} de__engine_desc;

/**
 * The engine behind the public de_engine. `pub` is first, so a de_engine * from the game and the
 * de__engine * are the same address (de__engine_from).
 */
typedef struct de__engine {
    de_engine pub;
    de__game_config config;
    de_timing timing;         /* the loop's timing; de_engine_set_timing writes it */
    de_atomic_u32 quit;       /* 1 after de_engine_request_quit */
    bool headless;            /* created headless (desc), or the platform has no video */
    bool owns_platform;       /* de_platform_init succeeded here; destroy shuts it down */
    de_log_category log_host; /* "host" */
    de_log_category log_game; /* "game" */
    bool owns_io;             /* DENG-143: de_io_init succeeded here; destroy shuts it down */
} de__engine;

/** The internal engine of a public pointer (no check; `e` must come from de__engine_create). */
static inline de__engine *de__engine_from(de_engine *e) {
    return (de__engine *)(void *)e;
}
static inline const de__engine *de__engine_from_const(const de_engine *e) {
    return (const de__engine *)(const void *)e;
}

/**
 * Creates the engine, in this order: the allocator (default heap), log categories `host` and
 * `game`, cvars (the registry is process-wide and created on first use: nothing to create),
 * the platform (de_platform_init; a platform that is already running is used as it is and not
 * shut down by destroy), the task system (`config.task_workers` frame workers), the type
 * registry, the ECS world and its built-in components (de_builtin_components: the transform and
 * hierarchy components and the transform propagation system, registered before anything else so
 * their ids are the same on every machine), the process I/O threads (de_io_init with `[assets]
 * io_threads`; an I/O system that is already running is used as it is and not shut down by
 * destroy) and the asset system (`assets`: source mode, packed in Shipping; attached to the world
 * at once, so the asset event type and the DeAssetSystemRef resource exist before the game
 * registers its systems; DENG-143), the input state (`input`: created with the engine
 * allocator and attached to the world, so de_input_resource and the input.actions /
 * input.fixed_tick systems exist before the game registers its systems; DENG-84), and a window
 * from `[window]` unless headless (or the platform runs without video). Optional modules (gpu,
 * renderer, audio, physics, ui, ...) stay NULL.
 *
 * Main thread only: the calling thread becomes the platform's main thread and the task
 * system's worker 0. On failure everything created so far is destroyed in reverse order, `*out`
 * is NULL and the error context says which step failed (wrapping the step's own error).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`out` NULL), DE_ERR_OUT_OF_MEMORY, or the error of the
 *         failed step (for example DE_ERR_UNSUPPORTED when no video driver can start, or
 *         DE_ERR_BUSY when the thread already runs a task system).
 */
de_result de__engine_create(const de__engine_desc *d, de_engine **out);

/** Destroys in the exact reverse order of creation: window, input (detached first; DENG-84),
 * asset system, I/O threads (if the engine started them), world, type registry, task system (its
 * threads are joined), platform (if the engine started it), then the engine. NULL is ignored.
 * Main thread only. */
void de__engine_destroy(de_engine *e);

DE_EXTERN_C_END
#endif /* DE_HOST_ENGINE_INTERNAL_H */
