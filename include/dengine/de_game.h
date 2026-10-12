/* de_game.h — engine host, game module API, hot reload, main loop configuration. SPEC.md §10.
 *
 * A game exports one function:
 *   DE_GAME_EXPORT const de_game_api *de_game_get_api(void);
 * Dev builds load it from a DLL and hot-reload it. Shipping builds link it statically. */
#ifndef DE_GAME_H
#define DE_GAME_H
#include "de_base.h"
#include "de_memory.h"
#include "de_platform.h"
#include "de_task.h"
#include "de_reflect.h"
#include "de_ecs.h"
#include "de_asset.h"
DE_EXTERN_C_BEGIN

/* Forward declarations of optional modules (NULL when compiled out or disabled). */
typedef struct de_gpu_device de_gpu_device;
typedef struct de_renderer de_renderer;
typedef struct de_physics_world de_physics_world;
typedef struct de_physics2d_world de_physics2d_world;
typedef struct de_audio de_audio;
typedef struct de_input de_input;
typedef struct de_ui de_ui;
typedef struct de_text_system de_text_system;
typedef struct de_anim_system de_anim_system;
typedef struct de_nav de_nav;
typedef struct de_net_session de_net_session;
typedef struct de_steam de_steam;

/* Everything the game can reach. Owned by the host; stable across hot reloads. */
typedef struct de_engine {
    const de_allocator *allocator;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *world;
    de_asset_system *assets;
    de_window window;
    de_gpu_device *gpu;    /* NULL on headless server */
    de_renderer *renderer; /* NULL on headless server */
    de_input *input;
    de_audio *audio;
    de_physics_world *physics;
    de_physics2d_world *physics2d;
    de_anim_system *anim;
    de_text_system *text;
    de_ui *ui;
    de_nav *nav;
    de_net_session *net; /* created by the game when needed */
    de_steam *steam;
    void *game_state; /* game-owned; survives hot reload */
} de_engine;

/* ---------- Timing ---------- */
typedef enum de_timing_mode {
    DE_TIMING_FIXED,   /* fixed-step FixedUpdate + interpolated rendering */
    DE_TIMING_VARIABLE /* FixedUpdate once per frame with the frame delta */
} de_timing_mode;

/** Default de_timing.fixed_hz (game.toml `[timing] fixed_hz`). */
#define DE_TIMING_DEFAULT_FIXED_HZ 60u
/** Default de_timing.max_catchup_steps (game.toml `[timing] max_catchup_steps`). */
#define DE_TIMING_DEFAULT_MAX_CATCHUP_STEPS 4u
/** Default de_timing.time_scale (game.toml `[timing] time_scale`). */
#define DE_TIMING_DEFAULT_TIME_SCALE 1.0f
/** Default de_timing.frame_cap_hz (game.toml `[timing] frame_cap_hz`): uncapped / vsync. */
#define DE_TIMING_DEFAULT_FRAME_CAP_HZ 0u

/** Main loop timing. The defaults (DE_TIMING_DEFAULT_*, mode DE_TIMING_FIXED) are what an empty
 * game.toml gives. */
typedef struct de_timing {
    de_timing_mode mode;
    u32 fixed_hz;          /* default 60 */
    u32 max_catchup_steps; /* default 4 */
    f32 time_scale;        /* 1.0 */
    u32 frame_cap_hz;      /* 0 = uncapped / vsync */
} de_timing;

/**
 * Changes the main loop timing at runtime; the loop uses it from its next frame. `*t` is copied.
 * A `fixed_hz` of 0 means DE_TIMING_DEFAULT_FIXED_HZ; a negative or NaN `time_scale` means 1.0.
 * NULL `e` or `t` is ignored. Call it from the main thread (or a DE_SYSTEM_MAIN_THREAD system).
 */
DE_API void de_engine_set_timing(de_engine *e, const de_timing *t);
/** The timing the loop uses: game.toml's `[timing]`, or the last de_engine_set_timing().
 * Zeroed for a NULL `e`. */
DE_API de_timing de_engine_get_timing(const de_engine *e);
/** Asks the main loop to stop after the current frame; the host then calls `shutdown`.
 * Thread-safe (any thread, any system). NULL is ignored. */
DE_API void de_engine_request_quit(de_engine *e);
/** True once de_engine_request_quit() was called. Thread-safe. False for a NULL `e`. */
DE_API bool de_engine_quit_requested(const de_engine *e);

/* ---------- Game API ---------- */
#define DE_GAME_API_VERSION 1

typedef struct de_game_api {
    u32 api_version; /* DE_GAME_API_VERSION */
    const char *name;
    de_result (*init)(de_engine *e); /* first load only */
    void (*shutdown)(de_engine *e);
    void (*register_types)(de_engine *e, de_type_registry *types); /* every load */
    void (*register_systems)(de_engine *e, de_world *w);           /* every load */
    void (*on_unload)(de_engine *e); /* before a hot reload swaps code */
    void (*on_reload)(de_engine *e); /* after a hot reload */
} de_game_api;

#if defined(_WIN32)
#define DE_GAME_EXPORT __declspec(dllexport)
#else
#define DE_GAME_EXPORT __attribute__((visibility("default")))
#endif

/* ---------- Host configuration (game.toml, overridable from the command line) ---------- */

/** The project file de_host_run() reads when neither de_host_desc.game_toml nor the command line
 * (`--game-toml <vpath>`) names one. */
#define DE_HOST_DEFAULT_GAME_TOML "game:/game.toml"

typedef struct de_host_desc {
    const char *game_toml; /* virtual path; NULL = DE_HOST_DEFAULT_GAME_TOML */
    /** Dev: native path of the game library (`game.dll`, `libgame.so`, `libgame.dylib`), which
     * must export de_game_get_api(). Wins over `static_api`. When both are NULL, game.toml's
     * `[game] library` is loaded (relative to `game:/`; without an extension the platform name
     * is used: "game" → "libgame.dylib"). */
    const char *game_library;
    /** Shipping: de_game_get_api() of the statically linked game (de_add_game() generates the
     * main() that passes it). */
    const de_game_api *static_api;
    bool headless; /* dedicated server: no window, GPU, audio, UI */
    int argc;
    char **argv;
    /** Native directory for `game:/` (de_fs_desc.game_dir), which holds game.toml and the game's
     * data; NULL = the directory of the executable. `dengine --game <lib>` sets it to the game
     * library's directory. Ignored when the caller has called de_fs_init already. */
    const char *game_dir;
} de_host_desc;

/**
 * Runs the whole engine: create modules, load game, loop, shut down. A NULL `desc` is a zeroed
 * one (which has no game, so it fails).
 *
 * Order: de_prof_init, de_log_init and de_fs_init (each only when the caller has not started it
 * already; the host stops only what it started), then the project file (`--game-toml <vpath>`
 * on the command line, else `desc->game_toml`, else DE_HOST_DEFAULT_GAME_TOML; a missing file
 * gives the defaults) with its `[cvars]`, then `user:/config.toml` (de_cvar_load), the `+name
 * value` pairs of the command line (de_cvar_apply_args) and de_cvar_lock_startup. Then it
 * creates the engine, attaches the game (`api_version` must be DE_GAME_API_VERSION), and calls
 * `init`, `register_types`, `register_systems`, runs the main loop, calls `shutdown`, and
 * destroys everything in reverse order. `shutdown` is called only when `init` succeeded.
 *
 * Main thread only (the platform layer and the task system's worker 0 live there).
 * @return the process exit code: 0 on success, 1 on any failure (the error and its cause chain
 *         are logged in category "host").
 */
DE_API int de_host_run(const de_host_desc *desc);

DE_EXTERN_C_END
#endif
