/*
 * host_internal.h: the app host's internal seams (internal to src/host/, not installed).
 * SPEC §10.
 *
 * Include as "host/host_internal.h". Shared file (append-only, S06 sprint plan): DENG-90 creates
 * the de__host state and the seams below with stub bodies; DENG-91 fills de__loop_run in
 * loop.c and DENG-92 fills de__game_attach / de__game_detach in game_module.c. Add fields at the
 * end of de__host and declarations at the end of the file; do not reorder or rewrite.
 */
#ifndef DE_HOST_HOST_INTERNAL_H
#define DE_HOST_HOST_INTERNAL_H

#include <dengine/de_game.h>

#include "host/engine_internal.h"
#include "host/game_module.h"   /* DENG-92: de__game_module */
#include "host/loop_internal.h" /* DENG-91: the hook lists de__host holds */

DE_EXTERN_C_BEGIN

/** Everything one de_host_run() call owns. Lives on de_host_run's stack. */
typedef struct de__host {
    /** The caller's desc (a zeroed one for NULL). Its strings belong to the caller. */
    de_host_desc desc;
    /** The project file used: `--game-toml`, else desc.game_toml, else the default. */
    const char *game_toml;
    /** The parsed project file (the defaults when it does not exist). */
    de__game_config config;
    /** Created by de__engine_create; NULL before and after. */
    de_engine *engine;
    /** The attached game's API table (de__game_attach); NULL when no game is attached. */
    const de_game_api *api;
    /** Log category "host". */
    de_log_category log;
    /** DENG-92: the attached game (library or static); `api` points at `game.table`. */
    de__game_module game;

    /* ---- DENG-91: main loop (loop.c). Zeroed: no hooks, no frame limit, the real clock. ---- */
    /** Frame-begin and frame-end hooks (de__loop_add_frame_begin_hook / _end_hook). */
    de__loop_hooks loop_hooks;
    /** Stop after this many frames; 0 runs until quit. `--frames N` on the command line
     * (desc.argv) overrides it. */
    u64 loop_max_frames;
    /** The loop's clock (frame deltas). NULL = de_time_now_ns. Frame pacing always sleeps on the
     * real clock. For tests. */
    de__loop_clock_fn loop_clock;
    void *loop_clock_user;
    /** Frames the last de__loop_run completed (Startup not counted). */
    u64 loop_frames;
} de__host;

/* ---------- Main loop (loop.c; DENG-91 owns the body) ---------- */

/**
 * Runs the main loop (SPEC §10.2) until it stops: DE_SCHEDULE_STARTUP once, then frames until
 * de_engine_request_quit (or DE_EV_QUIT from the platform) or the frame limit (`--frames N`, else
 * `h->loop_max_frames`); the frame in which quit is asked for completes. Each frame: event pump,
 * frame-begin hooks, First, PreUpdate, FixedUpdate 0..N times, Update, PostUpdate, Extract, Last,
 * frame-end hooks, frame arena swap, frame pacing. The timing (de_engine_get_timing) is read at
 * each frame start. Creates the frame arenas (de_frame_arenas_init) unless the caller has, and
 * destroys the ones it created before it returns. Main thread (frame worker 0) only.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (no engine, or a bad `--frames` value), the frame
 *         arenas' error, or the first failed schedule's or hook's error (wrapped with its name).
 */
de_result de__loop_run(de__host *h);

/* ---------- Game attachment (game_module.c; DENG-92 owns the bodies) ---------- */

/**
 * Checks a game API table: not NULL and `api_version == DE_GAME_API_VERSION`.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (NULL) or DE_ERR_VERSION_MISMATCH (the message names
 *         both versions and the game).
 */
de_result de__game_api_check(const de_game_api *api);

/**
 * Finds the game, loads it (game_module.h) and sets `h->api` to `&h->game.table`. The first
 * match wins: `d->game_library` (a native path; a bare file name means the current directory),
 * `d->static_api`, then game.toml's `[game] library` (resolved against game:/). The table is
 * validated (version, name, required callbacks). Called after the engine exists and before
 * `init`. Logs the game's name and where it came from at Info.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (no game at all, a NULL table, a missing name or
 *         callback), DE_ERR_NOT_FOUND (no such library, or no de_game_api symbol in it),
 *         DE_ERR_IO (the OS refused the library) or DE_ERR_VERSION_MISMATCH.
 */
de_result de__game_attach(de__host *h, const de_host_desc *d);

/** Lets go of the game: clears `h->api` and closes its library (de__game_module_unload).
 * Called after `shutdown` (or after a failed `init`), before the engine is destroyed. Safe to
 * call when nothing is attached. */
void de__game_detach(de__host *h);

/* ---------- Helpers (host_run.c) ---------- */

/** Logs `what` with the calling thread's last error (de_error_last) and its cause chain, at
 * Error level in category "host". */
void de__host_log_error(const char *what);

/* ---------- Main loop hooks (loop.c; DENG-91) ---------- */

/**
 * Adds `fn(h, user)` to the hooks that run once per frame after `Last`, before the frame arena
 * swap and frame pacing, in registration order (SPEC §10.2 step 6). Users: hot reload (DENG-93),
 * crash annotations (DENG-96), the zero-alloc window (DENG-97). Main thread; also allowed from a
 * hook (the new hook runs from that frame's list if it comes later in it).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`h` or `fn` NULL) or DE_ERR_BUSY (DE__LOOP_MAX_HOOKS
 *         frame-end hooks already).
 */
de_result de__loop_add_frame_end_hook(de__host *h, de__loop_hook_fn fn, void *user);

/**
 * Adds `fn(h, user)` to the hooks that run once per frame after the platform event pump (and the
 * input update), before `First`, in registration order. This is where per-frame work runs whose
 * results the frame's systems must see: the asset system update (de_asset_system_update, wired
 * by DENG-143). Same rules and results as de__loop_add_frame_end_hook.
 */
de_result de__loop_add_frame_begin_hook(de__host *h, de__loop_hook_fn fn, void *user);

DE_EXTERN_C_END
#endif /* DE_HOST_HOST_INTERNAL_H */
