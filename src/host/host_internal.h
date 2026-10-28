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
#include "host/hot_reload.h"    /* DENG-93: de__hot_reload */

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

    /* ---- DENG-95: headless mode (headless.c). ---- */
    /** FixedUpdate runs of the last de__loop_run (written by the loop when it returns). */
    u64 loop_fixed_steps;
    /** de__headless_begin installed the stop handlers; de__headless_end removes them. */
    bool headless_handlers;

    /* ---- DENG-93: game library hot reload (hot_reload.c). Zeroed: off. ---- */
    de__hot_reload hot_reload;

    /* ---- DENG-623: the Dev-only editor service (editor/editor.c). NULL: not running. ---- */
    struct de__editor *editor;
} de__host;

/* ---------- Main loop (loop.c; DENG-91 owns the body) ---------- */

/**
 * Runs the main loop (SPEC §10.2) until it stops: DE_SCHEDULE_STARTUP once, then frames until
 * de_engine_request_quit (or DE_EV_QUIT from the platform) or the frame limit (`--frames N`, else
 * `h->loop_max_frames`); the frame in which quit is asked for completes. Each frame: event pump,
 * the asset system update (DENG-143; when the engine has one), frame-begin hooks, First, PreUpdate,
 * FixedUpdate 0..N times, Update, PostUpdate, Extract, Last, frame-end hooks, frame arena swap,
 * frame pacing. The timing (de_engine_get_timing) is read at each frame start. Creates the frame
 * arenas (de_frame_arenas_init) unless the caller has, and destroys the ones it created before it
 * returns. Main thread (frame worker 0) only.
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
 * results the frame's systems must see. The asset system update (de_asset_system_update,
 * DENG-143) is not a hook: the loop calls it itself, just before the hooks, so hooks already see
 * that frame's load states. Same rules and results as de__loop_add_frame_end_hook.
 */
de_result de__loop_add_frame_begin_hook(de__host *h, de__loop_hook_fn fn, void *user);

/* ---------- Headless mode (headless.c; DENG-95) ---------- */

/**
 * Checks that `e` has no window, GPU device, renderer, audio device or UI, as a headless engine
 * must not (SPEC §16.3).
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (`e` NULL) or DE_ERR_INTERNAL naming the first one found.
 */
de_result de__headless_check(const de_engine *e);

/**
 * Starts headless mode for the loop of `h` (its engine exists): checks the engine
 * (de__headless_check), adds a frame-end hook and installs the stop handlers: SIGINT and SIGTERM
 * on POSIX, a console control handler on Windows. A handler only sets an atomic flag; the hook
 * then calls de_engine_request_quit, so the loop stops after the current frame. The handlers are
 * process-wide: when another run owns them, this one logs a warning and has none. Logs one Info
 * line. Main thread.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (no engine), DE_ERR_INTERNAL (the engine has a window
 *         or device), DE_ERR_BUSY (the frame-end hook list is full) or DE_ERR_IO (the OS refused
 *         a handler).
 */
de_result de__headless_begin(de__host *h);

/** Restores the handlers de__headless_begin replaced and clears a pending stop. Call after the
 * loop, before the engine is destroyed. Safe when de__headless_begin did not install them. */
void de__headless_end(de__host *h);

#if defined(_WIN32)
/** The console control handler's body (`ctrl_type` is a CTRL_*_EVENT): sets the stop flag and
 * returns true for Ctrl+C, Ctrl+Break, close, logoff and shutdown (the last three wait up to
 * 4.5 s for de__headless_end); false for anything else. Exposed for tests: generating a real
 * console event would also reach every other process on the console. */
bool de__headless_on_console_ctrl(u32 ctrl_type);
#endif

/* ---------- Hot reload swap (game_module.c; DENG-93) ---------- */

/**
 * Exchanges the attached game `m` with `other` (hot reload, DENG-93): everything but the
 * `generation` counters, which stay with their modules. `h->api` points at `m->table`, so it
 * names the other game's table afterwards. Neither library is opened or closed.
 */
void de__game_module_swap(de__game_module *m, de__game_module *other);

DE_EXTERN_C_END
#endif /* DE_HOST_HOST_INTERNAL_H */
