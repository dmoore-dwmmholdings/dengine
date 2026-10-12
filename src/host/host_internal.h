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
} de__host;

/* ---------- Main loop (loop.c; DENG-91 owns the body) ---------- */

/**
 * Runs the main loop until it stops. DENG-90's stub runs DE_SCHEDULE_STARTUP once, then one
 * frame of every other schedule in de_schedule order (FixedUpdate once with dt = 1 / fixed_hz),
 * and returns. DENG-91 replaces it with the SPEC §10.2 loop (timing modes, catch-up, pacing,
 * de_engine_request_quit).
 * @return DE_OK, or the first failed schedule's error (wrapped with the schedule's name).
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
 * Finds the game and sets `h->api`. DENG-90's stub uses `d->static_api` only (checked with
 * de__game_api_check); a `d->game_library` without a static API is DE_ERR_UNSUPPORTED until
 * DENG-92 loads libraries. Called after the engine exists and before `init`.
 * @return DE_OK, DE_ERR_INVALID_ARGUMENT (no game at all), DE_ERR_VERSION_MISMATCH or
 *         DE_ERR_UNSUPPORTED.
 */
de_result de__game_attach(de__host *h, const de_host_desc *d);

/** Lets go of the game (unloads its library, once DENG-92 loads one) and clears `h->api`.
 * Called after `shutdown` (or after a failed `init`), before the engine is destroyed. Safe to
 * call when nothing is attached. */
void de__game_detach(de__host *h);

/* ---------- Helpers (host_run.c) ---------- */

/** Logs `what` with the calling thread's last error (de_error_last) and its cause chain, at
 * Error level in category "host". */
void de__host_log_error(const char *what);

DE_EXTERN_C_END
#endif /* DE_HOST_HOST_INTERNAL_H */
